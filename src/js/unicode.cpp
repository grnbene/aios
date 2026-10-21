// Case mapping, normalisation and coarse character classes. See unicode.hpp for what is covered.
#include "unicode.hpp"

#include <algorithm>
#include <map>
#include <unordered_map>
#include <vector>

namespace aios::js::uni {

namespace {

struct Range { uint32_t lo, hi; int32_t delta; };   // upper-case range and the offset to its lower-case form
struct Span { uint32_t lo, hi; };

const Range kUpperRanges[] = {
    {0x41, 0x5A, 32}, {0xC0, 0xD6, 32}, {0xD8, 0xDE, 32}, {0x388, 0x38A, 37}, {0x38E, 0x38F, 63}, {0x391, 0x3A1, 32},
    {0x3A3, 0x3AB, 32}, {0x410, 0x42F, 32}, {0x400, 0x40F, 80}, {0x531, 0x556, 48}, {0x10A0, 0x10C5, 7264},
    {0x1F08, 0x1F0F, -8}, {0x1F18, 0x1F1D, -8}, {0x1F28, 0x1F2F, -8}, {0x1F38, 0x1F3F, -8}, {0x1F48, 0x1F4D, -8}, {0x1F68, 0x1F6F, -8},
    {0x2160, 0x216F, 16}, {0x24B6, 0x24CF, 26}, {0x2C00, 0x2C2F, 48}, {0xFF21, 0xFF3A, 32}, {0x10400, 0x10427, 40}, {0x118A0, 0x118BF, 32},
    {0x1E900, 0x1E921, 34}};

// Pairs where the even code point is upper case and the next one lower case.
const Span kEvenUpper[] = {{0x100, 0x12F}, {0x132, 0x137}, {0x14A, 0x177}, {0x182, 0x185}, {0x1A0, 0x1A5}, {0x1DE, 0x1EF}, {0x1F8, 0x21F},
                           {0x222, 0x233}, {0x246, 0x24F}, {0x370, 0x373}, {0x3D8, 0x3EF}, {0x460, 0x481}, {0x48A, 0x4BF}, {0x4D0, 0x52F},
                           {0x1E00, 0x1E95}, {0x1EA0, 0x1EFF}, {0x2C80, 0x2CE3}, {0xA640, 0xA66D}, {0xA680, 0xA69B}, {0xA722, 0xA72F},
                           {0xA732, 0xA76F}, {0x10C80, 0x10CB2}};
// Pairs where the odd code point is upper case.
const Span kOddUpper[] = {{0x139, 0x148}, {0x179, 0x17E}, {0x1B3, 0x1B6}, {0x1CD, 0x1DC}, {0x4C1, 0x4CE}};

struct Pair { uint32_t upper, lower; };
const Pair kSpecial[] = {
    {0x178, 0xFF}, {0x1C4, 0x1C6}, {0x1C5, 0x1C6}, {0x1C7, 0x1C9}, {0x1C8, 0x1C9}, {0x1CA, 0x1CC}, {0x1CB, 0x1CC}, {0x1F1, 0x1F3},
    {0x1F2, 0x1F3}, {0x1F4, 0x1F5}, {0x181, 0x253}, {0x186, 0x254}, {0x189, 0x256}, {0x18A, 0x257}, {0x18E, 0x1DD}, {0x18F, 0x259},
    {0x190, 0x25B}, {0x193, 0x260}, {0x194, 0x263}, {0x196, 0x269}, {0x197, 0x268}, {0x19C, 0x26F}, {0x19D, 0x272}, {0x19F, 0x275},
    {0x1A9, 0x283}, {0x1AE, 0x288}, {0x1B1, 0x28A}, {0x1B2, 0x28B}, {0x1B7, 0x292}, {0x1E9E, 0xDF}, {0x3F4, 0x3B8}, {0x3F7, 0x3F8},
    {0x3F9, 0x3F2}, {0x3FA, 0x3FB}, {0x3FD, 0x37B}, {0x3FE, 0x37C}, {0x3FF, 0x37D}, {0x37F, 0x3F3}, {0x3CF, 0x3D7}, {0x386, 0x3AC},
    {0x38C, 0x3CC}, {0x4C0, 0x4CF}, {0x130, 0x69}, {0x1A7, 0x1A8}, {0x1AC, 0x1AD}, {0x1B8, 0x1B9}, {0x1BC, 0x1BD}, {0x1F6, 0x195},
    {0x1F7, 0x1BF}, {0x220, 0x19E}, {0x23A, 0x2C65}, {0x23B, 0x23C}, {0x23D, 0x19A}, {0x23E, 0x2C66}, {0x241, 0x242}, {0x243, 0x180},
    {0x244, 0x289}, {0x245, 0x28C}, {0x376, 0x377}, {0x3D8, 0x3D9}, {0x1E9E, 0xDF}};
// Lower-case characters whose upper case is not the inverse of the rules above.
const Pair kLowerOnly[] = {{0x39C, 0xB5}, {0x178, 0xFF}, {0x49, 0x131}, {0x53, 0x17F}, {0x3A3, 0x3C2}, {0x392, 0x3D0}, {0x398, 0x3D1},
                           {0x3A6, 0x3D5}, {0x3A0, 0x3D6}, {0x39A, 0x3F0}, {0x3A1, 0x3F1}, {0x395, 0x3F5}, {0x399, 0x345}, {0x399, 0x1FBE}};

bool in_spans(const Span* spans, size_t n, uint32_t cp, uint32_t* lo = nullptr) {
  for (size_t i = 0; i < n; ++i)
    if (cp >= spans[i].lo && cp <= spans[i].hi) { if (lo) *lo = spans[i].lo; return true; }
  return false;
}

}  // namespace

uint32_t simple_lower(uint32_t cp) {
  if (cp < 0x80) return cp >= 'A' && cp <= 'Z' ? cp + 32 : cp;
  for (const Pair& p : kSpecial) if (p.upper == cp) return p.lower;
  for (const Range& r : kUpperRanges) if (cp >= r.lo && cp <= r.hi && r.delta != 0) return static_cast<uint32_t>(static_cast<int32_t>(cp) + r.delta);
  for (const Span& s : kEvenUpper) if (cp >= s.lo && cp <= s.hi && ((cp - s.lo) & 1) == 0) return cp + 1;
  for (const Span& s : kOddUpper) if (cp >= s.lo && cp <= s.hi && ((cp - s.lo) & 1) == 0) return cp + 1;
  return cp;
}

uint32_t simple_upper(uint32_t cp) {
  if (cp < 0x80) return cp >= 'a' && cp <= 'z' ? cp - 32 : cp;
  for (const Pair& p : kLowerOnly) if (p.lower == cp) return p.upper;
  for (const Pair& p : kSpecial) if (p.lower == cp && p.upper != 0x130 && p.upper != 0x1C5 && p.upper != 0x1C8 && p.upper != 0x1CB && p.upper != 0x1F2) return p.upper;
  if (cp == 0x1C6 || cp == 0x1C5) return 0x1C4;
  if (cp == 0x1C9 || cp == 0x1C8) return 0x1C7;
  if (cp == 0x1CC || cp == 0x1CB) return 0x1CA;
  if (cp == 0x1F3 || cp == 0x1F2) return 0x1F1;
  for (const Range& r : kUpperRanges) {
    if (r.delta == 0) continue;
    const uint32_t lo = static_cast<uint32_t>(static_cast<int32_t>(r.lo) + r.delta), hi = static_cast<uint32_t>(static_cast<int32_t>(r.hi) + r.delta);
    if (cp >= lo && cp <= hi) return static_cast<uint32_t>(static_cast<int32_t>(cp) - r.delta);
  }
  for (const Span& s : kEvenUpper) if (cp > s.lo && cp <= s.hi + 1 && ((cp - s.lo) & 1) == 1) return cp - 1;
  for (const Span& s : kOddUpper) if (cp > s.lo && cp <= s.hi + 1 && ((cp - s.lo) & 1) == 1) return cp - 1;
  return cp;
}

uint32_t fold(uint32_t cp) {
  if (cp == 0x130 || cp == 0x131) return cp;
  return simple_lower(simple_upper(cp));
}

bool to_upper(uint32_t cp, uint32_t out[3], int* n) {
  switch (cp) {
    case 0xDF: out[0] = 'S'; out[1] = 'S'; *n = 2; return true;
    case 0x149: out[0] = 0x2BC; out[1] = 'N'; *n = 2; return true;
    case 0x1F0: out[0] = 'J'; out[1] = 0x30C; *n = 2; return true;
    case 0x587: out[0] = 0x535; out[1] = 0x552; *n = 2; return true;
    case 0xFB00: out[0] = 'F'; out[1] = 'F'; *n = 2; return true;
    case 0xFB01: out[0] = 'F'; out[1] = 'I'; *n = 2; return true;
    case 0xFB02: out[0] = 'F'; out[1] = 'L'; *n = 2; return true;
    case 0xFB03: out[0] = 'F'; out[1] = 'F'; out[2] = 'I'; *n = 3; return true;
    case 0xFB04: out[0] = 'F'; out[1] = 'F'; out[2] = 'L'; *n = 3; return true;
    case 0xFB05: case 0xFB06: out[0] = 'S'; out[1] = 'T'; *n = 2; return true;
    default: break;
  }
  uint32_t u = simple_upper(cp);
  out[0] = u;
  *n = 1;
  return u != cp;
}

bool to_lower(uint32_t cp, uint32_t out[3], int* n) {
  if (cp == 0x130) { out[0] = 'i'; out[1] = 0x307; *n = 2; return true; }
  uint32_t l = simple_lower(cp);
  out[0] = l;
  *n = 1;
  return l != cp;
}

uint32_t code_point_at(const std::u16string& s, size_t i, int* len) {
  char16_t c = s[i];
  if (is_high_surrogate(c) && i + 1 < s.size() && is_low_surrogate(s[i + 1])) {
    *len = 2;
    return 0x10000 + ((static_cast<uint32_t>(c) - 0xD800) << 10) + (s[i + 1] - 0xDC00);
  }
  *len = 1;
  return c;
}

void append_code_point(std::u16string& s, uint32_t cp) {
  if (cp >= 0x10000) {
    cp -= 0x10000;
    s.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
    s.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
  } else {
    s.push_back(static_cast<char16_t>(cp));
  }
}

std::u16string to_upper_string(const std::u16string& s) {
  std::u16string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    int len;
    uint32_t cp = code_point_at(s, i, &len);
    i += static_cast<size_t>(len);
    uint32_t m[3];
    int n;
    to_upper(cp, m, &n);
    for (int k = 0; k < n; ++k) append_code_point(out, m[k]);
  }
  return out;
}

