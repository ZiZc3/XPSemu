#!/usr/bin/env bash
# Link xemu (build-ps5, from ps5/configure-ps5.sh) as a PS5 title in dist/PPSA97358.
#
# xemu's own link line is for a desktop executable; this relinks its objects and
# libraries the way PS5_Vulkan links titles that use RADV (tools/radv-link.sh and
# tools/build-radv-title.sh there): its CRT and C++ allocation runtime, the AGC
# link stubs, the payload SDK's libc++ and platform layer, then ps5-native-tool
# to make the signed eboot.bin, with PS5_Vulkan's libc.prx beside it.
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
src=$(dirname "$here")
build=$src/build-ps5
vk=${PS5_VULKAN:-$HOME/ps5/PS5_Vulkan}
sdk_root=${PS5_PAYLOAD_SDK:-$vk/.deps/native/ps5-payload-sdk}
archive=${RADV_ARCHIVE:-$vk/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a}
native=$vk/tooling/native
tool=$vk/build/host/ps5-native-tool
param=$here/sce_sys/param.json
work=$build/title
python3 "$here/embed-default-config.py"

for file in "$build/qemu-system-i386.rsp" "$archive" "$tool" "$param" "$sdk_root/bin/prospero-lld"; do
    [[ -e $file ]] || { echo "missing $file" >&2; exit 2; }
done

title_id=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$param")
module_sdk=0x02000009
companion_sdk=0x08050001
fself_magic=0x1D3D154F

mkdir -p "$work/obj" "$work/stubs"
cc() { PS5_PAYLOAD_SDK="$sdk_root" sh "$vk/tooling/prospero-clang18" "$@"; }

# Make sure every object and library is current.
ninja -C "$build" qemu-system-i386.p/ > /dev/null 2>&1 || true
mapfile -t objects < <(python3 - "$build" <<'PY'
import re, shlex, sys
build = sys.argv[1]
text = open(f"{build}/build.ninja").read()
m = re.search(r"^build qemu-system-i386: \S+ (.*?)(?: \|.*)?$", text, re.M)
inputs = [t for t in shlex.split(m.group(1).replace("$ ", " ")) if t.endswith(".o")]
args = re.search(r"^build qemu-system-i386:.*?\n LINK_ARGS = (.*)$", text, re.M | re.S)
args = shlex.split(args.group(1).split("\n")[0])
# Archives, in order; --whole-archive ones (the embedded data) marked.
whole = False
libs = []
for a in args:
    if a == "-Wl,--whole-archive": whole = True; continue
    if a == "-Wl,--no-whole-archive": whole = False; continue
    # RADV comes whole from the recipe below, with the same zlib (1.3.1) in it.
    if a.endswith((".a", ".fa")) and not a.endswith(("libvulkan_radeon.ps5.a", "/libz.a")):
        libs.append(("W:" if whole else "") + a)
for t in inputs + libs:
    print(t)
PY
)
ninja -C "$build" "${objects[@]#W:}" > /dev/null

