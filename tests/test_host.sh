#!/usr/bin/env bash
# End-to-end test suite for ps5-yt-uploader host build against tests/mock_youtube.py.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
bin=$here/build/host/ps5-yt-uploader

# Run C unit tests first
curl_dir=${HOST_CURL:-}
if [[ -z $curl_dir ]]; then
    for d in /opt/homebrew/opt/curl /home/linuxbrew/.linuxbrew/opt/curl; do
        if [[ -d $d/include && -d $d/lib ]]; then curl_dir=$d; break; fi
    done
fi
ucflags=(-std=c99 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -I"$here/src")
uldflags=(-lcurl -lpthread)
if [[ -n $curl_dir ]]; then
    ucflags+=(-I"$curl_dir/include")
    uldflags=(-L"$curl_dir/lib" -Wl,-rpath,"$curl_dir/lib" -lcurl -lpthread)
fi
unit_bin=$(mktemp "${TMPDIR:-/tmp}/ps5yt-unit.XXXXXX")
${CC:-cc} "${ucflags[@]}" "$here/tests/test_unit.c" "$here"/src/platform.c "$here"/src/util.c \
    "$here"/src/json.c "$here"/src/httpd.c "$here"/src/net.c \
    "${uldflags[@]}" -o "$unit_bin"
"$unit_bin"
rm -f "$unit_bin"

# Build host binary
"$here/build.sh" host

work=$(mktemp -d "${TMPDIR:-/tmp}/ps5yt-test.XXXXXX")
mkdir -p "$work/app" "$work/cap/PPSA01234" "$work/extra"
mport=18999 port=18085
export base=http://127.0.0.1:$port
export mbase=http://127.0.0.1:$mport
pids=()
cleanup() { for p in "${pids[@]}"; do kill "$p" 2>/dev/null || true; done; wait 2>/dev/null || true; }
trap cleanup EXIT

python3 -I "$here/tests/mock_youtube.py" $mport > "$work/mock.log" 2>&1 &
pids+=($!)
for _ in $(seq 50); do curl -s -o /dev/null "$mbase/control/stats" && break; sleep 0.1; done

start_app() {
    PS5YT_APP_DIR="$work/app" \
    PS5YT_CAPTURES_DIR="$work/cap" \
    PS5YT_ROOTS="$work" \
    PS5YT_PORT=$port \
    PS5YT_WATCH_MS=200 \
    PS5YT_OAUTH_BASE="$mbase/oauth" \
    PS5YT_API_BASE="$mbase/youtube/v3" \
    PS5YT_UPLOAD_BASE="$mbase/upload/youtube/v3" \
    "$bin" >> "$work/app.log" 2>&1 &
    app_pid=$!
    pids+=($app_pid)
    for _ in $(seq 50); do curl -s "$base/api/status" > /dev/null && return; sleep 0.1; done
    echo "app did not start" >&2
    curl -v "$base/api/status" >&2 || true
    cat "$work/app.log" >&2 || true
    cat "$work/mock.log" >&2 || true
    exit 1
}
start_app

fails=0
check() {
    local name=$1; shift
    if "$@"; then echo "ok   $name"; else echo "FAIL $name"; fails=$((fails + 1)); fi
}
get() { curl -s "$base/api/$1"; }
post() { local p=$1; shift; curl -s -X POST -H 'X-PS5YT: 1' "$@" "$base/api/$p"; }
jq_() { python3 -I -c "import json,sys; d=json.load(sys.stdin); print($1)"; }
file_mode() { stat -f %Lp "$1" 2>/dev/null || stat -c %a "$1"; }
export -f get post jq_ file_mode

