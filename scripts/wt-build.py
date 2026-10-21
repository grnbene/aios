#!/usr/bin/env python3
"""Incremental build of the web engine tools in a checkout that is not the main one (a fork, a worktree).

    python3 scripts/wt-build.py [checkout] [target ...]     checkout defaults to the current directory
                                                            targets default to aios-web-pane-drive aios-web-layout-drive

The main checkout (/repos/aios-qbc) has a full build. Here, only the objects whose source, or any header they include,
differs from the main checkout's copy are recompiled, into /tmp/aios-lite/<checkout name>/; every other object is the main
build's. A private libaios.a is assembled from the mix and the drive tools are linked against it, so edit-build-run is
seconds, not the ten minutes of a full build.

Rules: do not add new .cpp files (CMake would not know them); a header edit recompiles every object that includes it,
which takes a few minutes. Output: /tmp/aios-lite/<name>/<target>, printed at the end.
"""
import concurrent.futures, filecmp, os, shlex, subprocess, sys

MAIN = "/repos/aios-qbc"
B = MAIN + "/build"

args = sys.argv[1:]
wt = os.path.abspath(args.pop(0)) if args and os.path.isdir(args[0]) else os.getcwd()
targets = args or ["aios-web-pane-drive", "aios-web-layout-drive"]
name = os.path.basename(wt.rstrip("/")) or "checkout"
lite = f"/tmp/aios-lite/{name}"
os.makedirs(lite, exist_ok=True)

# What the main build was made from: the commit recorded in build/BUILD_SHA when it finished (written by scripts/wt-main-build.sh).
# The baseline is THAT commit's file contents, not the main checkout's files -- Avalon keeps merging landed work into the main
# checkout, so a build made earlier no longer matches it, and comparing against the working files made a fork link stale objects.
BASE = open(B + "/BUILD_SHA").read().strip() if os.path.exists(B + "/BUILD_SHA") else ""
import hashlib
_blobs = {}
if BASE:
    for line in subprocess.run(["git", "-C", MAIN, "ls-tree", "-r", BASE], capture_output=True, text=True).stdout.splitlines():
        meta, path = line.split("\t", 1)
        _blobs[path] = meta.split()[2]
_diff = {}
def differs(rel):
    """Is this file different from what the main build was made from?"""
    if rel not in _diff:
        a = f"{wt}/{rel}"
        if not os.path.exists(a) or rel not in _blobs:
            _diff[rel] = True
        else:
            data = open(a, "rb").read()
            _diff[rel] = hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest() != _blobs[rel]
    return _diff[rel]

def flags_of(dirname):
    out = {}
    for line in open(f"{B}/CMakeFiles/{dirname}/flags.make"):
        if " = " in line and line.split(" = ")[0] in ("CXX_DEFINES", "CXX_INCLUDES", "CXX_FLAGS"):
            k, v = line.rstrip("\n").split(" = ", 1)
            out[k] = v.replace(MAIN, wt)
    return " ".join(out.get(k, "") for k in ("CXX_DEFINES", "CXX_INCLUDES", "CXX_FLAGS"))

def deps_of(dfile):
    try:
        txt = open(dfile).read().replace("\\\n", " ")
    except OSError:
        return []
    return [t for t in txt.split()[1:] if t.startswith(MAIN + "/")]

def needed(rel_src, main_d, lite_d):
    ds = deps_of(lite_d) if os.path.exists(lite_d) else deps_of(main_d)
    return any(differs(d[len(MAIN) + 1:]) for d in [MAIN + "/" + rel_src] + ds)

