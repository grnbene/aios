// Drives the browser pane the way hands do, and writes out what the screen
// actually showed.
//
// Nothing here reaches around the compositor: it types into the url bar with
// input_text, presses Enter with input_key, clicks links with
// input_mouse_button and scrolls with input_mouse_wheel — the same five calls
// the SDL backend and the bare-metal PS/2 driver feed. So a frame this writes
// is a frame the machine would have put on its own glass.
//
//   aios-web-pane-drive <root> <out-dir> [url ...]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "aios/hal/keys.hpp"
#include "aios/kernel/kernel.hpp"
#include "aios/kernel/surface.hpp"
#include "aios/userland/compositor.hpp"
#include "aios/userland/web_pane.hpp"

using namespace aios;

namespace {

void write_ppm(const userland::HardwareCompositor& c, uint32_t w, uint32_t h, const std::string& path) {
  const auto& fb = c.framebuffer();
  std::ofstream out(path, std::ios::binary);
  out << "P6\n" << w << " " << h << "\n255\n";
  for (size_t i = 0; i < fb.size(); ++i) {
    uint32_t p = fb[i];
    char rgb[3] = {(char)((p >> 16) & 0xFF), (char)((p >> 8) & 0xFF), (char)(p & 0xFF)};
    out.write(rgb, 3);
  }
}

// Navigation and page handlers run on the page worker now, off the thread that
// draws -- so what a person sees a moment after pressing Enter is the page
// still arriving. The driver is after the finished page: keep drawing frames
// until the worker has nothing left to do.
void settle(userland::HardwareCompositor& c, Kernel& k) {
  for (int i = 0; i < 6000; ++i) {
    userland::WindowSurface* win = c.focused_window();
    const bool busy = win && win->web && (!win->web->working.empty() || win->web->pending_pics);
    c.render_desktop(k);
    if (!busy && i > 2) return;
    hal::sleep_ms(20);
  }
}

void frame(userland::HardwareCompositor& c, Kernel& k, uint32_t w, uint32_t h, const std::string& path) {
  settle(c, k);
  c.render_desktop(k);
  write_ppm(c, w, h, path);
  std::printf("        wrote %s\n", path.c_str());
}

// Where the browser pane's content starts on screen, so a click aimed at a
// link inside the page lands on it. The frame takes 1px of border and a 26px
// titlebar, exactly as WindowSurface::content_* says.
struct PaneAt {
  int x = 0, y = 0, w = 0, h = 0;
  bool found = false;
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: aios-web-pane-drive <root> <out-dir> [url ...]\n");
    return 2;
  }
  const std::string root = argv[1];
  const std::string out = argv[2];
  std::vector<std::string> urls;
  for (int i = 3; i < argc; ++i) urls.push_back(argv[i]);
  if (urls.empty()) urls.push_back("http://info.cern.ch/hypertext/WWW/TheProject.html");

  KernelConfig cfg;
  cfg.root = root;
  auto k = Kernel::boot(cfg);
  if (!k.ok()) {
    std::fprintf(stderr, "boot: %s\n", k.error().str().c_str());
    return 1;
  }
  Kernel& kernel = **k;

  AuthorityProfile profile;
  auto me = kernel.resident("web-pane-drive", profile);
  if (!me.ok()) {
    std::fprintf(stderr, "resident: %s\n", me.error().str().c_str());
    return 1;
  }
  Surface console = kernel.as(*me);

  // AIOS_PANE_SIZE=1920x1080 drives the window the desktop actually uses. A
  // page that fits at 1440 can overflow at 1920 -- percentage widths, a `ch`
  // measure and a flex row all change with the room they are given -- so a
  // defect that only shows on the real display has to be reproducible here.
  uint32_t W = 1440, H = 900;
  if (const char* sz = std::getenv("AIOS_PANE_SIZE"); sz && *sz) {
    const char* x = std::strchr(sz, 'x');
    const long w = std::strtol(sz, nullptr, 10);
    const long h = x ? std::strtol(x + 1, nullptr, 10) : 0;
    if (w >= 320 && w <= 8192) W = (uint32_t)w;
    if (h >= 240 && h <= 8192) H = (uint32_t)h;
  }
  userland::HardwareCompositor desk(W, H);
  desk.set_fullscreen(false);
  desk.set_console(&console);
  desk.set_async_commands(false);
  desk.set_dispatcher([](const std::string&, userland::OutputCallback) {});
  desk.init_default_workspace(kernel);

