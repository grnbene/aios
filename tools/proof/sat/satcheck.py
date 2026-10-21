#!/usr/bin/env python3
# One instance: kissat's answer vs aios-sat's, with independent evidence -
# a SAT model checked here against the file, an UNSAT proof checked by drat-trim.
import subprocess, sys, time, os, tempfile
cnf = sys.argv[1]; limit = float(sys.argv[2]) if len(sys.argv) > 2 else 60
KISSAT = "/tmp/satref/kissat/build/kissat"; DRAT = "/tmp/satref/drat-trim/drat-trim"
AIOS = "/tmp/aios-sat-suite"
def run(cmd, t):
    t0 = time.time()
    try: p = subprocess.run(cmd, capture_output=True, text=True, timeout=t); out, rc = p.stdout, p.returncode
    except subprocess.TimeoutExpired: out, rc = "", -1
    return rc, out, time.time() - t0
krc, _, kt = run([KISSAT, "-q", "-n", cnf], limit)
kans = {10: "SAT", 20: "UNSAT"}.get(krc, "UNKNOWN")
proof = tempfile.mktemp(suffix=".drat", dir="/tmp/satproofs")
arc, aout, at = run(["taskset", "-c", "0-2", AIOS, cnf, "--threads=1", "--proof=" + proof, "--seconds=%g" % limit], limit + 10)
aans = {10: "SAT", 20: "UNSAT", 1: "ERROR"}.get(arc, "UNKNOWN")
evidence = "-"
if aans == "SAT":
    model = set()
    for line in aout.splitlines():
        if line.startswith("v "): model.update(int(x) for x in line[2:].split() if x != "0")
    ok = True
    for line in open(cnf):
        line = line.strip()
        if not line or line[0] in "cp%": continue
        lits = [int(x) for x in line.split() if x != "0"]
        if lits and not any(l in model for l in lits): ok = False; break
    evidence = "model-ok" if ok else "MODEL-BAD"
elif aans == "UNSAT":
    r = subprocess.run([DRAT, cnf, proof, "-t", "600"], capture_output=True, text=True)
    evidence = "drat-VERIFIED" if "s VERIFIED" in r.stdout else "DRAT-FAILED"
if os.path.exists(proof): os.remove(proof)
agree = "agree" if aans == kans else ("aios-timeout" if aans == "UNKNOWN" else "DISAGREE")
print("%s\t%s\t%s\t%.3f\t%.3f\t%s\t%s" % (os.path.basename(cnf), kans, aans, kt, at, evidence, agree))
