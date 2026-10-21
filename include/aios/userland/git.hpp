#pragma once
// Git as an OS facility, not a program the OS shells out to.
//
// This is the git object format implemented in the kernel's own userland: a
// content-addressed object database where every object is
//     "<type> <size>\0<payload>"
// named by the SHA-1 of those bytes and stored loose at
// .git/objects/ab/cdef... as a legal zlib stream. Real git reads what this
// writes, and this reads what real git writes. Nothing here calls /usr/bin/git,
// and every byte that touches storage goes through hal::Host, so the same
// implementation works on the bare-metal substrate.
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "aios/core/result.hpp"
#include "aios/core/bytes.hpp"
#include "aios/kernel/kernel.hpp"
#include "aios/kernel/surface.hpp"
#include "aios/userland/git_syntax.hpp"

namespace aios::userland {

// A raw object as it exists in the database, after the header is stripped.
struct GitObject {
  std::string oid;        // 40 hex characters
  std::string type;       // blob | tree | commit | tag
  ByteBuffer payload;
};

// A ref and what it points at.
struct GitRemoteRefPair { std::string name, oid; };

struct GitIndexEntry {
  std::string path;       // relative to the work tree, '/' separated
  std::string oid;
  uint32_t mode = 0100644;
  // The size git records for this path. For a symbolic link that is the length
  // of the target it names, which is NOT what stat() of the link reports, so
  // the entry carries it rather than the index writer guessing.
  uint64_t size = 0;
  // When the work-tree file was last hashed into this entry (0: unknown). With `size` it lets
  // `status` skip re-reading a file that has not been touched since, as git's stat cache does.
  uint64_t mtime_ns = 0;
  bool assume_valid = false;      // git update-index --assume-unchanged
  bool skip_worktree = false;     // git update-index --skip-worktree
  bool intent_to_add = false;     // git add -N: named, content not yet staged (the empty blob stands in)
  uint8_t stage = 0;              // 1 base, 2 ours, 3 theirs: an unmerged path (kept apart from index())
};

struct GitTreeEntry {
  std::string name;
  std::string oid;
  uint32_t mode = 0100644;
  bool is_dir() const { return mode == 040000; }
};

struct GitCommit {
  std::string oid;
  std::string tree;
  std::vector<std::string> parents;
  std::string author;      // "Name <email>"
  std::string committer;
  uint64_t author_time = 0;      // seconds since the epoch
  uint64_t committer_time = 0;
  std::string author_tz = "+0000";     // as written in the commit header
  std::string committer_tz = "+0000";
  std::string message;       // without its trailing newlines
  std::string raw_message;   // exactly as stored (what git's %B prints)
  // Headers after committer that git does not interpret itself (change-id, conflict-sides, encoding, gpgsig...),
  // in order, each value with its continuation lines joined by '\n'.
  std::vector<std::pair<std::string, std::string>> extra_headers;
  std::string header(const std::string& name) const {
    for (auto& [k, v] : extra_headers) if (k == name) return v;
    return {};
  }
};

// What `status` sees. `staged` entries differ between HEAD and the index;
// `worktree` entries differ between the index and the files on disk.
enum class GitChange : uint8_t { Added, Modified, Deleted, Untracked };
struct GitStatusEntry { std::string path; GitChange change; };
const char* git_change_name(GitChange c);

struct GitStatus {
  std::string branch;
  bool detached = false;
  bool has_commits = false;
  std::vector<GitStatusEntry> staged;
  std::vector<GitStatusEntry> worktree;
  std::vector<GitStatusEntry> untracked;
  std::vector<std::string> unmerged;   // paths with conflict stages in the index
};

// The zlib envelope git wraps every loose object in. Deflate here emits stored
// (uncompressed) blocks, which is a legal deflate stream; inflate understands
// stored, fixed-Huffman and dynamic-Huffman blocks, so objects written by real
// git read back fine.
// LZ77 + deflate's fixed Huffman code: what the object database and the
// packfiles this machine sends are actually compressed with.
// `level` is zlib's: 6 the usual balance, 8-9 smaller and slower (a repack), 1-3 quick.
ByteBuffer zlib_deflate(ByteSpan data, int level = 6);
// The same envelope with stored (uncompressed) blocks — legal deflate, kept
// because it is the simplest thing that can be checked by eye.
ByteBuffer zlib_deflate_stored(ByteSpan data);
Result<ByteBuffer> zlib_inflate(ByteSpan data);
// Inflate one stream starting at `offset` in a larger buffer; `end_offset`, when
// given, comes back pointing just past the stream's adler32 trailer. Packfiles
// need this: they concatenate zlib streams with no lengths in front of them.
// `expect` (when the caller knows the inflated size) sizes the result once: no doubling, no copy of a copy.
Result<ByteBuffer> zlib_inflate_at(ByteSpan whole, size_t offset, size_t* end_offset, size_t expect = 0);
// The deflate stream with no wrapper at all, which is what a gzip member holds.
Result<ByteBuffer> inflate_raw(ByteSpan whole, size_t offset, size_t* end_offset, size_t expect = 0);
// Apply a git delta (the payload of an OFS_DELTA / REF_DELTA pack entry) to
// its base object's bytes. One implementation, used both when a fetched pack
// is indexed and when an object is read out of a local pack.
Result<ByteBuffer> git_apply_delta(ByteSpan base, ByteSpan delta);
uint32_t adler32(ByteSpan data);

// What `fsck` found walking every loose object in the database.
struct GitFsckReport {
  uint64_t objects = 0, blobs = 0, trees = 0, commits = 0, other = 0;
  std::vector<std::string> broken;   // "<oid>: <what is wrong with it>"
};

// This machine's UTC offset at `when`, as git writes it in a commit ("+0200", "-0500").
std::string git_local_tz(uint64_t when);

// git's wildmatch with WM_PATHNAME ('*' and '?' stop at '/', "**/" spans directories).
bool git_wildmatch(std::string_view pattern, std::string_view text, bool icase = false);

// What git leaves out of `status` and `add`: core.excludesFile, .git/info/exclude and every .gitignore,
// in git's precedence (a deeper file over a shallower one, a later line over an earlier one).
class GitIgnore {
 public:
  void load(hal::Host& host, const std::string& work, const std::string& git_dir,
            const std::string& excludes_file = {}, bool icase = false);
  // Reads <rel_dir>/.gitignore the first time a walk reaches that directory ("" = the top).
  void enter(const std::string& rel_dir);
  // `rel` is '/'-separated from the top of the work tree.
  bool ignored(const std::string& rel, bool is_dir);

