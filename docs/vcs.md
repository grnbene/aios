# aios-git beyond git

aios-git (`tools/aios-git`, installed as `git`) is git on aios' own git library: it reads and writes git's formats,
and real git reads what it writes. On top of that it has the version-control features below. None of them change
what an ordinary repository does: each is turned on per repository (or per command), and the rest of the time
aios-git behaves as before.

## The change layer: `git change init`

The working tree becomes a commit, the change `@` (`refs/changes/wc/default`; HEAD is detached at it). Every
command snapshots the files into `@` first, so there is no staging area and nothing to stash: `git add` does
nothing more, `git stash` says why it is not needed, `git commit -m` describes `@` and starts a new empty one on
top, and `git commit --amend` moves what `@` holds into the change below it. New files are taken in unless
ignored or bigger than `change.maxNewFileSize` (1 MiB; `git change track <path>` takes one anyway).

A change keeps a `change-id` header (32 letters k..z, as jj writes them) through every rewrite. Commits without one
(written by git) get an id derived from their commit id, so every commit is a change. Change ids and unique prefixes
of them work wherever a revision is asked for, with `@`, `@-` and `@--` for the working copy and its parents.

    git change status | log | show [REV] | diff [-r REV | --from A --to B]
    git new [-m MSG] [REV...]            a new change on REV (default @); several: a merge
    git change describe [-r REV] -m MSG
    git change commit [-m MSG] [paths]   describe @ (or only those paths of it) and start a new @
    git change edit REV                  make REV the working copy
    git change squash [-r REV] [--into DEST] [paths]
    git change split [-r REV] paths...
    git change restore [--from A] [--to B] [paths]
    git change abandon REV...
    git change rebase (-r | -s | -b) REV -d DEST
    git change resolve [-r REV] [--list | --ours | --theirs | --base] [paths]
    git change evolog [-r REV]           the versions a change went through
    git change bookmark (list | set | create | move | delete) NAME [-r REV]

Plain git commands still work on a repository in the change layer. When one moves HEAD (a checkout, a merge,
a reset), the next snapshot starts a new `@` on top of where it went.

### Descendants follow every rewrite

Describing, squashing, splitting, editing, restoring, resolving, abandoning or rebasing a change rebases every
visible change built on it, in order, and moves the branches that pointed at the old versions. Commits that
trunk branches on a remote (its HEAD, else main, master or trunk) or tags reach are immutable; rewriting one needs
`--ignore-immutable`. Branches of one's own that were pushed stay rewritable.

### Conflicts are kept in commits

A merge or rebase always completes. A path both sides changed incompatibly is written with conflict markers, and
the commit records the conflict's sides in a `conflict-sides <tree>` header (base, ours, theirs, the labels and the
blob it wrote). The change shows as conflicted in `git change log` and `status`, and everything built on it carries
the conflict until it is resolved. Resolving is an ordinary later edit: remove the markers (the next command records
it), or `git change resolve --ours|--theirs|--base`. Descendants are rebased onto the resolution. A conflicted
change rebased back onto one of the sides it came from comes out clean: the recorded sides are merged again with
the new parent, not with the markers.

## Clone, push, fetch against stock git

A clone of an empty repository stands on the branch the remote's HEAD names
(`main` on a repository made with `git init -b main`): ls-refs is asked for
`unborn`, which a server that offers it answers with that branch. It used to
land on `master`, and the first push went to the wrong branch. A clone or
fetch of an empty remote succeeds, as stock git's does, instead of failing on
"no branches".

