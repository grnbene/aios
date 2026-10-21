#!/usr/bin/env python3
"""A small IMAP4rev1 server for testing the in-tree client (src/net/imap.cpp). Test-only: the product
has no Python in it. It is written to behave like the servers that have bitten mail clients: literals
in both directions, LITERAL+ and the older synchronising literal, SASL XOAUTH2 with its error
round-trip, UIDVALIDITY/UIDNEXT, FETCH items answered out of the order they were asked for, FLAGS after
a literal, X-GM-RAW search, APPEND with APPENDUID.

    python3 scripts/mock-imap.py --port 3143 --dir mailbox-seed/ [--no-literal-plus] [--no-sasl-ir]

--dir holds INBOX/*.eml (one file per message, uid = order) and any other mailbox as a directory.
Credentials: LOGIN test@example.com / secret, or XOAUTH2 with the access token in --token.
Writes the port it listens on to stdout as "LISTENING <port>" once it is ready."""
import argparse, base64, json, os, re, socket, socketserver, ssl, sys, threading, time

args = None
LOCK = threading.RLock()
BOXES = {}       # name -> {"uidvalidity": n, "uidnext": n, "msgs": [dict(uid, flags:set, date:str, raw:bytes)]}


def load(dirpath):
    for name in sorted(os.listdir(dirpath)):
        p = os.path.join(dirpath, name)
        if not os.path.isdir(p):
            continue
        msgs = []
        for i, f in enumerate(sorted(os.listdir(p)), start=1):
            if f.endswith(".eml"):
                msgs.append({"uid": i, "flags": set(), "date": "17-Jul-2002 02:44:25 -0700",
                             "raw": open(os.path.join(p, f), "rb").read()})
        BOXES[name] = {"uidvalidity": 1000 if name == "INBOX" else 1001 + len(BOXES), "uidnext": len(msgs) + 1, "msgs": msgs}
    BOXES.setdefault("INBOX", {"uidvalidity": 1000, "uidnext": 1, "msgs": []})


