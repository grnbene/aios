#!/usr/bin/env python3
"""What a driver that converts by declared type hands the program: every SELECT in a DUMP= dir on
ASQL and on MariaDB through PyMySQL (INT -> int, DECIMAL -> Decimal, DATE/DATETIME -> datetime,
an unparsable value -> an exception), compared value AND Python type, cell by cell.
  python3 cmptyped.py STMT_DIR [asql=127.0.0.1:13401] [maria=127.0.0.1:3306]"""
import os, sys, glob, hashlib, decimal, collections
import pymysql
opt = {'asql': '127.0.0.1:13401', 'maria': '127.0.0.1:3306'}
for a in sys.argv[2:]:
    k, _, v = a.partition('=')
    opt[k] = v
import codecs
# The ERP holds latin1 bytes in utf8 columns; a strict decode killed the connection and every statement
# after it "errored". Text is decoded with replacement on both sides, so the same bytes compare equal.
codecs.register(lambda n: codecs.CodecInfo(name=n, encode=codecs.utf_8_encode,
                                           decode=lambda b, errors='strict': codecs.utf_8_decode(b, 'replace', True)) if n == 'utf8lenient' else None)
class Side:
    def __init__(self, spec, user, pw):
        self.h, p = spec.split(':'); self.p = int(p); self.user = user; self.pw = pw; self.c = None; self.open()
    def open(self):
        self.c = pymysql.connect(host=self.h, port=self.p, user=self.user, password=self.pw, database=os.environ.get('DBNAME','versioneight'), connect_timeout=30, read_timeout=600)
        self.c.encoding = 'utf8lenient'
A = Side(opt['asql'], os.environ.get('AUSER','root'), os.environ.get('APASS','')); M = Side(opt['maria'], os.environ.get('MUSER','devops'), os.environ.get('MPASS',''))
def run(s, q):
    try:
        cur = s.c.cursor()
        cur.execute(q)
        return cur.fetchall(), None
    except Exception as e:
        try: s.c.close()
        except Exception: pass
        try: s.open()   # a protocol error leaves the connection unusable; the next statement gets a fresh one
        except Exception: pass
        return None, f'{type(e).__name__}: {str(e)[:100]}'
seen = set(); n = 0; cells = 0; bad = collections.Counter(); ex = {}
errs = []
per_stmt = collections.Counter()   # statement -> cells that differ, so the few statements behind thousands of cells stand out
for f in sorted(glob.glob(sys.argv[1] + '/stmt-*.sql')):
    q = open(f).read()
    if not q.lstrip().upper().startswith(('SELECT', 'WITH')): continue
    h = hashlib.md5(q.encode()).hexdigest()
    if h in seen: continue
    seen.add(h)
    ra, ea = run(A, q); rm, em = run(M, q)
    if ea or em:
        errs.append((f.split('/')[-1], ea, em)); continue
    n += 1
    if len(ra) != len(rm): bad['row count'] += 1; ex.setdefault('row count', f.split('/')[-1]); continue
    # the same rows in any order: sort each side by a stable text form
    key = lambda r: tuple('' if v is None else str(v) for v in r)
    ra = sorted(ra, key=key); rm = sorted(rm, key=key)
    for x, y in zip(ra, rm):
        for v, w in zip(x, y):
            cells += 1
            if type(v) is not type(w):
                # an exact integral float vs int etc. is still a type difference a client sees
                k = f'type {type(v).__name__} vs {type(w).__name__}'
                bad[k] += 1; ex.setdefault(k, f.split('/')[-1] + f' [{v!r} vs {w!r}]'); per_stmt[(f.split('/')[-1], k)] += 1
            elif v != w and not (isinstance(v, (float, decimal.Decimal)) and abs(v - w) < 0.005):
                bad['value'] += 1; ex.setdefault('value', f.split('/')[-1] + f' [{v!r} vs {w!r}]'); per_stmt[(f.split('/')[-1], 'value')] += 1
print(f'{n} statements, {cells} typed cells compared; statements that errored on either side: {len(errs)}')
for e in errs[:5]: print('  ', e)
if not bad: print('typed results identical to MariaDB')
for k, v in bad.most_common(10): print(f'  {v:8d} {k}   e.g. {ex.get(k)}')
if per_stmt:
    print(f'statements behind the differences ({len({s for s, _ in per_stmt})} of {n}):')
    for (s, k), v in per_stmt.most_common(int(os.environ.get('TOP', '20'))): print(f'  {v:8d} {s:14s} {k}')
