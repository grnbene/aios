#include <chrono>
// Box layout: the DOM and the cascade go in, a list of things to paint comes
// out. Two formatting contexts, which between them are most of what a document
// is made of:
//
//   block  — boxes stack down the page, margins collapse between siblings,
//            width comes from the parent unless the style names one.
//   inline — words flow across a line and wrap when the line is full, every
//            run keeps its own face and colour, and everything on a line sits
//            on one baseline.
//
// Tables get a real (if simple) pass: columns are counted across the rows and
// the width is split between them, so a table is a grid rather than a stack.
//
// Nothing here measures a glyph. The renderer that owns the font hands in
// Metrics, so a line breaks against the type that actually gets drawn.
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <array>
#include <unordered_map>

#include "aios/userland/web.hpp"

namespace aios::userland::web {
namespace {

constexpr size_t kMaxBoxDepth = 96;
// A page that lays out to more fragments than this is a page nobody is reading
// past the top of; the cap keeps one document from eating ring 0's heap.
constexpr size_t kMaxFragments = 60000;

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

bool in_list_tags(const std::string& tag, const char* const* list) {
  for (size_t i = 0; list[i]; ++i)
    if (tag == list[i]) return true;
  return false;
}


// `text-transform`, applied where the words are made. ASCII and Latin-1, which
// is what the atlas draws anyway.
std::string cased(const std::string& in, const Style& st) {
  if (st.text_case == Style::TextCase::None) return in;
  std::string out;
  out.reserve(in.size());
  bool at_word_start = true;
  for (size_t i = 0; i < in.size(); ++i) {
    unsigned char c = (unsigned char)in[i];
    const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    unsigned char o = c;
    const bool up = st.text_case == Style::TextCase::Upper ||
                    (st.text_case == Style::TextCase::Capitalize && at_word_start);
    const bool down = st.text_case == Style::TextCase::Lower;
    if (up && c >= 'a' && c <= 'z') o = (unsigned char)(c - 'a' + 'A');
    else if (down && c >= 'A' && c <= 'Z') o = (unsigned char)(c - 'A' + 'a');
    else if ((c & 0xC0) == 0xC0 && i + 1 < in.size()) {
      // Two-byte Latin-1 supplement: the case bit is 0x20 in the second byte,
      // and the range that maps is C0..DE against E0..FE.
      const unsigned char c2 = (unsigned char)in[i + 1];
      if (c == 0xC3) {
        if (up && c2 >= 0xA0 && c2 <= 0xBE && c2 != 0xB7) {
          out += (char)c;
          out += (char)(c2 - 0x20);
          i += 1;
          at_word_start = false;
          continue;
        }
        if (down && c2 >= 0x80 && c2 <= 0x9E && c2 != 0x97) {
          out += (char)c;
          out += (char)(c2 + 0x20);
          i += 1;
          at_word_start = false;
          continue;
        }
      }
    }
    out += (char)o;
    at_word_start = !alpha;
  }
  return out;
}

FontKey font_of(const Style& s) {
  FontKey f;
  f.px = s.font_px;
  f.bold = s.bold;
  f.italic = s.italic;
  f.mono = s.mono;
  f.serif = s.serif;
  f.tracking = s.letter_spacing;
  return f;
}

// Elements whose children are laid out but which contribute nothing of their
// own, and elements that are not content at all.
bool is_skipped(const Node& el) {
  return el.tag == "script" || el.tag == "style" || el.tag == "head" || el.tag == "meta" ||
         el.tag == "link" || el.tag == "title" || el.tag == "noscript" || el.tag == "template" ||
         el.tag == "base" || el.tag == "iframe" || el.tag == "object" ||
         el.tag == "embed" ||
         el.tag == "option" || el.tag == "datalist" || el.tag == "map" ||
         el.tag == "area" || el.tag == "track" || el.tag == "source" || el.tag == "param" ||
         el.tag == "col" || el.tag == "colgroup";
}

// An ATOMIC inline: `inline-block`, `inline-flex`, `inline-grid`. It sits on a
// line like a word, but inside it is a full block/flex/grid formatting context,
// so having a <div> in it is normal and must NOT promote it to a block the way
// a real inline element with blocks inside is promoted. Shrink-to-fit width is
// the whole point -- it is what makes `position: relative` on one of these a
// containing block the size of the BUTTON rather than the size of the header.
bool is_atomic_inline(const Style& st) {
  return st.display == Display::InlineBlock || st.inline_level;
}

bool is_block_level(Display d) {
  return d == Display::Block || d == Display::ListItem || d == Display::Table || d == Display::TableRow ||
         d == Display::TableCell || d == Display::Flex || d == Display::Grid;
}

// The tags that are block boxes unless a stylesheet says otherwise. Used only
// to answer "does this inline element have a block inside it", which is a
// question about the document, not about the cascade — and asking it by tag
// costs one comparison where asking it by cascade costs a style computation
// per descendant.
const char* const kBlockTags[] = {
    "address", "article", "aside",   "blockquote", "details", "dd",     "div",     "dl",
    "dt",      "fieldset", "figcaption", "figure", "footer",  "form",   "h1",      "h2",
    "h3",      "h4",      "h5",      "h6",         "header",  "hgroup", "hr",      "li",
    "main",    "nav",     "ol",      "p",          "pre",     "section", "summary", "table",
    "tbody",   "td",      "tfoot",   "th",         "thead",   "tr",     "ul",      "menu",
    "caption", "center",  "dir",     "frameset",   "noframes", "output", nullptr};

// Whether an inline-level element has a block box somewhere inside it.
//
// This is what a 1992 document does to a parser: `<NEXTID 20>` is an unknown
// element with no close tag, so every heading, paragraph and list on the page
// becomes its descendant. An unknown element is inline, and laying that whole
// subtree out as one inline run put a 30px `<h1>` on the same baseline as the
// body text that followed it. Real engines break the inline box open and put
// anonymous block boxes around the block children; this finds the case so
// layout_children can do the same thing by treating the element as a block
// container.
bool has_block_descendant(const Node& el) {
  std::vector<const Node*> stack;
  for (size_t i = el.children.size(); i > 0; --i) stack.push_back(el.children[i - 1].get());
  size_t looked = 0;
  while (!stack.empty() && looked < 600) {
    const Node* n = stack.back();
    stack.pop_back();
    ++looked;
    if (n->kind == Node::Kind::Element && in_list_tags(n->tag, kBlockTags)) return true;
    for (size_t i = n->children.size(); i > 0; --i) stack.push_back(n->children[i - 1].get());
  }
  return false;
}

// ------------------------------------------------------------ inline items --

struct InlineItem {
  enum class Kind : uint8_t { Word, Space, Break, Image, Marker, Field, Box } kind = Kind::Word;
  std::string text;
  Style style;
  // The anchor this item is inside, already resolved to an absolute url, or
  // empty. Carried per item because that is the only place it is unambiguous:
  // an `<a>` can wrap three elements and half a paragraph, and the line it
  // wraps onto is not known until the line box is built.
  std::string href;
  int w = 0;
  int h = 0;
  int ascent = 0;
  std::string src;
  std::string alt;
  Node* node = nullptr;
  // Field only: the box drawn around the label, in the four Style edges that
  // put it there — the UA sheet already gives every control a border and
  // padding, this is just where layout finally reads it.
  int fb[4] = {0, 0, 0, 0};   // top, right, bottom, left border
  int fp[4] = {0, 0, 0, 0};   // top, right, bottom, left padding
  // The word was wider than its line and was cut with an ellipsis.
  bool cut = false;
  // Box only: the fragments and links this item already produced, laid out at a
  // provisional origin. An inline-block is ATOMIC -- it is one item on the line
  // with a box of its own -- so it is laid out here, measured, and then moved
  // to wherever the line breaking decides it goes.
  size_t frag_from = 0, frag_to = 0;
  size_t link_from = 0, link_to = 0;
};

struct Engine {
  Document& doc;
  const Stylesheet& ua;
  const Stylesheet& author;
  const Metrics& m;
  int viewport_w = 1200;
  int viewport_h = 0;          // 0 = unknown: a percentage height has no base
  // The definite height of the box being laid out INTO, or -1. `height: 100%`
  // is a percentage of this, and an app shell -- `html,body{height:100%}` then
  // `#app{height:100vh}` then a column of `flex:1` panes -- is a chain of them.
  // Carried on the Engine rather than threaded through ten signatures, saved
  // and restored around each box, which is what a containing block is.
  int avail_h_ = -1;
  // The containing block's content WIDTH, for percentage widths. Normally that
  // is the width a box is laid out into -- but a flex item is handed its own
  // computed size, and resolving `max-width: 78%` against that gives 78% of
  // itself. A chat bubble sized to its text then clamped itself to 78% of that
  // text and wrapped a line that fitted.
  int cb_w_ = -1;
  // The width of the block whose inline content is being collected, which is
  // what a replaced element's `width: 100%` is a percentage OF. It used to be
  // the viewport, so a thumbnail told to fill its 90px cell came out as wide as
  // the window.
  int inline_cb_w_ = -1;

  // ---- floats --------------------------------------------------------------
  // A float's OUTER box (margins included), in page coordinates, for the block
  // formatting context being laid out. Text beside it is shortened to fit; a
  // box that clears it moves below. A box that starts its own context -- a
  // float itself, `overflow` other than visible, a flex or grid item, a table
  // cell -- gets a fresh list and leaves the outer one as it found it.
  struct FloatBox { int x, y, w, h; bool right; };
  std::vector<FloatBox> floats_;
  struct FloatScope {
    std::vector<FloatBox>* slot;
    std::vector<FloatBox> saved;
    bool on;
    FloatScope(std::vector<FloatBox>* s2, bool enable) : slot(s2), on(enable) {
      if (on) { saved = std::move(*slot); slot->clear(); }
    }
    // The lowest edge of anything floated in this context, or `fallback`.
    int bottom(int fallback) const {
      int b = fallback;
      for (const FloatBox& f : *slot) if (f.y + f.h > b) b = f.y + f.h;
      return b;
    }
    ~FloatScope() { if (on) *slot = std::move(saved); }
  };
  // The part of [x0, x1) a band of text at [y, y+h) can use once floats have
  // taken their share.
  void float_room(int y, int h, int x0, int x1, int& left, int& right) const {
    left = x0;
    right = x1;
    for (const FloatBox& f : floats_) {
      if (f.y >= y + h || f.y + f.h <= y) continue;
      if (f.right) { if (f.x < right) right = f.x; }
      else if (f.x + f.w > left) left = f.x + f.w;
    }
  }
  // The next edge below `y` at which a float ends, or -1.
  int next_float_end(int y) const {
    int best = -1;
    for (const FloatBox& f : floats_) {
      const int e = f.y + f.h;
      if (e > y && (best < 0 || e < best)) best = e;
    }
    return best;
  }
  // Where a `clear` pushes a box to.
  int clear_to(ClearSide c, int y) const {
    int out = y;
    for (const FloatBox& f : floats_) {
      const bool hit = c == ClearSide::Both || (c == ClearSide::Left && !f.right) || (c == ClearSide::Right && f.right);
      if (hit && f.y + f.h > out) out = f.y + f.h;
    }
    return out;
  }
  // A height a flex parent is imposing on the child it is about to lay out --
  // a row item stretched to the line, which is what gives a `flex: 1` column
  // inside it something definite to divide. Consumed once, by that child.
  int imposed_h_ = -1;

  // The alignment the CONTAINING BLOCK is running, for the one box that needs
  // it and cannot read it off its own style: a table. The UA sheet gives
  // `table { text-align: left }` on purpose (so a `<center>` does not centre
  // every cell's text), which means a table's own style cannot say whether the
  // block it sits in is centred -- and a shrink-to-fit table inside `<center>`
  // has been centred by every browser since 1996.
  Align flow_align_ = Align::Left;

  // Where the last table put itself. A table is shrink-to-fit and may centre
  // itself, but its own background and border were emitted by layout_box
  // BEFORE the columns were measured -- at the container's full width. Without
  // this a bordered table drew a rule right across the page with its cells
  // huddled at one end.
  int table_x_ = 0, table_w_ = 0;
  struct FlowAlignScope {
    Align* slot;
    Align was;
    FlowAlignScope(Align* s2, Align now) : slot(s2), was(*s2) { *slot = now; }
    ~FlowAlignScope() { *slot = was; }
  };

  // The `<a>` currently being walked, so every word inside it becomes part of
  // the same link even when the anchor wraps three elements deep.
  std::string open_href;


  // A length in pixels. `vw` and `vh` are percentages of the VIEWPORT, which
  // only this layout knows -- the cascade cannot resolve them, and `vw` used to
  // be turned into a raw pixel count at parse time, so `width: 100vw` came out
  // one hundred pixels wide.
  int len_px(const Length& l, int against, int fallback) const {
    if (l.unit == Length::Unit::Vw)
      return (int)(l.value * (float)viewport_w / 100.0f + l.offset_px + 0.5f);
    if (l.unit == Length::Unit::Vh) {
      if (viewport_h <= 0) return fallback;
      return (int)(l.value * (float)viewport_h / 100.0f + l.offset_px + 0.5f);
    }
    return l.resolve(against, fallback);
  }

  struct MeasureKey {
    uint32_t hash = 0;
    FontKey font;
    std::string s;
    int width = -1;
  };
  mutable std::array<MeasureKey, 4096> measure_cache_{};
  mutable std::unordered_map<const Node*, int> min_content_cache_;
  mutable std::unordered_map<const Node*, int> max_content_cache_;

  static bool font_eq(const FontKey& a, const FontKey& b) {
    return a.px == b.px && a.bold == b.bold && a.italic == b.italic &&
           a.mono == b.mono && a.serif == b.serif && a.tracking == b.tracking;
  }

  int measure(const std::string& s, const Style& st) const {
    FontKey fk = font_of(st);
    uint32_t h = (uint32_t)s.size();
    for (char c : s) h = h * 31 + (unsigned char)c;
    h ^= (uint32_t)(fk.px * 10.0f);
    h ^= (fk.bold ? 1 : 0) | (fk.italic ? 2 : 0) | (fk.mono ? 4 : 0) | (fk.serif ? 8 : 0);
    size_t slot = (size_t)(h & 4095);
    const auto& entry = measure_cache_[slot];
    if (entry.width >= 0 && entry.hash == h && entry.s == s && font_eq(entry.font, fk)) {
      return entry.width;
    }
    int w = m.measure(s, fk);
    measure_cache_[slot] = {h, fk, s, w};
    return w;
  }
  int line_h(const Style& st) const {
    int base = m.line_height(font_of(st));
    return st.line_height > base ? st.line_height : base;
  }
  int ascent(const Style& st) const { return m.ascent ? m.ascent(font_of(st)) : m.line_height(font_of(st)) * 4 / 5; }

  // Whose box is being emitted. Set as the block walk enters an element, so
  // the single emit funnel can attribute every fragment without touching each
  // of the seven places one is built.
  Node* owner_ = nullptr;

  // An out-of-flow box, waiting for its containing block to finish. An
  // absolutely positioned element takes NO space where it was written, so it
  // cannot be laid out during the flow pass -- everything after it would be
  // displaced by a box that is supposed to be over the top of them. It is
  // parked here and laid out once the box it is positioned against knows its
  // own size.
  struct OutOfFlow {
    Node* node;
    Style st;
    bool fixed;        // against the viewport rather than an ancestor
    int z;             // the stacking level it paints at
  };
  std::vector<OutOfFlow> pending_;

  // The stacking level fragments are being emitted at, and the clip in force.
  int z_ = 0;
  int opacity_ = 255;
  bool clip_on_ = false;
  int clip_[4] = {0, 0, 0, 0};   // x, y, w, h

  // True when this element is out of flow, so the caller can park it instead
  // of giving it room. `position: fixed` with no offsets at all is how a page
  // pins something it has already placed, and it is still out of flow.
  static bool out_of_flow(const Style& st) {
    return st.position == Position::Absolute || st.position == Position::Fixed;
  }

  // Park it. Returns true when the caller must not lay it out in flow.
  bool park_if_out_of_flow(Node& child, const Style& cs) {
    if (!out_of_flow(cs)) return false;
    pending_.push_back({&child, cs, cs.position == Position::Fixed,
                        cs.has_z ? cs.z_index : z_});
    return true;
  }

  // Lay out every box parked since `mark` whose containing block is this one.
  // `box` is the padding box it resolves its offsets against.
  void drain_out_of_flow(size_t mark, int bx, int by, int bw, int bh, size_t depth,
                         bool is_root) {
    if (pending_.size() <= mark) return;
    // Taken by value: laying one out can park more (a dialog with its own
    // absolutely positioned close button), and those append to pending_.
    std::vector<OutOfFlow> mine(pending_.begin() + (long)mark, pending_.end());
    pending_.resize(mark);
    for (auto& o : mine) {
      // A fixed box belongs to the viewport, so only the root drains it.
      if (o.fixed && !is_root) {
        pending_.push_back(o);
        continue;
      }
      const int saved_z = z_;
      z_ = o.z;
      // Its width: an explicit one, else what left/right pin between them,
      // else its content.
      const Style& st = o.st;
      const int mh = st.margin[1] + st.margin[3];
      const bool has_l = !st.inset[3].is_auto(), has_r = !st.inset[1].is_auto();
      const bool has_t = !st.inset[0].is_auto(), has_b = !st.inset[2].is_auto();
      int w;
      if (!st.width.is_auto()) {
        w = len_px(st.width, bw, bw);
      } else if (has_l && has_r) {
        w = bw - len_px(st.inset[3], bw, 0) - len_px(st.inset[1], bw, 0) - mh;
      } else {
        // Shrink-to-fit, which is NOT "clamp to the containing block". The spec
        // is min(max(min-content, available), max-content), and for an absolute
        // box the containing block can be far narrower than the box -- a
        // dropdown anchored to a 40px icon button is the standard case. Hard
        // clamping to `bw` squeezed the menu to the width of its trigger, which
        // then made `right: 0` a no-op: the box was already exactly as wide as
        // the space it was being right-aligned in.
        const int prefer = max_content(*o.node, st, 0);
        const int floor_w = min_content(*o.node, st, 0);
        int avail = bw;
        if (has_r) avail -= len_px(st.inset[1], bw, 0);
        if (has_l) avail -= len_px(st.inset[3], bw, 0);
        avail -= mh;
        if (avail < 0) avail = 0;
        w = prefer < avail ? prefer : avail;
        if (w < floor_w) w = floor_w;
        // Still never wider than the viewport -- an intrinsic measurement of a
        // runaway subtree should not produce a mile-wide box.
        if (w > viewport_w) w = viewport_w;
      }
      if (!st.min_width.is_auto()) { const int v = len_px(st.min_width, bw, 0); if (w < v) w = v; }
      if (!st.max_width.is_auto()) { const int v = len_px(st.max_width, bw, bw); if (w > v) w = v; }
      if (w < 1) w = 1;

      // Its position. `left` wins over `right` when both are given and the
      // width is fixed, which is what the spec says for a left-to-right page.
      int px = bx, py = by;
      if (has_l) px = bx + len_px(st.inset[3], bw, 0);
      else if (has_r) px = bx + bw - len_px(st.inset[1], bw, 0) - w - mh;
      if (has_t) py = by + len_px(st.inset[0], bh, 0);

      const size_t first = doc.fragments.size();
      const int h = layout_box(*o.node, st, px, py, w, depth + 1);
      // `bottom` without `top`: the box hangs off the bottom edge, so its
      // height has to be known first -- hence the shift rather than a guess.
      if (has_b && !has_t) {
        const int want = by + bh - len_px(st.inset[2], bh, 0) - h;
        shift_fragments(first, doc.fragments.size(), 0, want - py);
      }
      z_ = saved_z;
    }
  }

  // Sets the opacity every fragment emitted inside this scope is stamped with.
  struct OpacityScope {
    int* slot;
    int was;
    OpacityScope(int* s2, float eff) : slot(s2), was(*s2) {
      int v = (int)(eff * 255.0f + 0.5f);
      *slot = v < 0 ? 0 : (v > 255 ? 255 : v);
    }
    ~OpacityScope() { *slot = was; }
  };

  void emit(Fragment&& f) {
    if (doc.fragments.size() >= kMaxFragments) return;
    if (!f.node) f.node = owner_;
    f.z = z_;
    f.opacity = (uint8_t)(opacity_ < 0 ? 0 : (opacity_ > 255 ? 255 : opacity_));
    if (clip_on_) {
      f.clipped = true;
      f.clip_x = clip_[0];
      f.clip_y = clip_[1];
      f.clip_w = clip_[2];
      f.clip_h = clip_[3];
    }
    doc.fragments.push_back(std::move(f));
  }
  void emit(Fragment& f) { emit(std::move(f)); }

  // Move everything a subtree emitted. A box has to be laid out before its
  // height is known, and alignment (and `position: relative`) is an offset
  // applied AFTER that -- so the box is laid out where it falls and then the
  // fragments it produced are moved as a unit. Cheaper and more accurate than
  // laying the same subtree out twice.
  // [from, to) -- a HALF-OPEN range, because a flex item's fragments end where
  // the next item's begin. Shifting "from here to the end of the list" instead
  // moved every later sibling too, so aligning the second of three items
  // displaced it by the sum of the shifts before it.
  void shift_fragments(size_t from, size_t to, int dx, int dy) {
    if (dx == 0 && dy == 0) return;
    if (to > doc.fragments.size()) to = doc.fragments.size();
    for (size_t i = from; i < to; ++i) {
      doc.fragments[i].x += dx;
      doc.fragments[i].y += dy;
      if (doc.fragments[i].shadow) { doc.fragments[i].cut_x += dx; doc.fragments[i].cut_y += dy; }
      // The clip an `overflow` ancestor imposed is a rectangle in page
      // coordinates, so it moves with what it clips. Left behind, a box that
      // was floated or aligned elsewhere kept a clip over the place it was
      // laid out at, and painted nothing.
      if (doc.fragments[i].clipped) { doc.fragments[i].clip_x += dx; doc.fragments[i].clip_y += dy; }
    }
  }

  // The same for link boxes. A link inside a moved box has to move with it, or
  // the pointer hits where the box WAS -- which, for an inline-block laid out
  // at a provisional origin, is the top-left corner of the page.
  void shift_links(size_t from, size_t to, int dx, int dy) {
    if (dx == 0 && dy == 0) return;
    if (to > doc.links.size()) to = doc.links.size();
    for (size_t i = from; i < to; ++i) {
      doc.links[i].x += dx;
      doc.links[i].y += dy;
    }
  }

  // ---------------------------------------------------------------- inline --

  // Walk an inline subtree, turning it into words, spaces and images. The
  // recursion is bounded by depth; a page cannot drive it past that.
  // The element a node belongs to: itself when it is one, its parent when it
  // is text. A click lands on an element, never on a text node.
  static Node* owning_element(Node& n) {
    if (n.kind == Node::Kind::Element) return &n;
    return n.parent;
  }


  // ---------------------------------------------------------- img sources --

