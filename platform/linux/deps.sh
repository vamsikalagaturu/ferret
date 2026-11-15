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

step "libsmb2 $LIBSMB2_VERSION, libssh2 $LIBSSH2_VERSION (over the system's libcrypto)"
pkg-config --exists libssl || { echo "missing: OpenSSL's libssl (libssl-dev)" >&2; exit 1; }
fetch_net_sources "$REPO"
NET_OUT=$ROOT/net-smb2-$LIBSMB2_VERSION-ssh2-$LIBSSH2_VERSION-nfs-$LIBNFS_VERSION-curl-$CURL_VERSION-expat-$EXPAT_VERSION-$BUILD_TAG
if [ ! -f "$NET_OUT/smb2/lib/libsmb2.a" ] || [ ! -f "$NET_OUT/ssh2/lib/libssh2.a" ]; then
    command -v ninja >/dev/null || { echo "missing: ninja" >&2; exit 1; }
    pkg-config --exists libcrypto || { echo "missing: OpenSSL's libcrypto (libssl-dev)" >&2; exit 1; }
    build_net "$NET_OUT"
fi

# FR_NET_PREFIX gets curl/, expat/ and nfs/ only for what the system lacks; CMakeLists.txt looks there first
if pkg-config --exists libnfs; then
    step "libnfs $(pkg-config --modversion libnfs) (the system's)"
else
    step "libnfs $LIBNFS_VERSION (static)"
    [ -f "$NET_OUT/nfs/lib/libnfs.a" ] || build_nfs "$NET_OUT"
fi
if pkg-config --exists expat; then
    step "expat $(pkg-config --modversion expat) (the system's)"
else
    step "expat $EXPAT_VERSION (static)"
    [ -f "$NET_OUT/expat/lib/libexpat.a" ] || build_expat "$NET_OUT"
fi
if pkg-config --exists libcurl; then
    step "libcurl $(pkg-config --modversion libcurl) (the system's)"
else
    step "libcurl $CURL_VERSION (static, over the system's OpenSSL)"
    [ -f "$NET_OUT/curl/lib/libcurl.a" ] || build_curl "$NET_OUT"
fi

step "libarchive $LIBARCHIVE_VERSION (static; the system's zlib, bzip2, xz and zstd)"
for h in zlib.h bzlib.h lzma.h zstd.h; do
    echo "#include <$h>" | cc -E -x c - >/dev/null 2>&1 || { echo "missing: $h (the zlib, bzip2, xz, zstd headers)" >&2; exit 1; }
done
fetch_tarball "$REPO" "$LIBARCHIVE_URL" "$LIBARCHIVE_SHA256" "libarchive-$LIBARCHIVE_VERSION"
LIBARCHIVE_OUT=$ROOT/libarchive-$LIBARCHIVE_VERSION-$BUILD_TAG
if [ ! -f "$LIBARCHIVE_OUT/lib/libarchive.a" ]; then
    cmake -S "$REPO/thirdparty/libarchive-$LIBARCHIVE_VERSION" -B "$ROOT/libarchive-build-$BUILD_TAG" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS_RELEASE="$SIZE_CFLAGS -DNDEBUG" -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DCMAKE_INSTALL_PREFIX="$LIBARCHIVE_OUT" -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_SHARED_LIBS=OFF \
        -DENABLE_TAR=OFF -DENABLE_CPIO=OFF -DENABLE_CAT=OFF -DENABLE_UNZIP=OFF -DENABLE_TEST=OFF \
        -DENABLE_OPENSSL=OFF -DENABLE_MBEDTLS=OFF -DENABLE_NETTLE=OFF -DENABLE_MD=OFF -DENABLE_CNG=OFF \
        -DENABLE_LIBB2=OFF -DENABLE_LZ4=OFF -DENABLE_LZO=OFF -DENABLE_LIBXML2=OFF -DENABLE_EXPAT=OFF \
        -DENABLE_BSDXML=OFF -DENABLE_PCREPOSIX=OFF -DENABLE_PCRE2POSIX=OFF -DENABLE_LIBGCC=OFF \
        -DENABLE_ACL=OFF -DENABLE_XATTR=OFF -DENABLE_ICONV=OFF -DENABLE_WERROR=OFF >/dev/null
    cmake --build "$ROOT/libarchive-build-$BUILD_TAG" --target install -j"$JOBS" >/dev/null
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
