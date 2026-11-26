# PS5 YouTube Uploader (`ps5-yt-uploader`)

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Release](https://img.shields.io/badge/release-v1.0.5-blue.svg)](https://github.com/temp-latif07/ps5-yt-uploader/releases)
[![Platform](https://img.shields.io/badge/platform-PlayStation%205-003791.svg)]()
[![Target Firmware](https://img.shields.io/badge/firmware-12.20%20%7C%20y2JB%20Relapse-792ee5.svg)]()
[![Payload Manager](https://img.shields.io/badge/Payload%20Manager-Compatible-2ea44f.svg)](payloads.json)

**PS5 YouTube Uploader** (`ps5-yt-uploader`) is an autonomous, direct console-to-YouTube video uploader for jailbroken PlayStation 5 consoles running firmware 12.20 on the **y2JB Relapse** exploit chain.

On jailbroken PS5 setups, PlayStation Network (PSN) telemetry and communication are blocked via `nanoDNS` to prevent forced firmware updates, rendering the native DualSense "Share" dialog inoperative (*"You're not signed in"*). **PS5 YouTube Uploader** restores seamless gameplay video publishing straight from console internal SSD to your YouTube channel—without external PCs, USB drives, Google Cloud developer accounts, API key configurations, or quota limits.

---

## Architecture Overview

```
+---------------------------------------------------------------------------------------+
|                       PS5 Console (y2JB Relapse Homebrew)                             |
|                                                                                       |
|   +--------------------------+                 +----------------------------------+   |
|   | Internal SSD Storage     |                 | Home Screen "Media" Category     |   |
|   | /user/av_content/video/  |                 | Launcher Tile (Fullscreen 1080p) |   |
|   +-------------+------------+                 +-----------------+----------------+   |
|                 |                                                |                    |
|                 v (inotify / polling watcher)                    v                    |
|   +-------------+------------------------------------------------+----------------+   |
|   |                   ps5-yt-uploader.elf (Single Unified Daemon)                 |   |
|   |                                                                               |   |
|   |  - Background Capture Watcher       - YouTube Studio Scotty Upload Engine     |   |
|   |  - Native PS5 Toast Notifications   - Embedded Web Companion Server (:8888)   |   |
|   |  - DualSense TV UI & Curation       - Session & Config Store (/data/...)      |   |
|   +-------------+------------------------------------------------+----------------+   |
|                 | (Scotty Resumable Upload)                      |                    |
+-----------------|------------------------------------------------|--------------------+
                  |                                                |
                  v                                                v
    +-----------------------------+                 +-------------------------------+
    |    YouTube Upload Server    |                 |   Local Wi-Fi Companion UI    |
    | (https://upload.youtube.com)|                 |  http://<ps5-ip>:8888 / QR    |
    |  Direct Chunked Ingestion   |                 | (Phone / PC Pairing & Studio) |
    +-----------------------------+                 +-------------------------------+
```

---

## Core Features

- **Direct Scotty Resumable Upload Protocol:** Video captures stream directly from the console's internal SSD (`/user/av_content/video/`) to YouTube Studio ingestion servers using the resilient Scotty chunked upload protocol—bypassing Google Cloud API quotas (avoiding the standard 10,000 units/day ceiling) with zero GCP billing or credit card requirements.
- **Manual Clip Selection & Curation First:** Automatic uploading is disabled by default to keep you in total control. Select any recorded clip, customize Title, Description, Privacy (*Public*, *Unlisted*, or *Private*), Audience (*Made for Kids* COPPA compliance), and optional Playlist ID before initiating upload.
- **Dual Operating Modes:**
  - **Mode A (Background Daemon):** Runs silently in the background on your PS5 via Payload Manager; accessible directly from your phone, tablet, or PC browser on local Wi-Fi (`http://<ps5-ip>:8888`).
  - **Mode B (Homescreen Shortcut):** Direct TV launch via an offline WebKit AppCache shortcut package (`param.json` + `icon0.png`), modeled after `itsPLK/ps5-webkit-autoloader`.
- **Native PS5 System Notifications:** Fires authentic system on-screen notification toasts via `sceKernelSendNotificationRequest` displaying real-time upload progress, completion confirmations with shortened video URLs (`youtu.be/xxx`), or actionable error alerts.
- **Web Companion Pairing Dashboard (Port 8888):** Embedded lightweight HTTP server serving a responsive dashboard on port 8888. Displays a dynamic QR code for instant 10-second pairing with any smartphone or desktop browser on the local network.
- **Ultra-Lightweight Footprint:** Single self-contained C binary (~161 KB stripped ELF) consuming < 4 MB RAM and < 0.1% background CPU, guaranteeing zero frame drops during 60/120 FPS gaming sessions.

---

## Prerequisites

- **PlayStation 5 Console:** Disc, Digital, Slim, or Pro.
- **Firmware:** 12.20 (compatible with 7.00–13.60 kernel exploit ranges).
- **Exploit Environment:** y2JB with Relapse kernel exploit chain.
- **Active Environment Payloads:** `kstuff-lite`, `shadowmount`, `nanoDNS` (blocking PSN domains while permitting outgoing internet access).
- **Network:** PS5 connected to local Wi-Fi or Ethernet with active internet connectivity to Google/YouTube endpoints.

---

## Installation

### Method 1: Via PS5 Payload Manager (Recommended)

The easiest and cleanest installation method is using `itsPLK/ps5-payload-manager` with our official custom repository catalog.

1. Open **Payload Manager** on your PS5 (via WebKit dashboard or home screen shortcut).
2. Navigate to **Settings** ➔ **Manage Sources** ➔ **Add Source**.
3. Enter the official repository URL:
   ```text
   https://raw.githubusercontent.com/temp-latif07/ps5-yt-uploader/main/payloads.json
   ```
4. Select **Add**. The repository will refresh and list **PS5 YouTube Uploader** under the **Utilities** category.
5. Select **PS5 YouTube Uploader** and choose **Install**.
6. *(Optional but recommended)* Toggle **Autoload** to automatically run the uploader daemon every time your PS5 boots or payloads are loaded.

Payload Manager will automatically download `ps5-yt-uploader.elf`, verify its SHA-256 checksum, place it in `/data/payloads/ps5-yt-uploader.elf`, and launch it.

---

### Method 2: Manual ELF Loading via Netcat

If you prefer launching payloads manually using standard ELF loaders (listening on port `9021`):

1. Download the latest `ps5-yt-uploader.elf` from the [GitHub Releases](https://github.com/temp-latif07/ps5-yt-uploader/releases) page.
2. Ensure your PS5 homebrew ELF loader is running on port `9021`.
3. Send the ELF binary to your console from your computer terminal:
   ```bash
   nc -w 3 <ps5-ip> 9021 < bin/ps5-yt-uploader.elf
   ```
   *(Replace `<ps5-ip>` with the actual local IP address of your PS5, e.g., `192.168.1.150`.)*
4. Check your TV screen: you will receive a notification toast indicating that PS5 YouTube Uploader is running and listening on port `8888`.

---

## Web Companion Pairing & Usage Guide

Once the daemon is running, pair your YouTube account in seconds:

1. **Access the Companion Dashboard:**
   - On your smartphone, tablet, or computer connected to the same local Wi-Fi/LAN, navigate to:
     ```text
     http://<ps5-ip>:8888
     ```
   - Alternatively, launch the **YouTube Uploader** home screen shortcut on your PS5 to view the dashboard directly on your TV.
2. **10-Second Account Pairing:**
   - Tap **Sign in with YouTube / Google**.
   - Authenticate in your trusted browser. The authentication token is exchanged and securely stored locally on the console's internal storage (`/data/youtube_uploader/config.json`).
   - No Google Cloud Console project, OAuth client secrets, or billing accounts required.
3. **Manual Clip Curation & Upload Workflow:**
   - Automatic uploading is **disabled by default**, giving you complete control over what gets published.
   - In the Web Companion Studio, browse your console's recorded gameplay clips with live thumbnail previews.
   - Click or tap any clip to open the **Upload Curation Modal**:
     - **Title:** Enter custom video title (defaults to original filename).
     - **Description:** Provide gameplay description, hashtags, or credits.
     - **Privacy:** Select *Public*, *Unlisted*, or *Private*.
     - **Audience (COPPA):** Choose whether the video is Made for Kids (*No, it's not made for kids* or *Yes, it's made for kids*).
     - **Playlist ID (Optional):** Specify a YouTube Playlist ID (e.g. `PLxxxxxxxx`) to automatically categorize your upload.
   - Click **Upload Now** to initiate direct Scotty resumable upload.
4. **Autonomous Mode (Optional):**
   - If you prefer hands-free uploading, you can toggle **Automatic Upload** ON in the Settings tab. The background watcher will then automatically detect and upload newly saved clips.
5. **On-Screen Confirmation:**
   - Watch live byte-level upload progress on the dashboard.
   - Receive a native PS5 notification toast on the top-right of your TV screen with the shortened `youtu.be/<id>` link upon completion.

---

## Homescreen Shortcut Installation (The itsPLK Model)

ELF payloads run headless in the background. To create a dedicated home screen shortcut tile on your PS5 dashboard:

1. Generate the shortcut package:
   ```bash
   make shortcut
   # or: python3 scripts/generate_shortcut.py --output dist/shortcut/PS5YT0001
   ```
2. Copy the resulting `dist/shortcut/PS5YT0001` folder to your PS5 at:
   ```text
   /user/app/PS5YT0001/
   ```
   *(Transfer via FTP payload or use the WebKit AppCache shortcut installer).*
3. The PS5 ShellUI registers the tile with Title **YouTube Uploader** and custom icon `icon0.png`, launching directly to `http://127.0.0.1:8888`.

---

## CLI Flags and Options

For testing, host simulation, or custom automation, `ps5-yt-uploader` supports command-line parameters:

```text
ps5-yt-uploader v1.0.5 (PS5 YouTube Uploader)
Usage: ps5-youtube [options]

Options:
  -v, --version        Show version information
  --port <p>           Port to listen on (default: 8888)
  --data-dir <dir>     Persistent storage directory (default: /data/youtube_uploader)
  --captures-dir <dir> Video captures path (default: /user/av_content/video)
  --app-db <path>      MMS database path (default: /system_data/priv/mms/app.db)
  --no-tile            Skip media tile registration
  -h, --help           Show this help message
```

---

## Building from Source

### Host Environment Build & Test Suite

Build the host development version and execute the automated test suite (macOS or Linux):

```bash
# Compile and run complete unit & integration test suite
make test

# Build host standalone binary
make
```

### Cross-Compiling for PS5 (FreeBSD 12 ELF)

Cross-compile the production ELF binary targeting the PS5 FreeBSD 12 kernel environment:

```bash
# Cross-compiles to bin/ps5-yt-uploader.elf and strips symbols
make ps5
```

*Requirements for cross-compilation:* Clang with `-target x86_64-unknown-freebsd12` and LLVM linker (`ld.lld`).

---

## Legal & TOS Compliance Notice

- **Clean-Room Implementation:** This project is a 100% clean-room C implementation utilizing standard POSIX interfaces, BSD sockets, SQLite3, and cJSON.
- **Zero Proprietary Sony IP:** This software contains no proprietary Sony Interactive Entertainment (SIE) SDK code, static libraries, leaked source, encryption keys, or DRM bypasses.
- **No Reverse Engineering of Proprietary Software:** System interactions use standard open-source homebrew techniques (FreeBSD system calls, unprivileged WebKit APIs, and public database formats).
- **Independent Utility:** This project is an independent homebrew development effort and is **not affiliated with, authorized, maintained, or endorsed by Sony Interactive Entertainment Inc. or Google LLC / YouTube**.
- **YouTube Terms of Service Compliance:** This utility is designed strictly for personal gameplay video backup and archival in compliance with the YouTube Terms of Service and Community Guidelines. Users remain solely responsible for the content, licensing, and copyright compliance of media uploaded through this tool.

---

## License

This project is licensed under the **MIT License**. See the [LICENSE](LICENSE) file for complete details.

Copyright (c) 2026 Latif Muhammad.
