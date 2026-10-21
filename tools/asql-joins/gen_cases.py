#!/usr/bin/env python3
"""Writes tools/asql-joins/cases/*.case, each query's expected rows worked out by SQLite on the same data (a form SQLite
lacks - STRAIGHT_JOIN, index hints, DELETE alias FROM, MySQL's SELECT * column order for USING - is given SQLite's
plain equivalent beside it). Run it after changing a case below; tools/asql-joins/check.py runs them against ASQL.

    python3 tools/asql-joins/gen_cases.py
"""
import os, random, sqlite3

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'cases')

BASE = """CREATE TABLE customers (id INT PRIMARY KEY, name VARCHAR(40), region VARCHAR(10));
CREATE TABLE orders (id INT PRIMARY KEY, cust_id INT, amount DECIMAL(10,2), status INT);
CREATE TABLE items (order_id INT, line INT, sku VARCHAR(10), qty INT, PRIMARY KEY (order_id, line));
CREATE TABLE products (sku VARCHAR(10) PRIMARY KEY, title VARCHAR(40), price DECIMAL(10,2));
CREATE TABLE regions (code VARCHAR(10) PRIMARY KEY, label VARCHAR(20));
INSERT INTO customers VALUES (1,'Acme','N'),(2,'Bolt','S'),(3,'Crane','N'),(4,'Dune',NULL),(5,'Echo','W');
INSERT INTO orders VALUES (10,1,100.00,0),(11,1,250.50,1),(12,2,75.25,0),(13,3,10.00,1),(14,NULL,99.99,0),(15,9,5.00,0),(16,2,75.25,1);
INSERT INTO items VALUES (10,1,'A',2),(10,2,'B',1),(11,1,'A',5),(12,1,'C',1),(13,1,'Z',3),(16,1,'B',4),(16,2,'C',2);
INSERT INTO products VALUES ('A','Anvil',10.00),('B','Bucket',2.50),('C','Cable',1.25),('D','Drill',99.00);
INSERT INTO regions VALUES ('N','North'),('S','South'),('E','East');
CREATE VIEW v_lines AS SELECT c.name, i.sku FROM customers c JOIN orders o ON o.cust_id = c.id JOIN items i ON i.order_id = o.id;
"""
# 50,000 rows each, made in SQL: a join that cross-joins before filtering cannot finish in time.
BIG = """CREATE TABLE d (n INT PRIMARY KEY);
INSERT INTO d VALUES (0),(1),(2),(3),(4),(5),(6),(7),(8),(9);
CREATE TABLE ba (id INT PRIMARY KEY, v INT);
CREATE TABLE bb (id INT PRIMARY KEY, a_id INT, w INT);
INSERT INTO ba SELECT a.n + 10*b.n + 100*c.n + 1000*e.n + 10000*f.n, a.n FROM d a CROSS JOIN d b CROSS JOIN d c CROSS JOIN d e CROSS JOIN d f WHERE f.n < 5;
INSERT INTO bb SELECT id, (id * 7) % 50000, v FROM ba;
"""


def fmt(v):
    if v is None: return 'NULL'
    if isinstance(v, float): return format(v, '.4f')
    return str(v)


def case(name, header, setup, steps):
    """steps: ('exec', mysql, sqlite) | ('query', mysql, sqlite[, maxms])"""
    db = sqlite3.connect(':memory:')
    db.executescript(setup)
    out = [f'-- {header}', '-- setup', setup.strip(), '']
    for st in steps:
        kind, my, lite = st[0], st[1], st[2]
        if kind == 'exec':
            db.execute(lite)
            out += ['-- exec', my, '']
        else:
            rows = db.execute(lite).fetchall()
            out += ['-- query', my, '-- expect'] + ['|'.join(fmt(c) for c in r) for r in rows] + ['']
            if len(st) > 3: out += [f'-- maxms {st[3]}', '']
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, name + '.case'), 'w') as f: f.write('\n'.join(out))
    print(name, 'written')


