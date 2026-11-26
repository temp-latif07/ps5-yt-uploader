#!/usr/bin/env python3
"""Extracts human-readable release notes for a tag from CHANGELOG.md."""
import re
import sys
from pathlib import Path


def extract_release_notes(changelog_path: Path, tag: str) -> str:
    if not changelog_path.exists():
        return f"Release {tag} of PS5 YouTube Uploader."
    content = changelog_path.read_text(encoding="utf-8")
    version = tag.lstrip("v").strip()
    match = re.search(rf"^##\s+v?{re.escape(version)}\b.*$", content, flags=re.MULTILINE)
    if not match:
        return f"Release {tag} of PS5 YouTube Uploader."
    start = match.end()
    nxt = re.search(r"^(##\s+|---)", content[start:], flags=re.MULTILINE)
    notes = content[start:start + nxt.start()] if nxt else content[start:]
    return notes.strip() or f"Release {tag} of PS5 YouTube Uploader."


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: extract_release_notes.py <tag> [CHANGELOG.md]", file=sys.stderr)
        return 1
    tag = sys.argv[1]
    path = Path(sys.argv[2]) if len(sys.argv) > 2 else Path("CHANGELOG.md")
    print(extract_release_notes(path, tag))
    return 0


if __name__ == "__main__":
    sys.exit(main())
