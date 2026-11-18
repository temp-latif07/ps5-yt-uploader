#!/usr/bin/env python3
"""
Extract release notes for a specified version tag from CHANGELOG.md.
Follows the format used by phantomptr/ps5upload.
"""

import sys
import re
from pathlib import Path

def extract_release_notes(changelog_path: Path, tag: str) -> str:
    if not changelog_path.exists():
        return f"Release {tag} of PS5 YouTube Uploader."

    content = changelog_path.read_text(encoding="utf-8")
    version = tag.lstrip("v").strip()

    # Match section: ## <version> or ## v<version>
    pattern = rf"^##\s+v?{re.escape(version)}\b.*$"
    match = re.search(pattern, content, flags=re.MULTILINE)
    if not match:
        return f"Release {tag} of PS5 YouTube Uploader.\n\nSee CHANGELOG.md for detailed release notes."

    start_idx = match.end()
    # Find next section starting with ## or ---
    next_match = re.search(r"^(##\s+|---)", content[start_idx:], flags=re.MULTILINE)
    if next_match:
        notes = content[start_idx:start_idx + next_match.start()]
    else:
        notes = content[start_idx:]

    notes = notes.strip()
    return notes if notes else f"Release {tag} of PS5 YouTube Uploader."

def main():
    if len(sys.argv) < 2:
        print("Usage: extract_release_notes.py <tag> [changelog_path]", file=sys.stderr)
        sys.exit(1)

    tag = sys.argv[1]
    changelog_path = Path(sys.argv[2]) if len(sys.argv) > 2 else Path("CHANGELOG.md")
    notes = extract_release_notes(changelog_path, tag)
    print(notes)

if __name__ == "__main__":
    main()
