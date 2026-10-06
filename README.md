# Q25 50MP: real 50 MP photos on the Zinwa Q25 (experimental)

![pipeline](docs/images/fig1_pipeline.png)

The Zinwa Q25's camera sensor (Samsung ISOCELL JN1) can deliver 50 MP, but the stock firmware only ever saves 12.5 MP photos. This project patches the kernel camera driver and the camera HAL and adds a custom remosaic library, so apps such as **Open Camera** can save real **6144 x 8160** (50 MP) JPEGs. It takes about 2.3 s per photo once warmed up.

> **Experimental alpha (v0.1.0-alpha).** Tested on **one phone**, firmware **`Q25_20.01.2026`** (package `OS-new-camera-0120-Q25-GMS`). It needs an **unlocked bootloader**, replaces the `vendor` and `vendor_dlkm` partitions, and turns off verified boot. A mistake can leave the phone unable to boot; recovery is described in [UNDO.md](UNDO.md). Use at your own risk. Not affiliated with Zinwa, Samsung, MediaTek or Nothing.

## The honest summary
The 50 MP files are clean and carry real fine detail, but they are **soft out of the box**. They are best for cropping and for well-lit scenes. Sharpened in post, small print comes out finer than from the 12.5 MP mode; in dim light the 12.5 MP mode can look punchier. Manual ISO/shutter modes skip the 50 MP path, and above ISO 3200 the camera falls back to its normal processing. See the write-up with pictures: **[docs/WRITEUP.md](docs/WRITEUP.md)** (also as a [PDF](docs/Q25_50MP_writeup.pdf)).

## Start here
| | |
|---|---|
| **[INSTALL.md](INSTALL.md)** | step-by-step install on Windows (copy-paste commands) |
| **[UNDO.md](UNDO.md)** | how to go back to stock, and what to do if the phone does not boot |
| **[FAQ.md](FAQ.md)** | why a photo is not 50 MP, warm-up, OTA updates, the orange boot screen |
| **Releases** (right-hand side of this page) | the patch files: `manifest.json`, `SHA256SUMS`, `vendor.qpatch`, `vendor_dlkm.qpatch` |

No Linux or WSL is needed to *use* a release: just Windows, Python 3.9+, platform-tools and the official Zinwa firmware zip.

## How it avoids distributing Zinwa's firmware
The release contains only small **binary patches**. You rebuild the patched images on your own PC from the stock images in your own copy of the official firmware, and every step is checked with SHA-256 (the tools refuse to continue if anything differs). No vendor image is published here.

## What is in this repository
| Folder | Contents |
|---|---|
| `tools/` | the four scripts you need to install (`apply_release`, `unsparse`, `imginfo`, `qpatch`) |
| `maintainer/` | for the project maintainer: building a release, comparing library bytes, and the tests |
| `src/shim/` | the remosaic library source (C), with build notes and the list of settings |
| `src/kernel/` | notes on the sensor driver changes (GPL) |
| `src/hal/` | notes on the camera HAL byte patches |
| `docs/` | the write-up with pictures |

## Credits
Built on the LineageOS kernel tree for this chipset (`android_kernel_xelex_mt6789`), the open-source JN1 driver from the Nothing Phone 2a (register table), the mainline Linux `s5kjn1` driver (cross-check), and Zinwa's official firmware and flash tool. The analysis, code and documentation were developed with AI assistance (Claude), with an outside second-opinion review of the PDAF analysis.

Special thanks to my wife, for putting up with the long hours this took and for coming along on the drives to take test photos.

## Support
If this project helped you, you can support it at [buymeacoffee.com/bellhopsw](https://buymeacoffee.com/bellhopsw). Thank you!

## Licence
See [LICENSE](LICENSE) and [LICENSES.md](LICENSES.md).
