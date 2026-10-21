# Window functions: ASQL against SQLite on the same rows, query by query (python3 wincheck.py PORT)
import os, sqlite3, subprocess, sys, random
P = sys.argv[1]
random.seed(7)
rows = []
for i in range(1, 61):
    rows.append((i, random.choice(['C1', 'C2', 'C3', None]), random.choice(['SHAMPOO', 'DRYER', 'CONDITION']),
                 random.choice([1, 2, 2, 3, 5, None]), round(random.uniform(1, 90), 2), f"2026-10-{random.randint(1, 28):02d}"))
ddl = "CREATE TABLE sales (id INT PRIMARY KEY, debtorno VARCHAR(10), stockid VARCHAR(20), qty INT, price DECIMAL(10,2), trandate DATE)"
def lit(v): return 'NULL' if v is None else (f"'{v}'" if isinstance(v, str) else str(v))
ins = "INSERT INTO sales VALUES " + ", ".join("(" + ", ".join(lit(v) for v in r) + ")" for r in rows)
con = sqlite3.connect(":memory:"); con.execute(ddl); con.execute(ins)
def asql(q):
    r = subprocess.run([os.environ.get("AIOS_CONSOLE", "aios-asql-console"), "--no-conf", "-h", "127.0.0.1", "-P", P, "-u", "root", "-p", "secret", "-d", "wt", "-e", q], capture_output=True, text=True)
    return r.stdout + r.stderr
def asql_rows(q):
    out = asql(q)
    if "ERROR" in out: return "ERROR " + out.strip().splitlines()[-1]
    lines = [l for l in out.splitlines() if l.startswith("|")]
    return [tuple(c.strip() for c in l.strip("|").split("|")) for l in lines[1:]]
asql("CREATE DATABASE wt"); asql(ddl); asql(ins)
def norm(v):
    if v is None or v == "NULL": return "NULL"
    try:
        f = float(v); return f"{f:.4f}"
    except: return str(v)
