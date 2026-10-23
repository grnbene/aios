// The regular-expression engine: parser, compiler and backtracking matcher. See regexp.hpp.
#include "regexp.hpp"

#include <algorithm>
#include <cstring>
#include <deque>
#include <unordered_map>

#include "unicode.hpp"

namespace aios::js::re {

namespace {

// ---------------------------------------------------------------------------------------------------
// opcodes
// ---------------------------------------------------------------------------------------------------

enum Op : uint8_t {
  OP_CHAR,          // a = code point, b = canonicalised code point (icase); flag bit0 = backward
  OP_ANY,           // flag bit0 = backward
  OP_CLASS,         // a = class index; flag bit0 = backward
  OP_SPLIT,         // a = preferred target, b = alternative
  OP_JMP,           // a
  OP_SAVE,          // a = capture slot
  OP_RESET_CAPS,    // a = first slot, b = end slot
  OP_COUNTER_INIT,  // a = counter
  OP_COUNTER_LOOP,  // a = counter, b = min, c = max (-1: unbounded), d = exit; flag bit0 = greedy
  OP_COUNTER_INC,   // a
  OP_POS_SAVE,      // a = position register
  OP_PROGRESS,      // a = position register, b = counter, c = min
  OP_LINE_START,
  OP_LINE_END,
  OP_WORDB,
  OP_NOT_WORDB,
  OP_BACKREF,       // a = group; flag bit0 = backward
  OP_LOOK,          // a = pc after the matching OP_LOOK_END; flag bit0 = negative, bit1 = behind
  OP_LOOK_END,
  OP_RUN,           // a = unused, b = min, c = max (-1), flag bit0 = greedy; followed by the atom instruction
  OP_MATCH,
};

// ---------------------------------------------------------------------------------------------------
// the syntax tree
// ---------------------------------------------------------------------------------------------------

struct Node {
  enum Kind { Empty, Char, Any, Class, Seq, Alt, Group, Look, BackRef, NamedBackRef, AssertStart, AssertEnd, WordB, NotWordB, Quant } kind = Empty;
  uint32_t ch = 0;
  int cls = -1;
  std::vector<Node*> kids;
  Node* child = nullptr;
  int cap = -1;
  bool neg = false, behind = false, greedy = true;
  int min = 0, max = -1;
  int cap_lo = 0, cap_hi = 0;   // capture groups numbered [cap_lo, cap_hi) are inside this node
  std::u16string name;
};

// ---------------------------------------------------------------------------------------------------
// character-class predicates
// ---------------------------------------------------------------------------------------------------

enum Prop : int {
  PR_DIGIT, PR_WORD, PR_SPACE, PR_CATEGORY, PR_ANY, PR_ASCII, PR_ALPHABETIC, PR_UPPERCASE, PR_LOWERCASE, PR_WHITESPACE, PR_HEX, PR_ASCII_HEX, PR_ID_START,
  PR_ID_CONTINUE, PR_EMOJI, PR_PICTO, PR_SCRIPT_BASE,   // PR_SCRIPT_BASE + n: the nth script
};

struct R { uint32_t lo, hi; };

#define RANGES(name, ...) const R name[] = {__VA_ARGS__}
RANGES(kLatin, {0x41, 0x5A}, {0x61, 0x7A}, {0xAA, 0xAA}, {0xBA, 0xBA}, {0xC0, 0xD6}, {0xD8, 0xF6}, {0xF8, 0x2B8}, {0x2E0, 0x2E4}, {0x1D00, 0x1D25}, {0x1D2C, 0x1D5C},
       {0x1D62, 0x1D65}, {0x1D6B, 0x1D77}, {0x1D79, 0x1DBE}, {0x1E00, 0x1EFF}, {0x2071, 0x2071}, {0x207F, 0x207F}, {0x2090, 0x209C}, {0x212A, 0x212B},
       {0x2132, 0x2132}, {0x214E, 0x214E}, {0x2160, 0x2188}, {0x2C60, 0x2C7F}, {0xA722, 0xA787}, {0xA78B, 0xA7FF}, {0xAB30, 0xAB5A}, {0xAB5C, 0xAB64},
       {0xFB00, 0xFB06}, {0xFF21, 0xFF3A}, {0xFF41, 0xFF5A});
RANGES(kGreek, {0x370, 0x373}, {0x375, 0x377}, {0x37A, 0x37D}, {0x37F, 0x37F}, {0x384, 0x384}, {0x386, 0x386}, {0x388, 0x3E1}, {0x3F0, 0x3FF}, {0x1D26, 0x1D2A},
       {0x1F00, 0x1FFE}, {0x2126, 0x2126}, {0xAB65, 0xAB65});
RANGES(kCyrillic, {0x400, 0x484}, {0x487, 0x52F}, {0x1C80, 0x1C88}, {0x1D2B, 0x1D2B}, {0x1D78, 0x1D78}, {0x2DE0, 0x2DFF}, {0xA640, 0xA69F}, {0xFE2E, 0xFE2F});
RANGES(kHan, {0x2E80, 0x2FD5}, {0x3005, 0x3005}, {0x3007, 0x3007}, {0x3021, 0x3029}, {0x3038, 0x303B}, {0x3400, 0x4DBF}, {0x4E00, 0x9FFF}, {0xF900, 0xFA6D},
       {0xFA70, 0xFAD9}, {0x20000, 0x2FA1D}, {0x30000, 0x3134A});
RANGES(kHiragana, {0x3041, 0x3096}, {0x309D, 0x309F}, {0x1B001, 0x1B11F}, {0x1F200, 0x1F200});
RANGES(kKatakana, {0x30A1, 0x30FA}, {0x30FD, 0x30FF}, {0x31F0, 0x31FF}, {0x32D0, 0x32FE}, {0x3300, 0x3357}, {0xFF66, 0xFF6F}, {0xFF71, 0xFF9D});
RANGES(kHangul, {0x1100, 0x11FF}, {0x302E, 0x302F}, {0x3131, 0x318E}, {0x3200, 0x321E}, {0x3260, 0x327E}, {0xA960, 0xA97C}, {0xAC00, 0xD7A3}, {0xD7B0, 0xD7FB}, {0xFFA0, 0xFFDC});
RANGES(kArabic, {0x600, 0x604}, {0x606, 0x60B}, {0x60D, 0x61A}, {0x61C, 0x61E}, {0x620, 0x63F}, {0x641, 0x64A}, {0x656, 0x66F}, {0x671, 0x6DC}, {0x6DE, 0x6FF},
       {0x750, 0x77F}, {0x8A0, 0x8FF}, {0xFB50, 0xFDFF}, {0xFE70, 0xFEFC});
RANGES(kHebrew, {0x591, 0x5C7}, {0x5D0, 0x5EA}, {0x5EF, 0x5F4}, {0xFB1D, 0xFB4F});
RANGES(kThai, {0xE01, 0xE3A}, {0xE40, 0xE5B});
RANGES(kDevanagari, {0x900, 0x950}, {0x955, 0x963}, {0x966, 0x97F}, {0xA8E0, 0xA8FF});
RANGES(kArmenian, {0x531, 0x556}, {0x559, 0x58A}, {0x58D, 0x58F}, {0xFB13, 0xFB17});
RANGES(kGeorgian, {0x10A0, 0x10C5}, {0x10C7, 0x10C7}, {0x10CD, 0x10CD}, {0x10D0, 0x10FA}, {0x10FC, 0x10FF}, {0x1C90, 0x1CBF}, {0x2D00, 0x2D2D});
RANGES(kPicto, {0xA9, 0xA9}, {0xAE, 0xAE}, {0x203C, 0x203C}, {0x2049, 0x2049}, {0x2122, 0x2122}, {0x2139, 0x2139}, {0x2194, 0x2199}, {0x21A9, 0x21AA},
       {0x231A, 0x231B}, {0x2328, 0x2328}, {0x2388, 0x2388}, {0x23CF, 0x23CF}, {0x23E9, 0x23F3}, {0x23F8, 0x23FA}, {0x24C2, 0x24C2}, {0x25AA, 0x25AB},
       {0x25B6, 0x25B6}, {0x25C0, 0x25C0}, {0x25FB, 0x25FE}, {0x2600, 0x2605}, {0x2607, 0x2612}, {0x2614, 0x2685}, {0x2690, 0x2705}, {0x2708, 0x2712},
       {0x2714, 0x2714}, {0x2716, 0x2716}, {0x271D, 0x271D}, {0x2721, 0x2721}, {0x2728, 0x2728}, {0x2733, 0x2734}, {0x2744, 0x2744}, {0x2747, 0x2747},
       {0x274C, 0x274C}, {0x274E, 0x274E}, {0x2753, 0x2755}, {0x2757, 0x2757}, {0x2763, 0x2767}, {0x2795, 0x2797}, {0x27A1, 0x27A1}, {0x27B0, 0x27B0},
       {0x27BF, 0x27BF}, {0x2934, 0x2935}, {0x2B05, 0x2B07}, {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2B55, 0x2B55}, {0x3030, 0x3030}, {0x303D, 0x303D},
       {0x3297, 0x3297}, {0x3299, 0x3299}, {0x1F000, 0x1F0FF}, {0x1F10D, 0x1F10F}, {0x1F12F, 0x1F12F}, {0x1F16C, 0x1F171}, {0x1F17E, 0x1F17F}, {0x1F18E, 0x1F18E},
       {0x1F191, 0x1F19A}, {0x1F1AD, 0x1F1E5}, {0x1F201, 0x1F20F}, {0x1F21A, 0x1F21A}, {0x1F22F, 0x1F22F}, {0x1F232, 0x1F23A}, {0x1F23C, 0x1F23F},
       {0x1F249, 0x1F3FA}, {0x1F400, 0x1F53D}, {0x1F546, 0x1F64F}, {0x1F680, 0x1F6FF}, {0x1F774, 0x1F77F}, {0x1F7D5, 0x1F7FF}, {0x1F80C, 0x1F80F},
       {0x1F848, 0x1F84F}, {0x1F85A, 0x1F85F}, {0x1F888, 0x1F88F}, {0x1F8AE, 0x1F8FF}, {0x1F90C, 0x1F93A}, {0x1F93C, 0x1F945}, {0x1F947, 0x1FAFF},
       {0x1FC00, 0x1FFFD});
RANGES(kEmojiExtra, {0x23, 0x23}, {0x2A, 0x2A}, {0x30, 0x39}, {0x1F1E6, 0x1F1FF});
#undef RANGES

struct Script { const char* name; const char* alias; const R* ranges; size_t n; };
#define SCRIPT(nm, al, arr) {nm, al, arr, sizeof(arr) / sizeof(arr[0])}
const Script kScripts[] = {SCRIPT("Latin", "Latn", kLatin), SCRIPT("Greek", "Grek", kGreek), SCRIPT("Cyrillic", "Cyrl", kCyrillic), SCRIPT("Han", "Hani", kHan),
                           SCRIPT("Hiragana", "Hira", kHiragana), SCRIPT("Katakana", "Kana", kKatakana), SCRIPT("Hangul", "Hang", kHangul),
                           SCRIPT("Arabic", "Arab", kArabic), SCRIPT("Hebrew", "Hebr", kHebrew), SCRIPT("Thai", "Thai", kThai),
                           SCRIPT("Devanagari", "Deva", kDevanagari), SCRIPT("Armenian", "Armn", kArmenian), SCRIPT("Georgian", "Geor", kGeorgian)};
#undef SCRIPT
constexpr int kScriptCount = sizeof(kScripts) / sizeof(kScripts[0]);

bool in_ranges(const R* r, size_t n, uint32_t c) {
  for (size_t i = 0; i < n; ++i) if (c >= r[i].lo && c <= r[i].hi) return true;
  return false;
}

uint32_t cat_bit(uni::Cat c) { return 1u << static_cast<unsigned>(c); }

uint32_t mask_of(std::initializer_list<uni::Cat> cats) {
  uint32_t m = 0;
  for (uni::Cat c : cats) m |= cat_bit(c);
  return m;
}

bool is_space_cp(uint32_t c) {
  return (c >= 9 && c <= 13) || c == 0x20 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000 || c == 0xFEFF;
}

bool is_line_terminator(uint32_t c) { return c == '\n' || c == '\r' || c == 0x2028 || c == 0x2029; }

bool item_matches(const ClassItem& it, uint32_t c, bool unicode_icase) {
  bool r;
  if (it.prop < 0) {
    r = c >= it.lo && c <= it.hi;
  } else {
    switch (it.prop) {
      case PR_DIGIT: r = c >= '0' && c <= '9'; break;
      case PR_WORD: r = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || (unicode_icase && (c == 0x17F || c == 0x212A)); break;
      case PR_SPACE: r = is_space_cp(c); break;
      case PR_CATEGORY: r = (cat_bit(uni::category(c)) & it.mask) != 0; break;
      case PR_ANY: r = true; break;
      case PR_ASCII: r = c < 128; break;
      case PR_ALPHABETIC: {
        uni::Cat k = uni::category(c);
        r = (cat_bit(k) & mask_of({uni::Cat::Lu, uni::Cat::Ll, uni::Cat::Lt, uni::Cat::Lm, uni::Cat::Lo, uni::Cat::Nl})) != 0;
        break;
      }
      case PR_UPPERCASE: r = uni::category(c) == uni::Cat::Lu; break;
      case PR_LOWERCASE: r = uni::category(c) == uni::Cat::Ll; break;
      case PR_WHITESPACE: r = is_space_cp(c) && c != 0xFEFF; break;
      case PR_HEX: r = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || (c >= 0xFF10 && c <= 0xFF19) || (c >= 0xFF21 && c <= 0xFF26) || (c >= 0xFF41 && c <= 0xFF46); break;
      case PR_ASCII_HEX: r = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); break;
      case PR_ID_START: r = uni::is_id_start(c); break;
      case PR_ID_CONTINUE: r = uni::is_id_continue(c); break;
      case PR_EMOJI: r = in_ranges(kPicto, sizeof(kPicto) / sizeof(kPicto[0]), c) || in_ranges(kEmojiExtra, sizeof(kEmojiExtra) / sizeof(kEmojiExtra[0]), c); break;
      case PR_PICTO: r = in_ranges(kPicto, sizeof(kPicto) / sizeof(kPicto[0]), c); break;
      default: {
        const int si = it.prop - PR_SCRIPT_BASE;
        r = si >= 0 && si < kScriptCount && in_ranges(kScripts[si].ranges, kScripts[si].n, c);
        break;
      }
    }
  }
  return r != it.negated;
}

bool class_has(const CharClass& cc, uint32_t c, bool uic) {
  bool in = false;
  for (const ClassItem& it : cc.items) if (item_matches(it, c, uic)) { in = true; break; }
  if (in) {
    for (const auto& s : cc.subtract) if (class_has(*s, c, uic)) { in = false; break; }
  }
  if (in && !cc.intersect.empty()) {
    for (const auto& s : cc.intersect) if (!class_has(*s, c, uic)) { in = false; break; }
  } else if (!in && !cc.intersect.empty() && cc.items.empty()) {
    in = true;
    for (const auto& s : cc.intersect) if (!class_has(*s, c, uic)) { in = false; break; }
  }
  return in;
}

uint32_t canonicalize(uint32_t c, bool unicode) {
  if (unicode) return uni::fold(c);
  uint32_t u = uni::simple_upper(c);
  if (c >= 128 && u < 128) return c;
  return u;
}

// ---------------------------------------------------------------------------------------------------
// parser
// ---------------------------------------------------------------------------------------------------

struct ParseError {
  std::string msg;
};

class Parser {
 public:
  Parser(const std::u16string& src, bool unicode, bool vmode, Program& prog) : s_(src), unicode_(unicode), vmode_(vmode), prog_(prog) {}

