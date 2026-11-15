#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Flameshot Contributors
#
# Builds and runs the Video tab "Annotation tools" check
# (tests/config/video_tools_test.cpp).
#
# Usage:
#   tests/config/run.sh [BUILD_DIR]
#
# BUILD_DIR is a configured and built Flameshot tree (default: the in-repo
# build_user, per AGENTS.md rule 4). The harness reuses that tree's compiler
# flags and link line, so it recompiles only its own sources: it is a targeted
# check, not a second project build.
#
# Everything runs on a *nested* X server with a throw-away configuration
# directory: the real configuration, the real desktop and the real sound server
# are never touched (AGENTS.md rule 1).
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
build_dir=${1:-${FLAMESHOT_BUILD_DIR:-$repo_root/build_user}}
out_dir=${FLAMESHOT_CONFIG_TEST_DIR:-$build_dir/config-tests}
display=${FLAMESHOT_TEST_DISPLAY:-:95}
work_dir=$out_dir/run

if [[ ! -f "$build_dir/compile_commands.json" ]]; then
    echo "error: $build_dir has no compile_commands.json" >&2
    echo "       configure and build Flameshot there first" >&2
    exit 2
fi
for required in \
    "$build_dir/src/CMakeFiles/flameshot.dir/link.txt" \
    "$build_dir/src/CMakeFiles/flameshot.dir/config/videoconf.cpp.o" \
    "$build_dir/src/recording/CMakeFiles/flameshot-recording.dir/annotationpolicy.cpp.o"; do
    if [[ ! -f "$required" ]]; then
        echo "error: missing $required" >&2
        echo "       build the flameshot target in $build_dir first" >&2
        exit 2
    fi
done

mkdir -p "$out_dir"

python3 - "$repo_root" "$build_dir" "$out_dir" <<'PYTHON'
import json
import os
import shlex
import subprocess
import sys

repo_root, build_dir, out_dir = sys.argv[1:4]
moc = os.environ.get("FLAMESHOT_MOC", "/usr/lib/qt6/libexec/moc")

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
    raise SystemExit("error: no compile command for " + suffix)


qt_test_include = None
flags = compiler_flags("config/videoconf.cpp")
for index, token in enumerate(flags):
    if token == "-isystem" and flags[index + 1].endswith("/qt6/QtCore"):
        qt_test_include = flags[index + 1][: -len("QtCore")] + "QtTest"
if qt_test_include is None:
    raise SystemExit("error: cannot locate the Qt include root")

source = os.path.join(repo_root, "tests/config/video_tools_test.cpp")
# The test is a .cpp with Q_OBJECT that ends in `#include "video_tools_test.moc"`,
# which is the pattern CMake's AUTOMOC generates for a source file. moc therefore
# has to write *exactly* that include target, and the result must not be
# compiled as a second translation unit: it is included by the source. Writing it
# under another name (the previous `video_tools_test_moc.cpp`) broke both halves
# at once -- the source could not find its `.moc` and the metaobject code was
# compiled twice.
moc_source = os.path.join(out_dir, "video_tools_test.moc")
subprocess.run([moc, source, "-o", moc_source], check=True)
if not os.path.exists(moc_source) or os.path.getsize(moc_source) == 0:
    raise SystemExit("error: moc produced no output for " + source)

common = flags + ["-isystem", qt_test_include, "-g", "-I", out_dir]
object_path = os.path.join(out_dir, "video_tools_test.cpp.o")
print("compiling", source)
subprocess.run(common + ["-c", source, "-o", object_path], check=True)
objects = [object_path]

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
    if (token.startswith(prefix) and token.endswith(".o") and
            "autogen" not in token):
        # The shared build directory can outlive a source that another agent
        # deleted, or contain a link entry whose source moved into a library.
        # Linking such an object fails for a reason this check must not present
        # as a test failure; a genuinely missing symbol still fails below.
        source_file = os.path.join(repo_root, "src", token[len(prefix):-2])
        object_file = os.path.join(build_dir, "src", token)
        if not os.path.exists(source_file):
            print("skipping stale object (source is gone):", token)
            continue
        if not os.path.exists(object_file):
            print("skipping unbuilt object (source lives in a library):", token)
            continue
    command.append(token)

binary = os.path.join(out_dir, "video_tools_test")
# The harness objects have to come before the static archives: an object after
# libflameshot-recording.a can no longer pull symbols out of it.
position = 0
for index, token in enumerate(command):
    if token.startswith("-l") or token.endswith(".a") or token.endswith(".so"):
        break
    position = index + 1
command = command[:position] + objects + command[position:] + ["-o", binary, "-lQt6Test"]
print("linking", binary)
subprocess.run(command, cwd=os.path.join(build_dir, "src"), check=True)
PYTHON

rm -rf "$work_dir"
mkdir -p "$work_dir/config/flameshot" "$work_dir/home" "$work_dir/runtime" \
         "$work_dir/videos"
chmod 700 "$work_dir/runtime"

# A complete, valid configuration: no unrecognised key, so the configuration is
# healthy before the tab is opened. That is the precondition the check needs.
cat > "$work_dir/config/flameshot/flameshot.ini" <<EOF
[General]
savePath=$work_dir/home
savePathFixed=false
recordingFfmpegPath=
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
drawColor=#ff0000
uiColor=#740096
contrastUiColor=#270032
EOF

cleanup() {
    if [[ -n "${xephyr_pid:-}" ]]; then
        kill "$xephyr_pid" 2>/dev/null || true
        wait "$xephyr_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT

rm -f "/tmp/.X11-unix/X${display#:}" "/tmp/.X${display#:}-lock"
Xephyr "$display" -screen 1024x768 -nolisten tcp -noreset \
    > "$out_dir/xephyr.log" 2>&1 &
xephyr_pid=$!
for _ in $(seq 1 100); do
    DISPLAY=$display xdpyinfo > /dev/null 2>&1 && break
    sleep 0.1
done
if ! DISPLAY=$display xdpyinfo > /dev/null 2>&1; then
    echo "error: nested X server $display did not start" >&2
    cat "$out_dir/xephyr.log" >&2
    exit 2
fi

echo "running the Video tab annotation-tools check"
status=0
env -i DISPLAY=$display HOME="$work_dir/home" \
    XDG_CONFIG_HOME="$work_dir/config" XDG_CACHE_HOME="$work_dir/cache" \
    XDG_DATA_HOME="$work_dir/data" XDG_RUNTIME_DIR="$work_dir/runtime" \
    GSETTINGS_BACKEND=memory PATH=/usr/bin:/bin QT_QPA_PLATFORM=xcb \
    "$out_dir/video_tools_test" || status=$?

echo
echo "configuration keys after the run (a 'annotation' key here means the UI"
echo "wrote a per-tool setting through ConfigHandler):"
grep -E '^(annotation|recording)' "$work_dir/config/flameshot/flameshot.ini" \
    | sed 's/^/      /' || true
exit $status
