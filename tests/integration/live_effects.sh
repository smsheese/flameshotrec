#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Flameshot Contributors
#
# Builds and runs the live-desktop + clipboard check
# (tests/integration/live_desktop_test.cpp).
#
# Usage:
#   tests/integration/live_effects.sh [BUILD_DIR]
#
# This check needs no application objects: the two modules it covers are
# compiled here with their own flags (plus -g, so a crash in the Xlib path has
# symbols), which also means it runs while the shared tree's application target
# cannot link. The widget-level overlay check is tests/integration/run.sh.
#
# Everything happens on a nested X server with a *real* compositing manager: only
# a composited server keeps the windows below the overlay readable, which is what
# the sampler relies on. The real desktop, pointer and clipboard are untouched
# (AGENTS.md rule 1).
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
build_dir=${1:-${FLAMESHOT_BUILD_DIR:-$repo_root/build_user}}
out_dir=${FLAMESHOT_INTEGRATION_DIR:-$build_dir/integration-tests}
display=${FLAMESHOT_EFFECTS_DISPLAY:-:95}
work_dir=$out_dir/effects-run

mkdir -p "$out_dir"

echo "building the live-desktop check"
# x11livedesktop.h carries a Q_OBJECT (LiveDesktop::Sampler), so its meta-object
# has to be generated here: the application target gets it from AUTOMOC, this
# standalone build does not.
moc=${FLAMESHOT_MOC:-/usr/lib/qt6/libexec/moc}
"$moc" "$repo_root/src/widgets/capture/x11livedesktop.h" \
    -o "$out_dir/moc_x11livedesktop.cpp"
env QT_QPA_PLATFORM=offscreen \
    g++ -std=c++17 -g -O1 -Wall -Wextra \
        -I"$repo_root/src" \
        $(pkg-config --cflags Qt6Widgets Qt6Gui Qt6Core) \
        "$repo_root/tests/integration/live_desktop_test.cpp" \
        "$repo_root/src/widgets/capture/x11livedesktop.cpp" \
        "$repo_root/src/widgets/capture/recordingclipboard.cpp" \
        "$out_dir/moc_x11livedesktop.cpp" \
        $(pkg-config --libs Qt6Widgets Qt6Gui Qt6Core) \
        -lXext -lX11 -o "$out_dir/live-desktop-test"

rm -rf "$work_dir"
mkdir -p "$work_dir/home" "$work_dir/config" "$work_dir/runtime" "$work_dir/cache"
chmod 700 "$work_dir/runtime"

cleanup() {
    if [[ -n "${wm_pid:-}" ]]; then
        kill "$wm_pid" 2>/dev/null || true
        wait "$wm_pid" 2>/dev/null || true
    fi
    if [[ -n "${xephyr_pid:-}" ]]; then
        kill "$xephyr_pid" 2>/dev/null || true
        wait "$xephyr_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT

rm -f "/tmp/.X11-unix/X${display#:}" "/tmp/.X${display#:}-lock"
Xephyr "$display" -screen 800x600 -nolisten tcp -noreset \
    > "$work_dir/xephyr.log" 2>&1 &
xephyr_pid=$!
for _ in $(seq 1 100); do
    DISPLAY=$display xdpyinfo > /dev/null 2>&1 && break
    sleep 0.1
done
if ! DISPLAY=$display xdpyinfo > /dev/null 2>&1; then
    echo "error: nested X server $display did not start" >&2
    cat "$work_dir/xephyr.log" >&2
    exit 2
fi

# metacity composites with XRender, which works on a nested, GL-less Xephyr.
env -i DISPLAY=$display HOME="$work_dir/home" \
    XDG_CONFIG_HOME="$work_dir/config" XDG_CACHE_HOME="$work_dir/cache" \
    XDG_RUNTIME_DIR="$work_dir/runtime" PATH=/usr/bin:/bin \
    metacity > "$work_dir/wm.log" 2>&1 &
wm_pid=$!
sleep 2

echo "running the live-desktop check"
env -i DISPLAY=$display HOME="$work_dir/home" \
    XDG_CONFIG_HOME="$work_dir/config" XDG_CACHE_HOME="$work_dir/cache" \
    XDG_RUNTIME_DIR="$work_dir/runtime" PATH=/usr/bin:/bin \
    QT_QPA_PLATFORM=xcb FLAMESHOT_DEBUG_SAMPLER=1 \
    "$out_dir/live-desktop-test"
