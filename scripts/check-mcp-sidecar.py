#!/usr/bin/env python3
"""Black-box check of `aios-sidecar mcp`: spawns the real daemon and speaks MCP over stdio.

usage: check-mcp-sidecar.py <path-to-aios-sidecar>
Exit 0 only if every check passed; any failed check or protocol hang exits 1.
"""
import json, os, subprocess, sys, threading, queue, tempfile

BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "./build/aios-sidecar")
passed = failed = 0


def check(ok, name, detail=""):
    global passed, failed
    if ok:
        passed += 1
        print(f"[PASS] {name}")
    else:
        failed += 1
        print(f"[FAIL] {name}: {detail}")


class Daemon:
    def __init__(self, env_extra=None, cwd=None):
        env = dict(os.environ)
        env.pop("AIOS_CAPABILITY_KEY", None)
        env.update(env_extra or {})
        self.p = subprocess.Popen([BIN, "mcp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True, env=env, cwd=cwd)
        self.q = queue.Queue()
        threading.Thread(target=self._pump, daemon=True).start()
        self.n = 0

    def _pump(self):
        for line in self.p.stdout:
            self.q.put(line)
        self.q.put(None)

    def send_raw(self, text):
        self.p.stdin.write(text + "\n")
        self.p.stdin.flush()

    def reply(self, timeout=10):
        line = self.q.get(timeout=timeout)
        return json.loads(line) if line else None

    def rpc(self, method, params=None):
        self.n += 1
        msg = {"jsonrpc": "2.0", "id": self.n, "method": method}
        if params is not None:
            msg["params"] = params
        self.send_raw(json.dumps(msg))
        r = self.reply()
        assert r is not None and r.get("id") == self.n, f"bad reply to {method}: {r}"
        return r

    def tool(self, name, args):
        """Returns (is_error, parsed_text_or_raw, full_response)."""
        r = self.rpc("tools/call", {"name": name, "arguments": args})
        if "error" in r:
            return None, r["error"], r
        res = r["result"]
        text = res["content"][0]["text"]
        try:
            text = json.loads(text)
        except ValueError:
            pass
        return res["isError"], text, r

    def quiet(self, timeout=0.5):
        """True if the daemon sends nothing for `timeout` seconds."""
        try:
            self.q.get(timeout=timeout)
            return False
        except queue.Empty:
            return True

    def close(self):
        self.p.stdin.close()
        try:
            self.p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.p.kill()
        return self.p.returncode


def mint(d, subject, resource, rights):
    err, out, _ = d.tool("cap_mint", {"actor": "admin", "subject": subject, "resource": resource, "rights": rights})
    assert err is False, out
    return out


d = Daemon()

# ---- handshake ---------------------------------------------------------------
r = d.rpc("initialize", {"protocolVersion": "2024-11-05", "capabilities": {}, "clientInfo": {"name": "t", "version": "0"}})
res = r["result"]
check(res["protocolVersion"] == "2024-11-05" and res["serverInfo"]["name"] == "aios-mcp-sidecar"
      and res["capabilities"]["tools"]["listChanged"] is False, "initialize handshake", r)

d.send_raw(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}))
check(d.quiet(), "notification gets no reply")
check(d.rpc("ping")["result"] == {}, "ping")

tools = {t["name"]: t for t in d.rpc("tools/list")["result"]["tools"]}
expect = {"git_read_file", "git_stage_file", "git_commit_and_merge", "git_patch_dry_run",
          "git_telemetry_stats", "git_telemetry_trace", "cap_mint", "cap_verify"}
check(set(tools) == expect, "tools/list names", sorted(tools))
check(all(t["inputSchema"]["type"] == "object" for t in tools.values()), "every tool has an object schema")
check(all({"cap", "subject"} <= set(tools[n]["inputSchema"]["required"])
          for n in ("git_read_file", "git_stage_file", "git_commit_and_merge")), "git tools declare cap+subject required")

# ---- protocol errors -----------------------------------------------------------
d.send_raw("{not json")
r = d.reply()
check(r["error"]["code"] == -32700 and r["id"] is None, "parse error -32700", r)
d.send_raw("[1,2]")
check(d.reply()["error"]["code"] == -32600, "batch/non-object -> -32600")
check(d.rpc("no/such/method")["error"]["code"] == -32601, "unknown method -32601")
check(d.rpc("tools/call", {"name": "nope", "arguments": {}})["error"]["code"] == -32602, "unknown tool -32602")
check(d.rpc("tools/call", {"name": "cap_mint", "arguments": {}})["error"]["code"] == -32602, "missing args -32602")