std::u16string to_lower_string(const std::u16string& s) {
  std::u16string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    int len;
    uint32_t cp = code_point_at(s, i, &len);
    if (cp == 0x3A3) {
      // final sigma: after a cased letter and not before one
      auto cased = [&](size_t pos, bool forward) {
        for (;;) {
          if (forward ? pos >= s.size() : pos == 0) return false;
          size_t p = forward ? pos : pos - 1;
          int l;
          uint32_t c = code_point_at(s, p, &l);
          Cat k = category(c);
          if (k == Cat::Mn || k == Cat::Cf || c == '\'' || c == 0x2019 || c == '.' || c == 0xAD) { pos = forward ? pos + static_cast<size_t>(l) : p; continue; }
          return k == Cat::Lu || k == Cat::Ll || k == Cat::Lt;
        }
      };
      const bool before = cased(i, false), after = cased(i + 1, true);
      out.push_back(before && !after ? 0x3C2 : 0x3C3);
      i += 1;
      continue;
    }
    i += static_cast<size_t>(len);
    uint32_t m[3];
    int n;
    to_lower(cp, m, &n);
    for (int k = 0; k < n; ++k) append_code_point(out, m[k]);
  }
  return out;
}

// ---------------------------------------------------------------------------------------------------
// normalisation
// ---------------------------------------------------------------------------------------------------

