# Design Document: PS5 YT Uploader GitHub Releases & Payload Manager Integration

## 1. Intent and The Why
The PS5 YouTube Uploader (`ps5-yt-uploader`) enables jailbroken PlayStation 5 consoles (firmware 12.20 / y2JB Relapse exploit chain) to autonomously upload gameplay video captures to YouTube without PSN access, Google Cloud developer portals, or credit cards.

To make the utility easily installable, maintainable, and updateable for the PS5 homebrew community, it requires:
1. An official, automated release pipeline on GitHub that builds the stripped `ps5-youtube.elf` payload, computes SHA-256 verification hashes, and publishes official GitHub Releases.
2. First-class integration with the standard PS5 Payload Manager (`itsPLK/ps5-payload-manager`), allowing users to browse, install into `/data/payloads/`, receive update notifications, and autoload the uploader directly from the console's web dashboard.
3. Strict adherence to GitHub Terms of Service and Acceptable Use Policies (no copyright violations, no proprietary binary blobs, clean-room POSIX code).

---

## 2. Goals, Non-Goals, and Anti-Goals

### Goals
- **GitHub Release Automation:** Automated GitHub Actions workflow triggered on version tags (`v*.*.*`) or manual dispatch to cross-compile `ps5-youtube.elf` for FreeBSD 12, calculate SHA-256 checksums, and publish release assets with release notes.
- **Payload Manager Custom Repository (`payloads.json`):** Provide a fully compliant JSON catalog following the `itsPLK/ps5-payload-manager` `CUSTOM_REPOSITORIES.md` specification, served directly from the canonical `main` branch:
  `https://raw.githubusercontent.com/temp-latif07/ps5-yt-uploader/main/payloads.json`
- **TOS & GitHub Policy Compliance:** Ensure 100% clean-room open-source code (MIT license), with zero proprietary Sony binaries/keys, zero circumvention of DRM, and full legal disclaimers.
- **Project Renaming & Documentation:** Rename repository branding to `ps5-yt-uploader`, and create a user-friendly `README.md` covering architecture, installation via Payload Manager or ELF loader, DualSense controller navigation, and configuration.
- **Version Awareness & CLI Flags:** Add `--version` / `-v` flag and include version metadata in `/api/status` response and the dashboard UI.
- **Local Release Tooling:** Provide a local `make release` / `scripts/release.sh` utility to build and publish releases directly via `gh` CLI when needed.

### Non-Goals
- **Hosting Custom Web Storefronts:** We will not maintain external cloud infrastructure or dedicated web servers; the GitHub raw CDN and GitHub Releases will serve as the sole reliable distribution layer.
- **Auto-Updating In-Place Daemon:** The running C payload will not overwrite its own running binary in memory while executing; updates are cleanly managed by Payload Manager downloading the updated ELF to `/data/payloads/ps5-youtube.elf` and restarting the daemon.

### Anti-Goals
- **No Private or Gated Distribution:** The repository and catalog will remain public, ensuring zero-cost GitHub Actions minutes and eliminating authentication token barriers for console HTTP requests.
- **No Proprietary Sony Header / Library Dependencies:** No Sony SDK files or static archives will be included or linked.

---

## 3. Constraints & Platform Requirements
- **Target Architecture:** ELF 64-bit LSB PIE executable, x86-64, FreeBSD 12 kernel target (`-target x86_64-unknown-freebsd12`).
- **Binary Footprint:** Stripped ELF payload must stay lightweight (< 700 KB, currently ~154 KB).
- **Payload Manager Catalog Spec:** Must conform strictly to `itsPLK/ps5-payload-manager` `CUSTOM_REPOSITORIES.md`:
  - Fields: `name`, `payloads` array.
  - Payload object: `id`, `name`, `filename`, `url`, `description`, `version`, `category`, `checksum` (64-char lowercase hex SHA-256), `author`, `source`.
- **GitHub Organization / User:** `temp-latif07` (`ps5-yt-uploader`).

---

## 4. Approaches Considered & Selected Architecture

### Approach 1: GitHub Actions with Canonical `main` Branch Catalog [SELECTED]
- **Description:** A GitHub Actions workflow compiles the release ELF on tag push, computes the SHA-256 hash, runs binary validation tests, creates the GitHub Release, and commits the updated `payloads.json` to the `main` branch.
- **Catalog Source URL:** `https://raw.githubusercontent.com/temp-latif07/ps5-yt-uploader/main/payloads.json`
- **Evaluation:** Selected for maximum reliability. Raw GitHub URLs return direct `200 OK` responses without multi-hop HTTP 302 redirects (which can fail on minimal PS5 network clients). It requires zero third-party hosting and 0 cost.

