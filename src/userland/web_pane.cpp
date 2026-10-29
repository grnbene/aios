// Fragments to pixels, and clicks to navigations.
#include <chrono>
#include "aios/userland/web_pane.hpp"
#include "aios/userland/web_js.hpp"
#include "aios/userland/organs.hpp"
#include "aios/storage/aql.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <functional>

#include "aios/channel/http_client.hpp"
#include "aios/hal/clipboard.hpp"
#include "aios/hal/keys.hpp"
#include "aios/userland/compositor.hpp"
#include "aios/userland/font_atlas.hpp"
#include "aios/userland/glyph_engine.hpp"
#include "aios/userland/git.hpp"

extern bool g_css_has_hover;   // web_css.cpp
extern bool g_css_has_active;  // web_css.cpp: a :active rule exists, so a press marks the pressed element
namespace aios::userland {
namespace {

// The chrome's palette. Its own, not the desktop's tokens: this is a browser,
// and a browser's chrome is meant to recede behind the page.
constexpr uint32_t kChrome = 0xFF1A1A1A;
constexpr uint32_t kChromeEdge = 0xFF2E2E2E;
constexpr uint32_t kField = 0xFF0E0E0E;
constexpr uint32_t kFieldEdge = 0xFF3A3A3A;
constexpr uint32_t kFieldFocus = 0xFF5B8DD9;
constexpr uint32_t kInk = 0xFFD8D8D8;
constexpr uint32_t kFaint = 0xFF8A8A8A;
constexpr uint32_t kLockOk = 0xFF57C77E;
constexpr uint32_t kErrorInk = 0xFFE8746A;
// Scrollbars follow the page's ground, as Chrome's do: a dark bar on a white page stood out like a tear in it.
constexpr uint32_t kScrollTrack = 0xFF181818;
constexpr uint32_t kScrollThumb = 0xFF4A4A4A;
constexpr uint32_t kScrollTrackLight = 0xFFF1F1F1;
constexpr uint32_t kScrollThumbLight = 0xFFC1C1C1;
// Chrome's focus ring on a light page; the chrome's own blue on a dark one.
constexpr uint32_t kFocusRingLight = 0xFF005FCC;

constexpr int kChromeH = 34;
constexpr int kStatusH = 20;
constexpr int kScrollW = 9;


}  // namespace

namespace {

std::string dec(uint64_t v) {
  if (!v) return "0";
  std::string s;
  while (v) {
    s.insert(s.begin(), (char)('0' + v % 10));
    v /= 10;
  }
  return s;
}

char lower_c(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

std::string lowered(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (char c : s) o += lower_c(c);
  return o;
}

// A gzip member, undone with the inflater the kernel already had for git. This
// is the transport's decompressor, not a fallback: the pane asks for gzip, and
// without this the page arrives as binary and parses to nothing.
bool gunzip(const std::string& in, std::string& out) {
  if (in.size() < 18) return false;
  const uint8_t* p = (const uint8_t*)in.data();
  if (p[0] != 0x1F || p[1] != 0x8B || p[2] != 8) return false;
  uint8_t flags = p[3];
  size_t at = 10;
  if (flags & 0x04) {   // FEXTRA
    if (at + 2 > in.size()) return false;
    at += 2 + (size_t)(p[at] | (p[at + 1] << 8));
  }
  if (flags & 0x08) {   // FNAME, NUL-terminated
    while (at < in.size() && p[at]) ++at;
    ++at;
  }
  if (flags & 0x10) {   // FCOMMENT, likewise
    while (at < in.size() && p[at]) ++at;
    ++at;
  }
  if (flags & 0x02) at += 2;   // FHCRC
  if (at >= in.size()) return false;
  // What follows the header is a bare deflate stream, which is exactly what
  // inflate_raw takes — no zlib wrapper to skip.
  size_t end = 0;
  auto raw = inflate_raw(ByteSpan(p, in.size()), at, &end);
  if (!raw.ok()) return false;
  out.assign((const char*)raw->data(), raw->size());
  return true;
}

}  // namespace

// The text a page draws, through the shared typesetter (web_type.cpp): the
// compositor is not needed to put glyphs into a buffer.
void web_draw_text(HardwareCompositor&, std::vector<uint32_t>& buf, int bw, int bh, int x, int y,
                   const std::string& text, uint32_t color, const web::FontKey& f, const DrawClip& clip) {
  web_draw_text_px(buf, bw, bh, x, y, text, color, f, clip);
}

// ---------------------------------------------------------------- fields ---
//
// Typing into the page, not into the url bar. The pane holds a raw Node* for
// the focused field, and a script is free to throw that node away between one
// keystroke and the next -- so every use is preceded by finding it again in
// the live tree.
namespace {

bool node_in_tree(const web::Node* root, const web::Node* want) {
  if (!root || !want) return false;
  if (root == want) return true;
  for (const auto& kid : root->children)
    if (node_in_tree(kid.get(), want)) return true;
  return false;
}

// The field under the pointer: the control itself, or the control an inner
// span belongs to.
web::Node* field_ancestor(web::Node* n) {
  for (web::Node* p = n; p; p = p->parent)
    if (p->kind == web::Node::Kind::Element && (p->tag == "input" || p->tag == "textarea")) {
      const std::string type = p->attr("type");
      if (p->tag == "input" && (type == "hidden" || type == "checkbox" || type == "radio" ||
                                type == "submit" || type == "button" || type == "file"))
        return nullptr;
      return p;
    }
  return nullptr;
}

// ---- checkboxes and radios: what a click on one does ----------------------------------------------------------

bool checkable(const web::Node* n) {
  if (!n || n->kind != web::Node::Kind::Element || n->tag != "input" || n->has_attr("disabled")) return false;
  std::string type = n->attr("type");
  for (auto& ch : type) ch = (char)std::tolower((unsigned char)ch);
  return type == "checkbox" || type == "radio";
}

web::Node* find_checkable(web::Node* n, int depth = 0) {
  if (!n || depth > 64) return nullptr;
  if (checkable(n)) return n;
  for (const auto& kid : n->children)
    if (web::Node* hit = find_checkable(kid.get(), depth + 1)) return hit;
  return nullptr;
}

web::Node* find_by_id(web::Node* n, const std::string& id, int depth = 0) {
  if (!n || id.empty() || depth > 256) return nullptr;
  if (n->kind == web::Node::Kind::Element && n->attr_ref("id") == id) return n;
  for (const auto& kid : n->children)
    if (web::Node* hit = find_by_id(kid.get(), id, depth + 1)) return hit;
  return nullptr;
}

// The checkbox or radio a click acts on: the control itself, or the one its <label> names with `for` or holds.
web::Node* toggle_target(web::Node* clicked) {
  for (web::Node* p = clicked; p; p = p->parent) {
    if (checkable(p)) return p;
    if (p->kind == web::Node::Kind::Element && p->tag == "label") {
      if (p->has_attr("for")) {
        web::Node* root = p;
        while (root->parent) root = root->parent;
        web::Node* named = find_by_id(root, p->attr("for"));
        return checkable(named) ? named : nullptr;
      }
      return find_checkable(p);
    }
  }
  return nullptr;
}

void set_checked(web::Node* n, bool on) {
  for (auto it = n->attrs.begin(); it != n->attrs.end(); ++it)
    if (it->name == "checked") {
      if (!on) n->attrs.erase(it);
      return;
    }
  if (on) n->attrs.push_back({"checked", ""});
}

// A checkbox flips; a radio is checked and every other radio of its name in the same form (or document) is not.
// Returns each touched control's state before, so a click a handler prevents can be put back.
std::vector<std::pair<web::Node*, bool>> activate_checkable(web::Node* t) {
  std::vector<std::pair<web::Node*, bool>> before;
  std::string type = t->attr("type");
  for (auto& ch : type) ch = (char)std::tolower((unsigned char)ch);
  if (type == "checkbox") {
    before.push_back({t, t->has_attr("checked")});
    set_checked(t, !t->has_attr("checked"));
    return before;
  }
  const std::string name = t->attr("name");
  if (!name.empty()) {
    web::Node* scope = web::enclosing_form(t);
    if (!scope) { scope = t; while (scope->parent) scope = scope->parent; }
    std::function<void(web::Node*)> walk = [&](web::Node* n) {
      if (!n) return;
      if (n != t && n->kind == web::Node::Kind::Element && n->tag == "input" && n->attr("name") == name &&
          n->has_attr("checked") && web::enclosing_form(n) == web::enclosing_form(t)) {
        std::string ty = n->attr("type");
        for (auto& ch : ty) ch = (char)std::tolower((unsigned char)ch);
        if (ty == "radio") { before.push_back({n, true}); set_checked(n, false); }
      }
      for (const auto& kid : n->children) walk(kid.get());
    };
    walk(scope);
  }
  before.push_back({t, t->has_attr("checked")});
  set_checked(t, true);
  return before;
}

std::string field_value(const web::Node* n) {
  if (!n) return {};
  if (n->tag == "textarea" && !n->has_attr("value")) return n->inner_text();
  return n->attr("value");
}

void field_set_value(web::Node* n, const std::string& v) {
  if (!n) return;
  bool found = false;
  for (auto& at : n->attrs)
    if (at.name == "value") { at.value = v; found = true; break; }
  if (!found) {
    web::Attr a;
    a.name = "value";
    a.value = v;
    n->attrs.push_back(a);
  }
  if (n->tag == "textarea") {
    n->children.clear();
    auto t = std::make_unique<web::Node>();
    t->kind = web::Node::Kind::Text;
    t->text = v;
    t->parent = n;
    n->children.push_back(std::move(t));
  }
}

// One character off the end, UTF-8 aware: a backspace that ate a single byte
// of a multi-byte character left the page holding invalid text.
void field_backspace(std::string& v) {
  if (v.empty()) return;
  size_t at = v.size() - 1;
  while (at > 0 && ((unsigned char)v[at] & 0xC0) == 0x80) --at;
  v.erase(at);
}

// Run one piece of the page's own work -- a handler, and the layout it makes
// necessary -- off the thread that draws, so a slow application does not stop
// the desktop. Everything it touches is reached through shared_ptrs it holds,
// so the pane cannot go away underneath it.
// True on the one thread that holds the page lock as the worker. The fetch threads a load starts hold nothing and
// must not let go of a lock they do not own.
thread_local bool t_lock_owner = false;

// `supersede`: a navigation. It does not wait for the page to finish what it is doing: it makes the load in progress
// give up at its next request and takes its place in the queue. Several in a row leave only the last to run.
bool run_page_work(const std::shared_ptr<WebPane>& pane_sp, int view_w, const std::string& what,
                   std::function<void(WebPane&, int&)> body, bool exclusive = true, bool supersede = false) {
  if (!pane_sp || !pane_sp->browser) return false;
  auto guard = pane_sp->guard;
  auto worker = pane_sp->worker;
  const uint64_t gen = supersede ? guard->nav_gen.fetch_add(1) + 1 : 0;
  auto job = [pane_sp, guard, view_w, body, gen]() {
    int wrote = 0;
    guard->lock_worker();
    t_lock_owner = true;
    if (gen && guard->nav_gen.load() != gen) {
      // A newer navigation is already waiting behind this one: skip it.
      t_lock_owner = false;
      guard->unlock_worker();
      return;
    }
    guard->running_gen.store(gen ? gen : guard->nav_gen.load());
    body(*pane_sp, wrote);
    if (wrote > 0) pane_sp->browser->relayout(pane_metrics(), view_w);
    pane_sp->working.clear();
    t_lock_owner = false;
    guard->unlock_worker();
  };
  // Called from inside the frame (the pictures a page still needs), the lock is already held by this thread.
  const bool own = guard->in_frame || guard->worker;
  {
    // The label goes up before the job starts, so the very next frame says the
    // page is working rather than looking like it ignored the click.
    if (!own) guard->lock_frame();
    pane_sp->working = what;
    if (!own) guard->unlock_frame();
  }
  if (!worker->submit(job, exclusive && !supersede)) {
    if (!own) guard->lock_frame();
    pane_sp->working.clear();
    pane_sp->message = "the page is still working on the last thing";
    if (!own) guard->unlock_frame();
    return false;
  }
  return true;
}

// The focused field, or null if it has gone away since it was focused.
web::Node* live_field(WebPane& pane) {
  if (!pane.field) return nullptr;
  if (!pane.browser || !pane.browser->document().dom) { pane.field = nullptr; return nullptr; }
  if (!node_in_tree(pane.browser->document().dom.get(), pane.field)) {
    pane.field = nullptr;
    return nullptr;
  }
  return pane.field;
}

}  // namespace

PageWorker::~PageWorker() {
  {
    hal::LockGuard g(m);
    stop = true;
  }
  if (thread.joinable()) thread.join();
}

void PageWorker::start() {
  if (thread.joinable()) return;
  PageWorker* self = this;
  thread = hal::Thread([self]() {
    for (;;) {
      std::function<void()> job;
      {
        hal::LockGuard g(self->m);
        if (self->stop) return;
        if (!self->jobs.empty()) {
          job = self->jobs.front().first;
          self->heavy = self->jobs.front().second;
          self->jobs.erase(self->jobs.begin());
          self->running = true;
        }
      }
      if (!job) {
        // Nothing to do. A poll rather than a condition variable: hal has a
        // mutex and a sleep, and 8ms of latency on a click is not felt.
        hal::sleep_ms(8);
        continue;
      }
      job();
      hal::LockGuard g(self->m);
      self->running = false;
      self->heavy = false;
    }
  });
}

bool PageWorker::submit(std::function<void()> job, bool exclusive) {
  start();
  hal::LockGuard g(m);
  // Only a heavy job in flight (or already queued) blocks another one. Typed
  // keys queue behind anything, so a send pressed a moment after the last
  // letter still lands.
  if (exclusive) {
    if (heavy) return false;
    for (const auto& q : jobs)
      if (q.second) return false;
  }
  // A bound on the queue: a held-down key on a page that cannot keep up should
  // lose the overflow rather than grow a queue without end.
  if (jobs.size() >= 64) return false;
  jobs.push_back({std::move(job), exclusive});
  return true;
}

bool PageWorker::busy() {
  hal::LockGuard g(m);
  if (heavy) return true;
  for (const auto& q : jobs)
    if (q.second) return true;
  return false;
}

// The clock a page gets, from the machine's own. Installed the first time a
// pane is drawn, which is before any page runs.
hal::Host* g_clock_host = nullptr;

double pane_wall_ms() {
  if (!g_clock_host) return 1757433600000.0;
  return (double)(g_clock_host->wall_ns() / 1000000ULL);
}

// The element under the pointer (and its ancestors) carry data-aios-hover so `:hover` rules match.
static web::Node* s_hover_node = nullptr;
static const void* s_hover_dom = nullptr;
static uint64_t s_hover_gen = 0, s_hover_ms = 0;
static bool s_hover_dirty = false;
static const void* s_hover_off_dom = nullptr;   // a page whose re-cascade is too slow to do per pointer move
static void set_mark_chain(web::Node* n, const char* attr, bool on) {
  for (; n; n = n->parent) {
    if (n->kind != web::Node::Kind::Element) continue;
    auto& a = n->attrs;
    for (size_t i = 0; i < a.size(); ++i)
      if (a[i].name == attr) { a.erase(a.begin() + (long)i); break; }
    if (on) a.push_back({attr, "1"});
  }
}
static void set_hover_chain(web::Node* n, bool on) { set_mark_chain(n, "data-aios-hover", on); }
// `:active`: the pressed element (and its ancestors) carry data-aios-active from the button going down until it
// comes up, the way the hover chain carries data-aios-hover; `:active` never matched before.
static web::Node* s_active_node = nullptr;
static const void* s_active_dom = nullptr;
// Layout of a big page takes hundreds of milliseconds and rewrites the tree the frame reads, so the frame never does
// it: it asks here, and a worker job (see dispatch_reflow) lays the page out holding the page lock. The frame meanwhile
// try-locks and, finding the worker in there, leaves the pixels of the last frame on screen.
static bool s_reflow_req = false;
static bool s_reflow_hover = false;               // the request came from :hover, whose cost decides whether to stop doing it
static std::atomic<bool> s_reflow_inflight{false};
static uint64_t s_live_cost_ms = 0;               // what the last in-frame layout of a loading page cost
// The pressed element: marked on the button going down, cleared when it comes up.
static void set_active(WebPane& pane, web::Node* n) {
  web::Node* dom = pane.browser ? pane.browser->document().dom.get() : nullptr;
  if (s_active_dom != dom) { s_active_node = nullptr; s_active_dom = dom; }
  if (n == s_active_node) return;
  set_mark_chain(s_active_node, "data-aios-active", false);
  set_mark_chain(n, "data-aios-active", true);
  s_active_node = n;
  s_reflow_req = true;
}
static void clear_active() {
  if (!s_active_node) return;
  set_mark_chain(s_active_node, "data-aios-active", false);
  s_active_node = nullptr;
  s_reflow_req = true;
}
// Called in the frame with the guard held.
static void update_hover(WebPane& pane, int view_w, uint64_t now_ms) {
  if (!s_hover_dirty || !::g_css_has_hover || !pane.working.empty()) return;
  if (now_ms - s_hover_ms < 40) return;
  s_hover_dirty = false;
  web::Node* dom = pane.browser->document().dom.get();
  if (s_hover_off_dom == dom || pane.browser->document().fragments.size() > 1500) return;
  if (s_hover_dom != dom) { s_hover_node = nullptr; s_hover_dom = dom; }
  web::Node* now = pane.last_px >= 0 ? pane.browser->node_at(pane.last_px, pane.last_py) : nullptr;
  if (now == s_hover_node) return;
  s_hover_ms = now_ms;
  set_hover_chain(s_hover_node, false);
  set_hover_chain(now, true);
  s_hover_node = now;
  (void)view_w;
  s_reflow_req = true;
  s_reflow_hover = true;
  ++s_hover_gen;
}

// ---- text selection on the page (drag, or Ctrl+A) -------------------------------------
// One pane per window, so the state is file-local like the hover state above.
static bool s_sel_drag = false, s_sel_all = false, s_sel_any = false;
static int s_sel_ax = 0, s_sel_ay = 0, s_sel_bx = 0, s_sel_by = 0;
static uint64_t s_sel_gen = 0;
static void sel_clear() { if (s_sel_any || s_sel_all) ++s_sel_gen; s_sel_any = s_sel_all = s_sel_drag = false; }
static bool frag_selected(const web::Fragment& f) {
  if (f.kind != web::Fragment::Kind::Text || f.text.empty()) return false;
  if (s_sel_all) return true;
  if (!s_sel_any) return false;
  int sx = s_sel_ax, sy = s_sel_ay, ex = s_sel_bx, ey = s_sel_by;
  if (sy > ey || (sy == ey && sx > ex)) { std::swap(sx, ex); std::swap(sy, ey); }
  bool after_start = (f.y + f.h <= sy) ? false : (sy >= f.y ? (f.x + f.w > sx) : true);
  bool before_end = (f.y > ey) ? false : (ey < f.y + f.h ? (f.x < ex) : true);
  return after_start && before_end;
}
static std::string selected_text(const web::Document& doc) {
  std::string out;
  const web::Fragment* prev = nullptr;
  for (const auto& f : doc.fragments) {
    if (!frag_selected(f)) continue;
    if (prev) {
      if (std::abs(f.y - prev->y) <= prev->h / 2 + 1) { if (f.x - (prev->x + prev->w) > 2) out += ' '; }
      else out += (f.y - prev->y > prev->h * 2) ? "\n\n" : "\n";
    }
    out += f.text;
    prev = &f;
  }
  return out;
}

// ---- page zoom and find-in-page -------------------------------------------------------
// Zoom is a REAL relayout: the page is laid out at the viewport's width in CSS pixels (view * 100 / zoom), painted at 1:1 into a
// scratch raster and sampled onto the pane, so 200% gives a 700px-wide layout on a 1400px window and the text reflows, as in Chrome.
static int s_zoom = 100;
static bool s_mod_shift = false, s_mod_alt = false;
static int zoom_px(int real) { return s_zoom == 100 ? real : real * 100 / s_zoom; }
static const int kZoomSteps[] = {25, 33, 50, 67, 75, 80, 90, 100, 110, 125, 150, 175, 200, 250, 300, 400, 500};
static void zoom_step(int dir) {
  const int n = (int)(sizeof(kZoomSteps) / sizeof(kZoomSteps[0]));
  int at = 0;
  for (int i = 0; i < n; ++i) if (kZoomSteps[i] <= s_zoom) at = i;
  if (dir > 0 && at + 1 < n) ++at;
  else if (dir < 0 && kZoomSteps[at] == s_zoom && at > 0) --at;
  s_zoom = kZoomSteps[at];
}

// Find in page: the matches are looked for in the laid-out text fragments each time they are needed (a page is a few thousand
// fragments), so there is no index to keep in step with a relayout or a navigation.
static bool s_find_open = false;
static std::string s_find_text;
static int s_find_cur = 0;
static uint64_t s_find_gen = 0;
struct FindHit { size_t frag, off, len; };
static uint64_t s_cached_find_gen = 0;
static std::vector<FindHit> s_cached_find_marks;
static std::string ascii_lower(const std::string& in) {
  std::string o = in;
  for (char& ch : o) if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
  return o;
}
static std::vector<FindHit> find_hits(const web::Document& doc) {
  std::vector<FindHit> hits;
  if (s_find_text.empty()) return hits;
  const std::string needle = ascii_lower(s_find_text);
  for (size_t i = 0; i < doc.fragments.size() && hits.size() < 4000; ++i) {
    const web::Fragment& f = doc.fragments[i];
    if (f.kind != web::Fragment::Kind::Text || f.text.empty()) continue;
    const std::string hay = ascii_lower(f.text);
    for (size_t at = hay.find(needle); at != std::string::npos && hits.size() < 4000; at = hay.find(needle, at + needle.size()))
      hits.push_back({i, at, needle.size()});
  }
  return hits;
}
// Puts match `s_find_cur` a third of the way down the viewport and says which one it is.
static void find_reveal(WebPane& pane, int view_h) {
  ++s_find_gen;
  if (!pane.browser) return;
  const web::Document& doc = pane.browser->document();
  const auto hits = find_hits(doc);
  pane.error = false;
  if (s_find_text.empty()) { pane.message = "find: type to search the page"; return; }
  if (hits.empty()) { pane.message = "find: no match for \"" + s_find_text + "\""; pane.error = true; return; }
  s_find_cur = ((s_find_cur % (int)hits.size()) + (int)hits.size()) % (int)hits.size();
  const web::Fragment& f = doc.fragments[hits[(size_t)s_find_cur].frag];
  pane.scroll = std::max(0, f.y - view_h / 3);
  pane.message = "find: " + std::to_string(s_find_cur + 1) + " of " + std::to_string(hits.size()) + " for \"" + s_find_text + "\"";
}

web::Metrics pane_metrics() {
  web::Metrics m;
  m.measure = [](const std::string& s, const web::FontKey& f) { return web_text_width(s, f); };
  m.line_height = [](const web::FontKey& f) { return web_line_height(f); };
  m.ascent = [](const web::FontKey& f) { return web_ascent(f); };
  return m;
}

// ------------------------------------------------------------------ bytes --

web::Fetch kernel_fetch(Kernel& k, std::shared_ptr<PageGuard> guard) {
  Kernel* kp = &k;
  return [kp, guard](const std::string& url, const web::FetchOptions& opt) -> Result<web::FetchResult> {
    // AIOS_FETCH_DEBUG=1 names every request and how long it took, which is how
    // a slow page load is told apart from a slow page.
    static const bool debug = std::getenv("AIOS_FETCH_DEBUG") != nullptr;
    const uint64_t t0 = debug ? kp->host().monotonic_ns() : 0;
    struct Report {
      bool on; uint64_t t0; const std::string& url; Kernel* k;
      ~Report() { if (on) std::fprintf(stderr, "fetch %6.0f ms  %.110s\n", (double)(k->host().monotonic_ns() - t0) / 1e6, url.c_str()); }
    } report{debug, t0, url, kp};
    // A newer navigation than the one running: end this request now, so a load nobody wants any more stops at its
    // next step instead of finishing its twenty.
    const auto cancelled = [&]() { return guard && guard->running_gen.load() != guard->nav_gen.load(); };
    if (cancelled()) return Result<web::FetchResult>(Status::Busy, "cancelled: a newer page was asked for");
    std::string current = url;
    // A surface this machine made lives under its own root: read it there.
    // file:///path, with the empty authority a local path has.
    if (lowered(current).compare(0, 7, "file://") == 0) {
      std::string path = current.substr(7);
      const size_t q = path.find_first_of("?#");
      if (q != std::string::npos) path = path.substr(0, q);
      auto f = kp->host().read_file(path);
      if (!f.ok()) return Result<web::FetchResult>(Status::NotFound, path + ": " + f.error().message);
      web::FetchResult out;
      out.status = 200;
      out.final_url = current;
      out.transport = "this machine's own root";
      out.body.assign((const char*)f->data(), f->size());
      const std::string low = lowered(path);
      out.content_type = low.size() > 4 && low.compare(low.size() - 4, 4, ".css") == 0 ? "text/css"
                       : low.size() > 3 && low.compare(low.size() - 3, 3, ".js") == 0 ? "text/javascript"
                       : low.size() > 4 && low.compare(low.size() - 4, 4, ".png") == 0 ? "image/png"
                       : "text/html";
      return out;
    }
    // Redirects, followed here rather than in the engine: the engine should
    // not have to know that 301 is a thing.
    for (int hop = 0; hop < 6; ++hop) {
      auto ep = channel::parse_endpoint(current);
      if (!ep.ok()) return Result<web::FetchResult>(ep.error());
      channel::HttpRequest req;
      req.endpoint = *ep;
      // A page's own request, not a page load: an application asking its agent
      // a question waits for the answer, and 20 seconds is not that.
      req.timeout_ms = opt.method == "GET" || opt.method.empty() ? 20000 : 300000;
      if (!opt.method.empty()) req.method = opt.method;
      // The body only. The http client writes Content-Length itself, and
      // sending a second one is a malformed request: every POST this pane made
      // came back 400 Bad Request, so an application could show its interface
      // and never send anything from it.
      if (!opt.body.empty()) req.body.assign(opt.body.begin(), opt.body.end());
      for (const auto& hdr : opt.headers) req.headers.push_back(hdr);
      req.host = &kp->host();
      req.kernel_root = kp->config().root;
      req.headers.push_back({"Accept", "text/html,application/xhtml+xml,text/css,image/png,image/gif,*/*"});
      // Ask for gzip. This used to ask for `identity` on the grounds that the
      // engine had no gzip in its transport -- which the decode below, using
      // the inflater git already ships, had made untrue: it was there to catch
      // the servers that send gzip whether or not you asked. Asking for the
      // uncompressed copy of every page on the web cost three to five times the
      // bytes over a link this machine has to bring up itself. `deflate` is not
      // requested: gunzip() reads gzip framing, and a bare zlib stream under
      // that name would be misparsed.
      req.headers.push_back({"Accept-Encoding", "gzip"});
      // A CONVENTIONAL User-Agent, which is a compatibility fiction every
      // browser tells for exactly this reason. Announcing "aios/1" got this
      // engine treated as a robot: google.com answered a search with a page
      // whose entire body is `<div style="display:none">If you are having
      // trouble accessing Google Search...</div>`, revealed after two seconds
      // by a script. Zero boxes, a white page, and nothing wrong with the
      // renderer. The same request with a Chrome-shaped string comes back with
      // the results. Chrome's own string claims to be Mozilla, KHTML and
      // Safari; this one claims no more than it has to.
#if defined(__aarch64__)
      const char* kUserAgent =
          "Mozilla/5.0 (X11; Linux aarch64) AppleWebKit/537.36 "
          "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";
#else
      const char* kUserAgent =
          "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
          "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";
#endif
      req.headers.push_back({"User-Agent", kUserAgent});
      req.headers.push_back({"Accept",
                             "text/html,application/xhtml+xml,application/xml;q=0.9,"
                             "image/avif,image/webp,*/*;q=0.8"});
      req.headers.push_back({"Accept-Language", "en-US,en;q=0.9"});
      channel::HttpTransportNote note;
      // The DOM is not being touched while this is in flight, and this is
      // where a slow application spends its minute -- so the frame gets to
      // draw. Held again before anything is returned to the interpreter.
      if (cancelled()) return Result<web::FetchResult>(Status::Busy, "cancelled: a newer page was asked for");
      const bool released = t_lock_owner && guard && guard->release_for_io();
      auto r = channel::http_send_noted(req, &note);
      if (released) guard->reacquire();
      if (!r.ok()) return Result<web::FetchResult>(r.error());

      if (r->status == 301 || r->status == 302 || r->status == 303 || r->status == 307 || r->status == 308) {
        std::string to = r->header("Location");
        if (to.empty()) return Result<web::FetchResult>(Status::NotFound, current + ": redirected to nowhere");
        std::string next = web::resolve_url(current, to);
        if (next.empty() || next == current)
          return Result<web::FetchResult>(Status::NotFound, current + ": redirect loop");
        current = next;
        continue;
      }

      web::FetchResult out;
      out.status = r->status;
      out.final_url = current;
      out.content_type = r->header("Content-Type");
      out.transport = note.tls_version.empty() ? std::string("cleartext")
                                               : note.tls_version + " " + note.cipher;
      out.body.assign((const char*)r->body.data(), r->body.size());
      std::string enc = lowered(r->header("Content-Encoding"));
      if (enc.find("gzip") != std::string::npos || enc.find("deflate") != std::string::npos) {
        std::string plain;
        if (gunzip(out.body, plain)) {
          out.body = std::move(plain);
        } else if (!out.body.empty()) {
          // Now that gzip is what we ASK for, a body that will not inflate is a
          // failure to report, not a body to hand the parser. Handing the
          // tokeniser a deflate stream produces a page of nothing and no
          // reason for it anywhere.
          return Result<web::FetchResult>(Status::Invalid,
                                          current + ": Content-Encoding said " + enc + " and the body would not inflate");
        }
      }
      if (r->status != 200 && r->status != 203 && r->status != 206) {
        // The body of an error page is still a page, and showing it is more
        // use than showing the number alone.
        if (out.body.empty())
          return Result<web::FetchResult>(Status::NotFound,
                                          current + " answered " + dec((uint64_t)r->status) + " " + r->reason);
      }
      return out;
    }
    return Result<web::FetchResult>(Status::NotFound, url + ": too many redirects");
  };
}

// --------------------------------------------------- the machine, for a page --

std::vector<float> text_embedding(const std::string& text) {
  // Hashed byte trigrams into 64 dimensions, L2-normalised. Deterministic and
  // model-free: what it buys is that the same words land near each other, which
  // is enough for a scanner's items to be found by an ERP asking for "pallet".
  std::vector<float> v(64, 0.f);
  std::string t;
  t.reserve(text.size() + 4);
  for (char c : text) t.push_back((c >= 'A' && c <= 'Z') ? (char)(c + 32) : c);
  t = "  " + t + "  ";
  for (size_t i = 0; i + 3 <= t.size(); ++i) {
    uint64_t h = 1469598103934665603ull;
    for (size_t j = 0; j < 3; ++j) { h ^= (uint8_t)t[i + j]; h *= 1099511628211ull; }
    const size_t dim = h % 64;
    const float sign = ((h >> 7) & 1) ? 1.f : -1.f;
    v[dim] += sign;
  }
  double n = 0;
  for (float f : v) n += double(f) * f;
  n = std::sqrt(n);
  if (n > 0) for (auto& f : v) f = (float)(f / n);
  return v;
}

bool is_face_url(const std::string& url) { return url.compare(0, 7, "face://") == 0; }

std::string face_source_path(const std::string& root, const std::string& url) {
  if (!is_face_url(url)) return {};
  std::string rest = url.substr(7);
  const size_t q = rest.find('?');
  if (q != std::string::npos) rest.erase(q);
  if (!rest.empty() && rest[0] == '/') return rest;          // face:///path/to/face.aos
  const size_t slash = rest.find('/');
  if (slash != std::string::npos) rest.erase(slash);
  return root + "/dreams/" + rest + "/face.aos";             // face://<dream>
}

std::string dream_of_url(const std::string& url) {
  if (is_face_url(url)) {
    std::string rest = url.substr(7);
    const size_t q = rest.find('?');
    if (q != std::string::npos) rest.erase(q);
    if (!rest.empty() && rest[0] != '/') { const size_t slash = rest.find('/'); return slash == std::string::npos ? rest : rest.substr(0, slash); }
  }
  const size_t at = url.find("/dreams/");
  if (at == std::string::npos) return {};
  const size_t start = at + 8;
  const size_t end = url.find('/', start);
  if (end == std::string::npos) return {};
  return url.substr(start, end - start);
}

namespace {

std::string json_str(const std::string& s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c == '\n') o += "\\n";
    else if (c == '\r') o += "\\r";
    else if (c == '\t') o += "\\t";
    else if ((unsigned char)c < 32) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", (unsigned)(unsigned char)c); o += b; }
    else o += c;
  }
  return o + "\"";
}

// One stored record as the object a page gets back: the payload's own fields
// with the machine's facts about it in front, so a page can show "_t" and
// call "_id" without knowing how the store is laid out.
std::string record_json(const StoredObject& o, float score = -1.f) {
  std::string head = "{\"_id\":" + json_str(o.id.str()) +
                     ",\"_t\":" + std::to_string(o.timestamp_ns / 1000000ull) +
                     ",\"_kind\":" + json_str(o.meta("kind")) +
                     ",\"_dream\":" + json_str(o.meta("dream"));
  if (!o.meta("dream").empty()) {
    head += ",\"dream\":" + json_str(o.meta("dream"));
    if (o.meta("name").empty()) head += ",\"name\":" + json_str(o.meta("dream"));
  }
  if (score >= 0.f) { char b[32]; std::snprintf(b, sizeof b, ",\"_score\":%.3f", (double)score); head += b; }
  // What the machine itself knows about the record. A face used to see only the
  // payload, so every record the MACHINE keeps — whose title, url, source and
  // score are metadata, not payload — arrived as a blob of text with no fields
  // on it: `{title}` came out blank however right the face was. The payload
  // wins where they collide, because that is the record's own body.
  std::string meta;
  for (const auto& [key, value] : o.metadata) {
    if (key.empty() || key == "kind" || key == "dream" || key == "made") continue;
    if (key[0] == '_') continue;
    meta += "," + json_str(key) + ":" + json_str(value);
  }
  std::string body(o.payload.begin(), o.payload.end());
  size_t a = body.find_first_not_of(" \t\r\n");
  size_t z = body.find_last_not_of(" \t\r\n");
  if (a != std::string::npos && body[a] == '{' && body[z] == '}') {
    const std::string inner = body.substr(a + 1, z - a - 1);
    if (inner.find_first_not_of(" \t\r\n") == std::string::npos) return head + meta + "}";
    return head + meta + "," + inner + "}";
  }
  // Not an object: what was stored, as text.
  return head + meta + ",\"text\":" + json_str(body) + "}";
}

std::string embed_text_of(const std::string& kind, const std::string& json) {
  // The words in the record, not its punctuation: quotes, braces and colons
  // carry no meaning and would dilute the trigrams that do.
  std::string t = kind + " ";
  for (char c : json) t += (c == '"' || c == '{' || c == '}' || c == '[' || c == ']' || c == ':' || c == ',') ? ' ' : c;
  return t;
}

}  // namespace

