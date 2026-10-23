// `face` — a dream's face, from the console: the language, a look at one, its
// text, putting it on the glass.
//
// A face is the machine's own description of what a dream looks like and does
// (userland/face.hpp). `face look` runs one the way the pane will — the pane's
// size, the pane's fonts, the same store — and says what it drew and what is
// wrong with it, before anyone sees it. It is what the dream loop calls on the
// mind's work; it is also how a person checks a face they wrote by hand.
#include "cli.hpp"

#include <algorithm>
#include <map>
#include <unordered_map>
#include "aios/userland/face.hpp"
#include "aios/userland/key_bindings.hpp"
#include "aios/userland/web_pane.hpp"
#include "aios/userland/compositor.hpp"
#include "aios/userland/web.hpp"

#include <cstdio>
#include <unistd.h>

namespace aios::cli {

namespace {

void need(Result<void> r) { if (!r.ok()) throw Fail{r.error()}; }

// The pane the desktop draws a face in, on a 1280-wide display.
// The pane a face is actually shown in. This was 1240x620 — about half the
// glass of the machine it runs on — and it is the size the LOOK reports back to
// the mind, so every face was imagined for a box half the size of the one it
// would be opened in, and arrived looking thin in the real pane with the bottom
// two thirds empty. A face lays itself out for the size it is given; give it
// the size it will get. AIOS_FACE_PANE overrides for a machine with a smaller
// display.
int face_viewport_w() {
  if (const char* e = getenv("AIOS_FACE_PANE")) { int w = 0, h = 0; if (sscanf(e, "%dx%d", &w, &h) == 2 && w > 200) return w; }
  return 1880;
}
int face_viewport_h() {
  if (const char* e = getenv("AIOS_FACE_PANE")) { int w = 0, h = 0; if (sscanf(e, "%dx%d", &w, &h) == 2 && h > 200) return h; }
  return 940;
}

std::string trim_ws(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// `<name>` is a dream under the root; anything with a slash or .aos is a file.
std::string face_url_of(const std::string& arg) {
  if (userland::is_face_url(arg)) return arg;
  if (arg.find('/') != std::string::npos || (arg.size() > 4 && arg.compare(arg.size() - 4, 4, ".aos") == 0)) {
    std::string p = arg;
    if (p[0] != '/') {
      char buf[4096];
      if (getcwd(buf, sizeof buf)) p = std::string(buf) + "/" + p;
    }
    return "face://" + p;
  }
  return "face://" + arg;
}

// The face's machine: as the dream's own entity when it has one, else as the
// console.
userland::HostBindings bindings_for(Session& s, const std::string& dream, const std::string& url) {
  Kernel& k = *s.kernel;
  if (!dream.empty()) {
    for (const auto& e : k.entities().list()) {
      if (e.label != dream) continue;
      auto as = s.me->act_as(e.id);
      if (as.ok()) return userland::surface_bindings(k, *as, dream);
      break;
    }
  }
  return userland::surface_bindings(k, *s.me, dream.empty() ? url : dream);
}

}  // namespace

// One fault, kept. See cli.hpp for why these are worth keeping at all.
void record_fault(Session& s, const std::string& where, const std::string& what, const std::string& about) {
  if (!s.kernel || !s.me) return;
  const std::string text = trim_ws(what);
  if (text.empty()) return;
  auto quote = [](const std::string& in) {
    std::string out = "\"";
    for (char c : in) {
      switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': break;
        case '\t': out += "\\t"; break;
        default:
          if ((unsigned char)c < 0x20) out += ' ';
          else out += c;
      }
    }
    return out + "\"";
  };
  // As the thing it happened TO where that is an entity of this machine, so the
  // record carries the dream's own provenance rather than the console's.
  userland::HostBindings hb = bindings_for(s, about, about);
  if (!hb.store_put) return;
  // Cut long compiler output to the first lines that carry the diagnosis; the
  // whole build log belongs in its file, not in a record meant to be counted.
  std::string body = text.size() > 600 ? text.substr(0, 600) + " …" : text;
  const std::string json = "{\"where\": " + quote(where) + ", \"what\": " + quote(body) +
                           ", \"about\": " + quote(about) + "}";
  (void)hb.store_put("fault", json);
}

// Run a face the way the pane will, for `frames` of its own time. What comes
// back is what the look step reports to the mind.
FaceLook face_look(Session& s, const std::string& url, int frames, const std::vector<std::string>& drive) {
  FaceLook L;
  Kernel& k = *s.kernel;
  L.url = url;
  L.path = userland::face_source_path(k.config().root, url);
  auto f = k.host().read_file(L.path);
  if (!f.ok()) { L.findings.push_back("NO FACE: nothing at " + L.path); return L; }
  const std::string source((const char*)f->data(), f->size());
  L.source = source;
  userland::face::Face face = userland::face::parse_face(source);
  L.name = face.name;
  L.title = face.title;
  L.errors = face.errors;
  userland::face::Runtime rt(std::move(face));
  const std::string dream = userland::dream_of_url(url);
  rt.bind(bindings_for(s, dream, url));
  rt.load();
  int frame = 0;
  const int pane_w = face_viewport_w(), pane_h = face_viewport_h();
  auto tick = [&] { rt.frame(16.7, pane_w, pane_h); ++frame; };
  tick();
  // Scripted use, the same words the headless desktop takes: type:<text>,
  // key:<Name>, click:<x>,<y>. Each followed by a frame, so what it did shows.
  for (const auto& d : drive) {
    const size_t c = d.find(':');
    const std::string kind = c == std::string::npos ? d : d.substr(0, c);
    const std::string arg = c == std::string::npos ? std::string() : d.substr(c + 1);
    if (kind == "type") rt.type(arg);
    else if (kind == "key") { rt.key(arg, true); rt.key(arg, false); }
    else if (kind == "click") { int x = 0, y = 0; std::sscanf(arg.c_str(), "%d,%d", &x, &y); rt.click(x, y); }
    else if (kind == "action") rt.run_action(arg);
    tick();
  }
  while (frame < frames) tick();
  rt.flush_storage();
  L.frames = frame;
  L.loaded = true;
  L.placed = (int)rt.placed().size();
  L.painted = (int)rt.paint_cmds().size();
  L.findings = rt.inspect();
  L.reads = rt.reads();
  // A face that DRAWS is rendered and written out as a picture. The mind writes
  // paint commands blind — it has never once seen what it drew, which is most
  // of why what it draws is poor — and a model running on this machine can
  // read images, so a path to a PNG is something it can open and look at.
  if (!rt.paint_cmds().empty()) {
    std::vector<uint32_t> px((size_t)pane_w * (size_t)pane_h, 0xFF0F1318u);
    rt.paint(px, pane_w, pane_h, 0, 0);
    // Encoded at full size. This used to be halved and 2x2-averaged, because
    // the encoder stored its pixels rather than compressing them and a full
    // pane was seven megabytes. It filters and deflates now — the same picture
    // is tens of kilobytes — and the halving was never free: this image exists
    // so the mind can judge shading, thin curves and small marks, which are
    // exactly what averaging takes away first.
    auto png = userland::web::encode_png(px.data(), pane_w, pane_h);
    if (!png.empty()) {
      const std::string at = k.config().root + "/run/face-look.png";
      const size_t slash = at.rfind('/');
      if (slash != std::string::npos) (void)k.host().make_dir(at.substr(0, slash));
      if (k.host().write_file(at, ByteSpan(png.data(), png.size())).ok()) L.picture = at;
    }
  }
  // Is there anything THERE?
  //
  // Everything above asks whether the face is well-FORMED: every word
  // understood, nothing cut, nothing off the pane, every glyph drawable. A
  // face that paints thirteen small discs into one corner of a black pane
  // passes all four, so the machine reported success while a person looked at
  // an empty screen. Asked for a deep-space oratorium it drew 13 shapes over a
  // fraction of a percent of the glass, and nothing in the look disagreed.
  //
  // So measure the picture that was just rendered, because it is already here.
  // Not a judgement of taste — a ruler. How much ink, where it sits, and
  // whether any of it moved. Each finding carries its number so the correction
  // that follows is aimed at something real rather than at "make it better".
  if (!rt.paint_cmds().empty() && pane_w > 0 && pane_h > 0) {
    std::vector<uint32_t> a((size_t)pane_w * (size_t)pane_h, 0xFF0F1318u);
    rt.paint(a, pane_w, pane_h, 0, 0);

    // The background is whatever colour the most pixels are, not the colour
    // that was cleared: a face that lays down a full-pane gradient and draws on
    // top of it HAS a background, and its subject is the part that differs.
    // Quantised to 5 bits a channel so a gradient does not read as a million
    // different backgrounds.
    // Measured over the DRAWING SURFACE, not the pane. The paint widget is
    // inset inside the pane, and the chrome around it is a different colour
    // from whatever the face cleared to — counted as pane, that border alone
    // is "ink" spanning the full width, which put the bounding box at 100% of
    // the pane and silenced both checks on a face that was plainly empty.
    int rx = 0, ry = 0, rw = pane_w, rh = pane_h;
    for (const auto& pl : rt.placed()) {
      if (pl.w && pl.w->type == "paint" && pl.width > 0 && pl.height > 0) {
        rx = pl.x; ry = pl.y; rw = pl.width; rh = pl.height;
        break;
      }
    }
    if (rx < 0) rx = 0;
    if (ry < 0) ry = 0;
    if (rx + rw > pane_w) rw = pane_w - rx;
    if (ry + rh > pane_h) rh = pane_h - ry;

    std::unordered_map<uint32_t, uint32_t> tally;
    for (int y = ry; y < ry + rh; ++y)
      for (int x = rx; x < rx + rw; ++x) ++tally[a[(size_t)y * (size_t)pane_w + (size_t)x] & 0xF8F8F8F8u];
    uint32_t bg = 0, bg_n = 0;
    for (const auto& kv : tally) if (kv.second > bg_n) { bg = kv.first; bg_n = kv.second; }

    const size_t total = (size_t)rw * (size_t)rh;
    size_t ink = 0;
    int x0 = rx + rw, y0 = ry + rh, x1 = -1, y1 = -1;
    for (int y = ry; y < ry + rh; ++y) {
      for (int x = rx; x < rx + rw; ++x) {
        if ((a[(size_t)y * (size_t)pane_w + (size_t)x] & 0xF8F8F8F8u) == bg) continue;
        ++ink;
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
      }
    }
    const int pct_ink = total ? (int)((ink * 100 + total / 2) / total) : 0;

    if (ink == 0) {
      L.findings.push_back("THE PANE IS EMPTY: " + std::to_string(L.painted) +
          " paint command(s) ran and every pixel came out one colour. Nothing you drew is visible — check the shapes fall inside 0,0 to " +
          std::to_string(rw) + "," + std::to_string(rh) + " and are not the colour you cleared to.");
    } else {
      const int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
      const int pct_box = total ? (int)(((long long)bw * bh * 100) / (long long)total) : 0;
      if (pct_ink < 4) {
        L.findings.push_back("NEARLY EMPTY: what you drew covers " + std::to_string(pct_ink) + "% of the " +
            std::to_string(rw) + "x" + std::to_string(rh) +
            " drawing surface and the rest is flat background. Fill the glass: give the background something of its own (a gradient, a field of small marks) and draw the subject large enough to be looked at.");
      }
      if (pct_box < 25) {
        L.findings.push_back("ALL IN ONE PLACE: everything you drew sits in a " + std::to_string(bw) + "x" + std::to_string(bh) +
            " box at " + std::to_string(x0) + "," + std::to_string(y0) + " — " + std::to_string(pct_box) + "% of the pane, with the other " +
            std::to_string(100 - pct_box) + "% untouched. Use the whole pane: spread the composition over it, or scale the subject up to fill it.");
      }
    }

    // Did any of it move? A face that says it animates and paints identical
    // pixels every frame is a still picture with a frame loop around it. Only
    // asked of a face that actually has an `on frame` rule to run.
    if (rt.has_frame_rule()) {
      tick();
      std::vector<uint32_t> b((size_t)pane_w * (size_t)pane_h, 0xFF0F1318u);
      rt.paint(b, pane_w, pane_h, 0, 0);
      size_t moved = 0;
      for (int y = ry; y < ry + rh; ++y)
        for (int x = rx; x < rx + rw; ++x) {
          const size_t i = (size_t)y * (size_t)pane_w + (size_t)x;
          if (a[i] != b[i]) ++moved;
        }
      if (moved * 1000 < total) {
        L.findings.push_back("NOTHING MOVES: `on frame` runs, but two consecutive frames are the same picture. If it is meant to turn, drift or pulse, advance a state by dt and draw from that.");
      }
    }
  }

  L.lines = rt.as_text();
  L.message = rt.message();
  L.focus = rt.has_focus();
  return L;
}

// The top-level keys of a json object, without parsing it: what a face would
// see as fields on this record. Depth is counted so keys inside a nested object
// are not offered as if they were on the record itself.
static void json_top_keys(const std::string& body, std::vector<std::string>& out) {
  int depth = 0;
  bool in_str = false, esc = false;
  std::string key;
  bool have_key = false;
  for (size_t i = 0; i < body.size(); ++i) {
    const char c = body[i];
    if (in_str) {
      if (esc) { esc = false; key += c; continue; }
      if (c == '\\') { esc = true; continue; }
      if (c == '"') { in_str = false; have_key = depth == 1; continue; }
      key += c;
      continue;
    }
    if (c == '"') { key.clear(); in_str = true; continue; }
    if (c == '{' || c == '[') { ++depth; continue; }
    if (c == '}' || c == ']') { --depth; continue; }
    if (c == ':' && have_key && depth == 1) {
      if (!key.empty() && std::find(out.begin(), out.end(), key) == out.end()) out.push_back(key);
      have_key = false;
    }
  }
}

std::string store_kinds_text(Kernel& k) {
  struct Kind {
    size_t count = 0;
    bool by_dream = false, by_machine = false;
    std::string dream;
    uint64_t newest = 0;
    std::vector<std::string> fields;
  };
  std::map<std::string, Kind> kinds;
  for (const auto& id : k.store().list()) {
    auto o = k.store().get(id);
    if (!o.ok()) continue;
    const std::string kind = o->meta("kind");
    if (kind.empty()) continue;
    Kind& e = kinds[kind];
    e.count++;
    if (o->meta("made") == "surface") { e.by_dream = true; if (e.dream.empty()) e.dream = o->meta("dream"); }
    else e.by_machine = true;
    if (o->timestamp_ns < e.newest && e.count > 1) continue;
    e.newest = o->timestamp_ns;
    // The fields a face sees: the record's metadata, then its payload's own
    // keys, which is exactly what record_json hands the runtime.
    e.fields.clear();
    for (const auto& [key, value] : o->metadata) {
      (void)value;
      if (key.empty() || key == "kind" || key == "dream" || key == "made" || key[0] == '_') continue;
      if (std::find(e.fields.begin(), e.fields.end(), key) == e.fields.end()) e.fields.push_back(key);
    }
    std::string body(o->payload.begin(), o->payload.end());
    const size_t a = body.find_first_not_of(" \t\r\n");
    if (a != std::string::npos && body[a] == '{') json_top_keys(body.substr(a), e.fields);
    else if (!body.empty()) e.fields.push_back("text");
  }
  if (kinds.empty()) return "  none yet — this machine keeps no records at all; this dream would be the first\n";
  std::string t;
  for (const auto& [kind, e] : kinds) {
    t += "  " + kind + "  x" + std::to_string(e.count) + "  kept by ";
    if (e.by_machine && e.by_dream) t += "this machine and by dreams";
    else if (e.by_machine) t += "this machine itself";
    else t += e.dream.empty() ? "a dream" : "dream " + e.dream;
    if (!e.fields.empty()) {
      t += "  fields:";
      for (size_t i = 0; i < e.fields.size() && i < 12; ++i) t += " " + e.fields[i];
      if (e.fields.size() > 12) t += " …";
    }
    t += "  (plus _id, _t, _kind on every record)\n";
  }
  return t;
}

bool FaceLook::ok() const {
  if (!loaded) return false;
  for (const auto& f : findings) {
    // Cut text is a defect the mind must fix; the rest are stops.
    if (f.rfind("NOT UNDERSTOOD", 0) == 0 || f.rfind("FAULT", 0) == 0 || f.rfind("EMPTY", 0) == 0 ||
        f.rfind("NO RECORDS", 0) == 0 || f.rfind("OFF THE PANE", 0) == 0 ||
        f.rfind("CUT TO FIT", 0) == 0 || f.rfind("GLYPHS", 0) == 0 || f.rfind("NO FACE", 0) == 0) return false;
  }
  return true;
}

std::string face_look_report(const FaceLook& L, size_t max_lines) {
  std::string r;
  if (!L.loaded) { for (const auto& f : L.findings) r += f + "\n"; return r; }
  r += "face: " + (L.name.empty() ? std::string("(unnamed)") : L.name) + (L.title.empty() ? "" : "  title: " + L.title) + "\n";
  r += "laid out " + std::to_string(L.placed) + " thing(s)" + (L.painted ? ", painted " + std::to_string(L.painted) + " shape(s)" : "") +
       " over " + std::to_string(L.frames) + " frame(s) on a " + std::to_string(face_viewport_w()) + "x" +
       std::to_string(face_viewport_h()) + " pane\n";
  if (!L.message.empty()) r += "it said: " + L.message + "\n";
  if (L.findings.empty()) r += "looked at the glass: every word understood, nothing cut, nothing off the pane, every glyph drawable\n";
  if (!L.reads.empty()) {
    r += "records it read:";
    for (const auto& [kind, rows] : L.reads) r += "  " + kind + " -> " + std::to_string(rows) + " row(s)";
    r += "\n";
  }
  for (const auto& f : L.findings) r += "  - " + f + "\n";
  if (!L.picture.empty())
    r += "the picture it drew is at " + L.picture +
         " - OPEN IT AND LOOK AT IT before you answer, and fix what is wrong with the drawing itself:"
         " shapes that do not join, a thing that does not read as what it is meant to be, flat colour where"
         " there should be shading, an empty half of the pane.\n";
  r += "the face, as text:\n";
  size_t n = 0;
  for (const auto& l : L.lines) {
    if (n++ >= max_lines) { r += "  …\n"; break; }
    r += "  " + l + "\n";
  }
  return r;
}

Result<void> face_command(Session& s, const std::vector<std::string>& a) {
  if (!s.kernel || !s.me) return Result<void>(Status::Invalid, "no kernel");
  Kernel& k = *s.kernel;
  Out& o = *s.out;
  const std::string sub = a.size() > 1 ? a[1] : "";
  const std::string usage = "face reference | face look <name|file> [type:… key:… click:x,y action:…]… | face text <name|file> | face open <name|file> | face source <name|file>";
  try {
    if (sub == "reference" || sub == "language" || sub == "help") {
      o << userland::face::face_language_reference();
      return Ok();
    }
    if (a.size() < 3) fail(Status::Invalid, usage);
    const std::string url = face_url_of(a[2]);
    if (sub == "look" || sub == "text") {
      std::vector<std::string> drive(a.begin() + 3, a.end());
      FaceLook L = face_look(s, url, 12, drive);
      if (sub == "text") { for (const auto& l : L.lines) o << l << "\n"; return Ok(); }
      o << face_look_report(L, 80);
      if (!L.ok()) return Result<void>(Status::Corrupt, "the face is not right yet");
      return Ok();
    }
    if (sub == "source") {
      auto f = k.host().read_file(userland::face_source_path(k.config().root, url));
      if (!f.ok()) fail(Status::NotFound, "no face at " + userland::face_source_path(k.config().root, url));
      o << std::string((const char*)f->data(), f->size());
      return Ok();
    }
    if (sub == "open" || sub == "show") {
      need(s.me->operate());
      const std::string bp = userland::run_browse_path(k.config().root);
      const size_t slash = bp.rfind('/');
      if (slash != std::string::npos) (void)k.host().make_dir(bp.substr(0, slash));
      need(k.host().write_file(bp, ByteSpan((const uint8_t*)url.data(), url.size())));
      o << url << " is on this machine's pane\n";
      return Ok();
    }
    fail(Status::Invalid, usage);
  } catch (const Fail& f) {
    return Result<void>(f.e.status, f.e.message);
  }
  (void)trim_ws;
  return Ok();
}

}  // namespace aios::cli
