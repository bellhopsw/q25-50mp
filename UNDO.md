# Q25 50MP: undo guide (v0.1.0-alpha, DRAFT)

Work from the mildest option to the most drastic one. The `.\adb` and `.\fastboot` commands must be run in PowerShell **inside the platform-tools folder** (see "How to run the commands" in INSTALL.md). Steps marked **[VERIFY]** come from my notes and have not been followed by anyone else yet.

## 1. Remove the mod (the phone still boots, or you can reach fastbootd)
You need the stock images from Part 4 of INSTALL.md (`vendor_a.img` and `vendor_dlkm_a.img`, with the hashes listed there).
```
cd C:\platform-tools
.\adb reboot fastboot
.\fastboot flash vendor_dlkm C:\q25\work\out\vendor_dlkm_a.img
.\fastboot flash vendor C:\q25\work\out\vendor_a.img
.\fastboot reboot
```
If the phone does not boot far enough for `adb`, open the boot menu: with the phone switched off, hold **Volume Up + Power** until the boot menu text appears, then choose **fastboot** **[VERIFY]** (this is the key combination listed on the LineageOS wiki page for the Q25; confirm it on your stock firmware). Then run `.\fastboot reboot fastboot` and continue from the first flash command.

## 2. Turn verified boot back on **[VERIFY]**
Only after step 1 and a successful boot into stock:
```
cd C:\platform-tools
.\adb reboot bootloader
.\fastboot flash vbmeta "$FW\vbmeta.img"
.\fastboot reboot
```
This is the stock `vbmeta.img` **without** the disable flags. (`$FW` is the firmware folder from Part 4 of INSTALL.md; set it again in this window if needed.) If the phone does not boot afterwards, flash it again with `--disable-verity --disable-verification` (as in INSTALL.md) and ask for help.

## 3. Re-locking the bootloader **[VERIFY]**
Optional, and the riskiest step. Leave the bootloader unlocked unless you have a reason to lock it. (Locking is the only way to get rid of the orange warning screen at boot.) Only lock it when **every** partition is stock and the phone boots normally. Locking erases the phone's data.

## 4. The phone is stuck or bricked: full re-flash with SP Flash Tool
This follows Zinwa's own "Q25 Flash OS Tutorial". **It erases all data on the phone.**
1. Prepare as in INSTALL.md Part 1 (driver, SP Flash Tool V6, a USB-A to USB-C data cable on a USB-A port) and extract the official firmware package.
2. Switch the phone **completely off** (required).
3. Start `SPFlashToolV6.exe`. On the **Download** tab, next to "Download-XML", click **choose** and select `flash.xml` in the `download_agent` folder of the firmware package.
4. Make sure the mode drop-down above the file list says **Download Only**. **Never** use Format All.
5. Click **Download**, then connect the cable. A status message appears in the bottom bar (if nothing shows for a long time, re-plug the cable). Wait for the completion pop-up.
6. Unplug the cable, then press and hold the power button to switch the phone on. It starts as a freshly flashed stock phone. *(Whether the bootloader stays unlocked afterwards is not verified here.)* **[VERIFY]**
7. If the IMEI, Wi-Fi or Bluetooth are missing afterwards, the calibration partitions need restoring from the backup you made in INSTALL.md Part 1. That is advanced; ask for help before attempting it and keep the backup files safe.

## Keep these until you are sure
The firmware zip, the Part 1 backups, and `vendor_a.img` / `vendor_dlkm_a.img` with their checksums.