HostBindings surface_bindings(Kernel& k, Surface who, const std::string& dream) {
  HostBindings h;
  Kernel* kp = &k;
  // The Surface goes in by value and is shared by the closures: an act_as for
  // the dream's entity, or the console's own, decided by whoever bound us.
  auto actor = std::make_shared<Surface>(who);
  const std::string storage_key = dream;

  h.store_put = [actor, dream](const std::string& kind, const std::string& json) -> std::string {
    StoredObject o;
    o.payload = ByteBuffer(json.begin(), json.end());
    o.embedding = text_embedding(embed_text_of(kind, json));
    o.metadata.emplace_back("kind", kind);
    if (!dream.empty()) o.metadata.emplace_back("dream", dream);
    o.metadata.emplace_back("made", "surface");
    auto id = actor->store(std::move(o));
    if (!id.ok()) { std::fprintf(stderr, "aos.store.put refused: %s\n", id.error().str().c_str()); return std::string(); }
    return id->str();
  };
  h.store_revision = [kp]() -> uint64_t { return (uint64_t)kp->store().count(); };
  h.store_list = [kp](const std::string& kind) -> std::string {
    if (kind == "kind" || kind == "kinds") {
      std::map<std::string, size_t> counts;
      for (const auto& id : kp->store().list()) {
        auto o = kp->store().get(id);
        if (!o.ok()) continue;
        const std::string obj_kind = o->meta("kind");
        if (!obj_kind.empty()) counts[obj_kind]++;
      }
      std::string out = "[";
      bool first = true;
      for (const auto& [kn, cnt] : counts) {
        if (!first) out += ",";
        first = false;
        out += "{\"kind\":\"" + kn + "\",\"count\":" + std::to_string(cnt) + "}";
      }
      out += "]";
      return out;
    }
    std::vector<StoredObject> rows;
    for (const auto& id : kp->store().find_meta("kind", kind)) {
      auto o = kp->store().get(id);
      if (!o.ok()) continue;
      // EVERY record of that kind, not only the ones a face wrote. This used to
      // require `made=surface`, which meant a face could see nothing but its own
      // output: a dream about this machine's horizon read `signal` and got zero
      // rows on a machine holding thirty-nine of them, laid itself out
      // beautifully around nothing, and shipped. The store is ONE store with one
      // kind namespace — that is the whole point of records being kernel objects
      // — so a face reads what is there.
      rows.push_back(std::move(*o));
    }
    std::sort(rows.begin(), rows.end(), [](const StoredObject& a, const StoredObject& b) { return a.timestamp_ns > b.timestamp_ns; });
    if (rows.size() > 500) rows.resize(500);
    std::string out = "[";
    for (size_t i = 0; i < rows.size(); ++i) { if (i) out += ","; out += record_json(rows[i]); }
    return out + "]";
  };
  h.store_get = [kp](const std::string& id_text) -> std::string {
    Id128 raw;
    if (!Id128::parse(id_text, raw)) return std::string();
    auto o = kp->store().get(ObjectID{raw});
    if (!o.ok()) return std::string();
    return record_json(*o);
  };
  h.store_similar = [kp](const std::string& text, int want) -> std::string {
    auto hits = kp->store().find_similar(text_embedding(text), (size_t)want * 4 + 8);
    std::string out = "[";
    int n = 0;
    for (const auto& hit : hits) {
      if (n >= want) break;
      auto o = kp->store().get(hit.id);
      if (!o.ok()) continue;   // every record, like store_list: one store, one namespace
      if (n) out += ",";
      out += record_json(*o, hit.score);
      ++n;
    }
    return out + "]";
  };
  h.store_query = [actor](const std::string& qstr) -> std::string {
    auto res = aios::storage::execute_aql(*actor, qstr);
    std::string out = "[";
    for (size_t i = 0; i < res.objects.size(); ++i) {
      if (i) out += ",";
      out += record_json(res.objects[i]);
    }
    return out + "]";
  };
  h.organ = [kp](const std::string& name, const std::string& request) -> std::string {
    auto r = organ_call(kp->config().root, name, request);
    if (!r.ok()) return "error: " + r.error().message;
    return *r;
  };
  h.storage_load = [kp, storage_key]() -> std::string {
    if (storage_key.empty()) return std::string();
    std::string newest_text;
    uint64_t newest = 0;
    for (const auto& id : kp->store().find_meta("kind", "dream-storage")) {
      auto o = kp->store().get(id);
      if (!o.ok() || o->meta("dream") != storage_key) continue;
      if (o->timestamp_ns >= newest) { newest = o->timestamp_ns; newest_text.assign(o->payload.begin(), o->payload.end()); }
    }
    return newest_text;
  };
  h.storage_save = [actor, storage_key](const std::string& text) {
    if (storage_key.empty()) return;
    StoredObject o;
    o.payload = ByteBuffer(text.begin(), text.end());
    o.metadata.emplace_back("kind", "dream-storage");
    o.metadata.emplace_back("dream", storage_key);
    auto id = actor->store(std::move(o));
    if (!id.ok()) std::fprintf(stderr, "localStorage for %s not kept: %s\n", storage_key.c_str(), id.error().str().c_str());
  };
  h.machine = [kp](const std::string& key) -> std::string {
    auto& host = kp->host();
    const auto& topo = host.topology();
    if (key == "cpus") return std::to_string(topo.logical_cpus);
    if (key == "arch") return topo.arch;
    if (key == "host") return topo.host_name;
    if (key == "mem_total") return std::to_string(topo.total_memory);
    if (key == "mem_free") return std::to_string(host.available_bytes());
    if (key == "uptime_s") return std::to_string(host.monotonic_ns() / 1000000000ull);
    if (key == "caps") return std::to_string(kp->capabilities().list({}).size());
    if (key == "records") return std::to_string(kp->store().count());
    return std::string();
  };
  h.now_ms = [kp]() -> double { return (double)(kp->host().wall_ns() / 1000000ull); };
  h.random_bytes = [kp](uint8_t* out, size_t n) { kp->host().random(MutableByteSpan(out, n)); };
  h.say = [dream](const std::string& s) {
    std::fprintf(stderr, "%s says: %s\n", dream.empty() ? "page" : dream.c_str(), s.c_str());
  };
  h.read_file = [kp](const std::string& path) -> std::string {
    auto f = kp->host().read_file(path);
    if (!f.ok() && !path.empty() && path[0] != '/') {
      std::string p = kp->config().root + "/" + path;
      f = kp->host().read_file(p);
    }
    if (!f.ok()) return "";
    return std::string((const char*)f->data(), f->size());
  };
  h.write_file = [kp](const std::string& path, const std::string& data) -> bool {
    auto res = kp->host().write_file(path, ByteSpan((const uint8_t*)data.data(), data.size()));
    if (!res.ok() && !path.empty() && path[0] != '/') {
      std::string p = kp->config().root + "/" + path;
      size_t last_slash = p.rfind('/');
      if (last_slash != std::string::npos) (void)kp->host().make_dir(p.substr(0, last_slash));
      res = kp->host().write_file(p, ByteSpan((const uint8_t*)data.data(), data.size()));
    }
    return res.ok();
  };
  h.list_dir = [kp](const std::string& dir) -> std::vector<std::string> {
    auto res = kp->host().list_dir(dir);
    if (!res.ok() && !dir.empty() && dir[0] != '/') {
      std::string p = kp->config().root + "/" + dir;
      res = kp->host().list_dir(p);
    }
    if (!res.ok()) return {};
    return *res;
  };
  h.http_get = [kp](const std::string& url) -> std::string {
    if (url.rfind("https://", 0) == 0) {
      auto res = channel::http_get_secure(url, kp->host(), kp->config().root);
      if (res.ok() && res->status >= 200 && res->status < 300) {
        return res->text();
      }
      return "";
    }
    auto res = channel::http_get(url);
    if (res.ok() && res->status >= 200 && res->status < 300) {
      return res->text();
    }
    return "";
  };
  return h;
}