  std::printf("step 1: F6, which is what the start page has been promising\n");
  desk.set_canvas_mode(false);
  desk.input_key(hal::KeyF6);
  frame(desk, kernel, W, H, out + "/0-empty-pane.ppm");

  // Find the pane on screen so the clicks below can be aimed at the page.
  PaneAt pane;
  for (size_t i = 0; i < desk.window_count(); ++i) {
    // The focused window after F6 IS the browser; ask the compositor rather
    // than guessing at the workspace's geometry.
    userland::WindowSurface* w = desk.focused_window();
    if (!w || w->type != userland::WindowType::Browser) break;
    pane.x = w->bounds.x + 1;
    pane.y = w->bounds.y + 26;
    pane.w = w->content_width();
    pane.h = w->content_height();
    pane.found = true;
    break;
  }
  if (!pane.found) {
    std::fprintf(stderr, "the browser pane did not come forward on F6\n");
    return 1;
  }
  std::printf("        pane content at (%d,%d) %dx%d\n", pane.x, pane.y, pane.w, pane.h);

  // The url bar: 34px of chrome, the field starts after three 26px buttons.
  const int field_x = pane.x + 8 + 3 * 29 + 8 + 30;
  const int field_y = pane.y + 17;

  std::printf("step 2: click the url bar and type a url into it\n");
  desk.input_mouse_button(field_x, field_y, true, hal::MouseLeft);
  desk.input_text(urls[0]);
  frame(desk, kernel, W, H, out + "/1-url-typed.ppm");

  std::printf("step 3: Enter — fetch, parse, cascade, lay out, paint\n");
  desk.input_key(hal::KeyEnter);
  frame(desk, kernel, W, H, out + "/2-page.ppm");

  // What the engine made of it, printed so the picture can be checked against
  // something other than an opinion.
  {
    userland::WindowSurface* w = desk.focused_window();
    if (w && w->web && w->web->browser) {
      hal::LockGuard g(w->web->guard->m);
      const auto& doc = w->web->browser->document();
      std::printf("        title    : %s\n", doc.title.c_str());
      std::printf("        url      : %s\n", doc.url.c_str());
      std::printf("        transport: %s\n", w->web->browser->transport().c_str());
      std::printf("        laid out : %zu fragments, %zu links, %dpx tall\n", doc.fragments.size(),
                  doc.links.size(), doc.content_height);
      if (!w->web->message.empty()) std::printf("        message  : %s\n", w->web->message.c_str());
      // AIOS_PANE_DUMP=1 prints the fragments this page actually produced. The
      // layout driver beside this one cannot: it runs no script, and a page
      // that builds its DOM in JavaScript has almost nothing to lay out until
      // it has run. This is the only way to see what a real application's
      // boxes came out as.
      // Is a given id in the final DOM at all? The difference between "the
      // engine hid it" and "the engine never had it" is the whole diagnosis.
      if (const char* want = std::getenv("AIOS_FIND_ID"); want && *want) {
        int found = 0;
        std::function<void(const userland::web::Node&)> walk = [&](const userland::web::Node& n) {
          if (n.kind == userland::web::Node::Kind::Element && n.attr("id") == want) ++found;
          for (const auto& kid : n.children) if (kid) walk(*kid);
        };
        if (doc.dom) walk(*doc.dom);
        std::printf("        find #%s : %d in the DOM\n", want, found);
      }
      // Is a piece of TEXT in the DOM at all? Same question as AIOS_FIND_ID,
      // for content that has no id to look up.
      if (const char* want = std::getenv("AIOS_FIND_TEXT"); want && *want) {
        int found = 0;
        std::string where;
        std::function<void(const userland::web::Node&)> walk = [&](const userland::web::Node& n) {
          if (n.kind == userland::web::Node::Kind::Text && n.text.find(want) != std::string::npos) {
            ++found;
            if (where.empty() && n.parent) where = n.parent->tag + "." + n.parent->attr("class");
          }
          for (const auto& kid : n.children) if (kid) walk(*kid);
        };
        if (doc.dom) walk(*doc.dom);
        std::printf("        find text : %d node(s), first parent <%s>\n", found, where.c_str());
      }
      for (size_t i = 0; i < doc.scrollers.size(); ++i) {
        const auto& sa = doc.scrollers[i];
        std::printf("        scroller : %d,%d %dx%d content=%d at=%d\n", sa.x, sa.y, sa.w, sa.h,
                    sa.content_h, w->web->scroll_offset_of(sa.node));
      }
      if (const char* dump = std::getenv("AIOS_PANE_DUMP"); dump && *dump && *dump != '0') {
        for (const auto& f : doc.fragments) {
          const char* kind = f.kind == userland::web::Fragment::Kind::Rect ? "rect"
                           : f.kind == userland::web::Fragment::Kind::Text ? "text"
                           : f.kind == userland::web::Fragment::Kind::Border ? "border"
                           : f.kind == userland::web::Fragment::Kind::Image ? "image"
                           : f.kind == userland::web::Fragment::Kind::Video ? "video"
                           : f.kind == userland::web::Fragment::Kind::Audio ? "audio"
                           : f.kind == userland::web::Fragment::Kind::Canvas ? "canvas" : "rule";
          std::printf("FRAG %-6s %5d %5d %5d %5d  #%08x", kind, f.x, f.y, f.w, f.h, f.color);
          if (f.opacity != 255) std::printf(" op=%u", (unsigned)f.opacity);
          if (f.radius) std::printf(" r=%d", f.radius);
          if (f.kind == userland::web::Fragment::Kind::Text) {
            std::printf("  %.2fpx%s%s", (double)f.font.px, f.font.bold ? ",b" : "",
                        f.font.mono ? ",m" : "");
            if (f.font.tracking != 0.0f) std::printf(",t%.2f", (double)f.font.tracking);
            std::printf("  \"%s\"", f.text.c_str());
          }
          if (f.kind == userland::web::Fragment::Kind::Image) std::printf("  src=%.90s%s", f.src.c_str(), f.mask ? " (mask)" : "");
          std::printf("\n");
        }
      }
      // What the image decoder made of the page's pictures. A page draws alt
      // text when a codec is missing, so "the image is not there" and "the
      // image did not decode" look identical on screen unless it is said.
      int shown = 0, drawn = 0, missing = 0;
      for (const auto& f : doc.fragments) {
        if (f.kind != userland::web::Fragment::Kind::Image) continue;
        const auto* bm = w->web->browser->image(f.src);
        if (bm && !bm->empty()) {
          ++drawn;
          if (shown++ < 6)
            std::printf("        image OK : %dx%d decoded, drawn at %dx%d  %s\n", bm->w, bm->h, f.w, f.h,
                        f.src.c_str());
        } else {
          ++missing;
          if (shown++ < 6) std::printf("        image -- : alt text only  %s\n", f.src.c_str());
        }
      }
      if (drawn || missing)
        std::printf("        images   : %d decoded and painted, %d shown as alt text\n", drawn, missing);
    }
  }

