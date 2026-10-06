# Camera HAL byte patches

The release patches three of Zinwa's vendor libraries, adds two properties to `build.prop`, and replaces one library (the remosaic wrapper). They are described here as exact byte edits instead of publishing the modified libraries.

| Library | What the edit does (details and exact bytes: [PATCHES.md](PATCHES.md)) |
|---|---|
| `libmtkcam_metastore.so` | size table: the largest picture size 4608 x 3456 becomes 8160 x 6144, so apps are offered 50 MP |
| `libmtkcam_featurepolicy.so` | size gate: a small code cave decides whether a request may use the 50 MP path (only requests wider than 4096 pixels) |
| `libmtkcam_3rdparty.mtk.so` | removes the app vendor-tag check and raises the ISO limit from 200 to 3200 |
| `lib64/libremosaic_wrapper.so` (replaced) | the stock MediaTek wrapper is replaced by the remosaic shim, see `src/shim/` |
| `build.prop` (vendor) | two lines added: `persist.vendor.remoshim.pdcolor=0` and `persist.vendor.remoshim.diag=0`; nothing else changed |

The libraries live in `/vendor/lib64/mt6789/`; the same names in `/vendor/lib64/` are symlinks to them.

**Status:** documented in [PATCHES.md](PATCHES.md): the three libraries byte by byte, the program-header change, and the complete list of other differences from the stock image. After any rebuild of the release image, re-check that list as described in `maintainer/README.md`.
