# Q25 50MP: FAQ (v0.1.0-alpha, DRAFT)

**My photo is only 12.5MP. Why?**
Check, in order: Open Camera is using the **Camera2 API** and its photo resolution is **8160 x 6144**; you are not in manual ISO/shutter mode (those skip the 50MP path); the scene is not very dark (above ISO 3200 the camera falls back to its normal processing, which is probably an upscaled 12.5MP frame); and you are not using the Zinwa camera app (it keeps the normal binned path).

**How can I tell a photo went through the 50MP path?**
`.\adb logcat -d -v time | findstr /C:"RemosaicShim"` (run in the platform-tools folder) shows one `process: 8160x6144 ... done in ... ms` line per 50MP photo. Clear the log first (`.\adb logcat -c`) and take the photo afterwards.

**Why are the first photos slower?**
The shim calibrates itself over roughly the first 8 photos (about 3 s each, then about 2.3 s). The calibration survives camera restarts.

**Why do the photos look soft?**
50MP files are clean but unsharpened by design. They are best for cropping and well-lit scenes. Sharpen in post (an unsharp mask of radius about 3 px), or try Open Camera's edge mode "High quality" (adds some grain). In dim light the normal 12.5MP mode often looks punchier. See the write-up for comparisons.

**Does it work on another firmware version or phone?**
No. The patches only apply to the exact stock images listed in the install guide, and the tools refuse anything else.

**What about OTA updates?**
An official update would overwrite or reject the modified partitions. Do not install OTAs; use UNDO.md first.

**Can I shoot RAW?**
Not with this release.

**Do banking or other apps care?**
An unlocked bootloader and disabled verified boot can make some apps refuse to run. This has not been tested.

**The phone shows an orange "Orange State" screen when it starts. Is something wrong?**
No. It appears because the bootloader is unlocked (you unlocked it in the install guide) and it is shown on every boot. It does not mean the mod failed. Locking the bootloader again removes it but erases the phone; see UNDO.md before you consider that.

**Open Camera does not offer 8160 x 6144.**
Turn on the Camera2 API in Open Camera's settings and restart the app. If the size is still missing, check the log (see "How can I tell a photo went through the 50MP path?") and make sure the install steps all finished.

**Which firmware do I need?**
`OS-new-camera-0120-Q25-GMS` (the With-GMS build, firmware id `Q25_20.01.2026`) from Zinwa's shared Drive folder. The tools refuse any other build.

**Can I use the kernel change in my own tree?**
Yes. It is a single commit on LineageOS `lineage-23.2`. See "Cherry-pick the
driver change" in `src/kernel/README.md`.