  // Scroll to the first picture the decoder actually produced, and paint it.
  // A page's images are usually well below the fold, so a frame of the top of
  // the document is no evidence at all about whether the codec works.
  {
    userland::WindowSurface* w = desk.focused_window();
    if (w && w->web && w->web->browser) {
      // The lock is dropped before the frame is painted: the frame takes the same
      // non-recursive mutex, and holding it here hung the tool for good on any
      // page with a decoded picture.
      int image_y = -1;
      {
        hal::LockGuard g(w->web->guard->m);
        const auto& doc = w->web->browser->document();
        for (const auto& f : doc.fragments) {
          if (f.kind != userland::web::Fragment::Kind::Image) continue;
          const auto* bm = w->web->browser->image(f.src);
          if (!bm || bm->empty()) continue;
          std::printf("step 3b: scroll to the first decoded image, at y=%d\n", f.y);
          w->web->scroll = f.y - 60 > 0 ? f.y - 60 : 0;
          image_y = f.y;
          break;
        }
      }
      if (image_y >= 0) frame(desk, kernel, W, H, out + "/2b-image.ppm");
    }
  }

  // A WindowSurface* is a pointer INTO the compositor's window vector, and
  // bring_to_front reorders that vector. Holding one across an input call is a
  // dangling pointer, which is exactly what crashed this tool the first time:
  // it read the document through a stale `w` after the click. Re-ask for the
  // window after anything that could touch the window list.
  auto pane_now = [&]() -> userland::WindowSurface* {
    userland::WindowSurface* w = desk.focused_window();
    return (w && w->type == userland::WindowType::Browser) ? w : nullptr;
  };