### Approach 2: GitHub Pages Catalog Hosting [REJECTED]
- **Description:** Deploy `payloads.json` to `gh-pages` branch.
- **Evaluation:** Unnecessary configuration overhead and extra branch maintenance for a single JSON file when GitHub's raw CDN already provides high-speed, direct HTTPS delivery.

### Approach 3: Release-Asset Only Catalog [REJECTED]
- **Description:** Attach `payloads.json` as an asset to GitHub Releases only.
- **Evaluation:** URLs like `.../releases/latest/download/payloads.json` perform HTTP 302 redirects to Amazon S3 CDN URLs, causing intermittent failures on constrained embedded HTTP clients on the console.

---

## 5. Implementation Specifications

### 5.1 Repository Setup & Compliance
- **Name:** `ps5-yt-uploader`
- **License:** MIT License (`LICENSE`)
- **README:** `README.md` covering:
  - Overview, key features, and architecture diagram
  - Requirements (Jailbroken PS5, firmware 12.20 / 7.xx-13.xx, y2JB Relapse)
  - Installation via PS5 Payload Manager (1-click source addition)
  - Manual ELF installation via Netcat / ELF loader (`port 9021`)
  - Web companion pairing instructions (`http://<ps5-ip>:8888`)
  - Legal disclaimer & YouTube Terms compliance statement

### 5.2 Versioning & Application Metadata
- Define `APP_VERSION "1.0.0"` in a dedicated header `src/version.h`.
- In `src/main.c`: Add support for `-v` and `--version` options:
  ```
  ps5-yt-uploader v1.0.0 (PS5 YouTube Uploader)
  ```
- In `src/api.c`: Include `"version": APP_VERSION` in GET `/api/status` JSON response.
- In `web/app.js`: Display `v1.0.0` in the dashboard header/footer.

### 5.3 Payload Manager Repository Schema (`payloads.json`)
The catalog located at the root of `main` will strictly follow the schema:
```json
{
  "name": "PS5 YouTube Uploader",
  "payloads": [
    {
      "id": "ps5-yt-uploader",
      "name": "PS5 YouTube Uploader",
      "filename": "ps5-youtube.elf",
      "url": "https://github.com/temp-latif07/ps5-yt-uploader/releases/download/v1.0.0/ps5-youtube.elf",
      "description": "Autonomous YouTube video uploader for PS5 captures with Media home screen tile and web companion.",
      "version": "v1.0.0",
      "last_update": "2026-11-18",
      "category": "Utilities",
      "checksum": "<sha256-hex>",
      "author": "Latif Muhammad",
      "source": "https://github.com/temp-latif07/ps5-yt-uploader"
    }
  ]
}
```

### 5.4 Release Automation & CI/CD Pipeline
- **Canonical Release Publisher:** GitHub Actions (`release.yml`) is the sole canonical release publisher. When a tag `v*.*.*` is pushed, CI cross-compiles the binary, validates tests, hashes the binary, generates release assets, creates the GitHub release, and updates `payloads.json` on `main`.
- **CI Runner Environment:** CI uses `macos-latest` runner. This matches the exact tested toolchain from development (Apple SDK + LLVM `ld.lld`), avoiding Linux glibc header ABI mismatches and eliminating the need for third-party FreeBSD sysroot downloads.
- **Local Helper Script (`scripts/release.sh`):** A helper script for the developer that:
  1. Checks for a clean working tree and tests passing (`make test`).
  2. Confirms version string alignment (`src/version.h`).
  3. Creates and pushes the git tag (e.g. `git tag -a v1.0.0 -m "Release v1.0.0" && git push origin v1.0.0`), which hands off canonical building and release publishing to GitHub Actions.
  4. (Optional `--offline-local` flag provided for manual override when GitHub Actions is not reachable).

### 5.5 Builder Decisions & Resolved Review Items
- *Call 1 (CI Source of Truth):* GitHub Actions is the canonical publisher on tag push. `scripts/release.sh` prepares and pushes the tag rather than simultaneously running `gh release create`, preventing race conditions and hash mismatches in `payloads.json`.
- *Call 2 (CI Runner Selection):* `macos-latest` is chosen for the GitHub Actions runner because public repositories have free access to it and it mirrors the exact local compilation environment without needing external FreeBSD sysroots.

---

## 6. What's Left to the Builder
- Detailed YAML step configurations in `.github/workflows/release.yml`.
- Exact test assertions in `tests/test_payload_repo.py` ensuring `payloads.json` parses cleanly with cJSON and Python standard library `json`.
- Git remote configuration and initial push to GitHub.
