#!/usr/bin/env python3
"""End to end through the real `aios` command line: `google auth` with a simulated browser, `google sync` of
mail, calendar, contacts and notes, reading the result back from fresh processes (a stand-in for a reboot),
sending mail with the OAuth token, an expired access token, and `google logout`. The servers are the mocks in
scripts/ (a stand-in for Google, IMAP and SMTP); the person's browser is this script. Test-only.

    python3 scripts/google-e2e.py build/aios <scratch-dir>
Exits non-zero if any check fails."""
import json, os, re, shutil, subprocess, sys, tempfile, time, urllib.request, urllib.parse

aios = os.path.abspath(sys.argv[1]); scratch = os.path.abspath(sys.argv[2])
here = os.path.dirname(os.path.abspath(__file__))
shutil.rmtree(scratch, ignore_errors=True); os.makedirs(scratch)
fails = 0
def check(ok, what):
    global fails
    print(("PASS  " if ok else "FAIL  ") + what)
    if not ok: fails += 1

procs = []
def start(script, *args):
    p = subprocess.Popen([sys.executable, os.path.join(here, script), *args], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    port = int(p.stdout.readline().split()[1]); procs.append(p); return p, port

def run(root, *cmd, timeout=120):
    r = subprocess.run([aios, "--root", root, "exec", " ".join(cmd)], capture_output=True, text=True, timeout=timeout)
    return r.returncode, r.stdout + r.stderr

def jget(url, method="GET", body=None):
    req = urllib.request.Request(url, method=method, data=json.dumps(body).encode() if body is not None else None, headers={"Content-Type": "application/json"} if body is not None else {})
    try:
        with urllib.request.urlopen(req, timeout=10) as r: return json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e: return {"_status": e.code}

class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *a, **k): return None