  Node* parse(std::string* err) {
    prescan();
    Node* n = parse_disjunction();
    if (failed_) { *err = error_; return nullptr; }
    if (pos_ < s_.size()) {
      fail(s_[pos_] == u')' ? "Unmatched ')'" : "Unexpected character");
      *err = error_;
      return nullptr;
    }
    // named back references resolve now that every group is known
    resolve_named(n);
    if (failed_) { *err = error_; return nullptr; }
    return n;
  }

  int total_caps() const { return ncaps_; }
  std::vector<std::u16string> names_;

 private:
  const std::u16string& s_;
  size_t pos_ = 0;
  bool unicode_, vmode_;
  Program& prog_;
  bool failed_ = false;
  std::string error_;
  std::deque<Node> arena_;
  int ncaps_ = 1;
  int prescan_total_ = 0;
  bool has_named_ = false;
  std::unordered_map<std::u16string, int> by_name_;

 public:
  std::deque<Node>& arena() { return arena_; }

 private:
  void fail(const char* m) {
    if (!failed_) { failed_ = true; error_ = m; }
  }

  Node* make(Node::Kind k) {
    arena_.emplace_back();
    arena_.back().kind = k;
    return &arena_.back();
  }

  bool at_end() const { return pos_ >= s_.size(); }
  char16_t peek() const { return pos_ < s_.size() ? s_[pos_] : 0; }
  bool eat(char16_t c) {
    if (pos_ < s_.size() && s_[pos_] == c) { ++pos_; return true; }
    return false;
  }

  // Count capture groups and detect named groups before parsing (back references can point forward).
  void prescan() {
    bool in_class = false;
    for (size_t i = 0; i < s_.size(); ++i) {
      char16_t c = s_[i];
      if (c == u'\\') { ++i; continue; }
      if (in_class) { if (c == u']') in_class = false; continue; }
      if (c == u'[') { in_class = true; continue; }
      if (c == u'(') {
        if (i + 1 < s_.size() && s_[i + 1] == u'?') {
          if (i + 2 < s_.size() && s_[i + 2] == u'<' && i + 3 < s_.size() && s_[i + 3] != u'=' && s_[i + 3] != u'!') { ++prescan_total_; has_named_ = true; }
        } else {
          ++prescan_total_;
        }
      }
    }
    prog_.has_named = has_named_;
  }

  // ---- structure ----

