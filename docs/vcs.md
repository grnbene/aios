# aios-git: Extended Version Control for AIOS

`aios-git` (`tools/aios-git`, installed as `git`) is built on AIOS's native Git library (`src/userland/git*.cpp` and `include/aios/userland/git*.hpp`). It reads and writes standard Git repository formats, ensuring complete bidirectional interoperability with stock Git. When queried, it reports its version as `git version 2.45.0 (aios)` (`tools/aios-git/git_cli.cpp:51`).

On top of standard Git functionality, `aios-git` provides extended version-control capabilities:
- A change-based workflow layer with automatic working-copy commits (`@`), persistent change IDs, and automatic descendant rebasing
- Non-destructive operation logging and transactional rollback (`git undo` / `git redo`)
- Language-aware AST structural merging and scoping via Tree-sitter across 16 grammars
- Monorepo scale tooling: cross-platform filesystem monitoring (`inotify`, `FSEvents`, `ReadDirectoryChangesW`), file journaling, sparse checkouts, and partial clones
- Columnar block storage (`git blocks`) for high-density object packing and instant graph traversals
- Native stacked diff workflows (`git stack`)
- SSH commit signing (`SSHSIG`) and RFC 9162 Merkle log receipts
- Lockless, compare-and-swap (CAS) ref updates and speculative in-memory merges

These extensions do not alter standard Git repository behavior by default; each is enabled per repository or per command.

---

## The Change Layer: `git change init`

The change layer introduces a change-oriented workflow (inspired by Jujutsu / `jj`) where the working tree itself is treated as a commit: the change `@` (`refs/changes/wc/<workspace>`, where workspace defaults to `default`, a linked worktree name, or `$GIT_CHANGE_WORKSPACE`, with `HEAD` detached at `@`).

### Key Workflow Differences

- **No staging area or stashes**: Every command snapshots working-tree files into `@` before executing. `git add` triggers a snapshot, and `git stash` is rejected (the working copy is already a change; use `git new @-` and `git change edit`).
- **Committing**: `git commit -m <msg>` describes `@` and opens a new empty change on top. `git commit --amend` squashes `@` into `@-` (the change immediately below it).
- **Auto-tracking**: Newly created files are tracked automatically unless ignored by `.gitignore` or larger than `change.maxNewFileSize` (default 1 MiB; `change_core.cpp:151`). Files exceeding the limit can be tracked explicitly with `git change track <path>`.
- **Persistent Change IDs**: Every change maintains a stable `change-id` header (32 lowercase letters `k` through `z`, matching `jj` format) across rewrites (`change.hpp:30`). Standard Git commits without one receive a deterministic change ID derived from their commit ID (`derived_change_id()`), making every commit accessible as a change.
- **Revision syntax**: Change IDs and their unique prefixes can be used anywhere a revision is accepted, along with `@` (working copy), `@-` (parent), and `@--` (grandparent).
- **Persistent Visibility**: Changes remain visible through `refs/changes/heads/<change-id>` (`change.hpp:16, 81`).

### Command Reference

```bash
# Activation and status
git change init | enable                      # Turn on change layer in the current repository
git change disable [--branch NAME] | off      # Turn off change layer; optionally switch HEAD to NAME
git change status | st                        # Show working-copy status and current change
git change log                                # Display change history graph
git change show [REV]                         # Inspect a specific change (default: @)
git change diff [-r REV | --from A --to B]    # Diff changes or revisions
git change id [REV]                           # Print the 32-character change ID for REV (default: @)

# Creating and modifying changes
git new [-m MSG] [REV...]                     # Start a new change on REV (default: @); multiple REVs create a merge
git change describe [-r REV] -m MSG           # Update change commit message (alias: desc)
git change commit [-m MSG] [paths]            # Describe @ (or specific paths) and start a new empty @ (alias: ci)
git change edit REV                           # Set REV as the active working copy (@)
git change squash [-r REV] [--into DEST] [paths] # Squash changes into target revision (default: @ into @-)
git change split [-r REV] paths...            # Split a change by path into two changes
git change restore [--from A] [--to B] [paths]# Restore file contents across changes or revisions
git change abandon REV...                     # Abandon changes; descendants rebase onto their parents
git change rebase (-r | -s | -b) REV -d DEST  # Rebase revision (-r), subtree (-s), or branch (-b) onto DEST
git change track <path>...                    # Explicitly track files exceeding change.maxNewFileSize

# Conflicts, history, and bookmarks
git change resolve [-r REV] [--list | --ours | --theirs | --base] [paths] # Resolve recorded conflicts
git change evolog [-r REV]                    # View evolution history (previous versions) of a change (alias: obslog)
git change bookmark (list | set | create | move | delete) NAME [-r REV]   # Manage named bookmarks (alias: b)
git change snapshot [--auto]                  # Snapshot working files into @ (--auto used by fsmonitor daemon)
```

