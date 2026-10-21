// The tokeniser and the tree builder.
//
// A page on the real web is not well-formed and never will be, so this parser
// is written the way the ones that work are written: it tolerates everything
// and closes what the author left open. `<p>` closes `<p>`, `<li>` closes
// `<li>`, a stray `</div>` that matches nothing is dropped rather than allowed
// to unwind the document. Two things it will not do: recurse (the open element
// stack is explicit, so a page cannot blow the kernel's stack through nesting)
// and parse the contents of a raw-text element (`<script>` holding `a < b` used
// to eat the rest of the document).
#include <cstring>

#include "aios/userland/web.hpp"

namespace aios::userland::web {
namespace {

// How deep the tree is allowed to get. A page that nests a thousand divs is
// either broken or hostile; either way ring 0 does not follow it down.
constexpr size_t kMaxDepth = 192;

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

bool in_list(const std::string& tag, const char* const* list) {
  for (size_t i = 0; list[i]; ++i)
    if (tag == list[i]) return true;
  return false;
}

// Elements that never have children. `<br>` and `<img>` are the ones a page
// leans on; the rest are here so a `<meta>` in the head cannot swallow it.
const char* const kVoid[] = {"area", "base",  "br",   "col",   "embed",  "hr",    "img", "input",
                             "link", "meta",  "param", "source", "track", "wbr",   "basefont",
                             "frame", "keygen", nullptr};

// Their content is text, not markup. Everything up to the matching close tag
// goes in verbatim.
//
// `noscript` is on this list because THIS ENGINE RUNS SCRIPTS, and the spec is
// explicit: with scripting enabled, a `<noscript>` element's content is parsed
// as raw text and never becomes markup. Parsing it as markup is not a cosmetic
// mistake -- the content of a noscript is written on the assumption that it
// will only ever be seen by a browser that cannot do anything. google.com's
// results page opens with
//
//     <noscript><style>table,div,span,p{display:none}</style>
//               <meta http-equiv="refresh" content="0;url=/...enablejs">
//
// so parsing it collected that stylesheet into the cascade and hid the entire
// page: search worked, fetched 90KB of results, and painted a blank white
// document with zero boxes in it.
const char* const kRawText[] = {"script",  "style",     "xmp",      "iframe",
                                "noembed", "noframes",  "noscript", nullptr};

// Their content is text with entities decoded, but still no markup.
const char* const kEscapableRawText[] = {"textarea", "title", nullptr};

// A start tag from this list implies the end of an open `<p>`.
const char* const kClosesP[] = {"address", "article", "aside",  "blockquote", "details", "div",
                                "dl",      "fieldset", "figcaption", "figure", "footer", "form",
                                "h1",      "h2",     "h3",     "h4",     "h5",     "h6",
                                "header",  "hr",     "main",   "nav",    "ol",     "p",
                                "pre",     "section", "table", "ul",     "li",     "dt",
                                "dd",      "menu",   "summary", "hgroup", nullptr};

// -------------------------------------------------------------- entities --

void append_utf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out += (char)cp;
  } else if (cp < 0x800) {
    out += (char)(0xC0 | (cp >> 6));
    out += (char)(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += (char)(0xE0 | (cp >> 12));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  } else if (cp <= 0x10FFFF) {
    out += (char)(0xF0 | (cp >> 18));
    out += (char)(0x80 | ((cp >> 12) & 0x3F));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  }
}

struct NamedEntity {
  const char* name;
  uint32_t cp;
};

// The named references a page's prose actually uses. Anything outside this set
// is left as the author wrote it, so `&foo;` shows up as `&foo;` instead of
// silently vanishing.
constexpr NamedEntity kEntities[] = {
    {"amp", '&'},      {"lt", '<'},        {"gt", '>'},        {"quot", '"'},     {"apos", '\''},
    {"nbsp", 0x00A0},  {"copy", 0x00A9},   {"reg", 0x00AE},    {"trade", 0x2122}, {"deg", 0x00B0},
    {"plusmn", 0x00B1},{"sup2", 0x00B2},   {"sup3", 0x00B3},   {"micro", 0x00B5}, {"middot", 0x00B7},
    {"frac14", 0x00BC},{"frac12", 0x00BD}, {"frac34", 0x00BE}, {"times", 0x00D7}, {"divide", 0x00F7},
    {"ndash", 0x2013}, {"mdash", 0x2014},  {"lsquo", 0x2018},  {"rsquo", 0x2019}, {"sbquo", 0x201A},
    {"ldquo", 0x201C}, {"rdquo", 0x201D},  {"bdquo", 0x201E},  {"dagger", 0x2020},{"Dagger", 0x2021},
    {"bull", 0x2022},  {"hellip", 0x2026}, {"prime", 0x2032},  {"Prime", 0x2033}, {"lsaquo", 0x2039},
    {"rsaquo", 0x203A},{"euro", 0x20AC},   {"pound", 0x00A3},  {"yen", 0x00A5},   {"cent", 0x00A2},
    {"larr", 0x2190},  {"uarr", 0x2191},   {"rarr", 0x2192},   {"darr", 0x2193},  {"harr", 0x2194},
    {"laquo", 0x00AB}, {"raquo", 0x00BB},  {"shy", 0x00AD},    {"ensp", 0x2002},  {"emsp", 0x2003},
    {"thinsp", 0x2009},{"minus", 0x2212},  {"le", 0x2264},     {"ge", 0x2265},    {"ne", 0x2260},
    {"alpha", 0x03B1}, {"beta", 0x03B2},   {"gamma", 0x03B3},  {"delta", 0x03B4}, {"pi", 0x03C0},
    {"mu", 0x03BC},    {"sigma", 0x03C3},  {"omega", 0x03C9},  {"lambda", 0x03BB},{"infin", 0x221E},
    {"check", 0x2713}, {"cross", 0x2717},  {"star", 0x2605},   {"hearts", 0x2665},{"spades", 0x2660},
};

bool hex_digit(char c, uint32_t& v) {
  if (c >= '0' && c <= '9') { v = (uint32_t)(c - '0'); return true; }
  if (c >= 'a' && c <= 'f') { v = (uint32_t)(c - 'a' + 10); return true; }
  if (c >= 'A' && c <= 'F') { v = (uint32_t)(c - 'A' + 10); return true; }
  return false;
}

// Decode the reference starting at markup[i] (which is the '&'). On success
// advances `i` past it and appends to `out`; on failure leaves both alone so
// the caller emits the '&' literally.
bool decode_entity(const std::string& s, size_t& i, std::string& out) {
  if (i >= s.size() || s[i] != '&') return false;
  size_t j = i + 1;
  if (j < s.size() && s[j] == '#') {
    ++j;
    bool hex = j < s.size() && (s[j] == 'x' || s[j] == 'X');
    if (hex) ++j;
    uint32_t cp = 0;
    size_t digits = 0;
    while (j < s.size()) {
      uint32_t d = 0;
      if (hex) {
        if (!hex_digit(s[j], d)) break;
      } else {
        if (s[j] < '0' || s[j] > '9') break;
        d = (uint32_t)(s[j] - '0');
      }
      // A reference longer than this is not a code point, it is a denial of
      // service dressed as one.
      if (digits > 7) return false;
      cp = cp * (hex ? 16 : 10) + d;
      ++digits;
      ++j;
    }
    if (!digits || cp == 0 || cp > 0x10FFFF) return false;
    if (j < s.size() && s[j] == ';') ++j;
    append_utf8(out, cp);
    i = j;
    return true;
  }
  // A named reference: letters and digits up to the semicolon.
  std::string name;
  while (j < s.size() && name.size() < 32 &&
         ((s[j] >= 'a' && s[j] <= 'z') || (s[j] >= 'A' && s[j] <= 'Z') || (s[j] >= '0' && s[j] <= '9')))
    name += s[j++];
  if (name.empty()) return false;
  for (const auto& e : kEntities) {
    if (name == e.name) {
      if (j < s.size() && s[j] == ';') ++j;
      append_utf8(out, e.cp);
      i = j;
      return true;
    }
  }
  return false;
}

std::string decode_entities(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    if (s[i] == '&' && decode_entity(s, i, out)) continue;
    out += s[i++];
  }
  return out;
}