  // The size of a picture the browser has already decoded, so an <img> with no
  // declared size can be boxed correctly the first time round (and again after
  // a fetch teaches the document a size it did not have).
  bool natural_size(const std::string& src, int& w, int& h) const {
    for (const auto& e : doc.image_sizes)
      if (e.first == src) { w = e.second.first; h = e.second.second; return w > 0 && h > 0; }
    return false;
  }

  // One length out of a `sizes` entry: `50vw`, `300px`, `calc(100vw - 40px)`.
  // Anything else is the whole viewport, which is what a missing `sizes` means.
  float sizes_length(std::string t) const {
    auto num_at = [&](size_t& i, float& v, bool& vw) -> bool {
      while (i < t.size() && t[i] == ' ') ++i;
      size_t j = i;
      while (j < t.size() && ((t[j] >= '0' && t[j] <= '9') || t[j] == '.' || t[j] == '-')) ++j;
      if (j == i) return false;
      v = (float)std::atof(t.substr(i, j - i).c_str());
      i = j;
      vw = false;
      if (t.compare(i, 2, "vw") == 0) { vw = true; v = v * (float)viewport_w / 100.0f; i += 2; }
      else if (t.compare(i, 2, "px") == 0) i += 2;
      else if (t.compare(i, 2, "em") == 0) { v *= 16.0f; i += 2; }
      else return false;
      return true;
    };
    for (auto& c : t) c = (char)std::tolower((unsigned char)c);
    while (!t.empty() && t.front() == ' ') t.erase(t.begin());
    size_t i = 0;
    if (t.compare(0, 5, "calc(") == 0) i = 5;
    float a = 0, b = 0;
    bool vw = false;
    if (!num_at(i, a, vw)) return (float)viewport_w;
    if (i == 5 || t.compare(0, 5, "calc(") == 0) {
      while (i < t.size() && t[i] == ' ') ++i;
      if (i < t.size() && (t[i] == '-' || t[i] == '+')) {
        const char op = t[i++];
        if (num_at(i, b, vw)) a = op == '-' ? a - b : a + b;
      }
    }
    return a > 0 ? a : (float)viewport_w;
  }

  // The width an image will be laid out at, from `sizes` (media conditions
  // first-match-wins), or the viewport when there is none.
  float slot_width(const std::string& sizes) const {
    if (sizes.empty()) return (float)viewport_w;
    int depth = 0;
    size_t start = 0;
    for (size_t i = 0; i <= sizes.size(); ++i) {
      const char c = i < sizes.size() ? sizes[i] : ',';
      if (c == '(') ++depth;
      else if (c == ')' && depth > 0) --depth;
      if (!(c == ',' && depth == 0)) continue;
      std::string entry = sizes.substr(start, i - start);
      start = i + 1;
      std::string media;
      size_t k = 0;
      while (k < entry.size() && entry[k] == ' ') ++k;
      // A leading `(...)` group (possibly several joined by `and`) is the condition.
      size_t cond_end = k;
      int d = 0;
      for (size_t m = k; m < entry.size(); ++m) {
        if (entry[m] == '(') ++d;
        else if (entry[m] == ')') { --d; if (d == 0) cond_end = m + 1; }
        else if (d == 0 && entry[m] == ' ' && m + 4 <= entry.size() && entry.compare(m, 5, " and ") == 0) { m += 4; }
        else if (d == 0 && entry[m] != ' ' && m >= cond_end) break;
      }
      if (cond_end > k) media = entry.substr(k, cond_end - k);
      const std::string len = entry.substr(cond_end > k ? cond_end : k);
      if (!media.empty() && !media_applies(media, viewport_w)) continue;
      return sizes_length(len);
    }
    return (float)viewport_w;
  }

  // Which address an <img> shows: the first <picture><source> that applies, else
  // the img's own srcset, else its src. A srcset candidate is the smallest that
  // still covers the slot at 1x, or the largest there is. A <source> for a
  // format this engine cannot decode (webp, avif) is skipped so the <img>
  // fallback inside it -- a jpeg or png -- is what shows.
  std::string pick_image_source(Node& img) const {
    std::string srcset = img.attr("srcset");
    std::string sizes = img.attr("sizes");
    if (img.parent && img.parent->tag == "picture") {
      for (auto& kid : img.parent->children) {
        if (!kid || kid.get() == &img) break;
        if (kid->kind != Node::Kind::Element || kid->tag != "source") continue;
        std::string type = kid->attr("type");
        for (auto& c : type) c = (char)std::tolower((unsigned char)c);
        if (!type.empty() && type != "image/jpeg" && type != "image/jpg" && type != "image/png" && type != "image/gif" &&
            type != "image/svg+xml" && type != "image/bmp")
          continue;
        const std::string media = kid->attr("media");
        if (!media.empty() && !media_applies(media, viewport_w)) continue;
        const std::string ss = kid->attr("srcset");
        if (ss.empty()) continue;
        srcset = ss;
        sizes = kid->attr("sizes");
        break;
      }
    }
    if (srcset.empty()) return img.attr("src");

    const float slot = slot_width(sizes);
    std::string best, largest;
    float best_d = 1e30f, largest_d = -1.0f;
    size_t i = 0;
    int guard = 0;
    while (i < srcset.size() && ++guard < 256) {
      while (i < srcset.size() && (srcset[i] == ' ' || srcset[i] == ',' || srcset[i] == '\n' || srcset[i] == '\t')) ++i;
      size_t u = i;
      while (u < srcset.size() && srcset[u] != ' ' && srcset[u] != '\n' && srcset[u] != '\t') ++u;
      std::string url = srcset.substr(i, u - i);
      i = u;
      float density = 1.0f;
      if (!url.empty() && url.back() == ',') {
        while (!url.empty() && url.back() == ',') url.pop_back();
      } else {
        size_t e = srcset.find(',', i);
        if (e == std::string::npos) e = srcset.size();
        const std::string desc = srcset.substr(i, e - i);
        i = e;
        const size_t a = desc.find_first_not_of(" \t\n");
        if (a != std::string::npos) {
          const float v = (float)std::atof(desc.c_str() + a);
          const size_t z = desc.find_last_not_of(" \t\n");
          if (v > 0 && desc[z] == 'w') density = v / (slot > 0 ? slot : 1.0f);
          else if (v > 0 && desc[z] == 'x') density = v;
        }
      }
      if (url.empty()) continue;
      if (density >= 1.0f && density < best_d) { best_d = density; best = url; }
      if (density > largest_d) { largest_d = density; largest = url; }
    }
    if (!best.empty()) return best;
    if (!largest.empty()) return largest;
    return img.attr("src");
  }

  // An inline <svg>, rasterised into the document's own picture store and
  // handed on as an image. Everything downstream -- the fragment, the painter,
  // the alpha blend -- already knows how to draw one of those.
  bool svg_image(Node& node, const Style& st, int& w, int& h, std::string& key) {
    if (node.tag != "svg") return false;
    int want_w = len_px(st.width, viewport_w, -1);
    int want_h = len_px(st.height, viewport_w, -1);
    if (want_w < 0) {
      const std::string a = node.attr("width");
      want_w = a.empty() ? -1 : (int)atoi(a.c_str());
    }
    if (want_h < 0) {
      const std::string a = node.attr("height");
      want_h = a.empty() ? -1 : (int)atoi(a.c_str());
    }
    // An icon with no size named anywhere is the size of the text beside it,
    // which is what `1em` would have made it.
    if (want_w <= 0 && want_h <= 0) { want_w = want_h = st.font_px > 0 ? st.font_px : 16; }
    if (want_w <= 0) want_w = want_h;
    if (want_h <= 0) want_h = want_w;
    if (want_w <= 0 || want_h <= 0) return false;

    // Keyed by POSITION in the document's own picture store, not by the node's
    // address: a key with a pointer in it changes on every run, which a golden
    // cannot be held to.
    key = "svg#" + std::to_string(doc.inline_images.size());
    auto bm = rasterise_svg(node, want_w, want_h, st.color);
    if (!bm.ok()) return false;
    doc.inline_images.emplace_back(key, std::move(*bm));
    w = want_w;
    h = want_h;
    return true;
  }


  // A generated box's text, as an inline item. `::before` and `::after` are how
  // a page writes separators, quote marks, required-field stars and the little
  // decorations between chips -- `.tok-seg + .tok-seg::before { content: " · " }`
  // is why a usage readout reads as one run-on word without them.
  void push_pseudo(Node& node, const Style& st, std::vector<InlineItem>& out, bool after) {
    Style ps;
    if (!pseudo_content(node, st, ua, author, viewport_w, after, &ps)) return;
    if (ps.display == Display::None || ps.content.empty()) return;
    InlineItem it;
    it.kind = InlineItem::Kind::Word;
    it.text = ps.content;
    it.style = ps;
    it.href = open_href;
    it.node = owning_element(node);
    it.w = measure(it.text, ps);
    it.h = line_h(ps);
    it.ascent = ascent(ps);
    out.push_back(std::move(it));
  }

  void collect_inline(Node& node, const Style& parent, std::vector<InlineItem>& out, size_t depth) {
    if (depth > kMaxBoxDepth) return;
    if (node.kind == Node::Kind::Text) {
      if (parent.hidden) return;
      const std::string t = cased(node.text, parent);
      if (parent.white_space == WhiteSpace::Pre) {
        // Preformatted: newlines are breaks and runs of spaces are kept.
        std::string word;
        auto flush_word = [&]() {
          if (word.empty()) return;
          InlineItem it;
          it.kind = InlineItem::Kind::Word;
          it.text = word;
          it.style = parent;
          it.href = open_href;
          it.w = measure(word, parent);
          it.h = line_h(parent);
          it.ascent = ascent(parent);
          out.push_back(std::move(it));
          word.clear();
        };
        for (char c : t) {
          if (c == '\n') {
            flush_word();
            InlineItem br;
            br.kind = InlineItem::Kind::Break;
            br.style = parent;
            br.h = line_h(parent);
            br.ascent = ascent(parent);
            out.push_back(std::move(br));
            continue;
          }
          if (c == '\t') {
            flush_word();
            word = "    ";
            flush_word();
            continue;
          }
          if (c == '\r') continue;
          word += c;
        }
        flush_word();
        return;
      }
      // Normal: collapse every run of whitespace to one space, and let the
      // line breaker decide which of those spaces disappear.
      size_t i = 0;
      while (i < t.size()) {
        if (is_space(t[i])) {
          while (i < t.size() && is_space(t[i])) ++i;
          InlineItem sp;
          sp.node = owning_element(node);
          sp.kind = InlineItem::Kind::Space;
          sp.style = parent;
          sp.href = open_href;
          sp.w = measure(" ", parent);
          sp.h = line_h(parent);
          sp.ascent = ascent(parent);
          out.push_back(std::move(sp));
          continue;
        }
        size_t start = i;
        while (i < t.size() && !is_space(t[i])) ++i;
        InlineItem w;
        w.node = owning_element(node);
        w.kind = InlineItem::Kind::Word;
        w.text = t.substr(start, i - start);
        w.style = parent;
        w.href = open_href;
        w.w = measure(w.text, parent);
        w.h = line_h(parent);
        w.ascent = ascent(parent);
        out.push_back(std::move(w));
      }
      return;
    }
    if (node.kind != Node::Kind::Element) return;
    if (is_skipped(node)) return;

    Style st = compute_style(node, parent, ua, author, viewport_w);
    if (st.display == Display::None || st.hidden) return;

    if (node.tag == "br") {
      InlineItem br;
      br.kind = InlineItem::Kind::Break;
      br.style = st;
      br.h = line_h(st);
      br.ascent = ascent(st);
      out.push_back(std::move(br));
      return;
    }
    if (node.tag == "canvas") {
      InlineItem im;
      im.kind = InlineItem::Kind::Image;
      im.node = owning_element(node);
      im.style = st;
      char buf[64];
      std::snprintf(buf, sizeof buf, "canvas://%p", (void*)&node);
      im.src = buf;
      im.alt = "canvas";
      int w = len_px(st.width, viewport_w, -1);
      int h = len_px(st.height, viewport_w, -1);
      if (w < 0) {
        std::string a = node.attr("width");
        if (!a.empty()) w = std::atoi(a.c_str());
        else w = 300;
      }
      if (h < 0) {
        std::string a = node.attr("height");
        if (!a.empty()) h = std::atoi(a.c_str());
        else if (st.aspect_ratio > 0.0f) h = (int)((float)w / st.aspect_ratio);
        else h = 150;
      }
      im.w = std::max(1, w);
      im.h = std::max(1, h);
      im.ascent = im.h;
      im.href = open_href;
      out.push_back(std::move(im));
      return;
    }
    if (node.tag == "video") {
      InlineItem im;
      im.kind = InlineItem::Kind::Image;
      im.node = owning_element(node);
      im.style = st;
      std::string src = node.attr("src");
      if (src.empty()) {
        if (Node* s = node.first("source")) src = s->attr("src");
      }
      im.src = resolve_url(doc.base, src);
      im.alt = node.has_attr("title") ? node.attr("title") : (src.empty() ? "Video" : src);
      int w = len_px(st.width, viewport_w, -1);
      int h = len_px(st.height, viewport_w, -1);
      if (w < 0) {
        std::string a = node.attr("width");
        if (!a.empty()) w = std::atoi(a.c_str());
        else w = 300;
      }
      if (h < 0) {
        std::string a = node.attr("height");
        if (!a.empty()) h = std::atoi(a.c_str());
        else if (st.aspect_ratio > 0.0f) h = (int)((float)w / st.aspect_ratio);
        else h = 150;
      }
      im.w = std::max(1, w);
      im.h = std::max(1, h);
      im.ascent = im.h;
      im.href = open_href;
      out.push_back(std::move(im));
      return;
    }
    if (node.tag == "audio") {
      InlineItem f;
      f.node = owning_element(node);
      f.kind = InlineItem::Kind::Field;
      f.style = st;
      std::string src = node.attr("src");
      if (src.empty()) {
        if (Node* s = node.first("source")) src = s->attr("src");
      }
      std::string title = node.has_attr("title") ? node.attr("title") : (src.empty() ? "Audio" : src);
      f.text = "\u25B6 " + title + "  [0:00 / 0:00]";
      f.href = open_href;
      int w = len_px(st.width, viewport_w, -1);
      int h = len_px(st.height, viewport_w, -1);
      if (w < 0) w = 300;
      if (h < 0) h = 36;
      f.w = std::max(1, w);
      f.h = std::max(1, h);
      f.ascent = f.h;
      out.push_back(std::move(f));
      return;
    }
    if (node.tag == "img") {
      InlineItem im;
      im.kind = InlineItem::Kind::Image;
      im.style = st;
      im.src = resolve_url(doc.base, pick_image_source(node));
      im.alt = node.attr("alt");
      // What the page says the picture is, in this order: the style, then the
      // HTML attribute, then a placeholder that at least holds a line.
      // A percentage is a percentage of the containing block: its width for
      // the width, its height -- if it HAS one -- for the height. Resolved
      // against the viewport instead, `width:100%` in a 90px thumbnail cell
      // made a 1881px picture and a 34,000px search page.
      const int w_base = inline_cb_w_ >= 0 ? inline_cb_w_ : viewport_w;
      int w = len_px(st.width, w_base, -1);
      // ...and only against a containing block whose height is its own to
      // give: an auto-height parent makes `height:100%` mean `auto`.
      const bool parent_h_definite = !parent.height.is_auto() && parent.height.unit != Length::Unit::Percent;
      int h = st.height.unit == Length::Unit::Percent ? (avail_h_ >= 0 && parent_h_definite ? len_px(st.height, avail_h_, -1) : -1)
                                                      : len_px(st.height, viewport_w, -1);
      // The attribute is only a hint for a size the stylesheet leaves alone: a
      // CSS width that cannot be resolved yet (a percentage of a box with no
      // height) is `auto`, not "use the attribute".
      if (w < 0 && st.width.is_auto()) {
        std::string a = node.attr("width");
        int n = 0;
        bool any = false;
        for (char c : a) {
          if (c < '0' || c > '9') break;
          n = n * 10 + (c - '0');
          any = true;
        }
        if (any) w = n;
      }
      if (h < 0 && st.height.is_auto()) {
        std::string a = node.attr("height");
        int n = 0;
        bool any = false;
        for (char c : a) {
          if (c < '0' || c > '9') break;
          n = n * 10 + (c - '0');
          any = true;
        }
        if (any) h = n;
      }
      if (im.src.empty()) return;
      if (w < 0) w = 0;
      if (h < 0) h = 0;
      // The picture's own shape, once it is known, decides what an `auto` side
      // of the box is. Before that the markup's width/height pair (or an
      // `aspect-ratio`) stands in, so a picture that has not loaded yet still
      // holds the room it will take and the text around it does not jump.
      int nat_w = 0, nat_h = 0;
      const bool have_nat = natural_size(im.src, nat_w, nat_h);
      float ratio = have_nat ? (float)nat_w / (float)nat_h : st.aspect_ratio;
      if (ratio <= 0 && w > 0 && h > 0) ratio = (float)w / (float)h;
      if (w <= 0 && h <= 0) {
        if (have_nat) { w = nat_w; h = nat_h; }
      } else if (ratio > 0) {
        // One side from the stylesheet and the other auto: the auto side
        // follows the ratio, not a stale width/height attribute.
        if (!st.width.is_auto() && st.height.is_auto() && w > 0) h = std::max(1, (int)((float)w / ratio + 0.5f));
        else if (!st.height.is_auto() && st.width.is_auto() && h > 0) w = std::max(1, (int)((float)h * ratio + 0.5f));
        else if (w > 0 && h <= 0) h = std::max(1, (int)((float)w / ratio + 0.5f));
        else if (h > 0 && w <= 0) w = std::max(1, (int)((float)h * ratio + 0.5f));
      }
      // `max-width: 100%; height: auto`, the responsive-image rule: shrink the
      // width and let the height follow.
      if (w > 0 && !st.max_width.is_auto()) {
        const int mw = len_px(st.max_width, w_base, -1);
        if (mw >= 0 && w > mw) {
          if (h > 0 && st.height.is_auto()) h = std::max(1, (int)((long long)h * mw / w));
          w = mw;
        }
      }
      // A picture left at its natural size is not allowed to be wider than the
      // box it sits in.
      if (have_nat && st.width.is_auto() && inline_cb_w_ > 0 && w > inline_cb_w_) {
        if (h > 0) h = std::max(1, (int)((long long)h * inline_cb_w_ / w));
        w = inline_cb_w_;
      }
      if (w <= 0 && h <= 0) { w = 0; h = 0; }      // unknown: measured after decode
      im.w = w;
      im.h = h;
      im.ascent = h > 0 ? h : 0;
      im.href = open_href;
      out.push_back(std::move(im));
      return;
    }

    // Form controls are void (input) or carry their initial value as a text
    // child (button, textarea) — either way the generic recursion below finds
    // no words to lay out and the box the UA sheet drew a border for never
    // gets painted. Give each one a label and let it flow like an image: a
    // fixed-size inline box the line breaker can wrap around.
    // A <button> WITH ELEMENT CHILDREN is not a label, it is a box with things
    // in it -- an icon, a pulse dot, a caret -- and it has to lay them out like
    // any other box. Only a button whose content is plain text (or nothing) is
    // the single-label control this branch draws. <input> is void and <select>
    // shows one option, so those are always labels.
    //
    // This matters now in a way it did not before: `display: inline-flex` used
    // to be mis-parsed as block-level `flex`, so an inline-flex button never
    // reached this code. Fixing that sent the console's environment toggle
    // through here, and it came out as a control labelled with the whitespace
    // between its three icons.
    const bool control_has_boxes = [&] {
      if (node.tag != "button") return false;
      for (const auto& kid : node.children)
        if (kid && kid->kind == Node::Kind::Element) return true;
      return false;
    }();
    if (!control_has_boxes && (node.tag == "input" || node.tag == "button" ||
                               node.tag == "textarea" || node.tag == "select" ||
                               node.tag == "progress" || node.tag == "meter")) {
      std::string type = node.tag == "input" ? node.attr("type") : "";
      for (auto& c : type) c = (char)tolower((unsigned char)c);
      if (type == "hidden") return;

      std::string label;
      bool checkbox_like = false;
      if (node.tag == "input") {
        if (type == "checkbox") { checkbox_like = true; label = node.has_attr("checked") ? "[x]" : "[ ]"; }
        else if (type == "radio") { checkbox_like = true; label = node.has_attr("checked") ? "(*)" : "( )"; }
        else if (type == "submit") label = node.has_attr("value") ? node.attr("value") : "Submit";
        else if (type == "button") label = node.has_attr("value") ? node.attr("value") : "Button";
        else if (type == "password") label = std::string(node.attr("value").size(), '*');
        else if (node.has_attr("value")) label = node.attr("value");
        else label = node.attr("placeholder");
      } else if (node.tag == "textarea") {
        label = node.inner_text();
        if (label.empty()) label = node.attr("placeholder");
      } else if (node.tag == "select") {
        // What a closed dropdown shows: the selected option, or the first one
        // -- which is what a browser shows when nothing is marked selected.
        const Node* chosen = nullptr;
        const Node* first_opt = nullptr;
        std::function<void(const Node&)> scan = [&](const Node& n) {
          for (const auto& kid : n.children) {
            if (!kid) continue;
            if (kid->kind == Node::Kind::Element && kid->tag == "option") {
              if (!first_opt) first_opt = kid.get();
              if (kid->has_attr("selected") && !chosen) chosen = kid.get();
            }
            scan(*kid);
          }
        };
        scan(node);
        const Node* show = chosen ? chosen : first_opt;
        if (show) {
          label = show->has_attr("label") ? show->attr("label") : show->inner_text();
        }
        // The arrow that says this opens. Drawn as type, because the atlas has
        // the glyph and a control's chevron should scale with its text.
        label += label.empty() ? "\u25BE" : "  \u25BE";
      } else if (node.tag == "progress" || node.tag == "meter") {
        // Drawn as a bar, with no label (its text is only the no-support fallback).
      } else {
        label = node.inner_text();
        if (label.empty()) label = node.has_attr("value") ? node.attr("value") : "Submit";
      }

      InlineItem f;
      f.node = owning_element(node);
      f.kind = InlineItem::Kind::Field;
      f.style = st;
      f.text = label;
      f.href = open_href;
      f.fb[0] = st.border[0]; f.fb[1] = st.border[1]; f.fb[2] = st.border[2]; f.fb[3] = st.border[3];
      f.fp[0] = st.padding[0]; f.fp[1] = st.padding[1]; f.fp[2] = st.padding[2]; f.fp[3] = st.padding[3];
      // What a control is as wide as, in the order a browser decides it:
      //
      //   1. its own CSS `width`/`height`, which used to be IGNORED here --
      //      this path never looked at the style at all, so `.lst{width:496px}`
      //      on google.com's search box did nothing and the field came out at
      //      the 90px floor below, one sixth of the form it is the point of;
      //   2. `size` (or `cols`) in characters, for a FIELD -- a text box is not
      //      sized by what is typed in it, which is why an empty one is not
      //      small and a full one does not grow;
      //   3. its label, for a button, which is the only control whose text is
      //      really its width.
      const int hpad = f.fp[1] + f.fp[3] + f.fb[1] + f.fb[3];
      const int vpad = f.fp[0] + f.fp[2] + f.fb[0] + f.fb[2];
      const int text_w = measure(label.empty() ? " " : label, st);
      const bool button_like = node.tag == "button" || type == "submit" || type == "button" ||
                               type == "reset" || node.tag == "select";
      const bool field_like = !checkbox_like && !button_like;

      int content_w;
      if (field_like) {
        int chars = 0;
        for (char c : node.attr(node.tag == "textarea" ? "cols" : "size")) {
          if (c < '0' || c > '9') { chars = 0; break; }
          chars = chars * 10 + (c - '0');
          if (chars > 1000) break;
        }
        if (chars <= 0) chars = 20;           // the HTML default for both
        const int ch = measure("0", st);      // the average advance, as a browser takes it
        content_w = chars * (ch > 0 ? ch : 8);
        if (text_w > content_w) content_w = text_w;
      } else {
        const int floor_w = checkbox_like ? 0 : 24;
        content_w = text_w > floor_w ? text_w : floor_w;
      }

      int content_h = line_h(st);
      // A gauge is a fixed-size bar a browser draws itself (Chrome: progress
      // 160x16, meter 80x16); its fill is painted where the field box is.
      if (node.tag == "progress" || node.tag == "meter") {
        content_w = node.tag == "progress" ? 160 : 80;
        content_h = 16;
      }
      if (node.tag == "textarea") {
        int rows = 0;
        for (char c : node.attr("rows")) {
          if (c < '0' || c > '9') { rows = 0; break; }
          rows = rows * 10 + (c - '0');
          if (rows > 200) break;
        }
        if (rows <= 0) rows = 2;
        content_h = line_h(st) * rows;
      }

      // A declared size wins, and `box-sizing` decides whether the number it
      // names already contains the frame.
      const int want_w = len_px(st.width, viewport_w, -1);
      if (want_w >= 0) content_w = st.border_box ? want_w - hpad : want_w;
      const int want_h = len_px(st.height, avail_h_ >= 0 ? avail_h_ : 0, -1);
      if (want_h >= 0) content_h = st.border_box ? want_h - vpad : want_h;
      if (!st.min_width.is_auto()) {
        const int v = len_px(st.min_width, viewport_w, 0) - (st.border_box ? hpad : 0);
        if (content_w < v) content_w = v;
      }
      if (!st.max_width.is_auto()) {
        const int v = len_px(st.max_width, viewport_w, viewport_w) - (st.border_box ? hpad : 0);
        if (content_w > v) content_w = v;
      }
      if (content_w < 0) content_w = 0;
      if (content_h < 0) content_h = 0;

      f.w = content_w + hpad;
      f.h = content_h + vpad;
      f.ascent = f.h;
      out.push_back(std::move(f));
      return;
    }

    if (node.tag == "svg") {
      int iw = 0, ih = 0;
      std::string key;
      if (svg_image(node, st, iw, ih, key)) {
        InlineItem im;
        im.kind = InlineItem::Kind::Image;
        im.style = st;
        im.src = key;
        im.alt = node.attr("aria-label");
        im.w = iw;
        im.h = ih;
        im.ascent = ih;
        im.href = open_href;
        im.node = owning_element(node);
        out.push_back(std::move(im));
      }
      return;
    }

    // `display: inline-block` -- a box that sits ON a line rather than breaking
    // it. Every pill, badge, chip, icon button and inline control on a modern
    // page is one. InlineBlock was in the Display enum and referred to NOWHERE
    // in this file, so all of them were flattened into their own text: no
    // background, no border, no padding, no size. The Assay console's status
    // pills and the dot beside a running session simply were not there.
    //
    // It is laid out HERE, at a provisional origin, and moved to its place on
    // the line once the line is broken. Shrink-to-fit width, like the spec
    // says: the narrower of what it needs and what is available.
    if (is_atomic_inline(st) && depth < kMaxBoxDepth) {
      const int avail = viewport_w;
      int want = len_px(st.width, avail, -1);
      if (want < 0) {
        want = max_content(node, parent, 0);
        const int cap = avail;
        if (want > cap) want = cap;
      }
      if (want < 0) want = 0;

      InlineItem b;
      b.kind = InlineItem::Kind::Box;
      b.style = st;
      b.node = owning_element(node);
      b.href = open_href;
      b.frag_from = doc.fragments.size();
      b.link_from = doc.links.size();
      const int h = layout_box(node, st, 0, 0, want, depth + 1);
      b.frag_to = doc.fragments.size();
      b.link_to = doc.links.size();

      // What it actually occupies: the widest fragment it produced, so a box
      // whose content is narrower than the room it was offered does not leave a
      // gap the size of the viewport after it.
      int right = 0;
      for (size_t i = b.frag_from; i < b.frag_to; ++i) {
        const int r = doc.fragments[i].x + doc.fragments[i].w;
        if (r > right) right = r;
      }
      const int explicit_w = len_px(st.width, avail, -1);
      b.w = explicit_w >= 0 ? explicit_w : (right > 0 ? right : want);
      // Never WIDER than the shrink-to-fit width it was laid out at: an
      // absolutely positioned descendant can paint outside this box (that is
      // what `right: 0` on a menu does), and counting its fragments here made
      // the box as wide as the thing hanging off it.
      if (explicit_w < 0 && b.w > want) b.w = want;
      b.h = h;
      b.ascent = h;
      if (b.w <= 0 && b.h <= 0) {
        // Nothing came of it -- drop the item rather than reserving a hole.
        doc.fragments.resize(b.frag_from);
        doc.links.resize(b.link_from);
        return;
      }
      out.push_back(std::move(b));
      return;
    }

    bool anchor = node.tag == "a" && node.has_attr("href");
    std::string saved = open_href;
    if (anchor) open_href = resolve_url(doc.base, node.attr("href"));
    push_pseudo(node, st, out, false);
    for (auto& child : node.children) collect_inline(*child, st, out, depth + 1);
    push_pseudo(node, st, out, true);
    if (anchor) open_href = saved;
  }

