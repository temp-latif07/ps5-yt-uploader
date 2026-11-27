# AGENTS.md — Repository Rules & Maintenance Guidelines

This document defines the development, maintenance, and release standards for **PS5 YouTube Uploader (`ps5-yt-uploader`)**. All AI assistants, autonomous agents, and human contributors working in this repository MUST follow these rules.

---

## 1. Repository Identity & Core Constraints

- **Project Name:** `ps5-yt-uploader`
- **Primary Artifacts:**
  - `build/ps5-yt-uploader.elf` — Background `elfldr` payload (port `8085`, static `libcurl` + TLS + embedded CA bundle).
  - `build/PPSA97631/` — Native home-screen launcher tile (`YouTube Uploader`, Title ID `PPSA97631`, Content ID `UP9000-PPSA97631_00-PS5YTUPLOADER001`) that boots `ps5-yt-uploader.elf` via `127.0.0.1:9021`, opens the PS5 browser on `http://127.0.0.1:8085`, and exits cleanly so uploads continue while playing games.
- **Coexistence:** Uses port `8085` and Title ID `PPSA97631` (never collides with `torbox-ps5` on `8080` / `PPSA97630` or PS5 Web File Manager on `8888`).
- **Clean-Room Guarantee:** 100% open-source C99/C11 and standard POSIX interfaces. Zero proprietary Sony SDK libraries, leaked decryption keys, or DRM circumvention code.
- **Resource Limits:** Static TLS ELF payload stays lean (~1.5–4 MB on disk), runtime RAM footprint under 16 MB, and < 0.1% CPU at idle.
- **Original Reference:** The earlier codebase is preserved under `archive/` for reference only; do not modify `archive/` unless explicitly requested.

---

## 2. Release & Changelog Standards

### 2.1 Human-Readable Release Notes (`CHANGELOG.md`)
- Never use raw git commit dumps as release notes. Write release notes for humans using the console.
- `## Unreleased` stays permanently at the top of `CHANGELOG.md`. Every user-facing feature or bugfix MUST include a bullet under `## Unreleased`.
- When cutting a release, organize changes under `## X.Y.Z` categorized into `**New**`, `**Fixes**`, and `**Behind the scenes**`.
- `tools/extract_release_notes.py` extracts release notes directly from `CHANGELOG.md`.

### 2.2 Version Synchronization
All version references must stay synchronized:
1. `src/app.h`: `#define APP_VERSION "X.Y.Z"` (source of truth).
2. `title/param.json`: `contentVersion` / `masterVersion`.
3. `CHANGELOG.md`: Section heading `## X.Y.Z`.
4. `payloads.json`: `"version": "vX.Y.Z"` and release download URL.
5. Git Tag: `vX.Y.Z`.

### 2.3 Quality Gates Before Release
Never tag or publish without passing all verification gates:
```bash
bash tests/test_host.sh
python3 tools/update_payloads.py --check payloads.json
bash tools/release.sh --dry-run
```

### 2.4 Release Execution
- Run `./build.sh release` (or `bash tools/release.sh`). Non-interactive execution requires `-y` / `--yes`.
- Releases update `payloads.json`, verify ELF/title builds, tag `vX.Y.Z`, and publish via `gh release create`.

---

## 3. PS5 Payload Manager Integration Rules

1. **Catalog Specification (`payloads.json`):** Follows `itsPLK/ps5-payload-manager` `CUSTOM_REPOSITORIES.md`:
   - Top-level `"name": "PS5 YouTube Uploader"`, `"payloads"` array.
   - Item fields: `id` (`ps5-yt-uploader`), `name`, `filename` (`ps5-yt-uploader.elf`), `url`, `version`, `category` (`Utilities`), `checksum`, `author` (`Latif Muhammad`), `source`.
2. **Checksum Integrity:** Always lowercase 64-character hexadecimal SHA-256 (`^[0-9a-f]{64}$`).
3. **Canonical Source URL:**
   `https://raw.githubusercontent.com/temp-latif07/ps5-yt-uploader/main/payloads.json`
4. **Idempotent Updates:** `tools/update_payloads.py` updates the `ps5-yt-uploader` entry in-place.

---

## 4. Code Architecture & Development Guidelines

1. **Idiomatic C99:** Compile cleanly with `-std=c99 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -Wall -Wextra` and zero warnings.
2. **PS5 Payload Runtime Rules:**
   - Guard EVERY blocking `accept()`, `recv()`, and `send()` with `poll()` (PS5 ignores `O_NONBLOCK`/`FIONBIO`).
   - Detached threads MUST use `THREAD_STACK` (`1024 * 1024` — below 2 MiB).
   - Use `gettimeofday()` instead of `clock_gettime()`.
   - Ignore `SIGPIPE`; drain briefly on socket close (`shutdown(SHUT_WR)`).
   - Use canonical 3120-byte `sceKernelSendNotificationRequest` struct (`useless1[45]`, `message[3075]`) and `sceKernelDebugOutText` for klog.
3. **Resumable Uploads & Concurrency:**
   - Store OAuth client credentials and refresh token atomically (`0600`) in `/data/ps5-yt-uploader/oauth.txt` via `net_oauth_set()`, keeping access tokens strictly in an in-memory mutex-protected cache.
   - Persist YouTube Data API v3 resumable `upload_url` in `queue.tsv` so interrupted uploads resume via `Content-Range: bytes */<total>` (`308` offset query) and re-init cleanly if `404`/`410` expired.
   - Enforce BOTH `plat_path_allowed()` and video extension validation in `/api/stream` and `/api/upload`.
   - Require `X-PS5YT` header on all `POST /api/*` routes.
4. **Embedded Web Assets:** `tools/embed.py` compiles `web/*` and CA certificates into `build/gen/assets.c` automatically during `./build.sh`.

---

## 5. Security & Git Hygiene

1. **Credentials & Secrets:** Never commit `oauth.txt`, `cookies.txt`, `session.txt`, `client_secret_*.json`, or tokens. Never log `client_secret`, `refresh_token`, `access_token`, or `device_code` values.
2. **Conventional Commits:** `feat(...)`, `fix(...)`, `docs(...)`, `chore(...)`, `test(...)`.
3. **Clean Diffs:** Inspect `git diff` before committing; keep `build/` and `ps5yt-data/` untracked.
