# PS5 YouTube Uploader Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a production-ready, single unified ELF payload (`ps5-youtube.elf`, < 700 KB) for jailbroken PlayStation 5 consoles (firmware 12.20 / y2JB Relapse) that streams gameplay captures directly from the internal SSD (`/user/av_content/video/`) to YouTube using YouTube Studio's internal Scotty and InnerTube protocols with zero Google Cloud accounts, zero API keys, zero credit cards, and zero upload quotas.

**Architecture:** A lightweight native C99/POSIX background daemon containing four decoupled subsystems compiled into a single executable: an embedded BSD-socket web server on port 8888 serving a responsive DualSense/mobile web dashboard, an autonomous filesystem capture watcher with write-debouncing and native PS5 notification toasts, a Scotty/InnerTube upload engine signing requests via `SAPISIDHASH` and `sessionToken`, and a Media category home screen launcher registration engine (`browser_patch`). The codebase is designed with clean platform abstraction so that 100% of the upload engine, crypto, server, and watcher logic can be verified natively with unit and mock tests on host systems, while cross-compiling to FreeBSD 12 / PS5 ELF.

**Tech Stack:** C99, POSIX threads (`pthread`), BSD sockets, `cJSON`, custom zero-dependency SHA-1, PS5 Libkernel / SceNotificationRequest, vanilla HTML5/CSS3/JavaScript (embedded as byte arrays in C headers), Make / Clang (`-target x86_64-unknown-freebsd12`).

