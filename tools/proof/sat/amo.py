import sys, collections
f=sys.argv[1]; cls=[]
for line in open(f):
    if line[0] in 'cp%': continue
    c=[int(x) for x in line.split()][:-1]
    if c: cls.append(c)
wide=[c for c in cls if len(c)>2]; bins=[c for c in cls if len(c)==2]
print("wide",len(wide),"pos-only",all(l>0 for c in wide for l in c),"disjoint",len({abs(l) for c in wide for l in c})==sum(len(c) for c in wide))
print("binaries all-negative",all(l<0 for c in bins for l in c))
# map var -> (wide clause index, position)
pos={}
for i,c in enumerate(wide):
    for j,l in enumerate(c): pos[abs(l)]=(i,j)
kinds=collections.Counter()
for a,b in bins:
    (i1,j1),(i2,j2)=pos.get(abs(a),(-1,-1)),pos.get(abs(b),(-1,-1))
    kinds["same-clause" if i1==i2 else "same-position" if j1==j2 else "other"]+=1
print(kinds)
# within same-position pairs: are they complete cliques per position?
n=len(wide)
print("pairs if complete per position:", len(wide[0])*n*(n-1)//2, "per clause complete:", n*len(wide[0])*(len(wide[0])-1)//2)