// The machine behind whatever is on the pane. A dream acts as its OWN entity,
// so its records carry its provenance and are bounded by its authority — not
// the console's. Anything else gets the machine as the console, its storage
// keyed by where it came from.
static bool pane_bindings(HardwareCompositor& c, Kernel& k, const std::string& dream, const std::string& url, HostBindings& out) {
  Surface* console = c.console();
  if (!console) return false;
  std::shared_ptr<Surface> who = std::make_shared<Surface>(*console);
  if (!dream.empty()) {
    for (const auto& e : k.entities().list()) {
      if (e.label != dream) continue;
      auto as = console->act_as(e.id);
      if (as.ok()) who = std::make_shared<Surface>(*as);
      break;
    }
  }
  out = surface_bindings(k, *who, dream.empty() ? url : dream);
  return true;
}

void web_pane_bind(HardwareCompositor& c, WebPane& pane, Kernel& k, const std::string& url, int view_h) {
  if (!pane.browser) pane.browser = std::make_shared<web::Browser>(kernel_fetch(k));
  pane.dream = dream_of_url(url);
  pane.field = nullptr;
  pane.held.clear();
  pane.browser->set_view_height(view_h);
  HostBindings h;
  if (pane_bindings(c, k, pane.dream, url, h)) pane.browser->set_host(std::move(h));
}

// Put a face on the pane: read its source, understand it, bind it to the
// machine as the dream it is, restore its state, run `on load`. From here on
// the frame lays it out and paints it; there is no page.
static void web_pane_open_face(HardwareCompositor& c, WindowSurface& win, WebPane& pane, Kernel& k, const std::string& url) {
  pane.face.reset();
  pane.face_url = url;
  pane.face_said.clear();
  pane.url_text = url;
  pane.scroll = 0;
  pane.hover_link = -1;
  pane.url_focus = false;
  pane.last_tick_ns = 0;
  pane.field = nullptr;
  pane.held.clear();
  pane.dream = dream_of_url(url);
  const std::string path = face_source_path(k.config().root, url);
  auto f = k.host().read_file(path);
  if (!f.ok()) {
    pane.message = "no face at " + path;
    pane.error = true;
    win.title = "no face";
    return;
  }
  const std::string source((const char*)f->data(), f->size());
  auto rt = std::make_shared<face::Runtime>(face::parse_face(source));
  HostBindings h;
  if (pane_bindings(c, k, pane.dream, url, h)) rt->bind(std::move(h));
  rt->load();
  pane.face = rt;
  pane.message.clear();
  pane.error = false;
  const face::Face& face = rt->face();
  win.title = !face.title.empty() ? face.title : !face.name.empty() ? face.name : std::string("a face");
  if (win.title.size() > 48) win.title = win.title.substr(0, 45) + "...";
  if (!face.errors.empty()) { pane.message = face.errors[0]; pane.error = true; }
}

static bool node_takes_text(web::Node* n) {
  if (!n || n->kind != web::Node::Kind::Element) return false;
  if (n->tag == "textarea") return true;
  if (n->tag != "input") return false;
  std::string type = lowered(n->attr("type"));
  return type.empty() || type == "text" || type == "search" || type == "number" || type == "password" ||
         type == "email" || type == "tel" || type == "url";
}


// ------------------------------------------------------------- the chrome --

namespace {

struct Chrome {
  int back_x, fwd_x, reload_x, btn_y, btn_w, btn_h;
  int field_x, field_y, field_w, field_h;
};

Chrome chrome_geom(int w) {
  Chrome g{};
  g.btn_w = 26;
  g.btn_h = 22;
  g.btn_y = (kChromeH - g.btn_h) / 2;
  g.back_x = 8;
  g.fwd_x = g.back_x + g.btn_w + 3;
  g.reload_x = g.fwd_x + g.btn_w + 3;
  g.field_x = g.reload_x + g.btn_w + 8;
  g.field_y = g.btn_y;
  g.field_h = g.btn_h;
  g.field_w = w - g.field_x - 10;
  if (g.field_w < 60) g.field_w = 60;
  return g;
}

// A solid triangle, for the two history buttons. The atlas carries real arrows
// now, but a drawn triangle scales with the chrome and sits on the pixel grid,
// which a glyph at a fixed cut does not.
void draw_triangle(HardwareCompositor& c, std::vector<uint32_t>& buf, int bw, int bh, int cx, int cy, int size,
                   bool left, uint32_t colour) {
  // Walk from the TIP to the base, widening as it goes. Sweeping dy from -size
  // to +size and narrowing towards both ends draws a diamond, which is what
  // this used to put in the two history buttons.
  const int wide = size;              // tip to base
  const int tall = size * 3 / 2;      // base height, halved below
  for (int i = 0; i <= wide; ++i) {
    int px = left ? (cx - wide / 2 + i) : (cx + wide / 2 - i);
    int half = i * tall / (wide * 2);
    for (int dy = -half; dy <= half; ++dy) c.blend_to(buf, bw, bh, px, cy + dy, colour, 255);
  }
}

// The reload glyph: three quarters of a ring with a nib. Same reason.
void draw_reload(HardwareCompositor& c, std::vector<uint32_t>& buf, int bw, int bh, int cx, int cy, int r,
                 uint32_t colour) {
  for (int dy = -r - 1; dy <= r + 1; ++dy) {
    for (int dx = -r - 1; dx <= r + 1; ++dx) {
      int d2 = dx * dx + dy * dy;
      if (d2 > (r + 1) * (r + 1) || d2 < (r - 1) * (r - 1)) continue;
      if (dx > 0 && dy < 0) continue;   // the gap the nib sits in
      c.blend_to(buf, bw, bh, cx + dx, cy + dy, colour, 255);
    }
  }
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4 - i; ++j) c.blend_to(buf, bw, bh, cx + r - 1 + j, cy - r + i, colour, 255);
}

}  // namespace

// A frame counter, only for the one-character spinner beside "working".
unsigned spin_tick() {
  static unsigned t = 0;
  return ++t;
}

// What a page painted with aos.paint, onto the viewport, clipped to it. The
// coordinates are the page's: origin at the viewport's top-left, unscrolled.
static void draw_paint_ops(HardwareCompositor& c, WindowSurface& win, int w, int h, int view_y, int view_w, int view_h,
                           const std::vector<PaintOp>& ops) {
  auto put = [&](int x, int y, uint32_t color) {
    if (x < 0 || x >= view_w || y < 0 || y >= view_h) return;
    const int px = x, py = view_y + y;
    if (px < 0 || px >= w || py < 0 || py >= h) return;
    const int a = (int)((color >> 24) & 0xFF);
    if (a == 0) return;
    if (a == 255) win.buffer[(size_t)py * (size_t)w + (size_t)px] = color;
    else c.blend_srgb_to(win.buffer, w, h, px, py, color, a);
  };
  auto fill = [&](int x, int y, int rw, int rh, uint32_t color) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + rw > view_w ? view_w : x + rw, y1 = y + rh > view_h ? view_h : y + rh;
    if (x1 <= x0 || y1 <= y0) return;
    const int a = (int)((color >> 24) & 0xFF);
    if (a == 255) c.draw_rect_to(win.buffer, w, h, x0, view_y + y0, x1 - x0, y1 - y0, color);
    else for (int yy = y0; yy < y1; ++yy) for (int xx = x0; xx < x1; ++xx) put(xx, yy, color);
  };
  for (const auto& op : ops) {
    switch (op.op) {
      case PaintOp::Clear: fill(0, 0, view_w, view_h, op.color); break;
      case PaintOp::Rect: fill((int)std::lround(op.a), (int)std::lround(op.b), (int)std::lround(op.c), (int)std::lround(op.d), op.color); break;
      case PaintOp::Outline: {
        const int x = (int)std::lround(op.a), y = (int)std::lround(op.b), rw = (int)std::lround(op.c), rh = (int)std::lround(op.d);
        fill(x, y, rw, 1, op.color); fill(x, y + rh - 1, rw, 1, op.color);
        fill(x, y, 1, rh, op.color); fill(x + rw - 1, y, 1, rh, op.color);
        break;
      }
      case PaintOp::Text: {
        int px = (int)std::lround(op.c);
        if (px < 8) px = 8;
        if (px > 96) px = 96;
        web::FontKey f{(float)px, px >= 20, false, false};
        const int y = (int)std::lround(op.b);
        if (y + px < 0 || y > view_h) break;
        web_draw_text(c, win.buffer, w, h, (int)std::lround(op.a), view_y + y, op.text, op.color, f);
        break;
      }
      case PaintOp::Line: {
        int x0 = (int)std::lround(op.a), y0 = (int)std::lround(op.b), x1 = (int)std::lround(op.c), y1 = (int)std::lround(op.d);
        const int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
        const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
        int err = dx + dy;
        for (int n = 0; n < 8192; ++n) {
          put(x0, y0, op.color);
          if (x0 == x1 && y0 == y1) break;
          const int e2 = 2 * err;
          if (e2 >= dy) { err += dy; x0 += sx; }
          if (e2 <= dx) { err += dx; y0 += sy; }
        }
        break;
      }
      case PaintOp::Circle: {
        const int cx = (int)std::lround(op.a), cy = (int)std::lround(op.b), r = (int)std::lround(op.c);
        if (r <= 0 || r > 4096) break;
        for (int yy = -r; yy <= r; ++yy) {
          const int half = (int)std::lround(std::sqrt((double)r * r - (double)yy * yy));
          fill(cx - half, cy + yy, 2 * half + 1, 1, op.color);
        }
        break;
      }
    }
  }
}

// What the chrome shows, whatever is on the pane.
struct ChromeState {
  std::string url;
  bool secure = false, can_back = false, can_fwd = false, loaded = false;
  std::string status;
  uint32_t status_ink = kFaint;
};

static void draw_pane_chrome(HardwareCompositor& c, WindowSurface& win, const WebPane& pane, int w, int h, const ChromeState& st) {
  // -- the chrome ----------------------------------------------------------
  c.draw_rect_to(win.buffer, w, h, 0, 0, w, kChromeH, kChrome);
  c.draw_rect_to(win.buffer, w, h, 0, kChromeH - 1, w, 1, kChromeEdge);
  Chrome g = chrome_geom(w);

  draw_triangle(c, win.buffer, w, h, g.back_x + g.btn_w / 2, g.btn_y + g.btn_h / 2, 6, true,
                st.can_back ? kInk : 0xFF4A4A4A);
  draw_triangle(c, win.buffer, w, h, g.fwd_x + g.btn_w / 2, g.btn_y + g.btn_h / 2, 6, false,
                st.can_fwd ? kInk : 0xFF4A4A4A);
  draw_reload(c, win.buffer, w, h, g.reload_x + g.btn_w / 2, g.btn_y + g.btn_h / 2, 7,
              st.loaded ? kInk : 0xFF4A4A4A);

  c.draw_rounded_rect_to(win.buffer, w, h, g.field_x, g.field_y, g.field_w, g.field_h, 4, kField);
  c.draw_round_outline_to(win.buffer, w, h, g.field_x, g.field_y, g.field_w, g.field_h, 4,
                          pane.url_focus ? kFieldFocus : kFieldEdge);

  int text_x = g.field_x + 9;
  if (!pane.url_focus && st.secure) {
    // The lock is drawn, and it is drawn from what the transport actually
    // negotiated — the note the TLS client handed back — not from the fact
    // that the url started with https.
    c.draw_rect_to(win.buffer, w, h, text_x, g.field_y + 8, 6, 6, kLockOk);
    c.draw_rect_outline_to(win.buffer, w, h, text_x + 1, g.field_y + 5, 4, 4, kLockOk);
    text_x += 13;
  }
  web::FontKey field_font{13, false, false, false};
  std::string shown = pane.url_focus ? pane.url_text : st.url;
  if (shown.empty() && !pane.url_focus) shown = "type a url";
  int room = g.field_w - (text_x - g.field_x) - 12;
  while (web_text_width(shown, field_font) > room && shown.size() > 4) shown.erase(shown.size() - 1);
  web_draw_text(c, win.buffer, w, h, text_x, g.field_y + 4, shown,
                pane.url_focus || !st.url.empty() ? kInk : kFaint, field_font);
  if (pane.url_focus) {
    int caret = text_x + web_text_width(shown, field_font) + 1;
    c.draw_rect_to(win.buffer, w, h, caret, g.field_y + 4, 1, 15, kFieldFocus);
  }

  // -- the status strip ----------------------------------------------------
  int sy = h - kStatusH;
  c.draw_rect_to(win.buffer, w, h, 0, sy, w, kStatusH, kChrome);
  c.draw_rect_to(win.buffer, w, h, 0, sy, w, 1, kChromeEdge);
  web::FontKey status_font{11, false, false, false};
  std::string status = st.status;
  while (web_text_width(status, status_font) > w - 16 && status.size() > 8) status.erase(status.size() - 1);
  web_draw_text(c, win.buffer, w, h, 8, sy + 4, status, st.status_ink, status_font);
}

// A face on the pane: its own time, its own layout, its own paint. Nothing
// from the web engine is drawn between the chrome and the status strip.
static void render_face_pane(HardwareCompositor& c, WindowSurface& win, Kernel& k, WebPane& pane, int w, int h,
                             int view_y, int view_w, int view_h) {
  face::Runtime& f = *pane.face;
  const uint64_t now_ns = k.host().monotonic_ns();
  const uint64_t now_ms = now_ns / 1000000ull;
  // Keys the hosted input path can only press are released a moment after.
  for (auto it = pane.held.begin(); it != pane.held.end();) {
    if (now_ms > it->second && now_ms - it->second > 160) {
      f.key(it->first, false);
      it = pane.held.erase(it);
    } else ++it;
  }
  const double dt = pane.last_tick_ns ? (double)(now_ns - pane.last_tick_ns) / 1e6 : 16.0;
  pane.last_tick_ns = now_ns;
  f.frame(dt, view_w + kScrollW, view_h);
  if (f.storage_dirty() && now_ms - pane.last_flush_ms > 1200) {
    f.flush_storage();
    pane.last_flush_ms = now_ms;
  }
  c.clear_buffer(win.buffer, 0xFF0F1318, w, h);
  f.paint(win.buffer, w, h, 0, view_y);

  // What happened, for the status strip: a fault stays until the face says
  // something newer.
  for (const auto& fault : f.take_faults()) { pane.message = fault; pane.error = true; }
  if (f.message() != pane.face_said) { pane.face_said = f.message(); pane.message.clear(); pane.error = false; }
  ChromeState st;
  st.url = pane.face_url;
  st.loaded = true;
  if (!pane.message.empty()) { st.status = pane.message; st.status_ink = pane.error ? kErrorInk : kFaint; }
  else if (!pane.face_said.empty()) { st.status = pane.face_said; st.status_ink = kInk; }
  else {
    const face::Face& face = f.face();
    st.status = (face.title.empty() ? face.name : face.title) + "  ·  a face of this machine  ·  " +
                dec((uint64_t)f.placed().size()) + " placed" + (f.paint_cmds().empty() ? std::string() : ", " + dec((uint64_t)f.paint_cmds().size()) + " painted");
  }
  draw_pane_chrome(c, win, pane, w, h, st);
}


