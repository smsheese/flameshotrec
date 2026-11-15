#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Flameshot Contributors
#
# Two-process clipboard check for a copied recording.
#
# Usage:
#   tests/integration/clipboard_handoff.sh [BUILD_DIR]
#
# Three separate processes on a nested X server:
#   clipboard_owner   the clipboard daemon (FlameshotDaemon + D-Bus service)
#   clipboard_copier  the capture process' side of "Copy", which exits
#   clipboard_reader  an independent process that links nothing from the app
#
# What it pins:
#   * with a session bus, the daemon keeps the file URI after the copying process
#     is gone, and all four flavours are readable from a *different* process
#     (text/uri-list, GNOME copied-files, KDE cut flag, plain path), both through
#     Qt and through xclip (not Qt at all);
#   * the copied file still exists and decodes after the copier exited;
#   * without any session bus the copier does not exit and does not lose the
#     data: it stays the selection owner and a different process still reads the
#     flavours. No clipboard manager is assumed anywhere.
#
# Nothing here captures the desktop or touches the sound server: the fixture is a
# synthetic test pattern written by ffmpeg (AGENTS.md rule 1).
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
build_dir=${1:-${FLAMESHOT_BUILD_DIR:-$repo_root/build_user}}
out_dir=${FLAMESHOT_INTEGRATION_DIR:-$build_dir/integration-tests}
display=${FLAMESHOT_CLIPBOARD_DISPLAY:-:94}
work_dir=$out_dir/clipboard-run
failures=0

check() {
    local state=PASS
    if [[ "$1" != "1" ]]; then
        state=FAIL
        failures=$((failures + 1))
    fi
    printf '%s  %s\n' "$state" "$2"
}

note() { printf '      %s\n' "$1"; }

for required in "$build_dir/compile_commands.json" \
                "$build_dir/src/CMakeFiles/flameshot.dir/link.txt"; do
    if [[ ! -f "$required" ]]; then
        echo "error: missing $required (configure and build Flameshot first)" >&2
        exit 2
    fi
done

mkdir -p "$out_dir" "$work_dir"

echo "building the three helper processes"

# The reader links nothing from the application on purpose.
g++ -std=c++17 -g -O1 -Wall -Wextra -I"$repo_root/src" \
    $(pkg-config --cflags Qt6Widgets Qt6Gui Qt6Core) \
    "$repo_root/tests/integration/clipboard_reader.cpp" \
    $(pkg-config --libs Qt6Widgets Qt6Gui Qt6Core) \
    -o "$out_dir/clipboard_reader"

# The owner and the copier are application code, so they are compiled with the
# tree's own flags and linked with the application's own libraries (minus its
# main()), exactly like the widget harness does.
python3 - "$repo_root" "$build_dir" "$out_dir" <<'PYTHON'
import json
import os
import shlex
import subprocess
import sys

repo_root, build_dir, out_dir = sys.argv[1:4]
with open(os.path.join(build_dir, "compile_commands.json")) as handle:
    database = json.load(handle)


def flags_for(suffix):
    for entry in database:
        if entry["file"].endswith(suffix):
            raw = entry.get("command") or " ".join(entry["arguments"])
            tokens = shlex.split(raw)
            flags = []
            drop_next = False
            for token in tokens:
                if drop_next:
                    drop_next = False
                    continue
                if token in ("-o", "-c"):
                    drop_next = True
                    continue
                if token.endswith(".cpp") or token.endswith(".o"):
                    continue
                flags.append(token)
            return flags
    raise SystemExit("error: no compile command for " + suffix)


flags = flags_for("widgets/capture/capturewidget.cpp")
objects = []
for name in ("clipboard_owner", "clipboard_copier"):
    source = os.path.join(repo_root, "tests/integration", name + ".cpp")
    obj = os.path.join(out_dir, name + ".o")
    print("compiling", source)
    subprocess.run(flags + ["-g", "-c", source, "-o", obj], check=True)
    objects.append(obj)

with open(os.path.join(build_dir, "src/CMakeFiles/flameshot.dir/link.txt")) as handle:
    link_text = handle.read()
link = shlex.split(link_text)
prefix = "CMakeFiles/flameshot.dir/"
command = []
skip_next = False
for token in link:
    if skip_next:
        skip_next = False
        continue
    if token.startswith("-Wl,--dependency-file="):
        continue
    if token == "-o":
        skip_next = True
        continue
    if token == "CMakeFiles/flameshot.dir/main.cpp.o":
        continue
    if (token.startswith(prefix) and token.endswith(".o")
            and "autogen" not in token):
        source = os.path.join(repo_root, "src", token[len(prefix):-2])
        if not os.path.exists(source):
            continue
        if not os.path.exists(os.path.join(build_dir, "src", token)):
            continue
    command.append(token)