namespace {

struct Compose { uint32_t mark; const char16_t* bases; const char16_t* composed; };
const Compose kCompose[] = {
    {0x300, u"AEIOUaeiouNnWwYy", u"ÀÈÌÒÙàèìòùǸǹẀẁỲỳ"},
    {0x301, u"AEIOUYaeiouyCcLlNnRrSsZzGgKkMmPpWw", u"ÁÉÍÓÚÝáéíóúýĆćĹĺŃńŔŕŚśŹźǴǵḰḱḾḿṔṕẂẃ"},
    {0x302, u"AEIOUaeiouCcGgHhJjSsWwYyZz", u"ÂÊÎÔÛâêîôûĈĉĜĝĤĥĴĵŜŝŴŵŶŷẐẑ"},
    {0x303, u"ANOanoIiUuEeVvYy", u"ÃÑÕãñõĨĩŨũẼẽṼṽỸỹ"},
    {0x304, u"AaEeIiOoUuYyGg", u"ĀāĒēĪīŌōŪūȲȳḠḡ"},
    {0x306, u"AaEeGgIiOoUu", u"ĂăĔĕĞğĬĭŎŏŬŭ"},
    {0x307, u"CcEeGgIZz", u"ĊċĖėĠġİŻż"},
    {0x308, u"AEIOUaeiouyY", u"ÄËÏÖÜäëïöüÿŸ"},
    {0x30A, u"AaUu", u"ÅåŮů"},
    {0x30B, u"OoUu", u"ŐőŰű"},
    {0x30C, u"CcDdEeLlNnRrSsTtZzAaIiOoUu", u"ČčĎďĚěĽľŇňŘřŠšŤťŽžǍǎǏǐǑǒǓǔ"},
    {0x323, u"AaEeIiOoUuYy", u"ẠạẸẹỊịỌọỤụỴỵ"},
    {0x327, u"CcGgKkLlNnRrSsTt", u"ÇçĢģĶķĻļŅņŖŗŞşŢţ"},
    {0x328, u"AaEeIiUu", u"ĄąĘęĮįŲų"},
};

struct Compat { uint32_t cp; const char16_t* to; };
const Compat kCompat[] = {
    {0xA0, u" "}, {0xAA, u"a"}, {0xB2, u"2"}, {0xB3, u"3"}, {0xB5, u"μ"}, {0xB9, u"1"}, {0xBA, u"o"}, {0xBC, u"1⁄4"}, {0xBD, u"1⁄2"}, {0xBE, u"3⁄4"},
    {0x132, u"IJ"}, {0x133, u"ij"}, {0x13F, u"L·"}, {0x140, u"l·"}, {0x149, u"ʼn"}, {0x17F, u"s"}, {0x1C4, u"DŽ"}, {0x1C5, u"Dž"}, {0x1C6, u"dž"},
    {0x1C7, u"LJ"}, {0x1C8, u"Lj"}, {0x1C9, u"lj"}, {0x1CA, u"NJ"}, {0x1CB, u"Nj"}, {0x1CC, u"nj"}, {0x2002, u" "}, {0x2003, u" "}, {0x2009, u" "},
    {0x2024, u"."}, {0x2025, u".."}, {0x2026, u"..."}, {0x2122, u"TM"}, {0x2126, u"Ω"}, {0x212A, u"K"}, {0x212B, u"Å"}, {0x2160, u"I"}, {0x2161, u"II"},
    {0x2162, u"III"}, {0x2163, u"IV"}, {0x2164, u"V"}, {0x2460, u"1"}, {0x2461, u"2"}, {0x2462, u"3"}, {0x3000, u" "}, {0xFB00, u"ff"}, {0xFB01, u"fi"},
    {0xFB02, u"fl"}, {0xFB03, u"ffi"}, {0xFB04, u"ffl"}, {0xFB05, u"st"}, {0xFB06, u"st"}, {0x2070, u"0"}, {0x2074, u"4"}, {0x2075, u"5"}, {0x2076, u"6"},
    {0x2077, u"7"}, {0x2078, u"8"}, {0x2079, u"9"}, {0x2080, u"0"}, {0x2081, u"1"}, {0x2082, u"2"}, {0x2083, u"3"}, {0x2084, u"4"}, {0x2085, u"5"},
    {0x2086, u"6"}, {0x2087, u"7"}, {0x2088, u"8"}, {0x2089, u"9"}};

// Canonical-only singletons.
struct Singleton { uint32_t cp, to; };
const Singleton kSingletons[] = {{0x212A, 'K'}, {0x2126, 0x3A9}, {0x212B, 0xC5}, {0x2000, 0x2002}, {0x2001, 0x2003}};

struct Tables {
  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> decomp;           // composed -> base, mark
  std::map<std::pair<uint32_t, uint32_t>, uint32_t> comp;                       // base, mark -> composed
  std::unordered_map<uint32_t, std::u16string> compat;
  Tables() {
    for (const Compose& c : kCompose) {
      std::u16string b = c.bases, r = c.composed;
      for (size_t i = 0; i < b.size() && i < r.size(); ++i) {
        decomp[r[i]] = {b[i], c.mark};
        comp[{b[i], c.mark}] = r[i];
      }
    }
    for (const Compat& c : kCompat) compat[c.cp] = c.to;
  }
};

const Tables& tables() {
  static Tables t;
  return t;
}

int ccc(uint32_t cp) {
  if (cp < 0x300 || cp > 0x36F) return 0;
  if (cp <= 0x314) return 230;
  if (cp == 0x315) return 232;
  if (cp <= 0x319) return 220;
  if (cp == 0x31A) return 232;
  if (cp == 0x31B) return 216;
  if (cp <= 0x320) return 220;
  if (cp <= 0x322) return 202;
  if (cp <= 0x326) return 220;
  if (cp <= 0x328) return 202;
  if (cp <= 0x333) return 220;
  if (cp <= 0x338) return 1;
  if (cp <= 0x33C) return 220;
  if (cp <= 0x344) return 230;
  if (cp == 0x345) return 240;
  if (cp == 0x346) return 230;
  if (cp <= 0x349) return 220;
  if (cp <= 0x34C) return 230;
  if (cp <= 0x34E) return 220;
  if (cp == 0x34F) return 0;
  if (cp <= 0x352) return 230;
  if (cp <= 0x356) return 220;
  if (cp == 0x357) return 230;
  if (cp == 0x358) return 232;
  if (cp <= 0x35A) return 220;
  if (cp == 0x35B) return 230;
  if (cp == 0x35C) return 233;
  if (cp <= 0x35E) return 234;
  if (cp == 0x35F) return 233;
  if (cp <= 0x361) return 234;
  if (cp == 0x362) return 233;
  return 230;
}

constexpr uint32_t kSBase = 0xAC00, kLBase = 0x1100, kVBase = 0x1161, kTBase = 0x11A7, kLCount = 19, kVCount = 21, kTCount = 28, kNCount = kVCount * kTCount,
                   kSCount = kLCount * kNCount;

void decompose_cp(uint32_t cp, bool compat, std::vector<uint32_t>& out) {
  if (cp >= kSBase && cp < kSBase + kSCount) {
    uint32_t s = cp - kSBase;
    out.push_back(kLBase + s / kNCount);
    out.push_back(kVBase + (s % kNCount) / kTCount);
    if (s % kTCount) out.push_back(kTBase + s % kTCount);
    return;
  }
  for (const Singleton& s : kSingletons) if (s.cp == cp) { decompose_cp(s.to, compat, out); return; }
  const Tables& t = tables();
  auto it = t.decomp.find(cp);
  if (it != t.decomp.end()) {
    decompose_cp(it->second.first, compat, out);
    out.push_back(it->second.second);
    return;
  }
  if (compat) {
    auto c = t.compat.find(cp);
    if (c != t.compat.end()) {
      for (size_t i = 0; i < c->second.size(); ++i) decompose_cp(c->second[i], compat, out);
      return;
    }
    if (cp >= 0xFF01 && cp <= 0xFF5E) { out.push_back(cp - 0xFF01 + 0x21); return; }
  }
  out.push_back(cp);
}

}  // namespace

