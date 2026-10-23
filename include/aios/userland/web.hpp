#pragma once
// The machine's own web engine. Not a binding, not an embed: the tokeniser,
// the tree builder, the cascade, the box layout and the painter all live here,
// in the kernel, and the pixels come out of the same compositor that draws the
// rest of the desktop.
//
// The pieces, in the order a page goes through them:
//
//   markup  -> parse_html      -> Node tree            (web_dom.cpp)
//   css     -> parse_css       -> Stylesheet           (web_css.cpp)
//   tree+css-> layout          -> Fragment list        (web_layout.cpp)
//   bytes   -> decode_image    -> Bitmap               (web_image.cpp)
//
// Layout cannot measure text — the thing that will draw it can — so every
// renderer hands the engine its own Metrics. That is why a line breaks against
// the glyphs that actually get drawn, whether that is the compositor's font
// atlas or a console's fixed cell.
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <string>
#include <vector>

#include "aios/core/bytes.hpp"
#include "aios/core/result.hpp"

namespace aios::userland {

// What the page's scripts did. Defined here rather than in web_js.hpp because
// a Browser holds one by value and web_js.hpp includes this file.
class JsRuntime;

struct ScriptRun {
  int scripts = 0;          // <script> elements executed
  int statements = 0;       // top-level statements run, across all of them
  int dom_writes = 0;       // DOM mutations, so a caller knows to lay out again
  int failed = 0;           // scripts that stopped early; the rest still ran
  int listeners = 0;        // handlers a page registered that nothing fires yet
  std::vector<std::string> log;    // what console.log said, in order
  std::string error;        // empty when every script ran to the end
};

// What a page paints straight onto its pane when it uses `aos.paint`, in the
// order it asked. The pane draws these over the laid-out document each frame;
// the engine only records them, because it does not own pixels.
struct PaintOp {
  enum Kind : uint8_t { Clear, Rect, Outline, Text, Line, Circle } op = Clear;
  double a = 0, b = 0, c = 0, d = 0;   // x y w h · x1 y1 x2 y2 · cx cy r · x y (size)
  uint32_t color = 0xFFFFFFFF;
  std::string text;
};

// The machine, as a page sees it through `aos`. Everything here is a callback
// the substrate binds: the engine is in the bare-metal CORE and cannot reach a
// store, a file or a library on its own. Left empty, the corresponding call
// answers honestly (an empty list, an empty string) rather than pretending.
// Here, like ScriptRun, because a Browser holds one by value.
struct HostBindings {
  std::function<std::string(const std::string& kind, const std::string& json)> store_put;   // -> object id
  std::function<std::string(const std::string& kind)> store_list;                            // -> json array
  // How many records this machine holds. The store is APPEND-ONLY — a put adds
  // and nothing is ever rewritten — so this number changing is the only way the
  // answer to list(kind) can change, and a face that reads records can stop
  // asking the store the same question sixty times a second.
  std::function<uint64_t()> store_revision;
  std::function<std::string(const std::string& id)> store_get;                               // -> json object, or ""
  std::function<std::string(const std::string& text, int k)> store_similar;                 // -> json array
  std::function<std::string(const std::string& qstr)> store_query;                          // -> json array
  std::function<std::string(const std::string& name, const std::string& request)> organ;    // -> text
  std::function<std::string()> storage_load;                 // localStorage, as last saved
  std::function<void(const std::string&)> storage_save;
  std::function<void(const std::string&)> say;               // what the page says to the machine's log
  // What this machine measures about itself, by key: cpus, arch, host,
  // mem_total, mem_free (bytes), uptime_s, caps, records. Empty when the key
  // is unknown - never a made-up number.
  std::function<std::string(const std::string& key)> machine;
  std::function<double()> now_ms;                            // the machine's wall clock, ms since the epoch
  std::function<std::string(const std::string& path)> read_file;
  std::function<bool(const std::string& path, const std::string& data)> write_file;
  std::function<std::vector<std::string>(const std::string& dir)> list_dir;
  std::function<std::string(const std::string& url)> http_get;
  // Cryptographic randomness for crypto.getRandomValues / randomUUID. Left empty, those throw: a page that asks for
  // entropy is never handed a predictable stream and told it is random.
  std::function<void(uint8_t* out, size_t n)> random_bytes;
};

}

namespace aios::userland::web {

// ---------------------------------------------------------------- the tree --

struct Attr {
  std::string name;    // lower case, as the cascade wants it
  std::string value;
};

struct Node;
using NodePtr = std::unique_ptr<Node>;

struct Node {
  enum class Kind : uint8_t { Document, Element, Text };

  Kind kind = Kind::Element;
  std::string tag;                  // lower case; elements only
  std::string text;                 // text nodes only, entities already decoded
  std::vector<Attr> attrs;
  std::vector<NodePtr> children;
  Node* parent = nullptr;