make_clip() { # path, mb
    python3 -I -c "import sys; p,mb=sys.argv[1],int(sys.argv[2]); pat=bytes((i*7+3)%251 for i in range(65536)); f=open(p,'wb'); [f.write(pat) for _ in range(mb*16)]; f.close()" "$1" "$2"
}
sha_of() { python3 -I -c "import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],'rb').read()).hexdigest())" "$1"; }
wait_jobs() {
    for _ in $(seq $(($1 * 5))); do
        get jobs | grep -q '"state":"\(queued\|starting\|uploading\|finalizing\)"' || return 0
        sleep 0.2
    done
    return 1
}
wait_state() { # job_id, desired_state, max_sec
    local jid=$1 want=$2 max=${3:-15}
    for _ in $(seq $((max * 5))); do
        if get jobs | python3 -I -c "import json,sys; j=[x for x in json.load(sys.stdin)['jobs'] if x['id']==$jid]; sys.exit(0 if j and j[0]['state']=='$want' else 1)"; then
            return 0
        fi
        sleep 0.2
    done
    return 1
}
sign_in_device() {
    local st
    st=$(post oauth/start)
    echo "$st" | jq_ 'd["verification_url"]=="https://www.google.com/device" and d["user_code"]=="ABCD-EFGH" and ("device_code" not in d)' | grep -q True
    curl -s "$mbase/control?approve_device=1" > /dev/null
    local p
    p=$(post oauth/poll)
    echo "$p" | jq_ 'd["state"]=="done"' | grep -q True
}

# 1. UI & CSRF checks
check "UI page served" bash -c "curl -s $base/ | grep -q 'YouTube Uploader'"
check "app.js served" bash -c "curl -s $base/app.js | grep -q loadStatus"
check "style.css served" bash -c "curl -s $base/style.css | grep -q ':focus-visible'"
check "no v2 strings in web/" bash -c "! grep -ri 'v2' $here/web/ | grep -v 'favicon.svg' | grep -q ."
check "initially no client and not logged in" bash -c "get status | jq_ '(not d[\"has_client\"]) and (not d[\"logged_in\"])' | grep -q True"
check "POST without X-PS5YT refused (403)" bash -c "[[ \$(curl -s -o /dev/null -w '%{http_code}' -d client_id=x $base/api/oauth/config) == 403 ]]"

# 2. Client JSON configuration & secret isolation
r=$(post oauth/config --data-urlencode 'client_json={"installed":{"client_id":"only"}}')
check "malformed client_json refused (400)" bash -c "grep -qi 'client_secret' <<<'$r'"

cjson='{"installed":{"client_id":"1234567890-testclient.apps.googleusercontent.com","client_secret":"GOCSPX-supersecret123"}}'
r=$(post oauth/config --data-urlencode "client_json=$cjson")
check "valid client_json accepted" bash -c "echo '$r' | jq_ 'd[\"has_client\"] and (not d[\"logged_in\"]) and (\"...\" in d[\"client_id_hint\"])' | grep -q True"
check "oauth.txt mode 600" bash -c "[[ \$(file_mode '$work/app/oauth.txt') == 600 ]]"
check "client_secret never leaked in status" bash -c "! get status | grep -q 'GOCSPX-supersecret123'"

