#pragma once
// aios-sh: a POSIX shell and the utilities a shell script expects, implemented here rather than borrowed.
//
// The reason it exists is the same reason aios has its own git: a machine should not depend on a host's
// userland for something this basic. Concretely, converge fences agents on Windows at low integrity, where
// Git for Windows' bash cannot start (its MSYS runtime needs kernel objects only a medium-integrity process
// may create), so an agent there had no POSIX shell at all. aios-sh is native Win32 (and POSIX elsewhere),
// needs nothing from the host but its programs, and runs inside the fence.
//
// What it is: the POSIX shell language (quoting, parameter and command substitution, arithmetic, globbing,
// pipelines, lists, redirections, here-documents, if/for/while/until/case, functions, subshells, set -e/-u/-x
// and -o pipefail) and, built in, the utilities agents and test scripts use (cat, grep, sed, awk, find, ...).
// A command that is neither runs as a host program found on PATH. Paths are the host's; on Windows the
// MSYS spellings scripts carry (/c/Users/..., /tmp, /dev/null) are understood too.
//
// Layout: sh_os.cpp (the host: files, pipes, processes), sh_parse.cpp (text to a tree), sh_exec.cpp
// (expansion and execution), sh_utils*.cpp (the utilities), main.cpp.
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace aiossh {

// ---------------------------------------------------------------------------------------------------
// The host
// ---------------------------------------------------------------------------------------------------

// An OS stream: a HANDLE on Windows, a file descriptor elsewhere. -1 is none.
using Fd = intptr_t;
constexpr Fd kNoFd = -1;

namespace os {
Fd std_fd(int which);                 // 0, 1, 2: this process's own
bool make_pipe(Fd& read_end, Fd& write_end);
void close_fd(Fd f);
Fd dup_fd(Fd f);                      // a new, separately closable handle on the same stream
long read_fd(Fd f, char* buf, size_t n);          // bytes read, 0 at end, -1 on error
bool write_all(Fd f, const char* p, size_t n);
enum class Open { Read, Write, Append, ReadWrite };
Fd open_file(const std::string& path, Open how, std::string* err);
bool is_console(Fd f);

// Paths. All take host paths (see host_path()).
bool exists(const std::string& p);
bool is_dir(const std::string& p);
bool is_symlink(const std::string& p);   // the link itself, not what it points at
bool read_link(const std::string& p, std::string* target);   // what a symbolic link holds, as written
std::string resolved(const std::string& p);   // links and . .. followed: realpath
#ifndef _WIN32
// A symbolic link at `link` holding `target` as given, or (symbolic false) a hard link to `target`'s file.
bool make_link(const std::string& target, const std::string& link, bool symbolic, std::string* err);
#endif
bool is_file(const std::string& p);
bool is_executable(const std::string& p);
uint64_t file_size(const std::string& p);
int64_t mtime(const std::string& p);  // seconds since the epoch, 0 when unknown
struct DirEntry { std::string name; bool dir = false; bool link = false; };
bool list_dir(const std::string& p, std::vector<DirEntry>& out);
bool make_dir(const std::string& p, std::string* err);
bool remove_file(const std::string& p, std::string* err);
bool remove_dir(const std::string& p, std::string* err);
bool rename_path(const std::string& from, const std::string& to, std::string* err);
bool copy_file(const std::string& from, const std::string& to, std::string* err);
bool touch(const std::string& p, std::string* err);
std::string temp_dir();
std::string home_dir();
std::string current_dir();
std::string absolute(const std::string& cwd, const std::string& p);   // joined and normalised (no symlink resolution)
bool read_whole(const std::string& p, std::string& out, std::string* err);
bool write_whole(const std::string& p, const std::string& data, bool append, std::string* err);

// A host process. argv[0] is resolved by the caller (find_program); env is NAME=VALUE.
struct Proc { intptr_t handle = 0; int pid = 0; };
bool spawn(const std::vector<std::string>& argv, const std::string& cwd, const std::vector<std::string>& env,
           Fd in, Fd out, Fd err, Proc& p, std::string* why);
int wait(Proc& p);                     // exit status (128+signal for a signal on POSIX)
void kill_proc(Proc& p);
int self_pid();
std::string find_program(const std::string& name, const std::string& path_var, const std::string& cwd);
std::string self_exe();
void sleep_ms(long ms);
int64_t now_seconds();
std::string error_text(int code);       // the host's words for an error code
}  // namespace os

// A path as the host knows it: on Windows "/c/x" is "C:\x"-able, "/tmp" the temp dir, "/dev/null" NUL;
// relative paths are joined to cwd. Elsewhere only the join.
std::string host_path(const std::string& cwd, const std::string& p);
// Whether a path names the null device in any spelling.
bool is_null_device(const std::string& p);

// ---------------------------------------------------------------------------------------------------
// I/O for builtins and utilities: three streams, read and written as bytes
// ---------------------------------------------------------------------------------------------------

struct Io {
  Fd in = kNoFd, out = kNoFd, err = kNoFd;
  void write(const std::string& s) const;
  void ewrite(const std::string& s) const;
  // The whole of standard input (utilities that need it all, e.g. sort).
  std::string read_all() const;
};

// Closes a stream this code opened when it goes out of scope, a write that throws (the reader gone) included.
struct FdCloser {
  Fd f = kNoFd;
  FdCloser() = default;
  explicit FdCloser(Fd fd) : f(fd) {}
  FdCloser(const FdCloser&) = delete;
  FdCloser& operator=(const FdCloser&) = delete;
  ~FdCloser() { if (f != kNoFd) os::close_fd(f); }
};

// A line reader over a stream (for utilities that work line by line and must stop early, e.g. head).
class LineReader {
 public:
  explicit LineReader(Fd f) : fd_(f) {}
  // The next line without its "\n" (a "\r" before it is kept: the bytes are the file's). False at end.
  bool next(std::string& line, bool* had_newline = nullptr);
  // Whether next() can answer without reading (a whole line is buffered, or the end was seen): a utility flushes
  // what it has gathered before a read that may wait, so a slow producer's lines still come out as they arrive.
  bool ready() const { return eof_ || buf_.find('\n', pos_) != std::string::npos; }
 private:
  Fd fd_;
  std::string buf_;
  size_t pos_ = 0;
  bool eof_ = false;
};

// ---------------------------------------------------------------------------------------------------
// The shell's state
// ---------------------------------------------------------------------------------------------------

struct Node;
struct Shell;

struct Var { std::string value; bool exported = false; bool readonly = false; };

constexpr char kArrayMark = '\x02';
constexpr char kArraySep = '\x1f';

// What a running shell (or a subshell, or one stage of a pipeline) carries. Copied for a subshell.
struct State {
  std::map<std::string, Var> vars;
  std::vector<std::string> params;      // $1 ...
  std::string arg0 = "aios-sh";
  std::string cwd;                      // host path; the process's own directory is never changed
  int last_status = 0;
  int last_bg_pid = 0;
  bool opt_errexit = false, opt_nounset = false, opt_xtrace = false, opt_pipefail = false, opt_noglob = false;
  bool opt_noexec = false;   // -n: read and parse only (bash -n file is how scripts are syntax-checked)
  std::map<std::string, std::shared_ptr<Node>> functions;
  std::vector<std::map<std::string, Var>> locals;    // saved values of `local` variables, per function call
  std::map<std::string, std::map<long long, std::string>> arrays;   // bash indexed (sparse) arrays: a=(x y), ${a[1]}
  std::map<std::string, std::map<std::string, std::string>> assoc;   // declare -A m; m[key]=v

  std::string get(const std::string& name) const;
  bool has(const std::string& name) const;
  // A value starting with kArrayMark is an array literal's elements, each led by kArraySep: a=(x y).
  void set(const std::string& name, const std::string& value);
  // Element access: key is an index for an indexed array, a string for an associative one.
  bool get_element(const std::string& name, const std::string& key, std::string& out) const;
  void set_element(const std::string& name, const std::string& key, const std::string& value);
  void unset_element(const std::string& name, const std::string& key);
  std::vector<std::string> elements(const std::string& name) const;   // ${a[@]}; a scalar is one element
  std::vector<std::string> keys(const std::string& name) const;       // ${!a[@]}
  std::vector<std::string> env_list() const;          // exported NAME=VALUE, for a host program
};

// Control flow that unwinds the executor.
struct ExitSignal { int status; };
struct ReturnSignal { int status; };
struct LoopSignal { bool is_break; int levels; };

// A utility: argv[0] is its name. Returns the exit status.
using Utility = int (*)(Shell& sh, State& st, const std::vector<std::string>& argv, const Io& io);
const std::map<std::string, Utility>& utilities();   // sh_utils*.cpp
int run_utility_by_name(Shell& sh, State& st, const std::vector<std::string>& argv, const Io& io);
int run_external(Shell& sh, State& st, const std::vector<std::string>& argv, const Io& io,
                 const std::vector<std::pair<std::string, std::string>>& env_extra);   // sh_exec.cpp
// What `git` runs: the host's git when PATH has one, aios' own (aios-git, built in) where there is none.
// AIOS_SH_GIT=builtin or =host chooses; `aios-git` is always the built-in one. Empty: the built-in.
std::string host_git(const State& st);

// ---------------------------------------------------------------------------------------------------
// The tree
// ---------------------------------------------------------------------------------------------------

// A word as written: its parts keep their quoting, which decides splitting and globbing.
struct WordPart {
  // ArrayLit: the body of a=( ... ); ProcSub: <( ... ) (text), a path to its output
  enum Kind { Literal, SingleQuoted, DoubleQuoted, Param, CommandSub, Arith, Tilde, ArrayLit, ProcSub } kind = Literal;
  std::string text;                       // Literal/SingleQuoted text; Param name or ${...} body; sub/arith source
  std::vector<WordPart> inner;            // DoubleQuoted: its parts (Literal, Param, CommandSub, Arith)
  bool braced = false;                    // ${...}
};
struct Word { std::vector<WordPart> parts; };

struct Redirect {
  int fd = 1;                             // the stream it changes
  enum Kind { In, Out, Append, Clobber, DupIn, DupOut, HereDoc, ReadWrite, OutErr, AppendOutErr, HereString } kind = Out;
  Word target;                            // file, or for Dup the fd number or "-"
  std::string heredoc;                    // HereDoc: the body
  bool heredoc_expand = true;             // false when the delimiter was quoted
  bool strip_tabs = false;                // <<-
};

struct Node {
  // ArithCmd: (( expr )) in name; Cond: [[ ... ]] with its words in words and, per word, "op" in ops when it
  // was an operator token (&& || ( ) < > !) rather than a word
  enum Kind { Simple, Pipeline, AndOr, List, Subshell, Group, If, For, While, Until, Case, FuncDef, Not, ArithCmd, Cond } kind = Simple;
  // Simple
  std::vector<std::pair<std::string, Word>> assigns;
  std::vector<Word> words;
  std::vector<Redirect> redirects;       // also for compound commands
  // Pipeline / List / AndOr: children; ops for AndOr ("&&" / "||") and List ("&" marks background)
  std::vector<std::shared_ptr<Node>> kids;
  std::vector<std::string> ops;
  // If: kids = cond1, body1, cond2, body2, ..., [else]
  // For: name, items (has_in), body = kids[0]
  std::string name;
  std::vector<Word> items;
  bool has_in = false;
  // Case: subject = words[0]; arms: patterns + body
  std::vector<std::vector<Word>> case_patterns;
  int line = 0;
};

struct ParseError { std::string message; int line; };
// Parses a script. `incomplete` (when given) is set instead of throwing when the text ends mid-construct.
std::shared_ptr<Node> parse(const std::string& text, bool* incomplete = nullptr);
// One word from text (no splitting at blanks or operators): the word inside ${x:-word} and the like.
Word parse_word_text(const std::string& text);

// ---------------------------------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------------------------------

struct Shell {
  std::vector<std::string> temp_files;   // <( ) outputs, removed at exit
  std::mutex temp_mu;
  std::string self;                      // this executable, for running a utility as its own process
  std::mutex io_mu;                      // xtrace and error lines from concurrent pipeline stages
  int run(State& st, const Node& n, const Io& io);           // status
  int run_text(State& st, const std::string& text, const Io& io);
  // Expansion, also used by utilities that take shell words (none today) and by the tests.
  std::vector<std::string> expand_words(State& st, const std::vector<Word>& words, const Io& io);
  std::string expand_word_nosplit(State& st, const Word& w, const Io& io);
  std::string command_substitution(State& st, const std::string& text, const Io& io);
  long long arith(State& st, const std::string& expr, const Io& io);
  void error(const Io& io, const std::string& msg);
};

// Pattern matching for case, [[ ]]-free globbing and ${x#pat}: * ? [...] with \ escapes.
bool glob_match(const std::string& pattern, const std::string& s);
// Filename expansion of one word (already expanded, with glob characters that were quoted escaped by \).
std::vector<std::string> glob_expand(const std::string& cwd, const std::string& pattern);
bool has_glob_chars(const std::string& s);   // unescaped * ? [

// Utility helpers shared by the sh_utils files.
std::string join(const std::vector<std::string>& v, const std::string& sep);
std::vector<std::string> split_lines(const std::string& s);   // without "\n"; a last line without one is kept
bool parse_int(const std::string& s, long long& out);

}  // namespace aiossh
