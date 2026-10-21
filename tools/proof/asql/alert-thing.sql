CREATE THING low_stock GRAIN KEY stockid READS items WATCH items(stockid) AS '
fn enroll() { let out = [] for r in read("SELECT stockid FROM items") { push(out, r.stockid) } return out }
fn shared() { return 1 }
fn things(ks, sh) { let out = {} for r in read("SELECT stockid, qty FROM items WHERE stockid IN (?)", ks) { if r.qty < 0 { error("negative stock for " + r.stockid) } let v = {qty: r.qty} if r.qty < 5 { v.alert = "low: " + str(r.qty) + " left" } out[r.stockid] = v } return out }
'
