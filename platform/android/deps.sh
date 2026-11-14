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