### Git Command Interceptions

Standard Git commands continue to operate normally within a repository using the change layer (`cmd_change.cpp:787-849`):
- `git add`: Snapshots current disk files into `@`.
- `git commit -m <msg>`: Translates to `git change commit -m <msg>`, describing `@` and opening a new `@`.
- `git commit --amend`: Translates to `git change squash`, moving `@` into `@-`.
- `git stash`: Refuses with an error explaining that `@` already holds edits safely.
- `git merge <rev>`: Creates a new change with multiple parents that always completes. Flags like `--abort`, `--continue`, and `--quit` are rejected because merges never halt halfway.
- If an external command moves `HEAD` (such as `git checkout`, `git switch`, or `git reset`), the subsequent snapshot automatically opens a new `@` on top of the destination commit.

### Automatic Descendant Rebasing

Modifying a change—whether through `describe`, `squash`, `split`, `edit`, `restore`, `resolve`, `abandon`, or `rebase`—automatically rebases all visible descendant changes in topological order and updates any branch pointers pointing to the rewritten versions (`change_core.cpp: Tx::finish()`).

- **Immutability protection**: Commits reachable by remote trunk branches (`refs/remotes/<remote>/HEAD`, else `main`, `master`, `trunk`), tags (`refs/tags/*`), or refs matching `change.immutable` are immutable (`change_core.cpp:392-426`). Rewriting an immutable commit requires `--ignore-immutable`.
- **Local branches**: Pushed local branches remain rewritable by default.

### First-Class Conflict Storage

In `aios-git`, merges and rebases always complete and produce a commit:

- **Conflict markers and headers**: Files with incompatible edits are written with standard conflict markers. The commit records the conflict state in a `conflict-sides <tree>` header containing base, ours, theirs, branch labels, conflict kind, and the materialized blob (`change.hpp:53-62`).
- **Propagation**: Conflicted changes appear marked with `x` in `git stack` and `(conflict)` in `git change log` and `git change status`. Downstream changes inherit the conflict until it is resolved.
- **Resolution**: Resolving a conflict is an ordinary subsequent edit. Either remove the markers manually (the next command automatically captures the resolution) or run `git change resolve --ours|--theirs|--base [paths]`. Descendants automatically rebase onto the resolved commit.
- **Clean re-merging**: If a conflicted change is rebased back onto one of its original parent branches, it resolves cleanly without conflict markers: the recorded original conflict sides are re-merged against the new parent rather than merging against the conflict markers (`change_core.cpp:110-145`).

---

## Clone, Push, and Fetch Optimizations

`aios-git` optimizes network and local repository transfer operations (`src/userland/git_remote.cpp` and `tools/aios-git/cmd_remote.cpp`):

