#!/usr/bin/env python3
"""unsparse.py: convert an Android sparse image (e.g. the big super.img in a firmware package) to a raw image.

  python unsparse.py super.img super_raw.img [--force]

Pure standard library, streaming (low memory). Handles RAW, FILL (with the real fill value), DONT_CARE and CRC32 chunks,
checks the result size against the header, writes to a temporary file first and only then renames it.
If the input is already a raw image it says so and does nothing.
"""
import argparse, os, shutil, struct, sys

SPARSE_MAGIC = 0xED26FF3A
HDR = struct.Struct("<IHHHHIIII")      # magic, major, minor, file_hdr_sz, chunk_hdr_sz, blk_sz, total_blks, total_chunks, image_checksum
CHK = struct.Struct("<HHII")           # chunk_type, reserved, chunk_sz (blocks), total_sz (bytes, incl. header)
RAW, FILL, DONT_CARE, CRC32 = 0xCAC1, 0xCAC2, 0xCAC3, 0xCAC4
BUF = 8 * 1024 * 1024


class SparseError(Exception):
    pass


def is_sparse(path):
    with open(path, "rb") as f:
        b = f.read(4)
    return len(b) == 4 and struct.unpack("<I", b)[0] == SPARSE_MAGIC


def raw_size(path):
    with open(path, "rb") as f:
        h = HDR.unpack(f.read(HDR.size))
    return h[5] * h[6]


def unsparse(src, dst, force=False, quiet=False):
    if os.path.exists(dst) and not force:
        raise SparseError(f"{dst} already exists (use --force to overwrite)")
    tmp = dst + ".partial"
    try:
        with open(src, "rb") as f:
            raw = f.read(HDR.size)
            if len(raw) < HDR.size or HDR.unpack(raw)[0] != SPARSE_MAGIC:
                raise SparseError(f"{src} is not an Android sparse image")
            magic, major, minor, fhs, chs, blk, total_blks, total_chunks, _ = HDR.unpack(raw)
            if major != 1 or chs < CHK.size or fhs < HDR.size or blk == 0 or blk % 4:
                raise SparseError("unsupported or damaged sparse header")
            total = blk * total_blks
            if shutil.disk_usage(os.path.dirname(os.path.abspath(dst))).free < total * 1.02:
                raise SparseError(f"not enough free disk space: the raw image needs {total / 2**30:.1f} GiB")
            if not quiet:
                print(f"sparse image: {total_chunks} chunks, raw size {total / 2**30:.2f} GiB", file=sys.stderr)
            f.seek(fhs)
            pos, last_pct = 0, -1
            with open(tmp, "wb") as out:
                for _ in range(total_chunks):
                    ch = f.read(CHK.size)
                    if len(ch) < CHK.size:
                        raise SparseError("sparse image is truncated")
                    ctype, _r, csz, tsz = CHK.unpack(ch)
                    if chs > CHK.size:
                        f.seek(chs - CHK.size, 1)
                    nbytes, data_len = csz * blk, tsz - chs
                    if ctype == RAW:
                        if data_len != nbytes:
                            raise SparseError("damaged RAW chunk")
                        left = nbytes
                        out.seek(pos)
                        while left:
                            b = f.read(min(BUF, left))
                            if not b:
                                raise SparseError("sparse image is truncated")
                            out.write(b)
                            left -= len(b)
                    elif ctype == FILL:
                        if data_len != 4:
                            raise SparseError("damaged FILL chunk")
                        val = f.read(4)
                        out.seek(pos)
                        if val != b"\0\0\0\0":
                            pat = val * (BUF // 4)
                            left = nbytes
                            while left:
                                out.write(pat[:min(BUF, left)])
                                left -= min(BUF, left)
                    elif ctype == DONT_CARE:
                        if data_len != 0:
                            raise SparseError("damaged DONT_CARE chunk")
                    elif ctype == CRC32:
                        f.read(data_len)
                        nbytes = 0
                    else:
                        raise SparseError(f"unknown chunk type 0x{ctype:X}")
                    pos += nbytes
                    pct = 100 * pos // max(total, 1) // 10 * 10
                    if not quiet and pct != last_pct:
                        last_pct = pct
                        print(f"  unsparse: {pct}%", file=sys.stderr, flush=True)
                if pos != total:
                    raise SparseError(f"size mismatch: wrote {pos} bytes, header says {total}")
                out.truncate(total)            # gives trailing DONT_CARE/zero chunks their zeros
        os.replace(tmp, dst)
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)
    return total


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src"); ap.add_argument("dst"); ap.add_argument("--force", action="store_true")
    a = ap.parse_args(argv)
    try:
        if not is_sparse(a.src):
            print(f"{a.src} is already a raw image (not sparse); nothing to do. Use it directly.")
            return 0
        n = unsparse(a.src, a.dst, a.force)
        print(f"OK: {a.dst} ({n} bytes)")
    except SparseError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
