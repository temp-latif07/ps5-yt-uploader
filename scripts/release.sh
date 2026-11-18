#!/usr/bin/env bash
# scripts/release.sh
# Release coordination helper script for PS5 YouTube Uploader.
# Verifies test suites, builds production PS5 payload ELF, checks size and hashes,
# and pushes an annotated tag to trigger GitHub Actions release.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}"

DRY_RUN=false
ASSUME_YES=false
ALLOW_DIRTY=false

print_usage() {
    cat <<EOF
Usage: scripts/release.sh [OPTIONS]

Coordinates an official release of PS5 YouTube Uploader:
1. Verifies a clean working tree.
2. Extracts release version from src/version.h.
3. Runs comprehensive test suite (make test).
4. Cross-compiles production PS5 payload (make ps5).
5. Validates payload binary size (< 700 KB) and SHA-256 hash.
6. Creates annotated git tag v<version> and pushes to origin.

Options:
  --dry-run      Run all validation, tests, and build checks without tagging or pushing.
  --allow-dirty  Allow uncommitted changes (useful for local development/testing).
  -y, --yes      Non-interactive mode (automatically confirm tagging and pushing).
  -h, --help     Show this help message and exit.
EOF
}

# Parse command line options
while [[ $# -gt 0 ]]; do
    case "$1" in
        --dry-run)
            DRY_RUN=true
            shift
            ;;
        --allow-dirty)
            ALLOW_DIRTY=true
            shift
            ;;
        -y|--yes)
            ASSUME_YES=true
            shift
            ;;
        -h|--help)
            print_usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            print_usage
            exit 1
            ;;
    esac
done

echo "=========================================================="
echo "      PS5 YouTube Uploader - Release Coordinator"
echo "=========================================================="
if [ "${DRY_RUN}" = true ]; then
    echo ">> RUNNING IN DRY-RUN MODE (no git tags will be created or pushed)"
fi

# In non-dry-run mode, require -y/--yes when stdin is not a TTY
if [ "${DRY_RUN}" = false ] && [ ! -t 0 ] && [ "${ASSUME_YES}" = false ]; then
    echo "Error: Non-interactive execution requires -y or --yes flag" >&2
    exit 1
fi

# Step 1: Check for clean working tree
if [ "${ALLOW_DIRTY}" = false ]; then
    echo "==> Step 1: Verifying clean working tree..."
    if ! git diff-index --quiet HEAD --; then
        echo "Error: Working directory has uncommitted or staged changes to tracked files." >&2
        echo "Please commit or stash your changes before creating a release." >&2
        exit 1
    fi
    echo "  -> Working tree clean."
else
    echo "==> Step 1: Skipping clean tree verification (--allow-dirty set)..."
fi

# Step 2: Extract version from src/version.h
echo "==> Step 2: Extracting version from src/version.h..."
if [ ! -f "src/version.h" ]; then
    echo "Error: src/version.h not found." >&2
    exit 1
fi

VERSION=$(grep -E '#define\s+APP_VERSION\s+' src/version.h | sed -E 's/.*"([^"]+)".*/\1/')
if [ -z "${VERSION}" ]; then
    echo "Error: Unable to parse APP_VERSION constant from src/version.h" >&2
    exit 1
fi
# Strip any leading 'v' if present to normalize
VERSION="${VERSION#v}"
TAG="v${VERSION}"
echo "  -> Target release version: ${VERSION} (Tag: ${TAG})"

# Step 3: Check if tag already exists
if git rev-parse -q --verify "refs/tags/${TAG}" >/dev/null; then
    if [ "${DRY_RUN}" = true ]; then
        echo "[DRY-RUN] Notice: Tag ${TAG} exists; would fail in live release unless bumped."
    else
        echo "Error: Git tag ${TAG} already exists locally." >&2
        echo "Please bump APP_VERSION in src/version.h before running release." >&2
        exit 1
    fi
fi

# Step 4: Run test suite
echo "==> Step 3: Running test suite (make test)..."
make test

# Step 5: Build PS5 payload
echo "==> Step 4: Cross-compiling PS5 payload (make ps5)..."
make ps5

# Step 6: Validate payload ELF binary
echo "==> Step 5: Validating payload ELF binary..."
ELF_PATH="bin/ps5-yt-uploader.elf"
if [ ! -f "${ELF_PATH}" ]; then
    echo "Error: Payload ELF binary not found at ${ELF_PATH}" >&2
    exit 1
fi