 private:
  struct Pattern { std::string base, glob; bool negate = false, dir_only = false, basename_only = false; };
  void add_patterns(const std::string& text, const std::string& base);
  int verdict(const std::string& rel, bool is_dir) const;   // 1 ignored, -1 re-included, 0 no opinion
  hal::Host* host_ = nullptr;
  std::string work_;
  bool icase_ = false;
  std::vector<Pattern> patterns_;
  std::set<std::string> loaded_;
};

class GitBlockFile;

class Git {
 public:
  // With an actor bound, every entry point is authorised through the kernel
  // first: reading the object database needs storage:read, writing to it needs
  // storage:write. An entity that was never granted those cannot reach a
  // repository at all, which is the point of it being an OS subsystem.
  explicit Git(Kernel& kernel, Surface* actor = nullptr);

  // Create a repository at `dir` (work tree), laying down .git/objects,
  // .git/refs/heads and a HEAD pointing at an unborn branch.
  Result<void> init(const std::string& dir, const std::string& branch = "main", bool bare = false);
  // Attach to an existing repository at `dir`.
  Result<void> open(const std::string& dir);

  // Hash a work-tree file (or every file under a directory) into the object
  // database and record it in the staging index. Returns the blob ids added.
  Result<std::vector<GitIndexEntry>> add(const std::string& path);

  // Write the staged tree and a commit pointing at it, then move the branch.
  Result<GitCommit> commit(const std::string& message);

  Result<std::vector<GitCommit>> log(size_t max_entries = 100) const;
  Result<GitStatus> status() const;