q = lambda my, lite=None, *ms: ('query', my, lite or my, *ms)
x = lambda my, lite=None: ('exec', my, lite or my)

case('comma', 'comma (old-style) joins: FROM a, b WHERE ...; the WHERE equalities must still join by hash', BASE + BIG, [
    q("SELECT customers.name, orders.id FROM customers, orders WHERE orders.cust_id = customers.id"),
    q("SELECT x.name, y.id FROM customers AS x, orders AS y WHERE y.cust_id = x.id AND y.status = 1"),
    q("SELECT c.name, i.sku FROM customers c, orders o, items i WHERE o.cust_id = c.id AND i.order_id = o.id AND i.sku = 'A'"),
    q("SELECT c.name, o.id, i.sku FROM customers c, orders o JOIN items i ON i.order_id = o.id WHERE o.cust_id = c.id AND i.sku = 'C'"),
    q("SELECT c.name, o.id FROM orders o, customers c WHERE c.id = o.cust_id AND (o.status = 0 OR c.region = 'S')"),
    q("SELECT COUNT(*) FROM customers, products"),
    q("SELECT c.name, COUNT(*) FROM customers c, orders o WHERE o.cust_id = c.id GROUP BY c.name"),
    q("SELECT COUNT(*), SUM(bb.w) FROM ba, bb WHERE ba.id = bb.a_id", None, 15000),
    x("UPDATE orders, customers c SET orders.status = 8 WHERE c.id = orders.cust_id AND c.region = 'S'",
      "UPDATE orders SET status = 8 WHERE cust_id IN (SELECT id FROM customers WHERE region = 'S')"),
    q("SELECT id, status FROM orders WHERE status = 8"),
])

case('delete', "multi-table DELETE: DELETE t FROM t JOIN ..., an aliased target, several targets, DELETE FROM t USING ...", BASE, [
    x("DELETE o FROM orders o LEFT JOIN customers c ON c.id = o.cust_id WHERE c.id IS NULL",
      "DELETE FROM orders WHERE cust_id IS NULL OR cust_id NOT IN (SELECT id FROM customers)"),
    q("SELECT id FROM orders"),
    x("DELETE i FROM items i JOIN products p ON p.sku = i.sku WHERE p.price < 2",
      "DELETE FROM items WHERE sku IN (SELECT sku FROM products WHERE price < 2)"),
    q("SELECT order_id, line FROM items"),
    x("DELETE s FROM orders s WHERE s.id = 13", "DELETE FROM orders WHERE id = 13"),
    q("SELECT id FROM orders"),
    x("DELETE FROM items USING items JOIN orders ON orders.id = items.order_id WHERE orders.status = 1",
      "DELETE FROM items WHERE order_id IN (SELECT id FROM orders WHERE status = 1)"),
    q("SELECT order_id, line FROM items"),
    x("DELETE o, i FROM orders o JOIN items i ON i.order_id = o.id WHERE o.id = 10", "DELETE FROM items WHERE order_id = 10"),
    q("SELECT id FROM orders", "SELECT id FROM orders WHERE id <> 10"),
    q("SELECT order_id, line FROM items"),
])

