// MIME for mail. See include/aios/net/mime.hpp.
#include "aios/net/mime.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>

#include "aios/core/base64.hpp"
#include "aios/core/bytes.hpp"

namespace aios::net::mime {

namespace {

#include "mime_charsets.inc"

char lc(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

std::string lower(std::string_view s) {
  std::string o(s);
  for (auto& c : o) c = lc(c);
  return o;
}

bool ieq(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (lc(a[i]) != lc(b[i])) return false;
  return true;
}

bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

std::string_view trim(std::string_view s) {
  while (!s.empty() && is_ws(s.front())) s.remove_prefix(1);
  while (!s.empty() && is_ws(s.back())) s.remove_suffix(1);
  return s;
}

bool contains_i(std::string_view hay, std::string_view needle) {
  if (needle.empty()) return true;
  if (hay.size() < needle.size()) return false;
  for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
    size_t j = 0;
    while (j < needle.size() && lc(hay[i + j]) == lc(needle[j])) ++j;
    if (j == needle.size()) return true;
  }
  return false;
}

// Position of `needle` in `hay` at or after `from`, case-insensitively; npos if none.
size_t find_i(std::string_view hay, std::string_view needle, size_t from = 0) {
  if (needle.empty()) return from;
  for (size_t i = from; i + needle.size() <= hay.size(); ++i) {
    size_t j = 0;
    while (j < needle.size() && lc(hay[i + j]) == lc(needle[j])) ++j;
    if (j == needle.size()) return i;
  }
  return std::string_view::npos;
}

void put_utf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) out += static_cast<char>(cp);
  else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

// Valid UTF-8 passes through; each byte that is not part of a valid sequence becomes U+FFFD.
std::string sanitize_utf8(std::string_view in) {
  std::string out;
  out.reserve(in.size());
  size_t i = 0;
  while (i < in.size()) {
    unsigned char c = static_cast<unsigned char>(in[i]);
    if (c < 0x80) { out += static_cast<char>(c); ++i; continue; }
    size_t n = 0;
    uint32_t cp = 0, minv = 0;
    if ((c & 0xE0) == 0xC0) { n = 1; cp = c & 0x1F; minv = 0x80; }
    else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0F; minv = 0x800; }
    else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07; minv = 0x10000; }
    bool ok = n != 0;
    if (ok) {
      for (size_t k = 1; k <= n; ++k) {
        if (i + k >= in.size()) { ok = false; break; }
        unsigned char cc = static_cast<unsigned char>(in[i + k]);
        if ((cc & 0xC0) != 0x80) { ok = false; break; }
        cp = (cp << 6) | (cc & 0x3F);
      }
    }
    if (ok && (cp < minv || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))) ok = false;
    if (ok) { out.append(in.substr(i, n + 1)); i += n + 1; }
    else { put_utf8(out, 0xFFFD); ++i; }
  }
  return out;
}

int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string normalize_charset(std::string_view cs) {
  cs = trim(cs);
  if (!cs.empty() && (cs.front() == '"' || cs.front() == '\'')) cs.remove_prefix(1);
  if (!cs.empty() && (cs.back() == '"' || cs.back() == '\'')) cs.remove_suffix(1);
  size_t star = cs.find('*');   // RFC 2231 language suffix: utf-8*en
  if (star != std::string_view::npos) cs = cs.substr(0, star);
  std::string o = lower(cs);
  for (auto& c : o) if (c == '_') c = '-';
  return o;
}

const uint16_t* table_for(const std::string& cs) {
  if (cs == "windows-1250" || cs == "cp1250") return kCs_CP1250;
  if (cs == "windows-1251" || cs == "cp1251") return kCs_CP1251;
  if (cs == "windows-1252" || cs == "cp1252") return kCs_CP1252;
  if (cs == "koi8-r" || cs == "koi8r") return kCs_KOI8_R;
  if (cs == "iso-8859-2" || cs == "latin2" || cs == "l2") return kCs_ISO8859_2;
  if (cs == "iso-8859-15" || cs == "latin9" || cs == "l9") return kCs_ISO8859_15;
  return nullptr;
}

