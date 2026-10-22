# ASQL drive — handoff (2026-10-21, session_01JuTR8eEdAqdgG1QozoJHhA)

Sean's brief: ASQL must be faster, lighter, leaner than MariaDB and return the
same results as his ERP's MariaDB for the ERP's real queries; durability,
compaction and consistency matter; commit as you go (as Sean, Co-Authored-By
Claude), never push; no test suites, drive it with probes.

## State

- Commits this session (local, not pushed): 0d0795f, 4fcd85a, a67ab60, 5b84371,
  107bfc3 (batches 5–16f), then the next commit = batches 16g–16k (typed numeric
  columns promoted as rows arrive, zone maps prune numeric text columns, auto-
  increment counter kept/INSERT IGNORE ids, union typing, SOUNDEX/REGEXP
  case-insensitive/CAST temporal/DOUBLE text/JSON_ARRAY true/unset @vars as
  DOUBLE/LEAVE labels, SUM/AVG static scale).
- OLTP mix (tput.py, 8 threads): 103 → 533 tx/s; MariaDB 540 on the same box.
  Single thread 56 → 479 tx/s.
- Known gaps: docs in memory `asql-open-gaps` (no MVCC/dirty reads, SEQUENCE,
  JSON_TABLE, CHECK OPTION enforcement, SHOW CREATE VIEW canonical form,
  ENUM `col = 2` through the per-code predicate path still misses, SUM(CASE)
  over all-ELSE rows prints 0 not 0.00 on the vectorised path).

## Real ERP data and queries (Sean's direction, do this first)

- `/repos/v8/server` = the ERP API ("apir"); `/repos/v8/client` = front end.
- ASQL runs on chi-il-1 (ssh alias) with root `/opt/aios/root-v8-full`
  (versioneight, 858 tables). Snapshot: `FLUSH TABLES` over
  `/opt/aios/run/asql.sock` (user erp, password `/opt/aios/mysql.pass`), then
  `tar czf /tmp/asql-v8-full.tgz -C /opt/aios root-v8-full` (~430 MB) and copy
  it here. Local copy: scratch `$S/asql-v8-full.tgz`, extracted to
  `$S/v8root/root-v8-full`; `$S/hk/v8.sh [binary]` starts an engine on it
  (port 3398, `--trust-loopback`; the capability/API key differs locally).
- Drive the ERP's own model code: `tools/erp-drive/erp-drive.php PORT nocache`
  with `DUMP=dir` to capture every statement; `same.php` / `cmpreport.php` /
  `race.php` compare against MariaDB (local `summitholding`, user bench@127.0.0.1,
  or chi-il-1's MariaDB versioneight).
- Scratch dir `$S=/tmp/claude-1000/-repos-aios-qbc/faada9b5-e6c0-4cb4-ac3c-3bcea3e95af8/scratchpad`;
  probe engine `$S/hk/eng.sh` (3396), `cmp.py` statement diff, `tput.py`,
  `crash2.py`, `conc.py`, batch patch scripts `$S/batch*.py`.
- Build: `cd $S/build && /repos/aios/scripts/build-slot.sh make -j2 aios-engine`
  (cpp-only ≈5 min, header change ≈15 min) — run in the background.

## Next

1. Extract the snapshot, start `v8.sh`, run `erp-drive.php 3398 nocache` with
   DUMP, and the same statements on MariaDB; fix every mismatch/error and the
   slowest statements. Keep the OLTP/durability probes green.
2. Then the gaps above, MVCC last (biggest).
