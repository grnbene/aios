import sys
lim = float(sys.argv[1])
for path in sys.argv[2:]:
    rows = [l.rstrip("\n").split("\t") for l in open(path) if l.strip()]
    kt = sum(float(r[2]) for r in rows); at = sum(float(r[4]) for r in rows)
    kp = sum(float(r[2]) if r[1] != "TIMEOUT" else 2 * lim for r in rows); ap = sum(float(r[4]) if r[3] != "TIMEOUT" else 2 * lim for r in rows)
    dis = sum(1 for r in rows if r[1] != r[3] and "TIMEOUT" not in (r[1], r[3]))
    aw = sum(1 for r in rows if float(r[4]) < float(r[2])); kw = len(rows) - aw
    print("%-12s %3d inst | kissat %8.1fs (PAR-2 %8.1f) | aios %8.1fs (PAR-2 %8.1f) | aios %+.0f%% faster | wins aios %d / kissat %d | disagreements %d" % (
        path.split("/")[-1].replace(".tsv", ""), len(rows), kt, kp, at, ap, 100.0 * (kp - ap) / kp, aw, kw, dis))