bool is_utf8_name(const std::string& cs) { return cs == "utf-8" || cs == "utf8" || cs.empty(); }
bool is_ascii_name(const std::string& cs) { return cs == "us-ascii" || cs == "ascii" || cs == "ansi-x3.4-1968"; }
bool is_latin1_name(const std::string& cs) {
  return cs == "iso-8859-1" || cs == "latin1" || cs == "latin-1" || cs == "l1" || cs == "iso8859-1";
}

// One logical line at a time out of a block of text, LF or CRLF.
size_t next_line(std::string_view s, size_t pos, std::string_view& line) {
  if (pos >= s.size()) { line = {}; return std::string_view::npos; }
  size_t e = s.find('\n', pos);
  size_t next;
  if (e == std::string_view::npos) { e = s.size(); next = s.size(); }
  else next = e + 1;
  size_t le = e;
  if (le > pos && s[le - 1] == '\r') --le;
  line = s.substr(pos, le - pos);
  return next;
}

// Header block and body of a part. The body starts after the first empty line.
void split_head_body(std::string_view raw, std::string_view& head, std::string_view& body) {
  size_t pos = 0;
  while (pos <= raw.size()) {
    std::string_view line;
    size_t next = next_line(raw, pos, line);
    if (next == std::string_view::npos) { head = raw; body = {}; return; }
    if (line.empty()) {
      head = raw.substr(0, pos);
      body = raw.substr(next);
      return;
    }
    pos = next;
  }
  head = raw;
  body = {};
}

std::vector<std::pair<std::string, std::string>> parse_headers(std::string_view head) {
  std::vector<std::pair<std::string, std::string>> out;
  size_t pos = 0;
  std::string name, value;
  bool have = false;
  auto flush = [&]() {
    if (have) out.emplace_back(std::move(name), std::string(trim(value)));
    name.clear();
    value.clear();
    have = false;
  };
  while (pos != std::string_view::npos && pos < head.size()) {
    std::string_view line;
    pos = next_line(head, pos, line);
    if (line.empty()) continue;
    if ((line[0] == ' ' || line[0] == '\t') && have) {
      value += ' ';
      value.append(trim(line));
      continue;
    }
    size_t colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) continue;   // not a header line: ignore it
    flush();
    name = std::string(trim(line.substr(0, colon)));
    value = std::string(trim(line.substr(colon + 1)));
    have = true;
  }
  flush();
  return out;
}

std::string find_header(const std::vector<std::pair<std::string, std::string>>& h, std::string_view name) {
  for (const auto& kv : h)
    if (ieq(kv.first, name)) return kv.second;
  return {};
}

std::string percent_decode(std::string_view in) {
  std::string out;
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '%' && i + 2 < in.size() && hexval(in[i + 1]) >= 0 && hexval(in[i + 2]) >= 0) {
      out += static_cast<char>(hexval(in[i + 1]) * 16 + hexval(in[i + 2]));
      i += 2;
    } else {
      out += in[i];
    }
  }
  return out;
}

// --- dates -----------------------------------------------------------------------------------------

int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void civil_from_days(int64_t z, int& y, int& m, int& d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t yy = static_cast<int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  y = static_cast<int>(yy + (m <= 2));
}

const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

int month_of(std::string_view t) {
  if (t.size() < 3) return 0;
  for (int i = 0; i < 12; ++i)
    if (ieq(t.substr(0, 3), kMonths[i])) return i + 1;
  return 0;
}

bool all_digits(std::string_view s) {
  if (s.empty()) return false;
  for (char c : s)
    if (c < '0' || c > '9') return false;
  return true;
}

int to_int(std::string_view s) {
  int v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') break;
    v = v * 10 + (c - '0');
    if (v > 100000000) break;
  }
  return v;
}

