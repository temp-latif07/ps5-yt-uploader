#!/usr/bin/env bash
# Builds the PPSA97631 title (the home-screen tile that starts the server payload).
#
#   title/build_title.sh OUT_DIR       -> OUT_DIR/PPSA97631/
set -euo pipefail
out=$1
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
deps=${PS5_DEPS:-$(cd -- "$here/../.." && pwd)/deps}
vulkan=${PS5_VULKAN_DIR:-$deps/ps5-vulkan}
sdk=${PS5_SDK_DIR:-${PS5_PAYLOAD_SDK:-$deps/ps5-sdk-full}}

tool=$vulkan/build/host/ps5-native-tool
native=$vulkan/tooling/native
work=$out/title-work
title=$out/PPSA97631
[[ -f $out/ps5-yt-uploader.elf ]] || { echo "build the payload first (./build.sh payload)" >&2; exit 1; }
for f in "$tool" "$sdk/bin/prospero-lld" "$native/app_crt.cpp" "$native/app_cpp_runtime.cpp" "$native/ps5-pie.ld" "$native/app-symbols.map" "$vulkan/runtime/libc.prx"; do
    [[ -e $f ]] || { echo "missing $f (set PS5_VULKAN_DIR / PS5_SDK_DIR or use Docker build)" >&2; exit 1; }
done

llvm_nm="$sdk/bin/llvm-nm"
[[ -x $llvm_nm ]] || llvm_nm=$(command -v llvm-nm || command -v llvm-nm-18)

rm -rf "$work"
mkdir -p "$work/obj"
cc() { PS5_PAYLOAD_SDK="$sdk" sh "$vulkan/tooling/prospero-clang18" "$@"; }

cc -std=c++20 -O2 -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections -c "$native/app_crt.cpp" -o "$work/obj/app_crt.o"
cc -std=c++20 -O2 -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections -c "$native/app_cpp_runtime.cpp" -o "$work/obj/app_cpp_runtime.o"
cc -std=c11 -O2 -Wall -Wextra -ffunction-sections -fdata-sections -c "$here/launcher.c" -o "$work/obj/launcher.o"

"$sdk/bin/prospero-lld" -T "$native/ps5-pie.ld" --eh-frame-hdr --gc-sections \
    --version-script "$native/app-symbols.map" --exclude-libs=ALL -e _start -o "$work/launcher.elf" \
    "$work/obj/app_crt.o" "$work/obj/app_cpp_runtime.o" "$work/obj/launcher.o" \
    --as-needed "$sdk"/target/lib/*.so

null_imports=$(comm -23 \
    <("$llvm_nm" -D --undefined-only "$work/launcher.elf" | awk '$1 == "U" { sub(/@.*/, "", $2); print $2 }' | sort -u) \
    <(for library in "$sdk"/target/lib/*.so; do
        case ${library##*/} in libkernel_sys.so | libScePosixForWebKit.so) continue ;; esac
        "$llvm_nm" -D --defined-only "$library" 2>/dev/null | awk '{ print $NF }'
    done | sort -u))
if [[ -n $null_imports ]]; then
    echo "imports that would be NULL in a title: $null_imports" >&2
    exit 1
fi

"$tool" link --in "$work/launcher.elf" --out "$work/eboot.elf" --stub-dir "$sdk/target/lib" \
    --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf
rm -rf "$title"
mkdir -p "$title/sce_sys" "$title/sce_module"
"$tool" self --sign --in "$work/eboot.elf" --out "$title/eboot.bin" --magic 0x1D3D154F
"$tool" self --inspect --file "$title/eboot.bin" > /dev/null
(cd "$vulkan/runtime" && sha256sum --check --strict --quiet libc.prx.sha256)
cp "$vulkan/runtime/libc.prx" "$title/sce_module/libc.prx"
cp "$here/param.json" "$title/sce_sys/param.json"
python3 -I "$here/make_icon.py" "$title/sce_sys/icon0.png"
cp "$out/ps5-yt-uploader.elf" "$title/ps5-yt-uploader.elf"
chmod 755 "$title/eboot.bin" "$title/sce_module/libc.prx"
printf 'title: %s (eboot.bin %s bytes)\n' "$title" "$(stat -c %s "$title/eboot.bin" 2>/dev/null || stat -f %z "$title/eboot.bin")"
