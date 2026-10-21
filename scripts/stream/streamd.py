#!/usr/bin/env python3
"""streamd: agents write one live tree; every edit becomes unit operations in ASQL, in one global order.

    python3 scripts/stream/streamd.py serve [--root DIR] [--port 8790]     the daemon
    python3 scripts/stream/streamd.py log [N]                              the newest operations
    python3 scripts/stream/streamd.py unit PATH SCOPE                      one unit's history, oldest first
    python3 scripts/stream/streamd.py agents                               who has written

No forks, branches, claims or merges. Each agent's Claude Code hook (scripts/stream/hook.py) tells the daemon when it is
about to edit a file (the daemon keeps the file as it was) and when it has (the daemon diffs the two with aios-unit-diff:
functions and methods whole, classes and namespaces by their own text). Each changed unit is one row of
aios_stream.ops on the ASQL server, numbered by its AUTO_INCREMENT: that sequence is the timeline. The live tree is the
latest write to each unit; nothing is ever rolled back.

When a write replaces a unit another agent wrote last, both are told on their next tool call (the hook returns it as
context): the one overwritten gets the unit as it is now and the writer's trace on it, the writer gets what it replaced.
C++ a write touched is type-checked (g++ -fsyntax-only, in the machine's build slots at idle priority); a break goes to
the writer as a notice, and so does its repair. Nothing waits on a check and nothing is refused.

Edits made outside the hooks (an editor, sed -i, another tool) are found at the next hooked edit of that file, by
comparing what the daemon last saw with the file before the edit, and are recorded as agent "outside".
"""
import collections, hashlib, json, os, queue, subprocess, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import pymysql

ROOT = os.environ.get("STREAM_ROOT", "/repos/aios-qbc")
PORT = int(os.environ.get("STREAM_PORT", "8790"))
DB = dict(host="127.0.0.1", port=int(os.environ.get("STREAM_DB_PORT", "3307")), user="root", database="aios_stream", autocommit=True)
UNIT_DIFF = os.environ.get("STREAM_UNIT_DIFF", ROOT + "/build/aios-unit-diff")
SLOT = os.environ.get("STREAM_SLOT", ROOT + "/scripts/build-slot.sh")
CXX = ("-std=c++20 -fsyntax-only -w -Iinclude -Ithird_party/tree-sitter/lib/include -I/usr/include/SDL2 -DAIOS_HAS_SDL2=1 "
       "-DAIOS_HAVE_LIBDEFLATE=1 -DAIOS_HAVE_ZLIB=1 -DAIOS_HAVE_ZSTD=1").split()
TEXT_LIMIT = 64 << 10          # a unit's text kept in the row (the file itself is the truth; this is history)
STATE = os.path.expanduser("~/.aios-stream")

SCHEMA = [
    "CREATE DATABASE IF NOT EXISTS aios_stream",
    """CREATE TABLE IF NOT EXISTS aios_stream.ops (seq BIGINT AUTO_INCREMENT PRIMARY KEY, at_ms BIGINT NOT NULL,
       agent VARCHAR(64) NOT NULL, path VARCHAR(512) NOT NULL, scope VARCHAR(1024) NOT NULL, kind VARCHAR(64), how CHAR(1),
       line INT, end_line INT, before_text MEDIUMTEXT, after_text MEDIUMTEXT, over_agent VARCHAR(64), over_seq BIGINT)""",
    """CREATE TABLE IF NOT EXISTS aios_stream.notices (id BIGINT AUTO_INCREMENT PRIMARY KEY, at_ms BIGINT NOT NULL,
       agent VARCHAR(64) NOT NULL, kind VARCHAR(32) NOT NULL, body MEDIUMTEXT NOT NULL, delivered TINYINT NOT NULL DEFAULT 0)""",
    """CREATE TABLE IF NOT EXISTS aios_stream.agents (agent VARCHAR(64) PRIMARY KEY, name VARCHAR(128), session VARCHAR(128),
       first_ms BIGINT, last_ms BIGINT, landed_seq BIGINT)""",
]