  Node* parse_disjunction() {
    std::vector<Node*> alts;
    const int lo = ncaps_;
    alts.push_back(parse_alternative());
    while (!failed_ && eat(u'|')) alts.push_back(parse_alternative());
    if (alts.size() == 1) return alts[0];
    Node* n = make(Node::Alt);
    n->kids = alts;
    n->cap_lo = lo;
    n->cap_hi = ncaps_;
    return n;
  }

  Node* parse_alternative() {
    Node* seq = make(Node::Seq);
    seq->cap_lo = ncaps_;
    while (!failed_ && !at_end() && peek() != u'|' && peek() != u')') {
      Node* t = parse_term();
      if (failed_) break;
      if (t) seq->kids.push_back(t);
    }
    seq->cap_hi = ncaps_;
    return seq;
  }

  bool parse_int(int* out) {
    size_t start = pos_;
    long v = 0;
    while (!at_end() && peek() >= u'0' && peek() <= u'9') {
      v = v * 10 + (peek() - u'0');
      if (v > 0x7FFFFFFF) v = 0x7FFFFFFF;
      ++pos_;
    }
    *out = static_cast<int>(v);
    return pos_ > start;
  }

  // {n} {n,} {n,m} at pos_ ('{' not yet consumed). Returns false (pos_ unchanged) when it is not a quantifier.
  bool try_braces(int* mn, int* mx) {
    size_t save = pos_;
    if (!eat(u'{')) return false;
    int a, b = -1;
    if (!parse_int(&a)) { pos_ = save; return false; }
    if (eat(u',')) {
      if (peek() == u'}') b = -1;
      else if (!parse_int(&b)) { pos_ = save; return false; }
    } else {
      b = a;
    }
    if (!eat(u'}')) { pos_ = save; return false; }
    *mn = a;
    *mx = b;
    return true;
  }

  Node* parse_term() {
    const int cap_before = ncaps_;
    char16_t c = peek();
    Node* atom = nullptr;
    bool quantifiable = true;
    switch (c) {
      case u'^': ++pos_; return make(Node::AssertStart);
      case u'$': ++pos_; return make(Node::AssertEnd);
      case u'(': {
        ++pos_;
        if (eat(u'?')) {
          if (eat(u':')) {
            Node* body = parse_disjunction();
            if (!eat(u')')) { fail("Unterminated group"); return nullptr; }
            atom = body;
            if (atom->kind == Node::Seq || atom->kind == Node::Alt) {
              Node* g = make(Node::Group);
              g->child = body;
              g->cap = -1;
              atom = g;
            }
          } else if (peek() == u'=' || peek() == u'!') {
            Node* look = make(Node::Look);
            look->neg = peek() == u'!';
            ++pos_;
            look->child = parse_disjunction();
            if (!eat(u')')) { fail("Unterminated group"); return nullptr; }
            atom = look;
            quantifiable = !unicode_;   // Annex B: lookaheads may be quantified in legacy patterns
          } else if (peek() == u'<' && pos_ + 1 < s_.size() && (s_[pos_ + 1] == u'=' || s_[pos_ + 1] == u'!')) {
            Node* look = make(Node::Look);
            look->behind = true;
            look->neg = s_[pos_ + 1] == u'!';
            pos_ += 2;
            look->child = parse_disjunction();
            if (!eat(u')')) { fail("Unterminated group"); return nullptr; }
            atom = look;
            quantifiable = false;
          } else if (peek() == u'<') {
            ++pos_;
            std::u16string name;
            if (!parse_group_name(&name)) return nullptr;
            if (by_name_.count(name)) { fail("Duplicate capture group name"); return nullptr; }
            const int idx = ncaps_++;
            by_name_[name] = idx;
            if (static_cast<int>(names_.size()) <= idx) names_.resize(static_cast<size_t>(idx) + 1);
            names_[static_cast<size_t>(idx)] = name;
            Node* g = make(Node::Group);
            g->cap = idx;
            g->child = parse_disjunction();
            if (!eat(u')')) { fail("Unterminated group"); return nullptr; }
            atom = g;
          } else {
            fail("Invalid group");
            return nullptr;
          }
        } else {
          const int idx = ncaps_++;
          Node* g = make(Node::Group);
          g->cap = idx;
          g->child = parse_disjunction();
          if (!eat(u')')) { fail("Unterminated group"); return nullptr; }
          atom = g;
        }
        break;
      }
      case u')': fail("Unmatched ')'"); return nullptr;
      case u'.': ++pos_; atom = make(Node::Any); break;
      case u'[': atom = parse_class(); if (failed_) return nullptr; break;
      case u'\\': {
        ++pos_;
        atom = parse_atom_escape(&quantifiable);
        if (failed_) return nullptr;
        if (!atom) return nullptr;
        break;
      }
      case u'*': case u'+': case u'?':
        fail("Nothing to repeat");
        return nullptr;
      case u'{': {
        int a, b;
        if (try_braces(&a, &b)) { fail("Nothing to repeat"); return nullptr; }
        if (unicode_) { fail("Nothing to repeat"); return nullptr; }
        ++pos_;
        atom = make_char('{');
        break;
      }
      case u'}': case u']':
        if (unicode_) { fail("Lone quantifier brackets"); return nullptr; }
        ++pos_;
        atom = make_char(c);
        break;
      default: {
        uint32_t cp = c;
        ++pos_;
        if (unicode_ && uni::is_high_surrogate(c) && !at_end() && uni::is_low_surrogate(peek())) {
          cp = 0x10000 + ((static_cast<uint32_t>(c) - 0xD800) << 10) + (peek() - 0xDC00);
          ++pos_;
        }
        atom = make_char(cp);
        break;
      }
    }
    // quantifier
    int mn = 0, mx = -1;
    bool has_q = false;
    if (!at_end()) {
      char16_t q = peek();
      if (q == u'*') { ++pos_; mn = 0; mx = -1; has_q = true; }
      else if (q == u'+') { ++pos_; mn = 1; mx = -1; has_q = true; }
      else if (q == u'?') { ++pos_; mn = 0; mx = 1; has_q = true; }
      else if (q == u'{') {
        if (try_braces(&mn, &mx)) {
          has_q = true;
          if (mx != -1 && mx < mn) { fail("numbers out of order in {} quantifier"); return nullptr; }
        } else if (unicode_) {
          fail("Incomplete quantifier");
          return nullptr;
        }
      }
    }
    if (!has_q) return atom;
    if (!quantifiable) { fail("Nothing to repeat"); return nullptr; }
    Node* qn = make(Node::Quant);
    qn->child = atom;
    qn->min = mn;
    qn->max = mx;
    qn->greedy = !eat(u'?');
    qn->cap_lo = cap_before;
    qn->cap_hi = ncaps_;
    return qn;
  }

  Node* make_char(uint32_t cp) {
    Node* n = make(Node::Char);
    n->ch = cp;
    return n;
  }

  Node* make_class_node(CharClass&& cc) {
    prog_.classes.push_back(std::move(cc));
    Node* n = make(Node::Class);
    n->cls = static_cast<int>(prog_.classes.size() - 1);
    return n;
  }

  // ---- group names ----

  bool parse_group_name(std::u16string* out) {
    // after '<'
    std::u16string name;
    bool first = true;
    for (;;) {
      if (at_end()) { fail("Invalid capture group name"); return false; }
      char16_t c = s_[pos_++];
      if (c == u'>') break;
      uint32_t cp = c;
      if (c == u'\\') {
        if (!eat(u'u')) { fail("Invalid capture group name"); return false; }
        if (!parse_unicode_escape_body(&cp, true)) { fail("Invalid Unicode escape"); return false; }
      } else if (uni::is_high_surrogate(c) && !at_end() && uni::is_low_surrogate(peek())) {
        cp = 0x10000 + ((static_cast<uint32_t>(c) - 0xD800) << 10) + (s_[pos_] - 0xDC00);
        ++pos_;
      }
      const bool ok = first ? (cp == '$' || cp == '_' || uni::is_id_start(cp)) : (cp == '$' || cp == '_' || uni::is_id_continue(cp));
      if (!ok) { fail("Invalid capture group name"); return false; }
      uni::append_code_point(name, cp);
      first = false;
    }
    if (name.empty()) { fail("Invalid capture group name"); return false; }
    *out = name;
    return true;
  }

  // ---- escapes ----

  static int hexval(char16_t c) {
    if (c >= u'0' && c <= u'9') return c - u'0';
    if (c >= u'a' && c <= u'f') return c - u'a' + 10;
    if (c >= u'A' && c <= u'F') return c - u'A' + 10;
    return -1;
  }

