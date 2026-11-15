#!/usr/bin/env bash
# Fetches the sources pinned in platform/versions.sh into thirdparty/ and checks the Android SDK and NDK.
# build.sh sources this file first and uses the paths it sets.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
. "$REPO/platform/versions.sh"
ROOT=${FR_ANDROID_OUT:-$HOME/.cache/ferret/android}
SDK=${ANDROID_HOME:-$HOME/Android/Sdk}
NDK=${ANDROID_NDK:-$SDK/ndk/28.2.13676358}
# Android 11: MANAGE_EXTERNAL_STORAGE and WindowInsets types
MIN_API=30
ABIS="arm64-v8a x86_64"
JOBS=$(nproc)

TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
[ -e "$TC/clang" ] || { echo "missing: $TC/clang" >&2; exit 1; }
for tool in cmake ninja curl git; do
    command -v $tool >/dev/null || { echo "missing: $tool" >&2; exit 1; }
done

step() { printf '\n== %s\n' "$*"; }
mkdir -p "$ROOT"

step "raylib $RAYLIB_TAG, nanosvg sources"
fetch_sources "$REPO"

# libarchive with bzip2, xz and zstd, all static, into $ROOT/libarchive-<version>-<abi>-<tag> (FR_LIBARCHIVE_DIR);
# zlib is the NDK's
fetch_tarball "$REPO" "$LIBARCHIVE_URL" "$LIBARCHIVE_SHA256" "libarchive-$LIBARCHIVE_VERSION"
fetch_tarball "$REPO" "$ZSTD_URL" "$ZSTD_SHA256" "zstd-$ZSTD_VERSION"
fetch_tarball "$REPO" "$XZ_URL" "$XZ_SHA256" "xz-$XZ_VERSION"
fetch_tarball "$REPO" "$BZIP2_URL" "$BZIP2_SHA256" "bzip2-$BZIP2_VERSION"
libarchive_dir() { echo "$ROOT/libarchive-$LIBARCHIVE_VERSION-$1-$BUILD_TAG"; }
for ABI in $ABIS; do
    OUT=$(libarchive_dir $ABI)
    [ -f "$OUT/lib/libarchive.a" ] && continue
    step "libarchive $LIBARCHIVE_VERSION, zstd $ZSTD_VERSION, xz $XZ_VERSION, bzip2 $BZIP2_VERSION ($ABI)"
    B=$ROOT/libarchive-build-$ABI-$BUILD_TAG
    NDK_CMAKE=(-G Ninja -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI=$ABI
        -DANDROID_PLATFORM=android-$MIN_API -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS_RELEASE="$SIZE_CFLAGS -DNDEBUG"
        -DCMAKE_INSTALL_PREFIX="$OUT" -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_SHARED_LIBS=OFF)
    mkdir -p "$B/bzip2" "$OUT/lib" "$OUT/include"
    case $ABI in arm64-v8a) TRIPLE=aarch64-linux-android ;; x86_64) TRIPLE=x86_64-linux-android ;; esac
    BZ=$REPO/thirdparty/bzip2-$BZIP2_VERSION
    for f in blocksort huffman crctable randtable compress decompress bzlib; do
        "$TC/clang" --target=$TRIPLE$MIN_API $SIZE_CFLAGS -fPIC -DNDEBUG -D_FILE_OFFSET_BITS=64 -c "$BZ/$f.c" -o "$B/bzip2/$f.o"
    done
    "$TC/llvm-ar" rcs "$OUT/lib/libbz2.a" "$B"/bzip2/*.o
    cp "$BZ/bzlib.h" "$OUT/include/"
    cmake -S "$REPO/thirdparty/zstd-$ZSTD_VERSION/build/cmake" -B "$B/zstd" "${NDK_CMAKE[@]}" -DZSTD_BUILD_PROGRAMS=OFF \
        -DZSTD_BUILD_TESTS=OFF -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_STATIC=ON -DZSTD_LEGACY_SUPPORT=OFF \
        -DZSTD_MULTITHREAD_SUPPORT=OFF >/dev/null
    cmake --build "$B/zstd" --target install -j"$JOBS" >/dev/null
    cmake -S "$REPO/thirdparty/xz-$XZ_VERSION" -B "$B/xz" "${NDK_CMAKE[@]}" -DXZ_NLS=OFF -DXZ_DOC=OFF \
        -DXZ_TOOL_XZDEC=OFF -DXZ_TOOL_LZMADEC=OFF -DXZ_TOOL_LZMAINFO=OFF -DXZ_TOOL_XZ=OFF -DXZ_TOOL_SCRIPTS=OFF >/dev/null
    cmake --build "$B/xz" --target install -j"$JOBS" >/dev/null
    cmake -S "$REPO/thirdparty/libarchive-$LIBARCHIVE_VERSION" -B "$B/libarchive" "${NDK_CMAKE[@]}" \
        -DENABLE_TAR=OFF -DENABLE_CPIO=OFF -DENABLE_CAT=OFF -DENABLE_UNZIP=OFF -DENABLE_TEST=OFF \
        -DENABLE_OPENSSL=OFF -DENABLE_MBEDTLS=OFF -DENABLE_NETTLE=OFF -DENABLE_MD=OFF -DENABLE_CNG=OFF \
        -DENABLE_LIBB2=OFF -DENABLE_LZ4=OFF -DENABLE_LZO=OFF -DENABLE_LIBXML2=OFF -DENABLE_EXPAT=OFF \
        -DENABLE_BSDXML=OFF -DENABLE_PCREPOSIX=OFF -DENABLE_PCRE2POSIX=OFF -DENABLE_LIBGCC=OFF \
        -DENABLE_ACL=OFF -DENABLE_XATTR=OFF -DENABLE_ICONV=OFF -DENABLE_WERROR=OFF \
        -DBZIP2_INCLUDE_DIR="$OUT/include" -DBZIP2_LIBRARY_RELEASE="$OUT/lib/libbz2.a" \
        -DLIBLZMA_INCLUDE_DIR="$OUT/include" -DLIBLZMA_LIBRARY_RELEASE="$OUT/lib/liblzma.a" \
        -DZSTD_INCLUDE_DIR="$OUT/include" -DZSTD_LIBRARY="$OUT/lib/libzstd.a" >/dev/null
    cmake --build "$B/libarchive" --target install -j"$JOBS" >/dev/null
done
