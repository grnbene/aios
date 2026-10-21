#!/usr/bin/env python3
"""Runs every check of the mail / OAuth / Google stack in one go, against the mock servers in scripts/:

  - the MIME reader against Python's `email` package (34 messages, 14 dates)
  - the JSON parser against Python's `json` (947 documents)
  - the IMAP client (plain, implicit TLS, STARTTLS; with and without LITERAL+ and SASL-IR)
  - the SMTP client (plain, TLS, STARTTLS, no 8BITMIME, LOGIN only)
  - OAuth 2.0 (PKCE, state, vault at rest, refresh, revocation)
  - Google calendar, contacts and notes sync (two-way, conflicts, paging, 429, expired token, stale reads)
  - the whole thing through the `aios` command line (sign in, sync, send, sign out)

    python3 scripts/mail-stack-check.py [build-dir]        (default: build)
Test-only; there is no Python in the product. Needs python3 and openssl for the mock servers and test certificate."""
import os, shutil, subprocess, sys, tempfile, time

build = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build")
here = os.path.dirname(os.path.abspath(__file__))
work = tempfile.mkdtemp(prefix="mailstack-")
results = []

def sh(*cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)

def record(name, ok, detail=""):
    results.append((name, ok)); print(("PASS  " if ok else "FAIL  ") + name + (("\n      " + detail) if (detail and not ok) else ""))

procs = []
def start(script, *args):
    p = subprocess.Popen([sys.executable, os.path.join(here, script), *args], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    port = int(p.stdout.readline().split()[1]); procs.append(p); return port

def stop():
    for p in procs: p.terminate()
    procs.clear()

def tail(r):
    lines = (r.stdout + r.stderr).strip().splitlines()
    return "\n      ".join(lines[-6:])

try:
    # --- offline comparisons ---
    r = sh(sys.executable, os.path.join(here, "mime-diff.py"), os.path.join(build, "aios-mime-check")); record("MIME reader vs Python email: " + (r.stdout.strip().splitlines() or ["?"])[-1], r.returncode == 0, tail(r))
    r = sh(sys.executable, os.path.join(here, "json-diff.py"), os.path.join(build, "aios-json-check")); record("JSON parser vs Python json: " + (r.stdout.strip().splitlines() or ["?"])[-1], r.returncode == 0, tail(r))

    # --- fixtures: a mailbox from the MIME corpus, a self-signed certificate ---
    seed = os.path.join(work, "seed")
    for d in ("INBOX", "Sent", "Drafts", "Trash"): os.makedirs(os.path.join(seed, d))
    ns = {"__file__": os.path.join(here, "mime-diff.py")}
    exec(open(os.path.join(here, "mime-diff.py")).read().split("def reference")[0], ns)
    for name, raw in ns["cases"].items(): open(os.path.join(seed, "INBOX", name + ".eml"), "wb").write(raw)
    cert, key = os.path.join(work, "c.pem"), os.path.join(work, "k.pem")
    r = sh("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", key, "-out", cert, "-days", "2", "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost")
    record("test certificate made", r.returncode == 0, tail(r))

    imap_check = os.path.join(build, "aios-imap-check")
    for label, mock_args, mode in [("plain, LITERAL+ and SASL-IR", [], "none"), ("plain, no LITERAL+, no SASL-IR", ["--no-literal-plus", "--no-sasl-ir"], "none"),
                                   ("implicit TLS", ["--tls", "--cert", cert, "--key", key], "tls"), ("STARTTLS", ["--starttls", "--cert", cert, "--key", key], "starttls")]:
        port = start("mock-imap.py", "--port", "0", "--dir", seed, *mock_args)
        r = sh(imap_check, str(port), seed, "ya29.good-token", mode, cert); record("IMAP client, " + label + ": " + r.stdout.strip().splitlines()[-1], r.returncode == 0, tail(r)); stop()

    smtp_check = os.path.join(build, "aios-smtp-check")
    for label, mock_args, mode, extra in [("plain", ["--auth-plain-text"], "none", []), ("implicit TLS", ["--tls", "--cert", cert, "--key", key], "tls", []),
                                           ("STARTTLS", ["--starttls", "--cert", cert, "--key", key], "starttls", []),
                                           ("STARTTLS, no 8BITMIME", ["--starttls", "--no-8bitmime", "--cert", cert, "--key", key], "starttls", ["no8bit"]),
                                           ("TLS, LOGIN only", ["--tls", "--no-auth-plain", "--cert", cert, "--key", key], "tls", [])]:
        out = os.path.join(work, "smtp-out"); shutil.rmtree(out, ignore_errors=True)
        port = start("mock-smtp.py", "--port", "0", "--out", out, *mock_args)
        r = sh(smtp_check, str(port), out, "ya29.good-token", mode, cert, *extra); record("SMTP client, " + label + ": " + r.stdout.strip().splitlines()[-1], r.returncode == 0, tail(r)); stop()

    port = start("mock-google.py", "--port", "0")
    r = sh(os.path.join(build, "aios-oauth-check"), str(port), os.path.join(work, "oauth")); record("OAuth 2.0 flow: " + r.stdout.strip().splitlines()[-1], r.returncode == 0, tail(r)); stop()

    gseed = os.path.join(work, "gseed"); [os.makedirs(os.path.join(gseed, d)) for d in ("INBOX", "Sent", "Trash")]
    gp = start("mock-google.py", "--port", "0", "--ttl", "3600")
    ip = start("mock-imap.py", "--port", "0", "--dir", gseed, "--token-check", "http://127.0.0.1:%d/userinfo" % gp)
    r = sh(os.path.join(build, "aios-google-sync-check"), str(gp), str(ip), os.path.join(work, "groot")); record("Google calendar, contacts and notes sync: " + r.stdout.strip().splitlines()[-1], r.returncode == 0, tail(r)); stop()

    r = sh(sys.executable, os.path.join(here, "google-e2e.py"), os.path.join(build, "aios"), os.path.join(work, "e2e")); record("through the aios command line: " + (r.stdout.strip().splitlines() or ["?"])[-1], r.returncode == 0, tail(r))
finally:
    stop()
    shutil.rmtree(work, ignore_errors=True)

failed = [n for n, ok in results if not ok]
print("\n%d checks, %d failed" % (len(results), len(failed)))
sys.exit(1 if failed else 0)