  // After "\u": XXXX or {X...} (with `braces_ok`), combining a surrogate pair when `allow_pair`.
  bool parse_unicode_escape_body(uint32_t* out, bool allow_braces_and_pair) {
    if (peek() == u'{' && allow_braces_and_pair) {
      size_t save = pos_;
      ++pos_;
      uint32_t v = 0;
      int digits = 0;
      while (!at_end() && hexval(peek()) >= 0) {
        v = v * 16 + static_cast<uint32_t>(hexval(peek()));
        if (v > 0x10FFFF) { pos_ = save; return false; }
        ++pos_;
        ++digits;
      }
      if (!digits || !eat(u'}')) { pos_ = save; return false; }
      *out = v;
      return true;
    }
    if (pos_ + 4 > s_.size()) return false;
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      int h = hexval(s_[pos_ + static_cast<size_t>(i)]);
      if (h < 0) return false;
      v = v * 16 + static_cast<uint32_t>(h);
    }
    pos_ += 4;
    if (allow_braces_and_pair && uni::is_high_surrogate(static_cast<char16_t>(v)) && pos_ + 6 <= s_.size() && s_[pos_] == u'\\' && s_[pos_ + 1] == u'u') {
      uint32_t lo = 0;
      bool ok = true;
      for (int i = 0; i < 4; ++i) {
        int h = hexval(s_[pos_ + 2 + static_cast<size_t>(i)]);
        if (h < 0) { ok = false; break; }
        lo = lo * 16 + static_cast<uint32_t>(h);
      }
      if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
        pos_ += 6;
        v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
      }
    }
    *out = v;
    return true;
  }

  ClassItem range_item(uint32_t lo, uint32_t hi) {
    ClassItem it;
    it.lo = lo;
    it.hi = hi;
    return it;
  }
  ClassItem prop_item(int prop, bool negated, uint32_t mask = 0) {
    ClassItem it;
    it.prop = prop;
    it.negated = negated;
    it.mask = mask;
    return it;
  }

  // \d \D \w \W \s \S: appends the class item and returns true.
  bool class_escape_item(char16_t c, ClassItem* out) {
    switch (c) {
      case u'd': *out = prop_item(PR_DIGIT, false); return true;
      case u'D': *out = prop_item(PR_DIGIT, true); return true;
      case u'w': *out = prop_item(PR_WORD, false); return true;
      case u'W': *out = prop_item(PR_WORD, true); return true;
      case u's': *out = prop_item(PR_SPACE, false); return true;
      case u'S': *out = prop_item(PR_SPACE, true); return true;
      default: return false;
    }
  }

  static uint32_t category_mask(const std::u16string& v) {
    using C = uni::Cat;
    struct E { const char* n; const char* l; uint32_t m; };
    static const E table[] = {
        {"L", "Letter", 0}, {"Lu", "Uppercase_Letter", 0}, {"Ll", "Lowercase_Letter", 0}, {"Lt", "Titlecase_Letter", 0}, {"Lm", "Modifier_Letter", 0},
        {"Lo", "Other_Letter", 0}, {"LC", "Cased_Letter", 0}, {"M", "Mark", 0}, {"Mn", "Nonspacing_Mark", 0}, {"Mc", "Spacing_Mark", 0}, {"Me", "Enclosing_Mark", 0},
        {"N", "Number", 0}, {"Nd", "Decimal_Number", 0}, {"Nl", "Letter_Number", 0}, {"No", "Other_Number", 0}, {"P", "Punctuation", 0}, {"Pc", "Connector_Punctuation", 0},
        {"Pd", "Dash_Punctuation", 0}, {"Ps", "Open_Punctuation", 0}, {"Pe", "Close_Punctuation", 0}, {"Pi", "Initial_Punctuation", 0}, {"Pf", "Final_Punctuation", 0},
        {"Po", "Other_Punctuation", 0}, {"S", "Symbol", 0}, {"Sm", "Math_Symbol", 0}, {"Sc", "Currency_Symbol", 0}, {"Sk", "Modifier_Symbol", 0}, {"So", "Other_Symbol", 0},
        {"Z", "Separator", 0}, {"Zs", "Space_Separator", 0}, {"Zl", "Line_Separator", 0}, {"Zp", "Paragraph_Separator", 0}, {"C", "Other", 0}, {"Cc", "Control", 0},
        {"Cf", "Format", 0}, {"Cs", "Surrogate", 0}, {"Co", "Private_Use", 0}, {"Cn", "Unassigned", 0}};
    (void)table;
    std::string n;
    for (char16_t c : v) n.push_back(static_cast<char>(c));
    auto is = [&](const char* a, const char* b) { return n == a || n == b; };
    if (is("L", "Letter")) return mask_of({C::Lu, C::Ll, C::Lt, C::Lm, C::Lo});
    if (is("Lu", "Uppercase_Letter")) return mask_of({C::Lu});
    if (is("Ll", "Lowercase_Letter")) return mask_of({C::Ll});
    if (is("Lt", "Titlecase_Letter")) return mask_of({C::Lt});
    if (is("Lm", "Modifier_Letter")) return mask_of({C::Lm});
    if (is("Lo", "Other_Letter")) return mask_of({C::Lo});
    if (is("LC", "Cased_Letter")) return mask_of({C::Lu, C::Ll, C::Lt});
    if (is("M", "Mark") || n == "Combining_Mark") return mask_of({C::Mn, C::Mc, C::Me});
    if (is("Mn", "Nonspacing_Mark")) return mask_of({C::Mn});
    if (is("Mc", "Spacing_Mark")) return mask_of({C::Mc});
    if (is("Me", "Enclosing_Mark")) return mask_of({C::Me});
    if (is("N", "Number")) return mask_of({C::Nd, C::Nl, C::No});
    if (is("Nd", "Decimal_Number") || n == "digit") return mask_of({C::Nd});
    if (is("Nl", "Letter_Number")) return mask_of({C::Nl});
    if (is("No", "Other_Number")) return mask_of({C::No});
    if (is("P", "Punctuation") || n == "punct") return mask_of({C::Pc, C::Pd, C::Ps, C::Pe, C::Pi, C::Pf, C::Po});
    if (is("Pc", "Connector_Punctuation")) return mask_of({C::Pc});
    if (is("Pd", "Dash_Punctuation")) return mask_of({C::Pd});
    if (is("Ps", "Open_Punctuation")) return mask_of({C::Ps});
    if (is("Pe", "Close_Punctuation")) return mask_of({C::Pe});
    if (is("Pi", "Initial_Punctuation")) return mask_of({C::Pi});
    if (is("Pf", "Final_Punctuation")) return mask_of({C::Pf});
    if (is("Po", "Other_Punctuation")) return mask_of({C::Po});
    if (is("S", "Symbol")) return mask_of({C::Sm, C::Sc, C::Sk, C::So});
    if (is("Sm", "Math_Symbol")) return mask_of({C::Sm});
    if (is("Sc", "Currency_Symbol")) return mask_of({C::Sc});
    if (is("Sk", "Modifier_Symbol")) return mask_of({C::Sk});
    if (is("So", "Other_Symbol")) return mask_of({C::So});
    if (is("Z", "Separator")) return mask_of({C::Zs, C::Zl, C::Zp});
    if (is("Zs", "Space_Separator")) return mask_of({C::Zs});
    if (is("Zl", "Line_Separator")) return mask_of({C::Zl});
    if (is("Zp", "Paragraph_Separator")) return mask_of({C::Zp});
    if (is("C", "Other")) return mask_of({C::Cc, C::Cf, C::Cs, C::Co, C::Cn, C::Other});
    if (is("Cc", "Control") || n == "cntrl") return mask_of({C::Cc});
    if (is("Cf", "Format")) return mask_of({C::Cf});
    if (is("Cs", "Surrogate")) return mask_of({C::Cs});
    if (is("Co", "Private_Use")) return mask_of({C::Co});
    if (is("Cn", "Unassigned")) return mask_of({C::Cn});
    return 0;
  }

  // \p{...} / \P{...} after "\p": parses "{name}" or "{name=value}".
  bool parse_property(bool negated, ClassItem* out) {
    if (!eat(u'{')) { fail("Invalid property name"); return false; }
    std::u16string name, value;
    bool has_value = false;
    while (!at_end() && peek() != u'}') {
      char16_t c = s_[pos_++];
      if (c == u'=' && !has_value) { has_value = true; continue; }
      (has_value ? value : name).push_back(c);
    }
    if (!eat(u'}')) { fail("Invalid property name"); return false; }
    std::string n, v;
    for (char16_t c : name) n.push_back(static_cast<char>(c));
    for (char16_t c : value) v.push_back(static_cast<char>(c));
    if (has_value) {
      if (n == "General_Category" || n == "gc") {
        uint32_t m = category_mask(value);
        if (!m) { fail("Invalid property name"); return false; }
        *out = prop_item(PR_CATEGORY, negated, m);
        return true;
      }
      if (n == "Script" || n == "sc" || n == "Script_Extensions" || n == "scx") {
        for (int i = 0; i < kScriptCount; ++i)
          if (v == kScripts[i].name || v == kScripts[i].alias) { *out = prop_item(PR_SCRIPT_BASE + i, negated); return true; }
        fail("Invalid property name");
        return false;
      }
      fail("Invalid property name");
      return false;
    }
    if (uint32_t m = category_mask(name)) { *out = prop_item(PR_CATEGORY, negated, m); return true; }
    struct B { const char* n; int p; };
    static const B bins[] = {{"Any", PR_ANY}, {"ASCII", PR_ASCII}, {"Alphabetic", PR_ALPHABETIC}, {"Alpha", PR_ALPHABETIC}, {"Uppercase", PR_UPPERCASE}, {"Upper", PR_UPPERCASE},
                             {"Lowercase", PR_LOWERCASE}, {"Lower", PR_LOWERCASE}, {"White_Space", PR_WHITESPACE}, {"space", PR_WHITESPACE}, {"Hex_Digit", PR_HEX},
                             {"AHex", PR_ASCII_HEX}, {"ASCII_Hex_Digit", PR_ASCII_HEX}, {"ID_Start", PR_ID_START}, {"IDS", PR_ID_START}, {"ID_Continue", PR_ID_CONTINUE},
                             {"IDC", PR_ID_CONTINUE}, {"XID_Start", PR_ID_START}, {"XIDS", PR_ID_START}, {"XID_Continue", PR_ID_CONTINUE}, {"XIDC", PR_ID_CONTINUE}, {"Emoji", PR_EMOJI}, {"Emoji_Presentation", PR_PICTO}, {"Extended_Pictographic", PR_PICTO}, {"ExtPict", PR_PICTO}};
    for (const B& b : bins)
      if (n == b.n) { *out = prop_item(b.p, negated); return true; }
    fail("Invalid property name");
    return false;
  }

  // The character a simple escape stands for, or -1. `in_class` enables \b = backspace and \-.
  Node* parse_atom_escape(bool* quantifiable) {
    (void)quantifiable;
    if (at_end()) { fail("\\ at end of pattern"); return nullptr; }
    char16_t c = s_[pos_++];
    ClassItem item;
    if (class_escape_item(c, &item)) {
      CharClass cc;
      cc.items.push_back(item);
      return make_class_node(std::move(cc));
    }
    switch (c) {
      case u'b': return make(Node::WordB);
      case u'B': return make(Node::NotWordB);
      case u'p': case u'P':
        if (unicode_) {
          ClassItem pi;
          if (!parse_property(c == u'P', &pi)) return nullptr;
          CharClass cc;
          cc.items.push_back(pi);
          return make_class_node(std::move(cc));
        }
        return make_char(c);
      case u'k':
        if (unicode_ || has_named_) {
          if (!eat(u'<')) { fail("Invalid named reference"); return nullptr; }
          std::u16string name;
          if (!parse_group_name(&name)) { return nullptr; }
          Node* n = make(Node::NamedBackRef);
          n->name = name;
          return n;
        }
        return make_char(c);
      default: break;
    }
    if (c >= u'1' && c <= u'9') {
      size_t save = pos_ - 1;
      int v = 0;
      pos_ = save;
      while (!at_end() && peek() >= u'0' && peek() <= u'9') {
        v = v * 10 + (peek() - u'0');
        if (v > 100000) v = 100000;
        ++pos_;
      }
      if (v <= prescan_total_) {
        Node* n = make(Node::BackRef);
        n->cap = v;
        return n;
      }
      if (unicode_) { fail("Invalid escape"); return nullptr; }
      pos_ = save;
      if (c >= u'8') { ++pos_; return make_char(c); }
      return make_char(parse_legacy_octal());
    }
    if (c == u'0') {
      if (!at_end() && peek() >= u'0' && peek() <= u'9') {
        if (unicode_) { fail("Invalid decimal escape"); return nullptr; }
        --pos_;
        return make_char(parse_legacy_octal());
      }
      return make_char(0);
    }
    uint32_t cp;
    if (!parse_character_escape(c, &cp, false)) return nullptr;
    return make_char(cp);
  }

  uint32_t parse_legacy_octal() {
    uint32_t v = 0;
    int n = 0;
    while (!at_end() && peek() >= u'0' && peek() <= u'7' && n < 3) {
      uint32_t nv = v * 8 + (peek() - u'0');
      if (nv > 0377) break;
      v = nv;
      ++pos_;
      ++n;
    }
    return v;
  }

  // After "\" and the escape letter `c` (already consumed): the code point it denotes.
  bool parse_character_escape(char16_t c, uint32_t* out, bool in_class) {
    switch (c) {
      case u't': *out = '\t'; return true;
      case u'n': *out = '\n'; return true;
      case u'v': *out = '\v'; return true;
      case u'f': *out = '\f'; return true;
      case u'r': *out = '\r'; return true;
      case u'b':
        if (in_class) { *out = '\b'; return true; }
        break;
      case u'c': {
        char16_t l = peek();
        if ((l >= u'a' && l <= u'z') || (l >= u'A' && l <= u'Z')) {
          ++pos_;
          *out = l % 32;
          return true;
        }
        if (in_class && !unicode_ && ((l >= u'0' && l <= u'9') || l == u'_')) {
          ++pos_;
          *out = l % 32;
          return true;
        }
        if (unicode_) { fail("Invalid unicode escape"); return false; }
        --pos_;   // legacy: the backslash is a literal and `c` follows
        *out = '\\';
        return true;
      }
      case u'x': {
        if (pos_ + 2 <= s_.size() && hexval(s_[pos_]) >= 0 && hexval(s_[pos_ + 1]) >= 0) {
          *out = static_cast<uint32_t>(hexval(s_[pos_]) * 16 + hexval(s_[pos_ + 1]));
          pos_ += 2;
          return true;
        }
        if (unicode_) { fail("Invalid escape"); return false; }
        *out = 'x';
        return true;
      }
      case u'u': {
        uint32_t v;
        if (parse_unicode_escape_body(&v, unicode_)) { *out = v; return true; }
        if (unicode_) { fail("Invalid Unicode escape"); return false; }
        *out = 'u';
        return true;
      }
      default: break;
    }
    // identity escapes
    if (unicode_) {
      static const char16_t syntax[] = u"^$\\.*+?()[]{}|/";
      if (std::char_traits<char16_t>::find(syntax, 15, c) || (in_class && c == u'-')) { *out = c; return true; }
      fail("Invalid escape");
      return false;
    }
    if (c == u'k' && has_named_) { fail("Invalid named reference"); return false; }
    *out = c;
    return true;
  }

  // ---- classes ----

  Node* parse_class() {
    ++pos_;   // [
    CharClass cc;
    if (!parse_class_body(&cc)) return nullptr;
    return make_class_node(std::move(cc));
  }

  // The contents after '[' up to and including ']'.
  bool parse_class_body(CharClass* cc) {
    if (eat(u'^')) cc->negate = true;
    if (vmode_) return parse_class_body_v(cc);
    for (;;) {
      if (at_end()) { fail("Unterminated character class"); return false; }
      if (eat(u']')) return true;
      ClassItem lo_item;
      bool lo_is_class = false;
      uint32_t lo = 0;
      if (!parse_class_atom(&lo, &lo_item, &lo_is_class)) return false;
      if (peek() == u'-' && pos_ + 1 < s_.size() && s_[pos_ + 1] != u']') {
        ++pos_;   // '-'
        ClassItem hi_item;
        bool hi_is_class = false;
        uint32_t hi = 0;
        if (!parse_class_atom(&hi, &hi_item, &hi_is_class)) return false;
        if (lo_is_class || hi_is_class) {
          if (unicode_) { fail("Invalid character class"); return false; }
          if (lo_is_class) cc->items.push_back(lo_item); else cc->items.push_back(range_item(lo, lo));
          cc->items.push_back(range_item('-', '-'));
          if (hi_is_class) cc->items.push_back(hi_item); else cc->items.push_back(range_item(hi, hi));
          continue;
        }
        if (lo > hi) { fail("Range out of order in character class"); return false; }
        cc->items.push_back(range_item(lo, hi));
        continue;
      }
      if (lo_is_class) cc->items.push_back(lo_item);
      else cc->items.push_back(range_item(lo, lo));
    }
  }

  bool parse_class_atom(uint32_t* cp, ClassItem* item, bool* is_class) {
    if (at_end()) { fail("Unterminated character class"); return false; }
    char16_t c = s_[pos_++];
    if (c != u'\\') {
      *cp = c;
      if (unicode_ && uni::is_high_surrogate(c) && !at_end() && uni::is_low_surrogate(peek())) {
        *cp = 0x10000 + ((static_cast<uint32_t>(c) - 0xD800) << 10) + (peek() - 0xDC00);
        ++pos_;
      }
      return true;
    }
    if (at_end()) { fail("\\ at end of pattern"); return false; }
    c = s_[pos_++];
    if (class_escape_item(c, item)) { *is_class = true; return true; }
    if ((c == u'p' || c == u'P') && unicode_) {
      if (!parse_property(c == u'P', item)) return false;
      *is_class = true;
      return true;
    }
    if (c == u'B' && unicode_) { fail("Invalid escape"); return false; }
    if (c >= u'0' && c <= u'9') {
      if (c == u'0' && !(peek() >= u'0' && peek() <= u'9')) { *cp = 0; return true; }
      if (unicode_) { fail("Invalid class escape"); return false; }
      if (c >= u'8') { *cp = c; return true; }
      --pos_;
      *cp = parse_legacy_octal();
      return true;
    }
    if (c == u'k' && !unicode_ && !has_named_) { *cp = 'k'; return true; }
    return parse_character_escape(c, cp, true);
  }

  // v-mode class: unions, ranges, nested classes, \q{...}, and the -- and && operators.
  bool parse_class_body_v(CharClass* cc) {
    int op = 0;   // 0 none yet, 1 subtraction, 2 intersection
    bool first_operand = true;
    for (;;) {
      if (at_end()) { fail("Unterminated character class"); return false; }
      if (eat(u']')) return true;
      if (peek() == u'-' && pos_ + 1 < s_.size() && s_[pos_ + 1] == u'-') {
        pos_ += 2;
        op = 1;
        auto operand = std::make_shared<CharClass>();
        if (!parse_class_operand_v(operand.get())) return false;
        cc->subtract.push_back(operand);
        continue;
      }
      if (peek() == u'&' && pos_ + 1 < s_.size() && s_[pos_ + 1] == u'&') {
        pos_ += 2;
        op = 2;
        auto operand = std::make_shared<CharClass>();
        if (!parse_class_operand_v(operand.get())) return false;
        cc->intersect.push_back(operand);
        continue;
      }
      if (op != 0) { fail("Invalid set operation in character class"); return false; }
      CharClass operand;
      if (!parse_class_operand_v(&operand, true)) return false;
      if (first_operand && peek() == u']' && operand.items.size() == 1 && operand.subtract.empty() && operand.intersect.empty() && !operand.negate) {
        cc->items.push_back(operand.items[0]);
      } else if (operand.negate || !operand.subtract.empty() || !operand.intersect.empty()) {
        // a nested class that is more than a flat union becomes an intersection operand of an empty-item class
        auto sub = std::make_shared<CharClass>(std::move(operand));
        if (cc->items.empty() && cc->intersect.empty() && first_operand) cc->intersect.push_back(sub);
        else { fail("Unsupported nested class in a unicode-sets character class"); return false; }
      } else {
        for (const ClassItem& it : operand.items) cc->items.push_back(it);
        for (auto& st : operand.strings) cc->strings.push_back(st);
      }
      first_operand = false;
    }
  }

  // One operand: a nested class, \q{...}, an escape, or a (possibly ranged) character.
  bool parse_class_operand_v(CharClass* out, bool allow_range = false) {
    if (eat(u'[')) {
      return parse_class_body(out);
    }
    if (peek() == u'\\' && pos_ + 1 < s_.size() && s_[pos_ + 1] == u'q') {
      pos_ += 2;
      if (!eat(u'{')) { fail("Invalid escape"); return false; }
      std::vector<uint32_t> cur;
      for (;;) {
        if (at_end()) { fail("Invalid escape"); return false; }
        char16_t c = s_[pos_++];
        if (c == u'}' || c == u'|') {
          if (cur.size() == 1) out->items.push_back(range_item(cur[0], cur[0]));
          else out->strings.push_back(cur);
          cur.clear();
          if (c == u'}') break;
          continue;
        }
        uint32_t cp = c;
        if (c == u'\\') {
          if (at_end()) { fail("Invalid escape"); return false; }
          char16_t e = s_[pos_++];
          if (!parse_character_escape(e, &cp, true)) return false;
        } else if (uni::is_high_surrogate(c) && !at_end() && uni::is_low_surrogate(peek())) {
          cp = 0x10000 + ((static_cast<uint32_t>(c) - 0xD800) << 10) + (peek() - 0xDC00);
          ++pos_;
        }
        cur.push_back(cp);
      }
      return true;
    }
    ClassItem item;
    bool is_class = false;
    uint32_t cp = 0;
    if (!parse_class_atom(&cp, &item, &is_class)) return false;
    if (is_class) { out->items.push_back(item); return true; }
    if (allow_range && peek() == u'-' && pos_ + 1 < s_.size() && s_[pos_ + 1] != u'-' && s_[pos_ + 1] != u']') {
      ++pos_;
      uint32_t hi = 0;
      ClassItem hitem;
      bool hclass = false;
      if (!parse_class_atom(&hi, &hitem, &hclass)) return false;
      if (hclass || cp > hi) { fail("Range out of order in character class"); return false; }
      out->items.push_back(range_item(cp, hi));
      return true;
    }
    out->items.push_back(range_item(cp, cp));
    return true;
  }

  void resolve_named(Node* n) {
    if (!n || failed_) return;
    if (n->kind == Node::NamedBackRef) {
      auto it = by_name_.find(n->name);
      if (it == by_name_.end()) { fail("Invalid named capture referenced"); return; }
      n->kind = Node::BackRef;
      n->cap = it->second;
    }
    if (n->child) resolve_named(n->child);
    for (Node* k : n->kids) resolve_named(k);
  }
};

