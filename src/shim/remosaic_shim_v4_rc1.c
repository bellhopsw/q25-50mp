/*
 * libremosaic_wrapper.so  --  remosaic shim v4 for the Zinwa Q25 (Samsung S5KJN1), MIT
 *
 * Pipeline (port of remosaic_final() in bench.py):
 *   1. quad equalisation   per-4x4-phase gains measured on flat tiles (self-calibrating XTC stand-in)
 *   2. noise estimate      sigma^2 = a*signal + b from the quietest 16x16 blocks per brightness level
 *   3. bilateral denoise   same-colour quad neighbours only, per-pixel sigma from the noise model
 *   4. green               Quad-PPG directional green, soft H/V blend by gradient
 *   5. refinement          G = C - smooth(C - G) at R/B samples, blended in by a noise ramp
 *   6. R/B                 residual interpolation guided by green
 * The output is a standard Bayer frame for the ISP.
 *
 * Memory: one extra full-frame 16-bit buffer, plus ~10 MB of per-thread scratch.
 * Intermediate frames are 16-bit fixed point with 4 fractional bits (FX = 16), so 10-bit
 * data keeps sub-code precision without doubling memory.
 * The HAL's input buffer is reused for the denoised frame (persist.vendor.remoshim.inplace=0
 * allocates a separate buffer instead).
 *
 * API (old MTK wrapper, as imported by the Q25 HAL):
 *   int remosaic_init(int img_w, int img_h, int bayer_order, int pedestal)
 *   int remosaic_gainmap_gen(void *eep, unsigned long size)
 *   int remosaic_process_param_set(void *param)
 *   int remosaic_process(void *in, unsigned long in_size, void *out, unsigned long out_size)
 *   int remosaic_deinit(void)
 *
 * Properties (/vendor/build.prop):
 *   persist.vendor.remoshim.mode     0 = v4 pipeline (default), 1 = pixel swap, 2 = passthrough
 *   persist.vendor.remoshim.eq       3 = gain+offset per position (default), 2 = crosstalk model,
 *                                    1 = per-position gains (v4 original), 0 = off
 *   persist.vendor.remoshim.usecal   1 = accumulate/use stored calibration in /data/vendor/camera (default)
 *   persist.vendor.remoshim.diag     1 = log pattern diagnostics (default 0: off, they cost time)
 *   persist.vendor.remoshim.grid     -1 = best of global/3x2/6x4/12x8 per shot (default), 0..3 = force one
 *   persist.vendor.remoshim.pd       1 = detect and repair PDAF pixels (default), 0 = off
 *   persist.vendor.remoshim.pdthr    repair threshold in 0.1% (default 150 = 15%: strong outliers only)
 *   persist.vendor.remoshim.pdedge   1 = PDAF repair follows edges (default), 0 = plain 4-neighbour mean (v4 original)
 *   persist.vendor.remoshim.pdfixed  1 = start from the known JN1 PDAF layout (default), 0 = discover it by scanning
 *   persist.vendor.remoshim.tile16   1 = 16x16 PDAF-lattice gain map (default)
 *   persist.vendor.remoshim.acc      1 = accumulate/reuse calibration across shots (default)
 *   persist.vendor.remoshim.sharpen  luminance sharpening amount x100 (default 0 = off)
 *   persist.vendor.remoshim.dn       denoise strength x10 (default 20; 0 = off)
 *   persist.vendor.remoshim.dnr      denoise window radius 2..4; default: by noise level (2 good light, 3 medium, 4 low light)
 *   persist.vendor.remoshim.gatelo   refinement ramp, noise sigma at mid-grey in DN (default 15)
 *   persist.vendor.remoshim.gatehi   (default 35)
 *   persist.vendor.remoshim.bayer    -1 = HAL value (default), or force 0..3
 *   persist.vendor.remoshim.inplace  1 = reuse the input buffer (default), 0 = allocate
 *   persist.vendor.remoshim.threads  total threads (default 8)
 *   persist.vendor.remoshim.tile     RI column tile width (default 384)
 */
#include <android/log.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/system_properties.h>
#include <time.h>

#define TAG "RemosaicShim"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define EXPORT __attribute__((visibility("default")))

#ifndef NTHREADS
#define NTHREADS 7               /* + the calling thread = 8 */
#endif
#define WHITE 1023
#define FX 16                  /* fixed-point scale of intermediate frames */
#define IFX (1.0f / FX)
enum { CR = 0, CG = 1, CB = 2 };

static int g_w, g_h, g_bayer = 2, g_mode, g_eq = 1, g_ped = 64, g_inplace = 1;
static float g_dn = 2.0f, g_lo = 15.0f, g_hi = 35.0f;
static int g_dnr = 4;                           /* denoise window radius in use (2..4) */
static int g_dnr_set = -1, g_dnr_tab = -1;      /* property (-1 = by noise level), radius the tables were built for */
static int g_qcol[4][4], g_tcol[2][2];          /* [y][x] */

/* --------------------------------------------------------------- helpers */
static int bayer_col(int order, int px, int py)
{
	static const int t[4][2][2] = {
		{ { CB, CG }, { CG, CR } }, { { CG, CB }, { CR, CG } },
		{ { CG, CR }, { CB, CG } }, { { CR, CG }, { CG, CB } },
	};
	return t[order & 3][py & 1][px & 1];
}
static inline int qcol(int x, int y) { return g_qcol[y & 3][x & 3]; }
static inline int tcol(int x, int y) { return g_tcol[y & 1][x & 1]; }
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
/* scipy 'mirror' (d c b | a b c d | c b a) and 'reflect' (c b a | a b c d | d c b) */
static inline int mir(int i, int n) { if (i < 0) i = -i; if (i >= n) i = 2 * n - 2 - i; return i; }
static inline int rfl(int i, int n) { if (i < 0) i = -i - 1; if (i >= n) i = 2 * n - 1 - i; return i; }
/* scratch frame: mmap with MAP_POPULATE so the kernel maps all pages up front instead of
 * taking ~25k separate page faults during the first pass that writes it */
static void *frame_alloc(size_t n)
{
#ifdef MAP_POPULATE
	void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
	return p == MAP_FAILED ? NULL : p;
#else
	return malloc(n);
#endif
}
static void frame_free(void *p, size_t n)
{
	if (!p) return;
#ifdef MAP_POPULATE
	munmap(p, n);
#else
	(void)n; free(p);
#endif
}
static long ms_since(struct timespec *t0)
{
	struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
	return (t1.tv_sec - t0->tv_sec) * 1000 + (t1.tv_nsec - t0->tv_nsec) / 1000000;
}
static int prop_int(const char *name, int def)
{
	char v[PROP_VALUE_MAX] = { 0 };
	if (__system_property_get(name, v) > 0) return atoi(v);
	return def;
}

/* work queue: NTHREADS threads take jobs of `rows` rows from a shared counter, so the
 * fast cores (2x A76 on the G99) automatically do more jobs than the slow ones (6x A55).
 * Every stage reads only the complete previous-stage buffer, so job size never changes the output. */
struct wq { void (*fn)(int, int, void *); void *ctx; int rows, next; long cpu_ns, jobs; };
static int g_threads = NTHREADS + 1;             /* total incl. caller; persist.vendor.remoshim.threads */
static long g_cpu_ms;                            /* CPU time of the last run_rows, summed over threads */
static void *wq_main(void *a)
{
	struct wq *q = a; struct timespec c0, c1; long jobs = 0;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c0);
	for (;;) {
		int y0 = __atomic_fetch_add(&q->next, q->rows, __ATOMIC_RELAXED);
		if (y0 >= g_h) break;
		q->fn(y0, y0 + q->rows < g_h ? y0 + q->rows : g_h, q->ctx);
		jobs++;
	}
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c1);
	__atomic_fetch_add(&q->cpu_ns, (c1.tv_sec - c0.tv_sec) * 1000000000L + (c1.tv_nsec - c0.tv_nsec), __ATOMIC_RELAXED);
	__atomic_fetch_add(&q->jobs, jobs, __ATOMIC_RELAXED);
	return NULL;
}
static void run_rows(void (*fn)(int, int, void *), void *ctx, int rows)
{
	pthread_t th[64]; struct wq q = { fn, ctx, rows, 0, 0, 0 }; int i, n = g_threads - 1;
	if (n < 0) n = 0;
	if (n > 64) n = 64;
	for (i = 0; i < n; i++) if (pthread_create(&th[i], NULL, wq_main, &q)) th[i] = 0;
	wq_main(&q);                                         /* the calling thread helps too */
	for (i = 0; i < n; i++) if (th[i]) pthread_join(th[i], NULL);
	g_cpu_ms = q.cpu_ns / 1000000;
}
#define run_strips(fn, ctx) run_rows(fn, ctx, 32)

struct ctx {
	const uint16_t *src; uint16_t *dst;   /* generic in/out for the current stage */
	const uint16_t *d;                     /* denoised quad */
	const int16_t *g;                      /* green (pedestal removed) */
	int stride;
	float gain[4][4], na, nb, mix;
};


/* --------------------------------------------------------------- 0. PDAF pixel repair
 * The JN1 has a lattice of phase-detection pixels repeating every 16 px; in full-resolution mode they reach us
 * uncorrected and show as a regular dot pattern. Detected per frame (matches pd_detect() in bench.py):
 * for every pixel, the relative deviation from the same-colour pixels 4 px away (L/R/U/D, which are in the
 * neighbouring quad cells), in flat spots only; statistics per position mod 32. A position (mod 16) is PDAF when
 * all four of its mod-32 copies deviate by >= pdthr (default 1%) with the same sign, each at >= 6 standard errors. Two passes:
 * the second excludes pass-1 PDAF pixels from the references. PDAF pixels are then replaced (in the raw input)
 * by the mean of their non-PDAF neighbours 4 px away. persist.vendor.remoshim.pd: 1 = on (default), 0 = off. */
#define PDP 32
static int g_pd = 1;
#ifndef PDEDGE_DEFAULT
#define PDEDGE_DEFAULT 1
#endif
static int g_pdedge = PDEDGE_DEFAULT;          /* persist.vendor.remoshim.pdedge: edge-aware PDAF repair (default on) */
static double g_pdthr = 0.01;                  /* persist.vendor.remoshim.pdthr, in 0.1% units */
static unsigned char g_pdm[16][16];
struct pdst { const uint16_t *in; int S; unsigned char ex[16][16]; double (*part)[PDP * PDP][3]; };
static void pd_job(int y0, int y1, void *p)
{
	struct pdst *f = p; int job = y0 / 64, x, y, k;
	static const int off[4][2] = { { -4, 0 }, { 4, 0 }, { 0, -4 }, { 0, 4 } };
	double (*A)[3] = f->part[job];
	memset(A, 0, sizeof(f->part[0]));
	for (y = y0 > 4 ? y0 : 4; y < y1 && y < g_h - 4; y++)
		for (x = 4; x < g_w - 4; x++) {
			double s = 0, big = -1e9, small = 1e9; int cnt = 0;
			for (k = 0; k < 4; k++) {
				int yy = y + off[k][0], xx = x + off[k][1];
				if (f->ex[yy & 15][xx & 15]) continue;
				double v = (double)f->in[(size_t)yy * f->S + xx] - g_ped;
				s += v; cnt++;
				if (v > big) big = v;
				if (v < small) small = v;
			}
			if (cnt < 2) continue;
			double m = s / cnt;
			if (m < 20 || big - small > 0.10 * m + 10) continue;
			double d = ((double)f->in[(size_t)y * f->S + x] - g_ped) / m - 1;
			double *a = A[(y % PDP) * PDP + (x % PDP)];
			a[0] += 1; a[1] += d; a[2] += d * d;
		}
}
static int g_pdshot = 0, g_pdw = 0, g_pdh = 0, g_pdn = 0, g_pdrescan = 10;
static unsigned char g_pdunion[16][16];
static char g_pdmsg[600];
/* The JN1's PDAF layout is a property of the sensor: 8 positions per 16x16 tile, as (y, x): vertical pairs, each pair
 * sharing a 2x2 quad cell with two clean same-colour pixels at x+-1. persist.vendor.remoshim.pdfixed = 1 (default)
 * starts from this layout instead of discovering it (the statistical scan runs on every 10th shot and only adds). */
static int g_pdfixed = 1;
static const unsigned char kPdPos[8][2] = { { 0, 2 }, { 1, 2 }, { 6, 5 }, { 7, 5 }, { 8, 14 }, { 9, 14 }, { 14, 9 }, { 15, 9 } };
static void pd_seed_fixed(void)
{
	int i;
	memset(g_pdunion, 0, sizeof g_pdunion);
	for (i = 0; i < 8; i++) g_pdunion[kPdPos[i][0]][kPdPos[i][1]] = 1;
	memcpy(g_pdm, g_pdunion, sizeof g_pdm);
	g_pdn = 8; g_pdw = g_w; g_pdh = g_h; g_pdshot = 0;
	snprintf(g_pdmsg, sizeof g_pdmsg, "pdaf: fixed JN1 layout, 8 positions per 16x16 (statistical scan on shot %d)", g_pdrescan);
}
static int pd_detect_fix_scan(uint16_t *in, int S, char *msg, size_t msgsz);
static int pd_detect_fix(uint16_t *in, int S, char *msg, size_t msgsz)
{
	/* PDAF positions are fixed: scan on the first shot, then reuse; rescan every 10th shot */
	if (g_pdfixed && (g_pdw != g_w || g_pdh != g_h)) pd_seed_fixed();
	if (g_pdw == g_w && g_pdh == g_h && ((g_pdrescan > 0 && (++g_pdshot % g_pdrescan) != 0) || (g_pdfixed && g_pdrescan <= 0))) {
		snprintf(msg, msgsz, "%s [reused]", g_pdmsg);
		return g_pdn;
	}
	g_pdshot = 0;
	if (g_pdw != g_w || g_pdh != g_h) memset(g_pdunion, 0, sizeof g_pdunion);
	pd_detect_fix_scan(in, S, g_pdmsg, sizeof g_pdmsg);
	/* PDAF response depends on the scene (light angle, focus): keep every strong position ever found */
	{ int y, x, n = 0;
	  for (y = 0; y < 16; y++) for (x = 0; x < 16; x++) { g_pdunion[y][x] |= g_pdm[y][x]; n += g_pdunion[y][x]; }
	  memcpy(g_pdm, g_pdunion, sizeof g_pdm); g_pdn = n;
	  size_t l = strlen(g_pdmsg); snprintf(g_pdmsg + l, sizeof g_pdmsg - l, "; repairing %d (all found so far)", n); }
	g_pdw = g_w; g_pdh = g_h;
	snprintf(msg, msgsz, "%s", g_pdmsg);
	return g_pdn;
}
static int pd_detect_fix_scan(uint16_t *in, int S, char *msg, size_t msgsz)
{
	struct pdst f; int nj = (g_h + 63) / 64, j, k, pass, y, x, dy, dx, npd = 0;
	double minn = 0.02 * (double)g_h * g_w / (PDP * PDP); if (minn < 50) minn = 50;
	static double T[PDP * PDP][3], mean[PDP * PDP], se[PDP * PDP];
	memset(&f, 0, sizeof f); f.in = in; f.S = S;
	f.part = malloc(sizeof(*f.part) * nj);
	memset(g_pdm, 0, sizeof g_pdm);
	if (!f.part) { snprintf(msg, msgsz, "pdaf: alloc failed"); return 0; }
	for (pass = 0; pass < 2; pass++) {
		memcpy(f.ex, g_pdm, sizeof g_pdm);
		run_rows(pd_job, &f, 64);
		memset(T, 0, sizeof T);
		for (j = 0; j < nj; j++) for (k = 0; k < PDP * PDP; k++) { T[k][0] += f.part[j][k][0]; T[k][1] += f.part[j][k][1]; T[k][2] += f.part[j][k][2]; }
		for (k = 0; k < PDP * PDP; k++) {
			double N = T[k][0];
			mean[k] = N > 0 ? T[k][1] / N : 0;
			double var = N > 1 ? T[k][2] / N - mean[k] * mean[k] : 1.0;
			se[k] = sqrt((var > 0 ? var : 0) / (N > 1 ? N : 1));
		}
		memset(g_pdm, 0, sizeof g_pdm);
		for (y = 0; y < 16; y++)
			for (x = 0; x < 16; x++) {
				int all = 1, sg = 0;
				for (dy = 0; dy < 32 && all; dy += 16)
					for (dx = 0; dx < 32 && all; dx += 16) {
						int i = (y + dy) * PDP + (x + dx);
						if (!(T[i][0] >= minn && fabs(mean[i]) >= g_pdthr && fabs(mean[i]) >= 6 * se[i])) all = 0;
						int s1 = mean[i] > 0 ? 1 : -1;
						if (sg == 0) sg = s1; else if (s1 != sg) all = 0;
					}
				g_pdm[y][x] = (unsigned char)all;
			}
	}
	free(f.part);
	int o = snprintf(msg, msgsz, "pdaf:");
	for (y = 0; y < 16; y++) for (x = 0; x < 16; x++) if (g_pdm[y][x]) {
		npd++;
		if (o < (int)msgsz - 24) o += snprintf(msg + o, msgsz - o, " (%d,%d)%+.0f%%", y, x, 100 * mean[y * PDP + x]);
	}
	snprintf(msg + o, msgsz - o, "%s; %d positions per 16x16", npd ? "" : " none found", npd);
	return npd;
}
struct pdfix { uint16_t *in; int S; };
/* PDAF repair. A PDAF pixel is replaced from its same-colour, same-phase neighbours 4 px away (a quad-Bayer colour
 * repeats every 4 px). Flat areas: the mean of the 4 axis neighbours (v4 original). Edges: the 4-neighbour mean lands
 * between the two sides of the edge and leaves a visible dot at every PDAF position along it, so there the value is
 * taken from the pair of opposite neighbours (horizontal, vertical or either diagonal) that agree best, i.e. the pair
 * lying along the edge. The pair is used only when it is clearly better than the worst pair: the margin is
 * 24 DN plus 1/16 of the local level, so noise alone never triggers it. Neighbours that are PDAF pixels are skipped,
 * and only non-PDAF pixels are read, so the result does not depend on the order or the threading of the rows. */
static void pd_fix_job(int y0, int y1, void *p)
{
	struct pdfix *f = p; int y, x, k;
	static const int off[4][2] = { { -4, 0 }, { 4, 0 }, { 0, -4 }, { 0, 4 } };
	/* opposite pairs: horizontal, vertical, diagonal, anti-diagonal (dy1,dx1,dy2,dx2) */
	static const int pr[4][4] = { { 0, -4, 0, 4 }, { -4, 0, 4, 0 }, { -4, -4, 4, 4 }, { -4, 4, 4, -4 } };
	for (y = y0; y < y1; y++)
		for (x = 0; x < g_w; x++) {
			if (!g_pdm[y & 15][x & 15]) continue;
			long s = 0; int n = 0;
			for (k = 0; k < 4; k++) {
				int yy = y + off[k][0], xx = x + off[k][1];
				if (yy < 0 || yy >= g_h || xx < 0 || xx >= g_w || g_pdm[yy & 15][xx & 15]) continue;
				s += f->in[(size_t)yy * f->S + xx]; n++;
			}
			int val = n ? (int)((s + n / 2) / n) : -1;
			if (g_pdedge) {
				int np = 0, dmin = 1 << 30, dmax = -1, best = 0; long lvl = 0;
				for (k = 0; k < 4; k++) {
					int ya = y + pr[k][0], xa = x + pr[k][1], yb = y + pr[k][2], xb = x + pr[k][3];
					if (ya < 0 || ya >= g_h || xa < 0 || xa >= g_w || yb < 0 || yb >= g_h || xb < 0 || xb >= g_w) continue;
					if (g_pdm[ya & 15][xa & 15] || g_pdm[yb & 15][xb & 15]) continue;
					int a = f->in[(size_t)ya * f->S + xa], b = f->in[(size_t)yb * f->S + xb];
					int d = a > b ? a - b : b - a;
					lvl += a + b; np++;
					if (d < dmin) { dmin = d; best = (a + b + 1) / 2; }
					if (d > dmax) dmax = d;
				}
				if (np >= 2 && dmax - dmin > 24 + (int)(lvl / (2 * np)) / 16) val = best;
			}
			if (val >= 0) f->in[(size_t)y * f->S + x] = (uint16_t)val;
		}
}

/* --------------------------------------------------------------- 1. equalisation */
struct eqm { const uint16_t *in; int stride; pthread_mutex_t mu; int64_t sum[4][4], cnt[4][4], nflat; };
static void eqm_job(int y0, int y1, void *p)    /* rows are 64-aligned, so tiles start at multiples of 8 */
{
	struct eqm *e = p; int64_t sum[4][4] = { { 0 } }, cnt[4][4] = { { 0 } }, nflat = 0;
	int tx, ty, px, py, c, h4 = g_h & ~3, w4 = g_w & ~3;
	for (ty = (y0 + 7) & ~7; ty < y1 && ty + 4 <= h4; ty += 8)
		for (tx = 0; tx + 4 <= w4; tx += 8) {
			int mn[3] = { 65535, 65535, 65535 }, mx[3] = { -1, -1, -1 }, flat = 1;
			for (py = 0; py < 4; py++)
				for (px = 0; px < 4; px++) {
					int v = e->in[(size_t)(ty + py) * e->stride + tx + px];
					c = g_qcol[py][px];
					if (v < mn[c]) mn[c] = v;
					if (v > mx[c]) mx[c] = v;
				}
			for (c = 0; c < 3; c++)
				if (!(mx[c] < 1000 && mx[c] - mn[c] <= (mn[c] - g_ped) / 4.0 + 16)) flat = 0;
			if (!flat) continue;
			nflat++;
			for (py = 0; py < 4; py++)
				for (px = 0; px < 4; px++) {
					int v = e->in[(size_t)(ty + py) * e->stride + tx + px] - g_ped;
					if (v > 0) { sum[py][px] += v; cnt[py][px] += 1; }
				}
		}
	pthread_mutex_lock(&e->mu);
	for (py = 0; py < 4; py++) for (px = 0; px < 4; px++) { e->sum[py][px] += sum[py][px]; e->cnt[py][px] += cnt[py][px]; }
	e->nflat += nflat;
	pthread_mutex_unlock(&e->mu);
}
static void eq_measure(const uint16_t *in, int stride, float gain[4][4])
{
	struct eqm e; double m[4][4], cm[3] = { 0 }, cc[3] = { 0 }; int px, py, c;
	memset(&e, 0, sizeof(e)); e.in = in; e.stride = stride; pthread_mutex_init(&e.mu, NULL);
	run_rows(eqm_job, &e, 64);
	pthread_mutex_destroy(&e.mu);
	for (py = 0; py < 4; py++) for (px = 0; px < 4; px++) gain[py][px] = 1.0f;
	if (e.nflat <= 500) return;
	for (py = 0; py < 4; py++)
		for (px = 0; px < 4; px++) {
			if (e.cnt[py][px] == 0) return;          /* no data for a phase: leave gains at 1 */
			m[py][px] = (double)e.sum[py][px] / e.cnt[py][px];
			cm[g_qcol[py][px]] += m[py][px]; cc[g_qcol[py][px]] += 1;
		}
	for (py = 0; py < 4; py++)
		for (px = 0; px < 4; px++) {
			c = g_qcol[py][px];
			double gg = (cm[c] / cc[c]) / m[py][px];
			gain[py][px] = (float)(gg < 0.85 ? 0.85 : gg > 1.15 ? 1.15 : gg);
		}
}
static void eq_apply(int y0, int y1, void *p)
{
	struct ctx *c = p; int x, y;
	for (y = y0; y < y1; y++) {
		const uint16_t *s = c->src + (size_t)y * c->stride; uint16_t *d = c->dst + (size_t)y * c->stride;
		const float *gr = c->gain[y & 3];
		for (x = 0; x < g_w; x++) {
			float v = g_ped + (s[x] - g_ped) * gr[x & 3];
			d[x] = (uint16_t)clampi((int)lrintf(v * FX), 0, WHITE * FX);
		}
	}
}

