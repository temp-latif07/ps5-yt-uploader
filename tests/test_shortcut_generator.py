#!/usr/bin/env python3
"""
tests/test_shortcut_generator.py

Unit tests for scripts/generate_shortcut.py (WebKit AppCache Homescreen Shortcut Generator).
Validates param.json metadata, icon0.png binary integrity, and index.html redirect.
"""

import json
import os
import shutil
import struct
import subprocess
import tempfile
import unittest

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GENERATE_SCRIPT = os.path.join(REPO_ROOT, "scripts", "generate_shortcut.py")


class TestShortcutGenerator(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.mkdtemp(prefix="ps5_shortcut_test_")

    def tearDown(self):
        shutil.rmtree(self.temp_dir, ignore_errors=True)

    def test_script_exists_and_executable(self):
        """Verify generate_shortcut.py exists and is executable."""
        self.assertTrue(os.path.isfile(GENERATE_SCRIPT), f"{GENERATE_SCRIPT} does not exist")
        self.assertTrue(os.access(GENERATE_SCRIPT, os.X_OK), f"{GENERATE_SCRIPT} is not executable")

    def test_generate_shortcut_package(self):
        """Verify full shortcut package generation."""
        pkg_dir = os.path.join(self.temp_dir, "PS5YT0001")
        cmd = [
            "python3",
            GENERATE_SCRIPT,
            "--output",
            pkg_dir,
            "--title-id",
            "PS5YT0001",
            "--title-name",
            "YouTube Uploader",
            "--port",
            "8888",
        ]
        res = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, f"Script failed: {res.stderr}")

        # Check directory structure
        sce_sys = os.path.join(pkg_dir, "sce_sys")
        param_json_path = os.path.join(sce_sys, "param.json")
        icon_path = os.path.join(sce_sys, "icon0.png")
        index_path = os.path.join(pkg_dir, "index.html")

        self.assertTrue(os.path.isdir(sce_sys), "sce_sys directory missing")
        self.assertTrue(os.path.isfile(param_json_path), "param.json missing")
        self.assertTrue(os.path.isfile(icon_path), "icon0.png missing")
        self.assertTrue(os.path.isfile(index_path), "index.html missing")

        # Validate param.json
        with open(param_json_path, "r", encoding="utf-8") as f:
            param = json.load(f)

        self.assertEqual(param.get("titleId"), "PS5YT0001")
        self.assertEqual(param.get("deeplinkUri"), "http://127.0.0.1:8888/")
        self.assertEqual(param.get("applicationCategoryType"), 65536)
        self.assertIn("localizedParameters", param)
        self.assertEqual(
            param["localizedParameters"].get("en-US", {}).get("titleName"),
            "YouTube Uploader",
        )

        # Validate icon0.png (valid 512x512 PNG)
        with open(icon_path, "rb") as f:
            header = f.read(24)
        self.assertEqual(header[:8], b"\x89PNG\r\n\x1a\n", "icon0.png missing PNG signature")
        self.assertEqual(header[12:16], b"IHDR", "Missing IHDR chunk")
        width, height = struct.unpack(">II", header[16:24])
        self.assertEqual(width, 512, f"Expected icon width 512, got {width}")
        self.assertEqual(height, 512, f"Expected icon height 512, got {height}")

        # Validate index.html contains redirect to loopback server
        with open(index_path, "r", encoding="utf-8") as f:
            html = f.read()
        self.assertIn("http://127.0.0.1:8888", html)


if __name__ == "__main__":
    unittest.main()
