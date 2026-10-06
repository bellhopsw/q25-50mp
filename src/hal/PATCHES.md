# Camera HAL byte patches (compared against the released image)

Method: the stock `vendor_a.img` (SHA-256 `292888a4...049f`) and the released `vendor` image (`5135c858...6b40`) were mounted read-only and the three libraries below were compared with `maintainer/bytediff.py`. (The byte comparison was made on the image before some unused leftover files were deleted from it; the three libraries are not affected by that, and the file-level comparison, `diff -rq`, was repeated on the final image.) The libraries are 64-bit ARM (AArch64). Their real location is `/vendor/lib64/mt6789/`; the same file names in `/vendor/lib64/` are symlinks to them. Instruction names come from disassembling the changed bytes; the *purpose* of each edit comes from the project notes and is marked as such where the disassembly alone does not prove it.

Everything else in this file was checked against the real bytes.

## `libmtkcam_metastore.so` (655,096 bytes): the picture-size table
| File offset | Old bytes | New bytes | What it is |
|---|---|---|---|
| `0x44741`-`0x44742` | `40 82` | `fc 83` | inside the instruction at `0x44740`, a `mov wN, #imm`: the constant **4608** becomes **8160** (width) |
| `0x44755`-`0x44756` | `b0 81` | `00 83` | inside the instruction at `0x44754`, a `mov wN, #imm`: the constant **3456** becomes **6144** (height) |

Total: 4 bytes. Result: the largest picture size offered to apps changes from 4608 x 3456 to 8160 x 6144.

## `libmtkcam_3rdparty.mtk.so` (416,632 bytes): two gates in the remosaic plugin
| File offset | Old bytes | New bytes | What it is |
|---|---|---|---|
| `0x3f048`-`0x3f04b` | `e8 14 00 34` | `1f 20 03 d5` | `cbz w8, 0x3f2e4` becomes `nop`: a conditional jump is never taken. *Project notes:* this removes the check for the app's vendor tag (only MediaTek's own camera app sets it), which otherwise stops the plugin from running |
| `0x4155d`-`0x4155e` | `19 80` | `90 81` | inside the instruction at `0x4155c`: `mov wN, #200` becomes `mov wN, #3200` |
| `0x41695`-`0x41696` | `19 80` | `90 81` | inside the instruction at `0x41694`: `mov wN, #200` becomes `mov wN, #3200` |
| `0x416d9`-`0x416da` | `19 80` | `90 81` | inside the instruction at `0x416d8`: `mov wN, #200` becomes `mov wN, #3200` |

Total: 10 bytes. *Project notes:* 200 is the ISO limit above which the plugin refused remosaic; above ISO 3200 captures now fall back to the normal processing.

## `libmtkcam_featurepolicy.so` (441,352 bytes): the size gate
| File offset | Old bytes | New bytes | What it is |
|---|---|---|---|
| `0x33c20`-`0x33c23` | `f5 03 00 aa` | `d8 d3 00 14` | `mov x21, x0` becomes `b 0x68b80`: a jump into the code cave |
| `0x68b80`-`0x68ba7` (40 bytes) | all `00` | code (see below) | the **code cave**, in what was zero padding |
| `0x000000d0`-`0x000000d1` | `80 7b` | `00 7c` | in the ELF program header table; see the note below |
| `0x000000d8`-`0x000000d9` | `80 7b` | `00 7c` | in the ELF program header table; see the note below |

Total: 48 bytes. The first 32 bytes of the cave, disassembled:
```
0x068b80  f5 03 00 aa   mov  x21, x0              ; the instruction that was replaced
0x068b84  7f 12 00 f1   cmp  x19, #4
0x068b88  e1 84 e5 54   b.ne 0x33c24              ; not the case we care about: resume the normal code
0x068b8c  f0 0e 40 f9   ldr  x16, [x23, #0x18]
0x068b90  b0 84 e5 b4   cbz  x16, 0x33c24         ; no request information: resume
0x068b94  11 96 41 b9   ldr  w17, [x16, #0x194]   ; the requested width
0x068b98  3f 06 40 71   cmp  w17, #4096
0x068b9c  4c 84 e5 54   b.gt 0x33c24              ; width above 4096: resume the normal code (50 MP allowed)
```
The last 8 bytes of the cave:
```
0x068ba0  f4 03 1f 2a   mov  w20, wzr             ; w20 = 0
0x068ba4  42 31 ff 17   b    0x350ac              ; jump to a different place in the function
```
So the logic is: when `x19` is 4 and the request information is present and the requested width is **4096 or less**, set `w20 = 0` and jump to `0x350ac`; in every other case continue with the normal code at `0x33c24`. Requests wider than 4096 pixels therefore take the unmodified path (50 MP allowed). *Project notes:* the jump to `0x350ac` makes the plugin decline remosaic for small requests, so that normal-size photos keep using the binned path.

*Program header:* `readelf -lW` confirms the edit. The executable `LOAD` segment (file offset `0x21000`) has `p_filesz` and `p_memsz` raised from `0x47b80` to `0x47c00`, so it now extends from file offset `0x68b80` to `0x68c00` and the loader maps the cave.

## Other differences from the stock image
* `build.prop`: two lines added, `persist.vendor.remoshim.pdcolor=0` and `persist.vendor.remoshim.diag=0`. **No other property changes** (in particular no ZSL property).
* `lib64/libremosaic_wrapper.so`: the stock MediaTek wrapper is replaced by the remosaic shim (see `src/shim/`).
* No other file differs. *(This list was produced with `diff -rq` on the mounted stock and released images. Re-run it after every rebuild; see `maintainer/README.md`.)*