// Whole header value split on whitespace, with (comments) removed.
std::vector<std::string> date_tokens(std::string_view text) {
  std::vector<std::string> toks;
  std::string cur;
  int depth = 0;
  for (char c : text) {
    if (c == '(') { ++depth; continue; }
    if (c == ')') { if (depth > 0) --depth; continue; }
    if (depth > 0) continue;
    if (is_ws(c) || c == ',') {
      if (!cur.empty()) { toks.push_back(cur); cur.clear(); }
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) toks.push_back(cur);
  return toks;
}

int zone_minutes(std::string_view z, bool& ok) {
  ok = true;
  if (z.empty()) { ok = false; return 0; }
  if ((z[0] == '+' || z[0] == '-') && z.size() >= 5 && all_digits(z.substr(1, 4))) {
    int hh = to_int(z.substr(1, 2)), mm = to_int(z.substr(3, 2));
    int v = hh * 60 + mm;
    return z[0] == '-' ? -v : v;
  }
  struct Z { const char* n; int h; };
  static const Z zones[] = {{"UT", 0}, {"UTC", 0}, {"GMT", 0}, {"Z", 0}, {"EST", -5}, {"EDT", -4}, {"CST", -6},
                            {"CDT", -5}, {"MST", -7}, {"MDT", -6}, {"PST", -8}, {"PDT", -7}};
  for (const auto& e : zones)
    if (ieq(z, e.n)) return e.h * 60;
  ok = false;
  return 0;
}

}  // namespace

// --- public pieces ---------------------------------------------------------------------------------

std::string decode_base64(std::string_view in) {
  std::string clean;
  clean.reserve(in.size());
  for (char c : in) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/') clean += c;
    else if (c == '-') clean += '+';   // base64url, in case a header carries it
    else if (c == '_') clean += '/';
    else if (c == '=') break;
  }
  while (clean.size() % 4 != 0) clean += '=';
  auto r = aios::base64_decode(clean);
  if (!r.ok()) {
    // A stream that is cut off mid-quantum: decode what is whole.
    std::string whole = clean.substr(0, clean.size() - clean.size() % 4);
    auto r2 = aios::base64_decode(whole);
    if (!r2.ok()) return {};
    return std::string(r2->begin(), r2->end());
  }
  return std::string(r->begin(), r->end());
}

std::string decode_quoted_printable(std::string_view in, bool header_mode) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    char c = in[i];
    if (c == '=') {
      if (i + 1 < in.size() && (in[i + 1] == '\n' || in[i + 1] == '\r')) {   // soft line break
        size_t j = i + 1;
        if (in[j] == '\r') ++j;
        if (j < in.size() && in[j] == '\n') ++j;
        i = j - 1;
        continue;
      }
      if (i + 2 < in.size() && hexval(in[i + 1]) >= 0 && hexval(in[i + 2]) >= 0) {
        out += static_cast<char>(hexval(in[i + 1]) * 16 + hexval(in[i + 2]));
        i += 2;
        continue;
      }
      out += c;
    } else if (header_mode && c == '_') {
      out += ' ';
    } else {
      out += c;
    }
  }
  return out;
}

bool charset_supported(std::string_view charset) {
  std::string cs = normalize_charset(charset);
  return is_utf8_name(cs) || is_ascii_name(cs) || is_latin1_name(cs) || table_for(cs) != nullptr;
}

std::string to_utf8(std::string_view data, std::string_view charset) {
  std::string cs = normalize_charset(charset);
  if (is_utf8_name(cs)) return sanitize_utf8(data);
  std::string out;
  out.reserve(data.size() + data.size() / 4);
  if (is_ascii_name(cs)) {
    for (char c : data) {
      unsigned char u = static_cast<unsigned char>(c);
      if (u < 0x80) out += c; else put_utf8(out, 0xFFFD);
    }
    return out;
  }
  if (is_latin1_name(cs)) {
    for (char c : data) put_utf8(out, static_cast<unsigned char>(c));
    return out;
  }
  if (const uint16_t* t = table_for(cs)) {
    for (char c : data) {
      unsigned char u = static_cast<unsigned char>(c);
      put_utf8(out, u < 0x80 ? u : t[u - 0x80]);
    }
    return out;
  }
  // No table for it: keep the ASCII, mark the rest, rather than pass bytes off as UTF-8.
  for (char c : data) {
    unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x80) out += c; else put_utf8(out, 0xFFFD);
  }
  return out;
}

