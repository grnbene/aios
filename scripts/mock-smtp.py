#!/usr/bin/env python3
"""A small SMTP submission server for testing the in-tree client (src/net/smtp.cpp). Test-only; the
product has no Python in it. It enforces what real servers enforce: AUTH before MAIL, dot-unstuffing,
8BITMIME only when advertised, a SIZE limit, and per-recipient refusals (any rcpt at refuse.example
gets 550). Messages it accepts are written to --out as N.eml with N.env (the envelope as JSON).

    python3 scripts/mock-smtp.py --port 0 --out DIR [--tls --cert C --key K | --starttls --cert C --key K]
        [--token ya29.good-token] [--no-8bitmime] [--size 5000000] [--no-auth-plain]
Credentials: test@example.com / secret, or XOAUTH2 with --token. Prints "LISTENING <port>"."""
import argparse, base64, json, os, socketserver, ssl, threading

args = None
COUNT = [0]
LOCK = threading.Lock()



def token_ok(blob_raw, user, fixed):
    """The XOAUTH2 blob is valid if it names `user` and carries either the fixed test token or, when
    --token-check URL is given, a Bearer token that URL (the mock Google's /userinfo) accepts for that user."""
    import urllib.request
    try:
        parts = blob_raw.decode().split("\x01")
        who = parts[0][5:] if parts[0].startswith("user=") else ""
        tok = parts[1][len("auth=Bearer "):] if len(parts) > 1 and parts[1].startswith("auth=Bearer ") else ""
    except Exception:
        return False
    if who != user:
        return False
    if args.token_check:
        try:
            req = urllib.request.Request(args.token_check, headers={"Authorization": "Bearer " + tok})
            with urllib.request.urlopen(req, timeout=5) as r:
                return r.status == 200
        except Exception:
            return False
    return tok == fixed