xemu_inputs=()
for item in "${objects[@]}"; do
    if [[ $item == W:* ]]; then
        xemu_inputs+=(--whole-archive "$build/${item#W:}" --no-whole-archive)
    elif [[ $item == /* ]]; then
        xemu_inputs+=("$item")
    else
        xemu_inputs+=("$build/$item")
    fi
done

# TLS for curl (ps5/deps build_mbedtls): curl's static library needs it,
# and meson's link line names only libcurl.a.
for lib in mbedtls mbedx509 mbedcrypto; do
    xemu_inputs+=("$here/deps/prefix/lib/lib$lib.a")
done

cc -std=c++20 -O2 -fno-exceptions -fno-rtti -c "$native/app_crt.cpp" -o "$work/obj/app_crt.o"
cc -std=c++20 -O2 -fno-exceptions -fno-rtti -c "$native/app_cpp_runtime.cpp" -o "$work/obj/app_cpp_runtime.o"

# AGC comes from system modules; these host-link stubs only name the imports.
stub() {
    local library=$1 source=$2
    cc -std=c11 -O2 -fPIC -c "$vk/$source" -o "$work/obj/${library}_stub.o"
    "$sdk_root/bin/prospero-lld" --shared -soname "${library}.prx" \
        -o "$work/stubs/${library}.so" "$work/obj/${library}_stub.o"
}
stub libSceAgc vendor/ps5/sdk/stubs/agc_canary_link_stub.c
stub libSceAgcDriver vendor/ps5/sdk/stubs/agc_driver_canary_link_stub.c

# shellcheck source=/dev/null
source "$vk/tools/radv-link.sh"
radv_link_recipe "$vk" "$sdk_root" "$archive" || exit 2
# Anonymous memory from flexible memory (ps5/compat/mmap.c): QEMU's JIT buffer
# and guest RAM.
radv_link_flags+=(--wrap=mmap --wrap=munmap)
# The real IDs for the effective ones (Mesa's disk cache, ps5/compat/libc_stubs.c).
radv_link_flags+=(--wrap=geteuid --wrap=getegid)
# QEMU's coroutines start without the ucontext system calls (ps5/compat/ucontext.c).
radv_link_flags+=(--wrap=getcontext --wrap=makecontext --wrap=swapcontext)
# libc functions the console leaves unresolved for a title (its GOT slot stays
# 0: xemu's import report found these), bound to ps5/compat/libc_missing.c and
# kept local so the title doesn't export libc's names (as radv-link.sh does).
missing_libc=(mkstemp isatty umask pathconf fnmatch getnameinfo gai_strerror
    gethostbyname fork vfork setsid chroot symlink link readlink setbuf)
{
    printf '{\n    local:\n'
    for name in "${missing_libc[@]}"; do
        radv_link_flags+=("--defsym=$name=xemu_ps5_$name")
        printf '        %s;\n' "$name"
    done
    printf '};\n'
} > "$work/libc-missing-local.map"
radv_link_flags+=(--version-script "$work/libc-missing-local.map")
"$sdk_root/bin/prospero-lld" "${radv_linker_script[@]}" --error-limit=0 \
    --thinlto-jobs="$(nproc)" --eh-frame-hdr --no-dynamic-linker \
    -z nodynamic-undefined-weak "${radv_link_flags[@]}" \
    --version-script "$native/app-symbols.map" --exclude-libs=ALL \
    -e _start -o "$work/llvm-pie.elf" \
    "$work/obj/app_crt.o" "$work/obj/app_cpp_runtime.o" \
    --start-group "${xemu_inputs[@]}" --end-group \
    "$work/stubs/libSceAgc.so" "$work/stubs/libSceAgcDriver.so" \
    "${radv_link_inputs[@]}" \
    --as-needed "$sdk_root"/target/lib/*.so
"$tool" link --in "$work/llvm-pie.elf" --out "$work/eboot.elf" \
    --stub-dir "$sdk_root/target/lib" --stub "$work/stubs/libSceAgc.so" \
    --stub "$work/stubs/libSceAgcDriver.so" --module-sdk "$module_sdk" \
    --companion-sdk "$companion_sdk" --file-name eboot.elf

app=$src/dist/$title_id
rm -rf -- "$app"
mkdir -p "$app/sce_sys" "$app/sce_module"
"$tool" self --sign --in "$work/eboot.elf" --out "$app/eboot.bin" --magic "$fself_magic"
cp "$param" "$app/sce_sys/param.json"
# Every import's GOT slot, for xemu to report the ones the console leaves empty
# (xemu_ps5_early_init): a function the SDK's stubs name may still have no
# provider at run time, and calling it jumps to 0 (mkstemp did).
llvm-readelf-18 -r --wide "$work/llvm-pie.elf" |
    awk '$3 == "R_X86_64_GLOB_DAT" || $3 == "R_X86_64_JUMP_SLOT" { print $1, $5 }' \
    > "$app/imports.txt"
for asset in icon0.png pic0.dds pic1.dds; do
    if [[ -f $here/sce_sys/$asset ]]; then
        cp "$here/sce_sys/$asset" "$app/sce_sys/$asset"
    elif [[ -f $vk/sce_sys/$asset ]]; then
        cp "$vk/sce_sys/$asset" "$app/sce_sys/$asset" # placeholder art
    fi
done
[[ -f $vk/runtime/libc.prx ]] || bash "$vk/tools/rebuild-libc.sh"
(cd "$vk/runtime" && sha256sum --check --strict --quiet libc.prx.sha256)
cp "$vk/runtime/libc.prx" "$app/sce_module/libc.prx"
"$tool" self --inspect --file "$app/eboot.bin" > /dev/null
printf 'xemu title: %s (%s bytes)\n' "$app" "$(stat -c %s "$app/eboot.bin")"
