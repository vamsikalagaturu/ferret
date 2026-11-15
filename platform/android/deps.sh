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

# OpenSSL's libcrypto, libsmb2 and libssh2, static, per ABI: net_prefix ABI names where (build.sh passes it on)
net_prefix() { echo "$ROOT/net-$1-smb2-$LIBSMB2_VERSION-ssh2-$LIBSSH2_VERSION-openssl-$OPENSSL_VERSION-$BUILD_TAG"; }
fetch_net_sources "$REPO" openssl
for ABI in $ABIS; do
    OUT=$(net_prefix $ABI)
    [ -f "$OUT/smb2/lib/libsmb2.a" ] && [ -f "$OUT/ssh2/lib/libssh2.a" ] && continue
    step "OpenSSL $OPENSSL_VERSION, libsmb2 $LIBSMB2_VERSION, libssh2 $LIBSSH2_VERSION ($ABI)"
    command -v perl >/dev/null || { echo "missing: perl (OpenSSL's Configure)" >&2; exit 1; }
    case $ABI in
    arm64-v8a) TARGET=android-arm64 ;;
    x86_64) TARGET=android-x86_64 ;;
    *) echo "no OpenSSL target for $ABI" >&2; exit 1 ;;
    esac
    if [ ! -f "$OUT/openssl/lib/libcrypto.a" ]; then
        rm -rf "$OUT/openssl.build"
        mkdir -p "$OUT/openssl.build"
        # libcrypto alone: no programs, no loadable modules, no protocols ferret does not speak
        (cd "$OUT/openssl.build" && ANDROID_NDK_ROOT=$NDK PATH=$TC:$PATH perl "$REPO/thirdparty/openssl-$OPENSSL_VERSION/Configure" \
            $TARGET -D__ANDROID_API__=$MIN_API no-shared no-module no-tests no-apps no-docs no-engine no-quic no-comp \
            --prefix="$OUT/openssl" --libdir=lib $SIZE_CFLAGS >/dev/null &&
            ANDROID_NDK_ROOT=$NDK PATH=$TC:$PATH make -j"$JOBS" build_libs >/dev/null &&
            make install_dev >/dev/null)
    fi
    build_net "$OUT" -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI=$ABI \
        -DANDROID_PLATFORM=android-$MIN_API -DOPENSSL_ROOT_DIR="$OUT/openssl" -DOPENSSL_INCLUDE_DIR="$OUT/openssl/include" \
        -DOPENSSL_CRYPTO_LIBRARY="$OUT/openssl/lib/libcrypto.a" -DOPENSSL_SSL_LIBRARY="$OUT/openssl/lib/libssl.a"
done