case('hints', 'STRAIGHT_JOIN as the join operator, and index hints (accepted; the engine picks its own plan)', BASE, [
    q("SELECT c.name, o.id FROM customers c STRAIGHT_JOIN orders o ON o.cust_id = c.id WHERE c.id = 2",
      "SELECT c.name, o.id FROM customers c JOIN orders o ON o.cust_id = c.id WHERE c.id = 2"),
    q("SELECT STRAIGHT_JOIN c.name, o.id FROM customers c JOIN orders o ON o.cust_id = c.id WHERE o.id = 11",
      "SELECT c.name, o.id FROM customers c JOIN orders o ON o.cust_id = c.id WHERE o.id = 11"),
    q("SELECT c.name, o.id FROM orders o USE INDEX (PRIMARY) JOIN customers c ON c.id = o.cust_id WHERE o.id = 10",
      "SELECT c.name, o.id FROM orders o JOIN customers c ON c.id = o.cust_id WHERE o.id = 10"),
    q("SELECT c.name, o.id FROM customers c FORCE INDEX (PRIMARY) JOIN orders o ON o.cust_id = c.id WHERE o.id = 12",
      "SELECT c.name, o.id FROM customers c JOIN orders o ON o.cust_id = c.id WHERE o.id = 12"),
    q("SELECT o.id FROM orders o IGNORE INDEX (PRIMARY) WHERE o.amount > 99", "SELECT o.id FROM orders o WHERE o.amount > 99"),
    q("SELECT c.name, o.id FROM customers AS c USE INDEX FOR JOIN (PRIMARY) LEFT JOIN orders o FORCE KEY (PRIMARY) ON o.cust_id = c.id WHERE c.id IN (4, 5)",
      "SELECT c.name, o.id FROM customers AS c LEFT JOIN orders o ON o.cust_id = c.id WHERE c.id IN (4, 5)"),
])

case('using', 'JOIN ... USING (columns), inner and LEFT', BASE, [
    q("SELECT o.id, p.title FROM items i JOIN products p USING (sku) JOIN orders o ON o.id = i.order_id WHERE i.sku = 'B'"),
    q("SELECT a.qty, b.qty FROM items a JOIN items b USING (order_id, line) WHERE a.order_id = 16"),
    q("SELECT i.order_id, p.title FROM items i LEFT JOIN products p USING (sku)"),
    q("SELECT sku, title FROM items JOIN products USING (sku) WHERE order_id = 10"),
    q("SELECT * FROM items JOIN products USING (sku) WHERE order_id = 16",
      "SELECT items.sku, order_id, line, qty, title, price FROM items JOIN products ON products.sku = items.sku WHERE order_id = 16"),
])

case('natural', 'NATURAL [LEFT] JOIN: joined on every column name the two share', BASE, [
    q("SELECT order_id, sku, title FROM items NATURAL JOIN products"),
    q("SELECT order_id, sku, title FROM items NATURAL LEFT JOIN products WHERE order_id IN (13, 16)"),
    q("SELECT * FROM items NATURAL JOIN products WHERE order_id = 10",
      "SELECT items.sku, order_id, line, qty, title, price FROM items JOIN products ON products.sku = items.sku WHERE order_id = 10"),
    q("SELECT COUNT(*) FROM customers NATURAL JOIN products"),
])

case('parens', 'nested parenthesised joins: a JOIN (b JOIN c ON ...) ON ...', BASE, [
    q("SELECT c.name, i.sku FROM customers c JOIN (orders o JOIN items i ON i.order_id = o.id) ON o.cust_id = c.id"),
    q("SELECT c.name, o.id, i.sku FROM customers c LEFT JOIN (orders o JOIN items i ON i.order_id = o.id AND i.sku = 'C') ON o.cust_id = c.id"),
    q("SELECT c.name, p.title FROM (customers c JOIN orders o ON o.cust_id = c.id) JOIN (items i JOIN products p ON p.sku = i.sku) ON i.order_id = o.id"),
    q("SELECT c.name, o.id FROM ((customers c JOIN orders o ON o.cust_id = c.id)) WHERE o.status = 1"),
])

