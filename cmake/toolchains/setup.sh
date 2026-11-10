#!/bin/sh
# Installs the pinned cross toolchains (pins.env) under SFCOP_TOOLS_DIR, outside the repository:
#
#   sh cmake/toolchains/setup.sh [llvm] [linux] [windows] [linux-arm64] [windows-arm64]
#                                                    (no argument: llvm, linux and windows)
#
#   $SFCOP_TOOLS_DIR/llvm/<LLVM_VERSION>/                                 clang, clang-cl, lld, ...
#   $SFCOP_TOOLS_DIR/sysroots/linux-x64/steamrt3-sniper-<version>/        glibc 2.31, GCC 10
#   $SFCOP_TOOLS_DIR/sysroots/windows-x64/msvc-<crt>-sdk-<sdk>/           MSVC CRT, Windows SDK
#   $SFCOP_TOOLS_DIR/sysroots/linux-arm64/debian12-<snapshot>/            glibc 2.36, GCC 12
#   $SFCOP_TOOLS_DIR/sysroots/windows-arm64/msvc-<crt>-sdk-<sdk>/         the same, aarch64
#
# The two arm64 sysroots serve the release packages (packaging/README.md); PR CI does not use them.
#
# Idempotent: an install whose stamp names the pinned hash is left alone. A download or an
# installed tree whose SHA-256 differs from the pin is refused and removed. Downloads go to
# $SFCOP_TOOLCHAIN_DOWNLOADS (default $SFCOP_TOOLS_DIR/.downloads) and are deleted once installed
# (SFCOP_TOOLCHAIN_KEEP=1 keeps the Linux archive, for changing what is extracted from it).
# Needs curl, tar, shasum, python3 and, for windows, Docker. CI never runs this (README.md).
set -eu

here=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=pins.env
. "$here/pins.env"
tools=${SFCOP_TOOLS_DIR:-}
[ -n "$tools" ] || { echo "setup: error: set SFCOP_TOOLS_DIR to the folder to install the toolchains into" >&2; exit 2; }
downloads=${SFCOP_TOOLCHAIN_DOWNLOADS:-$tools/.downloads}

say() { printf 'setup: %s\n' "$*"; }
die() { printf 'setup: error: %s\n' "$*" >&2; exit 1; }
sha256() { shasum -a 256 "$1" | cut -c1-64; }

# fetch <url> <file> <sha256> <size>: download into $downloads unless a verified copy is there.
fetch() {
    _url=$1 _file=$downloads/$2 _sha=$3 _size=$4
    mkdir -p "$downloads"
    if [ -f "$_file" ] && [ "$(sha256 "$_file")" = "$_sha" ]; then
        say "have $2"
        return 0
    fi
    say "downloading $2 ($_size bytes) from $_url"
    if ! nice -n 19 curl -fsSL --retry 3 --proto '=https' -o "$_file.part" "$_url"; then
        rm -f "$_file.part"
        die "$2: the download from $_url failed; the partial file is removed"
    fi
    _got=$(sha256 "$_file.part")
    if [ "$_got" != "$_sha" ]; then
        rm -f "$_file.part"
        die "$2: SHA-256 $_got, pinned $_sha; refused and removed"
    fi
    mv "$_file.part" "$_file"
}

# installed <dir> <pin>: true when <dir> carries the stamp of <pin>.
# An UNSET pin is never installed: the tree is extracted again and its hash printed.
installed() { [ "$2" != UNSET ] && [ -f "$1/.sfcop-pin" ] && [ "$(cat "$1/.sfcop-pin")" = "$2" ]; }

# check_tree <dir> <pin> <name>: refuse an installed tree whose content hash is not the pin.
# A pin of UNSET prints the hash to pin (first install of a new version) and keeps the tree.
check_tree() {
    _hash=$(nice -n 19 python3 -I "$here/sysroot.py" tree-hash "$1")
    if [ "$2" = UNSET ]; then
        say "$3: tree SHA-256 $_hash (not pinned yet: write it to pins.env)"
    elif [ "$_hash" != "$2" ]; then
        rm -rf "$1"
        die "$3: installed tree SHA-256 $_hash, pinned $2; refused and removed"
    fi
}

