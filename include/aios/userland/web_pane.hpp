#pragma once
// The browser pane: where the engine's fragments become pixels, and where a
// click becomes a navigation.
//
// This is deliberately NOT in the compositor. The compositor is a window
// manager — frames, focus, drag, the dock — and a browser is an application it
// hosts, the same way the editor and the SQL studio are. Everything about
// painting a page, scrolling one, and typing a url lives here.
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "aios/hal/sync.hpp"

#include "aios/kernel/kernel.hpp"
#include "aios/userland/face.hpp"
#include "aios/userland/web.hpp"
#include "aios/userland/web_type.hpp"

namespace aios::userland {

class HardwareCompositor;
struct WindowSurface;

// What one pane carries between frames. Held behind a shared_ptr on the
// WindowSurface so the surface stays copyable — `windows_` is a vector, and it
// grows.
// The lock that stands between the page's own work and the frame being drawn.
//
// A page's handler can take a minute -- an application asks its agent a
// question and the answer streams back when it is ready -- and running that on
// the thread that draws the desktop froze the whole machine until it landed.
// So page work runs on a worker of its own and takes this; the frame takes it
// too, for the microseconds it spends reading the layout. The transport
// releases it while it is blocked on the network, which is where all of that
// minute goes: the DOM is not being touched then, so there is nothing to
// protect.
//
// hal::Mutex, not aios::hal::Mutex: this file is in the bare-metal core, and there
// the lock is the machine's own.
struct PageGuard {
  hal::Mutex m;
  bool held = false;
  // Set while the frame (render_web_pane) holds it, so work started from inside the frame -- the pictures a page still
  // needs -- puts its label up without taking a lock the frame already has: the mutex is not recursive, and taking it
  // again from the same thread stopped the whole pane for good.
  bool in_frame = false;
  // Set only while the WORKER holds it. The frame holds the same lock but must
  // never have it released underneath it by a fetch, so the release is gated
  // on this rather than on thread identity.
  bool worker = false;
  // Which navigation is the newest asked for, and which one the worker is running. A newer one than the running one
  // makes the transport refuse every further request (see kernel_fetch), so a load in progress ends at its next step
  // rather than the user waiting it out. Atomic: the fetch threads read them with no lock.
  std::atomic<uint64_t> nav_gen{0};
  std::atomic<uint64_t> running_gen{0};

  void lock_frame() { m.lock(); held = true; }
  void unlock_frame() { held = false; m.unlock(); }
  void lock_worker() { m.lock(); held = true; worker = true; }
  void unlock_worker() { worker = false; held = false; m.unlock(); }
  // True when it actually let go, so the caller knows to take it back.
  bool release_for_io() {
    if (!held || !worker) return false;
    worker = false;
    held = false;
    m.unlock();
    return true;
  }
  void reacquire() { m.lock(); held = true; worker = true; }
};

// One worker per pane, for the page's own work. A queue and a thread rather
// than a thread per click: hal::Thread is joined by its destructor, so there is
// nothing to detach a one-shot onto.
struct PageWorker {
  hal::Mutex m;
  // Each job with whether it is one there should only ever be one of.
  std::vector<std::pair<std::function<void()>, bool>> jobs;
  bool stop = false;
  bool running = false;
  // An exclusive job -- a click, a send -- is in flight. Typed keys still
  // queue behind it; a second click does not.
  bool heavy = false;
  hal::Thread thread;

