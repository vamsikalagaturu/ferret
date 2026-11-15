#!/usr/bin/env bash
# Fetches the sources pinned in platform/versions.sh into thirdparty/ and builds raylib (GLFW with Wayland and
# X11) into $FR_LINUX_OUT (default ~/.cache/ferret/linux). build.sh sources this file first.
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

step "libsmb2 $LIBSMB2_VERSION, libssh2 $LIBSSH2_VERSION (over the system's libcrypto)"
fetch_net_sources "$REPO"
NET_OUT=$ROOT/net-smb2-$LIBSMB2_VERSION-ssh2-$LIBSSH2_VERSION-$BUILD_TAG
if [ ! -f "$NET_OUT/smb2/lib/libsmb2.a" ] || [ ! -f "$NET_OUT/ssh2/lib/libssh2.a" ]; then
    command -v ninja >/dev/null || { echo "missing: ninja" >&2; exit 1; }
    pkg-config --exists libcrypto || { echo "missing: OpenSSL's libcrypto (libssl-dev)" >&2; exit 1; }
    build_net "$NET_OUT"
fi