/* --------------------------------------------------------------- 2. noise estimate */
static int cmp_f(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return (x > y) - (x < y); }
static float sel_k(float *v, int n, int k)     /* quickselect, k-th smallest */
{
	int lo = 0, hi = n - 1;
	while (lo < hi) {
		float piv = v[(lo + hi) / 2]; int i = lo, j = hi;
		while (i <= j) {
			while (v[i] < piv) i++;
			while (v[j] > piv) j--;
			if (i <= j) { float t = v[i]; v[i] = v[j]; v[j] = t; i++; j--; }
		}
		if (k <= j) hi = j; else if (k >= i) lo = i; else return v[k];
	}
	return v[k];
}
static float quantile_sorted(const float *s, int n, double p)   /* numpy 'linear' */
{
	double pos = p * (n - 1); int i = (int)pos; double f = pos - i;
	return (i + 1 < n) ? (float)(s[i] + (s[i + 1] - s[i]) * f) : s[n - 1];
}
/* numpy-style linear quantile via selection (no full sort); reorders v */
static float quantile_sel(float *v, int n, double p)
{
	double pos = p * (n - 1); int i = (int)pos, k; double f = pos - i;
	float a = sel_k(v, n, i), b = a;
	if (i + 1 < n && f > 0) { b = v[i + 1]; for (k = i + 2; k < n; k++) if (v[k] < b) b = v[k]; }
	return (float)(a + (b - a) * f);
}
struct nblk { const uint16_t *q; int stride, wb, hb; float *sd, *lv; float scl; };
static void nblk_job(int y0, int y1, void *p)   /* rows are 64-aligned: whole 16-row blocks */
{
	struct nblk *nb = p; const int B = 16; int i, j, k, r; float d[128];
	for (i = y0 / B; i < (y1 + B - 1) / B && i < nb->hb; i++)
		for (j = 0; j < nb->wb; j++) {
			double l = 0; int m = 0;
			for (r = 0; r < B; r++) {
				const uint16_t *row = nb->q + (size_t)(i * B + r) * nb->stride + j * B;
				for (k = 0; k < B / 2; k++) {
					float a = row[2 * k] * nb->scl, b = row[2 * k + 1] * nb->scl;
					d[m++] = fabsf(a - b) * 0.70710678f;
					l += (a + b) * 0.5 - g_ped;
				}
			}
			/* numpy median of 128 = mean of the 64th and 65th smallest */
			float m1 = sel_k(d, 128, 63), m2 = d[64];
			for (k = 65; k < 128; k++) if (d[k] < m2) m2 = d[k];
			nb->sd[i * nb->wb + j] = (m1 + m2) * 0.5f / 0.6745f;
			nb->lv[i * nb->wb + j] = (float)(l / 128.0);
		}
}
/* exact ascending sort of floats: LSD radix on order-preserving keys (4 passes of 8 bits) */
static void sort_floats(float *v, int n)
{
	uint32_t *a = malloc(sizeof(uint32_t) * n), *b = malloc(sizeof(uint32_t) * n);
	int i, pass;
	if (!a || !b) { free(a); free(b); qsort(v, n, sizeof(float), cmp_f); return; }
	for (i = 0; i < n; i++) {
		uint32_t u; memcpy(&u, &v[i], 4);
		a[i] = (u & 0x80000000u) ? ~u : (u | 0x80000000u);
	}
	for (pass = 0; pass < 4; pass++) {
		int cnt[257] = { 0 }, sh = pass * 8;
		for (i = 0; i < n; i++) cnt[((a[i] >> sh) & 255) + 1]++;
		for (i = 0; i < 256; i++) cnt[i + 1] += cnt[i];
		for (i = 0; i < n; i++) b[cnt[(a[i] >> sh) & 255]++] = a[i];
		uint32_t *t = a; a = b; b = t;
	}
	for (i = 0; i < n; i++) {
		uint32_t u = (a[i] & 0x80000000u) ? (a[i] & 0x7fffffffu) : ~a[i];
		memcpy(&v[i], &u, 4);
	}
	free(a); free(b);
}
static void estimate_noise(const uint16_t *q, int stride, float scl, float *pa, float *pb)
{
	const int B = 16; int hb = g_h / B, wb = g_w / B, n = hb * wb, i, j;
	float *sd = malloc(sizeof(float) * n), *lv = malloc(sizeof(float) * n), *tmp = malloc(sizeof(float) * n);
	float *ls = malloc(sizeof(float) * n), edges[9], xs[8], vs[8];
	int nx = 0;
	*pa = 0.0f; *pb = 0.25f;
	if (!sd || !lv || !tmp || !ls || n < 20) goto out;
	{ struct nblk nb = { q, stride, wb, hb, sd, lv, scl }; run_rows(nblk_job, &nb, 64); }
	memcpy(ls, lv, sizeof(float) * n); sort_floats(ls, n);
	for (i = 0; i < 9; i++) edges[i] = quantile_sorted(ls, n, i / 8.0);
	for (i = 0; i < 8; i++) {
		int cnt = 0; float lo = edges[i], hi = edges[i + 1];
		for (j = 0; j < n; j++) if (lv[j] >= lo && lv[j] <= hi) tmp[cnt++] = lv[j];
		if (cnt < 20) continue;
		float med = quantile_sel(tmp, cnt, 0.5);
		cnt = 0;
		for (j = 0; j < n; j++) if (lv[j] >= lo && lv[j] <= hi) tmp[cnt++] = sd[j];
		float p10 = quantile_sel(tmp, cnt, 0.10) * 1.12f;
		xs[nx] = med > 0 ? med : 0; vs[nx] = p10 * p10; nx++;
	}
	if (nx >= 3) {                        /* least-squares line, like np.polyfit(..., 1) */
		double sx = 0, sy = 0, sxx = 0, sxy = 0;
		for (i = 0; i < nx; i++) { sx += xs[i]; sy += vs[i]; sxx += (double)xs[i] * xs[i]; sxy += (double)xs[i] * vs[i]; }
		double den = nx * sxx - sx * sx;
		if (den != 0) {
			double a = (nx * sxy - sx * sy) / den, b = (sy - a * sx) / nx;
			*pa = (float)(a > 0 ? a : 0); *pb = (float)(b > 0.25 ? b : 0.25);
		}
	}
out:
	free(sd); free(lv); free(tmp); free(ls);
}


/* --------------------------------------------------------------- 1c. per-position gain + offset (eq=3)
 * The real JN1 mismatch is ~1% and mostly uniform (measured by the diag on real frames), so the model is
 *   measured_p = g_p * (colour mean of the tile) + o_p          (16 positions -> 32 numbers)
 * fitted on STRICTLY flat tiles (per colour: spread <= 10% of mean + 10 DN), across all brightness levels.
 * Robust: pass 1 least squares (ridge on g-1), pass 2 residual sigma, pass 3 refit without |res| > 3 sigma.
 * Region sums (centre third + 4 corner thirds) in pass 3 show whether the mismatch varies over the sensor.
 * Optional stored calibration: pass-3 sums accumulate in a file across shots and are used once trusted. */
#define GO_NREG 6                                 /* 0 all, 1 centre, 2..5 corners TL TR BL BR */
struct gofit { const uint16_t *in; int stride, pass; double g[16], o[16], sig[16];
               double (*p1)[16][5]; double (*p2)[16][2]; double (*p3)[GO_NREG][16][5]; };
static float g_gog[16], g_goo[16];
static int go_region(int ty, int tx)
{
	int ry = ty * 3 / g_h, rx = tx * 3 / g_w;
	if (ry == 1 && rx == 1) return 1;
	if (ry == 0 && rx == 0) return 2;
	if (ry == 0 && rx == 2) return 3;
	if (ry == 2 && rx == 0) return 4;
	if (ry == 2 && rx == 2) return 5;
	return -1;
}
static void gofit_job(int y0, int y1, void *p)    /* rows 64-aligned, one partial per job */
{
	struct gofit *f = p; int job = y0 / 64, tx, ty, px, py, c, k;
	if (f->pass == 1) memset(f->p1[job], 0, sizeof(f->p1[0]));
	if (f->pass == 2) memset(f->p2[job], 0, sizeof(f->p2[0]));
	if (f->pass == 3) memset(f->p3[job], 0, sizeof(f->p3[0]));
	for (ty = (y0 + 7) & ~7; ty < y1 && ty + 4 <= g_h; ty += 8)
		for (tx = 0; tx + 4 <= g_w; tx += 8) {
			double cs[3] = { 0, 0, 0 }; int cn[3] = { 0, 0, 0 }, mn[3] = { 1 << 20, 1 << 20, 1 << 20 }, mx[3] = { -1, -1, -1 }, ok = 1, v[16];
			for (py = 0; py < 4; py++)
				for (px = 0; px < 4; px++) {
					int a = f->in[(size_t)(ty + py) * f->stride + tx + px] - g_ped;
					c = g_qcol[py][px]; v[py * 4 + px] = a;
					cs[c] += a; cn[c]++;
					if (a < mn[c]) mn[c] = a;
					if (a > mx[c]) mx[c] = a;
				}
			double cm[3];
			for (c = 0; c < 3; c++) { cm[c] = cs[c] / cn[c]; if (cm[c] < 2 || mx[c] - mn[c] > 0.10 * cm[c] + 10) ok = 0; }
			if (!ok) continue;
			int reg = f->pass == 3 ? go_region(ty, tx) : -1;
			for (k = 0; k < 16; k++) {
				double x = cm[g_qcol[k >> 2][k & 3]], vv = v[k];
				if (f->pass == 1) { double *a = f->p1[job][k]; a[0] += 1; a[1] += x; a[2] += x * x; a[3] += vv; a[4] += x * vv; }
				else {
					double r = vv - (f->g[k] * x + f->o[k]);
					if (f->pass == 2) { f->p2[job][k][0] += 1; f->p2[job][k][1] += r * r; }
					else if (fabs(r) <= 3 * f->sig[k]) {
						int rr, regs[2] = { 0, reg };
						for (rr = 0; rr < 2; rr++) {
							if (regs[rr] < 0) continue;
							double *a = f->p3[job][regs[rr]][k]; a[0] += 1; a[1] += x; a[2] += x * x; a[3] += vv; a[4] += x * vv;
						}
					}
				}
			}
		}
}
static int go_solve(const double a[5], double *g, double *o)   /* ridge 1e-3 on (g-1), like fit_go() */
{
	double n = a[0], sx = a[1], sxx = a[2], sv = a[3], sxv = a[4], lam = 1e-3 * sxx;
	double A = sxx + lam, B = sx, C = sx, D = n, r0 = sxv + lam, r1 = sv, det = A * D - B * C;
	if (n < 500 || fabs(det) < 1e-12) { *g = 1; *o = 0; return 0; }
	*g = (r0 * D - B * r1) / det; *o = (A * r1 - C * r0) / det;
	return 1;
}
static const char *g_calpath = "/data/vendor/camera/remoshim_cal.bin";
static char g_candmsg[700];
static void cand_fit(const uint16_t *in, int stride, const double gg[16], const double go[16], const double gsig[16], char *msg, size_t msgsz);
static int g_usecal = 1, g_calused = 0;
/* v9 settings: pdcolor (0 off, default; 1 = per-frame mate correction), pdcrho (ridge weight toward the running coefficients,
 * thousandths of the data weight), eqmin (equalisation refit is skipped when the flat tiles average less than this many DN of
 * green above the pedestal; 0 = never skip). g_pdc_on = pdcolor corrected the mates in this frame; g_accpdc = the value it had
 * when the map16 sums were accumulated (the map must not mix frames with and without the mate correction). */
static int g_pdcolor = 0, g_pdcrho = 20, g_eqmin = 12, g_pdc_on = 0, g_accpdc = 0, g_pdab = 0, g_pdcflat = 1;
static long g_pdc_cnt[2];
/* pdcolor=2 is an A/B mode for testing without changing the property between shots: the 1st, 3rd, 5th... shot of a process are
 * CORRECTED, the 2nd, 4th... UNCORRECTED, and every shot says which in the log. The map16 sums are reset whenever the mate
 * correction flips (g_accpdc), so each shot gets its own per-frame map and the two arms are treated alike. */
struct calfile { uint32_t magic, ver, w, h, bayer, shots; double s[16][5]; };
/* returns 1 when the gains were refitted, 0 when the frame carries too little signal and the previous gains were kept */
static int go_fit(const uint16_t *in, int stride, char *msg, size_t msgsz)
{
	struct gofit f; int nj = (g_h + 63) / 64, j, k, r, m, ret = 1; float sg[16], so[16];
	memset(&f, 0, sizeof f); f.in = in; f.stride = stride;
	f.p1 = malloc(sizeof(*f.p1) * nj); f.p2 = malloc(sizeof(*f.p2) * nj); f.p3 = malloc(sizeof(*f.p3) * nj);
	for (k = 0; k < 16; k++) { sg[k] = g_gog[k]; so[k] = g_goo[k]; g_gog[k] = 1; g_goo[k] = 0; }
	if (!f.p1 || !f.p2 || !f.p3) { snprintf(msg, msgsz, "alloc failed"); goto out; }
	double S1[16][5], S3[GO_NREG][16][5];
	f.pass = 1; run_rows(gofit_job, &f, 64);
	memset(S1, 0, sizeof S1);
	for (j = 0; j < nj; j++) for (k = 0; k < 16; k++) for (m = 0; m < 5; m++) S1[k][m] += f.p1[j][k][m];
	{   /* total-signal gate: the flat-tile test only needs each colour mean >= 2 DN, so in a near-black frame noise alone picks the
		 * tiles and the regression is biased (gains 0.74..1.19 seen on a black frame). Keep the previous gains instead. */
	  double gs = 0, gn = 0, gmean;
	  for (k = 0; k < 16; k++) if (g_qcol[k >> 2][k & 3] == CG && S1[k][0] > 0) { gs += S1[k][1]; gn += S1[k][0]; }
	  gmean = gn > 0 ? gs / gn : 0;
	  if (g_eqmin > 0 && gmean < g_eqmin) {
		int have = sg[0] > 0.5f;
		for (k = 0; k < 16; k++) { g_gog[k] = have ? sg[k] : 1.0f; g_goo[k] = have ? so[k] : 0.0f; }
		snprintf(msg, msgsz, "gain+offset fit SKIPPED: flat tiles average %.1f DN of green above the pedestal (eqmin %d), keeping %s gains, shot not counted",
		         gmean, g_eqmin, have ? "the previous" : "unity");
		snprintf(g_candmsg, sizeof g_candmsg, "candidates: not refitted (low-signal frame)");
		ret = 0; goto out;
	  } }
	for (k = 0; k < 16; k++) go_solve(S1[k], &f.g[k], &f.o[k]);
	f.pass = 2; run_rows(gofit_job, &f, 64);
	for (k = 0; k < 16; k++) {
		double n = 0, ss = 0;
		for (j = 0; j < nj; j++) { n += f.p2[j][k][0]; ss += f.p2[j][k][1]; }
		f.sig[k] = n > 0 ? sqrt(ss / n) : 1e9;
	}
	f.pass = 3; run_rows(gofit_job, &f, 64);
	memset(S3, 0, sizeof S3);
	for (j = 0; j < nj; j++) for (r = 0; r < GO_NREG; r++) for (k = 0; k < 16; k++) for (m = 0; m < 5; m++) S3[r][k][m] += f.p3[j][r][k][m];
	/* per-frame result */
	double gg[16], oo[16];
	for (k = 0; k < 16; k++) go_solve(S3[0][k], &gg[k], &oo[k]);
	cand_fit(in, stride, gg, oo, f.sig, g_candmsg, sizeof g_candmsg);
	/* centre vs corners */
	double dg = 0, dofs = 0; int regok = 1;
	{ double gc[16], oc[16];
	  for (k = 0; k < 16; k++) if (!go_solve(S3[1][k], &gc[k], &oc[k])) regok = 0;
	  for (r = 2; r < GO_NREG && regok; r++)
		for (k = 0; k < 16; k++) {
			double g2, o2;
			if (!go_solve(S3[r][k], &g2, &o2)) { regok = 0; break; }
			/* compare predicted value at a mid level (300 DN) rather than raw g/o, which trade off */
			double d = fabs((g2 * 300 + o2) - (gc[k] * 300 + oc[k])) / 3.0;   /* % of 300 DN */
			if (d > dg) dg = d;
			if (fabs(o2 - oc[k]) > dofs) dofs = fabs(o2 - oc[k]);
		}
	}
	/* stored calibration: accumulate pass-3 sums, use them once trusted */
	struct calfile cf; int have = 0, wrote = 0, used = 0;
	if (g_usecal) {
		FILE *fp = fopen(g_calpath, "rb");
		if (fp) {
			if (fread(&cf, sizeof cf, 1, fp) == 1 && cf.magic == 0x51324343u && cf.ver == 1 &&
			    cf.w == (uint32_t)g_w && cf.h == (uint32_t)g_h && cf.bayer == (uint32_t)g_bayer) have = 1;
			fclose(fp);
		}
		if (!have) { memset(&cf, 0, sizeof cf); cf.magic = 0x51324343u; cf.ver = 1; cf.w = g_w; cf.h = g_h; cf.bayer = g_bayer; }
		if (S3[0][0][0] >= 500) {
			for (k = 0; k < 16; k++) for (m = 0; m < 5; m++) cf.s[k][m] += S3[0][k][m];
			cf.shots++;
			fp = fopen(g_calpath, "wb");
			if (fp) { wrote = fwrite(&cf, sizeof cf, 1, fp) == 1; fclose(fp); }
			if (!wrote) { cf.shots--; for (k = 0; k < 16; k++) for (m = 0; m < 5; m++) cf.s[k][m] -= S3[0][k][m]; }
		}
		if (cf.shots >= 3 && cf.s[0][0] >= 1e6) {
			for (k = 0; k < 16; k++) go_solve(cf.s[k], &gg[k], &oo[k]);
			used = 1;
		}
	}
	g_calused = used;
	{
	}
	double gmin = 9, gmax = -9, omax = 0;
	for (k = 0; k < 16; k++) {
		g_gog[k] = (float)gg[k]; g_goo[k] = (float)oo[k];
		if (gg[k] < gmin) gmin = gg[k];
		if (gg[k] > gmax) gmax = gg[k];
		if (fabs(oo[k]) > omax) omax = fabs(oo[k]);
	}
	snprintf(msg, msgsz, "gain+offset fit on %.0f tiles, gain %.4f..%.4f, |offset| <= %.2f DN; centre-vs-corner diff %s%.2f%% (offset %.2f DN); "
	         "calibration: %s, %u shots stored, %s",
	         S3[0][0][0], gmin, gmax, omax, regok ? "" : "n/a ", dg, dofs,
	         used ? "USING stored" : "per-frame", cf.shots, !g_usecal ? "disabled" : wrote ? "file updated" : "file NOT writable");
out:
	free(f.p1); free(f.p2); free(f.p3);
	return ret;
}

/* --------------------------------------------------------------- 1d. candidate correction models (eq=3)
 * Four gain+offset models, from global to a 12x8 grid of sensor regions with bilinear blending between
 * region centres. Fitted on half of the strictly flat tiles (checkerboard of 8x8 tile cells) and scored on
 * the other half, so finer grids cannot win by overfitting. Grids nest (12x8 -> 6x4 -> 3x2 -> 1x1), and a
 * region with too few tiles inherits its parent's values. persist.vendor.remoshim.grid: -1 = use the best
 * candidate (default), 0..3 = force global / 3x2 / 6x4 / 12x8. */
#define GX 12
#define GY 8
#define NCAND 4
static const int g_cgx[NCAND] = { 1, 3, 6, 12 }, g_cgy[NCAND] = { 1, 2, 4, 8 };
static int g_grid = -1, g_gridused = 0;
static float g_cg[GY][GX][16], g_co[GY][GX][16];        /* coefficients of the model in use, per region */
static int g_cgw = 1, g_cgh = 1;                         /* its grid size */
static double (*g_cand_g)[GY][GX][16], (*g_cand_o)[GY][GX][16];   /* [NCAND] */
struct cand { const uint16_t *in; int stride; double gsig[16], gg[16], go[16];
              double (*cs)[GY * GX][16][5];           /* per job: fine-cell fit sums */
              double (*ev)[NCAND][5][17]; };          /* per job: per cand, band: 16 sums + count */
