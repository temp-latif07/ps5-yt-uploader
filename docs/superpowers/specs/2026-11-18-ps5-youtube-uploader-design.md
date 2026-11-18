# PS5 YouTube Uploader Design Document

## 1. Intent and The Why
On retail PlayStation 5 consoles, uploading gameplay clips to YouTube relies on the DualSense "Create" button, the system Media Gallery, and active PlayStation Network (PSN) authentication. On a jailbroken PS5 (firmware 12.20 utilizing the y2JB Relapse exploit chain), the console is deliberately disconnected and blocked from PSN (via `nanoDNS`) to prevent telemetry and forced system updates. Consequently, invoking the stock share option fails with *"You're not signed in"*.

Currently, users must manually copy video clips to an external PC (via FTP, web file managers, or USB) and manually upload them to YouTube.

The goal of this project is to build an open-source, production-ready, native solution for the jailbroken PS5 community that packages everything into **a single unified, all-in-one payload** (`ps5-youtube.elf`). It restores the ability to upload gameplay captures directly from the console's internal SSD to a user's personal YouTube channel, feeling as close to the native stock experience as possible: a single file to install, a dedicated home screen Media tile, on-screen notifications, zero Google Cloud developer accounts, zero credit cards, and zero upload quotas.

---

## 2. Goals, Non-Goals, and Anti-Goals

### Goals
- **Single All-in-One Payload (`ps5-youtube.elf`):** Everything compiled into one self-contained ELF binary (no managing multiple disjointed payloads or companion dependencies).
- **Direct Console-to-YouTube Uploads:** Stream video captures directly from the PS5 internal storage (`/user/av_content/video/`) to YouTube over the console's network interface using YouTube Studio's internal Scotty resumable upload protocol.
- **Zero Google Cloud Accounts & Zero Credit Cards:** Authenticate without Google Cloud Console, API keys, or billing accounts, completely avoiding the 10,000-unit daily API quota limit.
- **Native PS5 Home Screen Integration:** Automatically register a dedicated launcher tile in the PS5 home screen's **Media** category (via embedded `browser_patch`) that opens in 1080p fullscreen.
- **Autonomous Background Uploads & TV Notifications:** Background thread catches newly saved gameplay clips, uploads them directly from SSD, and fires native PS5 system notification toasts (`sceNotificationRequest`).
- **Flexible UI Access:** Access the dashboard directly on the TV screen via DualSense controller, or from any phone/PC browser on local Wi-Fi (`http://<ps5-ip>:8888`).

### Non-Goals
- **In-Game Overlay Injection:** No direct hooking of Sony's closed-source `shellcore` Share dialog in memory while games are rendering (which risks game crashes, memory corruption, and firmware instability).
- **Video Re-encoding / Transcoding on Console:** Video clips recorded by the PS5 are already properly encoded H.264/HEVC MP4 files. The uploader will not transcode or alter video streams on-device.
- **Multiple Disjointed Tools:** The user will not be forced to install or run multiple background daemons.

### Anti-Goals
- **No Shared Monolithic Quota Bottlenecks:** No dependency on public Google Cloud API quotas.
- **No Credit Cards or Developer Portal Setup:** Users and community members never need to register as developers or attach payment methods.
- **No Heavy Background Overhead:** The daemon must consume negligible CPU (< 0.1%) and RAM (< 4 MB) when idle, avoiding interference with 60/120 FPS gameplay.
- **No Intermediary Cloud Relays:** Media bytes stream strictly from `PS5 SSD -> YouTube Upload Servers`.

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

### Approach 1: On-Console Loopback Proxy (MitM TLS Interception)
- **Description:** Run a local loopback proxy on `127.0.0.1` to intercept `grst` responses and cookies when logging in via PS5 WebKit.
- **Evaluation Result:** **REJECTED.** Fatal blockers: Google's Botguard blocks the PS5 WebKit browser with *"This browser or app may not be secure"*, PS5 lacks custom CA trust installation (risking system SSL corruption in `/system_data`), and the binary footprint bloats to 2.5–4 MB.

### Approach 2: Direct SQLite DB Extraction (`NKCookie.jar.db`)
- **Description:** Read `/user/home/<userid>/webkit/shell/NKCookie.jar.db` directly via root filesystem privileges.
- **Evaluation Result:** **REJECTED.** Architectural defect: `sessionToken` is an ephemeral bearer token returned via API responses, not an HTTP cookie, and is never written to `NKCookie.jar.db`. Furthermore, cookies cannot enter the DB because Google blocks login on PS5 WebKit.

### Approach 3: Hybrid Architecture (Media Tile on TV + Local LAN Companion) [SELECTED]
- **Description:** Decouples viewing and status (on the TV screen via the 1080p Media tile) from credential negotiation (handled smoothly via phone/PC browser on local Wi-Fi).
- **Evaluation Result:** **APPROVED FOR PRODUCTION.**
  - Zero Google Cloud accounts, zero Client IDs, zero credit cards, zero quota limits.
  - Avoids Google Botguard blocks by using certified mobile/desktop browsers for initial session negotiation.
  - PS5 daemon binary stays ultra-lightweight (< 600 KB).
  - Full stock feel on the TV: Dedicated Media category tile, clip gallery, DualSense controls, and native PS5 on-screen notification popups.

