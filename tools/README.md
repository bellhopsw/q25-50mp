# Tools (for installing)

Four small Python scripts (Python 3.9 or newer, nothing else to install, Windows or Linux). They turn **your own stock images** into the patched images. **None of them flashes anything.** Follow [INSTALL.md](../INSTALL.md); it contains every command ready to copy and paste.

| Script | What it does |
|---|---|
| `unsparse.py` | converts the firmware's big `super.img` (an Android "sparse" image) into a raw image |
| `imginfo.py` | tells you what kind of image a file is, prints its SHA-256, and (with `--zeros`) checks that a backup file is not empty |
| `apply_release.py` | **the main one**: checks your stock images and the release files, then builds the patched images |
| `qpatch.py` | the patch engine used by `apply_release.py` |

You also need `lpunpack.py` (downloaded in INSTALL.md, Part 0). Put all of these scripts and the release files (`manifest.json`, `SHA256SUMS`, `vendor.qpatch`, `vendor_dlkm.qpatch`) in **one folder**.

## What the safety checks do
* Your stock images are only read, never changed.
* Everything is checked against SHA-256 values: your firmware images, the patch files, and the finished images. If anything does not match, the script stops and writes nothing.
* An existing output file is never overwritten unless you pass `--force`.
* It needs roughly 3 times the image size in free disk space while it works.

## If a script stops with an error
Read the message: it says what did not match. The usual causes are a different firmware build, a damaged download, or a file in the wrong folder. See the "If something goes wrong" section of INSTALL.md.
