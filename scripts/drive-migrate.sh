#!/usr/bin/env bash
# A living computation moves between kernels and CARRIES ON.
#
# Not "the files were copied and the program started again": a graph is run
# half way on one kernel, checkpointed where it stands, and finished by a
# DIFFERENT kernel — which must resolve the operations' device requirement
# against its own fabric, because its devices are different objects.
#
# The verdict has an outside oracle: the same computation, uninterrupted, on a
# third kernel. The migrated run has to produce those numbers exactly, and it
# has to skip the operations it had already done.
#
#   scripts/drive-migrate.sh [build-dir] [work-dir] [device-name]
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
build=${1:-$here/../build}
work=${2:-$build/migrate}
dev=${3:-cpu}
aios=$build/aios
[ -x "$aios" ] || { echo "no aios binary at $aios"; exit 1; }
rm -rf "$work"; mkdir -p "$work"
fail() { echo "MIGRATION: FAIL — $*"; exit 1; }

# The graph: a matmul, two operations that depend on its result, and a reduction
# over it. Four operations, so it can be stopped with real work still pending
# and an intermediate result that exists only inside the running thing.
graph_body() {
  cat <<AOS
entity create worker                            => w
cap grant \$w kernel * schedule
cap grant \$w compute * execute
cap grant \$w kernel * checkpoint
tensor create \$w f32 2x3 1 2 3 4 5 6           => A
tensor create \$w f32 3x2 7 8 9 10 11 12        => B
tensor create \$w f32 2x2                       => C
tensor create \$w f32 2x2                       => D
tensor create \$w f32 1                         => S
graph create \$w                                => g
graph op \$w \$g matmul in=\$A:f32:2x3 in=\$B:f32:3x2 out=\$C:f32:2x2 par=2 prio=200 dev=$dev
graph op \$w \$g relu in=\$C:f32:2x2 out=\$D:f32:2x2 dev=$dev
graph op \$w \$g softmax in=\$D:f32:2x2 out=\$D:f32:2x2 dev=$dev
graph op \$w \$g reduce-sum in=\$C:f32:2x2 out=\$S:f32:1 prio=50 dev=$dev
AOS
}

# ---- the oracle: never interrupted, on a kernel of its own -------------------
{ graph_body; echo "graph run \$w \$g"; echo "tensor show \$w \$C f32 2x2"; echo "tensor show \$w \$D f32 2x2"; echo "tensor show \$w \$S f32 1"; } > "$work/oracle.aos"
"$aios" --root "$work/root-oracle" run "$work/oracle.aos" > "$work/oracle.out" 2>&1 || fail "the oracle run itself failed; see $work/oracle.out"
oracle=$(grep -oE '= \[[^]]*\]' "$work/oracle.out" | tr -d '\n')
[ -n "$oracle" ] || fail "the oracle produced no tensor values"

# ---- kernel A: run half of it, then checkpoint where it stands ---------------
{ echo "info"; graph_body; echo "graph run \$w \$g 2"; echo "graph show \$g"; echo "checkpoint \$w"; } > "$work/a.aos"
"$aios" --root "$work/root-a" run "$work/a.aos" > "$work/a.out" 2>&1 || fail "kernel A failed; see $work/a.out"
grep -q 'position 2 (resumable)' "$work/a.out" || fail "kernel A did not stop mid-graph (expected position 2)"
kernel_a=$(grep -m1 'Kernel ID' "$work/a.out" | awk '{print $NF}')
entity=$(grep -m1 -oE '^graph [0-9a-f]+ owner=[0-9a-f]+' "$work/a.out" | awk '{print $3}' | cut -d= -f2)
graph=$(grep -m1 -oE '^graph [0-9a-f]{12}' "$work/a.out" | awk '{print $2}')
ckpt=$(grep -m1 -oE '^checkpoint .*\.aoc' "$work/a.out" | awk '{print $2}')
outs=$(grep -oE 'out f32\[[0-9x]+\]@[0-9a-f]{12}' "$work/a.out" | grep -oE '[0-9a-f]{12}$' | awk '!seen[$0]++')
mem_c=$(echo "$outs" | awk 'NR==1'); mem_d=$(echo "$outs" | awk 'NR==2'); mem_s=$(echo "$outs" | awk 'NR==3')
[ -n "$entity" ] && [ -n "$graph" ] && [ -n "$ckpt" ] && [ -n "$mem_s" ] || fail "could not read A's identities out of $work/a.out"
echo "kernel A $kernel_a ran 2 of 4 operations on '$dev', stopped at position 2, checkpointed $(wc -c < "$ckpt" | tr -d ' ') bytes"

# ---- kernel B: a different kernel finishes it -------------------------------
{
  echo "info"
  echo "restore $ckpt"
  echo "graph run $entity $graph"
  echo "tensor show $entity $mem_c f32 2x2"
  echo "tensor show $entity $mem_d f32 2x2"
  echo "tensor show $entity $mem_s f32 1"
} > "$work/b.aos"
"$aios" --root "$work/root-b" run "$work/b.aos" > "$work/b.out" 2>&1 || fail "kernel B could not carry on; see $work/b.out"
kernel_b=$(grep -m1 'Kernel ID' "$work/b.out" | awk '{print $NF}')
[ "$kernel_a" = "$kernel_b" ] && fail "A and B are the same kernel ($kernel_a); this proves nothing about moving"
grep -q "restored entity ${entity}" "$work/b.out" || fail "B restored a different entity than A checkpointed"
grep -qE 'ran 2/4 ops, position 4 \(complete\)' "$work/b.out" || fail "B did not resume: it should run the 2 REMAINING operations and complete (see $work/b.out)"
migrated=$(grep -oE '= \[[^]]*\]' "$work/b.out" | tr -d '\n')
[ "$migrated" = "$oracle" ] || fail "the migrated computation disagrees with the oracle
  oracle:   $oracle
  migrated: $migrated"
device_b=$(grep -m1 -oE '→ [^ ]+\([0-9a-f]+\)' "$work/b.out" | head -1)
echo "kernel B ${kernel_b:-?} restored $entity at position 2 and ran the REMAINING 2 operations to completion on ${device_b:-its own device}"
echo "results match the uninterrupted oracle exactly: $oracle"
echo "MIGRATION: PASS — a half-finished computation crossed kernels on '$dev' and carried on without redoing work"
