# git-soak: the kernel's git against real git, on real histories, for hours

`tools/git-merge-check.cpp` is a CI pass over generated inputs with one fixed seed. This is the other
thing the from-scratch git needs before it is trusted: an unattended soak whose inputs are real merge
commits, real blob pairs and real packfiles from pinned mirrors of real repositories, under a fresh
logged seed every iteration, where **any disagreement with real git blocks the release**.

## What is compared

Every check puts the same question to the kernel's implementation and to `git`, and the answers must
be identical bytes.

| kind | AIOS | real git |
|---|---|---|
| `treemerge` | `git_merge_commits` (plain, `-X ours`, `-X theirs`; criss-cross pairs sought out) | `git merge-tree --write-tree`, plus `git fsck --strict` on every object AIOS wrote |
| `merge3` | `git_merge3` (plain, ours, theirs, union) on blobs a real merge changed on both sides | `git merge-file -p` |
| `diff` | `git_unified_hunks` at -U0/1/3/5/10 on blobs a real commit changed | `git diff <blob> <blob>` (git's defaults: Myers + indent heuristic) |
| `mergebase` | `git_merge_bases`, `git_is_ancestor` (merge parents, near and far pairs) | `git merge-base --all`, `--is-ancestor` |
| `objects` | `read_object`, `read_commit`, `flatten_tree` through the mirror's pack, a deep-delta repack and a REF_DELTA repack | `git cat-file`, `git ls-tree -r` |
| `renames` | `git_detect_renames` | `git diff-tree -M` |
| `packwrite` | `git_build_pack` (what a push sends) | `git index-pack --strict`, `git verify-pack -v` |
| `synth_*` | the old generators, now under fresh seeds | `git merge-file`, `git diff --no-index` |

Not covered yet: checkout onto an NTFS work tree (long paths, case-insensitive collisions), wire
protocol, `git_merge_commits` with directory renames (git is run with `merge.directoryRenames=false`
because AIOS documents that it does not infer them), packs AIOS *reads from the wire*.

## Running it

```
scripts/soak-fetch-corpus.ps1                 # once: mirrors + pins (commit the pins in corpus.tsv)
cmake --build build-win --config Release --target aios-git-soak
scripts/soak-run.ps1 -Hours 8                  # unattended; restarts on a crash, stops on a finding
scripts/soak-run.ps1 -Hours 1 -InjectFault 20  # prove a corrupted answer is caught (writes the gate's proof)
scripts/release-gate.ps1 -MinHours 8           # exit 0 only if the release may proceed
```

State lives under `C:\Dev\git-soak` (`corpus\` mirrors, `work\` scratch, `findings\`, `ledger\`, `logs\`).

## Findings, and what resolves one

A disagreement writes `findings\<utc>-<kind>-<seed>\` with `repro.txt` (repo, pin, seed, and the exact
operands: commit or blob ids), `aios.out`, `git.out`, `detail.txt` and `status` = `open`. The run
exits 1.

`aios-git-soak ... --replay findings\<id>` re-runs exactly those operands against the current build.
A pass marks the finding `resolved` (with the commit it was resolved at). A failure, or a skipped
check, leaves it open. There is no allowlist and no waiver: the only way to clear a finding is for the
two answers to agree. A hung operation (default 15 minutes) and a process that died mid-operation are
findings too.

## The release gate

`release-gate.ps1` refuses unless, at exactly this commit on a clean tree: no finding is open; the
ledger holds at least `-MinHours` of **unrestricted** soak (sessions run with `--skip-kind`/`--only`,
fault injection, or a dirty tree do not count); every kind and every corpus repository has at least
`-MinChecks` checks; and a `--inject-fault` run at this commit showed the gate catches a corrupted
answer. A missing git, a different git than the corpus was baselined on (`corpus\GIT_VERSION`), an
empty corpus, or a harness that cannot move bytes faithfully is exit 3/4: a failure, never a skip.

Soak hours are wall-clock hours of one worker (git runs as child processes, so process CPU alone
undercounts); the ledger records the AIOS-side CPU seconds beside it.

## Harness notes

* `hal::run_tool` on Windows strips every `\r` from a child's output. The soak spawns git through
  `hal::spawn_process` with an output file instead, and proves byte fidelity (CR, CRLF, NUL, Ctrl-Z)
  at startup before comparing anything.
* The mirrors are bare, one pack, no loose objects. Each working copy is a hardlinked clone, recycled
  every `--recycle-every` iterations so AIOS-written loose objects do not accumulate.
* Every `git` call pins the config that could change an answer: `core.autocrlf=false`, commit-graph
  off, rename limits raised, no system/global config, fixed locale.
