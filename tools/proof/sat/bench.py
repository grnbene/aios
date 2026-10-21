#!/usr/bin/env python3
# kissat vs aios-sat, both on ONE thread pinned to the same core (taskset -c 3;
# aios-sat with --threads=1), one instance at a time, wall-clock seconds capped
# at the timeout (TIMEOUT rows count as the limit; PAR-2 is left to the reader).
import subprocess, sys, time, os, glob
K = "/tmp/satref/kissat/build/kissat"; A = "/tmp/aios-sat-bench"
lim = float(sys.argv[1]); files = sys.argv[2:]
def run(cmd):
    t0 = time.time()
    try: p = subprocess.run(cmd, capture_output=True, text=True, timeout=lim); rc = p.returncode
    except subprocess.TimeoutExpired: rc = -1
    return rc, min(time.time() - t0, lim)
for f in files:
    krc, kt = run(["taskset", "-c", "3", K, "-q", "-n", f])
    arc, at = run(["taskset", "-c", "3", A, f, "--quiet", "--threads=1"])
    ans = {10: "SAT", 20: "UNSAT", -1: "TIMEOUT"}
    print("%s\t%s\t%.3f\t%s\t%.3f" % (os.path.basename(f), ans.get(krc, str(krc)), kt, ans.get(arc, str(arc)), at), flush=True)
