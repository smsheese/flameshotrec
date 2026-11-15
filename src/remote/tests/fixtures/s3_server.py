#!/usr/bin/env python3
"""Loopback S3-compatible PUT endpoint that *independently* verifies SigV4.

This is a second implementation of AWS Signature Version 4 (stdlib only, no
shared code with src/remote/s3signer.cpp), so the C++ signer is checked against
a different implementation rather than against itself.

Usage: s3_server.py <object-dir> <port-file> <result-json> <access-key> <secret-key>
                    [mode-file] [--ipv6]
Result JSON: {"requests": [ {"key","verified","method","raw_path","if_none_match",
                             "token","status","bytes"} ]}

`mode-file` holds a single word read before every request:
  normal     store the object (default)
  redirect301 / redirect307   answer with a redirect and store nothing
  always412  refuse as if the object existed
A conditional create (If-None-Match: *) makes an existing object a 412.
"""
import hashlib
import hmac
import json
import os
import socket
import sys
import threading
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

OBJECT_DIR = sys.argv[1]
RESULT_JSON = sys.argv[3]
ACCESS_KEY = sys.argv[4]
SECRET_KEY = sys.argv[5]
MODE_FILE = sys.argv[6] if len(sys.argv) > 6 and not sys.argv[6].startswith("--") else ""
USE_IPV6 = "--ipv6" in sys.argv
LOCK = threading.Lock()
LAST_CANONICAL = ""
REQUESTS = []


def uri_encode(value, keep_slash=False):
    out = []
    for char in value:
        if (char.isalnum() and ord(char) < 128) or char in "-_.~" or (keep_slash and char == "/"):
            out.append(char)
        else:
            for byte in char.encode("utf-8"):
                out.append(f"%{byte:02X}")
    return "".join(out)


def sign(key, message):
    return hmac.new(key, message.encode(), hashlib.sha256).digest()


