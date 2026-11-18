# AGENTS.md — Repository Rules & Maintenance Guidelines

This document defines the development, maintenance, and release standards for **PS5 YouTube Uploader (`ps5-yt-uploader`)**. All AI assistants, autonomous agents, and human contributors working in this repository MUST follow these rules.

These rules adapt the disciplined software engineering and release practices of [`phantomptr/ps5upload`](https://github.com/phantomptr/ps5upload) to our native C PS5 homebrew ecosystem.

---

## 1. Repository Identity & Core Constraints

- **Project Name:** `ps5-yt-uploader`
- **Primary Binary Artifact:** `bin/ps5-yt-uploader.elf` (FreeBSD 12 64-bit ELF PIE executable).
- **Compatibility Alias:** `bin/ps5-youtube.elf` (maintained as a build symlink and secondary release asset).
- **Target Platform:** PlayStation 5 (firmware 12.20 / compatible with 7.00–13.60 kernel exploit range).
- **Clean-Room Guarantee:** 100% open-source C99 and standard POSIX interfaces. Zero proprietary Sony SDK libraries, leaked decryption keys, or DRM circumvention code.
- **Resource Limits:** Stripped ELF payload must stay lightweight (< 700 KB, typically ~154 KB). Background daemon memory footprint must remain under 4 MB RAM and < 0.1% CPU at idle.

---

## 2. Release & Changelog Standards (The ps5upload Model)

We strictly adopt the human-readable changelog and automated release process inspired by `phantomptr/ps5upload`:

### 2.1 Human-Readable Release Notes (`CHANGELOG.md`)
- **No Git Commit Dumps:** Never use auto-generated git commit lists or PR titles as release notes. Release notes are written for humans who use the console.
- **Structure:**
  - `## Unreleased` sits permanently at the top of `CHANGELOG.md`. Every user-facing feature or bugfix added in development MUST include a bullet under `## Unreleased`.
  - When cutting a release, `## Unreleased` changes are organized under a new heading `## X.Y.Z` (e.g. `## 1.0.1`).
  - Entries are categorized into:
    - `**New**` — User-facing capabilities, UI enhancements, or new options.
    - `**Fixes**` — Bug resolutions, network drop handling, crash prevention. Explain what failed and what the software does now.
    - `**Behind the scenes**` — Internal refactoring, test additions, CI/CD pipeline changes.
- **Automated Extraction:** The release script extracts notes for the specific tag directly from `CHANGELOG.md` via `scripts/extract_release_notes.py`.

### 2.2 Version Synchronization
All version references across the project must remain synchronized:
1. `src/version.h`: `#define APP_VERSION "X.Y.Z"` (source of truth).
2. `CHANGELOG.md`: Section heading `## X.Y.Z`.
3. `payloads.json`: `"version": "vX.Y.Z"` and release URL tag.
4. Git Tag: `vX.Y.Z`.

### 2.3 Quality Gates Before Release
A release must NEVER be tagged or published without passing all verification gates:
```bash
make test                          # All 10 host test suites pass
python3 tests/test_payload_repo.py  # 9 schema & checksum validation tests pass
bash tests/test_release_script.sh  # Release script & workflow validation tests pass
bash scripts/release.sh --dry-run  # Dry-run release simulation passes cleanly
```

### 2.4 Release Execution
- **Instant Local Releases:** Run `make release` or `bash scripts/release.sh`. It enforces a clean git status, runs tests, compiles the payload in 1.5s, verifies binary properties, updates `payloads.json`, creates an annotated git tag `vX.Y.Z`, and publishes the release directly to GitHub via `gh release create` with binaries, checksums, and notes extracted from `CHANGELOG.md` in under 5 seconds.
- **No Cloud Queue Bottlenecks:** To avoid slow GitHub Actions runner queues, releases are orchestrated and published locally from the developer environment using the `phantomptr/ps5upload` fast-release model.
- **Non-Interactive Safety:** `scripts/release.sh` explicitly refuses to run in non-interactive environments without an explicit `-y` / `--yes` flag to prevent accidental automated tagging.

---

## 3. PS5 Payload Manager Integration Rules

The repository provides first-class support for `itsPLK/ps5-payload-manager`:
1. **Catalog Specification:** `payloads.json` at the repository root strictly follows `CUSTOM_REPOSITORIES.md`:
   - Top-level: `"name": "PS5 YouTube Uploader"`, `"payloads"` array.
   - Item fields: `id` (`ps5-yt-uploader`), `name`, `filename` (`ps5-yt-uploader.elf`), `url`, `version`, `category` (`Utilities`), `checksum`, `author` (`Latif Muhammad`), `source`.
2. **Checksum Integrity:** Checksums must always be lowercase 64-character hexadecimal SHA-256 strings (`^[0-9a-f]{64}$`). Uppercase characters or length errors will cause PS5 Payload Manager download rejections.
3. **Canonical Source URL:**
   `https://raw.githubusercontent.com/temp-latif07/ps5-yt-uploader/main/payloads.json`
4. **Idempotent Updates:** `scripts/update_payload_repo.py` updates entries in-place matching on `id` without creating duplicate items.

---

## 4. Code Architecture & Development Guidelines

1. **Idiomatic C99:** Code must compile cleanly with `-Wall -Wextra -std=c99` with zero warnings.
2. **Cross-Compilation Linker:** PS5 FreeBSD 12 targets require LLVM's `ld.lld` (`-fuse-ld=... -nostdlib -Wl,-pie -Wl,-e,main`). The `Makefile` dynamically locates `ld.lld` from Homebrew or system paths and aborts with an informative error if `ld.lld` is missing.
3. **Robust Error Handling:**
   - Network operations (Scotty resumable uploads, InnerTube API) must gracefully handle dropped sockets and HTTP timeouts, resuming from byte offsets without re-uploading completed chunks.
   - All REST API routes in `src/api.c` must bound incoming payloads (max 64 KB) to avoid buffer overflow vulnerabilities.
   - Ignore `SIGPIPE` so socket disconnects from WebKit or network drops never terminate the daemon.
4. **Web Companion Assets:** Changes to files in `web/` (`index.html`, `style.css`, `app.js`) require regenerating `src/web_assets.h` using `python3 scripts/bundle_web.py`.
5. **No Blind Symptom Fixes:** When debugging CI, build, or runtime issues, follow the `systematic-debugging` discipline: find the root cause before proposing fixes.

---

## 5. Security & Git Hygiene

1. **Credentials & Secrets:** Never commit cookies, tokens, or private secrets (`cookies.txt`, `oauth_*.json`, `client_secret*.json`). These are listed in `.gitignore`.
2. **Conventional Commits:** Use structured commit messages:
   - `feat(...)`: New features
   - `fix(...)`: Bugfixes
   - `docs(...)`: Documentation updates
   - `ci(...)`: GitHub Actions and build system updates
   - `chore(...)`: Maintenance and dependency updates
3. **Clean Diffs:** Always inspect `git diff` before committing to avoid committing scratch files, temporary binaries, or untracked test output.
