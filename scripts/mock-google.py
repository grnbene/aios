#!/usr/bin/env python3
"""A stand-in for Google's OAuth 2.0 endpoints (and, further down, Calendar and People) for testing
the in-tree clients. Test-only; the product has no Python in it. It enforces what the real thing
enforces so a client that cuts a corner fails here: the authorization code is single use and bound to
its PKCE challenge (S256), the client id and secret must match, redirect_uri must be the one the
authorization used, access tokens expire, a refresh token can be revoked, and every API call needs a
live Bearer token.

    python3 scripts/mock-google.py --port 0 [--ttl 3600] [--client-id ID] [--client-secret S]
Prints "LISTENING <port>". Test hooks: GET /_state, POST /_expire (drop every access token),
POST /_revoke (revoke every refresh token)."""
import argparse, base64, hashlib, json, secrets, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

args = None
LOCK = threading.RLock()
CODES = {}       # code -> dict(challenge, redirect, client_id, email, used)
ACCESS = {}      # access token -> (email, expires_at)
REFRESH = {}     # refresh token -> email
COUNTERS = {"token_auth_code": 0, "token_refresh": 0, "revoked": 0, "api_calls": 0, "api_401": 0}
API = None       # filled by google_api()


def b64url(b):
    return base64.urlsafe_b64encode(b).decode().rstrip("=")


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def reply(self, code, body, ctype="application/json", extra=None):
        data = body if isinstance(body, bytes) else (json.dumps(body) if not isinstance(body, str) else body).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    def form(self):
        n = int(self.headers.get("Content-Length", 0))
        return {k: v[0] for k, v in parse_qs(self.rfile.read(n).decode()).items()}

    def bearer(self):
        a = self.headers.get("Authorization", "")
        if not a.startswith("Bearer "):
            return None
        with LOCK:
            rec = ACCESS.get(a[7:])
        if rec and rec[1] > time.time():
            return rec[0]
        return None

    def do_GET(self):
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        if u.path == "/auth":
            if q.get("client_id") != args.client_id or q.get("response_type") != "code":
                return self.reply(400, {"error": "invalid_request"})
            if q.get("code_challenge_method") != "S256" or not q.get("code_challenge"):
                return self.reply(400, {"error": "invalid_request", "error_description": "PKCE S256 required"})
            red = q.get("redirect_uri", "")
            if not (red.startswith("http://127.0.0.1:") or red.startswith("http://localhost:") or red.startswith("http://[::1]:")):
                return self.reply(400, {"error": "redirect_uri_mismatch"})
            sep = "&" if "?" in red else "?"
            if q.get("deny"):
                loc = "%s%serror=access_denied&state=%s" % (red, sep, q.get("state", ""))
            else:
                code = secrets.token_urlsafe(24)
                with LOCK:
                    CODES[code] = {"challenge": q["code_challenge"], "redirect": red, "client_id": q["client_id"],
                                   "email": q.get("login_hint") or "sean@example.com", "used": False,
                                   "scope": q.get("scope", ""), "offline": q.get("access_type") == "offline"}
                loc = "%s%scode=%s&state=%s" % (red, sep, code, q.get("state", ""))
            return self.reply(302, b"", "text/plain", {"Location": loc})
        if u.path == "/userinfo":
            who = self.bearer()
            if not who:
                return self.reply(401, {"error": "invalid_token"})
            return self.reply(200, {"email": who, "email_verified": True})
        if u.path == "/_state":
            with LOCK:
                return self.reply(200, dict(COUNTERS, live_access=[t for t, (e, x) in ACCESS.items() if x > time.time()],
                                            refresh=list(REFRESH)))
        if API and API.get(self, u.path, q):
            return
        self.reply(404, {"error": "not_found", "path": u.path})

    def do_POST(self):
        u = urlparse(self.path)
        if u.path == "/token":
            f = self.form()
            if f.get("client_id") != args.client_id or (args.client_secret and f.get("client_secret") != args.client_secret):
                return self.reply(401, {"error": "invalid_client"})
            gt = f.get("grant_type")
            if gt == "authorization_code":
                with LOCK:
                    rec = CODES.get(f.get("code", ""))
                    if not rec or rec["used"]:
                        return self.reply(400, {"error": "invalid_grant", "error_description": "Bad Request"})
                    rec["used"] = True
                if rec["redirect"] != f.get("redirect_uri"):
                    return self.reply(400, {"error": "redirect_uri_mismatch"})
                ver = f.get("code_verifier", "")
                if not (43 <= len(ver) <= 128) or b64url(hashlib.sha256(ver.encode()).digest()) != rec["challenge"]:
                    return self.reply(400, {"error": "invalid_grant", "error_description": "Invalid code verifier."})
                with LOCK:
                    COUNTERS["token_auth_code"] += 1
                    at = "ya29." + secrets.token_urlsafe(20)
                    ACCESS[at] = (rec["email"], time.time() + args.ttl)
                    out = {"access_token": at, "expires_in": args.ttl, "scope": rec["scope"], "token_type": "Bearer"}
                    if rec["offline"]:
                        rt = "1//" + secrets.token_urlsafe(20)
                        REFRESH[rt] = rec["email"]
                        out["refresh_token"] = rt
                    hdr = b64url(json.dumps({"alg": "none"}).encode())
                    out["id_token"] = hdr + "." + b64url(json.dumps({"email": rec["email"], "sub": "1"}).encode()) + "."
                return self.reply(200, out)
            if gt == "refresh_token":
                with LOCK:
                    who = REFRESH.get(f.get("refresh_token", ""))
                    if not who:
                        return self.reply(400, {"error": "invalid_grant", "error_description": "Token has been expired or revoked."})
                    COUNTERS["token_refresh"] += 1
                    at = "ya29." + secrets.token_urlsafe(20)
                    ACCESS[at] = (who, time.time() + args.ttl)
                return self.reply(200, {"access_token": at, "expires_in": args.ttl, "scope": "x", "token_type": "Bearer"})
            return self.reply(400, {"error": "unsupported_grant_type"})
        if u.path == "/revoke":
            f = self.form()
            with LOCK:
                REFRESH.pop(f.get("token", ""), None)
                COUNTERS["revoked"] += 1
            return self.reply(200, {})
        if u.path == "/_expire":
            with LOCK:
                ACCESS.clear()
            return self.reply(200, {})
        if u.path == "/_revoke":
            with LOCK:
                REFRESH.clear()
            return self.reply(200, {})
        n = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(n) if n else b""
        if API and API.post(self, u.path, parse_qs(u.query), body):
            return
        self.reply(404, {"error": "not_found"})

    def do_PUT(self):
        self.do_other("PUT")

    def do_PATCH(self):
        self.do_other("PATCH")

    def do_DELETE(self):
        self.do_other("DELETE")

    def do_other(self, method):
        u = urlparse(self.path)
        n = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(n) if n else b""
        if API and API.other(self, method, u.path, {k: v[0] for k, v in parse_qs(u.query).items()}, body):
            return
        self.reply(404, {"error": "not_found"})



