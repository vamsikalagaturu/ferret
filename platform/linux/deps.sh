#!/usr/bin/env bash
# Fetches the sources pinned in platform/versions.sh into thirdparty/ and builds raylib (GLFW with Wayland and
# X11), and libmtp when the system has none, into $FR_LINUX_OUT (default ~/.cache/ferret/linux). build.sh sources
# this file first.
# Separate worktrees build side by side with their own FR_LINUX_OUT.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
. "$REPO/platform/versions.sh"
ROOT=${FR_LINUX_OUT:-$HOME/.cache/ferret/linux}
JOBS=$(nproc)

for tool in cc cmake curl git; do
    command -v $tool >/dev/null || { echo "missing: $tool" >&2; exit 1; }
done

step() { printf '\n== %s\n' "$*"; }
mkdir -p "$ROOT"

step "raylib $RAYLIB_TAG (Wayland and X11), nanosvg"
fetch_sources "$REPO"
RAYLIB_OUT=$ROOT/raylib-install-$BUILD_TAG
if [ ! -f "$RAYLIB_OUT/lib/libraylib.a" ]; then
    cmake -S "$REPO/thirdparty/raylib" -B "$ROOT/raylib-build-$BUILD_TAG" -DCMAKE_BUILD_TYPE=Release -DBUILD_EXAMPLES=OFF \
        -DCMAKE_C_FLAGS_RELEASE="$SIZE_CFLAGS -DNDEBUG" -DGLFW_BUILD_WAYLAND=ON -DGLFW_BUILD_X11=ON \
        -DCMAKE_INSTALL_PREFIX="$RAYLIB_OUT" -DCMAKE_INSTALL_LIBDIR=lib >/dev/null
    cmake --build "$ROOT/raylib-build-$BUILD_TAG" --target install -j"$JOBS" >/dev/null
fi

if pkg-config --exists libmtp; then
    step "libmtp $(pkg-config --modversion libmtp) (the system's)"
else
    step "libmtp $LIBMTP_VERSION (static, over the system's libusb-1.0)"
    pkg-config --exists libusb-1.0 || { echo "missing: libusb-1.0 development files" >&2; exit 1; }
    MTP_OUT=$ROOT/libmtp-$LIBMTP_VERSION-$BUILD_TAG
    if [ ! -f "$MTP_OUT/lib/libmtp.a" ]; then
        fetch "$LIBMTP_URL" "$REPO/thirdparty/libmtp-$LIBMTP_VERSION.tar.gz" "$LIBMTP_SHA256"
        rm -rf "$ROOT/libmtp-src"
        mkdir -p "$ROOT/libmtp-src"
        tar -xzf "$REPO/thirdparty/libmtp-$LIBMTP_VERSION.tar.gz" -C "$ROOT/libmtp-src" --strip-components=1
        # mtpz needs libgcrypt and is only for a few old Zune players; the udev rules stay inside the prefix
        (cd "$ROOT/libmtp-src" && ./configure --prefix="$MTP_OUT" --libdir="$MTP_OUT/lib" --disable-shared --enable-static \
            --with-pic --disable-mtpz --disable-doxygen --with-udev="$MTP_OUT/udev" CFLAGS="$SIZE_CFLAGS -DNDEBUG" >/dev/null &&
            make -C src -j"$JOBS" install >/dev/null && make install-pkgconfigDATA >/dev/null)
    fi
    export PKG_CONFIG_PATH=$MTP_OUT/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}
fi
