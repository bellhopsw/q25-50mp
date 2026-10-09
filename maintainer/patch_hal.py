#!/usr/bin/env python3
"""Apply the Q25 50 MP HAL byte edits to COPIES of three libraries.
Usage: python3 patch_hal.py <dir with the 3 libs> <output dir>
Refuses to write anything unless sizes and old bytes match exactly."""
import sys, os, hashlib

H = bytes.fromhex
CAVE = H("f50300aa" "7f1200f1" "e184e554" "f00e40f9" "b084e5b4"
         "119641b9" "3f064071" "4c84e554" "f4031f2a" "4231ff17")
assert len(CAVE) == 40

# name: (expected size, [(offset, old, new), ...])
EDITS = {
 "libmtkcam_metastore.so": (655096, [
   (0x44741, H("4082"), H("fc83")),
   (0x44755, H("b081"), H("0083")),
 ]),
 "libmtkcam_3rdparty.mtk.so": (416632, [
   (0x3f048, H("e8140034"), H("1f2003d5")),
   (0x4155d, H("1980"), H("9081")),
   (0x41695, H("1980"), H("9081")),
   (0x416d9, H("1980"), H("9081")),
 ]),
 "libmtkcam_featurepolicy.so": (441352, [
   (0x33c20, H("f50300aa"), H("d8d30014")),
   (0x68b80, bytes(40), CAVE),
   (0xd0, H("807b"), H("007c")),
   (0xd8, H("807b"), H("007c")),
 ]),
}

def main(src, dst):
    data = {}
    for name, (size, edits) in EDITS.items():
        p = os.path.join(src, name)
        b = bytearray(open(p, "rb").read())
        print(f"{name}: {len(b)} bytes, sha256 {hashlib.sha256(b).hexdigest()}")
        if len(b) != size:
            sys.exit(f"STOP: {name} should be {size} bytes. Not the 0120 library; nothing written.")
        for off, old, new in edits:
            if bytes(b[off:off+len(old)]) != old:
                sys.exit(f"STOP: {name} @0x{off:x} is {bytes(b[off:off+len(old)]).hex()}, "
                         f"expected {old.hex()}. Nothing written.")
            b[off:off+len(new)] = new
        data[name] = b
    os.makedirs(dst, exist_ok=True)
    for name, b in data.items():
        open(os.path.join(dst, name), "wb").write(b)
        print(f"wrote {name}, sha256 {hashlib.sha256(b).hexdigest()}")

if __name__ == "__main__":
    if len(sys.argv) != 3: sys.exit(__doc__)
    if os.path.realpath(sys.argv[1]) == os.path.realpath(sys.argv[2]):
        sys.exit("STOP: output directory must differ from the input directory.")
    main(sys.argv[1], sys.argv[2])
