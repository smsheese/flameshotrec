#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Flameshot Contributors
#
# Builds and runs the integrated CaptureWidget check
# (tests/integration/capturewidget_test.cpp).
#
# Usage:
#   tests/integration/run.sh [BUILD_DIR]
#
# BUILD_DIR is a configured and built Flameshot tree (default: the in-repo
# build_user, per AGENTS.md rule 4). The harness reuses that tree's compiler
# flags and link line, recompiling only its own source, so it is a targeted
# check rather than a second project build.
#
# The whole run happens on a *nested* X server (Xephyr) with a window manager
# and a compositor, two synthetic monitors, a throw-away XDG_CONFIG_HOME and
# audio disabled: the real desktop, the real pointer and the real microphone are
# never touched (AGENTS.md rule 1).
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
build_dir=${1:-${FLAMESHOT_BUILD_DIR:-$repo_root/build_user}}
out_dir=${FLAMESHOT_INTEGRATION_DIR:-$build_dir/integration-tests}
display=${FLAMESHOT_TEST_DISPLAY:-:96}
work_dir=$out_dir/run

if [[ ! -f "$build_dir/compile_commands.json" ]]; then
    echo "error: $build_dir has no compile_commands.json" >&2
    echo "       configure and build Flameshot there first" >&2
    exit 2
fi
if [[ ! -f "$build_dir/src/CMakeFiles/flameshot.dir/link.txt" ]]; then
    echo "error: $build_dir has no link line (build the flameshot target)" >&2
    exit 2
fi

mkdir -p "$out_dir"

python3 - "$repo_root" "$build_dir" "$out_dir" <<'PYTHON'
import json
import os
import shlex
import subprocess
import sys

repo_root, build_dir, out_dir = sys.argv[1:4]

with open(os.path.join(build_dir, "compile_commands.json")) as handle:
    database = json.load(handle)


def compiler_flags(suffix):
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
    # A new translation unit has no entry of its own until it is registered; it
    # compiles with the same project flags as the capture widget, which is the
    # closest registered neighbour (same include roots, same ENABLE_RECORDING).
    for entry in database:
        if entry["file"].endswith("widgets/capture/capturewidget.cpp"):
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


# Sources this harness may have to compile itself:
#  * a new translation unit only links after its owner registers it in CMake, so
#    it is absent from the application object list;
#  * the shared build directory can be older than the tree (another agent is
#    editing, and a full build is not allowed while that happens), in which case
#    the application object does not contain the current source at all.
# Both are compiled here with that source's own flags and linked instead of the
# stale/absent entry, so the check always tests the tree as it is.
owned_sources = [
    "src/widgets/capture/capturerecording.cpp",
    "src/widgets/capture/capturewidget.cpp",
    "src/widgets/capture/x11livedesktop.cpp",
    "src/widgets/capture/recordingclipboard.cpp",
    "src/recording/capturerecordingsession.cpp",
]

source = os.path.join(repo_root, "tests/integration/capturewidget_test.cpp")
object_path = os.path.join(out_dir, "capturewidget_test.cpp.o")
flags = compiler_flags("widgets/capture/capturewidget.cpp")
# The application target does not link Qt Test, but QTest drives the widget
# events; reuse the configured Qt include root for the QtTest module.
qt_test_include = None
for index, token in enumerate(flags):
    if token == "-isystem" and flags[index + 1].endswith("/qt6/QtCore"):
        qt_test_include = flags[index + 1][: -len("QtCore")] + "QtTest"
if qt_test_include is None:
    raise SystemExit("error: cannot locate the Qt include root")
flags = flags + ["-isystem", qt_test_include, "-g"]
print("compiling", source)
subprocess.run(flags + ["-c", source, "-o", object_path], check=True)

with open(os.path.join(build_dir, "src/CMakeFiles/flameshot.dir/link.txt")) as handle:
    link_text = handle.read()
link = shlex.split(link_text)

extra_objects = []
replaced = set()
for relative in owned_sources:
    object_name = "CMakeFiles/flameshot.dir/" + relative[len("src/"):] + ".o"
    object_file = os.path.join(build_dir, "src", object_name)
    source_file = os.path.join(repo_root, relative)
    if not os.path.exists(source_file):
        continue
    registered = object_name in link_text

    def dependency_mtime(object_path):
        """Newest mtime of the object's recorded dependencies, or 0."""
        dep_path = object_path + ".d"
        if not os.path.exists(dep_path):
            return 0
        newest = 0.0
        with open(dep_path, "r", errors="replace") as handle:
            text = handle.read().replace("\\\n", " ")
        for token in text.replace("\\", " ").split():
            candidate = token.rstrip(":") if token.endswith(":") else token
            if not candidate.startswith("/") or not os.path.exists(candidate):
                continue
            newest = max(newest, os.path.getmtime(candidate))
        return newest

    # A header change is a change of the code: an object whose own source is
    # older than a header it includes is stale, and linking it against a fresh
    # translation unit is exactly how a mismatched struct layout turns into a
    # crash somewhere else.
    fresh = False
    if registered and os.path.exists(object_file):
        object_mtime = os.path.getmtime(object_file)
        newest_input = max(os.path.getmtime(source_file),
                           dependency_mtime(object_file))
        fresh = object_mtime >= newest_input
    if fresh:
        print("up to date in the application target:", relative)
        continue
    reason = "not yet registered" if not registered else "newer than its object"
    print(f"compiling ({reason}):", relative)
    source_flags = compiler_flags(relative[len("src/"):])
    extra_object = os.path.join(out_dir, os.path.basename(relative) + ".o")
    subprocess.run(source_flags + ["-g", "-c", source_file, "-o", extra_object],
                   check=True)
    extra_objects.append(extra_object)
    if registered:
        replaced.add(object_name)

