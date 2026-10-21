#pragma once
// The command surface: a human debugging instrument over the kernel API. It is
// not part of the OS; a human-interface entity replaces it later.
#include <map>
#include <string>
#include <vector>
#include <atomic>
#include <functional>
#include "aios/kernel/kernel.hpp"
#include "aios/kernel/surface.hpp"
#include "out.hpp"

namespace aios::cli {

struct Session {
  std::unique_ptr<Kernel> kernel;
  // The entity this console IS. Every command acts through it; the kernel
  // decides from its capabilities. Bound by the substrate (bind_console), never
  // by a command. mind → console entity → capabilities → kernel.
  std::unique_ptr<Surface> me;
  std::map<std::string, std::string> vars;   // $name → id string or text
  std::string last;                          // last produced identity/path
  Out* out = nullptr;                        // where output goes (set by the substrate's main)
  // The hardware host's bootstrap tool, when the substrate provides one (disks,
  // install, reboot...). It receives the acting Surface: operating the machine
  // is kernel:execute, wiping the volume the kernel lives on is kernel:destroy,
  // and the substrate knows which is which.
  std::function<Result<void>(const std::vector<std::string>&, Out&, Surface&)> host;
  bool polled_services = false;             // substrate has no threads: services are pumped from the console loop
  bool quiet = false;
  // Show the machine's working — the mind's cycles, the commands it runs, the
  // dream loop's look — instead of only the outcome. Off for a person at the
  // desk: an operating system does not narrate. On with AIOS_TRACE=1, or
  // `ask show=1 …`, for whoever is debugging the mind.
  bool verbose = false;
  bool colour = false;
  uint64_t commands = 0, failures = 0;
  // Depth of the currently-running `ask` cognitive loop. A bare unknown command
  // typed by a human is a convenience for asking the mind; but while a mind is
  // ALREADY inside `ask` and running its own commands, an unknown one must be an
  // error it can see and correct — never a fresh nested ask, which recurses
  // without bound when the mind keeps emitting the same bad command.
  int ask_depth = 0;
  // A dream is being imagined right now. The imagine step is itself an `ask`,
  // and a mind briefed that "software is made with `dream`" answered the
  // request to write a face by running `dream` again, forever. While this is
  // set, `dream <what>` is refused with the reason.
  bool dreaming = false;
  // Someone has asked for the mind to stop. A person who has just watched it
  // set off down the wrong road needs a way to say so; without this the only
  // way out of a twelve-cycle ask was to kill the machine.
  //
  // Atomic because it is set from whichever thread noticed the key or the
  // click, and read by the one running the loop.
  std::atomic<bool> interrupt{false};
  void interrupt_mind() { interrupt.store(true, std::memory_order_relaxed); }
  bool interrupted() const { return interrupt.load(std::memory_order_relaxed); }
  void clear_interrupt() { interrupt.store(false, std::memory_order_relaxed); }
};

// Thrown by command handlers on error; caught in run_line. Shared across the
// command-surface translation units so the shell can throw it too.
struct Fail { Error e; };
[[noreturn]] inline void fail(Status st, const std::string& msg) { throw Fail{Error{st, msg}}; }

// The console's authority: large, explicit, and bounded — kernel access, not
// kernel ownership. Applied when the resident is first created at a root.
AuthorityProfile console_authority();
// Substrate glue: make (or wake) the persistent console resident and bind the
// session to its Surface.
Result<void> bind_console(Session& s, const std::string& label = "console");
Result<void> auto_bind_mind(Session& s, const std::string& label = "mind");
// True when the resident `label` has an intelligence bound (binding one the
// way `ask` would) that computes an answer: not the echo reference.
bool mind_can_answer(Session& s, const std::string& label = "mind");
Result<void> hot_swap_mind(Session& s, const std::string& label = "mind");

// The human → intelligence request: hand `request` to the resident mind named
// `who` (default "mind"); it acts through console commands as itself and answers.
Result<void> ask(Session& s, const std::string& request, const std::string& who = "mind", uint64_t max_cycles = 12, std::string* answer_out = nullptr, bool remember = true);

// Dreaming: what a person asks for becomes a face of this machine. The mind
// imagines it, the machine renders the dream to itself and corrects it, then
// realises it — an entity with its kinds, a surface on the glass, its records
// in the store — and keeps dreaming about it while idle.
Result<void> dream_command(Session& s, const std::vector<std::string>& args);
// Organs: primitives the mind writes in C++ and the machine builds and loads.
Result<void> organ_command(Session& s, const std::vector<std::string>& args);
// A face (userland/face.hpp) run the way the pane runs it: what it laid out,
// what it painted, what is wrong. The dream loop's look step; `face look`.
struct FaceLook {
  bool loaded = false;
  std::string url, path, name, title, source, message;
  std::vector<std::string> errors;     // lines the parser could not understand
  std::vector<std::string> findings;   // everything inspect() reported
  std::vector<std::string> lines;      // the face as text
  std::vector<std::pair<std::string, size_t>> reads;   // kind -> rows it came back with
  std::string picture;   // where the face was rendered to, as a PNG, if it painted
  int frames = 0, placed = 0, painted = 0;
  bool focus = false;
  bool ok() const;
};
// Keep what went wrong, as a record this machine can read back.
//
// Everything the machine learns about its own defects it used to print once and
// throw away: the lines a parser could not understand, the findings a look
// reported, a compiler's refusal. So nothing could ever ask "what keeps
// breaking" — and the answer to that question is the most valuable thing this
// machine knows about itself.
//
// The faults worth keeping are the ones that SURVIVED correction. The dream
// loop already re-asks the mind twice; a finding still standing after that is
// one the mind could not fix, which usually means the fault is the machine's
// and not the dream's. A fault that then recurs across dreams about completely
// different subjects is a defect in the language or the renderer.
//
// `where` is the part that noticed (face.look, face.parse, organ.compile),
// `what` is the finding in its own words, `about` is the dream or organ it
// happened to. Recorded as a `fault` kind in the machine's own store, so
// list(fault), unique(fault, what) and similar(text, k) all work on it from
// inside a face — the machine can dream its own defect list.
void record_fault(Session& s, const std::string& where, const std::string& what, const std::string& about);

FaceLook face_look(Session& s, const std::string& url, int frames, const std::vector<std::string>& drive);
// Every kind of record this machine actually holds, with how many, who keeps
// them and the FIELDS a face would see on one. The imagine step and the
// correction step both hand this to the mind: a dream that does not know what
// the machine keeps writes a face about records that do not exist, lays it out
// perfectly around nothing, and ships.
std::string store_kinds_text(Kernel& k);
std::string face_look_report(const FaceLook& L, size_t max_lines);
Result<void> face_command(Session& s, const std::vector<std::string>& args);
Result<void> organ_grow(Session& s, const std::string& name, const std::string& what, std::string* report = nullptr);

// Standing directives that have come due: run them, record what happened, and
// return how many there were. Called wherever this machine is idle — the metal
// console's wait for a key, the hosted shell between prompts, `attend` — so
// work that was ordered once happens without anything being typed again.
size_t run_due_directives(Session& s, bool announce = true);

// One supervision pass over the services THIS kernel started: reap what has
// died, restart what the unit's policy says to restart. Nothing called
// supervise() on a timer, so `restart=always` was inert — a service that fell
// over stayed down until somebody happened to run a daemon command, which on
// an unattended machine is never. Returns how many things changed, and says
// what they were on the error stream, because a restart nobody is told about
// looks exactly like a service that never failed.
size_t supervise_services(Session& s);

// Bring up every service registered to start with the machine. Called once by
// whichever process is going to be long-lived, because a service outlives only
// the manager that started it.
size_t start_boot_services(Session& s);

std::vector<std::string> tokenize(const std::string& line);
Result<void> run_line(Session& s, const std::string& line);
// Whether to show the working, not just the outcome (Session::verbose or AIOS_TRACE).
bool tracing(const Session& s);
Result<void> run_script(Session& s, const std::string& text, const std::string& name);
std::vector<float> embed_text(const std::string& text);
void print_help(Out& o);
std::vector<std::string> split(const std::string& s, char sep);
bool parse_u64(const std::string& s, uint64_t& out);
bool parse_f64(const std::string& s, double& out);

}  // namespace aios::cli
