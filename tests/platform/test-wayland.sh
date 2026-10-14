#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
set -euo pipefail
app=$(realpath "${1:?Pass the app executable}")
logs=$(realpath -m "${2:?Pass a log directory}")
mkdir -p "$logs"
runtime=$(mktemp -d)
chmod 700 "$runtime"
compositor_pid=
cleanup() {
    if [[ -n "$compositor_pid" ]]; then
        kill "$compositor_pid" 2>/dev/null || true
        wait "$compositor_pid" 2>/dev/null || true
    fi
    rm -rf "$runtime"
}
trap cleanup EXIT
export XDG_RUNTIME_DIR="$runtime"
export WAYLAND_DISPLAY=yolopdf-test
weston --backend=headless-backend.so --renderer=pixman --shell=kiosk-shell.so --socket="$WAYLAND_DISPLAY" \
    --idle-time=0 --log="$logs/compositor.log" > "$logs/compositor-output.log" 2>&1 &
compositor_pid=$!
for attempt in {1..50}; do
    [[ -S "$runtime/$WAYLAND_DISPLAY" ]] && break
    if ! kill -0 "$compositor_pid" 2>/dev/null; then
        cat "$logs/compositor-output.log" "$logs/compositor.log"
        exit 1
    fi
    sleep .1
done
[[ -S "$runtime/$WAYLAND_DISPLAY" ]]
QT_QPA_PLATFORM=wayland QT_QUICK_BACKEND=software WAYLAND_DEBUG=client \
    timeout 30 "$app" --fixture-spike --smoke-test > "$logs/app.log" 2>&1
if ! grep -Eq 'set_app_id\("org\.yolopdf\.app"\)' "$logs/app.log"; then
    echo 'Wayland window did not use the installed desktop-file ID' >&2
    exit 1
fi
echo 'Native Wayland page rendering/editing and desktop identity verified'