# Joins that already worked, and the new forms mixed with them: nothing may change.
case('regress', 'join shapes that must keep their results (and new forms mixed with the old)', BASE, [
    q("SELECT c.name, o.id FROM customers c JOIN orders o ON o.cust_id = c.id"),
    q("SELECT c.name, o.id FROM customers c LEFT JOIN orders o ON o.cust_id = c.id"),
    q("SELECT c.name, o.id FROM customers c RIGHT JOIN orders o ON o.cust_id = c.id"),
    q("SELECT c.name FROM customers c LEFT JOIN orders o ON o.cust_id = c.id WHERE o.id IS NULL"),
    q("SELECT c.name, SUM(i.qty * p.price) FROM customers c JOIN orders o ON o.cust_id = c.id JOIN items i ON i.order_id = o.id JOIN products p ON p.sku = i.sku GROUP BY c.name"),
    q("SELECT c.name, o.id, i.sku, p.title FROM customers c LEFT JOIN orders o ON o.cust_id = c.id LEFT JOIN items i ON i.order_id = o.id LEFT JOIN products p ON p.sku = i.sku"),
    q("SELECT a.id, b.id FROM orders a JOIN orders b ON a.cust_id = b.cust_id AND a.id < b.id"),
    q("SELECT a.id, b.id FROM orders a JOIN orders b ON a.amount < b.amount AND b.status = 1"),
    q("SELECT c.name, o.id FROM customers c JOIN orders o ON o.cust_id = c.id OR o.amount > 200"),
    q("SELECT c.id, r.code FROM customers c CROSS JOIN regions r"),
    q("SELECT c.name, o.id FROM customers c CROSS JOIN orders o WHERE o.cust_id = c.id AND o.status = 0"),
    q("SELECT c.name, o.id FROM customers c JOIN orders o WHERE o.cust_id = c.id"),
    q("SELECT c.name, r.label, o.id FROM customers c LEFT JOIN regions r ON r.code = c.region, orders o WHERE o.cust_id = c.id"),
    q("SELECT c.name, o.id FROM customers c, orders o WHERE o.cust_id = c.id OR (c.id = 4 AND o.id = 14)"),
    q("SELECT c.name, o.id FROM regions r, customers c RIGHT JOIN orders o ON o.cust_id = c.id WHERE r.code = 'N'"),
    q("SELECT c.name, t.n FROM customers c JOIN (SELECT cust_id, COUNT(*) AS n FROM orders GROUP BY cust_id) t ON t.cust_id = c.id"),
    q("SELECT c.name, t.n FROM customers c, (SELECT cust_id, COUNT(*) AS n FROM orders GROUP BY cust_id) t WHERE t.cust_id = c.id"),
    q("WITH t AS (SELECT cust_id, SUM(amount) AS s FROM orders GROUP BY cust_id) SELECT c.name, t.s FROM customers c JOIN t ON t.cust_id = c.id"),
    q("SELECT c.name FROM customers c WHERE EXISTS (SELECT 1 FROM orders o WHERE o.cust_id = c.id AND o.status = 1)"),
    q("SELECT c.name FROM customers c WHERE c.id IN (SELECT o.cust_id FROM orders o, items i WHERE i.order_id = o.id AND i.sku = 'C')"),
    q("SELECT a.order_id, a.line, b.line FROM items a JOIN items b ON a.order_id = b.order_id AND a.line <> b.line"),
    q("SELECT o.id, c.name FROM orders o LEFT JOIN customers c ON c.id = o.cust_id"),
    q("SELECT c.name, o.id FROM customers c LEFT JOIN orders o ON o.cust_id = c.id AND o.status = 1"),
    q("SELECT c.name, o.id FROM customers c FULL OUTER JOIN orders o ON o.cust_id = c.id"),
    q("SELECT c.name, COALESCE(SUM(o.amount), 0) FROM customers c LEFT JOIN orders o ON o.cust_id = c.id GROUP BY c.name"),
    q("SELECT c.name, (SELECT MAX(o.amount) FROM orders o JOIN items i ON i.order_id = o.id WHERE o.cust_id = c.id) FROM customers c"),
    q("SELECT v.name, p.title FROM v_lines v JOIN products p ON p.sku = v.sku"),
    q("SELECT v.name, p.title FROM v_lines v, products p WHERE p.sku = v.sku"),
    q("SELECT c.name, r.label FROM customers c LEFT JOIN (regions r JOIN customers c2 ON c2.region = r.code AND c2.id = 1) ON r.code = c.region"),
    q("SELECT c.name FROM customers c, orders o WHERE o.cust_id = c.id AND o.status = 0 UNION SELECT c.name FROM customers c JOIN orders o ON o.cust_id = c.id WHERE o.status = 1"),
    q("SELECT COUNT(*) FROM regions NATURAL JOIN products"),
    q("SELECT o.id, i.line, p.title FROM orders o JOIN items i ON i.order_id = o.id JOIN products p USING (sku) WHERE o.status = 1",
      "SELECT o.id, i.line, p.title FROM orders o JOIN items i ON i.order_id = o.id JOIN products p ON p.sku = i.sku WHERE o.status = 1"),
])