def _safe(f, *a):
    try: f(*a)
    except Exception as e: log(f"{f.__name__} failed: {e!r}")

def now_ms():
    return int(time.time() * 1000)

def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)

class Store:
    """ASQL through the MySQL wire, one connection per thread."""
    def __init__(self):
        self.local = threading.local()
    def conn(self):
        c = getattr(self.local, "c", None)
        if c is None or not c.open:
            args = dict(DB); args.pop("database")
            c = pymysql.connect(**args)
            self.local.c = c
        return c
    def run(self, sql, args=None):
        for attempt in range(3):
            try:
                with self.conn().cursor() as cur:
                    cur.execute(sql, args)
                    return cur.fetchall(), cur.lastrowid
            except pymysql.err.OperationalError:
                self.local.c = None
                if attempt == 2: raise
                time.sleep(0.5)

db = Store()

# ---------------------------------------------------------------- the files as the daemon last saw them

class Seen:
    """path -> the text the daemon last saw (after the last hooked edit); first sight is git HEAD's copy."""
    def __init__(self):
        self.text = {}
        self.version = collections.Counter()   # rel -> how many times it was put: did someone write since an agent began?
        self.lock = threading.Lock()
    # Kept on disk too (STATE/seen/<sha1 of path>), so a restarted daemon diffs against what it last saw, not git HEAD
    # (which would credit every write since the last commit to "outside").
    def disk(self, rel):
        return os.path.join(STATE, "seen", hashlib.sha1(rel.encode()).hexdigest())
    def get(self, rel):
        with self.lock:
            if rel in self.text: return self.text[rel]
        try:
            with open(self.disk(rel), "rb") as f:
                data = f.read()
            with self.lock: self.text[rel] = data
            return data
        except OSError:
            pass
        r = subprocess.run(["git", "-C", ROOT, "show", "HEAD:" + rel], capture_output=True)
        t = r.stdout if r.returncode == 0 else None
        return t
    def put(self, rel, data):
        with self.lock:
            self.text[rel] = data
            self.version[rel] += 1
        try:
            os.makedirs(os.path.dirname(self.disk(rel)), exist_ok=True)
            tmp = self.disk(rel) + ".tmp"
            with open(tmp, "wb") as f: f.write(data)
            os.replace(tmp, self.disk(rel))
        except OSError:
            pass
    def ver(self, rel):
        with self.lock: return self.version[rel]

seen = Seen()
pre = {}                 # (agent, rel) -> bytes as the file was when the agent's edit began
pre_lock = threading.Lock()

def read(rel):
    try:
        with open(os.path.join(ROOT, rel), "rb") as f: return f.read()
    except OSError:
        return b""

def unit_diff(rel, before, after):
    """aios-unit-diff on two versions: the changed units."""
    d = os.path.join(STATE, "tmp"); os.makedirs(d, exist_ok=True)
    tag = hashlib.sha1((rel + str(threading.get_ident())).encode()).hexdigest()[:12]
    a, b = os.path.join(d, tag + ".a"), os.path.join(d, tag + ".b")
    with open(a, "wb") as f: f.write(before or b"")
    with open(b, "wb") as f: f.write(after or b"")
    try:
        r = subprocess.run(["nice", "-n", "10", UNIT_DIFF, rel, a, b], capture_output=True, timeout=60)
        if r.returncode != 0: return []
        return json.loads(r.stdout.decode("utf-8", "replace")).get("changes", [])
    except Exception as e:
        log("unit-diff failed on", rel, e)
        return []
    finally:
        for p in (a, b):
            try: os.remove(p)
            except OSError: pass

# ---------------------------------------------------------------- agents

tag_cache = {}           # session id -> agent id