  std::string attr(const std::string& name) const;
  // The same without the copy: a reference into the node, or to an empty string.
  const std::string& attr_ref(const std::string& name) const;
  bool has_attr(const std::string& name) const;
  bool is(const char* t) const { return kind == Kind::Element && tag == t; }
  // Every descendant text node, run together. What `<title>` and a link's
  // label are made of.
  std::string inner_text() const;
  Node* first(const char* tag);     // first descendant with this tag, or null
};

// Tokenise the markup and close what the page left open. Never fails: a
// document that is wrong is still a document, which is the whole job.
NodePtr parse_html(ByteSpan markup);

// ------------------------------------------------------------------- style --

// How one part of a compound selector connects to the part before it. `a b` is
// any descendant; `a > b` must be a direct child; `a + b` the next sibling;
// `a ~ b` any later sibling in the same parent.
enum class Combinator : uint8_t { Descendant, Child, Adjacent, Sibling };

// One link in a compound selector: `div`, `.note`, `#main`, `a[href]`, or any
// combination of those on the same element.
struct SimpleSelector {
  // How this part is joined to the one before it. Meaningless for the first
  // part of a selector, which every combinator except the leading (none) skips.
  Combinator prev = Combinator::Descendant;
  std::string tag;                  // empty matches any element
  std::string id;
  std::vector<std::string> classes;
  // An attribute term. `[href]` is presence; the rest compare the value, which
  // is what a page means when it writes `[type="submit"]` or `[href^="https"]`
  // -- both of which used to match every element carrying the attribute at all,
  // so a rule meant for submit buttons styled every input on the form.
  struct AttrTest {
    enum class Op : uint8_t { Present, Equals, Includes, DashMatch, Prefix, Suffix, Contains };
    std::string name;
    Op op = Op::Present;
    std::string value;
    bool matches(const Node& el) const;
  };
  std::vector<AttrTest> attrs;
  bool universal = false;
  // Structural pseudo-classes, evaluated against the element's position among
  // its siblings rather than dropped (a dropped :first-child over-matches every
  // sibling, which is how one responsive rule shredded a whole dashboard).
  bool first_child = false;
  bool last_child = false;
  bool only_child = false;
  bool has_nth = false;
  std::string nth_child;            // `2`, `odd`, `even`, `2n+1` … as written
  bool is_root = false;
  // This compound can never match a real element. A pseudo-ELEMENT styles
  // generated content no element here has, and a compound whose only content
  // was a pseudo has nothing left to match on — it must match NOTHING rather
  // than becoming the universal selector. `::-webkit-scrollbar{width:10px}`
  // became `*{width:10px}`, which made every box on the page ten pixels wide
  // and put every word on its own line.
  bool never = false;
  // `never` was set for a reason OTHER than this compound being a
  // pseudo-element (`:has(...)`, `:visited`, and the rest of the states/
  // functions this engine cannot evaluate). pseudo_content() clears `never`
  // on a `::before`/`::after` compound so it can test the element the
  // generated box hangs off -- but a compound like
  // `a:has(...)::after{content:']'}` carries BOTH markers on the same
  // SimpleSelector, and clearing `never` unconditionally threw the `:has()`
  // condition away with it, matching every plain `<a>` on the page. Real
  // case: Wikipedia's own VisualEditor fallback rule did exactly this,
  // stamping a stray "]" after every wikilink on the page.
  bool never_unevaluable = false;
  // `::before` / `::after`. The compound still matches NOTHING in the ordinary
  // cascade -- generated content is not the element -- but the marker is what
  // lets pseudo_content() find these rules and run them separately.
  enum class PseudoEl : uint8_t { None, Before, After };
  PseudoEl pseudo = PseudoEl::None;
  // `:not(...)` terms, flattened. The negation has to be EVALUATED: dropping
  // it inverts the author's meaning, which is how a responsive rule set gets
  // half-applied.
  // `:is(...)` / `:where(...)` argument lists, and any `:not(...)` whose
  // argument is more than a bare tag/class/id. Each is a selector list,
  // evaluated by re-entering the matcher, which is the only way to be right
  // about `:is(h1, .title)` -- accepting it and ignoring the argument made it
  // the universal selector, and refusing it dropped rules a page needed.
  std::vector<std::string> is_lists;      // all must match
  std::vector<std::string> not_lists;     // none may match
  bool where_only = false;                // :where() contributes no specificity
  std::vector<std::string> not_tags;
  std::vector<std::string> not_ids;
  std::vector<std::string> not_classes;
  int specificity() const;
};

// `nav ul li a` — ancestors first, the element being matched last. Only the
// descendant combinator; `>` is treated as one too, which is wrong in the
// same direction as no combinator at all and never wrong in the other.
struct Selector {
  std::vector<SimpleSelector> parts;
  int specificity() const;
};

struct Declaration {
  std::string property;
  std::string value;
  bool important = false;
};

struct Rule {
  std::vector<Selector> selectors;
  // The selector list as written, for diagnostics: a cascade you cannot name
  // is a cascade you cannot debug.
  std::string source;
  std::vector<Declaration> decls;
  // The `@media` query this rule was found inside, verbatim, or empty. Kept on
  // the rule rather than resolved at parse time so the same parsed sheet can be
  // cascaded again at a different viewport width — which is what a resize is.
  std::string media;
  // `media` evaluated for one viewport width, remembered: the query is a string
  // that has to be parsed, and the cascade asked it of every rule for every
  // element -- thousands of rules, thousands of elements.
  mutable int media_vw = -1;
  mutable bool media_ok = true;
};

struct Stylesheet {
  std::vector<Rule> rules;
  void append(const Stylesheet& other);
  // Rules bucketed by what their rightmost compound names (an id, a class or a
  // tag), so an element only tests the rules that could possibly apply to it.
  // Built on first use and again if rules are added.
  mutable std::shared_ptr<void> rule_index;
  mutable size_t rule_index_n = 0;
};

Stylesheet parse_css(const std::string& source);

// The engine's own default sheet: what an `<h1>` is, what a `<p>` is, why a
// link is blue. Every browser has one; this is ours, and it is the reason an
// unstyled document still reads like a document.
// The engine's own sheet. Two grounds, because a default that cannot follow the
// page is a default that makes pages unreadable: on the light one the link
// colour is darkened, which is the only colour left in the sheet that a ground
// can render illegible.
const Stylesheet& ua_stylesheet(bool light_ground = false);

enum class Display : uint8_t { None, Block, Inline, InlineBlock, ListItem, Table, TableRow, TableCell, Flex, Grid };
// Only the axis matters here: a row lays its children side by side, and
// anything else falls back to stacking, which is what a block already does.
enum class FlexDir : uint8_t { Row, Column };
enum class Align : uint8_t { Left, Center, Right, Justify };
// Flex alignment. `align-items` is the CROSS axis (vertical in a row),
// `justify-content` the MAIN one. Without these every row was top-left packed:
// a toolbar that puts its actions at the far end (`space-between`) had them
// bunched at the start, and a row of a tall label beside a short button
// (`center`) had the button sitting on the label's top edge.
enum class AlignItems : uint8_t { Auto, Stretch, Start, Center, End, Baseline };
// `float` and `clear`: a floated box leaves the flow and sits against one edge of its
// container; the lines of text beside it are shortened to fit.
enum class FloatSide : uint8_t { None, Left, Right };
enum class ClearSide : uint8_t { None, Left, Right, Both };
enum class Justify : uint8_t { Start, Center, End, SpaceBetween, SpaceAround, SpaceEvenly };
// `nowrap` was missing, and its absence is loud: an element that says its text
// must stay on one line, inside a flex row that gives it less room than that,
// wrapped one word -- often one CHARACTER -- per line. A usage readout came out
// as a vertical column of glyphs.
enum class WhiteSpace : uint8_t { Normal, Pre, Nowrap };
// `position`. Static is normal flow. Relative is normal flow, then offset --
// and, crucially, it makes the element a containing block for its absolute
// descendants. Absolute and Fixed are OUT of flow: they take no space, which
// is the whole point and is why laying them out in flow displaced everything
// after them.
enum class Position : uint8_t { Static, Relative, Absolute, Fixed, Sticky };
// `overflow`. Hidden and Clip cut the children off at the box; Auto and Scroll
// do the same and mean a scrollbar could be there. Visible lets them out.
enum class Overflow : uint8_t { Visible, Hidden, Scroll, Auto };

// A length that has not been resolved yet: `width: 60%` cannot become pixels
// until layout knows what it is a percentage of.
struct Length {
  // Vw and Vh are kept AS units rather than resolved at parse time: the cascade
  // does not know the viewport's height, and `100vh` turning into 100 pixels is
  // what `vw` used to do -- the comment said the caller would scale it and no
  // caller ever did.
  enum class Unit : uint8_t { Auto, Px, Percent, Vw, Vh } unit = Unit::Auto;
  float value = 0;
  // The px term of a `calc()` that mixed units: `calc(100% - 40px)` is a
  // percent of 100 with an offset of -40. Dropping the mixed case (which is
  // what "a percentage minus a gutter" is always written as) turned every one
  // of those widths into `auto`.
  float offset_px = 0;
  bool is_auto() const { return unit == Unit::Auto; }
  int resolve(int against, int fallback) const;
};

// The custom properties in scope on an element: `--accent: #00d2ff` and the
// rest of a page's theme. Inherited, and shared by pointer rather than copied
// per element -- a themed application declares its whole palette once on
// `:root`, and copying eighty strings into every one of a thousand nodes' style
// is a cost paid on every layout for nothing.
struct CustomProps {
  // Kept as a flat list: a page has tens of these, not thousands, and a linear
  // scan beats bringing a hash table into a freestanding build.
  std::vector<std::pair<std::string, std::string>> entries;
  const std::string* find(const std::string& name) const;
};
using CustomPropsP = std::shared_ptr<const CustomProps>;

struct Style {
  Display display = Display::Inline;
  // `inline-flex` / `inline-grid`: a flex or grid container INSIDE a line box
  // rather than one that breaks it. The inner display is the same machinery, so
  // this is a flag beside it and not two more enum values -- but without it
  // they were parsed as plain `flex`/`grid`, which made every one of them a
  // full-width block. That is why an anchored dropdown opened at the far right
  // of the page: its `.flyout-wrap { position: relative; display: inline-flex }`
  // was as wide as the header, so the menu's `right: 0` resolved against the
  // header instead of against the button.
  bool inline_level = false;
  FlexDir flex_dir = FlexDir::Row;
  // `flex: none` and `flex: 0 0 auto` pin a child to its own width; anything
  // else lets it share what is left. This is the whole of flex that a page
  // layout actually depends on.
  bool flex_none = false;
  float flex_grow = 0.0f;
  // The rest of the flex item's three numbers, `order`, the reversed
  // directions, and `align-content` (0 normal/stretch, 1 start, 2 center,
  // 3 end, 4 space-between, 5 space-around, 6 space-evenly) for wrapped lines.
  float flex_shrink = 1.0f;
  Length flex_basis;                 // auto = look at width / content
  int order = 0;
  bool flex_reverse = false;         // row-reverse / column-reverse
  bool wrap_reverse = false;
  uint8_t align_content = 0;
  // `grid-template-columns`, as written; the layout reads the tracks out of it
  // (px, fr, auto, repeat(n, …)). Every dashboard a mind writes is a grid, and
  // a grid that silently became a block stacked its tiles in one column.
  std::string grid_template;
  std::string grid_template_rows;
  std::string grid_template_areas;
  std::string grid_area;
  std::string grid_column;
  std::string grid_row;
  // `flex-wrap: wrap`: a row whose items do not fit breaks into more rows
  // instead of crushing every item to a sliver -- a toolbar of inputs and
  // buttons is written this way by every page that has one.
  bool flex_wrap = false;
  AlignItems align_items = AlignItems::Stretch;   // on the container
  AlignItems align_self = AlignItems::Auto;       // on the child, wins over the container's
  // The inline-axis twins, for a grid: `justify-items` on the container and
  // `justify-self` on the item. A grid track hands its item the whole track;
  // anything but stretch makes the item as wide as its content and puts it at
  // the start, the middle or the end of that track.
  AlignItems justify_items = AlignItems::Stretch;
  // `line-height: 1.6` inherits as the NUMBER, so a child in a smaller face gets
  // 1.6 of its own size. Zero means the line height is a length (or unset).
  float line_height_factor = 0.0f;
  // `mask-image: url(...)`: the picture is not drawn, it is a stencil, and the
  // element's background colour shows through where the stencil is opaque. How
  // a page paints a one-colour icon.
  std::string mask_image;
  bool flow_root = false;   // `display: flow-root`: a block that is its own formatting context
  FloatSide float_side = FloatSide::None;
  ClearSide clear_side = ClearSide::None;
  AlignItems justify_self = AlignItems::Auto;
  Justify justify_content = Justify::Start;
  // `min-width` / `max-width`, honoured by flex items and boxes. Without
  // min-width a field a page sized at 160px was handed 14px and its label was
  // drawn on top of its neighbour's.
  Length min_width;
  Length max_width;
  // `gap` between flex items and grid cells. Without it siblings butted
  // together and a page's own spacing was ignored, which reads as "broken".
  int col_gap = 0;
  int row_gap = 0;
  uint32_t color = 0xFFDDDDDD;
  uint32_t background = 0;
  bool has_background = false;
  // Fractional. Designs use 10.5px and 13.5px freely, and rounding each to a
  // whole pixel measures every word a few percent wide -- which is enough, in a
  // tight row, to overflow it and cut a label that fits perfectly well.
  float font_px = 15.0f;
  bool bold = false;
  bool italic = false;
  bool underline = false;
  bool strike = false;
  bool mono = false;
  bool serif = false;   // a serif generic/named face led the font-family list
  Align text_align = Align::Left;
  WhiteSpace white_space = WhiteSpace::Normal;
  int margin[4] = {0, 0, 0, 0};     // top right bottom left, as CSS orders them
  // `margin: auto` on a side. The width stays 0 in `margin` -- an auto margin
  // has no length of its own -- and the flag says the box takes whatever room
  // its width and its siblings leave. `margin: 0 auto` is how a page centres
  // a column.
  bool margin_auto[4] = {false, false, false, false};
  int padding[4] = {0, 0, 0, 0};
  int border[4] = {0, 0, 0, 0};
  uint32_t border_color = 0xFF444444;
  // `border-radius`, as one radius for all four corners. A page that gives its
  // corners four different radii is doing something this does not follow, and
  // the largest of them is closer than a square box: the whole visual language
  // of a modern interface is rounded, and square corners are the single thing
  // that most makes a rendered page look like it came from a different decade.
  int radius = 0;
  // `box-shadow`: the first non-inset shadow a page names. A card that does not
  // sit above the page reads as a panel painted on it, which is most of the
  // difference between a modern interface and a flat one.
  // `box-sizing: border-box` -- a declared width or height INCLUDES the padding
  // and the border. Nearly every page written this decade sets it globally, and
  // without it every padded box with a declared size comes out too big by its
  // own padding.
  bool border_box = false;
  bool has_shadow = false;
  int shadow_x = 0, shadow_y = 0, shadow_blur = 0, shadow_spread = 0;
  uint32_t shadow_color = 0x40000000;
  // `opacity`, and the product of it with every ancestor's. Real group opacity
  // composites a subtree offscreen and blends it once; this multiplies it into
  // each box's alpha instead, which is the same picture for everything that is
  // not overlapping itself, and is what makes `opacity: 0` mean invisible.
  // `transform: translate(...)`. Only the translation: it is the part that
  // moves a box somewhere else, and `translate(-50%, -50%)` is how half the web
  // centres a thing. A percentage is against the box's OWN size, so it cannot
  // be resolved until the box has one -- which is why these are Lengths and not
  // pixels. Scale and rotate are not followed: a rotated box is a different
  // rasterising job, and half-doing it puts text where it does not belong.
  // `content`, for a generated box. Empty string is a real value (a decorative
  // box with no text); `has_content` is what says the property was set.
  std::string content;
  std::string content_attr;   // `content: attr(x)`
  bool has_content = false;
  Length translate_x, translate_y;
  bool has_translate = false;
  // `transform` with more in it than translation -- scale, rotate, skew, matrix -- as the functions were written,
  // applied about `transform-origin` when the box is painted. A list of translations alone stays in
  // translate_x/translate_y above, which layout applies by moving the box. The standalone `rotate` and `scale`
  // properties come first, as CSS orders them (translate, rotate, scale, then transform).
  struct TransformFn {
    enum class Kind : uint8_t { Translate, Scale, Rotate, Skew, Matrix } kind = Kind::Translate;
    Length tx, ty;                    // Translate
    float v[6] = {0, 0, 0, 0, 0, 0};  // Scale: sx sy. Rotate: radians. Skew: x y radians. Matrix: a b c d e f
  };
  std::vector<TransformFn> transform_fns;
  float rotate_rad = 0.0f;            // the `rotate` property
  float scale_x = 1.0f, scale_y = 1.0f;   // the `scale` property
  Length origin_x, origin_y;          // `transform-origin`; auto = the centre
  bool has_xf() const { return !transform_fns.empty() || rotate_rad != 0.0f || scale_x != 1.0f || scale_y != 1.0f; }
  float opacity = 1.0f;
  float effective_opacity = 1.0f;
  // `letter-spacing`. Inherited, like the rest of the type properties: a page
  // sets it on a label class and expects the words inside to carry it.
  float letter_spacing = 0.0f;
  // `text-transform`. A label class that says `uppercase` is how nearly every
  // interface writes a section heading; parsed and dropped, the heading came
  // out in the case the markup happened to use.
  enum class TextCase : uint8_t { None, Upper, Lower, Capitalize };
  TextCase text_case = TextCase::None;
  Length width;
  Length height;
  int line_height = 0;              // 0: derive it from the face
  // `appearance: none`: a control the page draws itself, so the engine paints no checkbox or radio mark of its own.
  bool appearance_none = false;
  // `accent-color` (inherited): the fill of a checked checkbox or radio and of a gauge's bar. 0 = the browser's own.
  uint32_t accent = 0;
  bool list_bullet = true;          // list-style-type: none turns it off
  Position position = Position::Static;
  // `top` / `right` / `bottom` / `left`, unresolved: an absolute box's offsets
  // are percentages of its containing block, which layout knows and the
  // cascade does not.
  Length inset[4];                  // top right bottom left, as CSS orders them
  int z_index = 0;
  bool has_z = false;               // `z-index: auto` is not the same as 0
  Overflow overflow_x = Overflow::Visible;
  Overflow overflow_y = Overflow::Visible;
  bool hidden = false;              // display:none, or a <script>/<head> child
  // Aspect ratio (width / height), e.g. 16/9, 4/3, 1. 0 = unconstrained.
  float aspect_ratio = 0.0f;
  enum class ObjectFit : uint8_t { Fill, Contain, Cover, None, ScaleDown } object_fit = ObjectFit::Fill;
  // `object-position`, as percentages of the free space (center = 50).
  float object_pos_x = 50.0f, object_pos_y = 50.0f;
  // `background-image: url(...)`: the address as written (resolved at layout),
  // and how it is placed. bg_repeat bit 0 repeats across, bit 1 down.
  std::string bg_image;
  uint8_t bg_repeat = 3;
  enum class BgSize : uint8_t { Auto, Cover, Contain, Explicit } bg_size = BgSize::Auto;
  Length bg_size_w, bg_size_h;       // Explicit: auto where is_auto()
  float bg_pos_x = 0.0f, bg_pos_y = 0.0f;   // percent of the free space
  float bg_off_x = 0.0f, bg_off_y = 0.0f;   // px added to that
  // Linear gradient support in backgrounds.
  bool has_gradient = false;
  uint32_t gradient_start = 0;
  uint32_t gradient_end = 0;
  int gradient_deg = 180;
  // The custom properties visible here. Null means "none in scope", which is
  // the common case and costs nothing.
  CustomPropsP custom;
};

// Match and cascade, in the order the spec gives them: the UA sheet, then the
// author's, each rule weighted by its selector's specificity and broken by
// document order, then the element's own `style=""` on top of the lot. What is
// inherited is inherited from `parent`; what is not starts from the initial
// value.
// The style of an element's `::before` or `::after`, when the page gives it
// one. Returns false when there is no such rule -- which is the common case, so
// this is checked before anything is built.
bool pseudo_content(const Node& element, const Style& parent, const Stylesheet& ua,
                    const Stylesheet& author, int viewport_w, bool after, Style* out);

Style compute_style(const Node& element, const Style& parent, const Stylesheet& ua, const Stylesheet& author,
                    int viewport_w);

// Whether a rule found inside `@media (...)` applies to a viewport this wide.
// Exposed because it is the one piece of the cascade a caller may want to ask
// about on its own.
bool media_applies(const std::string& query, int viewport_w);

// Does this element match a selector written the way a page writes it --
// `div.msg`, `.a .b`, `#x > li`, `input[type=text]`, a comma-separated list?
// The CSS engine already knows how to answer that; querySelectorAll used to
// have its own one-simple-selector matcher beside it, so `div.msg` matched
// nothing and every page that asks that way came up empty.
bool selector_matches_text(const Node& el, const std::string& selector);

// A selector list parsed once, for a caller that tests many elements against it (querySelectorAll walks a whole tree).
// `valid` is false for text a browser would reject with a SyntaxError: unbalanced brackets, parentheses or quotes, an
// empty member of the list, a dangling combinator, a character that cannot be part of a selector.
struct CompiledSelectors {
  std::vector<Selector> list;
  bool valid = true;
};
CompiledSelectors compile_selectors(const std::string& selector);
bool compiled_selectors_match(const Node& el, const CompiledSelectors& compiled);

// ------------------------------------------------------------- the painter --

// What the renderer that owns the glyphs tells the engine about them.
struct FontKey {
  float px = 15.0f;
  bool bold = false;
  bool italic = false;
  bool mono = false;
  bool serif = false;
  // `letter-spacing`, in pixels, added after every glyph. It lives on the FONT
  // key rather than beside it so the measurer and the painter cannot disagree
  // about how wide a word is -- which is the way a tracked label ends up
  // overlapping whatever is next to it.
  // Fractional, because designs are: `letter-spacing: .6px` rounded to a whole
  // pixel is 67% too much tracking, and over a thirteen-character label that is
  // enough to overflow the row it sits in and cut the word with an ellipsis.
  float tracking = 0.0f;
};

struct Metrics {
  std::function<int(const std::string&, const FontKey&)> measure;
  std::function<int(const FontKey&)> line_height;
  // Line top to baseline. Without it, a line holding 30px and 15px type can
  // only be bottom-aligned; with it the two sit on the same baseline, which is
  // the difference between a heading with an inline link in it looking right
  // and looking broken.
  std::function<int(const FontKey&)> ascent;
};

// One thing to draw, in page coordinates. A renderer walks these in order and
// paints; it makes no decisions, which is what keeps the console dump and the
// framebuffer pane showing the same page.
struct Fragment {
  enum class Kind : uint8_t { Rect, Text, Border, Image, Rule, Video, Audio, Canvas } kind = Kind::Text;
  int x = 0, y = 0, w = 0, h = 0;
  uint32_t color = 0;
  std::string text;
  FontKey font;
  bool underline = false;
  bool strike = false;
  int link = -1;                    // index into Document::links; -1 is not a link
  // A box-shadow is painted only OUTSIDE the box it belongs to: under a box
  // with no background of its own it must not show through the middle. The
  // box's rectangle, set on the shadow's fragment, is what the painter leaves
  // out. `0 1px` shadows are how a page draws an underline on a bar.
  bool mask = false;                // an Image painted as a stencil in `tint`
  // An <img>'s `object-fit` / `object-position`: how the picture sits in its box.
  uint8_t fit = 0;                  // Style::ObjectFit
  float pos_x = 50.0f, pos_y = 50.0f;
  // A CSS background picture: tiled/sized into the box, not stretched to it.
  bool bg = false;
  uint8_t bg_repeat = 3;
  uint8_t bg_size = 0;              // Style::BgSize
  int bg_w = -1, bg_h = -1;         // Explicit size in px, -1 = auto
  float bg_w_pct = -1, bg_h_pct = -1;   // ...or a percent of the box (>= 0), which the painter resolves
  float bg_pos_x = 0, bg_pos_y = 0, bg_off_x = 0, bg_off_y = 0;
  uint32_t tint = 0;
  bool shadow = false;
  int cut_x = 0, cut_y = 0, cut_w = 0, cut_h = 0;
  std::string src;                  // Image / Media: the resolved absolute url
  std::string alt;
  std::string poster;               // Video poster image url
  std::string media_title;
  bool media_playing = false;
  double media_time = 0;
  double media_duration = 0;
  bool media_controls = true;
  float media_volume = 1.0f;
  // The element this box came from, so a click can find what it hit. A link
  // could already be resolved from `link`, but a handler can be on anything —
  // and without this there was no way to get from a pointer to a DOM node.
  Node* node = nullptr;
  // The stacking order this box paints in. Fragments are sorted by it (stably,
  // so document order still decides between equals) once layout is done: a
  // dialog with `z-index: 100` has to paint over the page it covers, and
  // emission order alone put it underneath whatever came after it in the DOM.
  int z = 0;
  // What this box is clipped to, from the nearest ancestor with a non-visible
  // `overflow`. Empty width means unclipped.
  int clip_x = 0, clip_y = 0, clip_w = 0, clip_h = 0;
  bool clipped = false;
  // The corner radius this box paints with, from `border-radius`.
  int radius = 0;
  // The accumulated `opacity` of this box and its ancestors, 0..255.
  uint8_t opacity = 255;
  // How far this box's edge fades over, in pixels. Only a shadow uses it.
  int blur = 0;
  // Which scroll area this box moves with, or -1. A box inside `overflow:
  // auto` is drawn at its laid-out position minus that area's offset -- which
  // is what makes a pane scroll under its own frame instead of the whole page
  // moving. The innermost area wins.
  int scroller = -1;
  // Which transformed box (Document::transforms) this is drawn through, innermost; -1 for none.
  int xform = -1;
};

// A box with `overflow: auto` or `scroll` and more inside it than fits. The
// frame stays where it is; what is in it moves. Everything here is in page
// coordinates, as the fragments are.
struct ScrollArea {
  Node* node = nullptr;      // what to key a remembered offset on
  int x = 0, y = 0, w = 0, h = 0;
  int content_h = 0;         // how tall the content is, so the offset can be clamped
  int max_offset() const { return content_h > h ? content_h - h : 0; }
};

struct Link {
  std::string href;
  std::string text;
  // The box the pointer has to be inside. A link that wraps across two lines
  // gets two boxes, both pointing at the same href.
  int x = 0, y = 0, w = 0, h = 0;
};

// A decoded picture, in ARGB. Declared here because a Document carries the
// ones the page drew itself.
struct Bitmap {
  int w = 0, h = 0;
  std::vector<uint32_t> pixels;     // 0xAARRGGBB, top row first
  bool empty() const { return w <= 0 || h <= 0 || pixels.empty(); }
};

struct Document {
  NodePtr dom;
  std::string title;
  std::string url;
  std::string base;                 // what relative urls resolve against
  std::vector<Fragment> fragments;
  std::vector<Link> links;
  // Moves on every layout. What a painter keys a cached frame on: a relayout can keep every fragment's count and
  // size and still change what is drawn (a checkbox ticked, a class swapped, an inline picture redrawn).
  uint64_t layout_gen = 0;
  // Boxes drawn through a 2D transform -- `scale`, `rotate`, `skew`, `matrix` (a translation alone just moves the
  // box). The box keeps its place in the flow; its fragments carry the group's index and the painter draws them
  // through x' = ox + a (x - ox) + c (y - oy) + e, y' = oy + b (x - ox) + d (y - oy) + f, where (ox, oy) is the
  // transform-origin in page coordinates. A transformed box inside another nests: `parent` is the enclosing one.
  struct TransformGroup {
    int parent = -1;
    float ox = 0, oy = 0;
    float a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    size_t frag_from = 0, frag_to = 0;   // the fragments it covered while layout ran (before the stacking sort)
    size_t link_from = 0, link_to = 0;
  };
  std::vector<TransformGroup> transforms;
  // Pictures the page carries in its own markup rather than fetching: an inline
  // <svg>, rasterised at layout time and keyed by a name only this document
  // uses. The pane looks here before it looks in the fetched-image store.
  std::vector<std::pair<std::string, Bitmap>> inline_images;
  // The natural size of every picture decoded so far, by resolved url. Layout
  // sizes an <img> that has no declared size from this, so once a fetch learns
  // a size the page is laid out again instead of the picture being squeezed
  // into a box the text already flowed around.
  std::vector<std::pair<std::string, std::pair<int, int>>> image_sizes;
  // The scrollable boxes this layout produced, innermost last.
  std::vector<ScrollArea> scrollers;
  int content_height = 0;
  int content_width = 0;
  uint32_t background = 0xFF121212;
};

// Style the tree and lay it out into a viewport this wide. Idempotent: call it
// again with a new width and the page reflows, which is what a resize is.
// `viewport_h` is what `vh` and a `height: 100%` chain resolve against. Zero
// means "unknown", and then a percentage height has nothing definite to be a
// percentage OF -- which is the old behaviour, and correct for a page laid out
// to its natural height.
void layout(Document& doc, const Stylesheet& author, const Metrics& m, int viewport_w,
            int viewport_h = 0);

// ------------------------------------------------------------------ images --


// PNG (greyscale/truecolour/palette/alpha, all five filters, interlace refused)
// and GIF87a/89a. Anything else comes back as an error and the page draws the
// image's alt text instead, which is what a browser without a codec should do.
Result<Bitmap> decode_image(ByteSpan bytes);
// And out again, as a PNG: what the machine hands to anything that reads
// pictures — including a mind being asked to look at what it drew.
ByteBuffer encode_png(const uint32_t* argb, int w, int h);

// An inline <svg> subtree, drawn into a bitmap of this size. `current` is the
// element's computed colour: every icon set of the shape the web actually uses
// paints with `stroke="currentColor"`, so without it they come out black on a
// dark ground, which is to say invisible.
Result<Bitmap> rasterise_svg(const Node& svg, int w, int h, uint32_t current);
// A CSS colour value (`#rgb[a]`, `#rrggbb[aa]`, `rgb()`/`rgba()`, `hsl()`, the modern functions, a name) as ARGB.
// False when it is not a colour. The cascade's own parser, for SVG's paint attributes.
bool css_colour(const std::string& value, uint32_t& out);

// Nearest-neighbour box resample, for fitting an image into the space layout
// gave it. Good enough for a page; a real resampler is a separate job.
Bitmap scale_bitmap(const Bitmap& src, int w, int h);

// ----------------------------------------------------------------- browsing --

// How the engine gets bytes. It does not know what a socket is: the caller
// hands it something that can fetch, and the same engine then runs on the
// metal, on a hosted kernel, and against a fixture in a drive script.
struct FetchResult {
  std::string body;
  std::string final_url;            // after redirects
  std::string content_type;
  std::string transport;            // "TLSv1.3 …", for the pane's lock
  int status = 0;
};
// What a request is, beyond its url. Browser::submit_form() carries a filled-in
// <form> through here, which is what makes an interface on a page something a
// person can use rather than only look at.
struct FetchOptions {
  std::string method = "GET";
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
};

using Fetch = std::function<Result<FetchResult>(const std::string& url, const FetchOptions& opt)>;

// ------------------------------------------------------------------ forms --

// The form an element is inside, or nullptr.
Node* enclosing_form(Node* n);
// Whether pressing this element submits. A <button> with no type does: that is
// the HTML default, and getting it wrong means the most common button on the
// web does nothing.
bool is_submit_control(const Node* n);

// What submitting a form asks for. GET puts the successful controls in the
// query string, POST puts them in the body with the content type the standard
// names -- and either way this is what turns an interface into something a
// person can actually use.
struct FormRequest {
  std::string url;
  FetchOptions options;
};
FormRequest form_request(const Node& form, const Node* submitter,
                         const std::string& base, const std::string& doc_url);

// A page writes most of its urls relative to itself.
std::string resolve_url(const std::string& base, const std::string& href);

// The page's own `<style>` blocks plus whatever `<link rel=stylesheet>` points
// at, in document order, so a later sheet wins as the cascade says it should.
Stylesheet collect_author_css(Node& doc, const std::string& base, const Fetch& fetch);

// The thing a window drives: one page, a history, and a status line.
class Browser {
 public:
  explicit Browser(Fetch fetch) : fetch_(std::move(fetch)) {}
  // Whether this browser runs the page's script at all. A mail message is
  // markup somebody ELSE wrote and sent you; it is laid out, never executed.
  // Off means the JsRuntime is not constructed, so there is nothing to escape
  // from rather than a sandbox to be trusted.
  void set_scripts_enabled(bool on) { scripts_enabled_ = on; }
  bool scripts_enabled() const { return scripts_enabled_; }

