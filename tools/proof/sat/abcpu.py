#!/usr/bin/env python3
# A/B by child CPU time (user+sys), interleaved per instance, one core.
# usage: abcpu.py LIMIT OUT.tsv 'name=ENV=1,ENV2=1@binary' ... -- files
import subprocess, sys, os, resource, time
lim=float(sys.argv[1]); out=open(sys.argv[2],"w")
i=sys.argv.index("--"); specs=sys.argv[3:i]; files=sys.argv[i+1:]
V=[]
for sp in specs:
    name,rest=sp.split("=",1); envs,binp=rest.split("@")
    env=dict(os.environ); 
    for e in filter(None,envs.split(",")): k,v=e.split("="); env[k]=v
    V.append((name,env,binp))
def run(env,binp,f):
    cmd=["taskset","-c","3",binp,f]+(["-q","-n"] if "kissat" in binp else ["--quiet","--threads=1"])
    p=subprocess.Popen(cmd,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,env=env)
    t=time.time()
    while True:
        pid,st,ru=os.wait4(p.pid,os.WNOHANG)
        if pid: break
        if time.time()-t>lim*2.5: p.kill(); pid,st,ru=os.wait4(p.pid,0); return -1,2*lim
        time.sleep(0.005)
    c=ru.ru_utime+ru.ru_stime; rc=os.waitstatus_to_exitcode(st)
    return (rc,c) if c<lim else (-1,2*lim)
tot={n:0.0 for n,_,_ in V}; wins={n:0 for n,_,_ in V}
for f in files:
    r={n:run(e,b,f) for n,e,b in V}
    for n in r: tot[n]+=r[n][1]
    best=min(r,key=lambda n:r[n][1]); wins[best]+=1
    sts={r[n][0] for n in r if r[n][0] in (10,20)}
    out.write(os.path.basename(f)+"\t"+"\t".join("%s:%d:%.3f"%(n,r[n][0],r[n][1]) for n in r)+("\tDISAGREE" if len(sts)>1 else "")+"\n"); out.flush()
print("  ".join("%s=%.1fs(best on %d)"%(n,tot[n],wins[n]) for n in tot),"n=%d"%len(files))