# ---- authorization: the part that has to be real --------------------------------
SUBJ = "agent-1"
args = {"repo": "r", "path": "src/a.txt", "content": "héllo\nworld", "subject": SUBJ}

err, out, _ = d.tool("git_stage_file", dict(args))
check(err is None and out["code"] == -32602, "stage without cap is rejected", out)

err, out, _ = d.tool("git_stage_file", dict(args, cap="00" * 48))
check(err is True and "denied" in out.lower(), "stage with forged zero cap is denied", out)

err, out, _ = d.tool("git_stage_file", dict(args, cap="zz" * 48))
check(err is None and out["code"] == -32602, "non-hex cap is invalid params", out)

read_cap = mint(d, SUBJ, "repo:r/src/a.txt", 1)
err, out, _ = d.tool("git_stage_file", dict(args, cap=read_cap))
check(err is True and "denied" in out.lower(), "read-only cap cannot stage", out)

write_cap = mint(d, SUBJ, "repo:r/src/a.txt", 2)
err, out, _ = d.tool("git_stage_file", dict(args, cap=write_cap, subject="someone-else"))
check(err is True and "denied" in out.lower(), "cap is bound to its subject", out)

other = mint(d, SUBJ, "repo:r/src/other.txt", 2)
err, out, _ = d.tool("git_stage_file", dict(args, cap=other))
check(err is True and "denied" in out.lower(), "cap is bound to its resource", out)

tampered = write_cap[:-1] + ("0" if write_cap[-1] != "0" else "1")
err, out, _ = d.tool("git_stage_file", dict(args, cap=tampered))
check(err is True and "denied" in out.lower(), "tampered MAC is denied", out)

err, out, _ = d.tool("git_stage_file", dict(args, cap=write_cap))
check(err is False and out["size"] == len(args["content"].encode()) and len(out["staged_hash"]) == 64,
      "stage with a valid write cap works", out)
trace = out["trace_id"]

err, out, _ = d.tool("git_read_file", {"repo": "r", "path": "src/a.txt", "subject": SUBJ, "cap": write_cap})
check(err is True and "denied" in out.lower(), "write-only cap cannot read", out)
err, out, _ = d.tool("git_read_file", {"repo": "r", "path": "src/a.txt", "subject": SUBJ, "cap": read_cap})
check(err is False and out["content"] == args["content"], "read returns exactly what was staged (UTF-8 intact)", out)

err, out, _ = d.tool("git_read_file", {"repo": "r", "path": "missing", "subject": SUBJ,
                                       "cap": mint(d, SUBJ, "repo:r/missing", 1)})
check(err is True and "not found" in out.lower(), "missing file is an isError tool result, not a protocol error", out)

# ---- attenuate / revoke through the RPC surface ------------------------------------
r = d.rpc("cap.attenuate", {"cap": mint(d, SUBJ, "repo:r/x", 19), "rights": 3})
child = r.get("result", "")
check("result" in r and len(r["result"]) == 96, "cap.attenuate yields a child cap", r)
err, out, _ = d.tool("cap_verify", {"cap": child, "subject": SUBJ, "resource": "repo:r/x", "rights": 4})
check(err is False and out is False, "attenuated child lacks the dropped right", out)
err, out, _ = d.tool("cap_verify", {"cap": child, "subject": SUBJ, "resource": "repo:r/x", "rights": 2})
check(err is False and out is True, "attenuated child keeps the kept right", out)
rev = mint(d, "victim", "repo:r/rev", 1)
check(d.tool("cap_verify", {"cap": rev, "subject": "victim", "resource": "repo:r/rev", "rights": 1})[1] is True, "cap valid before revoke")
d.rpc("cap.revoke", {"subject": "victim", "resource": "repo:r/rev"})
check(d.tool("cap_verify", {"cap": rev, "subject": "victim", "resource": "repo:r/rev", "rights": 1})[1] is False, "cap invalid after revoke")

# ---- argument validation ---------------------------------------------------------------
for bad, why in [({"rights": 0}, "rights 0"), ({"rights": 64}, "rights > 63"), ({"rights": 65537}, "rights wraps uint16"),
                 ({"rights": -1}, "negative rights"), ({"rights": 1.5}, "fractional rights")]:
    r = d.rpc("tools/call", {"name": "cap_mint", "arguments": dict({"actor": "a", "subject": "s", "resource": "x"}, **bad)})
    check(r.get("error", {}).get("code") == -32602, f"cap_mint rejects {why}", r)
