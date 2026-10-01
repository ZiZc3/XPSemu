#!/usr/bin/env bash
# Configure xemu for the PS5 in ./build-ps5: Vulkan renderer only, static libraries
# from ps5/deps/prefix (run ps5/deps/build-deps.sh first). No --static: the PS5
# toolchain rejects -static, and the dependencies are static libraries anyway.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
src=$(dirname "$here")
sdk=${PS5_PAYLOAD_SDK:-$HOME/ps5/PS5_Vulkan/.deps/native/ps5-payload-sdk}
prefix=$here/deps/prefix

mkdir -p "$src/build-ps5"
cd "$src/build-ps5"
PKG_CONFIG="$here/pkg-config" "$src/configure" \
    --cross-prefix="$sdk/bin/prospero-" \
    --cc="$sdk/bin/prospero-clang" --cxx="$sdk/bin/prospero-clang++" \
    --extra-cflags="-DXBOX=1 -march=znver2 -I$prefix/include" \
    --extra-cxxflags="-march=znver2 -I$prefix/include" \
    --extra-ldflags="-L$prefix/lib -lps5compat" \
    --target-list=i386-softmmu \
    --disable-werror \
    --enable-slirp \
    --disable-opengl --disable-gtk --disable-vnc --disable-curses \
    --disable-user --disable-bsd-user --disable-tools --disable-docs \
    --disable-guest-agent --disable-plugins --disable-install-blobs \
    -Dx86_version=3 -Db_lto=true -Db_lto_mode=thin \
    "$@"
