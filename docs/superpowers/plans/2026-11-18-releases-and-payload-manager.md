# PS5 YT Uploader: GitHub Releases & Payload Manager Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Establish an automated GitHub release pipeline and first-class PS5 Payload Manager custom repository integration for `ps5-yt-uploader`, ensuring seamless console-side installation, updates, and autoloading.

**Architecture:** Build a reproducible GitHub Actions workflow on `macos-latest` that compiles `ps5-youtube.elf`, verifies checksums, generates release assets, and automatically updates the root `payloads.json` catalog following the `itsPLK/ps5-payload-manager` specification. Supplement with version-aware CLI/API endpoints, a developer release orchestrator script, and comprehensive documentation complying with GitHub ToS.

**Tech Stack:** C99, Python 3, Make, LLVM/Clang (FreeBSD 12 target), GitHub Actions, GitHub CLI (`gh`).

**Spec:** `docs/superpowers/specs/2026-11-18-releases-and-payload-manager-design.md`

## Global Constraints

- **Platform Target:** x86_64 FreeBSD 12 ELF PIE executable (`-target x86_64-unknown-freebsd12`)
- **Binary Footprint:** Stripped ELF payload must not exceed 700 KB (target: ~154 KB)
- **Repository Name:** `ps5-yt-uploader` under user `temp-latif07`
- **Catalog Schema:** Strictly conform to `itsPLK/ps5-payload-manager` `CUSTOM_REPOSITORIES.md`
- **Catalog Location:** `payloads.json` at repository root on `main` branch
- **Licensing & Compliance:** MIT License, 100% clean-room C, zero proprietary Sony binaries/keys, zero circumvention of DRM

## Review Focus

1. **Checksum String Format:** SHA-256 hash must be exactly 64 lowercase hexadecimal characters; uppercase or length errors cause Payload Manager download rejection.
2. **Catalog Idempotency:** Running `scripts/update_payload_repo.py` multiple times on the same version must update the existing entry in-place rather than appending duplicates.
3. **Download URL Directness:** Release URLs in `payloads.json` must point directly to the binary release tag asset without broken path separators.
4. **API Version Consistency:** `/api/status` and `--version` CLI must read from a single canonical version definition (`src/version.h`).
5. **Workflow Token Permissions:** `.github/workflows/release.yml` must explicitly configure `permissions: contents: write` so release creation and catalog commits succeed without permission errors.

---

### Task 1: Version Metadata, CLI Flags, and Status API

**Files:**
- Create: `src/version.h`
- Modify: `src/main.c:20-80`
- Modify: `src/api.c:50-70`
- Modify: `web/index.html:1-30`
- Modify: `web/app.js:1-40`
- Test: `tests/test_version_cli.c`
- Modify: `Makefile:25-96`

**Interfaces:**
- Consumes: None
- Produces:
  - `APP_VERSION`: `"1.0.0"`
  - `APP_NAME`: `"ps5-yt-uploader"`
  - `APP_DISPLAY_NAME`: `"PS5 YouTube Uploader"`
  - CLI: `-v`, `--version` exits with code 0 and prints version string
  - API: `GET /api/status` returns JSON object with `"version"` and `"app_name"`

- [ ] **Step 1: Write failing unit test for version CLI flag and version metadata**

Create `tests/test_version_cli.c` testing:
1. Running `./bin/ps5-youtube-host --version` prints `ps5-yt-uploader v1.0.0` and exits 0.
2. Running `./bin/ps5-youtube-host -v` prints `ps5-yt-uploader v1.0.0` and exits 0.
3. Verification that `src/version.h` constants match expected strings.

- [ ] **Step 2: Run test to verify it fails**

Run: `make test_version` (or compile and execute `tests/test_version_cli.c`)
Expected: FAIL due to missing `src/version.h` or unrecognized `--version` argument.

- [ ] **Step 3: Implement `src/version.h` and update `src/main.c` and `src/api.c`**

