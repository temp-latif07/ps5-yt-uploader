# PS5 YouTube Uploader Design Document

## 1. Intent and The Why
On retail PlayStation 5 consoles, uploading gameplay clips to YouTube relies on the DualSense "Create" button, the system Media Gallery, and active PlayStation Network (PSN) authentication. On a jailbroken PS5 (firmware 12.20 utilizing the y2JB Relapse exploit chain), the console is deliberately disconnected and blocked from PSN (via `nanoDNS`) to prevent telemetry and forced system updates. Consequently, invoking the stock share option fails with *"You're not signed in"*.

Currently, users must manually copy video clips to an external PC (via FTP, web file managers, or USB) and manually upload them to YouTube. 

The goal of this project is to build an open-source, production-ready, native solution for the jailbroken PS5 community that restores the ability to upload gameplay captures directly from the console's internal SSD to a user's personal YouTube channel, feeling as close to the native stock experience as possible while requiring zero intermediate PC transfers.

---

## 2. Goals, Non-Goals, and Anti-Goals

### Goals
- **Direct Console-to-YouTube Uploads:** Stream video captures directly from the PS5 internal storage (`/user/av_content/video/`) to YouTube over the console's network interface.
- **User-Specific Account Binding:** Authenticate users directly to their own YouTube accounts using the Google OAuth 2.0 Device Authorization Grant (RFC 8628 - `google.com/device`).
- **Phased MVP Delivery:**
  - **Phase 1 (MVP):** Fork [StonedModder/PShare](https://github.com/StonedModder/PShare) to introduce a native YouTube destination with automated detection and on-screen PS5 notification toasts.
  - **Phase 2 (Stock Feel):** Add a dedicated launcher tile in the PS5 home screen's **Media** category (using the `browser_patch` technique from `owendswang/ps5-web-file-manager`) that opens in 1080p fullscreen.
  - **Phase 3 (Companion & Polish):** Deliver a responsive local web companion accessible from any phone or PC on the same Wi-Fi network for clip management, custom metadata editing (title, description, tags, privacy), and upload queue status.
- **Production Readiness & Community Distribution:** Provide robust error handling, resumable upload recovery (handling transient Wi-Fi drops), token refresh persistence in `/data/`, and clean configuration management.

### Non-Goals
- **In-Game Overlay Injection:** No direct hooking of Sony's closed-source `shellcore` Share dialog in memory while games are rendering (which risks game crashes, memory corruption, and firmware instability).
- **Video Re-encoding / Transcoding on Console:** Video clips recorded by the PS5 are already properly encoded H.264/HEVC MP4 files. The uploader will not transcode or alter video streams on-device.
- **Bypassing YouTube Platform Limits:** The tool will not circumvent YouTube's standard platform policies (e.g., maximum daily upload limits for new accounts, copyright ID matching).

### Anti-Goals
- **No Shared Monolithic Quota Bottlenecks:** The solution must not hardcode a single shared public API key that gets exhausted immediately by community usage. It must support user-supplied API credentials or verified project scopes.
- **No Heavy Background Overhead:** The daemon must consume negligible CPU and RAM when idle, avoiding interference with 60/120 FPS gameplay.
- **No Intermediary Cloud Relays:** Media bytes must never pass through third-party servers; uploads route strictly from `PS5 -> Google API`.

---

## 3. Constraints & Target Environment
- **Target Platform:** PlayStation 5 (Fat / Slim / Pro).
- **Firmware:** 12.20 (and compatible with 7.00 - 13.60 kernel exploit range).
- **Exploit & Environment:** y2JB (YouTube app exploit) running the Relapse kernel exploit chain.
- **Active System Payloads:** `kstuff-lite`, `shadowmount`, `ps5upload`, `nanoDNS`, `ps5-web-file-manager`.
- **Port Environment:** Homebrew ELF loader listening on port `9021`.
- **Filesystem Captures Path:** `/user/av_content/video/<TitleID>/` containing `.mp4` and `.webm` captures.
- **Persistent Data Storage:** `/data/` on internal console storage.
- **Network Constraints:** PSN domains (`*.playstation.net`, `auth.api.playstation.com`) blocked by `nanoDNS`. Public internet and Google endpoints (`accounts.google.com`, `www.googleapis.com`) are fully reachable.

---

## 4. Approaches Considered

### Approach 1: Dedicated Standalone C Daemon with Media Tile & Web Companion
- **Description:** Build a custom C payload from scratch using `PS5_PAYLOAD_SDK` that registers a Media tab tile, runs a local embedded web server, and handles YouTube resumable uploads.
- **Pros:** Minimal binary size (~500 KB), complete control over architecture.
- **Cons:** Reinventing filesystem watchers, game ID mapping, and capture status tracking from scratch.

### Approach 2: Fork / Extend PShare (Selected Foundation)
- **Description:** Fork `StonedModder/PShare`, an established open-source C++ homebrew project already running a daemon (`pshared`) that monitors `/user/av_content/` and dispatches media to Discord, Telegram, and file hosts.
- **Pros:** Proven stability on jailbroken PS5, robust capture detection, existing notification system, battle-tested filesystem polling.
- **Cons:** Larger existing codebase with unused dependencies (e.g., SMB/FFmpeg).

### Approach 3: Strictly Headless Daemon (Phone-Only Interface)
- **Description:** A purely background daemon without any PS5 UI elements, operated exclusively via mobile/PC browser.
- **Pros:** Simplest implementation, zero UI risk.
- **Cons:** Fails to provide the requested "stock feel" on the TV screen.

### Chosen Approach: Phased MVP Roadmap
The user selected a phased MVP approach combining the best elements:
1. **Phase 1 (MVP):** Fork PShare to add the YouTube destination and authentication engine.
2. **Phase 2:** Implement the dedicated PS5 Media category tile for on-console launching.
3. **Phase 3:** Polish the mobile/PC companion web controller for rich metadata editing.

---

## 5. Architectural Specification & Roadmap

```
                                  +---------------------------------------+
                                  |     PS5 DualSense "Create" Button     |
                                  +---------------------------------------+
                                                      |
                                           (Captures gameplay clip)
                                                      v
                                  +---------------------------------------+
                                  |     /user/av_content/video/<GameID>/  |
                                  +---------------------------------------+
                                                      |
                                            (inotify / stat poll)
                                                      v
  +-----------------------------------------------------------------------------------------------+
  | PS5 Background Daemon (pshared fork)                                                          |
  |                                                                                               |
  |  +---------------------------+    +----------------------------+    +-----------------------+ |
  |  |   Capture Watcher Engine  |--->|    YouTube Upload Engine   |--->|   PS5 Notification    | |
  |  |  (Detects & verifies file |    |  - OAuth Device Flow       |    |  (sceNotification     | |
  |  |   write completion)       |    |  - 8MB Resumable Chunks    |    |   On-Screen Toasts)   | |
  |  +---------------------------+    +----------------------------+    +-----------------------+ |
  |                                                 ^                                             |
  |                                                 | (Config / Auth Token)                       |
  |                                                 v                                             |
  |                                   +----------------------------+                              |
  |                                   |  /data/pshare/config.json  |                              |
  |                                   +----------------------------+                              |
  |                                                 ^                                             |
  |                                                 |                                             |
  |  +----------------------------------------------v------------------------------------------+  |
  |  | Embedded Web Companion Server (port 8888 / custom port)                                  |  |
  |  +-----------------------------------------------------------------------------------------+  |
  +-----------------------------------------------------------------------------------------------+
           |                                                                   |
           v                                                                   v
+-----------------------------+                                     +-----------------------------+
| PS5 Media Home Screen Tile  |                                     | Mobile / PC Browser         |
| (Fullscreen WebKit Browser) |                                     | (http://<ps5-ip>:8888)      |
+-----------------------------+                                     +-----------------------------+
```

### Phase 1: MVP - PShare Fork & YouTube Engine
1. **Google OAuth 2.0 Device Flow (RFC 8628):**
   - Daemon requests device authorization from `https://oauth2.googleapis.com/device/code` with scope `https://www.googleapis.com/auth/youtube.upload`.
   - Returns `verification_url` (`https://www.google.com/device`) and `user_code` (e.g., `ABCD-EFGH`).
   - Console logs / notifies user to approve on phone or PC.
   - Daemon polls `https://oauth2.googleapis.com/token` until granted, acquiring `access_token` and `refresh_token`.
   - Tokens and client credentials stored encrypted/securely in `/data/pshare/youtube.json`.
2. **Resumable Upload Implementation:**
   - POST to `https://www.googleapis.com/upload/youtube/v3/videos?uploadType=resumable&part=snippet,status`.
   - Payload specifies title (`[Game Name] - YYYY-MM-DD HH:MM`), description, privacy status (`unlisted` by default).
   - Extracts upload session URI from `Location` header.
   - Streams file in 8 MB chunks via HTTP `PUT` with `Content-Range`.
   - Handles network interruptions with HTTP `308 Resume Incomplete` range queries.
3. **System Notifications:**
   - On upload start: *"YouTube: Uploading clip..."*
   - On upload complete: *"YouTube: Clip uploaded successfully (youtu.be/xxx)"*
   - On failure: *"YouTube: Upload failed - [reason]"*

### Phase 2: Dedicated Home Screen Media Tile
1. **Media Tile Registration (`browser_patch`):**
   - Utilize memory patching of the PS5 shellcore/app database to register a dedicated tile in the **Media** tab.
   - Title: `YouTube Uploader` (or `PS5 Share`).
   - Icon: Stock-styled SVG/PNG asset.
2. **Fullscreen WebKit Execution:**
   - Launches `ps5-web-browser-fullscreen` directly to `http://127.0.0.1:<PORT>`.
   - DualSense controller navigation: Left stick for mouse pointer, Cross (X) to select, Circle (O) to exit.

### Phase 3: Phone/PC Companion Web App
1. **Interactive Gallery View:**
   - Scans `/user/av_content/video/` and parses metadata: Game Title, Timestamp, Duration, File Size, Thumbnail.
2. **Clip Details & Custom Publishing:**
   - Modal to customize:
     - Title
     - Description
     - Privacy Status (`Public`, `Unlisted`, `Private`)
     - Category / Tags
3. **Upload Management & Quota Monitor:**
   - Real-time progress bar with upload speed and remaining time.
   - Display estimated quota units consumed today.
   - Settings pane for custom Google Cloud Client ID / Secret.

---

## 6. What is Left to the Builder
- **HTTP Client Library Selection:** Determine whether to utilize PShare's existing `libcurl` linkage or an asynchronous socket implementation.
- **Chunk Size Tuning:** Benchmark 4 MB, 8 MB, and 16 MB chunks on PS5 network stack for optimal memory usage and throughput.
- **Web Companion Stack:** Build the frontend using a lightweight single-file vanilla HTML/CSS/JS interface (zero external CDN dependencies for offline/isolated LAN resilience).
- **Configuration Schema:** Exact JSON schema structure for `/data/pshare/youtube.json`.
- **Packaging:** Scripting the build process using `PS5_PAYLOAD_SDK` into ready-to-deploy ELF/ZIP packages compatible with RecompStore / ProsperoStore.
