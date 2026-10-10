# Q25 50 MP on LineageOS (alpha)

Copy-paste guide. Tested on **one phone** (mine). This is an alpha: back up your data, keep the undo images this guide makes for you, and stop at any line that says STOP.

**Supported:** the Zinwa Q25 running a LineageOS 23.2 nightly whose `vendor` and `vendor_dlkm` pass the checks below (the checks pass on the `20260927` and `20261004` nightlies; I flashed and tested `20261004` only). The tool tells you whether **your** build is supported before it builds anything. Nothing here wipes your data.

## What you need
* Windows 10/11 with **WSL** (Ubuntu): in PowerShell, `wsl --install` if you do not have it.
* **platform-tools** (adb and fastboot) and the Google USB driver, an **unlocked bootloader** (already true if you installed Lineage), and **USB debugging** and **Rooted debugging** switched on in Settings, System, Developer options.
* Your Lineage zip (the one you installed) in your Windows **Downloads** folder, with its original name. About 6 GB free on C:.

## Step 1: Install the tools (once)
Open **Ubuntu (WSL)** and paste:
```
sudo apt update && sudo apt install -y e2fsprogs python3 curl unzip
mkdir -p ~/q25 && cd ~/q25
curl -L -o pdg.tar.gz https://github.com/ssut/payload-dumper-go/releases/download/2.1.0/payload-dumper-go_2.1.0_linux_amd64.tar.gz
tar -xzf pdg.tar.gz payload-dumper-go
curl -LO https://raw.githubusercontent.com/bellhopsw/q25-50mp/main/tools/apply_lineage.py
curl -LO https://github.com/bellhopsw/q25-50mp/releases/download/v0.1.0-alpha.2-lineage/libremosaic_wrapper.so
curl -LO https://github.com/bellhopsw/q25-50mp/releases/download/v0.1.0-alpha.2-lineage/imgsensor_isp6s.ko
md5sum libremosaic_wrapper.so imgsensor_isp6s.ko
```
The last line must print `0d70942a2e0e8e345ae96f4545c1c3a8` for the wrapper and `20cf36063834ddb7100fb64bb5e53326` for the module.

## Step 2: Build your images (one command)
Still in WSL:
```
cd ~/q25 && sudo python3 apply_lineage.py
```
It finds your Lineage zip in Downloads, extracts what it needs, **checks that your build is supported**, builds the patched images, and puts them, your **undo images** and four small PowerShell scripts in a `q25_out` folder next to the zip. It flashes nothing. It takes a few minutes, and at the end it prints the four commands for Step 3, with your paths filled in.

* If it says `STOP: ...`, nothing was changed. The usual reasons are another Lineage build (not supported by this release), no zip found (use `--zip <path>`), or no space.
* To only ask "is my build supported?" without building: `sudo python3 apply_lineage.py --check-only`.

## Step 3: Check, then flash (PowerShell)
Open **PowerShell in the folder that contains `adb.exe`** (in Explorer: open the folder, then File, Open Windows PowerShell), with the phone connected and unlocked. Run the commands the tool printed, **one at a time**. They look like this (your path will differ):

1. **Check the phone** (changes nothing):
```
powershell -ExecutionPolicy Bypass -File "C:\Users\<you>\Downloads\q25_out\A_check.ps1"
```
It must end with `OK: slot ..., no verity on vendor/vendor_dlkm`. The scripts read **your** slot (a or b) from the phone, so you never choose it.

2. **Flash**, only if the check said OK:
```
powershell -ExecutionPolicy Bypass -File "C:\Users\<you>\Downloads\q25_out\B_flash.ps1"
```
It repeats the check, reads the slot again in fastbootd, **refuses if the two disagree**, flashes `vendor_dlkm` and then `vendor` to your slot, and restarts the phone. If it prints `STOP`, nothing further was flashed.

Never flash these images by hand with other commands, never use `-w`, and do not unplug the phone while it flashes. (The scripts are plain text: open one in Notepad if you want to read it first. `-ExecutionPolicy Bypass` applies to that one command only.)

## Step 4: Check that it works
After the phone has booted:
```
powershell -ExecutionPolicy Bypass -File "C:\Users\<you>\Downloads\q25_out\C_verify.ps1"
```
It prints three lines. They must show:
* `20cf36063834ddb7100fb64bb5e53326` for `imgsensor_isp6s.ko` (the module),
* `0d70942a2e0e8e345ae96f4545c1c3a8` for `libremosaic_wrapper.so` (the shim),
* `d43db9ca21f305b927fc12905d802c770fb078e28f21476b3a1403a887fee5b9` for `libmtkcam_featurepolicy.so` (the patched camera library).

Then take a photo. Either use Lineage's own camera app at its highest resolution (saves 6120 x 8160, about 9 MB on my phone), or install **Open Camera**, set **Camera API** to **Camera2**, and choose picture size **8160 x 6144** (saves 6144 x 8160, about 11 MB). To confirm the 50 MP path ran, in PowerShell:
```
.\adb logcat -c
```
take one photo, then
```
.\adb shell "logcat -d | grep -i RemosaicShim"
```
You should see one `process: 8160x6144 ... done` line. The first photos take about 3 s, later ones about 2.6 s; the calibration is kept while the phone stays on (also when you close and reopen the camera app), but it starts again after every reboot, so the first photos are slower again. The settings are built in, so you do not need to set any `remoshim` properties. A normal-size photo (for example 3072 x 4096) should not show that line and should look normal. The log line `calibration: ... file NOT writable` is normal.

## Undo
```
powershell -ExecutionPolicy Bypass -File "C:\Users\<you>\Downloads\q25_out\UNDO.ps1"
```
It flashes your own original images back (they are in `q25_out\stock_undo`). It also works if the phone is already in fastbootd. On my phone it restored Lineage's original module, camera library and wrapper exactly (checked by hash). Keep the `q25_out` folder until you are happy with the result.

## Limits (read these)
* One phone and one build so far. Another Lineage build may be refused by the tool (that is the point of the checks) or may work; please report the `--check-only` output.
* **[VERIFY: not yet checked by me: the scripts on a second phone, and builds newer than the two I checked.]**
* Manual ISO or shutter modes and ISO above 3200 skip the 50 MP path. Photos are unsharpened by design; sharpen in post.
* Lineage's own camera app also takes 50 MP photos through the shim when you pick its highest resolution (on my phone it saves 6120 x 8160 files of about 9 MB).
* The patches contain small blocks of Zinwa/MediaTek vendor files; only patches and the tool are published, never images. See `LICENSES.md`.