  // Read an object back by full id or unambiguous prefix (>= 4 hex chars). In a partial clone an object that is not
  // here is asked of the promisor remote (set_missing_object_hook) and read again.
  Result<GitObject> read_object(const std::string& id_or_prefix) const;
  // Whether the object is stored here (loose, packed, or borrowed), without reading it and without fetching it.
  bool has_object(const std::string& oid) const;
  // The shortest prefix of `oid` (at least `min_len` hex digits) that names no other object: git's
  // find_unique_abbrev, from the neighbours of the id in each pack's sorted index and the loose fan-out directory.
  int abbrev_len(const std::string& oid, int min_len) const;
  // One name in a tree, found by scanning its bytes (no entry list built): false when absent or unreadable.
  bool tree_lookup(const std::string& tree, std::string_view name, std::string& oid, uint32_t& mode) const;
  // The root tree the index holds, hashed without writing anything (an error when it names a malformed id).
  Result<std::string> index_tree_oid() const;
  // An object's type and size from its headers alone (git's oid_object_info): a packed entry's header, a delta's
  // result size, a loose object's "<type> <size>" - nothing inflated past them.
  Result<std::pair<std::string, uint64_t>> object_info(const std::string& oid) const;
  bool copy_loose_object_from(const Git& from, const std::string& oid);   // the loose file as is; false = not loose there
  // A partial clone (git clone --filter): `hook` fetches the objects named, returning whether it got them.
  void set_missing_object_hook(std::function<bool(const std::vector<std::string>&)> hook) { missing_hook_ = std::move(hook); }
  bool partial() const { return (bool)missing_hook_; }
  // Fetch, in one request, those of `oids` that are not here yet (a no-op unless this is a partial clone).
  void prefetch(const std::vector<std::string>& oids) const;
  // Object directories this repository borrows from (objects/info/alternates), absolute.
  const std::vector<std::string>& alternate_object_dirs() const;
  hal::Host& host() const { return kernel_.host(); }
  // This repository's own objects (not the ones it borrows through alternates): the ids of its loose ones, the ids
  // in its packs, and its packs' paths without the ".pack"/".idx".
  std::vector<std::string> local_loose_ids() const;
  std::vector<std::string> local_packed_ids() const;
  std::vector<std::string> local_packs() const;
  // A pack was added to (or removed from) objects/pack behind this instance's back: look again on the next read.
  void rescan_packs() const { packs_scanned_ = false; packs_.clear(); blocks_scanned_ = false; blocks_.clear(); }
  // Block storage (git_blocks.hpp): objects/blocks/*.blk, read after loose objects and packs.
  std::vector<std::string> local_block_files() const;
  std::vector<std::string> local_block_ids() const;
  // A commit's tree, parents and commit time without reading the commit, when a block file has it in its columns
  // (false otherwise: read_commit then).
  bool commit_meta(const std::string& oid, std::string& tree, std::vector<std::string>& parents, uint64_t& commit_time) const;
  bool has_blocks() const { load_blocks(); return !blocks_.empty(); }
  // The syntax outline (git_structural.hpp) of blob `oid` read as a file at `path`: from the block file that keeps
  // it, else parsed now and remembered for this instance. nullopt for a path no grammar reads, or a blob that does
  // not parse.
  std::optional<GitSyntaxOutline> outline(const std::string& oid, const std::string& path) const;
  struct OutlineCounts { uint64_t from_blocks = 0, parsed = 0, remembered = 0; };
  OutlineCounts outline_counts() const { return outline_counts_; }
  // An object as it sits in one of this repository's packs, for sending it on without inflating it: `data` is its
  // deflated bytes exactly as stored. A delta names its base by id (an offset delta's base is looked up), and
  // `size` is what the entry's header says (the object's size, or the delta's).
  struct PackedEntry {
    int type = 0;                // 1..4 a whole object; 7 a delta on base_oid
    uint64_t size = 0;
    std::string base_oid;        // deltas only
    ByteSpan data;               // the zlib stream, valid while this Git is alive and its packs are not rescanned
  };
  // NotFound when the object is loose (or absent): the caller reads it the ordinary way.
  Result<PackedEntry> packed_entry(const std::string& oid) const;
  // While a bulk write is open, loose objects are written without a flush each; the last one to close flushes the
  // object directory once. (Thousands of objects, one fsync each, is what made a fetch of a big pack take a minute.)
  void begin_bulk_write() const { ++bulk_depth_; }
  void end_bulk_write() const;
  // Borrow `objects_dir`'s objects too (what `git clone --shared` records).
  Result<void> add_alternate(const std::string& objects_dir);
  // Serialize "<type> <size>\0<payload>", name it, and write it loose.
  Result<std::string> write_object(const std::string& type, ByteSpan payload);

