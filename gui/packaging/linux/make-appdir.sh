#!/usr/bin/env bash
# Deploy RevStudio and the Qt runtime into an AppDir with linuxdeploy (GUI_Implementation_Note.md, section 10.4).
#
#   usage: make-appdir.sh <build-dir> <out-appdir>
#
# Environment:
#   QT_ROOT_DIR           the Qt installation to bundle. install-qt-action sets it in CI; for a local run point it at
#                         e.g. ~/Qt/6.8.3/gcc_64
#   QMAKE                 defaults to $QT_ROOT_DIR/bin/qmake (linuxdeploy-plugin-qt uses it to find Qt's plugins)
#   REVSTUDIO_TOOLS_DIR   where the downloaded tools are kept (default: ${RUNNER_TEMP:-/tmp}/revstudio-tools)
#
# The result is a relocatable directory tree (usr/bin, usr/lib, usr/plugins, ...) that runs without Qt installed.
set -euo pipefail

BUILD=${1:?usage: make-appdir.sh <build-dir> <out-appdir>}
APPDIR=${2:?usage: make-appdir.sh <build-dir> <out-appdir>}
: "${QT_ROOT_DIR:?QT_ROOT_DIR is not set (run install-qt-action, or point it at a Qt installation)}"

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TOOLS=${REVSTUDIO_TOOLS_DIR:-${RUNNER_TEMP:-/tmp}/revstudio-tools}
BUILD=$(cd "$BUILD" && pwd)

# Pinned releases with SHA-256 checksums (supply-chain hygiene: a moved tag or a tampered file fails the build).
# To update: pick a release, download it, verify it, and paste the output of sha256sum here.
LINUXDEPLOY_URL="https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage"
LINUXDEPLOY_SHA256="c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d"
QTPLUGIN_URL="https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/1-alpha-20250213-1/linuxdeploy-plugin-qt-x86_64.AppImage"
QTPLUGIN_SHA256="15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724"

fetch() {   # fetch <url> <sha256> <file>: download unless a verified copy is already there
    if [ -x "$3" ] && echo "$2  $3" | sha256sum -c --status - 2>/dev/null; then
        return
    fi
    curl -fsSL "$1" -o "$3"
    echo "$2  $3" | sha256sum -c -
    chmod +x "$3"
}

mkdir -p "$TOOLS"
( cd "$TOOLS"
  fetch "$LINUXDEPLOY_URL" "$LINUXDEPLOY_SHA256" linuxdeploy-x86_64.AppImage
  fetch "$QTPLUGIN_URL"    "$QTPLUGIN_SHA256"    linuxdeploy-plugin-qt-x86_64.AppImage )

rm -rf "$APPDIR"
mkdir -p "$APPDIR"
cmake --install "$BUILD" --prefix "$APPDIR/usr"

export APPIMAGE_EXTRACT_AND_RUN=1                       # CI runners have no FUSE
export QMAKE="${QMAKE:-$QT_ROOT_DIR/bin/qmake}"
export LD_LIBRARY_PATH="$QT_ROOT_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PATH="$TOOLS:$PATH"                              # linuxdeploy finds linuxdeploy-plugin-qt* on PATH

"$TOOLS/linuxdeploy-x86_64.AppImage" \
    --appdir "$APPDIR" \
    --executable "$APPDIR/usr/bin/RevStudio" \
    --desktop-file "$APPDIR/usr/share/applications/revstudio.desktop" \
    --icon-file "$HERE/revstudio.png" \
    --plugin qt

echo
echo "AppDir ready: $APPDIR"
