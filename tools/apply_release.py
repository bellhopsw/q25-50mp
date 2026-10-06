#!/usr/bin/env python3
"""apply_release.py (user): build the patched images from YOUR OWN stock images. It does not flash anything.

Example:
  python apply_release.py --vendor vendor_a.img --vendor-dlkm vendor_dlkm_a.img --out patched

Needs: Python 3.8+, the release files (manifest.json and the .qpatch files) in the same folder as this script,
and the stock vendor and vendor_dlkm images from the SAME firmware the release was built for.
Your stock images are only read, never changed. If anything does not match, nothing is written.
"""
import argparse, json, os, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qpatch


def phone_build_id():
    adb = shutil.which("adb")
    if not adb:
        return None
    try:
        r = subprocess.run([adb, "shell", "getprop", "ro.build.display.id"], capture_output=True, text=True, timeout=8)
        return r.stdout.strip() or None
    except Exception:
        return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--manifest", default=os.path.join(HERE, "manifest.json"))
    ap.add_argument("--vendor", required=True, help="your stock vendor image")
    ap.add_argument("--vendor-dlkm", required=True, help="your stock vendor_dlkm image")
    ap.add_argument("--out", default="patched")
    ap.add_argument("--force", action="store_true", help="overwrite existing output files")
    ap.add_argument("--ignore-firmware-check", action="store_true", help="continue even if the phone's firmware id differs")
    a = ap.parse_args()
    try:
        man = json.load(open(a.manifest))
    except Exception as e:
        sys.exit(f"ERROR: cannot read {a.manifest}: {e}")
    base = os.path.dirname(os.path.abspath(a.manifest))
    print(f"{man['name']} release {man['version']}")
    want = man.get("firmware", {}).get("build_id", "")
    print(f"built for firmware: {want or '(not recorded)'}  {man.get('firmware', {}).get('package', '')}")
    have = phone_build_id()
    if have is None:
        print("note: could not read the firmware id from a connected phone (adb not found or no phone). Make sure your phone runs the firmware above.")
    elif want and have != want:
        print(f"WARNING: the connected phone reports '{have}', this release is for '{want}'.")
        if not a.ignore_firmware_check:
            sys.exit("Stopping. Use --ignore-firmware-check only if you are sure the stock images match the release.")
    else:
        print(f"phone firmware id: {have}")
    inputs = {"vendor": a.vendor, "vendor_dlkm": a.vendor_dlkm}
    plan = []
    print("\nChecking your stock images and the patch files ...")
    for p in man["parts"]:
        stock = inputs[p["partition"]]
        pf = os.path.join(base, p["patch_file"])
        if not os.path.isfile(stock):
            sys.exit(f"ERROR: {stock} not found")
        if not os.path.isfile(pf):
            sys.exit(f"ERROR: patch file {pf} not found")
        if qpatch.sha256_file(pf) != p["patch_sha256"]:
            sys.exit(f"ERROR: {p['patch_file']} does not match the manifest (corrupted or tampered download). Download it again.")
        if os.path.getsize(stock) != p["stock_size"] or qpatch.sha256_file(stock, p["partition"]) != p["stock_sha256"]:
            sys.exit(f"ERROR: {stock} is not the expected stock {p['partition']} image for this release.\n"
                     f"  expected sha256 {p['stock_sha256']}\n"
                     "  Use the unmodified image from the same firmware package (see the install guide). Nothing was written.")
        plan.append((p, stock, pf))
    os.makedirs(a.out, exist_ok=True)
    outs = []
    for p, stock, pf in plan:
        out = os.path.join(a.out, f"{p['partition']}_patched.img")
        print(f"\nBuilding {out} ...")
        try:
            qpatch.apply_patch(stock, pf, out, force=a.force)
        except qpatch.PatchError as e:
            sys.exit(f"ERROR: {e}")
        if qpatch.sha256_file(out) != p["result_sha256"]:
            os.remove(out)
            sys.exit("ERROR: result checksum mismatch; output removed.")
        outs.append((p["partition"], out, p["result_sha256"]))
    print("\nAll done. The images were built and checked, but NOT flashed:")
    for part, out, sha in outs:
        print(f"  {part:12s} {out}\n               sha256 {sha}")
    print("\nNext: follow INSTALL.md (back up your phone's calibration partitions first).")


if __name__ == "__main__":
    main()
