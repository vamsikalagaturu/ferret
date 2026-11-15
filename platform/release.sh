#!/usr/bin/env bash
# Publishes the version in CMakeLists.txt's project() as GitHub release vX.Y.Z of vamsikalagaturu/ferret, which the
# apps' updater reads (src/net/release.h): release builds (FR_RELEASE=1) for Linux and Android, the tag on the commit
# built, and the assets ferret-linux-x86_64 and ferret.apk. Run on a clean main that is pushed.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
. "$HERE/versions.sh"
TAG=v$FR_VERSION

[ -z "$(git -C "$REPO" status --porcelain --untracked-files=no)" ] || { echo "commit first: the release is built from HEAD" >&2; exit 1; }
gh release view "$TAG" -R vamsikalagaturu/ferret >/dev/null 2>&1 && { echo "$TAG exists; raise project() VERSION" >&2; exit 1; }

export FR_RELEASE=1
"$HERE/linux/build.sh" release
"$HERE/android/build.sh"

OUT=$(mktemp -d)
cp "${FR_LINUX_OUT:-$HOME/.cache/ferret/linux}/build/ferret" "$OUT/ferret-linux-$(uname -m)"
cp "${FR_ANDROID_OUT:-$HOME/.cache/ferret/android}/out/ferret.apk" "$OUT/ferret.apk"
gh release create "$TAG" -R vamsikalagaturu/ferret --target "$(git -C "$REPO" rev-parse HEAD)" --title "ferret $FR_VERSION" \
    --generate-notes "$OUT/ferret-linux-$(uname -m)" "$OUT/ferret.apk"
echo "released $TAG"