  // Break the items into lines and paint them. Returns the height used.
  //
  // A Link entry is made HERE and not while collecting, because the box the
  // pointer has to hit is the line box, and that does not exist until the line
  // is broken. An anchor that wraps gets one entry per line, each pointing at
  // the same href — which is exactly how a wrapped link behaves.
  int flow_inline(std::vector<InlineItem>& items, int x, int y, int width, Align align) {
    if (items.empty()) return 0;
    if (width < 16) width = 16;
    int cursor_y = y;
    size_t i = 0;

    const int flow_x = x, flow_w = width;
    while (i < items.size()) {
      // Gather one line's worth. Leading spaces at the start of a line are
      // dropped: a wrap must not indent the next line by a space.
      while (i < items.size() && items[i].kind == InlineItem::Kind::Space) ++i;
      if (i >= items.size()) break;
      // Floats take room off the line. The band is the first item's own height;
      // if even that item does not fit in what is left, the line drops below
      // the nearest float and tries again, which is how text flows past the
      // bottom of a picture.
      int x = flow_x, width = flow_w;
      if (!floats_.empty()) {
        for (int guard = 0; guard < 64; ++guard) {
          const int band = items[i].h > 0 ? items[i].h : 18;
          int lo, hi;
          float_room(cursor_y, band, flow_x, flow_x + flow_w, lo, hi);
          x = lo;
          width = hi - lo;
          const int first_w = items[i].w;
          if (width >= first_w || width >= flow_w) break;
          const int below = next_float_end(cursor_y);
          if (below < 0) break;
          cursor_y = below;
        }
        if (width < 16) width = 16;
      }

      size_t line_start = i;
      size_t line_end = i;
      int line_w = 0;
      int max_h = 0;
      int max_ascent = 0;
      bool forced_break = false;

      while (i < items.size()) {
        InlineItem& it = items[i];
        if (it.kind == InlineItem::Kind::Break) {
          forced_break = true;
          if (it.h > max_h) max_h = it.h;
          if (it.ascent > max_ascent) max_ascent = it.ascent;
          ++i;
          line_end = i;
          break;
        }
        int w = it.w;
        // A single word wider than the line is CUT to the line, with an
        // ellipsis. It used to go on whole and overflow -- into the next flex
        // column, where a bold 16px SKU drew on top of the item's name and the
        // two read as one garbled word. What does not fit is not drawn; the
        // machine's look step reports the cut so the mind can widen the box.
        if (line_w == 0 && w > width && it.kind == InlineItem::Kind::Word && it.text.size() > 1 &&
            it.style.white_space != WhiteSpace::Nowrap) {
          const int ell_w = measure("\u2026", it.style);
          std::string cut = it.text;
          while (!cut.empty() && measure(cut, it.style) + ell_w > width) {
            size_t at = cut.size() - 1;
            while (at > 0 && ((unsigned char)cut[at] & 0xC0) == 0x80) --at;
            cut.erase(at);
          }
          if (!cut.empty() && cut.size() < it.text.size()) {
            it.text = cut + "\u2026";
            it.w = measure(it.text, it.style);
            it.cut = true;
            w = it.w;
          }
        }
        // `white-space: nowrap` -- the item says its line is not to be broken, so
        // it stays on this one and overflows rather than becoming a column of
        // single words.
        if (line_w > 0 && line_w + w > width && it.style.white_space != WhiteSpace::Nowrap) break;
        line_w += w;
        if (it.h > max_h) max_h = it.h;
        if (it.ascent > max_ascent) max_ascent = it.ascent;
        ++i;
        line_end = i;
      }
      if (line_end == line_start) {
        ++i;
        continue;
      }
      // Trailing spaces do not count towards the line's width, so a centred
      // line is centred on its words.
      int visible_w = line_w;
      size_t last = line_end;
      while (last > line_start && items[last - 1].kind != InlineItem::Kind::Word &&
             items[last - 1].kind != InlineItem::Kind::Image &&
             items[last - 1].kind != InlineItem::Kind::Box &&
             items[last - 1].kind != InlineItem::Kind::Field) {
        visible_w -= items[last - 1].w;
        --last;
      }
      if (max_h <= 0) max_h = 18;

      int pen_x = x;
      if (align == Align::Center && visible_w < width) pen_x = x + (width - visible_w) / 2;
      else if (align == Align::Right && visible_w < width) pen_x = x + (width - visible_w);

      // One Text fragment per run of items sharing a face, a colour and a
      // link. Merging them is not an optimisation: an underline has to be
      // continuous across the spaces inside a link, and two fragments with a
      // gap between them draw two underlines.
      size_t run_start = line_start;
      while (run_start < last) {
        const InlineItem& first = items[run_start];
        // Everything this iteration emits belongs to the item's own box, and
        // paints at its opacity.
        OpacityScope op_scope(&opacity_, first.style.effective_opacity);
        if (first.kind == InlineItem::Kind::Image) {
          Fragment f;
          f.node = first.node;
          f.kind = Fragment::Kind::Image;
          if (first.node) {
            if (first.node->tag == "video") {
              f.kind = Fragment::Kind::Video;
              f.poster = first.node->attr("poster");
              f.media_controls = !first.node->has_attr("nocontrols");
              f.media_title = first.alt;
            } else if (first.node->tag == "canvas") {
              f.kind = Fragment::Kind::Canvas;
            } else if (first.node->tag == "audio") {
              f.kind = Fragment::Kind::Audio;
              f.media_controls = !first.node->has_attr("nocontrols");
              f.media_title = first.alt;
            }
          }
          f.x = pen_x;
          f.w = first.w;
          f.h = first.h;
          f.y = cursor_y + (first.h > 0 && max_ascent > first.ascent ? max_ascent - first.ascent : 0);
          f.src = first.src;
          f.alt = first.alt;
          f.fit = (uint8_t)first.style.object_fit;
          f.pos_x = first.style.object_pos_x;
          f.pos_y = first.style.object_pos_y;
          f.color = first.style.color;
          f.font = font_of(first.style);
          f.link = -1;
          if (!first.href.empty()) {
            Link ln;
            ln.href = first.href;
            ln.text = first.alt.empty() ? first.src : first.alt;
            ln.x = f.x;
            ln.y = f.y;
            ln.w = f.w;
            ln.h = f.h > 0 ? f.h : max_h;
            doc.links.push_back(ln);
            f.link = (int)doc.links.size() - 1;
          }
          emit(f);
          pen_x += first.w;
          ++run_start;
          continue;
        }
        if (first.kind == InlineItem::Kind::Box) {
          // Already laid out at (0,0) during collection: move it to where the
          // line put it, and everything inside it -- fragments and link boxes
          // alike -- comes along.
          const int box_x = pen_x;
          const int box_y = cursor_y + (max_ascent > first.ascent ? max_ascent - first.ascent : 0);
          shift_fragments(first.frag_from, first.frag_to, box_x, box_y);
          shift_links(first.link_from, first.link_to, box_x, box_y);
          if (!first.href.empty()) {
            Link ln;
            ln.href = first.href;
            ln.text = first.text;
            ln.x = box_x;
            ln.y = box_y;
            ln.w = first.w;
            ln.h = first.h;
            doc.links.push_back(ln);
          }
          pen_x += first.w;
          ++run_start;
          continue;
        }
        if (first.kind == InlineItem::Kind::Field) {
          // Whose control this is, for the three layers below. Without it a
          // button's own box had no element and a click on it hit nothing —
          // which is every button on every page.
          struct FieldOwner {
            Node** slot;
            Node* was;
            ~FieldOwner() { *slot = was; }
          } fo{const_cast<Node**>(&owner_), owner_};
          if (first.node) owner_ = first.node;
          // A field is its own little box: background, border, then its label
          // inset by the padding and border the UA sheet gave it — the same
          // three layers a block box gets, just inline and one item wide.
          int box_x = pen_x;
          int box_y = cursor_y + (max_ascent > first.ascent ? max_ascent - first.ascent : 0);
          if (first.style.has_background && (first.style.background >> 24)) {
            Fragment bg;
            bg.kind = Fragment::Kind::Rect;
            bg.x = box_x;
            bg.y = box_y;
            bg.w = first.w;
            bg.h = first.h;
            bg.color = first.style.background;
            bg.radius = first.style.radius;
            emit(bg);
          }
          // A control the page paints NOTHING on -- a search field whose form
          // draws the white pill around it, which is how most of them are made
          // -- still occupies its box, and a click there has to land on it.
          // Hit-testing reads painted fragments, so a field with no background
          // and no border had no area at all and could not be focused. A
          // transparent rect is the box, unseen.
          if (!(first.style.has_background && (first.style.background >> 24)) &&
              !(first.fb[0] || first.fb[1] || first.fb[2] || first.fb[3])) {
            Fragment hit;
            hit.kind = Fragment::Kind::Rect;
            hit.x = box_x;
            hit.y = box_y;
            hit.w = first.w;
            hit.h = first.h;
            hit.color = 0x00000000;
            emit(hit);
          }
          // <progress>/<meter>: a track and the filled part of it. Indeterminate
          // progress (no value) shows a partly filled bar, as a still frame.
          if (first.node && (first.node->tag == "progress" || first.node->tag == "meter") &&
              first.w > 0 && first.h > 0) {
            const Node& g = *first.node;
            auto num = [&](const char* a, double d) {
              if (!g.has_attr(a)) return d;
              char* e = nullptr;
              const std::string v = g.attr(a);
              double r = strtod(v.c_str(), &e);
              return (e == v.c_str() || r != r) ? d : r;
            };
            const bool prog = g.tag == "progress";
            double lo = prog ? 0 : num("min", 0), hi = prog ? num("max", 1) : num("max", 1);
            if (prog && hi <= 0) hi = 1;
            if (hi < lo) hi = lo;
            double v = num("value", prog ? -1 : 0);
            double frac = hi > lo ? (v - lo) / (hi - lo) : 0;
            if (prog && v < 0) frac = 0.3;
            if (frac < 0) frac = 0;
            if (frac > 1) frac = 1;
            Fragment track;
            track.kind = Fragment::Kind::Rect;
            track.x = box_x; track.y = box_y; track.w = first.w; track.h = first.h;
            track.color = 0xFFE6E6E6;
            track.radius = first.h / 2;
            emit(track);
            uint32_t fill = 0xFF0075FF;
            if (!prog) {
              const double low = num("low", lo), high = num("high", hi), opt = num("optimum", (lo + hi) / 2);
              const bool good_high = opt > high, good_low = opt < low;
              const bool in_low = v < low, in_high = v > high;
              if (good_high ? in_low || v < high : good_low ? in_high || v > low : in_low || in_high)
                fill = ((good_high && v < low) || (good_low && v > high) || (!good_high && !good_low && (in_low || in_high)))
                           ? 0xFFE04040 : 0xFFF0B000;
              else fill = 0xFF10A010;
            }
            Fragment bar = track;
            bar.w = (int)(first.w * frac);
            bar.color = fill;
            if (bar.w > 0) emit(bar);
          }
          if (first.fb[0] || first.fb[1] || first.fb[2] || first.fb[3]) {
            Fragment bd;
            bd.kind = Fragment::Kind::Border;
            bd.x = box_x;
            bd.y = box_y;
            bd.w = first.w;
            bd.h = first.h;
            bd.color = first.style.border_color;
            bd.radius = first.style.radius;
            bd.text.push_back((char)(first.fb[0] > 255 ? 255 : first.fb[0]));
            bd.text.push_back((char)(first.fb[1] > 255 ? 255 : first.fb[1]));
            bd.text.push_back((char)(first.fb[2] > 255 ? 255 : first.fb[2]));
            bd.text.push_back((char)(first.fb[3] > 255 ? 255 : first.fb[3]));
            emit(bd);
          }
          if (!first.text.empty()) {
            Fragment tf;
            tf.kind = Fragment::Kind::Text;
            tf.text = first.text;
            tf.x = box_x + first.fb[3] + first.fp[3];
            tf.y = box_y + first.fb[0] + first.fp[0];
            tf.w = first.w - first.fb[1] - first.fb[3] - first.fp[1] - first.fp[3];
            tf.h = first.h - first.fb[0] - first.fb[2] - first.fp[0] - first.fp[2];
            // One line of label in a box taller than the line -- a button
            // with `height: 36px` -- sits in the MIDDLE of it, not hung from
            // the top edge. A textarea is the one control whose text starts at
            // the top.
            if (!(first.node && first.node->tag == "textarea")) {
              const int lh = line_h(first.style);
              if (tf.h > lh) { tf.y += (tf.h - lh) / 2; tf.h = lh; }
            }
            tf.color = first.style.color;
            tf.font = font_of(first.style);
            tf.link = -1;
            emit(tf);
          }
          if (!first.href.empty()) {
            Link ln;
            ln.href = first.href;
            ln.text = first.text;
            ln.x = box_x;
            ln.y = box_y;
            ln.w = first.w;
            ln.h = first.h;
            doc.links.push_back(ln);
          }
          pen_x += first.w;
          ++run_start;
          continue;
        }
        size_t run_end = run_start + 1;
        auto same = [&](const InlineItem& a, const InlineItem& b) {
          // An atomic item can never join a text run. Image was excluded here
          // and Box was not, so an inline-block sitting between two words was
          // merged into their run: it contributed its empty text, the pen
          // advanced by the words alone, and the box stayed at the provisional
          // origin it was laid out at -- the top-left corner of the page.
          return a.kind != InlineItem::Kind::Image && b.kind != InlineItem::Kind::Image &&
                 a.kind != InlineItem::Kind::Box && b.kind != InlineItem::Kind::Box &&
                 a.kind != InlineItem::Kind::Field && b.kind != InlineItem::Kind::Field &&
                 a.style.color == b.style.color && a.style.font_px == b.style.font_px &&
                 a.style.bold == b.style.bold && a.style.italic == b.style.italic &&
                 a.style.mono == b.style.mono && a.style.serif == b.style.serif &&
                 a.style.underline == b.style.underline &&
                 a.style.strike == b.style.strike && a.href == b.href;
        };
        while (run_end < last && same(items[run_start], items[run_end])) ++run_end;

        std::string text;
        int run_w = 0;
        for (size_t k = run_start; k < run_end; ++k) {
          text += items[k].kind == InlineItem::Kind::Space ? std::string(" ") : items[k].text;
          run_w += items[k].w;
        }
        // The words of a run were measured one at a time, each rounded UP, and
        // added: seventeen words came out thirteen pixels wider than the same
        // string is when it is drawn, which the painter draws in one piece. The
        // next fragment -- a link, usually -- then started that far to the
        // right, and every link in a paragraph sat in a gap. Measured whole, the
        // run is as wide as it is painted. Only ever narrower than the sum, so
        // nothing that fitted stops fitting.
        if (run_end - run_start > 1) {
          const int whole = measure(text, first.style);
          if (whole < run_w) run_w = whole;
        }
        // A run that is only spaces paints nothing but still moves the pen.
        bool blank = true;
        for (char c : text)
          if (!is_space(c)) { blank = false; break; }

        if (!blank) {
          Fragment f;
          f.node = first.node;
          f.kind = Fragment::Kind::Text;
          f.text = text;
          f.x = pen_x;
          f.y = cursor_y + (max_ascent - first.ascent);
          f.w = run_w;
          f.h = first.h;
          f.color = first.style.color;
          f.font = font_of(first.style);
          f.underline = first.style.underline;
          f.strike = first.style.strike;
          if (!first.href.empty()) {
            Link ln;
            ln.href = first.href;
            ln.text = text;
            ln.x = pen_x;
            ln.y = cursor_y;
            ln.w = run_w;
            ln.h = max_h;
            doc.links.push_back(ln);
            f.link = (int)doc.links.size() - 1;
          }
          emit(f);
        }
        pen_x += run_w;
        run_start = run_end;
      }

      cursor_y += max_h;
      (void)forced_break;
    }
    return cursor_y - y;
  }

  // ----------------------------------------------------------------- block --

