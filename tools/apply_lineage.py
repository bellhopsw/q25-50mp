#!/usr/bin/env python3
"""apply_lineage.py (user, Linux/WSL only): build patched vendor and vendor_dlkm images from YOUR OWN stock images,
working on files instead of whole-image patches. It does not flash anything and never changes your stock images.

What it does, per image: copies your stock image, mounts the COPY read-write, checks everything first, and only then writes:
  vendor      - applies the 62 documented byte edits to the three libmtkcam libraries (refuses unless sizes and old
                bytes match exactly) and replaces libremosaic_wrapper.so with the shim.
  vendor_dlkm - replaces imgsensor_isp6s.ko with the patched module, but only if its kernel version string (vermagic)
                and its imported symbol checksums match the module in your stock image.
Files are overwritten in place, so the image keeps its size and SELinux labels.

Needs: Linux or WSL, Python 3.8+, root (sudo), and the e2fsprogs tools (mount, e2fsck, tune2fs, dumpe2fs).
Easiest use (finds the Lineage zip in your Windows Downloads folder, extracts, checks, builds, and prints the commands
to paste into PowerShell, with your paths and your slot handled for you):
  sudo python3 apply_lineage.py
Other uses:
  sudo python3 apply_lineage.py --zip /path/to/lineage-...zip      (zip somewhere else)
  sudo python3 apply_lineage.py --check-only                        (only report whether your build is supported)
  sudo python3 apply_lineage.py --vendor vendor.img --vendor-dlkm vendor_dlkm.img --out patched   (images you extracted yourself)
"""
import argparse, glob, hashlib, os, shutil, struct, subprocess, sys, tempfile, zipfile

H = bytes.fromhex
CAVE = H("f50300aa" "7f1200f1" "e184e554" "f00e40f9" "b084e5b4"
         "119641b9" "3f064071" "4c84e554" "f4031f2a" "4231ff17")
assert len(CAVE) == 40

# library name: (expected size, [(file offset, old bytes, new bytes), ...])   (see src/hal/PATCHES.md)
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
WRAPPER = "libremosaic_wrapper.so"
MODULE = "imgsensor_isp6s.ko"
ASSET_MD5 = {WRAPPER: "0d70942a2e0e8e345ae96f4545c1c3a8", MODULE: "20cf36063834ddb7100fb64bb5e53326"}


CREATED = []   # output images created by this run (removed again if anything fails)


class Refuse(Exception):
    """A check failed. Nothing has been written to the image copy that matters."""


def digest(path, algo):
    h = hashlib.new(algo)
    with open(path, "rb") as f:
        for blk in iter(lambda: f.read(1 << 20), b""):
            h.update(blk)
    return h.hexdigest()


# ------------------------------------------------------------------ kernel module inspection
def elf_sections(data):
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        raise Refuse("not a 64-bit little-endian ELF file")
    shoff, = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    if shoff == 0 or shentsize < 40 or shoff + shnum * shentsize > len(data) or shstrndx >= shnum:
        raise Refuse("ELF section table is missing or damaged")

    def hdr(i):  # name, type, flags, addr, offset, size
        return struct.unpack_from("<IIQQQQ", data, shoff + i * shentsize)

    stroff = hdr(shstrndx)[4]
    out = {}
    for i in range(shnum):
        name_off, _t, _f, _a, off, size = hdr(i)
        end = data.index(b"\0", stroff + name_off)
        out[data[stroff + name_off:end].decode("ascii", "replace")] = (off, size)
    return out


def module_info(data):
    """Return (vermagic, {symbol: crc}) of a kernel module."""
    sec = elf_sections(data)
    if ".modinfo" not in sec or "__versions" not in sec:
        raise Refuse("module has no .modinfo or __versions section (built without CONFIG_MODVERSIONS?)")
    off, size = sec[".modinfo"]
    info = {}
    for item in data[off:off + size].split(b"\0"):
        if b"=" in item:
            k, v = item.split(b"=", 1)
            info[k.decode("ascii", "replace")] = v.decode("ascii", "replace")
    if "vermagic" not in info:
        raise Refuse("module has no vermagic string")
    off, size = sec["__versions"]
    if size % 64:
        raise Refuse("__versions section has an unexpected size")
    crcs = {}
    for i in range(0, size, 64):
        crc, = struct.unpack_from("<Q", data, off + i)
        crcs[data[off + i + 8:off + i + 64].split(b"\0")[0].decode("ascii", "replace")] = crc
    return info["vermagic"], crcs