// ---------------------------------------------------------------------------------------------------
// code generation
// ---------------------------------------------------------------------------------------------------

class Emitter {
 public:
  Emitter(Program& p) : p_(p) {}

  void emit_node(Node* n, bool back) {
    switch (n->kind) {
      case Node::Empty: return;
      case Node::Char: {
        Inst i;
        i.op = OP_CHAR;
        i.a = static_cast<int32_t>(n->ch);
        i.b = static_cast<int32_t>(p_.icase ? canonicalize(n->ch, p_.unicode || p_.unicode_sets) : n->ch);
        i.flag = back ? 1 : 0;
        p_.code.push_back(i);
        return;
      }
      case Node::Any: {
        Inst i;
        i.op = OP_ANY;
        i.flag = back ? 1 : 0;
        p_.code.push_back(i);
        return;
      }
      case Node::Class: {
        Inst i;
        i.op = OP_CLASS;
        i.a = n->cls;
        i.flag = back ? 1 : 0;
        const CharClass& cc = p_.classes[static_cast<size_t>(n->cls)];
        if (!cc.strings.empty()) { emit_class_with_strings(n, back); return; }
        p_.code.push_back(i);
        return;
      }
      case Node::Seq:
        if (back) for (size_t k = n->kids.size(); k-- > 0;) emit_node(n->kids[k], true);
        else for (Node* k : n->kids) emit_node(k, false);
        return;
      case Node::Alt: {
        std::vector<size_t> jumps;
        for (size_t k = 0; k < n->kids.size(); ++k) {
          if (k + 1 < n->kids.size()) {
            const size_t split = p_.code.size();
            Inst s;
            s.op = OP_SPLIT;
            p_.code.push_back(s);
            p_.code[split].a = static_cast<int32_t>(p_.code.size());
            emit_node(n->kids[k], back);
            jumps.push_back(p_.code.size());
            Inst j;
            j.op = OP_JMP;
            p_.code.push_back(j);
            p_.code[split].b = static_cast<int32_t>(p_.code.size());
          } else {
            emit_node(n->kids[k], back);
          }
        }
        for (size_t j : jumps) p_.code[j].a = static_cast<int32_t>(p_.code.size());
        return;
      }
      case Node::Group:
        if (n->cap > 0) {
          emit_save(back ? 2 * n->cap + 1 : 2 * n->cap);
          emit_node(n->child, back);
          emit_save(back ? 2 * n->cap : 2 * n->cap + 1);
        } else {
          emit_node(n->child, back);
        }
        return;
      case Node::Look: {
        const size_t at = p_.code.size();
        Inst l;
        l.op = OP_LOOK;
        l.flag = static_cast<uint8_t>((n->neg ? 1 : 0) | (n->behind ? 2 : 0));
        p_.code.push_back(l);
        emit_node(n->child, n->behind);
        Inst e;
        e.op = OP_LOOK_END;
        p_.code.push_back(e);
        p_.code[at].a = static_cast<int32_t>(p_.code.size());
        return;
      }
      case Node::BackRef: {
        Inst i;
        i.op = OP_BACKREF;
        i.a = n->cap;
        i.flag = back ? 1 : 0;
        p_.code.push_back(i);
        return;
      }
      case Node::AssertStart: p_.code.push_back(simple(OP_LINE_START)); return;
      case Node::AssertEnd: p_.code.push_back(simple(OP_LINE_END)); return;
      case Node::WordB: p_.code.push_back(simple(OP_WORDB)); return;
      case Node::NotWordB: p_.code.push_back(simple(OP_NOT_WORDB)); return;
      case Node::Quant: emit_quant(n, back); return;
      case Node::NamedBackRef: return;
    }
  }

