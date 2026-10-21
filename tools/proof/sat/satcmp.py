#!/usr/bin/env python3
# One instance three ways: kissat, aios with elimination, aios without; the
# aios answers verified (models against the file, UNSAT proofs by drat-trim).
import subprocess, sys, time, os, tempfile
cnf = sys.argv[1]; lim = float(sys.argv[2]) if len(sys.argv) > 2 else 120
K = "/tmp/satref/kissat/build/kissat"; D = "/tmp/satref/drat-trim/drat-trim"; A = "/tmp/aios-sat-suite"
def run(cmd):
    t0 = time.time()
    try: p = subprocess.run(cmd, capture_output=True, text=True, timeout=lim); return p.returncode, p.stdout, time.time() - t0
    except subprocess.TimeoutExpired: return -1, "", lim
def verify(rc, out, proof):
    if rc == 10:
        model = set()
        for l in out.splitlines():
            if l.startswith("v "): model.update(int(x) for x in l[2:].split() if x != "0")
        for line in open(cnf):
            line = line.strip()
            if not line or line[0] in "cp%": continue
            lits = [int(x) for x in line.split() if x != "0"]
            if lits and not any(x in model for x in lits): return "MODEL-BAD"
        return "model-ok"
    if rc == 20:
        r = subprocess.run([D, cnf, proof, "-t", "600"], capture_output=True, text=True)
        return "drat-ok" if "s VERIFIED" in r.stdout else "DRAT-FAIL"
    return "-"
ans = {10: "SAT", 20: "UNSAT"}
krc, _, kt = run([K, "-q", "-n", cnf])
res = [os.path.basename(cnf), ans.get(krc, "?"), "%.2f" % kt]
for extra in ([], ["--no-elim"]):
    pf = tempfile.mktemp(suffix=".drat", dir="/tmp/satproofs")
    rc, out, t = run(["taskset", "-c", "0-2", A, cnf, "--threads=1", "--proof=" + pf] + extra)
    el = [l for l in out.splitlines() if l.startswith("c vars")]
    elim = el[0].split("eliminated=")[1].split()[0] if el and "eliminated=" in el[0] else "?"
    res += [ans.get(rc, "?"), "%.2f" % t, verify(rc, out, pf), elim]
    if os.path.exists(pf): os.remove(pf)
print("\t".join(res))
