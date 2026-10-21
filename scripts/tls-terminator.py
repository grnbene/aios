#!/usr/bin/env python3
"""Plaintext HTTP front door onto an HTTPS chain endpoint.

Decision D3 option (b) in docs/TASK-014-chain-wire.md: terminate TLS out here and
let the kernel's own HTTP/1.1 client speak plaintext inward. Everything on the
AIOS side of this line — request framing, headers, content-length and chunked
bodies, the document codec — is then exercised against a real node rather than
against a stub.

    scripts/tls-terminator.py https://eth.drpc.org 8545
"""
import http.server
import json
import ssl
import sys
import urllib.error
import urllib.request

UPSTREAM = sys.argv[1] if len(sys.argv) > 1 else "https://eth.drpc.org"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8545


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def _relay(self, body, path, content_type):
        # The path matters for REST endpoints and is harmless for a JSON-RPC root,
        # so it is always carried through rather than dropped for POSTs.
        target = UPSTREAM.rstrip("/") + (path if path and path != "/" else "/")
        # Several public endpoints reject urllib's default identification with a
        # 403, which has nothing to do with the request and everything to do with
        # the header, so this one is set explicitly.
        req = urllib.request.Request(
            target,
            data=body or None,
            headers={"Content-Type": content_type, "User-Agent": "curl/8.7.1"},
        )
        try:
            with urllib.request.urlopen(req, timeout=25, context=ssl.create_default_context()) as r:
                return r.read(), r.status
        except urllib.error.HTTPError as exc:
            # A node's rejection is the answer, not a transport failure: pass the
            # body straight through so the caller reads what the node said.
            return exc.read(), exc.code
        except Exception as exc:
            return json.dumps({"error": {"message": str(exc)}}).encode(), 502

    def _answer(self, payload, status):
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        payload, status = self._relay(
            self.rfile.read(length), self.path, self.headers.get("Content-Type", "application/json")
        )
        self._answer(payload, status)

    def do_GET(self):
        payload, status = self._relay(b"", self.path, "application/json")
        self._answer(payload, status)


if __name__ == "__main__":
    print(f"terminating TLS for {UPSTREAM} on 127.0.0.1:{PORT}", flush=True)
    http.server.ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
