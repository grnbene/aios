#!/usr/bin/env bash
# aios-git's packs, held against real git over smart HTTP in both directions: a packed repository is served as it is
# stored (deltas included, nothing recompressed), a fetched pack is kept as a pack with an index real git verifies, and
# the cases around them (incremental fetches with real git's multi_ack_detailed negotiation, thin packs pushed by real
# git, thin bundles, shallow clones, a checkout out of a kept pack) all end in repositories `git fsck --strict` passes.
#
#   tools/proof/aios-git/packs.sh <aios-git>          needs git, node and python3; prints ok/FAIL lines, exit 1 on a FAIL
set -u
[ -x "${1:-}" ] || { echo "usage: packs.sh <aios-git>" >&2; exit 2; }
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(mktemp -d)"; trap 'pkill -f "[p]acks-http.mjs $WORK" 2>/dev/null; rm -rf "$WORK"' EXIT
git --version | grep -q "(aios)" && { echo "packs.sh: git on PATH is aios-git; the oracle must be real git" >&2; exit 2; }
cd "$WORK"
G="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"; RG=git; FAILED=
export GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@t GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@t
P1=$((20000 + RANDOM % 9000)); P2=$((P1+1))
ok(){ echo "ok   $1"; }; no(){ echo "FAIL $1"; FAILED=1; }
clean(){ [ -z "$($RG -C $1 fsck --strict --full 2>&1 | grep -v '^Checking\|^dangling')" ]; }
$RG init -q -b main src && cd src && python3 -c "
import random;random.seed(3)
open('big.txt','w').write('\n'.join(' '.join(str(random.random()) for _ in range(8)) for _ in range(4000)))" && $RG add -A && $RG commit -qm base
for i in $(seq 1 150); do echo "line $i" >> big.txt; sed -i "$((i*7))s/.*/changed $i/" big.txt; echo $i > f$i.txt; $RG add -A; $RG commit -qm "c$i"; done; cd ..
mkdir srv && $RG clone -q --bare src srv/r.git && $RG -C srv/r.git repack -adfq --depth=50 --window=20
(node "$HERE/packs-http.mjs" "$WORK/srv" $P1 $G 2> srv.log &); (node "$HERE/packs-http.mjs" "$WORK/srv" $P2 $RG 2>/dev/null &); sleep 1
A=http://127.0.0.1:$P1/r.git; R=http://127.0.0.1:$P2/r.git; tip(){ $RG -C $1 rev-parse main; }
$RG clone -q --bare $A c1 && clean c1 && [ "$(tip c1)" = "$(tip src)" ] && ok "aios server -> real client, deep delta chains sent as stored ($(grep -o 'upload-pack: [0-9.]* MB' srv.log | tail -1 | cut -d' ' -f2) MB on the wire, pack on disk $(du -sh srv/r.git/objects/pack/*.pack | cut -f1))" || no 1a
$G clone -q --bare $A c2 && clean c2 && $RG verify-pack c2/objects/pack/*.idx && ok "aios server -> aios client: keeps a pack real git verifies" || no 1b
$G clone -q --bare $R c3 && clean c3 && $RG verify-pack c3/objects/pack/*.idx && ok "real server (offset deltas, depth 50) -> aios client" || no 1c
cd src && for i in $(seq 151 290); do echo "more $i" >> big.txt; echo $i > g$i.txt; $RG add -A; $RG commit -qm "d$i"; done; cd .. && $RG -C src push -q ../srv/r.git main
$RG -C c1 fetch -q $A '+refs/heads/*:refs/heads/*' && [ "$(tip c1)" = "$(tip src)" ] && clean c1 && ok "incremental fetch: real client from aios server (was: bad band #78)" || no 2a
$G -C c2 fetch -q $A '+refs/heads/*:refs/heads/*' && [ "$(tip c2)" = "$(tip src)" ] && clean c2 && ok "incremental fetch: aios client from aios server" || no 2b
$G -C c3 fetch -q $R '+refs/heads/*:refs/heads/*' && [ "$(tip c3)" = "$(tip src)" ] && clean c3 && ok "incremental fetch of 420 objects: aios client from real server ($(ls c3/objects/pack/*.pack | wc -l) packs)" || no 2c
$RG clone -q $A w && cd w && for i in $(seq 1 60); do echo "p $i" >> big.txt; echo $i > h$i.txt; $RG add -A; $RG commit -qm "p$i"; done && $RG push -q origin main 2>&1 | tail -2; cd ..
[ "$(tip srv/r.git)" = "$(tip w)" ] && clean srv/r.git && ok "real git pushes 180 objects (a thin pack) to the aios server" || no 3
$G clone -q $A wa && cd wa && $G config user.name t && $G config user.email t@t && for i in $(seq 1 120); do echo "q $i" >> big.txt; echo $i > k$i.txt; $G add -A; $G commit -qm "q$i"; done && $G push -q origin main 2>&1 | tail -2; cd ..
[ "$(tip srv/r.git)" = "$(tip wa)" ] && clean srv/r.git && ok "aios client pushes 360 objects to the aios server" || no 3b
$RG -C w pull -q origin main 2>&1 | tail -1; [ "$(tip w)" = "$(tip wa)" ] && clean w && ok "real client pulls them back" || no 3c
$RG -C wa bundle create ../inc.bundle main~40..main 2>/dev/null; $G clone -q --bare $A c4 && $G -C c4 update-ref refs/heads/main $($RG -C wa rev-parse main~40) && $G -C c4 fetch -q ../inc.bundle '+refs/heads/main:refs/heads/fromb' 2>&1 | tail -1; [ "$($RG -C c4 rev-parse fromb 2>/dev/null)" = "$(tip wa)" ] && clean c4 && ok "a thin bundle (deltas on objects outside it) is taken in" || no 4
$G clone -q $A w2 && $RG -C w checkout -q main && diff -rq w2 w -x .git >/dev/null && ok "checkout out of a kept pack matches real git's work tree" || no 5
$G clone -q --depth 1 $A s1 && [ "$($RG -C s1 rev-list --count HEAD)" = 1 ] && diff -rq s1 w -x .git >/dev/null && ok "shallow clone from the aios server" || no 6
cd s1 && $G config user.name t && $G config user.email t@t && echo s >> big.txt && $G commit -qam shallow-work && $G push -q origin HEAD:refs/heads/from-shallow 2>&1 | tail -1; cd ..; [ "$($RG -C srv/r.git rev-parse from-shallow 2>/dev/null)" = "$($RG -C s1 rev-parse HEAD)" ] && clean srv/r.git && ok "commit and push from a shallow clone" || no 7
all(){ $RG -C $1 rev-list --objects --all | cut -d' ' -f1 | sort | $RG -C $1 cat-file --batch | sha256sum; }
want="$(all srv/r.git)"; size_before=$(du -sk srv/r.git/objects | cut -f1)
$G -C srv/r.git gc -q && clean srv/r.git && [ "$(ls srv/r.git/objects/pack/*.pack | wc -l)" = 1 ] && [ "$($RG -C srv/r.git count-objects | cut -d' ' -f1)" = 0 ] && [ "$(all srv/r.git)" = "$want" ] && $RG verify-pack srv/r.git/objects/pack/*.idx \
  && ok "aios-git gc: one pack, nothing loose, every object byte for byte what it was ($size_before KB -> $(du -sk srv/r.git/objects | cut -f1) KB)" || no 8
$RG clone -q --bare $A c5 && clean c5 && [ "$(tip c5)" = "$(tip wa)" ] && ok "after gc on the server, a real client still clones it" || no 8b
$G -C srv/r.git gc -q && [ "$(ls srv/r.git/objects/pack/*.pack | wc -l)" = 1 ] && clean srv/r.git && ok "gc again changes nothing" || no 8c
# loose objects only (a repository git never packed), with many versions of one file: deltas must make it small
$RG init -q -b main lo && (cd lo && $RG config gc.auto 0 && cp ../src/big.txt . && $RG add -A && $RG commit -qm b && for i in $(seq 1 80); do echo "v $i" >> big.txt; sed -i "$((i*11))s/.*/edit $i/" big.txt; $RG commit -qam "v$i"; done)
want="$(all lo)"; loose_kb=$(du -sk lo/.git/objects | cut -f1); $G -C lo repack -a -d -q && clean lo && [ "$(all lo)" = "$want" ] && packed_kb=$(du -sk lo/.git/objects | cut -f1) && [ $((packed_kb * 4)) -lt $loose_kb ] \
  && ok "repack of loose objects finds the deltas ($loose_kb KB loose -> $packed_kb KB packed; real git reads every object back identical)" || no "9 ($loose_kb KB -> ${packed_kb:-?} KB)"
# a fork that borrows its parent's objects: repacking it packs its own objects only and leaves the parent's where they are
$G clone -q --bare --shared srv/r.git fk.git && $RG clone -q fk.git fw 2>/dev/null && (cd fw && echo fork >> big.txt && $RG commit -qam fork && $RG push -q ../fk.git HEAD:main) && parent_before="$(ls srv/r.git/objects/pack)" \
  && $G -C fk.git repack -a -d -q && clean fk.git && [ "$(ls srv/r.git/objects/pack)" = "$parent_before" ] && [ "$(du -sk fk.git/objects | cut -f1)" -lt "$(du -sk srv/r.git/objects | cut -f1)" ] && [ "$(tip fk.git)" = "$(tip fw)" ] \
  && ok "repack in a --shared fork keeps to the fork's own objects" || no 10
[ -z "$FAILED" ] && echo "ALL OK" || echo "SOME FAILED"
[ -z "$FAILED" ]