def check_module_compat(ours, stock):
    v_ours, c_ours = module_info(ours)
    v_stock, c_stock = module_info(stock)
    if v_ours != v_stock:
        raise Refuse("the patched module was built for kernel '%s' but the module in your image says '%s'. "
                     "Your Lineage build is not supported by this release." % (v_ours, v_stock))
    missing = sorted(n for n in c_ours if n not in c_stock)
    differ = sorted(n for n in c_ours if n in c_stock and c_ours[n] != c_stock[n])
    if missing or differ:
        raise Refuse("symbol checksums do not match your stock module (missing in yours: %s; different: %s). "
                     "The module would be refused by your kernel." % (", ".join(missing[:5]) or "none",
                                                                      ", ".join(differ[:5]) or "none"))
    return v_stock, len(c_ours)


# ------------------------------------------------------------------ tree operations (work on a mounted image)
def regular_files(root, candidates):
    found = []
    for rel in candidates:
        p = os.path.join(root, rel)
        if os.path.isfile(p) and not os.path.islink(p):
            found.append(p)
    return found


def write_in_place(path, data):
    with open(path, "r+b") as f:       # no new blocks are needed: same file, same or smaller size
        f.write(data)
        f.truncate(len(data))


def patch_vendor_tree(root, shim, write=True, log=print):
    planned = []
    for name, (size, edits) in EDITS.items():
        paths = regular_files(root, ["lib64/mt6789/" + name, "lib64/" + name])
        if not paths:
            raise Refuse("%s not found as a regular file under lib64/ (or lib64/mt6789/)" % name)
        for p in paths:
            with open(p, "rb") as f:
                buf = bytearray(f.read())
            if len(buf) != size:
                raise Refuse("%s has %d bytes, expected %d: not the library this release was made for"
                             % (os.path.relpath(p, root), len(buf), size))
            for off, old, new in edits:
                if bytes(buf[off:off + len(old)]) != old:
                    raise Refuse("%s differs from the expected library at offset 0x%x: found %s, expected %s"
                                 % (os.path.relpath(p, root), off, bytes(buf[off:off + len(old)]).hex(), old.hex()))
                buf[off:off + len(new)] = new
            planned.append((p, bytes(buf)))
            log("  ok  %s (%d bytes, %d edits)" % (os.path.relpath(p, root), size, len(edits)))
    wr = regular_files(root, ["lib64/" + WRAPPER])
    if not wr:
        raise Refuse("lib64/%s not found as a regular file" % WRAPPER)
    old_size = os.path.getsize(wr[0])
    if len(shim) > old_size:
        raise Refuse("the shim (%d bytes) is larger than the wrapper it replaces (%d bytes)" % (len(shim), old_size))
    planned.append((wr[0], shim))
    log("  ok  lib64/%s (%d -> %d bytes)" % (WRAPPER, old_size, len(shim)))
    if write:
        for p, b in planned:
            write_in_place(p, b)
    return len(planned)


def patch_dlkm_tree(root, module, write=True, log=print):
    paths = regular_files(root, ["lib/modules/" + MODULE])
    if not paths:
        raise Refuse("lib/modules/%s not found as a regular file" % MODULE)
    with open(paths[0], "rb") as f:
        stock = f.read()
    vermagic, nsyms = check_module_compat(module, stock)
    if len(module) > len(stock):
        raise Refuse("the patched module (%d bytes) is larger than the one it replaces (%d bytes)" % (len(module), len(stock)))
    log("  ok  %s: kernel '%s', %d imported symbols match" % (MODULE, vermagic, nsyms))
    if write:
        write_in_place(paths[0], module)
    return vermagic


# ------------------------------------------------------------------ image handling (needs root)
def sh(cmd, check=True):
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
    if check and r.returncode != 0:
        raise Refuse("command failed: %s\n%s" % (" ".join(cmd), r.stdout.strip()))
    return r