def first_prompt(path):
    """The session's first user message: where Avalon's tag and a brief's "You are agent ..." are (later text, a
    conversation about the stream itself, may quote both). The first three, since a reminder block can come first."""
    got = []
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                try: j = json.loads(line)
                except ValueError: continue
                if j.get("type") != "user": continue
                c = (j.get("message") or {}).get("content")
                if isinstance(c, list): c = " ".join(x.get("text", "") for x in c if isinstance(x, dict))
                if isinstance(c, str) and c.strip():
                    got.append(c[:20000])
                    if len(got) == 3: break
    except OSError:
        pass
    return "\n".join(got)

def agent_of(info):
    """An agent is a task, named by the tag Avalon puts on every turn in stream mode ("[stream agent Q1]"), so a task's
    turns (each may be a new CLI session) are one agent. Without a tag: the name its first prompt gives ("You are agent
    Q1"), else the session itself."""
    import re
    sid = info.get("session_id") or "unknown"
    if info.get("agent"):    # Avalon reporting for an agent without hooks (Gemini): it names the agent itself
        aid = str(info["agent"])[:40]
        rows, _ = db.run("SELECT name FROM aios_stream.agents WHERE agent=%s", (aid,))
        if rows: db.run("UPDATE aios_stream.agents SET last_ms=%s WHERE agent=%s", (now_ms(), aid))
        else: db.run("INSERT INTO aios_stream.agents (agent,name,session,first_ms,last_ms) VALUES (%s,%s,%s,%s,%s)", (aid, aid, sid, now_ms(), now_ms()))
        return aid, (rows[0][0] if rows else aid)
    if sid in tag_cache:
        aid = tag_cache[sid]
        db.run("UPDATE aios_stream.agents SET last_ms=%s WHERE agent=%s", (now_ms(), aid))
        return aid, name_of(aid)
    rows, _ = db.run("SELECT agent FROM aios_stream.agents WHERE session=%s", (sid,))
    if rows:                 # a session the stream already knows keeps its agent (the lead's own session among them)
        tag_cache[sid] = rows[0][0]
        return rows[0][0], name_of(rows[0][0])
    head = first_prompt(info.get("transcript_path") or "")
    m = re.search(r"\[stream agent ([A-Za-z0-9_.-]{1,40})\]", head)
    if m:
        aid = name = m.group(1)
    else:
        aid = "s-" + sid[:8]
        m = re.search(r"\byou are agent ([A-Za-z]+[0-9]+)", head, re.I)
        name = m.group(1) if m else aid
    if not head and aid.startswith("s-"):
        return aid, aid          # no transcript yet: don't pin a name it may get on the next call
    tag_cache[sid] = aid
    rows, _ = db.run("SELECT name FROM aios_stream.agents WHERE agent=%s", (aid,))
    if rows:
        db.run("UPDATE aios_stream.agents SET last_ms=%s, session=%s WHERE agent=%s", (now_ms(), sid, aid))
        return aid, rows[0][0]
    db.run("INSERT INTO aios_stream.agents (agent,name,session,first_ms,last_ms) VALUES (%s,%s,%s,%s,%s)", (aid, name, sid, now_ms(), now_ms()))
    return aid, name

def name_of(aid):
    rows, _ = db.run("SELECT name FROM aios_stream.agents WHERE agent=%s", (aid,))
    return rows[0][0] if rows else aid

NOTIFY_AGENTS = os.environ.get("STREAM_NOTIFY_AGENTS") == "1"

def notify(aid, kind, body):
    """Off by default: agents see their own compile errors when they build, and the tree lands either way, so nothing
    is sent into an agent's context (no tokens spent on it). The event stays in the daemon's log for the owner."""
    log(f"[{kind}] {name_of(aid)}: " + body.split("\n")[0][:200])
    if NOTIFY_AGENTS:
        db.run("INSERT INTO aios_stream.notices (at_ms,agent,kind,body) VALUES (%s,%s,%s,%s)", (now_ms(), aid, kind, body))

def pull(aid):
    rows, _ = db.run("SELECT id,kind,body FROM aios_stream.notices WHERE agent=%s AND delivered=0 ORDER BY id", (aid,))
    if rows:
        db.run("UPDATE aios_stream.notices SET delivered=1 WHERE agent=%s AND id<=%s AND delivered=0", (aid, rows[-1][0]))
    return [(k, b) for _, k, b in rows]

