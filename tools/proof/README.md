# Proof harnesses

The scripts that produced the numbers and the correctness claims in the commit
log, kept so the claims can be re-run instead of re-believed. They were written
in /tmp while driving the real thing and still assume its paths (a frozen
binary under /tmp, a scratch root under /tmp); each says so at its top or in
its first lines. None of them holds a secret: passwords come from files
(/tmp/gt.pass, /opt/aios/mysql.pass) that are never committed, and `pw` is the
root password of a throwaway local MariaDB container (`docker ... mariadb:10.9`
as mdb109 on 127.0.0.1:13309).

Harnesses that read real customer data live on st, not here
(/root/aios-proof there): Things vs MariaDB, the ERP corpus A/B gate, the
reorder and price-book proofs.

## sat/ - the solver against kissat, and against itself

- `fetch-benchmarks.sh [satlib] [refs] [sc2023]` - the data: SATLIB sets
  (sha256-pinned to the tarballs measured, '%' trailers stripped), kissat and
  drat-trim at the commits used, SAT Competition 2023 main-track instances
  under 15 MB compressed (the 363 behind `results/`).
- `bench-pair.py LIMIT K AIOS OUT` + `bench-pair-report.py` - the fair
  comparison: each instance in its original order plus K shuffles, kissat on
  core 2 and aios on core 3 at the same time (same machine load), PAR-2.
  Single-order timings carry luck: kissat took 2.7 s on one ordering of an
  instance and 10-40+ s on shuffles of it.
- `bench.py` / `bench-report.py` - one core each, pinned, wall clock.
- `abcpu.py LIMIT OUT 'name=ENV=1@binary' ... -- files` - A/B of builds by the
  child's own CPU time (waiting on other processes does not count against it).
  Never read experiment switches with getenv in a hot path: it taxed every
  variant and skewed the first A/B.
- `satfuzz.py FROM TO` - small 2-SAT / gate / mixed formulas; every UNSAT proof
  through the independent checker, every model against the clauses. Caught the
  equivalence-pass proof bug (129 of 800 bad on the old build, 0 after).
- `proofcheck.sh BINARY files` - every UNSAT proof through drat-trim.
- `cnfshape.py`, `amo.py` - what an instance is (clause widths, XOR groups;
  at-most-one/at-least-one grids - how the colouring family was recognised).
- `screen.sh`, `knob.sh`, `satcheck.py`, `satcmp.py`, `satrun.sh` - screening
  and small sweeps.
- `results/` - the runs the commits quote (pair-v4.tsv: 70 instances x 3
  orderings, kissat PAR-2 5803 vs aios 9449, 0 disagreements).

## asql/ - ASQL and Things, on synthetic data

- `ready-drive.sh`, `ready-fuzz.py N` - ALLOCATE READY: planted traps, then
  random cases checked against brute force (optimal every time), FIFO lines
  identical to a previous build.
- `sqlsat-drive.sh` - CALL SAT, CALL SAT FROM, and a grain Thing calling sat().
- `alert-*`, `tgt-drive.sh`, `redo-clock*.sh` - alerts and the webhook outbox,
  WRITES ... TO a real MariaDB, the statement clock on writes and replay.

## grain/

- `grain-bench.cpp` - a standalone runner (no step limit) to build with -pg;
  `programs/maptest.gr` crosses the small-map/indexed-map line both ways and is
  a regression test for MapObj.

## pi/ and fleet/

- `pi_crumbs_listener.py` - the standing collector for the board's UDP
  breadcrumbs (/tmp/pi-crumbs.log); `pireset.sh`, `pishot.sh`, `piclick.sh`,
  `qmp.py`, `pi_soak.sh`, `udp42425.py` - driving the board and its QEMU twin.
- `hub-fd.sh`, `hub-wedge.sh` - the hub's descriptor leak and the stuck-member
  wedge, reproduced on a local hub (its key is a local test string).
