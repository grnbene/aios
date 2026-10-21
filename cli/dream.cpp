// `dream` — what a person asks for becomes a face of this machine.
//
// Not "generate an app". Software on this machine is not a program someone
// compiled and installed; it is a form the machine takes. When a person says
// "I need a warehouse scanner", the mind imagines the experience, writes the
// FACE — in this machine's own language, not a web page — the machine lays it
// out and looks at it, corrects it, and then REALISES it: an entity with
// authority over its own kinds of records, a face the compositor paints,
// records that are kernel objects any later dream can find. Then it is on the
// glass, and a standing directive keeps the machine dreaming about it while
// idle — software that evolves without a release, because there was never a
// release.
//
//     intend → imagine → look → correct → realise → live → keep dreaming
#include "cli.hpp"
#include "aios/capability/capability.hpp"
#include "aios/userland/face.hpp"
#include "aios/userland/key_bindings.hpp"
#include "aios/userland/organs.hpp"
#include "aios/userland/web_pane.hpp"

#include <algorithm>
#include <functional>
#include <map>

namespace aios::cli {

namespace {

template <typename T> T need(Result<T> r) { if (!r.ok()) throw Fail{r.error()}; return std::move(*r); }
void need(Result<void> r) { if (!r.ok()) throw Fail{r.error()}; }

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
std::string lowered(std::string s) {
  for (auto& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
  return s;
}

// A name the machine can use for a directory, an entity label and a record.
std::string slug(const std::string& s) {
  std::string o;
  bool dash = false;
  for (char c : lowered(s)) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    if (ok) { o += c; dash = false; }
    else if (!o.empty() && !dash) { o += '-'; dash = true; }
    if (o.size() >= 28) break;
  }
  while (!o.empty() && o.back() == '-') o.pop_back();
  return o.empty() ? "dream" : o;
}

std::string dreams_dir(Kernel& k) { return k.config().root + "/dreams"; }
std::string dream_dir(Kernel& k, const std::string& name) { return dreams_dir(k) + "/" + name; }
std::string face_path(Kernel& k, const std::string& name) { return dream_dir(k, name) + "/face.aos"; }
std::string face_url(const std::string& name) { return "face://" + name; }

std::string read_text(Kernel& k, const std::string& path) {
  auto f = k.host().read_file(path);
  if (!f.ok()) return {};
  return std::string((const char*)f->data(), f->size());
}

// ---- what the machine already keeps -----------------------------------------

// What this machine keeps, for the mind: every kind, who keeps it and the
// fields a face would see. This used to count only records a FACE had written
// (`made=surface`), so on a machine holding six hundred objects it said "none
// yet" — and the mind, told the machine keeps nothing, invented kind names for
// records that do not exist and wrote a face that renders zeroes forever.
std::string kinds_text(Kernel& k) { return store_kinds_text(k); }

// ---- imagine ----------------------------------------------------------------

std::string face_rules() {
  return
    "Rules:\n"
    "- Records are shared across dreams by KIND NAME. Reuse a kind that already exists on this machine when it means the same thing (an ERP over a scanner reads `item` and `movement`; it does not invent `inventoryRecord`). The store keeps EVERY record and `put` only ever ADDS one: a kind that means one row per thing (items by sku, orders by number, customers by id) MUST be declared `kind item key=sku`, so list(item) is the current rows and a put replaces the row. A kind that is a ledger (movement, scan, event) is not declared and every record is a row; `sort(list(movement), _t, desc)` is newest first; `find(items, sku, sku)` finds one.\n"
    "- A real workstation face: what a person needs at arm's length. Counts and totals as tiles; the field they use first with `focus` and an `enter=` action (a barcode gun is a keyboard that ends with Enter); tables and lists of the REAL records; `say` the last thing that happened so they know it worked. Never placeholder data — show what the store holds, and an `empty=` line when it holds nothing.\n"
    "- A game or anything animated: no title, one `paint fill`, `on frame` clears and draws it, `keys.ArrowLeft` is true while held, `on key Space:` for presses, `state` for what should survive (a best score). Keep it smooth and simple; dt is milliseconds.\n"
    "- Read what this machine ACTUALLY keeps. The kinds above are the whole of it, with the fields each record carries; a face that reads a kind this machine does not keep draws zeroes forever however well it is laid out, and the machine will tell you so. If nothing here fits what was asked for, the dream keeps its OWN records and says so in `kinds:`.\n"
    "- Stay INSIDE the language below. Every widget, function and verb this machine has is listed there; nothing else exists, and a word it does not know is reported back to you. After you reply the machine LAYS THE FACE OUT at the pane's size with its own fonts and sends back every line it could not understand, every fault, every text cut to fit its box, everything off the pane — fix every one.\n"
    "- If the dream truly needs what the language cannot do (heavy math, a codec, a solver, a device), name an organ and call organ(name, request) from the face.\n";
}

std::string imagine_request(const std::string& what, const std::string& kinds, const std::string& extra) {
  return
    "Write the face of what a person just asked this machine for.\n"
    "They said:\n    " + what + "\n\n"
    "This is a request for an ANSWER, not for a command: do not run anything — not `dream`, not `organ`, nothing. You are the imagining step of a dream that is already under way; reply with `answer:` and the face document in the form at the end.\n"
    "You are not writing a web page. You are giving this machine a new FACE, in its own language: the compositor lays it out and paints it every frame from the machine's state and store; its records are kernel objects with identity, provenance and an embedding, findable by every later dream. "
    "First imagine the EXPERIENCE — a person walks up, does what, sees what — then write the face that makes it so.\n\n"
    "Records other dreams already keep on this machine:\n" + kinds + "\n" +
    face_rules() +
    "\n" + userland::face::face_language_reference() + "\n" +
    extra +
    "\nReply in this form and nothing else:\n"
    "    answer:\n"
    "    name: <one or two words, lower-case, hyphenated>\n"
    "    experience: <two or three sentences: a person walks up, does this, sees that>\n"
    "    kinds: <comma-separated kind names this dream keeps, or none>\n"
    "    organs: <name :: what it must do; another :: …> or none\n"
    "    ---\n"
    "    face <name>\n"
    "    … the whole face, every line …\n";
}

struct Imagined {
  std::string name, experience, kinds, organs, source, problem;
};

// The face document out of a reply: what follows the `---` line, or from the
// first `face <name>` line; code fences taken off.
std::string face_of(const std::string& text) {
  std::string body;
  const size_t sep = text.find("\n---");
  if (sep != std::string::npos) {
    const size_t eol = text.find('\n', sep + 1);
    body = eol == std::string::npos ? "" : text.substr(eol + 1);
  } else {
    size_t at = text.find("\nface ");
    if (text.rfind("face ", 0) == 0) at = 0; else if (at != std::string::npos) at += 1;
    if (at == std::string::npos) return {};
    body = text.substr(at);
  }
  std::string out;
  for (auto& raw : split(body, '\n')) {
    std::string line = raw;
    while (!line.empty() && (line.back() == '\r')) line.pop_back();
    if (trim(line).rfind("```", 0) == 0) continue;
    out += line + "\n";
  }
  out = trim(out);
  return out.empty() ? out : out + "\n";
}

Imagined parse_imagined(const std::string& answer) {
  Imagined im;
  std::string head = answer;
  const size_t sep = answer.find("\n---");
  size_t face_at = answer.find("\nface ");
  if (answer.rfind("face ", 0) == 0) face_at = 0;
  if (sep != std::string::npos) head = answer.substr(0, sep);
  else if (face_at != std::string::npos) head = answer.substr(0, face_at);
  for (auto& raw : split(head, '\n')) {
    std::string line = trim(raw);
    if (line.rfind("answer:", 0) == 0) line = trim(line.substr(7));
    const size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    const std::string key = lowered(trim(line.substr(0, colon)));
    const std::string val = trim(line.substr(colon + 1));
    if (key == "name") im.name = slug(val);
    else if (key == "experience") im.experience = val;
    else if (key == "kinds") im.kinds = lowered(val) == "none" ? "" : val;
    else if (key == "organs") im.organs = lowered(val) == "none" ? "" : val;
  }
  im.source = face_of(answer);
  if (im.source.empty()) im.problem = "the reply held no face (nothing after a `---` line, no `face <name>` line)";
  else {
    userland::face::Face f = userland::face::parse_face(im.source);
    if (f.widgets.empty()) im.problem = "the face has no widgets: it would draw nothing";
    if (im.name.empty() && !f.name.empty()) im.name = slug(f.name);
  }
  return im;
}

// ---- realise ----------------------------------------------------------------

EntityID entity_named(Kernel& k, const std::string& name) {
  for (const auto& e : k.entities().list()) if (e.label == name) return e.id;
  return EntityID();
}

EntityID ensure_entity(Session& s, const std::string& name, Out& o) {
  Kernel& k = *s.kernel;
  EntityID id = entity_named(k, name);
  if (!id.is_null()) return id;
  // A RESIDENT, not a spawned entity: the first dream's entity was created
  // with spawn() and was gone the moment the process that dreamed it exited,
  // while its records and its face stayed — a face with nobody behind it.
  // A resident's identity is recorded at the root and comes back on every
  // boot, which is what a dream is.
  //
  // What a dream may do: keep records, hold state. Nothing else — it cannot
  // spawn, cannot grant, cannot reach the kernel. The kernel derives these
  // from what it holds and refuses anything else.
  AuthorityProfile profile = {
    {ResourceRef{ResourceKind::StorageObject, {}}, (uint16_t)(R_Read | R_Write), 0},
    {ResourceRef{ResourceKind::State, {}},         (uint16_t)(R_Read | R_Write | R_Allocate), 0},
  };
  auto made = k.resident(name, profile, /*self_improve=*/false);
  if (made.ok()) return *made;
  o << "  " << o.style(Yellow()) << "could not make " << name << " a resident (" << made.error().message
    << "); it lives for this process only" << o.style(Reset()) << "\n";
  return need(s.me->spawn(name));
}

void write_face(Kernel& k, const std::string& name, const std::string& source) {
  (void)k.host().make_dir(dreams_dir(k));
  (void)k.host().make_dir(dream_dir(k, name));
  need(k.host().write_file(face_path(k, name), ByteSpan((const uint8_t*)source.data(), source.size())));
}

void record_dream(Session& s, const std::string& name, const EntityID& e, const std::string& what,
                  const std::string& experience, const std::string& kinds, int revision) {
  StoredObject d;
  d.payload = ByteBuffer(experience.begin(), experience.end());
  d.embedding = embed_text(name + " " + what + " " + experience + " " + kinds);
  d.metadata.emplace_back("kind", "dream");
  d.metadata.emplace_back("dream", name);
  d.metadata.emplace_back("entity", e.str());
  d.metadata.emplace_back("url", face_url(name));
  d.metadata.emplace_back("spec", what);
  d.metadata.emplace_back("kinds", kinds);
  d.metadata.emplace_back("revision", std::to_string(revision));
  (void)s.me->store(std::move(d));
  // And as an app, so the launcher tile lists it and `app open` shows it.
  StoredObject a;
  a.metadata.emplace_back("kind", "app");
  a.metadata.emplace_back("app", name);
  a.metadata.emplace_back("url", face_url(name));
  a.metadata.emplace_back("dream", name);
  (void)s.me->store(std::move(a));
}

struct DreamRecord {
  bool found = false;
  std::string name, entity, url, spec, kinds, experience;
  int revision = 0;
  uint64_t at_ns = 0;
};

DreamRecord dream_record(Kernel& k, const std::string& name) {
  DreamRecord r;
  for (const auto& id : k.store().find_meta("kind", "dream")) {
    auto o = k.store().get(id);
    if (!o.ok() || o->meta("dream") != name) continue;
    if (o->timestamp_ns < r.at_ns) continue;
    r.found = true;
    r.at_ns = o->timestamp_ns;
    r.name = name;
    r.entity = o->meta("entity");
    r.url = o->meta("url");
    r.spec = o->meta("spec");
    r.kinds = o->meta("kinds");
    r.revision = std::atoi(o->meta("revision").c_str());
    r.experience.assign(o->payload.begin(), o->payload.end());
  }
  return r;
}

std::vector<DreamRecord> all_dreams(Kernel& k) {
  std::map<std::string, DreamRecord> newest;
  for (const auto& id : k.store().find_meta("kind", "dream")) {
    auto o = k.store().get(id);
    if (!o.ok()) continue;
    const std::string n = o->meta("dream");
    if (n.empty()) continue;
    auto it = newest.find(n);
    if (it != newest.end() && it->second.at_ns > o->timestamp_ns) continue;
    DreamRecord r;
    r.found = true;
    r.at_ns = o->timestamp_ns;
    r.name = n;
    r.entity = o->meta("entity");
    r.url = o->meta("url");
    r.spec = o->meta("spec");
    r.kinds = o->meta("kinds");
    r.revision = std::atoi(o->meta("revision").c_str());
    r.experience.assign(o->payload.begin(), o->payload.end());
    newest[n] = r;
  }
  std::vector<DreamRecord> out;
  for (auto& [n, r] : newest) if (r.url != "removed") out.push_back(r);
  return out;
}

// Put it on the glass: the desktop watches this file and raises the pane.
void show(Kernel& k, const std::string& url) {
  const std::string bp = userland::run_browse_path(k.config().root);
  const size_t slash = bp.rfind('/');
  if (slash != std::string::npos) (void)k.host().make_dir(bp.substr(0, slash));
  (void)k.host().write_file(bp, ByteSpan((const uint8_t*)url.data(), url.size()));
}

EntityID mind_entity(Kernel& k) {
  for (auto& r : k.authority().residents()) if (r.label == "mind") return r.id;
  return EntityID();
}

// Grow the organs a dream named: "name :: what; name2 :: what2".
void grow_named_organs(Session& s, const std::string& organs, Out& o) {
  for (auto& part : split(organs, ';')) {
    const size_t sep = part.find("::");
    if (sep == std::string::npos) continue;
    const std::string name = slug(trim(part.substr(0, sep)));
    const std::string what = trim(part.substr(sep + 2));
    if (name.empty() || what.empty()) continue;
    o << "  the dream needs an organ: " << o.style(Bold()) << name << o.style(Reset()) << " — " << what << "\n";
    o.flush();
    auto r = organ_grow(s, name, what);
    if (!r.ok()) o << "  " << o.style(Red()) << "organ " << name << " did not grow: " << r.error().message << o.style(Reset()) << "\n";
  }
}

// USE the face, the way the person who asked for it will. A look that only
// lays a face out cannot tell a working control from a painted one: the runtime
// reports "no `on X:` handler is written for that button" the moment an action
// is run, and nothing ever ran one — so every dead button, every field whose
// Enter does nothing, passed the look and shipped. This walks the widgets the
// mind wrote and does what a person would: type into the first field, press its
// Enter, then press each button once.
std::vector<std::string> drive_of(const userland::face::Face& f) {
  std::vector<std::string> drive;
  std::vector<std::string> actions;
  std::string first_field, first_enter;
  std::function<void(const userland::face::Widget&)> walk = [&](const userland::face::Widget& w) {
    const std::string action = w.attr("action");
    const std::string enter = w.attr("enter");
    if ((w.type == "field" || w.type == "input") && first_field.empty()) {
      first_field = w.attr("name", w.label);
      first_enter = enter;
    }
    if (!action.empty() && std::find(actions.begin(), actions.end(), action) == actions.end())
      actions.push_back(action);
    if (!enter.empty() && std::find(actions.begin(), actions.end(), enter) == actions.end())
      actions.push_back(enter);
    for (const auto& c : w.children) if (c) walk(*c);
  };
  for (const auto& w : f.widgets) if (w) walk(*w);
  // Something a barcode gun or a person would put in: short, printable, and
  // not a number, so a face that expects either still gets something.
  if (!first_field.empty()) drive.push_back("type:A1");
  for (const auto& a : actions) drive.push_back("action:" + a);
  return drive;
}

// One line of what the look found, for the console.
std::string look_summary(const FaceLook& L) {
  if (!L.loaded) return L.findings.empty() ? "could not be loaded" : L.findings[0];
  std::string r = std::to_string(L.placed) + " thing(s) laid out";
  if (L.painted) r += ", " + std::to_string(L.painted) + " painted";
  r += " over " + std::to_string(L.frames) + " frame(s)";
  return r;
}

// The whole loop for one request. `existing` is set for a revisit.
Result<void> dream_make(Session& s, const std::string& what, const DreamRecord* existing) {
  Kernel& k = *s.kernel;
  Out& o = *s.out;
  const bool revisit = existing && existing->found;
  if (s.dreaming)
    return Result<void>(Status::Busy, "this machine is already dreaming '" + what + "': do not run `dream` — you were asked to WRITE the face; reply with `answer:` and the face document");
  struct Dreaming { Session& s; explicit Dreaming(Session& x) : s(x) { s.dreaming = true; } ~Dreaming() { s.dreaming = false; } } dreaming_now{s};

  const bool trace = tracing(s);
  // Outcome, not narration: a person sees "dreaming …" and then "<name> is on
  // the glass". The look, the corrections and the face's text are for
  // AIOS_TRACE / `ask show=1`.
  o << o.style(Dim()) << (revisit ? "dreaming again: " : "dreaming: ") << what.substr(0, what.find('\n')) << o.style(Reset()) << "\n";
  o.flush();

  // 1. imagine
  std::string extra;
  if (revisit) {
    const std::string current = read_text(k, face_path(k, existing->name));
    extra += "\nThis machine already dreamed this as '" + existing->name + "' (revision " + std::to_string(existing->revision) +
             "). Its experience was: " + existing->experience + "\nKeep the SAME name and the SAME kinds so its records stay its records. Its current face follows; improve it for how it is actually being used, fix what is wrong, and reply with the WHOLE new face — or, if it is right as it is, reply exactly `answer: keep` and nothing else.\n" +
             "Current face:\n" + current + "\n";
    // What it LOOKS like, before deciding whether to keep it. The look used to
    // run only after a new face had been written, so a revisit judged a drawing
    // it had never seen and answered "keep" in one cycle — which is the one
    // answer a drawing nobody has looked at does not deserve.
    FaceLook before = face_look(s, face_url(existing->name), 8, drive_of(userland::face::parse_face(current)));
    extra += "\nThe machine ran that face and looked at it:\n" + face_look_report(before, 20) + "\n";
  }
  std::string answer;
  AIOS_TRY(ask(s, imagine_request(what, kinds_text(k), extra), "mind", 4, &answer, /*remember=*/false));
  if (revisit && lowered(trim(answer)).rfind("keep", 0) == 0) {
    o << "  the mind looked again and kept " << existing->name << " as it is\n";
    return Ok();
  }
  Imagined im = parse_imagined(answer);
  for (int round = 0; round < 2 && !im.problem.empty(); ++round) {
    if (trace) { o << "  " << o.style(Yellow()) << "not a dream yet: " << im.problem << o.style(Reset()) << " — asking again\n"; o.flush(); }
    AIOS_TRY(ask(s, imagine_request(what, kinds_text(k), extra + "\nYour previous reply was not accepted: " + im.problem + ". Reply again in the form below, with the whole face.\n"), "mind", 4, &answer, false));
    im = parse_imagined(answer);
  }
  if (!im.problem.empty()) return Result<void>(Status::Corrupt, "dream: " + im.problem);
  std::string name = revisit ? existing->name : (im.name.empty() ? slug(what) : im.name);
  if (!revisit) {
    // A second dream with the same name as a living one is a revisit in
    // disguise; give it its own name rather than overwriting a face someone
    // is using.
    if (dream_record(k, name).found && dream_record(k, name).url != "removed") {
      std::string base = name;
      for (int n = 2; n < 100 && dream_record(k, name).found; ++n) name = base + "-" + std::to_string(n);
    }
  }
  if (trace) {
    o << "  " << o.style(Bold()) << name << o.style(Reset()) << " — " << im.experience << "\n";
    if (!im.kinds.empty()) o << "  keeps: " << im.kinds << "\n";
    o.flush();
  }

  // 2. what it needs that the machine does not have
  if (!im.organs.empty()) grow_named_organs(s, im.organs, o);

  // 3. realise the entity, then look — as the dream itself, so what the look
  //    stores carries the dream's provenance.
  EntityID e = ensure_entity(s, name, o);
  write_face(k, name, im.source);
  FaceLook L = face_look(s, face_url(name), 8, drive_of(userland::face::parse_face(im.source)));
  for (int round = 0; round < 2 && !L.ok(); ++round) {
    if (trace) {
      o << "  " << o.style(Yellow()) << "looked at it: " << L.findings.size() << " thing(s) wrong" << o.style(Reset()) << " — correcting\n";
      for (const auto& f : L.findings) o << "    " << o.style(Dim()) << "saw: " << f << o.style(Reset()) << "\n";
      o.flush();
    }
    std::string corr = extra + "\nThe machine laid your face out and this is what it saw:\n" + face_look_report(L, 30) +
                       "\nFix every finding and reply with the WHOLE corrected face in the same form (name: " + name + ").\nYour face was:\n" + im.source + "\n";
    AIOS_TRY(ask(s, imagine_request(what, kinds_text(k), corr), "mind", 4, &answer, false));
    Imagined again = parse_imagined(answer);
    if (!again.problem.empty()) { if (trace) o << "  " << o.style(Yellow()) << again.problem << o.style(Reset()) << "\n"; continue; }
    im.source = again.source;
    if (!again.experience.empty()) im.experience = again.experience;
    if (!again.kinds.empty()) im.kinds = again.kinds;
    write_face(k, name, im.source);
    L = face_look(s, face_url(name), 8, drive_of(userland::face::parse_face(im.source)));
  }
  // A face that PAINTS gets one more round, with its own picture in hand. The
  // loop above only ever fixes faults, and a drawing's faults are not the kind
  // a layout check can see: a correct face can be three shapes that do not look
  // like the thing that was asked for. This is the only step that asks whether
  // it is any GOOD, and it is only worth asking now that the machine can show
  // the mind what it drew.
  if (L.ok() && !L.picture.empty()) {
    const std::string better =
        extra + "\nYour face is correct and this is what it actually draws:\n" + face_look_report(L, 20) +
        "\nOPEN THAT PICTURE AND LOOK AT IT. Then make the drawing better: more of the shapes the thing is"
        " actually made of, overlapping and layered largest first; shading with #rrggbbaa where a flat"
        " colour reads as cardboard; the whole pane used rather than a small thing in the middle of an"
        " empty one. Keep everything that works and reply with the WHOLE face. If it genuinely cannot be"
        " improved, reply exactly `answer: keep`.\n";
    std::string second;
    if (ask(s, imagine_request(what, kinds_text(k), better), "mind", 6, &second, false).ok() &&
        lowered(trim(second)).rfind("keep", 0) != 0) {
      Imagined nicer = parse_imagined(second);
      if (nicer.problem.empty()) {
        const std::string was = im.source;
        write_face(k, name, nicer.source);
        FaceLook L2 = face_look(s, face_url(name), 8, drive_of(userland::face::parse_face(nicer.source)));
        // Only if it is still right. A prettier face that faults is a worse face.
        if (L2.ok()) {
          im.source = nicer.source;
          if (!nicer.experience.empty()) im.experience = nicer.experience;
          L = L2;
          if (trace) o << "  " << o.style(Green()) << "looked at the picture and drew it again" << o.style(Reset()) << "\n";
        } else {
          write_face(k, name, was);
          if (trace) o << "  " << o.style(Yellow()) << "its second attempt was worse; kept the first" << o.style(Reset()) << "\n";
        }
      }
    }
  }
  if (trace) {
    o << "  looked at it: " << look_summary(L)
      << (L.ok() ? std::string() : std::string(o.style(Red()).code) + "  — still not right: " + (L.findings.empty() ? std::string("?") : L.findings[0]) + o.style(Reset()).code) << "\n";
    for (const auto& f : L.findings) o << "    " << o.style(Yellow()) << "saw: " << f << o.style(Reset()) << "\n";
    if (L.findings.empty() && L.loaded) o << "    " << o.style(Green()) << "saw: every word understood, nothing cut, nothing off the pane, every glyph drawable" << o.style(Reset()) << "\n";
    if (!L.message.empty()) o << "    it said: " << L.message << "\n";
    for (size_t i = 0; i < L.lines.size() && i < 12; ++i) o << "    " << o.style(Dim()) << "│ " << L.lines[i] << o.style(Reset()) << "\n";
    if (L.lines.size() > 12) o << "    " << o.style(Dim()) << "│ …" << o.style(Reset()) << "\n";
  } else if (!L.ok()) {
    // A face that went live with something wrong is worth one line, always.
    o << "  " << o.style(Yellow()) << "still not right: " << (L.findings.empty() ? std::string("?") : L.findings[0]) << o.style(Reset()) << "\n";
  }

  // Whatever is STILL wrong, kept as a record of this machine's own.
  //
  // These have already been through the correction rounds above: the mind was
  // shown each one and asked to fix it, twice. What is left is what it could
  // not fix, and that is usually a limit of the language or the renderer rather
  // than a mistake in this one dream. Printed, they scroll past; kept, they can
  // be counted — and the same finding turning up under dreams about a warehouse
  // and about deep space is the machine telling you where it is broken.
  for (const auto& f : L.findings) record_fault(s, "face.look", f, name);
  for (const auto& e2 : L.errors) record_fault(s, "face.parse", e2, name);

  // 4. record and show
  const int revision = revisit ? existing->revision + 1 : 1;
  record_dream(s, name, e, what, im.experience, im.kinds, revision);
  // A changed face under the same url would not be re-read; a fresh query
  // string makes the pane open it again.
  const std::string url = face_url(name) + (revisit ? "?r=" + std::to_string(revision) : std::string());
  show(k, url);
  if (trace)
    o << o.style(Green()) << (L.ok() ? "  living" : "  living, faulty") << o.style(Reset()) << ": " << name
      << " (entity " << e.short_str() << ") is on the glass — " << face_url(name) << "\n";
  else
    o << name << " is on the glass\n";

  // 5. keep dreaming — quietly: the directive's own report is the trace's.
  if (!revisit) {
    EntityID mind = mind_entity(k);
    if (!mind.is_null()) {
      std::string swallowed;
      Out quiet([&](const std::string& t) { swallowed += t; });
      Out* saved = s.out;
      if (!trace) s.out = &quiet;
      auto d = run_line(s, "directive add dream-" + name + " 6h as=" + mind.str() + " dream revisit " + name);
      s.out = saved;
      if (!d.ok() && trace) o << "  " << o.style(Dim()) << "(no standing directive: " << d.error().message << ")" << o.style(Reset()) << "\n";
    }
  }
  return Ok();
}

}  // namespace

Result<void> dream_command(Session& s, const std::vector<std::string>& a) {
  if (!s.kernel || !s.me) return Result<void>(Status::Invalid, "no kernel");
  Kernel& k = *s.kernel;
  Out& o = *s.out;
  const std::string sub = a.size() > 1 ? a[1] : "list";
  try {
    if (sub == "list" && a.size() <= 2) {
      auto ds = all_dreams(k);
      if (ds.empty()) { o << "this machine has not dreamed yet — `dream <what you want>`\n"; return Ok(); }
      o << o.style(Header()) << "What this machine has dreamed" << o.style(Reset()) << "\n";
      for (const auto& d : ds) {
        EntityID e = entity_named(k, d.name);
        o << "  " << o.style(Bold()) << d.name << o.style(Reset()) << "  rev " << d.revision
          << (e.is_null() ? "  (no entity)" : "  entity " + e.short_str()) << (d.kinds.empty() ? "" : "  keeps " + d.kinds) << "\n";
        o << "      asked: " << d.spec << "\n";
        o << "      " << o.style(Dim()) << d.experience << o.style(Reset()) << "\n";
      }
      o << "`dream open <name>` shows one; `dream revisit <name>` improves it; `dream kinds` lists what they keep\n";
      return Ok();
    }
    if (sub == "kinds") {
      o << o.style(Header()) << "Kinds of records dreams keep on this machine" << o.style(Reset()) << "\n" << kinds_text(k);
      return Ok();
    }
    if (sub == "show" || sub == "look" || sub == "open" || sub == "revisit" || sub == "remove" || sub == "face" || sub == "source") {
      if (a.size() < 3) fail(Status::Invalid, "dream " + sub + " <name>");
      const std::string name = slug(a[2]);
      DreamRecord d = dream_record(k, name);
      if (!d.found || d.url == "removed") fail(Status::NotFound, "this machine has no dream called '" + name + "' (`dream list`)");
      if (sub == "show") {
        o << o.style(Bold()) << d.name << o.style(Reset()) << "  rev " << d.revision << "  entity " << d.entity << "\n";
        o << "  asked: " << d.spec << "\n  experience: " << d.experience << "\n  keeps: " << (d.kinds.empty() ? "nothing" : d.kinds) << "\n  face: " << face_path(k, name) << "\n";
        return Ok();
      }
      if (sub == "face" || sub == "source") {
        o << read_text(k, face_path(k, name));
        return Ok();
      }
      if (sub == "look") {
        std::vector<std::string> drive(a.begin() + 3, a.end());
        FaceLook L = face_look(s, face_url(name), 12, drive);
        o << face_look_report(L, 60);
        return Ok();
      }
      if (sub == "open") {
        need(s.me->operate());
        show(k, face_url(name));
        o << name << " is on this machine's pane (" << face_url(name) << ")\n";
        return Ok();
      }
      if (sub == "revisit") {
        need(s.me->operate());
        // Evidence of how it is actually used goes with the request.
        std::string used;
        for (auto& kind : split(d.kinds, ',')) {
          const std::string kn = trim(kind);
          if (kn.empty()) continue;
          int n = 0;
          for (const auto& id : k.store().find_meta("kind", kn)) { auto ob = k.store().get(id); if (ob.ok() && ob->meta("made") == "surface") ++n; }
          used += "  " + kn + ": " + std::to_string(n) + " record(s)\n";
        }
        std::string what = d.spec + "\n(Revisiting. Since it was dreamed, its records:\n" + (used.empty() ? "  none kept\n" : used) + ")";
        need(dream_make(s, what, &d));
        return Ok();
      }
      if (sub == "remove") {
        need(s.me->operate());
        EntityID e = entity_named(k, name);
        if (!e.is_null()) (void)s.me->terminate(e);
        StoredObject t;
        t.metadata.emplace_back("kind", "dream");
        t.metadata.emplace_back("dream", name);
        t.metadata.emplace_back("url", "removed");
        (void)s.me->store(std::move(t));
        StoredObject ta;
        ta.metadata.emplace_back("kind", "app");
        ta.metadata.emplace_back("app", name);
        ta.metadata.emplace_back("url", "removed");
        (void)s.me->store(std::move(ta));
        (void)run_line(s, "directive remove dream-" + name);
        o << "dream " << name << " is over (its records stay in the store; its face stays under the root)\n";
        return Ok();
      }
    }
    // Everything else is the request itself.
    need(s.me->operate());
    std::string what;
    for (size_t i = 1; i < a.size(); ++i) { if (!what.empty()) what += " "; what += a[i]; }
    if (what.empty()) fail(Status::Invalid, "dream <what you want> | dream list | kinds | show|look|open|revisit|remove|face <name>");
    need(dream_make(s, what, nullptr));
  } catch (const Fail& f) {
    return Result<void>(f.e.status, f.e.message);
  }
  return Ok();
}

}  // namespace aios::cli