Q = [
 "SELECT id, ROW_NUMBER() OVER (ORDER BY id) FROM sales ORDER BY id",
 "SELECT id, ROW_NUMBER() OVER (PARTITION BY debtorno ORDER BY trandate, id) FROM sales ORDER BY id",
 "SELECT id, RANK() OVER (PARTITION BY stockid ORDER BY qty) FROM sales ORDER BY id",
 "SELECT id, DENSE_RANK() OVER (PARTITION BY stockid ORDER BY qty DESC) FROM sales ORDER BY id",
 "SELECT id, PERCENT_RANK() OVER (ORDER BY price) FROM sales ORDER BY id",
 "SELECT id, CUME_DIST() OVER (PARTITION BY debtorno ORDER BY qty) FROM sales ORDER BY id",
 "SELECT id, NTILE(4) OVER (ORDER BY id) FROM sales ORDER BY id",
 "SELECT id, NTILE(7) OVER (PARTITION BY stockid ORDER BY id) FROM sales ORDER BY id",
 "SELECT id, LAG(price) OVER (ORDER BY id) FROM sales ORDER BY id",
 "SELECT id, LAG(price, 2, 0) OVER (PARTITION BY stockid ORDER BY id) FROM sales ORDER BY id",
 "SELECT id, LEAD(qty) OVER (PARTITION BY debtorno ORDER BY id) FROM sales ORDER BY id",
 "SELECT id, LEAD(qty, 3, -1) OVER (ORDER BY id DESC) FROM sales ORDER BY id",
 "SELECT id, SUM(price) OVER () FROM sales ORDER BY id",
 "SELECT id, SUM(price) OVER (PARTITION BY stockid) FROM sales ORDER BY id",
 "SELECT id, SUM(qty) OVER (ORDER BY id) FROM sales ORDER BY id",
 "SELECT id, SUM(qty) OVER (PARTITION BY stockid ORDER BY qty) FROM sales ORDER BY id",
 "SELECT id, AVG(price) OVER (PARTITION BY debtorno) FROM sales ORDER BY id",
 "SELECT id, COUNT(*) OVER (PARTITION BY stockid) FROM sales ORDER BY id",
 "SELECT id, COUNT(qty) OVER (ORDER BY id ROWS BETWEEN 2 PRECEDING AND 2 FOLLOWING) FROM sales ORDER BY id",
 "SELECT id, SUM(price) OVER (ORDER BY id ROWS BETWEEN 3 PRECEDING AND CURRENT ROW) FROM sales ORDER BY id",
 "SELECT id, SUM(price) OVER (ORDER BY id ROWS BETWEEN 1 FOLLOWING AND UNBOUNDED FOLLOWING) FROM sales ORDER BY id",
 "SELECT id, AVG(qty) OVER (PARTITION BY stockid ORDER BY id ROWS 2 PRECEDING) FROM sales ORDER BY id",
 "SELECT id, MIN(price) OVER (PARTITION BY stockid ORDER BY id) FROM sales ORDER BY id",
 "SELECT id, MAX(price) OVER (PARTITION BY debtorno ORDER BY id ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) FROM sales ORDER BY id",
 "SELECT id, SUM(qty) OVER (ORDER BY qty RANGE BETWEEN 1 PRECEDING AND 1 FOLLOWING) FROM sales ORDER BY id",
 "SELECT id, COUNT(*) OVER (ORDER BY qty RANGE BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING) FROM sales ORDER BY id",
 "SELECT id, FIRST_VALUE(price) OVER (PARTITION BY stockid ORDER BY id) FROM sales ORDER BY id",
 "SELECT id, LAST_VALUE(price) OVER (PARTITION BY stockid ORDER BY id) FROM sales ORDER BY id",
 "SELECT id, LAST_VALUE(price) OVER (PARTITION BY stockid ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING) FROM sales ORDER BY id",
 "SELECT id, NTH_VALUE(price, 2) OVER (PARTITION BY stockid ORDER BY id) FROM sales ORDER BY id",
 "SELECT id, ROUND(100 * price / SUM(price) OVER (PARTITION BY stockid), 4) FROM sales ORDER BY id",
 "SELECT id, price - LAG(price, 1, price) OVER (ORDER BY id) FROM sales ORDER BY id",
 "SELECT stockid, SUM(qty), RANK() OVER (ORDER BY SUM(qty) DESC) FROM sales GROUP BY stockid ORDER BY stockid",
 "SELECT debtorno, COUNT(*), SUM(COUNT(*)) OVER () FROM sales GROUP BY debtorno ORDER BY debtorno",
 "SELECT id, stockid, rn FROM (SELECT id, stockid, ROW_NUMBER() OVER (PARTITION BY stockid ORDER BY price DESC, id) rn FROM sales) x WHERE rn <= 2 ORDER BY stockid, rn",
 "SELECT id, ROW_NUMBER() OVER w, SUM(qty) OVER w FROM sales WINDOW w AS (PARTITION BY stockid ORDER BY id) ORDER BY id",
 "SELECT id, ROW_NUMBER() OVER (ORDER BY price DESC, id) rn FROM sales ORDER BY rn LIMIT 5",
 "SELECT DISTINCT stockid, COUNT(*) OVER (PARTITION BY stockid) FROM sales ORDER BY stockid",
 "SELECT s.*, ROW_NUMBER() OVER (PARTITION BY s.debtorno ORDER BY s.id) AS rn FROM sales s WHERE s.qty > 1 ORDER BY s.id",
]
ok = bad = 0
for q in Q:
    want = [tuple(norm(v) for v in r) for r in con.execute(q).fetchall()]
    got = asql_rows(q)
    if isinstance(got, str):
        bad += 1; print("FAIL", q, "\n    ", got); continue
    got = [tuple(norm(v) for v in r) for r in got]
    if got == want: ok += 1
    else:
        bad += 1; print("FAIL", q)
        for a, b2 in zip(got, want):
            if a != b2: print("     asql", a, "sqlite", b2); break
        if len(got) != len(want): print("     rows", len(got), "vs", len(want))
print(f"{ok} match SQLite, {bad} differ")