std::string decode_words(std::string_view value) {
  std::string out;
  size_t i = 0;
  bool last_was_word = false;
  std::string pending_ws;   // whitespace seen since the last encoded word
  while (i < value.size()) {
    if (value[i] == '=' && i + 1 < value.size() && value[i + 1] == '?') {
      // =?charset?enc?text?=
      size_t c1 = value.find('?', i + 2);
      size_t c2 = (c1 == std::string_view::npos) ? c1 : value.find('?', c1 + 1);
      size_t end = (c2 == std::string_view::npos) ? c2 : value.find("?=", c2 + 1);
      if (c1 != std::string_view::npos && c2 != std::string_view::npos && end != std::string_view::npos &&
          c2 == c1 + 2) {
        std::string_view cs = value.substr(i + 2, c1 - i - 2);
        char enc = lc(value[c1 + 1]);
        std::string_view text = value.substr(c2 + 1, end - c2 - 1);
        std::string raw;
        bool ok = true;
        if (enc == 'b') raw = decode_base64(text);
        else if (enc == 'q') raw = decode_quoted_printable(text, true);
        else ok = false;
        if (ok) {
          if (!last_was_word) out += pending_ws;   // whitespace between two encoded words is dropped
          pending_ws.clear();
          out += to_utf8(raw, cs);
          last_was_word = true;
          i = end + 2;
          continue;
        }
      }
    }
    char c = value[i];
    if (is_ws(c)) {
      pending_ws += c;
      ++i;
      continue;
    }
    out += pending_ws;
    pending_ws.clear();
    out += c;
    last_was_word = false;
    ++i;
  }
  out += pending_ws;
  // collapse whitespace runs, trim
  std::string collapsed;
  bool in_ws = false;
  for (char c : out) {
    if (is_ws(c)) { in_ws = true; continue; }
    if (in_ws && !collapsed.empty()) collapsed += ' ';
    in_ws = false;
    collapsed += c;
  }
  return collapsed;
}

std::vector<Address> parse_addresses(std::string_view value) {
  // Split on commas that are outside quotes, <angle brackets> and (comments).
  std::vector<std::string> parts;
  std::string cur;
  bool quote = false;
  int angle = 0, paren = 0;
  for (size_t i = 0; i < value.size(); ++i) {
    char c = value[i];
    if (c == '\\' && i + 1 < value.size() && (quote || paren > 0)) { cur += c; cur += value[++i]; continue; }
    if (c == '"' && paren == 0) quote = !quote;
    else if (!quote && c == '(') ++paren;
    else if (!quote && c == ')' && paren > 0) --paren;
    else if (!quote && paren == 0 && c == '<') ++angle;
    else if (!quote && paren == 0 && c == '>' && angle > 0) --angle;
    if ((c == ',' || c == ';') && !quote && angle == 0 && paren == 0) {
      parts.push_back(cur);
      cur.clear();
      continue;
    }
    cur += c;
  }
  parts.push_back(cur);

  std::vector<Address> out;
  for (auto& p : parts) {
    std::string_view s = trim(p);
    if (s.empty()) continue;
    // group syntax "Name: a@x" - drop the label
    {
      bool q = false;
      for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '"') q = !q;
        if (s[i] == ':' && !q && s.find('<') == std::string_view::npos && s.find('@') > i) { s = trim(s.substr(i + 1)); break; }
      }
    }
    Address a;
    size_t lt = std::string_view::npos, gt = std::string_view::npos;
    {
      bool q = false;
      for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '"') q = !q;
        if (!q && s[i] == '<' && lt == std::string_view::npos) lt = i;
        if (!q && s[i] == '>' && lt != std::string_view::npos && gt == std::string_view::npos) gt = i;
      }
    }
    auto unquote = [](std::string_view n) {
      n = trim(n);
      std::string r;
      bool q = false;
      for (size_t i = 0; i < n.size(); ++i) {
        if (n[i] == '\\' && i + 1 < n.size()) { r += n[++i]; continue; }
        if (n[i] == '"') { q = !q; continue; }
        r += n[i];
      }
      (void)q;
      return r;
    };
    if (lt != std::string_view::npos && gt != std::string_view::npos && gt > lt) {
      a.addr = std::string(trim(s.substr(lt + 1, gt - lt - 1)));
      a.name = unquote(s.substr(0, lt));
    } else {
      // addr (Name)  or bare addr
      size_t po = s.find('(');
      std::string_view addr_part = po == std::string_view::npos ? s : s.substr(0, po);
      a.addr = std::string(trim(addr_part));
      if (po != std::string_view::npos) {
        size_t pc = s.find(')', po);
        a.name = std::string(trim(s.substr(po + 1, pc == std::string_view::npos ? std::string_view::npos : pc - po - 1)));
      }
    }
    // a route in front of the address ("@a,@b:user@c") is not supported; take what follows the last ':'
    a.name = decode_words(a.name);
    if (a.addr.empty() && a.name.empty()) continue;
    out.push_back(std::move(a));
  }
  return out;
}