  // A floated box: sized to its content unless it says otherwise, laid out once
  // where it would have started, then moved to the edge it floats to -- below
  // earlier floats if the line is full -- and recorded so the text that follows
  // is shortened beside it.
  void place_float(Node& el, Style cs, const Style& container, int x, int cursor_y, int width, size_t depth) {
    const bool right = cs.float_side == FloatSide::Right;
    const int ml = cs.margin[3], mr = cs.margin[1], mt = cs.margin[0], mb = cs.margin[2];
    const int frame = cs.border_box ? 0 : cs.padding[1] + cs.padding[3] + cs.border[1] + cs.border[3];
    const int room = width - ml - mr;
    const int pct_base = inline_cb_w_ >= 0 ? inline_cb_w_ : width;
    int box_w;
    if (!cs.width.is_auto()) {
      box_w = len_px(cs.width, pct_base, room) + frame;
    } else {
      box_w = max_content(el, container, 0) - ml - mr;
      if (box_w < 1) box_w = 1;
    }
    if (cs.display == Display::Table) {
      const int mc = min_content(el, container, 0) - ml - mr;
      if (mc > box_w) box_w = mc;
    }
    if (!cs.max_width.is_auto()) { const int mx = len_px(cs.max_width, pct_base, pct_base) + frame; if (box_w > mx) box_w = mx; }
    if (!cs.min_width.is_auto()) { const int mn = len_px(cs.min_width, pct_base, 0) + frame; if (box_w < mn) box_w = mn; }
    if (box_w > room) box_w = room;
    if (box_w < 8) box_w = 8;
    Style s2 = cs;
    s2.width.unit = Length::Unit::Px;
    s2.width.value = (float)(cs.border_box ? box_w : box_w - frame);
    s2.width.offset_px = 0;
    s2.margin_auto[1] = s2.margin_auto[3] = false;

    int fy = cursor_y;
    if (cs.clear_side != ClearSide::None) fy = clear_to(cs.clear_side, fy);
    for (const FloatBox& f : floats_) if (f.y > fy) fy = f.y;     // never above an earlier float
    const int fy0 = fy;

    const size_t frag_mark = doc.fragments.size();
    const size_t link_mark = doc.links.size();
    int h;
    int outer_w = box_w + ml + mr;
    {
      FloatScope inside(&floats_, true);
      h = layout_box(el, s2, x, fy + mt, outer_w, depth + 1);
    }
    // A table can come out wider than it was offered -- its columns would not
    // go narrower -- and the float is as wide as the table turned out.
    if (cs.display == Display::Table) {
      const int expected = box_w - (cs.padding[1] + cs.padding[3] + cs.border[1] + cs.border[3]);
      if (table_w_ > expected) outer_w += table_w_ - expected;
    }
    const int outer_h = mt + h + mb;

    int fx = x;
    for (int guard = 0; guard < 64; ++guard) {
      int lo, hi;
      float_room(fy, outer_h, x, x + width, lo, hi);
      fx = right ? hi - outer_w : lo;
      if (outer_w <= hi - lo) break;
      const int below = next_float_end(fy);
      if (below < 0) break;
      fy = below;
    }
    shift_fragments(frag_mark, doc.fragments.size(), fx - x, fy - fy0);
    shift_links(link_mark, doc.links.size(), fx - x, fy - fy0);
    floats_.push_back(FloatBox{fx, fy, outer_w, outer_h, right});
  }

  // Lay `el`'s children out as a block container whose content box starts at
  // (x, y) and is `width` wide. Returns the content height.
  int layout_children(Node& el, const Style& style, int x, int y, int width, size_t depth) {
    if (depth > kMaxBoxDepth) return 0;
    FlowAlignScope flow_align(&flow_align_, style.text_align);
    WidthScope inline_cb(&inline_cb_w_, width);
    int cursor = y;
    int last_margin = 0;      // the previous sibling's bottom margin, for collapsing
    bool first_child = true;

    // Consecutive inline-level children form one inline formatting context.
    std::vector<InlineItem> pending;
    pending.reserve(128);
    // This box's OWN `::before` leads its content, and its `::after` trails it.
    // The inline path handles a span's generated content where the span is
    // collected; a block never passes through there, so a `<p>::after` -- a
    // required-field star, a closing quote -- was simply never made.
    push_pseudo(el, style, pending, false);
    auto flush_inline = [&]() {
      if (pending.empty()) return;
      // The previous box's bottom margin is still owed. Without this a bare
      // text node after an `<h1>` sits tight against it, because the heading's
      // margin-bottom was only ever collapsed against a following BOX.
      cursor += last_margin;
      last_margin = 0;
      cursor += flow_inline(pending, x, cursor, width, style.text_align);
      pending.clear();
    };

    for (auto& child_ptr : el.children) {
      Node& child = *child_ptr;
      if (child.kind == Node::Kind::Text) {
        bool blank = true;
        for (char c : child.text)
          if (!is_space(c)) { blank = false; break; }
        // Whitespace between two BLOCKS is not content — but between two
        // INLINES it is one space, and it is the space that separates the
        // words. Skipping it outright rendered `<a>Gmail</a> <a>Images</a>`
        // as "GmailImages", which is what every real page looked like: each
        // run of inline elements welded into one word.
        //
        // There is inline content on the line already exactly when the
        // previous sibling was inline, because a block flushes pending — so
        // that is the whole test, and collect_inline collapses the run to one
        // Space by itself.
        if (blank && style.white_space != WhiteSpace::Pre && pending.empty()) continue;
        collect_inline(child, style, pending, 0);
        continue;
      }
      if (child.kind != Node::Kind::Element) continue;
      if (is_skipped(child)) continue;

      Style cs = compute_style(child, style, ua, author, viewport_w);
      if (cs.display == Display::None) continue;
      // Out of flow: parked, and takes no room here.
      if (park_if_out_of_flow(child, cs)) continue;

      // A float leaves the flow: it is laid out where it would have started,
      // moved against its edge, and takes no height here. Whatever it was
      // (a span, an image) it is a block once it floats.
      if (cs.float_side != FloatSide::None) {
        if (!is_block_level(cs.display)) cs.display = Display::Block;
        flush_inline();
        place_float(child, cs, style, x, cursor, width, depth);
        continue;
      }

      if (is_atomic_inline(cs) || (!is_block_level(cs.display) && !has_block_descendant(child))) {
        collect_inline(child, style, pending, 0);
        continue;
      }
      // An inline box with blocks inside it becomes a block container. Its own
      // inline styling still applies to its text, because `cs` is what its
      // children inherit from.
      if (!is_block_level(cs.display)) cs.display = Display::Block;

      flush_inline();

      if (cs.clear_side != ClearSide::None) {
        const int to = clear_to(cs.clear_side, cursor);
        if (to > cursor) { cursor = to; last_margin = 0; }
      }

      // Collapse this box's top margin against the previous box's bottom.
      int top = cs.margin[0];
      int gap = first_child ? top : (top > last_margin ? top : last_margin);
      cursor += gap;
      first_child = false;

      // A box that makes its own context (`overflow`, a table) does not run
      // under a float beside it: it takes the room that is left.
      int bx = x, bw = width;
      if (!floats_.empty() && (cs.overflow_x != Overflow::Visible || cs.display == Display::Table || cs.flow_root)) {
        int lo, hi;
        float_room(cursor, 1, x, x + width, lo, hi);
        if (hi - lo < width && hi - lo > 40) { bx = lo; bw = hi - lo; }
      }
      int h = layout_box(child, cs, bx, cursor, bw, depth + 1);
      cursor += h;
      last_margin = cs.margin[2];
    }
    push_pseudo(el, style, pending, true);
    flush_inline();
    // The last child's bottom margin belongs to this box's content height, or
    // a paragraph at the end of a page has nothing under it.
    cursor += last_margin;
    return cursor - y;
  }

  // What a subtree WANTS on one line: max-content, near enough. A flex item
  // that neither names a width nor asks to grow is sized by its content, and
  // without this every such child took an equal share of the row -- which is
  // what put assay's brand text in the middle of a 232px sidebar instead of
  // beside its mark, and what stretched every icon button into a slab.

  // The narrowest this can be without its content spilling: the widest single
  // unbreakable run inside it. CSS calls it min-content, and it is what
  // `min-width: auto` resolves to FOR A FLEX ITEM -- the rule that stops a row
  // squeezing an item to nothing. Without it the shrink pass below had a floor
  // of zero, and a usage readout in a crowded composer was given 22 pixels: one
  // glyph per line, seven lines tall, every one of them cut with an ellipsis.
  int min_content(Node& node, const Style& parent, size_t depth) {
    if (depth > 24) return 0;
    if (node.kind == Node::Kind::Element) {
      auto it = min_content_cache_.find(&node);
      if (it != min_content_cache_.end()) return it->second;
    }
    int res = min_content_inner(node, parent, depth);
    if (node.kind == Node::Kind::Element) min_content_cache_[&node] = res;
    return res;
  }
  int min_content_inner(Node& node, const Style& parent, size_t depth) {
    if (depth > 24) return 0;
    if (node.kind == Node::Kind::Text) {
      int widest = 0;
      std::string word;
      auto take = [&]() {
        if (word.empty()) return;
        const int w = measure(word, parent);
        if (w > widest) widest = w;
        word.clear();
      };
      for (char ch : cased(node.text, parent)) {
        if (is_space(ch)) take();
        else word += ch;
      }
      take();
      return widest;
    }
    if (node.kind != Node::Kind::Element) return 0;
    if (is_skipped(node)) return 0;
    Style st = compute_style(node, parent, ua, author, viewport_w);
    if (st.display == Display::None) return 0;
    const int edges = st.margin[1] + st.margin[3];
    const int inner = st.padding[1] + st.padding[3] + st.border[1] + st.border[3];
    const int frame_declared = st.border_box ? edges : (inner + edges);
    const int frame = inner + edges;
    if (!st.width.is_auto() && st.width.unit != Length::Unit::Percent)
      return len_px(st.width, viewport_w, viewport_w) + frame_declared;
    // Text that cannot wrap is one unbreakable run, so its min-content is its
    // whole width -- which is the entire point of saying `nowrap`.
    if (st.white_space == WhiteSpace::Nowrap || st.white_space == WhiteSpace::Pre)
      return max_content(node, parent, depth);
    if (node.tag == "img" || node.tag == "svg" || node.tag == "input" || node.tag == "textarea")
      return max_content(node, parent, depth);
    const bool side_by_side =
        (st.display == Display::Flex && st.flex_dir == FlexDir::Row) || st.display == Display::Grid;
    int sum = 0, widest = 0, kids = 0;
    for (auto& kid : node.children) {
      if (!kid) continue;
      if (kid->kind == Node::Kind::Element) {
        const Style ks = compute_style(*kid, st, ua, author, viewport_w);
        // Out of flow contributes nothing, the same as in max_content: an
        // absolutely positioned menu must not widen the button it hangs off.
        if (ks.display == Display::None || out_of_flow(ks)) continue;
        ++kids;
      }
      const int w = min_content(*kid, st, depth + 1);
      sum += w;
      if (w > widest) widest = w;
    }
    // `gap` is real width in a row that cannot wrap, and min-content is exactly
    // the measurement a no-wrap row is held to. Leaving it out measured a strip
    // of icon buttons short by one gap per seam.
    if (side_by_side && kids > 1) sum += st.col_gap * (kids - 1);
    return (side_by_side ? sum : widest) + frame;
  }

  // NOTE ON DEPTH: this counts ITS OWN recursion, and callers must start it at
  // zero. It used to be handed the LAYOUT depth, so by the time the walk
  // reached anything deeply nested the budget was already spent and the box
  // measured as ZERO -- on a real application, which nests a dozen levels
  // before it gets to a label, that is most of the page. A row of chips came
  // out at 22 pixels wide and its text was cut to one glyph per line.
  int max_content(Node& node, const Style& parent, size_t depth) {
    if (depth > 24) return 0;
    if (node.kind == Node::Kind::Element) {
      auto it = max_content_cache_.find(&node);
      if (it != max_content_cache_.end()) return it->second;
    }
    int res = max_content_inner(node, parent, depth);
    if (node.kind == Node::Kind::Element) max_content_cache_[&node] = res;
    return res;
  }
  int max_content_inner(Node& node, const Style& parent, size_t depth) {
    if (depth > 24) return 0;
    if (node.kind == Node::Kind::Text) {
      std::string t;
      bool space = false;
      // Through `text-transform` first: the words that get MEASURED have to be
      // the words that get DRAWN. Measuring "Conversations" and rendering
      // "CONVERSATIONS" sized the box for the narrower of the two, and the
      // label came out cut with an ellipsis inside a row with room to spare.
      // Measured the way the line breaker measures: word by word, with a space
      // between, summed. Measuring the collapsed string in one call gives a
      // slightly different answer once each measurement is rounded, and the
      // intrinsic width then comes out a pixel or two under what the same text
      // actually occupies -- which is a box its own content does not fit in.
      const std::string source = cased(node.text, parent);
      const int space_w = measure(" ", parent);
      int total = 0;
      bool first = true;
      std::string word;
      auto flush = [&]() {
        if (word.empty()) return;
        if (!first) total += space_w;
        total += measure(word, parent);
        first = false;
        word.clear();
      };
      for (char ch : source) {
        if (is_space(ch)) { flush(); continue; }
        word += ch;
      }
      flush();
      (void)t;
      (void)space;
      return total;
    }
    if (node.kind != Node::Kind::Element) return 0;
    if (is_skipped(node)) return 0;
    Style st = compute_style(node, parent, ua, author, viewport_w);
    if (st.display == Display::None) return 0;
    // Under `box-sizing: border-box` a declared width already contains the
    // padding and the border, so only the margins go around it. Adding them
    // unconditionally measured every sized box on a modern page too wide by its
    // own padding -- a 22px icon button came out 38, and the row it was in
    // shrank its neighbour to make space that was never needed.
    // Two different frames, and conflating them is a bug I just made.
    //
    //   A DECLARED width under `box-sizing: border-box` already contains the
    //   padding and the border, so only the margins go outside it.
    //   An AUTO width has to be the content PLUS its padding and border,
    //   whatever box-sizing says -- there is no declared number for them to be
    //   inside of.
    //
    // Using the border-box frame for both made every auto-width box measure to
    // its bare text: a chat bubble came out the width of the word in it with its
    // padding hanging off the end, and the text drawn outside its own border.
    const int edges = st.margin[1] + st.margin[3];
    const int inner = st.padding[1] + st.padding[3] + st.border[1] + st.border[3];
    const int frame_declared = st.border_box ? edges : (inner + edges);
    const int frame = inner + edges;
    // A PERCENTAGE width contributes nothing to an intrinsic size. The spec
    // says a percentage-based size behaves as auto when the containing block's
    // width is what is being worked out -- and resolving it here against the
    // VIEWPORT is worse than either: a child with `width: 100%` contributed a
    // whole screen, so every message card measured wider than the column and
    // took its `max-width: 78%` cap instead of shrinking to its text.
    if (!st.width.is_auto() && st.width.unit != Length::Unit::Percent)
      return len_px(st.width, viewport_w, viewport_w) + frame_declared;
    // A field's own text. NOT a button's: `inner_text()` concatenates every
    // descendant, hidden ones included, so measuring it made a pill as wide as
    // the labels its own stylesheet had already turned off. A button's width
    // comes from walking its children like anything else.
    if (node.tag == "input" || node.tag == "textarea") {
      // A BUTTON is as wide as the words on it. A FIELD is as wide as `size`
      // says, in characters -- never as wide as what is typed in it, which is
      // the whole reason a text box does not grow as you type. Measuring a
      // field by its `value` gave google.com's empty search box the 90px floor
      // where the page asked for `size="57"`, so it came out one sixth of the
      // width of the form it is the whole point of.
      std::string type = node.attr("type");
      for (auto& c : type) c = (char)tolower((unsigned char)c);
      const bool is_button = type == "submit" || type == "button" || type == "reset";
      if (node.tag == "input" && is_button) {
        int w = measure(node.attr("value"), st);
        if (w < 24) w = 24;
        return w + frame;
      }
      // `size` for an <input>, `cols` for a <textarea>; 20 either way when the
      // page does not say, which is the HTML default.
      int chars = 0;
      std::string sz = node.attr(node.tag == "textarea" ? "cols" : "size");
      while (!sz.empty() && is_space(sz.front())) sz.erase(sz.begin());
      while (!sz.empty() && is_space(sz.back())) sz.pop_back();
      for (char c : sz) {
        if (c < '0' || c > '9') { chars = 0; break; }
        chars = chars * 10 + (c - '0');
        if (chars > 1000) break;
      }
      if (chars <= 0) chars = 20;
      // The average advance, measured the way a browser does: the width of a
      // '0' in the field's own face, so a monospace field and a proportional
      // one both come out the size their page expects.
      const int ch = measure("0", st);
      int w = chars * (ch > 0 ? ch : 8);
      const int label = measure(node.attr("placeholder"), st);
      if (label > w) w = label;
      return w + frame;
    }
    // A replaced element is as wide as its picture. Walking its (empty) child
    // list measured an <svg> at nothing but its own padding, so anything sized
    // by its content -- a grid item being centred, a flex item with no width --
    // collapsed to a sliver around a logo that then spilled out of it.
    if (node.tag == "svg") {
      int iw = 0, ih = 0;
      std::string key;
      if (svg_image(node, st, iw, ih, key)) return iw + frame;
    }
    if (node.tag == "img") {
      const std::string wa = node.attr("width");
      int iw = 0;
      for (char c : wa) { if (c < '0' || c > '9') break; iw = iw * 10 + (c - '0'); if (iw > 100000) break; }
      if (iw > 0) return iw + frame;
    }
    int inline_sum = 0, block_max = 0;
    // The generated boxes count too. `::before` and `::after` put real text on
    // the line -- the separators between the segments of a usage readout are
    // four of them -- and leaving them out of the intrinsic width measured the
    // pill sixty pixels narrower than it draws, so the row shrank it and it
    // wrapped onto two lines.
    {
      Style ps;
      if (pseudo_content(node, st, ua, author, viewport_w, false, &ps) && !ps.content.empty())
        inline_sum += measure(ps.content, ps);
      if (pseudo_content(node, st, ua, author, viewport_w, true, &ps) && !ps.content.empty())
        inline_sum += measure(ps.content, ps);
    }
    // A flex ROW's children sit side by side, so its own width is their SUM,
    // not the widest of them. Measured as a block it came out as wide as one
    // tile, and a row of three stat tiles nested in a header was handed a
    // third of what it needed -- every label cut to two letters.
    const bool side_by_side = (st.display == Display::Flex && st.flex_dir == FlexDir::Row) || st.display == Display::Grid;
    int kids = 0;
    for (auto& kid : node.children) {
      if (kid->kind == Node::Kind::Text) {
        inline_sum += max_content(*kid, st, depth + 1);
        continue;
      }
      if (kid->kind != Node::Kind::Element) continue;
      if (is_skipped(*kid)) continue;
      // A <br> ENDS THE LINE, so max-content starts a new one: the widest line
      // is the answer, not the sum of every line run together. google.com's
      // search cell is a 512px field, a <br>, and two buttons -- measured as
      // one line it wanted 784px, so the column it is in took a third more of
      // the page than it needs and pushed the form off centre.
      if (kid->tag == "br" && !side_by_side) {
        if (inline_sum > block_max) block_max = inline_sum;
        inline_sum = 0;
        continue;
      }
      Style ks = compute_style(*kid, st, ua, author, viewport_w);
      if (ks.display == Display::None) continue;
      // Out of flow, out of the measurement. An absolutely positioned child is
      // sized against this box, so letting it widen this box is circular -- and
      // in practice it is how a dropdown made its own trigger as wide as the
      // menu it opens, which then put the menu somewhere else entirely.
      if (out_of_flow(ks)) continue;
      int w = max_content(*kid, st, depth + 1);
      if (!ks.min_width.is_auto() && ks.min_width.unit != Length::Unit::Percent) {
        const int mw = len_px(ks.min_width, viewport_w, 0);
        if (mw > w) w = mw;
      }
      ++kids;
      if (is_block_level(ks.display) && !side_by_side) {
        if (w > block_max) block_max = w;
      } else {
        inline_sum += w;
      }
    }
    if (side_by_side && kids > 1) inline_sum += st.col_gap * (kids - 1);
    int out = (inline_sum > block_max ? inline_sum : block_max) + frame;
    // Percentage min/max do not clamp an intrinsic size either, for the same
    // reason: there is no containing block width yet to be a percentage of.
    if (!st.min_width.is_auto() && st.min_width.unit != Length::Unit::Percent) {
      const int mw = len_px(st.min_width, viewport_w, 0) + frame_declared;
      if (mw > out) out = mw;
    }
    if (!st.max_width.is_auto() && st.max_width.unit != Length::Unit::Percent) {
      const int mx = len_px(st.max_width, viewport_w, out) + frame_declared;
      if (mx < out) out = mx;
    }
    return out;
  }

  // A flex ROW: children side by side rather than stacked.
  //
  // Without this `display: flex` fell back to a block, so a page's sidebar and
  // its main column stacked vertically — assay's whole chat interface was laid
  // out BELOW a 660px sidebar and off the visible area, which reads as an
  // empty pane. A row is the one flex axis a page layout actually depends on;
  // `column` is what a block already does.
  //
  // Sizing is the useful part of the algorithm and none of the rest: a child
  // with a width keeps it, `flex: none` pins a child to its own width, and
  // whatever is left is shared between the children that grow.
  // The child's own `align-self` when it set one, otherwise the container's
  // `align-items`.
  static AlignItems cross_align(const Style& container, const Style& child) {
    return child.align_self == AlignItems::Auto ? container.align_items : child.align_self;
  }

  // Where the main axis starts, and how much goes between items, for a
  // container with `spare` room left over. `space-between` puts it all in the
  // gaps, `center` splits it either side, and so on.
  static void main_axis_spacing(Justify j, int spare, int count, int& start, int& between) {
    start = 0;
    between = 0;
    if (spare <= 0 || count <= 0) return;
    switch (j) {
      case Justify::Start: break;
      case Justify::Center: start = spare / 2; break;
      case Justify::End: start = spare; break;
      case Justify::SpaceBetween:
        if (count > 1) between = spare / (count - 1);
        else start = 0;
        break;
      case Justify::SpaceAround:
        between = spare / count;
        start = between / 2;
        break;
      case Justify::SpaceEvenly:
        between = spare / (count + 1);
        start = between;
        break;
    }
  }

  // The first line of text in a fragment range: where its baseline sits, as an
  // offset from the range's own top. -1 when there is no text to align on.
  int first_baseline(size_t from, size_t to, int top) const {
    if (to > doc.fragments.size()) to = doc.fragments.size();
    for (size_t i = from; i < to; ++i) {
      const Fragment& fr = doc.fragments[i];
      if (fr.kind != Fragment::Kind::Text || fr.h <= 0) continue;
      const int lh = m.line_height(fr.font);
      const int asc = m.ascent ? m.ascent(fr.font) : lh * 4 / 5;
      return fr.y + (fr.h - lh) / 2 + asc - top;
    }
    return -1;
  }

