#pragma once
// Shared machinery of aios-git's commands: the invocation context, option parsing, configuration,
// revisions, history walking, paths and diffs. Each command lives in a cmd_*.cpp file.
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include "aios/kernel/kernel.hpp"
#include "aios/userland/git.hpp"
#include "aios/userland/git_merge.hpp"
#include "aios/userland/git_remote.hpp"

namespace aios_git {

using aios::Kernel;
using aios::userland::Git;
using aios::userland::GitCommit;
using aios::userland::GitIndexEntry;
using aios::userland::GitTreeEntry;

// A command ends by throwing this; run() catches it and returns the code.
struct Exit { int code; };

// ---------------------------------------------------------------------------
// configuration: ~/.gitconfig (or GIT_CONFIG_GLOBAL), the repository's config, GIT_CONFIG_COUNT
// entries, then -c — later wins, as in git.
// ---------------------------------------------------------------------------
// scope: system | global | local | worktree | command; origin: "file:<path>" or "command line:" (--show-scope, --show-origin)
struct ConfigEntry { std::string key, value; bool has_value = true; std::string scope, origin; };
class Config {
 public:
  void clear() { entries_.clear(); }
  void add_text(const std::string& text, const std::string& scope = {}, const std::string& origin = {});
  void add(const std::string& key, const std::string& value, bool has_value = true, const std::string& scope = {},
           const std::string& origin = {});
  std::optional<std::string> get(const std::string& key) const;
  std::vector<std::string> get_all(const std::string& key) const;
  bool get_bool(const std::string& key, bool def) const;
  long long get_int(const std::string& key, long long def) const;
  const std::vector<ConfigEntry>& entries() const { return entries_; }
  // "Section.Sub.Name" -> "section.Sub.name": section and name fold case, a subsection does not.
  static std::string canon(const std::string& key);
  static bool parse_bool(const std::string& v, bool has_value, bool* ok = nullptr);
 private:
  std::vector<ConfigEntry> entries_;
};
// Edits one config file in place, keeping everything else in it as written.
enum class ConfigEdit { Set, Add, Unset, UnsetAll, ReplaceAll };
// Returns false when an Unset found nothing (git exits 5 then).
bool edit_config_file(aios::hal::Host& host, const std::string& path, const std::string& key,
                      const std::string& value, ConfigEdit how, std::string* error);
// Every key in a file under a section prefix ("remote.origin.") is removed (git remote remove).
void remove_config_section(aios::hal::Host& host, const std::string& path, const std::string& section_key);
void rename_config_section(aios::hal::Host& host, const std::string& path, const std::string& from, const std::string& to);

// ---------------------------------------------------------------------------
// options
// ---------------------------------------------------------------------------
enum class Arg { None, Required, Optional };
struct OptSpec {
  char short_name;          // 0 when none
  std::string long_name;    // "" when none; also the key it is stored under (else the short name)
  Arg arg = Arg::None;
  bool negatable = true;    // --no-<long> clears it
};
struct Parsed {
  std::map<std::string, std::vector<std::string>> values;   // every occurrence, in order
  std::set<std::string> negated;                              // --no-x seen last
  std::vector<std::string> pos;                               // positionals before "--"
  std::vector<std::string> after_dashdash;                    // after "--"
  bool dashdash = false;
  bool has(const std::string& k) const { return values.count(k) && !values.at(k).empty() && !negated.count(k); }
  std::string get(const std::string& k, const std::string& def = "") const;
  std::vector<std::string> all(const std::string& k) const { return values.count(k) ? values.at(k) : std::vector<std::string>{}; }
  int count(const std::string& k) const { return values.count(k) ? (int)values.at(k).size() : 0; }
};

struct Ctx;
// Options anywhere among the arguments (as git allows), combined short flags (-qm msg, -ffdx),
// --name=value and --name value. An option the spec does not list is an error, exit 129.
Parsed parse_options(Ctx& c, const std::vector<std::string>& args, size_t start, const std::vector<OptSpec>& spec,
                     bool stop_at_first_positional = false);

// ---------------------------------------------------------------------------
// the invocation
// ---------------------------------------------------------------------------
struct Ctx {
  Ctx(std::ostream& o, std::ostream& e, std::istream& i) : out(o), err(e), in(i) {}
  std::vector<std::string> args;    // args[0] is the subcommand
  std::string cwd;                  // absolute, '/' separated
  std::map<std::string, std::string> env;
  std::ostream& out;
  std::ostream& err;
  std::istream& in;
  std::unique_ptr<Kernel> kernel;
  std::vector<std::pair<std::string, std::string>> cli_config;   // -c name=value
  std::string opt_git_dir, opt_work_tree;
  std::string opt_git_dir_given;     // as typed, for messages
  Config config;
  std::unique_ptr<Git> repo;
  std::string top;        // the work tree's root, '/' separated ("" when bare)
  std::string prefix;     // cwd below top: "" or "sub/dir/"
  std::string global_config_path;
  // -S / --gpg-sign[=key] (1), --no-gpg-sign (0), or commit.gpgsign decides (-1); the key -S named.
  int sign = -1;
  std::string sign_key;
  // The output reaches a terminal (or the pager on one): git's "auto" decorations and colors apply. Set by Terminal.
  bool terminal = false;
  bool color = false;               // write git's colors (decided by the command's --color, color.* config and the terminal)
  // `diff --check` found something to complain about (the command then exits 2)
  bool diff_check_failed = false;
  // diffcore-rename's give-ups, for the warning git prints once the command's output is out: the rename limit the
  // matrix needed, and whether -C -C fell back to the changed files' sources
  bool hide_merges = false;   // whatchanged: merges are walked (and counted) but not printed
  int needed_rename_limit = 0;
  bool degraded_cc_to_c = false;