std::u16string normalize(const std::u16string& s, int form) {
  const bool compat = form >= 2, compose = form == 0 || form == 2;
  std::vector<uint32_t> cps;
  for (size_t i = 0; i < s.size();) {
    int len;
    uint32_t cp = code_point_at(s, i, &len);
    i += static_cast<size_t>(len);
    decompose_cp(cp, compat, cps);
  }
  // canonical ordering of runs of combining marks
  for (size_t i = 0; i < cps.size();) {
    if (ccc(cps[i]) == 0) { ++i; continue; }
    size_t j = i;
    while (j < cps.size() && ccc(cps[j]) != 0) ++j;
    std::stable_sort(cps.begin() + static_cast<long>(i), cps.begin() + static_cast<long>(j), [](uint32_t a, uint32_t b) { return ccc(a) < ccc(b); });
    i = j;
  }
  if (compose) {
    const Tables& t = tables();
    std::vector<uint32_t> out;
    long starter = -1;
    int last_ccc = -1;
    for (uint32_t cp : cps) {
      const int c = ccc(cp);
      if (starter >= 0) {
        uint32_t st = out[static_cast<size_t>(starter)];
        uint32_t composed = 0;
        const bool unblocked = last_ccc < c || last_ccc == 0 || (static_cast<size_t>(starter) == out.size() - 1);
        if (unblocked || last_ccc < c) {
          if (st >= kLBase && st < kLBase + kLCount && cp >= kVBase && cp < kVBase + kVCount && static_cast<size_t>(starter) == out.size() - 1)
            composed = kSBase + ((st - kLBase) * kVCount + (cp - kVBase)) * kTCount;
          else if (st >= kSBase && st < kSBase + kSCount && (st - kSBase) % kTCount == 0 && cp > kTBase && cp < kTBase + kTCount && static_cast<size_t>(starter) == out.size() - 1)
            composed = st + (cp - kTBase);
          else {
            auto it = t.comp.find({st, cp});
            if (it != t.comp.end() && (static_cast<size_t>(starter) == out.size() - 1 || last_ccc < c)) composed = it->second;
          }
        }
        if (composed) {
          out[static_cast<size_t>(starter)] = composed;
          continue;
        }
      }
      if (c == 0) {
        starter = static_cast<long>(out.size());
        last_ccc = -1;
      } else {
        last_ccc = c;
      }
      out.push_back(cp);
      if (c == 0) last_ccc = 0;
    }
    cps = std::move(out);
  }
  std::u16string r;
  for (uint32_t cp : cps) append_code_point(r, cp);
  return r;
}

