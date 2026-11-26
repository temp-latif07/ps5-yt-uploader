#!/usr/bin/env python3
"""
scripts/update_payload_repo.py
Payload Manager Custom Repository Catalog Generator and Validator.

Maintains payloads.json in accordance with the itsPLK/ps5-payload-manager
CUSTOM_REPOSITORIES.md specification. Computes SHA-256 checksums, validates
schema constraints, and performs idempotent catalog updates.
"""

import argparse
import datetime
import hashlib
import json
import os
import re
import sys
from typing import Any, Dict, List, Optional

DEFAULT_REPO_NAME = "PS5 YouTube Uploader"
DEFAULT_PAYLOAD_ID = "ps5-yt-uploader"
DEFAULT_PAYLOAD_NAME = "PS5 YouTube Uploader"
DEFAULT_FILENAME = "ps5-yt-uploader.elf"
DEFAULT_CATEGORY = "Utilities"
DEFAULT_AUTHOR = "Latif Muhammad"
DEFAULT_SOURCE = "https://github.com/temp-latif07/ps5-yt-uploader"
DEFAULT_DESCRIPTION = (
    "Autonomous YouTube video uploader for PS5 captures with Media home screen tile "
    "and web companion."
)

REQUIRED_PAYLOAD_FIELDS = (
    "id",
    "name",
    "filename",
    "url",
    "version",
    "category",
    "checksum",
    "author",
    "source",
)


def extract_version_from_header(header_path: str) -> Optional[str]:
    """Extracts APP_VERSION string constant from a C header file (e.g. src/version.h)."""
    if not os.path.exists(header_path):
        return None
    with open(header_path, "r", encoding="utf-8") as f:
        content = f.read()
    match = re.search(r'#define\s+APP_VERSION\s+"([^"]+)"', content)
    if match:
        return match.group(1).strip()
    return None


def compute_sha256(filepath: str) -> str:
    """Calculates lowercase 64-hex character SHA-256 hash of a file."""
    if not os.path.isfile(filepath):
        raise FileNotFoundError(f"Target ELF file not found: {filepath}")

    hasher = hashlib.sha256()
    with open(filepath, "rb") as f:
        while chunk := f.read(65536):
            hasher.update(chunk)

    return hasher.hexdigest().lower()


def validate_checksum(checksum: Any) -> bool:
    """
    Validates that a checksum is strictly 64 lowercase hexadecimal characters.
    Raises ValueError if malformed.
    """
    if not isinstance(checksum, str):
        raise ValueError(f"Checksum must be a string, got {type(checksum).__name__}")
    if len(checksum) != 64:
        raise ValueError(
            f"Checksum must be exactly 64 characters in length (got {len(checksum)}): {checksum}"
        )
    if not re.match(r"^[0-9a-f]{64}$", checksum):
        raise ValueError(
            f"Checksum must strictly contain lowercase hexadecimal characters [0-9a-f]: {checksum}"
        )
    return True


def validate_payload_entry(entry: Dict[str, Any]) -> bool:
    """
    Validates a single payload object according to CUSTOM_REPOSITORIES.md schema.
    Raises ValueError if invalid.
    """
    if not isinstance(entry, dict):
        raise ValueError(f"Payload entry must be a dictionary, got {type(entry).__name__}")

    for field in REQUIRED_PAYLOAD_FIELDS:
        if field not in entry:
            raise ValueError(f"Payload entry missing required field: '{field}'")
        val = entry[field]
        if not isinstance(val, str) or not val.strip():
            raise ValueError(f"Payload field '{field}' must be a non-empty string, got: {val!r}")

    # Validate checksum formatting
    validate_checksum(entry["checksum"])

    # Validate URLs
    for url_field in ("url", "source"):
        val = entry[url_field]
        if not (val.startswith("http://") or val.startswith("https://")):
            raise ValueError(f"Field '{url_field}' must begin with http:// or https://: {val}")

    return True


def validate_repo_catalog(catalog: Dict[str, Any]) -> bool:
    """
    Validates the top-level repository catalog JSON object.
    Raises ValueError if invalid.
    """
    if not isinstance(catalog, dict):
        raise ValueError(f"Catalog root must be a JSON object, got {type(catalog).__name__}")

    if "name" not in catalog or not isinstance(catalog["name"], str) or not catalog["name"].strip():
        raise ValueError("Catalog must contain a non-empty string 'name' at root")

    if "payloads" not in catalog or not isinstance(catalog["payloads"], list):
        raise ValueError("Catalog must contain a 'payloads' array at root")

    seen_ids = set()
    for idx, entry in enumerate(catalog["payloads"]):
        try:
            validate_payload_entry(entry)
        except ValueError as e:
            raise ValueError(f"Invalid payload at index {idx} ('{entry.get('id', 'unknown')}'): {e}")

        payload_id = entry["id"]
        if payload_id in seen_ids:
            raise ValueError(f"Duplicate payload id found in catalog: '{payload_id}'")
        seen_ids.add(payload_id)

    return True


