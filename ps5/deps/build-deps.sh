#!/usr/bin/env bash
# Cross-compile xemu's libraries for the PS5 into ps5/deps/prefix.
# Needs PS5_PAYLOAD_SDK (default: PS5_Vulkan's pinned SDK fork).
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
sdk=${PS5_PAYLOAD_SDK:-$HOME/ps5/PS5_Vulkan/.deps/native/ps5-payload-sdk}
export PS5_PAYLOAD_SDK=$sdk
prefix=$here/prefix
src=$here/src
mkdir -p "$prefix" "$src"
printf "[constants]\nsdk = '%s'\nps5 = '%s'\n" "$sdk" "$(dirname "$here")" > "$here/sdk.ini"
cross=(--cross-file "$here/sdk.ini" --cross-file "$here/../ps5-cross.ini")
# FreeBSD targets (meson, SDL) install .pc files to libdata/pkgconfig.
export PKG_CONFIG_LIBDIR=$prefix/lib/pkgconfig:$prefix/libdata/pkgconfig

fetch() { # url dir
    [[ -d $src/$2 ]] && return
    curl -sL "$1" | tar -xJ -C "$src"
}

build_compat() {
    # Stubs for libc pieces the PS5 lacks: libresolv for glib's res_query() check,
    # libps5compat for everything else (ps5/compat/).
    mkdir -p "$src/build-compat" "$prefix/lib"
    local cc="$sdk/bin/prospero-clang" ar="$sdk/bin/prospero-ar" o="$src/build-compat"
    "$cc" -O2 -c "$here/../compat/resolv_stub.c" -o "$o/resolv_stub.o"
    "$ar" rcs "$prefix/lib/libresolv.a" "$o/resolv_stub.o"
    "$cc" -O2 -c "$here/../compat/libc_stubs.c" -o "$o/libc_stubs.o"
    "$cc" -O2 -c "$here/../compat/mmap.c" -o "$o/mmap.o"
    "$cc" -O2 -c "$here/../compat/ucontext.c" -o "$o/ucontext.o"
    "$cc" -O2 -c "$here/../compat/libc_missing.c" -o "$o/libc_missing.o"
    rm -f "$prefix/lib/libps5compat.a"
    "$ar" rcs "$prefix/lib/libps5compat.a" "$o/libc_stubs.o" "$o/mmap.o" "$o/ucontext.o" \
        "$o/libc_missing.o"
}

build_zlib() {
    local v=1.3.1
    [[ -f $prefix/lib/libz.a ]] && return
    [[ -d $src/zlib-$v ]] || curl -sL "https://zlib.net/fossils/zlib-$v.tar.gz" | tar -xz -C "$src"
    (cd "$src/zlib-$v" && CC="$sdk/bin/prospero-clang" AR="$sdk/bin/prospero-ar" CHOST=x86_64-pc-freebsd \
        ./configure --static --prefix="$prefix" && make -j"$(nproc)" libz.a && make install)
}

build_libiconv() {
    local v=1.18
    [[ -f $prefix/lib/libiconv.a ]] && return
    [[ -d $src/libiconv-$v ]] || curl -sL "https://ftp.gnu.org/pub/gnu/libiconv/libiconv-$v.tar.gz" | tar -xz -C "$src"
    mkdir -p "$src/build-libiconv" && cd "$src/build-libiconv"
    CC="$sdk/bin/prospero-clang" AR="$sdk/bin/prospero-ar" RANLIB="$sdk/bin/prospero-ranlib" \
        "$src/libiconv-$v/configure" --host=x86_64-pc-freebsd --prefix="$prefix" \
        --enable-static --disable-shared --disable-nls
    make -j"$(nproc)" && make install
    cd "$here"
}