install_llvm() {
    dest=$tools/llvm/$LLVM_VERSION
    if installed "$dest" "$LLVM_SHA256"; then say "llvm $LLVM_VERSION: installed"; return 0; fi
    file=LLVM-$LLVM_VERSION-macOS-ARM64.tar.xz
    fetch "$LLVM_URL" "$file" "$LLVM_SHA256" "$LLVM_SIZE"
    top=LLVM-$LLVM_VERSION-macOS-ARM64
    major=${LLVM_VERSION%%.*}
    rm -rf "$dest.tmp"
    mkdir -p "$dest.tmp"
    # The binaries are self-contained (system libraries only); the driver symlinks point at them.
    say "llvm $LLVM_VERSION: extracting the cross tools"
    nice -n 19 tar -xJf "$downloads/$file" -C "$dest.tmp" --strip-components 1 \
        "$top/bin/clang-$major" "$top/bin/clang" "$top/bin/clang++" "$top/bin/clang-cl" \
        "$top/bin/lld" "$top/bin/ld.lld" "$top/bin/lld-link" \
        "$top/bin/llvm-ar" "$top/bin/llvm-ranlib" "$top/bin/llvm-lib" "$top/bin/llvm-dlltool" \
        "$top/bin/llvm-rc" "$top/bin/llvm-nm" "$top/bin/llvm-objcopy" \
        "$top/bin/llvm-strip" "$top/bin/llvm-readobj" "$top/bin/llvm-objdump" \
        "$top/lib/clang/$major/include"
    "$dest.tmp/bin/clang" --version | head -n 1
    rm -rf "$dest"
    mv "$dest.tmp" "$dest"
    printf '%s\n' "$LLVM_SHA256" > "$dest/.sfcop-pin"
    rm -f "$downloads/$file"
    say "llvm $LLVM_VERSION: installed in $dest"
}