command = []
skip_next = False
prefix = "CMakeFiles/flameshot.dir/"
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
    if token in replaced:
        continue
    if (token.startswith(prefix) and token.endswith(".o") and
            "autogen" not in token):
        # A shared build directory can outlive the source of an object (another
        # agent deletes a file while this tree is not rebuilt). Linking such a
        # stale object against a library that now defines the same symbols fails
        # for a reason the harness must not present as a test failure.
        # Two shared-tree situations produce link entries that cannot work:
        # the source was deleted, or the source moved into a library
        # (libflameshot-*.a) and the per-source object was never built. Both are
        # integration-build leftovers; a genuinely missing symbol still fails
        # the link below.
        source = os.path.join(repo_root, "src", token[len(prefix):-2])
        object_file = os.path.join(build_dir, "src", token)
        if not os.path.exists(source):
            print("skipping stale object (source is gone):", token)
            continue
        if not os.path.exists(object_file):
            print("skipping unbuilt object (source now lives in a library):", token)
            continue
    command.append(token)

binary = os.path.join(out_dir, "capturewidget_test")
# Order matters for static archives: an object appended after
# libflameshot-recording.a can no longer pull symbols out of it. So the harness
# objects go with the application objects, before every library.
position = 0
for index, token in enumerate(command):
    if token.startswith("-l") or token.endswith(".a") or token.endswith(".so"):
        break
    position = index + 1
command = command[:position] + [object_path] + extra_objects + command[position:]
command += ["-o", binary, "-lQt6Test"]
print("linking", binary)
subprocess.run(command, cwd=os.path.join(build_dir, "src"), check=True)
PYTHON

rm -rf "$work_dir"
mkdir -p "$work_dir/config/flameshot" "$work_dir/home" "$work_dir/videos" "$work_dir/runtime"
chmod 700 "$work_dir/runtime"

# The recording UI needs a compositor (_NET_WM_CM_S0), and the live-invert
# checks read real composited pixels. metacity composites with XRender, which
# works on a nested, GL-less Xephyr; muffin (Cinnamon) is kept as a fallback but
# does not acquire the selection on this server.
wm_binary=metacity
if ! command -v "$wm_binary" > /dev/null; then
    wm_binary=muffin
fi
env -i DISPLAY=$display HOME="$work_dir/home" \
    XDG_CONFIG_HOME="$work_dir/config" XDG_CACHE_HOME="$work_dir/cache" \
    XDG_DATA_HOME="$work_dir/data" XDG_RUNTIME_DIR="$work_dir/runtime" \
    GSETTINGS_BACKEND=memory PATH=/usr/bin:/bin \
    "$wm_binary" > "$work_dir/wm.log" 2>&1 &
wm_pid=$!
sleep 2

cat > "$work_dir/config/flameshot/flameshot.ini" <<EOF
[General]
recordingVideoPath=$work_dir/videos
recordingFilenamePattern=capture
recordingContainer=mp4
recordingCodec=libx264
recordingFrameRate=10
recordingQuality=45
recordingBitrateKbps=2000
recordingRateControl=crf
recordingHoldDuration=3000
recordingFadeDuration=300
recordingAudioSystemEnabled=false
recordingAudioMicEnabled=false
recordingTransferEnabled=false
insecurePixelate=false
drawColor=#00ff00
contrastUiColor=#ff00ff
uiColor=#00ff00
EOF

cleanup() {
    if [[ -n "${xephyr_pid:-}" ]]; then
        kill "$xephyr_pid" 2>/dev/null || true
        wait "$xephyr_pid" 2>/dev/null || true
    fi
    if [[ -n "${wm_pid:-}" ]]; then
        kill "$wm_pid" 2>/dev/null || true
        wait "$wm_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT

rm -f "/tmp/.X11-unix/X${display#:}" "/tmp/.X${display#:}-lock"
Xephyr "$display" -screen 1200x600 -nolisten tcp -noreset \
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

# Two synthetic monitors; a real drag must cross their boundary.
DISPLAY=$display xrandr --setmonitor LEFT 800/212x600/159+0+0 none >/dev/null
DISPLAY=$display xrandr --setmonitor RIGHT 400/106x600/159+800+0 none >/dev/null
DISPLAY=$display xrandr --listmonitors | sed 's/^/      /'

env -i DISPLAY=$display HOME="$work_dir/home" \
    XDG_CONFIG_HOME="$work_dir/config" XDG_CACHE_HOME="$work_dir/cache" \
    XDG_DATA_HOME="$work_dir/data" XDG_RUNTIME_DIR="$work_dir/runtime" \
    GSETTINGS_BACKEND=memory PATH=/usr/bin:/bin \
    muffin > "$work_dir/wm.log" 2>&1 &
wm_pid=$!
sleep 2

echo "running the integrated capture widget check"
# FLAMESHOT_DEBUG_SAMPLER=1 makes the live-desktop sampler explain each window it
# walks and why it abstains, which is what a compositor regression needs.
env -i DISPLAY=$display HOME="$work_dir/home" \
    XDG_CONFIG_HOME="$work_dir/config" XDG_CACHE_HOME="$work_dir/cache" \
    XDG_DATA_HOME="$work_dir/data" XDG_RUNTIME_DIR="$work_dir/runtime" \
    FLAMESHOT_DEBUG_SAMPLER="${FLAMESHOT_DEBUG_SAMPLER:-}" \
    FLAMESHOT_DEBUG_ANNOTATIONS="${FLAMESHOT_DEBUG_ANNOTATIONS:-}" \
    FLAMESHOT_DEBUG_COPY="${FLAMESHOT_DEBUG_COPY:-}" \
    PATH=/usr/bin:/bin QT_QPA_PLATFORM=xcb \
    "$out_dir/capturewidget_test"
