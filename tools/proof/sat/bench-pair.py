#!/usr/bin/env python3
# Paired, shuffled: each instance in its original order plus K shuffles (clause
# order and literal order); kissat on core 2 and aios on core 3 AT THE SAME
# TIME (same machine load for both); wall-clock; PAR-2 at LIMIT.
# usage: bench-pair.py LIMIT K AIOS_BINARY OUT.tsv files...
import subprocess, sys, os, time, random
lim=float(sys.argv[1]); K=int(sys.argv[2]); A=sys.argv[3]; out=open(sys.argv[4],"a"); files=sys.argv[5:]
KS="/tmp/satref/kissat/build/kissat"; os.makedirs("/nvme/tmp/shuf",exist_ok=True)
def shuffled(f,seed):
    if seed==0: return f
    g="/nvme/tmp/shuf/%s.s%d.cnf"%(os.path.basename(f)[:16],seed)
    if os.path.exists(g): return g
    rng=random.Random(seed*7919); cls=[]; hdr=None
    for line in open(f):
        if not line.strip() or line[0] in 'c%': continue
        if line[0]=='p': hdr=line; continue
        x=line.split()[:-1]; rng.shuffle(x); cls.append(" ".join(x)+" 0\n")
    rng.shuffle(cls)
    with open(g,"w") as o: o.write(hdr); o.writelines(cls)
    return g
def start(cmd): return subprocess.Popen(cmd,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL), time.perf_counter()
def finish(p,t0):
    try: rc=p.wait(timeout=max(0.1,lim-(time.perf_counter()-t0)))
    except subprocess.TimeoutExpired: p.kill(); p.wait(); return "TIMEOUT",2*lim
    return {10:"SAT",20:"UNSAT"}.get(rc,str(rc)), time.perf_counter()-t0
for f in files:
    for s in range(K+1):
        g=shuffled(f,s)
        pk,tk=start(["taskset","-c","2",KS,"-q","-n",g]); pa,ta=start(["taskset","-c","3",A,g,"--quiet","--threads=1"])
        rk=finish(pk,tk); ra=finish(pa,ta)
        out.write("%s\t%d\t%s\t%.3f\t%s\t%.3f\n"%(os.path.basename(f)[:16],s,rk[0],rk[1],ra[0],ra[1])); out.flush()