 private:
  Program& p_;

  static Inst simple(Op op) {
    Inst i;
    i.op = op;
    return i;
  }

  void emit_save(int slot) {
    Inst i;
    i.op = OP_SAVE;
    i.a = slot;
    p_.code.push_back(i);
  }

  // [class with strings]: the strings (longest first) are alternatives tried before the single characters.
  void emit_class_with_strings(Node* n, bool back) {
    const CharClass& cc = p_.classes[static_cast<size_t>(n->cls)];
    std::vector<std::vector<uint32_t>> strs = cc.strings;
    std::stable_sort(strs.begin(), strs.end(), [](const auto& a, const auto& b) { return a.size() > b.size(); });
    std::vector<size_t> jumps;
    for (const auto& str : strs) {
      const size_t split = p_.code.size();
      p_.code.push_back(simple(OP_SPLIT));
      p_.code[split].a = static_cast<int32_t>(p_.code.size());
      if (back) for (size_t k = str.size(); k-- > 0;) emit_char(str[k], true);
      else for (uint32_t c : str) emit_char(c, false);
      jumps.push_back(p_.code.size());
      p_.code.push_back(simple(OP_JMP));
      p_.code[split].b = static_cast<int32_t>(p_.code.size());
    }
    Inst i;
    i.op = OP_CLASS;
    i.a = n->cls;
    i.flag = back ? 1 : 0;
    p_.code.push_back(i);
    for (size_t j : jumps) p_.code[j].a = static_cast<int32_t>(p_.code.size());
  }