# ---------------------------------------------------------------- operations

def clip(s):
    s = s or ""
    return s if len(s) <= TEXT_LIMIT else s[:TEXT_LIMIT] + "\n... (%d more bytes)" % (len(s) - TEXT_LIMIT)

def record(aid, rel, before, after, label):
    """The units that changed from before to after, as operations by aid; overwrites of another agent's unit noticed."""
    changes = unit_diff(rel, before, after)
    if len(changes) > 40:
        # A misread region of a huge file shifts many units at once: one file-level operation, nobody "overwritten".
        b, a = (before or b"").decode("utf-8", "replace"), (after or b"").decode("utf-8", "replace")
        changes = [{"scope": "", "kind": "file", "how": "~", "line": 1, "end_line": 0, "before": "", "after": "",
                    "note": f"{len(changes)} units"}]
        aid_file_level = True
    seqs = []
    for c in changes:
        scope = c["scope"]
        if scope == "" and c.get("kind") == "file" and not c.get("before") and not c.get("after") and len(changes) > 1:
            continue   # the file's own text moved with the units: no operation of its own
        last, _ = db.run("SELECT seq,agent,before_text,after_text FROM aios_stream.ops WHERE path=%s AND scope=%s ORDER BY seq DESC LIMIT 1",
                         (rel, scope))
        over_agent = over_seq = None
        if (last and last[0][1] != aid and last[0][1] not in ("outside", "shared") and aid not in ("outside", "shared")
                and scope != "" and not survives(last[0][2], last[0][3], c.get("after"))):
            over_seq, over_agent = last[0][0], last[0][1]
        _, seq = db.run("""INSERT INTO aios_stream.ops (at_ms,agent,path,scope,kind,how,line,end_line,before_text,after_text,over_agent,over_seq)
                           VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s)""",
                        (now_ms(), aid, rel, scope, c.get("kind"), c.get("how"), c.get("line"), c.get("end_line"),
                         clip(c.get("before")), clip(c.get("after")), over_agent, over_seq))
        seqs.append(seq)
        if over_agent:
            overwritten(aid, over_agent, over_seq, seq, rel, scope, c)
    if seqs: log(f"{label} {name_of(aid)} {rel}: {len(seqs)} op(s) #{seqs[0]}..#{seqs[-1]}")
    return seqs

def survives(prev_before, prev_after, now):
    """Is the previous writer's change still in the unit? Their added lines all still there: then this write edited
    beside it (two agents in one function, each Edit applied to the live text) and nothing of theirs was lost."""
    if prev_after is None or now is None: return False
    if "\n... (" in (prev_after or "") or "\n... (" in (prev_before or ""): return True   # clipped: cannot tell, do not cry wolf
    had = collections.Counter(l.strip() for l in (prev_before or "").split("\n") if l.strip())
    added = collections.Counter(l.strip() for l in prev_after.split("\n") if l.strip()) - had
    if not added: return True     # they only deleted: a deletion others wrote beside is not lost work
    have = collections.Counter(l.strip() for l in now.split("\n") if l.strip())
    return all(have[l] >= n for l, n in added.items())

def trace(rel, scope, aid, limit=4):
    rows, _ = db.run("""SELECT seq,how,after_text FROM aios_stream.ops WHERE path=%s AND scope=%s AND agent=%s
                        ORDER BY seq DESC LIMIT %s""", (rel, scope, aid, limit))
    out = []
    for seq, how, after in reversed(rows):
        out.append(f"  #{seq} {how}: " + ((after or "(deleted)")[:1500]))
    return "\n".join(out)