build_glib() {
    build_zlib
    build_compat
    build_libiconv
    local v=2.84.4
    fetch "https://download.gnome.org/sources/glib/2.84/glib-$v.tar.xz" "glib-$v"
    # libffi's x86-64 code is the same on FreeBSD; its table just doesn't list it.
    sed -i "s/\['darwin', 'ios', 'linux', 'android'\].contains(host_system)/['darwin', 'ios', 'linux', 'android', 'freebsd'].contains(host_system)/" \
        "$src/glib-$v/subprojects/libffi/meson.build"
    # The PS5 target rejects ms_abi. It only marks libffi's Windows-ABI (FFI_EFI64) closure
    # path, which glib never uses (it calls with FFI_DEFAULT_ABI, the Unix one).
    sed -i 's/__attribute__((ms_abi))//' "$src/glib-$v/subprojects/libffi/src/x86/ffiw64.c"
    # FreeBSD's <malloc.h> is an #error stub; glib uses posix_memalign here, so skip that include.
    sed -i 's/^#if defined(HAVE_MEMALIGN) || defined(HAVE__ALIGNED_MALLOC)$/#if defined(HAVE__ALIGNED_MALLOC)/' "$src/glib-$v/glib/gmem.c"
    meson setup --wipe "$src/build-glib" "$src/glib-$v" "${cross[@]}" --prefix "$prefix" \
        -Dbuildtype=release -Dtests=false -Dintrospection=disabled -Dnls=disabled \
        -Dselinux=disabled -Dxattr=false -Dlibmount=disabled -Dman-pages=disabled \
        -Dsysprof=disabled -Ddtrace=disabled -Dsystemtap=disabled -Dlibelf=disabled \
        -Dglib_debug=disabled \
        -Dc_args="-I$prefix/include" -Dc_link_args="-L$prefix/lib -lps5compat" -Dcpp_args="-I$prefix/include" -Dcpp_link_args="-L$prefix/lib -lps5compat" \
        -Dglib_assert=false -Dglib_checks=false \
        --force-fallback-for=pcre2,libffi,proxy-libintl
    ninja -C "$src/build-glib" install
}

build_pixman() {
    local v=0.44.2
    [[ -f $prefix/lib/libpixman-1.a ]] && return
    fetch "https://cairographics.org/releases/pixman-$v.tar.xz" "pixman-$v"
    meson setup --wipe "$src/build-pixman" "$src/pixman-$v" "${cross[@]}" --prefix "$prefix" \
        -Dbuildtype=release -Dtests=disabled -Ddemos=disabled -Dgtk=disabled -Dlibpng=disabled \
        -Dopenmp=disabled -Dtimers=false
    ninja -C "$src/build-pixman" install
}

build_libsamplerate() {
    local v=0.2.2
    [[ -f $prefix/lib/libsamplerate.a ]] && return
    fetch "https://github.com/libsndfile/libsamplerate/releases/download/$v/libsamplerate-$v.tar.xz" "libsamplerate-$v"
    mkdir -p "$src/build-libsamplerate" && cd "$src/build-libsamplerate"
    CC="$sdk/bin/prospero-clang" AR="$sdk/bin/prospero-ar" RANLIB="$sdk/bin/prospero-ranlib" \
        "$src/libsamplerate-$v/configure" --host=x86_64-pc-freebsd --prefix="$prefix" \
        --enable-static --disable-shared --disable-sndfile --disable-alsa --disable-fftw
    make -j"$(nproc)" src/libsamplerate.la && make install-libLTLIBRARIES install-includeHEADERS install-pkgconfigDATA
    cd "$here"
}

build_sdl3() {
    # Dummy video/audio/input drivers only; PS5 drivers (ScePad, SceAudioOut) come later.
    local v=3.2.24
    [[ -f $prefix/lib/libSDL3.a ]] && return
    [[ -d $src/SDL3-$v ]] || curl -sL "https://github.com/libsdl-org/SDL/releases/download/release-$v/SDL3-$v.tar.gz" | tar -xz -C "$src"
    cmake -S "$src/SDL3-$v" -B "$src/build-sdl3" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$sdk/toolchain/prospero.cmake" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$prefix" -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF \
        -DSDL_EXAMPLES=OFF -DSDL_UNIX_CONSOLE_BUILD=ON -DSDL_X11=OFF -DSDL_WAYLAND=OFF \
        -DSDL_KMSDRM=OFF -DSDL_OFFSCREEN=OFF -DSDL_HIDAPI=OFF -DSDL_LIBUDEV=OFF -DSDL_DBUS=OFF \
        -DSDL_IBUS=OFF -DSDL_ALSA=OFF -DSDL_PULSEAUDIO=OFF -DSDL_PIPEWIRE=OFF -DSDL_JACK=OFF \
        -DSDL_SNDIO=OFF -DSDL_OSS=OFF -DSDL_LIBUSB=OFF -DSDL_CAMERA=OFF -DSDL_SENSOR=OFF \
        -DSDL_DIALOG=OFF -DSDL_GPU=OFF -DSDL_RENDER_GPU=OFF -DSDL_DISABLE_INSTALL_DOCS=ON
    cmake --build "$src/build-sdl3" && cmake --install "$src/build-sdl3"
}