Address parse_address(std::string_view value) {
  auto v = parse_addresses(value);
  return v.empty() ? Address{} : v.front();
}

bool parse_date(std::string_view text, int64_t& epoch, int& offset_minutes) {
  auto toks = date_tokens(text);
  size_t i = 0;
  // optional weekday
  if (i < toks.size() && !toks[i].empty() && !(toks[i][0] >= '0' && toks[i][0] <= '9') && month_of(toks[i]) == 0) ++i;
  if (toks.size() < i + 3) return false;
  if (i >= toks.size() || !all_digits(toks[i])) return false;
  int day = to_int(toks[i++]);
  if (i >= toks.size()) return false;
  int mon = month_of(toks[i++]);
  if (mon == 0) return false;
  if (i >= toks.size() || !all_digits(toks[i])) return false;
  int year = to_int(toks[i]);
  size_t ydigits = toks[i].size();
  ++i;
  if (ydigits <= 2) year += year < 70 ? 2000 : 1900;
  else if (ydigits == 3) year += 1900;
  int hh = 0, mi = 0, ss = 0;
  if (i < toks.size()) {
    const std::string& t = toks[i];
    size_t c1 = t.find(':');
    if (c1 != std::string::npos) {
      hh = to_int(std::string_view(t).substr(0, c1));
      size_t c2 = t.find(':', c1 + 1);
      if (c2 == std::string::npos) mi = to_int(std::string_view(t).substr(c1 + 1));
      else {
        mi = to_int(std::string_view(t).substr(c1 + 1, c2 - c1 - 1));
        ss = to_int(std::string_view(t).substr(c2 + 1));
      }
      ++i;
    }
  }
  int off = 0;
  if (i < toks.size()) {
    bool ok = false;
    off = zone_minutes(toks[i], ok);
    if (!ok) off = 0;
  }
  if (day < 1 || day > 31 || hh > 24 || mi > 59 || ss > 60 || year < 1) return false;
  int64_t days = days_from_civil(year, static_cast<unsigned>(mon), static_cast<unsigned>(day));
  epoch = days * 86400 + hh * 3600 + mi * 60 + ss - static_cast<int64_t>(off) * 60;
  offset_minutes = off;
  return true;
}