// ---------------------------------------------------------- the tokeniser --

struct Tokeniser {
  const std::string src;
  size_t pos = 0;

  explicit Tokeniser(std::string s) : src(std::move(s)) {}

  bool done() const { return pos >= src.size(); }

  bool looking_at(const char* what) const {
    size_t n = std::strlen(what);
    if (pos + n > src.size()) return false;
    for (size_t i = 0; i < n; ++i)
      if (lower(src[pos + i]) != lower(what[i])) return false;
    return true;
  }

  void skip_to_after(const char* what) {
    size_t at = src.find(what, pos);
    pos = at == std::string::npos ? src.size() : at + std::strlen(what);
  }

  void skip_space() {
    while (pos < src.size() && is_space(src[pos])) ++pos;
  }

  std::string read_name() {
    std::string n;
    while (pos < src.size()) {
      char c = src[pos];
      if (is_space(c) || c == '>' || c == '/' || c == '=' || c == '<') break;
      n += lower(c);
      ++pos;
    }
    return n;
  }

  // Attributes up to the tag's '>'. Handles quoted, single-quoted and bare
  // values, and a bare name with no value at all (`<input disabled>`).
  void read_attrs(std::vector<Attr>& out, bool& self_closing) {
    self_closing = false;
    for (;;) {
      skip_space();
      if (pos >= src.size()) return;
      if (src[pos] == '>') { ++pos; return; }
      if (src[pos] == '/') {
        ++pos;
        self_closing = true;
        continue;
      }
      std::string name = read_name();
      if (name.empty()) {
        // Something that is not a name and not the end of the tag: step over
        // it rather than spinning here forever.
        ++pos;
        continue;
      }
      skip_space();
      std::string value;
      if (pos < src.size() && src[pos] == '=') {
        ++pos;
        skip_space();
        if (pos < src.size() && (src[pos] == '"' || src[pos] == '\'')) {
          char q = src[pos++];
          size_t end = src.find(q, pos);
          if (end == std::string::npos) end = src.size();
          value = src.substr(pos, end - pos);
          pos = end < src.size() ? end + 1 : end;
        } else {
          while (pos < src.size() && !is_space(src[pos]) && src[pos] != '>') value += src[pos++];
        }
      }
      if (out.size() < 64) out.push_back({name, decode_entities(value)});
    }
  }
};

}  // namespace