1. Create `src/version.h` with `APP_VERSION`, `APP_NAME`, and `APP_DISPLAY_NAME`.
2. In `src/main.c`: Add `--version` and `-v` handling in argument parsing loop, printing version and returning 0. Update usage message to reflect `ps5-yt-uploader`.
3. In `src/api.c`: In `handle_api_status`, add:
   `cJSON_AddStringToObject(root, "version", APP_VERSION);`
   `cJSON_AddStringToObject(root, "app_name", APP_NAME);`
4. Update `web/index.html` and `web/app.js` header branding, run `python3 scripts/bundle_web.py` to regenerate `src/web_assets.h`.

- [ ] **Step 4: Run test to verify it passes**

Run: `make test_version && make test`
Expected: PASS with all tests passing and version flag confirmed.

- [ ] **Step 5: Commit changes**

```bash
git add src/version.h src/main.c src/api.c web/ Makefile tests/test_version_cli.c src/web_assets.h
git commit -m "feat(core): add version metadata, CLI flags, and API status versioning"
```

---

### Task 2: Payload Manager Repository Schema & Generator Script

**Files:**
- Create: `scripts/update_payload_repo.py`
- Create: `payloads.json`
- Create: `tests/test_payload_repo.py`

**Interfaces:**
- Consumes: `bin/ps5-youtube.elf`, `src/version.h`
- Produces:
  - `scripts/update_payload_repo.py`: CLI tool accepting `--version`, `--elf`, `--url`, `--output`
  - `payloads.json`: Root JSON catalog complying with `CUSTOM_REPOSITORIES.md`
  - Validations: SHA-256 64-char lowercase hex check, required fields check, duplicate prevention

- [ ] **Step 1: Write failing Python unit test for payload repository generator**

Create `tests/test_payload_repo.py` testing:
1. Creation of a fresh `payloads.json` conforming to `CUSTOM_REPOSITORIES.md` schema.
2. SHA-256 calculation matches `hashlib.sha256` in lowercase hex.
3. Updating an existing `payloads.json` updates the `ps5-yt-uploader` entry in-place without duplicating items.
4. Validation rejection if checksum is malformed or required fields are omitted.

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 tests/test_payload_repo.py`
Expected: FAIL with `FileNotFoundError: scripts/update_payload_repo.py`.

- [ ] **Step 3: Implement `scripts/update_payload_repo.py` and generate initial `payloads.json`**

1. Write `scripts/update_payload_repo.py` using standard library `json`, `hashlib`, `argparse`, `os`.
2. Compute lowercase 64-hex SHA-256 of the specified ELF file.
3. Validate all fields per `CUSTOM_REPOSITORIES.md`:
   - Top-level: `"name": "PS5 YouTube Uploader"`
   - Item: `"id": "ps5-yt-uploader"`, `"name": "PS5 YouTube Uploader"`, `"filename": "ps5-youtube.elf"`, `"url": ...`, `"version": ...`, `"category": "Utilities"`, `"checksum": ...`, `"author": "Latif Muhammad"`, `"source": "https://github.com/temp-latif07/ps5-yt-uploader"`.
4. Ensure `payloads.json` is formatted with 2-space indentation.
5. Generate initial `payloads.json` from the current compiled `bin/ps5-youtube.elf`.

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 tests/test_payload_repo.py`
Expected: PASS with 100% schema validation verified.

- [ ] **Step 5: Commit changes**

```bash
git add scripts/update_payload_repo.py tests/test_payload_repo.py payloads.json
git commit -m "feat(distribution): implement Payload Manager custom repository catalog and generator"
```

---

### Task 3: Documentation, Licensing, and GitHub Compliance

**Files:**
- Create: `LICENSE`
- Create: `README.md`

**Interfaces:**
- Consumes: Repository specifications, Payload Manager custom repository URL
- Produces:
  - `LICENSE`: Standard MIT license under Latif Muhammad
  - `README.md`: Public-facing guide covering features, installation via Payload Manager, manual ELF loading, phone pairing, DualSense usage, and legal disclaimers.