  // Speculation in memory. While a MemoryWrites is alive, objects this repository did not have are kept in memory
  // instead of being written to the object store, and every read here sees them: merges that mostly come to nothing
  // (a merge train's, a preview's) cost no disk. What turns out to matter is written for real by persist_memory (the
  // closure of a commit that is checked out or lands); drop_memory forgets the rest. A ref must never point at an
  // object only memory holds: persist first.
  class MemoryWrites {
   public:
    explicit MemoryWrites(Git& g) : g_(g), was_(g.memory_writes_) { g_.memory_writes_ = true; }
    ~MemoryWrites() { g_.memory_writes_ = was_; }
    MemoryWrites(const MemoryWrites&) = delete;
    MemoryWrites& operator=(const MemoryWrites&) = delete;
   private:
    Git& g_;
    bool was_;
  };
  // The opposite, inside one: what a fetch brings, or anything a ref will name, goes to the object store.
  class DiskWrites {
   public:
    explicit DiskWrites(Git& g) : g_(g), was_(g.memory_writes_) { g_.memory_writes_ = false; }
    ~DiskWrites() { g_.memory_writes_ = was_; }
    DiskWrites(const DiskWrites&) = delete;
    DiskWrites& operator=(const DiskWrites&) = delete;
   private:
    Git& g_;
    bool was_;
  };
  // Writes every object reachable from `root` that only memory holds; returns how many.
  Result<size_t> persist_memory(const std::string& root);
  // Forgets every object only memory holds, except those reachable from `keep`.
  void drop_memory(const std::vector<std::string>& keep = {});
  size_t memory_objects() const { return mem_.size(); }
  uint64_t memory_bytes() const { return mem_bytes_; }

  // Walk every loose object: inflate it, check its header, and re-derive its
  // SHA-1. A truncated or corrupt file is reported, never fatal.
  Result<GitFsckReport> fsck() const;

  // Lay a commit's tree into the work tree and make the index match it. This is
  // what finishes a clone: the object database has the history, and the files
  // the user asked for are the ones that commit says exist.
  Result<size_t> checkout(const std::string& commit_oid);
  // Point a ref at an object, or move HEAD to a ref. A fetch and a clone both
  // land here: nothing else in the kernel writes .git/refs.
  // Refs are written as git writes them: <ref>.lock created exclusively, the new value written into it, renamed over
  // the ref; a lock another writer holds is waited on (up to two seconds, then Conflict, as git reports it), so no
  // update is lost to another process (aios-git or git) updating the same ref.
  Result<void> update_ref(const std::string& ref, const std::string& oid);
  // Writes these refs into packed-refs (with what is there already), one file for all: what a clone does.
  Result<void> write_packed_refs(const std::vector<std::pair<std::string, std::string>>& refs);
  // The same, only if the ref is still `expected` ("" = must not exist): Conflict otherwise, nothing written.
  // Optimistic writers read, compute, then swap, and start again when someone moved the ref first.
  Result<void> compare_and_swap_ref(const std::string& ref, const std::string& expected, const std::string& oid);
  // Several refs at once: every lock taken (in name order), every expected value checked, then every ref renamed into
  // place; Conflict and nothing written when one does not match. ("HEAD" here is a detached HEAD.)
  struct RefSwap { std::string ref, expected, oid; };
  Result<void> compare_and_swap_refs(const std::vector<RefSwap>& swaps);
  Result<void> delete_ref(const std::string& ref);
  // "HEAD" or a safe "refs/..." name (no "..", no empty or dot-leading components, no control
  // characters): a ref name becomes a path under .git, and ref names arrive from remotes.
  static bool valid_ref_name(const std::string& ref);
  // Where this repository came from and where it pushes to, recorded in
  // .git/config the way git records it — so real git carries on from here, and
  // so `fetch` and `push` need no URL typed twice.
  Result<void> set_remote(const std::string& name, const std::string& url);
  Result<std::string> remote_url(const std::string& name = "origin") const;
  Result<void> set_head(const std::string& ref);
  // HEAD pointing straight at a commit, which is what a detached checkout is.
  Result<void> set_head_detached(const std::string& oid);

  // The branches this repository holds, and one more made at `at` (or at HEAD).
  Result<std::vector<GitRemoteRefPair>> branches() const;
  Result<void> create_branch(const std::string& name, const std::string& at = {});
  Result<void> delete_branch(const std::string& name);
  // Move to a branch, a tag or a commit: HEAD follows, and the work tree is
  // made to match — including removing what the target does not have. Refuses
  // while there is uncommitted work, unless `discard`.
  Result<size_t> switch_to(const std::string& rev, bool discard = false);

  Result<std::vector<GitTreeEntry>> read_tree(const std::string& oid) const;
  Result<GitCommit> read_commit(const std::string& oid) const;

