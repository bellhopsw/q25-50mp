#!/usr/bin/env python3
"""qpatch: a small block-level patch tool for raw disk images (Python 3.8+, standard library only).

  qpatch.py make  OLD NEW PATCH     build a patch that turns OLD into NEW
  qpatch.py apply OLD PATCH OUT     rebuild NEW from OLD, refusing unless OLD is exactly the expected file
  qpatch.py info  PATCH             show what a patch expects and produces
  qpatch.py hash  FILE...           print SHA-256 and size

Safety rules: inputs are never modified; the output is written to a temporary file, checked against the
expected SHA-256, and only then renamed into place; an existing output is never overwritten without --force.
"""
import argparse, hashlib, json, lzma, os, shutil, struct, sys

MAGIC = b"Q25PATCH\x01\n"
BLOCK = 4096
MERGE_GAP = 8                  # unchanged blocks tolerated inside one changed run (keeps record count down)
CHUNK = 16 * 1024 * 1024
MAX_RUN = 64 * 1024 * 1024
END = 0xFFFFFFFFFFFFFFFF
REC = struct.Struct("<QI")
FORMAT_VERSION = 1


class PatchError(Exception):
    pass


class Progress:
    def __init__(self, label, total, enabled=True):
        self.label, self.total, self.enabled, self.last = label, max(total, 1), enabled, -1

    def update(self, done):
        if not self.enabled:
            return
        pct = int(100 * done / self.total) // 10 * 10
        if pct != self.last:
            self.last = pct
            print(f"  {self.label}: {min(pct, 100)}%", file=sys.stderr, flush=True)


def sha256_file(path, label=None):
    h, size = hashlib.sha256(), os.path.getsize(path)
    prog, done = Progress(label or "", size, label is not None), 0
    with open(path, "rb") as f:
        while True:
            b = f.read(CHUNK)
            if not b:
                break
            h.update(b)
            done += len(b)
            prog.update(done)
    return h.hexdigest()


def make_patch(old_path, new_path, patch_path, quiet=False):
    old_size, new_size = os.path.getsize(old_path), os.path.getsize(new_path)
    if not quiet:
        print(f"hashing {os.path.basename(old_path)} and {os.path.basename(new_path)} ...", file=sys.stderr)
    header = {"format": FORMAT_VERSION, "block": BLOCK,
              "old_size": old_size, "old_sha256": sha256_file(old_path, None if quiet else "old"),
              "new_size": new_size, "new_sha256": sha256_file(new_path, None if quiet else "new")}
    hb = json.dumps(header, sort_keys=True).encode()
    comp = lzma.LZMACompressor(format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC64, preset=6)
    tmp = patch_path + ".partial"
    stats = {"runs": 0, "changed_bytes": 0}
    try:
        with open(old_path, "rb") as fo, open(new_path, "rb") as fn, open(tmp, "wb") as out:
            out.write(MAGIC)
            out.write(struct.pack("<I", len(hb)))
            out.write(hb)

            def emit(offset, data):
                out.write(comp.compress(REC.pack(offset, len(data)) + bytes(data)))
                stats["runs"] += 1
                stats["changed_bytes"] += len(data)

            run_start, run, pending, pending_blocks = None, bytearray(), bytearray(), 0
            pos, prog = 0, Progress("diff", new_size, not quiet)
            while pos < new_size:
                n = min(CHUNK, new_size - pos)
                b = fn.read(n)
                o = fo.read(n)
                if len(o) < len(b):
                    o = o + b"\0" * (len(b) - len(o))      # beyond the old image's end, "old" is zeros
                if b == o and run_start is None:
                    pos += len(b)
                    prog.update(pos)
                    continue
                mb, mo = memoryview(b), memoryview(o)
                for i in range(0, len(b), BLOCK):
                    bb, ob = mb[i:i + BLOCK], mo[i:i + BLOCK]
                    if bb != ob:
                        if run_start is None:
                            run_start = pos + i
                        if pending:
                            run += pending
                            pending, pending_blocks = bytearray(), 0
                        run += bb
                        if len(run) >= MAX_RUN:
                            emit(run_start, run)
                            run, run_start = bytearray(), None
                    elif run_start is not None:
                        pending += bb
                        pending_blocks += 1
                        if pending_blocks > MERGE_GAP:
                            emit(run_start, run)
                            run, run_start, pending, pending_blocks = bytearray(), None, bytearray(), 0
                pos += len(b)
                prog.update(pos)
            if run_start is not None:
                emit(run_start, run)
            out.write(comp.compress(REC.pack(END, 0)))
            out.write(comp.flush())
        os.replace(tmp, patch_path)
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)
    stats["patch_size"] = os.path.getsize(patch_path)
    stats.update({"old_size": old_size, "new_size": new_size, "old_sha256": header["old_sha256"], "new_sha256": header["new_sha256"]})
    return stats


