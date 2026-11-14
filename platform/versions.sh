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