// ------------------------------------------------------------------- Node --

std::string Node::attr(const std::string& name) const {
  for (const auto& a : attrs)
    if (a.name == name) return a.value;
  return {};
}

const std::string& Node::attr_ref(const std::string& name) const {
  static const std::string kEmpty;
  for (const auto& a : attrs)
    if (a.name == name) return a.value;
  return kEmpty;
}

bool Node::has_attr(const std::string& name) const {
  for (const auto& a : attrs)
    if (a.name == name) return true;
  return false;
}

std::string Node::inner_text() const {
  std::string out;
  // Explicit walk: a document is user input, and this is called on the tree a
  // page handed us.
  std::vector<const Node*> stack;
  stack.push_back(this);
  std::vector<size_t> index;
  index.push_back(0);
  while (!stack.empty()) {
    const Node* n = stack.back();
    size_t& i = index.back();
    if (n->kind == Kind::Text && i == 0) out += n->text;
    if (i < n->children.size()) {
      const Node* child = n->children[i].get();
      ++i;
      if (stack.size() < kMaxDepth + 4) {
        stack.push_back(child);
        index.push_back(0);
      }
      continue;
    }
    stack.pop_back();
    index.pop_back();
  }
  return out;
}

Node* Node::first(const char* want) {
  std::vector<Node*> stack;
  stack.push_back(this);
  while (!stack.empty()) {
    Node* n = stack.back();
    stack.pop_back();
    if (n != this && n->kind == Kind::Element && n->tag == want) return n;
    for (size_t i = n->children.size(); i > 0; --i) stack.push_back(n->children[i - 1].get());
  }
  return nullptr;
}