# The ERP's stocktrans view (9 tables, inner and LEFT joins, as on chi-il-1), joined the ways its reports join it:
# st's `productlinepoints p JOIN stocktrans s ON p.lineid = s.lineid` read stockmaster.lineid before it had joined.
random.seed(7)
rows = {t: [] for t in ['stockmoves', 'debtortrans', 'custbranch', 'territory', 'salesorders', 'salesman', 'stockmaster', 'product_to_manufacturer', 'manufacturer', 'productlinepoints']}
for t in range(1, 4): rows['territory'].append((t, f"'T{t}'"))
for dn in range(1, 30):
    for br in ('A', 'B'): rows['custbranch'].append((f"'D{dn}'", f"'{br}'", f"'Br{dn}{br}'", random.randint(1, 4)))
for s in range(1, 6): rows['salesman'].append((f"'S{s}'", f"'Sales{s}'"))
for m in range(1, 4): rows['manufacturer'].append((m, f"'Brand{m}'"))
skus = [f'K{i}' for i in range(1, 40)] + ['DWI-1', 'DWI-2']
for k in skus:
    if k != 'K39': rows['stockmaster'].append((f"'{k}'", f"'Desc {k}'", "'C'", random.randint(1, 6)))
    if random.random() < .7: rows['product_to_manufacturer'].append((f"'{k}'", random.randint(1, 5)))
for ln in range(1, 6): rows['productlinepoints'].append((ln, f"'Line{ln}'", ln * 10, ln))
mv, seen = 0, set()
for tr in range(1, 400):
    dn, br, typ = random.randint(1, 31), random.choice('AB'), random.choice([10, 11, 12])
    rows['debtortrans'].append((tr, tr, typ, f"'D{dn}'", f"'{br}'", tr if random.random() < .8 else 99999, f"'{random.choice(['S1', 'S2', 'S9'])}'"))
    if random.random() < .8 and tr not in seen:
        seen.add(tr)
        rows['salesorders'].append((tr, 1, f"'u{tr % 5}'"))
    for _ in range(random.randint(1, 4)):
        mv += 1
        rows['stockmoves'].append((mv, f"'{random.choice(skus)}'", typ, tr, f"'D{dn}'", f"'{br}'", f"'2026-0{random.randint(1, 9)}-1{random.randint(0, 9)}'", random.randint(1, 9), round(random.uniform(1, 100), 2)))