static inline int tile_flat(const uint16_t *in, int S, int ty, int tx, int v[16], double cm[3])
{
	double cs[3] = { 0, 0, 0 }; int cn[3] = { 0, 0, 0 }, mn[3] = { 1 << 20, 1 << 20, 1 << 20 }, mx[3] = { -1, -1, -1 }, px, py, c;
	for (py = 0; py < 4; py++)
		for (px = 0; px < 4; px++) {
			int a = in[(size_t)(ty + py) * S + tx + px] - g_ped;
			c = g_qcol[py][px]; v[py * 4 + px] = a; cs[c] += a; cn[c]++;
			if (a < mn[c]) mn[c] = a;
			if (a > mx[c]) mx[c] = a;
		}
	for (c = 0; c < 3; c++) { cm[c] = cs[c] / cn[c]; if (cm[c] < 2 || mx[c] - mn[c] > 0.10 * cm[c] + 10) return 0; }
	return 1;
}
static inline int tile_ok(struct cand *f, const int v[16], const double cm[3])   /* 3-sigma vs global fit */
{
	int k;
	for (k = 0; k < 16; k++) {
		double x = cm[g_qcol[k >> 2][k & 3]];
		if (fabs(v[k] - (f->gg[k] * x + f->go[k])) > 3 * f->gsig[k]) return 0;
	}
	return 1;
}
static void cand_fit_job(int y0, int y1, void *p)
{
	struct cand *f = p; int job = y0 / 64, tx, ty, k, v[16]; double cm[3];
	memset(f->cs[job], 0, sizeof(f->cs[0]));
	for (ty = (y0 + 7) & ~7; ty < y1 && ty + 4 <= g_h; ty += 8)
		for (tx = 0; tx + 4 <= g_w; tx += 8) {
			if ((((ty >> 6) + (tx >> 6)) & 1) != 0) continue;           /* fit half: 64x64-px checkerboard */
			if (!tile_flat(f->in, f->stride, ty, tx, v, cm) || !tile_ok(f, v, cm)) continue;
			int cell = (ty * GY / g_h) * GX + (tx * GX / g_w);
			for (k = 0; k < 16; k++) {
				double x = cm[g_qcol[k >> 2][k & 3]], vv = v[k], *a = f->cs[job][cell][k];
				a[0] += 1; a[1] += x; a[2] += x * x; a[3] += vv; a[4] += x * vv;
			}
		}
}
/* coefficient of candidate c at sensor position (y, x): bilinear between region centres */
static inline void cand_coef(int c, double y, double x, int k, double *g, double *o)
{
	int gw = g_cgx[c], gh = g_cgy[c];
	double fy = y * gh / g_h - 0.5, fx = x * gw / g_w - 0.5;
	int y0 = (int)floor(fy), x0 = (int)floor(fx); double wy = fy - y0, wx = fx - x0;
	int y1 = y0 + 1, x1 = x0 + 1;
	if (y0 < 0) { y0 = 0; wy = 0; }
	if (y1 > gh - 1) { y1 = gh - 1; }
	if (y0 > gh - 1) { y0 = gh - 1; wy = 0; }
	if (x0 < 0) { x0 = 0; wx = 0; }
	if (x1 > gw - 1) { x1 = gw - 1; }
	if (x0 > gw - 1) { x0 = gw - 1; wx = 0; }
	double (*G)[GX][16] = g_cand_g[c], (*O)[GX][16] = g_cand_o[c];
	*g = (1 - wy) * ((1 - wx) * G[y0][x0][k] + wx * G[y0][x1][k]) + wy * ((1 - wx) * G[y1][x0][k] + wx * G[y1][x1][k]);
	*o = (1 - wy) * ((1 - wx) * O[y0][x0][k] + wx * O[y0][x1][k]) + wy * ((1 - wx) * O[y1][x0][k] + wx * O[y1][x1][k]);
}
static const int g_band[6] = { 0, 100, 300, 600, 850, 2000 };
static void cand_eval_job(int y0, int y1, void *p)
{
	struct cand *f = p; int job = y0 / 64, tx, ty, k, c, b, v[16]; double cm[3];
	memset(f->ev[job], 0, sizeof(f->ev[0]));
	for (ty = (y0 + 7) & ~7; ty < y1 && ty + 4 <= g_h; ty += 8)
		for (tx = 0; tx + 4 <= g_w; tx += 8) {
			if ((((ty >> 6) + (tx >> 6)) & 1) != 1) continue;           /* evaluation half */
			if (!tile_flat(f->in, f->stride, ty, tx, v, cm) || !tile_ok(f, v, cm)) continue;
			double gl = cm[CG];
			for (b = 0; b < 5; b++) if (gl >= g_band[b] && gl < g_band[b + 1]) break;
			if (b == 5) continue;
			for (c = 0; c < NCAND; c++) {
				double cv[16], s3[3] = { 0, 0, 0 }; int n3[3] = { 0, 0, 0 };
				for (k = 0; k < 16; k++) {
					double g, o; cand_coef(c, ty + 2.0, tx + 2.0, k, &g, &o);
					cv[k] = (v[k] - o) / g; int col = g_qcol[k >> 2][k & 3]; s3[col] += cv[k]; n3[col]++;
				}
				for (k = 0; k < 16; k++) { int col = g_qcol[k >> 2][k & 3]; f->ev[job][c][b][k] += cv[k] / (s3[col] / n3[col]); }
				f->ev[job][c][b][16] += 1;
			}
		}
}
static void cand_fit(const uint16_t *in, int stride, const double gg[16], const double go[16], const double gsig[16], char *msg, size_t msgsz)
{
	struct cand f; int nj = (g_h + 63) / 64, j, k, m, c, b, cy, cx;
	memset(&f, 0, sizeof f); f.in = in; f.stride = stride;
	for (k = 0; k < 16; k++) { f.gg[k] = gg[k]; f.go[k] = go[k]; f.gsig[k] = gsig[k]; }
	f.cs = malloc(sizeof(*f.cs) * nj); f.ev = malloc(sizeof(*f.ev) * nj);
	if (!g_cand_g) { g_cand_g = malloc(sizeof(*g_cand_g) * NCAND); g_cand_o = malloc(sizeof(*g_cand_o) * NCAND); }
	if (!f.cs || !f.ev || !g_cand_g || !g_cand_o) { snprintf(msg, msgsz, "candidates: alloc failed"); g_gridused = -1; goto out; }
	run_rows(cand_fit_job, &f, 64);
	static double S[GY * GX][16][5];
	memset(S, 0, sizeof S);
	for (j = 0; j < nj; j++) for (c = 0; c < GY * GX; c++) for (k = 0; k < 16; k++) for (m = 0; m < 5; m++) S[c][k][m] += f.cs[j][c][k][m];
	/* hierarchical fits: candidate c groups fine cells; sparse regions inherit the parent (previous c) */
	for (c = 0; c < NCAND; c++) {
		int gw = g_cgx[c], gh = g_cgy[c], ry, rx;
		for (ry = 0; ry < gh; ry++)
			for (rx = 0; rx < gw; rx++) {
				double a[16][5]; memset(a, 0, sizeof a);
				for (cy = ry * GY / gh; cy < (ry + 1) * GY / gh; cy++)
					for (cx = rx * GX / gw; cx < (rx + 1) * GX / gw; cx++)
						for (k = 0; k < 16; k++) for (m = 0; m < 5; m++) a[k][m] += S[cy * GX + cx][k][m];
				for (k = 0; k < 16; k++) {
					double g, o;
					if (a[k][0] >= 300 && go_solve(a[k], &g, &o)) { g_cand_g[c][ry][rx][k] = g; g_cand_o[c][ry][rx][k] = o; }
					else if (c == 0) { g_cand_g[c][ry][rx][k] = gg[k]; g_cand_o[c][ry][rx][k] = go[k]; }
					else {   /* parent region of the previous (coarser) candidate */
						int py = (int)((ry + 0.5) * g_cgy[c - 1] / gh), px = (int)((rx + 0.5) * g_cgx[c - 1] / gw);
						g_cand_g[c][ry][rx][k] = g_cand_g[c - 1][py][px][k]; g_cand_o[c][ry][rx][k] = g_cand_o[c - 1][py][px][k];
					}
				}
			}
	}
	run_rows(cand_eval_job, &f, 64);
	static double E[NCAND][5][17];
	memset(E, 0, sizeof E);
	for (j = 0; j < nj; j++) for (c = 0; c < NCAND; c++) for (b = 0; b < 5; b++) for (k = 0; k < 17; k++) E[c][b][k] += f.ev[j][c][b][k];
	double score[NCAND]; int o2 = 0;
	o2 += snprintf(msg + o2, msgsz - o2, "candidates, holdout pattern rms%% per band [0-100,100-300,300-600,600-850,850+]:");
	for (c = 0; c < NCAND; c++) {
		double ssum = 0; int sn = 0, bright = 0;
		o2 += snprintf(msg + o2, msgsz - o2, " %dx%d", g_cgx[c], g_cgy[c]);
		for (b = 0; b < 5; b++) {
			if (E[c][b][16] < 200) { o2 += snprintf(msg + o2, msgsz - o2, "%s-", b ? "/" : " "); continue; }
			double ss = 0;
			for (k = 0; k < 16; k++) { double d = E[c][b][k] / E[c][b][16] - 1; ss += d * d; }
			double r = 100 * sqrt(ss / 16);
			o2 += snprintf(msg + o2, msgsz - o2, "%s%.2f", b ? "/" : " ", r);
			if (b >= 1) { ssum += r; sn++; bright = 1; }
		}
		if (!bright) for (b = 0; b < 5; b++) if (E[c][b][16] >= 200) {   /* dark-only scene: use what exists */
			double ss = 0; for (k = 0; k < 16; k++) { double d = E[c][b][k] / E[c][b][16] - 1; ss += d * d; }
			ssum += 100 * sqrt(ss / 16); sn++;
		}
		score[c] = sn ? ssum / sn : 1e9;
	}
	int best = 0;
	for (c = 1; c < NCAND; c++) if (score[c] < score[best] - 0.01) best = c;   /* finer only if clearly better */
	g_gridused = (g_grid >= 0 && g_grid < NCAND) ? g_grid : best;
	g_cgw = g_cgx[g_gridused]; g_cgh = g_cgy[g_gridused];
	for (cy = 0; cy < g_cgh; cy++) for (cx = 0; cx < g_cgw; cx++) for (k = 0; k < 16; k++) {
		g_cg[cy][cx][k] = (float)g_cand_g[g_gridused][cy][cx][k]; g_co[cy][cx][k] = (float)g_cand_o[g_gridused][cy][cx][k];
	}
	snprintf(msg + o2, msgsz - o2, "; using %dx%d%s", g_cgw, g_cgh, (g_grid >= 0 && g_grid < NCAND) ? " (forced)" : " (best)");
out:
	free(f.cs); free(f.ev);
}
/* apply the chosen model: per row, coefficients for each 4-px block via bilinear blending */
static void cand_apply(int y0, int y1, void *p)
{
	struct ctx *c = p; const int S = c->stride; int x, y, k, blk, nb = (g_w + 3) / 4;
	float *gk = malloc(sizeof(float) * nb * 8);
	if (!gk) return;
	for (y = y0; y < y1; y++) {
		const uint16_t *s = c->src + (size_t)y * S; uint16_t *d = c->dst + (size_t)y * S;
		for (blk = 0; blk < nb; blk++)
			for (k = 0; k < 4; k++) {
				double g, o; cand_coef(g_gridused, y, blk * 4 + 1.5, (y & 3) * 4 + k, &g, &o);
				gk[blk * 8 + k] = (float)g; gk[blk * 8 + 4 + k] = (float)o;
			}
		for (x = 0; x < g_w; x++) {
			const float *t = gk + (x >> 2) * 8;
			float v = (s[x] - g_ped - t[4 + (x & 3)]) / t[x & 3] + g_ped;
			d[x] = (uint16_t)clampi((int)lrintf(v * FX), 0, WHITE * FX);
		}
	}
	free(gk);
}

static void go_apply(int y0, int y1, void *p)
{
	struct ctx *c = p; const int S = c->stride; int x, y;
	for (y = y0; y < y1; y++) {
		const uint16_t *s = c->src + (size_t)y * S; uint16_t *d = c->dst + (size_t)y * S;
		const float *gg = g_gog + (y & 3) * 4, *oo = g_goo + (y & 3) * 4;
		for (x = 0; x < g_w; x++) {
			float v = (s[x] - g_ped - oo[x & 3]) / gg[x & 3] + g_ped;
			d[x] = (uint16_t)clampi((int)lrintf(v * FX), 0, WHITE * FX);
		}
	}
}


/* --------------------------------------------------------------- 1e. 16x16 PDAF-lattice gain map (stage 2)
 * The JN1's PDAF structures affect a whole 16x16 tile: the PD pixels themselves (removed on-chip by the v4 driver,
 * or replaced by the PDAF repair above 15%) and a wide ring of weak +-2..6% deviations. After the 4x4 equalisation
 * (stage 1, which may vary over the sensor) this stage fits ONE global map over the 16x16 tile:
 *   v_p = g_p * x + o_p,  x = mean of the 16 same-quad-phase pixels in the flat 16x16 tile     (256 positions)
 * Flat = per colour, the four 8x8 quarter means agree within 5% + 4 DN (a per-pixel spread test would reject the
 * very tiles that contain the lattice). Robust: LS (ridge on g-1), sigma, refit without |res| > 3 sigma.
 * The fit sums are accumulated across shots in memory (the camera service process stays up), so the map
 * converges to the sensor's fixed pattern. Matches fit16()/apply16() in bench.py.
 * persist.vendor.remoshim.tile16: 1 = on (default); persist.vendor.remoshim.acc: 1 = accumulate (default). */
static int g_t16 = 1, g_acc = 1;
static float g_t16g[16][16], g_t16o[16][16];
static double g_accs[256][5]; static int g_accshots = 0, g_accw, g_acch, g_accb;
struct t16 { const uint16_t *b; int S, pass; double g[256], o[256], sig[256];
             double (*p1)[256][5]; double (*p2)[256][2]; double (*p3)[6][256][5]; };
static int t16_tile(const uint16_t *b, int S, int ty, int tx, double v[256], double x[256])
{
	double qsum[3][4] = { { 0 } }, csum[3] = { 0 }; int qn[3][4] = { { 0 } }, cn[3] = { 0 }, py, px, c, k;
	double ph[16] = { 0 };
	for (py = 0; py < 16; py++)
		for (px = 0; px < 16; px++) {
			double a = b[(size_t)(ty + py) * S + tx + px] * IFX - g_ped;
			c = g_qcol[py & 3][px & 3]; v[py * 16 + px] = a;
			int qd = (py >> 3) * 2 + (px >> 3);
			qsum[c][qd] += a; qn[c][qd]++; csum[c] += a; cn[c]++; ph[(py & 3) * 4 + (px & 3)] += a;
		}
	for (c = 0; c < 3; c++) {
		double m = csum[c] / cn[c], mn = 1e30, mx = -1e30;
		if (m < 2) return 0;
		for (k = 0; k < 4; k++) { double qm = qsum[c][k] / qn[c][k]; if (qm < mn) mn = qm; if (qm > mx) mx = qm; }
		if (mx - mn > 0.05 * m + 4) return 0;
	}
	for (py = 0; py < 16; py++) for (px = 0; px < 16; px++) x[py * 16 + px] = ph[(py & 3) * 4 + (px & 3)] / 16.0;
	return 1;
}
static void t16_job(int y0, int y1, void *p)
{
	struct t16 *f = p; int job = y0 / 64, ty, tx, k; double v[256], x[256];
	if (f->pass == 1) memset(f->p1[job], 0, sizeof(f->p1[0]));
	if (f->pass == 2) memset(f->p2[job], 0, sizeof(f->p2[0]));
	if (f->pass == 3) memset(f->p3[job], 0, sizeof(f->p3[0]));
	for (ty = (y0 + 15) & ~15; ty < y1 && ty + 16 <= g_h; ty += 16)
		for (tx = 0; tx + 16 <= g_w; tx += 16) {
			if (!t16_tile(f->b, f->S, ty, tx, v, x)) continue;
			int reg = f->pass == 3 ? go_region(ty, tx) : -1;
			for (k = 0; k < 256; k++) {
				if (f->pass == 1) { double *a = f->p1[job][k]; a[0] += 1; a[1] += x[k]; a[2] += x[k] * x[k]; a[3] += v[k]; a[4] += x[k] * v[k]; continue; }
				double r = v[k] - (f->g[k] * x[k] + f->o[k]);
				if (f->pass == 2) { f->p2[job][k][0] += 1; f->p2[job][k][1] += r * r; continue; }
				if (fabs(r) > 3 * f->sig[k]) continue;
				int rr, regs[2] = { 0, reg };
				for (rr = 0; rr < 2; rr++) {
					if (regs[rr] < 0) continue;
					double *a = f->p3[job][regs[rr]][k]; a[0] += 1; a[1] += x[k]; a[2] += x[k] * x[k]; a[3] += v[k]; a[4] += x[k] * v[k];
				}
			}
		}
}
static int t16_solve(const double a[5], double minn, double *g, double *o)
{
	double n = a[0], sx = a[1], sxx = a[2], sv = a[3], sxv = a[4], lam = 1e-3 * sxx;
	double A = sxx + lam, B = sx, D = n, r0 = sxv + lam, r1 = sv, det = A * D - B * B;
	if (n < minn || sxx <= 0) return 0;
	if (sxx - sx * sx / n < 0.05 * sxx) { *g = sxv / sxx; *o = 0; return 1; }   /* narrow range: gain only */
	if (fabs(det) < 1e-12) return 0;
	*g = (r0 * D - B * r1) / det; *o = (A * r1 - B * r0) / det;
	return 1;
}
static int g_t16shot = 0;
static void t16_fit(const uint16_t *b, int S, char *msg, size_t msgsz)
{
	/* calibrated: the map is a fixed sensor property; reuse it and only refit every 5th shot */
	g_t16shot++;
	if (g_acc && g_accshots >= 8 && g_accw == g_w && g_acch == g_h && g_accb == g_bayer && g_accpdc == g_pdc_on && g_t16shot % 5 != 0) {
		snprintf(msg, msgsz, "map16: reused (calibrated from %d shots)", g_accshots);
		return;
	}
	struct t16 f; int nj = (g_h + 63) / 64, j, k, m, r;
	static double S1[256][5], S3[6][256][5];
	memset(&f, 0, sizeof f); f.b = b; f.S = S;
	for (k = 0; k < 256; k++) { g_t16g[k >> 4][k & 15] = 1; g_t16o[k >> 4][k & 15] = 0; }
	f.p1 = malloc(sizeof(*f.p1) * nj); f.p2 = malloc(sizeof(*f.p2) * nj); f.p3 = malloc(sizeof(*f.p3) * nj);
	if (!f.p1 || !f.p2 || !f.p3) { snprintf(msg, msgsz, "map16: alloc failed"); goto out; }
	f.pass = 1; run_rows(t16_job, &f, 64);
	memset(S1, 0, sizeof S1);
	for (j = 0; j < nj; j++) for (k = 0; k < 256; k++) for (m = 0; m < 5; m++) S1[k][m] += f.p1[j][k][m];
	double ntile = S1[0][0];
	if (ntile < 200) { snprintf(msg, msgsz, "map16: only %.0f flat 16x16 tiles, map not changed this shot", ntile); }
	for (k = 0; k < 256; k++) { f.g[k] = 1; f.o[k] = 0; t16_solve(S1[k], 200, &f.g[k], &f.o[k]); }
	f.pass = 2; run_rows(t16_job, &f, 64);
	for (k = 0; k < 256; k++) {
		double n = 0, ss = 0;
		for (j = 0; j < nj; j++) { n += f.p2[j][k][0]; ss += f.p2[j][k][1]; }
		f.sig[k] = n > 0 ? sqrt(ss / n) : 1e30;
	}
	f.pass = 3; run_rows(t16_job, &f, 64);
	memset(S3, 0, sizeof S3);
	for (j = 0; j < nj; j++) for (r = 0; r < 6; r++) for (k = 0; k < 256; k++) for (m = 0; m < 5; m++) S3[r][k][m] += f.p3[j][r][k][m];
	/* this frame's map (as fit16(): refit if enough tiles survive, else keep pass 1) */
	double gf[256], of[256];
	for (k = 0; k < 256; k++) { gf[k] = f.g[k]; of[k] = f.o[k]; t16_solve(S3[0][k], 200, &gf[k], &of[k]); }
	/* accumulate across shots (sensor property): sums decay 0.9 per shot, ~10-shot memory */
	int using_acc = 0;
	if (g_acc) {
		if (g_accw != g_w || g_acch != g_h || g_accb != g_bayer || g_accpdc != g_pdc_on) { memset(g_accs, 0, sizeof g_accs); g_accshots = 0; g_accw = g_w; g_acch = g_h; g_accb = g_bayer; g_accpdc = g_pdc_on; }
		if (ntile >= 200) {
			for (k = 0; k < 256; k++) for (m = 0; m < 5; m++) g_accs[k][m] = 0.9 * g_accs[k][m] + S3[0][k][m];
			g_accshots++;
		}
		if (g_accshots >= 2) {
			for (k = 0; k < 256; k++) t16_solve(g_accs[k], 200, &gf[k], &of[k]);
			using_acc = 1;
		}
	}
	if (ntile < 200 && !using_acc) goto out;
	double gmin = 9, gmax = -9, dmax = 0;
	for (k = 0; k < 256; k++) {
		g_t16g[k >> 4][k & 15] = (float)gf[k]; g_t16o[k >> 4][k & 15] = (float)of[k];
		if (gf[k] < gmin) gmin = gf[k];
		if (gf[k] > gmax) gmax = gf[k];
	}
	/* centre vs corners, compared at each corner's own mean level */
	int regok = 1; double dss = 0; int dn = 0;
	for (r = 2; r < 6 && regok; r++)
		for (k = 0; k < 256; k++) {
			double gc, oc, g2, o2;
			if (!t16_solve(S3[1][k], 200, &gc, &oc) || !t16_solve(S3[r][k], 200, &g2, &o2)) { regok = 0; break; }
			double X = S3[r][k][1] / S3[r][k][0];
			double d = ((g2 * X + o2) - (gc * X + oc)) / (X > 1 ? X : 1) * 100;
			dss += d * d; dn++;
		}
	dmax = dn ? sqrt(dss / dn) : 0;
	snprintf(msg, msgsz, "map16: %.0f flat tiles this shot, gains %.3f..%.3f, %s (%d shots), centre-vs-corner rms diff %s%.2f%%",
	         ntile, gmin, gmax, using_acc ? "accumulated" : "this shot only", g_accshots, regok ? "" : "n/a ", dmax);
out:
	free(f.p1); free(f.p2); free(f.p3);
}
static void t16_apply(int y0, int y1, void *p)
{
	struct ctx *c = p; const int S = c->stride; int x, y;
	for (y = y0; y < y1; y++) {
		uint16_t *d = c->dst + (size_t)y * S;
		const float *gg = g_t16g[y & 15], *oo = g_t16o[y & 15];
		for (x = 0; x < g_w; x++) {
			float v = (d[x] * IFX - g_ped - oo[x & 15]) / gg[x & 15] + g_ped;
			d[x] = (uint16_t)clampi((int)lrintf(v * FX), 0, WHITE * FX);
		}
	}
}
/* lattice diagnostic: rms (%) of the 256 position means in flat 16x16 tiles, relative to their group mean
 * (group = quad phase for raw frames, Bayer colour for our output) */
static void diag16(const uint16_t *b, int S, float scl, int bayer_out, const char *tag, char *out, size_t outsz)
{
	double sum[256] = { 0 }; long n = 0; int ty, tx, k, py, px;
	for (ty = 0; ty + 16 <= g_h; ty += 16)
		for (tx = 0; tx + 16 <= g_w; tx += 16) {
			double v[256], gs[16] = { 0 }; int gn[16] = { 0 }, ok = 1, c;
			double csum[3] = { 0 }, qs[3][4] = { { 0 } }; int cn[3] = { 0 }, qn[3][4] = { { 0 } };
			for (py = 0; py < 16; py++)
				for (px = 0; px < 16; px++) {
					double a = b[(size_t)(ty + py) * S + tx + px] * scl - g_ped; v[py * 16 + px] = a;
					int grp = bayer_out ? g_tcol[py & 1][px & 1] : (py & 3) * 4 + (px & 3);
					gs[grp] += a; gn[grp]++;
					c = bayer_out ? g_tcol[py & 1][px & 1] : g_qcol[py & 3][px & 3];
					int qd = (py >> 3) * 2 + (px >> 3); csum[c] += a; cn[c]++; qs[c][qd] += a; qn[c][qd]++;
				}
			for (c = 0; c < 3; c++) {
				double m = csum[c] / cn[c], mn = 1e30, mx = -1e30; int q;
				if (m < 20) { ok = 0; break; }
				for (q = 0; q < 4; q++) { double qm = qs[c][q] / qn[c][q]; if (qm < mn) mn = qm; if (qm > mx) mx = qm; }
				if (mx - mn > 0.05 * m + 4) ok = 0;
			}
			if (!ok) continue;
			for (py = 0; py < 16; py++)
				for (px = 0; px < 16; px++) {
					int grp = bayer_out ? g_tcol[py & 1][px & 1] : (py & 3) * 4 + (px & 3);
					sum[py * 16 + px] += v[py * 16 + px] / (gs[grp] / gn[grp]);
				}
			n++;
		}
	if (n < 50) { snprintf(out, outsz, "%s n=%ld -", tag, n); return; }
	double ss = 0, sc[3] = { 0, 0, 0 }; int nc[3] = { 0, 0, 0 }; unsigned char cls[256]; memset(cls, 0, sizeof cls);
	for (k = 0; k < 8; k++) { cls[kPdPos[k][0] * 16 + kPdPos[k][1]] = 1; cls[kPdPos[k][0] * 16 + (kPdPos[k][1] ^ 1)] = 2; }
	for (k = 0; k < 256; k++) { double d = sum[k] / n - 1; ss += d * d; sc[cls[k]] += d * d; nc[cls[k]]++; }
	snprintf(out, outsz, "%s n=%ld %.3f%%", tag, n, 100 * sqrt(ss / 256));
	/* v11: the same deviations split by position class (rms within each class, so the 8 PD and 8 mate positions are not diluted
	 * among 256): the all-position figure above is sqrt((8*PD^2 + 8*mate^2 + 240*other^2) / 256). */
	LOGI("lattice16 split (%s): PD(8) %.3f%%  mates(8) %.3f%%  other(240) %.3f%%", tag, 100 * sqrt(sc[1] / nc[1]), 100 * sqrt(sc[2] / nc[2]), 100 * sqrt(sc[0] / nc[0]));
}