  // Building history without a work tree, which is what a merge does. Trees are
  // written bottom-up from flat '/'-separated paths (an index's shape), and a
  // commit is written for any tree and parents; neither moves a ref.
  Result<std::string> write_tree_from_entries(const std::vector<GitIndexEntry>& entries);
  // Every file a tree holds, as flat paths under `prefix` ("" for the root).
  Result<void> flatten_tree(const std::string& oid, const std::string& prefix,
                            std::vector<GitIndexEntry>& out) const;
  // `author` and `committer` are "Name <email>"; empty means this repository's
  // author. `when` is seconds since the epoch; 0 means now.
  Result<std::string> commit_tree(const std::string& tree, const std::vector<std::string>& parents,
                                  const std::string& message, const std::string& author = {},
                                  const std::string& committer = {}, uint64_t when = 0);

  // "HEAD", a branch name, a full id or an unambiguous id prefix.
  Result<std::string> resolve(const std::string& rev) const;
  Result<std::string> ref_oid(const std::string& ref) const;   // loose ref, then .git/packed-refs
  // Every loose ref this repository holds, "refs/..." -> object id. A fetch
  // needs it to tell the remote what it already has.
  Result<std::vector<GitRemoteRefPair>> refs() const;
  Result<std::string> head_oid() const;          // NotFound on an unborn branch
  std::string head_ref() const { return head_ref_; }
  std::string branch() const;                    // "" when HEAD is detached
  const std::string& work_dir() const { return work_; }
  const std::string& git_dir() const { return git_; }
  const std::vector<GitIndexEntry>& index() const { ensure_index(); return index_; }
  // The index is read the first time something needs it, not at open: a
  // push, a log or a fetch never does, and reading 3,000 entries was a tenth
  // of a push.
  void ensure_index() const { if (!index_loaded_) const_cast<Git*>(this)->load_index(); }

  void set_author(const std::string& name, const std::string& email) { author_name_ = name; author_email_ = email; }

  // The core.* settings that change what the work tree means. Read from this repository's config by
  // open(); a caller that layers in global config and -c overrides sets them afterwards.
  struct Settings {
    bool filemode = true;           // core.filemode: false keeps the index's mode instead of the file's
    bool ignorecase = false;        // core.ignorecase
    bool symlinks = true;           // core.symlinks
    std::string excludes_file;      // core.excludesFile
    // core.fsync includes references (reference, committed, added, all): refs, HEAD and reflogs reach the medium
    // before the command ends. git's default leaves them to the page cache, as the kernel's other files.
    bool fsync_refs = false;
    bool fsync_index = false;       // core.fsync includes the index (index, added, all)
  };
  const Settings& settings() const { return settings_; }
  static bool fsync_covers_refs(const std::string& core_fsync);   // core.fsync's value: are references synced
  static bool fsync_covers_index(const std::string& core_fsync);  // ...is the index
  static bool fsync_covers(const std::string& core_fsync, bool index);
  void set_settings(const Settings& s) { settings_ = s; }
  // Sparse checkout (core.sparseCheckout, $GIT_DIR/info/sparse-checkout, cone or pattern mode): which paths the
  // work tree holds. A path outside is never written (write_worktree_file returns its entry marked skip_worktree)
  // and its index entry stands for it. in_sparse() is true for everything when the checkout is not sparse.
  bool sparse() const;
  bool in_sparse(const std::string& rel) const;
  void reload_sparse() const;
  // A file watcher's answer (git fsmonitor--daemon): the only paths that may differ from the index or be new since
  // the index was last checked; everything else is taken as unchanged without a stat. nullptr: look at everything.
  void set_watch_hint(std::shared_ptr<const std::set<std::string>> paths) { watch_hint_ = std::move(paths); }
  bool watch_says_unchanged(const std::string& rel) const;
  // Every untracked path git would list, or with `collapse` the way `status` lists them by default: a
  // directory holding no tracked file appears once, as "dir/". Ignored paths are left out.
  // with_nested: a nested repository appears as "dir/", as git's listings show it; off, it is left out
  Result<std::vector<std::string>> untracked(bool collapse, bool with_nested = false) const;
  // Whether .gitignore and friends exclude `rel`.
  bool is_ignored(const std::string& rel, bool is_dir) const;
  // True when the file at `e.path` still holds what `e` records (stat cache first, content when unsure).
  bool worktree_matches(const GitIndexEntry& e, std::string* new_oid = nullptr) const;
  // Hash the work-tree file at `rel` into the object database and return the index entry git would
  // record for it (mode kept from `existing` where the file system cannot say). Does not touch the index.
  Result<GitIndexEntry> stage_entry(const std::string& rel, const GitIndexEntry* existing);
  // Index → work tree, without the HEAD comparison: the paths whose file differs from what is staged.
  Result<std::vector<GitStatusEntry>> worktree_changes() const;
  // Write one blob into the work tree at `rel` (mode decides executable/symlink) and return its index entry.
  Result<GitIndexEntry> write_worktree_file(const std::string& rel, const std::string& oid, uint32_t mode);
  // Move the work tree and index from what they hold to `commit_oid`'s tree, touching only paths that
  // differ, and refusing (unless `force`) to overwrite local changes or untracked files in the way.
  Result<size_t> checkout_tree(const std::string& tree_oid, bool force);
  // The commit's tree for a commit, the tree itself for a tree.
  Result<std::string> tree_of(const std::string& oid) const;