- **Default branch discovery on clone**: When cloning an empty repository, `aios-git` requests `unborn` via `ls-refs`. Servers supporting this capability return the configured default branch (e.g. `main` from `git init -b main`). `aios-git` stands on that branch immediately, preventing initial pushes from going to `master`.
- **Empty remote handling**: Cloning or fetching an empty remote succeeds cleanly, matching modern Git rather than erroring with "no branches".
- **Bidirectional ancestry walks on push**: During `git push`, remote refs and local tips are traversed concurrently from newest to oldest. The remote ancestry is expanded only as far as necessary to establish convergence. Stock Git previously loaded the full ancestry ahead of time (walking all commits 32 branches deep for a 1-commit push).
- **Targeted tree inspection**: Only root trees of commits where new history diverges are inspected in full, rather than inspecting trees across all remote tips (e.g. 32 root trees of 3,000 entries each). If an older commit or another branch already holds an object, `aios-git` resends it safely.
- **Commit-only fast-forward checks**: Fast-forward determinations are made strictly by traversing commits without inspecting trees.
- **Direct file copy for local push**: Local pushes copy loose object files directly instead of decompressing and recompressing them. Directory and file writes are synced specifically rather than issuing global filesystem syncs that block on unrelated dirty pages. When writing more than a few dozen files, a single filesystem flush is used instead.
- **Deferred index loading and validation**: The index is loaded only when required by a command (commands like `push`, `log`, and `fetch` do not parse index files). Commits verify blob existence against index hashes directly without inflating the blobs.
- **Compression and pack sync**: Objects are decompressed via system zlib (`AIOS_HAVE_ZLIB`) when available, falling back to an internal decoder. Kept pack files are synced as single files rather than flushing the entire filesystem.

### Performance Benchmarks

Measured on a 3,000-file, 300-commit repository against stock Git 2.43 (same machine, best of three runs, times in ms):

| Operation | `aios-git` | Stock Git 2.43 | Implementation Details |
|---|---|---|---|
| `git clone` (local) | 247–274 ms | 349–400 ms | Object store directory-linked by name; working-tree files written directly in place without temp-and-rename. |
| `git status` | 30–36 ms | 49–66 ms | Deferred index parsing and optimized stat checks. |
| `git diff` (50 commits) | 17–19 ms | 59–66 ms | Cached tree diffing. |
| `git add` | 28–37 ms | 58–66 ms | Streamlined hashing and index updates. |
| `git commit` | 110–131 ms | 147–169 ms | Validates blob existence without inflating each blob. |
| `git push` (1 commit) | 14–20 ms | 41–55 ms | Targeted ancestry walk and fine-grained filesystem sync. |
| `git clone` (SSH) | 1.73 s | 2.00 s | Optimized pack stream handling. |
| `git push` (SSH) | 152 ms | 550 ms | Minimal negotiation round-trips and selective tree reads. |

All clone and push operations pass `git fsck --strict` cleanly on both sender and receiver.

---

## The Operation Log: `git undo`

`git op enable` (or enabling the change layer via `change.enabled = true`) records every repository-altering command along with the state it left: `HEAD`, all refs, and (outside the change layer) tree objects representing the working tree and index (`tools/aios-git/oplog.cpp`). Operations are recorded as commits under `refs/changes/op`. File edits between commands are captured as separate operations before the next command executes.

### Operation Commands

```bash
git op enable             # Enable operation logging (sets core.operationLog = true)
git op disable            # Disable operation logging (keeps past log intact)
git op log [-n N | --all] # List recent operations (default limit: 30)
git op show [OP]          # Inspect state changes and rewrites in operation OP (default: @)
git op restore OP         # Restore repository refs, index, and work tree directly to OP
git undo [OP]             # Invert operation OP onto current state (default: latest normal operation)
git redo                  # Re-apply previously undone operations
git op undo OP            # Explicitly undo a specific historical operation
```

### Non-Destructive Rollback Semantics

`git undo` applies the inverse of an operation onto the present repository state rather than performing a naive time rewind (`oplog.cpp: revert_op()`):
- A ref moved by the operation is restored to its prior target if it has not moved since.
- A change rewritten since the operation is three-way merged with its previous version.
- Modified files are merged back using three-way merge.