/* --------------------------------------------------------------- 1b. crosstalk (leakage) model
 * Per 4x4 position p:  measured_p = a_p * C_own + l1_p * C_other1 + l2_p * C_other2
 * fitted by ridge least squares on the colour MEANS of flat 4x4 tiles (every 8 px), so it cannot
 * learn a blur. Flat test is noise-aware (noise model estimated on the raw frame first).
 * Correction per pixel: (v - l1*L1 - l2*L2) / a, where L = mean of that colour's samples in the
 * 5x5 neighbourhood (centre excluded, scipy 'mirror' edges). Matches equalise_leak() in bench.py. */
static float g_lk[4][4][3];                    /* a, l1, l2 per position */
static int g_oc[3][2] = { { CG, CB }, { CR, CB }, { CR, CG } };   /* other colours, ascending */
struct lkfit { const uint16_t *in; int stride; float na, nb; double (*part)[4][4][12]; long *nflat; };
static void lkfit_job(int y0, int y1, void *p)  /* rows 64-aligned; one partial per job, merged in order */
{
	struct lkfit *f = p; int job = y0 / 64, tx, ty, px, py, c, k;
	double (*A)[4][12] = f->part[job]; long nfl = 0;
	memset(A, 0, sizeof(double) * 4 * 4 * 12);
	int h4 = g_h & ~3, w4 = g_w & ~3;
	for (ty = (y0 + 7) & ~7; ty < y1 && ty + 4 <= h4; ty += 8)
		for (tx = 0; tx + 4 <= w4; tx += 8) {
			int mn[3] = { 65535, 65535, 65535 }, mx[3] = { -1, -1, -1 }, flat = 1; double sm[3] = { 0, 0, 0 }; int cn[3] = { 0, 0, 0 };
			for (py = 0; py < 4; py++)
				for (px = 0; px < 4; px++) {
					int v = f->in[(size_t)(ty + py) * f->stride + tx + px];
					c = g_qcol[py][px];
					if (v < mn[c]) mn[c] = v;
					if (v > mx[c]) mx[c] = v;
					sm[c] += v - g_ped; cn[c]++;
				}
			for (c = 0; c < 3; c++) {
				double lvl = mn[c] - g_ped > 0 ? mn[c] - g_ped : 0;
				double sig = sqrt(f->na * lvl + f->nb), lim = (mn[c] - g_ped) / 4.0 + 16;
				if (4.5 * sig > lim) lim = 4.5 * sig;
				if (!(mx[c] < 1000 && mx[c] - mn[c] <= lim)) flat = 0;
			}
			if (!flat) continue;
			nfl++;
			double cm[3]; for (c = 0; c < 3; c++) cm[c] = sm[c] / cn[c];
			for (py = 0; py < 4; py++)
				for (px = 0; px < 4; px++) {
					int cc = g_qcol[py][px];
					double x[3] = { cm[cc], cm[g_oc[cc][0]], cm[g_oc[cc][1]] };
					double t = f->in[(size_t)(ty + py) * f->stride + tx + px] - g_ped;
					double *a = A[py][px];
					for (k = 0; k < 3; k++) { int m; for (m = 0; m < 3; m++) a[k * 3 + m] += x[k] * x[m]; a[9 + k] += x[k] * t; }
				}
		}
	f->nflat[job] = nfl;
}
static int solve3(double M[9], double r[3], double out[3])
{
	double d = M[0] * (M[4] * M[8] - M[5] * M[7]) - M[1] * (M[3] * M[8] - M[5] * M[6]) + M[2] * (M[3] * M[7] - M[4] * M[6]);
	if (fabs(d) < 1e-300) return 0;
	out[0] = (r[0] * (M[4] * M[8] - M[5] * M[7]) - M[1] * (r[1] * M[8] - M[5] * r[2]) + M[2] * (r[1] * M[7] - M[4] * r[2])) / d;
	out[1] = (M[0] * (r[1] * M[8] - M[5] * r[2]) - r[0] * (M[3] * M[8] - M[5] * M[6]) + M[2] * (M[3] * r[2] - r[1] * M[6])) / d;
	out[2] = (M[0] * (M[4] * r[2] - r[1] * M[7]) - M[1] * (M[3] * r[2] - r[1] * M[6]) + r[0] * (M[3] * M[7] - M[4] * M[6])) / d;
	return 1;
}
static long lk_fit(const uint16_t *in, int stride)
{
	struct lkfit f; int nj = (g_h + 63) / 64, j, px, py, k;
	float na, nb;
	estimate_noise(in, stride, 1.0f, &na, &nb);
	f.in = in; f.stride = stride; f.na = na; f.nb = nb;
	f.part = malloc(sizeof(*f.part) * nj); f.nflat = calloc(nj, sizeof(long));
	for (py = 0; py < 4; py++) for (px = 0; px < 4; px++) { g_lk[py][px][0] = 1; g_lk[py][px][1] = g_lk[py][px][2] = 0; }
	if (!f.part || !f.nflat) { free(f.part); free(f.nflat); return 0; }
	run_rows(lkfit_job, &f, 64);
	double S[4][4][12]; long nflat = 0;
	memset(S, 0, sizeof S);
	for (j = 0; j < nj; j++) {                       /* fixed merge order: thread-count independent */
		nflat += f.nflat[j];
		for (py = 0; py < 4; py++) for (px = 0; px < 4; px++) for (k = 0; k < 12; k++) S[py][px][k] += f.part[j][py][px][k];
	}
	free(f.part); free(f.nflat);
	if (nflat <= 500) return nflat;
	for (py = 0; py < 4; py++)
		for (px = 0; px < 4; px++) {
			double *a = S[py][px], M[9], r[3], o[3];
			double lam = 1e-3 * (a[0] + a[4] + a[8]) / 3;
			for (k = 0; k < 9; k++) M[k] = a[k];
			M[0] += lam; M[4] += lam; M[8] += lam;
			r[0] = a[9] + lam; r[1] = a[10]; r[2] = a[11];
			if (solve3(M, r, o)) { g_lk[py][px][0] = (float)o[0]; g_lk[py][px][1] = (float)o[1]; g_lk[py][px][2] = (float)o[2]; }
		}
	for (k = 0; k < 3; k++) {                        /* normalise a within each colour */
		double s = 0; int n = 0;
		for (py = 0; py < 4; py++) for (px = 0; px < 4; px++) if (g_qcol[py][px] == k) { s += g_lk[py][px][0]; n++; }
		for (py = 0; py < 4; py++) for (px = 0; px < 4; px++) if (g_qcol[py][px] == k) g_lk[py][px][0] = (float)(g_lk[py][px][0] / (s / n));
	}
	return nflat;
}
/* interior same-colour offsets in the 5x5 ring, per position and colour */
static signed char g_lo_off[4][4][3][24][2];
static int g_lo_n[4][4][3];
static void lk_tables(void)
{
	int px, py, c, dx, dy;
	for (py = 0; py < 4; py++)
		for (px = 0; px < 4; px++)
			for (c = 0; c < 3; c++) {
				int n = 0;
				for (dy = -2; dy <= 2; dy++)
					for (dx = -2; dx <= 2; dx++) {
						if (!dx && !dy) continue;
						if (g_qcol[(py + dy + 8) & 3][(px + dx + 8) & 3] != c) continue;
						g_lo_off[py][px][c][n][0] = (signed char)dy; g_lo_off[py][px][c][n][1] = (signed char)dx; n++;
					}
				g_lo_n[py][px][c] = n;
			}
}
static inline float lk_level(const uint16_t *in, int S, int x, int y, int c, int interior)
{
	float s = 0; int n = 0, k, dx, dy;
	if (interior) {
		const signed char (*o)[2] = g_lo_off[y & 3][x & 3][c]; int m = g_lo_n[y & 3][x & 3][c];
		const uint16_t *b = in + (size_t)y * S + x;
		for (k = 0; k < m; k++) s += b[o[k][0] * S + o[k][1]];
		return s / m - g_ped;
	}
	for (dy = -2; dy <= 2; dy++)
		for (dx = -2; dx <= 2; dx++) {
			if (!dx && !dy) continue;
			int yy = mir(y + dy, g_h), xx = mir(x + dx, g_w);
			if (qcol(xx, yy) != c) continue;   /* scipy convolve: mask is mirrored the same way */
			s += in[(size_t)yy * S + xx]; n++;
		}
	return n ? s / n - g_ped : 0.0f;
}
static void lk_apply(int y0, int y1, void *p)
{
	struct ctx *c = p; const int S = c->stride; int x, y;
	for (y = y0; y < y1; y++) {
		int iy = y >= 2 && y < g_h - 2;
		const uint16_t *s = c->src + (size_t)y * S; uint16_t *d = c->dst + (size_t)y * S;
		for (x = 0; x < g_w; x++) {
			int cc = qcol(x, y), in = iy && x >= 2 && x < g_w - 2;
			const float *k = g_lk[y & 3][x & 3];
			float L1 = lk_level(c->src, S, x, y, g_oc[cc][0], in), L2 = lk_level(c->src, S, x, y, g_oc[cc][1], in);
			float v = (s[x] - g_ped - k[1] * L1 - k[2] * L2) / k[0] + g_ped;
			d[x] = (uint16_t)clampi((int)lrintf(v * FX), 0, WHITE * FX);
		}
	}
}

/* --------------------------------------------------------------- 3. bilateral denoise */
/* exp(-t) for t >= 0, ~2e-7 relative error, branch-free so loops vectorise (NEON).
 * Weights below exp(-30) are returned as 0, which keeps denormals out of the sums. */
static inline float fexpn(float t)
{
	float keep = t < 30.0f ? 1.0f : 0.0f;
	float x = (t < 30.0f ? t : 30.0f) * -1.44269504f;
	float xi = floorf(x), f = x - xi;
	float p = 1.0f + f * (0.69314718f + f * (0.24022650f + f * (0.05550411f + f * (0.00961813f + f * 0.00133336f))));
	int32_t e = ((int32_t)xi + 127) << 23; float sc;         /* 2^xi built from its exponent bits */
	memcpy(&sc, &e, sizeof sc);
	return p * sc * keep;
}
#define LUTN 4096
#define LUTMAX 16.0f
static float g_lut[LUTN + 1];
struct off { int dx, dy; float sw; };
static struct off g_offs[4][4][81];           /* same-colour offsets per 4x4 phase */
static int g_noffs[4][4];

static void dn_tables(void)
{
	int px, py, dx, dy, i;
	for (i = 0; i <= LUTN; i++) g_lut[i] = expf(-LUTMAX * i / LUTN);
	for (py = 0; py < 4; py++)
		for (px = 0; px < 4; px++) {
			int n = 0;
			for (dy = -g_dnr; dy <= g_dnr; dy++)
				for (dx = -g_dnr; dx <= g_dnr; dx++)
					if (g_qcol[(py + dy + 8) & 3][(px + dx + 8) & 3] == g_qcol[py][px]) {
						g_offs[py][px][n].dx = dx; g_offs[py][px][n].dy = dy;
						g_offs[py][px][n].sw = expf(-(dx * dx + dy * dy) / 8.0f); n++;
					}
			g_noffs[py][px] = n;
		}
}
static inline float rw(float t)       /* exp(-t), interpolated LUT; 0 beyond LUTMAX */
{
	if (t >= LUTMAX) return 0.0f;
	float f = t * (LUTN / LUTMAX); int i = (int)f; f -= i;
	return g_lut[i] + (g_lut[i + 1] - g_lut[i]) * f;
}
/* Rows are converted to float and split into 4 planes by x&3 (plane[p][k] = pixel 4k+p, with
 * 2 entries of padding each side). For one output phase px all same-colour neighbours at offset
 * (dx,dy) then sit in ONE plane at a constant index shift, so the inner loop over k is contiguous. */
#define DPAD 2
struct dnrow { int y; float *pl[4]; };
static void dn_load(struct dnrow *r, const uint16_t *src, int S, int y, int np)
{
	int x, p; const uint16_t *s = src + (size_t)y * S;
	r->y = y;
	for (p = 0; p < 4; p++) {
		float *d = r->pl[p];
		for (x = 0; x < np; x++) {
			int xx = 4 * (x - DPAD) + p;
			d[x] = s[clampi(xx, 0, g_w - 1)] * IFX;
		}
	}
}
static void dn_border_px(struct ctx *c, int x, int y)
{
	const int S = c->stride; int dx, dy, cc = qcol(x, y);
	float v0 = c->src[(size_t)y * S + x] * IFX;
	float s2 = c->na * (v0 - g_ped > 0 ? v0 - g_ped : 0) + c->nb;
	float inv = 1.0f / (2.0f * s2 * g_dn * g_dn + 1e-9f), num = 0, den = 0;
	for (dy = -g_dnr; dy <= g_dnr; dy++)
		for (dx = -g_dnr; dx <= g_dnr; dx++) {
			int yy = clampi(y + dy, 0, g_h - 1), xx = clampi(x + dx, 0, g_w - 1);
			if (qcol(xx, yy) != cc) continue;
			float v = c->src[(size_t)yy * S + xx] * IFX, dd = v - v0;
			float wgt = expf(-(dx * dx + dy * dy) / 8.0f) * fexpn(dd * dd * inv);
			num += wgt * v; den += wgt;
		}
	c->dst[(size_t)y * S + x] = (uint16_t)clampi((int)lrintf(num / den * FX), 0, WHITE * FX);
}
static void dn_run(int y0, int y1, void *p)
{
	struct ctx *c = p; const int S = c->stride;
	int nk = (g_w + 3) / 4, np = nk + 2 * DPAD, y, x, k, j, px;
	struct dnrow ring[9];
	float *mem = malloc(sizeof(float) * (9 * 4 * np + 4 * nk));
	float *num = mem + 9 * 4 * np, *den = num + nk, *v0 = den + nk, *inv = v0 + nk;
	if (!mem) { LOGE("dn: alloc failed"); return; }
	for (j = 0; j < 9; j++) { ring[j].y = -1; for (px = 0; px < 4; px++) ring[j].pl[px] = mem + (size_t)(j * 4 + px) * np; }
	for (y = y0; y < y1; y++) {
		if (y < 4 || y >= g_h - 4) { for (x = 0; x < g_w; x++) dn_border_px(c, x, y); continue; }
		for (j = -4; j <= 4; j++) {                       /* make sure rows y-4..y+4 are loaded */
			struct dnrow *r = &ring[(y + j + 9) % 9];
			if (r->y != y + j) dn_load(r, c->src, S, y + j, np);
		}
		for (px = 0; px < 4; px++) {
			/* pixels of this phase: x = 4k+px; interior k range (x in [4, w-4)) */
			int k0 = (4 - px + 3) / 4, k1 = (g_w - 4 - px + 3) / 4;
			if (k1 <= k0) continue;
			const float *ctr = ring[y % 9].pl[px] + DPAD;
			for (k = k0; k < k1; k++) {
				float v = ctr[k];
				float s2 = c->na * (v - g_ped > 0 ? v - g_ped : 0) + c->nb;
				v0[k] = v; inv[k] = 1.0f / (2.0f * s2 * g_dn * g_dn + 1e-9f);
				num[k] = 0; den[k] = 0;
			}
			const struct off *o = g_offs[y & 3][px]; int n = g_noffs[y & 3][px];
			for (j = 0; j < n; j++) {
				int t = px + o[j].dx, sh = (t + 8) / 4 - 2;      /* floor((px+dx)/4) */
				const float *restrict nb = ring[(y + o[j].dy + 9) % 9].pl[(t + 8) & 3] + DPAD + sh;
				float *restrict nu = num, *restrict de = den;
				const float *restrict vz = v0, *restrict iv = inv;
				float sw = o[j].sw;
				for (k = k0; k < k1; k++) {
					float vv = nb[k], dd = vv - vz[k];
					float w = sw * fexpn(dd * dd * iv[k]);
					nu[k] += w * vv; de[k] += w;
				}
			}
			uint16_t *drow = c->dst + (size_t)y * S;
			for (k = k0; k < k1; k++)
				drow[4 * k + px] = (uint16_t)clampi((int)lrintf(num[k] / den[k] * FX), 0, WHITE * FX);
		}
		for (x = 0; x < 4; x++) dn_border_px(c, x, y);
		for (x = g_w - 4; x < g_w; x++) dn_border_px(c, x, y);
	}
	free(mem);
}

/* --------------------------------------------------------------- 4. Quad-PPG green, soft blend */
/* One pixel, edge-replicated reads: used for the border (|offsets| <= 5). */
static void green_px(struct ctx *c, int16_t *G, int x, int y)
{
	const int S = c->stride;
#define Q(dy, dx) (c->d[(size_t)clampi(y + (dy), 0, g_h - 1) * S + clampi(x + (dx), 0, g_w - 1)] * IFX - g_ped)
	float q0 = c->d[(size_t)y * S + x] * IFX - g_ped, est;
	if (qcol(x, y) == CG) { G[(size_t)y * S + x] = (int16_t)lrintf(q0 * FX); return; }
	int nh, fh, mh, p1h, p2h, n1h, n2h, nv, fv, mv, p1v, p2v, n1v, n2v;
	if ((x & 1) == 0) { nh = -1; fh = 2; mh = 1; p1h = -4; p2h = -3; n1h = 4; n2h = 5; }
	else              { nh = 1; fh = -2; mh = -1; p1h = 3; p2h = 4; n1h = -5; n2h = -4; }
	if ((y & 1) == 0) { nv = -1; fv = 2; mv = 1; p1v = -4; p2v = -3; n1v = 4; n2v = 5; }
	else              { nv = 1; fv = -2; mv = -1; p1v = 3; p2v = 4; n1v = -5; n2v = -4; }
	float gl = Q(0, nh), gr = Q(0, fh);
	float gh = (2.0f * gl + 1.0f * gr) / 3.0f;
	float laph = (q0 + Q(0, mh)) - (Q(0, p1h) + Q(0, p2h)) * 0.5f - (Q(0, n1h) + Q(0, n2h)) * 0.5f;
	float gu = Q(nv, 0), gd = Q(fv, 0);
	float gv = (2.0f * gu + 1.0f * gd) / 3.0f;
	float lapv = (q0 + Q(mv, 0)) - (Q(p1v, 0) + Q(p2v, 0)) * 0.5f - (Q(n1v, 0) + Q(n2v, 0)) * 0.5f;
	float hg = fabsf(gl - gr) + fabsf(laph), vg = fabsf(gu - gd) + fabsf(lapv);
	float eh = gh + laph * 0.25f, ev = gv + lapv * 0.25f;
	est = (hg + vg > 1e-6f) ? (vg * eh + hg * ev) / (hg + vg) : (eh + ev) * 0.5f;
	G[(size_t)y * S + x] = (int16_t)clampi((int)lrintf(est * FX), -32768, 32767);
#undef Q
}
/* Interior: for each column phase px the colour and all neighbour offsets are fixed, so the
 * loop over x = 4k+px has no branches and no clamping. Same arithmetic as green_px. */
static void green_run(int y0, int y1, void *p)
{
	struct ctx *c = p; const int S = c->stride, W = g_w, H = g_h; int x, y, k, px;
	int16_t *G = (int16_t *)c->dst;
	const float ped = (float)g_ped;
	for (y = y0; y < y1; y++) {
		if (y < 5 || y >= H - 5 || W < 16) { for (x = 0; x < W; x++) green_px(c, G, x, y); continue; }
		const uint16_t *d = c->d + (size_t)y * S; int16_t *g = G + (size_t)y * S;
		int nv, fv, mv, p1v, p2v, n1v, n2v;
		if ((y & 1) == 0) { nv = -1; fv = 2; mv = 1; p1v = -4; p2v = -3; n1v = 4; n2v = 5; }
		else              { nv = 1; fv = -2; mv = -1; p1v = 3; p2v = 4; n1v = -5; n2v = -4; }
		const long Snv = (long)nv * S, Sfv = (long)fv * S, Smv = (long)mv * S, Sp1 = (long)p1v * S,
		           Sp2 = (long)p2v * S, Sn1 = (long)n1v * S, Sn2 = (long)n2v * S;
		for (px = 0; px < 4; px++) {
			int k0 = (5 - px + 3) / 4, k1 = (W - 5 - px + 3) / 4;
			if (g_qcol[y & 3][px] == CG) {
				for (k = k0; k < k1; k++) { int xx = 4 * k + px; g[xx] = (int16_t)lrintf((d[xx] * IFX - ped) * FX); }
				continue;
			}
			int nh, fh, mh, p1h, p2h, n1h, n2h;
			if ((px & 1) == 0) { nh = -1; fh = 2; mh = 1; p1h = -4; p2h = -3; n1h = 4; n2h = 5; }
			else               { nh = 1; fh = -2; mh = -1; p1h = 3; p2h = 4; n1h = -5; n2h = -4; }
			for (k = k0; k < k1; k++) {
				const uint16_t *q = d + 4 * k + px;
#define V(o) (q[o] * IFX - ped)
				float q0 = V(0);
				float gl = V(nh), gr = V(fh);
				float gh = (2.0f * gl + 1.0f * gr) / 3.0f;
				float laph = (q0 + V(mh)) - (V(p1h) + V(p2h)) * 0.5f - (V(n1h) + V(n2h)) * 0.5f;
				float gu = V(Snv), gd = V(Sfv);
				float gv = (2.0f * gu + 1.0f * gd) / 3.0f;
				float lapv = (q0 + V(Smv)) - (V(Sp1) + V(Sp2)) * 0.5f - (V(Sn1) + V(Sn2)) * 0.5f;
#undef V
				float hg = fabsf(gl - gr) + fabsf(laph), vg = fabsf(gu - gd) + fabsf(lapv);
				float eh = gh + laph * 0.25f, ev = gv + lapv * 0.25f;
				float tot = hg + vg;
				float est = (tot > 1e-6f) ? (vg * eh + hg * ev) / tot : (eh + ev) * 0.5f;
				int iv = (int)lrintf(est * FX);
				g[4 * k + px] = (int16_t)(iv < -32768 ? -32768 : iv > 32767 ? 32767 : iv);
			}
		}
		for (x = 0; x < 5; x++) green_px(c, G, x, y);
		for (x = W - 5; x < W; x++) green_px(c, G, x, y);
	}
}

