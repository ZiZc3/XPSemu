#!/usr/bin/env bash
# Builds XPSemu 3: hakuX's core, our PS5 layer, RADV, into a PS5 title (dist/).
# Stops on the first stage that fails; prints the errors, most frequent first.
set -u
B=$(cd "$(dirname "$0")/.." && pwd)
export B
core=$B/core
deps=${XOG_DEPS:-$B/deps}            # tools/deps.sh fills it
vk=${PS5_VULKAN:-$HOME/ps5/PS5_Vulkan}   # github.com/mihawk-99/PS5_Vulkan, built
sdk=$vk/.deps/native/ps5-payload-sdk
native=$vk/tooling/native
tool=${PS5_NATIVE_TOOL:-$vk/build/host/ps5-native-tool}
archive=$vk/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a
out=$B/build
PREFIX=${XOG_PREFIX:-$HOME/ps5/prefix}   # curl + mbedTLS built for the PS5 (lib/, include/)
# RELEASE=1: the release, over the old XPSemu (PPSA97358, "XPSemu"); else the test app.
if [ -n "${RELEASE:-}" ]; then ID=PPSA97358; NAME="XPSemu"; CONTENT=XEMUPS5000000001
else ID=PPSA97360; NAME="XPSemu V3"; CONTENT=XOG0000000000001; fi
export ID NAME CONTENT
th="$B/ps5/include/xog_title.h"
want="#define XOG_TITLE \"$ID\""
if [ "$(cat "$th" 2>/dev/null)" != "$want" ]; then
  echo "$want" > "$th"
  touch "$B/ps5/src/main_ps5.c" "$B/ps5/ui/dashboard.cc" "$B/ps5/ui/ui_main.cc"
fi
echo "title $ID ($NAME)"
mkdir -p "$out/obj" "$out/err" "$out/stubs" "$out/ld"
cd "$core" || exit 1

[ -f "$deps/glslang/glslang/build_info.h" ] || python3 "$deps/glslang/build_info.py" "$deps/glslang" -i "$deps/glslang/build_info.h.tmpl" -o "$deps/glslang/glslang/build_info.h"

COMMON="-O2 -march=znver2 -w -fno-delete-null-pointer-checks -ffunction-sections -fdata-sections -include xog_ps5.h -D_GNU_SOURCE -D__BSD_VISIBLE=1 -DCONFIG_VULKAN=1 -DHAKUX_HLE_GPU=1 -DHAKUX_NATIVE_CORE=1 -DVK_NO_PROTOTYPES -I $B/ps5/include -I $B/ps5/src -I $core -I $core/native -I $core/native/compat/glib -I $deps/root -I $deps/volk -I $deps/vma/include -I $deps/glslang -I $deps/spirv_reflect -I $deps/root/subprojects/nv2a_vsh_cpu/src -I $deps/libslirp/src -I $core/external/fex/External/xxhash -I $vk/.deps/native/zlib/root/usr/include -I ${PS5_MESA:-$HOME/ps5/PS5_Mesa}/include -DIMGUI_IMPL_VULKAN_USE_VOLK -I $B/ps5/ui -I $B/third_party/imgui -I $B/third_party/imgui/backends -I $B/third_party/stb_image -I $B/third_party/fpng -I $B/third_party/fa -I $PREFIX/include"

