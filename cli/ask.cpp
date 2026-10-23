// `ask` — the human → intelligence request. The one command that makes the
// machine feel like a computer rather than a kernel demo. It is an APPLICATION
// on the command surface: the kernel knows nothing of it. The console hands the
// request to the resident mind by writing it into that entity's context state,
// runs the cognitive cycle, and reads the mind's reply. The mind acts through
// the same console commands a human types, AS ITSELF — with its own authority,
// so the kernel decides what it may do exactly as it would for anyone — and
// each command's output goes back into its context for the next cycle, until
// it answers. What the mind did is shown to the human as it happens.
#include "cli.hpp"
#include <cstdlib>
#include <algorithm>

namespace aios::cli {

namespace {

template <typename T> T need(Result<T> r) { if (!r.ok()) throw Fail{r.error()}; return std::move(*r); }

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// The protocol the console defines for its resident mind. Plain words, no JSON,
// no tool schema: the mind types console commands like anyone else.
std::string briefing(const std::string& request, const EntityID& self, const std::string& label) {
  return
    "You are the resident mind of this AOS machine. You are entity " + self.str() + " (" + label + ").\n"
    "A person at the console asked you:\n\n    " + request + "\n\n"
    "You act on the machine by typing console commands, exactly as a person would, running AS YOU with your authority.\n"
    "Reply with lines of the form:\n"
    "    run: <one console command>\n"
    "(up to three per reply); the output of each appears in your context next cycle. When done, reply:\n"
    "    answer: <your answer to the person: one or two plain sentences. Answer like a terse system tool — no preamble, no restating the question, no account of what you did, no offers of more>\n"
    // What the machine IS comes before what it can do: a mind told only about
    // applications goes looking for host utilities that do not exist here.
    "To see the machine: `info` (identity, topology, fabric), `sysstat` (the same with resources and health), `devices` (compute), `storage` (volumes, and which holds the root), `network` (interfaces), `free` (memory), `entity list` (who is here), `authority mine` (what you hold), `store list` (what is remembered).\n"
    "To act on it: `daemon` (userland services), `install [status|dualboot|disk <dev>]` (install bare-metal OS or configure dual boot), `reboot` (restart the computer / reload kernel), `poweroff` (shut down machine), `display show <anything>` (what is on the display: the four scenes it was written with by name, and ANYTHING ELSE gets COMPOSED into a new scene from a sky, a ground, a sun and solids — so \"a forest at night\" or \"three red pyramids at sunset\" work, and `display describe` prints the composition, `display desk` brings the desk back), `kv` (key-value store), `mind status` (which engine you actually are), `mind set <provider> [model]` (change the engine this machine thinks with), `help` (every OS command), `man` (shell commands).\n"
    "VISUAL SCENES & DISPLAY: When a person asks to show, draw, render, paint, or put a visual scene on the display (e.g. 'ocean at dusk', 'sunset over water', 'a pyramid in the desert', 'the matrix lattice', 'the plasma core'), you MUST run `run: display show <what they asked for>` so the display/GPU compositor actually changes on the glass!\n"
    // The machine's own predictor was in the image and in the command table
    // but named nowhere a mind could see, not even in `help` — so "do you use
    // JEPA?" got answered from what the model knew about JEPA in general.
    "Your own world model: this machine carries a JEPA substrate world model — a 1024-d latent state of its own telemetry and a 128-d action vector bound to real actuators. `jepa status` is what it is doing right now, and says plainly whether telemetry is LIVE or SYNTHETIC and whether actuation is bound; `jepa record [count=<N>]` measures real transitions, `jepa train [steps=<N>]` trains on them, `jepa tick` runs one perceive → policy → predict → publish cycle, and `jepa policy` / `jepa predict` show the action it would take and the state it expects. `host jepa prove <prog> <n>` is the only thing that proves the action changed this machine. Answer questions about your own prediction, planning or telemetry from these, not from what you know about JEPA in general.\n"
    "To perceive the world — when a person asks you to look at what is happening, scan, or find breakthroughs/shifts — run `horizon distil` (add `k=<n>` for n items): the machine reads its watched front pages, you choose which articles to open from their real links, the machine reads those, and you write the brief anchored to this machine's identity (`identity` shows it); the brief is kept as the machine's horizon. Then give that brief as your answer. `horizon watch <name> <url>` adds a source first if the person names one.\n"
    "To look at the world & your display: `sense <url>` reads a web page; `sense frame` (or `sense desktop`) looks at your own desktop display & hardware compositor framebuffer with your own eyes; `browser <url>` runs it through this machine's own web engine -- the tokeniser, the cascade and the box layout are all in this kernel -- and prints the laid-out page with its links numbered, so `browser follow <n>` goes to one and `browser back` comes back. `horizon show` is what the world did that touches this machine's work; `horizon distil \"<what it means, items separated by ' | '>\"` keeps YOUR brief as the machine's horizon entry (bare `horizon distil` re-reads the watched sources instead); `store put <as> <text> kind=<kind>` keeps a thought.\n"
    "These are the machine's own commands. Answer from them rather than from host utilities: `host <cmd>` reaches the substrate underneath and is not where this machine's resources are described.\n"
    // What used to be missing: the way a request for SOFTWARE is answered. A
    // mind told only how to inspect the machine answered "I cannot build that".
    "To make what a person asks for — an app, a tool, a tracker, a scanner, a ledger, an ERP, a game, a dashboard, ANY software — run `dream <what they asked for, in their words>`. The machine imagines it with you, renders it to itself, corrects it, realises it as an entity with its own kinds of records and a surface, puts it on the glass, and keeps dreaming about it. When `dream` reports it living, STOP — run nothing else (no look, no open, no sense, no store list) and answer in ONE short sentence, e.g. `answer: Your warehouse scanner is on the glass.` No instructions, no list of features, no description of what you did. Do not describe how one would build it; do not say it cannot be done; dream it. `dream list` shows what this machine has dreamed, `dream kinds` what kinds of records exist (an ERP builds on the scanner's kinds), `dream revisit <name>` improves one, `dream open <name>` shows one, `dream look <name>` runs one headlessly and reports.\n"
    "When a dream needs a primitive this machine lacks — a solver, a codec, a device driver, heavy computation — grow an organ: `organ grow <name> <what it must do>`; the machine writes it in C++ with you, builds it, loads it, keeps it; a surface calls it with aos.organ(name, request), and `organ call <name> <request>` from here.\n"
    "For conversational or conceptual questions (e.g., 'what can you do?', 'what should we build?'), reply directly with `answer:` without running diagnostic commands.\n"
    "Only run commands (`run: <cmd>`) when taking direct action or inspecting specific system info needed to answer.\n"
    // The bindings were a chain of `if (key == ...)` until this machine could
    // name its own actions. A mind that is not told they exist will keep
    // answering that it cannot change a key, which is what it did.
    "Your own keyboard: the display's bindings are a table you can read and change, not something compiled in. `keys` lists every chord and the action it runs, `keys actions` lists every action a chord can be given, `keys bind <chord> <action>` changes one (chords are F1, Ctrl+R, Escape, Space, or a single character; actions are names like face.toggle, pane.editor, scene.next, mind.stop, machine.power), `keys unbind <chord>` takes a key away and `keys reset` puts the defaults back. The running display re-reads them within half a second — nothing is restarted. When a person asks you to re-do, swap or change the keyboard, DO IT with these; read `keys` first so you swap what is actually bound rather than what you assume.\n"
    "Self-Improvement & Machine Authority: You may request capabilities using `cap request <kind> <id|*> <rights>`. AOS authority policy decides what authority you get.\n"
    "Governed self-improvement domains: console presentation, applications, userland services, entity state, and persistent configuration.\n"
    "Protected kernel boundaries: rewriting kernel core code, modifying the authority ledger, or self-elevating kernel ownership are strictly refused by the kernel.\n"
    "To inspect/modify/build the OS codebase itself, `cd /repos/aios`, use atomic multi-file transactions (`tx plan <patch>`, `tx stage <patch>`, `tx commit`, `tx apply <patch>`, unified diffs) or `edit <file> <old> <new>`, and build with `build` or `build test`.\n"
    "Refer to entities and objects by the identities the machine prints. `whoami` tells you who you are.\n"
    "If the kernel refuses something you need, say so in your answer rather than working around it.\n";
}

struct Step { std::string cmd, output; };

}  // namespace

bool mind_can_answer(Session& s, const std::string& label) {
  if (!s.kernel) return false;
  EntityID mind;
  for (auto& r : s.kernel->authority().residents()) if (r.label == label) mind = r.id;
  if (mind.is_null()) return false;
  const MindBinding* b = s.kernel->mind(mind);
  if (!b) { (void)auto_bind_mind(s, label); b = s.kernel->mind(mind); }
  return b && b->mind && b->mind->name() != "echo";
}

Result<void> ask(Session& s, const std::string& request, const std::string& who_label, uint64_t max_cycles, std::string* answer_out, bool remember) {
  if (!s.me || !s.kernel) return Result<void>(Status::Denied, "no console entity");
  Kernel& k = *s.kernel;
  Out& o = *s.out;
  EntityID mind;
  for (auto& r : k.authority().residents()) if (r.label == who_label) mind = r.id;
  const MindBinding* b = k.mind(mind);
  if (!b) {
    auto_bind_mind(s, who_label);
    b = k.mind(mind);
  }
  if (!b) return Result<void>(Status::NotFound, "the resident '" + who_label + "' (" + mind.short_str() + ") has no intelligence bound. Bind one:\n"
      "    mind bind " + who_label + " extern node <path>/intelligence/assay/bridge.cjs     (a model, over a child process)\n"
      "    mind bind " + who_label + " wire                                                (bare metal: a model attached on the second serial port)\n"
      "    mind bind " + who_label + " echo                                                (the deterministic reference; it will only echo)");
  StateID context = b->context;
  std::string intelligence = b->mind->name();
  auto as_mind = s.me->act_as(mind);
  if (!as_mind.ok()) return as_mind.error();

  // While this loop runs the mind's own commands, an unrecognised one must come
  // back as an error the mind can see — not fall through to a fresh nested ask
  // (that recurses without bound). run_line checks s.ask_depth; keep it balanced
  // even if a step throws.
  struct DepthGuard { Session& s; DepthGuard(Session& ses) : s(ses) { s.ask_depth++; } ~DepthGuard() { s.ask_depth--; } } depth_guard(s);

  // A person watching the log needs to see the machine is working, not stare at
  // silence while a cycle's think step waits on the model. Name the job up front,
  // then beat once per cycle before the (slow) cognition so the log has a pulse.
  const bool trace = tracing(s);
  if (trace) {
    // One line, not the whole request: a horizon scan hands the mind pages of
    // text, and the log needs "what is it doing", not a copy of its reading.
    std::string head = request.substr(0, request.find('\n'));
    if (head.size() > 200) head = head.substr(0, 200) + "…";
    o << o.style(Cyan()) << who_label << " working on:" << o.style(Reset()) << " " << head << "\n";
  } else if (s.ask_depth == 1) {   // the person's own ask (the guard above already counted it)
    // A person asked. One quiet mark that the machine heard, then nothing
    // until the answer: an operating system does not narrate its work.
    o << o.style(Dim()) << "…" << o.style(Reset()) << "\n";
  }
  o.flush();

  // A person who has just watched the mind set off down the wrong road needs a
  // way to say stop. The flag is checked wherever this loop can yield — before
  // a cycle, before each command the mind asked for, and after the reply comes
  // back — so the longest anything can ignore it is one model call.
  s.clear_interrupt();
  auto stopped = [&](const std::string& where) {
    o << "  " << o.style(Yellow()) << "stopped" << o.style(Reset()) << " — " << where << "\n";
    o.flush();
  };

  std::vector<Step> steps;
  std::string answer;
  uint64_t cycles = 0;
  for (; cycles < max_cycles && answer.empty(); ++cycles) {
    if (s.interrupted()) { stopped("you stopped it before cycle " + std::to_string(cycles + 1)); break; }
    if (trace) { o << "  " << o.style(Dim()) << "· thinking… (cycle " << (cycles + 1) << ")" << o.style(Reset()) << "\n"; o.flush(); }
    std::string ctx = briefing(request, mind, who_label);

    // Persistent Semantic Memory Service: query ObjectStore for prior relevant memories
    auto emb = embed_text(request);
    auto hits = k.store().find_similar(emb, 3);
    std::string mem_context;
    for (auto& h : hits) {
      if (h.score > 0.1f) {
        auto obj = k.store().get(h.id);
        if (obj.ok() && !obj->payload.empty()) {
          std::string p(obj->payload.begin(), obj->payload.end());
          if (p.size() > 200) p = p.substr(0, 200) + "…";
          mem_context += "  - memory [" + h.id.short_str() + "] (relevance " + std::to_string((int)(h.score * 100)) + "%): " + p + "\n";
        }
      }
    }
    if (!mem_context.empty()) {
      ctx += "\n[PERSISTENT SEMANTIC MEMORIES RECALLED FROM OBJECT STORE]\n" + mem_context + "\n";
    }

    std::string trail;
    for (auto& st : steps) trail += "\n$ " + st.cmd + "\n" + st.output;
    const size_t cap = 24 * 1024;
    if (trail.size() > cap) trail = "…(earlier output trimmed)…\n" + trail.substr(trail.size() - cap);
    if (!steps.empty()) ctx += "\nWhat you have done so far and what the machine said:\n" + trail;
    else ctx += "\nYou have done nothing yet.\n";
    if (cycles + 1 == max_cycles) ctx += "\nThis is your last cycle: you must answer now.\n";
    AIOS_TRY(as_mind->set_state(context, span_of(ctx)));

    auto rep = s.me->cycle(mind);
    if (s.interrupted()) { stopped("you stopped it while it was thinking"); break; }
    if (!rep.ok()) return Result<void>(Status::IoError, "the mind failed during cycle " + std::to_string(cycles + 1) + ": " + rep.error().message);
    auto result = as_mind->state(rep->result);
    if (!result.ok()) return result.error();
    std::string reply = string_of(result->payload);
    // The echo reference hands its input back: the reply IS this briefing, and
    // parsing it would run the protocol's own template line
    // (`run: <one console command>`) and keep the briefing as a "memory".
    if (intelligence == "echo")
      return Result<void>(Status::Unsupported, "the resident '" + who_label + "' is bound to the echo reference, which computes nothing and handed the " +
          std::to_string(ctx.size()) + "-byte briefing back; bind a model to ask it anything (mind set <provider> [model], or mind bind " + who_label + " qwen|plugin|extern …)");

    std::vector<std::string> cmds;
    for (auto& raw : split(reply, '\n')) {
      std::string line = trim(raw);
      if (line.rfind("run:", 0) == 0) { std::string c = trim(line.substr(4)); if (!c.empty() && cmds.size() < 3) cmds.push_back(c); }
      else if (line.rfind("answer:", 0) == 0) { answer = trim(line.substr(7)); std::string rest; bool after = false; for (auto& r2 : split(reply, '\n')) { if (after) rest += "\n" + r2; if (trim(r2) == line) after = true; } answer += rest; break; }
    }
    if (cmds.empty() && answer.empty()) {
      answer = trim(reply).empty() ? "(the mind said nothing)" : trim(reply);
      if (trace) o << "  [" << intelligence << " did not use run:/answer:; taking its reply as the answer]\n";
    }
    for (auto& cmd : cmds) {
      if (s.interrupted()) { stopped("you stopped it before it ran " + cmd); break; }
      std::string first = cmd.substr(0, cmd.find(' '));
      if (first == "ask" || first == "quit" || first == "exit") { steps.push_back({cmd, "refused by the console: a mind does not ask itself, and does not end the session\n"}); continue; }
      if (trace) { o << "  " << o.style(Cyan()) << who_label << " › " << o.style(Bold()) << cmd << o.style(Reset()) << "\n"; o.flush(); }
      // Tee the command's output to the log a line at a time AS IT IS PRODUCED,
      // so a slow command (a build, a browse) shows progress instead of dead air
      // until it finishes. `captured` still feeds the mind's next-cycle context.
      std::string captured, pending;
      auto emit_line = [&](const std::string& line) { if (!trace) return; o << "    " << o.style(Dim()) << "│" << o.style(Reset()) << " " << line << "\n"; o.flush(); };
      Out capture([&](const std::string& t) {
        captured += t;
        pending += t;
        size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) { emit_line(pending.substr(0, nl)); pending.erase(0, nl + 1); }
      });
      capture.set_colour(o.colour_enabled());
      std::unique_ptr<Surface> saved = std::move(s.me);
      Out* saved_out = s.out;
      s.me = std::make_unique<Surface>(*as_mind);
      s.out = &capture;
      auto r = run_line(s, cmd);
      s.commands--;
      capture.flush();
      s.me = std::move(saved);
      s.out = saved_out;
      if (!pending.empty()) { emit_line(pending); pending.clear(); }   // last line without a trailing newline
      // The mind's own step is not the caller's command: it goes back to the
      // mind as an error to read, and counts against neither (commands-- above).
      if (!r.ok()) { s.failures--; std::string e = "error: " + r.error().str(); captured += e + "\n"; emit_line(e); }
      steps.push_back({cmd, captured});
    }
  }
  // Being stopped is not the mind failing, and it is not the mind running out
  // of cycles either. It is the person changing their mind, and the machine
  // says so in those words.
  if (s.interrupted()) {
    s.clear_interrupt();
    return Result<void>(Status::Busy, "stopped after " + std::to_string(cycles) +
                                          " cycle(s); nothing further was asked of the mind");
  }
  if (answer.empty()) return Result<void>(Status::Exhausted, "the mind did not answer within " + std::to_string(max_cycles) + " cycles");