This allows safely undoing destructive actions (such as `git reset --hard`, accidental `git checkout -- file`, `git branch -D`, or `git change describe`) without losing subsequent unrelated work. Repeated invocations of `git undo` step backward through the operation log; `git redo` steps forward in reverse order. Any new repository operation terminates the redo chain.

---

## Language-Aware Structural Merge

When standard line-based three-way merge produces conflicts, `aios-git` performs language-aware AST merge using Tree-sitter (`src/userland/git_structural.cpp` and `third_party/tree-sitter`). Files exceeding 4 MiB (`kMaxBytes = 4u << 20`) retain standard line-merge results.

### Supported Languages and Extensions

| Language | Extensions (`git_structural_language()`) | Semantic Units Extracted |
|---|---|---|
| JavaScript | `.js`, `.mjs`, `.cjs`, `.jsx` | Functions, classes, methods, imports, exported declarations, object literal keys |
| TypeScript / TSX | `.ts`, `.mts`, `.cts`, `.tsx` | Interfaces, type aliases, enums, classes, functions, namespaces |
| Python | `.py`, `.pyi` | Functions, classes, methods, imports, top-level assignments |
| Go | `.go` | Functions, methods, type declarations, struct fields, imports |
| Rust | `.rs` | Functions, structs, enums, traits, `impl` blocks, `use` declarations |
| C / C++ | `.c`, `.h`, `.cpp`, `.cc`, `.cxx`, `.hpp`, `.hh`, `.hxx`, `.ipp` | Functions, classes, structs, templates, namespaces, `#include`s |
| Java | `.java` | Classes, interfaces, methods, constructors, fields, imports |
| C# | `.cs`, `.csx` | Namespaces, classes, structs, methods (keyed by parameter types), properties, fields |
| XML / XAML | `.xml`, `.axaml`, `.xaml`, `.csproj`, `.props`, `.targets`, `.resx`, `.svg` | Elements keyed by tag name and identifier attributes (`x:Name`, `Name`, `Key`, `Text`, `Content`, `Header`) |
| JSON | `.json` | Object keys, array members |
| PHP | `.php`, `.phtml` | Classes, interfaces, traits, methods, functions, array keys |
| SQL | `.sql` | `CREATE TABLE`, columns, indices, views, `INSERT`/`ALTER` sequences |
| YAML | `.yaml`, `.yml` | Mapping keys, list entries |
| TOML | `.toml` | Tables, inline tables, key-value pairs, arrays |

### Unit-Based Merge Strategy

Files are parsed into ordered lists of keyed semantic units:
- **Declarations**: Keyed by kind and identifier (functions, classes, types).
- **Imports**: Keyed by module name.
- **Key-value mappings**: Object keys in JavaScript/JSON, YAML mapping keys, TOML tables/keys.
- **Methods and properties**: PHP methods and array keys; C# namespaces, types, methods (keyed with parameter types), properties, and fields.
- **Database schemas**: SQL tables, columns, indices, and views.
- **Markup elements**: XML and XAML elements keyed by tag name and identifier attributes.

This enables clean automatic merges in common collaboration scenarios:
- Two functions added in the same file location.
- Methods independently added to the same class.
- Independent keys added to the same object or dictionary.
- Columns independently added to a single `CREATE TABLE` statement.
- Dependencies independently added to `[dependencies]`.

Where both sides modify the same semantic unit, its child members are merged recursively one level in. Unordered collections (YAML list items, TOML array values, PHP array entries, SQL `INSERT` and `ALTER` statements) are matched by text content, preserving new items added by either side.

### Safety Invariants

The structural merge engine never guesses. If a conflict cannot be resolved with certainty, it yields (`give_up()`) and falls back to standard conflict markers:
- Both sides rewrite unnamed statements in the same block, reorder units incompatibly, or edit a unit that the other side deleted.
- One side renames/replaces a unit while the other deletes it, or both sides introduce conflicting versions of the same dependency.
- An edit results in resurrected lines that were explicitly deleted by the other side.
- Token counts or balancing delimiters become inconsistent (e.g. duplicated or missing separators).
- The resulting merged file fails to parse.