  void emit_char(uint32_t cp, bool back) {
    Inst i;
    i.op = OP_CHAR;
    i.a = static_cast<int32_t>(cp);
    i.b = static_cast<int32_t>(p_.icase ? canonicalize(cp, p_.unicode || p_.unicode_sets) : cp);
    i.flag = back ? 1 : 0;
    p_.code.push_back(i);
  }

  static bool single_atom(const Node* n) { return n->kind == Node::Char || n->kind == Node::Any || (n->kind == Node::Class); }

  void emit_quant(Node* n, bool back) {
    Node* child = n->child;
    if (n->max == 0) return;
    if (!back && single_atom(child) && !(child->kind == Node::Class && !p_.classes[static_cast<size_t>(child->cls)].strings.empty())) {
      Inst run;
      run.op = OP_RUN;
      run.b = n->min;
      run.c = n->max;
      run.flag = n->greedy ? 1 : 0;
      p_.code.push_back(run);
      emit_node(child, false);   // the atom template
      return;
    }
    const int counter = p_.ncounters++;
    const int posreg = p_.nposregs++;
    Inst init;
    init.op = OP_COUNTER_INIT;
    init.a = counter;
    p_.code.push_back(init);
    const size_t loop = p_.code.size();
    Inst l;
    l.op = OP_COUNTER_LOOP;
    l.a = counter;
    l.b = n->min;
    l.c = n->max;
    l.flag = n->greedy ? 1 : 0;
    p_.code.push_back(l);
    if (n->cap_hi > n->cap_lo) {
      Inst r;
      r.op = OP_RESET_CAPS;
      r.a = 2 * n->cap_lo;
      r.b = 2 * n->cap_hi;
      p_.code.push_back(r);
    }
    Inst ps;
    ps.op = OP_POS_SAVE;
    ps.a = posreg;
    p_.code.push_back(ps);
    emit_node(child, back);
    Inst pc;
    pc.op = OP_PROGRESS;
    pc.a = posreg;
    pc.b = counter;
    pc.c = n->min;
    p_.code.push_back(pc);
    Inst inc;
    inc.op = OP_COUNTER_INC;
    inc.a = counter;
    p_.code.push_back(inc);
    Inst j;
    j.op = OP_JMP;
    j.a = static_cast<int32_t>(loop);
    p_.code.push_back(j);
    p_.code[loop].d = static_cast<int32_t>(p_.code.size());
  }
};

// The code unit every match must begin with, or -1.
int first_unit(const Node* n, bool icase) {
  if (icase) return -1;
  switch (n->kind) {
    case Node::Char: return n->ch < 0xD800 || (n->ch >= 0xE000 && n->ch < 0x10000) ? static_cast<int>(n->ch) : -1;
    case Node::Seq:
      for (const Node* k : n->kids) {
        if (k->kind == Node::AssertStart || k->kind == Node::AssertEnd || k->kind == Node::WordB || k->kind == Node::NotWordB || k->kind == Node::Look) return -1;
        return first_unit(k, icase);
      }
      return -1;
    case Node::Group: return first_unit(n->child, icase);
    case Node::Quant: return n->min > 0 ? first_unit(n->child, icase) : -1;
    default: return -1;
  }
}

bool starts_with_caret(const Node* n) {
  if (n->kind == Node::AssertStart) return true;
  if (n->kind == Node::Seq) return !n->kids.empty() && starts_with_caret(n->kids[0]);
  if (n->kind == Node::Group) return starts_with_caret(n->child);
  return false;
}

}  // namespace

std::unique_ptr<Program> compile(const std::u16string& pattern, const std::u16string& flags, std::string* error) {
  auto prog = std::make_unique<Program>();
  prog->source = pattern;
  prog->flags = flags;
  for (char16_t f : flags) {
    bool* flag = nullptr;
    switch (f) {
      case u'g': flag = &prog->global; break;
      case u'i': flag = &prog->icase; break;
      case u'm': flag = &prog->multiline; break;
      case u's': flag = &prog->dotall; break;
      case u'u': flag = &prog->unicode; break;
      case u'v': flag = &prog->unicode_sets; break;
      case u'y': flag = &prog->sticky; break;
      case u'd': flag = &prog->has_indices; break;
      default: *error = "Invalid flags"; return nullptr;
    }
    if (*flag) { *error = "Invalid flags"; return nullptr; }
    *flag = true;
  }
  if (prog->unicode && prog->unicode_sets) { *error = "Invalid flags"; return nullptr; }
  Parser parser(pattern, prog->unicode || prog->unicode_sets, prog->unicode_sets, *prog);
  Node* root = parser.parse(error);
  if (!root) return nullptr;
  prog->ncaps = parser.total_caps();
  prog->group_names = parser.names_;
  prog->group_names.resize(static_cast<size_t>(prog->ncaps));
  Emitter em(*prog);
  em.emit_node(root, false);
  Inst m;
  m.op = OP_MATCH;
  prog->code.push_back(m);
  prog->first_char = first_unit(root, prog->icase);
  prog->anchored_start = !prog->multiline && starts_with_caret(root);
  return prog;
}

// ---------------------------------------------------------------------------------------------------
// the matcher
// ---------------------------------------------------------------------------------------------------

namespace {

enum BtKind : int { BT_CHOICE, BT_CAP, BT_CNT, BT_POS, BT_RUN_GREEDY, BT_RUN_LAZY };

struct Bt {
  int kind;
  int a, b, c;
};

class Matcher {
 public:
  Matcher(const Program& p, const std::u16string& s) : p_(p), in_(s.data()), len_(static_cast<int>(s.size())), unicode_(p.unicode || p.unicode_sets) {
    caps_.assign(static_cast<size_t>(p.ncaps) * 2, -1);
    counters_.assign(static_cast<size_t>(p.ncounters), 0);
    posregs_.assign(static_cast<size_t>(p.nposregs), -1);
    uic_ = p.icase && unicode_;
  }

  std::vector<int> caps_;
  bool too_complex_ = false;

  bool run_at(int start) {
    std::fill(caps_.begin(), caps_.end(), -1);
    stack_.clear();
    int end;
    if (!run(0, start, &end)) return false;
    caps_[0] = start;
    caps_[1] = end;
    return true;
  }

 private:
  const Program& p_;
  const char16_t* in_;
  int len_;
  bool unicode_;
  bool uic_ = false;
  std::vector<int> counters_, posregs_;
  std::vector<Bt> stack_;

  // ---- reading characters ----

  // The code point at pos going forward; false at the end.
  bool read_fwd(int pos, uint32_t* cp, int* next) const {
    if (pos >= len_) return false;
    char16_t c = in_[pos];
    if (unicode_ && uni::is_high_surrogate(c) && pos + 1 < len_ && uni::is_low_surrogate(in_[pos + 1])) {
      *cp = 0x10000 + ((static_cast<uint32_t>(c) - 0xD800) << 10) + (in_[pos + 1] - 0xDC00);
      *next = pos + 2;
    } else {
      *cp = c;
      *next = pos + 1;
    }
    return true;
  }

  bool read_back(int pos, uint32_t* cp, int* next) const {
    if (pos <= 0) return false;
    char16_t c = in_[pos - 1];
    if (unicode_ && uni::is_low_surrogate(c) && pos - 2 >= 0 && uni::is_high_surrogate(in_[pos - 2])) {
      *cp = 0x10000 + ((static_cast<uint32_t>(in_[pos - 2]) - 0xD800) << 10) + (c - 0xDC00);
      *next = pos - 2;
    } else {
      *cp = c;
      *next = pos - 1;
    }
    return true;
  }