def read_header(pf):
    if pf.read(len(MAGIC)) != MAGIC:
        raise PatchError("this is not a Q25 patch file (bad header)")
    (hl,) = struct.unpack("<I", pf.read(4))
    if hl > 1 << 20:
        raise PatchError("patch header is corrupt")
    try:
        header = json.loads(pf.read(hl))
    except ValueError:
        raise PatchError("patch header is corrupt")
    if header.get("format") != FORMAT_VERSION:
        raise PatchError(f"unsupported patch format {header.get('format')}")
    return header


def iter_records(pf):
    dec, buf = lzma.LZMADecompressor(), bytearray()

    def fill():
        chunk = pf.read(1 << 20)
        if not chunk:
            if not dec.eof:
                raise PatchError("patch file is truncated or damaged")
            return False
        try:
            buf.extend(dec.decompress(chunk))
        except lzma.LZMAError:
            raise PatchError("patch file is damaged (decompression failed)")
        return True

    while True:
        while len(buf) < REC.size:
            if not fill():
                raise PatchError("patch file ended unexpectedly")
        off, ln = REC.unpack_from(buf, 0)
        if off == END:
            return
        need = REC.size + ln
        while len(buf) < need:
            if not fill():
                raise PatchError("patch file ended unexpectedly")
        yield off, bytes(buf[REC.size:need])
        del buf[:need]


def apply_patch(old_path, patch_path, out_path, force=False, quiet=False):
    if os.path.exists(out_path) and not force:
        raise PatchError(f"{out_path} already exists (use --force to overwrite)")
    with open(patch_path, "rb") as pf:
        header = read_header(pf)
        old_size, new_size = header["old_size"], header["new_size"]
        if os.path.getsize(old_path) != old_size:
            raise PatchError(f"{os.path.basename(old_path)} is {os.path.getsize(old_path)} bytes but the patch expects {old_size}. "
                             "This is not the expected stock image.")
        outdir = os.path.dirname(os.path.abspath(out_path))
        if shutil.disk_usage(outdir).free < new_size * 1.02:
            raise PatchError(f"not enough free space in {outdir} (need about {new_size // 2**20} MiB)")
        tmp = out_path + ".partial"
        try:
            h, done, prog = hashlib.sha256(), 0, Progress("copy", old_size, not quiet)
            with open(old_path, "rb") as fi, open(tmp, "wb") as fo:
                while True:
                    b = fi.read(CHUNK)
                    if not b:
                        break
                    h.update(b)
                    fo.write(b)
                    done += len(b)
                    prog.update(done)
            if h.hexdigest() != header["old_sha256"]:
                raise PatchError(f"{os.path.basename(old_path)} is NOT the expected stock image (SHA-256 mismatch).\n"
                                 f"  expected {header['old_sha256']}\n  found    {h.hexdigest()}\n"
                                 "Nothing was written. Check that you have the right firmware and the right partition image.")
            with open(tmp, "r+b") as fo:
                fo.truncate(new_size)
                for off, data in iter_records(pf):
                    if off + len(data) > new_size:
                        raise PatchError("patch contains data outside the image (damaged patch)")
                    fo.seek(off)
                    fo.write(data)
            got = sha256_file(tmp, None if quiet else "verify")
            if got != header["new_sha256"]:
                raise PatchError("the rebuilt image failed its checksum (patch damaged or wrong tool version). Nothing was installed.")
            os.replace(tmp, out_path)
        finally:
            if os.path.exists(tmp):
                os.remove(tmp)
    return header


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("make"); m.add_argument("old"); m.add_argument("new"); m.add_argument("patch")
    a = sub.add_parser("apply"); a.add_argument("old"); a.add_argument("patch"); a.add_argument("out"); a.add_argument("--force", action="store_true")
    i = sub.add_parser("info"); i.add_argument("patch")
    h = sub.add_parser("hash"); h.add_argument("files", nargs="+")
    args = ap.parse_args(argv)
    try:
        if args.cmd == "make":
            s = make_patch(args.old, args.new, args.patch)
            print(f"patch written: {args.patch} ({s['patch_size'] / 2**20:.2f} MiB, {s['runs']} runs, {s['changed_bytes'] / 2**20:.2f} MiB changed)")
        elif args.cmd == "apply":
            hd = apply_patch(args.old, args.patch, args.out, args.force)
            print(f"OK: {args.out}\n  sha256 {hd['new_sha256']}")
        elif args.cmd == "info":
            with open(args.patch, "rb") as pf:
                hd = read_header(pf)
            print(json.dumps(hd, indent=2))
        else:
            for f in args.files:
                print(f"{sha256_file(f)}  {os.path.getsize(f)}  {f}")
    except PatchError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
