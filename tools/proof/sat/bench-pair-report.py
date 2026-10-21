import sys, collections, statistics
rows=[l.split('\t') for l in open(sys.argv[1])]; L=float(sys.argv[2]) if len(sys.argv)>2 else 60
k=sum(float(r[3]) for r in rows); a=sum(float(r[5]) for r in rows)
dis=sum(1 for r in rows if r[2] in("SAT","UNSAT") and r[4] in("SAT","UNSAT") and r[2]!=r[4])
print("runs=%d instances=%d | PAR-2 kissat %.0f aios %.0f (aios/kissat %.2f) | solved kissat %d aios %d | disagreements %d"%(len(rows),len({r[0] for r in rows}),k,a,a/k if k else 0,sum(r[2]!="TIMEOUT" for r in rows),sum(r[4]!="TIMEOUT" for r in rows),dis))
per=collections.defaultdict(list)
for r in rows: per[r[0]].append((float(r[3]),float(r[5])))
w=sum(1 for v in per.values() if statistics.median(x[1] for x in v)<statistics.median(x[0] for x in v))
print("instances where aios median < kissat median: %d of %d"%(w,len(per)))