  Result<void> go(const std::string& url, const Metrics& m, int viewport_w);
  // A <form> the person filled in and submitted. GET puts the fields in the
  // query string and POST puts them in the body; either way this is a visit,
  // and it goes in the history like one.
  Result<void> submit_form(const std::string& url, const FetchOptions& opt,
                           const Metrics& m, int viewport_w);
  Result<void> follow(size_t link_index, const Metrics& m, int viewport_w);
  Result<void> reload(const Metrics& m, int viewport_w);
  Result<void> back(const Metrics& m, int viewport_w);
  Result<void> forward(const Metrics& m, int viewport_w);
  bool can_go_back() const { return history_index_ > 0; }
  bool can_go_forward() const { return history_index_ >= 0 && (size_t)history_index_ + 1 < history_.size(); }

  // Re-run layout at a new width without re-fetching. A resize, not a visit.
  void relayout(const Metrics& m, int viewport_w);

  const Document& document() const { return doc_; }
  const std::string& url() const { return doc_.url; }
  const std::string& status() const { return status_; }
  // What script did on this page, so the pane can say so instead of leaving a
  // person to wonder whether anything ran.
  const ::aios::userland::ScriptRun& script() const { return script_; }
  // The page's live JavaScript, or null when it has none. A click goes through
  // this; it is what makes a page respond rather than only draw.
  const std::shared_ptr<::aios::userland::JsRuntime>& js() const { return js_; }
  // Which element is at this point, for delivering an event to it.
  Node* node_at(int x, int y) const;
  const std::string& transport() const { return transport_; }
  bool secure() const { return secure_; }
  // Which link's box the point is in, or -1. Page coordinates.
  int link_at(int x, int y) const;
  // The laid-out page as lines of text, for a console with no framebuffer. The
  // same fragments the pane paints, so the two cannot disagree about the page.
  std::vector<std::string> as_text(int columns) const;
  // Images the last layout asked for, fetched and decoded on demand. Kept by
  // resolved url so a page that uses one sprite forty times fetches it once.
  const Bitmap* image(const std::string& src);
  void fetch_images(size_t limit = 24);
  // The same, for the pictures whose boxes lie in [y0, y1) of the page. A page
  // with two hundred of them loads the ones on screen first, and the rest as
  // they are scrolled to.
  // True when a picture's size became known and its box was left unsized by
  // the last layout: the caller reflows (relayout) so text moves out of the way.
  bool fetch_images_range(size_t limit, int y0, int y1);
  bool has_unfetched_images(int y0, int y1) const;
  size_t image_count() const { return images_.size(); }

