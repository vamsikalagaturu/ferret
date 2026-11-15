#!/usr/bin/env bash
# Installs the release build (platform/linux/build.sh) for this user: ~/.local/bin/ferret, a desktop entry and its
# icon. ferret updates that copy itself from GitHub releases (src/app/update.c), so it stays user-writable.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
BUILT=${FR_LINUX_OUT:-$HOME/.cache/ferret/linux}/build/ferret
[ -x "$BUILT" ] || { echo "no release build at $BUILT; run platform/linux/build.sh first" >&2; exit 1; }

BIN=$HOME/.local/bin
APPS=${XDG_DATA_HOME:-$HOME/.local/share}/applications
ICONS=${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor/192x192/apps
mkdir -p "$BIN" "$APPS" "$ICONS"
# beside it, then renamed: a running ferret keeps the old file until it exits
install -m 755 "$BUILT" "$BIN/.ferret.new"
mv -f "$BIN/.ferret.new" "$BIN/ferret"
install -m 644 "$REPO/platform/android/res/mipmap-xxxhdpi/ic_launcher.png" "$ICONS/ferret.png"
cat >"$APPS/ferret.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=ferret
Comment=Keyboard-first file explorer
Exec=$BIN/ferret %U
Icon=ferret
Terminal=false
Categories=System;FileTools;FileManager;
MimeType=inode/directory;
EOF
command -v update-desktop-database >/dev/null && update-desktop-database "$APPS" 2>/dev/null || true
echo "installed $BIN/ferret"