  // Remove a path from the staging index, and optionally from the work tree.
  Result<void> rm(const std::string& path, bool cached = false);
  // Reset the index to a commit (or HEAD), without touching the work tree.
  Result<void> reset_index(const std::string& commit_oid = {});
  // Reset one path in the index to match a commit (or HEAD).
  Result<void> reset_path(const std::string& path, const std::string& commit_oid = {});

  // Git config (.git/config)
  Result<std::string> get_config(const std::string& key) const;
  Result<void> set_config(const std::string& key, const std::string& value);
  Result<std::vector<std::pair<std::string, std::string>>> list_config() const;

  // The index as a whole, for plumbing (read-tree, update-index) and merges. set_index takes stage-0
  // entries; set_unmerged the conflicted stages. Neither writes until write_index().
  void set_index(std::vector<GitIndexEntry> entries);
  void set_unmerged(std::vector<GitIndexEntry> entries);
  const std::vector<GitIndexEntry>& unmerged() const { ensure_index(); return unmerged_; }
  Result<void> write_index();
  // Use another index file (GIT_INDEX_FILE); takes effect on the next open().
  void set_index_file(const std::string& path) { index_file_ = path; }
  // Where objects, refs and config live: .git itself, or the main repository's .git for a linked work tree.
  const std::string& common_dir() const { return common_; }
  // A bare repository, or a work tree somewhere else (--git-dir / --work-tree).
  Result<void> open_dirs(const std::string& git_dir, const std::string& work_tree);
  bool bare() const { return bare_; }

  // Git notes
  Result<std::string> add_note(const std::string& notes_ref, const std::string& target,
                               const std::string& content, const std::string& author = {});
  Result<std::string> read_note(const std::string& notes_ref, const std::string& target) const;
  Result<std::vector<std::pair<std::string, std::string>>> list_notes(const std::string& notes_ref) const;

 private:
  Result<void> may(uint16_t rights) const;   // storage authority of the acting entity
  Result<GitObject> read_object_here(const std::string& id_or_prefix) const;
  std::function<bool(const std::vector<std::string>&)> missing_hook_;
  mutable bool in_missing_hook_ = false;
  Result<void> load_index();
  Result<void> save_index();
  Result<void> read_head();
  Result<std::string> write_tree_from_index() { ensure_index(); return write_tree_from_entries(index_); }
  Result<std::string> trees_from_entries(const std::vector<GitIndexEntry>& entries, bool write);
  Result<void> scan_worktree(const std::string& rel, std::vector<std::string>& out) const;
  struct TrackedView { std::unordered_set<std::string_view> files, dirs; };
  TrackedView tracked_view() const;
  // nested: a repository inside the work tree is listed as "dir/" (status, ls-files), not skipped
  Result<void> scan_worktree(const std::string& rel, std::vector<std::string>& out, const TrackedView& tv, bool with_tracked,
                             bool nested = false) const;
  std::string object_path(const std::string& oid) const;
  // Objects that live in .git/objects/pack/*.pack rather than loose -- which
  // is all of them after a `git gc`. Found through the pack's .idx.
  Result<GitObject> read_packed(const std::string& oid, int depth = 0) const;
  Result<GitObject> read_packed_raw(const uint8_t want[20], const std::string& oid, int depth) const;
  // Whether a pack (this repository's or a borrowed one's) holds `want`: its index only, nothing inflated.
  bool in_some_pack(const uint8_t want[20]) const;
  void load_pack_indexes() const;
  void packed_ids_with_prefix(const std::string& prefix, std::vector<std::string>& out) const;
  mutable std::unordered_map<std::string, std::vector<std::string>> loose_names_;   // objects/xx listings, for abbrev_len
  // packed-refs parsed (name -> id), kept while the file's size and mtime stay the same: every ref without a loose
  // file re-read and re-split the whole file (1,100 lines in redis) - five times per ref for %(refname:short).
  mutable std::unordered_map<std::string, std::string> packed_refs_;     // by name, only for an unsorted file
  mutable std::vector<std::pair<std::string, std::string>> packed_list_;   // the same refs in file order
  mutable bool packed_sorted_ = true;                                       // ...and whether that order is by name
  mutable uint64_t packed_refs_mtime_ = 0, packed_refs_size_ = 0;
  mutable bool packed_refs_loaded_ = false;
  const std::vector<std::pair<std::string, std::string>>& packed_refs() const;
  const std::string* packed_find(const std::string& ref) const;
  bool packed_commit_head(const std::string& oid, GitCommit& c) const;   // tree, parents, committer time: the head inflated
  // .git/config's text, kept while its size and mtime stay the same (every get_config read the file again).
  mutable std::string config_text_;
  mutable uint64_t config_mtime_ = 0, config_size_ = 0;
  mutable bool config_loaded_ = false;
  const std::string* config_text() const;

