# Third-party sources every build script uses: versions, checksums and the size flags they are compiled with.
# Sourced by platform/{linux,android}/deps.sh.

FR_VERSION=$(sed -n 's/^project(ferret VERSION \([0-9.]*\).*/\1/p' "$(dirname "${BASH_SOURCE[0]}")/../CMakeLists.txt")

SIZE_CFLAGS="-O2 -ffunction-sections -fdata-sections"
# build folders are named after the settings, so a change rebuilds everything with them
BUILD_TAG=o2

RAYLIB_TAG=6.2
RAYLIB_URL=https://github.com/raysan5/raylib.git

# no releases upstream: a commit of master
NANOSVG_COMMIT=239e102ec2c691f2902e20ace2ed36ee4a35cfe6
NANOSVG_URL=https://raw.githubusercontent.com/memononen/nanosvg/$NANOSVG_COMMIT
NANOSVG_H_SHA256=e34fd5d084be106cea972d19ce5d27fd96d17ba89f8d06bdceee058420c8b2b0
NANOSVGRAST_H_SHA256=79a9c5f4db19debf9f3a648a1589e96d92854f245a5cb4f3d823f263785234d8
NANOSVG_LICENSE_SHA256=e82bc3dd03400aecabe12201219ba14750dbc4b36faab58663a7a6068548d372

# archives (src/vfs/vfs_archive.c); the codecs are built for Android only, Linux links the system's
LIBARCHIVE_VERSION=3.8.11
LIBARCHIVE_URL=https://github.com/libarchive/libarchive/releases/download/v$LIBARCHIVE_VERSION/libarchive-$LIBARCHIVE_VERSION.tar.xz
LIBARCHIVE_SHA256=a3dc0872b1b39da34dc18ef1b944f0f11ffd37425d70ae04be679279f1cf43d2
ZSTD_VERSION=1.5.7
ZSTD_URL=https://github.com/facebook/zstd/releases/download/v$ZSTD_VERSION/zstd-$ZSTD_VERSION.tar.gz
ZSTD_SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
XZ_VERSION=5.8.3
XZ_URL=https://github.com/tukaani-project/xz/releases/download/v$XZ_VERSION/xz-$XZ_VERSION.tar.gz
XZ_SHA256=3d3a1b973af218114f4f889bbaa2f4c037deaae0c8e815eec381c3d546b974a0
BZIP2_VERSION=1.0.8
BZIP2_URL=https://sourceware.org/pub/bzip2/bzip2-$BZIP2_VERSION.tar.gz
BZIP2_SHA256=ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269

# desktop only, and only where the system has no libmtp development files (LGPL-2.1, linked statically)
LIBMTP_VERSION=1.1.23
LIBMTP_URL=https://github.com/libmtp/libmtp/releases/download/v$LIBMTP_VERSION/libmtp-$LIBMTP_VERSION.tar.gz
LIBMTP_SHA256=74a2b6e8cb4a0304e95b995496ea3ac644c29371649b892b856e22f12a0bdeed

# download $1 to $2 once and check it against SHA-256 $3
fetch() {
    [ -f "$2" ] || { curl -fL -o "$2.part" "$1" && mv "$2.part" "$2"; }
    local sum
    sum=$( (sha256sum "$2" 2>/dev/null || shasum -a 256 "$2") | cut -d' ' -f1)
    [ "$sum" = "$3" ] || { echo "checksum mismatch for $2: $sum" >&2; return 1; }
}

# raylib and nanosvg sources into thirdparty/ ($1 = repo root)
fetch_sources() {
    [ -d "$1/thirdparty/raylib" ] || git clone --depth 1 --branch "$RAYLIB_TAG" "$RAYLIB_URL" "$1/thirdparty/raylib"
    mkdir -p "$1/thirdparty/nanosvg"
    fetch "$NANOSVG_URL/src/nanosvg.h" "$1/thirdparty/nanosvg/nanosvg.h" "$NANOSVG_H_SHA256"
    fetch "$NANOSVG_URL/src/nanosvgrast.h" "$1/thirdparty/nanosvg/nanosvgrast.h" "$NANOSVGRAST_H_SHA256"
    fetch "$NANOSVG_URL/LICENSE.txt" "$1/thirdparty/nanosvg/LICENSE.txt" "$NANOSVG_LICENSE_SHA256"
}

# $2 (URL, SHA-256 $3) unpacked once into thirdparty/$4 ($1 = repo root)
fetch_tarball() {
    [ -d "$1/thirdparty/$4" ] && return
    fetch "$2" "$1/thirdparty/${2##*/}" "$3"
    tar -C "$1/thirdparty" -xf "$1/thirdparty/${2##*/}"
}