list_sources() {
  # hakuX's own lists (native/CMakeLists.txt, cmake/hle.cmake), less FEX and Android.
  grep -oE '(core|dev|net|kernel|state)/[a-z0-9_/]+\.c' native/CMakeLists.txt | grep -v 'net/slirp' | grep -v 'net/nat' | grep -vx 'core/xbe\.c' | sed 's#^#native/#'
  echo hw/xbox/hle/core/xbe.c
  grep -oE 'NC_HLE\}/[a-z0-9_/]+\.c' native/cmake/hle.cmake | sed 's#NC_HLE}#hw/xbox/hle#'
  grep -oE 'NC_APU\}/[a-z0-9_/]+\.c' native/cmake/hle.cmake | sed 's#NC_APU}#native/dev/apu#' | grep -v 'apu/out\.c'
  grep -oE 'CMAKE_CURRENT_SOURCE_DIR\}/hle/[a-z_]+\.c' native/cmake/hle.cmake | sed 's#.*}/#native/#'
  echo native/compat/glib/glib.c native/app/present.c | tr ' ' '\n'
  for f in nv2a_vsh_disassembler nv2a_vsh_cpu nv2a_vsh_emulator nv2a_vsh_emulator_execution_state; do
    echo "$deps/root/subprojects/nv2a_vsh_cpu/src/$f.c"
  done
  echo "$deps/volk/volk.c" "$deps/spirv_reflect/spirv_reflect.c" | tr ' ' '\n'
  ls "$B"/ps5/src/*.c "$B"/ps5/src/*.S
  ls "$B"/ps5/ui/*.cc "$B"/ps5/ui/data/*.c
  ls "$B"/ps5/framegen/src/fg_*.cpp
  im=$B/third_party/imgui
  echo "$im/imgui.cpp" "$im/imgui_draw.cpp" "$im/imgui_tables.cpp" "$im/imgui_widgets.cpp" "$im/backends/imgui_impl_vulkan.cpp" "$im/misc/cpp/imgui_stdlib.cpp" | tr ' ' '
'
  echo "$B/third_party/stb_image/stb_image_impl.c" "$B/third_party/fpng/fpng.cpp" | tr ' ' '
'
  ls "$deps"/glslang/glslang/CInterface/*.cpp "$deps"/glslang/glslang/GenericCodeGen/*.cpp \
     "$deps"/glslang/glslang/MachineIndependent/*.cpp "$deps"/glslang/glslang/MachineIndependent/preprocessor/*.cpp \
     "$deps"/glslang/glslang/OSDependent/Unix/ossource.cpp "$deps"/glslang/glslang/ResourceLimits/*.cpp
  for f in GlslangToSpv InReadableOrder Logger SpvBuilder SpvPostProcess disassemble doc CInterface/spirv_c_interface; do
    echo "$deps/glslang/SPIRV/$f.cpp"
  done
}

one() {
  f=$1
  n=$(echo "$f" | sed 's#^/##' | tr '/' '_')
  o="$out/obj/$n.o"
  if [ -f "$o" ] && [ "$o" -nt "$f" ] && [ "$o" -nt "$B/ps5/include/xog_ps5.h" ] && [ "$o" -nt "$B/core/hw/xbox/hle/host.h" ] && [ "$o" -nt "$B/ps5/src/vmblocks_ps5.inc" ] && [ "$o" -nt "$B/ps5/src/sites_ps5.inc" ] && [ "$o" -nt "$B/core/native/core/vm.h" ] && [ -z "${REBUILD:-}" ]; then echo "OK $f"; return; fi
  case "$f" in
    *.cc) std="-std=gnu++20 -DXPSEMU_DEV=0 -DPSEMU_CUSTOM_DASHBOARD=1 -DXPS_EMBEDDED_MUSIC=1 -DIMGUI_DEFINE_MATH_OPERATORS" ;;
    */imgui/*.cpp) std="-std=gnu++20" ;;
    */framegen/src/*.cpp) std="-std=gnu++20 -include volk.h -I $B/ps5/framegen/include -I $B/ps5/framegen/src" ;;
    *.cpp) std="-std=gnu++17" ;;
    *.S)
      if PS5_PAYLOAD_SDK="$sdk" sh "$vk/tooling/prospero-clang18" -c "$f" -o "$o" > "$out/err/$n.txt" 2>&1; then
        rm -f "$out/err/$n.txt"; echo "OK $f"; else echo "BAD $f"; fi
      return ;;
    *) std="-std=gnu11" ;;
  esac
  if PS5_PAYLOAD_SDK="$sdk" sh "$vk/tooling/prospero-clang18" $std $COMMON -c "$f" -o "$o" > "$out/err/$n.txt" 2>&1; then
    rm -f "$out/err/$n.txt"
    echo "OK $f"
  else
    echo "BAD $f"
  fi
}
export -f one
export out vk sdk COMMON REBUILD
rm -f "$out"/err/*.txt
list_sources | sort -u > "$out/sources.txt"
xargs -P "$(nproc)" -I{} bash -c 'one "$@"' _ {} < "$out/sources.txt" > "$out/result.txt"
echo "compile: $(grep -c '^OK' "$out/result.txt") ok, $(grep -c '^BAD' "$out/result.txt") failed, of $(wc -l < "$out/sources.txt")"
if grep -q '^BAD' "$out/result.txt"; then
  grep '^BAD' "$out/result.txt" | cut -c5- | head -30
  cat "$out"/err/*.txt | grep -E 'error:' | sed -E 's/^[^ ]+ //' | cut -c1-125 | sort | uniq -c | sort -rn | head -25
  for e in "$out"/err/*.txt; do grep -m2 -H 'error:' "$e"; done | sed "s#$out/err/##" | cut -c1-170 | head -40
  exit 1
fi

# our app objects run under the crt
cc() { PS5_PAYLOAD_SDK="$sdk" sh "$vk/tooling/prospero-clang18" "$@"; }
cc -std=c++20 -O2 -fno-exceptions -fno-rtti -c "$native/app_crt.cpp" -o "$out/stubs/app_crt.o"
cc -std=c++20 -O2 -fno-exceptions -fno-rtti -c "$native/app_cpp_runtime.cpp" -o "$out/stubs/app_cpp_runtime.o"
stub() {
  cc -std=c11 -O2 -fPIC -c "$vk/$2" -o "$out/stubs/$1_stub.o"
  "$sdk/bin/prospero-lld" --shared -soname "$1.prx" -o "$out/stubs/$1.so" "$out/stubs/$1_stub.o"
}
stub libSceAgc vendor/ps5/sdk/stubs/agc_canary_link_stub.c
stub libSceAgcDriver vendor/ps5/sdk/stubs/agc_driver_canary_link_stub.c

source "$vk/tools/radv-link.sh"
radv_link_recipe "$vk" "$sdk" "$archive" || exit 2
sed -E 's/^([[:space:]]*\.text)[[:space:]]+[^:]*:/\1 0x3000000000 :/' "$vk/tooling/native/ps5-pie.ld" > "$out/ld/ps5-pie.ld"
cp "$vk/tooling/psbc/ps5-pie-unwind.ld" "$out/ld/ps5-pie-unwind.ld"
echo 'HIDDEN(__dso_handle = ADDR(.text));' >> "$out/ld/ps5-pie-unwind.ld"
radv_linker_script=(-T "$out/ld/ps5-pie-unwind.ld" -L "$out/ld")
zlib=$vk/.deps/native/zlib/root/usr/lib/libz.a
# libc gaps, bound to ps5/src/libc_ps5.c and kept local: a title that exports libc's names is refused
{
  printf '{\n    local:\n'
  for name in memmem nftw strdup gai_strerror pthread_once clock_nanosleep readlink pathconf mkstemp futimens utimensat pipe2 if_nametoindex; do
    radv_link_flags+=("--defsym=$name=rx_$name")
    printf '        %s;\n' "$name"
  done
  printf '};\n'
} > "$out/libc-local.map"
radv_link_flags+=(--version-script "$out/libc-local.map")
radv_link_flags+=(--wrap=sceVideoOutIsOutputSupported --wrap=pthread_sigmask --wrap=exit --wrap=_exit --wrap=abort --wrap=geteuid --wrap=getegid)

"$sdk/bin/prospero-lld" "${radv_linker_script[@]}" --error-limit=0 --gc-sections --eh-frame-hdr --no-dynamic-linker \
  -z nodynamic-undefined-weak "${radv_link_flags[@]}" --version-script "$native/app-symbols.map" --exclude-libs=ALL \
  -e _start -o "$out/llvm-pie.elf" "$out/stubs/app_crt.o" "$out/stubs/app_cpp_runtime.o" \
  --start-group "$out"/obj/*.o --end-group "$out/stubs/libSceAgc.so" "$out/stubs/libSceAgcDriver.so" \
  "${radv_link_inputs[@]}" $PREFIX/lib/libcurl.a $PREFIX/lib/libmbedtls.a $PREFIX/lib/libmbedx509.a $PREFIX/lib/libmbedcrypto.a --as-needed "$sdk"/target/lib/*.so > "$out/link.txt" 2>&1
rc=$?
grep -v 'unknown -z' "$out/link.txt" | grep -E 'undefined symbol|duplicate symbol|error:' | sed -E 's/^ld.lld: error: //' | sort | uniq -c | sort -rn | cut -c1-120 | head -70
[ $rc -eq 0 ] || { echo "LINK FAILED ($(grep -c 'undefined symbol' "$out/link.txt") undefined)"; exit 1; }

"$tool" link --in "$out/llvm-pie.elf" --out "$out/eboot.elf" --stub-dir "$sdk/target/lib" \
  --stub "$out/stubs/libSceAgc.so" --stub "$out/stubs/libSceAgcDriver.so" --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf || exit 1
app=$B/dist/$ID
rm -rf "$app"
mkdir -p "$app/sce_sys" "$app/sce_module"
"$tool" self --sign --in "$out/eboot.elf" --out "$app/eboot.bin" --magic 0x1D3D154F || exit 1
python3 - "$B/ps5/sce_sys/param.json" "$app/sce_sys/param.json" <<'P'
import json,sys,os
p=json.load(open(sys.argv[1]))
i=os.environ["ID"]
p["titleId"]=i
p["attribute3"]=524352   # 120 Hz declared; the driver takes it only for a game asking (display_ps5.c)
p["conceptId"]=i[4:]
p["contentId"]="UP9000-"+i+"_00-"+os.environ["CONTENT"]
p["localizedParameters"]["en-US"]["titleName"]=os.environ["NAME"]
json.dump(p,open(sys.argv[2],"w"),indent=2)
P
cp "$B/ps5/sce_sys/icon0.png" "$B/ps5/sce_sys/pic0.dds" "$B/ps5/sce_sys/pic1.dds" "$app/sce_sys/"
# frame rate mods, the frame generation network and the licences go with the app
cp -r "$B/mods" "$B/licenses" "$app/"
mkdir -p "$app/framegen"; cp "$B/ps5/framegen/network/network.fgw" "$app/framegen/"
cp "$vk/runtime/libc.prx" "$app/sce_module/libc.prx"
"$tool" self --inspect --file "$app/eboot.bin" > /dev/null || exit 1
# every import's GOT slot, for the start-up report of ones the console leaves empty
llvm-readelf-18 -r --wide "$out/llvm-pie.elf" | awk '$3 == "R_X86_64_GLOB_DAT" || $3 == "R_X86_64_JUMP_SLOT" { print $1, $5 }' > "$app/imports.txt"
wc -l "$app/imports.txt"
ls -la "$app/eboot.bin"
echo BUILD_OK