// ----------------------------------------------------------- tree builder --

NodePtr parse_html(ByteSpan markup) {
  auto doc = std::make_unique<Node>();
  doc->kind = Node::Kind::Document;

  Tokeniser t(std::string(reinterpret_cast<const char*>(markup.data()), markup.size()));

  // The open element stack. `open.back()` is where the next node goes.
  std::vector<Node*> open;
  open.push_back(doc.get());

  auto current = [&]() -> Node* { return open.back(); };

  auto add_text = [&](const std::string& text) {
    if (text.empty()) return;
    Node* parent = current();
    // Two adjacent text nodes are one text node; the tokeniser produces them
    // whenever an entity or a comment splits a run of prose.
    if (!parent->children.empty() && parent->children.back()->kind == Node::Kind::Text) {
      parent->children.back()->text += text;
      return;
    }
    auto n = std::make_unique<Node>();
    n->kind = Node::Kind::Text;
    n->text = text;
    n->parent = parent;
    parent->children.push_back(std::move(n));
  };

  // Pop the stack down through the nearest open `tag`, if there is one. A
  // close tag that matches nothing at all is dropped: `</div>` in a document
  // that never opened a div must not unwind the body.
  auto close_tag = [&](const std::string& tag) {
    size_t found = 0;
    bool have = false;
    for (size_t i = open.size(); i > 1; --i) {
      if (open[i - 1]->tag == tag) {
        found = i - 1;
        have = true;
        break;
      }
    }
    if (!have) return;
    open.resize(found);
  };

  // The implied end tags. This is the whole of the "tolerate everything" rule
  // that matters for real pages: a list whose `<li>`s are never closed, a
  // table whose `<td>`s are never closed, paragraphs used as separators.
  auto imply_ends = [&](const std::string& tag) {
    auto top = [&]() { return open.size() > 1 ? open.back()->tag : std::string(); };
    if (tag == "li") {
      while (top() == "li") open.pop_back();
    } else if (tag == "dt" || tag == "dd") {
      while (top() == "dt" || top() == "dd") open.pop_back();
    } else if (tag == "td" || tag == "th") {
      while (top() == "td" || top() == "th") open.pop_back();
    } else if (tag == "tr") {
      while (top() == "td" || top() == "th" || top() == "tr") open.pop_back();
    } else if (tag == "thead" || tag == "tbody" || tag == "tfoot") {
      while (top() == "td" || top() == "th" || top() == "tr" || top() == "thead" || top() == "tbody" ||
             top() == "tfoot")
        open.pop_back();
    } else if (tag == "option") {
      while (top() == "option") open.pop_back();
    } else if (tag == "head" || tag == "body") {
      // A page that opens `<body>` after `<head>` without closing it.
      while (top() == "head") open.pop_back();
    }
    if (in_list(tag, kClosesP)) {
      while (top() == "p") open.pop_back();
    }
  };

  auto push_element = [&](const std::string& tag, std::vector<Attr>&& attrs) -> Node* {
    Node* parent = current();
    auto n = std::make_unique<Node>();
    n->kind = Node::Kind::Element;
    n->tag = tag;
    n->attrs = std::move(attrs);
    n->parent = parent;
    Node* raw = n.get();
    parent->children.push_back(std::move(n));
    if (!in_list(tag, kVoid) && open.size() < kMaxDepth) open.push_back(raw);
    return raw;
  };

  std::string pending_text;
  auto flush_text = [&]() {
    if (pending_text.empty()) return;
    add_text(decode_entities(pending_text));
    pending_text.clear();
  };

  while (!t.done()) {
    if (t.src[t.pos] != '<') {
      pending_text += t.src[t.pos++];
      continue;
    }
    // `<` that does not start a tag is text. `a < b` in prose is common, and
    // treating it as a tag start is how a whole paragraph disappears.
    if (t.pos + 1 >= t.src.size()) {
      pending_text += t.src[t.pos++];
      continue;
    }
    char next = t.src[t.pos + 1];
    bool tag_start = (next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z') || next == '/' || next == '!' ||
                     next == '?';
    if (!tag_start) {
      pending_text += t.src[t.pos++];
      continue;
    }

    if (t.looking_at("<!--")) {
      flush_text();
      t.pos += 4;
      t.skip_to_after("-->");
      continue;
    }
    if (t.looking_at("<![CDATA[")) {
      t.pos += 9;
      size_t end = t.src.find("]]>", t.pos);
      if (end == std::string::npos) end = t.src.size();
      pending_text += t.src.substr(t.pos, end - t.pos);
      t.pos = end < t.src.size() ? end + 3 : end;
      continue;
    }
    if (t.looking_at("<!") || t.looking_at("<?")) {
      flush_text();
      size_t end = t.src.find('>', t.pos);
      t.pos = end == std::string::npos ? t.src.size() : end + 1;
      continue;
    }

    if (next == '/') {
      flush_text();
      t.pos += 2;
      std::string tag = t.read_name();
      size_t end = t.src.find('>', t.pos);
      t.pos = end == std::string::npos ? t.src.size() : end + 1;
      if (!tag.empty()) close_tag(tag);
      continue;
    }

    // A start tag.
    flush_text();
    ++t.pos;
    std::string tag = t.read_name();
    if (tag.empty()) continue;
    std::vector<Attr> attrs;
    bool self_closing = false;
    t.read_attrs(attrs, self_closing);

    imply_ends(tag);
    Node* el = push_element(tag, std::move(attrs));

    if (self_closing) {
      if (!open.empty() && open.back() == el) open.pop_back();
      continue;
    }

    // Raw text: everything to the matching close tag is this element's text,
    // markup and all. Without this, `<script>if (a<b)</script>` opens a `<b>`.
    bool raw = in_list(tag, kRawText);
    bool escapable = in_list(tag, kEscapableRawText);
    if (raw || escapable) {
      std::string closer = "</" + tag;
      size_t search = t.pos;
      size_t end = std::string::npos;
      while (search < t.src.size()) {
        size_t at = t.src.find("</", search);
        if (at == std::string::npos) break;
        bool match = true;
        for (size_t i = 0; i < closer.size(); ++i) {
          if (at + i >= t.src.size() || lower(t.src[at + i]) != closer[i]) { match = false; break; }
        }
        // `</scriptable>` is not the end of a `<script>`.
        if (match) {
          size_t after = at + closer.size();
          char c = after < t.src.size() ? t.src[after] : '>';
          if (c == '>' || is_space(c) || c == '/') { end = at; break; }
        }
        search = at + 2;
      }
      std::string body = t.src.substr(t.pos, (end == std::string::npos ? t.src.size() : end) - t.pos);
      if (!body.empty()) {
        auto n = std::make_unique<Node>();
        n->kind = Node::Kind::Text;
        n->text = escapable ? decode_entities(body) : body;
        n->parent = el;
        el->children.push_back(std::move(n));
      }
      if (end == std::string::npos) {
        t.pos = t.src.size();
      } else {
        size_t gt = t.src.find('>', end);
        t.pos = gt == std::string::npos ? t.src.size() : gt + 1;
      }
      if (!open.empty() && open.back() == el) open.pop_back();
      continue;
    }
  }
  flush_text();
  return doc;
}

}  // namespace aios::userland::web
