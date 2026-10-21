#!/usr/bin/env bash
# Rebuild the SAT benchmark data these harnesses expect, exactly:
#   /tmp/satbench/<family>/   SATLIB sets, '%' trailers stripped (as benchmarked)
#   /tmp/satref/kissat, /tmp/satref/drat-trim   the reference solver and checker,
#                             built at the commits the published numbers used
#   /nvme/tmp/sc2023/         SAT Competition 2023 main-track instances under
#                             15 MB compressed (363 of results/sc2023-main-track.uris)
# A tarball whose sha256 differs from the one measured is refused.
#   fetch-benchmarks.sh [satlib] [refs] [sc2023]    (default: satlib refs)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
what=${*:-satlib refs}
B=https://www.cs.ubc.ca/~hoos/SATLIB/Benchmarks/SAT
declare -A URL=(
  [uf50-218.tar.gz]=RND3SAT [uuf50-218.tar.gz]=RND3SAT [uf100-430.tar.gz]=RND3SAT [uuf100-430.tar.gz]=RND3SAT
  [uf250-1065.tar.gz]=RND3SAT [uuf250-1065.tar.gz]=RND3SAT [flat200-479.tar.gz]=GCP [bmc.tar.gz]=BMC
  [blocksworld.tar.gz]=PLANNING/BlocksWorld [logistics.tar.gz]=PLANNING/Logistics [bf.tar.gz]=DIMACS/BF [ssa.tar.gz]=DIMACS/SSA)
SHA=$(cat <<'SUMS'
8a100ce6753ec4da9068f3915e63b7b95719c468c46cd78b0c9140d7acda5b4d bf.tar.gz
f1f2e630f84975857bec6661a459e3bf1220de857fe3b487bcfe1ef8a93ac3a2 blocksworld.tar.gz
5bda83de61aaa4f7fca7123b259c5d3fc0d6dfe8a22afdf3f09c7530ba3f5330 bmc.tar.gz
a27f57132377dc6be4fd11a938c78e6ec245e65fb33fa7a86579ef986c7c242b flat200-479.tar.gz
dcd3b7abfcf25ddf264d46b6ed95ec597eefcdcad88659877812a427c1c576af logistics.tar.gz
cea3458ff3dcdd757a7997c72da0ab97852cd94aa9fd2ca133262932a68b9b5f ssa.tar.gz
7f691acb8c45790ef8178dd01bb362cf50da608d478be9965599131f0a763e59 uf100-430.tar.gz
ebe9b90d674bfd4d3cc76ed048f144e2728b1758cb8aac157c7ce38bb1d1931d uf250-1065.tar.gz
a40862e52dfdce78620b037254579a02dbda8182d2879433658a4c15cb00327c uf50-218.tar.gz
736c6f94fecae346a00501b197c482e146b1c6ab05fb6d2e4666e520b8e8508a uuf100-430.tar.gz
1b6d59619e3c805947c5602703ff3190dfbfeff55d6cb62187838668b0fd4126 uuf250-1065.tar.gz
24cc9f492673e0f975fecc56b3a4d8ac21c385cc46205d83b7491a787f43e13d uuf50-218.tar.gz
SUMS
)
if [[ " $what " == *" satlib "* ]]; then
  mkdir -p /tmp/satbench && cd /tmp/satbench
  while read -r sum f; do
    [ -f "$f" ] || curl -fsSL -o "$f" "$B/${URL[$f]}/$f"
    echo "$sum  $f" | sha256sum -c --quiet - || { echo "$f: not the file that was measured" >&2; exit 1; }
    d=${f%.tar.gz}; mkdir -p "$d"; tar -xzf "$f" -C "$d"
  done <<< "$SHA"
  # SATLIB files end in '%' then '0'; strip that trailer (drat-trim reads it as a clause)
  find /tmp/satbench -name '*.cnf' -exec sed -i '/^%/,$d' {} +
  echo "satlib: $(find /tmp/satbench -name '*.cnf' | wc -l) instances"
fi
if [[ " $what " == *" refs "* ]]; then
  mkdir -p /tmp/satref && cd /tmp/satref
  [ -d kissat ] || git clone -q https://github.com/arminbiere/kissat.git
  git -C kissat checkout -q 8af8e56 && (cd kissat && ./configure >/dev/null && make -j"$(nproc)" >/dev/null)
  [ -d drat-trim ] || git clone -q https://github.com/marijnheule/drat-trim.git
  git -C drat-trim checkout -q 2e3b2dc && make -C drat-trim >/dev/null
  echo "refs: $(/tmp/satref/kissat/build/kissat --version) and drat-trim built"
fi
if [[ " $what " == *" sc2023 "* ]]; then
  mkdir -p /nvme/tmp/sc2023 && cd /nvme/tmp/sc2023
  while read -r u; do
    h=$(basename "$u"); [ -f "$h.cnf" ] && continue
    s=$(curl -sIL -m 20 "$u" | grep -i '^content-length' | tail -1 | tr -dc 0-9)
    [ -n "$s" ] && [ "$s" -lt 15000000 ] || continue
    curl -fsSL -m 300 "$u" -o "$h.cnf.xz" && unxz -f "$h.cnf.xz"
  done < "$here/results/sc2023-main-track.uris"
  echo "sc2023: $(ls *.cnf | wc -l) instances"
fi
