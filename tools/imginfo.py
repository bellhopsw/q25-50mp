#!/usr/bin/env python3
"""imginfo.py: tell what kind of image a file is (Windows has no `file` command), optionally with its SHA-256.

  python imginfo.py vendor_a.img vendor_dlkm_a.img --hash
  python imginfo.py nvram.bin persist.bin --zeros      # check that a backup is not empty (ignore the "type" line for these)
"""
import argparse, hashlib, os, struct, sys


def kind(path):
    with open(path, "rb") as f:
        head = f.read(4096 + 8)
        f.seek(1024)
        sb = f.read(64)
    if len(head) >= 4 and struct.unpack("<I", head[:4])[0] == 0xED26FF3A:
        return "Android SPARSE image (convert it first with unsparse.py)"
    if len(sb) >= 58 and struct.unpack("<H", sb[56:58])[0] == 0xEF53:
        return "raw ext4 filesystem image"
    if len(sb) >= 4 and struct.unpack("<I", sb[:4])[0] == 0xE0F5E1E2:
        return "EROFS filesystem image (read-only)"
    if len(head) >= 4104 and head[4096:4100] == b"gDla":
        return "raw Android 'super' image (dynamic partitions)"
    if head[:8] == b"ANDROID!":
        return "Android boot image"
    return "unknown (not ext4, EROFS, sparse or super)"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+"); ap.add_argument("--hash", action="store_true")
    ap.add_argument("--zeros", action="store_true", help="count the non-zero bytes (to check that a backup contains real data)")
    a = ap.parse_args(argv)
    for p in a.files:
        if not os.path.isfile(p):
            print(f"{p}: not found"); continue
        print(f"{p}\n  size  : {os.path.getsize(p)} bytes ({os.path.getsize(p) / 2**20:.1f} MiB)\n  type  : {kind(p)}")
        if a.zeros:
            nz = total = 0
            with open(p, "rb") as f:
                for b in iter(lambda: f.read(1 << 24), b""):
                    total += len(b)
                    nz += len(b) - b.count(0)
            verdict = "contains data" if nz else "ALL ZEROS (or empty): this backup is NOT usable"
            print(f"  data  : {nz} non-zero bytes of {total} ({100 * nz / max(total, 1):.2f} %): {verdict}")
        if a.hash:
            h = hashlib.sha256()
            with open(p, "rb") as f:
                for b in iter(lambda: f.read(1 << 24), b""):
                    h.update(b)
            print(f"  sha256: {h.hexdigest()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