### Fuzzing and Diagnostics

The merge invariants are verified by `aios-git-structural-fuzz [cases] [seed]` (`tools/git-structural-fuzz.cpp`), which performs randomized unit-level edits across supported languages and asserts that:
1. Merged output parses without syntax errors (`git_structural_parses()`).
2. No edits are fabricated (all lines originate from base, ours, or theirs).
3. Non-conflicting edits from both sides are preserved.
4. Merge operations are commutative: `merge(A, B) == merge(B, A)`.
5. Merging against base or identity is idempotent: `merge(Base, A) == A`.

Failures are written to `$AIOS_FUZZ_OUT` (default: `/tmp/aios-structural-fuzz/seed-<seed>`).

To diagnose a merge conflict manually:
```bash
aios-git-structural-fuzz --merge BASE OURS THEIRS
```
This executes the structural merge on the three files and prints the exact fallback reason from `git_structural_last_reason()`.

Structural merge is enabled in plain Git via `merge.structural = true` or `git merge -X structural` (or `-s structural`), and is enabled by default in the change layer (`change_core.cpp: opt.structural = c.config.get_bool("merge.structural", true)`). It is also used by converge for all automated merges.

---

## Code Outlines and Scopes: `git outline`, `git diff --scopes`

Semantic units are identified hierarchically as dot-separated scopes (e.g. `UserService.find`, `jobs.build.steps`, `dependencies.serde`):

```bash
git outline [<rev>:]<path>...              # List units in PATH with line ranges, kinds, and scopes
git outline --as <path> <file>             # Parse arbitrary FILE using the grammar for <path>
git outline --changes <old> <new> --as <p> # Scopes modified between files (exit 3 if new fails parse, 4 if old fails)
git diff --scopes [A [B]]                  # Diff revisions summarizing modifications by scope
```

In `git outline --changes` and `git diff --scopes` (`tools/aios-git/cli_diff.cpp:1693-1704`):
- `+`: Unit added
- `-`: Unit removed
- `~`: Unit modified internally
- `(file)`: Edits outside any recognized unit (top-level code, comments, or imports)

Converge uses these scopes to claim and enforce fine-grained intent leases. ASTs are updated incrementally using tree edits derived from line diff hunks (`ts_tree_edit`), and recent parse trees are cached so that diffing history incurs minimal parsing overhead. Pre-computed outlines are also stored directly in block storage (`git blocks`), enabling instant scope lookups without parsing raw blobs.

---

## Monorepo Scale

### Filesystem Monitor Daemon (`git fsmonitor--daemon`)

`git fsmonitor--daemon (start | run | stop | status)` (`tools/aios-git/fsmonitor.cpp`) tracks working-tree file modifications across major platforms:
- **Platform backends**:
  - Linux: `inotify`
  - macOS: `FSEvents` (`kFSEventStreamCreateFlagFileEvents`)
  - Windows: `ReadDirectoryChangesW`
- **Query caching**: When `core.fsmonitor = true` is enabled, commands query the daemon for paths changed since the last token, checking only modified paths, paths changed in the previous run, and index entries lacking stat information.
- **Automatic snapshots**: In the change layer, the daemon automatically snapshots changes into `@` after files remain quiet for `change.snapshotDelay` ms (default: 300 ms). Snapshots are recorded as discrete operations in the operation log without manual command execution (disabled via `change.autoSnapshot false`). Commands modifying the repository hold `$GIT_DIR/aios.lock`; if the background snapshot detects an active lock, it defers and retries once the command completes.
- **File Journal (`git journal`)**: Captures intermediate file versions across editor saves. Closed or renamed files are written to the object store after standing still for 25 ms, recorded in `$GIT_DIR/aios-journal`:
  ```bash
  git journal [-n N] [<path>...]  # List saved file versions (default limit: 50)
  git journal show <n>            # Output raw content of version <n>
  git journal restore <n> [--to P]# Restore version <n> to disk (optionally to path P)
  ```
  Saves occurring less than 25 ms apart are coalesced into the final save. Untracked ignored files and files exceeding `change.journalMaxSize` (default: 8 MiB) are excluded. Disable via `change.journal false`.

