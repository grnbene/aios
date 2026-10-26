# A test server for tests/grain/test_http_gzip.gr (run by scripts/grain-net-check.sh): python3 gzip_server.py PORTFILE
#   /plain    - "plain body", never encoded
#   /gz       - a JSON body, gzip-encoded only when the request's Accept-Encoding allows gzip; the body says whether
#               the client asked (so the test sees the header it sent)
#   /bomb     - 64 MB of zeros, gzip-encoded (~64 KB on the wire)
#   /badgz    - Content-Encoding: gzip with a body that is not gzip
import gzip, http.server, json, sys

class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def send(self, body, enc=None, ctype="application/json"):
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        if enc: self.send_header("Content-Encoding", enc)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def do_GET(self):
        ae = self.headers.get("Accept-Encoding", "")
        if self.path == "/plain":
            self.send(b"plain body", ctype="text/plain")
        elif self.path == "/gz":
            body = json.dumps({"asked": ae, "items": list(range(500))}).encode()
            if "gzip" in ae: self.send(gzip.compress(body), "gzip")
            else: self.send(body)
        elif self.path == "/bomb":
            self.send(gzip.compress(bytes(64 << 20), 9), "gzip", "application/octet-stream")
        elif self.path == "/badgz":
            self.send(b"this is not gzip", "gzip", "text/plain")
        else:
            self.send_response(404); self.end_headers()

srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
open(sys.argv[1], "w").write(str(srv.server_address[1]))
srv.serve_forever()