tok = mint(d, SUBJ, "repo:r/v", 1)
for bad in ({"agent_id": 70000}, {"agent_id": -1}, {"trace_id": -5}, {"trace_id": "7"}, {"trace_id": 2**60}, {"branch": 3}):
    r = d.rpc("tools/call", {"name": "git_read_file", "arguments": dict({"repo": "r", "path": "v", "subject": SUBJ, "cap": tok}, **bad)})
    check(r.get("error", {}).get("code") == -32602, f"git_read_file rejects {bad}", r)

# ---- staging, commit, merge conflict ----------------------------------------------------
def git(tool, **a):
    return d.tool(tool, a)

BR = "repo:r2:branch:main"
def stage(repo, path, content, agent):
    cap = mint(d, "ag%d" % agent, f"repo:{repo}/{path}", 2)
    return git("git_stage_file", repo=repo, path=path, content=content, subject="ag%d" % agent, cap=cap, agent_id=agent)

check(stage("r2", "f.txt", "base", 1)[0] is False, "stage base")
err, out, _ = git("git_commit_and_merge", repo="r2", branch="main", message="base",
                  subject="ag1", cap=mint(d, "ag1", BR, 1))
check(err is True and "denied" in out.lower(), "commit needs write on the branch", out)
err, out, _ = git("git_commit_and_merge", repo="r2", branch="main", message="base",
                  subject="ag1", cap=mint(d, "ag1", BR, 2))
check(err is False and len(out["commit_oid"]) == 64, "commit_and_merge returns a 64-hex oid", out)
oid1 = out["commit_oid"]

# Two agents on the same branch. Each daemon workspace is per repo:branch, so agent 2 edits via the same
# workspace; stage a change, commit, then a stale-base conflict needs a second workspace -> other branch name
# is a different workspace, so conflicts are exercised in the library tests. Here: a second commit advances the oid.
check(stage("r2", "f.txt", "second", 1)[0] is False, "stage second edit")
err, out, _ = git("git_commit_and_merge", repo="r2", branch="main", message="second", subject="ag1", cap=mint(d, "ag1", BR, 2))
check(err is False and out["commit_oid"] != oid1, "second commit has a different oid", out)

# ---- telemetry is real, per-call span ids are unique --------------------------------------------
err, out, _ = d.tool("git_telemetry_trace", {"trace_id": trace})
check(err is False and out["trace_id"] == trace and len(out["events"]) >= 1, "telemetry trace for the stage call", out)
t1 = git("git_read_file", repo="r", path="src/a.txt", subject=SUBJ, cap=read_cap, trace_id=424242)[1]
t2 = git("git_read_file", repo="r", path="src/a.txt", subject=SUBJ, cap=read_cap, trace_id=424242)[1]
err, out, _ = d.tool("git_telemetry_trace", {"trace_id": 424242})
spans = [e["span_id"] for e in out["events"]]
check(t1["trace_id"] == 424242 and len(spans) == 2 and len(set(spans)) == 2, "caller trace_id is honoured and spans are distinct", spans)
a = git("git_read_file", repo="r", path="src/a.txt", subject=SUBJ, cap=read_cap)[1]
b = git("git_read_file", repo="r", path="src/a.txt", subject=SUBJ, cap=read_cap)[1]
check(a["trace_id"] != b["trace_id"], "calls without a trace_id get distinct traces", (a["trace_id"], b["trace_id"]))
err, out, _ = d.tool("git_telemetry_stats", {"window_seconds": 3600})
check(err is False and out["ops_count"] >= 5 and out["error_count"] >= 1, "telemetry stats count real operations", out)

# denied call is visible in telemetry
git("git_stage_file", repo="r", path="den.txt", content="x", subject=SUBJ, cap="00" * 48, trace_id=777001)
err, out, _ = d.tool("git_telemetry_trace", {"trace_id": 777001})
check(err is False and any(e["status"] == 403 for e in out["events"]), "denied call recorded as 403 in telemetry", out)