// ---------------------------------------------------------------------------------------------------
// character classes
// ---------------------------------------------------------------------------------------------------

namespace {

const Span kOtherLetters[] = {
    {0x1BB, 0x1BB}, {0x1C0, 0x1C3}, {0x294, 0x294}, {0x5D0, 0x5EA}, {0x5EF, 0x5F2}, {0x620, 0x63F}, {0x641, 0x64A}, {0x66E, 0x66F}, {0x671, 0x6D3}, {0x6D5, 0x6D5},
    {0x6EE, 0x6EF}, {0x6FA, 0x6FC}, {0x6FF, 0x6FF}, {0x710, 0x710}, {0x712, 0x72F}, {0x74D, 0x7A5}, {0x7B1, 0x7B1}, {0x7CA, 0x7EA}, {0x800, 0x815}, {0x840, 0x858},
    {0x8A0, 0x8C9}, {0x904, 0x939}, {0x93D, 0x93D}, {0x950, 0x950}, {0x958, 0x961}, {0x972, 0x980}, {0x985, 0x9B9}, {0xA05, 0xA39}, {0xA85, 0xAB9}, {0xB05, 0xB39},
    {0xB85, 0xBB9}, {0xC05, 0xC39}, {0xC85, 0xCB9}, {0xD05, 0xD3A}, {0xD85, 0xDC6}, {0xE01, 0xE30}, {0xE32, 0xE33}, {0xE40, 0xE45}, {0xE81, 0xEB0}, {0xEB2, 0xEB3},
    {0xEC0, 0xEC4}, {0xF00, 0xF00}, {0xF40, 0xF6C}, {0x1000, 0x102A}, {0x10D0, 0x10FA}, {0x10FD, 0x10FF}, {0x1100, 0x1248}, {0x124A, 0x135A}, {0x1380, 0x138F},
    {0x1401, 0x166C}, {0x166F, 0x167F}, {0x1681, 0x169A}, {0x16A0, 0x16EA}, {0x1780, 0x17B3}, {0x1820, 0x1842}, {0x1844, 0x1878}, {0x1880, 0x18A8}, {0x1E9C, 0x1E9D},
    {0x2135, 0x2138}, {0x2D30, 0x2D67}, {0x3006, 0x3006}, {0x3041, 0x3096}, {0x309F, 0x309F}, {0x30A1, 0x30FA}, {0x30FF, 0x30FF}, {0x3105, 0x312F}, {0x3131, 0x318E},
    {0x31A0, 0x31BF}, {0x31F0, 0x31FF}, {0x3400, 0x4DBF}, {0x4E00, 0x9FFF}, {0xA000, 0xA48C}, {0xA4D0, 0xA4F7}, {0xA500, 0xA60B}, {0xAC00, 0xD7A3}, {0xF900, 0xFAFF},
    {0xFB1D, 0xFB4F}, {0xFB50, 0xFDFB}, {0xFE70, 0xFEFC}, {0xFF66, 0xFF9D}, {0xFFA0, 0xFFDC}, {0x10000, 0x100FA}, {0x20000, 0x2FA1D}, {0x30000, 0x323AF}};

const Span kDigits[] = {{0x660, 0x669}, {0x6F0, 0x6F9}, {0x7C0, 0x7C9}, {0x966, 0x96F}, {0x9E6, 0x9EF}, {0xA66, 0xA6F}, {0xAE6, 0xAEF}, {0xB66, 0xB6F}, {0xBE6, 0xBEF},
                        {0xC66, 0xC6F}, {0xCE6, 0xCEF}, {0xD66, 0xD6F}, {0xE50, 0xE59}, {0xED0, 0xED9}, {0xF20, 0xF29}, {0x1040, 0x1049}, {0x17E0, 0x17E9}, {0x1810, 0x1819},
                        {0xFF10, 0xFF19}, {0x1D7CE, 0x1D7FF}};

const Span kMarks[] = {{0x300, 0x36F}, {0x483, 0x489}, {0x591, 0x5BD}, {0x5BF, 0x5BF}, {0x5C1, 0x5C2}, {0x5C4, 0x5C5}, {0x5C7, 0x5C7}, {0x610, 0x61A}, {0x64B, 0x65F},
                       {0x670, 0x670}, {0x6D6, 0x6DC}, {0x6DF, 0x6E4}, {0x6E7, 0x6E8}, {0x6EA, 0x6ED}, {0x900, 0x903}, {0x93A, 0x93C}, {0x93E, 0x94F}, {0x951, 0x957},
                       {0x962, 0x963}, {0xE31, 0xE31}, {0xE34, 0xE3A}, {0xE47, 0xE4E}, {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF}, {0x20D0, 0x20FF}, {0x3099, 0x309A},
                       {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F}};

template <size_t N> bool in(const Span (&s)[N], uint32_t cp) { return in_spans(s, N, cp); }

}  // namespace

