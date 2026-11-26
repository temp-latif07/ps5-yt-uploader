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
    "$here"/src/json.c "$here"/src/sha1.c "$here"/src/httpd.c "$here"/src/net.c \
    "${uldflags[@]}" -o "$unit_bin"
"$unit_bin"
rm -f "$unit_bin"

# Build host binary
"$here/build.sh" host

work=$(mktemp -d "${TMPDIR:-/tmp}/ps5yt-test.XXXXXX")
mkdir -p "$work/app" "$work/cap/PPSA01234" "$work/extra"
mport=18999 port=18888
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
    PS5YT_STUDIO_URL="$mbase/studio/" \
    PS5YT_SCOTTY_URL="$mbase/upload/studio" \
    PS5YT_INNERTUBE_BASE="$mbase/youtubei/v1" \
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

# 1. UI & CSRF checks
check "UI page served" bash -c "curl -s $base/ | grep -q 'YouTube Uploader'"
check "app.js served" bash -c "curl -s $base/app.js | grep -q loadStatus"
check "style.css served" bash -c "curl -s $base/style.css | grep -q ':focus-visible'"
check "no v2 strings in web/" bash -c "! grep -ri 'v2' $here/web/ | grep -v 'favicon.svg' | grep -q ."
check "not logged in initially" bash -c "[[ \$(get status | jq_ 'd[\"logged_in\"]') == False ]]"
check "POST without X-PS5YT refused (403)" bash -c "[[ \$(curl -s -o /dev/null -w '%{http_code}' -d cookies=x $base/api/login) == 403 ]]"

# 2. Invalid vs Valid raw Cookie header login
r=$(post login --data-urlencode "cookies=SAPISID=bad-token; SID=sid123")
check "invalid cookies refused (401)" bash -c "grep -qi 'expired\|signed out\|rejected' <<<'$r'"
check "invalid cookies not saved" test ! -e "$work/app/cookies.txt"

r=$(post login --data-urlencode "cookies=Cookie: SAPISID=valid-sapisid; SID=sid123; __Secure-3PAPISID=valid-3p")
check "raw Cookie login accepted" bash -c "[[ \$(echo '$r' | jq_ 'd[\"logged_in\"]') == True ]] && grep -q 'PS5 Test Channel' <<<'$r'"
check "cookies.txt mode 600" bash -c "[[ \$(file_mode '$work/app/cookies.txt') == 600 ]]"
check "Set-Cookie rotation persisted" grep -q "YT_ROTATED" "$work/app/cookies.txt"
check "cookies never leaked in status" bash -c "! get status | grep -q 'valid-sapisid'"
check "cookies never leaked in logs" bash -c "! grep -rqE 'valid-sapisid|valid-3p|sid123' '$work/app/log.txt' '$work/app.log'"

# 3. Netscape cookies + Brand Account login
ns_cookies=$(printf "# Netscape HTTP Cookie File\n.youtube.com\tTRUE\t/\tTRUE\t2147483647\tSAPISID\tvalid-brand\n#HttpOnly_.google.com\tTRUE\t/\tTRUE\t2147483647\tSID\tsid_brand\n")
r=$(post login --data-urlencode "cookies=$ns_cookies" -d channel_id=UC_BRAND -d delegated_session_id=10987654321 -d session_index=1)
check "Netscape Brand Account login" bash -c "grep -q 'Brand Gaming' <<<'$r' && grep -q '10987654321' <<<'$r'"

