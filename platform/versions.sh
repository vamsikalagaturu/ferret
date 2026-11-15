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

# Network storage, linked statically: SMB (libsmb2, LGPL-2.1: ferret's own sources are open, as the licence asks of
# a static link) and SFTP (libssh2, BSD-3-Clause) over OpenSSL's libcrypto
LIBSMB2_VERSION=6.3
LIBSMB2_URL=https://github.com/sahlberg/libsmb2/archive/refs/tags/libsmb2-$LIBSMB2_VERSION.tar.gz
LIBSMB2_SHA256=5011003fd091513f897e1ebbb8e1a40af634386df7199716348b7f77569a2790
LIBSSH2_VERSION=1.11.1
LIBSSH2_URL=https://www.libssh2.org/download/libssh2-$LIBSSH2_VERSION.tar.gz
LIBSSH2_SHA256=d9ec76cbe34db98eec3539fe2c899d26b0c837cb3eb466a56b0f109cabf658f7
# Android has no libcrypto to link against; Linux uses the system's. 3.5 is the LTS line (to April 2030).
OPENSSL_VERSION=3.5.9
OPENSSL_URL=https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VERSION/openssl-$OPENSSL_VERSION.tar.gz
OPENSSL_SHA256=603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a

# FTP/FTPS and WebDAV (src/vfs/vfs_ftp.c, vfs_dav.c): libcurl (curl licence, MIT-like) with expat (MIT) for
# PROPFIND, over OpenSSL; NFS (src/vfs/vfs_nfs.c): libnfs (LGPL-2.1, static like libsmb2). Linux takes the system's
# libcurl and expat when it has their development files.
CURL_VERSION=8.23.0
CURL_URL=https://curl.se/download/curl-$CURL_VERSION.tar.xz
CURL_SHA256=8dadd7d2536a314f982ad05a651b713f426bf7d02cc2c76cffb9796f2218e6c5
EXPAT_VERSION=2.9.0
EXPAT_URL=https://github.com/libexpat/libexpat/releases/download/R_${EXPAT_VERSION//./_}/expat-$EXPAT_VERSION.tar.xz
EXPAT_SHA256=1e6371862cc31999b368c3b89b49994f0677e1bab5f1b2b85ae3741f5d803051
LIBNFS_VERSION=8.0.0
LIBNFS_URL=https://github.com/sahlberg/libnfs/archive/refs/tags/libnfs-$LIBNFS_VERSION.tar.gz
LIBNFS_SHA256=bc91216e927a85142b5de611c7f711558e02119a7daad67620ea631634038350

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

# tarball $1 (checked against $2) unpacked as thirdparty/$3 whatever its top folder is called ($4 = repo root):
# GitHub's tag archives name it repo-tag (libsmb2-libsmb2-6.3)
fetch_unpacked() {
    [ -d "$4/thirdparty/$3" ] && return 0
    fetch "$1" "$4/thirdparty/$3.tar.gz" "$2"
    mkdir -p "$4/thirdparty/$3.part"
    tar -xzf "$4/thirdparty/$3.tar.gz" -C "$4/thirdparty/$3.part" --strip-components=1
    mv "$4/thirdparty/$3.part" "$4/thirdparty/$3"
}

# libsmb2 and libssh2 into thirdparty/ ($1 = repo root); OpenSSL too with $2 = openssl
fetch_net_sources() {
    fetch_unpacked "$LIBSMB2_URL" "$LIBSMB2_SHA256" "libsmb2-$LIBSMB2_VERSION" "$1"
    fetch_unpacked "$LIBSSH2_URL" "$LIBSSH2_SHA256" "libssh2-$LIBSSH2_VERSION" "$1"
    if [ "${2:-}" = openssl ]; then fetch_unpacked "$OPENSSL_URL" "$OPENSSL_SHA256" "openssl-$OPENSSL_VERSION" "$1"; fi
}