### Sparse Checkout and Partial Clones

- **Sparse checkout**: `git sparse-checkout (init | list | set | add | reapply | disable | check-rules)` (`tools/aios-git/cmd_sparse.cpp`) supports standard cone and pattern modes (`core.sparseCheckoutCone`). Files outside the cone remain marked as `skip-worktree` in the index and are never written to disk.
- **Partial clones**: `git clone --filter=blob:none|blob:limit=<n>` configures a promisor remote (`extensions.partialClone`). Filtering is supported in `upload-pack` (protocol v0 and v2). Missing blobs are fetched on demand in batched requests during checkout. When used with `--sparse`, only blobs within the active sparse cone are downloaded.

---

## Columnar Block Storage: `git blocks`

`git blocks build [-d] [--no-outlines]` (or `git repack --blocks [-d]`) packs repository objects into columnar block files (`objects/blocks/*.blk`):

- **Columnar commit data**: Commit tree OIDs, parent OIDs, and commit timestamps are stored in fixed-width binary columns, allowing revision walks and ancestor checks without object inflation.
- **Dictionary-encoded trees**: Tree paths and file modes are encoded against shared string dictionaries.
- **Chunked and delta-compressed blobs**: Blobs are deduplicated as content-defined chunks or delta-compressed against adjacent versions of the same file path.
- **Compressed frames**: Frames containing objects of the same kind and path are compressed together.
- **Embedded code outlines**: Pre-computed Tree-sitter semantic units, byte offsets, and digests are embedded directly in block files, allowing `git diff --scopes` and converge lease checks to operate without re-parsing files (`--no-outlines` disables outline generation).

### Verification and Compatibility

```bash
git blocks stats                # Display storage and compression statistics
git blocks verify               # Check integrity of all block objects
git blocks unpack               # Repack block files into standard Git packfiles (git repack -a -d)
```

Every object is read back and checksummed before a block file is committed to disk. With `-d`, older loose objects, packs, and replaced block files are deleted (`cmd_blocks.cpp:57-70`).

**Benchmark**: On a 100,000-commit repository:
- **Storage**: 37 MiB block file vs. 55 MiB standard Git pack.
- **Query performance**: `git rev-list --count` and `git merge-base` run 2.5–3× faster.

---

## Stacks: `git stack`

`aios-git` provides native stacked diff workflows (`tools/aios-git/cmd_stack.cpp`):

```bash
git stack [log | show | status] # Display changes between trunk and @ (top first)
git stack up | down [n]         # Move @ up or down the stack (aliases: next, prev)
git stack sync [--onto R | -d R]# Rebase the entire stack onto trunk (or onto revision R)
git stack push [remote]         # Push each change as a remote branch with leases
```

- **Trunk detection**: Configured via `change.trunk`, defaulting to `origin/main`, `origin/master`, `main`, or `master`.
- **Sync behavior**: `git stack sync` rebases the entire stack onto trunk or `--onto R`; conflicts are recorded in the commit headers and never halt the rebase. Note that it does not fetch the remote automatically—run `git fetch` beforehand if updating against remote changes.
- **Branch naming**: Pushing publishes each change to its associated bookmark or to `refs/heads/push/<change-id>`.
- **Leases**: Pushes include `--force-with-lease` checking the tracked remote ref to protect against overwriting remote changes.
- **Automatic rebasing**: Amending or editing any change in the stack automatically rebases all changes above it.
- **Programmatic access**: Stacks can be queried and traversed programmatically via the `stack` and `switch_change` interfaces.

---

## Cryptographic Commit Signing: `commit -S`, `git verify-commit`

`aios-git` provides native SSH commit signing compatible with Git's `gpg.format=ssh` standard (`include/aios/userland/git_sign.hpp`, `src/userland/git_sign.cpp`, and `tools/aios-git/cli_core.cpp:1410-1460`):

