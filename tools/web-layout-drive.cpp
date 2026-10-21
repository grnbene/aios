// The layout tree, as text, for a page that is frozen in the tree.
//
// The pane driver beside this one writes PPM frames: right for "does the glass
// look correct", wrong for a regression test, because every frame changes when
// the font atlas changes and nobody can read a pixel diff. This writes what the
// engine DECIDED — one line per fragment, with the box, the cascaded font and
// the text — which is diffable, reviewable in a pull request, and stable across
// a change to the type that draws it.
//
// Metrics here are synthetic and fixed (advance = 0.55em, line = 1.4em) so a
// golden records the CASCADE's font size rather than whatever the atlas happens
// to be able to draw. That separation is the point: it is what lets the atlas be
// replaced underneath without churning every golden, and what makes a wrong
// font-size a test failure rather than an appearance someone has to notice.
//
//   aios-web-layout-drive <viewport-width> <page.html> [page.html ...]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "aios/userland/web.hpp"
#include "aios/userland/web_pane.hpp"

using namespace aios;
using namespace aios::userland;

namespace {

std::string read_file(const std::string& path, bool* ok) {
  std::ifstream in(path, std::ios::binary);
  if (!in) { *ok = false; return {}; }
  std::ostringstream ss;
  ss << in.rdbuf();
  *ok = true;
  return ss.str();
}

std::string dir_of(const std::string& path) {
  size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

// A page in the corpus may pull a stylesheet in beside it. Everything the
// corpus needs comes off the disk next to the page: no socket, so a golden is
// the same on a machine with no network as on one with.
web::Fetch local_fetch(const std::string& dir) {
  return [dir](const std::string& url, const web::FetchOptions&) -> Result<web::FetchResult> {
    std::string rel = url;
    // The engine hands back an absolute url; the corpus is flat, so the last
    // path segment is the file beside the page.
    size_t slash = rel.find_last_of('/');
    if (slash != std::string::npos) rel = rel.substr(slash + 1);
    bool ok = false;
    std::string body = read_file(dir + "/" + rel, &ok);
    if (!ok) return Result<web::FetchResult>(Status::NotFound, "corpus: no " + rel);
    web::FetchResult r;
    r.body.assign(body.begin(), body.end());
    r.final_url = url;
    r.content_type = "text/css";
    r.status = 200;
    return Result<web::FetchResult>(std::move(r));
  };
}

web::Metrics synthetic_metrics() {
  web::Metrics m;
  m.measure = [](const std::string& text, const web::FontKey& f) -> int {
    // Count code points, not bytes: a golden must not change width because a
    // word is spelled with an accent.
    int glyphs = 0;
    for (unsigned char c : text) if ((c & 0xC0) != 0x80) ++glyphs;
    const int advance = (f.mono ? (f.px * 60) : (f.px * 55)) / 100;
    const float per = (float)(advance > 0 ? advance : 1) + f.tracking;
    // Up, for the same reason the real metrics round up: a width that rounds
    // down is a box its own text does not fit in.
    return (int)((float)glyphs * per + 0.999f) + (f.bold ? glyphs / 8 : 0);
  };
  m.line_height = [](const web::FontKey& f) { return (f.px * 140) / 100; };
  m.ascent = [](const web::FontKey& f) { return (f.px * 80) / 100; };
  return m;
}

const char* kind_name(web::Fragment::Kind k) {
  switch (k) {
    case web::Fragment::Kind::Rect: return "rect";
    case web::Fragment::Kind::Text: return "text";
    case web::Fragment::Kind::Border: return "border";
    case web::Fragment::Kind::Image: return "image";
    case web::Fragment::Kind::Rule: return "rule";
    case web::Fragment::Kind::Video: return "video";
    case web::Fragment::Kind::Audio: return "audio";
    case web::Fragment::Kind::Canvas: return "canvas";
  }
  return "?";
}

// Text goes on the line as written, with the two characters that would break
// the format escaped. A golden is read by people, so this stays legible rather
// than becoming an encoding.
std::string escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    if (c == '\n') out += "\\n";
    else if (c == '\t') out += "\\t";
    else out += c;
  }
  return out;
}