def overwritten(writer, owner, owner_seq, seq, rel, scope, c):
    w, o = name_of(writer), name_of(owner)
    where = f"{rel} `{scope or '(file text)'}`"
    notify(owner, "overwritten",
           f"[stream #{seq}] {w} replaced your {where} (yours was #{owner_seq}). It is now:\n{clip(c.get('after')) or '(deleted)'}\n"
           f"{w}'s writes to this unit, oldest first:\n{trace(rel, scope, writer)}\n"
           "Build on it as it is now; if your change still matters, write it again on top of this.")
    notify(writer, "replaced",
           f"[stream #{seq}] your write to {where} replaced {o}'s version (#{owner_seq}), which was:\n{clip(c.get('before')) or '(none)'}\n"
           "Keep what of theirs still matters.")

# ---------------------------------------------------------------- the compile signal

checks = queue.Queue()
pending = {}             # rel -> (aid, due_ms)
pending_lock = threading.Lock()
broken = {}              # tu -> (aid, first error line)

def includers(rel):
    base = os.path.basename(rel)
    r = subprocess.run(["git", "-C", ROOT, "grep", "-l", "-E", r'#\s*include\s*["<]([^">]*/)?' + base.replace(".", r"\.") + '[">]',
                        "--", "*.cpp"], capture_output=True, text=True)
    return [f for f in r.stdout.split() if not f.startswith(("third_party/", "build/"))][:12]

def schedule(aid, rel):
    if not rel.endswith((".cpp", ".cc", ".hpp", ".h", ".inc", ".ipp")) or rel.startswith(("third_party/", "build/")): return
    with pending_lock: pending[rel] = (aid, now_ms() + 3000)    # quiet for 3 s: an agent's several edits are one check

def check_loop():
    while True:
        time.sleep(1)
        due = []
        with pending_lock:
            for rel, (aid, t) in list(pending.items()):
                if t <= now_ms(): due.append((rel, aid)); del pending[rel]
        for rel, aid in due:
            tus = [rel] if rel.endswith((".cpp", ".cc")) else includers(rel)
            for tu in tus:
                if not os.path.exists(os.path.join(ROOT, tu)): continue
                r = subprocess.run([SLOT, "g++"] + CXX + [tu], cwd=ROOT, capture_output=True, text=True)
                errs = [l for l in r.stderr.split("\n") if "error" in l][:8]
                if r.returncode != 0:
                    first = errs[0] if errs else r.stderr.strip().split("\n")[0]
                    if broken.get(tu, (None, None))[1] != first:
                        notify(aid, "compile", f"[stream] {tu} no longer compiles after your write to {rel}:\n" + "\n".join(errs) +
                               "\nFix it forward (nothing was rolled back; the tree holds your write).")
                        log(f"compile break {tu} after {name_of(aid)}: {first[:120]}")
                    broken[tu] = (aid, first)
                elif tu in broken:
                    was, _ = broken.pop(tu)
                    notify(aid, "compile", f"[stream] {tu} compiles again.")
                    if was != aid: notify(was, "compile", f"[stream] {tu} compiles again ({name_of(aid)}'s write fixed it).")
                    log(f"compile ok {tu} again")

# ---------------------------------------------------------------- the hooks' side

def rel_of(path):
    if not path: return None
    p = os.path.realpath(path)
    root = os.path.realpath(ROOT)
    if not p.startswith(root + "/"): return None
    rel = p[len(root) + 1:]
    if rel.startswith((".git/", "build/")) or "/node_modules/" in rel: return None
    return rel

EDIT_TOOLS = {"Edit", "Write", "MultiEdit", "NotebookEdit"}

# Who wrote what comes from Avalon, which sees every agent's tool calls and reports each file an edit tool wrote. A file a
# shell command changed is not named by any tool call: it lands with the tree at the next turn end all the same.

def changed_files():
    r = subprocess.run(["git", "-C", ROOT, "status", "--porcelain=v1", "-z", "--untracked-files=all"], capture_output=True)
    out, parts, i = [], r.stdout.split(b"\0"), 0
    while i < len(parts):
        e = parts[i]; i += 1
        if len(e) < 4: continue
        code, path = e[:2], e[3:].decode("utf-8", "replace")
        if code[:1] == b"R": i += 1          # a rename's old name follows
        rel = rel_of(os.path.join(ROOT, path))
        if rel and os.path.isfile(os.path.join(ROOT, rel)): out.append(rel)
    return out