build_vulkan() {
    # Headers and static RADV from PS5_Vulkan's release build, exposed as vulkan.pc.
    # No loader exports vk* on the PS5: everything calls through volk or loaded
    # function pointers, hence VK_NO_PROTOTYPES.
    local radv=${PS5_RADV:-$HOME/ps5/PS5_Vulkan/.deps/native/radv-release}
    [[ -f $radv/lib/libvulkan_radeon.ps5.a ]] || { echo "RADV release not found at $radv" >&2; return 1; }
    mkdir -p "$prefix/lib/pkgconfig"
    cat > "$prefix/lib/pkgconfig/vulkan.pc" <<PC
includedir=$radv/include

Name: Vulkan-Loader
Description: RADV for the PS5 (static, no loader)
Version: 1.4.0
Cflags: -I\${includedir} -DVK_NO_PROTOTYPES
Libs: $radv/lib/libvulkan_radeon.ps5.a
PC
}

build_libpcap() {
    # No packet capture on the PS5: the null backend keeps xemu's pcap code linking.
    local v=1.10.5
    [[ -f $prefix/lib/libpcap.a ]] && return
    [[ -d $src/libpcap-$v ]] || curl -sL "https://www.tcpdump.org/release/libpcap-$v.tar.xz" | tar -xJ -C "$src"
    mkdir -p "$src/build-libpcap" && cd "$src/build-libpcap"
    CC="$sdk/bin/prospero-clang" AR="$sdk/bin/prospero-ar" RANLIB="$sdk/bin/prospero-ranlib" \
        "$src/libpcap-$v/configure" --host=x86_64-pc-freebsd --prefix="$prefix" \
        --disable-shared --with-pcap=null --disable-usb --disable-netmap --disable-bluetooth \
        --disable-dbus --disable-rdma --without-dag --without-septel --without-snf --without-turbocap \
        --without-libnl --disable-remote
    make -j"$(nproc)" && make install
    cd "$here"
}

build_mbedtls() {
    # TLS for curl (XPSemu's cover downloads are HTTPS only).
    local v=3.6.4
    [[ -f $prefix/lib/libmbedtls.a ]] && return
    [[ -d $src/mbedtls-$v ]] ||
        curl -sL "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$v/mbedtls-$v.tar.bz2" |
        tar -xj -C "$src"
    make -C "$src/mbedtls-$v/library" -j"$(nproc)" CC="$sdk/bin/prospero-clang" \
        AR="$sdk/bin/prospero-ar" CFLAGS="-O2" static
    mkdir -p "$prefix/include" "$prefix/lib"
    cp -r "$src/mbedtls-$v/include/mbedtls" "$src/mbedtls-$v/include/psa" "$prefix/include/"
    cp "$src/mbedtls-$v/library/"lib{mbedtls,mbedx509,mbedcrypto}.a "$prefix/lib/"
}

build_curl() {
    # HTTP and HTTPS (mbedTLS: build_mbedtls first); certificates are given
    # by the caller (CURLOPT_CAINFO_BLOB), there's no system store.
    local v=8.16.0
    [[ -f $prefix/lib/libcurl.a ]] && return
    fetch "https://curl.se/download/curl-$v.tar.xz" "curl-$v"
    mkdir -p "$src/build-curl" && cd "$src/build-curl"
    CC="$sdk/bin/prospero-clang" AR="$sdk/bin/prospero-ar" RANLIB="$sdk/bin/prospero-ranlib"     CPPFLAGS="-I$prefix/include" LDFLAGS="-L$prefix/lib" LIBS="-lps5compat" \
        "$src/curl-$v/configure" --host=x86_64-pc-freebsd --prefix="$prefix" \
        --disable-shared --enable-static --with-mbedtls="$prefix" \
        --without-ca-bundle --without-ca-path --without-libpsl --without-brotli \
        --without-zstd --without-nghttp2 --without-libidn2 --without-librtmp --disable-ldap \
        --disable-docs --disable-manual --disable-threaded-resolver --with-zlib="$prefix"
    make -j"$(nproc)" -C lib && make -C lib install && make -C include install && make install-pkgconfigDATA
    cd "$here"
}

build_libslirp() {
    # xemu's NAT networking (its default network mode).
    local v=4.9.1
    [[ -f $prefix/lib/libslirp.a ]] && return
    [[ -d $src/libslirp-v$v ]] || curl -sL "https://gitlab.freedesktop.org/slirp/libslirp/-/archive/v$v/libslirp-v$v.tar.gz" | tar -xz -C "$src"
    meson setup --wipe "$src/build-libslirp" "$src/libslirp-v$v" "${cross[@]}" --prefix "$prefix" \
        -Dbuildtype=release -Doss-fuzz=false \
        -Dc_args="-I$prefix/include" -Dc_link_args="-L$prefix/lib -lps5compat"
    ninja -C "$src/build-libslirp" install
}

for dep in "${@:-glib}"; do "build_$dep"; done