- [ ] **Step 1: Write verification test for documentation presence and compliance links**

Verify:
1. `LICENSE` exists and contains MIT text.
2. `README.md` exists and contains the exact Payload Manager source URL:
   `https://raw.githubusercontent.com/temp-latif07/ps5-yt-uploader/main/payloads.json`
3. Legal disclaimers regarding Sony Interactive Entertainment and YouTube/Google are present.

- [ ] **Step 2: Run check to verify it fails**

Run: `test -f LICENSE && test -f README.md`
Expected: FAIL (files do not exist yet).

- [ ] **Step 3: Create `LICENSE` and `README.md`**

1. Write `LICENSE` with standard MIT terms.
2. Write comprehensive `README.md`:
   - Header with title "PS5 YouTube Uploader (`ps5-yt-uploader`)" and badges.
   - Core Features list (autonomous uploads, background watcher, Media home screen tile, DualSense UI, phone QR companion).
   - Payload Manager Installation:
     1. Open Payload Manager on PS5.
     2. Go to **Settings** ➔ **Manage Sources** ➔ **Add Source**.
     3. Enter: `https://raw.githubusercontent.com/temp-latif07/ps5-yt-uploader/main/payloads.json`.
     4. Install "PS5 YouTube Uploader" and optionally toggle **Autoload**.
   - Manual ELF Installation via Netcat (`nc -w 3 <ps5-ip> 9021 < ps5-youtube.elf`).
   - Configuration & Pairing instructions (`http://<ps5-ip>:8888`).
   - GitHub ToS & Legal Disclaimer (independent homebrew, non-commercial personal backup, no reverse-engineered Sony SDKs).

- [ ] **Step 4: Run verification check to confirm compliance**

Run: `grep -q "https://raw.githubusercontent.com/temp-latif07/ps5-yt-uploader/main/payloads.json" README.md && test -s LICENSE`
Expected: PASS

- [ ] **Step 5: Commit changes**

```bash
git add LICENSE README.md
git commit -m "docs: add MIT license and comprehensive README with Payload Manager setup instructions"
```

---

### Task 4: GitHub Actions Release Workflow and Local Release Script

**Files:**
- Create: `.github/workflows/release.yml`
- Create: `scripts/release.sh`
- Modify: `Makefile:90-105`

**Interfaces:**
- Consumes: `make test`, `make ps5`, `scripts/update_payload_repo.py`, `src/version.h`
- Produces:
  - `.github/workflows/release.yml`: Automated CI release publisher
  - `scripts/release.sh`: Local release coordinator script
  - `make release`: Makefile target invoking `scripts/release.sh`

- [ ] **Step 1: Write test validating release workflow syntax and release script execution**

Create `tests/test_release_script.sh` checking:
1. `.github/workflows/release.yml` is valid YAML and includes required steps (`actions/checkout`, `softprops/action-gh-release`, `contents: write` permissions).
2. `scripts/release.sh --dry-run` verifies clean working tree, extracts version from `src/version.h`, and executes build checks without pushing.

- [ ] **Step 2: Run test to verify it fails**

Run: `bash tests/test_release_script.sh`
Expected: FAIL (files do not exist yet).

- [ ] **Step 3: Implement `.github/workflows/release.yml`, `scripts/release.sh`, and `Makefile`**

1. Create `.github/workflows/release.yml`:
   - Trigger: `push: tags: ['v*.*.*']` and `workflow_dispatch`.
   - Runner: `macos-latest`.
   - Permissions: `contents: write`.
   - Steps:
     - Checkout code with full history.
     - Install LLVM tools (`brew install llvm` if needed or use Xcode toolchain with lld).
     - Run `make test` to verify host builds and integration tests.
     - Run `make ps5` to build stripped `bin/ps5-youtube.elf`.
     - Calculate SHA-256: `shasum -a 256 bin/ps5-youtube.elf > bin/ps5-youtube.elf.sha256`.
     - Run `python3 scripts/update_payload_repo.py --version ${{ github.ref_name }} --elf bin/ps5-youtube.elf --url https://github.com/temp-latif07/ps5-yt-uploader/releases/download/${{ github.ref_name }}/ps5-youtube.elf --output payloads.json`.
     - Commit and push updated `payloads.json` back to `main`.
     - Create release using `softprops/action-gh-release@v2` attaching `bin/ps5-youtube.elf` and `bin/ps5-youtube.elf.sha256`.
