#!/usr/bin/env python3
"""build_release.py (maintainer): turn your known-good images into a release folder.

Example:
  python3 build_release.py --out release --version 0.1.0-alpha \
      --fw-id "<output of: adb shell getprop ro.build.display.id>" --fw-package OS-new-camera-0120-Q25-GMS \
      --vendor-stock vendor_a_STOCK.img --vendor-patched vendor_rc1.img \
      --dlkm-stock vendor_dlkm_a_STOCK.img --dlkm-patched vendor_dlkm_v3.img

It writes <out>/vendor.qpatch, <out>/vendor_dlkm.qpatch, <out>/manifest.json and <out>/SHA256SUMS, and proves each patch by
rebuilding the patched image from the stock image and comparing checksums before anything is published.
No vendor image is copied into the release folder: users rebuild the patched images from THEIR OWN stock images.
"""
import argparse, datetime, json, os, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "tools"))   # qpatch.py lives in ../tools
import qpatch


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("--version", required=True)
    ap.add_argument("--fw-id", default="", help="firmware build id of the stock firmware (adb shell getprop ro.build.display.id)")
    ap.add_argument("--fw-package", default="", help="name of the official firmware package the stock images come from")
    ap.add_argument("--vendor-stock", required=True); ap.add_argument("--vendor-patched", required=True)
    ap.add_argument("--dlkm-stock", required=True); ap.add_argument("--dlkm-patched", required=True)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    parts = []
    for part, stock, patched in (("vendor", a.vendor_stock, a.vendor_patched), ("vendor_dlkm", a.dlkm_stock, a.dlkm_patched)):
        for f in (stock, patched):
            if not os.path.isfile(f):
                sys.exit(f"ERROR: {f} not found")
        if os.path.abspath(stock) == os.path.abspath(patched):
            sys.exit(f"ERROR: the stock and patched image for {part} are the same file")
        pf = os.path.join(a.out, f"{part}.qpatch")
        print(f"\n== {part}: building patch ==")
        s = qpatch.make_patch(stock, patched, pf)
        print(f"== {part}: proving the patch (rebuild from stock, compare checksums) ==")
        with tempfile.TemporaryDirectory(dir=a.out) as td:
            rebuilt = os.path.join(td, "rebuilt.img")
            qpatch.apply_patch(stock, pf, rebuilt)
            if qpatch.sha256_file(rebuilt) != s["new_sha256"]:
                sys.exit(f"ERROR: round-trip check failed for {part}; do not publish")
        parts.append({"partition": part, "stock_file_hint": os.path.basename(stock),
                      "stock_size": s["old_size"], "stock_sha256": s["old_sha256"],
                      "patch_file": os.path.basename(pf), "patch_size": s["patch_size"], "patch_sha256": qpatch.sha256_file(pf),
                      "result_size": s["new_size"], "result_sha256": s["new_sha256"]})
        print(f"{part}: patch {s['patch_size'] / 2**20:.2f} MiB, round-trip OK")
    manifest = {"schema": 1, "name": "Q25 50MP", "version": a.version, "created_utc": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                "firmware": {"build_id": a.fw_id, "package": a.fw_package}, "tool": "qpatch format 1", "parts": parts}
    with open(os.path.join(a.out, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    with open(os.path.join(a.out, "SHA256SUMS"), "w") as f:
        for name in ["manifest.json"] + [p["patch_file"] for p in parts]:
            f.write(f"{qpatch.sha256_file(os.path.join(a.out, name))}  {name}\n")
    print(f"\nDone. Release folder: {a.out}")
    for p in parts:
        print(f"  {p['partition']:12s} stock sha256 {p['stock_sha256'][:16]}...  ->  result sha256 {p['result_sha256'][:16]}...")
    print("Publish: manifest.json, SHA256SUMS and the .qpatch files. Do NOT publish any .img file.")


if __name__ == "__main__":
    main()