def quote(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def utf7_decode(s):
    out, i = "", 0
    while i < len(s):
        if s[i] != "&":
            out += s[i]; i += 1; continue
        j = s.index("-", i)
        if j == i + 1:
            out += "&"
        else:
            b = s[i + 1:j].replace(",", "/")
            b += "=" * (-len(b) % 4)
            out += base64.b64decode(b).decode("utf-16-be")
        i = j + 1
    return out



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

class Handler(socketserver.StreamRequestHandler):
    def send(self, data):
        if isinstance(data, str):
            data = data.encode()
        self.wfile.write(data)
        self.wfile.flush()

    def line(self):
        l = self.rfile.readline()
        if not l:
            raise EOFError
        return l.rstrip(b"\r\n").decode("utf-8", "surrogateescape")

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

    def caps(self):
        c = ["IMAP4rev1", "AUTH=XOAUTH2", "AUTH=PLAIN", "UIDPLUS", "X-GM-EXT-1", "MOVE"]
        if args.starttls and not self.tls_on:
            c.append("STARTTLS")
        if not args.no_sasl_ir:
            c.append("SASL-IR")
        if not args.no_literal_plus:
            c.append("LITERAL+")
        return " ".join(c)

    def tokens(self, rest):
        """Parse an argument string into atoms, quoted strings, lists and literals (reading them)."""
        pos = 0
        def val():
            nonlocal rest, pos
            while pos < len(rest) and rest[pos] == " ":
                pos += 1
            ch = rest[pos]
            if ch == "(":
                pos += 1; out = []
                while True:
                    while rest[pos] == " ":
                        pos += 1
                    if rest[pos] == ")":
                        pos += 1; return out
                    out.append(val())
            if ch == '"':
                pos += 1; s = ""
                while rest[pos] != '"':
                    if rest[pos] == "\\":
                        pos += 1
                    s += rest[pos]; pos += 1
                pos += 1
                return s
            if ch == "{":
                m = re.match(r"\{(\d+)(\+?)\}$", rest[pos:])
                n, plus = int(m.group(1)), m.group(2)
                if not plus:
                    self.send("+ ready\r\n")
                data = self.rfile.read(n)
                rest = self.line()   # the rest of the command line after the literal
                pos = 0
                return data
            m = re.match(r"[^ ()\"]+", rest[pos:])
            pos += m.end()
            return m.group(0)
        out = []
        while True:
            while pos < len(rest) and rest[pos] == " ":
                pos += 1
            if pos >= len(rest):
                return out
            out.append(val())

    def handle(self):
        self.state = "notauth"
        self.user = None
        self.sel = None
        self.readonly = False
        self.send("* OK [CAPABILITY %s] mock-imap ready\r\n" % self.caps())
        try:
            while True:
                l = self.line()
                if not l:
                    continue
                tag, _, rest = l.partition(" ")
                cmd, _, rest = rest.partition(" ")
                self.dispatch(tag, cmd.upper(), rest)
                if self.state == "logout":
                    return
        except (EOFError, ConnectionError):
            return

    def ok(self, tag, text="done", code=""):
        self.send("%s OK %s%s\r\n" % (tag, ("[%s] " % code) if code else "", text))

    def no(self, tag, text):
        self.send("%s NO %s\r\n" % (tag, text))

    def bad(self, tag, text):
        self.send("%s BAD %s\r\n" % (tag, text))

    def dispatch(self, tag, cmd, rest):
        if cmd == "CAPABILITY":
            self.send("* CAPABILITY %s\r\n" % self.caps()); return self.ok(tag)
        if cmd == "NOOP":
            return self.ok(tag)
        if cmd == "STARTTLS":
            if not args.starttls or self.tls_on:
                return self.bad(tag, "STARTTLS not available")
            self.ok(tag, "Begin TLS negotiation now")
            self.upgrade()
            return
        if cmd == "LOGOUT":
            self.send("* BYE bye\r\n"); self.ok(tag, "LOGOUT completed"); self.state = "logout"; return
        if cmd == "LOGIN":
            a = self.tokens(rest)
            if len(a) == 2 and a[0] == "test@example.com" and a[1] == "secret":
                self.state = "auth"; self.user = a[0]
                return self.ok(tag, "logged in", "CAPABILITY %s" % self.caps())
            return self.no(tag, "[AUTHENTICATIONFAILED] Invalid credentials")
        if cmd == "AUTHENTICATE":
            parts = rest.split(" ")
            mech = parts[0].upper()
            if len(parts) > 1:
                blob = parts[1]
            else:
                self.send("+ \r\n"); blob = self.line()
            raw = base64.b64decode(blob) if blob not in ("", "=") else b""
            if mech == "XOAUTH2":
                if token_ok(raw, "test@example.com", args.token):
                    self.state = "auth"; self.user = "test@example.com"
                    return self.ok(tag, "authenticated", "CAPABILITY %s" % self.caps())
                err = base64.b64encode(json.dumps({"status": "401", "schemes": "Bearer", "scope": "https://mail.google.com/"}).encode()).decode()
                self.send("+ %s\r\n" % err)
                self.line()   # the client's empty line
                return self.no(tag, "[AUTHENTICATIONFAILED] Invalid credentials (Failure)")
            if mech == "PLAIN":
                _, u, p = raw.split(b"\0")
                if u == b"test@example.com" and p == b"secret":
                    self.state = "auth"; self.user = "test@example.com"
                    return self.ok(tag, "authenticated", "CAPABILITY %s" % self.caps())
                return self.no(tag, "[AUTHENTICATIONFAILED] Invalid credentials")
            return self.no(tag, "unsupported mechanism")
        if self.state != "auth":
            return self.bad(tag, "authenticate first")
        a = self.tokens(rest) if cmd not in ("UID",) else None
        if cmd in ("SELECT", "EXAMINE"):
            name = utf7_decode(a[0])
            with LOCK:
                box = BOXES.get(name)
            if box is None:
                return self.no(tag, "[NONEXISTENT] Unknown Mailbox: %s" % name)
            self.sel, self.readonly = name, cmd == "EXAMINE"
            self.send("* FLAGS (\\Answered \\Flagged \\Deleted \\Seen \\Draft)\r\n")
            self.send("* %d EXISTS\r\n* 0 RECENT\r\n" % len(box["msgs"]))
            self.send("* OK [UIDVALIDITY %d] UIDs valid\r\n* OK [UIDNEXT %d] Predicted next UID\r\n" % (box["uidvalidity"], box["uidnext"]))
            return self.ok(tag, "selected", "READ-ONLY" if self.readonly else "READ-WRITE")
        if cmd == "STATUS":
            name = utf7_decode(a[0]); box = BOXES.get(name)
            if not box:
                return self.no(tag, "[NONEXISTENT] no such mailbox")
            self.send("* STATUS %s (UIDVALIDITY %d)\r\n" % (quote(a[0]), box["uidvalidity"]))
            return self.ok(tag)
        if cmd == "LIST":
            for name in sorted(BOXES):
                attrs = {"INBOX": "", "Sent": "\\Sent", "Drafts": "\\Drafts", "Trash": "\\Trash"}.get(name, "")
                self.send('* LIST (%s) "/" %s\r\n' % (attrs, quote(name)))
            return self.ok(tag)
        if cmd == "CREATE":
            name = utf7_decode(a[0])
            if name in BOXES:
                return self.no(tag, "[ALREADYEXISTS] exists")
            BOXES[name] = {"uidvalidity": 2000 + len(BOXES), "uidnext": 1, "msgs": []}
            return self.ok(tag)
        if cmd == "APPEND":
            name = utf7_decode(a[0]); box = BOXES.get(name)
            if box is None:
                return self.no(tag, "[TRYCREATE] no such mailbox")
            flags = set(a[1]) if isinstance(a[1], list) else set()
            data = a[-1]
            with LOCK:
                uid = box["uidnext"]; box["uidnext"] += 1
                box["msgs"].append({"uid": uid, "flags": flags, "date": "16-Oct-2026 12:00:00 +0000", "raw": data})
            return self.ok(tag, "append completed", "APPENDUID %d %d" % (box["uidvalidity"], uid))
        if cmd == "EXPUNGE":
            return self.expunge(tag, None)
        if cmd == "UID":
            sub, _, rest2 = rest.partition(" ")
            return self.uid_cmd(tag, sub.upper(), rest2)
        return self.bad(tag, "unknown command " + cmd)

    def uidset(self, spec, box):
        out = set()
        top = max([m["uid"] for m in box["msgs"]] or [0])
        for part in spec.split(","):
            if ":" in part:
                lo, hi = part.split(":")
                lo = top if lo == "*" else int(lo); hi = top if hi == "*" else int(hi)
                if lo > hi: lo, hi = hi, lo
                out.update(range(lo, hi + 1))
            else:
                out.add(top if part == "*" else int(part))
        return out

    def expunge(self, tag, uids):
        box = BOXES[self.sel]
        with LOCK:
            keep, gone = [], []
            for i, m in enumerate(box["msgs"], start=1):
                if "\\Deleted" in m["flags"] and (uids is None or m["uid"] in uids):
                    gone.append(i)
                else:
                    keep.append(m)
            box["msgs"] = keep
        for n in reversed(gone):
            self.send("* %d EXPUNGE\r\n" % n)
        return self.ok(tag, "expunged")

    def uid_cmd(self, tag, sub, rest):
        if self.sel is None:
            return self.bad(tag, "no mailbox selected")
        box = BOXES[self.sel]
        if sub == "SEARCH":
            tk = self.tokens(rest)
            hits = []
            with LOCK:
                msgs = list(box["msgs"])
            up = [str(t).upper() if not isinstance(t, (list, bytes)) else t for t in tk]
            for m in msgs:
                ok = True
                i = 0
                while i < len(up):
                    t = up[i]
                    if t == "ALL": pass
                    elif t == "UID":
                        ok = ok and m["uid"] in self.uidset(str(tk[i + 1]), box); i += 1
                    elif t == "X-GM-RAW":
                        ok = ok and str(tk[i + 1]).lower().encode() in m["raw"].lower(); i += 1
                    elif t == "TEXT":
                        ok = ok and str(tk[i + 1]).lower().encode() in m["raw"].lower(); i += 1
                    elif t == "UNSEEN":
                        ok = ok and "\\Seen" not in m["flags"]
                    elif t == "SUBJECT":
                        ok = ok and str(tk[i + 1]).lower().encode() in m["raw"].lower(); i += 1
                    else:
                        return self.bad(tag, "unsupported search key " + str(t))
                    i += 1
                if ok: hits.append(m["uid"])
            self.send("* SEARCH%s\r\n" % "".join(" %d" % u for u in hits))
            return self.ok(tag, "SEARCH completed")
        if sub == "FETCH":
            spec, _, items = rest.partition(" ")
            uids = self.uidset(spec, box)
            want = items.strip()
            if want.startswith("(") and want.endswith(")"):
                want = want[1:-1]
            with LOCK:
                msgs = [m for m in box["msgs"] if m["uid"] in uids]
            for m in msgs:
                seq = box["msgs"].index(m) + 1
                self.fetch_one(seq, m, want)
            return self.ok(tag, "FETCH completed")
        if sub == "STORE":
            spec, mode, rest2 = rest.split(" ", 2)
            flags = set(re.findall(r"\\?[A-Za-z$]+", rest2.split("(", 1)[1].rsplit(")", 1)[0]))
            with LOCK:
                for m in box["msgs"]:
                    if m["uid"] in self.uidset(spec, box):
                        if mode.startswith("+FLAGS"): m["flags"] |= flags
                        elif mode.startswith("-FLAGS"): m["flags"] -= flags
                        else: m["flags"] = set(flags)
            return self.ok(tag, "STORE completed")
        if sub == "COPY":
            spec, _, dest = rest.partition(" ")
            dest = utf7_decode(self.tokens(dest)[0])
            if dest not in BOXES:
                return self.no(tag, "[TRYCREATE] no such mailbox")
            with LOCK:
                for m in list(box["msgs"]):
                    if m["uid"] in self.uidset(spec, box):
                        d = BOXES[dest]; uid = d["uidnext"]; d["uidnext"] += 1
                        d["msgs"].append(dict(m, uid=uid, flags=set(m["flags"])))
            return self.ok(tag, "COPY completed")
        if sub == "EXPUNGE":
            return self.expunge(tag, self.uidset(rest.strip(), box))
        return self.bad(tag, "unknown UID subcommand " + sub)

    def fetch_one(self, seq, m, want):
        parts = []     # (name, bytes|str, is_literal)
        raw = m["raw"]
        names = re.findall(r"BODY\.PEEK\[[^\]]*\]|BODY\[[^\]]*\]|UID|FLAGS|INTERNALDATE|RFC822\.SIZE|X-GM-MSGID|X-GM-LABELS", want)
        blobs = []
        for n in names:
            if n == "UID": parts.append(("UID", str(m["uid"]), False))
            elif n == "FLAGS": parts.append(("FLAGS", "(" + " ".join(sorted(m["flags"])) + ")", False))
            elif n == "INTERNALDATE": parts.append(("INTERNALDATE", quote(m["date"]), False))
            elif n == "RFC822.SIZE": parts.append(("RFC822.SIZE", str(len(raw)), False))
            elif n == "X-GM-MSGID": parts.append(("X-GM-MSGID", str(1000000 + m["uid"]), False))
            elif n == "X-GM-LABELS": parts.append(("X-GM-LABELS", "()", False))
            else:
                sect = re.match(r"BODY(?:\.PEEK)?\[([^\]]*)\]", n).group(1)
                head, _, body = raw.partition(b"\r\n\r\n")
                if sect == "":
                    data = raw
                elif sect.upper().startswith("HEADER.FIELDS"):
                    fields = [f.lower() for f in re.findall(r"\(([^)]*)\)", sect)[0].split()]
                    keep, cur = [], None
                    for ln in head.split(b"\r\n"):
                        if ln[:1] in (b" ", b"\t") and cur is not None:
                            keep.append(ln); continue
                        cur = ln.split(b":", 1)[0].decode().lower() in fields
                        if cur: keep.append(ln)
                    data = b"\r\n".join(keep) + b"\r\n\r\n"
                elif sect.upper() == "HEADER":
                    data = head + b"\r\n\r\n"
                elif sect.upper() == "TEXT":
                    data = body
                else:
                    data = b""
                parts.append(("BODY[%s]" % sect, data, True))
        # Servers answer in their own order, and may put FLAGS after a literal: shuffle deterministically.
        if m["uid"] % 2 == 0:
            parts.reverse()
        out = b"* %d FETCH (" % seq
        first = True
        for name, val, lit in parts:
            if not first: out += b" "
            first = False
            out += name.encode() + b" "
            if lit:
                out += b"{%d}\r\n" % len(val) + val
            else:
                out += val.encode()
        out += b")\r\n"
        self.send(out)


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--dir", required=True)
    ap.add_argument("--token", default="ya29.good-token")
    ap.add_argument("--token-check", default="", help="URL (mock Google /userinfo) that decides whether a Bearer token is live")
    ap.add_argument("--no-literal-plus", action="store_true")
    ap.add_argument("--no-sasl-ir", action="store_true")
    ap.add_argument("--tls", action="store_true", help="TLS from the first byte (port 993 style)")
    ap.add_argument("--starttls", action="store_true", help="offer STARTTLS")
    ap.add_argument("--cert"); ap.add_argument("--key")
    args = ap.parse_args()
    load(args.dir)
    srv = Server(("127.0.0.1", args.port), Handler)
    print("LISTENING %d" % srv.server_address[1], flush=True)
    srv.serve_forever()