install_linux() {
    dest=$tools/sysroots/linux-x64/steamrt3-sniper-$LINUX_SYSROOT_VERSION
    if installed "$dest" "$LINUX_SYSROOT_TREE_SHA256"; then say "linux sysroot: installed"; return 0; fi
    file=steamrt3-sniper-sdk-sysroot-$LINUX_SYSROOT_VERSION.tar.gz
    fetch "$LINUX_SYSROOT_URL" "$file" "$LINUX_SYSROOT_SHA256" "$LINUX_SYSROOT_SIZE"
    rm -rf "$dest.tmp"
    mkdir -p "$dest.tmp"
    # x86_64 only, and only what compiling and linking against glibc and libstdc++ needs, plus the
    # headers and link libraries of what SDL3 probes for (X11, Wayland, EGL, ALSA, PulseAudio,
    # PipeWire, JACK, udev, D-Bus, ...): it finds them at configure time and loads them at run
    # time. No compilers, no i386, no debug or Python trees, and not FLTK's headers, whose Fl/ and FL/ folders collide on a case-insensitive volume.
    say "linux sysroot: extracting the x86_64 headers and link libraries"
    nice -n 19 tar -xzf "$downloads/$file" -C "$dest.tmp" \
        --exclude './usr/include/llvm*' --exclude './usr/include/c++/*/*/32' \
        --exclude './usr/include/Fl' --exclude './usr/include/FL' \
        --exclude './usr/lib/gcc/x86_64-linux-gnu/*/cc1*' --exclude './usr/lib/gcc/x86_64-linux-gnu/*/lto*' \
        --exclude './usr/lib/gcc/x86_64-linux-gnu/*/32' --exclude './usr/lib/gcc/x86_64-linux-gnu/*/x32' \
        --exclude './usr/lib/gcc/x86_64-linux-gnu/*/plugin' \
        './usr/include' './usr/lib/gcc/x86_64-linux-gnu' './lib64' \
        './lib/x86_64-linux-gnu/ld-*' './lib/x86_64-linux-gnu/libc[.-]*' './lib/x86_64-linux-gnu/libm[.-]*' \
        './lib/x86_64-linux-gnu/libmvec*' './lib/x86_64-linux-gnu/libpthread*' \
        './lib/x86_64-linux-gnu/libdl*' './lib/x86_64-linux-gnu/librt*' \
        './lib/x86_64-linux-gnu/libgcc_s*' \
        './usr/lib/x86_64-linux-gnu/*crt*.o' './usr/lib/x86_64-linux-gnu/libc.so' \
        './usr/lib/x86_64-linux-gnu/libc_nonshared.a' './usr/lib/x86_64-linux-gnu/libm.so' \
        './usr/lib/x86_64-linux-gnu/libm-*.a' './usr/lib/x86_64-linux-gnu/libmvec.so' \
        './usr/lib/x86_64-linux-gnu/libmvec.a' './usr/lib/x86_64-linux-gnu/libpthread.so' \
        './usr/lib/x86_64-linux-gnu/libdl.so' \
        './usr/lib/x86_64-linux-gnu/librt.so' './usr/lib/x86_64-linux-gnu/libstdc++.so*' \
        './usr/lib/x86_64-linux-gnu/pkgconfig' './usr/share/pkgconfig' \
        './usr/lib/x86_64-linux-gnu/dbus-1.0/include' './usr/lib/x86_64-linux-gnu/glib-2.0/include' \
        './usr/lib/x86_64-linux-gnu/libglib-2.0*' './usr/lib/x86_64-linux-gnu/libgobject-2.0*' \
        './usr/lib/x86_64-linux-gnu/libgio-2.0*' './usr/lib/x86_64-linux-gnu/libfribidi*' \
        './usr/lib/x86_64-linux-gnu/libthai*' './usr/lib/x86_64-linux-gnu/libdatrie*' \
        './lib/x86_64-linux-gnu/libdbus-1*' \
        './usr/lib/x86_64-linux-gnu/libX11*' './usr/lib/x86_64-linux-gnu/libXext*' \
        './usr/lib/x86_64-linux-gnu/libXcursor*' './usr/lib/x86_64-linux-gnu/libXi.*' \
        './usr/lib/x86_64-linux-gnu/libXfixes*' './usr/lib/x86_64-linux-gnu/libXrandr*' \
        './usr/lib/x86_64-linux-gnu/libXss*' './usr/lib/x86_64-linux-gnu/libXrender*' \
        './usr/lib/x86_64-linux-gnu/libXtst*' './usr/lib/x86_64-linux-gnu/libXinerama*' \
        './usr/lib/x86_64-linux-gnu/libxcb*' './usr/lib/x86_64-linux-gnu/libXau*' \
        './usr/lib/x86_64-linux-gnu/libXdmcp*' './usr/lib/x86_64-linux-gnu/libwayland-*' \
        './usr/lib/x86_64-linux-gnu/libxkbcommon*' './usr/lib/x86_64-linux-gnu/libEGL*' \
        './usr/lib/x86_64-linux-gnu/libGL*' './usr/lib/x86_64-linux-gnu/libOpenGL*' \
        './usr/lib/x86_64-linux-gnu/libdecor-0*' './usr/lib/x86_64-linux-gnu/libasound*' \
        './usr/lib/x86_64-linux-gnu/libpulse*' './usr/lib/x86_64-linux-gnu/libpipewire-0.3*' \
        './usr/lib/x86_64-linux-gnu/libjack*' './usr/lib/x86_64-linux-gnu/libdrm*' \
        './usr/lib/x86_64-linux-gnu/libgbm*' './usr/lib/x86_64-linux-gnu/libudev*' \
        './usr/lib/x86_64-linux-gnu/libdbus-1*' './usr/lib/x86_64-linux-gnu/libibus-1.0*' \
        './usr/lib/x86_64-linux-gnu/libusb-1.0*' './usr/lib/x86_64-linux-gnu/libvulkan*'
    python3 -I "$here/sysroot.py" relink "$dest.tmp"
    check_tree "$dest.tmp" "$LINUX_SYSROOT_TREE_SHA256" "linux sysroot"
    rm -rf "$dest"
    mv "$dest.tmp" "$dest"
    printf '%s\n' "$LINUX_SYSROOT_TREE_SHA256" > "$dest/.sfcop-pin"
    [ -n "${SFCOP_TOOLCHAIN_KEEP:-}" ] || rm -f "$downloads/$file"
    say "linux sysroot: installed in $dest"
}