  ~PageWorker();
  void start();
  // Queue it. `exclusive` is for the work a second one of makes no sense --
  // a click, a send -- and is refused while the page is mid-turn. Typing is
  // NOT exclusive: refusing a keystroke because the last one was still being
  // delivered dropped most of what was typed, so "name the capital of France"
  // reached the box as "na epaoFn er".
  bool submit(std::function<void()> job, bool exclusive);
  bool busy();
};

struct WebPane {
  std::shared_ptr<web::Browser> browser;
  // Held while the page's DOM is being read or written. Shared because the
  // transport closure and the worker both need it.
  std::shared_ptr<PageGuard> guard = std::make_shared<PageGuard>();
  std::shared_ptr<PageWorker> worker = std::make_shared<PageWorker>();
  // What the page is doing right now, for the status line. Written by the
  // worker under the guard, read by the frame under it.
  std::string working;
  int scroll = 0;
  int hover_link = -1;
  // Where the pointer was last seen, in page coordinates. The wheel arrives
  // without a position, and which box it scrolls depends entirely on where the
  // pointer is.
  int last_px = -1, last_py = -1;
  // How far each `overflow: auto` box has been scrolled, keyed by the element
  // so it survives a relayout. A page that rebuilds its DOM loses these, which
  // is the same thing a browser does.
  std::vector<std::pair<web::Node*, int>> scroll_offsets;
  int scroll_offset_of(web::Node* n) const {
    for (const auto& kv : scroll_offsets) if (kv.first == n) return kv.second;
    return 0;
  }
  void set_scroll_offset(web::Node* n, int v) {
    for (auto& kv : scroll_offsets) if (kv.first == n) { kv.second = v; return; }
    scroll_offsets.emplace_back(n, v);
  }
  bool url_focus = false;
  // The field was just focused and nothing has been typed into it yet. Every
  // browser select-alls on focus so the first keystroke REPLACES the address;
  // this field pre-filled the current url and parked the caret at the end, so
  // typing "bbc.com" gave "http://www.google.com/bbc.com" and navigating went
  // nowhere. Without a selection model, this is the same promise: the first
  // character (or a backspace) clears what was there.
  bool url_fresh = false;
  // The same promise for a field ON THE PAGE: Ctrl+A said "all of this", so the
  // next character typed replaces it rather than appending to it. There is no
  // selection model here -- no caret, no anchor -- and inventing half of one
  // would be worse than saying plainly what select-all means: the field is
  // about to be replaced.
  bool field_all = false;
  std::string url_text;      // what is being typed, which is not yet the url
  // The field ON THE PAGE that has the keyboard, if any. A pane that can click
  // but not type is a pane that can look at an application, not use one: every
  // interface worth opening starts with a box you type into. Cleared on
  // navigation and re-checked against the live tree before every use, because
  // a script is free to replace the node under it.
  web::Node* field = nullptr;
  std::string message;       // the last thing that happened, good or bad
  bool error = false;
  int last_viewport_w = 0;
  // The page's clock, so a frame handler is told how much time passed.
  uint64_t last_tick_ns = 0;
  uint64_t last_flush_ms = 0;
  // Keys the pane has told the page are down, and when. The hosted input
  // path delivers presses only, so a held key is inferred: a press is down
  // until a short while after the last repeat.
  std::vector<std::pair<std::string, uint64_t>> held;
  // Which dream this pane is showing, when it is one; what its records are
  // stamped with.
  std::string dream;
  // A FACE on the pane instead of a page: the machine's own description of a
  // dream, laid out and painted by the compositor itself (face.hpp). While this
  // is set the web engine is not consulted for anything but the chrome.
  std::shared_ptr<face::Runtime> face;
  std::string face_url;        // face://<dream> or face:///path, as opened
  std::string face_said;       // the last thing the face said, so a newer one clears an old fault
  // When the frame last laid the page out WHILE the page was mid-turn. The
  // work thread only lays out when its job ends, so without this a person's
  // own message sat invisible for the whole minute the answer took: the DOM
  // had it, the screen did not.
  uint64_t live_layout_ms = 0;
  uint64_t last_image_check_ms = 0;
  // What the page area looked like the last time it was painted. A frame whose
  // inputs are all the same repaints nothing: clearing the buffer and drawing
  // every glyph again, sixty times a second, for a page nobody touched, was the
  // whole of the slowness.
  // Pictures to fetch, noticed during a frame but started only after the frame
  // has let go of the page lock: starting a job takes that lock, and the frame
  // is already holding it.
  bool pending_pics = false;
  int pending_y0 = 0, pending_y1 = 0;
  uint64_t render_key = 0;
  const void* render_buf = nullptr;
  size_t render_buf_size = 0;   // when the frame last looked for pictures to fetch
  // Cached document fragments hash to avoid hashing thousands of text fragments per frame
  const void* cached_frag_ptr = nullptr;
  uint64_t cached_layout_gen = 0;
  size_t cached_frag_size = 0;
  int cached_content_h = 0;
  int cached_content_w = 0;
  uint64_t cached_frag_hash = 0;
};

// face://<dream> names a face under <root>/dreams/<dream>/face.aos; face:///path
// is a face file anywhere. Everything else is a page.
bool is_face_url(const std::string& url);
std::string face_source_path(const std::string& root, const std::string& url);

// Metrics off the compositor's own font atlas. Handing these to the engine is
// what makes a line break against the glyphs that actually get drawn.
web::Metrics pane_metrics();

// Bytes over the machine's own TCP/IP and TLS — the same stack `sense` uses.
// No host process, no curl: on the metal there is nothing underneath this.
// file: urls read from the machine's own root, for the surfaces it made.
web::Fetch kernel_fetch(Kernel& k, std::shared_ptr<PageGuard> guard = nullptr);

// The 64-d trigram embedding every record and every query on this machine is
// compared with. One implementation: what `store similar` searches with is
// what `aos.store.put` stores.
std::vector<float> text_embedding(const std::string& text);

// The machine behind a page's `aos`: the object store through `who`'s
// authority, the organs under the kernel root, localStorage kept as a kernel
// object. `dream` stamps every record and names the storage object.
HostBindings surface_bindings(Kernel& k, Surface who, const std::string& dream);

// The dream a surface url belongs to (…/dreams/<name>/…), or "".
std::string dream_of_url(const std::string& url);

// Bind a pane's browser to the machine for whatever it is about to show, as
// the dream's own entity when the url is a dream's surface.
void web_pane_bind(HardwareCompositor& c, WebPane& pane, Kernel& k, const std::string& url, int view_h);

// Paint the pane into `win.buffer`.
void render_web_pane(HardwareCompositor& c, WindowSurface& win, Kernel& k);

// Input, in the window's CONTENT coordinates (the frame has already been taken
// off). `web_pane_key` returns true when it consumed the key.
void web_pane_click(HardwareCompositor& c, WindowSurface& win, Kernel& k, int rel_x, int rel_y);
void web_pane_release();   // left button up: ends a text-selection drag
void web_pane_motion(WindowSurface& win, int rel_x, int rel_y);
void web_pane_wheel(WindowSurface& win, int notches);
bool web_pane_key(HardwareCompositor& c, WindowSurface& win, Kernel& k, int key);
bool web_pane_text(WindowSurface& win, const std::string& text);
// Shift and Alt are modifiers, not keys: Shift+Enter and Enter, Alt+Left and Left arrive as the same code. The input backend
// says which are held before it feeds a key, the way the editor's word/shift modifiers work.
void web_pane_set_modifiers(bool shift, bool alt);
// Ctrl+plus, Ctrl+minus and Ctrl+0: the input backend has no ASCII control code for them, so they get codes of their own
// (above the F-keys, where no printable character can collide).
constexpr int kWebKeyZoomIn = 0x1100, kWebKeyZoomOut = 0x1101, kWebKeyZoomReset = 0x1102;

// Go somewhere. Used by the pane's own url bar and by `browser <url>` at the
// command surface, so both paths land in the same place.
void web_pane_go(HardwareCompositor& c, WindowSurface& win, Kernel& k, const std::string& url);

// Measuring text, DrawClip and drawing into a buffer: userland/web_type.hpp.
void web_draw_text(HardwareCompositor& c, std::vector<uint32_t>& buf, int bw, int bh, int x, int y,
                   const std::string& text, uint32_t color, const web::FontKey& f,
                   const DrawClip& clip = DrawClip{});

}  // namespace aios::userland