class H(socketserver.StreamRequestHandler):
    def setup(self):
        super().setup()
        self.tls_on = False
        if args.tls:
            self.upgrade()

    def upgrade(self):
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(args.cert, args.key)
        self.request = ctx.wrap_socket(self.request, server_side=True)
        self.rfile = self.request.makefile("rb")
        self.wfile = self.request.makefile("wb", buffering=0)
        self.tls_on = True

    def send(self, s):
        self.wfile.write(s.encode() if isinstance(s, str) else s)
        self.wfile.flush()

    def line(self):
        l = self.rfile.readline()
        if not l:
            raise EOFError
        return l.rstrip(b"\r\n").decode("utf-8", "surrogateescape")

    def handle(self):
        self.authed = False
        self.mail_from = None
        self.rcpts = []
        self.send("220 mock-smtp ESMTP ready\r\n")
        try:
            while True:
                l = self.line()
                cmd, _, rest = l.partition(" ")
                cmd = cmd.upper()
                if cmd in ("EHLO", "HELO"):
                    if cmd == "HELO":
                        self.send("250 mock-smtp\r\n"); continue
                    lines = ["mock-smtp", "PIPELINING", "SIZE %d" % args.size, "ENHANCEDSTATUSCODES"]
                    if not args.no_8bitmime: lines.append("8BITMIME")
                    if args.starttls and not self.tls_on: lines.append("STARTTLS")
                    if args.tls or self.tls_on or args.auth_plain_text:
                        mechs = ["LOGIN", "XOAUTH2"] + ([] if args.no_auth_plain else ["PLAIN"])
                        lines.append("AUTH " + " ".join(mechs))
                    self.send("".join(("250-" if i < len(lines) - 1 else "250 ") + t + "\r\n" for i, t in enumerate(lines)))
                elif cmd == "STARTTLS":
                    if not args.starttls or self.tls_on:
                        self.send("503 no\r\n"); continue
                    self.send("220 Ready to start TLS\r\n")
                    self.upgrade()
                elif cmd == "AUTH":
                    parts = rest.split(" ")
                    mech = parts[0].upper()
                    if mech == "PLAIN":
                        blob = parts[1] if len(parts) > 1 else (self.send("334 \r\n") or self.line())
                        _, u, p = base64.b64decode(blob).split(b"\0")
                        if (u, p) == (b"test@example.com", b"secret"):
                            self.authed = True; self.send("235 2.7.0 Accepted\r\n")
                        else:
                            self.send("535 5.7.8 Authentication credentials invalid\r\n")
                    elif mech == "LOGIN":
                        self.send("334 " + base64.b64encode(b"Username:").decode() + "\r\n")
                        u = base64.b64decode(self.line())
                        self.send("334 " + base64.b64encode(b"Password:").decode() + "\r\n")
                        p = base64.b64decode(self.line())
                        if (u, p) == (b"test@example.com", b"secret"):
                            self.authed = True; self.send("235 2.7.0 Accepted\r\n")
                        else:
                            self.send("535 5.7.8 Authentication credentials invalid\r\n")
                    elif mech == "XOAUTH2":
                        raw = base64.b64decode(parts[1])
                        if token_ok(raw, "test@example.com", args.token):
                            self.authed = True; self.send("235 2.7.0 Accepted\r\n")
                        else:
                            self.send("334 " + base64.b64encode(json.dumps({"status": "400", "schemes": "Bearer", "scope": "https://mail.google.com/"}).encode()).decode() + "\r\n")
                            self.line()
                            self.send("535 5.7.8 Username and Password not accepted\r\n")
                    else:
                        self.send("504 unrecognised mechanism\r\n")
                elif cmd == "MAIL":
                    if not self.authed:
                        self.send("530 5.7.0 Authentication Required\r\n"); continue
                    up = rest.upper()
                    if "BODY=8BITMIME" in up and args.no_8bitmime:
                        self.send("555 8BITMIME not supported\r\n"); continue
                    import re
                    m = re.search(r"SIZE=(\d+)", up)
                    if m and int(m.group(1)) > args.size:
                        self.send("552 5.3.4 Message size exceeds fixed limit\r\n"); continue
                    self.mail_from = rest[rest.index("<") + 1:rest.index(">")]
                    self.rcpts = []
                    self.send("250 2.1.0 OK\r\n")
                elif cmd == "RCPT":
                    if self.mail_from is None:
                        self.send("503 need MAIL first\r\n"); continue
                    a = rest[rest.index("<") + 1:rest.index(">")]
                    if a.endswith("@refuse.example"):
                        self.send("550 5.1.1 No such user\r\n")
                    else:
                        self.rcpts.append(a); self.send("250 2.1.5 OK\r\n")
                elif cmd == "DATA":
                    if not self.rcpts:
                        self.send("503 no valid recipients\r\n"); continue
                    self.send("354 End data with <CR><LF>.<CR><LF>\r\n")
                    data = []
                    while True:
                        l = self.rfile.readline()
                        if not l:
                            raise EOFError
                        if l == b".\r\n":
                            break
                        assert l.endswith(b"\r\n"), "bare LF on the wire: %r" % l[:40]
                        data.append(l[1:] if l.startswith(b"..") else l)
                    with LOCK:
                        COUNT[0] += 1; n = COUNT[0]
                    open(os.path.join(args.out, "%d.eml" % n), "wb").write(b"".join(data))
                    open(os.path.join(args.out, "%d.env" % n), "w").write(json.dumps({"from": self.mail_from, "to": self.rcpts}))
                    self.send("250 2.0.0 OK queued as %d\r\n" % n)
                    self.mail_from = None
                elif cmd == "RSET":
                    self.mail_from = None; self.rcpts = []; self.send("250 OK\r\n")
                elif cmd == "NOOP":
                    self.send("250 OK\r\n")
                elif cmd == "QUIT":
                    self.send("221 bye\r\n"); return
                else:
                    self.send("500 5.5.1 unrecognised command\r\n")
        except (EOFError, ConnectionError, ssl.SSLError):
            return


class S(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--out", required=True)
    ap.add_argument("--token", default="ya29.good-token")
    ap.add_argument("--token-check", default="", help="URL (mock Google /userinfo) that decides whether a Bearer token is live")
    ap.add_argument("--tls", action="store_true")
    ap.add_argument("--starttls", action="store_true")
    ap.add_argument("--cert"); ap.add_argument("--key")
    ap.add_argument("--no-8bitmime", action="store_true")
    ap.add_argument("--no-auth-plain", action="store_true")
    ap.add_argument("--auth-plain-text", action="store_true", help="advertise AUTH on an unencrypted connection (test only)")
    ap.add_argument("--size", type=int, default=5000000)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    srv = S(("127.0.0.1", args.port), H)
    print("LISTENING %d" % srv.server_address[1], flush=True)
    srv.serve_forever()