// Trimmed: a size that is a whole number prints as one, so a golden does not
// carry six decimal places of nothing.
std::string font_of(const web::FontKey& f) {
  char px[32];
  if (f.px == (float)(int)f.px) std::snprintf(px, sizeof px, "%dpx", (int)f.px);
  else std::snprintf(px, sizeof px, "%.2fpx", (double)f.px);
  std::string s = px;
  if (f.bold) s += ",bold";
  if (f.italic) s += ",italic";
  if (f.mono) s += ",mono";
  if (f.tracking != 0.0f) {
    char buf[32];
    std::snprintf(buf, sizeof buf, f.tracking == (float)(int)f.tracking ? ",track%.0f" : ",track%.2f",
                  (double)f.tracking);
    s += buf;
  }
  return s;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: aios-web-layout-drive <width[xheight]> <page.html> [page.html ...]\n");
    return 2;
  }
  // `1024` or `1024x768`. The height matters: `100vh` and a `height: 100%`
  // chain have nothing to resolve against without one, and an app shell is
  // exactly such a chain.
  const std::string vp = argv[1];
  const size_t xat = vp.find('x');
  const int viewport = std::atoi(vp.c_str());
  const int viewport_h = xat == std::string::npos ? 0 : std::atoi(vp.c_str() + xat + 1);
  if (viewport <= 0) { std::fprintf(stderr, "viewport width must be positive\n"); return 2; }

  int failures = 0;
  for (int a = 2; a < argc; ++a) {
    const std::string path = argv[a];
    bool ok = false;
    const std::string markup = read_file(path, &ok);
    if (!ok) { std::fprintf(stderr, "cannot read %s\n", path.c_str()); ++failures; continue; }

    const std::string name = path.substr(path.find_last_of('/') + 1);
    web::Document doc;
    doc.url = "file:///" + name;
    doc.base = "file:///";
    doc.dom = web::parse_html(ByteSpan((const uint8_t*)markup.data(), markup.size()));
    if (!doc.dom) { std::fprintf(stderr, "%s: parsed to nothing\n", name.c_str()); ++failures; continue; }

    // No script runs here, so a node missing from THIS walk was lost by the
    // parser and a node present here but gone in the pane was lost by the page's
    // own JavaScript. That distinction is the whole diagnosis.
    if (const char* want = std::getenv("AIOS_FIND_ID"); want && *want) {
      int found = 0;
      std::function<void(const web::Node&)> walk = [&](const web::Node& n) {
        if (n.kind == web::Node::Kind::Element && n.attr("id") == want) ++found;
        for (const auto& kid : n.children) if (kid) walk(*kid);
      };
      walk(*doc.dom);
      std::printf("# find #%s %d\n", want, found);
    }

    const web::Stylesheet author = web::collect_author_css(*doc.dom, doc.base, local_fetch(dir_of(path)));
    const web::Metrics m = synthetic_metrics();
    web::layout(doc, author, m, viewport, viewport_h);

    if (viewport_h > 0) std::printf("# page %s  viewport=%dx%d\n", name.c_str(), viewport, viewport_h);
    else std::printf("# page %s  viewport=%d\n", name.c_str(), viewport);
    std::printf("# title %s\n", doc.title.c_str());
    std::printf("# content %dx%d  background=%08x  links=%zu\n",
                doc.content_width, doc.content_height, doc.background, doc.links.size());

    for (const auto& f : doc.fragments) {
      // The colour goes out as #aarrggbb: alpha is the difference between a
      // wash and a solid white box, and a golden that prints only the rgb
      // cannot tell them apart.
      std::printf("%-6s %5d %5d %5d %5d  #%08x", kind_name(f.kind), f.x, f.y, f.w, f.h, f.color);
      if (f.kind == web::Fragment::Kind::Text) {
        std::printf("  %-16s", font_of(f.font).c_str());
        if (f.underline) std::printf(" u");
        if (f.strike) std::printf(" s");
        if (f.link >= 0) std::printf(" link=%d", f.link);
        std::printf("  \"%s\"", escape(f.text).c_str());
      } else if (f.kind == web::Fragment::Kind::Image) {
        std::printf("  src=%s alt=\"%s\"", f.src.c_str(), escape(f.alt).c_str());
        if (f.mask) std::printf(" mask=true");
      }
      if (f.radius) std::printf("  r=%d", f.radius);
      if (f.blur) std::printf("  blur=%d", f.blur);
      // Opacity is a paint property: the box is still in the layout, so a
      // golden that does not print it cannot tell a hidden control from a
      // drawn one.
      if (f.opacity != 255) std::printf("  op=%u", (unsigned)f.opacity);
      if (f.z) std::printf("  z=%d", f.z);
      if (f.clipped) std::printf("  clip=%d,%d,%dx%d", f.clip_x, f.clip_y, f.clip_w, f.clip_h);
      std::printf("\n");
    }

    for (size_t i = 0; i < doc.links.size(); ++i) {
      const auto& l = doc.links[i];
      std::printf("link  %5d %5d %5d %5d  %s  \"%s\"\n", l.x, l.y, l.w, l.h, l.href.c_str(), escape(l.text).c_str());
    }

    // The scrollable boxes. A frame with more inside it than fits is a
    // structural fact about the page, and a golden that cannot see one cannot
    // tell a scroll pane from content that was simply cut off.
    for (size_t i = 0; i < doc.scrollers.size(); ++i) {
      const auto& sa = doc.scrollers[i];
      std::printf("# scroller %zu  %d,%d %dx%d  content=%d  max-offset=%d\n", i, sa.x, sa.y, sa.w,
                  sa.h, sa.content_h, sa.max_offset());
    }

    // The pictures the page drew itself. A layout golden records WHERE an icon
    // went; without this it says nothing about whether the icon is still the
    // right shape, so a rasteriser regression would pass silently. The digest
    // is cheap and order-dependent, which is all it has to be.
    for (const auto& kv : doc.inline_images) {
      const auto& bm = kv.second;
      uint32_t digest = 2166136261u;
      uint32_t inked = 0;
      for (uint32_t px : bm.pixels) {
        digest = (digest ^ px) * 16777619u;
        if (px >> 24) ++inked;
      }
      std::printf("# inline-image %-8s %dx%d  inked=%u  digest=%08x\n",
                  kv.first.c_str(), bm.w, bm.h, inked, digest);
    }

    // What the atlas could not draw, on the page's own text. This is the line
    // that makes a typographic regression visible without a picture.
    std::string all;
    for (const auto& f : doc.fragments) if (f.kind == web::Fragment::Kind::Text) all += f.text;
    const std::string gaps = display_glyph_gaps(all);
    std::printf("# glyph-gaps %s\n", gaps.empty() ? "(none)" : gaps.c_str());
    std::printf("\n");
  }
  return failures ? 1 : 0;
}
