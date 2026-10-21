import http.server, sys, os
class H(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0))).decode()
        mode = open("/tmp/alert-recv.mode").read().strip() if os.path.exists("/tmp/alert-recv.mode") else "ok"
        code = 200 if mode == "ok" else 500
        with open("/tmp/alert-recv.log", "a") as f: f.write("%d %s %s\n" % (code, self.path, body))
        self.send_response(code); self.send_header("Content-Length", "2"); self.end_headers(); self.wfile.write(b"ok")
    def log_message(self, *a): pass
http.server.HTTPServer((sys.argv[2] if len(sys.argv) > 2 else "127.0.0.1", int(sys.argv[1])), H).serve_forever()