  std::printf("step 4: hover the first link, so it lights and the status names it\n");
  int click_x = 0, click_y = 0;
  bool have_link = false;
  {
    userland::WindowSurface* w = pane_now();
    if (w && w->web && w->web->browser) {
      w->web->scroll = 0;   // back to the top, so link 0 is on screen to click
      const auto& doc = w->web->browser->document();
      if (!doc.links.empty()) {
        const auto& l = doc.links[0];
        click_x = pane.x + l.x + l.w / 2;
        click_y = pane.y + 34 + l.y + l.h / 2;
        have_link = true;
        std::printf("        link 0 '%s' -> %s at screen (%d,%d)\n", l.text.c_str(), l.href.c_str(), click_x,
                    click_y);
      }
    }
  }
  // AIOS_CLICK_ID=<id> clicks the middle of the element with that id. A
  // coordinate is only right for the window size it was read at; an id is the
  // thing the page itself names, so a menu check survives a reflow.
  if (const char* want = std::getenv("AIOS_CLICK_ID"); want && *want) {
    userland::WindowSurface* w = pane_now();
    int cx = -1, cy = -1;
    if (w && w->web && w->web->browser) {
      for (const auto& f : w->web->browser->document().fragments) {
        for (const userland::web::Node* n = f.node; n; n = n->parent) {
          if (n->kind == userland::web::Node::Kind::Element && n->attr("id") == want) {
            cx = pane.x + f.x + f.w / 2;
            cy = pane.y + 34 + f.y + f.h / 2;
            break;
          }
        }
        if (cx >= 0) break;
      }
    }
    if (cx < 0) {
      std::printf("step 5: #%s has no painted box to click\n", want);
    } else {
      std::printf("step 5: click #%s at screen (%d,%d)\n", want, cx, cy);
      desk.input_mouse_motion(cx, cy);
      desk.input_mouse_button(cx, cy, true, hal::MouseLeft);
      desk.input_mouse_button(cx, cy, false, hal::MouseLeft);
      for (int i = 0; i < 4; ++i) { desk.render_desktop(kernel); hal::sleep_ms(60); }
      frame(desk, kernel, W, H, out + "/7-clicked.ppm");
    }
  } else
  // AIOS_PANE_CLICK=x,y clicks a point instead of the first link, so an
  // interactive state -- an open menu, a flyout, a toggled panel -- can be
  // driven and looked at offscreen. Without it the only reachable state is
  // "the page as it loaded".
  if (const char* at = std::getenv("AIOS_PANE_CLICK"); at && *at) {
    const char* comma = std::strchr(at, ',');
    const int cx = (int)std::strtol(at, nullptr, 10);
    const int cy = comma ? (int)std::strtol(comma + 1, nullptr, 10) : 0;
    std::printf("step 5: click %d,%d\n", cx, cy);
    desk.input_mouse_motion(cx, cy);
    desk.input_mouse_button(cx, cy, true, hal::MouseLeft);
    desk.input_mouse_button(cx, cy, false, hal::MouseLeft);
    // A click on a page handler runs off the render thread, so give it frames to land in. AIOS_PANE_DUMP=1 then says
    // what the page looked like AFTER, which a picture alone cannot be searched for.
    for (int i = 0; i < 12; ++i) { desk.render_desktop(kernel); hal::sleep_ms(60); }
    // AIOS_PANE_TYPE=text types it after the click, into whatever took the focus.
    if (const char* typed = std::getenv("AIOS_PANE_TYPE"); typed && *typed) {
      std::printf("step 5b: type '%s'\n", typed);
      desk.input_text(typed);
      for (int i = 0; i < 12; ++i) { desk.render_desktop(kernel); hal::sleep_ms(60); }
    }
    frame(desk, kernel, W, H, out + "/7-clicked.ppm");
    if (const char* dump = std::getenv("AIOS_PANE_DUMP"); dump && *dump && *dump != '0') {
      if (userland::WindowSurface* w = pane_now(); w && w->web && w->web->browser) {
        std::printf("        message  : %s\n", w->web->message.c_str());
        for (const auto& f : w->web->browser->document().fragments)
          if (f.kind == userland::web::Fragment::Kind::Text) std::printf("AFTER text  \"%s\"\n", f.text.c_str());
      }
    }
  } else if (!have_link) {
    std::printf("        no links on the page to click\n");
  } else {
    desk.input_mouse_motion(click_x, click_y);
    frame(desk, kernel, W, H, out + "/3-link-hover.ppm");

    std::printf("step 5: click it\n");
    desk.input_mouse_button(click_x, click_y, true, hal::MouseLeft);
    frame(desk, kernel, W, H, out + "/4-followed.ppm");
    userland::WindowSurface* w = pane_now();
    if (w && w->web && w->web->browser) {
      const auto& after = w->web->browser->document();
      std::printf("        now at   : %s\n", after.url.c_str());
      std::printf("        title    : %s\n", after.title.c_str());
      if (!w->web->message.empty()) std::printf("        message  : %s\n", w->web->message.c_str());
    }
  }