def engine_trees():
    """Every agent Avalon's engine runs (claude, agy, gemini... and all they start), at nice 19 / idle I/O. An agent is
    whatever runs under the engine's node process (its command line names Avalon's engine/); the person's own sessions,
    and stray processes outside the engine, are left alone."""
    kids = collections.defaultdict(list); roots = []
    for d in os.listdir("/proc"):
        if not d.isdigit(): continue
        try:
            with open(f"/proc/{d}/stat") as f: kids[int(f.read().rsplit(")", 1)[1].split()[1])].append(int(d))
            with open(f"/proc/{d}/cmdline", "rb") as f: cmd = f.read()
            if b"Avalon.App/bin" in cmd and b"/engine/" in cmd: roots.append(int(d))
        except (OSError, IndexError, ValueError): pass
    for r in roots:
        frontier = list(kids.get(r, []))
        while frontier:
            p = frontier.pop()
            frontier += kids.get(p, [])
            try:
                if os.getpriority(os.PRIO_PROCESS, p) < 19:
                    os.setpriority(os.PRIO_PROCESS, p, 19)
                    subprocess.run(["ionice", "-c", "3", "-p", str(p)], capture_output=True)
            except OSError: pass

def sweep_loop():
    """Writes no agent's tool call covered (the owner's editor, a script): recorded as "outside" once they settle.
    And the engine's agents idled."""
    while True:
        time.sleep(20)
        try: engine_trees()
        except Exception as e: log("idle failed", repr(e))


niced = set()

def idle(pid, name):
    """An agent's claude process and everything under it at nice 19 / idle I/O: its builds, runs and tests inherit it.
    The lead's own session (the person's) is left alone."""
    if not pid or pid in niced or name == "lead": return
    niced.add(pid)
    tree, frontier = [pid], [pid]
    kids = collections.defaultdict(list)
    for d in os.listdir("/proc"):
        if d.isdigit():
            try:
                with open(f"/proc/{d}/stat") as f: kids[int(f.read().rsplit(")", 1)[1].split()[1])].append(int(d))
            except OSError: pass
    while frontier:
        nxt = []
        for p in frontier: nxt += kids.get(p, [])
        tree += nxt; frontier = nxt
    for p in tree:
        try: os.setpriority(os.PRIO_PROCESS, p, 19)
        except OSError: pass
        subprocess.run(["ionice", "-c", "3", "-p", str(p)], capture_output=True)
    log(f"{name}: claude {pid} and {len(tree) - 1} process(es) under it at nice 19 / idle I/O")

# ---------------------------------------------------------------- landing: an agent's finished turn is a commit on the branch

land_lock = threading.Lock()

def git(*args, env=None, check=True):
    r = subprocess.run(["git", "-C", ROOT, *args], capture_output=True, text=True, env={**os.environ, **(env or {})})
    if check and r.returncode != 0: raise RuntimeError("git " + " ".join(args[:3]) + ": " + (r.stderr or r.stdout).strip()[:300])
    return r.stdout.strip()

