#!/usr/bin/env python3
"""Tests for apply_lineage.py on synthetic trees and synthetic ELF modules (no image, root or phone needed).
Run: python3 test_apply_lineage.py   (needs gcc to build the test modules)"""
import os, random, shutil, subprocess, sys, tempfile, unittest
_here = os.path.dirname(os.path.abspath(__file__))
for _p in (_here, os.path.join(_here, "..", "..", "tools"), os.path.join(_here, "..", "tools")):
    if os.path.isfile(os.path.join(_p, "apply_lineage.py")):
        sys.path.insert(0, _p)
        break
import apply_lineage as A

C_SRC = r"""
struct mv { unsigned long crc; char name[56]; };
__attribute__((section("__versions"), used)) static const struct mv v[] = { %s };
__attribute__((section(".modinfo"), used)) static const char vm[] = "vermagic=%s";
static const char pad[%d] = {1};
const char *keep(void) { return pad; }
"""
VM = "5.10.198-g2a873a3511ee SMP preempt mod_unload modversions aarch64"
SYMS = [(0x11111111, "module_layout"), (0x22222222, "printk"), (0x33333333, "kfree")]


def rd(p):
    with open(p, "rb") as f:
        return f.read()


def wr(p, b, mode="wb"):
    with open(p, mode) as f:
        f.write(b)


def build_module(tmp, name, syms=SYMS, vermagic=VM, pad=100):
    entries = ", ".join('{0x%x, "%s"}' % (c, n) for c, n in syms)
    src = os.path.join(tmp, name + ".c"); obj = os.path.join(tmp, name + ".o")
    wr(src, (C_SRC % (entries, vermagic, pad)).encode())
    subprocess.check_call(["gcc", "-c", "-O0", "-o", obj, src])
    return rd(obj)


def make_vendor(root, flat=True, wrapper_size=981216, tamper=None):
    rnd = random.Random(1)
    base = "lib64" if flat else "lib64/mt6789"
    os.makedirs(os.path.join(root, base), exist_ok=True)
    for name, (size, edits) in A.EDITS.items():
        b = bytearray(rnd.randbytes(size))
        for off, old, new in edits:
            b[off:off + len(old)] = old
        if tamper == name:
            b[A.EDITS[name][1][0][0]] ^= 0xFF
        wr(os.path.join(root, base, name), bytes(b))
    wr(os.path.join(root, "lib64", A.WRAPPER), rnd.randbytes(wrapper_size))