def process_image(label, stock, out_path, fn, check_only):
    """check_only: mount the stock image read-only and only run the checks. Otherwise copy it and write into the copy."""
    feats = sh(["dumpe2fs", "-h", stock]).stdout
    if "shared_blocks" in feats:
        raise Refuse("%s uses shared blocks and cannot be mounted read-write; ask for help" % label)
    target = stock if check_only else out_path
    if not check_only:
        shutil.copyfile(stock, out_path)
        CREATED.append(out_path)
    mnt = tempfile.mkdtemp(prefix="q25_")
    mounted = False
    try:
        pre = sh(["e2fsck", "-fn", target], check=False).returncode
        sh(["mount", "-o", "loop,ro" if check_only else "loop", target, mnt])
        mounted = True
        result = fn(mnt, not check_only)
    finally:
        if mounted:
            sh(["umount", mnt], check=False)
        try:
            os.rmdir(mnt)
        except OSError:
            pass
    if check_only:
        return result, None
    sh(["tune2fs", "-M", "", out_path])           # forget the temporary mount point path
    post = sh(["e2fsck", "-fn", out_path], check=False).returncode
    if post != 0 and pre == 0:
        raise Refuse("the filesystem of %s has errors after patching" % label)
    if os.path.getsize(out_path) != os.path.getsize(stock):
        raise Refuse("size of the patched %s image changed" % label)
    return result, digest(out_path, "sha256")


def copy_verified(src, dst):
    """Copy src to dst and prove the copy: the file you will flash must be exactly the file that was built and checked."""
    shutil.copyfile(src, dst)
    want, got = digest(src, "sha256"), digest(dst, "sha256")
    if want != got:
        os.remove(dst)
        raise Refuse("the copy %s does not match the original (sha256 %s, expected %s)" % (dst, got[:16], want[:16]))
    return want


def load_asset(path, name):
    if not os.path.isfile(path):
        raise Refuse("%s not found (put it next to this script or pass the path)" % path)
    got = digest(path, "md5")
    if got != ASSET_MD5[name]:
        raise Refuse("%s has md5 %s, expected %s (use the file from the same release)" % (path, got, ASSET_MD5[name]))
    with open(path, "rb") as f:
        return f.read()


# ------------------------------------------------------------------ zip mode: find, extract, and print the PowerShell
ZIP_PATTERNS = ["/mnt/*/Users/*/Downloads/lineage-*-Q25-signed.zip", "./lineage-*-Q25-signed.zip"]


def invoker_home():
    user = os.environ.get("SUDO_USER")
    try:
        import pwd
        return pwd.getpwnam(user).pw_dir if user else os.path.expanduser("~")
    except Exception:
        return os.path.expanduser("~")


def find_zip(arg=None, patterns=None):
    if arg:
        if not os.path.isfile(arg):
            raise Refuse("%s not found" % arg)
        return arg
    found = []
    for pat in (patterns or ZIP_PATTERNS):
        found += glob.glob(pat)
    found = sorted(set(found))
    if not found:
        raise Refuse("no Lineage zip (lineage-*-Q25-signed.zip) found in your Windows Downloads folder. "
                     "Download the zip there, or pass its path with --zip")
    if len(found) > 1:
        raise Refuse("more than one Lineage zip found:\n  " + "\n  ".join(found) +
                     "\nPass the one you installed with --zip <path>")
    return found[0]


def find_dumper(extra_dirs=()):
    exe = shutil.which("payload-dumper-go")
    if exe:
        return exe
    for d in list(extra_dirs) + [os.path.dirname(os.path.abspath(__file__)), invoker_home(), os.path.join(invoker_home(), "q25")]:
        p = os.path.join(d, "payload-dumper-go")
        if os.path.isfile(p) and os.access(p, os.X_OK):
            return p
    raise Refuse("payload-dumper-go not found. Install it once (see the install step of LINEAGE.md) and run this again")


def win_path(p):
    """Windows form of a WSL path, for the commands we print."""
    try:
        r = subprocess.run(["wslpath", "-w", p], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, universal_newlines=True)
        if r.returncode == 0 and r.stdout.strip():
            return r.stdout.strip()
    except OSError:
        pass
    parts = os.path.abspath(p).split("/")
    if len(parts) > 3 and parts[1] == "mnt" and len(parts[2]) == 1:
        return parts[2].upper() + ":\\" + "\\".join(parts[3:])
    return p


