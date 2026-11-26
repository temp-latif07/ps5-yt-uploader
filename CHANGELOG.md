# Changelog

What's new in ps5-yt-uploader, written for humans.

---

## Unreleased

Planned improvements and upcoming fixes.

---

## 2.0.0

Ground-up rebuild modeled on the `torbox-ps5` architecture for rock-solid reliability on the PS5 home screen, in the PS5 browser, and on phones/PCs.

**New**
- **Dedicated Home-Screen Launcher Tile (`PPSA97631`):** Installs under `/data/homebrew/PPSA97631/` via ShadowMountPlus. Opening it starts the background payload via `elfldr` (port `9021`), opens the PS5 browser on `http://127.0.0.1:8888`, and exits cleanly so uploads continue while you play games.
- **One-Time Isolated Login Helper (`tools/yt-login.py`):** Opens a temporary isolated browser window to YouTube Studio, saves your cookies and selected channel metadata to the PS5 (`--choose-channel` supported for Brand Accounts), and deletes the temporary profile so daily browsing does not rotate or break your session.
- **Built-in Phone QR Code:** Scannable QR code in the TV UI (on the login screen and anytime from the header) to open the dashboard on your phone for easy cookie upload, metadata typing, previewing, and queue management.
- **In-Browser Clip Preview:** Stream saved `.mp4` and `.webm` gameplay captures with full HTTP `Range` seeking right inside the web UI before uploading.
- **Folder & USB Drive Browser:** Browse `/user/av_content/video`, `/data`, and mounted USB drives (`/mnt/usbN`, `/mnt/extN`).
- **Safe Off-by-Default Auto-Upload:** Optional toggle in Settings records a timestamp watermark and ignores pre-existing captures, uploading only brand-new clips saved after you turn it on.

**Fixes**
- **Reliable Static `libcurl` + mbedTLS Networking:** Replaced Sony `libSceHttp`/`libSceSsl` calls with static `libcurl` + embedded CA certificates, streaming directly from disk with zero 8 MiB chunk RAM buffers, live transfer speed/ETA, and fast pause/cancel.
- **PS5 Socket & Thread Stability:** Guarded all HTTP server `accept()`/`recv()`/`send()` calls with `poll()`, enforced 1 MiB thread stacks, and added CSRF protection (`X-PS5YT`) and strict path/extension checks on video streaming.
- **YouTube Studio `WEB_CREATOR` Flow:** Automatically extracts live Studio API keys and session metadata, persists `Set-Cookie` rotations via an atomic cookie jar, supports two-step `createvideo` + `metadata_update` (title, description, privacy, Made for Kids, playlist), and handles expired Scotty URLs (`410`) or partial metadata failures without creating duplicate uploads.

**Behind the scenes**
- **Reproducible Build & Docker Cross-Toolchain:** Single `./build.sh` script (`host | payload | title | package | release | all`) with `docker/Dockerfile.ps5` and comprehensive host E2E tests against `tests/mock_youtube.py`.
- **Reference Archive:** Preserved the original implementation under `archive/`.
