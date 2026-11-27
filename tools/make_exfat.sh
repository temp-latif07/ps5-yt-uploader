#!/usr/bin/env bash
# Build a ShadowMountPlus-compatible 64 KiB-cluster exFAT image from a PS5 title directory.
# Usage: tools/make_exfat.sh <input_dir> <output.exfat>
set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 <input_dir> <output.exfat>" >&2
    exit 2
fi

INPUT_DIR=$1
OUTPUT=$2

[[ -d $INPUT_DIR ]] || { echo "Error: directory not found: $INPUT_DIR" >&2; exit 1; }
[[ -f $INPUT_DIR/eboot.bin ]] || { echo "Error: eboot.bin missing in $INPUT_DIR" >&2; exit 1; }

mkdir -p "$(dirname "$OUTPUT")"
OUTPUT="$(cd "$(dirname "$OUTPUT")" && pwd)/$(basename "$OUTPUT")"

CLUSTER_SIZE=65536
RAW_BYTES=$(python3 -c '
import os, sys
tot = 0
for r, _, fs in os.walk(sys.argv[1]):
    for f in fs:
        tot += os.path.getsize(os.path.join(r, f))
print(tot)
' "$INPUT_DIR")
ALLOC_BYTES=$((RAW_BYTES + 12 * 1024 * 1024))
MB=$(( (ALLOC_BYTES + 1024 * 1024 - 1) / (1024 * 1024) ))
if [[ $MB -lt 16 ]]; then
    MB=16
fi

MOUNT_POINT=$(mktemp -d)
LOOP_DEV=""
cleanup() {
    if [[ -n $MOUNT_POINT ]] && mount | grep -q "on $MOUNT_POINT "; then
        umount "$MOUNT_POINT" 2>/dev/null || true
    fi
    if [[ -n $LOOP_DEV ]]; then
        hdiutil detach "$LOOP_DEV" 2>/dev/null || true
    fi
    if [[ -n $MOUNT_POINT ]]; then
        rmdir "$MOUNT_POINT" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

rm -f "$OUTPUT"

if [[ $(uname -s) == Darwin ]]; then
    mkfile -n "${MB}m" "$OUTPUT"
    ATTACH_OUT=$(hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount "$OUTPUT")
    LOOP_DEV=$(printf '%s\n' "$ATTACH_OUT" | awk 'NR==1 {print $1; exit}')
    [[ -n $LOOP_DEV ]] || { echo "Error: hdiutil attach failed for $OUTPUT" >&2; exit 1; }
    newfs_exfat -b "$CLUSTER_SIZE" -v PPSA97631 "$LOOP_DEV"
    mount -t exfat -o nobrowse "$LOOP_DEV" "$MOUNT_POINT"
    COPYFILE_DISABLE=1 rsync -r --exclude='.DS_Store' --exclude='._*' "$INPUT_DIR"/ "$MOUNT_POINT"/
    find "$MOUNT_POINT" \( -name '._*' -o -name '.DS_Store' \) -delete 2>/dev/null || true
    rm -rf "$MOUNT_POINT/.fseventsd" "$MOUNT_POINT/.Spotlight-V100" "$MOUNT_POINT/.Trashes" 2>/dev/null || true
    sync
    umount "$MOUNT_POINT"
    hdiutil detach "$LOOP_DEV"
    LOOP_DEV=""
else
    truncate -s "${MB}M" "$OUTPUT"
    mkfs.exfat -c 64K -L PPSA97631 "$OUTPUT"
    if command -v mount.exfat-fuse >/dev/null 2>&1; then
        mount -t exfat-fuse -o loop "$OUTPUT" "$MOUNT_POINT"
    else
        mount -o loop "$OUTPUT" "$MOUNT_POINT"
    fi
    rsync -r --exclude='.DS_Store' --exclude='._*' "$INPUT_DIR"/ "$MOUNT_POINT"/
    sync
    umount "$MOUNT_POINT"
fi

echo "exfat: $OUTPUT (${MB} MB, 64 KiB clusters)"
