# Changelog

What's new in ps5-yt-uploader, written for humans.

---

## Unreleased

Planned improvements and upcoming fixes for subsequent releases.

---

## 1.0.0

Initial public release of the PS5 YouTube Uploader (`ps5-yt-uploader`) for jailbroken PlayStation 5 consoles.

**New**
- **Autonomous gameplay uploads directly from PS5 SSD to YouTube.** Watches `/user/av_content/video/` for new clips saved via the DualSense "Create" button, streaming MP4/WebM video captures directly to personal YouTube channels using YouTube Studio's internal Scotty resumable upload protocol.
- **Dedicated Media home screen launcher tile.** Injects an official tile directly into the PS5 home screen **Media** category (via embedded `tbl_appbrowse` MMS database patch) that launches a fullscreen 1080p WebKit dashboard navigable via DualSense controller.
- **Local Web Companion on port 8888.** Provides a responsive web dashboard accessible over local Wi-Fi from any phone, tablet, or PC (`http://<ps5-ip>:8888`), featuring a dynamic QR code for instant 10-second credential pairing.
- **1-Click PS5 Payload Manager integration.** Provides a live, specification-compliant custom payload repository (`payloads.json`) for `itsPLK/ps5-payload-manager`, supporting direct installation into `/data/payloads/` and automated startup autoloading.
- **Native PS5 system notifications.** Pops up on-screen toast notifications (`sceNotificationRequest`) when an upload begins, succeeds (including short `youtu.be` link), or encounters network interruptions.
- **Zero Google Cloud accounts and zero quota limits.** Authenticates via standard YouTube Studio session tokens, completely eliminating Google Cloud API keys, project registration, credit card requirements, and the 10,000-unit daily API quota cap.

**Behind the scenes**
- **Clean-room C implementation.** 100% open-source C99 codebase targeting FreeBSD 12 ELF PIE executables with zero proprietary Sony SDK libraries, leaked decryption keys, or DRM circumvention.
- **Ultra-lean daemon footprint.** Stripped ELF binary compiles at ~154 KB with idle RAM usage under 4 MB and CPU utilization under 0.1%, ensuring zero performance impact on 60/120 FPS gaming.
- **Resilient resumable chunking.** Network disconnections during upload automatically query Scotty byte offsets and resume streaming from the exact interruption byte without restarting.
- **Backward-compatible payload aliasing.** Automated Makefile targets and release assets provide both `ps5-yt-uploader.elf` and `ps5-youtube.elf` symlinks for compatibility with existing homebrew launcher scripts.