- **Format**: Produces standard SSHSIG signatures (OpenSSH `PROTOCOL.sshsig`, namespace `git`, SHA-512) stored in the commit's `gpgsig` header.
- **Commands**: `-S[<key>]`, `--gpg-sign`, `--no-gpg-sign`, and `commit.gpgsign` are supported across `commit`, `merge`, `cherry-pick`, `revert`, and `commit-tree` (`commit-tree` signs only when explicitly passed `-S`). The change layer automatically signs snapshots when `commit.gpgsign = true`.
- **Key handling**:
  - Setting `user.signingkey` to an Ed25519 private key path (or corresponding `.pub` file with the private key adjacent) generates signatures directly in-process via `aios::userland::sshsig_sign()` without invoking `ssh-keygen`.
  - SSH agent keys (`key::ssh-ed25519 ...`) or external signing binaries (`gpg.ssh.program`) are invoked using Git's standard signature interface: `<program> -Y sign -n git -f <key> [-U] <file>`.
- **Verification**: `git verify-commit [-v | --verbose] [--raw] <commit>...` (`tools/aios-git/cmd_sign.cpp`) checks signatures against `gpg.ssh.allowedSignersFile` and matches Git's standard output:
  ```text
  Good "git" signature for <principal> with ED25519 key SHA256:...
  ```
  Signatures by unlisted keys fail verification (`No principal matched.`).
- **Interoperability**: Signatures created by `aios-git` verify in stock Git, and vice versa.
- **Merkle log receipts**: Transaction receipts and tamper-evident audit logs are implemented in `include/aios/crypto/merkle_log.hpp` according to RFC 9162 (leaf and node hashing, root calculation, and inclusion/consistency proofs).

---

## Concurrency and Atomicity

### Compare-and-Swap Reference Updates

All ref writes follow Git's locking protocol: an exclusive `<ref>.lock` file is created, new values are written, and the lock is renamed over the target ref (`src/userland/git.cpp`). This prevents lost updates when running alongside stock Git processes (`update-ref <ref> <new> <old>` validates the old value under the lock).
- C++ API: `Git::compare_and_swap_ref(ref, expected, oid)` and multi-ref batch `Git::compare_and_swap_refs(swaps)`.
- **Operation log**: Appending to `refs/changes/op` uses compare-and-swap against the recorded log head.
- **Change layer race resolution**: Before mutating secondary refs, a command validates `@` via CAS against the value read at startup. If another process modified `@` in the interim, the losing command aborts cleanly without altering state (`WcMoved`: background snapshots re-read files; CLI commands report the conflict and exit).

### Lock-Free Snapshots

The fsmonitor daemon's background snapshot (`git change snapshot --auto`) does not acquire `$GIT_DIR/aios.lock`. Instead, it constructs the new `@` object tree and updates `HEAD`, `@`, and the change head simultaneously using compare-and-swap.
- If a foreground command or user action updates `@` first, the foreground command takes precedence and the auto-snapshot yields.
- If `HEAD` was shifted by an external Git command, or if `@` has descendant changes requiring rebases, the daemon falls back to acquiring `$GIT_DIR/aios.lock` without blocking.

---

## In-Memory Speculative Execution

`Git::MemoryWrites` (`include/aios/userland/git.hpp:251-283` and `src/userland/git.cpp:1001`) holds newly created Git objects in memory without immediately persisting them to disk:
- Subsequent read operations query memory buffers before accessing storage.
- `persist_memory(root)` writes the complete object closure of reachable objects to the object store and returns the count of persisted objects.
- `drop_memory(keep)` releases speculative objects from memory, preserving only those reachable from `keep`.
- `Git::DiskWrites` temporarily forces disk writes within a speculative block (e.g. for fetches).

Converge uses this mechanism for merge-train simulations and speculative preview builds: branch merges and candidate builds execute entirely in memory, writing to disk only when changes land or are explicitly checked out.