# 3. OAuth TV Device Flow (reuse active flow -> slow_down -> pending -> approve -> done)
curl -s "$mbase/control?slow_down_once=1" > /dev/null
ds_before=$(curl -s "$mbase/control/stats" | jq_ 'd["device_starts"]')
st=$(post oauth/start)
ds_mid=$(curl -s "$mbase/control/stats" | jq_ 'd["device_starts"]')
st2=$(post oauth/start)
ds_after=$(curl -s "$mbase/control/stats" | jq_ 'd["device_starts"]')
check "oauth/start returns URL & user_code without device_code" bash -c "echo '$st' | jq_ 'd[\"user_code\"]==\"ABCD-EFGH\" and d[\"verification_url\"]==\"https://www.google.com/device\" and (\"device_code\" not in d)' | grep -q True"
check "oauth/start reuses active flow across tabs/reloads" bash -c "[[ $ds_mid -eq $((ds_before + 1)) && $ds_after -eq $ds_mid ]] && echo '$st2' | jq_ 'd[\"user_code\"]==\"ABCD-EFGH\"' | grep -q True"
p1=$(post oauth/poll)
check "oauth/poll handles slow_down & increases interval" bash -c "echo '$p1' | jq_ 'd[\"state\"]==\"slow_down\" and d[\"interval\"]>=6' | grep -q True"
p2=$(post oauth/poll)
check "oauth/poll handles authorization_pending" bash -c "echo '$p2' | jq_ 'd[\"state\"]==\"pending\"' | grep -q True"
curl -s "$mbase/control?approve_device=1" > /dev/null
p3=$(post oauth/poll)
check "oauth/poll completes after user approves" bash -c "echo '$p3' | jq_ 'd[\"state\"]==\"done\"' | grep -q True"
check "status reports channel after sign-in" bash -c "get status | jq_ 'd[\"logged_in\"] and d[\"channel\"][\"name\"]==\"PS5 Test Channel\" and d[\"channel\"][\"id\"]==\"UC_TEST_123\"' | grep -q True"
check "tokens & secrets never leaked in status" bash -c "! get status | grep -qE 'ref_1|tok_2|GOCSPX-supersecret123|DEV_CODE_'"
check "tokens & secrets never leaked in logs" bash -c "! grep -rqE 'ref_1|tok_2|GOCSPX-supersecret123|DEV_CODE_' '$work/app/log.txt' '$work/app.log'"

# 4. Clips scanning, nested PS5 layout, folder picker, Range streaming & security
mkdir -p "$work/cap/NPXS40087/PPSA55555/2026"
make_clip "$work/cap/NPXS40087/PPSA55555/2026/deep_clip.mp4" 1
sleep 1.1
make_clip "$work/cap/clip2.webm" 1
sleep 1.1
make_clip "$work/cap/PPSA01234/clip1.mp4" 9
make_clip "$work/extra/usb_clip.mp4" 2
check "clips sorted newest first with nested title_id" bash -c "get clips | jq_ 'len(d[\"clips\"])==3 and d[\"clips\"][0][\"name\"]==\"clip1.mp4\" and d[\"clips\"][0][\"title_id\"]==\"PPSA01234\" and d[\"clips\"][1][\"name\"]==\"clip2.webm\" and d[\"clips\"][2][\"name\"]==\"deep_clip.mp4\" and d[\"clips\"][2][\"title_id\"]==\"PPSA55555\"' | grep -q True"
check "dirs lists subdirs and refuses /etc and .." bash -c "get 'dirs?path=$work/cap' | grep -q PPSA01234 && get 'dirs?path=/etc' | grep -q outside && get 'dirs?path=$work/cap/../..' | grep -q outside"
check "stream returns 206 Partial Content" bash -c "[[ \$(curl -s -o /dev/null -w '%{http_code}' -H 'Range: bytes=0-99' '$base/api/stream?path=$work/cap/PPSA01234/clip1.mp4') == 206 ]]"
check "stream blocks oauth.txt (403)" bash -c "[[ \$(curl -s -o /dev/null -w '%{http_code}' '$base/api/stream?path=$work/app/oauth.txt') == 403 ]]"

