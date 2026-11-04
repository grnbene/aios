# bench_sales: 500,000 rows, fixed seed. Writes data/bench_sales.csv and data/inserts.sql (100 INSERTs of 5,000 rows).
import random, datetime, os
random.seed(20261104)
base = datetime.date(2024, 1, 1)
statuses = ['shipped', 'pending', 'returned', 'cancelled', 'invoiced']
rows = []
for i in range(1, 500001):
    rows.append((i, random.randint(1, 50000), random.randint(1, 50), round(random.uniform(1, 1000), 2),
                 (base + datetime.timedelta(days=random.randint(0, 729))).isoformat(), random.choice(statuses)))
d = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'data')
with open(os.path.join(d, 'bench_sales.csv'), 'w') as f:
    for r in rows: f.write('%d,%d,%d,%.2f,%s,%s\n' % r)
with open(os.path.join(d, 'inserts.sql'), 'w') as f:
    for b in range(0, 500000, 5000):
        f.write('INSERT INTO bench_sales VALUES ' + ','.join("(%d,%d,%d,%.2f,'%s','%s')" % r for r in rows[b:b+5000]) + ';\n')