# ---- binary-safe output stays valid JSON ----------------------------------------------------------
# bytes that are not UTF-8 can only get in via the file system; the escaper is exercised through patch_check reason text
# and through control characters in content:
ctl = "a\x01b\x1fc\"d\\e\tf"
check(stage("r3", "ctl.txt", ctl, 4)[0] is False, "stage content with control chars")
err, out, _ = git("git_read_file", repo="r3", path="ctl.txt", subject="ag4", cap=mint(d, "ag4", "repo:r3/ctl.txt", 1))
check(err is False and out["content"] == ctl, "control characters round-trip", out)
# a lone surrogate escape decodes to bytes that are not valid UTF-8; output must still be valid JSON (U+FFFD)
d.send_raw(json.dumps({"jsonrpc": "2.0", "id": 9001, "method": "tools/call", "params": {"name": "git_stage_file", "arguments": {
    "repo": "r3", "path": "bad.txt", "content": "A\udc00B", "subject": "ag4", "cap": mint(d, "ag4", "repo:r3/bad.txt", 2)}}}).replace("\\\\udc00", "\\udc00"))
check(d.reply()["result"]["isError"] is False, "stage content with a lone surrogate")
err, out, _ = git("git_read_file", repo="r3", path="bad.txt", subject="ag4", cap=mint(d, "ag4", "repo:r3/bad.txt", 1))
check(err is False and out["content"].startswith("A\ufffd") and out["content"].endswith("B") and "\ufffd" in out["content"], "invalid UTF-8 is replaced, output stays valid JSON", out)
big = "x" * 300000
check(stage("r3", "big.txt", big, 4)[0] is False, "stage 300KB content")
err, out, _ = git("git_read_file", repo="r3", path="big.txt", subject="ag4", cap=mint(d, "ag4", "repo:r3/big.txt", 1))
check(err is False and out["content"] == big, "300KB content round-trips", None if err is False else out)

# ---- patch dry run: works, and cannot be pointed outside the working root ---------------------------
with tempfile.TemporaryDirectory() as td:
    d2 = Daemon(cwd=td)
    open(os.path.join(td, "t.txt"), "w").write("one\ntwo\nthree\n")
    diff = "--- a/t.txt\n+++ b/t.txt\n@@ -1,3 +1,3 @@\n one\n-two\n+TWO\n three\n"
    err, out, _ = d2.tool("git_patch_dry_run", {"diff": diff})
    check(err is False and out["can_apply"] is True and out["hunks"] == 1, "patch dry run applies", out)
    bad = diff.replace("-two", "-nope")
    err, out, _ = d2.tool("git_patch_dry_run", {"diff": bad})
    check(err is False and out["can_apply"] is False, "patch dry run reports a non-applying hunk", out)
    check(open(os.path.join(td, "t.txt")).read() == "one\ntwo\nthree\n", "dry run wrote nothing")
    err, out, _ = d2.tool("git_patch_dry_run", {"diff": diff, "root": "/etc"})
    check(err is None and out["code"] == -32602, "patch root outside the working root is refused", out)
    err, out, _ = d2.tool("git_patch_dry_run", {"diff": diff, "root": ".."})
    check(err is None and out["code"] == -32602, "patch root '..' is refused", out)
    d2.close()

# ---- one trust root: a shared key makes caps verify across processes ------------------------------------
KEY = {"AIOS_CAPABILITY_KEY": "test-key-shared-between-two-daemons"}
da, db = Daemon(KEY), Daemon(KEY)
cap = mint(da, "s", "repo:z/f", 1)
check(db.tool("cap_verify", {"cap": cap, "subject": "s", "resource": "repo:z/f", "rights": 1})[1] is True,
      "cap minted by one daemon verifies in another with the same key")
dr = Daemon()
check(dr.tool("cap_verify", {"cap": cap, "subject": "s", "resource": "repo:z/f", "rights": 1})[1] is False,
      "without the key, caps from another process are rejected (no hardcoded default key)")
da.close(); db.close(); dr.close()

# ---- the CLI modes named in the docs ----------------------------------------------------------------------------
cp = subprocess.run([BIN, "telemetry", "stats", "60"], capture_output=True, text=True)
check(cp.returncode == 0 and "Total Operations" in cp.stdout, "telemetry stats CLI")
cp = subprocess.run([BIN, "telemetry", "trace", "1"], capture_output=True, text=True)
check(cp.returncode == 0 and cp.stdout.strip().startswith("{"), "telemetry trace CLI")

# ---- every advertised method dispatches ---------------------------------------------------------------------------
methods = d.rpc("methods")["result"]
missing = []
for m in methods:
    if m == "methods":
        continue
    r = d.rpc(m, {})
    if r.get("error", {}).get("code") == -32601:
        missing.append(m)
check(not missing, "every method in `methods` is dispatched", missing)

rc = d.close()
check(rc == 0, "daemon exits 0 on stdin EOF", rc)

print(f"\n  {passed + failed} TOTAL   {passed} PASSED   {failed} FAILED")
sys.exit(0 if failed == 0 else 1)