2. Create `scripts/release.sh`:
   - Supports `--dry-run` for verification.
   - Verifies tests pass (`make test`).
   - Builds PS5 payload (`make ps5`).
   - Reads version from `src/version.h`.
   - Prompts or creates annotated tag `v<version>`.
   - Pushes tag to origin to trigger GitHub Actions release.
3. In `Makefile`:
   - Add `release` target running `bash scripts/release.sh`.
   - Add `test_version` target.
   - Include Python test in `make test`.

- [ ] **Step 4: Run test to verify it passes**

Run: `bash tests/test_release_script.sh && make test`
Expected: PASS with dry-run release simulation verified.

- [ ] **Step 5: Commit changes**

```bash
git add .github/workflows/release.yml scripts/release.sh tests/test_release_script.sh Makefile
git commit -m "ci(release): add GitHub Actions release workflow and release coordinator script"
```

---

### Task 5: End-to-End Release Pipeline Verification, Remote Setup, and Initial Release

**Files:**
- Modify: `.git/config` (or configure via `gh`)
- Execute: GitHub repository creation & initial release

**Interfaces:**
- Consumes: Clean committed branch, authenticated `gh` CLI
- Produces:
  - GitHub Repository: `https://github.com/temp-latif07/ps5-yt-uploader`
  - GitHub Release: `v1.0.0` with `ps5-youtube.elf` and `ps5-youtube.elf.sha256`
  - Live Payload Manager URL: `https://raw.githubusercontent.com/temp-latif07/ps5-yt-uploader/main/payloads.json`

- [ ] **Step 1: Run comprehensive local test suite and dry-run release**

Run: `make clean && make test && make ps5 && python3 tests/test_payload_repo.py && bash scripts/release.sh --dry-run`
Expected: All tests pass, ELF builds cleanly (154KB), checksum matches `payloads.json`.

- [ ] **Step 2: Create public GitHub repository via `gh`**

Run: `gh repo create temp-latif07/ps5-yt-uploader --public --description "PS5 YouTube Uploader: Direct console-to-YouTube gameplay captures uploader for jailbroken PS5" --source=. --remote=origin`
Expected: Repository created and `origin` remote configured.

- [ ] **Step 3: Push `main` branch to GitHub**

Run: `git push -u origin main`
Expected: `main` pushed successfully.

- [ ] **Step 4: Publish initial release `v1.0.0` via `gh release create`**

Run:
```bash
gh release create v1.0.0 bin/ps5-youtube.elf bin/ps5-youtube.elf.sha256 \
  --title "v1.0.0 - Initial Release: PS5 YouTube Uploader" \
  --notes "Initial public release of the PS5 YouTube Uploader for jailbroken PS5 consoles.

### Features
- Direct console-to-YouTube video uploads via YouTube Scotty resumable upload protocol.
- Dedicated Media category tile on PS5 home screen.
- Autonomous background capture watcher with on-screen notification popups.
- Web companion pairing dashboard (DualSense and mobile friendly).
- Compatible with PS5 Payload Manager."
```
Expected: GitHub Release `v1.0.0` created with assets attached.

- [ ] **Step 5: Verify live Payload Manager catalog download from GitHub**

Run: `curl -sSL https://raw.githubusercontent.com/temp-latif07/ps5-yt-uploader/main/payloads.json | python3 -m json.tool`
Expected: Valid JSON returned matching the published release and SHA-256 checksum.
