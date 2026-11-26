#!/usr/bin/env bash
# Builds PS5 YouTube Uploader.
#
#   ./build.sh host      Native build for testing on Mac/Linux     -> build/host/ps5-yt-uploader
#   ./build.sh payload   PS5 payload for elfldr (port 9021)        -> build/ps5-yt-uploader.elf
#   ./build.sh title     PS5 title (home-screen tile) PPSA97631    -> build/PPSA97631/
#   ./build.sh package   payload + title + helper + README in zip  -> build/ps5-yt-uploader-<version>.zip
#   ./build.sh release   Cut/verify release via tools/release.sh
#   ./build.sh all       host + payload + title + package
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
deps=${PS5_DEPS:-$(cd -- "$here/.." && pwd)/deps}
sdk=${PS5_PAYLOAD_SDK:-$deps/ps5-sdk-full}
vulkan=${PS5_VULKAN_DIR:-$deps/ps5-vulkan}
out=$here/build
version=$(sed -n 's/^#define APP_VERSION "\(.*\)"/\1/p' "$here/src/app.h")
srcs=("$here"/src/*.c)
warn=(-std=c99 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers -Wno-format-truncation -Wno-unknown-warning-option)
docker_image=${PS5YT_DOCKER_IMAGE:-ps5-yt-uploader-sdk:latest}

file_size() {
    stat -c %s "$1" 2>/dev/null || stat -f %z "$1"
}

find_ca() {
    if [[ -n ${CA_BUNDLE:-} && -f ${CA_BUNDLE} ]]; then
        echo "$CA_BUNDLE"
        return 0
    fi
    for p in /etc/ssl/certs/ca-certificates.crt /etc/ssl/cert.pem /etc/pki/tls/certs/ca-bundle.crt; do
        if [[ -f $p ]]; then
            echo "$p"
            return 0
        fi
    done
    echo "CA bundle not found; set CA_BUNDLE" >&2
    exit 1
}

gen_assets() {
    local ca
    ca=$(find_ca)
    mkdir -p "$out/gen"
    python3 -I "$here/tools/embed.py" "$here/web" "$ca" "$out/gen/assets.c"
}

ensure_docker_image() {
    command -v docker >/dev/null 2>&1 || {
        echo "PS5 SDK not found at $sdk and docker is not installed." >&2
        exit 1
    }
    if ! docker image inspect "$docker_image" >/dev/null 2>&1; then
        echo "Building Docker PS5 SDK image ($docker_image)..."
        docker build --platform linux/amd64 -t "$docker_image" -f "$here/docker/Dockerfile.ps5" "$here"
    fi
}

run_in_docker() {
    ensure_docker_image
    docker run --rm --platform linux/amd64 \
        -v "$here:/work" -w /work \
        -e PS5_DEPS=/opt/deps \
        -e PS5_PAYLOAD_SDK=/opt/deps/ps5-sdk-full \
        -e PS5_VULKAN_DIR=/opt/deps/ps5-vulkan \
        -e PS5_SDK_DIR=/opt/deps/ps5-sdk-full \
        -e CA_BUNDLE=/etc/ssl/certs/ca-certificates.crt \
        "$docker_image" "$@"
}

build_host() {
    gen_assets
    local curl_dir=${HOST_CURL:-}
    if [[ -z $curl_dir ]]; then
        for d in /opt/homebrew/opt/curl /home/linuxbrew/.linuxbrew/opt/curl; do
            if [[ -d $d/include && -d $d/lib ]]; then
                curl_dir=$d
                break
            fi
        done
    fi
    local cflags=()
    local ldflags=(-lcurl -lpthread)
    if [[ -n $curl_dir ]]; then
        cflags+=(-I"$curl_dir/include")
        ldflags=(-L"$curl_dir/lib" -Wl,-rpath,"$curl_dir/lib" -lcurl -lpthread)
    fi
    mkdir -p "$out/host"
    ${CC:-cc} -O2 -g "${warn[@]}" -I"$here/src" "${cflags[@]}" "${srcs[@]}" "$out/gen/assets.c" \
        "${ldflags[@]}" -o "$out/host/ps5-yt-uploader"
    echo "host: $out/host/ps5-yt-uploader"
}

build_payload() {
    if [[ -x $sdk/bin/prospero-clang ]]; then
        gen_assets
        local hb=$sdk/target/user/homebrew
        local libs=()
        if [[ -x $sdk/bin/prospero-pkg-config ]] && \
           PS5_PAYLOAD_SDK=$sdk PKG_CONFIG_PATH="$hb/lib/pkgconfig" "$sdk/bin/prospero-pkg-config" --exists libcurl 2>/dev/null; then
            read -r -a libs <<< "$(PS5_PAYLOAD_SDK=$sdk PKG_CONFIG_PATH="$hb/lib/pkgconfig" "$sdk/bin/prospero-pkg-config" --static --libs libcurl)"
            libs+=(-lpthread)
        else
            libs=(-lcurl -lssl -lcrypto -lz -lpthread)
        fi
        mkdir -p "$out"
        PS5_PAYLOAD_SDK=$sdk "$sdk/bin/prospero-clang" -O2 "${warn[@]}" -D_XOPEN_SOURCE=700 -DCURL_STATICLIB -I"$here/src" -I"$hb/include" \
            "${srcs[@]}" "$out/gen/assets.c" -L"$hb/lib" "${libs[@]}" \
            -o "$out/ps5-yt-uploader.elf"
        local strip_bin="$sdk/bin/llvm-strip"
        [[ -x $strip_bin ]] || strip_bin=$(command -v llvm-strip || command -v llvm-strip-18 || true)
        if [[ -n $strip_bin ]]; then
            "$strip_bin" --strip-debug "$out/ps5-yt-uploader.elf" 2>/dev/null || true
        fi
        echo "payload: $out/ps5-yt-uploader.elf ($(file_size "$out/ps5-yt-uploader.elf") bytes)"
    else
        run_in_docker ./build.sh payload
    fi
}

build_title() {
    if [[ -x $vulkan/build/host/ps5-native-tool && -x $sdk/bin/prospero-lld ]]; then
        [[ -f $out/ps5-yt-uploader.elf ]] || build_payload
        bash "$here/title/build_title.sh" "$out"
    else
        run_in_docker ./build.sh title
    fi
}

package() {
    [[ -f $out/ps5-yt-uploader.elf ]] || build_payload
    [[ -f $out/PPSA97631/eboot.bin ]] || build_title
    local pkg=$out/ps5-yt-uploader-$version
    rm -rf "$pkg" "$pkg.zip"
    mkdir -p "$pkg"
    cp "$out/ps5-yt-uploader.elf" "$pkg/"
    cp -r "$out/PPSA97631" "$pkg/"
    cp "$here/tools/yt-login.py" "$pkg/"
    cp "$here/README.md" "$pkg/"
    (cd "$out" && python3 -I -m zipfile -c "ps5-yt-uploader-$version.zip" "ps5-yt-uploader-$version")
    echo "package: $pkg.zip"
}

case ${1:-all} in
host) build_host ;;
payload) build_payload ;;
title) build_title ;;
package) package ;;
release) shift; bash "$here/tools/release.sh" "$@" ;;
all) build_host && build_payload && build_title && package ;;
*) echo "usage: $0 host|payload|title|package|release|all" >&2; exit 2 ;;
esac
