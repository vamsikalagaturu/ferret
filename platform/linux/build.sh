#!/usr/bin/env bash
# Builds ferret for Linux and runs the tests. Output: $FR_LINUX_OUT/build/ferret (default ~/.cache/ferret/linux).
# `build.sh debug` builds with -O0 -g and AddressSanitizer instead.
set -euo pipefail

MODE=${1:-release}
case "$MODE" in
release | debug) ;;
*) echo "usage: $0 [release | debug]" >&2; exit 1 ;;
esac

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
. "$HERE/deps.sh"

step "ferret ($MODE)"
if [ "$MODE" = debug ]; then
    BUILD=$ROOT/build-debug
    cmake --fresh -S "$REPO" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH="$RAYLIB_OUT" \
        -DFR_LIBARCHIVE_DIR="$LIBARCHIVE_OUT" -DFR_NET_PREFIX="$NET_OUT" \
        -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
        -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null
else
    BUILD=$ROOT/build
    cmake --fresh -S "$REPO" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$RAYLIB_OUT" \
        -DFR_LIBARCHIVE_DIR="$LIBARCHIVE_OUT" -DFR_NET_PREFIX="$NET_OUT" \
        -DCMAKE_C_FLAGS_RELEASE="$SIZE_CFLAGS -DNDEBUG" -DCMAKE_EXE_LINKER_FLAGS="-Wl,--gc-sections" \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null
fi
cmake --build "$BUILD" -j"$JOBS"
ctest --test-dir "$BUILD" --output-on-failure
ls -l "$BUILD/ferret"
