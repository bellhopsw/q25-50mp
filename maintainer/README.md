# Maintainer tools

Not needed to install the release. These are for building a release and checking the tools.

| File | Purpose |
|---|---|
| `build_release.py` | turns known-good images into a release folder (patches, `manifest.json`, `SHA256SUMS`) and proves each patch by rebuilding the patched image from the stock one |
| `scan_strings.py` | looks for local paths and user names inside binary files (images, libraries, modules) before you publish them |
| `bytediff.py` | lists exactly which bytes differ between two same-size files (used to document the library patches in `src/hal/`) |
| `patch_hal.py` | applies the 62 documented HAL byte edits to copies of the three libmtkcam libraries and refuses to write unless sizes and old bytes match; reproduces the released libraries byte for byte on With-GMS stock |
| `tests/` | automatic checks of the tools on synthetic images (no phone or firmware needed) |

## Before every release: check what is in the image
The patch contains **every** difference between the stock and the patched image, so make sure the patched image contains only what you intend, and nothing from other vendors (leftovers from earlier test builds are easy to forget). Mount both images read-only and list the differences:
```
mkdir -p ~/vs ~/vp
sudo mount -o loop,ro <stock vendor image> ~/vs
sudo mount -o loop,ro <patched vendor image> ~/vp
sudo diff -rq ~/vs ~/vp
sudo diff ~/vs/build.prop ~/vp/build.prop
sudo umount ~/vs ~/vp
```
Expected for the vendor image: `build.prop` with only the `persist.vendor.remoshim.*` lines added, `lib64/libremosaic_wrapper.so` (the shim), and the three patched libraries (listed twice, because `lib64/` has symlinks to `lib64/mt6789/`). **Anything under "Only in ...patched..." must be explained, and must not be someone else's proprietary file.** (Messages about missing symlink targets such as `bin/v3avpud` appear in both images and are harmless.) Do the same for the `vendor_dlkm` image: only the kernel module should differ.

## Before every release: scan the binaries for personal paths
Disk images record where they were last mounted (for example `/home/<you>/vw`) inside the filesystem itself, and libraries can carry build paths. The patches contain those bytes, so scan the **final images** (and any library or module you add), using your own user name as an extra needle:
```
python maintainer\scan_strings.py <patched vendor image> <patched vendor_dlkm image> --needle <your user name>
```
A whole-image scan also reports text that is already in the **stock** firmware (the vendor's own build paths, for example). To check only what a patch would carry, add `--vs <stock image>`: it then scans just the 4 KiB blocks that differ from stock, for example `python maintainer\scan_strings.py <patched vendor image> --vs <stock vendor image> --needle <your user name>`. It must say `CLEAN` for every file. If an image reports a last-mounted path, check it with `sudo dumpe2fs -h <image> | grep -i "mounted on"` and reset it on a copy of the image with `sudo tune2fs -M /vendor <image>` (use `/vendor_dlkm` for the dlkm image). That changes only the superblock; rescan, then rebuild the release (the hashes change) and test-flash the new images.

## Make a release
The inputs must be **raw** images (`imginfo.py` says "raw ext4 filesystem image"). The **stock** images must be exactly what a user gets from the official firmware with the steps in INSTALL.md; the **patched** images are your known-good ones.

```
python maintainer\build_release.py --out release --version 0.1.0-alpha `
    --fw-id "<output of: adb shell getprop ro.build.display.id>" --fw-package OS-new-camera-0120-Q25-GMS `
    --vendor-stock <stock vendor_a.img> --vendor-patched <final vendor image> `
    --dlkm-stock <stock vendor_dlkm_a.img> --dlkm-patched <final vendor_dlkm image>
```
(PowerShell: the backtick at the end of a line continues the command, and must be the last character on the line. You can also put everything on one line.) Publish `manifest.json`, `SHA256SUMS` and the two `.qpatch` files as release assets. **Never publish a `.img` file.**

## Tests
```
python maintainer\tests\test_release_tools.py     # 31 end-to-end checks, about 20 s
python maintainer\tests\fuzz_qpatch.py            # 400 randomised round-trips with tiny chunk and run limits
python maintainer\tests\test_unsparse.py          # sparse converter vs the original and vs Google's simg2img (needs img2simg; skipped if absent)
```
They use synthetic data, so they do not replace one full run with the real firmware: start from freshly extracted official images, build the patched images, and compare their checksums with the release.
