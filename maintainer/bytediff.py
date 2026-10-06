#!/usr/bin/env python3
"""bytediff.py: list exactly which bytes differ between two files of the same size (e.g. a stock and a patched .so library).

  python bytediff.py stock/libmtkcam_metastore.so patched/libmtkcam_metastore.so [--gap 4] [--max 200] [--bytes 32]

Prints one line per changed region: file offset (hex), length, old bytes -> new bytes. Use it to document byte patches
(for example in src/hal/PATCHES.md) instead of publishing the modified vendor libraries themselves.
"""
import argparse, os, sys


def regions(a, b, gap):
    out, start, last = [], None, None
    for i in range(0, len(a), 1 << 20):
        ca, cb = a[i:i + (1 << 20)], b[i:i + (1 << 20)]
        if ca == cb:
            continue
        for j in range(len(ca)):
            if ca[j] != cb[j]:
                pos = i + j
                if start is None:
                    start = last = pos
                elif pos - last <= gap:
                    last = pos
                else:
                    out.append((start, last)); start = last = pos
    if start is not None:
        out.append((start, last))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("old"); ap.add_argument("new")
    ap.add_argument("--gap", type=int, default=4, help="merge differences closer than this many bytes")
    ap.add_argument("--max", type=int, default=200, help="print at most this many regions")
    ap.add_argument("--bytes", type=int, default=32, help="show at most this many old/new bytes per region (default 32)")
    x = ap.parse_args()
    a, b = open(x.old, "rb").read(), open(x.new, "rb").read()
    if len(a) != len(b):
        print(f"NOTE: sizes differ ({len(a)} vs {len(b)} bytes); comparing the first {min(len(a), len(b))} bytes only.")
        a, b = a[:min(len(a), len(b))], b[:min(len(a), len(b))]
    reg = regions(a, b, x.gap)
    print(f"{len(reg)} changed region(s), {sum(e - s + 1 for s, e in reg)} byte(s) in total, file size {len(a)}")
    for s, e in reg[:x.max]:
        n = e - s + 1
        o, w = a[s:e + 1][:x.bytes], b[s:e + 1][:x.bytes]
        print(f"  0x{s:08x}  len {n:5d}   {o.hex(' ')}{' ...' if n > x.bytes else ''}  ->  {w.hex(' ')}{' ...' if n > x.bytes else ''}")
    if len(reg) > x.max:
        print(f"  ... {len(reg) - x.max} more region(s) not shown (use --max)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
