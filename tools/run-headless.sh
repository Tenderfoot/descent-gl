#!/usr/bin/env bash
# Run the game with no physical display, for smoke tests and screenshots in a
# Linux dev container. Mesa's llvmpipe only advertises GL 4.5, so GL is routed
# through zink (GL on Vulkan) over lavapipe, which does advertise 4.6 core.
#
#   tools/run-headless.sh --frames 120 --screenshot /tmp/shot.png
#
# Software rendering: expect tens of fps, not thousands. The step rate in the
# exit line should still sit on 120.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
config="${DVK_CONFIG:-Debug}"
exe="$root/build/$config/descent"
display="${DVK_DISPLAY:-:99}"

[ -x "$exe" ] || { echo "not built: $exe" >&2; exit 1; }

if ! xdpyinfo -display "$display" >/dev/null 2>&1; then
    Xvfb "$display" -screen 0 1600x900x24 >/tmp/xvfb.log 2>&1 &
    sleep 2
fi

cd "$(dirname "$exe")"
DISPLAY="$display" \
MESA_LOADER_DRIVER_OVERRIDE=zink \
GALLIUM_DRIVER=zink \
LIBGL_ALWAYS_SOFTWARE=1 \
exec ./descent "$@"