# ----------------------------------------------------------------------------------------------
# Calendar v3 and People v1, enough of each to hold a client to the real contract: Bearer required,
# etags and If-Match, paging, read-only calendars, 429 with Retry-After, stale reads after a write.
# ----------------------------------------------------------------------------------------------
import re
from urllib.parse import unquote

CALS = {
    "sean@example.com": {"info": {"id": "sean@example.com", "summary": "Sean", "primary": True, "accessRole": "owner", "backgroundColor": "#9fe1e7"}, "events": {}},
    "team@group.calendar.google.com": {"info": {"id": "team@group.calendar.google.com", "summary": "Team", "accessRole": "reader", "backgroundColor": "#f83a22"}, "events": {}},
}
PEOPLE = {}
PEOPLE_PREV = [None]        # a snapshot from before the latest write, for stale reads
STATE = {"fail_next": None, "stale_reads": 0, "ctr": 0, "send_updates": [], "list_calls": 0, "page_size": 250}


def new_id(prefix=""):
    STATE["ctr"] += 1
    return "%s%08x%d" % (prefix, secrets.randbits(32), STATE["ctr"])


def etag_of(obj):
    return '"%s"' % hashlib.md5(json.dumps(obj, sort_keys=True).encode()).hexdigest()[:16]


def rfc_epoch(t):
    from datetime import datetime, timezone
    t = t.replace("Z", "+00:00")
    return datetime.fromisoformat(t).astimezone(timezone.utc).timestamp()