  // Distribute a line's free space onto its flexible items, the way CSS Flexbox
  // 9.7 says: grow or shrink by the factors (shrink scaled by base size), clamp
  // each against its min/max, freeze the violators and repeat. `base`, `lo`,
  // `hi` and the result are border-box sizes; `hi` < 0 means no maximum. Items
  // are at most a few hundred on a real page, and every pass freezes at least
  // one, so the loop is bounded by the item count.
  static void flex_resolve(std::vector<float>& size, const std::vector<float>& base,
                           const std::vector<float>& lo, const std::vector<float>& hi,
                           const std::vector<float>& grow, const std::vector<float>& shrink,
                           float room) {
    const size_t n = size.size();
    std::vector<char> frozen(n, 0);
    float used0 = 0;
    for (size_t i = 0; i < n; ++i) used0 += base[i];
    const bool growing = used0 < room;
    for (size_t i = 0; i < n; ++i) {
      size[i] = base[i];
      const float factor = growing ? grow[i] : shrink[i];
      const float hyp = std::max(lo[i], hi[i] >= 0 ? std::min(base[i], hi[i]) : base[i]);
      // Inflexible: no factor, or the hypothetical size already disagrees with
      // the direction it would move in.
      if (factor <= 0 || (growing ? base[i] > hyp : base[i] < hyp)) { frozen[i] = 1; size[i] = hyp; }
    }
    for (size_t pass = 0; pass <= n; ++pass) {
      float taken = 0, factors = 0;
      bool any = false;
      for (size_t i = 0; i < n; ++i) {
        taken += frozen[i] ? size[i] : base[i];
        if (!frozen[i]) {
          any = true;
          factors += growing ? grow[i] : shrink[i] * base[i];
        }
      }
      if (!any) break;
      const float free_space = room - taken;
      std::vector<float> want(n, 0);
      for (size_t i = 0; i < n; ++i) {
        if (frozen[i]) continue;
        float share = 0;
        if (factors > 0) share = free_space * (growing ? grow[i] : shrink[i] * base[i]) / factors;
        want[i] = base[i] + share;
      }
      float violation = 0;
      std::vector<char> clamp_min(n, 0), clamp_max(n, 0);
      for (size_t i = 0; i < n; ++i) {
        if (frozen[i]) continue;
        float c = want[i];
        if (hi[i] >= 0 && c > hi[i]) c = hi[i];
        if (c < lo[i]) c = lo[i];
        if (c < 0) c = 0;
        violation += c - want[i];
        if (c > want[i]) clamp_min[i] = 1;
        else if (c < want[i]) clamp_max[i] = 1;
        want[i] = c;
      }
      for (size_t i = 0; i < n; ++i) {
        if (frozen[i]) continue;
        const bool freeze = violation == 0 ? true : violation > 0 ? clamp_min[i] : clamp_max[i];
        size[i] = want[i];
        if (freeze) frozen[i] = 1;
      }
      if (violation == 0) break;
    }
  }

  int layout_flex_row(Node& el, const Style& cs, int x, int y, int avail_w, size_t depth) {
    if (depth > kMaxBoxDepth) return 0;
    struct Item {
      Node* node;
      Style st;
      int ml = 0, mr = 0;                 // margins, auto ones counted as 0
      bool auto_l = false, auto_r = false, auto_t = false, auto_b = false;
      float base = 0, lo = 0, hi = -1;    // border-box flex base size and min / max
      float size = 0;                     // what it ends up with, border-box
      int w = 0;                          // ...rounded, and the width layout_box is given less margins
      int order = 0;
      // Text sitting directly inside the flex container. The spec makes each
      // contiguous run of it an ANONYMOUS flex item; this engine skipped every
      // non-element child, so that text was dropped from the sizing entirely --
      // a container of it measured as though it were empty, and everything
      // beside it was handed the room the text should have had.
      bool anonymous = false;
    };
    std::vector<Item> items;
    // Element children this row is responsible for, counted BEFORE display is
    // consulted. A flex row whose every child is display:none has no items,
    // and the empty-items fallback below lays the children out in normal flow
    // -- which un-hides them. That is how assay's collapsed sidebar kept its
    // brand text: `.brand` is a flex row, `.brand-text` its only child, and
    // the rail rule hides exactly that child.
    int laid_out_kids = 0;
    const int gap = cs.col_gap > 0 ? cs.col_gap : 0;
    const int row_gap = cs.row_gap > 0 ? cs.row_gap : 0;
    // Items kept per row are bounded: a hostile page can have millions of
    // children and every pass below is linear in them.
    constexpr size_t kMaxFlexItems = 20000;

    for (auto& child_ptr : el.children) {
      if (items.size() >= kMaxFlexItems) break;
      Node& child = *child_ptr;
      if (child.kind != Node::Kind::Element) {
        // An anonymous flex item: text between the elements. Whitespace on its
        // own is not one -- it is the gap between two items, not an item.
        if (child.kind != Node::Kind::Text) continue;
        bool only_space = true;
        for (char ch : child.text) if (!is_space(ch)) { only_space = false; break; }
        if (only_space) continue;
        Item it;
        it.node = &child;
        it.st = cs;
        it.st.flex_grow = 0; it.st.flex_shrink = 1; it.st.order = 0;
        it.st.flex_basis = Length(); it.st.min_width = Length(); it.st.max_width = Length();
        it.anonymous = true;
        it.base = (float)std::max(1, max_content(child, cs, 0));
        it.lo = (float)std::min((int)it.base, min_content(child, cs, 0));
        items.push_back(std::move(it));
        ++laid_out_kids;
        continue;
      }
      if (is_skipped(child)) continue;
      ++laid_out_kids;
      // viewport_w, NOT avail_w: the last argument is what `@media` and `vw`
      // are measured against. Handing it this row's available width meant a
      // flex child inside assay's 66px rail had every `min-width: 601px` rule
      // dropped -- including the one hiding its own label.
      Style child_style = compute_style(child, cs, ua, author, viewport_w);
      if (child_style.display == Display::None) continue;
      if (park_if_out_of_flow(child, child_style)) { --laid_out_kids; continue; }
      Item it;
      it.node = &child;
      it.st = child_style;
      it.order = child_style.order;
      it.ml = child_style.margin_auto[3] ? 0 : child_style.margin[3];
      it.mr = child_style.margin_auto[1] ? 0 : child_style.margin[1];
      it.auto_l = child_style.margin_auto[3]; it.auto_r = child_style.margin_auto[1];
      it.auto_t = child_style.margin_auto[0]; it.auto_b = child_style.margin_auto[2];
      const int pad = child_style.padding[1] + child_style.padding[3] + child_style.border[1] + child_style.border[3];
      // A DECLARED size under border-box already contains the padding and
      // border; under content-box it does not. Everything below is a
      // BORDER-box size, with the margins kept apart.
      const int declared_frame = child_style.border_box ? 0 : pad;
      auto bb = [&](const Length& l, int fallback) { return len_px(l, avail_w, fallback) + declared_frame; };
      const bool pct_unknown = false;
      (void)pct_unknown;
      if (!child_style.flex_basis.is_auto()) {
        it.base = (float)std::max(bb(child_style.flex_basis, avail_w), pad);
      } else if (!child_style.width.is_auto()) {
        it.base = (float)std::max(bb(child_style.width, avail_w), pad);
      } else {
        // max_content counts the margins and the padding in; take the margins out.
        it.base = (float)std::max(1, max_content(child, cs, 0) - child_style.margin[1] - child_style.margin[3]);
      }
      if (!child_style.max_width.is_auto()) it.hi = (float)std::max(bb(child_style.max_width, avail_w), pad);
      if (!child_style.min_width.is_auto()) {
        it.lo = (float)std::max(bb(child_style.min_width, 0), pad);
      } else if (child_style.overflow_x == Overflow::Visible) {
        // `min-width: auto`: the content's min-content, but never more than a
        // declared size would be.
        float mc = (float)std::max(0, min_content(child, cs, 0) - child_style.margin[1] - child_style.margin[3]);
        if (!child_style.width.is_auto()) mc = std::min(mc, (float)bb(child_style.width, avail_w));
        if (child_style.aspect_ratio > 0 && child_style.width.is_auto()) mc = 0;
        it.lo = std::max(mc, (float)pad);
      } else {
        it.lo = (float)pad;
      }
      if (it.hi >= 0 && it.lo > it.hi) it.hi = it.lo;
      items.push_back(std::move(it));
    }
    if (items.empty()) {
      // Nothing to place: every element child was hidden. Falling through to
      // normal flow here would draw what CSS just told us not to draw. Only a
      // row with no element children at all (text, an entity) goes to flow.
      if (laid_out_kids > 0) return 0;
      return layout_children(el, cs, x, y, avail_w, depth);
    }

    // `order`, then the reversed direction: a stable sort keeps document order
    // between equal ones. Ordering is visual only, which is all this paints.
    {
      bool any_order = false;
      for (const Item& it : items) if (it.order != 0) any_order = true;
      if (any_order)
        std::stable_sort(items.begin(), items.end(),
                         [](const Item& a, const Item& b) { return a.order < b.order; });
      if (cs.flex_reverse) std::reverse(items.begin(), items.end());
    }

    // Lines. With `flex-wrap: wrap`, items whose hypothetical size does not
    // fit go on the next line; otherwise there is one line.
    std::vector<std::vector<Item*>> lines;
    auto hyp_of = [](const Item& it) {
      float h = it.base;
      if (it.hi >= 0 && h > it.hi) h = it.hi;
      if (h < it.lo) h = it.lo;
      return h + (float)(it.ml + it.mr);
    };
    if (cs.flex_wrap) {
      std::vector<Item*> line;
      float used = 0;
      for (auto& it : items) {
        const float need = hyp_of(it);
        if (!line.empty() && used + gap + need > (float)avail_w + 0.01f) { lines.push_back(line); line.clear(); used = 0; }
        used += (line.empty() ? 0 : gap) + need;
        line.push_back(&it);
      }
      if (!line.empty()) lines.push_back(line);
    } else {
      std::vector<Item*> line;
      for (auto& it : items) line.push_back(&it);
      lines.push_back(line);
    }
    if (cs.wrap_reverse) std::reverse(lines.begin(), lines.end());

    struct Placed { size_t first, last; int h; int top; int base; };
    struct LineOut { std::vector<Placed> placed; std::vector<Item*> its; int tallest = 0; int off = 0; };
    std::vector<LineOut> outs(lines.size());
    const int row_h = avail_h_;
    const bool single = lines.size() == 1 && !cs.flex_wrap;
    int cursor_y = y;
    for (size_t li = 0; li < lines.size(); ++li) {
      auto& line = lines[li];
      const size_t n = line.size();
      const int room = std::max(0, avail_w - gap * (int)(n - 1));
      std::vector<float> size(n), base(n), lo(n), hi(n), grow(n), shrink(n);
      float margins = 0;
      for (size_t k = 0; k < n; ++k) {
        Item* it = line[k];
        base[k] = it->base; lo[k] = it->lo; hi[k] = it->hi;
        grow[k] = it->st.flex_grow; shrink[k] = it->st.flex_shrink;
        margins += (float)(it->ml + it->mr);
      }
      flex_resolve(size, base, lo, hi, grow, shrink, (float)room - margins);
      // Round without losing a pixel: each item takes the rounded running total.
      float run = 0; int prev = 0;
      int taken = gap * (int)(n - 1);
      for (size_t k = 0; k < n; ++k) {
        run += size[k];
        const int cum = (int)std::lround(run);
        line[k]->size = size[k];
        line[k]->w = std::max(0, cum - prev);
        prev = cum;
        taken += line[k]->w + line[k]->ml + line[k]->mr;
      }
      // Auto margins on the main axis take the free space before
      // `justify-content` sees any.
      int auto_count = 0;
      for (Item* it : line) auto_count += (it->auto_l ? 1 : 0) + (it->auto_r ? 1 : 0);
      const int spare = avail_w - taken;
      int lead = 0, between = 0;
      std::vector<int> autom_l(n, 0), autom_r(n, 0);
      if (auto_count > 0 && spare > 0) {
        int each = spare / auto_count, extra = spare - each * auto_count;
        for (size_t k = 0; k < n; ++k) {
          if (line[k]->auto_l) { autom_l[k] = each + (extra > 0 ? 1 : 0); if (extra > 0) --extra; }
          if (line[k]->auto_r) { autom_r[k] = each + (extra > 0 ? 1 : 0); if (extra > 0) --extra; }
        }
      } else {
        Justify j = cs.justify_content;
        // Reversed, the main-start edge is the right one: start and end swap.
        if (cs.flex_reverse) {
          if (j == Justify::Start) j = Justify::End; else if (j == Justify::End) j = Justify::Start;
        }
        main_axis_spacing(j, spare, (int)n, lead, between);
      }

      int pen = x + lead;
      auto& out = outs[li];
      out.placed.reserve(n);
      for (size_t k = 0; k < n; ++k) {
        Item* it = line[k];
        const size_t before = doc.fragments.size();
        int h;
        WidthScope cb(&cb_w_, avail_w);
        pen += autom_l[k];
        // A single definite-height row hands each stretched item the line.
        if (single && row_h > 0 && cross_align(cs, it->st) == AlignItems::Stretch &&
            it->st.height.is_auto() && !it->auto_t && !it->auto_b)
          imposed_h_ = row_h;
        if (it->anonymous) {
          std::vector<InlineItem> run;
          WidthScope icb(&inline_cb_w_, it->w);
          collect_inline(*it->node, it->st, run, depth + 1);
          h = flow_inline(run, pen, cursor_y, it->w, it->st.text_align);
        } else {
          // The size flexing settled on, as a border-box width: layout_box
          // honours a declared width, so state it. Margins are the page's own.
          Style forced = it->st;
          forced.width.unit = Length::Unit::Px;
          forced.width.value = (float)it->w;
          forced.width.offset_px = 0;
          forced.border_box = true;
          forced.min_width = Length();
          forced.max_width = Length();
          forced.margin_auto[1] = forced.margin_auto[3] = false;
          forced.margin[1] = it->mr;
          forced.margin[3] = it->ml;
          h = layout_box(*it->node, forced, pen, cursor_y, it->w + it->ml + it->mr, depth + 1);
        }
        imposed_h_ = -1;
        if (h > out.tallest) out.tallest = h;
        out.placed.push_back({before, doc.fragments.size(), h, cursor_y,
                              cross_align(cs, it->st) == AlignItems::Baseline
                                  ? first_baseline(before, doc.fragments.size(), cursor_y) : -1});
        out.its.push_back(it);
        pen += it->w + it->ml + it->mr + gap + between + autom_r[k];
      }
      // Baseline-aligned items grow the line to fit the deepest hang below it.
      {
        int above = 0, below = 0;
        bool any = false;
        for (const Placed& p : out.placed) {
          if (p.base < 0) continue;
          any = true;
          above = std::max(above, p.base);
          below = std::max(below, p.h - p.base);
        }
        if (any) out.tallest = std::max(out.tallest, above + below);
      }
    }

    // A single line in a box of definite height fills it (stretch, centre and
    // end all mean something then); several lines share the box by
    // `align-content`.
    int total = 0;
    for (const LineOut& o : outs) total += o.tallest;
    total += row_gap * (int)(outs.size() - 1);
    int extra = 0;
    if (single) {
      if (row_h > 0 && row_h > outs[0].tallest) outs[0].tallest = row_h;
    } else if (row_h > 0 && row_h > total) {
      extra = row_h - total;
    }
    int lead_y = 0, between_y = 0;
    if (extra > 0) {
      const int cnt = (int)outs.size();
      switch (cs.align_content) {
        case 1: break;
        case 2: lead_y = extra / 2; break;
        case 3: lead_y = extra; break;
        case 4: if (cnt > 1) between_y = extra / (cnt - 1); break;
        case 5: between_y = extra / cnt; lead_y = between_y / 2; break;
        case 6: between_y = extra / (cnt + 1); lead_y = between_y; break;
        default: {   // normal / stretch: every line takes an equal share
          int each = extra / cnt, rest = extra - each * cnt;
          for (LineOut& o : outs) { o.tallest += each + (rest > 0 ? 1 : 0); if (rest > 0) --rest; }
        }
      }
    }
    // Final cross positions, then each item's own alignment within its line.
    int line_top = y + lead_y;
    for (size_t li = 0; li < outs.size(); ++li) {
      LineOut& o = outs[li];
      int above = 0;
      for (const Placed& p : o.placed) if (p.base >= 0) above = std::max(above, p.base);
      for (size_t k = 0; k < o.placed.size(); ++k) {
        Item* it = o.its[k];
        const Placed& p = o.placed[k];
        int dy = line_top - y;                       // line shift (items were laid out at `y`... see below)
        dy = line_top - p.top;
        const AlignItems a = cross_align(cs, it->st);
        const int slack = o.tallest - p.h;
        if (it->auto_t || it->auto_b) {
          if (slack > 0) dy += (it->auto_t && it->auto_b) ? slack / 2 : (it->auto_t ? slack : 0);
        } else if (a == AlignItems::Center) { if (slack > 0) dy += slack / 2; }
        else if (a == AlignItems::End) { if (slack > 0) dy += slack; }
        else if (a == AlignItems::Baseline && p.base >= 0) dy += above - p.base;
        else if (a == AlignItems::Stretch && it->st.height.is_auto() && slack > 0 && !it->anonymous) {
          // Stretching a box's own background, border and shadow to the line
          // costs no re-layout: by construction they are the first fragments it emitted.
          for (size_t f = p.first; f < p.last && f < doc.fragments.size(); ++f) {
            Fragment& fr = doc.fragments[f];
            if (fr.kind != Fragment::Kind::Rect && fr.kind != Fragment::Kind::Border) break;
            if (fr.h == p.h || fr.h + 1 == p.h) fr.h = o.tallest;
          }
        }
        shift_fragments(p.first, p.last, 0, dy);
      }
      line_top += o.tallest + row_gap + between_y;
    }
    int used = 0;
    for (const LineOut& o : outs) used += o.tallest;
    used += row_gap * (int)(outs.size() - 1) + lead_y + between_y * (int)(outs.size() - 1);
    return extra > 0 && cs.align_content != 0 ? row_h : used;
  }

  // A flex COLUMN: children stacked on the main axis, aligned on the cross one.
  //
  // Falling back to a block for this was defensible only as long as nothing
  // depended on the difference. It does: `gap` between children, `align-items`
  // across them, `justify-content` down a container with a known height, and
  // `flex: 1` on the child that should absorb the leftover height. An
  // application shell is a column of header / body / footer where the body
  // takes the slack -- as a block, the body was its content's height and the
  // footer floated up under it.
  int layout_flex_column(Node& el, const Style& cs, int x, int y, int avail_w, size_t depth) {
    if (depth > kMaxBoxDepth) return 0;
    struct Item {
      Node* node;
      Style st;
      int w = 0;          // cross-axis size
      int h = 0;          // main-axis size, once known
      bool grows = false;
      size_t first = 0;   // its fragment range, for the cross-axis shift
      size_t last = 0;
    };
    std::vector<Item> items;
    int laid_out_kids = 0;
    const int gap = cs.row_gap > 0 ? cs.row_gap : 0;

    for (auto& child_ptr : el.children) {
      Node& child = *child_ptr;
      if (child.kind != Node::Kind::Element) continue;
      if (is_skipped(child)) continue;
      ++laid_out_kids;
      Style child_style = compute_style(child, cs, ua, author, viewport_w);
      if (child_style.display == Display::None) continue;
      if (park_if_out_of_flow(child, child_style)) { --laid_out_kids; continue; }
      Item it;
      it.node = &child;
      it.st = child_style;
      it.grows = child_style.flex_grow > 0.0f && child_style.height.is_auto();
      const int edges = child_style.margin[1] + child_style.margin[3];
      const int frame = child_style.border_box
                            ? edges
                            : (child_style.padding[1] + child_style.padding[3] +
                               child_style.border[1] + child_style.border[3] + edges);
      // Cross axis: `stretch` (the default) is the whole width; anything else
      // is the child's own width, or its content.
      const AlignItems a = cross_align(cs, child_style);
      if (!child_style.width.is_auto()) {
        it.w = len_px(child_style.width, avail_w, avail_w) + frame;
      } else if (a == AlignItems::Stretch) {
        it.w = avail_w;
      } else {
        it.w = max_content(child, cs, 0);
        if (it.w > avail_w) it.w = avail_w;
      }
      if (!child_style.min_width.is_auto()) {
        const int mw = len_px(child_style.min_width, avail_w, 0) + frame;
        if (mw > it.w) it.w = mw;
      }
      if (!child_style.max_width.is_auto()) {
        const int mx = len_px(child_style.max_width, avail_w, avail_w) + frame;
        if (it.w > mx) it.w = mx;
      }
      if (it.w > avail_w) it.w = avail_w;
      if (it.w < 1) it.w = 1;
      items.push_back(std::move(it));
    }
    if (items.empty()) {
      if (laid_out_kids > 0) return 0;
      return layout_children(el, cs, x, y, avail_w, depth);
    }

    // Lay each child out where it falls, then move the whole column once the
    // leftover height is known. The main-axis size of a grower is settled
    // here only when the container's own height is definite -- with an auto
    // height there is no slack to hand out, which is also true in a browser.
    // The height this column has to divide: the one it declared, or the one it
    // was given by the box around it. A `flex: 1` pane inside a shell has no
    // height of its own -- it has the one its parent handed down, and without
    // looking there the column had no slack to distribute and every pane sat at
    // its natural height.
    int definite_h = len_px(cs.height, avail_h_ >= 0 ? avail_h_ : 0, -1);
    if (definite_h <= 0 && avail_h_ >= 0) definite_h = avail_h_;

    int cursor = y;
    for (size_t k = 0; k < items.size(); ++k) {
      Item& it = items[k];
      it.first = doc.fragments.size();
      {
        WidthScope cb(&cb_w_, avail_w);
        it.h = layout_box(*it.node, it.st, x, cursor, it.w, depth + 1);
      }
      it.last = doc.fragments.size();
      cursor += it.h;
      if (k + 1 < items.size()) cursor += gap;
    }
    int used = cursor - y;

    float grow_total = 0.0f;
    for (const Item& it : items) if (it.grows) grow_total += it.st.flex_grow;

    // `flex: 1` is `flex-grow:1; flex-shrink:1; flex-basis:0`. A grower's own
    // content height is NOT its size: it takes a share of whatever the column
    // has left after the fixed items, which is as true when the content is too
    // tall as when it is too short. Only growing meant a pane full of messages
    // pushed the composer below it off the bottom of a shell that was supposed
    // to be exactly one viewport tall.
    if (definite_h > 0 && grow_total > 0.0f) {
      int fixed_used = 0;
      for (const Item& it : items) if (!it.grows) fixed_used += it.h;
      fixed_used += gap * (int)(items.size() > 0 ? items.size() - 1 : 0);
      int share_pool = definite_h - fixed_used;
      if (share_pool < 0) share_pool = 0;
      {
        // Re-lay the column with each grower given its share: a grower's height
        // changes what its own children do, so it cannot be fixed up by
        // shifting fragments.
        doc.fragments.resize(items.front().first);
        cursor = y;
        for (size_t k = 0; k < items.size(); ++k) {
          Item& it = items[k];
          it.first = doc.fragments.size();
          if (it.grows) {
            int mine = (int)((float)share_pool * it.st.flex_grow / grow_total);
            if (mine < 0) mine = 0;
            Style stretched = it.st;
            stretched.height.unit = Length::Unit::Px;
            stretched.height.value = (float)mine;
            stretched.height.offset_px = 0;
            // The share is the box's OUTER height; layout_box will take its own
            // padding off if the page asked for border-box.
            stretched.border_box = true;
            it.h = layout_box(*it.node, stretched, x, cursor, it.w, depth + 1);
          } else {
            it.h = layout_box(*it.node, it.st, x, cursor, it.w, depth + 1);
          }
          it.last = doc.fragments.size();
          cursor += it.h;
          if (k + 1 < items.size()) cursor += gap;
        }
        used = cursor - y;
      }
    } else if (definite_h > 0 && used < definite_h) {
      {
        // Nothing grows, so `justify-content` decides where the slack goes.
        int lead = 0, between = 0;
        main_axis_spacing(cs.justify_content, definite_h - used, (int)items.size(), lead, between);
        if (lead || between) {
          for (size_t k = 0; k < items.size(); ++k)
            shift_fragments(items[k].first, items[k].last, 0, lead + between * (int)k);
          used = definite_h;
        }
      }
    }

    // Cross axis, for the children that did not stretch.
    for (const Item& it : items) {
      const AlignItems a = cross_align(cs, it.st);
      if (a != AlignItems::Center && a != AlignItems::End) continue;
      const int slack = avail_w - it.w;
      if (slack <= 0) continue;
      shift_fragments(it.first, it.last, a == AlignItems::Center ? slack / 2 : slack, 0);
    }
    return used;
  }

