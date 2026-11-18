# Changelog

What's new in ps5-yt-uploader, written for humans.

---

## Unreleased

Planned improvements and upcoming fixes for subsequent releases.

---

## 1.0.1

Release and repository maintenance overhaul adopting the disciplined engineering standards of `phantomptr/ps5upload`.

**New**
- **Instant local release execution (`make release`).** Cut, verify, and publish official releases with binaries, checksums, and release notes in under 5 seconds directly from the developer machine via `gh release create`, completely bypassing slow cloud CI queues.
- **Human-readable release notes extractor.** Automated `scripts/extract_release_notes.py` extracts release notes directly from `CHANGELOG.md` for any version tag, ensuring release notes are written for humans who use the console rather than raw git commit dumps.
- **Repository maintenance standards (`AGENTS.md`).** Added comprehensive engineering and release rules adapted from `phantomptr/ps5upload` to guide autonomous agents and contributors.
- **Dynamic test assertions.** Updated test suites to dynamically assert against `src/version.h` macros, ensuring seamless version bumps without broken test assertions.

**Fixes**
- **Dry-run release safety on tagged branches.** Fixed an issue where `scripts/release.sh --dry-run` failed if the version tag already existed locally.
- **Non-interactive safety enforcement.** Explicitly mandated `-y` or `--yes` flags when running `scripts/release.sh` without a TTY to prevent accidental automated tagging.
- **Payload artifact renaming.** Standardized primary ELF binary to `ps5-yt-uploader.elf` across the build system, catalog manifest, test suites, and documentation, while retaining `bin/ps5-youtube.elf` as a backward-compatible symlink.

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
