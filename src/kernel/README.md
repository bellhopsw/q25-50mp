# Sensor driver changes (GPL-2.0)

The camera driver is a loadable module (`imgsensor_isp6s.ko`) built from the LineageOS kernel for this chipset. The published source is exactly what produced the module in the released `vendor_dlkm` image: the stripped module built from it has **md5 `20cf36063834ddb7100fb64bb5e53326`**, the same as the module inside the image.

| File | What it is |
|---|---|
| `BASE_COMMIT.txt` | the upstream commit the changes sit on |
| `kernel-changes.patch` | the changes, made with `git diff`: one file, `drivers/misc/mediatek/imgsensor/src/common/v1_1/s5kjn1_mipi_raw/s5kjn1mipiraw_Sensor.c` |
| `kernel.config` | the kernel `.config` used for the build |

* **Upstream:** https://github.com/LineageOS/android_kernel_xelex_mt6789, branch `lineage-23.2`, commit `2a873a3511ee0eeead1442e60145093192a4535d`
* **Toolchain:** the AOSP prebuilt **clang r416183b** (the build configuration records `Android (7284624, based on r416183b) clang version 12.0.5`), with `LLVM=1 LLVM_IAS=1`

## What the changes do
`kernel-changes.patch` changes one file in six places (a 356-line patch, of which about 280 lines are the new register table):
1. **The capture mode (`.cap`)** becomes the 50 MP mode: 8160 x 6144, pixel clock 560 MHz, line length 8688, frame length 6400, MIPI pixel rate 556 MHz, at most 10 frames per second.
2. **`sensor_output_dataformat`** is declared as `SENSOR_OUTPUT_FORMAT_RAW_4CELL_BAYER_Gr`, so the camera HAL uses its 4-cell session (the remosaic path).
3. **The window size information** for the capture mode is 8160 x 6144.
4. **A new register table**, `capture_50mp_setting_array` (277 address/value pairs), ported verbatim from the Nothing Phone 2a's open-source kernel (NothingOSS, `android_kernel_5.15_nothing_mt6886`, table `addr_data_pair_custom1_jn1`). It matches 167 of the 170 registers of the mainline Linux full-resolution mode; the three registers `0x06b6`, `0x2140`, `0x2176` are not written and untested.
5. **`capture_setting()`** now writes that table; the original capture table is kept and marked `__maybe_unused`.

The stock `custom1` mode is left untouched. The 50 MP mode is placed on the capture scenario.

The version published here is the one called "v3" in the project notes. An experiment that tried to switch on the sensor's own PDAF correction registers (`0x4D92`, `0x84C8` and the table at `0x4D94`) made things worse and is **not** part of this release.

**Line endings:** the upstream source file uses Windows (CRLF) line endings, so the patch does too (a few header lines have none). Do not convert it; `.gitattributes` in this repository keeps these files byte-exact.

## Build environment
```
export PATH=<clang-r416183b>/bin:$PATH
export ARCH=arm64 LLVM=1 LLVM_IAS=1
export CROSS_COMPILE=aarch64-linux-gnu-
export CROSS_COMPILE_COMPAT=arm-linux-gnueabi-
export KCFLAGS="-Wno-error=frame-larger-than="
```
(`CROSS_COMPILE` and `CROSS_COMPILE_COMPAT` are the prefixes of the GNU cross tools; on Debian or Ubuntu they come from the packages `gcc-aarch64-linux-gnu` and `gcc-arm-linux-gnueabi`.) ThinLTO with CFI_CLANG is enabled in the config (`CONFIG_LTO_CLANG_THIN=y`, `CONFIG_CFI_CLANG=y`); it is needed so that the module's symbol checksums match the stock module (all 117 match).

## Rebuild and check
```
git clone https://github.com/LineageOS/android_kernel_xelex_mt6789.git
cd android_kernel_xelex_mt6789
git checkout 2a873a3511ee0eeead1442e60145093192a4535d
git apply <path>/kernel-changes.patch
mkdir out && cp <path>/kernel.config out/.config
# (set the environment above)
make O=out olddefconfig          # should change nothing
make O=out -j$(nproc)
llvm-strip --strip-debug -o imgsensor_isp6s.ko out/drivers/misc/mediatek/imgsensor/src/isp6s/imgsensor_isp6s.ko
md5sum imgsensor_isp6s.ko        # 20cf36063834ddb7100fb64bb5e53326

## Cherry-pick the driver change

The kernel change is a single commit on top of LineageOS `lineage-23.2` at
`2a873a3511ee0eeead1442e60145093192a4535d`, published at
https://github.com/bellhopsw/android_kernel_xelex_mt6789 on branch `q25-50mp`
(commit `c4c1b8e14abe241a71c34ad6169414745193be9f`). It touches one file,
`drivers/misc/mediatek/imgsensor/src/common/v1_1/s5kjn1_mipi_raw/s5kjn1mipiraw_Sensor.c`.

```
git remote add q25 https://github.com/bellhopsw/android_kernel_xelex_mt6789.git
git fetch q25 q25-50mp
git cherry-pick c4c1b8e14abe241a71c34ad6169414745193be9f
```

It applies cleanly to the base commit above; on other trees you may need to
resolve conflicts by hand. The `kernel-changes.patch` in this folder produces
the same file.
```
**[VERIFY]** The md5 match was proven in the author's own working tree (a clean upstream checkout with only this one source file changed, the same configuration and toolchain), and `kernel-changes.patch` was checked to reproduce that source file exactly from the upstream one. A rebuild from a fresh clone with exactly the commands above has not been run yet.

The stripped module is then copied to `lib/modules/imgsensor_isp6s.ko` inside a copy of the stock `vendor_dlkm` image.
