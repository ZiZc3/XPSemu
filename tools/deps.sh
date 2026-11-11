#!/usr/bin/env bash
# hakuX's third-party sources at the revisions android/README.md names, in
# ~/ps5/xog/deps (NC_DEPS). Prints that table first.
set -u
core=$HOME/ps5/xog/core
deps=$HOME/ps5/xog/deps
mkdir -p "$deps"
sed -n 27,42p "$core/android/README.md" | cut -c1-110
get() {  # name url tag
  [ -e "$deps/$1/.done" ] && { echo "have $1"; return; }
  rm -rf "${deps:?}/$1"
  if git clone -q --depth 1 --branch "$3" "$2" "$deps/$1" 2>/dev/null; then
    touch "$deps/$1/.done"
    echo "got  $1 $3"
  else
    echo "FAIL $1 $3"
  fi
}
get volk https://github.com/zeux/volk.git 1.4.304
get glslang https://github.com/KhronosGroup/glslang.git 16.1.0
get vma https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git v3.3.0
get libslirp https://gitlab.freedesktop.org/slirp/libslirp.git v4.7.0
get spirv_reflect https://github.com/KhronosGroup/SPIRV-Reflect.git main
get libchdr https://github.com/rtissera/libchdr.git master    # CHD images (BSD-3-Clause)
[ -e "$deps/root/subprojects/nv2a_vsh_cpu/.done" ] || {
  rm -rf "$deps/root/subprojects/nv2a_vsh_cpu"
  mkdir -p "$deps/root/subprojects"
  git clone -q --depth 1 https://github.com/abaire/nv2a_vsh_cpu.git "$deps/root/subprojects/nv2a_vsh_cpu" && touch "$deps/root/subprojects/nv2a_vsh_cpu/.done" && echo "got  nv2a_vsh_cpu" || echo "FAIL nv2a_vsh_cpu"
}
ls "$deps" "$deps/root/subprojects/nv2a_vsh_cpu/src" | head -40
du -sh "$deps"