/* --------------------------------------------------------------- 5. green refinement */
static float g_k1[7][7], g_k15[7][7];
static void gauss_tables(void)
{
	int i, j;
	for (i = 0; i < 7; i++)
		for (j = 0; j < 7; j++) {
			float r2 = (float)((i - 3) * (i - 3) + (j - 3) * (j - 3));
			g_k1[i][j] = expf(-r2 / 2.0f); g_k15[i][j] = expf(-r2 / (2.0f * 2.25f));
		}
}
struct rtap { int dy, dx; float w; };
/* same-colour taps of the 7x7 refinement kernel, per 4x4 phase, in raster order */
static struct rtap g_ftap[4][4][49];
static int g_nftap[4][4];
static float g_fden[4][4];
static void refine_tables(void)
{
	int px, py, i, j;
	for (py = 0; py < 4; py++)
		for (px = 0; px < 4; px++) {
			int n = 0, cc = g_qcol[py][px]; float den = 0;
			for (i = 0; i < 7; i++)
				for (j = 0; j < 7; j++)
					if (g_qcol[(py + i - 3 + 8) & 3][(px + j - 3 + 8) & 3] == cc) {
						g_ftap[py][px][n].dy = i - 3; g_ftap[py][px][n].dx = j - 3;
						g_ftap[py][px][n].w = g_k1[i][j]; den += g_k1[i][j]; n++;
					}
			g_nftap[py][px] = n; g_fden[py][px] = den;
		}
}
static void refine_px(struct ctx *c, int16_t *G2, int x, int y)    /* border pixel, mirrored */
{
	const int S = c->stride; const int16_t *G = c->g; int i, j;
	size_t o = (size_t)y * S + x; int cc = qcol(x, y);
	if (cc == CG) { G2[o] = G[o]; return; }
	float num = 0, den = 0;
	for (i = 0; i < 7; i++) {
		int yy = mir(y + i - 3, g_h);
		for (j = 0; j < 7; j++) {
			int xx = mir(x + j - 3, g_w);
			if (qcol(xx, yy) != cc) continue;
			size_t oo = (size_t)yy * S + xx;
			num += g_k1[i][j] * (c->d[oo] * IFX - g_ped - G[oo] * IFX); den += g_k1[i][j];
		}
	}
	float q0 = c->d[o] * IFX - g_ped, dd = num / (den > 1e-9f ? den : 1e-9f);
	float g2 = (1 - c->mix) * G[o] * IFX + c->mix * (q0 - dd);
	G2[o] = (int16_t)clampi((int)lrintf(g2 * FX), -32768, 32767);
}
/* Interior: rows of P = q - g (float) in an 8-row ring; for each column phase the taps are
 * fixed, so x = 4k+px runs as contiguous loops. Same tap order and arithmetic as refine_px. */
static void refine_run(int y0, int y1, void *p)
{
	struct ctx *c = p; const int S = c->stride, W = g_w, H = g_h; int x, y, j, k, px;
	const int16_t *G = c->g; int16_t *G2 = (int16_t *)c->dst;
	if (c->mix <= 0) { for (y = y0; y < y1; y++) memcpy(G2 + (size_t)y * S, G + (size_t)y * S, sizeof(int16_t) * W); return; }
	float *mem = malloc(sizeof(float) * (size_t)W * (8 + 1));
	if (!mem) { for (y = y0; y < y1; y++) for (x = 0; x < W; x++) refine_px(c, G2, x, y); return; }
	float *ring[8], *num = mem + (size_t)W * 8; int have[8];
	for (j = 0; j < 8; j++) { ring[j] = mem + (size_t)W * j; have[j] = -1; }
	const float ped = (float)g_ped, mix = c->mix;
	for (y = y0; y < y1; y++) {
		if (y < 3 || y >= H - 3 || W < 16) { for (x = 0; x < W; x++) refine_px(c, G2, x, y); continue; }
		for (j = -3; j <= 3; j++) {
			int yy = y + j, r = yy & 7;
			if (have[r] == yy) continue;
			const uint16_t *d = c->d + (size_t)yy * S; const int16_t *g = G + (size_t)yy * S; float *P = ring[r];
			for (x = 0; x < W; x++) P[x] = d[x] * IFX - ped - g[x] * IFX;
			have[r] = yy;
		}
		const uint16_t *d0 = c->d + (size_t)y * S; const int16_t *g0 = G + (size_t)y * S; int16_t *o = G2 + (size_t)y * S;
		for (px = 0; px < 4; px++) {
			int k0 = (3 - px + 3) / 4, k1 = (W - 3 - px + 3) / 4;
			if (g_qcol[y & 3][px] == CG) {
				for (k = k0; k < k1; k++) o[4 * k + px] = g0[4 * k + px];
				continue;
			}
			const struct rtap *tp = g_ftap[y & 3][px]; int n = g_nftap[y & 3][px];
			float den = g_fden[y & 3][px]; den = den > 1e-9f ? den : 1e-9f;
			for (k = k0; k < k1; k++) num[k] = 0;
			for (j = 0; j < n; j++) {
				const float *restrict src = ring[(y + tp[j].dy) & 7] + px + tp[j].dx; float w = tp[j].w;
				float *restrict nu = num;
				for (k = k0; k < k1; k++) nu[k] += w * src[4 * k];
			}
			for (k = k0; k < k1; k++) {
				int xx = 4 * k + px;
				float q0 = d0[xx] * IFX - ped, dd = num[k] / den;
				float g2 = (1 - mix) * g0[xx] * IFX + mix * (q0 - dd);
				int iv = (int)lrintf(g2 * FX);
				o[xx] = (int16_t)(iv < -32768 ? -32768 : iv > 32767 ? 32767 : iv);
			}
		}
		for (x = 0; x < 3; x++) refine_px(c, G2, x, y);
		for (x = W - 3; x < W; x++) refine_px(c, G2, x, y);
	}
	free(mem);
}

/* --------------------------------------------------------------- 6. residual interpolation + output */
/* Per channel C, rows are produced in a pipeline, each stage keeping a ring of 8 rows:
 *   hs   horizontal 7-sums (mirror) of m, g, q, g*g, g*q over C samples      (double, sliding)
 *   ab   a, b from the vertical 7-sums (running sum in the interior)          -> then 7-sums of a, b ('reflect')
 *   tent (7x7 mean of a)*g + (7x7 mean of b)
 *   out  tent + normalised gaussian of (q - tent) over C samples, with precomputed sparse taps */

static struct rtap g_rtap[3][4][4][49];      /* [C][y&3][x&3] */
static int g_nrtap[3][4][4];
static float g_rden[3][4][4];
static void ri_tables(void)
{
	int C, py, px, i, j;
	for (C = 0; C < 3; C += 2)
		for (py = 0; py < 4; py++)
			for (px = 0; px < 4; px++) {
				int n = 0; float d = 0;
				for (i = 0; i < 7; i++)
					for (j = 0; j < 7; j++)
						if (g_qcol[(py + i - 3 + 8) & 3][(px + j - 3 + 8) & 3] == C) {
							g_rtap[C][py][px][n].dy = i - 3; g_rtap[C][py][px][n].dx = j - 3;
							g_rtap[C][py][px][n].w = g_k15[i][j]; d += g_k15[i][j]; n++;
						}
				g_nrtap[C][py][px] = n; g_rden[C][py][px] = d;
			}
}
/* Streaming RI in single precision with planar rows, so every inner loop vectorises.
 * g and q are shifted by RIK before summing: the local fit a = cov/(var+eps) does not change
 * under a constant shift, and the smaller magnitudes keep var = E[g^2]-E[g]^2 accurate in float. */
#define RIK 256.0f
static float *g_mask[3][4];                    /* [C][y&3]: 1.0 where qcol == C, else 0 (W floats) */
static void ri_masks(void)
{
	int C, py, x;
	for (C = 0; C < 3; C += 2)
		for (py = 0; py < 4; py++) {
			free(g_mask[C][py]);
			g_mask[C][py] = malloc(sizeof(float) * g_w);
			if (g_mask[C][py]) for (x = 0; x < g_w; x++) g_mask[C][py][x] = g_qcol[py][x & 3] == C;
		}
}
/* RI is processed in column tiles so its 8-row rings stay in L2 (full-width rings were
 * ~1.3 MB per thread and the stage was memory-bound: it stopped scaling past the 2 fast cores).
 * For output columns [X0,X1) the stages need: E/tent at X0-3..X1+3, ah/bh at X0-6..X1+6,
 * a/b and hs at X0-9..X1+9, input planes at X0-12..X1+12 (all clipped to the image; mirrored
 * references near the image edges always fall inside these windows). Arrays are indexed by
 * (global x - L0) where [L0,L1) is the input-plane window. */
static int g_tile = 384;                       /* persist.vendor.remoshim.tile */
struct ri {
	struct ctx *c; int C;
	int L0, L1, X0, X1;                       /* input window, output columns */
	int hA, hB, aA, aB, eA, eB;               /* hs/a cols, ah cols, tent/E cols */
	float *hs[8][5], *ah[8], *bh[8], *tn[8], *E[8];
	float *pl[5], *arow, *brow;
	int hs_next, ab_next, tn_next;
};
static inline int row_has(int C, int y) { return g_qcol[y & 3][0] == C || g_qcol[y & 3][2] == C; }
/* o[x-L0] = sum_{j=-3..3} v[edge(x+j)-L0] for global x in [A,B) */
static void hsum7(float *restrict o, const float *restrict v, int A, int B, int L0, int reflect)
{
	const int W = g_w; int x, j;
	int fa = A > 3 ? A : 3, fb = B < W - 3 ? B : W - 3;
	for (x = fa; x < fb; x++) {
		const float *p = v + (x - L0);
		o[x - L0] = p[-3] + p[-2] + p[-1] + p[0] + p[1] + p[2] + p[3];
	}
	for (x = A; x < B; x++) {
		if (x >= fa && x < fb) { x = fb - 1; continue; }
		float t = 0;
		for (j = -3; j <= 3; j++) t += v[(reflect ? rfl(x + j, W) : mir(x + j, W)) - L0];
		o[x - L0] = t;
	}
}
static void ri_hs(struct ri *r, int y)
{
	struct ctx *c = r->c; const int S = c->stride, L0 = r->L0, n = r->L1 - r->L0; int x, k;
	float **h = r->hs[y & 7];
	if (!row_has(r->C, y)) { for (k = 0; k < 5; k++) memset(h[k], 0, sizeof(float) * n); return; }
	const uint16_t *restrict drow = c->d + (size_t)y * S + L0; const int16_t *restrict grow = c->g + (size_t)y * S + L0;
	const float *restrict mk = g_mask[r->C][y & 3] + L0;
	float *restrict p0 = r->pl[0], *restrict p1 = r->pl[1], *restrict p2 = r->pl[2], *restrict p3 = r->pl[3], *restrict p4 = r->pl[4];
	const float ped = (float)g_ped;
	for (x = 0; x < n; x++) {
		float m = mk[x], g = (grow[x] * IFX - RIK) * m, q = (drow[x] * IFX - ped - RIK) * m;
		p0[x] = m; p1[x] = g; p2[x] = q; p3[x] = g * g; p4[x] = g * q;
	}
	for (k = 0; k < 5; k++) hsum7(h[k], r->pl[k], r->hA, r->hB, L0, 0);
}
static void ri_ab(struct ri *r, int y)
{
	const int H = g_h, L0 = r->L0; int x, j, k;
	while (r->hs_next <= (y + 3 < H ? y + 3 : H - 1)) ri_hs(r, r->hs_next++);
	const float *R[5][7];
	for (k = 0; k < 5; k++) for (j = 0; j < 7; j++) R[k][j] = r->hs[mir(y + j - 3, H) & 7][k];
	float *restrict A = r->arow, *restrict B = r->brow;
	for (x = r->hA - L0; x < r->hB - L0; x++) {
		float t0 = R[0][0][x] + R[0][1][x] + R[0][2][x] + R[0][3][x] + R[0][4][x] + R[0][5][x] + R[0][6][x];
		float t1 = R[1][0][x] + R[1][1][x] + R[1][2][x] + R[1][3][x] + R[1][4][x] + R[1][5][x] + R[1][6][x];
		float t2 = R[2][0][x] + R[2][1][x] + R[2][2][x] + R[2][3][x] + R[2][4][x] + R[2][5][x] + R[2][6][x];
		float t3 = R[3][0][x] + R[3][1][x] + R[3][2][x] + R[3][3][x] + R[3][4][x] + R[3][5][x] + R[3][6][x];
		float t4 = R[4][0][x] + R[4][1][x] + R[4][2][x] + R[4][3][x] + R[4][4][x] + R[4][5][x] + R[4][6][x];
		float in = 1.0f / (t0 > 1e-9f ? t0 : 1e-9f);
		float mg = t1 * in, mc = t2 * in, mgg = t3 * in, mgc = t4 * in;
		float a = (mgc - mg * mc) / (mgg - mg * mg + 4.0f);
		A[x] = a; B[x] = (mc + RIK) - a * (mg + RIK);
	}
	hsum7(r->ah[y & 7], A, r->aA, r->aB, L0, 1);
	hsum7(r->bh[y & 7], B, r->aA, r->aB, L0, 1);
}
static void ri_tent(struct ri *r, int y)
{
	const int H = g_h, S = r->c->stride, L0 = r->L0; int x, j;
	while (r->ab_next <= (y + 3 < H ? y + 3 : H - 1)) ri_ab(r, r->ab_next++);
	const float *A[7], *B[7];
	for (j = 0; j < 7; j++) { int yy = rfl(y + j - 3, H) & 7; A[j] = r->ah[yy]; B[j] = r->bh[yy]; }
	const int16_t *restrict grow = r->c->g + (size_t)y * S + L0; const uint16_t *restrict drow = r->c->d + (size_t)y * S + L0;
	const float *restrict mk = g_mask[r->C][y & 3] + L0;
	float *restrict t = r->tn[y & 7], *restrict e = r->E[y & 7];
	const float ped = (float)g_ped;
	for (x = r->eA - L0; x < r->eB - L0; x++) {
		float sa = A[0][x] + A[1][x] + A[2][x] + A[3][x] + A[4][x] + A[5][x] + A[6][x];
		float sb = B[0][x] + B[1][x] + B[2][x] + B[3][x] + B[4][x] + B[5][x] + B[6][x];
		float tv = (sa * (1.0f / 49.0f)) * (grow[x] * IFX) + sb * (1.0f / 49.0f);
		t[x] = tv;
		e[x] = (drow[x] * IFX - ped - tv) * mk[x];
	}
}
static void ri_out(struct ri *r, int y)
{
	struct ctx *c = r->c; const int W = g_w, H = g_h, S = c->stride, C = r->C, L0 = r->L0; int x, j, k, px;
	while (r->tn_next <= (y + 3 < H ? y + 3 : H - 1)) ri_tent(r, r->tn_next++);
	const float *Er[7]; for (j = 0; j < 7; j++) Er[j] = r->E[mir(y + j - 3, H) & 7];
	const float *tc = r->tn[y & 7]; uint16_t *orow = c->dst + (size_t)y * S;
	const float ped = (float)g_ped;
	int border_y = (y < 3 || y >= H - 3);
	for (px = 0; px < 4; px++) {
		if (g_tcol[y & 1][px & 1] != C || g_qcol[y & 3][px] == C) continue;
		/* interior columns x = 4k+px in [max(X0,3), min(X1,W-3)) */
		int xa = r->X0 > 3 ? r->X0 : 3, xb = r->X1 < W - 3 ? r->X1 : W - 3;
		int k0 = (xa - px + 3) / 4, k1 = (xb - px + 3) / 4;
		if (!border_y && k1 > k0) {
			const struct rtap *tp = g_rtap[C][y & 3][px]; int n = g_nrtap[C][y & 3][px], kk;
			float iden = 1.0f / g_rden[C][y & 3][px], acc[512];
			int kb;
			for (kb = k0; kb < k1; kb += 512) {
				int ke = kb + 512 < k1 ? kb + 512 : k1;
				for (kk = 0; kk < ke - kb; kk++) acc[kk] = 0;
				for (j = 0; j < n; j++) {
					const float *restrict src = Er[tp[j].dy + 3] + px + tp[j].dx - L0; float w = tp[j].w;
					for (kk = 0; kk < ke - kb; kk++) acc[kk] += w * src[4 * (kb + kk)];
				}
				for (kk = 0; kk < ke - kb; kk++) {
					int xx = 4 * (kb + kk) + px;
					orow[xx] = (uint16_t)clampi((int)lrintf(tc[xx - L0] + acc[kk] * iden + ped), 0, WHITE);
				}
			}
		}
		for (x = r->X0 + ((px - r->X0) & 3); x < r->X1; x += 4) {   /* x == px (mod 4) */
			if (!border_y && x >= 3 && x < W - 3) continue;
			float num = 0, den = 0;
			for (j = 0; j < 7; j++) {
				int yy = mir(y + j - 3, H);
				for (k = 0; k < 7; k++) {
					int xx = mir(x + k - 3, W);
					if (qcol(xx, yy) != C) continue;
					num += g_k15[j][k] * Er[j][xx - L0]; den += g_k15[j][k];
				}
			}
			orow[x] = (uint16_t)clampi((int)lrintf(tc[x - L0] + num / den + ped), 0, WHITE);
		}
	}
}
static void final_run(int y0, int y1, void *p)
{
	struct ctx *c = p; const int S = c->stride, W = g_w; int x, y, C, i, k, X0;
	struct ri r;
	int tw = g_tile > 16 ? g_tile : 16, lw = tw + 24;
	if (lw > W) lw = W;
	float *mem = malloc(sizeof(float) * (size_t)lw * (8 * 5 + 8 * 4 + 5 + 2));
	if (!mem) { LOGE("final: scratch alloc failed"); return; }
	for (y = y0; y < y1; y++)                       /* pass-through and green output */
		for (x = 0; x < W; x++) {
			size_t o = (size_t)y * S + x; int t = tcol(x, y);
			if (qcol(x, y) == t) c->dst[o] = (uint16_t)clampi((int)lrintf(c->d[o] * IFX), 0, WHITE);
			else if (t == CG) c->dst[o] = (uint16_t)clampi((int)lrintf(c->g[o] * IFX) + g_ped, 0, WHITE);
		}
	for (X0 = 0; X0 < W; X0 += tw)
		for (C = 0; C < 3; C += 2) {
			float *m = mem;
			r.c = c; r.C = C; r.X0 = X0; r.X1 = X0 + tw < W ? X0 + tw : W;
			r.L0 = X0 - 12 > 0 ? X0 - 12 : 0;  r.L1 = r.X1 + 12 < W ? r.X1 + 12 : W;
			r.hA = X0 - 9 > 0 ? X0 - 9 : 0;    r.hB = r.X1 + 9 < W ? r.X1 + 9 : W;
			r.aA = X0 - 6 > 0 ? X0 - 6 : 0;    r.aB = r.X1 + 6 < W ? r.X1 + 6 : W;
			r.eA = X0 - 3 > 0 ? X0 - 3 : 0;    r.eB = r.X1 + 3 < W ? r.X1 + 3 : W;
			for (i = 0; i < 8; i++) for (k = 0; k < 5; k++) { r.hs[i][k] = m; m += lw; }
			for (i = 0; i < 8; i++) { r.ah[i] = m; m += lw; r.bh[i] = m; m += lw; r.tn[i] = m; m += lw; r.E[i] = m; m += lw; }
			for (k = 0; k < 5; k++) { r.pl[k] = m; m += lw; }
			r.arow = m; m += lw; r.brow = m;
			r.tn_next = y0 - 3 > 0 ? y0 - 3 : 0;
			r.ab_next = r.tn_next - 3 > 0 ? r.tn_next - 3 : 0;
			r.hs_next = r.ab_next - 3 > 0 ? r.ab_next - 3 : 0;
			for (y = y0; y < y1; y++) ri_out(&r, y);
		}
	free(mem);
}


/* --------------------------------------------------------------- 7. mild luminance sharpening
 * Unsharp mask driven by the reconstructed full-resolution green (G2, int16 Q4): hp = g - mean3x3(g), cored by
 * 1.5 x the noise sigma at that level, then amount * hp is added to EVERY output pixel. The same boost on R, G and B
 * sharpens brightness detail without colour fringes. persist.vendor.remoshim.sharpen = amount x100 (default 30). */
static int g_sharpen = 0;
static void sharpen_run(int y0, int y1, void *p)
{
	struct ctx *c = p; const int S = c->stride, W = g_w, H = g_h; int x, y;
	const float amt = g_sharpen / 100.0f, ped = (float)g_ped;
	for (y = y0; y < y1; y++) {
		if (y < 1 || y >= H - 1) continue;
		const int16_t *gm = c->g + (size_t)(y - 1) * S, *g0 = c->g + (size_t)y * S, *gp = c->g + (size_t)(y + 1) * S;
		uint16_t *o = c->dst + (size_t)y * S;
		for (x = 1; x < W - 1; x++) {
			float m = (gm[x - 1] + gm[x] + gm[x + 1] + g0[x - 1] + g0[x] + g0[x + 1] + gp[x - 1] + gp[x] + gp[x + 1]) * (IFX / 9.0f);
			float gv = g0[x] * IFX, hp = gv - m;
			float lvl = gv > 0 ? gv : 0, sig = sqrtf(c->na * lvl + c->nb), t = 1.5f * sig;
			float a = fabsf(hp) - t;
			if (a <= 0) continue;
			float d = amt * (hp > 0 ? a : -a);
			int v = (int)lrintf(o[x] + d);
			o[x] = (uint16_t)(v < 0 ? 0 : v > WHITE ? WHITE : v);
		}
	}
	(void)ped;
}

/* --------------------------------------------------------------- pixel swap (mode 1) */
static void swap_run(int y0, int y1, void *p)
{
	struct ctx *c = p; int x, y;
	for (y = y0; y < y1; y++) {
		int sy = y; if ((y & 3) == 1) sy = y + 1; else if ((y & 3) == 2) sy = y - 1;
		if (sy >= g_h) sy = y;
		for (x = 0; x < g_w; x++) {
			int sx = x; if ((x & 3) == 1) sx = x + 1; else if ((x & 3) == 2) sx = x - 1;
			if (sx >= g_w) sx = x;
			c->dst[(size_t)y * c->stride + x] = c->src[(size_t)sy * c->stride + sx];
		}
	}
}


/* --------------------------------------------------------------- diagnostics: fixed 4x4 pattern vs brightness
 * Samples 4x4 tiles (every 8 px) that are flat per colour, bins them by green level, and reports the
 * rms deviation (%) of the 16 positions from their colour's mean. Run on the raw input (colours by quad
 * layout) and on the output Bayer (colours by Bayer layout): shows whether OUR output carries the
 * period-4 pattern, and at which brightness. persist.vendor.remoshim.diag=0 disables it. */
static int g_diag = 0;
static void diag_pattern_s(const uint16_t *b, int S, int bayer_out, const char *tag, float scl)
{
	static const int edges[6] = { 0, 100, 300, 600, 850, 2000 };
	double sum[5][16]; long n[5]; int bi, tx, ty, px, py, c;
	memset(sum, 0, sizeof sum); memset(n, 0, sizeof n);
	for (ty = 0; ty + 4 <= g_h; ty += 8)
		for (tx = 0; tx + 4 <= g_w; tx += 8) {
			double cs[3] = { 0, 0, 0 }; int cn[3] = { 0, 0, 0 }, mn[3] = { 1 << 20, 1 << 20, 1 << 20 }, mx[3] = { -1, -1, -1 }, ok = 1;
			for (py = 0; py < 4; py++)
				for (px = 0; px < 4; px++) {
					int v = (int)lrintf(b[(size_t)(ty + py) * S + tx + px] * scl) - g_ped;
					c = bayer_out ? g_tcol[py & 1][px & 1] : g_qcol[py][px];
					cs[c] += v; cn[c]++;
					if (v < mn[c]) mn[c] = v;
					if (v > mx[c]) mx[c] = v;
				}
			for (c = 0; c < 3; c++) {
				double m = cs[c] / cn[c];
				if (m < 2 || mx[c] - mn[c] > 0.10 * m + 10) ok = 0;
			}
			if (!ok) continue;
			double gl = cs[CG] / cn[CG];
			for (bi = 0; bi < 5; bi++) if (gl >= edges[bi] && gl < edges[bi + 1]) break;
			if (bi == 5) continue;
			for (py = 0; py < 4; py++)
				for (px = 0; px < 4; px++) {
					c = bayer_out ? g_tcol[py & 1][px & 1] : g_qcol[py][px];
					sum[bi][py * 4 + px] += (b[(size_t)(ty + py) * S + tx + px] * scl - g_ped) / (cs[c] / cn[c]);
				}
			n[bi]++;
		}
	char line[400]; int o = 0;
	o += snprintf(line + o, sizeof line - o, "diag %s 4x4 pattern rms%% by green level:", tag);
	for (bi = 0; bi < 5; bi++) {
		if (n[bi] < 50) { o += snprintf(line + o, sizeof line - o, " [%d-%d) n=%ld -", edges[bi], edges[bi + 1], n[bi]); continue; }
		double ss = 0; int k;
		for (k = 0; k < 16; k++) { double d = sum[bi][k] / n[bi] - 1.0; ss += d * d; }
		o += snprintf(line + o, sizeof line - o, " [%d-%d) n=%ld %.2f", edges[bi], edges[bi + 1], n[bi], 100.0 * sqrt(ss / 16));
	}
	LOGI("%s", line);
}
static void diag_pattern(const uint16_t *b, int S, int bayer_out, const char *tag) { diag_pattern_s(b, S, bayer_out, tag, 1.0f); }
static void diag_clip(const uint16_t *b, int S)
{
	long hi = 0, cl = 0, tot = 0; int x, y;
	for (y = 0; y < g_h; y += 2)
		for (x = 0; x < g_w; x += 2) { int v = b[(size_t)y * S + x]; tot++; if (v >= 1000) hi++; if (v >= 1020) cl++; }
	LOGI("diag raw clipping: %.2f%% >= 1000, %.2f%% >= 1020 (sampled)", 100.0 * hi / tot, 100.0 * cl / tot);
}