# 4. Clips scanning, folder picker, Range streaming & security
make_clip "$work/cap/clip2.webm" 1
sleep 1.1
make_clip "$work/cap/PPSA01234/clip1.mp4" 9
make_clip "$work/extra/usb_clip.mp4" 2
check "clips sorted newest first with title_id" bash -c "get clips | jq_ 'len(d[\"clips\"])==2 and d[\"clips\"][0][\"name\"]==\"clip1.mp4\" and d[\"clips\"][0][\"title_id\"]==\"PPSA01234\" and d[\"clips\"][1][\"name\"]==\"clip2.webm\"' | grep -q True"
check "dirs lists subdirs and refuses /etc and .." bash -c "get 'dirs?path=$work/cap' | grep -q PPSA01234 && get 'dirs?path=/etc' | grep -q outside && get 'dirs?path=$work/cap/../..' | grep -q outside"
check "stream returns 206 Partial Content" bash -c "[[ \$(curl -s -o /dev/null -w '%{http_code}' -H 'Range: bytes=0-99' '$base/api/stream?path=$work/cap/PPSA01234/clip1.mp4') == 206 ]]"
check "stream blocks cookies.txt (403)" bash -c "[[ \$(curl -s -o /dev/null -w '%{http_code}' '$base/api/stream?path=$work/app/cookies.txt') == 403 ]]"