  // ---------------------------------------------------------------- grid --
  //
  // `display: grid` with `grid-template-columns`: the tracks as written (px,
  // fr, auto, repeat(n, …)), children placed row by row, each row as tall as
  // its tallest cell, `gap` between columns and rows. No areas, no spans, no
  // named lines -- a dashboard of tiles and a two-column form are what a mind
  // writes, and those are exactly this.
  // A single dimension's length: px, %, or a font-relative unit resolved
  // against this layout's own root/em convention (15px, matching
  // apply_declaration's `root_px` in web_css.cpp) -- `atoi` on "15.5rem"
  // silently truncates to the integer 15 *pixels*, throwing away both the
  // fraction and the x15 the unit demands. That single truncation is what
  // turned Wikipedia's 15.5rem sidebar column into a literal 15px one and
  // squeezed the entire article body into the 15px left over.
  static int grid_len_px(const std::string& raw, int against, int em) {
    size_t b = 0, e = raw.size();
    while (b < e && is_space(raw[b])) ++b;
    while (e > b && is_space(raw[e - 1])) --e;
    std::string s = raw.substr(b, e - b);
    if (s.empty()) return 0;
    char* end = nullptr;
    double n = std::strtod(s.c_str(), &end);
    std::string unit;
    if (end) {
      size_t ub = (size_t)(end - s.c_str()), ue = s.size();
      while (ub < ue && is_space(s[ub])) ++ub;
      while (ue > ub && is_space(s[ue - 1])) --ue;
      unit = s.substr(ub, ue - ub);
    }
    for (auto& c : unit) c = (char)std::tolower((unsigned char)c);
    if (unit == "%") return (int)(n * against / 100.0);
    if (unit == "rem") return (int)std::lround(n * 15.0);
    if (unit == "em") return (int)std::lround(n * em);
    if (unit == "pt") return (int)std::lround(n * 96.0 / 72.0);
    if (unit.empty() || unit == "px") return (int)std::lround(n);
    return (int)std::lround(n);
  }

  // Track sizes for a template. `content`, when given, holds the width the
  // ITEMS in each track want (0 where unknown), which is what `auto`,
  // `min-content` and `max-content` tracks are sized by; `kinds` reports which
  // tracks those are so a caller knows there is something to measure.
  //
  // The order is the grid algorithm's, reduced to what pages use:
  //   1. fixed lengths are what they say; content tracks start at their content;
  //      `minmax(a, b)` starts at `a`, `fr` tracks at their floor;
  //   2. free space grows the `minmax(a, <length>)` tracks up to their cap --
  //      which is why Wikipedia's article column stops at 59.25rem -- and, when
  //      there is no `fr` track to take it, stretches the `auto` ones;
  //   3. whatever is left is shared among the `fr` tracks.
  // A `min-content` or `max-content` track never grows past its content.
  std::vector<int> grid_tracks(const std::string& tmpl, int avail_w, int gap, int em = 15,
                               const std::vector<int>* content = nullptr,
                               std::vector<char>* kinds = nullptr) {
    // kind: 0 fixed or fr, 1 min-content, 2 max-content, 3 auto, 4 capped minmax
    struct Track { float fr = 0; int px = 0; int min_px = 0; int max_px = -1; char kind = 0; };
    std::vector<Track> tracks;
    std::string t = tmpl;
    // repeat(n, X) -> X X X …
    for (;;) {
      size_t r = t.find("repeat(");
      if (r == std::string::npos) break;
      size_t close = t.find(')', r);
      if (close == std::string::npos) break;
      std::string inner = t.substr(r + 7, close - r - 7);
      size_t comma = inner.find(',');
      int n = comma == std::string::npos ? 1 : std::atoi(inner.substr(0, comma).c_str());
      std::string unit = comma == std::string::npos ? inner : inner.substr(comma + 1);
      if (n < 1) n = 1;
      if (n > 24) n = 24;
      std::string expanded;
      for (int i = 0; i < n; ++i) { if (i) expanded += ' '; expanded += unit; }
      t = t.substr(0, r) + expanded + t.substr(close + 1);
    }
    std::string cur;
    auto flush = [&]() {
      if (cur.empty()) return;
      Track tr;
      if (cur.compare(0, 7, "minmax(") == 0 && cur.back() == ')') {
        std::string inner = cur.substr(7, cur.size() - 8);
        size_t comma = inner.find(',');
        std::string mn = comma == std::string::npos ? inner : inner.substr(0, comma);
        std::string mx = comma == std::string::npos ? std::string() : inner.substr(comma + 1);
        // `minmax(0, 59.25rem)`: this track flexes like a bare `fr`, but never
        // past its own max -- that cap is the whole reason Wikipedia's article
        // column stays a readable ~950px instead of stretching the full
        // window width. `minmax(200px, 1fr)`: the min is a real floor under
        // the flex growth, not a fixed size in its own right.
        if (mx.find("fr") != std::string::npos) {
          tr.fr = (float)std::atof(mx.c_str());
          if (tr.fr <= 0) tr.fr = 1.0f;
          tr.min_px = grid_len_px(mn, avail_w, em);
        } else if (!mx.empty() && mx != "auto" && mx != "min-content" && mx != "max-content") {
          // A length for the cap: the track is its floor until there is room,
          // and never more than the cap.
          tr.kind = 4;
          tr.min_px = grid_len_px(mn, avail_w, em);
          tr.max_px = grid_len_px(mx, avail_w, em);
        } else {
          tr.kind = mx == "min-content" ? 1 : mx == "max-content" ? 2 : 3;
          tr.min_px = grid_len_px(mn, avail_w, em);
        }
      } else if (cur.find("fr") != std::string::npos) {
        tr.fr = (float)std::atof(cur.c_str());
      } else if (cur == "min-content") {
        tr.kind = 1;
      } else if (cur == "max-content") {
        tr.kind = 2;
      } else if (cur == "auto") {
        tr.kind = 3;
      } else {
        tr.px = grid_len_px(cur, avail_w, em);
      }
      if (tr.fr <= 0 && tr.px <= 0 && tr.kind == 0) tr.fr = 1.0f;
      tracks.push_back(tr);
      cur.clear();
    };
    int paren = 0;
    for (char c : t) {
      if (c == '(') paren++;
      if (c == ')') paren--;
      if ((c == ' ' || c == '\t') && paren == 0) flush();
      else cur += c;
    }
    flush();
    if (tracks.empty()) tracks.push_back(Track{1.0f, 0});
    if (tracks.size() > 24) tracks.resize(24);
    if (kinds) { kinds->clear(); for (auto& tr : tracks) kinds->push_back(tr.kind); }
    const int n = (int)tracks.size();
    const int gaps = gap * (n > 0 ? n - 1 : 0);
    std::vector<int> w(n, 0);
    float frs = 0;
    int base_sum = 0;
    for (int i = 0; i < n; ++i) {
      Track& tr = tracks[i];
      if (tr.px > 0) w[i] = tr.px;
      else if (tr.kind == 1 || tr.kind == 2 || tr.kind == 3) {
        w[i] = content && i < (int)content->size() ? (*content)[i] : 0;
        if (w[i] < tr.min_px) w[i] = tr.min_px;
      } else if (tr.kind == 4) w[i] = tr.min_px;
      else { w[i] = tr.min_px; frs += tr.fr; }
      base_sum += w[i];
    }
    int free_px = avail_w - gaps - base_sum;
    if (free_px < 0) free_px = 0;
    // Capped tracks take the free space, equally, up to their caps.
    for (int pass = 0; pass < 8 && free_px > 0; ++pass) {
      int growers = 0;
      for (int i = 0; i < n; ++i) if (tracks[i].kind == 4 && w[i] < tracks[i].max_px) ++growers;
      if (!growers) break;
      const int share = free_px / growers > 0 ? free_px / growers : 1;
      for (int i = 0; i < n && free_px > 0; ++i) {
        if (tracks[i].kind != 4 || w[i] >= tracks[i].max_px) continue;
        int give = std::min(share, tracks[i].max_px - w[i]);
        if (give > free_px) give = free_px;
        w[i] += give;
        free_px -= give;
      }
    }
    if (frs > 0) {
      // The flexible tracks share what is left. Their floors are already in
      // `w`; the share is of the whole remaining room, as it always was.
      int others = 0;
      for (int i = 0; i < n; ++i) if (!(tracks[i].px <= 0 && tracks[i].kind == 0 && tracks[i].fr > 0)) others += w[i];
      int room = avail_w - gaps - others;
      if (room < 0) room = 0;
      for (int i = 0; i < n; ++i) {
        if (!(tracks[i].px <= 0 && tracks[i].kind == 0 && tracks[i].fr > 0)) continue;
        int share = (int)((float)room * tracks[i].fr / frs);
        w[i] = std::max(tracks[i].min_px, share);
      }
    } else if (free_px > 0) {
      // No flexible track: `auto` tracks stretch to use the room.
      int autos = 0;
      for (int i = 0; i < n; ++i) if (tracks[i].kind == 3) ++autos;
      for (int i = 0; i < n && autos; ++i) if (tracks[i].kind == 3) w[i] += free_px / autos;
    }
    std::vector<int> out;
    for (int i = 0; i < n; ++i) {
      int v = w[i];
      if (tracks[i].max_px >= 0 && tracks[i].kind == 4 && v > tracks[i].max_px) v = tracks[i].max_px;
      if (v < 8) v = 8;
      out.push_back(v);
    }
    return out;
  }

  struct GridAreaRect {
    int r0 = 0, r1 = 0; // 0-based row range [r0, r1] inclusive
    int c0 = 0, c1 = 0; // 0-based col range [c0, c1] inclusive
  };

  static void parse_line_span(const std::string& s, int& start_idx, int& span_cnt) {
    start_idx = -1;
    span_cnt = 1;
    if (s.empty()) return;
    std::string str = s;
    size_t slash = str.find('/');
    if (slash != std::string::npos) {
      std::string p1 = str.substr(0, slash);
      std::string p2 = str.substr(slash + 1);
      while (!p1.empty() && is_space(p1.front())) p1.erase(p1.begin());
      while (!p1.empty() && is_space(p1.back())) p1.pop_back();
      while (!p2.empty() && is_space(p2.front())) p2.erase(p2.begin());
      while (!p2.empty() && is_space(p2.back())) p2.pop_back();
      int v1 = std::atoi(p1.c_str());
      if (v1 > 0) start_idx = v1 - 1;
      if (p2.find("span") != std::string::npos) {
        span_cnt = std::max(1, std::atoi(p2.substr(p2.find("span") + 4).c_str()));
      } else {
        int v2 = std::atoi(p2.c_str());
        if (v2 > 0 && start_idx >= 0) {
          span_cnt = std::max(1, v2 - (start_idx + 1));
        }
      }
    } else {
      if (str.find("span") != std::string::npos) {
        span_cnt = std::max(1, std::atoi(str.substr(str.find("span") + 4).c_str()));
      } else {
        int v = std::atoi(str.c_str());
        if (v > 0) start_idx = v - 1;
      }
    }
  }

  // A grid item in the track it was given. A track hands over its whole width;
  // `justify-items`/`justify-self` other than stretch make the item as wide as
  // its content and set it at the start, the middle or the end of the track --
  // which is how a grid centres a logo. The same auto margins a block uses do
  // the placing, so the item's own width, max-width and padding still count.
  int layout_grid_item(Node& n, const Style& st, const Style& grid, int x, int y, int track_w, size_t depth) {
    const AlignItems j = st.justify_self != AlignItems::Auto ? st.justify_self : grid.justify_items;
    if (j != AlignItems::Center && j != AlignItems::End) return layout_box(n, st, x, y, track_w, depth);
    Style s2 = st;
    if (s2.width.is_auto() && n.tag != "img" && n.tag != "svg") {
      const int frame = st.border_box ? 0 : st.padding[1] + st.padding[3] + st.border[1] + st.border[3];
      int want = max_content(n, grid, 0) - frame - st.margin[1] - st.margin[3];
      if (want < 1) want = 1;
      if (want < track_w) {
        s2.width.unit = Length::Unit::Px;
        s2.width.value = (float)want;
      }
    }
    s2.margin_auto[3] = true;
    s2.margin_auto[1] = j == AlignItems::Center;
    return layout_box(n, s2, x, y, track_w, depth);
  }

  int layout_grid(Node& el, const Style& cs, int x, int y, int avail_w, size_t depth) {
    if (depth > kMaxBoxDepth) return 0;
    const int col_gap = cs.col_gap > 0 ? cs.col_gap : 0;
    const int row_gap = cs.row_gap > 0 ? cs.row_gap : 0;

    // Parse area matrix if grid-template-areas is defined
    std::vector<std::vector<std::string>> area_matrix;
    std::unordered_map<std::string, GridAreaRect> area_map;
    if (!cs.grid_template_areas.empty()) {
      const std::string& str = cs.grid_template_areas;
      size_t i = 0;
      while (i < str.size()) {
        while (i < str.size() && is_space(str[i])) ++i;
        if (i >= str.size()) break;
        char quote = 0;
        if (str[i] == '"' || str[i] == '\'') {
          quote = str[i++];
        }
        std::string row_str;
        while (i < str.size()) {
          if (quote && str[i] == quote) { ++i; break; }
          if (!quote && (str[i] == '\n' || str[i] == ';')) { ++i; break; }
          row_str += str[i++];
        }
        std::vector<std::string> row;
        std::string token;
        for (char c : row_str) {
          if (is_space(c)) {
            if (!token.empty()) { row.push_back(token); token.clear(); }
          } else {
            token += c;
          }
        }
        if (!token.empty()) row.push_back(token);
        if (!row.empty()) area_matrix.push_back(row);
      }
      for (int r = 0; r < (int)area_matrix.size(); ++r) {
        for (int c = 0; c < (int)area_matrix[r].size(); ++c) {
          const std::string& name = area_matrix[r][c];
          if (name.empty() || name == ".") continue;
          auto it = area_map.find(name);
          if (it == area_map.end()) {
            area_map[name] = GridAreaRect{r, r, c, c};
          } else {
            it->second.r0 = std::min(it->second.r0, r);
            it->second.r1 = std::max(it->second.r1, r);
            it->second.c0 = std::min(it->second.c0, c);
            it->second.c1 = std::max(it->second.c1, c);
          }
        }
      }
    }

    size_t min_cols = 0;
    for (const auto& r : area_matrix) {
      if (r.size() > min_cols) min_cols = r.size();
    }

    const int grid_em = cs.font_px > 0 ? (int)cs.font_px : 15;
    std::vector<char> track_kinds;
    std::vector<int> tracks = grid_tracks(cs.grid_template, avail_w, col_gap, grid_em, nullptr, &track_kinds);
    if (min_cols > tracks.size()) {
      std::string expanded_tmpl = "repeat(" + std::to_string(min_cols) + ", 1fr)";
      tracks = grid_tracks(expanded_tmpl, avail_w, col_gap, grid_em);
      track_kinds.clear();
    }
    if (tracks.empty()) tracks.push_back(std::max(8, avail_w));

    std::vector<int> row_spec_tracks;
    if (!cs.grid_template_rows.empty()) {
      row_spec_tracks = grid_tracks(cs.grid_template_rows, 0, row_gap, cs.font_px > 0 ? cs.font_px : 15);
    }

    std::vector<std::pair<Node*, Style>> cells;
    for (auto& child_ptr : el.children) {
      Node& child = *child_ptr;
      if (child.kind != Node::Kind::Element) continue;
      if (is_skipped(child)) continue;
      Style st = compute_style(child, cs, ua, author, viewport_w);
      if (st.display == Display::None) continue;
      if (park_if_out_of_flow(child, st)) continue;
      cells.emplace_back(&child, st);
    }
    if (cells.empty()) return layout_children(el, cs, x, y, avail_w, depth);

    // Precalculate column x offsets and helper functions
    std::vector<int> col_x(tracks.size(), 0);
    for (size_t c = 1; c < tracks.size(); ++c) {
      col_x[c] = col_x[c - 1] + tracks[c - 1] + col_gap;
    }
    auto get_col_w = [&](int c0, int c1) -> int {
      int w = 0;
      for (int c = c0; c <= c1 && c < (int)tracks.size(); ++c) {
        w += tracks[c];
        if (c < c1) w += col_gap;
      }
      return w;
    };

    // Tracks sized by their content (`auto`, `min-content`, `max-content`) are
    // only known once the items are placed, so the widths are worked out again
    // with what each column's items want. Columns are counted the same either
    // way, so nothing placed so far moves.
    bool has_content_tracks = false;
    for (char k : track_kinds) if (k == 1 || k == 2 || k == 3) has_content_tracks = true;
    auto retrack = [&](const std::vector<std::pair<Node*, int>>& by_col) {
      if (!has_content_tracks) return;
      std::vector<int> content(tracks.size(), 0);
      for (const auto& pr : by_col) {
        const int c = pr.second;
        if (c < 0 || c >= (int)tracks.size()) continue;
        const char k = c < (int)track_kinds.size() ? track_kinds[c] : 0;
        if (k != 1 && k != 2 && k != 3) continue;
        const int want = k == 1 ? min_content(*pr.first, cs, 0) : max_content(*pr.first, cs, 0);
        if (want > content[c]) content[c] = want;
      }
      std::vector<int> fresh = grid_tracks(cs.grid_template, avail_w, col_gap, grid_em, &content);
      if (fresh.size() != tracks.size()) return;
      tracks = fresh;
      for (size_t c = 1; c < tracks.size(); ++c) col_x[c] = col_x[c - 1] + tracks[c - 1] + col_gap;
    };

    // Fast path: if no custom areas, no explicit rows, and no grid item placement
    bool has_custom_placement = !area_map.empty() || !row_spec_tracks.empty();
    if (!has_custom_placement) {
      for (const auto& c : cells) {
        if (!c.second.grid_area.empty() || !c.second.grid_column.empty() || !c.second.grid_row.empty()) {
          has_custom_placement = true;
          break;
        }
      }
    }

    if (!has_custom_placement) {
      {
        std::vector<std::pair<Node*, int>> by_col;
        for (size_t k = 0; k < cells.size(); ++k) by_col.emplace_back(cells[k].first, (int)(k % tracks.size()));
        retrack(by_col);
      }
      int cursor_y = y;
      size_t i = 0;
      while (i < cells.size()) {
        int pen = x;
        int tallest = 0;
        for (size_t col = 0; col < tracks.size() && i < cells.size(); ++col, ++i) {
          const int h = layout_grid_item(*cells[i].first, cells[i].second, cs, pen, cursor_y, tracks[col], depth + 1);
          if (h > tallest) tallest = h;
          pen += tracks[col] + col_gap;
        }
        cursor_y += tallest;
        if (i < cells.size()) cursor_y += row_gap;
      }
      return cursor_y - y;
    }

    // Grid item assignment
    struct PlacedItem {
      Node* node;
      Style style;
      GridAreaRect rect;
    };
    std::vector<PlacedItem> placed;
    placed.reserve(cells.size());

    int max_rows = std::max(1, (int)area_matrix.size());
    std::vector<std::vector<bool>> occupied(100, std::vector<bool>(tracks.size(), false));

    for (const auto& c : cells) {
      Node& child = *c.first;
      const Style& st = c.second;
      GridAreaRect rect{-1, -1, -1, -1};

      if (!st.grid_area.empty()) {
        auto it = area_map.find(st.grid_area);
        if (it != area_map.end()) {
          rect = it->second;
        }
      }
      if (rect.c0 < 0 && !area_map.empty()) {
        auto it = area_map.find(child.tag);
        if (it == area_map.end() && child.has_attr("id")) it = area_map.find(child.attr("id"));
        if (it == area_map.end() && child.has_attr("class")) it = area_map.find(child.attr("class"));
        if (it != area_map.end()) rect = it->second;
      }
      // `grid-area: 1 / 1` -- row-start / column-start / row-end / column-end by
      // line number -- is the same placement as `grid-row` and `grid-column`.
      // Pages stack several items in ONE cell with it; read as a name that
      // matched nothing, each landed in a row of its own.
      std::string col_spec = st.grid_column, row_spec = st.grid_row;
      if (rect.c0 < 0 && col_spec.empty() && row_spec.empty() && !st.grid_area.empty() &&
          (std::isdigit((unsigned char)st.grid_area[0]) || st.grid_area.compare(0, 4, "span") == 0)) {
        std::vector<std::string> p4;
        std::string cur;
        for (char ch : st.grid_area) {
          if (ch == '/') { p4.push_back(cur); cur.clear(); } else cur += ch;
        }
        p4.push_back(cur);
        row_spec = p4[0] + (p4.size() > 2 ? " / " + p4[2] : "");
        if (p4.size() > 1) col_spec = p4[1] + (p4.size() > 3 ? " / " + p4[3] : "");
      }
      if (rect.c0 < 0 && (!col_spec.empty() || !row_spec.empty())) {
        int c_start = -1, c_span = 1;
        int r_start = -1, r_span = 1;
        parse_line_span(col_spec, c_start, c_span);
        parse_line_span(row_spec, r_start, r_span);
        if (c_start >= 0) {
          rect.c0 = std::min(c_start, (int)tracks.size() - 1);
          rect.c1 = std::min(rect.c0 + c_span - 1, (int)tracks.size() - 1);
        }
        if (r_start >= 0) {
          rect.r0 = r_start;
          rect.r1 = r_start + r_span - 1;
        }
      }

      if (rect.c0 < 0 || rect.r0 < 0) {
        int span = (rect.c0 >= 0 && rect.c1 >= rect.c0) ? (rect.c1 - rect.c0 + 1) : 1;
        int found_r = 0, found_c = 0;
        bool placed_ok = false;
        for (int r = (rect.r0 >= 0 ? rect.r0 : 0); r < 100 && !placed_ok; ++r) {
          for (int col = 0; col + span <= (int)tracks.size(); ++col) {
            bool fits = true;
            for (int k = 0; k < span; ++k) {
              if (occupied[r][col + k]) { fits = false; break; }
            }
            if (fits) {
              found_r = r;
              found_c = col;
              placed_ok = true;
              break;
            }
          }
        }
        if (rect.c0 < 0) {
          rect.c0 = found_c;
          rect.c1 = found_c + span - 1;
        }
        if (rect.r0 < 0) {
          rect.r0 = found_r;
          rect.r1 = found_r;
        }
      }

      rect.c0 = std::max(0, std::min(rect.c0, (int)tracks.size() - 1));
      rect.c1 = std::max(rect.c0, std::min(rect.c1, (int)tracks.size() - 1));
      rect.r0 = std::max(0, std::min(rect.r0, 99));
      rect.r1 = std::max(rect.r0, std::min(rect.r1, 99));
      if (rect.r1 + 1 > max_rows) max_rows = rect.r1 + 1;

      for (int r = rect.r0; r <= rect.r1 && r < 100; ++r) {
        for (int col = rect.c0; col <= rect.c1 && col < (int)tracks.size(); ++col) {
          occupied[r][col] = true;
        }
      }

      placed.push_back(PlacedItem{&child, st, rect});
    }

    {
      std::vector<std::pair<Node*, int>> by_col;
      for (const auto& item : placed)
        if (item.rect.c0 == item.rect.c1) by_col.emplace_back(item.node, item.rect.c0);
      retrack(by_col);
    }

    // Row by row layout
    int cursor_y = y;
    int span_bottom = y;
    for (int r = 0; r < max_rows; ++r) {
      int tallest = (r < (int)row_spec_tracks.size() && row_spec_tracks[r] > 0) ? row_spec_tracks[r] : 0;
      for (auto& item : placed) {
        if (item.rect.r0 != r) continue;
        int item_x = x + col_x[item.rect.c0];
        int item_w = get_col_w(item.rect.c0, item.rect.c1);
        int h = layout_grid_item(*item.node, item.style, cs, item_x, cursor_y, item_w, depth + 1);
        // An item spanning several rows is no taller a reason for THIS row than
        // for the next one; it only has to fit inside the grid when all of them
        // are done. Counting it here made the first row of Wikipedia's article
        // as tall as its whole tools column.
        if (item.rect.r1 > item.rect.r0) { if (cursor_y + h > span_bottom) span_bottom = cursor_y + h; }
        else if (h > tallest) tallest = h;
      }
      cursor_y += tallest;
      if (r + 1 < max_rows) cursor_y += row_gap;
    }
    if (span_bottom > cursor_y) cursor_y = span_bottom;
    return cursor_y - y;
  }