def land(aid, name):
    """An agent's turn ended: the tree as it is now lands on the checked-out branch, everything changed in it, whoever
    wrote it (the stream's attribution is for notices and history; the tree is the truth). A private index from HEAD
    takes the changed paths, the branch moves by compare-and-swap, and the real index takes those paths from the new
    HEAD, so nobody's staging is disturbed. Forward only: a write that does not compile lands too; its fix lands after."""
    with land_lock:
        tracked = set(git("ls-files", "-m", "-d", check=False).split("\n"))
        # new files land when they are source (an agent's scratch output in the repo root does not)
        paths = [p for p in changed_files() if p in tracked or p.startswith(("src/", "include/", "tools/", "scripts/", "docs/", "cli/", "metal/", "third_party/"))]
        if not paths: return
        rows, _ = db.run("SELECT COALESCE(MAX(seq),0) FROM aios_stream.ops")
        top = rows[0][0]
        rows, _ = db.run("SELECT COALESCE(MAX(landed_seq),0) FROM aios_stream.agents")
        since = rows[0][0] or 0
        who, _ = db.run("""SELECT COALESCE(a.name,o.agent), COUNT(*) FROM aios_stream.ops o LEFT JOIN aios_stream.agents a ON a.agent=o.agent
                           WHERE o.seq>%s GROUP BY 1 ORDER BY 2 DESC""", (since,))
        for attempt in range(5):
            head = git("rev-parse", "HEAD")
            idx = os.path.join(STATE, "land.index")
            env = {"GIT_INDEX_FILE": idx}
            try:
                git("read-tree", head, env=env)
                git("add", "-A", "--", *paths, env=env)
                tree = git("write-tree", env=env)
            finally:
                try: os.remove(idx)
                except OSError: pass
            if tree == git("rev-parse", head + "^{tree}"): return
            msg = (f"{', '.join(paths[:4])}{' ...' if len(paths) > 4 else ''}: {len(paths)} file(s) from the stream\n\n"
                   + "\n".join(f"- {w}: {n} unit write(s)" for w, n in who)
                   + f"\n\nstream ops #{since + 1}..#{top} (scripts/stream/streamd.py log)"
                   + f"\n\nStream-Agent: {name}")   # Avalon shows this commit as that task's landing
            # The repo's own identity (user.name / user.email) and, when the repo signs (commit.gpgsign with
            # user.signingkey), its key: the landing is the owner's commit; the agents are named in the message only.
            sign = ["-S"] if git("config", "--bool", "commit.gpgsign", check=False) == "true" else []
            commit = git("commit-tree", *sign, tree, "-p", head, "-m", msg)
            if subprocess.run(["git", "-C", ROOT, "update-ref", "-m", f"stream: {name} landed", "HEAD", commit, head],
                              capture_output=True).returncode != 0:
                time.sleep(0.5); continue
            for _ in range(10):
                if subprocess.run(["git", "-C", ROOT, "reset", "-q", "--", *paths], capture_output=True).returncode == 0: break
                time.sleep(0.5)
            db.run("UPDATE aios_stream.agents SET landed_seq=%s WHERE agent=%s", (top, aid))
            log(f"landed {name}: {commit[:9]} ({len(paths)} file(s), ops #{since + 1}..#{top})")
            notify(aid, "landed", f"[stream] the tree landed on the branch as {commit[:9]} at your turn end ({len(paths)} file(s)).")
            return
        log(f"land {name}: the branch kept moving, will land at the next turn end")

def on_hook(info):
    aid, name = agent_of(info)
    idle(info.get("claude_pid"), name)
    event = info.get("hook_event_name", "")
    tool = info.get("tool_name", "")
    path = (info.get("tool_input") or {}).get("file_path") or (info.get("tool_input") or {}).get("notebook_path")
    rel = rel_of(path) if tool in EDIT_TOOLS else None
    if rel and event == "PreToolUse":
        cur = read(rel)
        last = seen.get(rel)
        if last is not None and last != cur:
            threading.Thread(target=record, args=("outside", rel, last, cur, "outside"), daemon=True).start()
        elif last is None and cur:
            seen.put(rel, cur)
        seen.put(rel, cur)
        with pre_lock: pre[(aid, rel)] = (cur, seen.ver(rel))
    elif rel and event == "PostToolUse":
        with pre_lock: snap = pre.pop((aid, rel), None)
        # The file just before this write: the agent's own snapshot, unless another write landed on it since (the edit
        # applied to the file as it was then, so that newer version is this write's base, and the other write stays theirs).
        before = snap[0] if snap and seen.ver(rel) == snap[1] else (seen.get(rel) or b"")
        after = read(rel)
        seen.put(rel, after)
        if before != after:
            threading.Thread(target=record, args=(aid, rel, before, after, tool), daemon=True).start()
            schedule(aid, rel)
    if event == "Stop" and name != "lead":
        def finish():
            time.sleep(3)                       # the turn's last writes are still being recorded
            land(aid, name)
        threading.Thread(target=lambda: _safe(finish), daemon=True).start()
    notes = pull(aid) if event == "PostToolUse" else []    # only a PostToolUse hook can hand the agent context
    return {"agent": aid, "name": name, "notices": [b for _, b in notes]}

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_POST(self):
        try:
            n = int(self.headers.get("content-length") or 0)
            info = json.loads(self.rfile.read(n) or b"{}")
            out = on_hook(info) if self.path == "/hook" else {"error": "unknown"}
            body = json.dumps(out).encode()
            self.send_response(200)
        except Exception as e:
            log("hook error", repr(e))
            body = json.dumps({"error": str(e)}).encode()
            self.send_response(500)
        self.send_header("content-type", "application/json"); self.send_header("content-length", str(len(body))); self.end_headers()
        self.wfile.write(body)
    def do_GET(self):
        body = json.dumps({"ok": True, "root": ROOT}).encode()
        self.send_response(200); self.send_header("content-length", str(len(body))); self.end_headers(); self.wfile.write(body)

