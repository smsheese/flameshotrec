#!/usr/bin/env python3
"""Minimal FTP server for the remote-storage tests.

Loopback only. Supports the subset curl needs for an upload (USER/PASS/PWD/PASV/
TYPE I/STOR/QUIT) and stores what it receives. Not a general FTP server.

Usage: ftp_server.py <upload-dir> <port-file> [user password] [mode-file]
The chosen port is written to <port-file> once the socket is bound.

The optional mode-file holds one word, read before every command:
  normal    behave normally (default)
  nosize    answer SIZE with 500, i.e. the server has no size support
  slowstor  delay the STOR data phase, so a client can be cancelled mid-upload
"""
import os
import socket
import sys
import threading
import time


def current_mode(mode_file):
    if not mode_file or not os.path.exists(mode_file):
        return "normal"
    with open(mode_file) as handle:
        return handle.read().strip() or "normal"


class Session(threading.Thread):
    def __init__(self, conn, upload_dir, credentials, log, mode_file=""):
        super().__init__(daemon=True)
        self.conn = conn
        self.upload_dir = upload_dir
        self.credentials = credentials
        self.log = log
        self.mode_file = mode_file
        self.data_listener = None
        self.pending_rename = None

    def send(self, text):
        self.conn.sendall((text + "\r\n").encode())

    def open_passive(self):
        if self.data_listener is not None:
            self.data_listener.close()
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        self.data_listener = listener
        port = listener.getsockname()[1]
        self.send(f"227 Entering Passive Mode (127,0,0,1,{port >> 8},{port & 0xFF})")

    def run(self):
        self.send("220 flameshot-test-fixture ready")
        user = None
        buffer = b""
        try:
            while True:
                chunk = self.conn.recv(4096)
                if not chunk:
                    return
                buffer += chunk
                while b"\r\n" in buffer:
                    line, buffer = buffer.split(b"\r\n", 1)
                    command = line.decode("utf-8", "replace")
                    self.log.append(command.split(" ")[0])
                    verb, _, argument = command.partition(" ")
                    verb = verb.upper()
                    if verb == "USER":
                        user = argument
                        self.send(f"331 password required for {argument}")
                    elif verb == "PASS":
                        ok = (user, argument) == self.credentials
                        self.send("230 login ok" if ok else "530 authentication failed")
                    elif verb == "PWD":
                        self.send('257 "/" is the current directory')
                    elif verb == "TYPE":
                        self.send("200 type set")
                    elif verb in ("CWD", "XCWD"):
                        self.send("250 directory changed")
                    elif verb in ("SIZE", "MDTM"):
                        # What `curl --head` asks to read a stored size back.
                        if verb == "SIZE" and current_mode(self.mode_file) == "nosize":
                            self.send("500 SIZE not understood")
                            continue
                        name = os.path.basename(argument.replace("\\", "/"))
                        candidate = os.path.join(self.upload_dir, name)
                        if os.path.isfile(candidate):
                            if verb == "SIZE":
                                self.send(f"213 {os.path.getsize(candidate)}")
                            else:
                                self.send("213 20260101000000")
                        else:
                            self.send("550 no such file")
                    elif verb == "EPSV":
                        # Force the client onto PASV so the data port is simple.
                        self.send("500 EPSV not understood")
                    elif verb == "PASV":
                        self.open_passive()
                    elif verb == "STOR":
                        if self.data_listener is None:
                            self.send("425 use PASV first")
                            continue
                        self.send("150 ok to send data")
                        if current_mode(self.mode_file) == "slowstor":
                            # Hold the data phase open so a test can cancel here.
                            time.sleep(4)
                        data_conn, _ = self.data_listener.accept()
                        data_conn.settimeout(10)
                        payload = b""
                        while True:
                            piece = data_conn.recv(65536)
                            if not piece:
                                break
                            payload += piece
                        data_conn.close()
                        self.data_listener.close()
                        self.data_listener = None
                        name = os.path.basename(argument.replace("\\", "/"))
                        # Keep the real name, including a leading dot: the client
                        # uploads to a dot-prefixed transfer temporary and reads
                        # its size back by that exact name.
                        if not name or name in (".", ".."):
                            name = "unnamed"
                        target = os.path.join(self.upload_dir, name)
                        with open(target, "wb") as handle:
                            handle.write(payload)
                        self.send("226 transfer complete")
                    elif verb == "RNFR":
                        # Publication for this client is a rename of its own
                        # per-transfer temporary; the final name never receives a
                        # partially written file.
                        name = os.path.basename(argument.replace("\\", "/"))
                        candidate = os.path.join(self.upload_dir, name)
                        if os.path.isfile(candidate):
                            self.pending_rename = candidate
                            self.send("350 ready for destination name")
                        else:
                            self.send("550 no such file")
                    elif verb == "RNTO":
                        name = os.path.basename(argument.replace("\\", "/"))
                        if not self.pending_rename:
                            self.send("503 RNFR required first")
                        else:
                            target = os.path.join(self.upload_dir, name)
                            try:
                                os.replace(self.pending_rename, target)
                                self.send("250 rename ok")
                            except OSError:
                                self.send("550 rename failed")
                            self.pending_rename = None
                    elif verb in ("LIST", "NLST"):
                        if self.data_listener is None:
                            self.send("425 use PASV first")
                            continue
                        self.send("150 sending listing")
                        data_conn, _ = self.data_listener.accept()
                        listing = b"\r\n".join(
                            entry.encode() for entry in sorted(os.listdir(self.upload_dir))
                        )
                        try:
                            data_conn.sendall(listing + b"\r\n")
                        finally:
                            data_conn.close()
                            self.data_listener.close()
                            self.data_listener = None
                        self.send("226 listing complete")
                    elif verb == "QUIT":
                        self.send("221 bye")
                        return
                    else:
                        self.send("502 command not implemented")
        finally:
            if self.data_listener is not None:
                self.data_listener.close()
            self.conn.close()

def main():
    upload_dir, port_file = sys.argv[1], sys.argv[2]
    if len(sys.argv) >= 5:
        credentials = (sys.argv[3], sys.argv[4])
    else:
        credentials = ("fixture", "fixture-password")
    mode_file = sys.argv[5] if len(sys.argv) > 5 else ""
    os.makedirs(upload_dir, exist_ok=True)
    log = []
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("127.0.0.1", 0))
    server.listen(8)
    with open(port_file, "w") as handle:
        handle.write(str(server.getsockname()[1]))
    while True:
        conn, _ = server.accept()
        Session(conn, upload_dir, credentials, log, mode_file).start()

if __name__ == "__main__":
    main()
