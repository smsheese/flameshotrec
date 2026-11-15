#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Flameshot Contributors
#
# Local-only OpenSSH server fixture for the remote-storage tests.
#
#   sshd_fixture.sh start  <workdir>   # prints the port on stdout
#   sshd_fixture.sh stop   <workdir>
#
# Constraints, by design:
#   * nothing is installed system-wide: a pre-split openssh-server package
#     (9.x, single sshd binary) is downloaded and extracted under <workdir>;
#   * the server listens on 127.0.0.1 only, on an ephemeral high port;
#   * the host key, the client key and every known_hosts file are generated here,
#     never the user's real keys;
#   * sshd runs as the current (unprivileged) user and serves only that user.
#
# OpenSSH 9.8+ splits out sshd-session and looks for it at a hard-coded /usr/lib
# path, which needs root, so the fixture deliberately stays on 9.x.
set -euo pipefail

action=${1:-start}
work=${2:-}
if [[ -z "$work" ]]; then
    echo "usage: sshd_fixture.sh {start|stop} <workdir>" >&2
    exit 2
fi
mkdir -p "$work"
work=$(cd "$work" && pwd)
port_file=$work/sshd.port
pid_file=$work/sshd.pid

deb_url="http://deb.debian.org/debian/pool/main/o/openssh/openssh-server_9.2p1-2+deb12u10_amd64.deb"
deb_name=$(basename "$deb_url")

stop_server() {
    if [[ -f "$pid_file" ]]; then
        kill "$(cat "$pid_file")" 2>/dev/null || true
        rm -f "$pid_file"
    fi
    pkill -f "$work/root/usr/sbin/sshd" 2>/dev/null || true
}

if [[ "$action" == "stop" ]]; then
    stop_server
    exit 0
fi

if [[ ! -x "$work/root/usr/sbin/sshd" ]]; then
    mkdir -p "$work/download"
    if [[ ! -f "$work/download/$deb_name" ]]; then
        curl -sfL -o "$work/download/$deb_name" "$deb_url" \
            || { echo "error: could not download $deb_name" >&2; exit 1; }
    fi
    rm -rf "$work/root"
    dpkg-deb -x "$work/download/$deb_name" "$work/root"
fi

mkdir -p "$work/keys"
rm -rf "$work/remote"
mkdir -p "$work/remote"
if [[ ! -f "$work/keys/host_key" ]]; then
    ssh-keygen -q -t ed25519 -N '' -f "$work/keys/host_key"
    ssh-keygen -q -t ed25519 -N '' -f "$work/keys/client_key"
    # A second, unrelated key: it stands in for a host key trusted elsewhere.
    ssh-keygen -q -t ed25519 -N '' -f "$work/keys/other_key"
fi

stop_server
port=$(python3 - <<'PY'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
)

cat > "$work/sshd_config" <<EOF
Port $port
ListenAddress 127.0.0.1
HostKey $work/keys/host_key
PidFile $pid_file
AuthorizedKeysFile $work/keys/client_key.pub
PasswordAuthentication no
KbdInteractiveAuthentication no
PubkeyAuthentication yes
PermitRootLogin no
StrictModes no
UsePAM no
Subsystem sftp internal-sftp
LogLevel ERROR
EOF

"$work/root/usr/sbin/sshd" -f "$work/sshd_config" -E "$work/sshd.log"

for _ in $(seq 1 100); do
    if (exec 3<>/dev/tcp/127.0.0.1/"$port") 2>/dev/null; then
        exec 3>&-
        break
    fi
    sleep 0.1
done

# Trust files: the correct key, an unrelated key, and a "global alternative"
# that contains the correct key under a name the fixture never selects. The last
# one exists so a test can prove the global database is ignored.
ssh-keyscan -p "$port" -t ed25519 127.0.0.1 2>/dev/null | grep -v '^#' > "$work/known_hosts"
printf '[127.0.0.1]:%s %s\n' "$port" "$(cut -d' ' -f1,2 "$work/keys/other_key.pub")" \
    > "$work/known_hosts_wrong"
cp "$work/known_hosts" "$work/known_hosts_alternate"

echo "$port" > "$port_file"
echo "$port"