    // AIOS_TYPE=<text> types into whatever the click focused, and
    // AIOS_KEYS=Enter,CtrlA,... presses keys after it. Between them a form
    // can be filled in and submitted the way a person does it, which is the
    // only way to find out whether Enter in a search box actually searches.
    if (const char* typed = std::getenv("AIOS_TYPE"); typed && *typed) {
      std::printf("        type     : \"%s\"\n", typed);
      desk.input_text(typed);
      for (int i = 0; i < 3; ++i) { desk.render_desktop(kernel); hal::sleep_ms(60); }
    }
    if (const char* keys = std::getenv("AIOS_KEYS"); keys && *keys) {
      std::string all(keys);
      size_t at = 0;
      while (at <= all.size()) {
        const size_t comma = all.find(',', at);
        std::string one = all.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        if (!one.empty() && one[0] == '+') {
          // `+some text` in the key list types it, so a sequence like
          // `+alpha,CtrlA,CtrlC,+x,CtrlV,Enter` can be driven in one go.
          const std::string t = one.substr(1);
          std::printf("        type     : \"%s\"\n", t.c_str());
          desk.input_text(t);
          for (int i = 0; i < 4; ++i) { desk.render_desktop(kernel); hal::sleep_ms(90); }
        } else if (!one.empty()) {
          int key = 0;
          if (one == "Enter") key = hal::KeyEnter;
          else if (one == "Backspace") key = hal::KeyBackspace;
          else if (one == "Escape") key = hal::KeyEscape;
          else if (one == "Tab") key = hal::KeyTab;
          else if (one == "CtrlA") key = hal::KeyCtrlA;
          else if (one == "CtrlC") key = hal::KeyCtrlC;
          else if (one == "CtrlV") key = hal::KeyCtrlV;
          else if (one == "CtrlX") key = hal::KeyCtrlX;
          else if (one == "CtrlL") key = hal::KeyCtrlL;
          else if (one == "CtrlR") key = hal::KeyCtrlR;
          else if (one == "F5") key = hal::KeyF5;
          else if (one == "PageDown") key = hal::KeyPageDown;
          else if (one == "PageUp") key = hal::KeyPageUp;
          else if (one == "CtrlF") key = hal::KeyCtrlF;
          else if (one == "ShiftEnter" || one == "AltLeft" || one == "AltRight") {
            // A modifier is held for exactly this key, the way the window system reports it.
            userland::web_pane_set_modifiers(one == "ShiftEnter", one != "ShiftEnter");
            key = one == "ShiftEnter" ? hal::KeyEnter : one == "AltLeft" ? hal::KeyLeft : hal::KeyRight;
            desk.input_key(key);
            userland::web_pane_set_modifiers(false, false);
            for (int i = 0; i < 10; ++i) { desk.render_desktop(kernel); hal::sleep_ms(120); }
            key = 0;
          }
          else if (one == "ZoomIn") key = userland::kWebKeyZoomIn;
          else if (one == "ZoomOut") key = userland::kWebKeyZoomOut;
          else if (one == "ZoomReset") key = userland::kWebKeyZoomReset;
          if (key) {
            std::printf("        key      : %s\n", one.c_str());
            desk.input_key(key);
            for (int i = 0; i < 10; ++i) { desk.render_desktop(kernel); hal::sleep_ms(120); }
          }
        }
        if (comma == std::string::npos) break;
        at = comma + 1;
      }
      frame(desk, kernel, W, H, out + "/8-keys.ppm");
      if (userland::WindowSurface* a = pane_now(); a && a->web && a->web->browser) {
        const auto& after = a->web->browser->document();
        std::printf("        now at   : %s\n", after.url.c_str());
        std::printf("        title    : %s\n", after.title.c_str());
        std::printf("        laid out : %zu fragments, %zu links, %dpx tall\n",
                    after.fragments.size(), after.links.size(), after.content_height);
        if (!a->web->message.empty()) std::printf("        message  : %s\n", a->web->message.c_str());
      }
    }
    // What the click left behind: the classes on the elements the page keys
    // its menus off, and how many boxes it painted.
    if (userland::WindowSurface* a = pane_now(); a && a->web && a->web->browser) {
      const auto& after = a->web->browser->document();
      std::printf("        after    : %zu fragments\n", after.fragments.size());
      // The boxes the click produced. AIOS_PANE_DUMP shows the page as it
      // LOADED; a menu only exists after something opened it.
      if (const char* d2 = std::getenv("AIOS_DUMP_AFTER"); d2 && *d2 && *d2 != '0') {
        for (const auto& f : after.fragments) {
          const char* kind = f.kind == userland::web::Fragment::Kind::Rect ? "rect"
                           : f.kind == userland::web::Fragment::Kind::Text ? "text"
                           : f.kind == userland::web::Fragment::Kind::Border ? "border"
                           : f.kind == userland::web::Fragment::Kind::Image ? "image"
                           : f.kind == userland::web::Fragment::Kind::Video ? "video"
                           : f.kind == userland::web::Fragment::Kind::Audio ? "audio"
                           : f.kind == userland::web::Fragment::Kind::Canvas ? "canvas" : "rule";
          std::string who;
          for (const userland::web::Node* n = f.node; n; n = n->parent)
            if (n->kind == userland::web::Node::Kind::Element && !n->attr("class").empty()) {
              who = n->tag + "." + n->attr("class");
              break;
            }
          std::printf("AFTER %-6s %5d %5d %5d %5d  #%08x  %-34s %s\n", kind, f.x, f.y, f.w, f.h,
                      f.color, who.c_str(), f.text.c_str());
        }
      }
      if (const char* ids = std::getenv("AIOS_REPORT_IDS"); ids && *ids) {
        std::string all(ids);
        size_t at2 = 0;
        while (at2 <= all.size()) {
          size_t c = all.find(',', at2);
          std::string one = all.substr(at2, c == std::string::npos ? std::string::npos : c - at2);
          if (!one.empty()) {
            int boxes = 0;
            std::string cls = "(not in the DOM)";
            std::function<void(const userland::web::Node&)> walk = [&](const userland::web::Node& n) {
              if (n.kind == userland::web::Node::Kind::Element && n.attr("id") == one) cls = "class='" + n.attr("class") + "'";
              for (const auto& kid : n.children) if (kid) walk(*kid);
            };
            if (after.dom) walk(*after.dom);
            for (const auto& f : after.fragments)
              for (const userland::web::Node* n = f.node; n; n = n->parent)
                if (n->kind == userland::web::Node::Kind::Element && n->attr("id") == one) { ++boxes; break; }
            std::printf("        #%s %s %d painted box(es)\n", one.c_str(), cls.c_str(), boxes);
          }
          if (c == std::string::npos) break;
          at2 = c + 1;
        }
      }
    }

