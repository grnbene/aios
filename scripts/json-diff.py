#!/usr/bin/env python3
"""Holds the in-tree JSON parser (tools/json-check.cpp) against Python's json module: the same documents
must be accepted or refused by both (Python run strict: no NaN/Infinity, no duplicate-key surprises),
and what the in-tree writer prints must mean the same thing as the input. Random documents, random
damage to them, and a list of the classic edge cases.

    python3 scripts/json-diff.py build/aios-json-check
Test-only; no Python in the product."""
import json, os, random, subprocess, sys, tempfile

random.seed(2026)
def strict_load(s):
    def bad(c): raise ValueError(c)
    return json.loads(s, parse_constant=bad)

def rstr():
    pool = ["a", "é", "日", "😀", "\n", "\t", '"', "\\", "/", "\u2028", "\x01", " ", "key", "0"]
    return "".join(random.choice(pool) for _ in range(random.randint(0, 8)))

def rval(d=0):
    k = random.random()
    if d > 4 or k < 0.35:
        return random.choice([None, True, False, 0, 1, -1, 12345678901234, 3.14159, -2.5e10, 1e-7, rstr(), "", 10**17])
    if k < 0.65: return [rval(d + 1) for _ in range(random.randint(0, 4))]
    return {rstr(): rval(d + 1) for _ in range(random.randint(0, 4))}

docs = []
for _ in range(600):
    docs.append(json.dumps(rval(), ensure_ascii=random.random() < 0.5, indent=random.choice([None, 2])))
edge = ['', ' ', '[', ']', '{', '}', '[]', '{}', '[1,]', '{"a":1,}', '[,1]', '{"a"}', '{"a":}', '{a:1}', "{'a':1}", '01', '-', '-0', '1.', '.5',
        '1e', '1e+', '+1', 'NaN', 'Infinity', '-Infinity', 'nul', 'true false', '"\\x41"', '"\\u12"', '"\\ud800"', '"\\ud800\\u0041"', '"\\udc00"',
        '"a\nb"', '"\t"', '"unterminated', '[1 2]', '{"a":1 "b":2}', '1 2', 'null,', '[[[[[[[[[[[[[[[[[[[[1]]]]]]]]]]]]]]]]]]]]',
        '"\\ud83d\\ude00"', '{"a":1,"a":2}', '1E400', '-1e-400', '123456789012345678901234567890', '0.1e1', '[1e2,1E2,1e+2]', '"\\/"', '\ufeff[]']
docs += edge
for d in list(docs[:400]):   # damage
    if len(d) > 2:
        i = random.randrange(len(d))
        docs.append(random.choice([d[:i], d[:i] + d[i + 1:], d[:i] + random.choice('[]{},:"\\ x0') + d[i:]]))

# Deliberate differences from Python's json: it keeps a lone surrogate escape as an unpaired code unit
# (which no UTF-8 text can hold), this parser turns it into U+FFFD; and it accepts 1E400 as infinity,
# this parser refuses a number it cannot hold.
KNOWN = {'"\\ud800"', '"\\ud800\\u0041"', '"\\udc00"', '1E400'}

def same(a, b):
    if isinstance(a, float) or isinstance(b, float):
        return isinstance(a, (int, float)) and isinstance(b, (int, float)) and (a == b or abs(a - b) <= 1e-9 * max(abs(a), abs(b)))
    if isinstance(a, dict) and isinstance(b, dict):
        return list(a.keys()) == list(b.keys()) and all(same(a[k], b[k]) for k in a)
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(same(x, y) for x, y in zip(a, b))
    return type(a) == type(b) and a == b

exe = sys.argv[1] if len(sys.argv) > 1 else "build/aios-json-check"
with tempfile.TemporaryDirectory() as d:
    paths = []
    for i, doc in enumerate(docs):
        p = os.path.join(d, "%d.json" % i); open(p, "wb").write(doc.encode("utf-8", "surrogatepass")); paths.append(p)
    out = subprocess.run([exe] + paths, capture_output=True, check=True).stdout.decode("utf-8", "replace").split("\n")
bad = 0
for i, doc in enumerate(docs):
    mine = out[i]
    try:
        want = strict_load(doc.encode("utf-8", "surrogatepass").decode("utf-8", "replace") if "\ufeff" not in doc else doc)
        py_ok = True
    except Exception:
        py_ok = False
    my_ok = mine.startswith("OK ")
    if doc.startswith("\ufeff"):   # a BOM: Python refuses it in str form; we refuse it too
        py_ok = False
    if doc in KNOWN:
        continue
    if py_ok != my_ok:
        bad += 1; print("ACCEPTANCE differs for %r: python=%s aios=%s" % (doc[:70], py_ok, my_ok)); continue
    if my_ok:
        try:
            back = strict_load(mine[3:])
        except Exception as e:
            bad += 1; print("aios output is not valid JSON for %r: %r (%s)" % (doc[:50], mine[:80], e)); continue
        if not same(back, want):
            bad += 1; print("VALUE differs for %r:\n   python=%r\n   aios  =%r" % (doc[:70], want, back))
print("%d documents, %d differ" % (len(docs), bad))
sys.exit(1 if bad else 0)