  struct PackFile {
    std::string pack_path;
    // The .idx (v1 or v2) and the .pack, mapped: a lookup touches the pages it reads, where reading the whole
    // pack (224 MB for redis) cost 0.3 s before the first object came out.
    std::shared_ptr<const hal::FileView> idx_view;
    const uint8_t* idx = nullptr;
    size_t idx_size = 0;
    uint32_t idx_version = 0;
    uint32_t count = 0;
    mutable std::shared_ptr<const hal::FileView> pack_view;   // mapped on first use
    const uint8_t* pack_data() const { return pack_view ? pack_view->data : nullptr; }
    size_t pack_size() const { return pack_view ? pack_view->size : 0; }
    // Every entry's offset, ascending, with its index in the .idx (so an entry's end and an offset delta's base are
    // found): built on first use by packed_entry.
    mutable std::vector<std::pair<uint64_t, uint32_t>> by_offset;
    // Delta bases inflated recently, by their offset (git's delta base cache): the versions of one file are a chain
    // of deltas, and each was re-inflated from the bottom for every version read.
    struct Base { int type; std::shared_ptr<const ByteBuffer> bytes; uint64_t used; };
    mutable std::unordered_map<uint64_t, Base> bases;
    mutable size_t bases_bytes = 0;
    mutable uint64_t bases_clock = 0;
  };
  // The object at `off` in pack `pf`: type (1-4) and bytes, through its delta chain and the base cache.
  Result<std::pair<int, std::shared_ptr<const ByteBuffer>>> unpack_at(const PackFile& pf, uint64_t off, int depth) const;
  // Entry index of `want` in pf's .idx, or -1.
  static int64_t pack_find(const PackFile& pf, const uint8_t want[20]);
  Result<void> map_pack(const PackFile& pf) const;
  mutable std::vector<PackFile> packs_;
  mutable std::vector<std::shared_ptr<GitBlockFile>> blocks_;
  mutable bool blocks_scanned_ = false;
  mutable std::map<std::string, std::optional<GitSyntaxOutline>> outlines_;   // "<language> <oid>"
  mutable OutlineCounts outline_counts_;
  void load_blocks() const;
  mutable bool packs_scanned_ = false;
  // objects/info/alternates: other repositories' object directories this one reads from (git clone --shared).
  mutable std::unordered_set<std::string> worktree_dirs_made_;   // directories made for this checkout (write_worktree_file)
  mutable int bulk_depth_ = 0;                                  // > 0: loose objects are written unsynced (see BulkWrite)
  bool memory_writes_ = false;
  std::unordered_map<std::string, std::pair<std::string, ByteBuffer>> mem_;   // oid -> (type, payload)
  uint64_t mem_bytes_ = 0;
  // The objects of mem_ that `oid`'s commit or tree names directly.
  std::vector<std::string> memory_children(const std::string& oid) const;
  Result<void> write_ref_locked(const std::string& ref, const std::string& oid, const std::string* expected);
  mutable std::unordered_set<std::string> fanout_made_;         // objects/xx directories this instance has made
  mutable std::vector<std::string> bulk_written_;               // loose files written unsynced in the current bulk write
  mutable std::vector<std::string> alt_dirs_;
  mutable bool alts_loaded_ = false;
  void load_alternates(const std::string& objects_dir, int depth) const;

