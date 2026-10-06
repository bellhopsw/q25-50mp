#!/usr/bin/env python3
"""Randomised round-trip test for qpatch with tiny chunk/run limits, to exercise chunk and run boundaries."""
import hashlib, os, random, sys, tempfile
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "tools"))
import qpatch

qpatch.CHUNK = 16384          # multiple of BLOCK, so many chunk boundaries
qpatch.MAX_RUN = 20000        # forces long runs to be split
rnd = random.Random(1234)
sha = lambda b: hashlib.sha256(b).hexdigest()
N, bad = 400, 0
with tempfile.TemporaryDirectory() as T:
    o, n, p, out = (os.path.join(T, x) for x in ("o", "n", "p", "out"))
    for it in range(N):
        size_o = rnd.choice([0, 1, 4095, 4096, 4097, 16384, 16385, 40000, rnd.randrange(0, 200000)])
        old = bytearray(rnd.randbytes(size_o))
        new = bytearray(old)
        mode = rnd.choice(["none", "bytes", "runs", "all", "zeros", "gaps"])
        if new:
            if mode == "bytes":
                for _ in range(rnd.randrange(1, 6)): new[rnd.randrange(len(new))] ^= 0xFF
            elif mode == "runs":
                for _ in range(rnd.randrange(1, 5)):
                    a = rnd.randrange(len(new)); b = min(len(new), a + rnd.randrange(1, 60000)); new[a:b] = rnd.randbytes(b - a)
            elif mode == "all": new = bytearray(rnd.randbytes(len(new)))
            elif mode == "zeros": new[len(new) // 3: 2 * len(new) // 3] = bytes(len(new) // 3)
            elif mode == "gaps":                       # changed blocks separated by gaps around MERGE_GAP
                pos = 0
                while pos < len(new):
                    new[pos:pos + 10] = rnd.randbytes(len(new[pos:pos + 10])); pos += qpatch.BLOCK * rnd.choice([1, 2, 8, 9, 10, 11])
        delta = rnd.choice([0, 0, 1, 4095, 4096, 70000, -1, -4096, -min(len(new), 50000)])
        if delta > 0: new += rnd.choice([bytes(delta), rnd.randbytes(delta)])
        elif delta < 0: new = new[:max(0, len(new) + delta)]
        open(o, "wb").write(old); open(n, "wb").write(new)
        try:
            qpatch.make_patch(o, n, p, quiet=True)
            if os.path.exists(out): os.remove(out)
            qpatch.apply_patch(o, p, out, quiet=True)
            ok = sha(open(out, "rb").read()) == sha(new)
        except Exception as e:
            ok = False; print("exception:", repr(e))
        if not ok:
            bad += 1; print(f"FAIL iteration {it}: size_old={size_o} size_new={len(new)} mode={mode} delta={delta}")
print(f"{N - bad}/{N} random round-trips identical")
sys.exit(1 if bad else 0)