  // Paint one block-level box and lay its children out inside it.
  // Sets the containing height for everything laid out inside this scope.
  // Sets the containing block width for everything laid out inside this scope.
  struct WidthScope {
    int* slot;
    int was;
    WidthScope(int* s2, int v) : slot(s2), was(*s2) { *slot = v; }
    ~WidthScope() { *slot = was; }
  };

  struct HeightScope {
    int* slot;
    int was;
    HeightScope(int* s2, int v) : slot(s2), was(*s2) { *slot = v; }
    ~HeightScope() { *slot = was; }
  };

  // An `<a href>` laid out as a BOX -- display:block, a flex or grid item, an inline-block -- never passes through
  // collect_inline's anchor handling, so its words would carry no href and the link would not exist. Its own href is in
  // scope for everything inside it.
  struct HrefScope {
    std::string* slot;
    std::string was;
    HrefScope(std::string* s2, const std::string* now) : slot(s2), was(*s2) { if (now) *slot = *now; }
    ~HrefScope() { *slot = was; }
  };

  int layout_box(Node& el, const Style& cs, int x, int y, int avail_w, size_t depth) {
    OpacityScope op_scope(&opacity_, cs.effective_opacity);
    const std::string own_href = el.tag == "a" && el.has_attr("href") ? resolve_url(doc.base, el.attr("href")) : std::string();
    HrefScope href_scope(&open_href, own_href.empty() ? nullptr : &own_href);
    // An <svg> reached as a block -- which is what `svg { display: block }` in a
    // page's own sheet makes every icon in it.
    if (el.tag == "svg") {
      int iw = 0, ih = 0;
      std::string key;
      if (svg_image(el, cs, iw, ih, key)) {
        Fragment f;
        f.kind = Fragment::Kind::Image;
        f.x = x;
        f.y = y;
        f.w = iw;
        f.h = ih;
        f.src = key;
        f.alt = el.attr("aria-label");
        f.color = cs.color;
        f.node = &el;
        emit(f);
        return ih;
      }
      return 0;
    }
    // A text field has no children to lay out: its whole visual — the box, the
    // border, the placeholder — comes from the field path in collect_inline.
    // Reached as a BLOCK (a flex item, or `display: block` in the page's own
    // sheet) it laid out its zero children and drew nothing, which is why
    // assay's composer was an empty 633px frame with no input in it.
    if (el.tag == "input" || el.tag == "textarea" || el.tag == "audio" || el.tag == "video" ||
        el.tag == "canvas" || el.tag == "img") {
      struct Owner {
        Node** slot;
        Node* was;
        ~Owner() { *slot = was; }
      } fowner{&owner_, owner_};
      owner_ = &el;
      std::vector<InlineItem> run;
      WidthScope icb(&inline_cb_w_, avail_w);
      collect_inline(el, cs, run, depth);
      if (run.empty()) return 0;
      // The field sizes itself to its text; as a block it takes the width it
      // was given, the way a real control does.
      const int edges = run[0].fp[1] + run[0].fp[3] + run[0].fb[1] + run[0].fb[3];
      int box_w = cs.width.is_auto() ? avail_w : len_px(cs.width, avail_w, avail_w) + edges;
      if (box_w > avail_w) box_w = avail_w;
      if (!cs.width.is_auto()) run[0].w = box_w;                       // the author's width, over the 90px floor
      else if (box_w > edges + 8 && run[0].w < box_w) run[0].w = box_w;
      // A field handed less room than its label takes the room and cuts the
      // label -- the box stays where the layout put it instead of drawing its
      // placeholder across the next field's. Only a label that truly does not
      // fit is cut: a "5" in a 50px box is a 5, not a "5…".
      if (run[0].w > avail_w) run[0].w = avail_w;
      const int label_w = measure(run[0].text, run[0].style) + edges;
      if (label_w > run[0].w && run[0].w > edges + 12) {
        const int ell_w = measure("\u2026", run[0].style);
        std::string cut = run[0].text;
        while (!cut.empty() && measure(cut, run[0].style) + ell_w + edges > run[0].w) {
          size_t at = cut.size() - 1;
          while (at > 0 && ((unsigned char)cut[at] & 0xC0) == 0x80) --at;
          cut.erase(at);
        }
        if (cut.size() < run[0].text.size()) {
          run[0].text = cut.empty() ? std::string() : cut + "\u2026";
          run[0].cut = true;
        }
      }
      return flow_inline(run, x, y, avail_w, cs.text_align);
    }

    // Whose box this is, for the whole of it and restored on the way out, so a
    // nested box does not leave its parent's fragments attributed to a child.
    struct Owner {
      Node** slot;
      Node* was;
      ~Owner() { *slot = was; }
    } owner{&owner_, owner_};
    owner_ = &el;
    int ml = cs.margin[3], mr = cs.margin[1];
    int bl = cs.border[3], br = cs.border[1], bt = cs.border[0], bb = cs.border[2];
    int pl = cs.padding[3], pr = cs.padding[1], pt = cs.padding[0], pb = cs.padding[2];

    // With `box-sizing: border-box` a declared width already contains the
    // padding and the border; with the default it does not, and they go around
    // it. Getting this wrong makes every padded box on a modern page too wide
    // by exactly its own padding.
    const int frame_w = cs.border_box ? 0 : (pl + pr + bl + br);
    // Percentages resolve against the containing block, which is not always the
    // width this box was handed -- see cb_w_.
    const int pct_base = cb_w_ >= 0 ? cb_w_ : avail_w;
    int outer_w = cs.width.is_auto() ? avail_w - ml - mr : len_px(cs.width, pct_base, avail_w) + frame_w;
    if (!cs.max_width.is_auto()) { const int mx = len_px(cs.max_width, pct_base, pct_base) + frame_w; if (outer_w > mx) outer_w = mx; }
    // `display: table` on a non-table element (Wikipedia's Contents box) is
    // shrink-to-fit like a table: as wide as its content, not its container.
    if (cs.display == Display::Table && el.tag != "table" && cs.width.is_auto()) {
      const int fit = max_content(el, cs, 0) - ml - mr;
      if (fit > 0 && fit < outer_w) outer_w = fit;
    }
    if (outer_w > avail_w - ml - mr) outer_w = avail_w - ml - mr;
    if (!cs.min_width.is_auto()) { const int mn = len_px(cs.min_width, pct_base, 0) + frame_w; if (outer_w < mn && mn <= avail_w - ml - mr) outer_w = mn; }
    if (outer_w < 8) outer_w = 8;
    // `margin-left/right: auto` takes the room the width left over: both sides
    // split it, which centres the box; one side takes all of it, which pushes
    // the box to the other edge. With no width and no max-width there is no
    // room left and an auto margin is zero, as it was before.
    if (cs.margin_auto[3] || cs.margin_auto[1]) {
      const bool al = cs.margin_auto[3], ar = cs.margin_auto[1];
      int spare = avail_w - outer_w - (al ? 0 : ml) - (ar ? 0 : mr);
      if (spare < 0) spare = 0;
      if (al && ar) { ml = spare / 2; mr = spare - ml; }
      else if (al) ml = spare;
      else mr = spare;
    }
    int content_w = outer_w - bl - br - pl - pr;
    if (content_w < 8) content_w = 8;

    int box_x = x + ml;
    int content_x = box_x + bl + pl;
    int content_y = y + bt + pt;

    // Everything this box and its descendants emit, so a `position: relative`
    // offset can move the lot once its size is known.
    const size_t frag_start = doc.fragments.size();
    // Out-of-flow descendants parked while this box is being laid out. A
    // positioned box is the containing block for them; a static one is not,
    // and passes them further up.
    const size_t pending_mark = pending_.size();
    const bool is_containing_block = cs.position != Position::Static;
    // `z-index` on a positioned box opens a stacking level for its whole
    // subtree.
    const int saved_z = z_;
    if (is_containing_block && cs.has_z) z_ = cs.z_index;

    // The background and the border have to be painted UNDER the content, but
    // their height is not known until the content is laid out. Reserve the
    // slots now and fill them in after — the fragment list is painted in
    // order, so the reservation is what puts them behind.
    const size_t box_mark = doc.fragments.size();
    const size_t link_mark = doc.links.size();
    size_t bg_slot = SIZE_MAX, border_slot = SIZE_MAX, shadow_slot = SIZE_MAX, bgimg_slot = SIZE_MAX;
    if (cs.has_shadow && (cs.shadow_color >> 24)) {
      // Behind the background, so the box sits on top of its own shadow.
      Fragment f;
      f.kind = Fragment::Kind::Rect;
      f.x = box_x + cs.shadow_x - cs.shadow_spread;
      f.y = y + cs.shadow_y - cs.shadow_spread;
      f.w = outer_w + 2 * cs.shadow_spread;
      f.color = cs.shadow_color;
      // A negative spread shrinks the shadow, and with it the corner. Clamped
      // at zero: a negative radius reached the painter as a nonsense value.
      f.radius = cs.radius > 0 ? cs.radius + cs.shadow_spread : 0;
      if (f.radius < 0) f.radius = 0;
      f.blur = cs.shadow_blur;
      f.shadow = true;
      f.cut_x = box_x;
      f.cut_y = y;
      f.cut_w = outer_w;
      f.cut_h = 0;               // the box's height is not known yet; set with the shadow's own
      shadow_slot = doc.fragments.size();
      emit(f);
    }
    if (cs.has_background && (cs.background >> 24)) {
      Fragment f;
      f.kind = Fragment::Kind::Rect;
      f.x = box_x;
      f.y = y;
      f.w = outer_w;
      f.color = cs.background;
      f.radius = cs.radius;
      // A masked box is a stencil: the colour is the ink and the picture the
      // shape. Painted as a plain rectangle it was a solid square -- which is
      // every icon on a modern page.
      if (!cs.mask_image.empty()) {
        f.kind = Fragment::Kind::Image;
        f.src = resolve_url(doc.base, cs.mask_image);
        f.mask = true;
        f.tint = cs.background;
        f.radius = 0;
      }
      bg_slot = doc.fragments.size();
      emit(f);
    }
    if (bt || br || bb || bl) {
      Fragment f;
      f.kind = Fragment::Kind::Border;
      f.x = box_x;
      f.y = y;
      f.w = outer_w;
      f.color = cs.border_color;
      f.radius = cs.radius;
      // The four widths ride in the fragment's text field as bytes: the
      // painter needs all four and Fragment has one h.
      f.text.push_back((char)(bt > 255 ? 255 : bt));
      f.text.push_back((char)(br > 255 ? 255 : br));
      f.text.push_back((char)(bb > 255 ? 255 : bb));
      f.text.push_back((char)(bl > 255 ? 255 : bl));
      border_slot = doc.fragments.size();
      emit(f);
    }
    // `background-image: url(...)`, over the colour and inside the border. It is
    // its own fragment so the painter can tile and size it; the picture is
    // fetched like any other, by the url it carries.
    if (!cs.bg_image.empty() && cs.mask_image.empty()) {
      Fragment f;
      f.kind = Fragment::Kind::Image;
      f.bg = true;
      f.x = box_x + bl;
      f.y = y + bt;
      f.w = std::max(0, outer_w - bl - br);
      f.src = resolve_url(doc.base, cs.bg_image);
      f.radius = cs.radius;
      f.bg_repeat = cs.bg_repeat;
      f.bg_size = (uint8_t)cs.bg_size;
      f.bg_pos_x = cs.bg_pos_x;
      f.bg_pos_y = cs.bg_pos_y;
      f.bg_off_x = cs.bg_off_x;
      f.bg_off_y = cs.bg_off_y;
      // An explicit size is in px, or a percent of the box that the painter
      // resolves once the box has a height.
      f.bg_w = !cs.bg_size_w.is_auto() && cs.bg_size_w.unit == Length::Unit::Px ? (int)cs.bg_size_w.value : -1;
      f.bg_h = !cs.bg_size_h.is_auto() && cs.bg_size_h.unit == Length::Unit::Px ? (int)cs.bg_size_h.value : -1;
      if (cs.bg_size_w.unit == Length::Unit::Percent) f.bg_w_pct = cs.bg_size_w.value;
      if (cs.bg_size_h.unit == Length::Unit::Percent) f.bg_h_pct = cs.bg_size_h.value;
      bgimg_slot = doc.fragments.size();
      emit(f);
    }

    // A list item's marker sits in the padding the UA sheet put on the list.
    if (cs.display == Display::ListItem && cs.list_bullet) {
      // A drawn dot, not a typeset one. U+2022 is not in the kernel's ASCII
      // atlas, so setting it substituted an asterisk — which sits high, is the
      // wrong weight, and was landing on the first letter of the item because
      // the marker's x was being clamped INTO the content box. A marker lives
      // in the list's padding, which is to the left of it.
      int dot = cs.font_px >= 20 ? 5 : 4;
      Fragment f;
      f.kind = Fragment::Kind::Rect;
      f.color = cs.color;
      f.x = content_x - 14;
      if (f.x < 0) f.x = 0;
      f.y = content_y + (line_h(cs) - dot) / 2;
      f.w = dot;
      f.h = dot;
      emit(f);
    }

    // This box's own definite height, worked out BEFORE its children are laid
    // out -- they need it: `height: 100%` on a child, and `flex: 1` in a
    // column, are both percentages of a height that has to already exist.
    int own_h = -1;
    if (imposed_h_ >= 0) {
      own_h = imposed_h_ - (cs.border_box ? 0 : 0) - (pt + pb + bt + bb);
      if (own_h < 0) own_h = 0;
      imposed_h_ = -1;
    }
    if (!cs.height.is_auto()) {
      int h = len_px(cs.height, avail_h_ >= 0 ? avail_h_ : 0, -1);
      if (cs.height.unit == Length::Unit::Percent && avail_h_ < 0) h = -1;
      if (h >= 0) {
        // The CONTENT height, which is what a child fills. Under the default
        // box-sizing a declared height already IS that.
        own_h = cs.border_box ? h - (pt + pb + bt + bb) : h;
        if (own_h < 0) own_h = 0;
      }
    }
    HeightScope height_scope(&avail_h_, own_h);

    // A box that starts its own formatting context -- a float, `overflow` other
    // than visible, a flex or grid container, a table -- keeps its floats to
    // itself, and its height reaches down to the lowest of them.
    const bool bfc_root = cs.flow_root || cs.overflow_x != Overflow::Visible || cs.overflow_y != Overflow::Visible ||
                          cs.float_side != FloatSide::None || cs.display == Display::Flex ||
                          cs.display == Display::Grid || cs.display == Display::Table;
    FloatScope float_scope(&floats_, bfc_root);

    int inner_h = 0;
    if (cs.display == Display::Table) {
      table_x_ = content_x;
      table_w_ = content_w;
      inner_h = layout_table(el, cs, content_x, content_y, content_w, depth);
      // The table decided where it goes and how wide it is; its own box has to
      // follow, or the border is drawn round the container instead of round
      // the table.
      if (table_w_ != content_w || table_x_ != content_x) {
        const int dx = table_x_ - content_x;
        const int dw = table_w_ - content_w;
        for (size_t slot : {shadow_slot, bg_slot, border_slot, bgimg_slot}) {
          if (slot == SIZE_MAX) continue;
          doc.fragments[slot].x += dx;
          doc.fragments[slot].w += dw;
        }
      }
    } else if (el.tag == "hr") {
      Fragment f;
      f.kind = Fragment::Kind::Rule;
      f.x = content_x;
      f.y = content_y;
      f.w = content_w;
      f.h = bt > 0 ? bt : 1;
      f.color = cs.border_color;
      emit(f);
      inner_h = f.h;
    } else if (cs.display == Display::Flex && cs.flex_dir == FlexDir::Row) {
      inner_h = layout_flex_row(el, cs, content_x, content_y, content_w, depth);
    } else if (cs.display == Display::Flex) {
      inner_h = layout_flex_column(el, cs, content_x, content_y, content_w, depth);
    } else if (cs.display == Display::Grid) {
      inner_h = layout_grid(el, cs, content_x, content_y, content_w, depth);
    } else {
      inner_h = layout_children(el, cs, content_x, content_y, content_w, depth);
    }

    if (bfc_root) inner_h = float_scope.bottom(content_y + inner_h) - content_y;

    // What the box was told to be, as a content height. Worked out above, so
    // that the children could be laid out into it.
    int named_h = own_h;
    if (named_h > inner_h) inner_h = named_h;
    // `overflow` other than visible cuts the content off at the box, so a
    // content-sized height is the DECLARED one and the overflow is hidden
    // rather than making the box taller. This is what a scroll pane is: a
    // fixed frame with more inside it than fits.
    const bool clips_y = cs.overflow_y != Overflow::Visible;
    // What the content ACTUALLY came to, before the frame cuts it down. This is
    // the number a scroll pane needs: the clamp below replaces inner_h with the
    // declared height, and comparing that against itself says nothing ever
    // overflows.
    const int content_h = inner_h;
    if (clips_y && named_h > 0) inner_h = named_h;
    int total = bt + pt + inner_h + pb + bb;

    // A box with a non-visible overflow clips everything its subtree drew to
    // its padding box. Applied here, after the height is known, by narrowing
    // each fragment's clip to the intersection of this box and whatever clip
    // it already carried from an ancestor.
    if (cs.overflow_x != Overflow::Visible || clips_y) {
      const int cx = box_x + bl, cy = y + bt;
      const int cw = outer_w - bl - br, ch = total - bt - bb;
      for (size_t i = frag_start; i < doc.fragments.size(); ++i) {
        Fragment& f = doc.fragments[i];
        int nx = cx, ny = cy, nw = cw, nh = ch;
        if (cs.overflow_x == Overflow::Visible) { nx = f.clipped ? f.clip_x : -1 << 28; nw = f.clipped ? f.clip_w : 1 << 29; }
        if (!clips_y) { ny = f.clipped ? f.clip_y : -1 << 28; nh = f.clipped ? f.clip_h : 1 << 29; }
        if (f.clipped) {
          const int x0 = nx > f.clip_x ? nx : f.clip_x;
          const int y0 = ny > f.clip_y ? ny : f.clip_y;
          const int x1 = (nx + nw) < (f.clip_x + f.clip_w) ? (nx + nw) : (f.clip_x + f.clip_w);
          const int y1 = (ny + nh) < (f.clip_y + f.clip_h) ? (ny + nh) : (f.clip_y + f.clip_h);
          nx = x0; ny = y0; nw = x1 - x0; nh = y1 - y0;
        }
        f.clipped = true;
        f.clip_x = nx;
        f.clip_y = ny;
        f.clip_w = nw > 0 ? nw : 0;
        f.clip_h = nh > 0 ? nh : 0;
      }

      // `auto` and `scroll` mean the content can be REACHED, not just cut off.
      // `hidden` does not: that is a page saying it does not want the rest
      // shown. Recorded only when there is something to scroll to.
      const bool scrolls = (cs.overflow_y == Overflow::Auto || cs.overflow_y == Overflow::Scroll);
      if (scrolls && content_h > ch && ch > 0) {
        ScrollArea sa;
        sa.node = &el;
        sa.x = cx;
        sa.y = cy;
        sa.w = cw;
        sa.h = ch;
        sa.content_h = content_h;
        const int id = (int)doc.scrollers.size();
        doc.scrollers.push_back(sa);
        for (size_t i = frag_start; i < doc.fragments.size(); ++i) {
          // The box's own frame does not move with its contents.
          if (i == bg_slot || i == border_slot || i == shadow_slot || i == bgimg_slot) continue;
          // Innermost wins: an inner scroller finished before this one did.
          if (doc.fragments[i].scroller < 0) doc.fragments[i].scroller = id;
        }
      }
    }

    if (bg_slot != SIZE_MAX) doc.fragments[bg_slot].h = total;
    if (border_slot != SIZE_MAX) doc.fragments[border_slot].h = total;
    if (bgimg_slot != SIZE_MAX) doc.fragments[bgimg_slot].h = std::max(0, total - bt - bb);
    // `transform: translate(...)`: move the whole box, and everything it drew,
    // AFTER it has a size -- a percentage here is against the box's own width
    // and height, which is the whole point of `translate(-50%, -50%)`.
    if (cs.has_translate) {
      const int dx = cs.translate_x.is_auto() ? 0 : len_px(cs.translate_x, outer_w, 0);
      const int dy = cs.translate_y.is_auto() ? 0 : len_px(cs.translate_y, total, 0);
      if (dx || dy) {
        shift_fragments(box_mark, doc.fragments.size(), dx, dy);
        shift_links(link_mark, doc.links.size(), dx, dy);
      }
    }
    if (shadow_slot != SIZE_MAX) {
      doc.fragments[shadow_slot].h = total + 2 * cs.shadow_spread;
      doc.fragments[shadow_slot].cut_h = total;
    }

    // The boxes positioned against this one, now that it has a size. Done
    // AFTER the clip pass on purpose: an absolute child of a scroll pane is
    // clipped by it, and one that escaped would be drawn over the page.
    if (is_containing_block) {
      const int save_clip_on = clip_on_;
      int save_clip[4] = {clip_[0], clip_[1], clip_[2], clip_[3]};
      if (cs.overflow_x != Overflow::Visible || clips_y) {
        clip_on_ = true;
        clip_[0] = box_x + bl;
        clip_[1] = y + bt;
        clip_[2] = outer_w - bl - br;
        clip_[3] = total - bt - bb;
      }
      drain_out_of_flow(pending_mark, box_x + bl, y + bt, outer_w - bl - br, total - bt - bb,
                        depth, /*is_root=*/false);
      clip_on_ = save_clip_on != 0;
      clip_[0] = save_clip[0]; clip_[1] = save_clip[1];
      clip_[2] = save_clip[2]; clip_[3] = save_clip[3];
    }

    // `position: relative` (and an unstuck `sticky`): laid out in flow, then
    // moved. The box still occupies the space it was given, which is the
    // whole difference from `absolute`.
    if (cs.position == Position::Relative || cs.position == Position::Sticky) {
      int dx = 0, dy = 0;
      if (!cs.inset[3].is_auto()) dx = len_px(cs.inset[3], avail_w, 0);
      else if (!cs.inset[1].is_auto()) dx = -len_px(cs.inset[1], avail_w, 0);
      if (!cs.inset[0].is_auto()) dy = len_px(cs.inset[0], total, 0);
      else if (!cs.inset[2].is_auto()) dy = -len_px(cs.inset[2], total, 0);
      shift_fragments(frag_start, doc.fragments.size(), dx, dy);
    }

    z_ = saved_z;
    if (content_w + bl + br + pl + pr > doc.content_width) doc.content_width = outer_w + ml + mr;
    return total;
  }

