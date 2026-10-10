# v0.1.0-alpha: first public alpha (experimental)

Real 50 MP photos (6144 x 8160) on the Zinwa Q25, firmware `Q25_20.01.2026` only. **Read INSTALL.md and UNDO.md first.** Tested on one phone.

## Files attached to this release
* `manifest.json`: what the patches expect and produce (with all checksums)
* `SHA256SUMS`: checksums of the files below
* `vendor.qpatch` (99 KB) and `vendor_dlkm.qpatch` (72 KB): binary patches that turn the stock images into the patched ones

The scripts are in the `tools` folder of the repository.

## Checksums
| | SHA-256 |
|---|---|
| official firmware zip `OS-new-camera-0120-Q25-GMS.zip` | `1C00769031A31125CFC0A2C71FC68C4AFD36660F75344358B034466397DEF2E8` |
| stock `vendor_a.img` (832167936 bytes) | `292888a467345e472cab1e7a2fd6f895607c263cde496653ad345c24e0ab049f` |
| stock `vendor_dlkm_a.img` (33808384 bytes) | `aafee0cc1be8baf60d884e08e0e8d6d6c0163a254d977eb9bf13fd916d95b50c` |
| result `vendor_patched.img` | `5135c858f7c7a635d914b54447f36583aa2a4a3f2028654d0031bd7c10f16b40` |
| result `vendor_dlkm_patched.img` | `085e407f6d8c9233aad7334e578115e714d114ce9ae5b7845ae3c46fa03a4ce0` |

`SHA256SUMS`:
```
9537052e056cb546f9cea5569d973721f6ede4f6652a8a77d544aa99531953c5  manifest.json
9065cfabbfc705e09e154c768e66f00f57c0780627e2b68eb0c152b347681b9d  vendor.qpatch
65fbee323f5aa8c469dbd57e4050b40d7aae016cd0d18d7e5b1edd84e8125e21  vendor_dlkm.qpatch
```

## What it is and is not
* Settings: denoise 2.0, sharpening off. Photos are clean but soft by default; sharpen in post.
* Manual ISO/shutter modes skip the 50 MP path; above ISO 3200 the camera falls back to its normal processing.
* About 2.3 s per photo; the first ~8 photos are slower while it calibrates.
* Needs an unlocked bootloader and disabled verified boot. The boot screen shows an orange warning; that is normal.

## Known gaps
* Install steps for the unlock, the re-lock and turning verified boot back on come from my notes and are marked `[VERIFY]`.
* Not yet tested by anyone other than me; not tested from a factory-reset phone.
