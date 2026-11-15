#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Flameshot Contributors
#
# Runs the remote-storage suites against loopback fixtures.
#
#   src/remote/tests/run.sh [BUILD_DIR]
#
# BUILD_DIR is a configured Flameshot tree (default: the in-repo build_user, per
# AGENTS.md rule 4). Only the two remote targets are built, with -j2.
#
# Nothing leaves the machine: the FTP and S3 endpoints are Python fixtures bound
# to 127.0.0.1 on ephemeral ports, and ssh/sftp are exercised through PATH shims.
# Qt runs offscreen, so no window ever appears on the live display.
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
build_dir=${1:-${FLAMESHOT_BUILD_DIR:-$repo_root/build_user}}
work_dir=${FLAMESHOT_REMOTE_TEST_DIR:-$build_dir/remote-tests}
fixtures=$repo_root/src/remote/tests/fixtures

if [[ ! -f "$build_dir/CMakeCache.txt" ]]; then
    echo "error: $build_dir is not a configured build tree" >&2
    exit 2
fi

mkdir -p "$work_dir"
rm -f "$work_dir"/*.port "$work_dir"/s3.json
rm -rf "$work_dir/ftp-inbox" "$work_dir/s3-objects" "$work_dir/s3-objects6" \
       "$work_dir/sshd/remote"
mkdir -p "$work_dir/ftp-inbox" "$work_dir/s3-objects"

jobs=${FLAMESHOT_BUILD_JOBS:-2}   # AGENTS.md rule 2: never saturate the laptop
cmake --build "$build_dir" --target remote-storage-test remote-upload-test -j "$jobs"

pids=()
cleanup() {
    "$fixtures/sshd_fixture.sh" stop "$work_dir/sshd" >/dev/null 2>&1 || true
    for pid in "${pids[@]:-}"; do
        [[ -n "$pid" ]] && kill "$pid" 2>/dev/null || true
    done
    wait 2>/dev/null || true
}
trap cleanup EXIT

rm -f "$work_dir/ftp.mode" "$work_dir/s3.mode" "$work_dir/s3v6.mode"
python3 "$fixtures/ftp_server.py" "$work_dir/ftp-inbox" "$work_dir/ftp.port" \
    fixture fixture-password "$work_dir/ftp.mode" >"$work_dir/ftp.log" 2>&1 &
pids+=("$!")

python3 "$fixtures/s3_server.py" "$work_dir/s3-objects" "$work_dir/s3.port" \
    "$work_dir/s3.json" AKIDEXAMPLE 'wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY' \
    "$work_dir/s3.mode" >"$work_dir/s3.log" 2>&1 &
pids+=("$!")

# IPv6 loopback endpoint (skipped silently when the host has no ::1).
if python3 -c 'import socket; s=socket.socket(socket.AF_INET6); s.bind(("::1",0)); s.close()' 2>/dev/null; then
    python3 "$fixtures/s3_server.py" "$work_dir/s3-objects6" "$work_dir/s3v6.port" \
        "$work_dir/s3v6.json" AKIDEXAMPLE 'wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY' \
        "$work_dir/s3v6.mode" --ipv6 >"$work_dir/s3v6.log" 2>&1 &
    pids+=("$!")
fi

# Local-only OpenSSH server (own keys, 127.0.0.1, ephemeral port).
if port=$("$fixtures/sshd_fixture.sh" start "$work_dir/sshd" 2>"$work_dir/sshd.setup.log"); then
    export FLAMESHOT_TEST_SSHD_PORT="$port"
    export FLAMESHOT_TEST_SSHD_DIR="$work_dir/sshd"
    echo "fixtures: OpenSSH sftp/ssh on 127.0.0.1:$port (real server)"
else
    # Loud, not silent: the SFTP/SSH wire cases would otherwise SKIP and look green.
    echo "WARNING: the OpenSSH fixture did not start; SFTP/SSH wire tests WILL SKIP" >&2
    echo "         see $work_dir/sshd.setup.log" >&2
fi

for file in ftp.port s3.port; do
    for _ in $(seq 1 100); do
        [[ -s "$work_dir/$file" ]] && break
        sleep 0.1
    done
    if [[ ! -s "$work_dir/$file" ]]; then
        echo "error: fixture did not start ($file); see $work_dir/*.log" >&2
        exit 1
    fi
done

export FLAMESHOT_TEST_FTP_PORT="$(cat "$work_dir/ftp.port")"
export FLAMESHOT_TEST_FTP_DIR="$work_dir/ftp-inbox"
export FLAMESHOT_TEST_FTP_MODE="$work_dir/ftp.mode"
export FLAMESHOT_TEST_S3_PORT="$(cat "$work_dir/s3.port")"
export FLAMESHOT_TEST_S3_RESULT="$work_dir/s3.json"
export FLAMESHOT_TEST_S3_MODE="$work_dir/s3.mode"
if [[ -s "$work_dir/s3v6.port" ]]; then
    export FLAMESHOT_TEST_S3_V6_PORT="$(cat "$work_dir/s3v6.port")"
    export FLAMESHOT_TEST_S3_V6_RESULT="$work_dir/s3v6.json"
fi
export QT_QPA_PLATFORM=offscreen
# Keep the throwaway configuration away from the real one.
export XDG_CONFIG_HOME="$work_dir/config"
export XDG_CACHE_HOME="$work_dir/cache"
mkdir -p "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME"

status=0
"$build_dir/src/remote/remote-storage-test" "$@" || status=$?
"$build_dir/src/remote/remote-upload-test" "$@" 2>&1 | tee "$work_dir/last-run.log" || status=$?

skipped=$(grep -c "^SKIP" "$work_dir"/last-run.log 2>/dev/null || true)
if [[ $status -eq 0 ]]; then
    echo "remote storage suites: PASS (fixture logs in $work_dir; skipped=$skipped)"
    if [[ "${skipped:-0}" != "0" ]]; then
        echo "note: $skipped case(s) skipped - see the SKIP lines below"
    fi
fi
exit $status
