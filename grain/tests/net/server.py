"""The HTTP server grain's client test talks to (grain/tests/net/http_local.gr).

    python3 server.py <port file>

Binds 127.0.0.1 on a free port, writes the port to <port file> once it is
listening, and serves until a GET /quit. Each path answers a fixed way
(redirect chains, chunked bodies, POST echo) so the client's output is the
same on every machine.
"""
import http.server, os, sys, threading
class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def reply(self, code, body, extra=()):
        b = body.encode()
        self.send_response(code)
        for k, v in extra: self.send_header(k, v)
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)
    def do_GET(self):
        if self.path == "/hello": self.reply(200, "hello, " + self.headers.get("User-Agent", "?"), [("X-Test", "a"), ("X-Test", "b")])
        elif self.path == "/r1": self.reply(302, "", [("Location", "/r2")])
        elif self.path == "/r2": self.reply(301, "", [("Location", "hello")])
        elif self.path == "/loop": self.reply(302, "", [("Location", "/loop")])
        elif self.path == "/chunked":
            self.send_response(200); self.send_header("Transfer-Encoding", "chunked"); self.end_headers()
            for part in ["chunk one, ", "chunk two"]:
                self.wfile.write(b"%x\r\n%s\r\n" % (len(part), part.encode()))
            self.wfile.write(b"0\r\n\r\n")
        elif self.path == "/quit":
            self.reply(200, "bye")
            threading.Thread(target=self.server.shutdown).start()
        else: self.reply(404, "no such page")
    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(n).decode()
        if self.path == "/see-other": self.reply(303, "", [("Location", "/hello")])
        elif self.path == "/keep": self.reply(307, "", [("Location", "/echo")])
        else: self.reply(200, self.command + " " + self.path + " " + self.headers.get("Content-Type", "-") + " " + body)
srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
tmp = sys.argv[1] + ".tmp"
with open(tmp, "w") as f:
    f.write(str(srv.server_address[1]))
os.replace(tmp, sys.argv[1])   # the client never reads a half-written port
srv.serve_forever()
