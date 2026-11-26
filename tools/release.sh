#!/usr/bin/env bash
# Fast local release workflow for PS5 YouTube Uploader.
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$here"

DRY_RUN=false
ASSUME_YES=false
ALLOW_DIRTY=false

while [[ $# -gt 0 ]]; do
    case "$1" in
    --dry-run) DRY_RUN=true; shift ;;
    -y|--yes) ASSUME_YES=true; shift ;;
    --allow-dirty) ALLOW_DIRTY=true; shift ;;
    -h|--help)
        echo "usage: tools/release.sh [--dry-run] [--allow-dirty] [-y|--yes]"
        exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

if [[ $DRY_RUN == false && ! -t 0 && $ASSUME_YES == false ]]; then
    echo "Error: non-interactive release requires -y or --yes" >&2
    exit 1
fi

if [[ $ALLOW_DIRTY == false && -n "$(git --no-optional-locks status --porcelain)" ]]; then
    echo "Error: working tree is not clean (commit or stash changes first)" >&2
    exit 1
fi

VERSION=$(sed -n 's/^#define APP_VERSION "\(.*\)"/\1/p' src/app.h)
[[ -n $VERSION ]] || { echo "Error: cannot read APP_VERSION from src/app.h" >&2; exit 1; }
TAG="v$VERSION"

if git rev-parse -q --verify "refs/tags/$TAG" >/dev/null; then
    if [[ $DRY_RUN == true ]]; then
        echo "[dry-run] notice: tag $TAG already exists locally"
    else
        echo "Error: git tag $TAG already exists" >&2
        exit 1
    fi
fi

echo "==> Running host test suite..."
bash tests/test_host.sh

if [[ $DRY_RUN == true ]]; then
    python3 tools/update_payloads.py --check payloads.json
    python3 tools/extract_release_notes.py "$TAG" >/dev/null
    echo "[dry-run] release checks passed for $TAG"
    exit 0
fi

echo "==> Building PS5 payload, title, and package..."
./build.sh all

ELF=build/ps5-yt-uploader.elf
ZIP="build/ps5-yt-uploader-${VERSION}.zip"
file "$ELF" | grep -qi "ELF 64-bit"

python3 tools/update_payloads.py --version "$TAG" --elf "$ELF" --output payloads.json
if [[ -n "$(git status --porcelain payloads.json)" ]]; then
    git add payloads.json
    git commit -m "chore(catalog): update payloads.json for $TAG"
    git push origin main
fi

if [[ -t 0 && $ASSUME_YES == false ]]; then
    read -r -p "Create annotated tag $TAG and publish GitHub release? [y/N] " ans
    [[ $ans =~ ^[Yy]$ ]] || { echo "Aborted."; exit 1; }
fi

git tag -a "$TAG" -m "Release $TAG - PS5 YouTube Uploader"
git push origin "$TAG"
NOTES=$(python3 tools/extract_release_notes.py "$TAG")

if command -v gh >/dev/null 2>&1 && gh auth token >/dev/null 2>&1; then
    gh release create "$TAG" "$ELF" "$ZIP" --title "$TAG - PS5 YouTube Uploader" --notes "$NOTES"
fi
echo "Released $TAG"
