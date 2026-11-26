#!/usr/bin/env python3
"""Updates and validates payloads.json for itsPLK/ps5-payload-manager."""
import argparse
import datetime
import hashlib
import json
import os
import re
import sys

REPO_NAME = "PS5 YouTube Uploader"
PAYLOAD_ID = "ps5-yt-uploader"
PAYLOAD_NAME = "PS5 YouTube Uploader"
FILENAME = "ps5-yt-uploader.elf"
CATEGORY = "Utilities"
AUTHOR = "Latif Muhammad"
SOURCE = "https://github.com/temp-latif07/ps5-yt-uploader"
DESCRIPTION = "Upload PS5 gameplay clips directly to YouTube Studio via home-screen tile or web UI."
REQUIRED = ("id", "name", "filename", "url", "version", "category", "checksum", "author", "source")


def read_app_version(app_h: str) -> str:
    with open(app_h, "r", encoding="utf-8") as f:
        m = re.search(r'#define\s+APP_VERSION\s+"([^"]+)"', f.read())
    if not m:
        raise ValueError(f"APP_VERSION not found in {app_h}")
    return m.group(1).strip()


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest().lower()


def validate_catalog(cat: dict) -> None:
    if not isinstance(cat, dict) or not isinstance(cat.get("name"), str) or not cat["name"].strip():
        raise ValueError("catalog missing non-empty 'name'")
    payloads = cat.get("payloads")
    if not isinstance(payloads, list):
        raise ValueError("catalog missing 'payloads' array")
    seen = set()
    for i, item in enumerate(payloads):
        if not isinstance(item, dict):
            raise ValueError(f"payload[{i}] is not an object")
        for k in REQUIRED:
            v = item.get(k)
            if not isinstance(v, str) or not v.strip():
                raise ValueError(f"payload[{i}] missing '{k}'")
        if not re.match(r"^[0-9a-f]{64}$", item["checksum"]):
            raise ValueError(f"payload[{i}] invalid checksum: {item['checksum']}")
        for uf in ("url", "source"):
            if not item[uf].startswith(("http://", "https://")):
                raise ValueError(f"payload[{i}] invalid URL in '{uf}'")
        if item["id"] in seen:
            raise ValueError(f"duplicate id '{item['id']}'")
        seen.add(item["id"])


def main() -> int:
    ap = argparse.ArgumentParser(description="Update or validate payloads.json")
    ap.add_argument("--version", help="Release version (default: src/app.h APP_VERSION)")
    ap.add_argument("--elf", default="build/ps5-yt-uploader.elf", help="Path to compiled ELF")
    ap.add_argument("--url", help="Download URL override")
    ap.add_argument("--output", default="payloads.json", help="Output catalog path")
    ap.add_argument("--check", metavar="FILE", help="Validate existing catalog JSON and exit")
    args = ap.parse_args()

    if args.check:
        with open(args.check, "r", encoding="utf-8") as f:
            validate_catalog(json.load(f))
        print(f"ok   {args.check} valid")
        return 0

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ver = (args.version or read_app_version(os.path.join(root, "src", "app.h"))).lstrip("v")
    tag = f"v{ver}"
    url = args.url or f"{SOURCE}/releases/download/{tag}/{FILENAME}"
    checksum = sha256_file(args.elf)

    cat = {"name": REPO_NAME, "payloads": []}
    if os.path.exists(args.output):
        with open(args.output, "r", encoding="utf-8") as f:
            loaded = json.load(f)
            if isinstance(loaded, dict):
                cat = loaded
    cat["name"] = cat.get("name") or REPO_NAME
    if not isinstance(cat.get("payloads"), list):
        cat["payloads"] = []

    entry = {
        "id": PAYLOAD_ID,
        "name": PAYLOAD_NAME,
        "filename": FILENAME,
        "url": url,
        "description": DESCRIPTION,
        "version": tag,
        "last_update": datetime.date.today().isoformat(),
        "category": CATEGORY,
        "checksum": checksum,
        "author": AUTHOR,
        "source": SOURCE,
    }
    for idx, cur in enumerate(cat["payloads"]):
        if isinstance(cur, dict) and cur.get("id") == PAYLOAD_ID:
            cat["payloads"][idx].update(entry)
            break
    else:
        cat["payloads"].append(entry)

    validate_catalog(cat)
    with open(args.output, "w", encoding="utf-8") as f:
        json.dump(cat, f, indent=2)
        f.write("\n")
    print(f"updated {args.output}: {tag} ({checksum})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
