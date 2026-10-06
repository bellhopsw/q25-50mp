# Remosaic shim (C)

`libremosaic_wrapper.so` replaces the vendor remosaic step. It receives the sensor's quad-Bayer frame (8160 x 6144) and returns an ordinary Bayer frame.

| File | Notes |
|---|---|
| `remosaic_shim_v4_rc1.c` | **the release**. md5 `35f588721cb8e93f28b332df9fe9f145`, 137294 bytes |
| `remosaic_shim_v4_rc3.c` | experimental: adds the `cycle` preset test and the `sharpr` sharpen radius. With default settings its output is identical to rc1 (checked bit for bit on a synthetic frame). md5 `b87be99de07cac920079c4592c4ad7a8` |

## Build (Linux or WSL, Android NDK r26d)
```
CC=~/android-ndk-r26d/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android31-clang
$CC -O3 -mcpu=cortex-a76 -shared -fPIC -Wl,-z,nodelete -o libremosaic_wrapper.so remosaic_shim_v4_rc1.c -llog -lm
```
The library inside the released `vendor` image is 149608 bytes with md5 `0d70942a2e0e8e345ae96f4545c1c3a8` (built from rc1; a different toolchain may give a different binary). It goes into `/vendor/lib64/` of the vendor image. Building an image means loop-mounting the ext4 image, which needs Linux; users of a release never do this.

## Pipeline
PDAF repair (fixed JN1 layout, edge-aware) -> 4 x 4 quad equalisation -> learned 16 x 16 gain map -> noise model -> bilateral denoise (radius 2, 3 or 4 by noise level) -> direction-aware green interpolation (QPPG) -> noise-gated refinement -> residual interpolation for red and blue -> optional mild sharpening (off).

## Settings
Read from the vendor `build.prop` as `persist.vendor.remoshim.<name>`; changing one means rebuilding and reflashing the `vendor` image.

| name | default | meaning |
|---|---|---|
| `dn` | 20 | denoise strength x 10 (20 = 2.0; 0 = off) |
| `dnr` | -1 | denoise radius; -1 = automatic from the noise level |
| `sharpen` | 0 | luminance sharpening x 100 (off) |
| `sharpr` | 1 | sharpen radius, 1 to 4 (rc3 only) |
| `gatelo`, `gatehi` | 15, 35 | refinement noise gates |
| `eq`, `tile16`, `acc`, `pd`, `pdthr`, `pdfixed`, `grid` | 3, 1, 1, 1, 150, 1, -1 | equalisation, 16 x 16 map, accumulation and PDAF repair options |
| `eqmin` | 12 | skip equalisation refit on very dark frames |
| `pdcolor` | 0 | per-frame correction of the PDAF cell-mates (off: mixed results) |
| `pdcflat` | 1 | edge/clip gate for `pdcolor` |
| `diag` | 0 | extra diagnostics in logcat |
| `cycle` | 0 | rc3 only: rotate through four sharpening presets, one per photo (a test build) |
| `threads` | 8 | worker threads |

The log tag is `RemosaicShim`.
