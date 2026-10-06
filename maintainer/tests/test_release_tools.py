#!/usr/bin/env python3
"""End-to-end tests with synthetic images (no phone or real firmware needed). Python 3.9+, Windows or Linux.
Run:  python tests/test_release_tools.py   (Windows)   or   python3 tests/test_release_tools.py   (Linux)"""
import hashlib, json, os, random, shutil, stat, subprocess, sys, tempfile, time

MAINT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))      # the maintainer folder
TOOLS = os.path.join(os.path.dirname(MAINT), "tools")                           # the user scripts
PY = sys.executable
sys.path.insert(0, TOOLS)
import qpatch

fails = []
def check(name, cond, extra=""):
    print(("PASS  " if cond else "FAIL  ") + name + (f"   {extra}" if extra and not cond else ""))
    if not cond: fails.append(name)

def sha(p): return hashlib.sha256(open(p, "rb").read()).hexdigest()
def run(args, env=None, cwd=None):
    return subprocess.run([PY] + args, capture_output=True, text=True, env=env, cwd=cwd)

def make_images(d, mib_v=24, mib_d=16):
    rnd = random.Random(7)
    sv = bytearray(rnd.randbytes(mib_v * 2**20))
    pv = bytearray(sv)
    pv[5 * 2**20: 5 * 2**20 + 300 * 1024] = rnd.randbytes(300 * 1024)      # a replaced "file"
    pv[11 * 2**20 + 100: 11 * 2**20 + 4196] = rnd.randbytes(4096)           # a block not aligned to 4 KiB
    pv[20 * 2**20 + 7] ^= 0xFF                                               # a single byte
    pv += rnd.randbytes(2**20)                                               # patched image is LARGER
    sd = bytearray(rnd.randbytes(mib_d * 2**20))
    pd = bytearray(sd[:-2**20])                                              # patched image is SMALLER
    pd[3 * 2**20: 3 * 2**20 + 50000] = rnd.randbytes(50000)
    for name, data in (("vendor_stock.img", sv), ("vendor_patched.img", pv), ("dlkm_stock.img", sd), ("dlkm_patched.img", pd)):
        open(os.path.join(d, name), "wb").write(data)