  std::string getenv(const std::string& k) const;
  bool hasenv(const std::string& k) const;   // set, even to ""
  aios::hal::Host& host();
  // Finds the repository from cwd (or --git-dir / GIT_DIR) and opens it; dies when there is none.
  void open_repo(bool need_work_tree = true);
  bool try_open_repo();
  void load_config();     // system, global (+ repository's when open), environment, -c
  // "file:<path>", the way git names a config file (a repository's own, at the top of its work tree, as ".git/config")
  std::string config_origin(const std::string& path) const;
  void apply_repo_settings();
  Git& git() { return *repo; }
  // "Name <email> <seconds> <tz>" for author or committer, from the environment then config.
  // strict: refuse without a name and email (commits, tags). Not strict: reflog lines, where git falls back to the
  // user's login and host instead of failing the command that moved the ref (checkout -b, branch, reset, merge --ff).
  std::string ident(bool committer, bool strict = true);
  // Short object name: the length git would choose for this repository, longer when ambiguous.
  std::string abbrev(const std::string& oid, int min_len = 0);
  int default_abbrev();
  int abbrev_cache = 0;
  // A path the user typed (relative to cwd) as a path relative to the top of the work tree.
  std::string to_repo_path(const std::string& user_path) const;
  // A repository path as shown to the user from cwd (git's relative display).
  std::string to_display_path(const std::string& repo_path) const;
  std::string abs_path(const std::string& p) const;     // absolute, '/' separated
  void write_git_file(const std::string& name, const std::string& content);   // .git/<name>
  std::optional<std::string> read_git_file(const std::string& name);
  void remove_git_file(const std::string& name);
};

[[noreturn]] void die(Ctx& c, const std::string& msg, int code = 128);       // "fatal: msg"
[[noreturn]] void fail(Ctx& c, const std::string& msg, int code = 1);        // "error: msg"
[[noreturn]] void usage(Ctx& c, const std::string& msg);                     // "error: msg", exit 129
[[noreturn]] void unsupported(Ctx& c, const std::string& what);              // not implemented: says so, exit 128

// ---------------------------------------------------------------------------
// paths
// ---------------------------------------------------------------------------
std::string slashes(std::string p);                   // '\' -> '/'
bool is_absolute(const std::string& p);
std::string normalize_path(const std::string& p);     // folds "." and ".." (absolute or relative)
std::string join(const std::string& a, const std::string& b);
std::string dirname_of(const std::string& p);
std::string basename_of(const std::string& p);
// git's C-style quoting of unusual paths ("a\tb", octal for bytes >= 0x80 with core.quotePath).
std::string quote_path(const std::string& p, bool quote_high = true, bool quote_space = false);

// Which paths a command applies to: exact, a directory and everything below, or a glob
// (where '*' crosses '/', as in a git pathspec). Empty matches everything.
// Magic as git has it: ":(glob)" ('*' stays within a folder, '**' crosses), ":(icase)", ":(literal)" (no
// wildcards), ":(top)" / ":/", and ":(exclude)" / ":!" / ":^", combinable as ":(exclude,icase)x" or ":!/x".
struct Pathspec {
  enum Magic : unsigned { kGlob = 1, kIcase = 2, kLiteral = 4 };
  std::vector<std::string> items;    // repository-relative; "" = the whole tree
  std::vector<std::string> raw;      // what the user wrote for each item (for "did not match" messages)
  std::vector<unsigned> magic;       // Magic bits for each item (missing = none)
  // ":!path", ":^path" and ":(exclude)path": what the others (or, with none, everything) must not include.
  std::vector<std::string> excludes;
  std::vector<unsigned> exclude_magic;
  bool icase = false;
  bool empty() const { return items.empty() && excludes.empty(); }
  bool match(const std::string& path) const;
  // Whether anything under directory `dir` could match (used to prune walks).
  bool may_match_below(const std::string& dir) const;
  int matched_index(const std::string& path) const;    // which item matched, -1 none
  // Whether item i has wildcards that are honoured (so naming a directory is not what it does).
  bool is_wild(size_t i) const;
  // The user's text for item i (falls back to the item when the pathspec was not built from arguments).
  const std::string& raw_of(size_t i) const { return i < raw.size() ? raw[i] : items[i]; }
};
Pathspec make_pathspec(Ctx& c, const std::vector<std::string>& user_paths);
// git's wildmatch: `pathname` keeps '*', '?' and '[...]' inside one folder and lets a "**" component cross
// folders (":(glob)", gc.<pattern>.reflogExpire uses it without).
bool wildmatch(const std::string& pattern, const std::string& text, bool icase, bool pathname);

// ---------------------------------------------------------------------------
// revisions
// ---------------------------------------------------------------------------
// The full ref name a short name means (HEAD, refs/heads/x, refs/remotes/x, ...), "" when none.
std::string dwim_ref(Ctx& c, const std::string& name);
std::optional<std::string> read_ref(Ctx& c, const std::string& full);    // loose, packed or pseudo (FETCH_HEAD)
// Any revision expression: abc123, main, origin/main, HEAD~2, v1^{commit}, HEAD:path, :path, @{u}...
std::optional<std::string> resolve(Ctx& c, const std::string& spec, std::string* why = nullptr);
std::string must_resolve(Ctx& c, const std::string& spec);                 // dies like git when it cannot
std::string peel_to(Ctx& c, const std::string& oid, const std::string& type);   // "" when it does not peel
std::string commit_of(Ctx& c, const std::string& spec);                    // dies unless a commit-ish
std::string type_of(Ctx& c, const std::string& oid);
GitCommit read_commit(Ctx& c, const std::string& oid);
std::string upstream_of(Ctx& c, const std::string& branch);                // "refs/remotes/origin/main" or ""
std::string current_branch(Ctx& c);                                        // "" when detached
std::vector<std::pair<std::string, std::string>> all_refs(Ctx& c);        // name, oid (sorted)

struct WalkOptions {
  std::vector<std::string> tips, hide;        // commit ids
  bool first_parent = false;
  int max_parents = -1, min_parents = 0;
  long long max_count = -1, skip = 0;
  bool reverse = false;
  bool topo = false;
  // the caller only needs each commit's id, parents, tree and time: from a block file's columns, nothing is read
  bool ids_only = false;
  bool simplify_merges_by_paths = true;
  Pathspec paths;
  std::vector<std::string> authors, greps;
  bool all_match = false, grep_icase = false, invert_grep = false;
  int64_t since = INT64_MIN, until = INT64_MAX;
  bool symmetric = false;                     // A...B: mark left/right
  std::set<std::string> left;                 // with symmetric: reachable from the left side
  std::vector<std::string> left_tips;         // the A of each A...B
  bool date_order = false, author_date_order = false;
  bool rewrite_parents = false;               // emitted commits carry the parents the simplified history shows
  bool want_children = false;
  bool cherry_pick = false, cherry_mark = false;
  std::map<std::string, std::vector<std::string>> children;   // filled with want_children, git's order
  std::set<std::string> patchsame;            // filled with cherry_mark: commits with a twin on the other side
  // Runs on a commit that passed every filter, after --skip and before --max-count counts it.
  std::function<bool(const GitCommit&)> post_filter;
  // Set by walk_each while it runs: whether a commit would be shown (what git's graph calls interesting).
  std::function<bool(const std::string&)> interesting;
  // When set, receives the commits the walk marked hidden (git's UNINTERESTING), once they are known.
  std::unordered_set<std::string>* hidden_out = nullptr;
};
// Consumes revision arguments (ranges, ^x, --not, --all, --branches...) into `w`. Returns the
// arguments that were not revisions. With `default_head` and no tips, HEAD is used.
std::vector<std::string> parse_revisions(Ctx& c, const std::vector<std::string>& args, WalkOptions& w,
                                         bool default_head, bool paths_after = true);
std::vector<GitCommit> walk(Ctx& c, WalkOptions& w);
// The same commits handed over one at a time, each as soon as its place in the output is known.
void walk_each(Ctx& c, WalkOptions& w, const std::function<void(const GitCommit&)>& emit);
bool is_ancestor(Ctx& c, const std::string& anc, const std::string& of);
std::vector<std::string> merge_bases(Ctx& c, const std::string& a, const std::string& b);

// ---------------------------------------------------------------------------
// trees, the index and the work tree as flat path -> entry maps
// ---------------------------------------------------------------------------
struct FileState { std::string oid; uint32_t mode = 0; bool present = false; };
using Snapshot = std::map<std::string, FileState>;
Snapshot snapshot_tree(Ctx& c, const std::string& tree_oid);      // "" = the empty tree
Snapshot snapshot_index(Ctx& c);
// The work tree for the paths the index holds plus untracked ones when asked. A file that differs from
// the index is hashed, and its content kept in `texts` (when given) so a diff can show it.
Snapshot snapshot_worktree(Ctx& c, bool with_untracked, std::map<std::string, std::string>* texts);
std::string blob_text(Ctx& c, const std::string& oid);
std::string hash_blob(const std::string& content);
std::string empty_tree_oid();
std::string write_tree_of(Ctx& c, const std::vector<GitIndexEntry>& entries);
std::string read_worktree_file(Ctx& c, const std::string& repo_path, bool* ok = nullptr);

// ---------------------------------------------------------------------------
// diff
// ---------------------------------------------------------------------------
struct DiffPair {
  char status = 'M';            // A D M R C T U
  std::string old_path, new_path;
  FileState a, b;
  int score = 0;                // similarity, for R and C
  // When the new side is the work tree, its content (not in the object database).
  std::optional<std::string> b_text;
  std::optional<std::string> a_text;   // likewise for the old side, once a diff is reversed (-R)
};
struct DiffOptions {
  enum Format { Patch, NameOnly, NameStatus, NumStat, Stat, ShortStat, Raw, Quiet, Summary, Scopes, DirStat, Check } format = Patch;
  std::vector<Format> extra;         // e.g. --stat plus --patch
  int context = 3;
  bool renames = true;
  int rename_threshold = 50;
  int rename_score = -1;   // the same on git's scale (0..60000) as -M/-C gave it; -1: rename_threshold * 600
  bool copies = false, copies_harder = false;   // -C, -C -C / --find-copies-harder
  long rename_limit = -1;            // -l<n>: the most files rename detection pairs up (diff.renameLimit by default)
  bool binary = false;               // --binary: full binary patches
  bool z = false;
  std::string filter;                // --diff-filter letters (uppercase include, lowercase exclude)
  bool full_index = false;
  bool text = false;                 // -a
  int stat_width = 0;                // --stat=<n>; 0: 80 columns less the line prefix (git's term width without a tty)
  int stat_prefix_bytes = 0;         // strlen of what each line is prefixed with (log --graph's padding, color codes and all)
  int stat_name_width = 0, stat_graph_width = 0, stat_count = 0;
  int abbrev = -1;                   // --abbrev=<n> (-1: the default length, 40: --no-abbrev)
  bool compact_summary = false;
  bool src_prefix_none = false;      // --no-prefix
  std::string src_prefix = "a/", dst_prefix = "b/", line_prefix;
  bool relative = false;
  std::string relative_to;           // --relative=<path>
  bool reverse = false;              // -R
  bool ignore_space_at_eol = false, ignore_space_change = false, ignore_all_space = false, ignore_cr_at_eol = false;
  bool ignore_blank_lines = false;
  std::vector<std::string> ignore_matching;   // -I<regex>
  bool function_context = false;     // -W
  int inter_hunk = 0;
  std::string algorithm;             // myers, minimal, patience, histogram
  enum Words { WordsNone, WordsPlain, WordsColor, WordsPorcelain } words = WordsNone;
  int color_moved = -1;              // --color-moved: 0 no, 1 plain, 2 blocks, 3 zebra, 4 dimmed-zebra; -1 diff.colorMoved
  std::string word_regex;
  std::string pickaxe_s, pickaxe_g;  // -S<string>, -G<regex>
  bool pickaxe_regex = false, pickaxe_all = false, has_pickaxe_s = false;
  bool irreversible_delete = false;  // -D
  std::string dirstat;               // --dirstat[=<params>]
  bool dirstat_cumulative = false;
  bool ita_visible = false;          // --ita-visible-in-index
  bool indent_heuristic = true;
  std::string submodule;             // --submodule[=short|log|diff]
};
std::vector<DiffPair> diff_snapshots(Ctx& c, const Snapshot& a, const Snapshot& b, const Pathspec& ps,
                                     const DiffOptions& o, const std::map<std::string, std::string>* b_texts = nullptr);
// Two trees diffed reading only the subtrees that differ (the whole trees with -C -C).
std::vector<DiffPair> diff_trees(Ctx& c, const std::string& from, const std::string& to, const Pathspec& ps, const DiffOptions& o);
void snapshot_tree_pair(Ctx& c, const std::string& from, const std::string& to, Snapshot& a, Snapshot& b);
// One file of a diffstat: what `--stat`, `--numstat` and `apply --stat` count.
struct DiffStatEntry {
  std::string name;                  // already quoted and, for a rename, in its "a => b" form
  long add = 0, del = 0;
  bool binary = false;
  uint64_t old_size = 0, new_size = 0;
  bool interesting = true;           // shown even with no line changed (a rename, a mode change...)
  char status = 'M';
  std::string comment;               // --compact-summary: "new", "gone", "mode +x"...
  bool unmerged = false;
};
// The diffstat of `stats` (or only its "N files changed" line with `shortstat`), laid out for o.stat_width and friends.
struct DiffPalette;
// pal: the colors of the +/- graph and a binary file's sizes (git's color.diff new/old), none when null
void print_diffstat(const std::vector<DiffStatEntry>& stats, const DiffOptions& o, bool shortstat, std::ostream& out,
                    const DiffPalette* pal = nullptr);
// Whether both a diffstat and a patch are asked for: log and show then put "---" between the message and the diff.
bool diff_stat_and_patch(const DiffOptions& o);
// Writes the pairs in the requested formats; returns whether there was any difference.
bool print_diff(Ctx& c, const std::vector<DiffPair>& pairs, const DiffOptions& o, std::ostream& out);
// Options common to diff, log, show: consumes them out of `args`, returns the rest.
std::vector<std::string> take_diff_options(Ctx& c, const std::vector<std::string>& args, DiffOptions& o, bool* any_format = nullptr);
std::string pair_text(Ctx& c, const DiffPair& p, bool new_side);
bool is_binary(const std::string& s);
// "dir/{old => new}.txt": how git names a rename in --stat and --numstat.
std::string rename_display(const std::string& from, const std::string& to);
// Lines added and removed between two texts (what --numstat counts).
std::pair<long, long> count_changes(const std::string& a, const std::string& b);
std::pair<long, long> count_changes(const std::string& a, const std::string& b, const aios::userland::GitDiffTextOptions& opt);
// The line-diff settings a set of diff options stands for (context, whitespace, algorithm, -W, -I).
aios::userland::GitDiffTextOptions diff_text_options(Ctx& c, const DiffOptions& o);

// ---------------------------------------------------------------------------
// misc
// ---------------------------------------------------------------------------
std::string format_date(int64_t t, const std::string& tz, const std::string& mode);   // default|iso|iso-strict|short|unix|raw|rfc|relative
bool parse_date(const std::string& s, int64_t& t, std::string& tz);   // git's accepted forms
// parse_date's forms plus the relative ones --since/--until take: "2.weeks.ago", "3 days ago", "yesterday", "now"
bool parse_approxidate(const std::string& s, int64_t& t);
// reflog / stash list --format: the format a --format=/--pretty= argument names ("" when it is not one), and one entry
// printed with it: %gd/%gD the selector ("stash@{0}"), %gs its message, every other placeholder from the commit.
std::string reflog_user_format(const std::string& arg);
std::string format_reflog_entry(Ctx& c, const std::string& fmt, const std::string& oid, const std::string& gd,
                                const std::string& gs);
int64_t now_seconds();
// git's hint when a repository is made without init.defaultBranch or -b.
void default_branch_hint(Ctx& c);
std::string cleanup_message(const std::string& msg, bool strip_comments);
std::string read_all(std::istream& in);
std::string subject_of(const std::string& message);

// ---------------------------------------------------------------------------
// ref listing: %(atom) formats, --sort keys, --contains/--merged/--points-at (ref_format.cpp)
// ---------------------------------------------------------------------------
struct RefRow { std::string ref, oid; bool detached = false; };   // detached: the HEAD row `branch` lists first
enum class RefQuote { None, Shell, Perl, Python, Tcl };
class RefFormat {
 public:
  struct Impl;
  // Parses `format` at once: an unknown or unsupported atom stops the command before any output.
  RefFormat(Ctx& c, const std::string& format, RefQuote quote = RefQuote::None);
  std::string expand(const RefRow& row);
 private:
  std::shared_ptr<Impl> impl_;
};
struct RefSelect { std::vector<std::string> contains, no_contains, merged, no_merged, points_at; };   // revisions as typed ("" = HEAD)
void select_refs(Ctx& c, std::vector<RefRow>& rows, const RefSelect& sel);
// Keys as --sort takes them ("-committerdate", "version:refname"); the last key is the primary one, refname breaks ties.
void sort_refs(Ctx& c, std::vector<RefRow>& rows, const std::vector<std::string>& keys, bool icase);
// %(refname:short). `known`: every ref name, when the caller has them (a clash is a name in it).
std::string unambiguous_ref_name(Ctx& c, const std::string& ref, const std::unordered_set<std::string>* known = nullptr);
std::string head_description(Ctx& c);
void detached_advice(Ctx& c, const std::string& name);   // "Note: switching to ..." (advice.detachedHead)                               // "(HEAD detached at abc1234)"
// Commits a tree with parents and a message as `git commit-tree` would; returns the id.
// `signable`: a commit the user makes (commit, merge, cherry-pick, revert, commit-tree), which is signed when -S or
// commit.gpgsign says so; stash, notes and the operation log never are, as in git.
std::string make_commit(Ctx& c, const std::string& tree, const std::vector<std::string>& parents,
                        const std::string& message, const std::string& author = {}, const std::string& committer = {},
                        bool signable = false);
// A commit body with its SSH signature added when signing is asked for (gpg.format=ssh, user.signingkey).
std::string sign_commit_body(Ctx& c, const std::string& body);
// -S[<key>] / --gpg-sign[=<key>] / --no-gpg-sign into c.sign and c.sign_key.
void take_sign_option(Ctx& c, const Parsed& p);
void update_ref_checked(Ctx& c, const std::string& ref, const std::string& new_oid, const std::optional<std::string>& old_oid,
                        const std::string& reflog_msg = {});
void append_reflog(Ctx& c, const std::string& ref, const std::string& old_oid, const std::string& new_oid, const std::string& msg);
// The "N files changed, X insertions(+), Y deletions(-)" summary and per-file stat for a commit.
void print_commit_summary(Ctx& c, const std::string& from_tree, const std::string& to_tree, std::ostream& out);
// "[branch abc1234] subject", the Author/Date lines git adds, and the change summary.
void print_new_commit(Ctx& c, const std::string& oid, bool show_date, bool root);
// Http headers for a URL from http.extraHeader / http.<url>.extraHeader.
std::vector<std::pair<std::string, std::string>> http_headers_for(Ctx& c, const std::string& url);
std::string remote_url(Ctx& c, const std::string& remote_or_url, bool* is_named = nullptr);

using Command = int (*)(Ctx&);
const std::map<std::string, Command>& commands();

// cmd_*.cpp
int cmd_init(Ctx&);
int cmd_status(Ctx&);
int cmd_add(Ctx&);
int cmd_rm(Ctx&);
int cmd_mv(Ctx&);
int cmd_commit(Ctx&);
int cmd_reset(Ctx&);
int cmd_restore(Ctx&);
int cmd_clean(Ctx&);
int cmd_log(Ctx&);
int cmd_show(Ctx&);
int cmd_whatchanged(Ctx&);
int cmd_diff(Ctx&);
int cmd_rev_list(Ctx&);
int cmd_rev_parse(Ctx&);
int cmd_branch(Ctx&);
int cmd_tag(Ctx&);
int cmd_checkout(Ctx&);
int cmd_switch(Ctx&);
int cmd_merge(Ctx&);
int cmd_merge_base(Ctx&);
int cmd_cherry_pick(Ctx&);
int cmd_revert(Ctx&);
int cmd_stash(Ctx&);
int cmd_config(Ctx&);
int cmd_cat_file(Ctx&);
int cmd_hash_object(Ctx&);
int cmd_mktree(Ctx&);
int cmd_write_tree(Ctx&);
int cmd_read_tree(Ctx&);
int cmd_checkout_index(Ctx&);
int cmd_update_index(Ctx&);
int cmd_commit_tree(Ctx&);
int cmd_update_ref(Ctx&);
int cmd_symbolic_ref(Ctx&);
int cmd_ls_files(Ctx&);
int cmd_ls_tree(Ctx&);
int cmd_notes(Ctx&);
int cmd_apply(Ctx&);
int cmd_archive(Ctx&);
int cmd_worktree(Ctx&);
int cmd_show_ref(Ctx&);
int cmd_for_each_ref(Ctx&);
int cmd_clone(Ctx&);
int cmd_fetch(Ctx&);
int cmd_pull(Ctx&);
int cmd_push(Ctx&);
int cmd_ls_remote(Ctx&);
int cmd_remote(Ctx&);
int cmd_bundle(Ctx&);
int cmd_http_backend(Ctx&);
int cmd_upload_pack(Ctx&);
int cmd_receive_pack(Ctx&);
int cmd_fsck(Ctx&);
int cmd_gc(Ctx&);
int cmd_reflog(Ctx&);
int cmd_repack(Ctx&);
int cmd_check_ignore(Ctx&);
int cmd_var(Ctx&);
int cmd_grep(Ctx&);
int cmd_change(Ctx&);
int cmd_new(Ctx&);
int cmd_blocks(Ctx&);
int cmd_outline(Ctx&);
int cmd_journal(Ctx&);
int cmd_verify_commit(Ctx&);
int cmd_blame(Ctx&);
int cmd_shortlog(Ctx&);
int cmd_count_objects(Ctx&);
int cmd_format_patch(Ctx&);
int cmd_submodule(Ctx&);
// A side of a diff pair as a syntax outline (from the block files, or parsed), nullopt when no grammar reads it.
std::optional<aios::userland::GitSyntaxOutline> pair_outline(Ctx& c, const DiffPair& p, bool new_side);
int cmd_stack(Ctx&);
int build_blocks(Ctx& c, bool remove_old, bool quiet, bool outlines = true);
int cmd_sparse_checkout(Ctx&);
int cmd_fsmonitor_daemon(Ctx&);
// core.fsmonitor: ask the watcher what changed and give the repository the hint (fsmonitor.cpp)
void fsmonitor_prepare(Ctx& c);
// The repository's command lock ($GIT_DIR/aios.lock): held by every command that may change a repository in the
// change layer or with the operation log on, for as long as it runs, so the fsmonitor daemon's background snapshots
// and two commands at once never rewrite @ or the log over each other. `wait` false: give up at once when another
// holds it (false returned). Programs the holder starts (hooks, merge drivers) inherit it (AIOS_GIT_LOCK_HELD).
bool take_repo_lock(Ctx& c, bool wait);
// Whether no command holds the repository lock now (taken and let go at once).
bool repo_lock_free(Ctx& c);
// extensions.partialClone: objects not here are fetched from the promisor remote when read (cmd_remote.cpp)
void install_promisor(Ctx& c);

}  // namespace aios_git
