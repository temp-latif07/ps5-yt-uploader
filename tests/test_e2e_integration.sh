#!/usr/bin/env bash
set -e

# ==============================================================================
# End-to-End Integration Test for PS5 YouTube Uploader
# Verifies:
# 1. Mock YouTube Scotty & InnerTube server integration
# 2. Host runner bin/ps5-youtube-host boot & media tile registration
# 3. Companion UI asset serving & /api/auth pairing
# 4. Capture watcher write-debouncing, Scotty chunk streaming, and uploaded.json record
# 5. Clean SIGINT graceful termination
# 6. PS5 FreeBSD 12 ELF cross-compilation and < 700 KB payload size verification
# ==============================================================================

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_ROOT"

TMP_DIR=$(mktemp -d /tmp/ps5_youtube_e2e_XXXXXX)
DATA_DIR="$TMP_DIR/data"
CAPTURES_DIR="$TMP_DIR/captures"
APP_DB="$TMP_DIR/mms/app.db"
mkdir -p "$DATA_DIR" "$CAPTURES_DIR" "$TMP_DIR/mms"

MOCK_PORT=8998
HOST_PORT=8891
MOCK_PID=""
HOST_PID=""

cleanup() {
    local exit_code=$?
    echo "[E2E Cleanup] Terminating child processes and cleaning temp files..."
    if [ -n "$HOST_PID" ] && kill -0 "$HOST_PID" 2>/dev/null; then
        kill -INT "$HOST_PID" 2>/dev/null || true
        sleep 0.3
        kill -9 "$HOST_PID" 2>/dev/null || true
    fi
    if [ -n "$MOCK_PID" ] && kill -0 "$MOCK_PID" 2>/dev/null; then
        kill -TERM "$MOCK_PID" 2>/dev/null || true
        sleep 0.2
        kill -9 "$MOCK_PID" 2>/dev/null || true
    fi
    rm -rf "$TMP_DIR"
    exit $exit_code
}
trap cleanup EXIT INT TERM

echo "=== Step 1: Starting Mock YouTube Scotty & InnerTube Server on port $MOCK_PORT ==="
python3 tests/mock_youtube_server.py $MOCK_PORT > "$TMP_DIR/mock.log" 2>&1 &
MOCK_PID=$!

for i in {1..30}; do
    if curl -s "http://127.0.0.1:$MOCK_PORT/health" > /dev/null 2>&1; then
        break
    fi
    sleep 0.1
done

if ! curl -s "http://127.0.0.1:$MOCK_PORT/health" > /dev/null 2>&1; then
    echo "FAIL: Mock YouTube server failed to start on port $MOCK_PORT"
    cat "$TMP_DIR/mock.log"
    exit 1
fi
echo "  -> Mock YouTube server running on port $MOCK_PORT!"

echo "=== Step 2: Building and Launching bin/ps5-youtube-host on port $HOST_PORT ==="
make bin/ps5-youtube-host

export PS5_YOUTUBE_SCOTTY_URL="http://127.0.0.1:$MOCK_PORT/upload/studio?authuser=0"
export PS5_YOUTUBE_INNERTUBE_URL="http://127.0.0.1:$MOCK_PORT/youtubei/v1/upload/createvideo"

./bin/ps5-youtube-host \
    --port $HOST_PORT \
    --data-dir "$DATA_DIR" \
    --captures-dir "$CAPTURES_DIR" \
    --app-db "$APP_DB" \
    > "$TMP_DIR/host.log" 2>&1 &
HOST_PID=$!

for i in {1..30}; do
    if curl -s "http://127.0.0.1:$HOST_PORT/api/status" > /dev/null 2>&1; then
        break
    fi
    sleep 0.1
done

if ! curl -s "http://127.0.0.1:$HOST_PORT/api/status" > /dev/null 2>&1; then
    echo "FAIL: ps5-youtube-host failed to start on port $HOST_PORT"
    cat "$TMP_DIR/host.log"
    exit 1
fi
echo "  -> ps5-youtube-host running on port $HOST_PORT!"

echo "=== Step 3: Verifying Media Category Tile Registration in MMS Database ==="
if [ ! -f "$APP_DB" ]; then
    echo "FAIL: MMS database file $APP_DB was not created"
    exit 1
fi

TABLE_NAME=$(sqlite3 "$APP_DB" "SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'tbl_appbrowse_%' LIMIT 1;")
if [ -z "$TABLE_NAME" ]; then
    echo "FAIL: No tbl_appbrowse_% table found in $APP_DB"
    exit 1
fi
echo "  -> Found MMS table: $TABLE_NAME"

TILE_ROW=$(sqlite3 "$APP_DB" "SELECT titleId, category, titleName, url FROM \"$TABLE_NAME\" WHERE titleId = 'YOUTUBE00';" 2>/dev/null || true)
echo "  -> Found tile row in app.db: $TILE_ROW"
if [ -z "$TILE_ROW" ]; then
    echo "FAIL: Tile record YOUTUBE00 not found in $APP_DB"
    exit 1
fi
if ! echo "$TILE_ROW" | grep -q "http://127.0.0.1:$HOST_PORT"; then
    echo "FAIL: Tile URL does not point to http://127.0.0.1:$HOST_PORT: $TILE_ROW"
    exit 1
fi
echo "  -> Media category tile verified successfully!"

