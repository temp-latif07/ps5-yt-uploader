# Changelog

What's new in ps5-yt-uploader, written for humans.

---

## Unreleased

Planned improvements and upcoming fixes.

---

## 2.1.0

**New**
- **Full-Viewport PS5 Media Gallery UI:** Redesigned the dashboard into a dark edge-to-edge Media Gallery grid with lazy 16:9 video thumbnails and durations, game filter dropdown, upload-status filter (`All` / `Not uploaded` / `Uploaded`), controller-friendly focus highlights, a live active-upload banner, and a unified side-by-side **Clip Viewer & Upload** studio modal.
- **Real PS5 Game Titles:** Reads PS5 `.dat` sidecar headers (and `/user/appmeta/<TitleID>/param.json` fallback) to display actual game names instead of raw Title IDs.
- **Live YouTube Playlists Dropdown:** Fetches your channel's playlists (`GET /api/playlists`) with a one-click refresh button so you can add clips to an existing playlist without memorizing playlist IDs.
- **Sticky Last-Used Upload Settings:** Automatically remembers your last-used Privacy (`Public` / `Unlisted` / `Private`), Made for Kids selection, and Playlist ID across clips and console restarts.
- **Self-Registering `.elf` Home-Screen Tile + `.exfat` Tile:** Loading `ps5-yt-uploader.elf` directly (e.g. via PS5 Payload Manager) now stages `/user/app/YTUP08085` and registers a home-screen shortcut automatically via `libSceAppInstUtil`, alongside the standalone `PPSA97631` `.exfat` launcher image.
- **Share QR & Copy Link:** Completed uploads now include **Copy link** and **Share QR** buttons to open `youtu.be/<id>` on your phone.

---

## 2.0.2

**Fixes**
- **Correct PS5 Captures Path (`/user/av_contents/video`):** Updated capture auto-detection and allowed paths to use `/user/av_contents/video` (with automatic migration away from the old `/user/av_content/video` setting and fallback to `/user/av_contents` or `/data`).
- **Recursive Clip Scanning & Smart Title IDs:** Recursively scans up to 5 levels deep under captures folders so nested layouts like `/user/av_contents/video/NPXS40087/<TitleID>/...` are discovered cleanly while extracting the game Title ID instead of system folder `NPXS40087`.
- **Resilient Folder Picker:** Automatically falls back through all available storage roots (`/user/av_contents/video`, `/data`, USB drives) when browsing instead of stalling on a missing folder.

---

## 2.0.1

**Fixes**
- **Background Device Flow Polling & Tab/Device Reuse:** The console server now polls Google's OAuth device-code endpoint on a background thread and reuses the active device code across tab reloads and second devices (such as a phone + TV browser), so returning after Google shows "Success! Device connected" completes sign-in reliably even when mobile browsers pause background JavaScript timers.
- **Instant Sign-In Sync & Manual Refresh:** Added focus/visibility resume checks, cross-device setup status polling, clear error surfacing via `/api/status`, and a **Check now** button on the device sign-in screen.

---

## 2.0.0

Ground-up rebuild modeled on the `torbox-ps5` architecture for rock-solid reliability on the PS5 home screen, in the PS5 browser, and on phones/PCs.

**New**
- **Dedicated Home-Screen Launcher Tile (`PPSA97631`):** Installs under `/data/homebrew/PPSA97631/` via ShadowMountPlus. Opening it starts the background payload via `elfldr` (port `9021`), opens the PS5 browser on `http://127.0.0.1:8085`, and exits cleanly so uploads continue while you play games.
- **BYO Google Cloud OAuth + QR Device Sign-In:** Import your own Google Cloud *TVs and Limited Input devices* Client ID (via `client_secret_*.json` upload or manual entry) and sign in on any device by scanning the on-screen `google.com/device` QR code — zero browser cookie extraction or helper scripts required.
- **Official YouTube Data API v3 Resumable Uploads:** Streams captures directly from disk via Google's `308 Resume Incomplete` resumable upload protocol with title, description, privacy, Made for Kids audience status, and optional playlist assignment.
- **Built-in Phone QR Code:** Scannable QR code in the setup view and header to open the dashboard on your phone for uploading `client_secret_*.json`, curating metadata, previewing clips, and managing the queue.
- **In-Browser Clip Preview:** Stream saved `.mp4` and `.webm` gameplay captures with full HTTP `Range` seeking right inside the web UI before uploading.
- **Folder & USB Drive Browser:** Browse `/user/av_content/video`, `/data`, and mounted USB drives (`/mnt/usbN`, `/mnt/extN`).
- **Safe Off-by-Default Auto-Upload:** Optional toggle in Settings records a timestamp watermark and ignores pre-existing captures, uploading only brand-new clips saved after you turn it on.

**Fixes**
- **Reliable Static `libcurl` + mbedTLS Networking:** Replaced Sony `libSceHttp`/`libSceSsl` calls with static `libcurl` + embedded CA certificates, streaming directly from disk with zero 8 MiB chunk RAM buffers, live transfer speed/ETA, silent OAuth token refresh, and fast pause/cancel.
- **PS5 Socket & Thread Stability:** Guarded all HTTP server `accept()`/`recv()`/`send()` calls with `poll()`, enforced 1 MiB thread stacks, and added CSRF protection (`X-PS5YT`) and strict path/extension checks on video streaming.
- **Resilient Recovery & History Tracking:** Persists resumable upload session URLs across console restarts, re-initializes expired sessions (`410`) cleanly, handles daily API quota and `invalid_grant` errors clearly, and records completed `video_id` history before optional playlist assignment so partial failures never create duplicate uploads.

**Behind the scenes**
- **Reproducible Build & Docker Cross-Toolchain:** Single `./build.sh` script (`host | payload | title | package | release | all`) with `docker/Dockerfile.ps5` and comprehensive host E2E tests against `tests/mock_youtube.py`.
- **Reference Archive:** Preserved the original implementation under `archive/`.
