import os, pty, subprocess, select, time, re
import sys
AIOS=sys.argv[1] if len(sys.argv)>1 else "aios"; TOKEN="fleetmigrationtoken01"
S=(sys.argv[2] if len(sys.argv)>2 else "/tmp")+"/aos-migrate-"+str(os.getpid())
os.system("rm -rf %s/mvA %s/mvB"%(S,S))
def spawn(root):
    m,s=pty.openpty(); p=subprocess.Popen([AIOS,"--root",root,"shell"],stdin=s,stdout=s,stderr=s,close_fds=True); os.close(s); return p,m
def flush(fd):
    while True:
        r,_,_=select.select([fd],[],[],0.3)
        if not r: return
        try: 
            if not os.read(fd,65536): return
        except OSError: return
def cmd(fd,l,t=8):
    flush(fd); os.write(fd,(l+"\n").encode()); buf=""; end=time.time()+t
    while time.time()<end:
        r,_,_=select.select([fd],[],[],0.2)
        if fd in r:
            try:d=os.read(fd,65536)
            except OSError:break
            if not d:break
            buf+=d.decode("utf-8","replace")
            if re.search(r"\naios> $",re.sub(r"\x1b\[[0-9;=?]*[a-zA-Z]","",buf)):break
    o=re.sub(r"\x1b\[[0-9;=?]*[a-zA-Z]","",buf).splitlines()
    return [x for x in o if x.strip() and not x.strip().startswith("aios>") and x.strip()!=l]
A,af=spawn(S+"/mvA"); B,bf=spawn(S+"/mvB"); time.sleep(1.2); cmd(af,"echo x"); cmd(bf,"echo x")
try:
    cmd(bf,"node serve 9000 "+TOKEN,3); time.sleep(0.5)
    cmd(af,"entity create worker"); cmd(af,"set w")
    for g in ["compute * execute,schedule","kernel * schedule,checkpoint","network * publish"]: cmd(af,"cap grant $w "+g)
    for t,v in [("f32 2x3 1 2 3 4 5 6","A"),("f32 3x2 7 8 9 10 11 12","B"),("f32 2x2","C"),("f32 2x2","D")]:
        cmd(af,"tensor create $w "+t); cmd(af,"set "+v)
    cmd(af,"graph create $w"); cmd(af,"set g")
    cmd(af,"graph op $w $g matmul in=$A:f32:2x3 in=$B:f32:3x2 out=$C:f32:2x2")
    cmd(af,"graph op $w $g relu in=$C:f32:2x2 out=$D:f32:2x2")
    cmd(af,"graph op $w $g softmax in=$D:f32:2x2 out=$D:f32:2x2")
    wid=cmd(af,"echo $w")[-1].strip(); gid=cmd(af,"echo $g")[-1].strip(); did=cmd(af,"echo $D")[-1].strip(); cid=cmd(af,"echo $C")[-1].strip()
    print("MACHINE A  worker %s"%wid[:12])
    print("  A run 1/3:  "+"".join(cmd(af,"graph run $w $g 1")).split("in ")[0].strip())
    print("  A C now:    "+[l for l in cmd(af,"tensor show $w "+cid+" f32 2x2") if "f32[" in l][0].strip())
    print("  A connect:  "+cmd(af,"node connect 127.0.0.1 9000 "+TOKEN,8)[-1].strip())
    print("  A MIGRATE:  "+cmd(af,"node migrate $w 0",10)[-1].strip())
    moved = not any("worker" in l for l in cmd(af,"entity list"))
    print("  A entities: "+("worker STILL here" if not moved else "worker is GONE from A"))
    print("MACHINE B")
    es=cmd(bf,"entity show "+wid,5)
    arrived = any(("@1" in l and "execution" in l) for l in es)
    print("  B has it:   "+([l.strip() for l in es if "execution" in l] or [l.strip() for l in es])[0])
    print("  B D before: "+([l for l in cmd(bf,"tensor show "+wid+" "+did+" f32 2x2") if "f32[" in l] or ["?"])[0].strip())
    print("  B RESUME:   "+"".join(cmd(bf,"graph run "+wid+" "+gid,8)).split("in ")[0].strip())
    print("  B D final:  "+([l for l in cmd(bf,"tensor show "+wid+" "+did+" f32 2x2") if "f32[" in l] or ["?"])[0].strip())
    fin = ([l for l in cmd(bf,"tensor show "+wid+" "+did+" f32 2x2") if "f32[" in l] or ["?"])[0]
    correct = "0.99753" in fin and "0.0024726" in fin
    ok = moved and arrived and correct
    print("  checks: moved-off-A=%s arrived-on-B@1=%s resumed-correct=%s" % (moved, arrived, correct))
    print("\nMIGRATE: "+("PASS — a running job checkpointed on A, moved over the wire, resumed and completed on B" if ok else "FAIL"))
    A.kill(); B.kill()
    sys.exit(0 if ok else 1)
finally:
    A.kill(); B.kill()