std::string format_rfc5322_date(int64_t epoch) {
  static const char* wd[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  int64_t days = epoch >= 0 ? epoch / 86400 : -((-epoch + 86399) / 86400);
  int64_t rem = epoch - days * 86400;
  int y, m, d;
  civil_from_days(days, y, m, d);
  const int dow = static_cast<int>(((days % 7) + 11) % 7);   // 1970-01-01 was a Thursday
  char buf[48];
  std::snprintf(buf, sizeof buf, "%s, %02d %s %d %02d:%02d:%02d +0000", wd[dow], d, kMonths[m - 1], y, static_cast<int>(rem / 3600),
                static_cast<int>(rem % 3600 / 60), static_cast<int>(rem % 60));
  return buf;
}

std::string format_date(int64_t epoch, int offset_minutes) {
  int64_t local = epoch + static_cast<int64_t>(offset_minutes) * 60;
  int64_t days = local >= 0 ? local / 86400 : -((-local + 86399) / 86400);
  int y, m, d;
  civil_from_days(days, y, m, d);
  char buf[32];
  std::snprintf(buf, sizeof buf, "%s %02d %d", kMonths[m - 1], d, y);
  return buf;
}

std::string html_to_text(std::string_view html) {
  // <style>..</style> and <script>..</script> go whole.
  std::string s;
  s.reserve(html.size());
  size_t i = 0;
  while (i < html.size()) {
    if (html[i] == '<') {
      auto starts = [&](std::string_view tag) {
        return i + tag.size() <= html.size() && ieq(html.substr(i, tag.size()), tag);
      };
      if (starts("<style") || starts("<script")) {
        bool style = starts("<style");
        size_t close = find_i(html, style ? "</style>" : "</script>", i);
        if (close != std::string_view::npos) { i = close + (style ? 8 : 9); continue; }
      }
      if (starts("<br")) {
        size_t gt = html.find('>', i);
        if (gt != std::string_view::npos) { s += '\n'; i = gt + 1; continue; }
      }
      if (starts("</p>")) { s += "\n\n"; i += 4; continue; }
      size_t gt = html.find('>', i);
      if (gt == std::string_view::npos) { s += html[i]; ++i; continue; }
      i = gt + 1;   // any other tag: dropped
      continue;
    }
    s += html[i];
    ++i;
  }
  // entities
  std::string out;
  out.reserve(s.size());
  for (size_t k = 0; k < s.size(); ++k) {
    if (s[k] != '&') { out += s[k]; continue; }
    size_t semi = s.find(';', k);
    if (semi == std::string::npos || semi - k > 10) { out += s[k]; continue; }
    std::string_view ent = std::string_view(s).substr(k + 1, semi - k - 1);
    if (ent == "nbsp") out += ' ';
    else if (ent == "amp") out += '&';
    else if (ent == "lt") out += '<';
    else if (ent == "gt") out += '>';
    else if (ent == "quot") out += '"';
    else if (ent == "apos") out += '\'';
    else if (!ent.empty() && ent[0] == '#') {
      uint32_t cp = 0;
      bool ok = ent.size() > 1;
      if (ok && (ent[1] == 'x' || ent[1] == 'X')) {
        ok = ent.size() > 2;
        for (size_t q = 2; q < ent.size() && ok; ++q) {
          int h = hexval(ent[q]);
          if (h < 0) ok = false; else cp = cp * 16 + static_cast<uint32_t>(h);
        }
      } else {
        for (size_t q = 1; q < ent.size() && ok; ++q) {
          if (ent[q] < '0' || ent[q] > '9') ok = false; else cp = cp * 10 + static_cast<uint32_t>(ent[q] - '0');
        }
      }
      if (ok && cp > 0 && cp <= 0x10FFFF) put_utf8(out, cp); else { out += s[k]; continue; }
    } else {
      out += s[k];
      continue;
    }
    k = semi;
  }
  return std::string(trim(out));
}

TypedValue parse_typed_value(std::string_view v) {
  TypedValue tv;
  // value part
  size_t semi = std::string_view::npos;
  {
    bool q = false;
    for (size_t i = 0; i < v.size(); ++i) {
      if (v[i] == '"') q = !q;
      if (v[i] == ';' && !q) { semi = i; break; }
    }
  }
  tv.value = lower(trim(v.substr(0, semi)));
  if (semi == std::string_view::npos) return tv;

  struct Seg { std::string name; int index; bool ext; std::string value; };
  std::vector<Seg> segs;
  size_t pos = semi;
  while (pos != std::string_view::npos && pos < v.size()) {
    ++pos;   // past ';'
    // name
    while (pos < v.size() && is_ws(v[pos])) ++pos;
    size_t eq = pos;
    while (eq < v.size() && v[eq] != '=' && v[eq] != ';') ++eq;
    if (eq >= v.size() || v[eq] != '=') {
      size_t nx = v.find(';', eq);
      pos = nx;
      continue;
    }
    std::string name = lower(trim(v.substr(pos, eq - pos)));
    size_t p = eq + 1;
    while (p < v.size() && is_ws(v[p])) ++p;
    std::string val;
    if (p < v.size() && v[p] == '"') {
      ++p;
      while (p < v.size() && v[p] != '"') {
        if (v[p] == '\\' && p + 1 < v.size()) ++p;
        val += v[p++];
      }
      if (p < v.size()) ++p;
      while (p < v.size() && v[p] != ';') ++p;
    } else {
      size_t e = p;
      while (e < v.size() && v[e] != ';') ++e;
      val = std::string(trim(v.substr(p, e - p)));
      p = e;
    }
    pos = (p < v.size()) ? p : std::string_view::npos;

    Seg sg{name, -1, false, val};
    // name*  name*N  name*N*
    size_t star = name.find('*');
    if (star != std::string::npos) {
      sg.name = name.substr(0, star);
      std::string rest = name.substr(star + 1);
      if (rest.empty()) { sg.ext = true; sg.index = 0; }
      else if (rest.back() == '*') { sg.ext = true; sg.index = to_int(rest.substr(0, rest.size() - 1)); }
      else { sg.index = to_int(rest); }
    }
    segs.push_back(std::move(sg));
  }

  // Join continuations by base name, in index order, then decode extended ones.
  std::vector<std::string> done;
  for (const auto& s0 : segs) {
    if (std::find(done.begin(), done.end(), s0.name) != done.end()) continue;
    done.push_back(s0.name);
    std::vector<const Seg*> mine;
    for (const auto& s1 : segs)
      if (s1.name == s0.name) mine.push_back(&s1);
    std::stable_sort(mine.begin(), mine.end(), [](const Seg* a, const Seg* b) { return a->index < b->index; });
    if (mine.size() == 1 && mine[0]->index < 0) {
      tv.params.emplace_back(s0.name, mine[0]->value);
      continue;
    }
    std::string charset;
    std::string joined;
    bool first = true;
    for (const Seg* sg : mine) {
      std::string piece = sg->value;
      if (sg->ext) {
        if (first) {
          // charset'language'text
          size_t a = piece.find('\'');
          size_t b = (a == std::string::npos) ? a : piece.find('\'', a + 1);
          if (a != std::string::npos && b != std::string::npos) {
            charset = piece.substr(0, a);
            piece = piece.substr(b + 1);
          }
        }
        piece = percent_decode(piece);
      }
      joined += piece;
      first = false;
    }
    tv.params.emplace_back(s0.name, charset.empty() ? joined : to_utf8(joined, charset));
  }
  return tv;
}

std::string TypedValue::param(std::string_view name) const {
  for (const auto& kv : params)
    if (ieq(kv.first, name)) return kv.second;
  return {};
}

std::string Parsed::header(std::string_view name) const { return find_header(headers, name); }
std::string Parsed::decoded_header(std::string_view name) const { return decode_words(find_header(headers, name)); }

// --- the walk --------------------------------------------------------------------------------------

namespace {

struct Walk {
  Parsed& out;
  std::map<std::string, std::pair<std::string, std::string>> cid_parts;   // cid -> (mime, bytes)
  bool have_body = false;
  bool have_html = false;
  size_t parts = 0;
};

std::string strip_angle(std::string_view v) {
  v = trim(v);
  while (!v.empty() && (v.front() == '<' || is_ws(v.front()))) v.remove_prefix(1);
  while (!v.empty() && (v.back() == '>' || is_ws(v.back()))) v.remove_suffix(1);
  return std::string(v);
}

std::string transfer_decode(std::string_view body, std::string_view cte) {
  std::string c = lower(trim(cte));
  if (c == "base64") return decode_base64(body);
  if (c == "quoted-printable") return decode_quoted_printable(body);
  return std::string(body);
}

void walk_part(Walk& w, std::string_view raw, int depth);

void walk_multipart(Walk& w, std::string_view body, const std::string& boundary, int depth) {
  const std::string delim = "--" + boundary;
  size_t pos = 0;
  std::vector<std::string_view> chunks;
  size_t chunk_start = std::string_view::npos;
  while (pos <= body.size()) {
    size_t line_start = pos;
    std::string_view line;
    size_t next = next_line(body, pos, line);
    if (next == std::string_view::npos) break;
    if (line.size() >= delim.size() && line.substr(0, delim.size()) == delim) {
      bool final_delim = line.size() >= delim.size() + 2 && line.substr(delim.size(), 2) == "--";
      bool plain_delim = trim(line.substr(delim.size())).empty();
      if (final_delim || plain_delim) {
        if (chunk_start != std::string_view::npos) {
          size_t e = line_start;
          // the line break before the delimiter belongs to the delimiter
          if (e > chunk_start && body[e - 1] == '\n') --e;
          if (e > chunk_start && body[e - 1] == '\r') --e;
          chunks.push_back(body.substr(chunk_start, e - chunk_start));
        }
        if (final_delim) { chunk_start = std::string_view::npos; break; }
        chunk_start = next;
      }
    }
    pos = next;
  }
  if (chunk_start != std::string_view::npos && chunk_start < body.size()) {   // no closing delimiter: take the rest
    std::string_view rest = body.substr(chunk_start);
    if (!rest.empty() && rest.back() == '\n') rest.remove_suffix(1);
    if (!rest.empty() && rest.back() == '\r') rest.remove_suffix(1);
    chunks.push_back(rest);
  }
  for (auto c : chunks) walk_part(w, c, depth + 1);
}

void walk_part(Walk& w, std::string_view raw, int depth) {
  if (depth > 24 || ++w.parts > 2000) return;
  std::string_view head, body;
  split_head_body(raw, head, body);
  auto hdrs = parse_headers(head);

  TypedValue ct = parse_typed_value(find_header(hdrs, "Content-Type"));
  if (ct.value.empty()) ct.value = "text/plain";
  std::string major = ct.value.substr(0, ct.value.find('/'));

  if (major == "multipart") {
    std::string boundary = ct.param("boundary");
    if (!boundary.empty()) { walk_multipart(w, body, boundary, depth); return; }
  }
  if (ct.value == "message/rfc822") {
    walk_part(w, body, depth + 1);
    return;
  }

  std::string cte = find_header(hdrs, "Content-Transfer-Encoding");
  TypedValue cd = parse_typed_value(find_header(hdrs, "Content-Disposition"));
  std::string filename = cd.param("filename");
  if (filename.empty()) filename = ct.param("name");
  filename = decode_words(filename);
  std::string cid = strip_angle(find_header(hdrs, "Content-ID"));

  const bool is_text = major == "text";
  std::string payload;
  const bool need_payload = !cid.empty() || !filename.empty() || is_text;
  if (need_payload) payload = transfer_decode(body, cte);

  if (!cid.empty() && ct.value.compare(0, 6, "image/") == 0) w.cid_parts[cid] = {ct.value, payload};
  if (!filename.empty()) {
    AttachmentInfo a;
    a.filename = filename;
    a.mime_type = ct.value;
    a.content_id = cid;
    a.size_bytes = payload.size();
    w.out.attachments.push_back(std::move(a));
    return;
  }
  if (cd.value == "attachment" || contains_i(find_header(hdrs, "Content-Disposition"), "attachment")) return;
  if (!is_text || payload.empty()) return;

  std::string text = to_utf8(payload, ct.param("charset"));
  if (ct.value == "text/html") {
    if (!w.have_html) { w.out.html = text; w.have_html = true; }
    if (!w.have_body) { w.out.text = html_to_text(text); w.have_body = true; }
  } else if (ct.value == "text/plain" && !w.have_body) {
    w.out.text = text;
    w.have_body = true;
  }
}

std::string b64(std::string_view s) {
  return aios::base64_encode(aios::ByteSpan(reinterpret_cast<const uint8_t*>(s.data()), s.size()));
}

// src="cid:XYZ" / src='cid:XYZ' -> the part's own bytes as a data: url, so the html is complete alone.
std::string inline_cids(std::string html, const std::map<std::string, std::pair<std::string, std::string>>& parts) {
  if (html.empty() || parts.empty()) return html;
  std::string out;
  size_t i = 0;
  while (i < html.size()) {
    size_t at = find_i(html, "cid:", i);
    if (at == std::string::npos) break;
    char q = at > 0 ? html[at - 1] : 0;
    if (q != '"' && q != '\'') { out.append(html, i, at + 4 - i); i = at + 4; continue; }
    size_t e = html.find(q, at + 4);
    if (e == std::string::npos) break;
    std::string id = html.substr(at + 4, e - at - 4);
    const std::pair<std::string, std::string>* hit = nullptr;
    for (const auto& kv : parts)
      if (ieq(kv.first, id)) { hit = &kv.second; break; }
    out.append(html, i, at - i);
    if (hit && !hit->second.empty()) out += "data:" + hit->first + ";base64," + b64(hit->second);
    else out.append(html, at, e - at);
    i = e;
  }
  out.append(html, i, std::string::npos);
  return out;
}

}  // namespace

Parsed parse(std::string_view raw) {
  Parsed p;
  std::string_view head, body;
  split_head_body(raw, head, body);
  p.headers = parse_headers(head);
  Walk w{p, {}, false, false, 0};
  walk_part(w, raw, 0);
  p.html = inline_cids(std::move(p.html), w.cid_parts);
  return p;
}

}  // namespace aios::net::mime