position = 0
for index, token in enumerate(command):
    if token.startswith("-l") or token.endswith(".a") or token.endswith(".so"):
        break
    position = index + 1
for name, obj in zip(("clipboard_owner", "clipboard_copier"), objects):
    binary = os.path.join(out_dir, name)
    print("linking", binary)
    subprocess.run(command[:position] + [obj] + command[position:] + ["-o", binary],
                   cwd=os.path.join(build_dir, "src"), check=True)
PYTHON

cleanup() {
    if [[ -n "${xephyr_pid:-}" ]]; then
        kill "$xephyr_pid" 2>/dev/null || true
        wait "$xephyr_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT

rm -rf "$work_dir"
mkdir -p "$work_dir/config/flameshot" "$work_dir/home" "$work_dir/runtime" "$work_dir/staged"
chmod 700 "$work_dir/runtime"

# A deterministic, throw-away configuration: no tray icon (there is no tray on a
# nested server), no update checks, and a daemon that stays up.
cat > "$work_dir/config/flameshot/flameshot.ini" <<EOF
[General]
disabledTrayIcon=true
checkForUpdates=false
autoCloseIdleDaemon=false
savePath=$work_dir/home
recordingVideoPath=$work_dir/staged
recordingHoldDuration=3000
recordingFadeDuration=300
drawColor=#00ff00
uiColor=#00ff00
contrastUiColor=#ff00ff
EOF

run_env() {
    env -i DISPLAY=$display HOME="$work_dir/home" \
        XDG_CONFIG_HOME="$work_dir/config" XDG_CACHE_HOME="$work_dir/cache" \
        XDG_DATA_HOME="$work_dir/data" XDG_RUNTIME_DIR="$work_dir/runtime" \
        PATH=/usr/bin:/bin QT_QPA_PLATFORM=xcb "$@"
}

rm -f "/tmp/.X11-unix/X${display#:}" "/tmp/.X${display#:}-lock"
Xephyr "$display" -screen 1024x768 -nolisten tcp -noreset \
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

# A real, decodable video file: a synthetic test pattern, not a capture.
video=$work_dir/fixture.mp4
if command -v ffmpeg > /dev/null; then
    ffmpeg -nostdin -v error -f lavfi -i testsrc=size=64x48:rate=10 -t 1 \
        -pix_fmt yuv420p -y "$video" > /dev/null 2>&1 || true
fi
if [[ ! -s "$video" ]]; then
    echo "  note  ffmpeg is unavailable, using a plain file (decode check skipped)"
    : > "$video"
    printf 'not a video' > "$video"
fi

owner_log=$work_dir/owner.log
copier_log=$work_dir/copier.log
reader_log=$work_dir/reader.log

echo
echo "scenario A: a session bus, and the copier exits before the read"
if command -v dbus-run-session > /dev/null; then
    dbus-run-session -- bash -c '
        set -u
        out_dir="$1"; work_dir="$2"; video="$3"
        display="$4"
        env -i DISPLAY=$display HOME="$work_dir/home" \
            XDG_CONFIG_HOME="$work_dir/config" XDG_CACHE_HOME="$work_dir/cache" \
            XDG_DATA_HOME="$work_dir/data" XDG_RUNTIME_DIR="$work_dir/runtime" \
            PATH=/usr/bin:/bin QT_QPA_PLATFORM=xcb \
            "$out_dir/clipboard_owner" > "$work_dir/owner.log" 2>&1 &
        owner=$!
        for _ in $(seq 1 100); do
            grep -q OWNER_READY "$work_dir/owner.log" 2>/dev/null && break
            sleep 0.1
        done
        env -i DISPLAY=$display HOME="$work_dir/home" \
            XDG_CONFIG_HOME="$work_dir/config" XDG_CACHE_HOME="$work_dir/cache" \
            XDG_DATA_HOME="$work_dir/data" XDG_RUNTIME_DIR="$work_dir/runtime" \
            PATH=/usr/bin:/bin QT_QPA_PLATFORM=xcb \
            "$out_dir/clipboard_copier" "$video" > "$work_dir/copier.log" 2>&1
        echo "COPIER_EXIT $?" >> "$work_dir/copier.log"
        sleep 0.3
        # The copying process is gone; whatever still offers the file is the daemon.
        if kill -0 "$owner" 2>/dev/null; then
            echo "OWNER_ALIVE 1" >> "$work_dir/owner.log"
        fi
        env -i DISPLAY=$display HOME="$work_dir/home" \
            PATH=/usr/bin:/bin \
            xclip -o -selection clipboard -t text/uri-list \
            > "$work_dir/xclip-uri.txt" 2>/dev/null || true
        env -i DISPLAY=$display HOME="$work_dir/home" \
            PATH=/usr/bin:/bin \
            xclip -o -selection clipboard -t x-special/gnome-copied-files \
            > "$work_dir/xclip-gnome.txt" 2>/dev/null || true
        env -i DISPLAY=$display HOME="$work_dir/home" \
            PATH=/usr/bin:/bin \
            xclip -o -selection clipboard -t application/x-kde-cutselection \
            > "$work_dir/xclip-kde.txt" 2>/dev/null || true
        env -i DISPLAY=$display HOME="$work_dir/home" \
            PATH=/usr/bin:/bin \
            xclip -o -selection clipboard -t text/plain \
            > "$work_dir/xclip-text.txt" 2>/dev/null || true
        env -i DISPLAY=$display HOME="$work_dir/home" \
            XDG_CONFIG_HOME="$work_dir/config" PATH=/usr/bin:/bin \
            QT_QPA_PLATFORM=xcb \
            "$out_dir/clipboard_reader" > "$work_dir/reader.log" 2>&1
        echo "READER_EXIT $?" >> "$work_dir/reader.log"
        kill "$owner" 2>/dev/null || true
        wait "$owner" 2>/dev/null || true
    ' bash "$out_dir" "$work_dir" "$video" "$display"

    owner_ready=$(grep -c OWNER_READY "$owner_log" || true)
    check "$([[ "$owner_ready" -ge 1 ]] && echo 1 || echo 0)" \
        "the clipboard daemon started and owns org.flameshot.Flameshot"
    copier_offered=$(sed -n 's/^COPIER_OFFERED //p' "$copier_log" | head -1)
    check "$([[ "${copier_offered:-0}" == "1" ]] && echo 1 || echo 0)" \
        "the copier put the file URI on the clipboard"
    copier_exit=$(sed -n 's/^COPIER_EXIT //p' "$copier_log" | head -1)
    check "$([[ "${copier_exit:-1}" == "0" ]] && echo 1 || echo 0)" \
        "the copier exited successfully (nothing is keeping it alive)"
    staged=$(sed -n 's/^COPIER_FILE //p' "$copier_log" | head -1)
    note "staged file: ${staged:-<none>}"
    check "$([[ -n "${staged:-}" && -s "${staged:-/nonexistent}" ]] && echo 1 || echo 0)" \
        "the staged file still exists after the copier exited"
    if [[ -n "${staged:-}" ]] && command -v ffprobe > /dev/null; then
        if ffprobe -v error -select_streams v:0 -show_entries stream=codec_name \
                -of csv=p=0 "$staged" 2>/dev/null | grep -q .; then
            check 1 "the copied file decodes as a video after the copier exited"
        elif [[ -s "$video" ]] && [[ "$(head -c 4 "$video")" == "----" ]]; then
            note "decode check skipped: the fixture is not a video"
            check 1 "decode check not applicable (no ffmpeg fixture)"
        else
            check 0 "the copied file decodes as a video after the copier exited"
        fi
    fi

    uri=$(head -1 "$work_dir/xclip-uri.txt" 2>/dev/null | tr -d '\r' || true)
    expected_uri="file://${staged:-/nonexistent}"
    note "xclip text/uri-list: ${uri:-<empty>}"
    check "$([[ "$uri" == "$expected_uri" ]] && echo 1 || echo 0)" \
        "an external, non-Qt process reads text/uri-list from the daemon"
    gnome=$(head -1 "$work_dir/xclip-gnome.txt" 2>/dev/null | tr -d '\r' || true)
    check "$([[ "$gnome" == "copy" ]] && echo 1 || echo 0)" \
        "the GNOME copied-files flavour says copy (got '${gnome:-<empty>}')"
    kde=$(head -1 "$work_dir/xclip-kde.txt" 2>/dev/null | tr -d '\r' || true)
    check "$([[ "$kde" == "0" ]] && echo 1 || echo 0)" \
        "the KDE cut flag says copy, not move (got '${kde:-<empty>}')"
    text=$(head -1 "$work_dir/xclip-text.txt" 2>/dev/null | tr -d '\r' || true)
    check "$([[ "$text" == "${staged:-/nonexistent}" ]] && echo 1 || echo 0)" \
        "the plain path is readable for text targets"

    has_urls=$(sed -n 's/^READER_HASURLS //p' "$reader_log" | head -1)
    check "$([[ "${has_urls:-0}" == "1" ]] && echo 1 || echo 0)" \
        "an independent process sees a file URI after the copier exited"
    reader_gnome=$(sed -n 's/^READER_GNOME //p' "$reader_log" | head -1)
    decoded_gnome=$(printf '%s' "${reader_gnome:-}" | base64 -d 2>/dev/null | head -2 | tr -d '\n')
    note "reader GNOME flavour: '${decoded_gnome:-<none>}'"
    check "$([[ "$decoded_gnome" == "copy$expected_uri" ]] && echo 1 || echo 0)" \
        "the independent reader sees the GNOME copy flavour with the file URI"
    reader_kde=$(sed -n 's/^READER_KDE //p' "$reader_log" | head -1)
    decoded_kde=$(printf '%s' "${reader_kde:-}" | base64 -d 2>/dev/null)
    check "$([[ "$decoded_kde" == "0" ]] && echo 1 || echo 0)" \
        "the independent reader sees the KDE copy flag"
else
    note "dbus-run-session is unavailable: scenario A skipped (not a pass)"
    failures=$((failures + 1))
fi

echo
echo "scenario B: no usable session bus -- the copier must stay the owner"
# Qt autolaunches a session bus on X11 when DBUS_SESSION_BUS_ADDRESS is unset, so
# "absent" is expressed as an address that cannot exist. That is the documented
# way to disable the autolaunch, and it is what makes this the *no daemon* case.
no_bus_address="unix:path=$work_dir/definitely-not-a-bus"
copier_stay_log=$work_dir/copier-stay.log
env -i DISPLAY=$display HOME="$work_dir/home" \
    XDG_CONFIG_HOME="$work_dir/config" XDG_CACHE_HOME="$work_dir/cache" \
    XDG_DATA_HOME="$work_dir/data" XDG_RUNTIME_DIR="$work_dir/runtime" \
    DBUS_SESSION_BUS_ADDRESS="$no_bus_address" \
    PATH=/usr/bin:/bin QT_QPA_PLATFORM=xcb \
    "$out_dir/clipboard_copier" "$video" --stay > "$copier_stay_log" 2>&1 &
stay_pid=$!
for _ in $(seq 1 100); do
    grep -q '^COPIER_OFFERED' "$copier_stay_log" 2>/dev/null && break
    sleep 0.1
done
offered=$(sed -n 's/^COPIER_OFFERED //p' "$copier_stay_log" | head -1)
check "$([[ "${offered:-0}" == "1" ]] && echo 1 || echo 0)" \
    "without a session bus the copy still succeeds locally"
if kill -0 "$stay_pid" 2>/dev/null; then
    check 1 "without a session bus the copier keeps running instead of exiting"
else
    check 0 "without a session bus the copier keeps running instead of exiting"
fi
note "copier fallback log: $(grep -cE 'no session bus|did not take the copied file' "$copier_stay_log" || true) notice(s)"
reader_stay=$work_dir/reader-stay.log
env -i DISPLAY=$display HOME="$work_dir/home" PATH=/usr/bin:/bin \
    QT_QPA_PLATFORM=xcb "$out_dir/clipboard_reader" > "$reader_stay" 2>&1 || true
stay_uri=$(sed -n 's/^READER_URI //p' "$reader_stay" | head -1 | tr -d '\r')
check "$([[ -n "${stay_uri:-}" ]] && echo 1 || echo 0)" \
    "with no bus, an independent process still reads the file URI from the owner"
if grep -qE 'no session bus|did not take the copied file' "$copier_stay_log"; then
    check 1 "the fallback is reported, not silent"
else
    check 0 "the fallback is reported, not silent"
fi
kill "$stay_pid" 2>/dev/null || true
wait "$stay_pid" 2>/dev/null || true
if [[ -n "${staged:-}" && -s "${staged:-}" ]]; then
    check 1 "losing the selection owner never deletes the staged file"
else
    check 0 "losing the selection owner never deletes the staged file"
fi

echo
printf 'RESULT: %s (%d failure(s))\n' \
    "$([[ $failures -eq 0 ]] && echo 'all checks passed' || echo FAILURES)" \
    "$failures"
[[ $failures -eq 0 ]]