# cmake project $1 built and installed into $2, with the extra arguments after them
build_static() {
    local src=$1 out=$2
    shift 2
    cmake -Wno-deprecated -S "$src" -B "$out.build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS_RELEASE="$SIZE_CFLAGS -DNDEBUG" \
        -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DCMAKE_INSTALL_PREFIX="$out" -DCMAKE_INSTALL_LIBDIR=lib "$@" >/dev/null
    cmake --build "$out.build" --target install -j"$JOBS" >/dev/null
}

# libsmb2 (no Kerberos: NTLM sign-in) and libssh2 (OpenSSL backend) into $1; the rest goes to cmake (the toolchain)
build_net() {
    local out=$1
    shift
    build_static "$REPO/thirdparty/libsmb2-$LIBSMB2_VERSION" "$out/smb2" -DENABLE_LIBKRB5=OFF -DENABLE_GSSAPI=OFF \
        -DENABLE_EXAMPLES=OFF "$@"
    build_static "$REPO/thirdparty/libssh2-$LIBSSH2_VERSION" "$out/ssh2" -DCRYPTO_BACKEND=OpenSSL -DBUILD_STATIC_LIBS=ON \
        -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF -DENABLE_ZLIB_COMPRESSION=OFF "$@"
}

# libnfs into $1/nfs (NFSv3 and v4, no RPC-over-TLS); the rest goes to cmake
build_nfs() {
    local out=$1
    shift
    fetch_unpacked "$LIBNFS_URL" "$LIBNFS_SHA256" "libnfs-$LIBNFS_VERSION" "$REPO"
    build_static "$REPO/thirdparty/libnfs-$LIBNFS_VERSION" "$out/nfs" -DENABLE_TLS=OFF -DENABLE_UTILS=OFF \
        -DENABLE_EXAMPLES=OFF -DENABLE_TESTS=OFF -DENABLE_DOCUMENTATION=OFF "$@"
}

# expat into $1/expat
build_expat() {
    local out=$1
    shift
    fetch_tarball "$REPO" "$EXPAT_URL" "$EXPAT_SHA256" "expat-$EXPAT_VERSION"
    build_static "$REPO/thirdparty/expat-$EXPAT_VERSION" "$out/expat" -DEXPAT_BUILD_TOOLS=OFF -DEXPAT_BUILD_EXAMPLES=OFF \
        -DEXPAT_BUILD_TESTS=OFF -DEXPAT_SHARED_LIBS=OFF -DEXPAT_BUILD_DOCS=OFF -DEXPAT_BUILD_FUZZERS=OFF "$@"
}

# libcurl into $1/curl with HTTP(S) and FTP(S) only, over OpenSSL (the toolchain and OPENSSL_* in the rest); the CA
# certificates are the system's, named at run time
build_curl() {
    local out=$1 off=()
    shift
    fetch_tarball "$REPO" "$CURL_URL" "$CURL_SHA256" "curl-$CURL_VERSION"
    for p in DICT FILE GOPHER IMAP LDAP LDAPS MQTT POP3 RTSP SMTP TELNET TFTP WEBSOCKETS IPFS ALTSVC HSTS COOKIES DOH \
        AWS NETRC KERBEROS_AUTH NEGOTIATE_AUTH BEARER_AUTH FORM_API MIME HTTPSIG; do
        off+=(-DCURL_DISABLE_$p=ON)
    done
    build_static "$REPO/thirdparty/curl-$CURL_VERSION" "$out/curl" -DBUILD_CURL_EXE=OFF -DBUILD_STATIC_LIBS=ON \
        -DBUILD_TESTING=OFF -DBUILD_EXAMPLES=OFF -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF -DENABLE_CURL_MANUAL=OFF \
        -DCURL_USE_OPENSSL=ON -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF -DCURL_USE_LIBSSH=OFF -DCURL_USE_GSSAPI=OFF \
        -DUSE_LIBIDN2=OFF -DUSE_NGHTTP2=OFF -DCURL_ZLIB=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DCURL_ENABLE_NTLM=OFF \
        -DCURL_CA_BUNDLE=none -DCURL_CA_PATH=none -DENABLE_THREADED_RESOLVER=ON -DCURL_USE_PKGCONFIG=OFF "${off[@]}" "$@"
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