  // The machine a page reaches through `aos`, and the height of the pane it is
  // drawn in. Bound to the page's runtime at every visit, so a dream's surface
  // finds the store and the pixels no matter how it was navigated to.
  void set_host(const ::aios::userland::HostBindings& h) { host_ = h; has_host_ = true; }
  void set_view_height(int h) { if (h > 0) view_h_ = h; }

 private:
  ::aios::userland::ScriptRun script_;
  std::shared_ptr<::aios::userland::JsRuntime> js_;
  ::aios::userland::HostBindings host_;
  bool has_host_ = false;
  int view_h_ = 600;
Result<void> visit(const std::string& url, const Metrics& m, int viewport_w, bool record,
                     const FetchOptions& opt = FetchOptions{});

  Fetch fetch_;
  bool scripts_enabled_ = true;
  Document doc_;
  Stylesheet author_;
  std::vector<std::string> history_;
  int history_index_ = -1;
  std::string status_;
  std::string transport_;
  bool secure_ = false;
  std::vector<std::pair<std::string, Bitmap>> images_;
  int last_width_ = 0;
  // What the last layout measured text with, kept so a picture that turns out
  // to have a size can reflow the page without the caller's help.
  Metrics metrics_;
  bool have_metrics_ = false;
};

}  // namespace aios::userland::web
