#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Flameshot IA Contributors
#
# Builds and runs the AI flow regression harness (tests/ai/ai_flow_test.cpp).
#
# Usage:
#   tests/ai/run.sh [BUILD_DIR]
#
# BUILD_DIR must be a configured and built Flameshot tree (it defaults to
# $FLAMESHOT_BUILD_DIR, then the in-repo build_user, per AGENTS.md rule 4). The harness reuses
# that tree's compiler flags and link line, recompiling only the AI sources it
# covers, so it is a targeted check instead of a second project build.
#
# The harness never captures the desktop, never opens a microphone and never
# talks to a real API: it paints its own snapshot, uses a throw-away
# XDG_CONFIG_HOME and a stub OpenAI endpoint on 127.0.0.1.
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
build_dir=${1:-${FLAMESHOT_BUILD_DIR:-$repo_root/build_user}}
out_dir=${FLAMESHOT_AI_HARNESS_DIR:-$build_dir/ai-harness}

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
    raise SystemExit("error: no compile command for " + suffix)


def compile_source(source, suffix, output):
    command = compiler_flags(suffix)
    print("compiling", source)
    subprocess.run(
        command + ["-c", os.path.join(repo_root, source), "-o", output],
        check=True,
    )


compile_source(
    "src/widgets/ai/aidialog.cpp",
    "tools/ai/aitool.cpp",
    os.path.join(out_dir, "aidialog.cpp.o"),
)
compile_source(
    "src/tools/ai/aitool.cpp",
    "tools/ai/aitool.cpp",
    os.path.join(out_dir, "aitool.cpp.o"),
)
compile_source(
    "src/core/flameshot.cpp",
    "core/flameshot.cpp",
    os.path.join(out_dir, "flameshot.cpp.o"),
)
compile_source(
    "tests/ai/ai_flow_test.cpp",
    "core/flameshot.cpp",
    os.path.join(out_dir, "ai_flow_test.cpp.o"),
)

with open(os.path.join(build_dir, "src/CMakeFiles/flameshot.dir/link.txt")) as handle:
    link = shlex.split(handle.read())

replaced = {
    "CMakeFiles/flameshot.dir/main.cpp.o",
    "CMakeFiles/flameshot.dir/tools/ai/aitool.cpp.o",
    "CMakeFiles/flameshot.dir/widgets/ai/aidialog.cpp.o",
    "CMakeFiles/flameshot.dir/core/flameshot.cpp.o",
}
objects = [
    os.path.join(out_dir, name)
    for name in (
        "aidialog.cpp.o",
        "aitool.cpp.o",
        "flameshot.cpp.o",
        "ai_flow_test.cpp.o",
    )
]

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
    if token in replaced:
        continue
    command.append(token)

binary = os.path.join(out_dir, "ai_flow_test")
command += objects + ["-o", binary]
print("linking", binary)
# The link line uses paths relative to <build>/src, where CMake runs it.
subprocess.run(command, cwd=os.path.join(build_dir, "src"), check=True)
PYTHON

config_home="$out_dir/config"
runtime_home="$out_dir/runtime"
rm -rf "$config_home" "$runtime_home"
mkdir -p "$config_home" "$runtime_home"
chmod 700 "$runtime_home"

echo "running the AI flow harness"
cd "$out_dir"
QT_QPA_PLATFORM=offscreen \
XDG_CONFIG_HOME="$config_home" \
XDG_CACHE_HOME="$out_dir/cache" \
XDG_DATA_HOME="$out_dir/data" \
XDG_RUNTIME_DIR="$runtime_home" \
    "$out_dir/ai_flow_test"