VIEWS = """CREATE TABLE stockmoves (stkmoveno INT PRIMARY KEY, stockid VARCHAR(20), type INT, transno INT, debtorno VARCHAR(10), branchcode VARCHAR(10), trandate DATE, qty INT, price DOUBLE);
CREATE TABLE debtortrans (id INT PRIMARY KEY, transno INT, type INT, debtorno VARCHAR(10), branchcode VARCHAR(10), order_ INT, salesperson VARCHAR(5));
CREATE TABLE custbranch (debtorno VARCHAR(10), branchcode VARCHAR(10), brname VARCHAR(30), territory_id INT, PRIMARY KEY (debtorno, branchcode));
CREATE TABLE territory (territory_id INT PRIMARY KEY, territory_name VARCHAR(20));
CREATE TABLE salesorders (orderno INT PRIMARY KEY, placedfrom INT, enteredby VARCHAR(10));
CREATE TABLE salesman (salesmancode VARCHAR(5) PRIMARY KEY, salesmanname VARCHAR(20));
CREATE TABLE stockmaster (stockid VARCHAR(20) PRIMARY KEY, description VARCHAR(30), categoryid VARCHAR(5), lineid INT);
CREATE TABLE product_to_manufacturer (stockid VARCHAR(20) PRIMARY KEY, manufacturer_id INT);
CREATE TABLE manufacturer (manufacturer_id INT PRIMARY KEY, name VARCHAR(20));
CREATE TABLE productlinepoints (lineid INT PRIMARY KEY, name VARCHAR(20), dvalue INT, points INT);
""" + ''.join(f"INSERT INTO {t} VALUES " + ','.join('(' + ','.join(str(v) for v in r) + ')' for r in rs) + ';\n' for t, rs in rows.items() if rs) + """CREATE VIEW stocktrans AS select debtortrans.transno AS transno, debtortrans.type AS systype, debtortrans.order_ AS orderno, debtortrans.debtorno AS debtorno, stockmoves.stockid AS stockid, stockmoves.trandate AS trandate, stockmoves.stkmoveno AS stkmoveno, stockmoves.qty AS qty, salesorders.enteredby AS enteredby, (stockmoves.price * stockmoves.qty) AS discountedlinetotal, salesman.salesmanname AS salesmanname, custbranch.brname AS brname, territory.territory_name AS territory_name, stockmaster.description AS description, stockmaster.lineid AS lineid, manufacturer.name AS BRANDNAME from stockmoves join debtortrans on (stockmoves.transno = debtortrans.transno and stockmoves.type = debtortrans.type and stockmoves.debtorno = debtortrans.debtorno and stockmoves.branchcode = debtortrans.branchcode) join custbranch on (custbranch.debtorno = debtortrans.debtorno and custbranch.branchcode = debtortrans.branchcode) join territory on (territory.territory_id = custbranch.territory_id) left join salesorders on (debtortrans.order_ = salesorders.orderno) left join salesman on (salesman.salesmancode = debtortrans.salesperson) join stockmaster on (stockmaster.stockid = stockmoves.stockid) left join product_to_manufacturer on (stockmaster.stockid = product_to_manufacturer.stockid) left join manufacturer on (manufacturer.manufacturer_id = product_to_manufacturer.manufacturer_id) where (stockmoves.type = 10 or stockmoves.type = 11) and stockmoves.stockid not like 'DWI-%';
CREATE VIEW plp AS SELECT lineid, name, dvalue, points FROM productlinepoints;
"""
case('views', "the ERP's 9-table stocktrans view joined the ways its reports join it", VIEWS, [
    q("select `p`.`lineid` AS `lineid`,`p`.`name` AS `name`,`p`.`dvalue` AS `dvalue`,`p`.`points` AS `points`,sum(`s`.`discountedlinetotal` * -1) AS `total` from (`productlinepoints` `p` join `stocktrans` `s` on(`p`.`lineid` = `s`.`lineid`)) where 1 group by `p`.`lineid`",
      "select p.lineid, p.name, p.dvalue, p.points, sum(s.discountedlinetotal * -1) from productlinepoints p join stocktrans s on p.lineid = s.lineid group by p.lineid"),
    q("SELECT s.lineid, p.name, COUNT(*) FROM stocktrans s JOIN plp p ON p.lineid = s.lineid GROUP BY s.lineid, p.name"),
    q("SELECT p.name, s.stkmoveno, s.BRANDNAME FROM productlinepoints p JOIN stocktrans s ON p.lineid = s.lineid AND s.qty > 5"),
    q("SELECT p.name, s.stkmoveno FROM productlinepoints p, stocktrans s WHERE p.lineid = s.lineid AND p.points >= 2"),
    q("SELECT s.stkmoveno, sm.salesmanname FROM stocktrans s LEFT JOIN salesman sm ON sm.salesmancode = 'S1' AND s.qty > 7"),
    q("SELECT p.name, SUM(s.qty) FROM productlinepoints p JOIN stocktrans s ON p.lineid = s.lineid WHERE s.trandate >= '2026-05-01' AND s.BRANDNAME IS NOT NULL GROUP BY p.name"),
    q("SELECT * FROM productlinepoints p JOIN stocktrans s ON p.lineid = s.lineid WHERE s.stkmoveno < 20"),
])
