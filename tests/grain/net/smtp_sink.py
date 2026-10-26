# A test SMTP server for tests/grain/test_mail.gr (run by scripts/grain-net-check.sh): python3 smtp_sink.py PORTFILE OUTDIR
# Plain SMTP on loopback: EHLO (AUTH PLAIN LOGIN, 8BITMIME), AUTH PLAIN/LOGIN (user "app", password "s3cret"),
# MAIL/RCPT/DATA (dot-unstuffed), RSET, NOOP, QUIT. Each message goes to OUTDIR/<n>.json:
# {"from", "rcpts", "auth", "data"}.
import base64, json, os, socketserver, sys, threading

OUT = sys.argv[2]
count = [0]
lock = threading.Lock()

class H(socketserver.StreamRequestHandler):
    def say(self, line): self.wfile.write((line + "\r\n").encode())
    def handle(self):
        self.say("220 sink ready")
        mail_from, rcpts, auth = None, [], None
        while True:
            raw = self.rfile.readline()
            if not raw: return
            line = raw.decode("utf-8", "replace").rstrip("\r\n")
            up = line.upper()
            if up.startswith("EHLO") or up.startswith("HELO"):
                self.wfile.write(b"250-sink\r\n250-AUTH PLAIN LOGIN\r\n250-8BITMIME\r\n250 SIZE 10485760\r\n")
            elif up.startswith("AUTH PLAIN"):
                parts = line.split(" ", 2)
                if len(parts) < 3:
                    self.say("334 "); parts.append(self.rfile.readline().decode().strip())
                _, user, pw = base64.b64decode(parts[2]).split(b"\0")
                if user == b"app" and pw == b"s3cret": auth = "app"; self.say("235 ok")
                else: self.say("535 bad credentials")
            elif up.startswith("AUTH LOGIN"):
                self.say("334 VXNlcm5hbWU6"); user = base64.b64decode(self.rfile.readline().strip())
                self.say("334 UGFzc3dvcmQ6"); pw = base64.b64decode(self.rfile.readline().strip())
                if user == b"app" and pw == b"s3cret": auth = "app"; self.say("235 ok")
                else: self.say("535 bad credentials")
            elif up.startswith("MAIL FROM:"):
                mail_from = line[10:].strip().strip("<>").split(">")[0]; rcpts = []; self.say("250 ok")
            elif up.startswith("RCPT TO:"):
                rcpts.append(line[8:].strip().strip("<>").split(">")[0]); self.say("250 ok")
            elif up == "DATA":
                self.say("354 go")
                data = []
                while True:
                    l = self.rfile.readline()
                    if not l or l in (b".\r\n", b".\n"): break
                    if l.startswith(b".."): l = l[1:]
                    data.append(l)
                with lock:
                    count[0] += 1; n = count[0]
                with open(os.path.join(OUT, "%d.json" % n), "w") as f:
                    json.dump({"from": mail_from, "rcpts": rcpts, "auth": auth, "data": b"".join(data).decode("utf-8", "replace")}, f)
                self.say("250 queued as %d" % n)
            elif up == "RSET": mail_from, rcpts = None, []; self.say("250 ok")
            elif up == "NOOP": self.say("250 ok")
            elif up == "QUIT": self.say("221 bye"); return
            else: self.say("502 not implemented")

class S(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

srv = S(("127.0.0.1", 0), H)
open(sys.argv[1], "w").write(str(srv.server_address[1]))
srv.serve_forever()