def expected_signature(handler, body):
    raw_path = handler.path.split("?", 1)[0]
    # The client already percent-encodes the key. Decode first, then canonicalise,
    # so the comparison does not double-encode (%20 -> %2520).
    # Keep empty segments: a repeated slash is part of the object key and must
    # not be normalised away by the verifier.
    segments = [urllib.parse.unquote(s) for s in raw_path.split("/")]
    if segments and segments[0] == "":
        segments = segments[1:]
    canonical_uri = "/" + "/".join(uri_encode(s) for s in segments)
    headers = {
        "host": handler.headers.get("Host", ""),
        "x-amz-content-sha256": handler.headers.get("x-amz-content-sha256", ""),
        "x-amz-date": handler.headers.get("x-amz-date", ""),
    }
    token = handler.headers.get("x-amz-security-token")
    if token:
        headers["x-amz-security-token"] = token
    content_type = handler.headers.get("Content-Type")
    if content_type:
        headers["content-type"] = content_type.strip()
    # A conditional create signs this header, so it belongs in the canonical request.
    if_none_match = handler.headers.get("If-None-Match")
    if if_none_match:
        headers["if-none-match"] = if_none_match.strip()
    signed_headers = ";".join(sorted(headers))
    canonical_headers = "".join(f"{name}:{headers[name]}\n" for name in sorted(headers))
    payload_hash = hashlib.sha256(body).hexdigest()
    canonical_request = "\n".join(
        ["PUT", canonical_uri, "", canonical_headers, signed_headers, payload_hash]
    )
    global LAST_CANONICAL
    LAST_CANONICAL = canonical_request
    amz_date = headers["x-amz-date"]
    scope = f"{amz_date[:8]}/eu-west-1/s3/aws4_request"
    string_to_sign = "\n".join(
        ["AWS4-HMAC-SHA256", amz_date, scope, hashlib.sha256(canonical_request.encode()).hexdigest()]
    )
    key = sign(("AWS4" + SECRET_KEY).encode(), amz_date[:8])
    key = sign(key, "eu-west-1")
    key = sign(key, "s3")
    key = sign(key, "aws4_request")
    return hmac.new(key, string_to_sign.encode(), hashlib.sha256).hexdigest()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, format, *args):  # noqa: A002 - Qt/base signature
        pass

    def do_PUT(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length) if length else b""
        authorization = self.headers.get("Authorization", "")
        received = ""
        if "Signature=" in authorization:
            received = authorization.rsplit("Signature=", 1)[1].strip()
        try:
            expected = expected_signature(self, body)
        except Exception as error:  # noqa: BLE001 - report, do not crash the fixture
            expected = f"error: {error}"
        mode = current_mode()
        if mode in ("redirect301", "redirect307"):
            # Nothing is stored, and the client must not follow this.
            record = {
                "key": self.path.lstrip("/"),
                "verified": False,
                "method": "PUT",
                "raw_path": self.path,
                "if_none_match": self.headers.get("If-None-Match", ""),
                "token": self.headers.get("x-amz-security-token", ""),
                "status": 301 if mode == "redirect301" else 307,
                "bytes": len(body),
            }
            with LOCK:
                REQUESTS.append(record)
                with open(RESULT_JSON, "w") as handle:
                    json.dump({"requests": REQUESTS}, handle)
            self.send_response(record["status"])
            self.send_header("Location", "http://127.0.0.1:1/elsewhere")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        key = self.path.lstrip("/")
        safe = key.replace("/", "_").replace("..", "_") or "unnamed"
        target = os.path.join(OBJECT_DIR, safe if safe else "unnamed")
        conditional = self.headers.get("If-None-Match", "") == "*"
        if (mode == "always412") or (conditional and os.path.exists(target)):
            # Conditional create: refuse rather than replace.
            record = {
                "key": key,
                "verified": False,
                "method": "PUT",
                "raw_path": self.path,
                "if_none_match": self.headers.get("If-None-Match", ""),
                "token": self.headers.get("x-amz-security-token", ""),
                "status": 412,
                "bytes": len(body),
            }
            with LOCK:
                REQUESTS.append(record)
                with open(RESULT_JSON, "w") as handle:
                    json.dump({"requests": REQUESTS}, handle)
            self.send_response(412)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        with open(target, "wb") as handle:
            handle.write(body)
        record = {
            "canonical": globals().get("LAST_CANONICAL", ""),
            "sent_headers": dict(self.headers.items()),
            "method": "PUT",
            "raw_path": self.path,
            "if_none_match": self.headers.get("If-None-Match", ""),
            "token": self.headers.get("x-amz-security-token", ""),
            "status": 200,
            "key": key,
            "verified": received == expected and expected != "",
            "received": received,
            "expected": expected,
            "bytes": len(body),
            "credential_ok": ACCESS_KEY in authorization,
        }
        with LOCK:
            REQUESTS.append(record)
            with open(RESULT_JSON, "w") as handle:
                json.dump({"requests": REQUESTS}, handle)
        self.send_response(200)
        self.send_header("ETag", f'"{hashlib.sha256(body).hexdigest()}"')
        self.send_header("Content-Length", "0")
        self.end_headers()


def current_mode():
    if not MODE_FILE or not os.path.exists(MODE_FILE):
        return "normal"
    with open(MODE_FILE) as handle:
        return handle.read().strip() or "normal"


def main():
    os.makedirs(OBJECT_DIR, exist_ok=True)
    if USE_IPV6:
        class V6Server(ThreadingHTTPServer):
            address_family = socket.AF_INET6

        server = V6Server(("::1", 0), Handler)
    else:
        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    with open(sys.argv[2], "w") as handle:
        handle.write(str(server.server_address[1]))
    with open(RESULT_JSON, "w") as handle:
        json.dump({"requests": []}, handle)
    server.serve_forever()


if __name__ == "__main__":
    main()