def extract_images(zip_path, workdir, log=print):
    log("Extracting payload.bin from the zip (about 1.2 GB) ...")
    with zipfile.ZipFile(zip_path) as z:
        if "payload.bin" not in z.namelist():
            raise Refuse("payload.bin is not in %s: is it a full Lineage zip?" % zip_path)
        z.extract("payload.bin", workdir)
    payload = os.path.join(workdir, "payload.bin")
    outdir = os.path.join(workdir, "stock")
    os.makedirs(outdir, exist_ok=True)
    log("Extracting vendor and vendor_dlkm ...")
    r = subprocess.run([find_dumper(), "-p", "vendor,vendor_dlkm", "-o", outdir, payload],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
    os.remove(payload)
    v, d = os.path.join(outdir, "vendor.img"), os.path.join(outdir, "vendor_dlkm.img")
    if r.returncode != 0 or not (os.path.isfile(v) and os.path.isfile(d)):
        raise Refuse("payload-dumper-go did not produce vendor.img and vendor_dlkm.img:\n" + r.stdout[-600:])
    return v, d


def _block(lines, wrap=True):
    """One PowerShell script block (wrap=True), or plain script text (wrap=False, for a .ps1 file where a `throw`
    ends the script). Pasted loose lines would each run on their own, even after an error, so never paste unwrapped text."""
    if not wrap:
        return "".join(l + "\n" for l in lines)
    return "& {\n" + "".join("  " + l + "\n" for l in lines) + "}\n"


_READ_SLOT = [
    "$o = (.\\adb shell getprop ro.boot.slot_suffix 2>$null)",
    "$slot = $null",
]
_VERITY_CHECK = [
    "if (-not $o) { throw 'STOP: no phone found by adb. Connect it, with USB debugging on.' }",
    "$slot = \"$o\".Trim().TrimStart('_')",
    "if ($slot -notin 'a','b') { throw 'STOP: could not read the active slot from the phone' }",
    ".\\adb root",
    ".\\adb wait-for-device",
    "\"slot: $slot   kernel: \" + (.\\adb shell uname -r)",
    "$t = ((.\\adb shell \"dmctl table vendor_$slot\") -join ' ') + ((.\\adb shell \"dmctl table vendor_dlkm_$slot\") -join ' ')",
    "if ($t -notmatch 'linear' -or $t -match 'verity') { throw 'STOP: verity may be active, or Rooted debugging is off. Do not flash.' }",
    "\"OK: slot $slot, no verity on vendor/vendor_dlkm\"",
]


def ps_preflight(wrap=True):
    return _block(_READ_SLOT + _VERITY_CHECK, wrap)


def ps_flash(dirwin, dlkm_img="vendor_dlkm_patched.img", vendor_img="vendor_patched.img", check_phone=True, wrap=True):
    """check_phone=False is for the undo: it also works when the phone is already in fastbootd (adb cannot see it)."""
    lines = list(_READ_SLOT)
    if check_phone:
        lines += _VERITY_CHECK
    else:
        lines += ["if ($o) { $slot = \"$o\".Trim().TrimStart('_') }"]
    lines += [
        "if ($o) { .\\adb reboot fastboot; Start-Sleep 10 }",
        "$cur = (.\\fastboot getvar current-slot 2>&1 | Out-String)",
        "if ($cur -match 'current-slot:\\s*([ab])') { $fs = $Matches[1] } else { throw \"STOP: cannot read the slot from fastboot ($cur)\" }",
        "if ($slot -and $slot -ne $fs) { throw \"STOP: slot mismatch: adb says $slot, fastboot says $fs\" }",
        "$slot = $fs",
        "\"Flashing slot $slot\"",
        ".\\fastboot flash \"vendor_dlkm_$slot\" \"%s\\%s\"" % (dirwin, dlkm_img),
        "if ($LASTEXITCODE -ne 0) { throw 'STOP: flashing vendor_dlkm failed. Do not reboot; ask for help.' }",
        ".\\fastboot flash \"vendor_$slot\" \"%s\\%s\"" % (dirwin, vendor_img),
        "if ($LASTEXITCODE -ne 0) { throw 'STOP: flashing vendor failed. Do not reboot; ask for help.' }",
        ".\\fastboot reboot",
    ]
    return _block(lines, wrap)


def ps_verify(wrap=True):
    return _block([
        ".\\adb wait-for-device",
        ".\\adb root",
        ".\\adb wait-for-device",
        ".\\adb shell \"md5sum /vendor_dlkm/lib/modules/imgsensor_isp6s.ko /vendor/lib64/libremosaic_wrapper.so\"",
        ".\\adb shell \"sha256sum /vendor/lib64/libmtkcam_featurepolicy.so\"",
    ], wrap)


SCRIPT_NAMES = ("A_check.ps1", "B_flash.ps1", "C_verify.ps1", "UNDO.ps1")


def write_scripts(out_dir, win_out, win_undo):
    """Write the four PowerShell steps as .ps1 files (UTF-8 with BOM, CRLF) so nobody has to paste long blocks."""
    texts = {
        "A_check.ps1": ps_preflight(False),
        "B_flash.ps1": ps_flash(win_out, wrap=False),
        "C_verify.ps1": ps_verify(False),
        "UNDO.ps1": ps_flash(win_undo, "vendor_dlkm.img", "vendor.img", check_phone=False, wrap=False),
    }
    os.makedirs(out_dir, exist_ok=True)
    for name in SCRIPT_NAMES:
        with open(os.path.join(out_dir, name), "w", encoding="utf-8-sig", newline="") as f:
            f.write(texts[name].replace("\n", "\r\n"))
    return list(SCRIPT_NAMES)


def run_hint(win_dir, name):
    return 'powershell -ExecutionPolicy Bypass -File "%s\\%s"' % (win_dir, name)


def main(argv=None):
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description="Build patched vendor/vendor_dlkm images from your own Lineage build (file level).")
    ap.add_argument("--zip", help="your Lineage zip (default: looked up in your Windows Downloads folder)")
    ap.add_argument("--vendor", help="your stock vendor image (instead of --zip)")
    ap.add_argument("--vendor-dlkm", help="your stock vendor_dlkm image (instead of --zip)")
    ap.add_argument("--out", help="output folder (default: q25_out next to the zip)")
    ap.add_argument("--shim", default=os.path.join(here, WRAPPER))
    ap.add_argument("--module", default=os.path.join(here, MODULE))
    ap.add_argument("--check-only", action="store_true", help="only check that your build is supported; write nothing")
    ap.add_argument("--force", action="store_true", help="overwrite existing output files")
    ap.add_argument("--write-scripts", metavar="OUT_DIR", help="only (re)write the four PowerShell scripts into an existing output folder")
    a = ap.parse_args(argv)
    if a.write_scripts:
        d = os.path.abspath(a.write_scripts)
        names = write_scripts(d, win_path(d), win_path(os.path.join(d, "stock_undo")))
        print("Wrote %s into %s" % (", ".join(names), win_path(d)))
        return 0
    workdir = None
    try:
        if sys.platform != "linux":
            raise Refuse("this tool needs Linux or WSL (it mounts ext4 images)")
        if os.geteuid() != 0:
            raise Refuse("run it with sudo: sudo python3 apply_lineage.py")
        for tool in ("mount", "umount", "e2fsck", "tune2fs", "dumpe2fs"):
            if not shutil.which(tool):
                raise Refuse("%s not found; install it with: sudo apt install -y e2fsprogs" % tool)
        shim = load_asset(a.shim, WRAPPER)
        module = load_asset(a.module, MODULE)
        zip_mode = not (a.vendor or a.vendor_dlkm)
        if bool(a.vendor) != bool(a.vendor_dlkm):
            raise Refuse("give both --vendor and --vendor-dlkm, or neither")
        if zip_mode:
            zpath = find_zip(a.zip)
            print("Lineage zip: %s" % os.path.basename(zpath))
            if shutil.disk_usage("/tmp").free < (4 << 30):
                raise Refuse("less than 4 GB free in the Linux file system; free some space and try again")
            workdir = tempfile.mkdtemp(prefix="q25work_")
            stock_v, stock_d = extract_images(zpath, workdir)
            out = a.out or os.path.join(os.path.dirname(os.path.abspath(zpath)), "q25_out")
        else:
            for p in (a.vendor, a.vendor_dlkm):
                if not os.path.isfile(p):
                    raise Refuse("%s not found" % p)
            stock_v, stock_d = a.vendor, a.vendor_dlkm
            out = a.out or "patched"
            workdir = tempfile.mkdtemp(prefix="q25work_")
        final_v, final_d = os.path.join(out, "vendor_patched.img"), os.path.join(out, "vendor_dlkm_patched.img")
        undo_dir = os.path.join(out, "stock_undo")
        if not a.check_only:
            os.makedirs(out, exist_ok=True)
            for p in (final_v, final_d):
                if os.path.exists(p) and not a.force:
                    raise Refuse("%s exists (use --force)" % p)
            if shutil.disk_usage(out).free < (os.path.getsize(stock_v) + os.path.getsize(stock_d)) * 2 + (300 << 20):
                raise Refuse("not enough free disk space for the output folder (need about 2.5 GB)")
        work_v, work_d = os.path.join(workdir, "vendor_patched.img"), os.path.join(workdir, "vendor_dlkm_patched.img")
        print("vendor_dlkm: checking ...")
        vermagic, d_hash = process_image("vendor_dlkm", stock_d, work_d, lambda m, w: patch_dlkm_tree(m, module, w), a.check_only)
        print("vendor: checking ...")
        _n, v_hash = process_image("vendor", stock_v, work_v, lambda m, w: patch_vendor_tree(m, shim, w), a.check_only)
        if a.check_only:
            print("\nYour build is supported. Nothing was written.")
        else:
            os.makedirs(undo_dir, exist_ok=True)
            for src, dst in ((work_v, final_v), (work_d, final_d),
                             (stock_v, os.path.join(undo_dir, "vendor.img")), (stock_d, os.path.join(undo_dir, "vendor_dlkm.img"))):
                CREATED.append(dst)
                copy_verified(src, dst)
            uid = os.environ.get("SUDO_UID")
            for root_, _d, files in os.walk(out):
                for name in files + [""]:
                    try:
                        if uid:
                            os.chown(os.path.join(root_, name), int(uid), int(os.environ.get("SUDO_GID", uid)))
                    except OSError:
                        pass
            w_out, w_undo = win_path(out), win_path(undo_dir)
            print("\nAll done. The images were built and checked, but NOT flashed.")
            print("  vendor       sha256 %s\n  vendor_dlkm  sha256 %s" % (v_hash, d_hash))
            print("  patched images: %s\n  YOUR UNDO IMAGES (keep them): %s" % (w_out, w_undo))
            print("\nThe module is built for kernel '%s'. Your phone must run the kernel from the SAME Lineage zip." % vermagic)
            write_scripts(out, w_out, w_undo)
            for name in SCRIPT_NAMES:
                try:
                    if uid:
                        os.chown(os.path.join(out, name), int(uid), int(os.environ.get("SUDO_GID", uid)))
                except OSError:
                    pass
            bar = "-" * 78
            print("\n%s\nNEXT, in PowerShell opened in the folder that contains adb.exe and fastboot.exe, one step at a time.\n"
                  "(Each step is a small script in your output folder; open one in Notepad if you want to read it first.)\n%s\n"
                  "STEP A - check the phone (changes nothing):\n  %s\n"
                  "STEP B - flash (only if step A said OK; the phone restarts into fastbootd):\n  %s\n"
                  "STEP C - after it has booted, check:\n  %s\n"
                  "TO UNDO (also works from fastbootd):\n  %s\n%s"
                  % (bar, bar, run_hint(w_out, "A_check.ps1"), run_hint(w_out, "B_flash.ps1"),
                     run_hint(w_out, "C_verify.ps1"), run_hint(w_out, "UNDO.ps1"), bar))
            CREATED.clear()
        return 0
    except Refuse as e:
        print("\nSTOP: %s\nNothing was flashed and your stock images were not changed." % e)
        for p in CREATED:
            if os.path.exists(p):
                os.remove(p)
        return 1
    except Exception as e:                      # unexpected error: still do not leave half-built images behind
        print("\nERROR: %s\nNothing was flashed and your stock images were not changed." % e)
        for p in CREATED:
            if os.path.exists(p):
                os.remove(p)
        return 2
    finally:
        if workdir and os.path.isdir(workdir):
            shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