with tempfile.TemporaryDirectory() as T:
    imgs, rel, user = (os.path.join(T, x) for x in ("imgs", "release", "user"))
    os.makedirs(imgs); make_images(imgs)
    P = lambda n: os.path.join(imgs, n)
    # ---- 1. maintainer builds a release
    r = run([os.path.join(MAINT, "build_release.py"), "--out", rel, "--version", "0.0-test", "--fw-id", "TEST.FW.1", "--fw-package", "TEST-PKG",
             "--vendor-stock", P("vendor_stock.img"), "--vendor-patched", P("vendor_patched.img"),
             "--dlkm-stock", P("dlkm_stock.img"), "--dlkm-patched", P("dlkm_patched.img")])
    check("build_release exits 0", r.returncode == 0, r.stderr[-300:] + r.stdout[-300:])
    check("release has manifest, SHA256SUMS, 2 patches", all(os.path.exists(os.path.join(rel, f)) for f in ("manifest.json", "SHA256SUMS", "vendor.qpatch", "vendor_dlkm.qpatch")))
    check("release contains no .img files", not [f for f in os.listdir(rel) if f.endswith(".img")])
    check("no temp files left in release folder", not [f for f in os.listdir(rel) if "partial" in f or f.startswith("tmp")])
    sz = {f: os.path.getsize(os.path.join(rel, f)) for f in ("vendor.qpatch", "vendor_dlkm.qpatch")}
    check("patches are small compared to the images", sz["vendor.qpatch"] < 2.5 * 2**20 and sz["vendor_dlkm.qpatch"] < 1 * 2**20, str(sz))
    man = json.load(open(os.path.join(rel, "manifest.json")))
    check("manifest records stock + result hashes", man["parts"][0]["stock_sha256"] == sha(P("vendor_stock.img")) and man["parts"][1]["result_sha256"] == sha(P("dlkm_patched.img")))
    # ---- 2. a user "downloads" the release and applies it to their own stock images
    shutil.copytree(rel, user)
    for f in ("qpatch.py", "apply_release.py"): shutil.copy(os.path.join(TOOLS, f), user)
    before = {n: sha(P(n)) for n in ("vendor_stock.img", "dlkm_stock.img")}
    out = os.path.join(T, "patched")
    args = [os.path.join(user, "apply_release.py"), "--vendor", P("vendor_stock.img"), "--vendor-dlkm", P("dlkm_stock.img"), "--out", out]
    emptybin = os.path.join(T, "emptybin"); os.makedirs(emptybin)
    env_noadb = dict(os.environ, PATH=emptybin)
    r = run(args, env=env_noadb)
    check("apply_release exits 0 on matching stock images", r.returncode == 0, r.stdout[-400:] + r.stderr[-400:])
    check("patched vendor is byte-identical to the maintainer's", os.path.exists(f"{out}/vendor_patched.img") and sha(f"{out}/vendor_patched.img") == sha(P("vendor_patched.img")))
    check("patched vendor_dlkm (smaller image) is byte-identical", os.path.exists(f"{out}/vendor_dlkm_patched.img") and sha(f"{out}/vendor_dlkm_patched.img") == sha(P("dlkm_patched.img")))
    check("the user's stock images were not modified", before == {n: sha(P(n)) for n in before})
    check("output says nothing was flashed", "NOT flashed" in r.stdout)
    # ---- 3. refuses to overwrite
    r = run(args, env=env_noadb)
    check("second run refuses to overwrite existing output", r.returncode != 0 and "already exists" in (r.stdout + r.stderr))
    r = run(args + ["--force"], env=env_noadb)
    check("--force overwrites", r.returncode == 0)
    # ---- 4. wrong stock image (one flipped byte)
    bad = os.path.join(T, "bad_vendor.img"); shutil.copy(P("vendor_stock.img"), bad)
    with open(bad, "r+b") as f: f.seek(1234567); b = f.read(1); f.seek(1234567); f.write(bytes([b[0] ^ 1]))
    out2 = os.path.join(T, "patched2")
    r = run([os.path.join(user, "apply_release.py"), "--vendor", bad, "--vendor-dlkm", P("dlkm_stock.img"), "--out", out2], env=env_noadb)
    check("wrong stock image is refused", r.returncode != 0 and "not the expected stock" in (r.stdout + r.stderr))
    check("...and nothing was written (not even the other partition)", not os.path.exists(out2) or not os.listdir(out2))
    # ---- 5. tampered / damaged patch
    shutil.copy(os.path.join(user, "vendor.qpatch"), os.path.join(T, "vendor.qpatch.orig"))
    with open(os.path.join(user, "vendor.qpatch"), "r+b") as f: f.seek(3000); b = f.read(1); f.seek(3000); f.write(bytes([b[0] ^ 0x55]))
    out3 = os.path.join(T, "patched3")
    r = run([os.path.join(user, "apply_release.py"), "--vendor", P("vendor_stock.img"), "--vendor-dlkm", P("dlkm_stock.img"), "--out", out3], env=env_noadb)
    check("tampered patch is refused by the manifest hash", r.returncode != 0 and "does not match the manifest" in (r.stdout + r.stderr))
    check("...and nothing was written", not os.path.exists(out3) or not os.listdir(out3))
    shutil.copy(os.path.join(T, "vendor.qpatch.orig"), os.path.join(user, "vendor.qpatch"))
    # damaged patch caught by qpatch itself even if the manifest were bypassed
    dam = os.path.join(T, "dam.qpatch"); shutil.copy(os.path.join(user, "vendor.qpatch"), dam)
    with open(dam, "r+b") as f: f.seek(os.path.getsize(dam) // 2); b = f.read(1); f.seek(os.path.getsize(dam) // 2); f.write(bytes([b[0] ^ 0x55]))
    try: qpatch.apply_patch(P("vendor_stock.img"), dam, os.path.join(T, "dam.img"), quiet=True); ok = False
    except qpatch.PatchError: ok = True
    check("damaged patch body raises PatchError", ok)
    check("...and leaves no partial output", not [f for f in os.listdir(T) if f.startswith("dam.img")])
    trunc = os.path.join(T, "trunc.qpatch"); open(trunc, "wb").write(open(os.path.join(user, "vendor.qpatch"), "rb").read()[:-200])
    try: qpatch.apply_patch(P("vendor_stock.img"), trunc, os.path.join(T, "t.img"), quiet=True); ok = False
    except qpatch.PatchError: ok = True
    check("truncated patch raises PatchError", ok)
    # ---- 6. firmware check with a fake adb
    fake = os.path.join(T, "fakebin"); os.makedirs(fake)
    def fake_adb(idstr):
        if os.name == "nt":
            open(os.path.join(fake, "adb.bat"), "w").write(f"@echo off\necho {idstr}\n")
        else:
            p = os.path.join(fake, "adb"); open(p, "w").write(f"#!/bin/sh\necho '{idstr}'\n"); os.chmod(p, 0o755)
    out4 = os.path.join(T, "patched4")
    a4 = [os.path.join(user, "apply_release.py"), "--vendor", P("vendor_stock.img"), "--vendor-dlkm", P("dlkm_stock.img"), "--out", out4]
    fake_adb("OTHER.FW.9"); env = dict(os.environ, PATH=fake + os.pathsep + os.environ["PATH"])
    r = run(a4, env=env)
    check("firmware mismatch on the phone stops the run", r.returncode != 0 and "OTHER.FW.9" in (r.stdout + r.stderr) and not os.path.exists(out4))
    r = run(a4 + ["--ignore-firmware-check"], env=env)
    check("--ignore-firmware-check lets it continue", r.returncode == 0)
    shutil.rmtree(out4)
    fake_adb("TEST.FW.1")
    r = run(a4, env=env)
    check("matching firmware id passes", r.returncode == 0 and "phone firmware id: TEST.FW.1" in r.stdout)
    # ---- 7. identical images give a tiny, valid patch
    same = os.path.join(T, "same.qpatch")
    s = qpatch.make_patch(P("dlkm_stock.img"), P("dlkm_stock.img"), same, quiet=True)
    qpatch.apply_patch(P("dlkm_stock.img"), same, os.path.join(T, "same.img"), quiet=True)
    check("identical images -> tiny patch that reproduces the image", s["patch_size"] < 400 and sha(os.path.join(T, "same.img")) == sha(P("dlkm_stock.img")), str(s["patch_size"]))
    # ---- 8. qpatch CLI sanity
    r = run([os.path.join(TOOLS, "qpatch.py"), "info", os.path.join(user, "vendor.qpatch")])
    check("qpatch info prints the expected hashes", r.returncode == 0 and man["parts"][0]["stock_sha256"] in r.stdout)

# ---- imginfo --zeros (backup check)
with tempfile.TemporaryDirectory() as T2:
    open(os.path.join(T2, "empty.bin"), "wb").write(bytes(5 * 2**20))
    open(os.path.join(T2, "data.bin"), "wb").write(bytes(2**20) + b"\x01" + bytes(2**20))
    r = run([os.path.join(TOOLS, "imginfo.py"), os.path.join(T2, "empty.bin"), os.path.join(T2, "data.bin"), "--zeros"])
    out = r.stdout
    check("imginfo --zeros flags an all-zero file", "ALL ZEROS" in out and r.returncode == 0, out)
    check("imginfo --zeros counts a single non-zero byte", "1 non-zero bytes of 2097153" in out, out)
    check("imginfo --zeros says a file with data contains data", "contains data" in out)

# ---- scan_strings --vs (only the blocks that differ from stock)
with tempfile.TemporaryDirectory() as T3:
    rnd = __import__("random").Random(5); MiB = 2 ** 20
    stock = bytearray(rnd.randbytes(10 * MiB)); s = b"/home/vendor_dev/tool"; stock[3 * MiB:3 * MiB + len(s)] = s
    leak = bytearray(stock); leak[8 * MiB - 4096:8 * MiB + 4096] = rnd.randbytes(8192); m_ = b"/HOME/me/vw"; leak[8 * MiB - 5:8 * MiB - 5 + len(m_)] = m_
    ok_ = bytearray(stock); ok_[5 * MiB:5 * MiB + 4096] = rnd.randbytes(4096)
    for name, data in (("stock", stock), ("leak", leak), ("ok", ok_)):
        open(os.path.join(T3, name), "wb").write(data)
    scan = os.path.join(MAINT, "scan_strings.py")
    r1 = run([scan, os.path.join(T3, "leak"), "--vs", os.path.join(T3, "stock")])
    r2 = run([scan, os.path.join(T3, "ok"), "--vs", os.path.join(T3, "stock")])
    r3 = run([scan, os.path.join(T3, "ok")])
    check("scan --vs finds a leak in changed data (across a chunk edge)", r1.returncode == 1 and "/HOME/me/vw" in r1.stdout and "vendor_dev" not in r1.stdout, r1.stdout)
    check("scan --vs ignores text that is already in the stock image", r2.returncode == 0 and "CLEAN" in r2.stdout, r2.stdout)
    check("scan without --vs reports the stock text too", r3.returncode == 1 and "vendor_dev" in r3.stdout, r3.stdout)

print("\n%d check(s) failed" % len(fails) if fails else "\nALL TESTS PASSED")
sys.exit(1 if fails else 0)
