import sys, collections, itertools
for f in sys.argv[1:]:
    cls=[]
    for line in open(f):
        if line[0] in 'cp%': continue
        c=[int(x) for x in line.split()][:-1]
        if c: cls.append(c)
    w=collections.Counter(len(c) for c in cls)
    # XOR detection: groups of clauses over the same var set, size k, count 2^(k-1), each with a parity-consistent sign pattern
    by=collections.defaultdict(list)
    for c in cls: by[tuple(sorted(abs(l) for l in c))].append(c)
    xors=0
    for vs,g in by.items():
        k=len(vs)
        if 3<=k<=6 and len(g)==2**(k-1):
            par={sum(l<0 for l in c)%2 for c in g}
            if len(par)==1: xors+=1
    nv=max(abs(l) for c in cls for l in c)
    print(f.split('/')[-1][:12], "vars",nv,"clauses",len(cls),"widths",dict(sorted(w.items())),"xor-groups",xors)