A push works out what the remote lacks alongside the walk from our tips:
the remote's refs are read (their commits), and the two walks advance
newest-commit-first together, the remote side expanded only as far as our
walk needs to know where to stop - its whole ancestry was read ahead of
time before (every commit of the history, 32 branches deep, for a push of
one commit). Only the trees of the commits our new history grows from are
read whole (every remote tip's tree was, 32 root trees of 3,000 entries);
an object only an older commit or another branch holds is sent again, which
the remote takes without harm. A fast-forward is decided by a walk of
commits alone. A local push copies a loose object's file as it is instead
of inflating and compressing it again, and syncs the files and directories
it wrote rather than the whole filesystem (which waited on every other
program's dirty pages: 28 ms for three objects, 200 beside a database
checkpoint); past a few dozen files one flush of the filesystem is the
cheaper. The index is read the first time a command needs it, not at open
(a push, a log or a fetch never does; 3,000 entries were a tenth of a push),
and a commit checks that the blobs its index names exist rather than
inflating each of them (40% of a commit on a 3,000-file tree).

On a 3,000-file, 300-commit repository (same box, stock git 2.43, times in
ms, best of three): clone 247-274 against 349-400 (the object store is
linked by name, directory by directory, instead of a recursive walk that
canonicalised a path per object; working-tree files are written in place,
no temp file and rename each); status 30-36 against 49-66; diff of 50
commits 17-19 against 59-66; add 28-37 against 58-66; commit 110-131
against 147-169; push of one commit 14-20 against 41-55; over ssh, clone
1.73 s against 2.0 and push 152 ms against 550. `git fsck --strict` on the
receiving side and on the clone is clean after every run.

Objects are inflated through the system zlib when the build has one
(`AIOS_HAVE_ZLIB`), the engine's own decoder otherwise; a fetched pack that
is kept is synced as one file, not with a sync of the whole filesystem.

## The operation log: `git undo`

`git op enable` (or the change layer) records every command that changes the repository, with the state it left:
HEAD, every ref, and outside the change layer the work tree and the index as trees. Operations are commits on
`refs/changes/op`. Edits made between commands are recorded on their own before the next command runs.

    git op log [-n N]        git op show [OP]        git op restore OP
    git undo [OP]            git redo                git op undo OP

Undo reverts one operation onto the present instead of rewinding to before it: a ref the operation moved goes back
if nothing has moved it since, a change rewritten since is merged with its earlier version, and the files it
changed are merged back three ways, so a `reset --hard` that threw away edits, a `checkout -- file`, a `branch -D`
and a `describe` can each be undone without losing what happened after. Repeated undo walks further back; redo
takes the undos back, newest first; any new operation ends the redo chain.

## Language-aware merge

When the line merge of a file both sides changed conflicts, the three versions are parsed with tree-sitter
(`third_party/tree-sitter`: JavaScript, TypeScript, TSX, Python, Go, Rust, C, C++, Java, C#, XML, JSON, PHP, SQL, YAML, TOML) and
merged as ordered lists of keyed units: a declaration by its kind and name, an import by its module, an object key by
its key, a PHP method or array key, a SQL table, column, index or view, a YAML mapping key, a TOML table or key, a C# namespace, type, method (by parameters), property or field, an XML or XAML element (by its tag and x:Name, Name, Key, Text, Content, Header... or, for a container with none of those, its start tag). Two
functions added at one place, a method each added to one class, two keys added to the end of one object, an import
each, a column each added to one `CREATE TABLE`, a dependency each added to `[dependencies]`: all merge. Where both
sides changed the same unit, its members are merged one level in. A collection's items (a YAML list's entries, a
TOML array's values, a PHP array's values, a SQL script's `INSERT`s and `ALTER`s) are known by their text: each
side's new ones are kept.

The merge gives up rather than guess, so it only turns conflicts into clean merges:

- both sides rewriting unnamed statements at one level, conflicting reorders, an edit against a deletion;
- a rename (a unit taken out and a new one put in its place) against a deletion of that unit, and an item both sides
  took out while either put something in (two new versions of one dependency);
- a result in which a line one side deleted and the other left alone comes back (text moved into a new unit), or in
  which a token's count is not what the sides made it (a separator doubled or lost on the way);
- a result that does not parse.

`aios-git-structural-fuzz [cases] [seed]` holds it to that: random unit-level edits of a sample of every language,
two at a time, and every merge it makes checked (it parses, nothing is made up, each side's changes are kept, the
sides swapped give the same, a side merged with the base or itself gives that side). `--merge BASE OURS THEIRS` runs
one merge and says why it gave up. On with `merge.structural` or `git merge -X structural`, and in the change layer
by default. converge's engine uses it for every merge it makes.

### Outlines: `git outline`, `git diff --scopes`

The same units, as scopes: names from the top down, joined by `.` (`UserService.find`, `jobs.build.steps`,
`dependencies.serde`). `git outline [REV:]PATH` lists a file's units with their lines; `git diff --scopes [A [B]]`
says which units each file's change touched: `+` added, `-` removed, `~` changed in itself, `(file)` for text
outside every unit. converge's intent leases claim scopes.

Each version of a file is parsed incrementally from the tree of the one before it (one tree edit per hunk of their
line diff), and recent trees are kept, so a merge's three versions and the outlines of a history cost little more
than one parse. Block files keep every source blob's outline (see below), so reading one costs no parse at all.

## Monorepo scale

- `git fsmonitor--daemon start|run|stop|status` (Linux, inotify) keeps every change under the work tree numbered.
  With `core.fsmonitor` on, each command asks what changed since its last token and checks only those paths, the
  ones it found different last time, and index entries without stat information.
  In the change layer the daemon also snapshots: once the files have been quiet for `change.snapshotDelay` ms (300)
  after an edit, `@` takes in what changed, as an operation of its own in the log, with no command run
  (`change.autoSnapshot false` turns it off). Commands that may change the repository hold its lock
  (`$GIT_DIR/aios.lock`) while they run; a background snapshot that finds it held leaves the work to that command and
  looks again after it.
  It also keeps every saved version of every file, not only the settled ones: a file closed after writing (or renamed
  into place) is put in the object store once it has stood still for 25 ms, and `git journal` lists those versions,
  `git journal show N` prints one and `git journal restore N [--to PATH]` puts it back. inotify reports that a file
  was saved, not what it held, so saves less than 25 ms apart are kept as the last of them. Ignored untracked files
  and files over `change.journalMaxSize` (8 MiB) are left out; `change.journal false` turns it off.
- `git sparse-checkout init|set|add|list|reapply|disable|check-rules` (cone or pattern mode, git's file format) and
  `git clone --sparse`: paths outside stay in the index as skip-worktree and are never written.
- `git clone --filter=blob:none|blob:limit=<n>` makes a partial clone (promisor remote, `extensions.partialClone`).
  The server side filters in upload-pack (v0 and v2). A blob read that is not here is fetched from the promisor
  remote then, and a checkout fetches all it needs in one request. With `--sparse`, only the cone's blobs come down.

## Block storage: `git blocks`

`git blocks build -d` (or `git repack --blocks -d`) puts every object into one block file (`objects/blocks/*.blk`),
laid out by kind and by column: commits' tree, parents and time as fixed-width columns read without inflating
anything; trees encoded against dictionaries of names and modes; blobs as content-defined chunks stored once, or as
deltas against the newer version of the same path; frames of one kind and one path compressed together; and every
source file's outline (its units, places and digests), so `git diff --scopes` and converge's lease checks read
which units a commit touched instead of parsing (`--no-outlines` leaves them out). Every object is read back and
its id checked before the file is kept. `git blocks stats|verify`, and `git blocks unpack` (`git repack -a -d`) to
go back to a pack real git reads. On a 100,000-commit history: 37 MiB against git's 55 MiB pack, and `rev-list
--count` / `merge-base` 2.5-3x faster.

## Stacks: `git stack`

    git stack [log]          the changes between trunk and @, top first
    git stack up | down [n]  move @ along the stack
    git stack sync           rebase the whole stack onto trunk (fetch first)
    git stack push [remote]  push every change as a branch (its bookmark, else push/<change id>), with leases

Trunk is `change.trunk`, else origin/main, origin/master, main or master. Amending any change in the stack rebases
the ones above it (see above). converge has stacks for its agents too: the `stack` and `switch_change` tools.

## Signed commits: `commit -S`, `git verify-commit`

aios-git signs commits as git does with `gpg.format=ssh`: an SSHSIG signature (namespace `git`, SHA-512) over the
commit without its `gpgsig` header, carried in that header. `-S[<key>]`, `--gpg-sign`, `--no-gpg-sign` and
`commit.gpgsign` work on `commit`, `merge`, `cherry-pick`, `revert` and `commit-tree` (which signs only with `-S`), and
the change layer's commits are signed under `commit.gpgsign` too. With `user.signingkey` naming an Ed25519 private key
(or its `.pub`, the private half beside it) the signature is made here, with no `ssh-keygen`; a key given as a literal
(`key::ssh-ed25519 ...`, held by an ssh-agent) or a `gpg.ssh.program` other than ssh-keygen is used the way git uses
it: `<program> -Y sign -n git -f <key> [-U] <file>`. `git verify-commit` checks against `gpg.ssh.allowedSignersFile`
and prints what git prints (`Good "git" signature for <principal> with ED25519 key SHA256:...`; a good signature by an
unlisted key is still a failure). Signatures made here verify with stock git, and stock git's verify here. The library
side is `aios/userland/git_sign.hpp`; converge checks agents' pushes with it.

The Merkle log converge keeps over its receipts is `aios/crypto/merkle_log.hpp` (RFC 9162: leaf and node hashing,
roots, inclusion and consistency proofs and their verification).

## Refs by compare-and-swap; a snapshot without the lock

Every ref is written as git writes it: `<ref>.lock` created exclusively, the new value written into it, renamed over
the ref, so aios-git and stock git running side by side never lose an update (`update-ref <ref> <new> <old>` checks
the old value under that lock). `Git::compare_and_swap_ref` and `compare_and_swap_refs` (several refs, all or none)
build on it. The operation log is appended the same way: an operation is written on the head it read and the log's ref
swapped from it, again on top when another process came first. The change layer swaps `@` from the version a
command started with, before moving any other ref, so a command that loses the race has changed nothing (a snapshot
reads the files again on top of the new `@`; any other command says so and stops).

The fsmonitor daemon's snapshot (`git change snapshot --auto`) takes no lock: it builds the new `@` from the files as
objects and swaps `HEAD`, `@` and its change head together from the values it read. A command running at the time, or
one that moved `@` first, wins, and its own snapshot takes the files. When `HEAD` was moved by a plain git command, or
`@` has descendants to rebase, it falls back to the lock (without waiting).

## Speculation in memory

`Git::MemoryWrites` keeps new objects in memory while it lives; reads see them, `persist_memory(root)` writes the
closure of a commit to the store and `drop_memory(keep)` forgets the rest. converge's merge trains and previews merge
this way: only what is checked out for a run, or lands, reaches the disk.
