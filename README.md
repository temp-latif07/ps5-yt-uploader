# YouTube Uploader for PS5

Browse your PlayStation 5 gameplay clips (`/user/av_content/video`, `/data`, and USB drives) and upload them **directly from the console to YouTube Studio**, controlled from the TV browser or your phone/PC at `http://<console-ip>:8888`.

- **One-time sign-in with YouTube Studio cookies** (no Google Cloud project, no API key setup, no 6-video daily quota, and no forced Private lock):
  - Run `python3 tools/yt-login.py http://<console-ip>:8888` once on your Mac/PC (opens an isolated browser window, signs in, pushes cookies + channel metadata to the console, and closes cleanly so daily browsing doesn't invalidate them; add `--choose-channel` for Brand Accounts).
  - Or scan the **Phone QR** code shown in the TV UI and upload/paste `cookies.txt` from your phone.
- **Clip Browser & Range Video Preview:** Scans `/user/av_content/video` (plus `/data` and mounted `/mnt/usbN` / `/mnt/extN` drives), groups by Game Title ID, marks already-uploaded clips with clickable `youtu.be` links, and previews videos in-browser before uploading.
- **Manual Curation & Resumable Background Queue:** Edit Title, Description, Privacy (*Unlisted*, *Public*, *Private*), Audience (*Made for Kids*), and optional Playlist ID. Uploads stream directly from disk via YouTube's Scotty resumable protocol with live progress, speed, ETA, Pause, Resume, Retry, and Cancel — surviving network drops and console restarts.
- **Optional Auto-Upload (Off by Default):** Enable in Settings to automatically upload *new* clips saved after turning it on (pre-existing clips are never backfilled).

---

## How It Runs (and Why)

There are two pieces, modeled on [`torbox-ps5`](https://github.com/roieeedouek/torbox-ps5):

| Piece | What it is | Why |
|---|---|---|
| `ps5-yt-uploader.elf` | Background web server and uploader payload for `elfldr` (port `9021`). | Lives independently of games: uploads keep running in the background while you play. |
| `PPSA97631` title | Home-screen tile **"YouTube Uploader"**. Opening it starts `ps5-yt-uploader.elf` via `127.0.0.1:9021` if not running yet, opens the PS5 browser on `http://127.0.0.1:8888`, and exits. | One click from the PS5 home screen without losing uploads when switching games. |

> Runs on port `8888` with Title ID `PPSA97631`, so it coexists cleanly alongside `torbox-ps5` (`8080` / `PPSA97630`).

---

## Install

### Option 1: Home-Screen Tile + Payload (Recommended)

You need FTP access to the console (e.g. `ftpsrv` / `zftpd`), `elfldr` on port `9021`, and ShadowMountPlus to register titles.

1. Copy the `PPSA97631` folder (from the release zip, or `build/PPSA97631` after building) to **`/data/homebrew/PPSA97631/`**:
   ```sh
   IP=192.168.1.50
   H=ftp://$IP:2120 D=build/PPSA97631
   for f in eboot.bin ps5-yt-uploader.elf sce_sys/param.json sce_sys/icon0.png sce_module/libc.prx; do
       curl --disable-epsv --ftp-create-dirs -T "$D/$f" "$H/data/homebrew/PPSA97631/$f"
   done
   curl --disable-epsv -Q "SITE CHMOD 755 /data/homebrew/PPSA97631/eboot.bin" \
        -Q "SITE CHMOD 755 /data/homebrew/PPSA97631/sce_module/libc.prx" "$H/data/homebrew/PPSA97631/"
   ```
2. Register the title with ShadowMountPlus. **YouTube Uploader** appears on your PS5 home screen.
3. Open the tile — the payload starts, shows a notification toast with `http://<console-ip>:8888`, and opens the PS5 browser.

### Option 2: Payload Only / PS5 Payload Manager

- **PS5 Payload Manager (`itsPLK/ps5-payload-manager`):** Add custom repository URL:
  `https://raw.githubusercontent.com/temp-latif07/ps5-yt-uploader/main/payloads.json`
- **Manual Netcat:** `nc -w 3 <console-ip> 9021 < build/ps5-yt-uploader.elf`

---

## Sign In & Upload

1. Open `http://<console-ip>:8888` on your Mac/PC or scan the **Phone QR** code on your TV.
2. Run the one-time login helper on your Mac/PC (requires Chrome/Chromium/Edge/Brave or Playwright):
   ```sh
   python3 tools/yt-login.py http://<console-ip>:8888
   ```
   *(Add `--choose-channel` if your channel is under a Brand Account so you can pick the channel before confirming.)*
   Or upload/paste a Netscape `cookies.txt` file or raw `Cookie:` header directly in the web UI.
3. Browse **Clips**, click **Preview** to verify the clip, or **Upload** to set Title, Description, Privacy, Audience (Made for Kids), and optional Playlist ID.
4. Watch live upload progress in **Uploads** and keep playing — a PS5 notification toast displays the `youtu.be/<id>` link when finished.

---

## Files on the Console

```text
/data/ps5-yt-uploader/cookies.txt    YouTube Studio cookie jar (unencrypted, mode 600)
/data/ps5-yt-uploader/session.txt    Active channel ID & metadata (mode 600)
/data/ps5-yt-uploader/queue.tsv      Pending/paused/failed uploads & resumable Scotty URLs
/data/ps5-yt-uploader/uploaded.tsv   Completed upload history (path -> youtu.be video ID)
/data/ps5-yt-uploader/settings.txt   Default privacy, chunk size, captures dir, auto-upload flag
/data/ps5-yt-uploader/log.txt        Event log (never contains cookies or SAPISID)
```

---

## Build & Test

Requires `cc` + `libcurl` for host builds, and either local PS5 SDKs (`PS5_PAYLOAD_SDK` + `PS5_VULKAN_DIR`) or Docker (`docker/Dockerfile.ps5`) for PS5 targets:

```sh
./build.sh host      # Native Mac/Linux binary      -> build/host/ps5-yt-uploader
./build.sh payload   # PS5 background ELF payload   -> build/ps5-yt-uploader.elf
./build.sh title     # PS5 home-screen tile         -> build/PPSA97631/
./build.sh package   # Distributable release zip    -> build/ps5-yt-uploader-<ver>.zip
./build.sh all       # Build everything

bash tests/test_host.sh   # Full unit + end-to-end host suite against tests/mock_youtube.py
```

---

## Code Map

```text
src/main.c        Startup, PS5 notification toast, shutdown
src/platform.c    PS5 / host paths, notifications, klog, path sandbox, 1 MiB threads
src/util.c        String buffer (sbuf), JSON escaping, form/URL helpers, path utils
src/json.c        Minimal zero-dependency JSON parser
src/sha1.c        RFC 3174 SHA-1 & SAPISIDHASH computation
src/httpd.c       poll()-guarded HTTP/1.1 server & CSRF check (X-PS5YT)
src/net.c         Atomic cookie jar (0600), session storage, libcurl/TLS/socket tuning
src/youtube.c     Studio WEB_CREATOR bootstrap, Scotty resumable upload, createvideo, metadata_update
src/clips.c       Capture scanner, folder/USB browser, uploaded.tsv history
src/upload.c      Persistent upload queue, workers, pause/resume/cancel, safe auto-watcher
src/api.c         JSON REST routes, Range video streaming, static assets
web/              Plain HTML/CSS/JS UI + inline SVG QR code generator
title/            PPSA97631 native launcher title (launcher.c, param.json, icon, build script)
tools/            embed.py, yt-login.py, release.sh, update_payloads.py, extract_release_notes.py
docker/           Dockerfile.ps5 (ps5-payload-sdk + static zlib/mbedTLS/curl + ps5-native-tool)
archive/          Original implementation preserved for reference
```

---

## License

MIT License — see [LICENSE](LICENSE).