def serve():
    for s in SCHEMA:
        db.run(s)
    os.makedirs(STATE, exist_ok=True)
    # Files the stream has seen but holds no copy of (a daemon from before copies were kept): the tree as it is now.
    rows, _ = db.run("SELECT DISTINCT path FROM aios_stream.ops")
    for (rel,) in rows:
        if not os.path.exists(seen.disk(rel)) and os.path.exists(os.path.join(ROOT, rel)): seen.put(rel, read(rel))
    threading.Thread(target=check_loop, daemon=True).start()
    threading.Thread(target=sweep_loop, daemon=True).start()
    log(f"streamd: {ROOT} on 127.0.0.1:{PORT}, operations in aios_stream on :{DB['port']}")
    ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()

# ---------------------------------------------------------------- reading the timeline

def cli(argv):
    cmd = argv[0] if argv else "log"
    if cmd == "serve":
        global ROOT, PORT, UNIT_DIFF, SLOT
        if "--root" in argv:
            ROOT = os.path.realpath(argv[argv.index("--root") + 1])
            UNIT_DIFF = os.environ.get("STREAM_UNIT_DIFF", ROOT + "/build/aios-unit-diff")
            SLOT = os.environ.get("STREAM_SLOT", ROOT + "/scripts/build-slot.sh")
        if "--port" in argv: PORT = int(argv[argv.index("--port") + 1])
        return serve()
    if cmd == "log":
        n = int(argv[1]) if len(argv) > 1 else 30
        rows, _ = db.run("""SELECT o.seq, o.at_ms, COALESCE(a.name,o.agent), o.how, o.path, o.scope, o.over_agent FROM aios_stream.ops o
                            LEFT JOIN aios_stream.agents a ON a.agent=o.agent ORDER BY o.seq DESC LIMIT %s""", (n,))
        for seq, at, who, how, path, scope, over in reversed(rows):
            print(f"#{seq:<6} {time.strftime('%H:%M:%S', time.localtime(at / 1000))} {who:<10} {how} {path} {scope}" + (f"   (over {name_of(over)})" if over else ""))
    elif cmd == "unit":
        rows, _ = db.run("SELECT seq,agent,how,after_text FROM aios_stream.ops WHERE path=%s AND scope=%s ORDER BY seq", (argv[1], argv[2]))
        for seq, aid, how, after in rows:
            print(f"#{seq} {name_of(aid)} {how}\n{after}\n")
    elif cmd == "agents":
        rows, _ = db.run("""SELECT a.agent,a.name,COUNT(o.seq),MAX(o.seq) FROM aios_stream.agents a LEFT JOIN aios_stream.ops o ON o.agent=a.agent
                            GROUP BY a.agent,a.name ORDER BY a.agent""")
        for aid, name, n, last in rows: print(f"{name:<10} {aid}  {n} ops, last #{last}")
    else:
        print(__doc__)

if __name__ == "__main__":
    cli(sys.argv[1:])
