# Releasing

This document describes the release process for **PS5 YouTube Uploader (`ps5-yt-uploader`)**, modeled after the disciplined maintenance standards of `phantomptr/ps5upload`.

A release publishes an official cross-compiled FreeBSD 12 ELF payload, generates SHA-256 integrity checksums, updates the live PS5 Payload Manager custom repository catalog (`payloads.json`), and publishes human-readable release notes.

---

## 1. Release Principles

1. **Written for Humans:** Release notes in `CHANGELOG.md` and GitHub Releases are written for end users, not raw git commit dumps. Every release categorizes changes into:
   - `**New**` – User-facing features, enhancements, and UI additions.
   - `**Fixes**` – Bug resolutions, crash fixes, error handling improvements.
   - `**Behind the scenes**` – Architectural refactoring, test additions, CI/CD updates.
2. **Single Source of Truth:**
   - C Code: `src/version.h` (`APP_VERSION`, `APP_NAME`, `APP_DISPLAY_NAME`).
   - Catalog: `payloads.json` (`version`, `checksum`, `url`).
   - Changelog: `CHANGELOG.md` (`## X.Y.Z`).
   - Git Tag: `vX.Y.Z`.
3. **Automated Verification Gates:** A release is NEVER tagged or published unless all quality gates pass:
   - Unit and integration tests: `make test` (10 test suites).
   - Payload Manager schema verification: `python3 tests/test_payload_repo.py` (9 tests).
   - Release script dry-run: `bash scripts/release.sh --dry-run`.
4. **Zero Proprietary Blobs:** Releases contain strictly clean-room C99 binaries compiled against open POSIX interfaces.

---

## 2. Standard Release Procedure

### Step 1: Update the Changelog
Move changes from `## Unreleased` to a new section `## X.Y.Z` at the top of `CHANGELOG.md`:

```markdown
## Unreleased

---

## 1.0.1

**Fixes**
- **Scotty resumable chunk retry:** Fixed an edge case where transient Wi-Fi drops on PS5 could stall socket timeouts.

**Behind the scenes**
- Updated CI runner toolchain detection for Homebrew LLD linker.
```

### Step 2: Bump Version in Code
Update `APP_VERSION` in `src/version.h`:

```c
#define APP_VERSION "1.0.1"
```

Rebuild web assets if web files were touched:
```bash
python3 scripts/bundle_web.py
```

### Step 3: Run Verification Gates
Run the full automated verification test suite:

```bash
make test && bash tests/test_release_script.sh
```

Ensure git working tree is clean:
```bash
git status
```

Commit the version bump and changelog:
```bash
git add src/version.h CHANGELOG.md src/web_assets.h
git commit -m "chore(release): prepare v1.0.1"
git push origin main
```

### Step 4: Tag & Trigger Release
Run the release coordinator script:

```bash
make release        # or: bash scripts/release.sh
```

This will:
1. Verify working tree cleanliness.
2. Confirm all tests pass.
3. Cross-compile the PS5 ELF payload (`make ps5`).
4. Validate binary properties (ELF 64-bit, size < 700 KB, SHA-256 hash).
5. Prompt for confirmation.
6. Create an annotated git tag `v1.0.1` and push it to `origin`.

---

## 3. Instant Local Release Architecture

To avoid slow 5–15 minute cloud runner queues on GitHub Actions, releases are orchestrated and published directly using `make release` (or `bash scripts/release.sh`), following the `make fast-release` model from `phantomptr/ps5upload`:

1. Verifies working tree cleanliness and test gates.
2. Cross-compiles `bin/ps5-yt-uploader.elf` in ~1.5 seconds.
3. Calculates SHA-256 checksum and saves `bin/ps5-yt-uploader.elf.sha256`.
4. Extracts human-readable release notes directly from `CHANGELOG.md` via `scripts/extract_release_notes.py`.
5. Updates root `payloads.json` with the exact release URL, tag, and SHA-256 hash.
6. Commits updated `payloads.json` and pushes to `main`.
7. Creates and pushes annotated git tag `vX.Y.Z`.
8. Publishes the official GitHub Release instantly via `gh release create` with attached assets:
   - `ps5-yt-uploader.elf` (primary payload)
   - `ps5-yt-uploader.elf.sha256` (checksum)
   - `ps5-youtube.elf` (compatibility symlink)

---

## 4. Post-Release Verification

After release publication, verify live distribution endpoints:

```bash
# 1. Verify GitHub Release exists
gh release view v1.0.1

# 2. Verify live Payload Manager catalog JSON
curl -sSL https://raw.githubusercontent.com/temp-latif07/ps5-yt-uploader/main/payloads.json | python3 -m json.tool

# 3. Verify published binary matches catalog checksum
curl -sSL https://github.com/temp-latif07/ps5-yt-uploader/releases/download/v1.0.1/ps5-yt-uploader.elf | shasum -a 256
```