install_windows() {
    install_windows_arch x86_64 windows-x64 "$WINDOWS_SYSROOT_TREE_SHA256" "windows sysroot"
}

install_windows_arm64() {
    install_windows_arch aarch64 windows-arm64 "$WINDOWS_ARM64_SYSROOT_TREE_SHA256" "windows arm64 sysroot"
}

# install_windows_arch <xwin arch> <sysroot folder> <tree pin> <name>
install_windows_arch() {
    arch=$1 dest=$tools/sysroots/$2/msvc-$WINDOWS_CRT_VERSION-sdk-$WINDOWS_SDK_VERSION pin=$3 name=$4
    if installed "$dest" "$pin"; then say "$name: installed"; return 0; fi
    command -v docker >/dev/null 2>&1 || die "$name: docker is needed to run xwin"
    file=xwin-$XWIN_VERSION-aarch64-unknown-linux-musl.tar.gz
    fetch "$XWIN_URL" "$file" "$XWIN_SHA256" "$XWIN_SIZE"
    work=$downloads/xwin-work
    # The download cache is kept until the install succeeds, so a failed run resumes.
    rm -rf "$work/bin" "$work/out" "$dest.tmp"
    mkdir -p "$work/bin" "$work/cache"
    tar -xzf "$downloads/$file" -C "$work/bin" --strip-components 1
    # The top-level manifest names exactly one installer manifest, the pinned one; xwin checks
    # the SHA-256 of it and of every package it downloads from it.
    cat > "$work/channel.json" <<EOF
{"manifestVersion":"1.1","info":{"id":"VisualStudio.17.Release/pinned","manifestName":"VisualStudio.17.Release","manifestType":"channel"},
 "channelItems":[{"id":"Microsoft.VisualStudio.Manifests.VisualStudio","version":"$WINDOWS_VSMAN_VERSION","type":"Manifest",
  "payloads":[{"fileName":"VisualStudio.vsman","sha256":"$WINDOWS_VSMAN_SHA256","size":$WINDOWS_VSMAN_SIZE,"url":"$WINDOWS_VSMAN_URL"}]}]}
EOF
    # The licence acceptance is the owner's (README.md, "Licence"). The splat keeps no case
    # symlinks: the tools volume is case-insensitive APFS, where they would collide.
    say "$name: MSVC CRT $WINDOWS_CRT_VERSION and Windows SDK $WINDOWS_SDK_VERSION ($arch) via xwin $XWIN_VERSION"
    nice -n 19 docker run --rm \
        -v "$work:/work" "$XWIN_IMAGE" \
        /work/bin/xwin --accept-license --cache-dir /work/cache --manifest /work/channel.json \
            --arch "$arch" --variant desktop \
            --crt-version "$WINDOWS_CRT_VERSION" --sdk-version "$WINDOWS_SDK_VERSION" \
            splat --output /work/out --disable-symlinks
    # One mount for the cache and the output: xwin moves files from one to the other.
    mkdir -p "$(dirname "$dest")"
    mv "$work/out" "$dest.tmp"
    check_tree "$dest.tmp" "$pin" "$name"
    rm -rf "$dest"
    mv "$dest.tmp" "$dest"
    printf '%s\n' "$pin" > "$dest/.sfcop-pin"
    rm -rf "$work" "$downloads/$file"
    say "$name: installed in $dest"
}