echo "=== Step 4: Verifying Web UI Companion Dashboard Asset Delivery ==="
STATUS_CODE=$(curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:$HOST_PORT/")
if [ "$STATUS_CODE" != "200" ]; then
    echo "FAIL: GET / returned HTTP $STATUS_CODE (expected 200)"
    exit 1
fi
CSS_STATUS=$(curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:$HOST_PORT/style.css")
JS_STATUS=$(curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:$HOST_PORT/app.js")
if [ "$CSS_STATUS" != "200" ] || [ "$JS_STATUS" != "200" ]; then
    echo "FAIL: Embedded assets returned invalid status ($CSS_STATUS, $JS_STATUS)"
    exit 1
fi
echo "  -> Embedded Web UI assets verified successfully!"

echo "=== Step 5: Pairing Session via POST /api/auth ==="
AUTH_RESP=$(curl -s -X POST "http://127.0.0.1:$HOST_PORT/api/auth" \
    -H "Content-Type: application/json" \
    -d '{
        "cookies": "SAPISID=mock_sapisid_token_12345; __Secure-3PSID=mock_sid_67890;",
        "session_token": "mock_session_token_abc123",
        "channel_name": "PS5 E2E Test Channel",
        "channel_id": "UC_MOCK_E2E_123"
    }')

echo "  -> Auth response: $AUTH_RESP"
if ! echo "$AUTH_RESP" | grep -q '"success":true'; then
    echo "FAIL: Pairing via /api/auth failed: $AUTH_RESP"
    exit 1
fi

STATUS_AFTER_AUTH=$(curl -s "http://127.0.0.1:$HOST_PORT/api/status")
echo "  -> Status after auth: $STATUS_AFTER_AUTH"
if ! echo "$STATUS_AFTER_AUTH" | grep -q '"authenticated":true'; then
    echo "FAIL: /api/status does not show authenticated=true"
    exit 1
fi
echo "  -> Authentication confirmed!"

echo "=== Step 5.5: Enabling Auto-Upload via POST /api/config for E2E Pipeline ==="
CONFIG_RESP=$(curl -s -X POST -H "Content-Type: application/json" -d '{"auto_upload":true,"default_privacy":"UNLISTED"}' "http://127.0.0.1:$HOST_PORT/api/config")
echo "  -> Config response: $CONFIG_RESP"

echo "=== Step 6: Simulating Capture File Creation & Verifying Auto-Upload ==="
mkdir -p "$CAPTURES_DIR/CUSA00123"
SAMPLE_SRC="tests/assets/sample_clip.mp4"
if [ ! -f "$SAMPLE_SRC" ]; then
    echo "FAIL: $SAMPLE_SRC not found!"
    exit 1
fi

TARGET_CLIP="$CAPTURES_DIR/CUSA00123/gameplay_test.mp4"
cp "$SAMPLE_SRC" "$TARGET_CLIP"
echo "  -> Copied sample clip to $TARGET_CLIP"

echo "  -> Waiting for watcher debouncing, detection, and upload to complete..."
UPLOAD_OK=0
for i in {1..60}; do
    if [ -f "$DATA_DIR/uploaded.json" ]; then
        if grep -q "gameplay_test.mp4" "$DATA_DIR/uploaded.json" 2>/dev/null && grep -q "mock_vid_98765" "$DATA_DIR/uploaded.json" 2>/dev/null; then
            UPLOAD_OK=1
            break
        fi
    fi
    sleep 0.2
done

if [ "$UPLOAD_OK" -ne 1 ]; then
    echo "FAIL: Clip was not recorded in uploaded.json within timeout!"
    echo "=== Host Log ==="
    cat "$TMP_DIR/host.log"
    echo "=== Mock Server Log ==="
    cat "$TMP_DIR/mock.log"
    exit 1
fi
echo "  -> Upload succeeded! uploaded.json contents:"
cat "$DATA_DIR/uploaded.json"
echo ""

echo "=== Step 7: Verifying Clean Teardown on SIGINT ==="
kill -INT "$HOST_PID"
wait "$HOST_PID"
HOST_EXIT=$?
HOST_PID="" # Clear so trap doesn't kill it
if [ "$HOST_EXIT" -ne 0 ]; then
    echo "FAIL: ps5-youtube-host exited with status $HOST_EXIT on SIGINT (expected 0)"
    exit 1
fi
echo "  -> ps5-youtube-host exited cleanly with status 0!"

echo "=== Step 8: Verifying PS5 Cross-Compilation & Binary Size ==="
make ps5
if [ ! -f "bin/ps5-yt-uploader.elf" ]; then
    echo "FAIL: bin/ps5-yt-uploader.elf was not created by make ps5"
    exit 1
fi

ELF_INFO=$(file bin/ps5-yt-uploader.elf)
echo "  -> ELF Info: $ELF_INFO"
if ! echo "$ELF_INFO" | grep -qi "ELF 64-bit"; then
    echo "FAIL: bin/ps5-yt-uploader.elf is not an ELF 64-bit binary"
    exit 1
fi

ELF_SIZE=$(stat -f "%z" bin/ps5-yt-uploader.elf 2>/dev/null || stat -c "%s" bin/ps5-yt-uploader.elf 2>/dev/null)
echo "  -> Final Payload Size: $ELF_SIZE bytes"
MAX_SIZE=716800 # 700 KB in bytes
if [ "$ELF_SIZE" -ge "$MAX_SIZE" ]; then
    echo "FAIL: bin/ps5-yt-uploader.elf ($ELF_SIZE bytes) exceeds 700 KB limit ($MAX_SIZE bytes)"
    exit 1
fi
echo "  -> Payload size constraint passed ($ELF_SIZE < 716800 bytes)!"

echo "=========================================================="
echo "End-to-End Integration Test Passed Successfully!"
echo "=========================================================="