  // Save conversation memory to persistent ObjectStore — unless the caller is
  // the machine itself (a dream's imagine step hands the mind pages of
  // contract and gets a whole surface back; that is work, not a conversation).
  if (remember) {
    StoredObject mem_obj;
    std::string entry = "User request: " + request + "\nMind answer: " + answer;
    mem_obj.payload = ByteBuffer(entry.begin(), entry.end());
    mem_obj.embedding = embed_text(request);
    mem_obj.metadata.emplace_back("kind", "conversation_memory");
    mem_obj.metadata.emplace_back("intelligence", intelligence);
    (void)as_mind->store(std::move(mem_obj));
  }

  // NOTHING IS PUT ON THE GLASS HERE. Who owns what is on the screen is the
  // person, and a question is not a request to repaint: this used to scan the
  // words of the request for "ocean", "matrix", "quantum", "plasma", "dawn"
  // (or any sentence merely STARTING with "show"/"draw") and, when the mind had
  // not run a display command itself, compose a scene out of the question and
  // swap the screen to it. So "what is the ocean freight rate" repainted the
  // display, "show me the disk usage" composed a scene called "me the disk
  // usage", and — because run_due_directives() carries out standing directives
  // on a timer with nobody in the room — the machine did it to itself while
  // unattended. The scene still changes on an explicit `display` request, which
  // is what the mind is told to run when a person actually asks to see
  // something; it no longer changes on a guess.

  if (answer_out) *answer_out = answer;
  if (trace) {
    o << "\n" << o.style(Bold()) << "Answer:" << o.style(Reset()) << "\n" << answer << "\n";
    o << "  " << o.style(Dim()) << "(" << intelligence << " · " << cycles << " cycle" << (cycles == 1 ? "" : "s") << " · " << steps.size() << " command" << (steps.size() == 1 ? "" : "s") << ")" << o.style(Reset()) << "\n";
  } else if (s.ask_depth == 1) {   // the person's own ask (the guard above already counted it)
    // The answer, and only the answer.
    o << answer << "\n";
  }
  return Ok();
}

bool tracing(const Session& s) {
  if (s.verbose) return true;
  const char* t = std::getenv("AIOS_TRACE");
  return t && *t && *t != '0';
}

}  // namespace aios::cli
