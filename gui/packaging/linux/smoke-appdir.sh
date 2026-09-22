#!/usr/bin/env bash
# Smoke-test a DEPLOYED AppDir (GUI_Implementation_Note.md, sections 9.7 and 10.4).
#
#   usage: smoke-appdir.sh <AppDir> <path-of-a-backend>
#
# The backend is `mock-rb` in gui.yml (there is no rb to run there) and the real `rb` in the release bundle job.
#
# The point of the script is the scrubbed environment: everything runs under `env -i`, so nothing from the runner's
# Qt installation (LD_LIBRARY_PATH, QT_PLUGIN_PATH, ...) can hide a library or plugin that the bundle is missing.
#   1. every Qt and ICU library of RevStudio must resolve to a file INSIDE the AppDir
#   2. the self-test must pass on the real xcb platform under Xvfb
#
# Why xcb and not offscreen: the bundle deploys only the platform plugins that users need (xcb; linuxdeploy-plugin-qt
# does not add `offscreen`), so `QT_QPA_PLATFORM=offscreen` cannot be used on a DEPLOYED bundle. The build-tree tests
# in ctest do use offscreen. Found in phase 0.
set -euo pipefail

APPDIR=$(cd "${1:?usage: smoke-appdir.sh <AppDir> <backend>}" && pwd)
BACKEND=$(cd "$(dirname "${2:?usage: smoke-appdir.sh <AppDir> <backend>}")" && pwd)/$(basename "$2")
BIN="$APPDIR/usr/bin/RevStudio"
CLEAN=(env -i HOME="${HOME:-/tmp}" PATH=/usr/bin:/bin LANG=C.UTF-8)

# Some backends (mock-rb) are linked against the CI Qt through their build RPATH, which is fine: they are separate
# processes and are run from where they are, not copied into the AppDir.

echo "== 1. library resolution in a scrubbed environment"
if "${CLEAN[@]}" ldd "$BIN" | grep -q "not found"; then
    "${CLEAN[@]}" ldd "$BIN" | grep "not found" >&2
    echo "FAILED: unresolved libraries" >&2
    exit 1
fi
outside=$("${CLEAN[@]}" ldd "$BIN" | awk '/libQt6|libicu/ && $3 !~ "^'"$APPDIR"'/" {print}' || true)
if [ -n "$outside" ]; then
    echo "FAILED: these Qt/ICU libraries resolve OUTSIDE the AppDir:" >&2
    echo "$outside" >&2
    exit 1
fi
echo "ok: all Qt6 and ICU libraries come from the AppDir"

echo "== 2. self-test on the xcb platform under Xvfb"
if ! command -v xvfb-run >/dev/null 2>&1; then
    echo "FAILED: xvfb-run is required to test a deployed bundle (apt-get install xvfb)" >&2
    exit 1
fi
# `env -i` would also wipe DISPLAY and XAUTHORITY, which xvfb-run has just set; hand them over explicitly.
xvfb-run -a bash -c 'exec env -i HOME="$1" PATH=/usr/bin:/bin LANG=C.UTF-8 DISPLAY="$DISPLAY" XAUTHORITY="${XAUTHORITY:-}" QT_QPA_PLATFORM=xcb "${@:2}"' \
    _ "${HOME:-/tmp}" "$BIN" --selftest --rb "$BACKEND"

echo "smoke test passed"
