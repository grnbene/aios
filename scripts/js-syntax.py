#!/usr/bin/env python3
"""Syntax suite for the in-tree JavaScript parser (scripts/js-syntax/*.js). Each case starts with a line
`//// ok <name>` (the program must parse) or `//// bad <name>` (it must be refused with a SyntaxError).
    python3 scripts/js-syntax.py build/aios-js-parse [-v]
Test-only; no Python in the product."""
import os, re, subprocess, sys, tempfile, glob
tool = os.path.abspath(sys.argv[1]); verbose = "-v" in sys.argv
here = os.path.dirname(os.path.abspath(__file__))
cases = []
for path in sorted(glob.glob(os.path.join(here, "js-syntax", "*.js"))):
    cur, body = None, []
    for line in open(path, encoding="utf-8").read().split("\n"):
        m = re.match(r"^////\s*(ok|bad)\s+(.*)$", line)
        if m:
            if cur: cases.append((cur[0], cur[1], "\n".join(body)))
            cur, body = (m.group(1), m.group(2)), []
        else:
            body.append(line)
    if cur: cases.append((cur[0], cur[1], "\n".join(body)))
fails = 0
with tempfile.TemporaryDirectory() as d:
    for i, (want, name, src) in enumerate(cases):
        p = os.path.join(d, "c%d.js" % i)
        open(p, "w", encoding="utf-8").write(src + "\n")
        r = subprocess.run([tool, p], capture_output=True, text=True)
        got_ok = r.stdout.startswith("ok")
        if (want == "ok") != got_ok:
            fails += 1
            print("FAIL  %s %s\n      %s" % (want, name, r.stdout.strip().replace(p, "<case>")))
        elif verbose:
            print("pass  %s %s" % (want, name))
print("%d cases, %d failed" % (len(cases), fails))
sys.exit(1 if fails else 0)
