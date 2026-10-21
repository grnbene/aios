# 40 orders, 3 lines each over the same 3 items in a rotating order, with their pick rows
items = [('SHAMPOO', 9.5), ('CONDITION', 11.0), ('DRYER', 89.0)]
bins = {'SHAMPOO': 'A1', 'CONDITION': 'A2', 'DRYER': 'B1'}
for o in range(1, 41):
    cust = 'C1' if o % 2 else 'C2'
    print(f"INSERT INTO salesorders (orderno, debtorno, branchcode, customerref, orddate, ordertype, shipvia, fromstkloc, salesperson, payterms, placedfrom, quotation, comments) VALUES ({o}, '{cust}', '{cust}', 'PO{o}', '2026-10-14', 'RE', 1, 'MAIN', 'S1', '30', 1, 0, '')")
    rot = items[o % 3:] + items[:o % 3]
    vals = ", ".join(f"({n}, {o}, '{s}', '{s}', 0, {p}, {1 + (o + n) % 3}, 0, 0, 0)" for n, (s, p) in enumerate(rot))
    print(f"INSERT INTO salesorderdetails (orderlineno, orderno, stkcode, itemdesc, qtyinvoiced, unitprice, quantity, discountpercent, completed, estimate) VALUES {vals}")
    print(f"INSERT INTO oms (orderno, status, invoiced) VALUES ({o}, 1, 0)")
    vals = ", ".join(f"('{s}', 'MAIN', '{bins[s]}', {n}, {1 + (o + n) % 3}, {1 + (o + n) % 3}, {1 + (o + n) % 3}, 0, {o}, 0, 0, 0)" for n, (s, _) in enumerate(rot))
    print(f"INSERT INTO pickingfreeze (stockid, loccode, bin, linenumber, ordered, pick, picked, completed, orderno, invoiced, cleared, dropship) VALUES {vals}")
