#!/usr/bin/env python3
"""apir/orders through the open_orders_view Thing (&thing=1) against the usual path (&nothing=1).

  openorders_parity.py 'https://api3.qbcsys.com/?route=apir/orders&debtorno=&loccode=00&key=...&userid=JACK'

Fetches the Thing path cold, the usual path, then the Thing path warm, and diffs
the warm answer against the usual one order by order, field by field.
"""
import json
import sys
import time
import urllib.request


def fetch(url):
    req = urllib.request.Request(url, headers={"Accept-Encoding": "identity"})
    t0 = time.time()
    with urllib.request.urlopen(req, timeout=300) as r:
        body = r.read()
    return json.loads(body), time.time() - t0, body


def diffs(a, b, path, out, limit):
    if len(out) >= limit:
        return
    if type(a) is not type(b):
        out.append(f"{path}: {a!r} vs {b!r}")
    elif isinstance(a, dict):
        for k in list(a.keys()) + [k for k in b.keys() if k not in a]:
            if k not in a or k not in b:
                out.append(f"{path}.{k}: {'missing' if k not in a else repr(a[k])} vs {'missing' if k not in b else repr(b[k])}")
            else:
                diffs(a[k], b[k], f"{path}.{k}", out, limit)
        if list(a.keys()) != list(b.keys()) and set(a.keys()) == set(b.keys()):
            out.append(f"{path}: same keys, different order")
    elif isinstance(a, list):
        if len(a) != len(b):
            out.append(f"{path}: {len(a)} items vs {len(b)}")
        for i, (x, y) in enumerate(zip(a, b)):
            diffs(x, y, f"{path}[{i}]", out, limit)
    elif a != b:
        out.append(f"{path}: {a!r} vs {b!r}")


def main():
    base = sys.argv[1]
    sep = "&" if "?" in base else "?"
    cold, t_cold, n_cold = fetch(base + sep + "thing=1")
    usual, t_usual, n_usual = fetch(base + sep + "nothing=1")
    warm, t_warm, n_warm = fetch(base + sep + "thing=1")
    print(f"thing cold  {t_cold:6.2f}s  {len(n_cold):>10} bytes  {len(cold)} orders")
    print(f"usual       {t_usual:6.2f}s  {len(n_usual):>10} bytes  {len(usual)} orders")
    print(f"thing warm  {t_warm:6.2f}s  {len(n_warm):>10} bytes  {len(warm)} orders")

    u_order = [o["orderno"] for o in usual]
    w_order = [o["orderno"] for o in warm]
    print("same orders, same order" if u_order == w_order else
          f"ORDER DIFFERS: only usual {sorted(set(u_order) - set(w_order))[:20]}, only thing {sorted(set(w_order) - set(u_order))[:20]}")

    by_no = {}
    for o in warm:
        by_no.setdefault(o["orderno"], []).append(o)
    bad = 0
    for i, u in enumerate(usual):
        w = by_no.get(u["orderno"], [])
        if not w:
            continue
        out = []
        diffs(u, w.pop(0), f"[{u['orderno']}]", out, 12)
        if out:
            bad += 1
            if bad <= 15:
                print("\n".join("  " + d for d in out))
    print(f"{bad} of {len(usual)} orders differ in content")
    print("IDENTICAL" if bad == 0 and u_order == w_order and n_usual == n_warm else "NOT IDENTICAL")


if __name__ == "__main__":
    main()