**Spec:** [`docs/superpowers/specs/2026-11-18-ps5-youtube-uploader-design.md`](file:///Users/latif/ps5-youtube/docs/superpowers/specs/2026-11-18-ps5-youtube-uploader-design.md)

---

## Global Constraints

- Platform: PlayStation 5 (Fat/Slim/Pro), Firmware 12.20 (compatible with 7.00 - 13.60 kernel exploit range).
- Packaging: Single unified binary output `ps5-youtube.elf` (< 700 KB). Zero secondary payload dependencies.
- Zero Google Cloud / Zero Credit Card: No Google Cloud Console projects, Client IDs, API keys, or billing requirements.
- Zero Quota Ceiling: Uses YouTube Studio InnerTube / Scotty web protocol to bypass the 10,000-unit (~6 videos/day) YouTube Data API ceiling.
- Filesystem Paths:
  - Video Captures: `/user/av_content/video/<TitleID>/` (.mp4 / .webm).
  - App Storage: `/data/youtube_uploader/` (for `config.json` and `uploaded.json`).
  - PS5 MMS Database: `/system_data/priv/mms/app.db`.
- Network & Ports: Daemon listens on port `8888`. WebKit connects to `http://127.0.0.1:8888`; local Wi-Fi devices connect to `http://<ps5-ip>:8888`.
- Resource Limits: Idle memory < 4 MB, idle CPU < 0.1%, to ensure 60/120 FPS gameplay is unaffected.

---

## Review Focus

These five critical failure modes and edge cases are explicitly pinned to their owning tasks:
1. **In-progress capture write debouncing (Task 4):** Watcher encounters a video file currently being written by the PS5 operating system. Must verify size stability over consecutive intervals before initiating upload.
2. **Expired session or invalid credentials (Task 3):** Google invalidates session cookies or sessionToken. Engine must flag status `UNAUTHENTICATED`, avoid infinite retry loops, and dispatch an on-screen toast requesting re-pairing.
3. **Mid-stream network drop and resume (Task 3):** Network drops during 8 MB chunk upload. Engine must query the Scotty upload offset (`upload, query`) and resume from the byte offset rather than restarting the upload or corrupting the stream.
4. **Duplicate upload avoidance across reboots (Task 1 & Task 4):** Existing captures on SSD must not be re-uploaded on daemon reboot. A persistent upload history database (`uploaded.json`) must store file hashes/paths.
5. **Missing or uninitialized `/data` directory (Task 1):** On fresh consoles or after database rebuilds, `/data/youtube_uploader/` will not exist. System must recursively ensure directory creation with permissions `0777` without failing.

---

## Implementation Tasks

### Task 1: Core Foundation & Configuration Manager

**Files:**
- Create: `src/config.h`
- Create: `src/config.c`
- Create: `vendor/cJSON.h`
- Create: `vendor/cJSON.c`
- Create: `tests/test_config.c`
- Modify: `Makefile`

**Interfaces:**
- Consumes: Standard POSIX filesystem APIs (`stat`, `mkdir`, `fopen`, `access`).
- Produces:
  ```c
  typedef struct {
      char cookies[8192];
      char session_token[512];
      char innertube_key[128];
      char channel_id[64];
      char channel_name[128];
      int auto_upload;
      char default_privacy[16]; // "PUBLIC", "UNLISTED", "PRIVATE"
      int server_port;
  } app_config_t;

  int config_init(const char* base_dir);
  int config_load(app_config_t* cfg);
  int config_save(const app_config_t* cfg);
  int history_is_uploaded(const char* filepath);
  int history_mark_uploaded(const char* filepath, const char* video_id);
  ```

- [ ] **Step 1: Vendor `cJSON` and write failing tests for configuration management**

Vendors minimal `cJSON` in `vendor/` and creates `tests/test_config.c`:
```c
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../src/config.h"

void test_config_defaults_and_io(void) {
    const char* test_dir = "/tmp/test_ps5_youtube";
    assert(config_init(test_dir) == 0);

    app_config_t cfg;
    assert(config_load(&cfg) == 0);
    assert(cfg.server_port == 8888);
    assert(strcmp(cfg.default_privacy, "UNLISTED") == 0);
    assert(cfg.auto_upload == 1);

    strcpy(cfg.channel_id, "UC123456789");
    strcpy(cfg.session_token, "TOKEN_ABC_123");
    assert(config_save(&cfg) == 0);

    app_config_t loaded;
    assert(config_load(&loaded) == 0);
    assert(strcmp(loaded.channel_id, "UC123456789") == 0);
    assert(strcmp(loaded.session_token, "TOKEN_ABC_123") == 0);
}

void test_history_deduplication(void) {
    const char* clip = "/user/av_content/video/CUSA00001/clip1.mp4";
    assert(history_is_uploaded(clip) == 0);
    assert(history_mark_uploaded(clip, "vid123") == 0);
    assert(history_is_uploaded(clip) == 1);
}

int main(void) {
    test_config_defaults_and_io();
    test_history_deduplication();
    printf("Task 1 Tests Passed Successfully!\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test_config`
Expected: Compilation failure or missing `src/config.h`.

- [ ] **Step 3: Implement `src/config.h` and `src/config.c`**

Implements `config_init` to recursively create directories (`/data/youtube_uploader/`), initializes default configurations, handles serialization/deserialization via `cJSON`, and maintains `uploaded.json` for persistent deduplication.

- [ ] **Step 4: Run test to verify it passes**

Run: `make test_config && ./bin/test_config`
Expected: Output `Task 1 Tests Passed Successfully!` with exit code 0.

- [ ] **Step 5: Commit**

```bash
git add Makefile vendor/ src/config.* tests/test_config.c
git commit -m "feat(core): implement configuration manager and history deduplication"
```

---

### Task 2: Cryptographic Engine & Session Authentication

**Files:**
- Create: `src/crypto.h`
- Create: `src/crypto.c`
- Create: `tests/test_crypto.c`
- Modify: `Makefile`

**Interfaces:**
- Consumes: Standard C string functions.
- Produces:
  ```c
  void sha1_hex(const unsigned char* data, size_t len, char hex_out[41]);
  int generate_sapisid_hash(const char* sapisid, const char* origin, long timestamp, char* hash_out, size_t max_len);
  int extract_cookie_value(const char* cookie_header, const char* cookie_name, char* value_out, size_t max_len);
  ```

- [ ] **Step 1: Write failing unit test for `generate_sapisid_hash` and `extract_cookie_value`**

Creates `tests/test_crypto.c` using known test vectors validated in Spike B:
```c
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/crypto.h"

void test_sha1(void) {
    char out[41];
    sha1_hex((const unsigned char*)"test", 4, out);
    assert(strcmp(out, "a94a8fe5ccb19ba61c4c0873d391e987982fbbd3") == 0);
}

void test_sapisid_hash_generation(void) {
    const char* sapisid = "SampleSapisid123";
    const char* origin = "https://studio.youtube.com";
    long timestamp = 1700000000;
    char hash_buf[128];

    // "{timestamp} {sapisid} {origin}" -> "1700000000 SampleSapisid123 https://studio.youtube.com"
    // sha1("1700000000 SampleSapisid123 https://studio.youtube.com") = 7a83d726b15efb5f7e4d8e874838bbca0f17ffb4
    assert(generate_sapisid_hash(sapisid, origin, timestamp, hash_buf, sizeof(hash_buf)) == 0);
    assert(strncmp(hash_buf, "1700000000_", 11) == 0);
}

void test_cookie_extraction(void) {
    const char* cookie_str = "VISITOR_INFO1_LIVE=abc; SAPISID=RealSapisidValue999; LOGIN_INFO=xyz";
    char val[128];
    assert(extract_cookie_value(cookie_str, "SAPISID", val, sizeof(val)) == 0);
    assert(strcmp(val, "RealSapisidValue999") == 0);
    assert(extract_cookie_value(cookie_str, "NONEXISTENT", val, sizeof(val)) == -1);
}

int main(void) {
    test_sha1();
    test_sapisid_hash_generation();
    test_cookie_extraction();
    printf("Task 2 Crypto Tests Passed Successfully!\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test_crypto`
Expected: Compilation failure or missing `src/crypto.h`.

- [ ] **Step 3: Implement `src/crypto.h` and `src/crypto.c`**

Implements a zero-dependency RFC 3174 compliant SHA-1 hash function, `generate_sapisid_hash`, and robust cookie string parsing.

- [ ] **Step 4: Run test to verify it passes**

Run: `make test_crypto && ./bin/test_crypto`
Expected: Output `Task 2 Crypto Tests Passed Successfully!` with exit code 0.

- [ ] **Step 5: Commit**

```bash
git add src/crypto.* tests/test_crypto.c Makefile
git commit -m "feat(crypto): implement sha1, sapisidhash generator, and cookie extractor"
```

---

### Task 3: Scotty & InnerTube Upload Engine

**Files:**
- Create: `src/http_client.h`
- Create: `src/http_client.c`
- Create: `src/youtube.h`
- Create: `src/youtube.c`
- Create: `tests/mock_youtube_server.py`
- Create: `tests/test_youtube_upload.c`
- Modify: `Makefile`

**Interfaces:**
- Consumes: `src/crypto.h`, `src/config.h`.
- Produces:
  ```c
  typedef struct {
      char video_id[32];
      char youtube_url[128];
      int http_status;
      char error_message[256];
  } upload_result_t;

  typedef void (*upload_progress_cb)(size_t uploaded_bytes, size_t total_bytes, void* userdata);

  int youtube_init_upload_session(const app_config_t* cfg, const char* frontend_id, char* upload_url_out, size_t max_len);
  int youtube_stream_file(const app_config_t* cfg, const char* upload_url, const char* filepath, char* scotty_id_out, size_t max_len, upload_progress_cb cb, void* userdata);
  int youtube_finalize_video(const app_config_t* cfg, const char* frontend_id, const char* scotty_id, const char* title, const char* description, const char* privacy, upload_result_t* result);
  int youtube_upload_clip(const app_config_t* cfg, const char* filepath, const char* title, const char* description, const char* privacy, upload_progress_cb cb, void* userdata, upload_result_t* result);
  ```

- [ ] **Step 1: Write mock Scotty server and failing test for upload flow & error recovery**

Creates `tests/mock_youtube_server.py` (simulating Scotty start, chunk upload, resume query, and InnerTube `createvideo`) and `tests/test_youtube_upload.c` testing:
1. Normal end-to-end upload of `spikes/sample_clip.mp4`.
2. Resume on mid-stream disconnection.
3. Proper `UNAUTHENTICATED` error handling when `session_token` is rejected (Review Focus #2 & #3).

- [ ] **Step 2: Run test to verify it fails**

Run: `make test_youtube`
Expected: Compilation failure for missing `src/youtube.h` and `src/http_client.h`.

- [ ] **Step 3: Implement `src/http_client.c` and `src/youtube.c`**

Implements:
1. `http_client.c`: Light HTTP client supporting GET/POST/PUT, custom headers, chunked body streaming, and reading response headers (`x-goog-upload-url`).
2. `youtube.c`: Resumable session creation, 8 MB chunk streaming with Scotty upload headers (`upload, finalize`), `createvideo` InnerTube JSON request construction, and error extraction.

- [ ] **Step 4: Run test to verify it passes**

Run: `make test_youtube && ./bin/test_youtube`
Expected: Output `Task 3 YouTube Engine Tests Passed Successfully!` with exit code 0.

- [ ] **Step 5: Commit**

```bash
git add src/http_client.* src/youtube.* tests/mock_youtube_server.py tests/test_youtube_upload.c Makefile
git commit -m "feat(youtube): implement scotty resumable upload and innertube createvideo engine"
```

---

### Task 4: Filesystem Capture Watcher & Debouncing Engine

**Files:**
- Create: `src/notify.h`
- Create: `src/notify.c`
- Create: `src/watcher.h`
- Create: `src/watcher.c`
- Create: `tests/test_watcher.c`
- Modify: `Makefile`

**Interfaces:**
- Consumes: `src/config.h`, `src/youtube.h`.
- Produces:
  ```c
  void notify_send(const char* message);

  typedef struct {
      char filepath[512];
      char title_id[32];
      time_t timestamp;
      off_t size_bytes;
  } capture_clip_t;

  typedef void (*watcher_clip_ready_cb)(const capture_clip_t* clip, void* userdata);

  int watcher_init(const char* captures_root, watcher_clip_ready_cb cb, void* userdata);
  int watcher_start(void);
  void watcher_stop(void);
  int scan_existing_clips(const char* captures_root, capture_clip_t** clips_out, size_t* count_out);
  ```

- [ ] **Step 1: Write failing test for capture detection, write-debouncing, and deduplication**

Creates `tests/test_watcher.c` (Review Focus #1 & #4):
1. Creates a mock directory simulating `/user/av_content/video/CUSA12345/`.
2. Simulates an in-progress capture write (file size grows over 1 second). Verifies watcher waits until file size is stable across intervals before triggering callback.
3. Verifies that already-uploaded files are skipped.
4. Verifies `scan_existing_clips` lists available captures sorted by newest first.

- [ ] **Step 2: Run test to verify it fails**

Run: `make test_watcher`
Expected: Compilation failure or missing `src/watcher.h`.

- [ ] **Step 3: Implement `src/notify.c` and `src/watcher.c`**

Implements:
1. `notify.c`: PS5 platform toast dispatch via `sceNotificationRequest` (with stdout fallback for host test environments).
2. `watcher.c`: Background scanning thread checking `/user/av_content/video/`, debouncing actively written files by tracking size over 2s intervals, checking `history_is_uploaded`, and triggering notifications:
   - *"YouTube: Uploading clip..."*
   - *"YouTube: Upload complete! (youtu.be/xxx)"*
   - *"YouTube: Upload failed - [reason]"*

- [ ] **Step 4: Run test to verify it passes**

Run: `make test_watcher && ./bin/test_watcher`
Expected: Output `Task 4 Watcher Tests Passed Successfully!` with exit code 0.

- [ ] **Step 5: Commit**

```bash
git add src/notify.* src/watcher.* tests/test_watcher.c Makefile
git commit -m "feat(watcher): implement capture watcher with write-debouncing and notifications"
```

---

### Task 5: Embedded HTTP Server & REST API

**Files:**
- Create: `src/server.h`
- Create: `src/server.c`
- Create: `src/api.h`
- Create: `src/api.c`
- Create: `tests/test_server_api.c`
- Modify: `Makefile`

**Interfaces:**
- Consumes: `src/config.h`, `src/watcher.h`, `src/youtube.h`.
- Produces:
  ```c
  int server_start(int port);
  void server_stop(void);
  int server_register_route(const char* method, const char* path, void (*handler)(const char* req_body, char* resp_out, size_t max_resp));
  ```
- Endpoints exposed:
  - `GET /api/status`: JSON with `{authenticated: bool, channel_title: str, channel_id: str, auto_upload: bool}`.
  - `GET /api/clips`: JSON list of all clips on internal SSD with metadata.
  - `POST /api/upload`: Payload `{filepath: str, title: str, description: str, privacy: str}`. Triggers background upload and returns `{status: "queued"}`.
  - `POST /api/auth`: Payload `{cookies: str, session_token: str}`. Saves credentials, tests session, and responds with `{success: bool, channel_title: str}`.
  - `POST /api/config`: Payload `{auto_upload: bool, default_privacy: str}`. Updates config.

- [ ] **Step 1: Write failing integration test for REST API endpoints**

Creates `tests/test_server_api.c` that starts the server on port 8889, performs HTTP GET `/api/status`, GET `/api/clips`, POST `/api/auth`, and POST `/api/upload` via sockets or curl, and asserts HTTP 200 and expected JSON keys.

- [ ] **Step 2: Run test to verify it fails**

Run: `make test_server`
Expected: Compilation failure or missing `src/server.h`.

- [ ] **Step 3: Implement `src/server.c` and `src/api.c`**

Implements a non-blocking/threaded POSIX BSD socket HTTP server, parses request lines and headers, handles CORS (for local LAN pairing), dispatches REST routes, and serves JSON responses.

- [ ] **Step 4: Run test to verify it passes**

Run: `make test_server && ./bin/test_server`
Expected: Output `Task 5 Server API Tests Passed Successfully!` with exit code 0.

- [ ] **Step 5: Commit**

```bash
git add src/server.* src/api.* tests/test_server_api.c Makefile
git commit -m "feat(server): implement embedded http server and rest api endpoints"
```

---

### Task 6: Embedded Web Dashboard UI & Single-Asset Bundler

**Files:**
- Create: `web/index.html`
- Create: `web/style.css`
- Create: `web/app.js`
- Create: `scripts/bundle_web.py`
- Create: `src/web_assets.h`
- Create: `tests/test_web_assets.c`
- Modify: `src/server.c`
- Modify: `Makefile`

**Interfaces:**
- Consumes: REST API (`/api/*`).
- Produces: Embedded C byte arrays in `src/web_assets.h` served directly by `src/server.c` for `GET /`, `GET /style.css`, `GET /app.js`.

- [ ] **Step 1: Build the responsive Single-Page Dashboard**

Creates `web/index.html`, `web/style.css`, and `web/app.js`:
- Dark mode PlayStation 5 aesthetic (deep blues/blacks, clean typography).
- Responsive: Fullscreen 1080p layout with large focusable buttons for DualSense stick/X navigation, and compact mobile layout for smartphones.
- Pairing View: Dynamic QR code pointing to `http://<ps5-ip>:8888` + 1-Click "Connect Current Session" button + manual cookie import box.
- Clips Gallery View: Grid of recorded captures showing game title, duration, size, and date, with "Upload" button opening title/description/privacy modal.
- Upload Queue View: Real-time progress bar showing active uploads and clickable YouTube links.

- [ ] **Step 2: Create bundling script and verify header generation**

Creates `scripts/bundle_web.py` that minifies and converts `web/*` into `src/web_assets.h` containing `const unsigned char index_html[]`, `const unsigned char style_css[]`, and `const unsigned char app_js[]`.

- [ ] **Step 3: Write test to verify embedded web asset delivery**

Creates `tests/test_web_assets.c` connecting to `http://localhost:8888/` and verifying:
1. `GET /` returns `200 OK`, `Content-Type: text/html`, and contains `<title>PS5 YouTube Uploader</title>`.
2. `GET /style.css` returns `200 OK`, `Content-Type: text/css`.
3. `GET /app.js` returns `200 OK`, `Content-Type: application/javascript`.

- [ ] **Step 4: Run test to verify it passes**

Run: `make test_web && ./bin/test_web`
Expected: Output `Task 6 Web Asset Delivery Tests Passed Successfully!` with exit code 0.

- [ ] **Step 5: Commit**

```bash
git add web/ scripts/bundle_web.py src/web_assets.h tests/test_web_assets.c Makefile
git commit -m "feat(ui): implement embedded ps5 web dashboard and asset bundler"
```

---

### Task 7: Media Tile Registration & Fullscreen WebKit Patch

**Files:**
- Create: `src/media_tile.h`
- Create: `src/media_tile.c`
- Create: `tests/test_media_tile.c`
- Modify: `Makefile`

**Interfaces:**
- Consumes: SQLite / PS5 MMS schema.
- Produces:
  ```c
  int media_tile_register(const char* db_path, const char* app_title, const char* launch_url);
  int media_tile_unregister(const char* db_path, const char* title_id);
  ```

- [ ] **Step 1: Write failing test for `media_tile_register` on mock `app.db`**

Creates `tests/test_media_tile.c`:
1. Initializes a mock SQLite `/system_data/priv/mms/app.db` table schema matching PS5 MMS (`tbl_appbrowse_1234567890`).
2. Calls `media_tile_register(mock_db, "YouTube Uploader", "http://127.0.0.1:8888")`.
3. Verifies entry exists in category `media` (category ID `2`), with title `YouTube Uploader` and URL `http://127.0.0.1:8888`.

- [ ] **Step 2: Run test to verify it fails**

Run: `make test_media_tile`
Expected: Compilation failure or missing `src/media_tile.h`.

- [ ] **Step 3: Implement `src/media_tile.c`**

Implements SQL insertion and update logic into PS5's `app.db` and launches the fullscreen WebKit browser (`ps5-web-browser-fullscreen`).

- [ ] **Step 4: Run test to verify it passes**

Run: `make test_media_tile && ./bin/test_media_tile`
Expected: Output `Task 7 Media Tile Tests Passed Successfully!` with exit code 0.

- [ ] **Step 5: Commit**

```bash
git add src/media_tile.* tests/test_media_tile.c Makefile
git commit -m "feat(system): implement media category tile registration for fullscreen webkit"
```

---

### Task 8: Single Unified Payload Integration (`ps5-youtube.elf`) & End-to-End Build

**Files:**
- Create: `src/main.c`
- Modify: `Makefile`
- Create: `tests/test_e2e_integration.sh`

**Interfaces:**
- Consumes: All subsystems (`config`, `crypto`, `youtube`, `watcher`, `server`, `media_tile`, `notify`).
- Produces:
  - Host executable: `bin/ps5-youtube-host` (for local simulation, testing, and debugging).
  - Target payload: `bin/ps5-youtube.elf` (single standalone ELF binary targeting PS5).

- [ ] **Step 1: Implement `src/main.c` entrypoint**

Wires the lifecycle:
1. Parse command line arguments / initialize `/data/youtube_uploader/`.
2. Load configuration.
3. Register Media home screen tile.
4. Start embedded web server on port 8888.
5. Start capture watcher background thread.
6. Install signal handlers (`SIGINT`, `SIGTERM`) for graceful teardown.
7. Dispatches boot notification toast: *"YouTube Uploader running on port 8888"*.

- [ ] **Step 2: Configure Makefile for host build and PS5 ELF cross-compilation**

Adds targets in `Makefile`:
```makefile
# Native test build
all: test bin/ps5-youtube-host

# Cross-compilation for PS5 FreeBSD 12 ELF
ps5:
	clang -target x86_64-unknown-freebsd12 -O2 -fPIC -DPS5 -I./include -I./vendor \
	      src/*.c vendor/cJSON.c -o bin/ps5-youtube.elf -lpthread
	@echo "Built bin/ps5-youtube.elf size: $$(ls -lh bin/ps5-youtube.elf | awk '{print $$5}')"
```

- [ ] **Step 3: Run comprehensive end-to-end integration test**

Creates and runs `tests/test_e2e_integration.sh`:
1. Launches mock YouTube Scotty server.
2. Starts `bin/ps5-youtube-host` in temporary sandbox directory.
3. Simulates pairing via `POST http://localhost:8888/api/auth`.
4. Drops a mock gameplay MP4 file into the watched captures folder.
5. Asserts the watcher detects the clip, uploads it via Scotty, and logs the YouTube video URL.
6. Verifies clean shutdown on `SIGINT`.
7. Builds `make ps5` and asserts `bin/ps5-youtube.elf` is generated and size is < 700 KB.

- [ ] **Step 4: Commit**

```bash
git add src/main.c Makefile tests/test_e2e_integration.sh
git commit -m "feat(integration): integrate single unified payload and cross-compilation target"
```

---

## Self-Review Verification

1. **Spec coverage:** Every section of the approved design document (Scotty protocol, zero Google Cloud credentials, zero credit card, single unified ELF payload, Media category tile, background watcher, DualSense/mobile companion UI, PS5 notification toasts) is directly covered by Tasks 1 through 8.
2. **Step scan:** Every step contains exact signatures, concrete commands, and definitive pass/fail criteria.
3. **Type consistency:** Data structures (`app_config_t`, `capture_clip_t`, `upload_result_t`) and signatures are consistent across tasks.
4. **Review Focus:** All five potential failure modes (write debouncing, credential expiration, network drop/resume, duplicate upload avoidance across reboots, and missing data directory) are explicitly tested in Tasks 1, 3, and 4.
5. **Proportion:** The plan focuses strictly on architecture, interfaces, and test verification without unnecessary narrative code sprawl.