# 5. Upload with Brand Account + playlist + 4 MB chunk + SHA-256 verification
post settings -d chunk_mb=4 > /dev/null
r=$(post upload --data-urlencode "path=$work/cap/PPSA01234/clip1.mp4" -d "title=Epic Victory" \
    --data-urlencode "description=Line1
Line2" -d privacy=PUBLIC -d made_for_kids=0 -d playlist_id=PL123)
jid1=$(echo "$r" | jq_ 'd["id"]')
check "upload 1 finishes" wait_jobs 30
want_sha=$(sha_of "$work/cap/PPSA01234/clip1.mp4")
check "uploaded SHA-256, Brand Account headers, createvideo & playlist match" bash -c "
curl -s '$mbase/control/stats' | python3 -I -c '
import json,sys; s=json.load(sys.stdin)
cv=s[\"createvideo\"][-1]; mu=s[\"metadata_update\"][-1]
assert cv[\"sha256\"]==\"$want_sha\"
assert cv[\"channel_id\"]==\"UC_BRAND\" and cv[\"x_goog_pageid\"]==\"10987654321\" and cv[\"on_behalf_of\"]==\"10987654321\"
assert cv[\"title\"]==\"Epic Victory\" and cv[\"privacy\"]==\"PUBLIC\"
assert mu[\"new_mfk\"]==\"MDE_MADE_FOR_KIDS_TYPE_NOT_MFK\" and mu[\"playlists\"]==[\"PL123\"]
'
"
check "clip marked uploaded in /api/clips" bash -c "get clips | grep -q 'VID_1'"

# 6. Made for Kids omits playlist
post upload --data-urlencode "path=$work/cap/clip2.webm" -d title="Kids Clip" -d made_for_kids=1 -d playlist_id=PL_SHOULD_OMIT > /dev/null
check "MFK upload finishes" wait_jobs 20
check "MFK omits addToPlaylist" bash -c "curl -s '$mbase/control/stats' | python3 -I -c 'import json,sys; mu=json.load(sys.stdin)[\"metadata_update\"][-1]; assert mu[\"new_mfk\"]==\"MDE_MADE_FOR_KIDS_TYPE_MFK\" and mu[\"playlists\"]==[]'"

# 7. Mid-stream network drop recovery via Scotty query
make_clip "$work/cap/drop_clip.mp4" 9
curl -s "$mbase/control?drop_once_after=$((1048576))" > /dev/null
post upload --data-urlencode "path=$work/cap/drop_clip.mp4" -d title="Drop Recovery" > /dev/null
check "drop recovery finishes" wait_jobs 30
drop_sha=$(sha_of "$work/cap/drop_clip.mp4")
check "drop recovery SHA-256 exact" bash -c "curl -s '$mbase/control/stats' | python3 -I -c 'import json,sys; s=json.load(sys.stdin); assert s[\"createvideo\"][-1][\"sha256\"]==\"$drop_sha\" and s[\"scotty_queries\"]>=1'"

# 8. Pause + server restart + resume + expired 410 Scotty re-init
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
check "pause+restart resume SHA-256 exact" bash -c "curl -s '$mbase/control/stats' | python3 -I -c 'import json,sys; assert json.load(sys.stdin)[\"createvideo\"][-1][\"sha256\"]==\"$pause_sha\"'"

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
check "expired 410 SHA-256 exact" bash -c "curl -s '$mbase/control/stats' | python3 -I -c 'import json,sys; assert json.load(sys.stdin)[\"createvideo\"][-1][\"sha256\"]==\"$exp_sha\"'"

# 9. Partial failure after createvideo (metadata_update 500)
make_clip "$work/cap/partial_clip.mp4" 2
curl -s "$mbase/control?fail_metadata=1" > /dev/null
before_cv=$(curl -s "$mbase/control/stats" | jq_ 'len(d["createvideo"])')
jid_m=$(post upload --data-urlencode "path=$work/cap/partial_clip.mp4" -d title="Partial Meta" | jq_ 'd["id"]')
check "partial metadata failure still marks done" wait_jobs 20
after_cv=$(curl -s "$mbase/control/stats" | jq_ 'len(d["createvideo"])')
check "partial failure keeps video_id + warning note + 1 createvideo call" bash -c "
[[ $((after_cv - before_cv)) == 1 ]] && get jobs | python3 -I -c '
import json,sys; j=[x for x in json.load(sys.stdin)[\"jobs\"] if x[\"id\"]==$jid_m][0]
assert j[\"state\"]==\"done\" and j[\"video_id\"].startswith(\"VID_\") and \"check YouTube Studio\" in j[\"note\"]
'
"

# 10. 401 fail-fast & cancel
make_clip "$work/cap/fail401.mp4" 2
curl -s "$mbase/control?force_401=1" > /dev/null
jid_401=$(post upload --data-urlencode "path=$work/cap/fail401.mp4" -d title="Fail 401" | jq_ 'd["id"]')
check "401 fails quickly" wait_jobs 15
check "401 state is failed" bash -c "get jobs | python3 -I -c 'import json,sys; j=[x for x in json.load(sys.stdin)[\"jobs\"] if x[\"id\"]==$jid_401][0]; assert j[\"state\"]==\"failed\" and \"expired\" in j[\"err\"]'"
curl -s "$mbase/control?force_401=0" > /dev/null

make_clip "$work/cap/cancel_clip.mp4" 16
curl -s "$mbase/control?rate=1500000" > /dev/null
jid_c=$(post upload --data-urlencode "path=$work/cap/cancel_clip.mp4" -d title="Cancel Me" | jq_ 'd["id"]')
sleep 1.0
post jobs/cancel -d id="$jid_c" > /dev/null
check "cancel stops upload" wait_jobs 15
check "cancel state is cancelled" bash -c "get jobs | python3 -I -c 'import json,sys; j=[x for x in json.load(sys.stdin)[\"jobs\"] if x[\"id\"]==$jid_c][0]; assert j[\"state\"]==\"cancelled\"'"
curl -s "$mbase/control?rate=0" > /dev/null

# 11. Auto-upload watcher: off by default, never backfills existing clips, uploads brand-new clip
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

# 12. Logout, quit, helper tools, and release dry-run checks
post logout > /dev/null
check "logout deletes cookies.txt and session.txt" bash -c "test ! -e '$work/app/cookies.txt' && test ! -e '$work/app/session.txt'"
post quit > /dev/null
sleep 1
check "quit stops server" bash -c "! curl -s -m 2 '$base/api/status' > /dev/null"

check "yt-login self-test" python3 "$here/tools/yt-login.py" --self-test
check "make_icon produces PNG" bash -c "python3 '$here/title/make_icon.py' '$work/icon.png' && test -s '$work/icon.png'"
check "payloads.json valid" python3 "$here/tools/update_payloads.py" --check "$here/payloads.json"
check "extract_release_notes finds 2.0.0" bash -c "python3 '$here/tools/extract_release_notes.py' v2.0.0 '$here/CHANGELOG.md' | grep -q 'PPSA97631'"

echo "$fails failure(s); work dir $work"
[[ $fails == 0 ]] && rm -rf "$work"
exit $((fails > 0))
