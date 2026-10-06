#!/usr/bin/env python3
"""scan_strings.py: look for text (local paths, user names, ...) inside big binary files: disk images, libraries, kernel modules.

  python scan_strings.py FILE... [--needle TEXT]... [--max 8]
  python scan_strings.py PATCHED_IMAGE --vs STOCK_IMAGE [--needle TEXT]...    # only the blocks that differ from stock

The search is case-insensitive and streams the files, so 1 GB images are fine. Default needles: /home/, /Users/,
C:\\Users\\ and /mnt/c/. Add your own with --needle (for example your user name). With --vs, only the 4 KiB blocks that differ from the given stock image are scanned. That is exactly what a release
patch carries, and it ignores text that is already in the stock firmware (build paths of the vendor's developers, for
example). Prints, for every hit, the file offset
and a little printable context. Exit status 1 if anything was found. Run it on every image and library you are about
to publish (ext4 images record the last mount point, e.g. /home/<you>/vw, inside the filesystem itself).
"""
import argparse, os, sys

CHUNK = 8 * 1024 * 1024


def scan(path, needles, maxhits):
    nb = [n.lower().encode() for n in needles]
    keep = max(len(n) for n in nb) - 1
    counts = {n: 0 for n in needles}
    shown, tail, base = 0, b"", 0           # tail: the end of the previous chunk, so a match spanning two chunks is found
    with open(path, "rb") as f:
        while True:
            data = f.read(CHUNK)
            if not data:
                break
            buf = (tail + data)
            low = buf.lower()
            start = base - len(tail)         # file offset of buf[0]
            for n, raw in zip(needles, nb):
                i = low.find(raw)
                while i >= 0:
                    if i + len(raw) > len(tail) or len(tail) == 0:      # skip matches fully inside the old tail (already counted)
                        counts[n] += 1
                        if shown < maxhits:
                            ctx = buf[max(0, i - 20):i + len(raw) + 40]
                            txt = "".join(chr(c) if 32 <= c < 127 else "." for c in ctx)
                            print(f"  {path}: offset {start + i} (0x{start + i:x}), needle {n!r}: ...{txt}...")
                            shown += 1
                    i = low.find(raw, i + 1)
            tail = buf[-keep:] if keep > 0 else b""
            base += len(data)
    return counts


BLOCK = 4096
RUN_MAX = 64 * 1024 * 1024


def scan_changed(path, stock, needles, maxhits):
    """Scan only the blocks of `path` that differ from the same block of `stock` (missing stock bytes count as zero)."""
    nb = [n.lower().encode() for n in needles]
    keep = max(len(n) for n in nb) - 1
    counts = {n: 0 for n in needles}
    state = {"shown": 0, "blocks": 0}

    def scan_run(run, run_start, skip):
        low = bytes(run).lower()
        for n, raw in zip(needles, nb):
            i = low.find(raw)
            while i >= 0:
                if i + len(raw) > skip:                       # not fully inside the overlap that was already scanned
                    counts[n] += 1
                    if state["shown"] < maxhits:
                        ctx = bytes(run[max(0, i - 20):i + len(raw) + 40])
                        txt = "".join(chr(c) if 32 <= c < 127 else "." for c in ctx)
                        print(f"  {path}: changed data at offset {run_start + i} (0x{run_start + i:x}), needle {n!r}: ...{txt}...")
                        state["shown"] += 1
                i = low.find(raw, i + 1)

    run, run_start, skip, pos = bytearray(), None, 0, 0
    with open(path, "rb") as f, open(stock, "rb") as g:
        while True:
            a = f.read(CHUNK)
            if not a:
                break
            b = g.read(len(a))
            b = b + b"\0" * (len(a) - len(b))
            for i in range(0, len(a), BLOCK):
                ab, bb = a[i:i + BLOCK], b[i:i + BLOCK]
                if ab != bb:
                    state["blocks"] += 1
                    if run_start is None:
                        run_start, skip = pos + i, 0
                    run += ab
                    if len(run) >= RUN_MAX:                    # very long run: scan it and carry an overlap into the next part
                        scan_run(run, run_start, skip)
                        tail = bytes(run[-keep:]) if keep > 0 else b""
                        run_start += len(run) - len(tail)
                        run, skip = bytearray(tail), len(tail)
                elif run_start is not None:
                    scan_run(run, run_start, skip)
                    run, run_start, skip = bytearray(), None, 0
            pos += len(a)
        if run_start is not None:
            scan_run(run, run_start, skip)
    return counts, state["blocks"]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+")
    ap.add_argument("--needle", action="append", default=[], help="extra text to look for (repeatable)")
    ap.add_argument("--max", type=int, default=8, help="show at most this many hits per file")
    ap.add_argument("--vs", metavar="STOCK_IMAGE", help="scan only the blocks that differ from this stock image")
    a = ap.parse_args(argv)
    needles = ["/home/", "/Users/", "C:\\Users\\", "/mnt/c/"] + a.needle
    found = 0
    for p in a.files:
        if not os.path.isfile(p):
            print(f"{p}: not found"); continue
        if a.vs:
            c, blocks = scan_changed(p, a.vs, needles, a.max)
            note = f" ({blocks} changed block(s), {blocks * BLOCK // 1024} KiB, compared with {os.path.basename(a.vs)})"
        else:
            c, note = scan(p, needles, a.max), ""
        total = sum(c.values())
        found += total
        print(f"{p}: {'CLEAN' if not total else 'FOUND ' + str(total) + ' hit(s): ' + ', '.join(f'{n!r} x{k}' for n, k in c.items() if k)}{note}")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
