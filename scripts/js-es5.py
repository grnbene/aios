#!/usr/bin/env python3
"""Runs the ES5 conformance corpus (scripts/js-es5/*.js) through the in-tree JavaScript engine.

A corpus file holds many small tests separated by lines of the form `//// name`. Each test is a
self-contained program that ends by printing the word `ok` (console.log("ok")) when it holds and anything
else, or an engine error, when it does not. They are run one per `aios exec "js run <file>"`, in parallel,
because the engine's errors are fatal to a program: one failing test must not hide the ones after it.

    python3 scripts/js-es5.py build/aios [-k substring] [-v]
Exits non-zero if any test fails. Test-only; there is no Python in the product."""
import concurrent.futures, glob, os, re, shutil, subprocess, sys, tempfile

aios = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
pattern = None; verbose = False
if "-k" in sys.argv: pattern = sys.argv[sys.argv.index("-k") + 1]
if "-v" in sys.argv: verbose = True

tests = []
prelude_path = os.path.join(here, "js-es5", "_prelude.js.txt")
prelude = open(prelude_path, encoding="utf-8").read() if os.path.exists(prelude_path) else ""
for path in sorted(glob.glob(os.path.join(here, "js-es5", "*.js"))):
    cur = None; body = []
    text = open(path, encoding="utf-8").read()
    autook = text.startswith("//# autook")
    def finish(name, lines):
        src = "\n".join(lines)
        if autook: src = prelude + "\n" + src + "\nconsole.log('ok');"
        tests.append((os.path.basename(path)[:-3] + ": " + name, src))
    for line in text.splitlines():
        m = re.match(r"^////\s*(.+?)\s*$", line)
        if m:
            if cur: finish(cur, body)
            cur, body = m.group(1), []
        else:
            body.append(line)
    if cur: finish(cur, body)
if pattern: tests = [t for t in tests if pattern in t[0]]

work = tempfile.mkdtemp(prefix="jses5-"); root = os.path.join(work, "root"); os.makedirs(root)
def run(i):
    name, src = tests[i]
    f = os.path.join(work, "t%d.js" % i)
    open(f, "w", encoding="utf-8").write(src + "\n")
    try:
        if os.path.basename(aios).startswith("aios-js-run"):
            r = subprocess.run([aios, f], capture_output=True, text=True, timeout=60)   # the standalone engine runner
        else:
            r = subprocess.run([aios, "--root", root, "exec", "js run " + f], capture_output=True, text=True, timeout=60)
        out = (r.stdout + r.stderr).strip()
    except subprocess.TimeoutExpired:
        out = "TIMEOUT"
    last = out.splitlines()[-1].strip() if out else ""
    return i, last == "ok", out

failed = []
with concurrent.futures.ThreadPoolExecutor(max_workers=8) as ex:
    for i, ok, out in ex.map(run, range(len(tests))):
        if not ok:
            failed.append(i)
            print("FAIL  %s\n        %s" % (tests[i][0], out.replace("\n", "\n        ")[:300]))
        elif verbose:
            print("pass  " + tests[i][0])
shutil.rmtree(work, ignore_errors=True)
print("%d tests, %d failed" % (len(tests), len(failed)))
sys.exit(1 if failed else 0)