try:
    g, gp = start("mock-google.py", "--port", "0", "--ttl", "3600")
    seed = os.path.join(scratch, "seed"); [os.makedirs(os.path.join(seed, d)) for d in ("INBOX", "Sent", "Trash")]
    # a few real messages in the inbox
    for i, (sub, body) in enumerate([("Welcome", "hello there"), ("Invoice 42", "please pay"), ("café menu", "olé")], 1):
        open(os.path.join(seed, "INBOX", "m%d.eml" % i), "wb").write(("From: Bob <bob@example.com>\r\nTo: test@example.com\r\nSubject: %s\r\nDate: Fri, 16 Oct 2026 1%d:00:00 +0000\r\nMessage-ID: <m%d@x>\r\nContent-Type: text/plain; charset=utf-8\r\n\r\n%s\r\n" % (sub, i, i, body)).encode())
    tok = "http://127.0.0.1:%d/userinfo" % gp
    im, ip = start("mock-imap.py", "--port", "0", "--dir", seed, "--token-check", tok)
    outdir = os.path.join(scratch, "smtp"); sm, sp = start("mock-smtp.py", "--port", "0", "--out", outdir, "--token-check", tok, "--auth-plain-text")
    base = "http://127.0.0.1:%d" % gp
    root = os.path.join(scratch, "root"); os.makedirs(os.path.join(root, "oauth"))
    open(os.path.join(root, "oauth", "google.json"), "w").write(json.dumps({"installed": {
        "client_id": "mock-client.apps.googleusercontent.com", "client_secret": "mock-secret", "auth_uri": base + "/auth", "token_uri": base + "/token",
        "userinfo_uri": base + "/userinfo", "revoke_uri": base + "/revoke", "calendar_api": base + "/calendar/v3", "people_api": base + "/v1",
        "imap_host": "127.0.0.1", "imap_port": ip, "imap_security": "none", "smtp_host": "127.0.0.1", "smtp_port": sp, "smtp_security": "none"}}))
    # something to sync on the Google side
    now = time.time()
    def iso(t, off="+00:00"): return time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(t)) + off
    jget(base + "/_admin/put_event", "POST", {"cal": "sean@example.com", "event": {"id": "ev1", "summary": "Board meeting", "start": {"dateTime": iso(now + 86400)}, "end": {"dateTime": iso(now + 86400 + 3600)}}})
    jget(base + "/_admin/put_person", "POST", {"resourceName": "people/c9", "names": [{"givenName": "Margaret", "familyName": "Hamilton"}], "emailAddresses": [{"value": "margaret@example.com"}]})

    # --- no client configured on a different root: a clear message ---
    other = os.path.join(scratch, "bare"); os.makedirs(other)
    rc, out = run(other, "google auth you@gmail.com")
    check(rc != 0 and "oauth/google.json" in out and "Desktop app" in out, "with no OAuth client saved, `google auth` says what to create and where to save it")

    # --- sign in: start `google auth` waiting on its loopback listener, play the browser ---
    p = subprocess.Popen([aios, "--root", root, "exec", "google auth test@example.com"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    url = None
    for line in p.stdout:
        m = re.search(r"(http://127\.0\.0\.1:\d+/auth\?\S+)", line)
        if m: url = m.group(1); break
    check(url is not None and "code_challenge_method=S256" in url, "`google auth` prints a consent URL with PKCE")
    opener = urllib.request.build_opener(NoRedirect)
    try: opener.open(url)
    except urllib.error.HTTPError as e:
        loc = e.headers["Location"]; urllib.request.urlopen(loc, timeout=10).read()
    out = p.stdout.read(); p.wait(timeout=30)
    check(p.returncode == 0 and "[SIGNED IN]" in out and "test@example.com" in out, "the redirect lands, the sign-in completes: " + out.strip().splitlines()[-1][:80])
    check(os.path.exists(os.path.join(root, "oauth", "tokens.vault")) and b"ya29" not in open(os.path.join(root, "oauth", "tokens.vault"), "rb").read(), "the tokens are on disk, encrypted")
    rc, out = run(root, "mail account list")
    check("test@example.com" in out and "127.0.0.1:%d" % ip in out, "a mail account was set up for it")

    # --- the status, from a fresh process ---
    rc, out = run(root, "google status")
    check(rc == 0 and "test@example.com" in out and "refresh token: held" in out, "`google status` from a new process sees the sign-in")

    # --- sync everything ---
    rc, out = run(root, "google sync")
    print("    " + out.strip().replace("\n", "\n    "))
    check(rc == 0 and "[mail]" in out and "[calendar]" in out and "[contacts]" in out and "[notes]" in out and "FAILED" not in out, "`google sync` syncs mail, calendar, contacts and notes with no failures")
    rc, out = run(root, "mail list")
    check("Welcome" in out and "Invoice 42" in out and "caf" in out, "the mail is in the local inbox (3 messages)")
    rc, out = run(root, "calendar agenda") if False else run(root, "calendar list")
    check("Board meeting" in out, "the Google calendar event is local")
    rc, out = run(root, "contacts list")
    check("Margaret" in out, "the Google contact is local")

    # --- a later process, as after a reboot: no sign-in again, only what changed moves ---
    jget(base + "/_admin/put_event", "POST", {"cal": "sean@example.com", "event": {"id": "ev2", "summary": "Lunch", "start": {"dateTime": iso(now + 2 * 86400)}, "end": {"dateTime": iso(now + 2 * 86400 + 3600)}}})
    rc, out = run(root, "google sync calendar")
    check(rc == 0 and "pulled 1 new" in out, "a later sync in a new process, with no sign-in, pulls just the new event")

    # --- the access token dies: the next sync refreshes it ---
    before = jget(base + "/_state")["token_refresh"]
    jget(base + "/_expire", "POST", {})
    rc, out = run(root, "google sync calendar contacts")
    after = jget(base + "/_state")["token_refresh"]
    check(rc == 0 and after == before + 1, "an access token the server dropped is refreshed by the next sync (no new sign-in)")

    # --- send mail with the OAuth token ---
    rc, out = run(root, "mail send you@example.com 'From aios' 'sent with an OAuth token'")
    check("[SENT]" in out, "`mail send` delivers through SMTP with XOAUTH2: " + out.strip()[:90])
    files = sorted(f for f in os.listdir(outdir) if f.endswith(".eml")) if os.path.isdir(outdir) else []
    check(len(files) == 1 and b"sent with an OAuth token" in open(os.path.join(outdir, files[0]), "rb").read(), "and the SMTP server received it")

    # --- sign out ---
    revoked_before = jget(base + "/_state")["revoked"]
    rc, out = run(root, "google logout")
    check(rc == 0 and "SIGNED OUT" in out and jget(base + "/_state")["revoked"] == revoked_before + 1, "`google logout` revokes the token at the provider")
    rc, out = run(root, "google sync")
    check(rc != 0 and "no Google account is signed in" in out, "and a sync afterwards says to sign in again")
finally:
    for p in procs:
        p.terminate()
print("%s (%d failed)" % ("FAILED" if fails else "ALL PASSED", fails))
sys.exit(1 if fails else 0)