/* --------------------------------------------------------------- diag: PDAF behaviour in the raw domain
 * persist.vendor.remoshim.diag=1. Runs on the raw input BEFORE the PDAF repair, on a sparse set of 16x16 tiles
 * (every 3rd tile row, every 5th tile column, so that all four copies of the 32x32 super-tile are sampled).
 * Answers: (a) is the PDAF response additive or multiplicative, and does it change with brightness? (b) do the PD
 * pixels clip? (c) how far does the leakage ring around a PD pixel reach? (d) is the black level really g_ped?
 * (e) do the four 32x32 copies of a PD position respond alike?
 * For a flat pixel, dev = (v - ped) / (mean of its four same-phase neighbours 4 px away - ped) - 1.
 * Bins are by that neighbour level above the pedestal. Ring dK = mean dev of the non-PD pixels at Chebyshev distance
 * K from a PD pixel (positions taken modulo 16). "clip" = PD pixel >= 1015. The ring is measured against the same-phase
 * neighbours 4 px away, which can themselves carry some leakage, so the d1..d3 values are lower bounds.
 * "c00 c01 c10 c11" = PD deviation in the copy (tile row parity, tile column parity). */
#define DPB 6
static void diag_pdraw(const uint16_t *in, int S)
{
	static const int blo[DPB + 1] = { 0, 16, 48, 128, 300, 600, 1 << 20 };
	static const int off[4][2] = { { -4, 0 }, { 4, 0 }, { 0, -4 }, { 0, 4 } };
	unsigned char isp[16][16];
	const size_t NN = (size_t)DPB * 4 * 256;
	double *sum = calloc(NN, sizeof(double));
	long *cnt = calloc(NN, sizeof(long)), *clp = calloc(NN, sizeof(long));
	long *hist = calloc(4096, sizeof(long)), nh = 0, bc[16] = { 0 }; double bs[16] = { 0 };
	int ty, tx, y, x, k, b, i, vmax = 0;
	if (!sum || !cnt || !clp || !hist) { free(sum); free(cnt); free(clp); free(hist); return; }
	memset(isp, 0, sizeof isp);
	for (i = 0; i < 8; i++) isp[kPdPos[i][0]][kPdPos[i][1]] = 1;
#define IX(b, cp, pos) (((size_t)(b) * 4 + (cp)) * 256 + (pos))
	for (ty = 1; ty * 16 + 20 < g_h; ty += 3)
		for (tx = 1; tx * 16 + 20 < g_w; tx += 5) {
			int cp = (ty & 1) * 2 + (tx & 1);
			for (y = 0; y < 16; y++)
				for (x = 0; x < 16; x++) {
					int yy = ty * 16 + y, xx = tx * 16 + x, v = in[(size_t)yy * S + xx], nb[4], mn = 1 << 30, mx = -1, ok = 1;
					hist[v < 4095 ? v : 4095]++; nh++; if (v > vmax) vmax = v;
					if (v < g_ped + 40) { bs[(yy & 3) * 4 + (xx & 3)] += v; bc[(yy & 3) * 4 + (xx & 3)]++; }
					for (k = 0; k < 4; k++) {
						int y2 = yy + off[k][0], x2 = xx + off[k][1];
						if (isp[y2 & 15][x2 & 15]) { ok = 0; break; }
						nb[k] = in[(size_t)y2 * S + x2];
						if (nb[k] < mn) mn = nb[k];
						if (nb[k] > mx) mx = nb[k];
					}
					if (!ok || mx >= 1015) continue;
					double m = (nb[0] + nb[1] + nb[2] + nb[3]) / 4.0 - g_ped;
					if (m < 16 || mx - mn > 0.10 * m + 10) continue;
					for (b = 0; b < DPB - 1 && m >= blo[b + 1]; b++) ;
					sum[IX(b, cp, y * 16 + x)] += (v - g_ped) / m - 1; cnt[IX(b, cp, y * 16 + x)]++;
					if (isp[y][x] && v >= 1015) clp[IX(b, cp, y * 16 + x)]++;
				}
		}
	{ long acc = 0, tot = nh; int p01 = -1, p1 = -1, p50 = -1; double mean = 0;
	  for (i = 0; i < 4096; i++) { mean += (double)i * hist[i]; acc += hist[i];
		if (p01 < 0 && acc >= tot * 0.001) p01 = i; if (p1 < 0 && acc >= tot * 0.01) p1 = i; if (p50 < 0 && acc >= tot * 0.5) p50 = i; }
	  LOGI("pdraw: raw mean %.1f  p0.1 %d  p1 %d  median %d  max %d  (pedestal assumed %d)", mean / (double)(tot > 0 ? tot : 1), p01, p1, p50, vmax, g_ped);
	  char bl[300]; int o = 0; o += snprintf(bl, sizeof bl, "pdraw: mean raw of pixels < pedestal+40, per quad phase [y&3][x&3] (only a true black level in a dark frame):");
	  for (i = 0; i < 16 && o < (int)sizeof bl - 20; i++) o += snprintf(bl + o, sizeof bl - o, bc[i] >= 200 ? " %.1f" : " n/a", bc[i] ? bs[i] / bc[i] : 0.0);
	  LOGI("%s", bl); }
	for (b = 0; b < DPB; b++) {
		long n0 = 1 << 30; int cp;
		for (i = 0; i < 8; i++) { long c = 0; for (cp = 0; cp < 4; cp++) c += cnt[IX(b, cp, kPdPos[i][0] * 16 + kPdPos[i][1])]; if (c < n0) n0 = c; }
		if (n0 < 100) continue;
		LOGI("pdraw bin %d: neighbour level %d..%d DN above pedestal, >= %ld flat samples per position (all 4 copies)", b, blo[b], b == DPB - 1 ? 1023 : blo[b + 1], n0);
		for (i = 0; i < 8; i++) {
			int py = kPdPos[i][0], px = kPdPos[i][1], pos = py * 16 + px, d, dy, dx; double ring[4] = { 0 }; int rn[4] = { 0 };
			double ps = 0, pc = 0, cl = 0, cpd[4]; char cps[80]; int o2 = 0;
			for (cp = 0; cp < 4; cp++) {
				ps += sum[IX(b, cp, pos)]; pc += cnt[IX(b, cp, pos)]; cl += clp[IX(b, cp, pos)];
				cpd[cp] = cnt[IX(b, cp, pos)] >= 20 ? 100 * sum[IX(b, cp, pos)] / cnt[IX(b, cp, pos)] : 0.0;
				o2 += snprintf(cps + o2, sizeof cps - o2, cnt[IX(b, cp, pos)] >= 20 ? " %+.0f" : " n/a", cpd[cp]);
			}
			for (d = 1; d <= 3; d++)
				for (dy = -d; dy <= d; dy++) for (dx = -d; dx <= d; dx++) {
					if ((dy < 0 ? -dy : dy) != d && (dx < 0 ? -dx : dx) != d) continue;
					int ry = (py + dy) & 15, rx = (px + dx) & 15, rp = ry * 16 + rx; double rs = 0, rc = 0;
					if (isp[ry][rx]) continue;
					for (cp = 0; cp < 4; cp++) { rs += sum[IX(b, cp, rp)]; rc += cnt[IX(b, cp, rp)]; }
					if (rc < 50) continue;
					ring[d] += rs / rc; rn[d]++;
				}
			LOGI("pdraw   (%2d,%2d) pd %+6.1f%% [c00 c01 c10 c11:%s]  clip %5.1f%%  ring d1 %+5.2f%%  d2 %+5.2f%%  d3 %+5.2f%%", py, px, pc > 0 ? 100 * ps / pc : 0.0, cps,
			     pc > 0 ? 100 * cl / pc : 0.0, rn[1] ? 100 * ring[1] / rn[1] : 0.0, rn[2] ? 100 * ring[2] / rn[2] : 0.0, rn[3] ? 100 * ring[3] / rn[3] : 0.0);
		}
	}
#undef IX
	free(sum); free(cnt); free(clp); free(hist);
}

/* --------------------------------------------------------------- diag: colour dependence of the PD response
 * Hypothesis under test: the PD pixels are green-filtered pixels sitting in non-green quad slots, so that
 * PD / neighbours = a * (G / slot colour) with a constant per position type. For every flat, unclipped 16x16 tile the
 * mean of the non-PD pixels per colour (above the pedestal) gives G/slot for the two PD types (weak: slot of
 * (0,2); strong: slot of (6,5)); each PD pixel gives y = (v - ped) / (mean of its 4 neighbours 4 px away - ped).
 * Logged per type and green-level bin: the fit y = a * r (through the origin) and y = c + b * r, with R2 against
 * the constant-y model, and the table of y/r per ratio bin. If the hypothesis holds, a is constant across ratio
 * bins and level bins and R2 is close to 1; if y is flat in r, it is wrong. */
#define DCB 7
#define DCL 4
static void diag_pdcol(const uint16_t *in, int S)
{
	static const double redge[DCB + 1] = { 0, 0.7, 0.9, 1.1, 1.4, 1.8, 2.5, 1e9 };
	static const int ledge[DCL + 1] = { 16, 48, 128, 300, 600 };
	static const int off[4][2] = { { -4, 0 }, { 4, 0 }, { 0, -4 }, { 0, 4 } };
	static const char cn[3] = { 'R', 'G', 'B' };
	static double acc[2][DCL][DCB][3], fs[2][DCL][6];
	static double rg[2][DCB][2][2], rf[2][2][5];   /* ring by colour ratio: [type][ratio bin][mate, other d1][n, sum dev]; fit sums [type][kind][n, x, y, xx, xy] with x = r - 1 */
	unsigned char isp[16][16]; long ntile = 0, nvalid = 0; int ty, tx, y, x, k, i, t, l, rb;
	int slot[2] = { g_qcol[kPdPos[0][0] & 3][kPdPos[0][1] & 3], g_qcol[kPdPos[2][0] & 3][kPdPos[2][1] & 3] };
	memset(acc, 0, sizeof acc); memset(fs, 0, sizeof fs); memset(isp, 0, sizeof isp); memset(rg, 0, sizeof rg); memset(rf, 0, sizeof rf);
	for (i = 0; i < 8; i++) isp[kPdPos[i][0]][kPdPos[i][1]] = 1;
	if (slot[0] == CG || slot[1] == CG) { LOGI("pdcol: a PD slot is green, ratio undefined"); return; }
	for (ty = 1; ty * 16 + 20 < g_h; ty += 3)
		for (tx = 1; tx * 16 + 20 < g_w; tx += 5) {
			double s1[3] = { 0, 0, 0 }, s2[3] = { 0, 0, 0 }, m[3]; long n[3] = { 0, 0, 0 }; int clipped = 0, okc[3], okt[2];
			ntile++;
			for (y = 0; y < 16; y++)
				for (x = 0; x < 16; x++) {
					if (isp[y][x]) continue;
					int v = in[(size_t)(ty * 16 + y) * S + tx * 16 + x], c = g_qcol[y & 3][x & 3];
					if (v >= 1015) clipped = 1;
					s1[c] += v - g_ped; s2[c] += (double)(v - g_ped) * (v - g_ped); n[c]++;
				}
			if (clipped) continue;
			for (k = 0; k < 3; k++) {
				m[k] = s1[k] / n[k];
				double sd = sqrt(s2[k] / n[k] - m[k] * m[k] > 0 ? s2[k] / n[k] - m[k] * m[k] : 0);
				okc[k] = !(m[k] < 16 || sd > 0.05 * m[k] + 6);
			}
			/* only green and the type's own slot colour need to be flat and above noise: a warm-lit scene has almost no blue */
			okt[0] = okc[CG] && okc[slot[0]]; okt[1] = okc[CG] && okc[slot[1]];
			if (!okt[0] && !okt[1]) continue;
			nvalid++;
			for (l = 0; l < DCL && !(m[CG] >= ledge[l] && m[CG] < ledge[l + 1]); l++) ;
			if (l == DCL) continue;
			for (i = 0; i < 8; i++) {
				int yy = ty * 16 + kPdPos[i][0], xx = tx * 16 + kPdPos[i][1], v = in[(size_t)yy * S + xx], nb[4], mn = 1 << 30, mx = -1, good = 1;
				for (k = 0; k < 4; k++) {
					int y2 = yy + off[k][0], x2 = xx + off[k][1];
					if (isp[y2 & 15][x2 & 15]) { good = 0; break; }
					nb[k] = in[(size_t)y2 * S + x2]; if (nb[k] < mn) mn = nb[k]; if (nb[k] > mx) mx = nb[k];
				}
				if (!good || mx >= 1015) continue;
				double mm = (nb[0] + nb[1] + nb[2] + nb[3]) / 4.0 - g_ped;
				if (mm < 16 || mx - mn > 0.10 * mm + 10) continue;
				t = (i == 2 || i == 3 || i == 6 || i == 7);
				if (!okt[t]) continue;
				double yv = (v - g_ped) / mm, r = m[CG] / m[slot[t]];
				for (rb = 0; rb < DCB - 1 && r >= redge[rb + 1]; rb++) ;
				acc[t][l][rb][0] += 1; acc[t][l][rb][1] += yv; acc[t][l][rb][2] += r;
				fs[t][l][0] += 1; fs[t][l][1] += yv; fs[t][l][2] += yv * yv; fs[t][l][3] += r; fs[t][l][4] += r * r; fs[t][l][5] += r * yv;
				{ int dy, dx, mdx = (kPdPos[i][1] & 1) ? -1 : 1;     /* the cell-mate is the other pixel of the same 2x2 cell: same row, x ^ 1 */
				  for (dy = -1; dy <= 1; dy++)
					for (dx = -1; dx <= 1; dx++) {
						if (!dy && !dx) continue;
						int qy = yy + dy, qx = xx + dx, ky = (kPdPos[i][0] + dy) & 15, kx = (kPdPos[i][1] + dx) & 15, kind = (dy == 0 && dx == mdx) ? 0 : 1, q2, good2 = 1, qmn = 1 << 30, qmx = -1, nb2[4];
						if (isp[ky][kx]) continue;
						for (q2 = 0; q2 < 4; q2++) {
							int y3 = qy + off[q2][0], x3 = qx + off[q2][1];
							if (isp[y3 & 15][x3 & 15]) { good2 = 0; break; }
							nb2[q2] = in[(size_t)y3 * S + x3]; if (nb2[q2] < qmn) qmn = nb2[q2]; if (nb2[q2] > qmx) qmx = nb2[q2];
						}
						if (!good2 || qmx >= 1015) continue;
						double qm = (nb2[0] + nb2[1] + nb2[2] + nb2[3]) / 4.0 - g_ped;
						if (qm < 16 || qmx - qmn > 0.10 * qm + 10) continue;
						double dv = (in[(size_t)qy * S + qx] - g_ped) / qm - 1;
						rg[t][rb][kind][0] += 1; rg[t][rb][kind][1] += dv;
						rf[t][kind][0] += 1; rf[t][kind][1] += r - 1; rf[t][kind][2] += dv; rf[t][kind][3] += (r - 1) * (r - 1); rf[t][kind][4] += (r - 1) * dv;
					} }
			}
		}
	LOGI("pdcol: %ld tiles sampled, %ld flat, unclipped and above noise in green and at least one slot colour; weak positions sit in a %c slot, strong in a %c slot",
	     ntile, nvalid, cn[slot[0]], cn[slot[1]]);
	for (t = 0; t < 2; t++)
		for (l = 0; l < DCL; l++) {
			double *f = fs[t][l]; double n = f[0];
			if (n < 100) continue;
			double sst = f[2] - f[1] * f[1] / n, a = f[5] / f[4], sxy = f[5] - f[1] * f[3] / n, sxx = f[4] - f[3] * f[3] / n;
			double b = sxx > 1e-9 ? sxy / sxx : 0, c = (f[1] - b * f[3]) / n;
			double r2a = sst > 1e-12 ? 1 - (f[2] - a * f[5]) / sst : 0, r2b = sst > 1e-12 ? 1 - (sst - b * sxy) / sst : 0;
			LOGI("pdcol %s (%c slot), green level %d..%d DN above ped: n=%.0f  mean y=%.3f  fit y=a*r: a=%.3f R2=%.2f | y=c+b*r: c=%.3f b=%.3f R2=%.2f",
			     t ? "strong" : "weak  ", cn[slot[t]], ledge[l], ledge[l + 1], n, f[1] / n, a, r2a, c, b, r2b);
			for (rb = 0; rb < DCB; rb++) {
				double *q = acc[t][l][rb];
				if (q[0] < 20) continue;
				LOGI("pdcol    G/%c %.1f..%.1f: n=%5.0f  mean r %.2f  mean y %.3f  y/r %.3f", cn[slot[t]], redge[rb], rb == DCB - 1 ? 9.9 : redge[rb + 1], q[0], q[2] / q[0], q[1] / q[0], q[1] / q[2]);
			}
		}
	for (t = 0; t < 2; t++) {
		int kind;
		for (rb = 0; rb < DCB; rb++) {
			double *a0 = rg[t][rb][0], *a1 = rg[t][rb][1];
			if (a0[0] < 100 || a1[0] < 100) continue;
			LOGI("pdcol ring %s (%c slot) G/%c %.1f..%.1f: cell-mate %+.2f%% (n=%.0f)  other d1 %+.2f%% (n=%.0f)", t ? "strong" : "weak  ", cn[slot[t]], cn[slot[t]], redge[rb], rb == DCB - 1 ? 9.9 : redge[rb + 1],
			     100 * a0[1] / a0[0], a0[0], 100 * a1[1] / a1[0], a1[0]);
		}
		for (kind = 0; kind < 2; kind++) {
			double *f = rf[t][kind]; double n = f[0];
			if (n < 200) continue;
			double sxx = f[3] - f[1] * f[1] / n, sxy = f[4] - f[1] * f[2] / n, b = sxx > 1e-9 ? sxy / sxx : 0, c = (f[2] - b * f[1]) / n;
			LOGI("pdcol ring fit %s (%c slot) %s: dev = %+.2f%% %+.2f%% * (G/%c - 1)   (n=%.0f)", t ? "strong" : "weak  ", cn[slot[t]], kind ? "other d1 " : "cell-mate", 100 * c, 100 * b, cn[slot[t]], n);
		}
	}
}

/* --------------------------------------------------------------- diag: per-position linear mixing fit + black levels
 * Model under test (suggested in review): every PD pixel and every cell-mate is a linear mix of the local colour levels,
 *   v - ped = cR*Rbar + cG*Gbar + cB*Bbar + off,
 * where Rbar/Gbar/Bbar are the tile means (above the pedestal) of the pixels that are neither PD pixels nor within
 * Chebyshev distance 1 of one, and off is a per-position offset (a PD-specific black level would show up here).
 * Fitted by least squares over flat, unclipped tiles, separately for the centre third of the frame and the rest
 * (optical mixing depends on chief-ray angle and would drift with field position; charge crosstalk would not).
 * Targets: the 8 PD positions and their 8 cell-mates (same row, x ^ 1). Positions are pooled by type for the
 * "weak"/"strong" lines and also printed one by one. "rms3" is the residual with all three colours + offset,
 * "rms2" with only green and the slot colour + offset: if rms3 << rms2 the third colour matters.
 * pddark: mean raw of PD pixels, mates and clean pixels of the same colour. Only a true black-level comparison
 * in a dark (lens-covered) frame; in a lit scene it just shows the usual PD excess. */