  bool is_word_at(int pos) const {
    if (pos < 0 || pos >= len_) return false;
    char16_t c = in_[pos];
    if ((c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || (c >= u'0' && c <= u'9') || c == u'_') return true;
    return uic_ && (c == 0x17F || c == 0x212A);
  }

  bool class_match(const CharClass& cc, uint32_t c) const {
    bool in = class_has(cc, c, uic_);
    if (!in && p_.icase) {
      const uint32_t alts[3] = {uni::simple_lower(c), uni::simple_upper(c), canonicalize(c, unicode_)};
      for (uint32_t a : alts)
        if (a != c && class_has(cc, a, uic_)) { in = true; break; }
    }
    return in != cc.negate;
  }

  // One atom (CHAR / ANY / CLASS) at `pos` in the instruction's direction: the new position, or -1.
  int atom_step(const Inst& i, int pos) const {
    uint32_t c;
    int next;
    const bool back = (i.flag & 1) != 0;
    if (!(back ? read_back(pos, &c, &next) : read_fwd(pos, &c, &next))) return -1;
    switch (i.op) {
      case OP_CHAR:
        if (p_.icase) { if (canonicalize(c, unicode_) != static_cast<uint32_t>(i.b)) return -1; }
        else if (c != static_cast<uint32_t>(i.a)) return -1;
        return next;
      case OP_ANY:
        if (!p_.dotall && is_line_terminator(c)) return -1;
        return next;
      default:  // OP_CLASS
        return class_match(p_.classes[static_cast<size_t>(i.a)], c) ? next : -1;
    }
  }

  void push(int kind, int a, int b, int c) {
    stack_.push_back({kind, a, b, c});
    if (stack_.size() > 50'000'000) too_complex_ = true;
  }

  void unwind(size_t base) {
    while (stack_.size() > base) {
      Bt e = stack_.back();
      stack_.pop_back();
      switch (e.kind) {
        case BT_CAP: caps_[static_cast<size_t>(e.a)] = e.b; break;
        case BT_CNT: counters_[static_cast<size_t>(e.a)] = e.b; break;
        case BT_POS: posregs_[static_cast<size_t>(e.a)] = e.b; break;
        default: break;
      }
    }
  }

  void set_cap(int slot, int v) {
    push(BT_CAP, slot, caps_[static_cast<size_t>(slot)], 0);
    caps_[static_cast<size_t>(slot)] = v;
  }

  // Run from `pc` at `pos` until OP_MATCH / OP_LOOK_END; on success `*end` is the final position and the stack above
  // the entry level holds whatever alternatives remain. On failure the stack is back at its entry level.
  bool run(int pc, int pos, int* end) {
    const size_t base = stack_.size();
    uint64_t steps = 0;
    for (;;) {
      if (too_complex_) { unwind(base); return false; }
      if (++steps > 400'000'000ull) { too_complex_ = true; unwind(base); return false; }
      const Inst& i = p_.code[static_cast<size_t>(pc)];
      switch (i.op) {
        case OP_CHAR: case OP_ANY: case OP_CLASS: {
          int np = atom_step(i, pos);
          if (np < 0) goto fail;
          pos = np;
          ++pc;
          continue;
        }
        case OP_SPLIT:
          push(BT_CHOICE, i.b, pos, 0);
          pc = i.a;
          continue;
        case OP_JMP: pc = i.a; continue;
        case OP_SAVE: set_cap(i.a, pos); ++pc; continue;
        case OP_RESET_CAPS:
          for (int s = i.a; s < i.b; ++s)
            if (caps_[static_cast<size_t>(s)] != -1) set_cap(s, -1);
          ++pc;
          continue;
        case OP_COUNTER_INIT:
          push(BT_CNT, i.a, counters_[static_cast<size_t>(i.a)], 0);
          counters_[static_cast<size_t>(i.a)] = 0;
          ++pc;
          continue;
        case OP_COUNTER_LOOP: {
          const int cnt = counters_[static_cast<size_t>(i.a)];
          if (cnt < i.b) { ++pc; continue; }
          if (i.c >= 0 && cnt >= i.c) { pc = i.d; continue; }
          if (i.flag & 1) { push(BT_CHOICE, i.d, pos, 0); ++pc; }
          else { push(BT_CHOICE, pc + 1, pos, 0); pc = i.d; }
          continue;
        }
        case OP_COUNTER_INC:
          push(BT_CNT, i.a, counters_[static_cast<size_t>(i.a)], 0);
          ++counters_[static_cast<size_t>(i.a)];
          ++pc;
          continue;
        case OP_POS_SAVE:
          push(BT_POS, i.a, posregs_[static_cast<size_t>(i.a)], 0);
          posregs_[static_cast<size_t>(i.a)] = pos;
          ++pc;
          continue;
        case OP_PROGRESS:
          if (pos == posregs_[static_cast<size_t>(i.a)] && counters_[static_cast<size_t>(i.b)] >= i.c) goto fail;
          ++pc;
          continue;
        case OP_LINE_START:
          if (pos == 0 || (p_.multiline && is_line_terminator(in_[pos - 1]))) { ++pc; continue; }
          goto fail;
        case OP_LINE_END:
          if (pos == len_ || (p_.multiline && is_line_terminator(in_[pos]))) { ++pc; continue; }
          goto fail;
        case OP_WORDB:
          if (is_word_at(pos - 1) != is_word_at(pos)) { ++pc; continue; }
          goto fail;
        case OP_NOT_WORDB:
          if (is_word_at(pos - 1) == is_word_at(pos)) { ++pc; continue; }
          goto fail;
        case OP_BACKREF: {
          const int s = caps_[static_cast<size_t>(i.a) * 2], e = caps_[static_cast<size_t>(i.a) * 2 + 1];
          if (s < 0 || e < 0) { ++pc; continue; }
          const int n = e - s;
          const bool back = (i.flag & 1) != 0;
          const int from = back ? pos - n : pos;
          if (from < 0 || from + n > len_) goto fail;
          for (int k = 0; k < n; ++k) {
            uint32_t a = in_[s + k], b = in_[from + k];
            if (a != b && !(p_.icase && canonicalize(a, unicode_) == canonicalize(b, unicode_))) goto fail;
          }
          pos = back ? from : pos + n;
          ++pc;
          continue;
        }
        case OP_LOOK: {
          const bool neg = (i.flag & 1) != 0;
          const size_t mark = stack_.size();
          int sub_end;
          const bool ok = run(pc + 1, pos, &sub_end);
          if (too_complex_) { unwind(base); return false; }
          if (neg) {
            if (ok) { unwind(mark); goto fail; }
            pc = i.a;
            continue;
          }
          if (!ok) goto fail;
          // keep the capture/counter restores, drop the alternatives inside the lookaround
          size_t w = mark;
          for (size_t r = mark; r < stack_.size(); ++r)
            if (stack_[r].kind == BT_CAP || stack_[r].kind == BT_CNT || stack_[r].kind == BT_POS) stack_[w++] = stack_[r];
          stack_.resize(w);
          pc = i.a;
          continue;
        }
        case OP_LOOK_END: *end = pos; return true;
        case OP_MATCH: *end = pos; return true;
        case OP_RUN: {
          const Inst& atom = p_.code[static_cast<size_t>(pc) + 1];
          const int next_pc = pc + 2;
          int cur = pos, count = 0;
          // the mandatory repetitions
          while (count < i.b) {
            int np = atom_step(atom, cur);
            if (np < 0) goto fail;
            cur = np;
            ++count;
          }
          if (i.flag & 1) {
            const int min_pos = cur;
            while (i.c < 0 || count < i.c) {
              int np = atom_step(atom, cur);
              if (np < 0) break;
              cur = np;
              ++count;
            }
            if (cur > min_pos) push(BT_RUN_GREEDY, next_pc, cur, min_pos);
            pos = cur;
          } else {
            if (i.c < 0 || count < i.c) push(BT_RUN_LAZY, pc, cur, count);
            pos = cur;
          }
          pc = next_pc;
          continue;
        }
        default: goto fail;
      }
    fail:
      for (;;) {
        if (stack_.size() <= base) return false;
        Bt e = stack_.back();
        stack_.pop_back();
        switch (e.kind) {
          case BT_CAP: caps_[static_cast<size_t>(e.a)] = e.b; continue;
          case BT_CNT: counters_[static_cast<size_t>(e.a)] = e.b; continue;
          case BT_POS: posregs_[static_cast<size_t>(e.a)] = e.b; continue;
          case BT_CHOICE: pc = e.a; pos = e.b; break;
          case BT_RUN_GREEDY: {
            // give back one character
            int np = e.b - 1;
            if (unicode_ && np - 1 >= e.c && uni::is_low_surrogate(in_[np]) && uni::is_high_surrogate(in_[np - 1])) --np;
            if (np > e.c) push(BT_RUN_GREEDY, e.a, np, e.c);
            pc = e.a;
            pos = np;
            break;
          }
          case BT_RUN_LAZY: {
            const Inst& run_inst = p_.code[static_cast<size_t>(e.a)];
            const Inst& atom = p_.code[static_cast<size_t>(e.a) + 1];
            int np = atom_step(atom, e.b);
            if (np < 0) continue;
            const int count = e.c + 1;
            if (run_inst.c < 0 || count < run_inst.c) push(BT_RUN_LAZY, e.a, np, count);
            pc = e.a + 2;
            pos = np;
            break;
          }
          default: continue;
        }
        break;
      }
    }
  }
};

}  // namespace

Result exec(const Program& p, const std::u16string& input, size_t start, bool sticky, std::vector<int>* caps) {
  const int len = static_cast<int>(input.size());
  if (start > input.size()) return Result::NoMatch;
  Matcher m(p, input);
  const bool unicode = p.unicode || p.unicode_sets;
  for (int i = static_cast<int>(start); i <= len;) {
    if (!sticky && p.first_char >= 0) {
      size_t f = input.find(static_cast<char16_t>(p.first_char), static_cast<size_t>(i));
      if (f == std::u16string::npos) return Result::NoMatch;
      i = static_cast<int>(f);
    }
    if (m.run_at(i)) {
      *caps = m.caps_;
      return Result::Match;
    }
    if (m.too_complex_) return Result::TooComplex;
    if (sticky) return Result::NoMatch;
    if (p.anchored_start) return Result::NoMatch;
    if (unicode && i < len && uni::is_high_surrogate(input[static_cast<size_t>(i)]) && i + 1 < len && uni::is_low_surrogate(input[static_cast<size_t>(i) + 1])) i += 2;
    else ++i;
  }
  return Result::NoMatch;
}

}  // namespace aios::js::re