# 5. Chunked Data API v3 upload + playlist + SHA-256 verification
post settings -d chunk_mb=4 > /dev/null
r=$(post upload --data-urlencode "path=$work/cap/PPSA01234/clip1.mp4" -d "title=Epic Victory" \
    --data-urlencode "description=Line1
Line2" -d privacy=PUBLIC -d made_for_kids=0 -d playlist_id=PL123)
jid1=$(echo "$r" | jq_ 'd["id"]')
check "upload 1 finishes" wait_jobs 30
want_sha=$(sha_of "$work/cap/PPSA01234/clip1.mp4")
check "Data API v3 SHA-256, categoryId=20, privacy=public, playlist match" bash -c "
curl -s '$mbase/control/stats' | python3 -I -c '
import json,sys; s=json.load(sys.stdin)
v=s[\"videos\"][-1]; pl=s[\"playlists\"][-1]
assert v[\"sha256\"]==\"$want_sha\" and v[\"mime\"]==\"video/mp4\"
assert v[\"categoryId\"]==\"20\" and v[\"privacyStatus\"]==\"public\"
assert v[\"selfDeclaredMadeForKids\"] is False and v[\"hasPaidProductPlacement\"] is False
assert v[\"title\"]==\"Epic Victory\" and pl=={\"playlistId\":\"PL123\",\"videoId\":v[\"id\"]}
'
"
check "clip marked uploaded in /api/clips" bash -c "get clips | grep -q 'VID_1'"

# 6. Made for Kids omits playlistItems & uses video/webm for .webm
before_pl=$(curl -s "$mbase/control/stats" | jq_ 'len(d["playlists"])')
post upload --data-urlencode "path=$work/cap/clip2.webm" -d title="Kids Clip" -d made_for_kids=1 -d playlist_id=PL_SHOULD_OMIT > /dev/null
check "MFK upload finishes" wait_jobs 20
check "MFK sets flag & omits playlistItems" bash -c "
curl -s '$mbase/control/stats' | python3 -I -c '
import json,sys; s=json.load(sys.stdin); v=s[\"videos\"][-1]
assert v[\"mime\"]==\"video/webm\" and v[\"selfDeclaredMadeForKids\"] is True
assert len(s[\"playlists\"])==$before_pl
'
"

# 7. Mid-stream network drop recovery via 308 offset query
make_clip "$work/cap/drop_clip.mp4" 9
curl -s "$mbase/control?drop_once_after=1048576" > /dev/null
post upload --data-urlencode "path=$work/cap/drop_clip.mp4" -d title="Drop Recovery" > /dev/null
check "drop recovery finishes" wait_jobs 30
drop_sha=$(sha_of "$work/cap/drop_clip.mp4")
check "drop recovery SHA-256 exact" bash -c "curl -s '$mbase/control/stats' | python3 -I -c 'import json,sys; s=json.load(sys.stdin); assert s[\"videos\"][-1][\"sha256\"]==\"$drop_sha\" and s[\"queries\"]>=1'"

# 8. Pause + server restart + resume + expired 410 re-init
make_clip "$work/cap/pause_clip.mp4" 16
curl -s "$mbase/control?rate=1500000" > /dev/null
jid_p=$(post upload --data-urlencode "path=$work/cap/pause_clip.mp4" -d title="Pause Resume" | jq_ 'd["id"]')
sleep 1.0
post jobs/pause -d id="$jid_p" > /dev/null
check "paused state & upload_url persisted" bash -c "wait_state() { for _ in \$(seq 75); do get jobs | grep -q '\"state\":\"paused\"' && return 0; sleep 0.2; done; return 1; }; wait_state && grep -q '/upload/session/' '$work/app/queue.tsv'"
kill "$app_pid"; wait "$app_pid" 2>/dev/null || true
curl -s "$mbase/control?rate=0" > /dev/null
start_app
check "paused job survived restart" bash -c "get jobs | grep -q 'Pause Resume'"
jid_after=$(get jobs | jq_ "[j['id'] for j in d['jobs'] if j['title']=='Pause Resume'][0]")
post jobs/resume -d id="$jid_after" > /dev/null
check "resumed job finishes" wait_jobs 30
pause_sha=$(sha_of "$work/cap/pause_clip.mp4")
check "pause+restart resume SHA-256 exact" bash -c "curl -s '$mbase/control/stats' | python3 -I -c 'import json,sys; assert json.load(sys.stdin)[\"videos\"][-1][\"sha256\"]==\"$pause_sha\"'"

# Expired 410 re-init test
make_clip "$work/cap/expire_clip.mp4" 16
curl -s "$mbase/control?rate=1500000" > /dev/null
jid_e=$(post upload --data-urlencode "path=$work/cap/expire_clip.mp4" -d title="Expired 410" | jq_ 'd["id"]')
sleep 1.0
post jobs/pause -d id="$jid_e" > /dev/null
wait_state "$jid_e" paused 15
curl -s "$mbase/control?rate=0&expire_next_query=1" > /dev/null
post jobs/resume -d id="$jid_e" > /dev/null
check "expired 410 re-init finishes" wait_jobs 30
exp_sha=$(sha_of "$work/cap/expire_clip.mp4")
check "expired 410 SHA-256 exact" bash -c "curl -s '$mbase/control/stats' | python3 -I -c 'import json,sys; assert json.load(sys.stdin)[\"videos\"][-1][\"sha256\"]==\"$exp_sha\"'"

# 9. Silent 401 token refresh + rotated refresh_token persisted
make_clip "$work/cap/refresh_clip.mp4" 2
curl -s "$mbase/control?force_401_once=1&rotate_refresh=1" > /dev/null
before_ref=$(curl -s "$mbase/control/stats" | jq_ 'd["refreshes"]')
jid_r=$(post upload --data-urlencode "path=$work/cap/refresh_clip.mp4" -d title="Silent Refresh" | jq_ 'd["id"]')
check "401 silent refresh retries and finishes" wait_jobs 20
after_ref=$(curl -s "$mbase/control/stats" | jq_ 'd["refreshes"]')
check "refresh called and rotated refresh_token persisted" bash -c "
[[ $((after_ref - before_ref)) -ge 1 ]] && grep -q 'refresh_token=ref_rotated' '$work/app/oauth.txt'
"

# 10. Partial failure after upload (playlistItems 500)
make_clip "$work/cap/partial_clip.mp4" 2
curl -s "$mbase/control?fail_playlist=1" > /dev/null
before_vid=$(curl -s "$mbase/control/stats" | jq_ 'len(d["videos"])')
jid_m=$(post upload --data-urlencode "path=$work/cap/partial_clip.mp4" -d title="Partial Playlist" -d playlist_id=PLFAIL | jq_ 'd["id"]')
check "partial playlist failure still marks done" wait_jobs 20
after_vid=$(curl -s "$mbase/control/stats" | jq_ 'len(d["videos"])')
check "partial failure keeps video_id + warning note + 1 uploaded video" bash -c "
[[ $((after_vid - before_vid)) == 1 ]] && get jobs | python3 -I -c '
import json,sys; j=[x for x in json.load(sys.stdin)[\"jobs\"] if x[\"id\"]==$jid_m][0]
assert j[\"state\"]==\"done\" and j[\"video_id\"].startswith(\"VID_\") and \"check YouTube Studio\" in j[\"note\"]
'
"

# 11. 403 quotaExceeded fail-fast, 401+invalid_grant fail-fast, and cancel
make_clip "$work/cap/quota_clip.mp4" 2
curl -s "$mbase/control?force_quota_403=1" > /dev/null
jid_q=$(post upload --data-urlencode "path=$work/cap/quota_clip.mp4" -d title="Quota Fail" | jq_ 'd["id"]')
check "quotaExceeded fails quickly" wait_jobs 15
check "quotaExceeded message shown" bash -c "get jobs | python3 -I -c 'import json,sys; j=[x for x in json.load(sys.stdin)[\"jobs\"] if x[\"id\"]==$jid_q][0]; assert j[\"state\"]==\"failed\" and \"quota\" in j[\"err\"].lower()'"

make_clip "$work/cap/revoked_clip.mp4" 2
curl -s "$mbase/control?force_401_once=1&force_invalid_grant=1" > /dev/null
jid_inv=$(post upload --data-urlencode "path=$work/cap/revoked_clip.mp4" -d title="Revoked Grant" | jq_ 'd["id"]')
check "invalid_grant fails quickly" wait_jobs 15
check "invalid_grant clears tokens but preserves Client ID" bash -c "
get jobs | python3 -I -c 'import json,sys; j=[x for x in json.load(sys.stdin)[\"jobs\"] if x[\"id\"]==$jid_inv][0]; assert j[\"state\"]==\"failed\" and \"expired\" in j[\"err\"]' &&
get status | jq_ 'd[\"has_client\"] and (not d[\"logged_in\"])' | grep -q True
"

# Re-sign in using preserved Client ID via QR Device Flow
check "re-sign in works using preserved Client ID" sign_in_device

make_clip "$work/cap/cancel_clip.mp4" 16
curl -s "$mbase/control?rate=1500000" > /dev/null
jid_c=$(post upload --data-urlencode "path=$work/cap/cancel_clip.mp4" -d title="Cancel Me" | jq_ 'd["id"]')
sleep 1.0
post jobs/cancel -d id="$jid_c" > /dev/null
check "cancel stops upload" wait_jobs 15
check "cancel state is cancelled" bash -c "get jobs | python3 -I -c 'import json,sys; j=[x for x in json.load(sys.stdin)[\"jobs\"] if x[\"id\"]==$jid_c][0]; assert j[\"state\"]==\"cancelled\"'"
curl -s "$mbase/control?rate=0" > /dev/null

# 12. Auto-upload watcher: off by default, never backfills existing clips, uploads brand-new clip
make_clip "$work/cap/historical_old.mp4" 1
post settings -d auto_upload=1 > /dev/null
sleep 1.2
check "auto_upload_since persisted and no historical backfill" bash -c "
grep -q 'auto_upload=1' '$work/app/settings.txt' && \
! get jobs | grep -q 'historical_old.mp4'
"
make_clip "$work/cap/brand_new_auto.mp4" 1
for _ in $(seq 40); do
    get jobs | grep -q 'brand_new_auto.mp4' && break
    sleep 0.2
done
check "brand-new clip auto-queued and finishes" wait_jobs 20
check "brand-new clip uploaded automatically" bash -c "get jobs | python3 -I -c 'import json,sys; j=[x for x in json.load(sys.stdin)[\"jobs\"] if \"brand_new_auto.mp4\" in x[\"path\"]][0]; assert j[\"state\"]==\"done\" and j[\"video_id\"].startswith(\"VID_\")'"
post settings -d auto_upload=0 > /dev/null

# 13. Sign out, background server-side device poll, Reset Client ID, quit, and release dry-run checks
post logout > /dev/null
check "logout clears refresh_token but keeps Client ID" bash -c "get status | jq_ 'd[\"has_client\"] and (not d[\"logged_in\"])' | grep -q True && grep -q 'client_id=1234567890' '$work/app/oauth.txt'"

post oauth/start > /dev/null
curl -s "$mbase/control?approve_device=1" > /dev/null
bg_ok=0
for _ in $(seq 25); do
    if [[ $(get status | jq_ 'd["logged_in"]') == True ]]; then
        bg_ok=1
        break
    fi
    sleep 0.2
done
check "background server polling completes sign-in without /api/oauth/poll" test "$bg_ok" = 1

post logout > /dev/null
post oauth/config/clear > /dev/null
check "oauth/config/clear deletes oauth.txt" test ! -e "$work/app/oauth.txt"
post quit > /dev/null
sleep 1
check "quit stops server" bash -c "! curl -s -m 2 '$base/api/status' > /dev/null"

check "make_icon produces PNG" bash -c "python3 '$here/title/make_icon.py' '$work/icon.png' && test -s '$work/icon.png'"
check "payloads.json valid" python3 "$here/tools/update_payloads.py" --check "$here/payloads.json"
ver=$(sed -n 's/^#define APP_VERSION "\(.*\)"/\1/p' "$here/src/app.h")
check "extract_release_notes finds $ver" bash -c "python3 '$here/tools/extract_release_notes.py' 'v$ver' '$here/CHANGELOG.md' | grep -q '.'"

echo "$fails failure(s); work dir $work"
[[ $fails == 0 ]] && rm -rf "$work"
exit $((fails > 0))