def update_payload_catalog(
    elf_path: str,
    version: Optional[str] = None,
    url: Optional[str] = None,
    output_path: str = "payloads.json",
    repo_name: str = DEFAULT_REPO_NAME,
    payload_id: str = DEFAULT_PAYLOAD_ID,
    payload_name: str = DEFAULT_PAYLOAD_NAME,
    filename: str = DEFAULT_FILENAME,
    category: str = DEFAULT_CATEGORY,
    author: str = DEFAULT_AUTHOR,
    source: str = DEFAULT_SOURCE,
    description: str = DEFAULT_DESCRIPTION,
    date_str: Optional[str] = None,
) -> Dict[str, Any]:
    """
    Updates or creates payloads.json with the given ELF payload entry in an idempotent manner.
    """
    # Resolve version
    if not version:
        version_header = os.path.join(
            os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "src", "version.h"
        )
        version = extract_version_from_header(version_header)

    if not version:
        raise ValueError("Version string could not be determined. Please supply --version.")

    version = version.strip()
    if not version.startswith("v"):
        version = f"v{version}"

    # Resolve filename & url
    if not filename:
        filename = DEFAULT_FILENAME

    if not url:
        url = f"https://github.com/temp-latif07/ps5-yt-uploader/releases/download/{version}/{filename}"

    if not date_str:
        date_str = datetime.date.today().isoformat()

    # Compute checksum
    checksum = compute_sha256(elf_path)
    validate_checksum(checksum)

    # Load existing catalog if present
    catalog: Dict[str, Any] = {"name": repo_name, "payloads": []}
    if os.path.exists(output_path):
        try:
            with open(output_path, "r", encoding="utf-8") as f:
                loaded = json.load(f)
                if isinstance(loaded, dict):
                    catalog = loaded
        except Exception as e:
            print(f"Warning: Failed to parse existing {output_path} ({e}), creating fresh catalog.")

    if "name" not in catalog or not catalog["name"]:
        catalog["name"] = repo_name

    if "payloads" not in catalog or not isinstance(catalog["payloads"], list):
        catalog["payloads"] = []

    # Build payload entry
    new_entry: Dict[str, Any] = {
        "id": payload_id,
        "name": payload_name,
        "filename": filename,
        "url": url,
        "description": description,
        "version": version,
        "last_update": date_str,
        "category": category,
        "checksum": checksum,
        "author": author,
        "source": source,
    }

    validate_payload_entry(new_entry)

    # In-place update or append
    existing_index = None
    for idx, item in enumerate(catalog["payloads"]):
        if isinstance(item, dict) and item.get("id") == payload_id:
            existing_index = idx
            break

    if existing_index is not None:
        # Preserve any custom fields not in new_entry, then update standard fields
        catalog["payloads"][existing_index].update(new_entry)
    else:
        catalog["payloads"].append(new_entry)

    # Validate entire catalog
    validate_repo_catalog(catalog)

    # Write formatted catalog with 2-space indentation and trailing newline
    output_dir = os.path.dirname(os.path.abspath(output_path))
    os.makedirs(output_dir, exist_ok=True)
    with open(output_path, "w", encoding="utf-8") as f:
        json.dump(catalog, f, indent=2)
        f.write("\n")

    return catalog


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Update or validate PS5 Payload Manager custom repository catalog (payloads.json)."
    )
    parser.add_argument(
        "--version",
        dest="version",
        help="Release version string (e.g., v1.0.0). Defaults to src/version.h",
    )
    parser.add_argument(
        "--elf",
        dest="elf",
        default="bin/ps5-yt-uploader.elf",
        help="Path to compiled ELF binary (default: bin/ps5-yt-uploader.elf)",
    )
    parser.add_argument(
        "--url",
        dest="url",
        help="Direct download URL for the ELF asset in GitHub Releases",
    )
    parser.add_argument(
        "--output",
        dest="output",
        default="payloads.json",
        help="Output path for repository catalog (default: payloads.json)",
    )
    parser.add_argument(
        "--check",
        dest="check_path",
        metavar="CATALOG_JSON",
        help="Validate an existing payloads.json file against schema and exit",
    )

    args = parser.parse_args()

    if args.check_path:
        if not os.path.exists(args.check_path):
            print(f"Error: Catalog file not found: {args.check_path}", file=sys.stderr)
            return 1
        try:
            with open(args.check_path, "r", encoding="utf-8") as f:
                catalog_data = json.load(f)
            validate_repo_catalog(catalog_data)
            print(f"Catalog {args.check_path} is valid.")
            return 0
        except Exception as e:
            print(f"Validation error: {e}", file=sys.stderr)
            return 1

    try:
        catalog = update_payload_catalog(
            elf_path=args.elf,
            version=args.version,
            url=args.url,
            output_path=args.output,
        )
        updated_item = next(p for p in catalog["payloads"] if p["id"] == DEFAULT_PAYLOAD_ID)
        print(
            f"Successfully updated {args.output}:\n"
            f"  ID:       {updated_item['id']}\n"
            f"  Version:  {updated_item['version']}\n"
            f"  Checksum: {updated_item['checksum']}\n"
            f"  URL:      {updated_item['url']}"
        )
        return 0
    except Exception as e:
        print(f"Error updating payload repository: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
