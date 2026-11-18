#!/usr/bin/env python3
"""
tests/test_payload_repo.py
Unit and integration tests for Payload Manager repository generator script.
Tests catalog schema conformity, SHA-256 checksum formatting, idempotency,
and validation rejection rules.
"""

import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import unittest

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT_PATH = os.path.join(REPO_ROOT, "scripts", "update_payload_repo.py")

# Ensure scripts directory can be imported
if REPO_ROOT not in sys.path:
    sys.path.insert(0, REPO_ROOT)

# Require scripts/update_payload_repo.py to exist
if not os.path.exists(SCRIPT_PATH):
    raise FileNotFoundError(f"scripts/update_payload_repo.py not found at {SCRIPT_PATH}")

from scripts.update_payload_repo import (
    compute_sha256,
    validate_checksum,
    validate_payload_entry,
    validate_repo_catalog,
    update_payload_catalog,
    extract_version_from_header,
    REQUIRED_PAYLOAD_FIELDS,
)


class TestPayloadRepoGenerator(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.test_elf = os.path.join(self.temp_dir.name, "test_payload.elf")
        with open(self.test_elf, "wb") as f:
            f.write(b"\x7fELF\x02\x01\x01\x00ps5-yt-uploader-mock-payload-bytes")
        self.output_json = os.path.join(self.temp_dir.name, "payloads.json")

    def tearDown(self):
        self.temp_dir.cleanup()

    def test_sha256_calculation(self):
        """Verify SHA-256 calculation matches hashlib and is strictly 64 lowercase hex characters."""
        calculated_hash = compute_sha256(self.test_elf)
        with open(self.test_elf, "rb") as f:
            expected_hash = hashlib.sha256(f.read()).hexdigest().lower()

        self.assertEqual(calculated_hash, expected_hash)
        self.assertEqual(len(calculated_hash), 64)
        self.assertTrue(re.match(r"^[0-9a-f]{64}$", calculated_hash))
        self.assertFalse(any(c.isupper() for c in calculated_hash))

    def test_fresh_catalog_creation(self):
        """Verify creation of a fresh payloads.json conforming to CUSTOM_REPOSITORIES.md schema."""
        catalog = update_payload_catalog(
            elf_path=self.test_elf,
            version="v1.0.0",
            url="https://github.com/temp-latif07/ps5-yt-uploader/releases/download/v1.0.0/ps5-youtube.elf",
            output_path=self.output_json,
        )

        # Validate in-memory catalog
        validate_repo_catalog(catalog)

        # Check top-level schema
        self.assertEqual(catalog["name"], "PS5 YouTube Uploader")
        self.assertIn("payloads", catalog)
        self.assertEqual(len(catalog["payloads"]), 1)

        # Check payload entry item
        item = catalog["payloads"][0]
        self.assertEqual(item["id"], "ps5-yt-uploader")
        self.assertEqual(item["name"], "PS5 YouTube Uploader")
        self.assertEqual(item["filename"], "ps5-youtube.elf")
        self.assertEqual(
            item["url"],
            "https://github.com/temp-latif07/ps5-yt-uploader/releases/download/v1.0.0/ps5-youtube.elf",
        )
        self.assertEqual(item["version"], "v1.0.0")
        self.assertEqual(item["category"], "Utilities")
        self.assertEqual(item["author"], "Latif Muhammad")
        self.assertEqual(item["source"], "https://github.com/temp-latif07/ps5-yt-uploader")
        self.assertEqual(item["checksum"], compute_sha256(self.test_elf))
        self.assertIn("description", item)
        self.assertIn("last_update", item)

        # Check file on disk
        self.assertTrue(os.path.exists(self.output_json))
        with open(self.output_json, "r", encoding="utf-8") as f:
            raw_content = f.read()
            loaded_json = json.loads(raw_content)

        self.assertEqual(loaded_json, catalog)
        # Check 2-space indentation formatting
        self.assertIn('  "name": "PS5 YouTube Uploader"', raw_content)
        self.assertIn('    "id": "ps5-yt-uploader"', raw_content)

    def test_idempotent_catalog_update(self):
        """Verify updating an existing payloads.json updates in-place without duplicating items."""
        # Pre-populate with another tool and an older version of ps5-yt-uploader
        existing_data = {
            "name": "PS5 YouTube Uploader",
            "payloads": [
                {
                    "id": "other-payload",
                    "name": "Other Payload Tool",
                    "filename": "other.elf",
                    "url": "https://example.com/other.elf",
                    "description": "Another payload",
                    "version": "v0.9.0",
                    "last_update": "2026-01-01",
                    "category": "Tools",
                    "checksum": "a" * 64,
                    "author": "Someone Else",
                    "source": "https://example.com/repo",
                },
                {
                    "id": "ps5-yt-uploader",
                    "name": "PS5 YouTube Uploader",
                    "filename": "ps5-youtube.elf",
                    "url": "https://github.com/temp-latif07/ps5-yt-uploader/releases/download/v0.9.0/ps5-youtube.elf",
                    "description": "Older version",
                    "version": "v0.9.0",
                    "last_update": "2026-01-01",
                    "category": "Utilities",
                    "checksum": "b" * 64,
                    "author": "Latif Muhammad",
                    "source": "https://github.com/temp-latif07/ps5-yt-uploader",
                },
            ],
        }
        with open(self.output_json, "w", encoding="utf-8") as f:
            json.dump(existing_data, f, indent=2)

        # Update with new version
        catalog = update_payload_catalog(
            elf_path=self.test_elf,
            version="v1.0.0",
            url="https://github.com/temp-latif07/ps5-yt-uploader/releases/download/v1.0.0/ps5-youtube.elf",
            output_path=self.output_json,
        )

        # Must maintain exactly 2 items (no duplicate ps5-yt-uploader added)
        self.assertEqual(len(catalog["payloads"]), 2)
        payload_ids = [p["id"] for p in catalog["payloads"]]
        self.assertEqual(payload_ids, ["other-payload", "ps5-yt-uploader"])

        # Check that other-payload was preserved intact
        other_item = catalog["payloads"][0]
        self.assertEqual(other_item["id"], "other-payload")
        self.assertEqual(other_item["version"], "v0.9.0")
        self.assertEqual(other_item["checksum"], "a" * 64)

        # Check that ps5-yt-uploader was updated in-place
        uploader_item = catalog["payloads"][1]
        self.assertEqual(uploader_item["id"], "ps5-yt-uploader")
        self.assertEqual(uploader_item["version"], "v1.0.0")
        self.assertEqual(uploader_item["checksum"], compute_sha256(self.test_elf))

        # Re-run same update again: must remain idempotent
        catalog2 = update_payload_catalog(
            elf_path=self.test_elf,
            version="v1.0.0",
            url="https://github.com/temp-latif07/ps5-yt-uploader/releases/download/v1.0.0/ps5-youtube.elf",
            output_path=self.output_json,
        )
        self.assertEqual(len(catalog2["payloads"]), 2)

    def test_validation_rejection_malformed_checksum(self):
        """Verify rejection if checksum is malformed (uppercase, bad length, non-hex)."""
        valid_hash = "a" * 64
        self.assertTrue(validate_checksum(valid_hash))

        # Uppercase hexadecimal characters
        with self.assertRaises(ValueError):
            validate_checksum("A" * 64)
        with self.assertRaises(ValueError):
            validate_checksum("a" * 63 + "F")

        # Invalid lengths
        with self.assertRaises(ValueError):
            validate_checksum("a" * 63)
        with self.assertRaises(ValueError):
            validate_checksum("a" * 65)
        with self.assertRaises(ValueError):
            validate_checksum("")

        # Non-hexadecimal characters
        with self.assertRaises(ValueError):
            validate_checksum("g" * 64)
        with self.assertRaises(ValueError):
            validate_checksum("!" * 64)

        # Non-string types
        with self.assertRaises(ValueError):
            validate_checksum(None)
        with self.assertRaises(ValueError):
            validate_checksum(12345)

    def test_validation_rejection_missing_fields(self):
        """Verify rejection if any required fields are omitted or catalog structure is invalid."""
        valid_entry = {
            "id": "ps5-yt-uploader",
            "name": "PS5 YouTube Uploader",
            "filename": "ps5-youtube.elf",
            "url": "https://github.com/temp-latif07/ps5-yt-uploader/releases/download/v1.0.0/ps5-youtube.elf",
            "version": "v1.0.0",
            "category": "Utilities",
            "checksum": "c" * 64,
            "author": "Latif Muhammad",
            "source": "https://github.com/temp-latif07/ps5-yt-uploader",
        }
        # Valid entry passes
        self.assertTrue(validate_payload_entry(valid_entry))

        # Check each required field omission raises ValueError
        for field in REQUIRED_PAYLOAD_FIELDS:
            incomplete_entry = dict(valid_entry)
            del incomplete_entry[field]
            with self.assertRaises(ValueError):
                validate_payload_entry(incomplete_entry)

            # Empty string also rejected
            empty_field_entry = dict(valid_entry)
            empty_field_entry[field] = ""
            with self.assertRaises(ValueError):
                validate_payload_entry(empty_field_entry)

    def test_catalog_validation_rules(self):
        """Verify catalog level validation (top-level name, duplicate IDs, payloads array)."""
        valid_catalog = {
            "name": "PS5 YouTube Uploader",
            "payloads": [
                {
                    "id": "item-1",
                    "name": "Item 1",
                    "filename": "item1.elf",
                    "url": "https://example.com/item1.elf",
                    "version": "1.0",
                    "category": "Utilities",
                    "checksum": "1" * 64,
                    "author": "Author",
                    "source": "https://example.com",
                }
            ],
        }
        self.assertTrue(validate_repo_catalog(valid_catalog))

        # Missing name
        bad_catalog = dict(valid_catalog)
        del bad_catalog["name"]
        with self.assertRaises(ValueError):
            validate_repo_catalog(bad_catalog)

        # Missing payloads
        bad_catalog2 = dict(valid_catalog)
        del bad_catalog2["payloads"]
        with self.assertRaises(ValueError):
            validate_repo_catalog(bad_catalog2)

        # Duplicate ID in payloads
        duplicate_catalog = {
            "name": "PS5 YouTube Uploader",
            "payloads": [
                valid_catalog["payloads"][0],
                dict(valid_catalog["payloads"][0]),
            ],
        }
        with self.assertRaises(ValueError):
            validate_repo_catalog(duplicate_catalog)

    def test_extract_version_from_header(self):
        """Verify extracting APP_VERSION from src/version.h."""
        version_header = os.path.join(REPO_ROOT, "src", "version.h")
        if os.path.exists(version_header):
            version = extract_version_from_header(version_header)
            self.assertEqual(version, "1.0.0")

    def test_cli_invocation(self):
        """Verify update_payload_repo.py CLI with options and --check validation."""
        cmd = [
            sys.executable,
            SCRIPT_PATH,
            "--elf",
            self.test_elf,
            "--version",
            "v1.0.0",
            "--url",
            "https://github.com/temp-latif07/ps5-yt-uploader/releases/download/v1.0.0/ps5-youtube.elf",
            "--output",
            self.output_json,
        ]
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.assertEqual(res.returncode, 0, f"CLI failed: {res.stderr}")
        self.assertTrue(os.path.exists(self.output_json))

        # Test CLI --check flag
        check_cmd = [sys.executable, SCRIPT_PATH, "--check", self.output_json]
        check_res = subprocess.run(check_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.assertEqual(check_res.returncode, 0, f"Check failed: {check_res.stderr}")

    def test_root_payloads_json_if_present(self):
        """If root payloads.json exists in repo root, verify it strictly passes all schema validations."""
        root_catalog_path = os.path.join(REPO_ROOT, "payloads.json")
        if os.path.exists(root_catalog_path):
            with open(root_catalog_path, "r", encoding="utf-8") as f:
                root_catalog = json.load(f)
            self.assertTrue(validate_repo_catalog(root_catalog))
            ps5_item = next(p for p in root_catalog["payloads"] if p["id"] == "ps5-yt-uploader")
            self.assertEqual(ps5_item["name"], "PS5 YouTube Uploader")
            self.assertEqual(ps5_item["filename"], "ps5-youtube.elf")
            self.assertEqual(ps5_item["author"], "Latif Muhammad")

            bin_elf = os.path.join(REPO_ROOT, "bin", "ps5-youtube.elf")
            if os.path.exists(bin_elf):
                self.assertEqual(ps5_item["checksum"], compute_sha256(bin_elf))


if __name__ == "__main__":
    unittest.main()