def ev_epoch(ev):
    st = ev.get("start", {})
    if "dateTime" in st:
        return rfc_epoch(st["dateTime"])
    from datetime import datetime, timezone
    return datetime.fromisoformat(st["date"]).replace(tzinfo=timezone.utc).timestamp()


class Api:
    def guard(self, h):
        if STATE["fail_next"]:
            code, retry = STATE["fail_next"]; STATE["fail_next"] = None
            h.reply(code, {"error": {"code": code, "message": "injected"}}, extra={"Retry-After": str(retry)} if retry else None)
            return False
        COUNTERS["api_calls"] += 1
        if not h.bearer():
            COUNTERS["api_401"] += 1
            h.reply(401, {"error": {"code": 401, "message": "Invalid Credentials", "status": "UNAUTHENTICATED"}})
            return False
        return True

    def get(self, h, path, q):
        m = re.match(r"^/calendar/v3/users/me/calendarList$", path)
        if m:
            if not self.guard(h): return True
            h.reply(200, {"items": [c["info"] for c in CALS.values()]}); return True
        m = re.match(r"^/calendar/v3/calendars/([^/]+)/events$", path)
        if m:
            if not self.guard(h): return True
            cal = CALS.get(unquote(m.group(1)))
            if not cal:
                h.reply(404, {"error": {"code": 404, "message": "Not Found"}}); return True
            tmin = rfc_epoch(q["timeMin"]) if "timeMin" in q else -1e18
            tmax = rfc_epoch(q["timeMax"]) if "timeMax" in q else 1e18
            evs = sorted([e for e in cal["events"].values() if e.get("status") != "cancelled" and tmin <= ev_epoch(e) < tmax], key=ev_epoch)
            size = min(int(q.get("maxResults", 250)), STATE["page_size"])
            off = int(q.get("pageToken", "0") or 0)
            out = {"items": evs[off:off + size]}
            if off + size < len(evs):
                out["nextPageToken"] = str(off + size)
            h.reply(200, out); return True
        if path == "/v1/people/me/connections":
            if not self.guard(h): return True
            STATE["list_calls"] += 1
            src = PEOPLE
            if STATE["stale_reads"] > 0 and PEOPLE_PREV[0] is not None:
                STATE["stale_reads"] -= 1; src = PEOPLE_PREV[0]
            plist = sorted(src.values(), key=lambda p: p["resourceName"])
            size = min(int(q.get("pageSize", 100)), STATE["page_size"])
            off = int(q.get("pageToken", "0") or 0)
            out = {"connections": plist[off:off + size], "totalItems": len(plist)}
            if off + size < len(plist):
                out["nextPageToken"] = str(off + size)
            h.reply(200, out); return True
        if path == "/_admin/dump":
            h.reply(200, {"cals": {k: list(v["events"].values()) for k, v in CALS.items()}, "people": list(PEOPLE.values()), "send_updates": STATE["send_updates"]}); return True
        return False

    def post(self, h, path, query, body):
        data = json.loads(body) if body else {}
        m = re.match(r"^/calendar/v3/calendars/([^/]+)/events$", path)
        if m:
            if not self.guard(h): return True
            cal = CALS.get(unquote(m.group(1)))
            if not cal or cal["info"]["accessRole"] not in ("owner", "writer"):
                h.reply(403, {"error": {"code": 403, "message": "Forbidden"}}); return True
            STATE["send_updates"].append(query.get("sendUpdates", [""])[0])
            ev = dict(data); ev["id"] = new_id("e"); ev["status"] = "confirmed"; ev["etag"] = etag_of(ev)
            cal["events"][ev["id"]] = ev
            h.reply(200, ev); return True
        if path == "/v1/people:createContact":
            if not self.guard(h): return True
            PEOPLE_PREV[0] = json.loads(json.dumps(PEOPLE))
            p = dict(data); p["resourceName"] = "people/c%d" % (1000 + len(PEOPLE) + STATE["ctr"]); STATE["ctr"] += 1
            p["metadata"] = {"sources": [{"type": "CONTACT"}]}; p["etag"] = etag_of(p)
            PEOPLE[p["resourceName"]] = p
            h.reply(200, p); return True
        if path.startswith("/_admin/"):
            op = path.split("/")[-1]
            if op == "put_event":
                cal = CALS[data["cal"]]; ev = dict(data["event"]); ev.setdefault("id", new_id("e")); ev.setdefault("status", "confirmed")
                ev["etag"] = etag_of(dict(ev, _n=STATE["ctr"])); STATE["ctr"] += 1
                cal["events"][ev["id"]] = ev; h.reply(200, ev); return True
            if op == "delete_event":
                CALS[data["cal"]]["events"].pop(data["id"], None); h.reply(200, {}); return True
            if op == "put_person":
                p = dict(data); STATE["ctr"] += 1; p["etag"] = etag_of(dict(p, _n=STATE["ctr"])); p.setdefault("metadata", {"sources": [{"type": "CONTACT"}]})
                PEOPLE_PREV[0] = json.loads(json.dumps(PEOPLE)); PEOPLE[p["resourceName"]] = p; h.reply(200, p); return True
            if op == "delete_person":
                PEOPLE.pop(data["resourceName"], None); h.reply(200, {}); return True
            if op == "fail_next":
                STATE["fail_next"] = (data["code"], data.get("retry", 0)); h.reply(200, {}); return True
            if op == "stale_reads":
                STATE["stale_reads"] = data["n"]; h.reply(200, {}); return True
            if op == "page_size":
                STATE["page_size"] = data["n"]; h.reply(200, {}); return True
        return False

    def other(self, h, method, path, q, body):
        data = json.loads(body) if body else {}
        m = re.match(r"^/calendar/v3/calendars/([^/]+)/events/([^/]+)$", path)
        if m:
            if not self.guard(h): return True
            cal = CALS.get(unquote(m.group(1)))
            if not cal or cal["info"]["accessRole"] not in ("owner", "writer"):
                h.reply(403, {"error": {"code": 403, "message": "Forbidden"}}); return True
            STATE["send_updates"].append(q.get("sendUpdates", ""))
            ev = cal["events"].get(unquote(m.group(2)))
            if method == "DELETE":
                if not ev:
                    h.reply(410, {"error": {"code": 410, "message": "Resource has been deleted"}}); return True
                del cal["events"][ev["id"]]; h.reply(204, b"", "text/plain"); return True
            if not ev:
                h.reply(404, {"error": {"code": 404, "message": "Not Found"}}); return True
            im = h.headers.get("If-Match")
            if im and im != ev["etag"]:
                h.reply(412, {"error": {"code": 412, "message": "Precondition Failed"}}); return True
            ev.update(data); STATE["ctr"] += 1; ev["etag"] = etag_of(dict(ev, _n=STATE["ctr"]))
            h.reply(200, ev); return True
        m = re.match(r"^/v1/(people/c\d+):(updateContact|deleteContact)$", path)
        if m:
            if not self.guard(h): return True
            rn = m.group(1)
            p = PEOPLE.get(rn)
            if not p:
                h.reply(404, {"error": {"code": 404, "message": "Requested entity was not found."}}); return True
            if m.group(2) == "deleteContact":
                PEOPLE_PREV[0] = json.loads(json.dumps(PEOPLE)); del PEOPLE[rn]; h.reply(200, {}); return True
            if data.get("etag") != p["etag"]:
                h.reply(400, {"error": {"code": 400, "message": "Request person.etag is different than the current person.etag", "status": "FAILED_PRECONDITION"}}); return True
            mask = [f for f in q.get("updatePersonFields", "").split(",") if f]
            if not mask:
                h.reply(400, {"error": {"code": 400, "message": "updatePersonFields is required"}}); return True
            PEOPLE_PREV[0] = json.loads(json.dumps(PEOPLE))
            for f in mask:
                if f in data: p[f] = data[f]
                else: p.pop(f, None)
            STATE["ctr"] += 1; p["etag"] = etag_of(dict(p, _n=STATE["ctr"]))
            h.reply(200, p); return True
        return False


API = Api()


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--ttl", type=int, default=3600)
    ap.add_argument("--client-id", default="mock-client.apps.googleusercontent.com")
    ap.add_argument("--client-secret", default="mock-secret")
    args = ap.parse_args()
    srv = ThreadingHTTPServer(("127.0.0.1", args.port), H)
    srv.daemon_threads = True
    print("LISTENING %d" % srv.server_address[1], flush=True)
    srv.serve_forever()
