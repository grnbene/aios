import subprocess
import re
from collections import defaultdict

pw = open("/opt/aios/mysql.pass").read().strip()
out = subprocess.check_output([
    "mysql", "-S", "/opt/aios/run/asql.sock",
    "-uerp", f"-p{pw}",
    "-e", "SHOW ASQL SLOW",
    "-B"
]).decode("utf-8", errors="replace")

lines = out.strip().split("\n")
header = lines[0].split("\t")
print(f"Header: {header}")
rows = [line.split("\t") for line in lines[1:] if line.strip()]
print(f"Total slow queries in log: {len(rows)}")

def normalize_sql(sql):
    s = re.sub(r"'[^']*'", "'?'", sql)
    s = re.sub(r"\b\d+\b", "?", s)
    s = re.sub(r"VALUES\s*\(.*?\)(\s*,\s*\(.*?\))*", "VALUES (...)", s, flags=re.IGNORECASE)
    s = re.sub(r"\s+", " ", s).strip()
    return s

patterns = defaultdict(list)
raw_examples = {}
for r in rows:
    if len(r) < 11:
        continue
    try:
        dur = float(r[4])
    except ValueError:
        continue
    q = r[10]
    norm = normalize_sql(q)
    patterns[norm].append(dur)
    if norm not in raw_examples:
        raw_examples[norm] = q

print("\n--- TOP PATTERNS BY TOTAL TIME (sum ms) ---")
sorted_pats = sorted(patterns.items(), key=lambda x: sum(x[1]), reverse=True)
for pat, durs in sorted_pats[:25]:
    avg_d = sum(durs) / len(durs)
    max_d = max(durs)
    min_d = min(durs)
    total_d = sum(durs)
    print(f"Count: {len(durs):3d} | Total: {total_d:8.1f} ms | Avg: {avg_d:6.2f} ms | Max: {max_d:6.2f} ms")
    print(f"  {raw_examples[pat][:200]}")
    print()

print("\n--- TOP PATTERNS BY MAX DURATION (longest single query) ---")
sorted_by_max = sorted(patterns.items(), key=lambda x: max(x[1]), reverse=True)
for pat, durs in sorted_by_max[:15]:
    max_d = max(durs)
    avg_d = sum(durs) / len(durs)
    print(f"Max: {max_d:6.2f} ms | Avg: {avg_d:6.2f} ms | Count: {len(durs):3d}")
    print(f"  {raw_examples[pat][:200]}")
    print()