# The Linux arm64 sysroot: Debian 12 (bookworm) arm64, because the Steam Runtime 3 has no arm64
# build. The pinned image (native arm64 on the Mac) installs the -dev packages of glibc, libstdc++
# and of what SDL3 probes for from the Debian snapshot archive at the pinned time, and streams the
# headers and link libraries out, sorted by name; the tree hash pins what is kept.
install_linux_arm64() {
    dest=$tools/sysroots/linux-arm64/debian12-$LINUX_ARM64_SYSROOT_SNAPSHOT
    if installed "$dest" "$LINUX_ARM64_SYSROOT_TREE_SHA256"; then say "linux arm64 sysroot: installed"; return 0; fi
    command -v docker >/dev/null 2>&1 || die "linux arm64 sysroot: docker is needed"
    rm -rf "$dest.tmp"
    mkdir -p "$dest.tmp"
    say "linux arm64 sysroot: Debian 12 arm64 from the snapshot of $LINUX_ARM64_SYSROOT_SNAPSHOT"
    nice -n 19 docker run --rm --platform linux/arm64 -e SNAP="$LINUX_ARM64_SYSROOT_SNAPSHOT" \
        "$LINUX_ARM64_SYSROOT_IMAGE" sh -euc '
        exec 3>&1 1>&2
        rm -f /etc/apt/sources.list.d/*
        echo "deb [check-valid-until=no] http://snapshot.debian.org/archive/debian/$SNAP bookworm main" > /etc/apt/sources.list
        echo "deb [check-valid-until=no] http://snapshot.debian.org/archive/debian/$SNAP bookworm-updates main" >> /etc/apt/sources.list
        echo "deb [check-valid-until=no] http://snapshot.debian.org/archive/debian-security/$SNAP bookworm-security main" >> /etc/apt/sources.list
        apt-get -o Acquire::Retries=3 update -qq
        DEBIAN_FRONTEND=noninteractive apt-get install -qq -y --no-install-recommends \
            libc6-dev libstdc++-12-dev libgcc-12-dev \
            libx11-dev libxext-dev libxcursor-dev libxi-dev libxfixes-dev libxrandr-dev \
            libxss-dev libxrender-dev libxtst-dev libxinerama-dev libxkbcommon-dev \
            libwayland-dev libegl-dev libgl-dev libgles-dev libdrm-dev libgbm-dev \
            libasound2-dev libpulse-dev libpipewire-0.3-dev libjack-jackd2-dev libudev-dev \
            libdbus-1-dev libibus-1.0-dev libdecor-0-dev libusb-1.0-0-dev libvulkan-dev \
            libfribidi-dev libthai-dev >/dev/null
        cd /
        tar --sort=name --numeric-owner -cf - \
            --exclude usr/lib/aarch64-linux-gnu/dri --exclude "usr/lib/aarch64-linux-gnu/libLLVM*" \
            --exclude usr/lib/aarch64-linux-gnu/perl --exclude usr/lib/aarch64-linux-gnu/perl-base \
            --exclude usr/lib/aarch64-linux-gnu/gconv --exclude "usr/lib/aarch64-linux-gnu/libicudata*" \
            --exclude "usr/lib/gcc/aarch64-linux-gnu/*/cc1*" --exclude "usr/lib/gcc/aarch64-linux-gnu/*/lto*" \
            --exclude "usr/lib/gcc/aarch64-linux-gnu/*/plugin" --exclude "usr/include/Fl" --exclude "usr/include/FL" \
            lib usr/lib/ld-linux-aarch64.so.1 usr/include usr/lib/aarch64-linux-gnu \
            usr/lib/gcc/aarch64-linux-gnu usr/share/pkgconfig >&3
    ' | nice -n 19 tar -xf - -C "$dest.tmp"
    python3 -I "$here/sysroot.py" relink "$dest.tmp"
    check_tree "$dest.tmp" "$LINUX_ARM64_SYSROOT_TREE_SHA256" "linux arm64 sysroot"
    rm -rf "$dest"
    mv "$dest.tmp" "$dest"
    printf '%s\n' "$LINUX_ARM64_SYSROOT_TREE_SHA256" > "$dest/.sfcop-pin"
    say "linux arm64 sysroot: installed in $dest"
}

[ $# -gt 0 ] || set -- llvm linux windows
for part in "$@"; do
    case $part in
        llvm) install_llvm ;;
        linux) install_linux ;;
        windows) install_windows ;;
        linux-arm64) install_linux_arm64 ;;
        windows-arm64) install_windows_arm64 ;;
        *) die "unknown part: $part (llvm, linux, windows, linux-arm64, windows-arm64)" ;;
    esac
done
rmdir "$downloads" 2>/dev/null || true