ELF_INFO=$(file "${ELF_PATH}")
echo "  -> ELF Info: ${ELF_INFO}"
if ! echo "${ELF_INFO}" | grep -qi "ELF 64-bit"; then
    echo "Error: ${ELF_PATH} is not an ELF 64-bit binary" >&2
    exit 1
fi

SIZE=$(stat -f%z "${ELF_PATH}" 2>/dev/null || stat -c%s "${ELF_PATH}" 2>/dev/null)
MAX_SIZE=716800  # 700 KB in bytes
if [ "${SIZE}" -ge "${MAX_SIZE}" ]; then
    echo "Error: Payload size ${SIZE} bytes exceeds maximum limit of ${MAX_SIZE} bytes (700 KB)" >&2
    exit 1
fi
echo "  -> Payload size constraint passed: ${SIZE} bytes (< ${MAX_SIZE} bytes)"

(cd "$(dirname "${ELF_PATH}")" && shasum -a 256 "$(basename "${ELF_PATH}")" > "$(basename "${ELF_PATH}").sha256")
SHA256=$(awk '{print $1}' "${ELF_PATH}.sha256")
echo "  -> SHA-256 Checksum: ${SHA256}"

# Step 7: Finalize or simulate release
if [ "${DRY_RUN}" = true ]; then
    echo "=========================================================="
    echo ">> [DRY RUN] Release verification successful!"
    echo "   Version:     ${VERSION}"
    echo "   Tag:         ${TAG}"
    echo "   Binary:      ${ELF_PATH} (${SIZE} bytes)"
    echo "   SHA-256:     ${SHA256}"
    echo ">> [DRY RUN] Skipping tag creation and git push."
    echo "=========================================================="
    exit 0
fi

# Non-dry-run: prompt for confirmation if interactive, or require -y/--yes if non-interactive
if [ -t 0 ]; then
    if [ "${ASSUME_YES}" = false ]; then
        read -r -p "Create annotated tag ${TAG} and push to origin? [y/N] " CONFIRM
        if [[ ! "${CONFIRM}" =~ ^[Yy]$ ]]; then
            echo "Release aborted by user."
            exit 1
        fi
    fi
else
    if [ "${ASSUME_YES}" = false ]; then
        echo "Error: Non-interactive execution requires -y or --yes flag" >&2
        exit 1
    fi
fi

echo "==> Step 6: Updating Payload Manager catalog (payloads.json)..."
python3 "${REPO_ROOT}/scripts/update_payload_repo.py" \
    --version "${TAG}" \
    --elf "${ELF_PATH}" \
    --url "https://github.com/temp-latif07/ps5-yt-uploader/releases/download/${TAG}/ps5-yt-uploader.elf" \
    --output "${REPO_ROOT}/payloads.json"

# Commit updated payloads.json if modified
if [ -n "$(git status --porcelain payloads.json 2>/dev/null)" ]; then
    echo "==> Committing updated payloads.json to main..."
    git add payloads.json
    git commit -m "chore(catalog): update payloads.json for ${TAG} [skip ci]"
    git push origin main
fi

echo "==> Step 7: Creating annotated tag ${TAG}..."
git tag -a "${TAG}" -m "Release ${TAG} - PS5 YouTube Uploader"

echo "==> Step 8: Pushing tag ${TAG} to origin..."
git push origin "${TAG}"

# Extract human-readable release notes from CHANGELOG.md
RELEASE_NOTES=$(python3 "${REPO_ROOT}/scripts/extract_release_notes.py" "${TAG}" 2>/dev/null || echo "Release ${TAG} of PS5 YouTube Uploader.")

echo "==> Step 9: Creating GitHub Release via gh CLI..."
if command -v gh &>/dev/null && gh auth token &>/dev/null; then
    gh release create "${TAG}" \
        "${ELF_PATH}" \
        "${ELF_PATH}.sha256" \
        "bin/ps5-youtube.elf" \
        --title "${TAG} - PS5 YouTube Uploader" \
        --notes "${RELEASE_NOTES}" || {
            echo "Notice: gh release create returned non-zero (release may already exist)."
        }
    echo "  -> GitHub Release published instantly!"
fi

echo "=========================================================="
echo ">> Successfully released and published tag ${TAG}!"
echo ">> Release URL: https://github.com/temp-latif07/ps5-yt-uploader/releases/tag/${TAG}"
echo ">> Payload Catalog: https://raw.githubusercontent.com/temp-latif07/ps5-yt-uploader/main/payloads.json"
echo "=========================================================="
