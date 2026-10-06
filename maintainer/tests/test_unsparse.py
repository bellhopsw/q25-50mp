#!/usr/bin/env python3
"""unsparse.py vs the original raw image and (when installed) Google's simg2img. Needs img2simg for generating sparse test files."""
import hashlib, os, random, shutil, subprocess, sys, tempfile
TOOLS = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "tools")
sys.path.insert(0, TOOLS)
import unsparse

if not shutil.which("img2simg"):
    print("SKIP: img2simg not installed (sudo apt install android-sdk-libsparse-utils)"); sys.exit(0)
sha = lambda p: hashlib.sha256(open(p, "rb").read()).hexdigest()
rnd = random.Random(42)
B = 4096
def build(kind):
    out = bytearray()
    if kind == "random+zeros":
        for _ in range(300): out += rnd.randbytes(B) if rnd.random() < 0.4 else bytes(B) * rnd.randrange(1, 40)
    elif kind == "nonzero fill":
        for _ in range(200):
            pat = rnd.randbytes(4) if rnd.random() < 0.7 else b"\0\0\0\0"
            out += pat * (B // 4) * rnd.randrange(1, 60); out += rnd.randbytes(B)
    elif kind == "ends in zeros":
        out += rnd.randbytes(B * 50) + bytes(B * 500)
    elif kind == "all zeros":
        out += bytes(B * 2000)
    elif kind == "single block":
        out += rnd.randbytes(B)
    elif kind == "ext4-like":
        out += bytes(1024) + rnd.randbytes(B * 3 - 1024)
        for _ in range(100): out += bytes(B * rnd.randrange(1, 100)) + rnd.randbytes(B * rnd.randrange(1, 8))
    return bytes(out)
fails = 0
with tempfile.TemporaryDirectory() as T:
    raw, sp, mine, ref = (os.path.join(T, x) for x in ("raw", "sp", "mine", "ref"))
    for kind in ("random+zeros", "nonzero fill", "ends in zeros", "all zeros", "single block", "ext4-like"):
        for flag in ([], ["-s"]):
            data = build(kind); open(raw, "wb").write(data)
            for p in (sp, mine, ref):
                if os.path.exists(p): os.remove(p)
            subprocess.run(["img2simg"] + flag + [raw, sp], check=True, capture_output=True)
            ok = True
            try: unsparse.unsparse(sp, mine, quiet=True)
            except Exception as e: ok = False; print("   exception", e)
            same_raw = ok and sha(mine) == sha(raw)
            same_ref = True
            if shutil.which("simg2img"):
                subprocess.run(["simg2img", sp, ref], check=True, capture_output=True); same_ref = ok and sha(mine) == sha(ref)
            good = same_raw and same_ref
            print(("PASS  " if good else "FAIL  ") + f"{kind:14s} img2simg {' '.join(flag) or '(default)':9s} -> identical to original: {same_raw}, to simg2img: {same_ref}   [{len(data)//1024} KiB]")
            fails += (not good)
    # error handling
    open(sp, "wb").write(b"not sparse at all")
    try: unsparse.unsparse(sp, mine, quiet=True); ok = False
    except unsparse.SparseError: ok = True
    print(("PASS  " if ok else "FAIL  ") + "non-sparse input is refused"); fails += (not ok)
    data = build("random+zeros"); open(raw, "wb").write(data); subprocess.run(["img2simg", raw, sp], check=True, capture_output=True)
    open(sp, "wb").write(open(sp, "rb").read()[:-5000])
    try: unsparse.unsparse(sp, ref + "2", quiet=True); ok = False
    except unsparse.SparseError: ok = True
    print(("PASS  " if ok else "FAIL  ") + "truncated sparse file is refused, no partial output left" + ("" if not os.path.exists(ref + "2.partial") else "  (partial left!)")); fails += (not ok)
print("\nALL PASSED" if not fails else f"\n{fails} FAILED"); sys.exit(1 if fails else 0)
