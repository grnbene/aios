// See include/aios/core/html_entities.hpp.
#include "aios/core/html_entities.hpp"

namespace aios {

namespace {

struct NamedEntity {
  const char* name;
  uint32_t cp;
};

constexpr NamedEntity kEntities[] = {
    // the markup characters
    {"amp", '&'},      {"lt", '<'},        {"gt", '>'},        {"quot", '"'},     {"apos", '\''},
    // Latin-1 Supplement, U+00A0..U+00FF, the HTML 4 names in code point order
    {"nbsp", 0x00A0},  {"iexcl", 0x00A1},  {"cent", 0x00A2},   {"pound", 0x00A3}, {"curren", 0x00A4},
    {"yen", 0x00A5},   {"brvbar", 0x00A6}, {"sect", 0x00A7},   {"uml", 0x00A8},   {"copy", 0x00A9},
    {"ordf", 0x00AA},  {"laquo", 0x00AB},  {"not", 0x00AC},    {"shy", 0x00AD},   {"reg", 0x00AE},
    {"macr", 0x00AF},  {"deg", 0x00B0},    {"plusmn", 0x00B1}, {"sup2", 0x00B2},  {"sup3", 0x00B3},
    {"acute", 0x00B4}, {"micro", 0x00B5},  {"para", 0x00B6},   {"middot", 0x00B7},{"cedil", 0x00B8},
    {"sup1", 0x00B9},  {"ordm", 0x00BA},   {"raquo", 0x00BB},  {"frac14", 0x00BC},{"frac12", 0x00BD},
    {"frac34", 0x00BE},{"iquest", 0x00BF}, {"Agrave", 0x00C0}, {"Aacute", 0x00C1},{"Acirc", 0x00C2},
    {"Atilde", 0x00C3},{"Auml", 0x00C4},   {"Aring", 0x00C5},  {"AElig", 0x00C6}, {"Ccedil", 0x00C7},
    {"Egrave", 0x00C8},{"Eacute", 0x00C9}, {"Ecirc", 0x00CA},  {"Euml", 0x00CB},  {"Igrave", 0x00CC},
    {"Iacute", 0x00CD},{"Icirc", 0x00CE},  {"Iuml", 0x00CF},   {"ETH", 0x00D0},   {"Ntilde", 0x00D1},
    {"Ograve", 0x00D2},{"Oacute", 0x00D3}, {"Ocirc", 0x00D4},  {"Otilde", 0x00D5},{"Ouml", 0x00D6},
    {"times", 0x00D7}, {"Oslash", 0x00D8}, {"Ugrave", 0x00D9}, {"Uacute", 0x00DA},{"Ucirc", 0x00DB},
    {"Uuml", 0x00DC},  {"Yacute", 0x00DD}, {"THORN", 0x00DE},  {"szlig", 0x00DF}, {"agrave", 0x00E0},
    {"aacute", 0x00E1},{"acirc", 0x00E2},  {"atilde", 0x00E3}, {"auml", 0x00E4},  {"aring", 0x00E5},
    {"aelig", 0x00E6}, {"ccedil", 0x00E7}, {"egrave", 0x00E8}, {"eacute", 0x00E9},{"ecirc", 0x00EA},
    {"euml", 0x00EB},  {"igrave", 0x00EC}, {"iacute", 0x00ED}, {"icirc", 0x00EE}, {"iuml", 0x00EF},
    {"eth", 0x00F0},   {"ntilde", 0x00F1}, {"ograve", 0x00F2}, {"oacute", 0x00F3},{"ocirc", 0x00F4},
    {"otilde", 0x00F5},{"ouml", 0x00F6},   {"divide", 0x00F7}, {"oslash", 0x00F8},{"ugrave", 0x00F9},
    {"uacute", 0x00FA},{"ucirc", 0x00FB},  {"uuml", 0x00FC},   {"yacute", 0x00FD},{"thorn", 0x00FE},
    {"yuml", 0x00FF},
    // Latin Extended-A and the letters HTML 4 names there
    {"OElig", 0x0152}, {"oelig", 0x0153},  {"Scaron", 0x0160}, {"scaron", 0x0161},{"Yuml", 0x0178},
    {"fnof", 0x0192},  {"circ", 0x02C6},   {"tilde", 0x02DC},
    // typography
    {"ensp", 0x2002},  {"emsp", 0x2003},   {"thinsp", 0x2009}, {"zwnj", 0x200C},  {"zwj", 0x200D},
    {"lrm", 0x200E},   {"rlm", 0x200F},    {"ndash", 0x2013},  {"mdash", 0x2014}, {"lsquo", 0x2018},
    {"rsquo", 0x2019}, {"sbquo", 0x201A},  {"ldquo", 0x201C},  {"rdquo", 0x201D}, {"bdquo", 0x201E},
    {"dagger", 0x2020},{"Dagger", 0x2021}, {"bull", 0x2022},   {"hellip", 0x2026},{"permil", 0x2030},
    {"prime", 0x2032}, {"Prime", 0x2033},  {"lsaquo", 0x2039}, {"rsaquo", 0x203A},{"oline", 0x203E},
    {"euro", 0x20AC},  {"trade", 0x2122},
    // arrows, mathematics, symbols
    {"larr", 0x2190},  {"uarr", 0x2191},   {"rarr", 0x2192},   {"darr", 0x2193},  {"harr", 0x2194},
    {"minus", 0x2212}, {"le", 0x2264},     {"ge", 0x2265},     {"ne", 0x2260},    {"infin", 0x221E},
    {"check", 0x2713}, {"cross", 0x2717},  {"star", 0x2605},   {"hearts", 0x2665},{"spades", 0x2660},
    {"clubs", 0x2663}, {"diams", 0x2666},
    // Greek letters that turn up in prose
    {"alpha", 0x03B1}, {"beta", 0x03B2},   {"gamma", 0x03B3},  {"delta", 0x03B4}, {"pi", 0x03C0},
    {"mu", 0x03BC},    {"sigma", 0x03C3},  {"omega", 0x03C9},  {"lambda", 0x03BB},{"Omega", 0x03A9},
};

}  // namespace

bool html_named_entity(std::string_view name, uint32_t& cp) {
  for (const auto& e : kEntities) {
    if (name == e.name) { cp = e.cp; return true; }
  }
  return false;
}

}  // namespace aios