#define PMK 16
static int pm_solve(double *A, double *b, int n, double *x)
{
	double M[4][5]; int i, j, k;
	for (i = 0; i < n; i++) { for (j = 0; j < n; j++) M[i][j] = A[i * 4 + j]; M[i][n] = b[i]; M[i][i] += 1e-9 * (A[0] + A[5] + A[10] + 1.0); }
	for (i = 0; i < n; i++) {
		int p = i; for (k = i + 1; k < n; k++) if (fabs(M[k][i]) > fabs(M[p][i])) p = k;
		if (fabs(M[p][i]) < 1e-12) return 0;
		if (p != i) for (j = 0; j <= n; j++) { double t = M[i][j]; M[i][j] = M[p][j]; M[p][j] = t; }
		for (k = i + 1; k < n; k++) { double f = M[k][i] / M[i][i]; for (j = i; j <= n; j++) M[k][j] -= f * M[i][j]; }
	}
	for (i = n - 1; i >= 0; i--) { double t = M[i][n]; for (j = i + 1; j < n; j++) t -= M[i][j] * x[j]; x[i] = t / M[i][i]; }
	return 1;
}
static void diag_pdmix(const uint16_t *in, int S)
{
	static double A[PMK][2][16], Bv[PMK][2][4], Syy[PMK][2], N[PMK][2];
	unsigned char isp[16][16], ex[16][16];
	int mate[8][2], ty, tx, y, x, i, k, j, reg, dy, dx, nx = g_w / 16, ny = g_h / 16;
	double dsum[3] = { 0, 0, 0 }, dcnt[3] = { 0, 0, 0 }, cs[3] = { 0, 0, 0 }, cn[3] = { 0, 0, 0 };   /* [0] weak PD, [1] strong PD, [2] unused  */
	double msum[2] = { 0, 0 }, mcnt[2] = { 0, 0 }, psum[2] = { 0, 0 }, pcnt[2] = { 0, 0 };
	int slot[2] = { g_qcol[kPdPos[0][0] & 3][kPdPos[0][1] & 3], g_qcol[kPdPos[2][0] & 3][kPdPos[2][1] & 3] };
	long ntile = 0, nok = 0;
	memset(A, 0, sizeof A); memset(Bv, 0, sizeof Bv); memset(Syy, 0, sizeof Syy); memset(N, 0, sizeof N);
	memset(isp, 0, sizeof isp); memset(ex, 0, sizeof ex);
	for (i = 0; i < 8; i++) {
		isp[kPdPos[i][0]][kPdPos[i][1]] = 1; mate[i][0] = kPdPos[i][0]; mate[i][1] = kPdPos[i][1] ^ 1;
		for (dy = -1; dy <= 1; dy++) for (dx = -1; dx <= 1; dx++) ex[(kPdPos[i][0] + dy) & 15][(kPdPos[i][1] + dx) & 15] = 1;
	}
	for (ty = 1; ty * 16 + 20 < g_h; ty += 3)
		for (tx = 1; tx * 16 + 20 < g_w; tx += 5) {
			double s1[3] = { 0, 0, 0 }, s2[3] = { 0, 0, 0 }, m[3], xv[4]; long n[3] = { 0, 0, 0 }; int clipped = 0, ok = 1;
			ntile++;
			for (y = 0; y < 16; y++)
				for (x = 0; x < 16; x++) {
					int v = in[(size_t)(ty * 16 + y) * S + tx * 16 + x];
					if (v >= 1015) clipped = 1;
					if (ex[y][x]) continue;
					int c = g_qcol[y & 3][x & 3];
					s1[c] += v - g_ped; s2[c] += (double)(v - g_ped) * (v - g_ped); n[c]++;
					cs[c] += v; cn[c]++;
				}
			if (clipped) continue;
			for (k = 0; k < 3; k++) {
				m[k] = n[k] ? s1[k] / n[k] : 0;
				double sd = n[k] ? sqrt(s2[k] / n[k] - m[k] * m[k] > 0 ? s2[k] / n[k] - m[k] * m[k] : 0) : 1e9;
				if (sd > 0.05 * (m[k] > 0 ? m[k] : 0) + 6) ok = 0;      /* the third colour may be small, but it must be flat */
			}
			if (!ok || m[CG] < 16 || m[CG] >= 600) continue;
			nok++;
			reg = (tx >= nx / 3 && tx < 2 * nx / 3 && ty >= ny / 3 && ty < 2 * ny / 3) ? 0 : 1;
			xv[0] = m[CR]; xv[1] = m[CG]; xv[2] = m[CB]; xv[3] = 1.0;
			for (k = 0; k < 16; k++) {
				int py = k < 8 ? kPdPos[k][0] : mate[k - 8][0], px = k < 8 ? kPdPos[k][1] : mate[k - 8][1];
				double yv = in[(size_t)(ty * 16 + py) * S + tx * 16 + px] - g_ped;
				if (yv + g_ped >= 1015) continue;
				for (i = 0; i < 4; i++) { for (j = 0; j < 4; j++) A[k][reg][i * 4 + j] += xv[i] * xv[j]; Bv[k][reg][i] += xv[i] * yv; }
				Syy[k][reg] += yv * yv; N[k][reg] += 1;
				{ int t = (k & 7) == 2 || (k & 7) == 3 || (k & 7) == 6 || (k & 7) == 7;
				  if (k < 8) { psum[t] += yv + g_ped; pcnt[t] += 1; } else { msum[t] += yv + g_ped; mcnt[t] += 1; } }
			}
			(void)dsum; (void)dcnt;
		}
	LOGI("pdmix: %ld tiles sampled, %ld flat/unclipped with green 16..600 DN; centre third = tile rows/cols in the middle third of the frame", ntile, nok);
	{ static const char *tn[2] = { "weak  ", "strong" }, *kn[2] = { "PD  ", "mate" }, *rn[3] = { "all   ", "centre", "outer " };
	  int t, kind, rr;
	  for (t = 0; t < 2; t++)
		for (kind = 0; kind < 2; kind++)
			for (rr = 0; rr < 3; rr++) {
				double AA[16] = { 0 }, BB[4] = { 0 }, syy = 0, nn = 0, x4[4] = { 0 }, x3[4] = { 0 };
				for (i = 0; i < 4; i++) {
					int pos = t ? (i == 0 ? 2 : i == 1 ? 3 : i == 2 ? 6 : 7) : (i == 0 ? 0 : i == 1 ? 1 : i == 2 ? 4 : 5), kk = kind * 8 + pos;
					for (reg = 0; reg < 2; reg++) {
						if (rr == 1 && reg != 0) continue;
						if (rr == 2 && reg != 1) continue;
						for (j = 0; j < 16; j++) AA[j] += A[kk][reg][j];
						for (j = 0; j < 4; j++) BB[j] += Bv[kk][reg][j];
						syy += Syy[kk][reg]; nn += N[kk][reg];
					}
				}
				if (nn < 300) continue;
				if (!pm_solve(AA, BB, 4, x4)) continue;
				double sse4 = syy; for (j = 0; j < 4; j++) sse4 -= x4[j] * BB[j];
				/* two-colour model: green and the slot colour + offset */
				{ int idx[3] = { CG, slot[t], 3 }; double A2[16] = { 0 }, B2[4] = { 0 }; int a, b2;
				  for (a = 0; a < 3; a++) { B2[a] = BB[idx[a]]; for (b2 = 0; b2 < 3; b2++) A2[a * 4 + b2] = AA[idx[a] * 4 + idx[b2]]; }
				  double sse3 = syy; if (pm_solve(A2, B2, 3, x3)) for (a = 0; a < 3; a++) sse3 -= x3[a] * B2[a];
				  double meany = 0; (void)meany;
				  LOGI("pdmix %s %s %s: R %+.3f G %+.3f B %+.3f off %+5.2f DN (sum of colours %.3f) | rms3 %.2f DN, rms2(G,slot) %.2f DN  n=%.0f", tn[t], kn[kind], rn[rr],
				       x4[0], x4[1], x4[2], x4[3], x4[0] + x4[1] + x4[2], sqrt(sse4 > 0 ? sse4 / nn : 0), sqrt(sse3 > 0 ? sse3 / nn : 0), nn); }
			} }
	for (k = 0; k < 16; k++) {
		double AA[16], BB[4], x4[4]; int reg2;
		for (j = 0; j < 16; j++) AA[j] = A[k][0][j] + A[k][1][j];
		for (j = 0; j < 4; j++) BB[j] = Bv[k][0][j] + Bv[k][1][j];
		(void)reg2;
		if (N[k][0] + N[k][1] < 300 || !pm_solve(AA, BB, 4, x4)) continue;
		LOGI("pdmix pos (%2d,%2d) %s: R %+.3f G %+.3f B %+.3f off %+5.2f DN  n=%.0f", k < 8 ? kPdPos[k][0] : mate[k - 8][0], k < 8 ? kPdPos[k][1] : mate[k - 8][1], k < 8 ? "PD  " : "mate", x4[0], x4[1], x4[2], x4[3], N[k][0] + N[k][1]);
	}
	LOGI("pddark: mean raw  weak PD %.1f  weak mate %.1f  clean R %.1f | strong PD %.1f  strong mate %.1f  clean B %.1f | clean G %.1f  (a black-level comparison only if the frame is dark)",
	     pcnt[0] ? psum[0] / pcnt[0] : 0.0, mcnt[0] ? msum[0] / mcnt[0] : 0.0, cn[slot[0]] ? cs[slot[0]] / cn[slot[0]] : 0.0,
	     pcnt[1] ? psum[1] / pcnt[1] : 0.0, mcnt[1] ? msum[1] / mcnt[1] : 0.0, cn[slot[1]] ? cs[slot[1]] / cn[slot[1]] : 0.0, cn[CG] ? cs[CG] / cn[CG] : 0.0);
}

/* --------------------------------------------------------------- calibration kept across library reloads
 * The HAL gives us a fresh copy of this library's statics whenever the camera session is reopened (screen lock,
 * app resume) although the camera process keeps running, so every such reopen cost ~8 shots of recalibration.
 * Fix: after every shot the calibration is copied into a malloc'd block that is never freed, and its address is
 * kept in an environment variable of the process (which survives library unload/reload). remosaic_init copies it
 * back when the statics are fresh. It does not survive a restart of the camera process. Android builds only, so
 * the PC test is unaffected. persist.vendor.remoshim.persist=0 turns it off. */
static int g_eqshots, g_eqw, g_eqh;
#ifdef __ANDROID__
#define CAL_ENV   "REMOSHIM_CALSTORE"
#define CAL_MAGIC 0x52454d53u
#define CAL_VER   2u
struct calstate {
	uint32_t magic, ver, w, h, bayer;
	int accshots, accw, acch, accb, t16shot, eqshots, eqw, eqh, gridused, calused, pdn, pdw, pdh, pdshot, have_cand, accpdc;
	double accs[256][5];
	float t16g[16][16], t16o[16][16], gog[16], goo[16];
	unsigned char pdm[16][16], pdunion[16][16];
	char pdmsg[600], candmsg[700];
	double cand_g[NCAND][GY][GX][16], cand_o[NCAND][GY][GX][16];
};
static struct calstate *cal_find(void)
{
	const char *e = getenv(CAL_ENV);
	struct calstate *b;
	if (!e || !*e) return NULL;
	b = (struct calstate *)(uintptr_t)strtoull(e, NULL, 16);
	return (b && b->magic == CAL_MAGIC && b->ver == CAL_VER) ? b : NULL;
}
static void cal_save(void)
{
	struct calstate *b = cal_find();
	char buf[40];
	if (!prop_int("persist.vendor.remoshim.persist", 1) || g_accshots <= 0) return;
	if (!b) {
		b = calloc(1, sizeof *b);
		if (!b) return;
		snprintf(buf, sizeof buf, "%llx", (unsigned long long)(uintptr_t)b);
		setenv(CAL_ENV, buf, 1);
	}
	b->magic = CAL_MAGIC; b->ver = CAL_VER; b->w = g_w; b->h = g_h; b->bayer = g_bayer;
	b->accshots = g_accshots; b->accw = g_accw; b->acch = g_acch; b->accb = g_accb; b->t16shot = g_t16shot; b->accpdc = g_accpdc;
	b->eqshots = g_eqshots; b->eqw = g_eqw; b->eqh = g_eqh; b->gridused = g_gridused; b->calused = g_calused;
	b->pdn = g_pdn; b->pdw = g_pdw; b->pdh = g_pdh; b->pdshot = g_pdshot;
	memcpy(b->accs, g_accs, sizeof b->accs); memcpy(b->t16g, g_t16g, sizeof b->t16g); memcpy(b->t16o, g_t16o, sizeof b->t16o);
	memcpy(b->gog, g_gog, sizeof b->gog); memcpy(b->goo, g_goo, sizeof b->goo);
	memcpy(b->pdm, g_pdm, sizeof b->pdm); memcpy(b->pdunion, g_pdunion, sizeof b->pdunion);
	memcpy(b->pdmsg, g_pdmsg, sizeof b->pdmsg); memcpy(b->candmsg, g_candmsg, sizeof b->candmsg);
	b->have_cand = g_cand_g && g_cand_o;
	if (b->have_cand) {
		memcpy(b->cand_g, g_cand_g, sizeof b->cand_g); memcpy(b->cand_o, g_cand_o, sizeof b->cand_o);
	}
}
static void cal_restore(void)
{
	struct calstate *b = cal_find();
	if (!b || !prop_int("persist.vendor.remoshim.persist", 1)) return;
	if (b->w != (uint32_t)g_w || b->h != (uint32_t)g_h || b->bayer != (uint32_t)g_bayer) return;
	if (g_accshots >= b->accshots) return;                 /* statics survived on their own, nothing to restore */
	if (b->have_cand && !g_cand_g) {
		g_cand_g = malloc(sizeof(*g_cand_g) * NCAND); g_cand_o = malloc(sizeof(*g_cand_o) * NCAND);
		if (!g_cand_g || !g_cand_o) { free(g_cand_g); free(g_cand_o); g_cand_g = NULL; g_cand_o = NULL; return; }
	}
	g_accshots = b->accshots; g_accw = b->accw; g_acch = b->acch; g_accb = b->accb; g_t16shot = b->t16shot; g_accpdc = b->accpdc;
	g_eqshots = b->eqshots; g_eqw = b->eqw; g_eqh = b->eqh; g_gridused = b->gridused; g_calused = b->calused;
	g_pdn = b->pdn; g_pdw = b->pdw; g_pdh = b->pdh; g_pdshot = b->pdshot;
	memcpy(g_accs, b->accs, sizeof g_accs); memcpy(g_t16g, b->t16g, sizeof g_t16g); memcpy(g_t16o, b->t16o, sizeof g_t16o);
	memcpy(g_gog, b->gog, sizeof g_gog); memcpy(g_goo, b->goo, sizeof g_goo);
	memcpy(g_pdm, b->pdm, sizeof g_pdm); memcpy(g_pdunion, b->pdunion, sizeof g_pdunion);
	memcpy(g_pdmsg, b->pdmsg, sizeof g_pdmsg); memcpy(g_candmsg, b->candmsg, sizeof g_candmsg);
	if (b->have_cand) { memcpy(g_cand_g, b->cand_g, sizeof b->cand_g); memcpy(g_cand_o, b->cand_o, sizeof b->cand_o); }
	LOGI("init: restored calibration from the process store (accshots=%d, eqshots=%d, pd positions=%d)", g_accshots, g_eqshots, g_pdn);
}
#else
static void cal_save(void) {}
static void cal_restore(void) {}
#endif

/* --------------------------------------------------------------- API */
/* --------------------------------------------------------------- diag (v8): pddark2 - ungated black-level comparison
 * pdmix/pddark only look at flat tiles with green >= 16 DN above the pedestal, so a dark (lens-covered / black-fabric)
 * frame gives 0.0 for the PD and mate means. This pass has no flatness or brightness-floor gate: it takes every sampled
 * tile that is unclipped (no sample >= 1015) and whose mean green above the pedestal is below PDK_DARK, and compares
 * each of the 8 PD positions and 8 cell-mates with the mean of the NON-PD pixels at the same quad phase (y&3, x&3),
 * excluding the 3x3 neighbourhood of every PD pixel. In a dark frame the difference is the PD / mate black-level offset
 * in DN. Pooled lines (weak/strong x PD/mate) average the per-position differences. se = standard error of the mean. */
#define PDK_DARK 20.0
static void diag_pddark2(const uint16_t *in, int S)
{
	unsigned char ex[16][16];
	double ks[16] = { 0 }, kq[16] = { 0 }, kn[16] = { 0 }, ps[4][4] = { { 0 } }, pn[4][4] = { { 0 } }, df[16], se[16], mn[16];
	int mate[8][2], ty, tx, y, x, i, k, dy, dx;
	long ntile = 0, ndark = 0, nclip = 0;
	memset(ex, 0, sizeof ex);
	for (i = 0; i < 8; i++) {
		mate[i][0] = kPdPos[i][0]; mate[i][1] = kPdPos[i][1] ^ 1;
		for (dy = -1; dy <= 1; dy++) for (dx = -1; dx <= 1; dx++) ex[(kPdPos[i][0] + dy) & 15][(kPdPos[i][1] + dx) & 15] = 1;
	}
	for (ty = 1; ty * 16 + 20 < g_h; ty += 3)
		for (tx = 1; tx * 16 + 20 < g_w; tx += 5) {
			double gs = 0, gn = 0; int clipped = 0;
			ntile++;
			for (y = 0; y < 16; y++)
				for (x = 0; x < 16; x++) {
					int v = in[(size_t)(ty * 16 + y) * S + tx * 16 + x];
					if (v >= 1015) clipped = 1;
					if (!ex[y][x] && g_qcol[y & 3][x & 3] == CG) { gs += v - g_ped; gn += 1; }
				}
			if (clipped) { nclip++; continue; }
			if (gn < 1 || gs / gn >= PDK_DARK) continue;
			ndark++;
			for (y = 0; y < 16; y++)
				for (x = 0; x < 16; x++) {
					if (ex[y][x]) continue;
					ps[y & 3][x & 3] += in[(size_t)(ty * 16 + y) * S + tx * 16 + x]; pn[y & 3][x & 3] += 1;
				}
			for (k = 0; k < 16; k++) {
				int py = k < 8 ? kPdPos[k][0] : mate[k - 8][0], px = k < 8 ? kPdPos[k][1] : mate[k - 8][1];
				double v = in[(size_t)(ty * 16 + py) * S + tx * 16 + px];
				ks[k] += v; kq[k] += v * v; kn[k] += 1;
			}
		}
	LOGI("pddark2: %ld tiles sampled, %ld dark (green < %.0f DN above pedestal, unclipped), %ld dropped for clipped/hot samples", ntile, ndark, PDK_DARK, nclip);
	if (ndark < 100) { LOGI("pddark2: too few dark tiles (need >= 100) - no result; use a darker, unclipped scene"); return; }
	for (k = 0; k < 16; k++) {
		int py = k < 8 ? kPdPos[k][0] : mate[k - 8][0], px = k < 8 ? kPdPos[k][1] : mate[k - 8][1];
		double m = ks[k] / kn[k], var = kq[k] / kn[k] - m * m, ph = ps[py & 3][px & 3] / pn[py & 3][px & 3];
		mn[k] = m; df[k] = m - ph; se[k] = sqrt((var > 0 ? var : 0) / kn[k]);
		LOGI("pddark2 %s (%2d,%2d) %s: mean %.2f  same-phase clean %.2f  diff %+.2f DN (se %.2f)  n=%.0f", k < 8 ? "PD  " : "mate", py, px,
		     ((k & 7) == 2 || (k & 7) == 3 || (k & 7) == 6 || (k & 7) == 7) ? "strong" : "weak  ", m, ph, df[k], se[k], kn[k]);
	}
	{ static const char *tn[2] = { "weak  ", "strong" }, *kn2[2] = { "PD  ", "mate" };
	  int t, kind;
	  for (t = 0; t < 2; t++)
		for (kind = 0; kind < 2; kind++) {
			double d = 0, s2 = 0; int j;
			for (j = 0; j < 4; j++) {
				int pos = t ? (j == 0 ? 2 : j == 1 ? 3 : j == 2 ? 6 : 7) : (j == 0 ? 0 : j == 1 ? 1 : j == 2 ? 4 : 5), kk = kind * 8 + pos;
				d += df[kk] / 4; s2 += se[kk] * se[kk] / 16;
			}
			LOGI("pddark2 pooled %s %s: diff vs same-phase clean %+.2f DN (se %.2f)", tn[t], kn2[kind], d, sqrt(s2));
		} }
	{ char b[300]; int o = 0, py, px;
	  o += snprintf(b, sizeof b, "pddark2 clean mean raw per quad phase [y&3][x&3]:");
	  for (py = 0; py < 4; py++) for (px = 0; px < 4; px++) o += snprintf(b + o, sizeof b - o, " %.2f", pn[py][px] > 0 ? ps[py][px] / pn[py][px] : 0.0);
	  LOGI("%s", b); }
	(void)mn;
}

/* --------------------------------------------------------------- v9: pdcolor - per-frame correction of the 8 cell-mates
 * Each PDAF pixel shares its 2x2 quad cell with a same-colour "mate" (same row, x^1). Measured (pdmix / pddark2): the mate is
 *   mate - ped = alpha * S + beta * G          (S = own-colour level, G = local green, both above the pedestal)
 * with alpha ~ 0.8, beta ~ 0.15 and NO offset (a dark-frame test found none to within 0.05 DN), so a mate reads low in a
 * green-poor scene and high in a green-rich one. The PD pixel itself behaves like G through a different filter and cannot
 * say anything about S, so PD slots stay with the neighbour repair above. Correction per mate position k (8 fits, the
 * upper and lower pixel of a pair differ):
 *     S_hat = ((mate - ped) - beta_k * Ghat) / alpha_k + ped
 * where Ghat is the mean of the nearest green sample in each of the four directions (never a PDAF pixel). alpha_k and beta_k
 * are fitted every frame by ridge least squares on flat unclipped tiles (tile means of the clean slot colour and green as the
 * two regressors; the third colour buys ~2 % and costs collinearity), pulled toward the running coefficients
 * (EMA 0.3 per frame, starting from the measured priors) with weight pdcrho/1000 of the data. Fits with fewer than 300
 * tiles keep the running coefficients. persist.vendor.remoshim.pdcolor=1 enables it (default 0; 2 = alternate corrected/uncorrected per shot; 3 = cycle uncorrected / corrected with gate / corrected without gate).
 * pdcflat=1 (default) leaves a mate alone at edges and next to clipped samples (see pdc_local). It needs pdfixed=1.
 * With diag=1 a check line per colour-ratio bin compares the mate's bias against the tile mean of its colour before and
 * after the correction. */
#define PDC_MINN 300
static const double kPdcPrior[2][2] = { { 0.82, 0.156 }, { 0.74, 0.150 } };       /* weak, strong: alpha, beta */
static double g_pdc_hist[8][2]; static int g_pdc_have = 0;
static const int kPdcPos[2][4] = { { 0, 1, 4, 5 }, { 2, 3, 6, 7 } };
static inline int pdc_type(int k) { return k == 2 || k == 3 || k == 6 || k == 7; }
/* Local green level at a mate (mean of the nearest green in each of the four directions, above the pedestal). With gate=1 it also
 * refuses (returns 0) where the correction would be unreliable: a clipped sample among the greens or the same-colour neighbours
 * 4 px away, or an edge (the four greens, or the four 4-px neighbours, spread by more than 40 DN + 12 % of their mean). At an edge
 * the greens beside the mate can sit on either side of the step while the mate's own cross-talk comes from both, so
 * beta*(G_hat) is wrong by up to ~beta times the step; there the mate is left alone (map16 still applies its average gain). */