  std::printf("step 6: scroll down three notches\n");
  desk.input_mouse_wheel(-3);
  frame(desk, kernel, W, H, out + "/5-scrolled.ppm");

  std::printf("step 7: Backspace, which is back\n");
  desk.input_key(hal::KeyBackspace);
  frame(desk, kernel, W, H, out + "/6-back.ppm");
  {
    userland::WindowSurface* w = pane_now();
    if (w && w->web && w->web->browser)
      std::printf("        back at  : %s\n", w->web->browser->document().url.c_str());
  }

  // Any further urls named on the command line, each one a fresh visit.
  for (size_t i = 1; i < urls.size(); ++i) {
    std::printf("step %zu: %s\n", 7 + i, urls[i].c_str());
    desk.input_key(hal::KeyCtrlL);
    if (userland::WindowSurface* f = pane_now())
      if (f->web) f->web->url_text.clear();
    desk.input_text(urls[i]);
    desk.input_key(hal::KeyEnter);
    frame(desk, kernel, W, H, out + "/" + std::to_string(6 + i) + "-extra.ppm");
    userland::WindowSurface* w = pane_now();
    if (w && w->web && w->web->browser) {
      const auto& doc = w->web->browser->document();
      std::printf("        %s — %zu fragments, %zu links, %dpx\n", doc.title.c_str(), doc.fragments.size(),
                  doc.links.size(), doc.content_height);
      if (!w->web->message.empty()) std::printf("        message: %s\n", w->web->message.c_str());
    }
  }

  std::printf("done\n");
  return 0;
}