Cat category(uint32_t cp) {
  if (cp < 0x80) {
    if (cp >= 'A' && cp <= 'Z') return Cat::Lu;
    if (cp >= 'a' && cp <= 'z') return Cat::Ll;
    if (cp >= '0' && cp <= '9') return Cat::Nd;
    if (cp == ' ') return Cat::Zs;
    if (cp < 0x20 || cp == 0x7F) return Cat::Cc;
    switch (cp) {
      case '_': return Cat::Pc;
      case '-': return Cat::Pd;
      case '(': case '[': case '{': return Cat::Ps;
      case ')': case ']': case '}': return Cat::Pe;
      case '+': case '<': case '=': case '>': case '|': case '~': return Cat::Sm;
      case '$': return Cat::Sc;
      case '^': case '`': return Cat::Sk;
      default: return Cat::Po;
    }
  }
  if (cp < 0xC0) {
    switch (cp) {
      case 0xA0: return Cat::Zs;
      case 0xA1: case 0xA7: case 0xB6: case 0xB7: case 0xBF: return Cat::Po;
      case 0xA2: case 0xA3: case 0xA4: case 0xA5: return Cat::Sc;
      case 0xA6: case 0xA9: case 0xAE: case 0xB0: return Cat::So;
      case 0xA8: case 0xAF: case 0xB4: case 0xB8: return Cat::Sk;
      case 0xAA: case 0xBA: return Cat::Lo;
      case 0xAB: return Cat::Pi;
      case 0xBB: return Cat::Pf;
      case 0xAC: case 0xB1: return Cat::Sm;
      case 0xAD: return Cat::Cf;
      case 0xB2: case 0xB3: case 0xB9: case 0xBC: case 0xBD: case 0xBE: return Cat::No;
      case 0xB5: return Cat::Ll;
      default: return Cat::Cc;
    }
  }
  if (cp == 0xD7 || cp == 0xF7) return Cat::Sm;
  if (cp >= 0xD800 && cp <= 0xDFFF) return Cat::Cs;
  if (cp >= 0xE000 && cp <= 0xF8FF) return Cat::Co;
  if (cp >= 0x2000 && cp <= 0x200A) return Cat::Zs;
  if (cp == 0x2028) return Cat::Zl;
  if (cp == 0x2029) return Cat::Zp;
  if (cp == 0x202F || cp == 0x205F || cp == 0x3000 || cp == 0x1680) return Cat::Zs;
  if ((cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2060 && cp <= 0x2064) || cp == 0xFEFF) return Cat::Cf;
  if (cp >= 0x80 && cp <= 0x9F) return Cat::Cc;
  if (cp >= 0x2010 && cp <= 0x2015) return Cat::Pd;
  if (cp == 0x2018 || cp == 0x201C || cp == 0x2039) return Cat::Pi;
  if (cp == 0x2019 || cp == 0x201D || cp == 0x203A) return Cat::Pf;
  if ((cp >= 0x2016 && cp <= 0x2027) || (cp >= 0x2030 && cp <= 0x205E)) return Cat::Po;
  if (cp >= 0x20A0 && cp <= 0x20C0) return Cat::Sc;
  if (cp >= 0x2190 && cp <= 0x21FF) return (cp >= 0x2190 && cp <= 0x2194) ? Cat::Sm : Cat::So;
  if (cp >= 0x2200 && cp <= 0x22FF) return Cat::Sm;
  if (cp >= 0x2300 && cp <= 0x2BFF) return Cat::So;
  if (cp >= 0x2460 && cp <= 0x249B) return Cat::No;
  if (cp >= 0x3001 && cp <= 0x3003) return Cat::Po;
  if (cp >= 0x1F000 && cp <= 0x1FAFF) return Cat::So;
  if (cp >= 0x2150 && cp <= 0x215F) return Cat::No;
  if (cp >= 0x2160 && cp <= 0x2188) return Cat::Nl;
  if (cp >= 0x2070 && cp <= 0x209F) return (cp == 0x2071 || cp == 0x207F || (cp >= 0x2090 && cp <= 0x209C)) ? Cat::Lm : Cat::No;
  if (cp >= 0x2B0 && cp <= 0x2C1) return Cat::Lm;
  if (in(kMarks, cp)) return Cat::Mn;
  if (in(kDigits, cp)) return Cat::Nd;
  if (simple_lower(cp) != cp) return Cat::Lu;
  if (simple_upper(cp) != cp) return Cat::Ll;
  if (cp >= 0xDF && cp <= 0x24F) return Cat::Ll;
  if (cp >= 0x250 && cp <= 0x2AF) return Cat::Ll;
  if (cp >= 0x370 && cp <= 0x3FF) return (cp >= 0x3AC && cp <= 0x3CE) ? Cat::Ll : Cat::Lu;
  if (cp >= 0x400 && cp <= 0x52F) return Cat::Ll;
  if (in(kOtherLetters, cp)) return Cat::Lo;
  if (cp >= 0x1D400 && cp <= 0x1D7CB) return Cat::Lu;
  return Cat::Cn;
}

bool is_id_start(uint32_t cp) {
  Cat c = category(cp);
  return c == Cat::Lu || c == Cat::Ll || c == Cat::Lt || c == Cat::Lm || c == Cat::Lo || c == Cat::Nl;
}

bool is_id_continue(uint32_t cp) {
  if (is_id_start(cp)) return true;
  Cat c = category(cp);
  return c == Cat::Mn || c == Cat::Mc || c == Cat::Nd || c == Cat::Pc || cp == 0x200C || cp == 0x200D;
}

}  // namespace aios::js::uni