class Tests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp)
        self.root = os.path.join(self.tmp, "root"); os.makedirs(self.root)
        self.quiet = lambda *_: None

    def snapshot(self):
        out = {}
        for d, _, fs in os.walk(self.root):
            for f in fs:
                p = os.path.join(d, f)
                out[p] = rd(p)
        return out

    # ---- the edit table itself
    def test_table_is_the_documented_62_bytes(self):
        total = sum(len(n) for _s, es in A.EDITS.values() for _o, _old, n in es)
        changed = sum(1 for _s, es in A.EDITS.values() for _o, old, n in es for a, b in zip(old, n) if a != b)
        self.assertEqual(total, 62)
        self.assertEqual(changed, 59)          # 3 of the 62 bytes keep their value (per the byte comparison)
        for _s, es in A.EDITS.values():
            for _o, old, n in es:
                self.assertEqual(len(old), len(n))

    def test_table_matches_patch_hal_if_present(self):
        """The edit table must stay identical to maintainer/patch_hal.py (the proven copy)."""
        here = os.path.dirname(os.path.abspath(__file__))
        for d in (here, os.path.join(here, ".."), os.path.join(here, "..", "..", "maintainer"), os.path.join(here, "..", "maintainer")):
            if os.path.isfile(os.path.join(d, "patch_hal.py")):
                sys.path.insert(0, d)
                import patch_hal
                self.assertEqual(patch_hal.EDITS, A.EDITS)
                self.assertEqual(patch_hal.CAVE, A.CAVE)
                return
        self.skipTest("patch_hal.py not found next to the tests")

    # ---- vendor tree
    def test_flat_layout_is_patched_in_place(self):
        make_vendor(self.root, flat=True)
        before = self.snapshot()
        shim = random.Random(2).randbytes(149608)
        A.patch_vendor_tree(self.root, shim, True, self.quiet)
        after = self.snapshot()
        for name, (size, edits) in A.EDITS.items():
            p = os.path.join(self.root, "lib64", name)
            self.assertEqual(len(after[p]), size)
            exp = bytearray(before[p])
            for off, old, new in edits:
                exp[off:off + len(new)] = new
            self.assertEqual(after[p], bytes(exp))
        self.assertEqual(after[os.path.join(self.root, "lib64", A.WRAPPER)], shim)

    def test_mt6789_layout_with_symlinks(self):
        make_vendor(self.root, flat=False)
        for name in A.EDITS:                     # lib64/NAME are symlinks, as on the Zinwa image
            os.symlink("mt6789/" + name, os.path.join(self.root, "lib64", name))
        A.patch_vendor_tree(self.root, b"S" * 1000, True, self.quiet)
        real = os.path.join(self.root, "lib64/mt6789/libmtkcam_metastore.so")
        self.assertEqual(rd(real)[0x44741:0x44743], bytes.fromhex("fc83"))
        self.assertTrue(os.path.islink(os.path.join(self.root, "lib64/libmtkcam_metastore.so")))

    def test_wrong_old_bytes_refuse_and_write_nothing(self):
        make_vendor(self.root, tamper="libmtkcam_featurepolicy.so")
        before = self.snapshot()
        with self.assertRaises(A.Refuse):
            A.patch_vendor_tree(self.root, b"S" * 1000, True, self.quiet)
        self.assertEqual(self.snapshot(), before)

    def test_wrong_size_refuses(self):
        make_vendor(self.root)
        p = os.path.join(self.root, "lib64/libmtkcam_metastore.so")
        wr(p, b"x", "ab")
        with self.assertRaises(A.Refuse):
            A.patch_vendor_tree(self.root, b"S", True, self.quiet)

    def test_missing_library_refuses(self):
        make_vendor(self.root)
        os.remove(os.path.join(self.root, "lib64/libmtkcam_3rdparty.mtk.so"))
        with self.assertRaises(A.Refuse):
            A.patch_vendor_tree(self.root, b"S", True, self.quiet)

    def test_shim_larger_than_wrapper_refuses(self):
        make_vendor(self.root, wrapper_size=1000)
        with self.assertRaises(A.Refuse):
            A.patch_vendor_tree(self.root, b"S" * 2000, True, self.quiet)

    def test_check_only_writes_nothing(self):
        make_vendor(self.root)
        before = self.snapshot()
        A.patch_vendor_tree(self.root, b"S" * 1000, False, self.quiet)
        self.assertEqual(self.snapshot(), before)

    # ---- kernel module
    def dlkm(self, stock):
        os.makedirs(os.path.join(self.root, "lib/modules"), exist_ok=True)
        wr(os.path.join(self.root, "lib/modules", A.MODULE), stock)

    def test_module_replaced_when_compatible(self):
        ours = build_module(self.tmp, "ours", pad=100)
        stock = build_module(self.tmp, "stock", pad=5000)
        self.dlkm(stock)
        vm = A.patch_dlkm_tree(self.root, ours, True, self.quiet)
        self.assertEqual(vm, VM)
        self.assertEqual(rd(os.path.join(self.root, "lib/modules", A.MODULE)), ours)

    def test_module_refused_on_other_kernel(self):
        ours = build_module(self.tmp, "ours")
        stock = build_module(self.tmp, "stock", vermagic="5.10.200-gdeadbeef SMP preempt mod_unload modversions aarch64", pad=5000)
        self.dlkm(stock)
        with self.assertRaises(A.Refuse):
            A.patch_dlkm_tree(self.root, ours, True, self.quiet)
        self.assertEqual(rd(os.path.join(self.root, "lib/modules", A.MODULE)), stock)

    def test_module_refused_on_different_checksum(self):
        ours = build_module(self.tmp, "ours")
        other = [(0x11111111, "module_layout"), (0x22222299, "printk"), (0x33333333, "kfree")]
        self.dlkm(build_module(self.tmp, "stock", syms=other, pad=5000))
        with self.assertRaises(A.Refuse):
            A.patch_dlkm_tree(self.root, ours, True, self.quiet)

    def test_module_refused_on_missing_symbol(self):
        ours = build_module(self.tmp, "ours")
        self.dlkm(build_module(self.tmp, "stock", syms=SYMS[:2], pad=5000))
        with self.assertRaises(A.Refuse):
            A.patch_dlkm_tree(self.root, ours, True, self.quiet)

    def test_module_larger_than_stock_refuses(self):
        ours = build_module(self.tmp, "ours", pad=20000)
        self.dlkm(build_module(self.tmp, "stock", pad=100))
        with self.assertRaises(A.Refuse):
            A.patch_dlkm_tree(self.root, ours, True, self.quiet)

    def test_not_an_elf_refuses(self):
        with self.assertRaises(A.Refuse):
            A.module_info(b"not an elf file at all" * 10)

class ZipModeTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp)

    def zipfile_with(self, name, members):
        import zipfile
        p = os.path.join(self.tmp, name)
        with zipfile.ZipFile(p, "w") as z:
            for m, data in members.items():
                z.writestr(m, data)
        return p

    # ---- finding the zip
    def test_find_zip_by_pattern(self):
        d = os.path.join(self.tmp, "Users", "Someone", "Downloads"); os.makedirs(d)
        wr(os.path.join(d, "lineage-23.2-20261004-nightly-Q25-signed.zip"), b"x")
        wr(os.path.join(d, "unrelated.zip"), b"x")
        got = A.find_zip(None, [os.path.join(self.tmp, "Users", "*", "Downloads", "lineage-*-Q25-signed.zip")])
        self.assertTrue(got.endswith("nightly-Q25-signed.zip"))

    def test_find_zip_none_or_many_refuses(self):
        pats = [os.path.join(self.tmp, "lineage-*-Q25-signed.zip")]
        with self.assertRaises(A.Refuse):
            A.find_zip(None, pats)
        wr(os.path.join(self.tmp, "lineage-1-Q25-signed.zip"), b"x")
        wr(os.path.join(self.tmp, "lineage-2-Q25-signed.zip"), b"x")
        with self.assertRaises(A.Refuse):
            A.find_zip(None, pats)

    def test_find_zip_explicit_path(self):
        with self.assertRaises(A.Refuse):
            A.find_zip(os.path.join(self.tmp, "nope.zip"))

    # ---- extraction with a fake payload-dumper-go
    def fake_dumper(self, produce=True):
        d = os.path.join(self.tmp, "bin"); os.makedirs(d, exist_ok=True)
        exe = os.path.join(d, "payload-dumper-go")
        body = "import sys,os\nargs=sys.argv\nout=args[args.index('-o')+1]\nos.makedirs(out,exist_ok=True)\n"
        if produce:
            body += "open(os.path.join(out,'vendor.img'),'wb').write(b'v'); open(os.path.join(out,'vendor_dlkm.img'),'wb').write(b'd')\n"
        wr(exe, ("#!/usr/bin/env python3\n" + body).encode())
        os.chmod(exe, 0o755)
        return d

    def test_extract_images_runs_dumper_and_removes_payload(self):
        z = self.zipfile_with("l.zip", {"payload.bin": b"P" * 100, "other": b"x"})
        d = self.fake_dumper()
        old = os.environ["PATH"]; os.environ["PATH"] = d + os.pathsep + old
        try:
            work = os.path.join(self.tmp, "work"); os.makedirs(work)
            v, dl = A.extract_images(z, work, lambda *_: None)
        finally:
            os.environ["PATH"] = old
        self.assertEqual((rd(v), rd(dl)), (b"v", b"d"))
        self.assertFalse(os.path.exists(os.path.join(work, "payload.bin")))

    def test_extract_images_refuses_zip_without_payload(self):
        z = self.zipfile_with("l.zip", {"boot.img": b"x"})
        with self.assertRaises(A.Refuse):
            A.extract_images(z, self.tmp, lambda *_: None)

    def test_extract_images_refuses_when_dumper_makes_nothing(self):
        z = self.zipfile_with("l.zip", {"payload.bin": b"P"})
        d = self.fake_dumper(produce=False)
        old = os.environ["PATH"]; os.environ["PATH"] = d + os.pathsep + old
        try:
            with self.assertRaises(A.Refuse):
                A.extract_images(z, self.tmp, lambda *_: None)
        finally:
            os.environ["PATH"] = old

    # ---- verified copy
    def test_copy_verified_copies_exactly(self):
        src = os.path.join(self.tmp, "a.img"); dst = os.path.join(self.tmp, "b.img")
        wr(src, b"x" * 5000)
        h = A.copy_verified(src, dst)
        self.assertEqual(rd(dst), rd(src))
        self.assertEqual(h, A.digest(src, "sha256"))

    def test_copy_verified_refuses_and_removes_a_bad_copy(self):
        src = os.path.join(self.tmp, "a.img"); dst = os.path.join(self.tmp, "b.img")
        wr(src, b"x" * 5000)
        real = A.shutil.copyfile
        def broken(s_, d_):
            real(s_, d_)
            with open(d_, "r+b") as f:
                f.write(b"Y")                       # corrupt the copy
        A.shutil.copyfile = broken
        try:
            with self.assertRaises(A.Refuse):
                A.copy_verified(src, dst)
        finally:
            A.shutil.copyfile = real
        self.assertFalse(os.path.exists(dst))

    # ---- the .ps1 files
    def test_write_scripts_files(self):
        d = os.path.join(self.tmp, "q25_out"); os.makedirs(d)
        names = A.write_scripts(d, "C:\\out", "C:\\out\\stock_undo")
        self.assertEqual(names, ["A_check.ps1", "B_flash.ps1", "C_verify.ps1", "UNDO.ps1"])
        for n in names:
            raw = rd(os.path.join(d, n))
            self.assertTrue(raw.startswith(b"\xef\xbb\xbf"), n + " needs a UTF-8 BOM")
            body = raw[3:].decode("utf-8")
            self.assertIn("\r\n", body)
            self.assertNotIn("\n", body.replace("\r\n", ""))              # CRLF only
            self.assertNotIn("& {", body)                                 # a file needs no script-block wrapper
        b = rd(os.path.join(d, "B_flash.ps1")).decode("utf-8-sig")
        self.assertIn("C:\\out\\vendor_dlkm_patched.img", b)
        self.assertIn("C:\\out\\vendor_patched.img", b)
        self.assertLess(b.index("dmctl table"), b.index("fastboot flash"))
        self.assertLess(b.index("slot mismatch"), b.index("fastboot flash"))
        u = rd(os.path.join(d, "UNDO.ps1")).decode("utf-8-sig")
        self.assertIn("C:\\out\\stock_undo\\vendor.img", u)
        self.assertNotIn("dmctl", u)
        a_ = rd(os.path.join(d, "A_check.ps1")).decode("utf-8-sig")
        self.assertNotIn("fastboot", a_)

    def test_write_scripts_option_needs_no_root(self):
        d = os.path.join(self.tmp, "o"); os.makedirs(d)
        self.assertEqual(A.main(["--write-scripts", d]), 0)
        self.assertTrue(os.path.isfile(os.path.join(d, "B_flash.ps1")))

    def test_run_hint(self):
        self.assertEqual(A.run_hint("C:\\x y", "B_flash.ps1"), 'powershell -ExecutionPolicy Bypass -File "C:\\x y\\B_flash.ps1"')

    # ---- paths and PowerShell text
    def test_win_path_fallback_without_wslpath(self):
        real = A.subprocess.run

        def no_wslpath(*a, **k):
            raise OSError("wslpath not installed")
        A.subprocess.run = no_wslpath
        try:
            self.assertEqual(A.win_path("/mnt/d/Users/Someone/Downloads/q25_out").lower(),
                             "d:\\users\\someone\\downloads\\q25_out")
            self.assertEqual(A.win_path("/tmp/x"), "/tmp/x")          # not a drive path: returned unchanged
        finally:
            A.subprocess.run = real

    def test_win_path_uses_wslpath_when_it_answers(self):
        real = A.subprocess.run

        class R:
            returncode = 0
            stdout = "X:\\from\\wslpath\n"
        A.subprocess.run = lambda *a, **k: R()
        try:
            self.assertEqual(A.win_path("/anything"), "X:\\from\\wslpath")
        finally:
            A.subprocess.run = real

    def ps(self, **kw):
        return A.ps_flash("C:\\out", **kw)

    def test_blocks_are_single_script_blocks_with_balanced_braces(self):
        for text in (A.ps_preflight(), self.ps(), self.ps(check_phone=False), A.ps_verify()):
            self.assertTrue(text.startswith("& {\n") and text.rstrip().endswith("}"))
            self.assertEqual(text.count("{"), text.count("}"))

    def test_flash_block_is_safe(self):
        t = self.ps()
        self.assertNotIn(" -w", t)
        self.assertLess(t.index("dmctl table"), t.index("fastboot flash"))          # verity check before any flash
        self.assertLess(t.index("slot mismatch"), t.index("fastboot flash"))         # slot confirmed before any flash
        self.assertLess(t.index("vendor_dlkm_$slot"), t.index("\"vendor_$slot\""))   # dlkm first, then vendor
        self.assertEqual(t.count("$LASTEXITCODE"), 2)
        self.assertNotIn("vendor_b", t); self.assertNotIn("vendor_a", t)             # the slot is never hard-coded
        self.assertNotIn("vendor_dlkm_b", t); self.assertNotIn("vendor_dlkm_a", t)

    def test_undo_block_works_without_adb(self):
        t = self.ps(check_phone=False)
        self.assertNotIn("dmctl", t)
        self.assertIn("fastboot getvar current-slot", t)
        self.assertNotIn("throw 'STOP: no phone found", t)

    def test_preflight_flashes_nothing(self):
        self.assertNotIn("fastboot", A.ps_preflight())


if __name__ == "__main__":
    unittest.main(verbosity=2)
