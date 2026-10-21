import subprocess, random, itertools, sys, os, time, signal
PW=open('/tmp/gt.pass').read().strip(); os.environ['MYSQL_PWD']=PW
def serve(binp, root, port):
    subprocess.run(["rm","-rf",root]); os.makedirs(root)
    p=subprocess.Popen([binp,"--root",root,"exec",f"compute serve port={port+4000} lanes=1 sql=1 mysql_port={port} mysql_bind=127.0.0.1 mysql_user=asql mysql_pass_file=/tmp/gt.pass"],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    for _ in range(100):
        if subprocess.run(["mysql","-h127.0.0.1",f"-P{port}","-uasql","-e","SELECT 1"],capture_output=True).returncode==0: return p
        time.sleep(0.2)
    raise SystemExit("server did not start")
def q(port, sql, db="erp"):
    r=subprocess.run(["mysql","-N","-B","-h127.0.0.1",f"-P{port}","-uasql",db,"-e",sql],capture_output=True,text=True)
    if r.returncode: raise RuntimeError(sql[:80]+" -> "+r.stderr.strip())
    return [l.split('\t') for l in r.stdout.splitlines()]
DECL="READS salesorderdetails, salesorders AS SELECT d.orderno, d.orderlineno, so.fromstkloc, d.stkcode, d.quantity - d.qtyinvoiced FROM salesorderdetails d JOIN salesorders so ON so.orderno = d.orderno WHERE d.completed = 0 AND so.quotation = 0 AND d.quantity > d.qtyinvoiced"
def setup(port, caps, orders, mode):
    for t in ("open_order","stock"): subprocess.run(["mysql","-h127.0.0.1",f"-P{port}","-uasql","erp","-e",f"DROP THING {t}"],capture_output=True)
    subprocess.run(["mysql","-h127.0.0.1",f"-P{port}","-uasql","-e","DROP DATABASE IF EXISTS erp; CREATE DATABASE erp"],capture_output=True)
    q(port,"CREATE TABLE locstock (loccode VARCHAR(5), stockid VARCHAR(10), quantity DECIMAL(10,2), PRIMARY KEY (loccode, stockid))")
    q(port,"CREATE TABLE salesorders (orderno INT PRIMARY KEY, orddate DATE, fromstkloc VARCHAR(5), quotation INT)")
    q(port,"CREATE TABLE salesorderdetails (orderno INT, orderlineno INT, stkcode VARCHAR(10), quantity DECIMAL(10,2), qtyinvoiced DECIMAL(10,2), completed INT, PRIMARY KEY (orderno, orderlineno))")
    q(port,"INSERT INTO locstock VALUES "+",".join(f"('W','I{i}',{c})" for i,c in enumerate(caps)))
    q(port,"INSERT INTO salesorders VALUES "+",".join(f"({o},'2026-09-{1+(o*7)%27:02d}','W',0)" for o in orders))
    q(port,"INSERT INTO salesorderdetails VALUES "+",".join(f"({o},{j+1},'I{it}',{n},0,0)" for o,ls in orders.items() for j,(it,n) in enumerate(ls)))
    q(port,"CREATE THING stock RESOURCE KEY loccode, stockid CAPACITY quantity FROM locstock READS locstock")
    q(port,f"CREATE THING open_order CLAIMS stock KEY orderno PRIORITY so.orddate, d.orderno, d.orderlineno {'ALLOCATE READY ' if mode else ''}{DECL}")
def best(caps, orders):
    os_=list(orders); m=0
    for r in range(len(os_),0,-1):
        for S in itertools.combinations(os_,r):
            use=[0]*len(caps)
            for o in S:
                for it,n in orders[o]: use[it]+=n
            if all(use[i]<=caps[i] for i in range(len(caps))): return r
    return 0
def check_ready(port, caps, orders):
    lines=q(port,"SHOW THING LINES open_order"); st=dict((r[0],r[1]) for r in q(port,"SHOW THINGS open_order"))
    used=[0.0]*len(caps); full={}
    for r in lines:
        o,it,need,al=r[0],int(r[3][1:]),float(r[4]),float(r[5]); used[it]+=al
        full[o]=full.get(o,True) and al>=need-1e-9
    assert all(used[i]<=caps[i]+1e-9 for i in range(len(caps))), ("over", used, caps)
    ready=sum(1 for o in full if full[o]); b=best(caps,orders)
    assert ready==b, ("not optimal", ready, b)
    assert all((st[o].startswith("Ready"))==full[o] for o in full)
    return ready
po, pn = serve("/tmp/aios-head","/tmp/rf-old",13418), serve("/tmp/aios-ready","/tmp/rf-new",13419)
old, new = 13418, 13419
n=int(sys.argv[1]); gains=0; fifo_tot=0; ready_tot=0; steps=0
try:
    for seed in range(n):
        rng=random.Random(seed); caps=[rng.randint(0,10) for _ in range(6)]
        orders={o:[(it,rng.randint(1,6)) for it in rng.sample(range(6),rng.randint(1,3))] for o in range(1,rng.randint(12,14)+1)}
        setup(old,caps,orders,False); setup(new,caps,orders,False)
        a=[r[:-1] for r in q(old,"SHOW THING LINES open_order")]; b=[r[:-1] for r in q(new,"SHOW THING LINES open_order")]
        if a!=b:
            print("OLD", a[:5]); print("NEW", b[:5]); raise SystemExit("FIFO differs seed %d" % seed)
        q(new,"DROP THING open_order"); q(new,f"CREATE THING open_order CLAIMS stock KEY orderno PRIORITY so.orddate, d.orderno, d.orderlineno ALLOCATE READY {DECL}")
        r=check_ready(new,caps,orders); steps+=1
        fifo=sum(1 for x in q(old,"SHOW THINGS open_order") if x[1].startswith("Ready")); fifo_tot+=fifo; ready_tot+=r; gains+=r>fifo
        for m in range(2):   # reactions
            if rng.random()<0.5:
                i=rng.randrange(6); caps[i]=rng.randint(0,10); sql=f"UPDATE locstock SET quantity={caps[i]} WHERE stockid='I{i}'"
            else:
                o=rng.choice(list(orders)); del orders[o]; sql=f"UPDATE salesorderdetails SET completed=1 WHERE orderno={o}"
            q(old,sql); q(new,sql)
            a=q(old,"SHOW THING LINES open_order")
            check_ready(new,caps,orders); steps+=1
            # FIFO path: the old and new FIFO must still agree (a fresh FIFO Thing beside READY on new is not possible: one claims per resource)
    print(f"cases={n} checks={steps}: READY optimal every time (brute force); FIFO old==new on every initial state; READY beat FIFO in {gains} cases; whole orders FIFO {fifo_tot} -> READY {ready_tot}")
finally:
    po.send_signal(signal.SIGTERM); pn.send_signal(signal.SIGTERM)