---

## 5. Architectural Specification: The Single Unified Payload (`ps5-youtube.elf`)

```
=================================================================================================
                               ps5-youtube.elf (Single Unified Payload)
=================================================================================================
  
  [ Subsystem 1: Media Tile & Browser Patch Engine ]
    - Injects home screen launcher icon directly into the PS5 "Media" category (Title: YouTube Uploader)
    - Fullscreen 1080p WebKit browser launcher (DualSense controller navigation)
    - Auto-registers on boot / payload launch

  [ Subsystem 2: Embedded Web Server & Dashboard (Port 8888) ]
    - Embedded lightweight HTTP server (serving responsive TV + mobile web dashboard)
    - Local clip gallery viewer with thumbnails, timestamps, and duration from /user/av_content/video/
    - Interactive clip curation modal: Edit title, description, tags, and privacy (Public/Unlisted/Private)
    - Pairing wizard: Dynamic QR code for 10-second phone session sync

  [ Subsystem 3: Capture Watcher & Autonomous Upload Daemon ]
    - Monitors /user/av_content/video/<GameID>/ for new clips saved via DualSense Create button
    - Autonomous resumable streaming from PS5 SSD -> YouTube Upload Servers (Scotty protocol)
    - Signs requests using internal SAPISIDHASH + sessionToken (proven in Spike B)
    - Emits native on-screen PS5 notification popups:
        * "YouTube: Uploading clip..."
        * "YouTube: Upload complete! (youtu.be/xxx)"

  [ Subsystem 4: Local Storage & Session Maintenance ]
    - Path: /data/youtube_uploader/config.json
    - Stores authenticated YouTube session and user preferences
    - Background keep-alive heartbeat to prevent session expiration
=================================================================================================
                                  |                             |
                                  v                             v
                    +---------------------------+ +---------------------------+
                    | PS5 TV Screen             | | Mobile / PC Browser       |
                    | (1080p Fullscreen Tile)   | | (http://<ps5-ip>:8888)    |
                    +---------------------------+ +---------------------------+
```

### Single-File Deployment Model
* **File:** `ps5-youtube.elf` (compiled using `PS5_PAYLOAD_SDK`).
* **Installation:** Transferred via `ps5upload` or sent to port 9021 (`elfldr`). Once dropped in the console's autoloader directory (alongside `kstuff-lite`), it boots automatically with the jailbreak.
* **Zero Dependencies:** No external Python runtimes, no secondary payload chains, no cloud middleman. Everything is compiled into one native C/C++ binary (< 700 KB).

### Key Architectural Decisions
1. **Zero Google Cloud & Zero Card Requirements:** The uploader operates via YouTube Studio's internal Scotty and InnerTube protocols, validated in Spike B (`https://youtu.be/6_ZUsDALKh0`). It completely frees users from Google Cloud projects, Client IDs, credit card attachments, and the 10,000-unit API quota limit.
2. **Hybrid Pairing Experience:**
   - Launching the **YouTube Uploader** tile in the PS5's **Media** category displays the gallery and a pairing QR code.
   - Pointing your phone camera at the screen opens `http://<ps5-ip>:8888` on your phone (which is already logged into YouTube).
   - Tapping "Connect" transfers the session over local LAN in under 10 seconds.
   - The TV screen instantly turns green: *"Connected as [Channel Name]"*.
3. **Daemon Footprint & Efficiency:**
   - Stripped-down C/C++ background daemon (< 600 KB).
   - Near-zero idle RAM (< 4 MB) and 0.0% CPU overhead while gaming.
   - Native PS5 system toasts on start, completion, and session expiration.
4. **Autonomous SSD Streaming:**
   - Captures are streamed directly from `/user/av_content/video/` on the PS5 internal SSD to YouTube's upload servers in 8 MB chunks.
   - Videos never route through an external phone or computer during gameplay.

### Phase 1: MVP - PShare Fork & YouTube Engine
1. **Google OAuth 2.0 Device Flow (RFC 8628):**
   - Minimal embedded web endpoint `/setup` displays a QR code and link to `https://www.google.com/device` along with the 8-character `user_code`.
   - Daemon securely acquires and saves `access_token` and `refresh_token` in `/data/pshare/youtube.json`.
2. **Resumable Upload Implementation:**
   - Initiates resumable session with Google Video Upload API.
   - Streams MP4 capture in 8 MB chunks via HTTP `PUT` with `Content-Range`.
   - Handles network interruptions with HTTP `308 Resume Incomplete` range queries.
3. **System Notifications:**
   - On upload start: *"YouTube: Uploading clip..."*
   - On upload complete: *"YouTube: Clip uploaded successfully (youtu.be/xxx)"*
   - On failure: *"YouTube: Upload failed - [reason]"*

### Phase 2: Dedicated Home Screen Media Tile
1. **Media Tile Registration (`browser_patch`):**
   - Memory-patches PS5 shellcore / `app.db` to register a dedicated tile in the **Media** category.
   - Title: `YouTube Uploader` (or `PS5 Share`).
   - Icon: Stock-styled SVG/PNG asset.
2. **Fullscreen WebKit Execution:**
   - Launches `ps5-web-browser-fullscreen` directly to `http://127.0.0.1:8888`.
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
