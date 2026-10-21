import random, subprocess, sys, os
S="/repos/aios/build/aios-sat"
bad=0; n_uns=0; n_sat=0
for seed in range(int(sys.argv[1]), int(sys.argv[2])):
    r=random.Random(seed); nv=r.randint(3,40); cls=[]
    kind=seed%4
    m = int(nv*(r.uniform(0.8,2.5) if kind==0 else r.uniform(3.5,5.0)))
    for _ in range(m):
        k = 2 if kind==0 else (r.choice([2,2,3]) if kind==1 else (3 if kind==2 else r.choice([1,2,3,4])))
        vs=r.sample(range(1,nv+1),min(k,nv)); cls.append([v if r.random()<.5 else -v for v in vs])
    if kind==1:  # AND gates spelled out
        for g in range(r.randint(1,4)):
            o,a,b=r.sample(range(1,nv+1),3); cls += [[-o,a],[-o,b],[o,-a,-b]]
    f=f"/tmp/fz/{seed}.cnf"; open(f,"w").write(f"p cnf {nv} {len(cls)}\n"+"".join(" ".join(map(str,c))+" 0\n" for c in cls))
    for flags in ([],["--no-elim"]):
        p=f"/tmp/fz/{seed}.bin"
        o=subprocess.run([S,f,"--threads=1","--binary",f"--proof={p}"]+flags,capture_output=True,text=True).stdout
        if "UNSATISFIABLE" in o:
            n_uns+=1; ck=subprocess.run([S,"--check",f,p],capture_output=True,text=True).stdout
            if "s VERIFIED" not in ck: bad+=1; print("PROOF",seed,flags,ck.strip().splitlines()[0])
        elif "SATISFIABLE" in o:
            n_sat+=1; vals=set()
            for line in o.splitlines():
                if line.startswith("v "): vals |= {int(x) for x in line[2:].split()}
            if not all(any(l in vals for l in c) for c in cls): bad+=1; print("MODEL",seed,flags)
        else: bad+=1; print("??",seed,o[:80])
print(f"sat={n_sat} unsat={n_uns} bad={bad}")