jobs, replaced = [], {}     # replaced: main object path -> this checkout's object path
for tdir in ["aios.dir"] + [f"{t}.dir" for t in targets]:
    prefix = f"{B}/CMakeFiles/{tdir}/"
    for root, _, files in os.walk(prefix):
        for f in files:
            if not f.endswith(".o"):
                continue
            obj = os.path.join(root, f)
            rel_obj = obj[len(prefix):]
            rel_src = rel_obj[:-2]
            lobj = f"{lite}/{tdir}/{rel_obj}"
            if not rel_src.endswith(".cpp"):
                continue
            if needed(rel_src, obj + ".d", lobj + ".d"):
                replaced[obj] = lobj
                src = f"{wt}/{rel_src}"
                stale = not os.path.exists(lobj)
                if not stale:
                    t = os.path.getmtime(lobj)
                    stale = os.path.getmtime(src) > t or any(
                        os.path.exists(wt + "/" + d[len(MAIN) + 1:]) and os.path.getmtime(wt + "/" + d[len(MAIN) + 1:]) > t
                        for d in deps_of(lobj + ".d"))
                if stale:
                    jobs.append((tdir, src, lobj))

SLOT = MAIN + "/scripts/build-slot.sh"
flag_cache = {}
def compile_one(job):
    tdir, src, lobj = job
    os.makedirs(os.path.dirname(lobj), exist_ok=True)
    if tdir not in flag_cache:
        flag_cache[tdir] = flags_of(tdir)
    # Through the machine's build slots at idle priority: several agents build at once on a box people are using.
    cmd = f"{SLOT} /usr/bin/c++ {flag_cache[tdir]} -MD -MF {shlex.quote(lobj)}.d -MT {shlex.quote(lobj)} -o {shlex.quote(lobj)} -c {shlex.quote(src)}"
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True)
    errs = [l for l in r.stderr.split("\n") if "error" in l.lower()]
    if r.returncode != 0:
        try: os.remove(lobj)
        except OSError: pass
        return src[len(wt) + 1:], errs[:12] or r.stderr.split("\n")[:6]
    return src[len(wt) + 1:], None

if not BASE: print("warning: build/BUILD_SHA is missing, so every object counts as different; run scripts/wt-main-build.sh", flush=True)
print(f"{len(replaced)} objects differ from the main build; recompiling {len(jobs)}", flush=True)
failed = False
with concurrent.futures.ThreadPoolExecutor(max_workers=1) as ex:
    for nm, errs in ex.map(compile_one, jobs):
        if errs:
            failed = True
            print(f"FAILED {nm}")
            for e in errs: print("   ", e[:300])
if failed:
    sys.exit(1)

# a private archive: the main objects, with this checkout's in their place
link = open(f"{B}/CMakeFiles/aios.dir/link.txt").read()
objs = [t for t in shlex.split(link) if t.endswith(".o")]
mix = [replaced.get(o if os.path.isabs(o) else os.path.join(B, o), o if os.path.isabs(o) else os.path.join(B, o)) for o in objs]
arch = lite + "/libaios.a"
sig = "\n".join(sorted(m for m in mix if m.startswith(lite)))
sigfile = lite + "/archive.sig"
# Rebuilt when ANY member is newer than the archive -- the main build's objects too: when the main checkout is rebuilt (its
# headers changed, so struct layouts did), an archive that still holds the older objects links into a binary that
# crashes at the first printf. Only the fork's own objects used to be looked at.
rebuild = (not os.path.exists(arch) or not os.path.exists(sigfile) or open(sigfile).read() != sig or
           any(os.path.exists(m) and os.path.getmtime(m) > os.path.getmtime(arch) for m in mix))
if rebuild:
    if os.path.exists(arch): os.remove(arch)
    subprocess.run(["ar", "qc", arch] + mix, check=True)
    subprocess.run(["ranlib", arch], check=True)
    open(sigfile, "w").write(sig)

for t in targets:
    lk = shlex.split(open(f"{B}/CMakeFiles/{t}.dir/link.txt").read())
    out, i = [], 0
    while i < len(lk):
        tok = lk[i]
        if tok == "-o":
            out += ["-o", f"{lite}/{t}"]; i += 2; continue
        if tok == "libaios.a":
            out.append(arch)
        elif tok.endswith(".o") and (os.path.join(B, tok) in replaced):
            out.append(replaced[os.path.join(B, tok)])
        else:
            out.append(tok)
        i += 1
    r = subprocess.run([SLOT] + out, cwd=B, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"LINK FAILED {t}")
        print("\n".join(r.stderr.split("\n")[:8]))
        sys.exit(1)
    print(f"built {lite}/{t}")