  Kernel& kernel_;
  Surface* actor_ = nullptr;       // whose authority this subsystem is acting under
  std::string work_;               // work tree, as the caller named it
  std::string git_;                // work_ + "/.git"
  std::string head_ref_ = "refs/heads/main";
  std::string head_detached_;      // set when HEAD holds an oid directly
  std::vector<GitIndexEntry> index_;
  std::unordered_map<std::string, std::pair<long, std::string>> cache_tree_;   // git's TREE extension: dir -> (entries, tree)
  uint64_t cache_tree_fp_ = 0;                                                // index_fingerprint(index_) when read
  const std::unordered_map<std::string, std::pair<long, std::string>>* tree_cache_in_use_ = nullptr;
  static uint64_t index_fingerprint(const std::vector<GitIndexEntry>& entries);
  bool index_loaded_ = false;   // index_ reflects the index file (or there is none to reflect)
  std::vector<GitIndexEntry> unmerged_;   // stages 1-3
  uint64_t index_mtime_ns_ = 0;           // when the index file was last written: entries this new are "racy"
  std::string common_;                    // objects, refs, config (== git_ unless a linked work tree)
  std::string index_file_;                // GIT_INDEX_FILE, else git_/index
  bool bare_ = false;
  Settings settings_;
  mutable GitIgnore ignore_;
  mutable bool sparse_loaded_ = false, sparse_on_ = false, sparse_cone_ = true;
  mutable std::vector<std::string> sparse_recursive_, sparse_parents_;
  mutable std::vector<std::pair<std::string, bool>> sparse_patterns_;   // pattern, negated
  std::shared_ptr<const std::set<std::string>> watch_hint_;
  mutable bool ignore_loaded_ = false;
  void load_settings();
  std::string index_path() const { return index_file_.empty() ? git_ + "/index" : index_file_; }
  // Where a ref lives: HEAD and the pseudo-refs (FETCH_HEAD, MERGE_HEAD, ...) belong to the work tree's
  // own git dir, everything under refs/ to the common one.
  std::string ref_path(const std::string& ref) const;
  std::string author_name_ = "aios";
  std::string author_email_ = "kernel@aios";
};

// Bulk object writing for the length of a scope: every path out of it (an error included) flushes once.
class GitBulkWrite {
 public:
  explicit GitBulkWrite(const Git& g) : g_(g) { g_.begin_bulk_write(); }
  ~GitBulkWrite() { g_.end_bulk_write(); }
  GitBulkWrite(const GitBulkWrite&) = delete;
  GitBulkWrite& operator=(const GitBulkWrite&) = delete;
 private:
  const Git& g_;
};

// ---------------------------------------------------------------------------
// The subsystem's kernel identity
// ---------------------------------------------------------------------------
// A repository is not private to whoever typed the command: it is REGISTERED
// with the kernel. The registry lives in a kernel state object, so `checkpoint`
// captures it and a `restore` in a new process brings the machine's
// repositories back — which one is current, its branch and where its HEAD was.
// A durable copy under <root>/git/registry backs a machine that was never
// checkpointed. Every access goes through a Surface, so the kernel decides
// whether the acting entity may reach the object database at all.
struct GitRepoRecord {
  std::string name;
  std::string path;          // work tree
  std::string branch;
  std::string head;          // commit id; empty while the branch is unborn
  uint64_t registered_ns = 0;
  uint64_t commits = 0;      // commits this kernel has written into it
};

class GitRegistry {
 public:
  explicit GitRegistry(Surface& actor);

  // Adopt the registry this kernel already holds (including one restored from
  // a checkpoint image into another entity), or create it, seeded from the
  // durable copy the previous process left under the root.
  Result<void> attach();
  Result<void> record(const GitRepoRecord& r, bool make_current = true);
  Result<void> set_current(const std::string& path);
  Result<std::string> current();
  const std::vector<GitRepoRecord>& repos() const { return repos_; }
  const GitRepoRecord* find(const std::string& name_or_path) const;

  const StateID& state() const { return state_; }
  const EntityID& holder() const { return holder_; }
  // True when the state was already in the kernel when we attached: in a fresh
  // process that means it came back in a checkpoint image, not off the disk.
  bool adopted() const { return adopted_; }
  std::string registry_path() const;

 private:
  Result<void> commit_state();
  ByteBuffer encode() const;
  void decode(ByteSpan b);

  Surface& me_;
  StateID state_;
  EntityID holder_;
  bool adopted_ = false;
  std::vector<GitRepoRecord> repos_;
  std::string current_;
};

// A bare name is a repository OF THIS MACHINE and lands under the kernel root;
// anything carrying a path separator is a host path as typed.
std::string git_repo_path(Kernel& kernel, const std::string& name_or_path);
// One line for `info`: what the machine's git subsystem currently holds.
std::string git_registry_line(Kernel& kernel);

}  // namespace aios::userland
