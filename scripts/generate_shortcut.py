#!/usr/bin/env python3
"""
scripts/generate_shortcut.py

Generates a PS5 WebKit AppCache Homescreen Shortcut package.
Modeled after the itsPLK/ps5-webkit-autoloader architecture.

Outputs:
  <output_dir>/
    ├── sce_sys/
    │   ├── param.json      (PS5 Shell metadata & deeplink URI to 127.0.0.1:8888)
    │   └── icon0.png       (512x512 app icon for PS5 home screen carousel)
    └── index.html          (Offline WebKit fallback & redirector)

When installed to /user/app/<titleId>/ (e.g., /user/app/PS5YT0001/), the PS5
creates a home screen tile that boots directly into the local Web Companion Studio.
"""

import argparse
import json
import os
import struct
import sys
import zlib


def create_png_chunk(chunk_type: bytes, data: bytes) -> bytes:
    """Create a standard PNG chunk with length, type, data, and CRC32."""
    length = len(data)
    crc = zlib.crc32(chunk_type + data) & 0xFFFFFFFF
    return struct.pack(">I", length) + chunk_type + data + struct.pack(">I", crc)


def generate_icon_png(width: int = 512, height: int = 512) -> bytes:
    """
    Generates a 512x512 RGBA PNG icon in pure Python standard library.
    Design: PS5 dark slate background with YouTube red rounded badge and white play triangle.
    """
    # Build uncompressed scanlines: 1 filter byte (0 = None) + width * 4 (RGBA)
    raw_rows = []

    # Center coordinates and geometry
    cx = width // 2
    cy = height // 2
    rect_w = 260
    rect_h = 180
    r_corner = 40

    for y in range(height):
        row = bytearray(1 + width * 4)
        row[0] = 0  # Filter byte: None

        for x in range(width):
            offset = 1 + x * 4

            # Default: Dark PS5 console slate background (#0a0f1d)
            r, g, b, a = 10, 15, 29, 255

            # Outer subtle radial border / glow
            dist_center_sq = (x - cx) ** 2 + (y - cy) ** 2
            if dist_center_sq > (240 ** 2):
                # Fade outer corners slightly
                r, g, b = 6, 9, 18

            # Check if inside YouTube red badge (rounded rectangle)
            dx = abs(x - cx) - (rect_w // 2 - r_corner)
            dy = abs(y - cy) - (rect_h // 2 - r_corner)

            in_badge = False
            if dx <= 0 and abs(y - cy) <= (rect_h // 2):
                in_badge = True
            elif dy <= 0 and abs(x - cx) <= (rect_w // 2):
                in_badge = True
            elif dx > 0 and dy > 0 and (dx * dx + dy * dy) <= (r_corner * r_corner):
                in_badge = True

            if in_badge:
                # YouTube Red (#FF0000 / #EE1010)
                r, g, b = 255, 0, 0

                # White play triangle inside badge
                # Triangle vertices: Left at cx - 35 (y: cy - 45 to cy + 45), tip at cx + 45 (y: cy)
                tx_left = cx - 30
                tx_right = cx + 45
                ty_top = cy - 45
                ty_bottom = cy + 45

                if tx_left <= x <= tx_right:
                    # Height fraction from left to right
                    prog = (x - tx_left) / float(tx_right - tx_left)
                    half_h = (ty_bottom - cy) * (1.0 - prog)
                    if abs(y - cy) <= half_h:
                        # White play icon
                        r, g, b = 255, 255, 255

            row[offset] = r
            row[offset + 1] = g
            row[offset + 2] = b
            row[offset + 3] = a

        raw_rows.append(bytes(row))

    raw_data = b"".join(raw_rows)
    compressed_data = zlib.compress(raw_data, level=9)

    # Assemble PNG structure
    png_signature = b"\x89PNG\r\n\x1a\n"
    # IHDR chunk: width (4), height (4), bit depth (1), color type (6 = RGBA), compression (0), filter (0), interlace (0)
    ihdr_data = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    ihdr_chunk = create_png_chunk(b"IHDR", ihdr_data)
    idat_chunk = create_png_chunk(b"IDAT", compressed_data)
    iend_chunk = create_png_chunk(b"IEND", b"")

    return png_signature + ihdr_chunk + idat_chunk + iend_chunk


def generate_param_json(title_id: str, title_name: str, port: int) -> dict:
    """Generate PS5 ShellUI metadata structure (param.json)."""
    return {
        "titleId": title_id,
        "conceptId": "90001",
        "contentId": f"IV9999-{title_id}_00-0000000000000000",
        "contentVersion": "01.00.000",
        "masterVersion": "01.00",
        "applicationCategoryType": 65536,
        "deeplinkUri": f"http://127.0.0.1:{port}/",
        "localizedParameters": {
            "defaultLanguage": "en-US",
            "en-US": {
                "titleName": title_name
            }
        }
    }


def generate_index_html(title_name: str, port: int) -> str:
    """Generate offline WebKit fallback and loopback redirect page."""
    return f"""<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta http-equiv="refresh" content="0; url=http://127.0.0.1:{port}/">
  <title>{title_name}</title>
  <style>
    body {{
      background: #0a0f1d;
      color: #f0f4f8;
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
      height: 100vh;
      margin: 0;
      text-align: center;
    }}
    .badge {{
      background: #ff0000;
      color: #ffffff;
      padding: 12px 24px;
      border-radius: 12px;
      font-weight: 700;
      font-size: 24px;
      margin-bottom: 20px;
    }}
    h1 {{ margin: 0 0 10px; font-size: 32px; }}
    p {{ color: #94a3b8; font-size: 18px; max-width: 600px; line-height: 1.5; }}
    .status {{ margin-top: 20px; font-size: 14px; color: #64748b; }}
  </style>
  <script>
    window.location.replace("http://127.0.0.1:{port}/");
  </script>
</head>
<body>
  <div class="badge">YouTube</div>
  <h1>{title_name}</h1>
  <p>Connecting to local background daemon at <code>http://127.0.0.1:{port}/</code>...</p>
  <div class="status">If this screen does not redirect, please launch <code>ps5-yt-uploader.elf</code> via PS5 Payload Manager first.</div>
</body>
</html>
"""


def main():
    parser = argparse.ArgumentParser(
        description="Generate PS5 WebKit AppCache Homescreen Shortcut package (itsPLK model)."
    )
    parser.add_argument(
        "--output",
        default="dist/shortcut/PS5YT0001",
        help="Destination directory for shortcut package (default: dist/shortcut/PS5YT0001)",
    )
    parser.add_argument(
        "--title-id",
        default="PS5YT0001",
        help="PS5 Title ID (default: PS5YT0001)",
    )
    parser.add_argument(
        "--title-name",
        default="YouTube Uploader",
        help="Display title for PS5 home screen (default: YouTube Uploader)",
    )
    parser.add_argument(
        "--port",
        type=int,
        default=8888,
        help="Local daemon HTTP port (default: 8888)",
    )

    args = parser.parse_args()

    # Create destination directories
    pkg_dir = os.path.abspath(args.output)
    sce_sys_dir = os.path.join(pkg_dir, "sce_sys")
    os.makedirs(sce_sys_dir, exist_ok=True)

    # 1. Write param.json
    param_path = os.path.join(sce_sys_dir, "param.json")
    param_data = generate_param_json(args.title_id, args.title_name, args.port)
    with open(param_path, "w", encoding="utf-8") as f:
        json.dump(param_data, f, indent=2)
    print(f"Created {param_path}")

    # 2. Write icon0.png
    icon_path = os.path.join(sce_sys_dir, "icon0.png")
    icon_bytes = generate_icon_png(512, 512)
    with open(icon_path, "wb") as f:
        f.write(icon_bytes)
    print(f"Created {icon_path} ({len(icon_bytes)} bytes)")

    # 3. Write index.html
    index_path = os.path.join(pkg_dir, "index.html")
    index_content = generate_index_html(args.title_name, args.port)
    with open(index_path, "w", encoding="utf-8") as f:
        f.write(index_content)
    print(f"Created {index_path}")

    print(f"\nShortcut package generated successfully in: {pkg_dir}")
    print("Deployment instructions:")
    print(f"  Copy '{pkg_dir}' to your PS5 at '/user/app/{args.title_id}' via FTP or WebKit installer.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