// A rect the page asked for, honouring the colour's ALPHA.
//
// draw_rect_to() is std::fill: it writes the colour and throws the alpha away.
// That is right for the desktop's own chrome, whose colours are opaque, and
// catastrophic for a web page. The whole visual language of a modern dark UI is
// `rgba(255,255,255,.05)` -- a wash a few percent above the ground, used for
// every card, button, input and divider. Filled opaque, every one of them came
// out SOLID WHITE, which is what the Assay console looked like in this pane:
// a page of white boxes where the panels should be.

// A colour at a fragment's accumulated opacity. `opacity: 0` has to mean the box
// is not painted at all -- a page keeps hover-revealed controls in the layout
// and out of sight that way, and drawing them put an edit button on top of
// every message bubble.
uint32_t with_opacity(uint32_t color, uint8_t op) {
  if (op >= 255) return color;
  uint32_t a = (color >> 24) & 0xFF;
  if (!a) a = 255;
  a = a * (uint32_t)op / 255u;
  return (a << 24) | (color & 0x00FFFFFFu);
}

void fill_rect_alpha(HardwareCompositor& c, std::vector<uint32_t>& buf, int bw, int bh,
                     int x, int y, int rw, int rh, uint32_t color, const DrawClip& clip) {
  const int a = (int)((color >> 24) & 0xFF);
  if (rw <= 0 || rh <= 0) return;
  // Clamped to the clip FIRST, so an opaque fill can still take the fast path.
  int cx0 = x < clip.x0 ? clip.x0 : x, cy0 = y < clip.y0 ? clip.y0 : y;
  int cx1 = x + rw > clip.x1 ? clip.x1 : x + rw, cy1 = y + rh > clip.y1 ? clip.y1 : y + rh;
  if (cx1 <= cx0 || cy1 <= cy0) return;
  if (a >= 255) { c.draw_rect_to(buf, bw, bh, cx0, cy0, cx1 - cx0, cy1 - cy0, color); return; }
  if (a <= 0) return;
  const int x0 = cx0 < 0 ? 0 : cx0, y0 = cy0 < 0 ? 0 : cy0;
  const int x1 = cx1 > bw ? bw : cx1, y1 = cy1 > bh ? bh : cy1;
  const uint32_t alpha = static_cast<uint32_t>(a);
  const uint32_t inv = 255 - alpha;
  const uint32_t sr = (color >> 16) & 0xFF, sg = (color >> 8) & 0xFF, sb = color & 0xFF;
  const uint32_t sr_a = sr * alpha + 127;
  const uint32_t sg_a = sg * alpha + 127;
  const uint32_t sb_a = sb * alpha + 127;
  for (int py = y0; py < y1; ++py) {
    uint32_t* row = &buf[(size_t)py * (size_t)bw];
    for (int px = x0; px < x1; ++px) {
      uint32_t dst = row[px];
      uint32_t dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
      uint32_t r = (sr_a + dr * inv) / 255;
      uint32_t g = (sg_a + dg * inv) / 255;
      uint32_t b = (sb_a + db * inv) / 255;
      row[px] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
  }
}

// How much of a pixel is inside a rounded rectangle: 1 in the middle, 0 well
// outside, and a fraction across the curve so a corner has an edge rather than
// a staircase. The box's own text is antialiased; a hard-edged corner beside it
// reads as a rendering fault.
float rounded_coverage(float px, float py, float x, float y, float w, float h, float r) {
  // Distance from the nearest corner CENTRE, which is only interesting inside
  // the corner squares -- everywhere else the pixel is simply in the box.
  const float cx = px < x + r ? x + r : (px > x + w - r ? x + w - r : px);
  const float cy = py < y + r ? y + r : (py > y + h - r ? y + h - r : py);
  const float dx = px - cx, dy = py - cy;
  const float d2 = dx * dx + dy * dy;
  if (d2 <= (r - 0.5f) * (r - 0.5f)) return 1.0f;
  if (d2 >= (r + 0.5f) * (r + 0.5f)) return 0.0f;
  const float d = std::sqrt(d2);
  return 0.5f - (d - r);
}

// A soft-edged rounded rectangle: a box shadow. The coverage ramps over `blur`
// pixels either side of the edge instead of one, which is a linear falloff
// rather than a gaussian -- close enough at the radii a page actually uses, and
// it costs one divide per pixel.
void fill_shadow(HardwareCompositor& c, std::vector<uint32_t>& buf, int bw, int bh,
                 int x, int y, int rw, int rh, uint32_t color, int radius, int blur,
                 const DrawClip& clip) {
  if (rw <= 0 || rh <= 0) return;
  const int a = (int)((color >> 24) & 0xFF);
  const int alpha = a ? a : 255;
  if (!alpha) return;
  int r = radius;
  const int half = (rw < rh ? rw : rh) / 2;
  if (r > half) r = half;
  if (r < 0) r = 0;
  const float soft = blur > 0 ? (float)blur : 1.0f;
  const int pad = blur + 1;
  const int x0 = std::max(clip.x0, (x - pad) < 0 ? 0 : (x - pad));
  const int y0 = std::max(clip.y0, (y - pad) < 0 ? 0 : (y - pad));
  const int x1 = std::min(clip.x1, (x + rw + pad) > bw ? bw : (x + rw + pad));
  const int y1 = std::min(clip.y1, (y + rh + pad) > bh ? bh : (y + rh + pad));
  for (int py = y0; py < y1; ++py) {
    for (int px = x0; px < x1; ++px) {
      // Distance outside the rounded rect, the same corner-centre trick the
      // sharp version uses.
      const float fx = (float)px + 0.5f, fy = (float)py + 0.5f;
      const float cx = fx < (float)(x + r) ? (float)(x + r)
                     : (fx > (float)(x + rw - r) ? (float)(x + rw - r) : fx);
      const float cy = fy < (float)(y + r) ? (float)(y + r)
                     : (fy > (float)(y + rh - r) ? (float)(y + rh - r) : fy);
      const float dx = fx - cx, dy = fy - cy;
      const float d = std::sqrt(dx * dx + dy * dy) - (float)r;
      float cov;
      if (d <= -soft * 0.5f) cov = 1.0f;
      else if (d >= soft * 0.5f) continue;
      else cov = 0.5f - d / soft;
      const int am = (int)((float)alpha * cov + 0.5f);
      if (am > 0 && clip.has(px, py)) c.blend_srgb_to(buf, bw, bh, px, py, color, am);
    }
  }
}

// A filled rounded rectangle, honouring alpha. `radius` is clamped to half the
// shorter side, which is what makes `border-radius: 50%` a circle.
void fill_round_rect(HardwareCompositor& c, std::vector<uint32_t>& buf, int bw, int bh,
                     int x, int y, int rw, int rh, uint32_t color, int radius,
                     const DrawClip& clip) {
  if (rw <= 0 || rh <= 0) return;
  int r = radius;
  const int half = (rw < rh ? rw : rh) / 2;
  if (r > half) r = half;
  if (r <= 0) { fill_rect_alpha(c, buf, bw, bh, x, y, rw, rh, color, clip); return; }
  const int a = (int)((color >> 24) & 0xFF);
  const int alpha = a ? a : 255;
  const int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
  const int x1 = x + rw > bw ? bw : x + rw, y1 = y + rh > bh ? bh : y + rh;
  for (int py = y0; py < y1; ++py) {
    // Rows away from both corner bands are a straight run.
    const bool in_corner_band = (py < y + r) || (py >= y + rh - r);
    if (py < clip.y0 || py >= clip.y1) continue;
    if (!in_corner_band) {
      const int rx0 = x0 < clip.x0 ? clip.x0 : x0, rx1 = x1 > clip.x1 ? clip.x1 : x1;
      if (rx1 <= rx0) continue;
      if (alpha >= 255) c.draw_rect_to(buf, bw, bh, rx0, py, rx1 - rx0, 1, color);
      else for (int px = rx0; px < rx1; ++px) c.blend_srgb_to(buf, bw, bh, px, py, color, alpha);
      continue;
    }
    const int cx0 = std::max(x0, clip.x0), cx1 = std::min(x1, clip.x1);
    for (int px = cx0; px < cx1; ++px) {
      const float cov = rounded_coverage((float)px + 0.5f, (float)py + 0.5f,
                                         (float)x, (float)y, (float)rw, (float)rh, (float)r);
      if (cov <= 0.0f) continue;
      int am = (int)((float)alpha * (cov > 1.0f ? 1.0f : cov) + 0.5f);
      if (am > 0 && clip.has(px, py)) c.blend_srgb_to(buf, bw, bh, px, py, color, am);
    }
  }
}

// A rounded border: the filled outer shape minus the filled inner one, done as
// coverage so the ring is a ring and not four bands with notches at the corners.
void stroke_round_rect(HardwareCompositor& c, std::vector<uint32_t>& buf, int bw, int bh,
                       int x, int y, int rw, int rh, uint32_t color, int radius,
                       int bt, int br, int bb, int bl, const DrawClip& clip) {
  if (rw <= 0 || rh <= 0) return;
  int r = radius;
  const int half = (rw < rh ? rw : rh) / 2;
  if (r > half) r = half;
  const int a = (int)((color >> 24) & 0xFF);
  const int alpha = a ? a : 255;
  const int ix = x + bl, iy = y + bt;
  const int iw = rw - bl - br, ih = rh - bt - bb;
  const int inner_r = r - (bl > bt ? bl : bt);
  const int x0 = std::max(clip.x0, x < 0 ? 0 : x);
  const int y0 = std::max(clip.y0, y < 0 ? 0 : y);
  const int x1 = std::min(clip.x1, x + rw > bw ? bw : x + rw);
  const int y1 = std::min(clip.y1, y + rh > bh ? bh : y + rh);
  for (int py = y0; py < y1; ++py) {
    for (int px = x0; px < x1; ++px) {
      const float outer = rounded_coverage((float)px + 0.5f, (float)py + 0.5f,
                                           (float)x, (float)y, (float)rw, (float)rh, (float)r);
      if (outer <= 0.0f) continue;
      float inner = 0.0f;
      if (iw > 0 && ih > 0)
        inner = rounded_coverage((float)px + 0.5f, (float)py + 0.5f, (float)ix, (float)iy,
                                 (float)iw, (float)ih, inner_r > 0 ? (float)inner_r : 0.0f);
      float cov = outer - inner;
      if (cov <= 0.0f) continue;
      if (cov > 1.0f) cov = 1.0f;
      const int am = (int)((float)alpha * cov + 0.5f);
      if (am > 0 && clip.has(px, py)) c.blend_srgb_to(buf, bw, bh, px, py, color, am);
    }
  }
}

namespace {
void render_web_pane_frame(HardwareCompositor& c, WindowSurface& win, Kernel& k);
}

void render_web_pane(HardwareCompositor& c, WindowSurface& win, Kernel& k) {
  render_web_pane_frame(c, win, k);
  // Reflow asked for by the frame (resize, :hover, a script's tick): done by the worker, one at a time, at the width
  // the frame last had. Dispatched here for the same reason as the pictures below.
  if (s_reflow_req && win.web && win.web->browser && !s_reflow_inflight.load()) {
    s_reflow_req = false;
    const bool hover = s_reflow_hover;
    s_reflow_hover = false;
    s_reflow_inflight = true;
    if (!run_page_work(win.web, win.web->last_viewport_w > 0 ? win.web->last_viewport_w : 800, "reflowing", [hover](WebPane& p, int&) {
          auto t0 = std::chrono::steady_clock::now();
          const int vw = p.last_viewport_w > 0 ? p.last_viewport_w : 800;
          p.browser->relayout(pane_metrics(), vw);
          p.browser->fetch_images(0);   // no new fetches on a reflow; re-box what is cached
          const auto ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
          if (hover && ms > 60) s_hover_off_dom = p.browser->document().dom.get();
          if (getenv("AOS_LATENCY")) fprintf(stderr, "reflow on worker %lld ms\n", ms);
          s_reflow_inflight = false;
        }, /*exclusive=*/false))
      s_reflow_inflight = false;
  }
  // The frame has let go of the page lock now, so a job can be started. Starting
  // one from INSIDE the frame took the lock a second time on the same thread and
  // hung the whole desktop.
  if (win.web && win.web->pending_pics && win.web->browser) {
    WebPane& pane = *win.web;
    pane.pending_pics = false;
    const int y0 = pane.pending_y0, y1 = pane.pending_y1;
    run_page_work(win.web, pane.last_viewport_w > 0 ? pane.last_viewport_w : 800, "pictures",
                  [y0, y1](WebPane& p, int& wrote) {
                    p.browser->fetch_images_range(6, y0, y1);
                    wrote = 0;
                  }, /*exclusive=*/false);
  }
}

namespace {
void render_web_pane_frame(HardwareCompositor& c, WindowSurface& win, Kernel& k) {
  int w = win.content_width();
  int h = win.content_height();
  if (!win.web) win.web = std::make_shared<WebPane>();
  WebPane& pane = *win.web;
  if (!pane.browser) pane.browser = std::make_shared<web::Browser>(kernel_fetch(k, pane.guard));
  if (!g_clock_host) {
    g_clock_host = &k.host();
    js_set_clock(&pane_wall_ms);
  }

  int view_y = kChromeH;
  int view_h = h - kChromeH - kStatusH;
  if (view_h < 40) view_h = 40;
  int view_w = w - kScrollW;
  if (view_w < 80) view_w = 80;

  // The page's own work runs on another thread. Reading its layout while it is
  // being rebuilt is how a frame gets half a document, so the frame takes the
  // same lock -- for as long as it takes to read, which is microseconds. The
  // transport lets go of it while it waits on the network, so an application
  // that is thinking does not stop the machine from drawing.
  struct Frame {
    PageGuard* g;
    ~Frame() { if (g) { g->in_frame = false; g->unlock_frame(); } }
  } frame{pane.guard.get()};
  // Never wait for the worker: it may be laying out a big page for a second. The buffer still holds the last frame.
  if (!pane.guard->m.try_lock()) { frame.g = nullptr; return; }
  pane.guard->held = true;
  pane.guard->in_frame = true;

  if (pane.face) { render_face_pane(c, win, k, pane, w, h, view_y, view_w, view_h); return; }

  // Zoomed, everything below that is about the PAGE (layout width, viewport height, painting) works in CSS pixels; the real
  // sizes come back after the page raster is sampled onto the pane.
  const int real_w = w, real_h = h, real_view_w = view_w, real_view_h = view_h;
  if (s_zoom != 100) {
    view_w = std::max(80, zoom_px(real_view_w));
    view_h = std::max(40, zoom_px(real_view_h));
    w = view_w + kScrollW;
    h = view_h + kChromeH + kStatusH;
  }

  // Pictures arrive as they are needed: the ones on screen, and a screen's worth
  // either side. Looked for a few times a second, and fetched off this thread.
  if (pane.working.empty()) s_live_cost_ms = 0;   // the next load starts the estimate over
  if (pane.working.empty() && pane.browser->document().dom) {
    const uint64_t now_ms = k.host().monotonic_ns() / 1000000ULL;
    if (now_ms - pane.last_image_check_ms >= 300) {
      pane.last_image_check_ms = now_ms;
      const int y0 = pane.scroll - view_h / 2, y1 = pane.scroll + view_h + view_h;
      if (pane.browser->has_unfetched_images(y0, y1)) {
        pane.pending_pics = true;
        pane.pending_y0 = y0;
        pane.pending_y1 = y1;
      }
    }
  }

  // A resized window is a reflowed page, not a scaled picture of one.
  if (pane.last_viewport_w != view_w && pane.browser->document().dom) {
    s_reflow_req = true;   // on the worker: see dispatch_reflow
  }
  pane.last_viewport_w = view_w;
  update_hover(pane, view_w, k.host().monotonic_ns() / 1000000ULL);
  pane.browser->set_view_height(view_h);

  // The page's own time. A frame handler, a due timer, an animation frame: each
  // runs here, once per frame the pane is painted, with the real elapsed time.
  // Keys the hosted input path can only press are released a moment after.
  if (auto js = pane.browser->js()) {
    js->set_view(view_w, view_h);
    const uint64_t now_ns = k.host().monotonic_ns();
    const uint64_t now_ms = now_ns / 1000000ull;
    for (auto it = pane.held.begin(); it != pane.held.end();) {
      if (now_ms > it->second && now_ms - it->second > 160) {
        int wrote = 0;
        js->key(it->first, false, &wrote);
        it = pane.held.erase(it);
      } else ++it;
    }
    if (js->live()) {
      double dt = pane.last_tick_ns ? (double)(now_ns - pane.last_tick_ns) / 1e6 : 16.0;
      int wrote = 0;
      js->tick(dt, &wrote);
      if (wrote > 0) s_reflow_req = true;
    }
    pane.last_tick_ns = now_ns;
    if (web::Node* f = js->take_focus_request()) {
      if (node_takes_text(f)) { pane.field = f; pane.url_focus = false; }
    }
    if (js->storage_dirty() && now_ms - pane.last_flush_ms > 1200) {
      js->flush_storage();
      pane.last_flush_ms = now_ms;
    }
  }

  const web::Document& doc = pane.browser->document();

  // -- the page ------------------------------------------------------------
  int max_scroll = doc.content_height - view_h;
  if (max_scroll < 0) max_scroll = 0;
  if (pane.scroll > max_scroll) pane.scroll = max_scroll;
  if (pane.scroll < 0) pane.scroll = 0;

  // Everything the page area is drawn FROM, folded into one number. Equal to the
  // last frame's, and the pixels already in the buffer are the pixels this frame
  // would put there. A page that is animating (script timers, a canvas it paints
  // itself) is never skipped: its state is not in the document.
  uint64_t key = 0xcbf29ce484222325ull;
  auto mix = [&](uint64_t v) { key = (key ^ v) * 0x100000001b3ull; key ^= key >> 29; };
  const bool animating = pane.browser->js() && (pane.browser->js()->live() || !pane.browser->js()->paint_ops().empty());
  web::Node* focused_now = live_field(pane);
  {
    mix((uint64_t)w); mix((uint64_t)h); mix((uint64_t)view_w); mix((uint64_t)view_h);
    mix((uint64_t)(int64_t)pane.scroll); mix(doc.background); mix((uint64_t)doc.content_height);
    mix((uint64_t)(int64_t)pane.hover_link); mix(pane.browser->image_count()); mix(s_hover_gen); mix(s_sel_gen);
    mix((uint64_t)s_zoom); mix(s_find_gen); mix(s_find_open ? 1 : 2); mix((uint64_t)real_w); mix((uint64_t)real_h);
    mix((uint64_t)(uintptr_t)focused_now); mix(pane.field_all ? 1 : 2);
    if (focused_now) {
      mix(std::hash<std::string>{}(field_value(focused_now)));
      mix(k.host().monotonic_ns() / 500000000ULL);          // the caret blinks
    }
    for (const auto& sc : doc.scrollers) mix((uint64_t)(int64_t)pane.scroll_offset_of(sc.node));
    mix(doc.layout_gen);   // a new layout is a new picture, whatever its fragments hash to
    if (pane.cached_layout_gen != doc.layout_gen ||
        pane.cached_frag_ptr != doc.fragments.data() || pane.cached_frag_size != doc.fragments.size() ||
        pane.cached_content_h != doc.content_height || pane.cached_content_w != doc.content_width) {
      uint64_t fh = 0xcbf29ce484222325ull;
      for (const auto& f : doc.fragments) {
        fh = (fh ^ (((uint64_t)(uint32_t)f.x << 32) | (uint32_t)f.y)) * 0x100000001b3ull;
        fh = (fh ^ (((uint64_t)(uint32_t)f.w << 32) | (uint32_t)f.h)) * 0x100000001b3ull;
        fh = (fh ^ (((uint64_t)f.color << 8) | f.opacity)) * 0x100000001b3ull;
        fh = (fh ^ ((uint64_t)f.kind | ((uint64_t)f.text.size() << 8) | ((uint64_t)f.src.size() << 32))) * 0x100000001b3ull;
        if (!f.text.empty()) fh = (fh ^ std::hash<std::string>{}(f.text)) * 0x100000001b3ull;
      }
      pane.cached_frag_ptr = doc.fragments.data();
      pane.cached_layout_gen = doc.layout_gen;
      pane.cached_frag_size = doc.fragments.size();
      pane.cached_content_h = doc.content_height;
      pane.cached_content_w = doc.content_width;
      pane.cached_frag_hash = fh;
    }
    mix(pane.cached_frag_hash);
  }
  const bool unchanged = !animating && pane.render_key == key && pane.render_buf == (const void*)win.buffer.data() &&
                         pane.render_buf_size == win.buffer.size();
  pane.render_key = key;
  pane.render_buf = win.buffer.data();
  pane.render_buf_size = win.buffer.size();
  const bool zoomed = s_zoom != 100;
  static thread_local std::vector<uint32_t> s_zoom_scratch;
  if (!unchanged) {
  if (zoomed) { s_zoom_scratch.assign((size_t)w * (size_t)h, 0); win.buffer.swap(s_zoom_scratch); }
  c.clear_buffer(win.buffer, doc.dom ? doc.background : 0xFF141414, w, h);
  // A light ground or a dark one, judged the way layout chose the UA sheet: the scrollbars and focus ring follow it.
  const uint32_t ground = doc.dom ? doc.background : 0xFF141414;
  const bool light_page = (((ground >> 16) & 0xFF) * 2126 + ((ground >> 8) & 0xFF) * 7152 + (ground & 0xFF) * 722) / 10000 > 140;
  if (s_find_open && s_cached_find_gen != s_find_gen) {
    s_cached_find_marks = find_hits(doc);
    s_cached_find_gen = s_find_gen;
  }
  static const std::vector<FindHit> kNoFindHits;
  const std::vector<FindHit>& find_marks = s_find_open ? s_cached_find_marks : kNoFindHits;
  size_t find_at = 0;

  // Boxes with a 2D transform (Document::transforms) are drawn the first time paint order reaches one: everything
  // inside it, offscreen, then put on the page through its matrix. Offscreen means twice, once over black and once
  // over white: the difference between the two is each pixel's coverage, which is what lets every routine below
  // paint into the scratch exactly as it paints the page, antialiased edges included.
  const size_t n_xf = doc.transforms.size();
  std::vector<char> xf_done(n_xf, 0);
  std::function<void(const std::vector<size_t>&, int)> paint_list;
  static thread_local std::vector<std::vector<uint32_t>> s_xf_pool;   // two buffers per nesting depth
  int xf_depth = 0;
  auto in_group = [&](int g, int top) {
    while (g >= 0 && g != top) g = doc.transforms[(size_t)g].parent;
    return g == top;
  };
  auto render_group = [&](int gi, const std::vector<size_t>& sub) {
    if (sub.empty() || xf_depth >= 8) return;
    const auto& g = doc.transforms[(size_t)gi];
    // Where it is drawn untransformed, in window pixels, with room for a shadow's blur.
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
    int shift_y = 0;
    bool first = true;
    for (size_t fi : sub) {
      const auto& f = doc.fragments[fi];
      int inner = 0;
      if (f.scroller >= 0 && (size_t)f.scroller < doc.scrollers.size())
        inner = pane.scroll_offset_of(doc.scrollers[(size_t)f.scroller].node);
      const int fy = view_y + f.y - pane.scroll - inner;
      if (first) { shift_y = view_y - pane.scroll - inner; first = false; }
      x0 = std::min(x0, f.x); y0 = std::min(y0, fy);
      x1 = std::max(x1, f.x + std::max(f.w, 1)); y1 = std::max(y1, fy + std::max(f.h, 1));
    }
    x0 = std::max(0, x0 - 48); y0 = std::max(0, y0 - 48);
    x1 = std::min(w, x1 + 48); y1 = std::min(h, y1 + 48);
    if (x1 <= x0 || y1 <= y0) return;
    // Sized once for the deepest nesting allowed: a nested group must never reallocate the pool under the
    // references an enclosing one holds.
    if (s_xf_pool.size() < 16) s_xf_pool.resize(16);
    std::vector<uint32_t>& black = s_xf_pool[(size_t)(2 * xf_depth)];
    std::vector<uint32_t>& white = s_xf_pool[(size_t)(2 * xf_depth + 1)];
    const size_t need = (size_t)w * (size_t)h;
    if (black.size() != need) black.assign(need, 0xFF000000u);
    if (white.size() != need) white.assign(need, 0xFFFFFFFFu);
    for (int yy = y0; yy < y1; ++yy) {
      std::fill(black.begin() + (ptrdiff_t)((size_t)yy * w + x0), black.begin() + (ptrdiff_t)((size_t)yy * w + x1), 0xFF000000u);
      std::fill(white.begin() + (ptrdiff_t)((size_t)yy * w + x0), white.begin() + (ptrdiff_t)((size_t)yy * w + x1), 0xFFFFFFFFu);
    }
    ++xf_depth;
    for (int pass = 0; pass < 2; ++pass) {
      std::vector<uint32_t>& scratch = pass == 0 ? black : white;
      for (size_t j = 0; j < n_xf; ++j) if ((int)j != gi && in_group((int)j, gi)) xf_done[j] = 0;   // nested ones, again
      find_at = 0;
      win.buffer.swap(scratch);
      paint_list(sub, gi);
      win.buffer.swap(scratch);
    }
    --xf_depth;
    find_at = 0;
    // The map in window pixels, and back.
    const float ox = g.ox, oy = g.oy + (float)shift_y;
    const float det = g.a * g.d - g.b * g.c;
    if (std::fabs(det) < 1e-6f) return;   // scaled to nothing: nothing to draw
    const float ia = g.d / det, ib = -g.b / det, ic = -g.c / det, id = g.a / det;
    float dx0 = 1e30f, dy0 = 1e30f, dx1 = -1e30f, dy1 = -1e30f;
    for (int corner = 0; corner < 4; ++corner) {
      const float px = (float)((corner & 1) ? x1 : x0) - ox, py = (float)((corner & 2) ? y1 : y0) - oy;
      const float qx = ox + g.a * px + g.c * py + g.e, qy = oy + g.b * px + g.d * py + g.f;
      dx0 = std::min(dx0, qx); dy0 = std::min(dy0, qy); dx1 = std::max(dx1, qx); dy1 = std::max(dy1, qy);
    }
    // Inside the page area, and inside whatever its ancestors' `overflow` cuts it to.
    int cx0 = 0, cy0 = view_y, cx1 = view_w, cy1 = view_y + view_h;
    if (xf_depth > 0) { cx0 = 0; cy0 = 0; cx1 = w; cy1 = h; }   // into an enclosing group's scratch: it clips later
    const int px0 = std::max(cx0, (int)std::floor(dx0)), py0 = std::max(cy0, (int)std::floor(dy0));
    const int px1 = std::min(cx1, (int)std::ceil(dx1)), py1 = std::min(cy1, (int)std::ceil(dy1));
    auto sample = [&](const std::vector<uint32_t>& buf, uint32_t outside, float sx, float sy, float out[3]) {
      // Bilinear, reading `outside` (what an empty pixel of this scratch is) beyond what was drawn.
      const int ix = (int)std::floor(sx), iy = (int)std::floor(sy);
      const float fx = sx - (float)ix, fy = sy - (float)iy;
      auto at = [&](int x, int y) -> uint32_t {
        if (x < x0 || y < y0 || x >= x1 || y >= y1) return outside;
        return buf[(size_t)y * (size_t)w + (size_t)x];
      };
      const uint32_t p00 = at(ix, iy), p10 = at(ix + 1, iy), p01 = at(ix, iy + 1), p11 = at(ix + 1, iy + 1);
      for (int ch = 0; ch < 3; ++ch) {
        const int sh = 16 - 8 * ch;
        const float top = (float)((p00 >> sh) & 0xFF) * (1 - fx) + (float)((p10 >> sh) & 0xFF) * fx;
        const float bot = (float)((p01 >> sh) & 0xFF) * (1 - fx) + (float)((p11 >> sh) & 0xFF) * fx;
        out[ch] = top * (1 - fy) + bot * fy;
      }
    };
    for (int py = py0; py < py1; ++py) {
      for (int px = px0; px < px1; ++px) {
        const float u = (float)px + 0.5f - ox - g.e, v = (float)py + 0.5f - oy - g.f;
        const float sx = ox + ia * u + ic * v - 0.5f, sy = oy + ib * u + id * v - 0.5f;
        if (sx < (float)x0 - 1 || sy < (float)y0 - 1 || sx > (float)x1 || sy > (float)y1) continue;
        float kb[3], kw[3];
        sample(black, 0xFF000000u, sx, sy, kb);
        sample(white, 0xFFFFFFFFu, sx, sy, kw);
        const float cover = 1.0f - ((kw[0] - kb[0]) + (kw[1] - kb[1]) + (kw[2] - kb[2])) / (3.0f * 255.0f);
        if (cover <= 0.004f) continue;
        uint32_t& dst = win.buffer[(size_t)py * (size_t)w + (size_t)px];
        const float a = cover > 1.0f ? 1.0f : cover;
        uint32_t outp = 0xFF000000u;
        for (int ch = 0; ch < 3; ++ch) {
          const int sh = 16 - 8 * ch;
          float col = kb[ch] / a;                          // what was drawn, before it was laid over black
          if (col > 255.0f) col = 255.0f;
          const float d = (float)((dst >> sh) & 0xFF);
          outp |= (uint32_t)std::lround(col * a + d * (1.0f - a)) << sh;
        }
        dst = outp;
      }
    }
  };
  paint_list = [&](const std::vector<size_t>& list, int current) {
  for (size_t fi_ : list) {
    const auto& f = doc.fragments[fi_];
    if (f.xform != current) {
      // The first of a transformed box's fragments: the whole box, through its matrix, now.
      int top = f.xform;
      while (top >= 0 && doc.transforms[(size_t)top].parent != current) top = doc.transforms[(size_t)top].parent;
      if (top < 0 || xf_done[(size_t)top]) continue;
      xf_done[(size_t)top] = 1;
      std::vector<size_t> sub;
      for (size_t fj : list) if (in_group(doc.fragments[fj].xform, top)) sub.push_back(fj);
      render_group(top, sub);
      continue;
    }
    // A box inside a scroll pane is drawn at its laid-out position minus that
    // pane's own offset. The pane's frame does not carry a scroller id, so it
    // stays put while its contents move under it.
    int inner = 0;
    if (f.scroller >= 0 && (size_t)f.scroller < doc.scrollers.size())
      inner = pane.scroll_offset_of(doc.scrollers[(size_t)f.scroller].node);
    int y = view_y + f.y - pane.scroll - inner;

    const int fh = f.h > 0 ? f.h : 24;
    // Off the top or the bottom of the viewport: nothing to paint, and no
    // reason to walk the glyphs of a paragraph nobody can see.
    if (y + fh < view_y || y > view_y + view_h) continue;

    // What this fragment is allowed to touch. The viewport always; and the
    // clip its ancestors' `overflow` imposed, moved into window coordinates the
    // same way the fragment itself is.
    DrawClip clip;
    clip.x0 = 0;
    clip.x1 = view_w;
    clip.y0 = view_y;
    clip.y1 = view_y + view_h;
    if (f.clipped) {
      const int cx0 = f.clip_x;
      const int cy0 = view_y + f.clip_y - pane.scroll - inner;
      if (cx0 > clip.x0) clip.x0 = cx0;
      if (cy0 > clip.y0) clip.y0 = cy0;
      if (cx0 + f.clip_w < clip.x1) clip.x1 = cx0 + f.clip_w;
      if (cy0 + f.clip_h < clip.y1) clip.y1 = cy0 + f.clip_h;
    }
    if (clip.x1 <= clip.x0 || clip.y1 <= clip.y0) continue;
    switch (f.kind) {
      case web::Fragment::Kind::Rect: {
        int y0 = y < view_y ? view_y : y;
        int y1 = y + f.h > view_y + view_h ? view_y + view_h : y + f.h;
        if (y1 > y0) {
          const int fw = f.w > view_w ? view_w : f.w;
          const uint32_t col = with_opacity(f.color, f.opacity);
          if (!(col >> 24)) break;
          // A radius is a property of the WHOLE box, so the rounded fill is
          // given the box's real top and height and clips itself to the
          // viewport -- passing it the clipped slice would round the middle of
          // a scrolled card.
          auto paint = [&](const DrawClip& cl) {
            if (cl.x1 <= cl.x0 || cl.y1 <= cl.y0) return;
            if (f.blur > 0 && y >= view_y && y + f.h <= view_y + view_h)
              fill_shadow(c, win.buffer, w, h, f.x, y, fw, f.h, col, f.radius, f.blur, cl);
            else if (f.radius > 0 && y >= view_y && y + f.h <= view_y + view_h)
              fill_round_rect(c, win.buffer, w, h, f.x, y, fw, f.h, col, f.radius, cl);
            else
              fill_rect_alpha(c, win.buffer, w, h, f.x, y0, fw, y1 - y0, col, cl);
          };
          if (f.shadow && f.cut_w > 0 && f.cut_h > 0) {
            // Everything but the box itself: the band above it, the band below
            // it, and the two beside it.
            const int cy0 = view_y + f.cut_y - pane.scroll - inner;
            const int cy1 = cy0 + f.cut_h;
            const int cx0 = f.cut_x, cx1 = f.cut_x + f.cut_w;
            DrawClip top = clip;    top.y1 = std::min(clip.y1, cy0);
            DrawClip bot = clip;    bot.y0 = std::max(clip.y0, cy1);
            DrawClip left = clip;   left.y0 = std::max(clip.y0, cy0);  left.y1 = std::min(clip.y1, cy1);  left.x1 = std::min(clip.x1, cx0);
            DrawClip right = clip;  right.y0 = std::max(clip.y0, cy0); right.y1 = std::min(clip.y1, cy1); right.x0 = std::max(clip.x0, cx1);
            paint(top); paint(bot); paint(left); paint(right);
          } else {
            paint(clip);
          }
        }
        break;
      }
      case web::Fragment::Kind::Border: {
        // The four widths came through in the fragment's text field, because a
        // border is four numbers and a Fragment has one h.
        int bt = f.text.size() > 0 ? (unsigned char)f.text[0] : 0;
        int brr = f.text.size() > 1 ? (unsigned char)f.text[1] : 0;
        int bb = f.text.size() > 2 ? (unsigned char)f.text[2] : 0;
        int bl = f.text.size() > 3 ? (unsigned char)f.text[3] : 0;
        const uint32_t bcol = with_opacity(f.color, f.opacity);
        if (!(bcol >> 24)) break;
        auto band = [&](int bx, int by, int bw2, int bh2) {
          if (bw2 <= 0 || bh2 <= 0) return;
          int y0 = by < view_y ? view_y : by;
          int y1 = by + bh2 > view_y + view_h ? view_y + view_h : by + bh2;
          if (y1 > y0) fill_rect_alpha(c, win.buffer, w, h, bx, y0, bw2, y1 - y0, bcol, clip);
        };
        if (f.radius > 0 && y >= view_y && y + f.h <= view_y + view_h) {
          stroke_round_rect(c, win.buffer, w, h, f.x, y, f.w, f.h, bcol, f.radius, bt, brr, bb, bl, clip);
        } else {
          band(f.x, y, f.w, bt);
          band(f.x, y + f.h - bb, f.w, bb);
          band(f.x, y, bl, f.h);
          band(f.x + f.w - brr, y, brr, f.h);
        }
        break;
      }
      case web::Fragment::Kind::Rule:
        if (y >= view_y && y < view_y + view_h)
          c.draw_rect_to(win.buffer, w, h, f.x, y, f.w > view_w ? view_w : f.w, f.h > 0 ? f.h : 1, f.color);
        break;
      case web::Fragment::Kind::Canvas: {
        const web::Bitmap* bm = pane.browser->image(f.src);
        if (bm && !bm->empty() && f.w > 0 && f.h > 0) {
          for (int row = 0; row < f.h; ++row) {
            int py = y + row;
            if (py < view_y || py >= view_y + view_h) continue;
            int sy = bm->h > 1 ? row * bm->h / f.h : 0;
            if (sy >= bm->h) sy = bm->h - 1;
            for (int col = 0; col < f.w; ++col) {
              int px = f.x + col;
              if (px < 0 || px >= view_w) continue;
              int sx = bm->w > 1 ? col * bm->w / f.w : 0;
              if (sx >= bm->w) sx = bm->w - 1;
              uint32_t p = bm->pixels[(size_t)sy * bm->w + sx];
              if (!clip.has(px, py)) continue;
              int a = (int)((p >> 24) & 0xFF) * (int)f.opacity / 255;
              if (a) c.blend_srgb_to(win.buffer, w, h, px, py, p, a);
            }
          }
        } else {
          int box_h = f.h > 0 ? f.h : 150;
          int box_w = f.w > 0 ? f.w : 300;
          c.draw_rect_outline_to(win.buffer, w, h, f.x, y, box_w, box_h, 0xFF3A3A3A);
        }
        break;
      }
      case web::Fragment::Kind::Video: {
        int box_h = f.h > 0 ? f.h : 150;
        int box_w = f.w > 0 ? f.w : 300;
        // Video background frame
        fill_round_rect(c, win.buffer, w, h, f.x, y, box_w, box_h, 0xFF0A0A0A, f.radius > 0 ? f.radius : 4, clip);
        // Poster / thumbnail image if available
        const std::string img_src = !f.poster.empty() ? f.poster : f.src;
        const web::Bitmap* bm = pane.browser->image(img_src);
        if (bm && !bm->empty()) {
          for (int row = 0; row < box_h; ++row) {
            int py = y + row;
            if (py < view_y || py >= view_y + view_h) continue;
            int sy = bm->h > 1 ? row * bm->h / box_h : 0;
            if (sy >= bm->h) sy = bm->h - 1;
            for (int col = 0; col < box_w; ++col) {
              int px = f.x + col;
              if (px < 0 || px >= view_w) continue;
              int sx = bm->w > 1 ? col * bm->w / box_w : 0;
              if (sx >= bm->w) sx = bm->w - 1;
              uint32_t p = bm->pixels[(size_t)sy * bm->w + sx];
              if (!clip.has(px, py)) continue;
              c.blend_srgb_to(win.buffer, w, h, px, py, p, 255);
            }
          }
        }
        // Center play overlay if paused
        if (!f.media_playing && box_w >= 60 && box_h >= 60) {
          int cx = f.x + box_w / 2;
          int cy = y + box_h / 2;
          fill_round_rect(c, win.buffer, w, h, cx - 20, cy - 20, 40, 40, 0x90000000, 20, clip);
          web::FontKey play_font{16, true, false, false};
          web_draw_text(c, win.buffer, w, h, cx - 5, cy - 8, "\u25B6", 0xFFFFFFFF, play_font, clip);
        }
        // Controls bar at bottom
        if (f.media_controls && box_h >= 40) {
          int bar_h = 26;
          int bar_y = y + box_h - bar_h;
          fill_round_rect(c, win.buffer, w, h, f.x, bar_y, box_w, bar_h, 0xD0151515, 4, clip);
          web::FontKey ctrl_font{12, false, false, false};
          // Play/pause icon
          const char* pp = f.media_playing ? "||" : "\u25B6";
          web_draw_text(c, win.buffer, w, h, f.x + 8, bar_y + 6, pp, 0xFFFFFFFF, ctrl_font, clip);
          // Progress track
          int bar_start = f.x + 30;
          int bar_end = f.x + box_w - 70;
          if (bar_end > bar_start + 20) {
            c.draw_rect_to(win.buffer, w, h, bar_start, bar_y + 11, bar_end - bar_start, 4, 0xFF404040);
            c.draw_rect_to(win.buffer, w, h, bar_start, bar_y + 11, (bar_end - bar_start) / 3, 4, 0xFF5B8DD9);
          }
          // Time readout
          web_draw_text(c, win.buffer, w, h, f.x + box_w - 65, bar_y + 6, "0:00/1:30", 0xFFA0A0A0, ctrl_font, clip);
        }
        break;
      }
      case web::Fragment::Kind::Audio: {
        int box_h = f.h > 0 ? f.h : 36;
        int box_w = f.w > 0 ? f.w : 300;
        // Audio card container
        fill_round_rect(c, win.buffer, w, h, f.x, y, box_w, box_h, 0xFF1C1C1E, 6, clip);
        stroke_round_rect(c, win.buffer, w, h, f.x, y, box_w, box_h, 0xFF38383A, 6, 1, 1, 1, 1, clip);
        // Play button circle
        fill_round_rect(c, win.buffer, w, h, f.x + 6, y + (box_h - 24) / 2, 24, 24, 0xFF3A3A3C, 12, clip);
        web::FontKey icon_font{11, true, false, false};
        const char* pp = f.media_playing ? "||" : "\u25B6";
        web_draw_text(c, win.buffer, w, h, f.x + 13, y + (box_h - 24) / 2 + 5, pp, 0xFFFFFFFF, icon_font, clip);
        // Audio Title & Time
        web::FontKey label_font{12, false, false, false};
        std::string title = f.alt.empty() ? (f.src.empty() ? "Audio Player" : f.src) : f.alt;
        if (title.size() > 24) title = title.substr(0, 21) + "...";
        web_draw_text(c, win.buffer, w, h, f.x + 36, y + (box_h - 14) / 2, title, 0xFFE5E5EA, label_font, clip);
        // Progress scrubber
        int track_x = f.x + 150;
        int track_w = box_w - 220;
        if (track_w > 30) {
          c.draw_rect_to(win.buffer, w, h, track_x, y + (box_h - 4) / 2, track_w, 4, 0xFF48484A);
          c.draw_rect_to(win.buffer, w, h, track_x, y + (box_h - 4) / 2, track_w / 4, 4, 0xFF0A84FF);
        }
        // Time
        if (box_w >= 280) {
          web_draw_text(c, win.buffer, w, h, f.x + box_w - 60, y + (box_h - 14) / 2, "0:00/3:45", 0xFF8E8E93, label_font, clip);
        }
        break;
      }
      case web::Fragment::Kind::Image: {
        const web::Bitmap* bm = pane.browser->image(f.src);
        if (f.mask) {
          // A stencil: the picture's opacity, in the tint's colour, fitted into
          // the box and centred -- `mask-size: contain`, which is what every
          // icon sheet asks for. Until it has loaded nothing is drawn, not an
          // alt-text box.
          if (!bm || bm->empty() || f.w <= 0 || f.h <= 0) break;
          const int side = f.w < f.h ? f.w : f.h;
          const int ox = f.x + (f.w - side) / 2, oy = y + (f.h - side) / 2;
          const uint32_t ink = with_opacity(f.tint, f.opacity);
          const int ink_a = (int)((ink >> 24) & 0xFF);
          for (int row = 0; row < side; ++row) {
            const int py = oy + row;
            if (py < view_y || py >= view_y + view_h) continue;
            const int sy = bm->h > 1 ? row * bm->h / side : 0;
            for (int col = 0; col < side; ++col) {
              const int px = ox + col;
              if (px < 0 || px >= view_w || !clip.has(px, py)) continue;
              const int sx = bm->w > 1 ? col * bm->w / side : 0;
              const uint32_t p = bm->pixels[(size_t)(sy < bm->h ? sy : bm->h - 1) * bm->w + (size_t)(sx < bm->w ? sx : bm->w - 1)];
              const int a = (int)((p >> 24) & 0xFF) * ink_a / 255;
              if (a) c.blend_srgb_to(win.buffer, w, h, px, py, ink, a);
            }
          }
          break;
        }
        // The picture's own size, which for an SVG is what its markup declares
        // rather than the (larger) bitmap it was drawn into.
        auto natural = [&](int& nw, int& nh) {
          nw = bm ? bm->w : 0;
          nh = bm ? bm->h : 0;
          for (const auto& e : doc.image_sizes)
            if (e.first == f.src) { nw = e.second.first; nh = e.second.second; break; }
        };
        if (f.bg) {
          // A CSS background picture: sized by `background-size`, placed by
          // `background-position`, and tiled unless told not to.
          if (!bm || bm->empty() || f.w <= 0 || f.h <= 0) break;
          int nw, nh;
          natural(nw, nh);
          if (nw <= 0 || nh <= 0) break;
          int rw = f.bg_w_pct >= 0 ? (int)((float)f.w * f.bg_w_pct / 100.0f) : f.bg_w;
          int rh = f.bg_h_pct >= 0 ? (int)((float)f.h * f.bg_h_pct / 100.0f) : f.bg_h;
          int tw = nw, th = nh;
          if (f.bg_size == 1 || f.bg_size == 2) {            // Cover / Contain
            const float sx = (float)f.w / (float)nw, sy2 = (float)f.h / (float)nh;
            const float sc = (f.bg_size == 1) == (sx > sy2) ? sx : sy2;
            tw = (int)((float)nw * sc + 0.5f);
            th = (int)((float)nh * sc + 0.5f);
          } else if (f.bg_size == 3) {                       // Explicit
            if (rw > 0 && rh > 0) { tw = rw; th = rh; }
            else if (rw > 0) { tw = rw; th = (int)((long long)nh * rw / nw); }
            else if (rh > 0) { th = rh; tw = (int)((long long)nw * rh / nh); }
          }
          if (tw < 1) tw = 1;
          if (th < 1) th = 1;
          const int ox = (int)((float)(f.w - tw) * f.bg_pos_x / 100.0f + f.bg_off_x);
          const int oy = (int)((float)(f.h - th) * f.bg_pos_y / 100.0f + f.bg_off_y);
          static thread_local std::vector<int> s_bg_xs;
          if (s_bg_xs.size() < (size_t)f.w) s_bg_xs.resize((size_t)f.w);
          std::fill(s_bg_xs.begin(), s_bg_xs.begin() + f.w, -1);
          for (int col = 0; col < f.w; ++col) {
            int rel = col - ox;
            if (f.bg_repeat & 1) { rel %= tw; if (rel < 0) rel += tw; }
            else if (rel < 0 || rel >= tw) continue;
            int sx = (int)((long long)rel * bm->w / tw);
            s_bg_xs[(size_t)col] = sx < bm->w ? sx : bm->w - 1;
          }
          for (int row = 0; row < f.h; ++row) {
            const int py = y + row;
            if (py < view_y || py >= view_y + view_h) continue;
            int rel = row - oy;
            if (f.bg_repeat & 2) { rel %= th; if (rel < 0) rel += th; }
            else if (rel < 0 || rel >= th) continue;
            int sy = (int)((long long)rel * bm->h / th);
            if (sy >= bm->h) sy = bm->h - 1;
            const uint32_t* srow = &bm->pixels[(size_t)sy * bm->w];
            for (int col = 0; col < f.w; ++col) {
              const int sx = s_bg_xs[(size_t)col];
              if (sx < 0) continue;
              const int px = f.x + col;
              if (px < 0 || px >= view_w || !clip.has(px, py)) continue;
              const uint32_t p = srow[sx];
              const int a = (int)((p >> 24) & 0xFF) * (int)f.opacity / 255;
              if (a) c.blend_srgb_to(win.buffer, w, h, px, py, p, a);
            }
          }
          break;
        }
        if (bm && !bm->empty() && f.w > 0 && f.h > 0) {
          // Nearest-neighbour into place, clipped to the viewport, with the
          // image's own alpha honoured so a transparent PNG shows the page
          // behind it rather than a black box. `object-fit` / `object-position`
          // decide which part of the box the picture covers.
          int dx = 0, dy = 0, dw = f.w, dh = f.h;
          if (f.fit != 0) {
            int nw, nh;
            natural(nw, nh);
            if (nw > 0 && nh > 0) {
              const float sx = (float)f.w / (float)nw, sy2 = (float)f.h / (float)nh;
              float sc = 1.0f;
              if (f.fit == 1) sc = sx < sy2 ? sx : sy2;                    // contain
              else if (f.fit == 2) sc = sx > sy2 ? sx : sy2;               // cover
              else if (f.fit == 4) sc = (sx < sy2 ? sx : sy2) < 1.0f ? (sx < sy2 ? sx : sy2) : 1.0f;  // scale-down
              dw = (int)((float)nw * sc + 0.5f);
              dh = (int)((float)nh * sc + 0.5f);
              if (dw < 1) dw = 1;
              if (dh < 1) dh = 1;
              dx = (int)((float)(f.w - dw) * f.pos_x / 100.0f);
              dy = (int)((float)(f.h - dh) * f.pos_y / 100.0f);
            }
          }
          for (int row = 0; row < f.h; ++row) {
            int py = y + row;
            if (py < view_y || py >= view_y + view_h) continue;
            const int rrow = row - dy;
            if (rrow < 0 || rrow >= dh) continue;
            int sy = bm->h > 1 ? (int)((long long)rrow * bm->h / dh) : 0;
            if (sy >= bm->h) sy = bm->h - 1;
            for (int col = 0; col < f.w; ++col) {
              int px = f.x + col;
              if (px < 0 || px >= view_w) continue;
              const int rcol = col - dx;
              if (rcol < 0 || rcol >= dw) continue;
              int sx = bm->w > 1 ? (int)((long long)rcol * bm->w / dw) : 0;
              if (sx >= bm->w) sx = bm->w - 1;
              uint32_t p = bm->pixels[(size_t)sy * bm->w + sx];
              if (!clip.has(px, py)) continue;
              int a = (int)((p >> 24) & 0xFF) * (int)f.opacity / 255;
              if (a >= 255) win.buffer[(size_t)py * (size_t)w + (size_t)px] = p;
              else if (a > 0) c.blend_srgb_to(win.buffer, w, h, px, py, p, a);
            }
          }
          break;
        }
        // No picture: say what it was meant to be, in a box, which is the
        // honest thing and also shows the layout reserved the space.
        if (!f.opacity) break;
        int box_h = f.h > 0 ? f.h : 22;
        int box_w = f.w > 0 ? f.w : 120;
        c.draw_rect_outline_to(win.buffer, w, h, f.x, y, box_w, box_h, 0xFF3A3A3A);
        std::string label = f.alt.empty() ? std::string("image") : f.alt;
        web::FontKey small{11, false, true, false};
        if (y + 3 >= view_y && y + 3 < view_y + view_h)
          web_draw_text(c, win.buffer, w, h, f.x + 4, y + (box_h - 13) / 2, label, kFaint, small);
        break;
      }
      case web::Fragment::Kind::Text: {
        if (f.text.empty()) break;
        uint32_t ink = with_opacity(f.color, f.opacity);
        if (!(ink >> 24)) break;
        bool hot = f.link >= 0 && f.link == pane.hover_link;
        if (hot) ink = with_opacity(0xFFB8DBFF, f.opacity);
        if ((s_sel_all || s_sel_any) && frag_selected(f)) {
          c.draw_rect_to(win.buffer, w, h, f.x, y, f.w, f.h > 0 ? f.h : web_line_height(f.font), 0xFF2F5FB0);
          ink = 0xFFFFFFFF;
        }
        // Find-in-page matches: a yellow box under the matched letters (orange for the current one), the letters redrawn dark.
        if (!find_marks.empty()) {
          std::vector<std::pair<int, int>> found_spans;
          const size_t fi = (size_t)(&f - doc.fragments.data());
          while (find_at < find_marks.size() && find_marks[find_at].frag < fi) ++find_at;
          for (size_t q = find_at; q < find_marks.size() && find_marks[q].frag == fi; ++q) {
            const int mx = f.x + web_text_width(f.text.substr(0, find_marks[q].off), f.font);
            const int mw = web_text_width(f.text.substr(find_marks[q].off, find_marks[q].len), f.font);
            c.draw_rect_to(win.buffer, w, h, mx, y, mw, f.h > 0 ? f.h : web_line_height(f.font),
                           (int)q == s_find_cur ? 0xFFFF9632 : 0xFFF2D14A);
            found_spans.emplace_back((int)q, mx);
          }
          web_draw_text(c, win.buffer, w, h, f.x, y, f.text, ink, f.font, clip);
          for (const auto& sp : found_spans) {
            const FindHit& hit = find_marks[(size_t)sp.first];
            web_draw_text(c, win.buffer, w, h, sp.second, y, f.text.substr(hit.off, hit.len), 0xFF000000, f.font, clip);
          }
        } else {
          web_draw_text(c, win.buffer, w, h, f.x, y, f.text, ink, f.font, clip);
        }
        int asc = web_ascent(f.font);
        // Decoration lines scale with the type, as a browser draws them from the face's metrics: about a
        // fourteenth of the size thick, an underline a tenth of the size below the baseline, in the full ink.
        const int deco_t = std::max(1, (int)std::lround(f.font.px / 14.0f));
        if (f.underline) {
          const int uy = y + asc + std::max(1, (int)std::lround(f.font.px * 0.1f));
          const int top = std::max(uy, clip.y0), bot = std::min(uy + deco_t, clip.y1);
          if (bot > top) c.draw_rect_to(win.buffer, w, h, f.x, top, f.w, bot - top, ink);
        }
        if (f.strike) {
          const int sy = y + asc - (int)std::lround(f.font.px * 0.3f);
          const int top = std::max(sy, clip.y0), bot = std::min(sy + deco_t, clip.y1);
          if (bot > top) c.draw_rect_to(win.buffer, w, h, f.x, top, f.w, bot - top, ink);
        }
        break;
      }
    }
  }
  };
  {
    std::vector<size_t> all(doc.fragments.size());
    for (size_t i = 0; i < all.size(); ++i) all[i] = i;
    paint_list(all, -1);
  }

  // Inner scroll areas (`overflow: auto`/`scroll` with more in them than fits): a thin bar inside the right edge,
  // so a person can see the box scrolls and how far along it is.
  for (const auto& sa : doc.scrollers) {
    if (sa.content_h <= sa.h || sa.h < 16 || sa.w < 16) continue;
    const int top = sa.y - pane.scroll + kChromeH;
    if (top + sa.h <= view_y || top >= view_y + view_h) continue;
    const int off = pane.scroll_offset_of(sa.node);
    const int max_off = sa.max_offset();
    int thumb_h = (int)((int64_t)sa.h * sa.h / sa.content_h);
    if (thumb_h < 12) thumb_h = 12;
    if (thumb_h > sa.h - 4) thumb_h = sa.h - 4;
    const int thumb_y = top + 2 + (max_off > 0 ? (int)((int64_t)off * (sa.h - 4 - thumb_h) / max_off) : 0);
    const int bar_x = sa.x + sa.w - 8;
    DrawClip area;
    area.x0 = std::max(0, sa.x);
    area.x1 = std::min(view_w, sa.x + sa.w);
    area.y0 = std::max(view_y, top);
    area.y1 = std::min(view_y + view_h, top + sa.h);
    if (area.x1 <= area.x0 || area.y1 <= area.y0) continue;
    const uint32_t thumb = light_page ? kScrollThumbLight : kScrollThumb;
    for (int py = std::max(area.y0, thumb_y); py < std::min(area.y1, thumb_y + thumb_h); ++py)
      for (int px = std::max(area.x0, bar_x); px < std::min(area.x1, bar_x + 6); ++px) {
        // rounded ends: the corners of the 6px bar are left out
        const bool corner = (px == bar_x || px == bar_x + 5) && (py == thumb_y || py == thumb_y + thumb_h - 1);
        if (!corner && py >= 0 && py < h && px >= 0 && px < w) win.buffer[(size_t)py * (size_t)w + (size_t)px] = thumb;
      }
  }

  // The focused field, marked: a ring around its box and a caret after its
  // text. Without this a person types and has no idea which box is listening --
  // and a field whose placeholder still shows looks like it took nothing.
  // The ring follows the control's own corners: a pill gets a pill-shaped ring.
  if (web::Node* focused = live_field(pane)) {
    bool ringed = false;
    for (const auto& f : doc.fragments) {
      if (f.node != focused) continue;
      int y = f.y - pane.scroll + kChromeH;
      if (f.kind == web::Fragment::Kind::Border || f.kind == web::Fragment::Kind::Rect) {
        if (!ringed && y + f.h > view_y && y < view_y + view_h) {
          DrawClip page;
          page.x0 = 0; page.x1 = view_w; page.y0 = view_y; page.y1 = view_y + view_h;
          const int fh = f.h > 0 ? f.h : 20;
          stroke_round_rect(c, win.buffer, w, h, f.x - 2, y - 2, f.w + 4, fh + 4,
                            light_page ? kFocusRingLight : kFieldFocus, f.radius > 0 ? f.radius + 2 : 3, 2, 2, 2, 2, page);
          ringed = true;
        }
        continue;
      }
      if (f.kind != web::Fragment::Kind::Text) continue;
      // After the text if there is any, at the left edge if the box is empty
      // (what is drawn then is the placeholder, which is not the value).
      const std::string v = field_value(focused);
      int caret_x = v.empty() ? f.x : f.x + web_text_width(f.text, f.font);
      int lh = web_line_height(f.font);
      if (y >= view_y && y < view_y + view_h)
        c.draw_rect_to(win.buffer, w, h, caret_x + 1, y + 2, 2, lh > 4 ? lh - 4 : lh, kFieldFocus);
    }
  }

  // What the page painted for itself, over its document.
  if (pane.browser->js() && !pane.browser->js()->paint_ops().empty())
    draw_paint_ops(c, win, w, h, view_y, view_w, view_h, pane.browser->js()->paint_ops());

  // Nothing loaded yet: the pane says what it is and what to type, rather than
  // showing an empty grey rectangle that looks like a failure.
  if (!doc.dom) {
    web::FontKey title{22, true, false, false};
    web::FontKey body{14, false, false, false};
    const char* lines[] = {
        "This machine's own web engine.",
        "The tokeniser, the cascade, the box layout and the painter are all in this kernel.",
        "No WebKit, no Gecko, no host browser: there is nothing under this but the NIC.",
        "",
        "Type a url above and press Enter, or run  browser <url>  at the command surface.",
    };
    int ty = view_y + 40;
    web_draw_text(c, win.buffer, w, h, 26, ty, "Unblink", kInk, title);
    ty += 34;
    for (const char* l : lines) {
      web_draw_text(c, win.buffer, w, h, 26, ty, l, kFaint, body);
      ty += 22;
    }
  }

  // -- the scrollbar -------------------------------------------------------
  if (doc.content_height > view_h) {
    c.draw_rect_to(win.buffer, w, h, view_w, view_y, kScrollW, view_h, light_page ? kScrollTrackLight : kScrollTrack);
    int thumb_h = view_h * view_h / doc.content_height;
    if (thumb_h < 20) thumb_h = 20;
    int span = view_h - thumb_h;
    int thumb_y = view_y + (max_scroll > 0 ? pane.scroll * span / max_scroll : 0);
    c.draw_rounded_rect_to(win.buffer, w, h, view_w + 2, thumb_y, kScrollW - 4, thumb_h, 2, light_page ? kScrollThumbLight : kScrollThumb);
  }

  if (zoomed) {
    // The page raster (CSS pixels) onto the pane (device pixels), nearest sample. The scrollbar is drawn real, not scaled.
    win.buffer.swap(s_zoom_scratch);
    static thread_local std::vector<int> s_zoom_xs;
    if (s_zoom_xs.size() < (size_t)real_view_w) s_zoom_xs.resize((size_t)real_view_w);
    for (int rx = 0; rx < real_view_w; ++rx) s_zoom_xs[(size_t)rx] = std::min(view_w - 1, rx * 100 / s_zoom);
    for (int ry = 0; ry < real_view_h; ++ry) {
      const int sy = std::min(kChromeH + view_h - 1, kChromeH + ry * 100 / s_zoom);
      const size_t src = (size_t)sy * (size_t)w, dst = (size_t)(kChromeH + ry) * (size_t)real_w;
      if (dst + (size_t)real_view_w > win.buffer.size() || src + (size_t)view_w > s_zoom_scratch.size()) break;
      for (int rx = 0; rx < real_view_w; ++rx) win.buffer[dst + (size_t)rx] = s_zoom_scratch[src + (size_t)s_zoom_xs[(size_t)rx]];
    }
    if (doc.content_height > view_h) {
      c.draw_rect_to(win.buffer, real_w, real_h, real_view_w, kChromeH, kScrollW, real_view_h, light_page ? kScrollTrackLight : kScrollTrack);
      int thumb_h = std::max(20, (int)((int64_t)real_view_h * view_h / doc.content_height));
      int span = real_view_h - thumb_h;
      int thumb_y = kChromeH + (max_scroll > 0 ? pane.scroll * span / max_scroll : 0);
      c.draw_rounded_rect_to(win.buffer, real_w, real_h, real_view_w + 2, thumb_y, kScrollW - 4, thumb_h, 2, light_page ? kScrollThumbLight : kScrollThumb);
    }
  }
  if (s_find_open) {
    // The find bar, top right of the page: what is typed, and which match of how many.
    const auto n = find_marks.size();
    std::string label = "Find: " + s_find_text + "|";
    std::string tally = s_find_text.empty() ? std::string() : n ? std::to_string(s_find_cur + 1) + "/" + std::to_string(n) : std::string("0/0");
    const web::FontKey ff{13, false, false, false};
    const int bw = 300, bx = std::max(0, real_view_w - bw - 8), by = kChromeH + 6;
    c.draw_rect_to(win.buffer, real_w, real_h, bx, by, bw, 26, 0xFF262626);
    c.draw_rect_outline_to(win.buffer, real_w, real_h, bx, by, bw, 26, n || s_find_text.empty() ? 0xFF6E6E6E : 0xFFE8746A);
    web_draw_text(c, win.buffer, real_w, real_h, bx + 8, by + 6, label, 0xFFFFFFFF, ff);
    web_draw_text(c, win.buffer, real_w, real_h, bx + bw - 8 - web_text_width(tally, ff), by + 6, tally, 0xFFA0A0A0, ff);
  }
  }   // !unchanged
  w = real_w; h = real_h; view_w = real_view_w; view_h = real_view_h;

  // The window's title is the page's, whichever thread loaded it.
  if (doc.dom && !doc.title.empty()) {
    std::string t = doc.title;
    if (t.size() > 48) t = t.substr(0, 45) + "...";
    if (win.title != t) win.title = t;
  }

  // -- the chrome and the status strip ---------------------------------------
  ChromeState st;
  st.url = doc.url;
  st.secure = pane.browser->secure();
  st.can_back = pane.browser->can_go_back();
  st.can_fwd = pane.browser->can_go_forward();
  st.loaded = doc.dom != nullptr;
  if (!pane.working.empty()) {
    // A page in the middle of its own work says so. An application whose
    // answer takes a minute otherwise looks like one that swallowed the
    // question.
    static const char* kSpin = "|/-\\";
    st.status = std::string("the page is working (") + pane.working + ") " +
                std::string(1, kSpin[(spin_tick() / 6) % 4]);
    st.status_ink = kInk;
  } else if (pane.hover_link >= 0 && (size_t)pane.hover_link < doc.links.size()) {
    st.status = doc.links[(size_t)pane.hover_link].href;
  } else if (!pane.message.empty()) {
    st.status = pane.message;
    if (pane.error) st.status_ink = kErrorInk;
  } else if (doc.dom) {
    st.status = doc.title + "  ·  " + dec(doc.fragments.size()) + " boxes, " + dec(doc.links.size()) +
                " links, " + dec((uint64_t)doc.content_height) + "px";
    if (!pane.browser->transport().empty()) st.status += "  ·  " + pane.browser->transport();
  } else {
    st.status = "no page loaded";
  }
  draw_pane_chrome(c, win, pane, w, h, st);
}
}  // namespace

// ------------------------------------------------------------------ input --

namespace {

// A navigation, off the thread that draws. Loading a page waits on the network
// for every stylesheet and script and then lays the whole thing out, and done
// on the caller's thread that froze the desktop for the length of it -- no
// frames, no input, for four or five seconds on a big page. The page worker
// does it instead; the frame loop keeps running and shows the page as it comes.
void navigate_async(WindowSurface& win, int view_w, const char* what,
                    std::function<Result<void>(WebPane&)> nav) {
  if (!win.web) return;
  win.web->url_focus = false;
  run_page_work(win.web, view_w, what, [nav](WebPane& p, int& wrote) {
    auto r = nav(p);
    p.scroll = 0;
    p.hover_link = -1;
    p.url_focus = false;
    p.last_tick_ns = 0;
    if (r.ok()) {
      p.browser->fetch_images_range(24, 0, 3000);
      p.message.clear();
      p.error = false;
    } else {
      p.message = r.error().message.empty() ? std::string("could not load the page") : r.error().message;
      p.error = true;
    }
    p.url_text = p.browser->document().url;
    wrote = 0;
  }, true, true);
}

}  // namespace

void web_pane_go(HardwareCompositor& c, WindowSurface& win, Kernel& k, const std::string& url) {
  if (!win.web) win.web = std::make_shared<WebPane>();
  WebPane& pane = *win.web;
  if (!pane.browser) pane.browser = std::make_shared<web::Browser>(kernel_fetch(k, pane.guard));

  std::string target = url;
  if (is_face_url(target)) { web_pane_open_face(c, win, pane, k, target); return; }
  pane.face.reset();   // a page after a face: the engine is back
  pane.face_url.clear();
  // A bare host is a url. Typing "example.com" and getting an error is not a
  // browser.
  if (target.find("://") == std::string::npos) {
    std::string low = lowered(target);
    if (low.compare(0, 7, "mailto:") != 0 && low.compare(0, 5, "data:") != 0 && low.compare(0, 11, "javascript:") != 0) {
      // What is typed in the address bar is a search unless it looks like an address: words (a space), or a single word with no
      // dot, colon or slash in it ("localhost" and "host:8080" are still hosts). Searched on Brave, which answers a plain page
      // without script; Google answers a program, and this engine gets its "if you're having trouble" page and nothing else.
      const bool spaced = target.find(' ') != std::string::npos;
      const bool hostlike = target.find_first_of("./:") != std::string::npos || low == "localhost";
      if (spaced || !hostlike) {
        std::string q;
        static const char* kHex = "0123456789ABCDEF";
        for (unsigned char ch : target) {
          if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~') q += (char)ch;
          else if (ch == ' ') q += '+';
          else { q += '%'; q += kHex[ch >> 4]; q += kHex[ch & 15]; }
        }
        target = "https://search.brave.com/search?q=" + q;
      } else {
        target = "http://" + target;
      }
    }
  }

  int view_w = win.content_width() - kScrollW;
  if (view_w < 80) view_w = 80;
  view_w = std::max(80, zoom_px(view_w));   // the page is laid out in CSS pixels
  pane.last_viewport_w = view_w;
  int view_h = win.content_height() - kChromeH - kStatusH;
  if (view_h < 40) view_h = 40;
  web_pane_bind(c, pane, k, target, view_h);
  pane.url_text = target;
  navigate_async(win, view_w, "loading", [target, view_w](WebPane& p) {
    return p.browser->go(target, pane_metrics(), view_w);
  });
  (void)c;
}

void web_pane_motion(WindowSurface& win, int rel_x, int rel_y) {
  if (win.web && win.web->face) return;
  if (!win.web || !win.web->browser) return;
  WebPane& pane = *win.web;
  int page_y = zoom_px(rel_y - kChromeH) + pane.scroll;
  if (rel_y < kChromeH || rel_y > win.content_height() - kStatusH) {
    pane.hover_link = -1;
    return;
  }
  rel_x = zoom_px(rel_x);   // layout coordinates, not device ones
  pane.last_px = rel_x;
  pane.last_py = page_y;
  pane.hover_link = pane.browser->link_at(rel_x, page_y);
  s_hover_dirty = true;
  if (s_sel_drag && (rel_x != s_sel_bx || page_y != s_sel_by)) {
    s_sel_bx = rel_x; s_sel_by = page_y;
    s_sel_any = std::abs(s_sel_bx - s_sel_ax) > 3 || std::abs(s_sel_by - s_sel_ay) > 3;
    ++s_sel_gen;
  }
}

void web_pane_release() { s_sel_drag = false; clear_active(); }

void web_pane_wheel(WindowSurface& win, int notches) {
  if (win.web && win.web->face) return;   // a face has no scroll
  if (!win.web) return;
  WebPane& pane = *win.web;
  const int step = notches * 3 * 21;      // a notch is three lines

  // The innermost `overflow: auto` box under the pointer takes the wheel, and
  // only what it cannot use passes to the page. Without this a page's own
  // scroll panes were frames with content cut off at the bottom and no way to
  // reach the rest.
  if (pane.browser && pane.last_px >= 0) {
    const auto& doc = pane.browser->document();
    const web::ScrollArea* best = nullptr;
    for (const auto& sa : doc.scrollers) {
      const int vy = sa.y - pane.scroll;   // the frame sits in the page, which scrolls too
      if (pane.last_px < sa.x || pane.last_px >= sa.x + sa.w) continue;
      if (pane.last_py < vy || pane.last_py >= vy + sa.h) continue;
      // Later areas are inner ones: layout finishes a child before its parent.
      best = &sa;
    }
    if (best && best->max_offset() > 0) {
      int at = pane.scroll_offset_of(best->node) - step;
      if (at < 0) at = 0;
      if (at > best->max_offset()) at = best->max_offset();
      pane.set_scroll_offset(best->node, at);
      return;
    }
  }

  pane.scroll -= step;
  if (pane.scroll < 0) pane.scroll = 0;
}


// A form the person filled in and pressed. The engine works out what the
// request is; the pane's job is only to send it and take the page that comes
// back. Until this was here a <button> inside a <form> did nothing at all,
// which is a browser that can show an application and never operate one.
bool submit_the_form_on(WebPane& pane, web::Node* form, web::Node* submitter, int view_w) {
  const web::FormRequest req =
      web::form_request(*form, submitter, pane.browser->document().base, pane.browser->document().url);
  auto r = pane.browser->submit_form(req.url, req.options, pane_metrics(), view_w);
  if (!r.ok()) {
    pane.message = r.error().message.empty() ? ("could not submit to " + req.url) : r.error().message;
    pane.error = true;
    return false;
  }
  pane.browser->fetch_images_range(24, 0, 3000);
  pane.scroll = 0;
  pane.hover_link = -1;
  pane.message.clear();
  pane.error = false;
  return true;
}

// What the page's script asked the pane to do while its handler ran: send a
// form (`form.submit()`, `requestSubmit()`, a submit button's `click()`) or go
// to an address (`location.assign`, `location.href = ...`). The script cannot
// fetch a page itself -- the pane owns the browser -- so this runs once the
// handler has returned. True when the page was replaced, so nothing else about
// the old one applies.
bool follow_script_requests(WebPane& pane, int view_w) {
  auto js = pane.browser->js();
  if (!js) return false;
  web::Node* form = nullptr;
  web::Node* by = nullptr;
  if (js->take_submit_request(form, by)) {
    if (!form || !submit_the_form_on(pane, form, by, view_w)) return false;
    pane.field = nullptr;
    pane.field_all = false;
    return true;
  }
  const std::string url = js->take_navigation_request();
  if (url.empty()) return false;
  auto r = pane.browser->go(url, pane_metrics(), view_w);
  if (!r.ok()) {
    pane.message = r.error().message.empty() ? ("could not go to " + url) : r.error().message;
    pane.error = true;
    return false;
  }
  pane.browser->fetch_images_range(24, 0, 3000);
  pane.scroll = 0;
  pane.hover_link = -1;
  pane.message.clear();
  pane.error = false;
  pane.field = nullptr;
  pane.field_all = false;
  return true;
}

// The same, plus the window title — for callers that have a window.
void submit_the_form(WindowSurface& win, WebPane& pane, web::Node* form, web::Node* submitter,
                     int view_w) {
  if (!submit_the_form_on(pane, form, submitter, view_w)) return;
  win.title = pane.browser->document().title;
  if (win.title.size() > 48) win.title = win.title.substr(0, 45) + "...";

}

void web_pane_click(HardwareCompositor& c, WindowSurface& win, Kernel& k, int rel_x, int rel_y) {
  if (!win.web) win.web = std::make_shared<WebPane>();
  WebPane& pane = *win.web;
  if (!pane.browser) pane.browser = std::make_shared<web::Browser>(kernel_fetch(k, pane.guard));
  int w = win.content_width();
  int h = win.content_height();
  Chrome g = chrome_geom(w);
  int view_w = std::max(80, zoom_px(w - kScrollW));   // the layout width; the scrollbar below is hit in device pixels

  // A face takes every click between the chrome and the status strip, in its
  // own pixels.
  if (pane.face && rel_y >= kChromeH && rel_y < h - kStatusH) {
    pane.url_focus = false;
    pane.face->click(rel_x, rel_y - kChromeH);
    return;
  }

  if (rel_y < kChromeH) {
    auto in_btn = [&](int bx) { return rel_x >= bx && rel_x < bx + g.btn_w; };
    web::Metrics m = pane_metrics();
    if (pane.face && in_btn(g.reload_x)) { web_pane_go(c, win, k, pane.face_url); return; }
    if (in_btn(g.back_x)) {
      navigate_async(win, view_w, "going back", [m, view_w](WebPane& p) { return p.browser->back(m, view_w); });
      return;
    }
    if (in_btn(g.fwd_x)) {
      navigate_async(win, view_w, "going forward", [m, view_w](WebPane& p) { return p.browser->forward(m, view_w); });
      return;
    }
    if (in_btn(g.reload_x)) {
      navigate_async(win, view_w, "reloading", [m, view_w](WebPane& p) { return p.browser->reload(m, view_w); });
      return;
    }
    if (rel_x >= g.field_x && rel_x < g.field_x + g.field_w) {
      // Already focused: a second click means "put the caret here", so leave
      // what is there alone rather than re-arming the replace.
      if (!pane.url_focus) {
        pane.url_focus = true;
        pane.url_text = pane.browser->document().url;
        pane.url_fresh = true;
      }
      return;
    }
    return;
  }
  pane.url_focus = false;
  if (rel_y > h - kStatusH) return;

  // The scrollbar: a click jumps the thumb to the pointer.
  const web::Document& doc = pane.browser->document();
  int view_h = h - kChromeH - kStatusH;
  if (rel_x >= w - kScrollW && doc.content_height > zoom_px(view_h)) {
    int frac = rel_y - kChromeH;
    int max_scroll = doc.content_height - view_h;
    pane.scroll = view_h > 0 ? frac * max_scroll / view_h : 0;
    if (pane.scroll < 0) pane.scroll = 0;
    if (pane.scroll > max_scroll) pane.scroll = max_scroll;
    return;
  }

  int page_y = zoom_px(rel_y - kChromeH) + pane.scroll;
  rel_x = zoom_px(rel_x);

  // The page's own handlers first. A modern interface binds a click on a
  // button or a row rather than on a link, so without this the whole of it was
  // inert — every element drew and nothing answered.
  // What was clicked, and whether it is something you type into. Focus moves on
  // every click in the page area -- clicking away from a field ends its turn
  // with the keyboard, which is what a person expects.
  web::Node* clicked = pane.browser->node_at(rel_x, page_y);
  if (::g_css_has_active) set_active(pane, clicked);
  pane.field = field_ancestor(clicked);
  if (pane.field) pane.message = "typing into <" + pane.field->tag + ">";
  sel_clear();
  if (!pane.field && pane.browser->link_at(rel_x, page_y) < 0 && !web::is_submit_control(clicked) &&
      !(pane.browser->js() && clicked && pane.browser->js()->has_handler(clicked, "click"))) {
    s_sel_ax = s_sel_bx = rel_x; s_sel_ay = s_sel_by = page_y;
    s_sel_drag = true;
  }

  if (pane.browser->js() && clicked && pane.browser->js()->has_handler(clicked, "click")) {
    // Off the render thread: a click on Send is a whole turn of an agent, and
    // this used to hold the frame until it came back.
    const int hit_link = pane.browser->link_at(rel_x, page_y);
    run_page_work(win.web, view_w, "click", [clicked, hit_link, vw = view_w](WebPane& p, int& wrote) {
      bool prevented = false;
      // A checkbox or radio changes BEFORE the click's handlers run, so they read the new state; a handler that
      // prevents the click puts it back, and one that does not is followed by `input` and `change`.
      // (dispatch() SETS its count of DOM writes, so each call gets its own and they are added up.)
      web::Node* box = toggle_target(clicked);
      std::vector<std::pair<web::Node*, bool>> was;
      if (box) was = activate_checkable(box);
      const int ran = p.browser->js()->dispatch(clicked, "click", &wrote, nullptr, &prevented);
      if (box) {
        ++wrote;   // the control's own change, or putting it back: either way the page is drawn again
        if (prevented) {
          for (const auto& [n, on] : was) set_checked(n, on);
        } else {
          int more = 0;
          p.browser->js()->dispatch(box, "input", &more);
          wrote += more;
          more = 0;
          p.browser->js()->dispatch(box, "change", &more);
          wrote += more;
        }
      }
      // The handler may have sent a form or navigated, and if it did not take
      // the click, the click still does what it always does: a submit button
      // sends its form and a link goes where it points. A handler used to
      // swallow both, so a page that listens for clicks on its own search
      // button could never be searched.
      if (follow_script_requests(p, vw)) { wrote = 0; return; }
      if (!prevented) {
        if (web::is_submit_control(clicked)) {
          if (web::Node* form = web::enclosing_form(clicked)) {
            if (submit_the_form_on(p, form, clicked, vw)) { p.field = nullptr; wrote = 0; return; }
          }
        } else if (hit_link >= 0) {
          if (p.browser->follow((size_t)hit_link, pane_metrics(), vw).ok()) {
            p.browser->fetch_images_range(24, 0, 3000);
            p.scroll = 0;
            p.hover_link = -1;
            p.message.clear();
            p.error = false;
            wrote = 0;
            return;
          }
        }
      }
      p.message = (p.field ? "typing into <" + p.field->tag + ">, " : std::string()) +
                  std::to_string(ran) + " handler(s) ran" +
                  (wrote > 0 ? ", " + std::to_string(wrote) + " dom change(s)" : std::string());
      if (!p.browser->js()->last_error().empty())
        p.message += " - handler failed: " + p.browser->js()->last_error();
      p.error = false;
    });
    return;
  }

  // A checkbox or radio (or its label) with no click handler on the way: it changes, the page's `input` and
  // `change` listeners run, and the page is laid out again to show it.
  // A script that listens for `input` or `change` (not `click`) still hears about it, as in a browser.
  if (web::Node* box = toggle_target(clicked)) {
    run_page_work(win.web, view_w, "a checkbox", [box, clicked](WebPane& p, int& wrote) {
      const auto was = activate_checkable(box);
      wrote = 1;
      if (!p.browser->js()) return;
      // dispatch() SETS its count of DOM writes, so each call gets its own and they are added up.
      bool prevented = false;
      int more = 0;
      p.browser->js()->dispatch(clicked, "click", &more, nullptr, &prevented);
      wrote += more;
      if (prevented) {
        for (const auto& [n, on] : was) set_checked(n, on);
        return;
      }
      more = 0;
      p.browser->js()->dispatch(box, "input", &more);
      wrote += more;
      more = 0;
      p.browser->js()->dispatch(box, "change", &more);
      wrote += more;
    });
    return;
  }

  // A submit control, before the link check: a <button> in a <form> is not a
  // link, and nothing below this would have looked at it.
  if (web::is_submit_control(clicked)) {
    if (web::Node* form = web::enclosing_form(clicked)) {
      navigate_async(win, view_w, "sending a form", [form, clicked, view_w](WebPane& p) {
        const web::FormRequest req =
            web::form_request(*form, clicked, p.browser->document().base, p.browser->document().url);
        return p.browser->submit_form(req.url, req.options, pane_metrics(), view_w);
      });
      return;
    }
  }

  int hit = pane.browser->link_at(rel_x, page_y);
  if (hit < 0) return;
  navigate_async(win, view_w, "following a link", [hit, view_w](WebPane& p) {
    return p.browser->follow((size_t)hit, pane_metrics(), view_w);
  });
  (void)c;
}

void web_pane_set_modifiers(bool shift, bool alt) { s_mod_shift = shift; s_mod_alt = alt; }

bool web_pane_text(WindowSurface& win, const std::string& text) {
  if (!win.web) return false;
  // The find bar has the keyboard while it is open: what is typed is what is looked for.
  if (s_find_open && !win.web->url_focus && !win.web->face && win.web->browser) {
    for (char ch : text) if ((unsigned char)ch >= 32 && (unsigned char)ch < 127) s_find_text += ch;
    s_find_cur = 0;
    find_reveal(*win.web, zoom_px(win.content_height() - kChromeH - kStatusH));
    return true;
  }
  // The page's field first: while a person is typing into an application, the
  // url bar is not what they mean.
  if (!win.web->url_focus) {
    WebPane& pane = *win.web;
    if (pane.face) {
      // Into the focused field, or as key presses the face's `on key` hears.
      if (pane.face->has_focus()) {
        std::string typed;
        for (char ch : text) if ((unsigned char)ch >= 32) typed += ch;
        pane.face->type(typed);
      } else {
        for (char ch : text) {
          if ((unsigned char)ch < 32) continue;
          const std::string name(1, ch);
          pane.face->key(name, true);
          pane.held.emplace_back(name, pane.last_tick_ns / 1000000ull);
        }
      }
      return true;
    }
    web::Node* f = live_field(pane);
    if (!f) {
      // No field has the keyboard: a page that listens for raw keys (a game)
      // still hears each character as a keydown/keyup pair. Letters and
      // digits arrive here as typed text on the hosted input path, not as
      // named keys (those go through web_pane_key), so without this a game
      // bound only to aos.onKey/keydown never heard 'a' or '1'. Synchronous,
      // not run_page_work: a game's key handler is expected to be fast, the
      // way a frame handler is, not a slow network call.
      if (pane.browser && pane.browser->js() && pane.browser->js()->wants_keys()) {
        auto js = pane.browser->js();
        const int view_w = pane.last_viewport_w > 80 ? pane.last_viewport_w : 800;
        int wrote = 0;
        for (char ch : text) {
          if ((unsigned char)ch < 32) continue;
          const std::string name(1, ch);
          js->key(name, true, &wrote);
          pane.held.emplace_back(name, pane.last_tick_ns / 1000000ull);
        }
        if (wrote > 0) pane.browser->relayout(pane_metrics(), view_w);
        return true;
      }
      // Space pages down when nothing on the page wants the keyboard, as in
      // every browser.
      if (text == " " && !pane.url_focus && !pane.face && pane.browser) {
        // Shift+Space is the same page the other way.
        const int page = zoom_px(win.content_height() - kChromeH - kStatusH) - 30;
        pane.scroll = s_mod_shift ? std::max(0, pane.scroll - page) : pane.scroll + page;
        return true;
      }
      return false;
    }
    std::string typed;
    for (char ch : text)
      if ((unsigned char)ch >= 32) typed += ch;
    if (typed.empty()) return true;
    int view_w = win.content_width() - kScrollW;
    if (view_w < 80) view_w = 80;
    view_w = std::max(80, zoom_px(view_w));
    const bool replace_all = win.web->field_all;
    win.web->field_all = false;
    run_page_work(win.web, view_w, "typing", [f, typed, replace_all](WebPane& p, int& wrote) {
      std::string v = (replace_all ? std::string() : field_value(f)) + typed;
      field_set_value(f, v);
      if (p.browser->js()) {
        int w = 0;
        p.browser->js()->dispatch(f, "input", &w);
        wrote += w;
        w = 0;
        p.browser->js()->dispatch(f, "keyup", &w);
        wrote += w;
      }
      // A field the page just took text into always needs laying out again:
      // the box grows, and the caret has to follow it.
      wrote = wrote > 0 ? wrote : 1;
    }, /*exclusive=*/false);
    return true;
  }
  for (char ch : text) {
    if ((unsigned char)ch < 32) continue;
    // The first character after focusing replaces the address, the way a
    // select-all would.
    if (win.web->url_fresh) { win.web->url_text.clear(); win.web->url_fresh = false; }
    win.web->url_text += ch;
  }
  return true;
}

bool web_pane_key(HardwareCompositor& c, WindowSurface& win, Kernel& k, int key) {
  using namespace aios::hal;
  if (!win.web) win.web = std::make_shared<WebPane>();
  WebPane& pane = *win.web;
  if (!pane.browser) pane.browser = std::make_shared<web::Browser>(kernel_fetch(k, pane.guard));
  int h = win.content_height();
  int view_h = zoom_px(h - kChromeH - kStatusH);          // page keys work in layout pixels, so zoomed they page by less
  int view_w = std::max(80, zoom_px(win.content_width() - kScrollW));
  web::Metrics m = pane_metrics();

  if (pane.url_focus) {
    if (key == KeyEnter) {
      std::string want = pane.url_text;
      if (!want.empty()) web_pane_go(c, win, k, want);
      pane.url_focus = false;
      return true;
    }
    if (key == KeyBackspace) {
      // On a freshly focused field this is select-all + delete, which empties
      // it — not "shave one character off the address you did not type".
      if (pane.url_fresh) { pane.url_text.clear(); pane.url_fresh = false; return true; }
      if (!pane.url_text.empty()) pane.url_text.erase(pane.url_text.size() - 1);
      return true;
    }
    if (key == KeyLeft || key == KeyRight || key == KeyHome || key == KeyEnd) {
      // Moving in the field means the person means to EDIT what is there
      // rather than replace it.
      pane.url_fresh = false;
      return true;
    }
    if (key == KeyEscape) {
      pane.url_focus = false;
      return true;
    }
    // The four a person's hands already know. There is no selection model in
    // this pane, so select-all is the promise `url_fresh` already makes: the
    // next thing typed replaces the address instead of appending to it.
    if (key == KeyCtrlA) { pane.url_fresh = true; return true; }
    if (key == KeyCtrlC || key == KeyCtrlX) {
      if (!pane.url_text.empty()) hal::clipboard_set(pane.url_text);
      if (key == KeyCtrlX) { pane.url_text.clear(); pane.url_fresh = false; }
      pane.message = key == KeyCtrlC ? "copied the address" : "cut the address";
      pane.error = false;
      return true;
    }
    if (key == KeyCtrlV) {
      auto got = hal::clipboard_get();
      if (got.ok()) {
        if (pane.url_fresh) { pane.url_text.clear(); pane.url_fresh = false; }
        for (char ch : *got)
          if ((unsigned char)ch >= 32) pane.url_text += ch;
      }
      return true;
    }
    return true;   // while the field has the keyboard, it has all of it
  }

  if (pane.face) {
    const char* name = key == KeyEnter ? "Enter" : key == KeyBackspace ? "Backspace" : key == KeyEscape ? "Escape"
                     : key == KeyTab ? "Tab" : key == KeyLeft ? "ArrowLeft" : key == KeyRight ? "ArrowRight"
                     : key == KeyUp ? "ArrowUp" : key == KeyDown ? "ArrowDown" : key == KeyHome ? "Home"
                     : key == KeyEnd ? "End" : key == KeyPageUp ? "PageUp" : key == KeyPageDown ? "PageDown"
                     : key == KeyDelete ? "Delete" : nullptr;
    if (name) {
      const bool took = pane.face->key(name, true);
      pane.held.emplace_back(name, pane.last_tick_ns / 1000000ull);
      // Escape the face did not want stays the desktop's; everything else is
      // the face's whether or not it listened (a pane showing a face has no
      // page to scroll).
      if (took || key != KeyEscape) return true;
    }
    if (key == KeyCtrlL) {
      pane.url_focus = true;
      pane.url_fresh = true;
      pane.url_text = pane.face_url;
      return true;
    }
    return false;
  }

  // Browser shortcuts that outrank whatever the page has focused, as in Chrome.
  if (key == KeyCtrlF && pane.browser) {
    s_find_open = true;
    ++s_find_gen;
    pane.message = s_find_text.empty() ? "find: type to search the page" : "find: Enter next, Shift+Enter previous, Esc closes";
    pane.error = false;
    return true;
  }
  if (key == kWebKeyZoomIn || key == kWebKeyZoomOut || key == kWebKeyZoomReset) {
    if (key == kWebKeyZoomReset) s_zoom = 100; else zoom_step(key == kWebKeyZoomIn ? 1 : -1);
    ++s_find_gen;
    pane.message = "zoom " + std::to_string(s_zoom) + "%";
    pane.error = false;
    return true;
  }
  if (s_mod_alt && (key == KeyLeft || key == KeyRight) && pane.browser) {
    if (key == KeyLeft) navigate_async(win, view_w, "going back", [m, view_w](WebPane& p) { return p.browser->back(m, view_w); });
    else navigate_async(win, view_w, "going forward", [m, view_w](WebPane& p) { return p.browser->forward(m, view_w); });
    return true;
  }
  if (s_find_open && pane.browser) {
    if (key == KeyEscape) { s_find_open = false; ++s_find_gen; pane.message.clear(); return true; }
    if (key == KeyEnter) { s_find_cur += s_mod_shift ? -1 : 1; find_reveal(pane, view_h); return true; }
    if (key == KeyBackspace) {
      if (!s_find_text.empty()) s_find_text.erase(s_find_text.size() - 1);
      s_find_cur = 0;
      find_reveal(pane, view_h);
      return true;
    }
  }

  // A field on the page has the keyboard: Enter, Backspace and Escape belong to
  // it, and the page gets a keydown first so an application that handles Enter
  // itself (every composer does) can take it.
  if (web::Node* f = live_field(pane)) {
    // Copy, cut, paste and select-all on the field the page has focused. These
    // go straight to the value: they are the pane's own editing, not something
    // the page asked for, and every one of them was simply dropped before.
    if (key == KeyCtrlA) {
      pane.field_all = true;
      pane.message = "all of <" + f->tag + "> selected - typing replaces it";
      pane.error = false;
      return true;
    }
    if (key == KeyCtrlC || key == KeyCtrlX || key == KeyCtrlV) {
      if (pane.worker->busy()) return true;
      const bool cut = key == KeyCtrlX, paste = key == KeyCtrlV;
      std::string pasted;
      if (paste) {
        auto got = hal::clipboard_get();
        if (!got.ok()) return true;
        for (char ch : *got)
          if ((unsigned char)ch >= 32 || ch == '\n') pasted += ch;
      } else {
        hal::clipboard_set(field_value(f));
      }
      if (!cut && !paste) {
        pane.message = "copied <" + f->tag + ">";
        pane.error = false;
        return true;
      }
      const bool replace = pane.field_all || cut;
      pane.field_all = false;
      run_page_work(win.web, view_w, paste ? "paste" : "cut",
                    [f, pasted, replace, paste](WebPane& p, int& wrote) {
        std::string v = replace ? std::string() : field_value(f);
        if (paste) v += pasted;
        field_set_value(f, v);
        if (p.browser->js()) {
          int w = 0;
          p.browser->js()->dispatch(f, "input", &w);
          wrote += w;
        }
        wrote = wrote > 0 ? wrote : 1;
        p.message = paste ? "pasted into <" + f->tag + ">" : "cut <" + f->tag + ">";
        p.error = false;
      }, /*exclusive=*/false);
      return true;
    }

    EventInit init;
    if (key == KeyEnter) init.key = "Enter";
    else if (key == KeyBackspace) init.key = "Backspace";
    else if (key == KeyEscape) init.key = "Escape";
    else if (key == KeyTab) init.key = "Tab";

    if (!init.key.empty()) {
      if (pane.worker->busy()) return true;
      const bool textarea = f->tag == "textarea";
      const bool all = pane.field_all;
      pane.field_all = false;
      run_page_work(win.web, view_w, init.key == "Enter" ? "send" : "key",
                    [f, init, textarea, key, all, vw = view_w](WebPane& p, int& wrote) {
        // Every dispatch reports the writes IT made, so they have to be added
        // up. Passing the same counter to each one meant the last dispatch --
        // keyup, which changes nothing -- overwrote the count with zero, the
        // pane decided there was nothing to lay out, and every send landed in
        // the DOM without ever reaching the screen.
        auto fire = [&](const char* type, const EventInit* ev, bool* prevented) {
          if (!p.browser->js()) return 0;
          int w = 0;
          const int ran = p.browser->js()->dispatch(f, type, &w, ev, prevented);
          wrote += w;
          return ran;
        };
        bool prevented = false;
        const int ran = fire("keydown", &init, &prevented);
        // The page's own Enter handler -- google.com's search box is one -- ends
        // in `form.submit()` or a navigation; follow it before anything else.
        if (follow_script_requests(p, vw)) {
          wrote = 0;
          return;
        }
        if (!prevented) {
          if (key == KeyBackspace) {
            std::string v = field_value(f);
            // After a select-all this empties the field, the way it would in
            // any browser -- not "shave one character off".
            if (all) { v.clear(); field_set_value(f, v); fire("input", nullptr, nullptr); ++wrote; }
            else if (!v.empty()) {
              field_backspace(v);
              field_set_value(f, v);
              fire("input", nullptr, nullptr);
              ++wrote;
            }
          } else if (key == KeyEnter && textarea) {
            field_set_value(f, field_value(f) + "\n");
            fire("input", nullptr, nullptr);
            ++wrote;
          } else if (key == KeyEnter) {
            // IMPLICIT SUBMISSION. Enter in a text field inside a form submits
            // that form -- it is how every search box on the web is used, and
            // it is why the address bar was the only way to run a search here:
            // this branch did not exist, so Enter fired keydown, changed
            // nothing, and stopped.
            if (web::Node* form = web::enclosing_form(f)) {
              if (submit_the_form_on(p, form, nullptr, vw)) {
                p.field = nullptr;
                p.field_all = false;
                wrote = 0;              // the page is gone; nothing to lay out
                return;                 // and nothing after this applies to it
              }
            }
          } else if (key == KeyEscape) {
            p.field = nullptr;
          }
        }
        fire("keyup", &init, nullptr);
        p.message = init.key + ": " + std::to_string(ran) + " handler(s), " +
                    (prevented ? "the page took it" : "the pane keeps it") + ", " +
                    std::to_string(wrote) + " dom change(s)";
        p.error = false;
        if (!p.browser->js()->last_error().empty()) {
          p.message += " - handler failed: " + p.browser->js()->last_error();
          p.error = true;
        }
      });
      return true;
    }
    // Anything else the pane still owns (scrolling, back) falls through.
  } else if (pane.browser->js() && pane.browser->js()->wants_keys() && key != KeyEscape) {
    // No field has the keyboard, and the page listens for keys: a game steers
    // on the arrows. Escape stays the desktop's regardless.
    const char* name = key == KeyLeft ? "ArrowLeft" : key == KeyRight ? "ArrowRight" : key == KeyUp ? "ArrowUp"
                     : key == KeyDown ? "ArrowDown" : key == KeyEnter ? "Enter" : key == KeyBackspace ? "Backspace"
                     : key == KeyTab ? "Tab" : key == KeyHome ? "Home" : key == KeyEnd ? "End"
                     : key == KeyPageUp ? "PageUp" : key == KeyPageDown ? "PageDown" : key == KeyDelete ? "Delete" : nullptr;
    if (name) {
      int wrote = 0;
      pane.browser->js()->key(name, true, &wrote);
      pane.held.emplace_back(name, pane.last_tick_ns / 1000000ull);
      if (wrote > 0) pane.browser->relayout(m, view_w);
      return true;
    }
  }

  switch (key) {
    case KeyCtrlA:
      if (pane.browser) { s_sel_all = true; s_sel_any = false; ++s_sel_gen; pane.message = "selected all text"; pane.error = false; }
      return true;
    case KeyCtrlC: {
      std::string t = pane.browser ? selected_text(pane.browser->document()) : std::string();
      if (!t.empty()) {
        hal::clipboard_set(t);
        pane.message = "copied " + std::to_string(t.size()) + " characters";
      } else {
        pane.message = "nothing selected - drag over text, or Ctrl+A";
      }
      pane.error = false;
      return true;
    }
    case KeyCtrlL:
      pane.url_focus = true;
      pane.url_fresh = true;
      pane.url_text = pane.browser->document().url;
      return true;
    case KeyF5:
    case KeyCtrlR:
      // Chrome's reload shortcuts; the toolbar button does the same thing.
      navigate_async(win, view_w, "reloading", [m, view_w](WebPane& p) { return p.browser->reload(m, view_w); });
      return true;
    case KeyDown:
      pane.scroll += 42;
      return true;
    case KeyUp:
      pane.scroll -= 42;
      if (pane.scroll < 0) pane.scroll = 0;
      return true;
    case KeyPageDown:
      pane.scroll += view_h - 30;
      return true;
    case KeyPageUp:
      pane.scroll -= view_h - 30;
      if (pane.scroll < 0) pane.scroll = 0;
      return true;
    case KeyHome:
      pane.scroll = 0;
      return true;
    case KeyEnd:
      pane.scroll = pane.browser->document().content_height;
      return true;
    case KeyBackspace: {
      navigate_async(win, view_w, "going back", [m, view_w](WebPane& p) { return p.browser->back(m, view_w); });
      return true;
    }
    default:
      break;
  }
  return false;
}

}  // namespace aios::userland