static int pdc_local(const uint16_t *in, int S, int y, int x, double *g, int gate)
{
	static const int dd[4][2] = { { 0, -1 }, { 0, 1 }, { -1, 0 }, { 1, 0 } }, sd[4][2] = { { 0, -4 }, { 0, 4 }, { -4, 0 }, { 4, 0 } };
	double s = 0, lo = 1e9, hi = -1e9; int n = 0, i, d, clip = 0;
	for (i = 0; i < 4; i++)
		for (d = 1; d <= 3; d++) {
			int yy = y + dd[i][0] * d, xx = x + dd[i][1] * d;
			if (yy < 0 || yy >= g_h || xx < 0 || xx >= g_w) break;
			if (g_qcol[yy & 3][xx & 3] == CG) {
				int v = in[(size_t)yy * S + xx]; double a = v - g_ped;
				if (v >= 1015) clip = 1;
				s += a; n++; if (a < lo) lo = a; if (a > hi) hi = a; break;
			}
		}
	if (n < 2) return 0;
	*g = s / n;
	if (!gate) return 1;
	if (clip || hi - lo > 40 + 0.12 * (*g > 0 ? *g : 0)) return 0;
	{ double ss = 0, slo = 1e9, shi = -1e9; int ns = 0;
	  for (i = 0; i < 4; i++) {
		int yy = y + sd[i][0], xx = x + sd[i][1];
		if (yy < 0 || yy >= g_h || xx < 0 || xx >= g_w) continue;
		int v = in[(size_t)yy * S + xx]; double a = v - g_ped;
		if (v >= 1015) return 0;
		ss += a; ns++; if (a < slo) slo = a; if (a > shi) shi = a;
	  }
	  if (ns >= 2 && shi - slo > 40 + 0.12 * (ss / ns > 0 ? ss / ns : 0)) return 0; }
	return 1;
}
/* iterate flat tiles: calls back with the tile origin, the colour means (above the pedestal) and the slot colours */
struct pdc_tile { int ty, tx; double m[3]; int ok[2]; };
static int pdc_next_tile(const uint16_t *in, int S, int *ty, int *tx, unsigned char ex[16][16], const int slot[2], struct pdc_tile *t)
{
	for (;;) {
		if (*tx == 0 && *ty == 0) { *ty = 1; *tx = 1; }
		else { *tx += 3; if (*tx * 16 + 20 >= g_w) { *tx = 1; *ty += 2; } }
		if (*ty * 16 + 20 >= g_h) return 0;
		double s1[3] = { 0, 0, 0 }, s2[3] = { 0, 0, 0 }; long n[3] = { 0, 0, 0 }; int clipped = 0, y, x, c, flat[3];
		for (y = 0; y < 16; y++)
			for (x = 0; x < 16; x++) {
				int v = in[(size_t)(*ty * 16 + y) * S + *tx * 16 + x];
				if (v >= 1015) clipped = 1;
				if (ex[y][x]) continue;
				c = g_qcol[y & 3][x & 3]; s1[c] += v - g_ped; s2[c] += (double)(v - g_ped) * (v - g_ped); n[c]++;
			}
		if (clipped) continue;
		for (c = 0; c < 3; c++) {
			double m = n[c] ? s1[c] / n[c] : 0, var = n[c] ? s2[c] / n[c] - m * m : 1e9;
			t->m[c] = m; flat[c] = sqrt(var > 0 ? var : 0) <= 0.05 * (m > 0 ? m : 0) + 6;
		}
		if (!flat[CG] || t->m[CG] < 16 || t->m[CG] >= 600) continue;
		t->ok[0] = flat[slot[0]] && t->m[slot[0]] >= 16 && t->m[slot[0]] < 600;
		t->ok[1] = flat[slot[1]] && t->m[slot[1]] >= 16 && t->m[slot[1]] < 600;
		if (!t->ok[0] && !t->ok[1]) continue;
		t->ty = *ty; t->tx = *tx; return 1;
	}
}
static void pdc_setup(unsigned char ex[16][16], int slot[2])
{
	int i, dy, dx;
	memset(ex, 0, 256);
	for (i = 0; i < 8; i++) for (dy = -1; dy <= 1; dy++) for (dx = -1; dx <= 1; dx++) ex[(kPdPos[i][0] + dy) & 15][(kPdPos[i][1] + dx) & 15] = 1;
	slot[0] = g_qcol[kPdPos[0][0] & 3][kPdPos[0][1] & 3]; slot[1] = g_qcol[kPdPos[2][0] & 3][kPdPos[2][1] & 3];
}
static void pdc_fit(const uint16_t *in, int S, double th[8][2], long nt[2], double nn[8], int fitted[8])
{
	unsigned char ex[16][16]; int slot[2], k, ty = 0, tx = 0, j, t; struct pdc_tile T;
	double A[8][3], B[8][2], N[8];
	memset(A, 0, sizeof A); memset(B, 0, sizeof B); memset(N, 0, sizeof N); nt[0] = nt[1] = 0;
	pdc_setup(ex, slot);
	if (!g_pdc_have) { for (k = 0; k < 8; k++) { g_pdc_hist[k][0] = kPdcPrior[pdc_type(k)][0]; g_pdc_hist[k][1] = kPdcPrior[pdc_type(k)][1]; } g_pdc_have = 1; }
	while (pdc_next_tile(in, S, &ty, &tx, ex, slot, &T))
		for (t = 0; t < 2; t++) {
			if (!T.ok[t]) continue;
			nt[t]++;
			for (j = 0; j < 4; j++) {
				k = kPdcPos[t][j];
				int py = T.ty * 16 + kPdPos[k][0], px = T.tx * 16 + (kPdPos[k][1] ^ 1);
				int v = in[(size_t)py * S + px];
				if (v >= 1015) continue;
				double y = v - g_ped, x1 = T.m[slot[t]], x2 = T.m[CG];
				A[k][0] += x1 * x1; A[k][1] += x1 * x2; A[k][2] += x2 * x2; B[k][0] += x1 * y; B[k][1] += x2 * y; N[k] += 1;
			}
		}
	for (k = 0; k < 8; k++) {
		double *th0 = g_pdc_hist[k]; fitted[k] = 0; nn[k] = N[k];
		th[k][0] = th0[0]; th[k][1] = th0[1];
		if (N[k] < PDC_MINN) continue;
		double lam = g_pdcrho / 1000.0 * 0.5 * (A[k][0] + A[k][2]);
		double a11 = A[k][0] + lam, a12 = A[k][1], a22 = A[k][2] + lam, r1 = B[k][0] + lam * th0[0], r2 = B[k][1] + lam * th0[1];
		double det = a11 * a22 - a12 * a12;
		if (det <= 1e-12 * a11 * a22) continue;
		double al = (r1 * a22 - a12 * r2) / det, be = (a11 * r2 - a12 * r1) / det;
		if (al < 0.55 || al > 1.05 || be < -0.05 || be > 0.40) continue;      /* implausible: keep the running values */
		th[k][0] = al; th[k][1] = be; fitted[k] = 1;
	}
	for (k = 0; k < 8; k++) if (fitted[k]) { g_pdc_hist[k][0] = 0.7 * g_pdc_hist[k][0] + 0.3 * th[k][0]; g_pdc_hist[k][1] = 0.7 * g_pdc_hist[k][1] + 0.3 * th[k][1]; }
}
/* diag: mate bias against the tile mean of its own colour, by green/slot ratio, before and after the correction */
static void pdc_check(const uint16_t *in, int S, double th[8][2])
{
	unsigned char ex[16][16]; int slot[2], ty = 0, tx = 0, j, t, b; struct pdc_tile T;
	double sb[2][3] = { { 0 } }, sa[2][3] = { { 0 } }, sx[2][3] = { { 0 } }, nb[2][3] = { { 0 } };
	static const char *bn[3] = { "G/S<0.9", "0.9..1.4", "G/S>=1.4" };
	pdc_setup(ex, slot);
	while (pdc_next_tile(in, S, &ty, &tx, ex, slot, &T))
		for (t = 0; t < 2; t++) {
			if (!T.ok[t]) continue;
			double r = T.m[CG] / T.m[slot[t]]; b = r < 0.9 ? 0 : r < 1.4 ? 1 : 2;
			for (j = 0; j < 4; j++) {
				int k = kPdcPos[t][j], py = T.ty * 16 + kPdPos[k][0], px = T.tx * 16 + (kPdPos[k][1] ^ 1); double g;
				int v = in[(size_t)py * S + px];
				if (v >= 1015 || !pdc_local(in, S, py, px, &g, g_pdcflat)) continue;
				double y = v - g_ped;
				sb[t][b] += y; sa[t][b] += (y - th[k][1] * g) / th[k][0]; sx[t][b] += T.m[slot[t]]; nb[t][b] += 1;
			}
		}
	for (t = 0; t < 2; t++)
		for (b = 0; b < 3; b++)
			if (nb[t][b] >= 50) LOGI("pdcolor check %s mates, %s: n=%.0f  bias vs tile mean of own colour: before %+.2f%%  after %+.2f%%", t ? "strong" : "weak  ", bn[b], nb[t][b], 100 * (sb[t][b] / sx[t][b] - 1), 100 * (sa[t][b] / sx[t][b] - 1));
			else LOGI("pdcolor check %s mates, %s: n=%.0f (too few)", t ? "strong" : "weak  ", bn[b], nb[t][b]);
}
struct pdcc { uint16_t *in; int S; double th[8][2]; };
static void pdc_job(int y0, int y1, void *p)
{
	struct pdcc *f = p; int y, x, k; long na = 0, nk = 0;
	for (y = y0; y < y1; y++) {
		int ym = y & 15;
		for (k = 0; k < 8; k++) if (kPdPos[k][0] == ym) break;
		if (k == 8) continue;
		for (x = kPdPos[k][1] ^ 1; x < g_w; x += 16) {
			double g, v = f->in[(size_t)y * f->S + x];
			if (v >= 1015 || !pdc_local(f->in, f->S, y, x, &g, g_pdcflat)) { nk++; continue; }
			double c = ((v - g_ped) - f->th[k][1] * g) / f->th[k][0] + g_ped;
			f->in[(size_t)y * f->S + x] = (uint16_t)(c < 0 ? 0 : c > 1023 ? 1023 : lrint(c));
			na++;
		}
	}
	__atomic_fetch_add(&g_pdc_cnt[0], na, __ATOMIC_RELAXED); __atomic_fetch_add(&g_pdc_cnt[1], nk, __ATOMIC_RELAXED);
}
static int pdcolor_run(uint16_t *in, int S)
{
	struct pdcc f; long nt[2]; double nn[8]; int fitted[8], k, nf = 0;
	if (!g_pdfixed || g_pdn != 8) { LOGI("pdcolor: skipped (needs the fixed 8-position PDAF layout: pdfixed=%d, positions=%d)", g_pdfixed, g_pdn); return 0; }
	f.in = in; f.S = S;
	pdc_fit(in, S, f.th, nt, nn, fitted);
	for (k = 0; k < 8; k++) nf += fitted[k];
	LOGI("pdcolor: flat tiles weak %ld strong %ld, %d of 8 positions fitted (others keep the running coefficients)", nt[0], nt[1], nf);
	{ char b[2][260]; int o[2] = { 0, 0 };
	  for (k = 0; k < 8; k++) {
		int t = pdc_type(k);
		o[t] += snprintf(b[t] + o[t], sizeof b[t] - o[t], " (%d,%d) a=%.3f b=%.3f n=%.0f%s", kPdPos[k][0], kPdPos[k][1] ^ 1, f.th[k][0], f.th[k][1], nn[k], fitted[k] ? "" : "*");
	  }
	  LOGI("pdcolor: weak mates  %s", b[0]); LOGI("pdcolor: strong mates%s   (* = running coefficients, too few tiles)", b[1]); }
	if (g_diag) pdc_check(in, S, f.th);
	g_pdc_cnt[0] = g_pdc_cnt[1] = 0;
	run_rows(pdc_job, &f, 32);
	LOGI("pdcolor: corrected %ld of %ld mates, left alone %ld (%s)", g_pdc_cnt[0], g_pdc_cnt[0] + g_pdc_cnt[1], g_pdc_cnt[1], g_pdcflat ? "edge/clip gate ON" : "no gate");
	return 1;
}

EXPORT int remosaic_init(int img_w, int img_h, int bayer_order, int pedestal)
{
	int force = prop_int("persist.vendor.remoshim.bayer", -1), px, py;
	g_w = img_w; g_h = img_h;
	g_mode = prop_int("persist.vendor.remoshim.mode", 0);
	g_eq = prop_int("persist.vendor.remoshim.eq", 3);
	g_usecal = prop_int("persist.vendor.remoshim.usecal", 1);
	g_grid = prop_int("persist.vendor.remoshim.grid", -1);
	g_pd = prop_int("persist.vendor.remoshim.pd", 1);
	g_pdedge = prop_int("persist.vendor.remoshim.pdedge", PDEDGE_DEFAULT);
	g_pdfixed = prop_int("persist.vendor.remoshim.pdfixed", 1);
	g_pdrescan = prop_int("persist.vendor.remoshim.pdrescan", 10);
	g_sharpen = prop_int("persist.vendor.remoshim.sharpen", 0);
	g_pdthr = prop_int("persist.vendor.remoshim.pdthr", 150) / 1000.0;
	g_t16 = prop_int("persist.vendor.remoshim.tile16", 1);
	g_acc = prop_int("persist.vendor.remoshim.acc", 1);
	g_dn = prop_int("persist.vendor.remoshim.dn", 20) / 10.0f;
	g_lo = (float)prop_int("persist.vendor.remoshim.gatelo", 15);
	g_hi = (float)prop_int("persist.vendor.remoshim.gatehi", 35);
	g_inplace = prop_int("persist.vendor.remoshim.inplace", 1);
	if (g_hi <= g_lo) g_hi = g_lo + 1;
	g_ped = pedestal > 0 ? pedestal : 64;
	g_bayer = (force >= 0 && force <= 3) ? force : (bayer_order & 3);
	for (py = 0; py < 4; py++)
		for (px = 0; px < 4; px++) g_qcol[py][px] = bayer_col(g_bayer, (px >> 1) & 1, (py >> 1) & 1);
	for (py = 0; py < 2; py++)
		for (px = 0; px < 2; px++) g_tcol[py][px] = bayer_col(g_bayer, px, py);
	g_dnr_set = prop_int("persist.vendor.remoshim.dnr", -1);
	if (g_dnr_set >= 0) g_dnr_set = g_dnr_set < 2 ? 2 : g_dnr_set > 4 ? 4 : g_dnr_set;
	g_dnr = g_dnr_set >= 0 ? g_dnr_set : 4; g_dnr_tab = g_dnr;
	dn_tables(); gauss_tables(); refine_tables(); lk_tables(); ri_tables(); ri_masks();
	g_threads = prop_int("persist.vendor.remoshim.threads", NTHREADS + 1);
	g_tile = prop_int("persist.vendor.remoshim.tile", 384);
	g_diag = prop_int("persist.vendor.remoshim.diag", 0);
	g_pdcolor = prop_int("persist.vendor.remoshim.pdcolor", 0);
	g_pdcflat = prop_int("persist.vendor.remoshim.pdcflat", 1);
	g_pdcrho = prop_int("persist.vendor.remoshim.pdcrho", 20);
	g_eqmin = prop_int("persist.vendor.remoshim.eqmin", 12);
	LOGI("init v4: %dx%d HAL bayer=%d -> %d, pedestal=%d, mode=%d, eq=%d, dn=%.1f r%d, gate=%.0f..%.0f, inplace=%d",
	     img_w, img_h, bayer_order, g_bayer, pedestal, g_mode, g_eq, g_dn, g_dnr, g_lo, g_hi, g_inplace);
	LOGI("init: carried state at entry: accshots=%d t16shot=%d pdw=%d", g_accshots, g_t16shot, g_pdw);
	LOGI("init: v9 options: pdcolor=%d pdcrho=%d eqmin=%d pdcflat=%d", g_pdcolor, g_pdcrho, g_eqmin, g_pdcflat);
	LOGI("init: release candidate rc1 (built-in defaults: pdcolor off, diag off, eqmin 12; properties still override)");
	cal_restore();
	return 0;
}

EXPORT int remosaic_gainmap_gen(void *eep, unsigned long size)
{
	(void)eep; LOGI("gainmap_gen: size=%lu - not used", size); return 0;
}
EXPORT int remosaic_process_param_set(void *param) { (void)param; return 0; }

EXPORT int remosaic_process(void *in, unsigned long in_size, void *out, unsigned long out_size)
{
	struct timespec t0, ts; struct ctx c; long t_eq, t_dn, t_g, t_ref, t_ri, c_dn = 0, c_g = 0, c_ref = 0, c_ri = 0;
	long nflat = -1; float fit_amin = 1, fit_amax = 1, fit_lmax = 0;
	char gomsg[400], pdmsg[600], t16msg[300], d16raw[80] = "", d16out[80] = "";
	uint16_t *A, *T = NULL, *Dbuf = NULL;
	int stride;
	if (!in || !out || g_w <= 0 || g_h <= 0) { LOGE("process: bad args"); return -1; }
	stride = (int)(in_size / ((unsigned long)g_h * 2));
	if (stride < g_w || out_size < in_size) {
		LOGE("process: size mismatch in=%lu out=%lu w=%d h=%d", in_size, out_size, g_w, g_h);
		return -1;
	}
	memset(&c, 0, sizeof(c)); c.stride = stride;
	clock_gettime(CLOCK_MONOTONIC, &t0);

	if (g_mode == 2) { memcpy(out, in, in_size); LOGI("process: passthrough"); return 0; }
	if (g_mode == 1) {
		c.src = in; c.dst = out; run_strips(swap_run, &c);
		LOGI("process: swap done in %ld ms", ms_since(&t0)); return 0;
	}

	/* buffers: A = input (reused for the denoised frame unless inplace=0), T = scratch, out = output */
	T = frame_alloc(in_size);
	if (!g_inplace) Dbuf = malloc(in_size);
	if (!T || (!g_inplace && !Dbuf)) {
		LOGE("process: alloc failed, falling back to pixel swap");
		frame_free(T, in_size); free(Dbuf); c.src = in; c.dst = out; run_strips(swap_run, &c); return 0;
	}
	A = g_inplace ? (uint16_t *)in : Dbuf;

	pdmsg[0] = 0;
	if (g_diag) { diag_pdraw((const uint16_t *)in, stride); diag_pdcol((const uint16_t *)in, stride); diag_pdmix((const uint16_t *)in, stride); diag_pddark2((const uint16_t *)in, stride); }
	if (g_pd) { if (pd_detect_fix(in, stride, pdmsg, sizeof pdmsg) > 0) { struct pdfix pf = { in, stride }; run_rows(pd_fix_job, &pf, 32); } }
	{ int want = g_pdcolor;
	  if (g_pdcolor == 2) { want = (g_pdab & 1) == 0; g_pdab++; LOGI("pdcolor: A/B mode, shot %d of this process: %s", g_pdab, want ? "CORRECTED" : "UNCORRECTED"); }
	  if (g_pdcolor == 3) {   /* three-way cycle: uncorrected, corrected with the edge/clip gate, corrected without it */
		int ph = g_pdab % 3; g_pdab++; want = ph != 0; g_pdcflat = ph == 1;
		LOGI("pdcolor: A/B/C mode, shot %d of this process: %s", g_pdab, ph == 0 ? "UNCORRECTED" : ph == 1 ? "CORRECTED with edge/clip gate" : "CORRECTED without gate");
	  }
	  g_pdc_on = (g_pd && want) ? (pdcolor_run((uint16_t *)in, stride) ? ((g_pdcolor == 3 && !g_pdcflat) ? 2 : 1) : 0) : 0; }
	long t_pd = ms_since(&t0);
	if (g_diag) { diag_clip(in, stride); diag_pattern(in, stride, 0, "raw input"); }
	/* 1. equalise: in -> T */
	ts = t0;
	long t_alloc = ms_since(&ts), t_eqm, t_eqa, t_nz;
	{ struct timespec tq; clock_gettime(CLOCK_MONOTONIC, &tq);
	nflat = -1;
	gomsg[0] = 0;
	if (g_eq == 3) {
		if (g_eqw != g_w || g_eqh != g_h) { g_eqshots = 0; g_eqw = g_w; g_eqh = g_h; }
		g_eqshots++;
		if (g_acc && g_eqshots > 8 && g_eqshots % 5 != 0)
			snprintf(gomsg, sizeof gomsg, "4x4 equalisation reused (calibrated, shot %d)", g_eqshots);
		else
			if (!go_fit(in, stride, gomsg, sizeof gomsg)) g_eqshots--;
	}
	else if (g_eq == 2) nflat = lk_fit(in, stride);
	else if (g_eq == 1) eq_measure(in, stride, c.gain);
	else { int i, j; for (i = 0; i < 4; i++) for (j = 0; j < 4; j++) c.gain[i][j] = 1.0f; }
	t_eqm = ms_since(&tq); clock_gettime(CLOCK_MONOTONIC, &tq);
	c.src = in; c.dst = T; run_strips(g_eq == 3 ? (g_gridused > 0 && !g_calused ? cand_apply : go_apply) : g_eq == 2 ? lk_apply : eq_apply, &c);
	if (g_eq == 2) {
		float amin = 9, amax = -9, lmax = 0; int i, j;
		for (i = 0; i < 4; i++) for (j = 0; j < 4; j++) {
			float *k = g_lk[i][j];
			if (k[0] < amin) amin = k[0];
			if (k[0] > amax) amax = k[0];
			if (fabsf(k[1]) > lmax) lmax = fabsf(k[1]);
			if (fabsf(k[2]) > lmax) lmax = fabsf(k[2]);
		}
		fit_amin = amin; fit_amax = amax; fit_lmax = lmax;
	}
	t_eqa = ms_since(&tq);
	t16msg[0] = 0;
	if (g_t16) { t16_fit(T, stride, t16msg, sizeof t16msg); c.dst = T; run_strips(t16_apply, &c); }
	if (g_diag && g_eq) diag_pattern_s(T, stride, 0, "corrected raw", IFX);
	if (g_diag) diag16(T, stride, IFX, 0, "corrected raw", d16raw, sizeof d16raw);
	clock_gettime(CLOCK_MONOTONIC, &tq);
	/* 2. noise model on the equalised frame */
	estimate_noise(T, stride, IFX, &c.na, &c.nb);
	t_nz = ms_since(&tq); }
	float sigma = sqrtf(c.na * 300.0f + c.nb);
	c.mix = (g_hi - sigma) / (g_hi - g_lo); c.mix = c.mix < 0 ? 0 : c.mix > 1 ? 1 : c.mix;
	t_eq = ms_since(&ts); clock_gettime(CLOCK_MONOTONIC, &ts);
	/* 3. denoise: T -> A */
	/* denoise radius by light: the noise estimate is already known here */
	{ int r = g_dnr_set >= 0 ? g_dnr_set : (sigma < 15 ? 2 : sigma < 30 ? 3 : 4);
	  if (r != g_dnr_tab) { g_dnr = r; dn_tables(); g_dnr_tab = r; } }
	if (g_dn > 0) { c.src = T; c.dst = A; run_strips(dn_run, &c); }
	else memcpy(A, T, in_size);
	t_dn = ms_since(&ts); c_dn = g_cpu_ms; clock_gettime(CLOCK_MONOTONIC, &ts);
	/* 4. green: A -> out (int16) */
	c.d = A; c.dst = out; run_strips(green_run, &c);
	t_g = ms_since(&ts); c_g = g_cpu_ms; clock_gettime(CLOCK_MONOTONIC, &ts);
	/* 5. refine: (A, out) -> T (int16) */
	c.g = (const int16_t *)out; c.dst = T; run_strips(refine_run, &c);
	t_ref = ms_since(&ts); c_ref = g_cpu_ms; clock_gettime(CLOCK_MONOTONIC, &ts);
	/* 6. RI + output: (A, T) -> out */
	c.g = (const int16_t *)T; c.dst = out; run_rows(final_run, &c, 128);
	if (g_sharpen > 0) { c.g = (const int16_t *)T; c.dst = out; run_strips(sharpen_run, &c); }
	t_ri = ms_since(&ts); c_ri = g_cpu_ms;

	if (g_diag) { diag_pattern(out, stride, 1, "our output"); diag16(out, stride, 1.0f, 1, "our output", d16out, sizeof d16out); }
	if (g_eq == 3) { LOGI("eq: %s", gomsg); LOGI("eq: %s", g_candmsg); }
	if (g_pd) LOGI("%s (%ld ms)", pdmsg, t_pd);
	if (g_t16) LOGI("%s", t16msg);
	if (g_diag) LOGI("diag lattice16 (rms of 256 positions): %s | %s", d16raw, d16out);
	LOGI("noise: a=%.4f b=%.2f sigma@300=%.1f refine=%.2f", c.na, c.nb, sigma, c.mix);
	LOGI("eq: mode %d, crosstalk fit on %ld flat tiles, gain %.3f..%.3f, max leak %.3f", g_eq, nflat, fit_amin, fit_amax, fit_lmax);
	LOGI("process: %dx%d stride=%d v4 done in %ld ms (eq+noise %ld, denoise %ld, green %ld, refine %ld, ri %ld)",
	     g_w, g_h, stride, ms_since(&t0), t_eq, t_dn, t_g, t_ref, t_ri);
	LOGI("eq+noise detail: alloc %ld, eq measure %ld, eq apply %ld, noise estimate %ld ms", t_alloc, t_eqm, t_eqa, t_nz);
	LOGI("cpu/wall per stage (threads=%d): denoise %ld/%ld, green %ld/%ld, refine %ld/%ld, ri %ld/%ld ms",
	     g_threads, c_dn, t_dn, c_g, t_g, c_ref, t_ref, c_ri, t_ri);
	frame_free(T, in_size); free(Dbuf);
	cal_save();
	return 0;
}

EXPORT int remosaic_deinit(void) { return 0; }