  // ---------------------------------------------------------------- tables --

  // A real table pass: find the rows wherever they are, count the columns, work
  // out what each column WANTS, and give it a share of the width in proportion.
  //
  // Splitting the width evenly (which is what this did first) is what makes a
  // table-laid-out page look broken: on Hacker News the rank column "1." got
  // exactly as much room as the headline, so every title wrapped to three
  // lines. This is the shape of CSS's automatic table layout without the
  // iteration: a max-content width per column, a min-content floor, and the
  // slack shared out between them.
  int layout_table(Node& table, const Style& style, int x, int y, int width, size_t depth) {
    if (depth > kMaxBoxDepth) return 0;

    std::vector<Node*> rows;
    std::vector<Node*> queue;
    for (auto& c : table.children) queue.push_back(c.get());
    for (size_t qi = 0; qi < queue.size() && rows.size() < 4096; ++qi) {
      Node* n = queue[qi];
      if (n->kind != Node::Kind::Element) continue;
      if (n->tag == "tr") {
        rows.push_back(n);
        continue;
      }
      if (n->tag == "thead" || n->tag == "tbody" || n->tag == "tfoot")
        for (auto& c : n->children) queue.push_back(c.get());
    }
    if (rows.empty()) return layout_children(table, style, x, y, width, depth);

    auto cell_span = [](const Node& c) -> int {
      if (!c.has_attr("colspan")) return 1;
      int v = std::atoi(c.attr("colspan").c_str());
      return v > 1 ? v : 1;
    };

    size_t cols = 1;
    for (Node* r : rows) {
      size_t n = 0;
      for (auto& c : r->children)
        if (c->kind == Node::Kind::Element && (c->tag == "td" || c->tag == "th")) n += cell_span(*c);
      if (n > cols) cols = n;
    }
    if (cols > 24) cols = 24;

    // What each column wants, the least it can live with, and what a
    // `<td width=...>` pinned it to.
    std::vector<int> want((size_t)cols, 0), least((size_t)cols, 0), pinned((size_t)cols, 0);
    std::vector<float> pct((size_t)cols, 0.0f);

    struct SpanningCell {
      size_t col = 0;
      int span = 1;
      int whole = 0;
      int longest = 0;
      int want_px = 0;
    };
    std::vector<SpanningCell> spanning;

    for (Node* r : rows) {
      Style rs = compute_style(*r, style, ua, author, viewport_w);
      size_t col = 0;
      for (auto& c : r->children) {
        if (c->kind != Node::Kind::Element) continue;
        if (c->tag != "td" && c->tag != "th") continue;
        if (col >= cols) break;
        int span = cell_span(*c);
        if (col + span > cols) span = (int)(cols - col);
        Style cellst = compute_style(*c, rs, ua, author, viewport_w);
        // What the cell's CONTENT is worth, through the same intrinsic sizing
        // every other box uses. It used to measure `inner_text()` and look for
        // a direct <img> child, which meant a form control counted for nothing:
        // google.com's middle column holds a 512px search field and no text at
        // all, so it was measured as empty and the two 25% columns either side
        // took the page between them.
        int whole = max_content(*c, rs, 0);
        int longest = min_content(*c, rs, 0);
        int want_px = 0;
        // A declared column width is a floor AND what the column wants: that is
        // what `<td width="25%">` is for, and it is how a layout table says
        // where its columns go.
        if (!cellst.width.is_auto()) {
          if (cellst.width.unit == Length::Unit::Percent) {
            // Kept as a FRACTION. A percentage column is a percentage of the
            // table, and the table's width is what is being worked out.
            if (span == 1) {
              const float f = cellst.width.value / 100.0f;
              if (f > 0.0f && f < 1.0f && f > pct[col]) pct[col] = f;
            }
          } else {
            want_px = len_px(cellst.width, width, 0);
            if (want_px > 0) {
              if (want_px > whole) whole = want_px;
              if (want_px > longest) longest = want_px;
              if (span == 1 && want_px > pinned[col]) pinned[col] = want_px;
            }
          }
        }
        if (span == 1) {
          if (whole > want[col]) want[col] = whole;
          if (longest > least[col]) least[col] = longest;
        } else {
          spanning.push_back({col, span, whole, longest, want_px});
        }
        col += span;
      }
    }

    // Spanning cells distribute their remaining needs across the spanned columns.
    for (const auto& sc : spanning) {
      int sum_w = 0, sum_l = 0;
      for (size_t k = sc.col; k < sc.col + sc.span; ++k) {
        sum_w += want[k];
        sum_l += least[k];
      }
      int target_w = sc.whole > sc.want_px ? sc.whole : sc.want_px;
      if (target_w > sum_w && sc.span > 0) {
        int extra = target_w - sum_w;
        int each = extra / sc.span;
        int rem = extra % sc.span;
        for (size_t k = sc.col; k < sc.col + sc.span; ++k) {
          want[k] += each + (k == sc.col ? rem : 0);
        }
      }
      int target_l = sc.longest > sc.want_px ? sc.longest : sc.want_px;
      if (target_l > sum_l && sc.span > 0) {
        int extra = target_l - sum_l;
        int each = extra / sc.span;
        int rem = extra % sc.span;
        for (size_t k = sc.col; k < sc.col + sc.span; ++k) {
          least[k] += each + (k == sc.col ? rem : 0);
        }
      }
    }
    for (size_t i = 0; i < cols; ++i) {
      if (least[i] < 24) least[i] = 24;
      if (want[i] < least[i]) want[i] = least[i];
      // No single column may claim the whole table and squeeze the rest out --
      // unless the page PINNED it with a width, in which case that is the
      // page's decision and not this heuristic's to overrule.
      int cap = width * 3 / 4;
      if (cap < 40) cap = 40;
      if (pinned[i] > cap) cap = pinned[i];
      if (want[i] > cap) want[i] = cap;
      // The cap limits what a column WANTS, not the least it can be: a column
      // holding a 370px picture is at least 370px wide whatever room the table
      // was offered, and clamping that is how a picture got cut off.
      if (want[i] < least[i]) want[i] = least[i];
    }

    long total_want = 0, total_least = 0;
    for (size_t i = 0; i < cols; ++i) {
      total_want += want[i];
      total_least += least[i];
    }

    // A TABLE IS SHRINK-TO-FIT. It is not a block: with no width of its own it
    // is as wide as its columns need and no wider, which is why a centred
    // layout table sits in the middle of the page instead of spanning it. This
    // used to hand the container's full width to the column algorithm, so
    // google.com's `<td width="25%">` gutters were a quarter of the WINDOW --
    // 460px each side -- and the form was pushed off its own centre.
    int table_w = width;
    if (!style.width.is_auto()) {
      table_w = len_px(style.width, width, width);
      // A declared width is a wish, not a limit: a table is never narrower than
      // its columns can be, so a 22em infobox holding a 370px picture is 370px
      // plus its padding and not a box the picture is cut off by.
      if (table_w < (int)total_least) table_w = (int)total_least;
    } else {
      table_w = (int)(total_want < (long)width ? total_want : (long)width);
      // A PERCENTAGE column is a share of the table, so the table has to be big
      // enough that the columns which are NOT percentages still fit in what is
      // left over. google.com pins a 25% gutter either side of a cell holding a
      // 512px search field: the table cannot be 512 + two small gutters, it has
      // to be 1024, so that half of it is the 512 the middle column needs.
      float sum_pct = 0.0f;
      long auto_want = 0;
      for (size_t i = 0; i < cols; ++i) {
        if (pct[i] > 0.0f) sum_pct += pct[i];
        else auto_want += want[i];
      }
      if (sum_pct > 0.0f) {
        long from_auto = sum_pct < 0.99f ? (long)((float)auto_want / (1.0f - sum_pct)) : (long)width;
        if (from_auto > (long)table_w) table_w = (int)from_auto;
        for (size_t i = 0; i < cols; ++i) {
          if (pct[i] <= 0.0f) continue;
          const long need = (long)((float)want[i] / pct[i]);
          if (need > (long)table_w) table_w = (int)need;
        }
      }
      if (table_w < (int)total_least) table_w = (int)total_least;
      if (table_w > width) table_w = width;
    }
    if (table_w < 1) table_w = 1;
    // Now the table's width is known, a percentage column is a real number.
    bool repin = false;
    for (size_t i = 0; i < cols; ++i) {
      if (pct[i] <= 0.0f) continue;
      const int px = (int)(pct[i] * (float)table_w);
      if (px <= 0 || px >= table_w) continue;
      if (px > want[i]) { want[i] = px; repin = true; }
      if (px > least[i]) { least[i] = px; repin = true; }
      if (px > pinned[i]) pinned[i] = px;
    }
    if (repin) {
      total_want = 0;
      total_least = 0;
      for (size_t i = 0; i < cols; ++i) {
        if (want[i] < least[i]) want[i] = least[i];
        total_want += want[i];
        total_least += least[i];
      }
    }
    // A shrink-to-fit table still has to be PLACED, and a legacy layout table
    // is placed by the alignment in force -- which is what `<center>` around a
    // table has always meant.
    if (table_w < width) {
      if (style.margin_auto[3] && style.margin_auto[1]) x += (width - table_w) / 2;
      else if (style.margin_auto[3]) x += width - table_w;
      else if (flow_align_ == Align::Center) x += (width - table_w) / 2;
      else if (flow_align_ == Align::Right) x += width - table_w;
    }
    width = table_w;
    table_x_ = x;
    table_w_ = table_w;

    std::vector<int> col_w((size_t)cols, 0);
    if (total_want <= (long)width && total_want > 0) {
      // Everything fits: give each column what it asked for and share the
      // slack out to unpinned columns so pinned/fixed columns keep their width.
      long slack = (long)width - total_want;
      long unpinned_want = 0;
      for (size_t i = 0; i < cols; ++i) {
        if (pinned[i] == 0 && pct[i] <= 0.0f) unpinned_want += want[i];
      }
      if (unpinned_want > 0) {
        for (size_t i = 0; i < cols; ++i) {
          if (pinned[i] == 0 && pct[i] <= 0.0f)
            col_w[i] = want[i] + (int)(slack * (long)want[i] / unpinned_want);
          else
            col_w[i] = want[i];
        }
      } else {
        for (size_t i = 0; i < cols; ++i)
          col_w[i] = want[i] + (int)(slack * (long)want[i] / total_want);
      }
    } else if (total_least <= (long)width && total_want > total_least) {
      // Tight: everyone gets their floor plus a share of what is left, in
      // proportion to how much MORE they wanted.
      long room = (long)width - total_least;
      long spread = total_want - total_least;
      for (size_t i = 0; i < cols; ++i)
        col_w[i] = least[i] + (int)(room * (long)(want[i] - least[i]) / spread);
    } else {
      // Overfull: scale the floors down together and let the cells wrap.
      for (size_t i = 0; i < cols; ++i)
        col_w[i] = total_least > 0 ? (int)((long)width * least[i] / total_least) : width / (int)cols;
    }
    for (size_t i = 0; i < cols; ++i)
      if (col_w[i] < 20) col_w[i] = 20;

    int cursor = y;
    for (Node* r : rows) {
      Style rs = compute_style(*r, style, ua, author, viewport_w);
      if (rs.display == Display::None) continue;
      int cell_x = x;
      int row_h = !rs.height.is_auto() ? len_px(rs.height, viewport_h, 0) : 0;
      size_t col = 0;
      // Two passes over the row: lay the cells out to find the tallest, then
      // stretch every cell's background and border to that height so the row
      // reads as a row.
      std::vector<size_t> bg_slots;
      std::vector<size_t> border_slots;
      for (auto& c : r->children) {
        if (c->kind != Node::Kind::Element) continue;
        if (c->tag != "td" && c->tag != "th") continue;
        if (col >= cols) break;
        int span = cell_span(*c);
        if (col + span > cols) span = (int)(cols - col);
        int cell_w = 0;
        for (size_t s = 0; s < (size_t)span; ++s) cell_w += col_w[col + s];

        Style cellst = compute_style(*c, rs, ua, author, viewport_w);
        if (cellst.display == Display::None) {
          cell_x += cell_w;
          col += span;
          continue;
        }
        size_t before = doc.fragments.size();
        int h = layout_box(*c, cellst, cell_x, cursor, cell_w, depth + 1);
        // The cell's OWN background and border, to be stretched to the row. It
        // used to take "the first two fragments the cell made" on faith, and a
        // cell with no background of its own -- which is every cell in every
        // layout table, since the UA sheet gives td none -- makes no such
        // fragments: the first two are its CONTENT'S. So the first thing in the
        // cell was resized to the height of the row. On google.com that is the
        // search field, stretched from 28px to the row, and it compounded with
        // every level of nesting.
        //
        // A fragment knows which element it came from, so ask.
        for (size_t k = before; k < doc.fragments.size() && k < before + 2; ++k) {
          if (doc.fragments[k].node != c.get()) break;
          if (doc.fragments[k].kind == Fragment::Kind::Rect) bg_slots.push_back(k);
          if (doc.fragments[k].kind == Fragment::Kind::Border) border_slots.push_back(k);
        }
        if (h > row_h) row_h = h;
        cell_x += cell_w;
        col += span;
      }
      for (size_t k : bg_slots) doc.fragments[k].h = row_h;
      for (size_t k : border_slots) doc.fragments[k].h = row_h;
      cursor += row_h;
    }
    return cursor - y;
  }
};

}  // namespace

// ------------------------------------------------------------------ layout --

extern double g_cs_ms, g_cs_match; extern long g_cs_n, g_cs_miss; extern bool g_style_cache_on; void clear_style_cache();
void layout(Document& doc, const Stylesheet& author, const Metrics& m, int viewport_w,
            int viewport_h) {
  g_cs_ms = 0; g_cs_n = 0; g_cs_match = 0; g_cs_miss = 0;
  struct CacheOn { CacheOn() { clear_style_cache(); g_style_cache_on = true; } ~CacheOn() { g_style_cache_on = false; clear_style_cache(); } } cache_on_;
  struct Prof { std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now(); ~Prof() { if (getenv("AIOS_LAYOUT_PROF")) fprintf(stderr, "layout %.0f ms, compute_style %.0f ms in %ld calls, match %.0f ms for %ld misses\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), g_cs_ms, g_cs_n, g_cs_match, g_cs_miss); } } prof_;
  doc.fragments.clear();
  doc.links.clear();
  doc.fragments.reserve(8192);
  doc.links.reserve(2048);
  doc.content_height = 0;
  doc.content_width = 0;
  if (!doc.dom) return;
  if (viewport_w < 120) viewport_w = 120;

  // The initial containing block. `html` and `body` are ordinary boxes styled
  // by the UA sheet; the only thing special here is that their background
  // becomes the page's, because there is nothing behind them.
  Style root;
  root.display = Display::Block;
  root.color = 0xFFD8D8D8;
  root.font_px = 15;
  root.background = 0xFF121212;

  Node* html = doc.dom->first("html");
  Node* body = doc.dom->first("body");
  Node* start = body ? body : (html ? html : doc.dom.get());

  // Probe the ground first, with the dark sheet: a background does not depend
  // on which sheet is chosen, and the choice depends on the background.
  // `html` is an element with a style like any other: a stylesheet that puts
  // custom properties, a font size or a colour on it expects them to reach the
  // body. The walk used to begin AT the body, so every `html { ... }` rule was
  // silently dropped -- Wikipedia's whole type scale lives on `html.vector-...`.
  Style html_probe = root;
  if (html && body && html->kind == Node::Kind::Element)
    html_probe = compute_style(*html, root, ua_stylesheet(false), author, viewport_w);
  Style start_style = start->kind == Node::Kind::Element
                          ? compute_style(*start, html_probe, ua_stylesheet(false), author, viewport_w)
                          : root;
  if (start_style.has_background && (start_style.background >> 24)) doc.background = start_style.background;
  else if (html_probe.has_background && (html_probe.background >> 24)) doc.background = html_probe.background;
  else doc.background = 0xFF121212;

  // The ink has to survive the author's ground.
  //
  // This machine's default sheet is dark -- #d8d8d8 on #121212 -- which is
  // right for a page that brings no colours of its own. But most of the web was
  // written against a browser whose default is black on white, so a page very
  // commonly sets a light BACKGROUND and no colour at all, having no reason to:
  // its author already knew what the text would be. example.com is exactly this
  // (`background-color: #f0f0f2`, no `color`), and it rendered #d8d8d8 on
  // #f0f0f2 -- grey on grey, technically a page, practically unreadable.
  //
  // So: when the page never named a text colour and the ground it did name is
  // light, the ink goes dark. An author who set a colour keeps it, whatever it
  // looks like -- that is their page. This is the same reasoning that took
  // `color` off every heading in the UA sheet: the default has to be able to
  // lose to the page.
  const uint32_t bg = doc.background;
  const int lum = (int)((((bg >> 16) & 0xFF) * 2126 + ((bg >> 8) & 0xFF) * 7152 + (bg & 0xFF) * 722) / 10000);
  const bool author_set_ink = start_style.color != root.color;
  const bool light_ground = lum > 140;
  if (!author_set_ink && light_ground) root.color = 0xFF1A1A1A;

  Engine e{doc, ua_stylesheet(light_ground), author, m, viewport_w, viewport_h};
  // The initial containing block is the viewport. Everything `height: 100%`
  // hangs off this; with no viewport height there is nothing definite and a
  // percentage height stays auto.
  e.avail_h_ = viewport_h > 0 ? viewport_h : -1;
  Style html_style = root;
  if (html && body && html->kind == Node::Kind::Element)
    html_style = compute_style(*html, root, e.ua, author, viewport_w);
  if (start->kind == Node::Kind::Element)
    start_style = compute_style(*start, html_style, e.ua, author, viewport_w);
  else
    start_style = root;
  if (start_style.has_background && (start_style.background >> 24)) doc.background = start_style.background;
  else doc.background = bg;
  if (start_style.display == Display::None || start_style.display == Display::Inline)
    start_style.display = Display::Block;

  // The frame around the page is the BODY'S OWN MARGIN, not a number this
  // engine picked. It used to be a hardcoded 18px on all four sides, which no
  // stylesheet could turn off -- so every application shell that says
  // `body { margin: 0 }` and expects to reach the edges was drawn as a card
  // floating on 18px of background, and its `height: 100%` chain stopped short
  // of the bottom by the same amount. The UA sheet supplies the 8px a plain
  // document expects; an author who sets 0 now gets 0.
  const int gl = start_style.margin[3], gr = start_style.margin[1];
  const int gt = start_style.margin[0], gb = start_style.margin[2];
  int content_w = viewport_w - gl - gr;
  if (content_w < 80) content_w = 80;
  e.avail_h_ = viewport_h > 0 ? viewport_h - gt - gb : -1;
  int h = e.layout_children(*start, start_style, gl, gt, content_w, 0);

  // Everything still out of flow belongs to the initial containing block:
  // `position: fixed`, and any `absolute` box with no positioned ancestor.
  // The viewport is what they resolve against, so its height is the page's --
  // a toast pinned to `bottom: 20px` sits above the bottom of the document.
  e.drain_out_of_flow(0, gl, gt, content_w, viewport_h > 0 ? viewport_h - gt - gb : h, 0,
                      /*is_root=*/true);
  for (const auto& f : doc.fragments) {
    const int bottom = f.y + f.h - gt;
    if (bottom > h) h = bottom;
  }
  doc.content_height = h + gt + gb;
  if (doc.content_width < viewport_w) doc.content_width = viewport_w;

  // Stacking order. A STABLE sort, so document order still decides between
  // boxes at the same level -- which is what makes the background of a box
  // stay behind its own text. Painting in emission order alone put a dialog
  // underneath whatever the page happened to write after it.
  {
    // The DISTINCT levels present, in order -- not every integer between the
    // lowest and the highest: a page that writes `z-index: 9999` would
    // otherwise cost ten thousand passes over every fragment on it.
    std::vector<int> levels;
    for (const auto& f : doc.fragments) {
      bool seen = false;
      for (int l : levels) if (l == f.z) { seen = true; break; }
      if (!seen) levels.push_back(f.z);
    }
    if (levels.size() > 1) {
      for (size_t i = 1; i < levels.size(); ++i) {
        const int v = levels[i];
        size_t j = i;
        while (j > 0 && levels[j - 1] > v) { levels[j] = levels[j - 1]; --j; }
        levels[j] = v;
      }
      std::vector<Fragment> sorted;
      sorted.reserve(doc.fragments.size());
      for (int level : levels)
        for (auto& f : doc.fragments)
          if (f.z == level) sorted.push_back(std::move(f));
      if (sorted.size() == doc.fragments.size()) doc.fragments = std::move(sorted);
    }
  }

  // The title, from the document rather than from the url.
  if (Node* t = doc.dom->first("title")) {
    std::string raw = t->inner_text();
    std::string clean;
    bool space = false;
    for (char c : raw) {
      bool sp = c == ' ' || c == '\t' || c == '\n' || c == '\r';
      if (sp) {
        space = true;
        continue;
      }
      if (space && !clean.empty()) clean += ' ';
      space = false;
      clean += c;
    }
    if (!clean.empty()) doc.title = clean;
  }
  if (doc.title.empty()) doc.title = doc.url;
}

}  // namespace aios::userland::web
