// IMAP4rev1 client. See include/aios/net/imap.hpp.
#include "aios/net/imap.hpp"

#include <algorithm>
#include <cstring>

#include "aios/core/base64.hpp"
#include "aios/core/bytes.hpp"

namespace aios::net {

namespace {

char lc(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
char uc(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }

std::string upper(std::string_view s) {
  std::string o(s);
  for (auto& c : o) c = uc(c);
  return o;
}

bool ieq(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (lc(a[i]) != lc(b[i])) return false;
  return true;
}

std::string b64enc(std::string_view s) {
  return aios::base64_encode(aios::ByteSpan(reinterpret_cast<const uint8_t*>(s.data()), s.size()));
}

uint64_t to_u64(std::string_view s) {
  uint64_t v = 0;
  bool any = false;
  for (char c : s) {
    if (c < '0' || c > '9') return any ? v : 0;
    any = true;
    if (v > (UINT64_MAX - 9) / 10) return v;
    v = v * 10 + static_cast<uint64_t>(c - '0');
  }
  return v;
}

bool all_digits(std::string_view s) {
  if (s.empty()) return false;
  for (char c : s)
    if (c < '0' || c > '9') return false;
  return true;
}

std::string quote(std::string_view s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    o += c;
  }
  return o + "\"";
}

bool needs_literal(std::string_view s) {
  for (unsigned char c : s)
    if (c == 0 || c == '\r' || c == '\n' || c >= 0x80) return true;
  return false;
}

// --- parsing a response line, pulling literals off the stream where the line says {n} -------------

struct Cursor {
  LineStream& ls;
  std::string line;
  size_t pos = 0;

  void skip_spaces() { while (pos < line.size() && line[pos] == ' ') ++pos; }
  bool at_end() { skip_spaces(); return pos >= line.size(); }
};

bool atom_stop(char c) { return c == ' ' || c == '(' || c == ')' || c == '{' || c == '"' || c == ']' || c == '\r' || c == '\n'; }

Result<ImapVal> parse_value(Cursor& c, int depth);

Result<ImapVal> parse_list(Cursor& c, int depth) {
  ImapVal v;
  v.t = ImapVal::T::List;
  ++c.pos;   // '('
  for (;;) {
    c.skip_spaces();
    if (c.pos >= c.line.size()) return Result<ImapVal>(Status::Corrupt, "an unterminated list in a server response");
    if (c.line[c.pos] == ')') { ++c.pos; return v; }
    auto e = parse_value(c, depth + 1);
    if (!e.ok()) return e;
    v.l.push_back(std::move(*e));
    if (v.l.size() > 2000000) return Result<ImapVal>(Status::Exhausted, "a list too long to be real");
  }
}

Result<ImapVal> parse_value(Cursor& c, int depth) {
  if (depth > 64) return Result<ImapVal>(Status::Exhausted, "lists nested too deep in a server response");
  c.skip_spaces();
  if (c.pos >= c.line.size()) return Result<ImapVal>(Status::Corrupt, "a response ended where a value was expected");
  char ch = c.line[c.pos];
  if (ch == '(') return parse_list(c, depth);
  ImapVal v;
  if (ch == '"') {
    v.t = ImapVal::T::Str;
    ++c.pos;
    while (c.pos < c.line.size() && c.line[c.pos] != '"') {
      if (c.line[c.pos] == '\\' && c.pos + 1 < c.line.size()) ++c.pos;
      v.s += c.line[c.pos++];
    }
    if (c.pos >= c.line.size()) return Result<ImapVal>(Status::Corrupt, "an unterminated string in a server response");
    ++c.pos;
    return v;
  }
  if (ch == '{') {
    size_t close = c.line.find('}', c.pos);
    if (close == std::string::npos || close != c.line.size() - 1)
      return Result<ImapVal>(Status::Corrupt, "a literal marker that does not end its line");
    std::string num = c.line.substr(c.pos + 1, close - c.pos - 1);
    if (!num.empty() && (num.back() == '+' || num.back() == '-')) num.pop_back();
    if (!all_digits(num)) return Result<ImapVal>(Status::Corrupt, "a literal marker with no length");
    uint64_t n = to_u64(num);
    if (n > (512ull << 20)) return Result<ImapVal>(Status::Exhausted, "a literal over 512 MB");
    auto data = c.ls.read_exact(static_cast<size_t>(n));
    if (!data.ok()) return Result<ImapVal>(data.error());
    auto rest = c.ls.read_line();
    if (!rest.ok()) return Result<ImapVal>(rest.error());
    v.t = ImapVal::T::Str;
    v.s = std::move(*data);
    c.line = std::move(*rest);
    c.pos = 0;
    return v;
  }
  // atom; BODY[HEADER.FIELDS (FROM TO)] keeps its bracketed part, spaces and all, and a <partial>
  size_t start = c.pos;
  while (c.pos < c.line.size()) {
    char k = c.line[c.pos];
    if (k == '[') {
      int d = 0;
      bool q = false;
      while (c.pos < c.line.size()) {
        char x = c.line[c.pos];
        if (q) { if (x == '\\' && c.pos + 1 < c.line.size()) ++c.pos; else if (x == '"') q = false; }
        else if (x == '"') q = true;
        else if (x == '[') ++d;
        else if (x == ']') { if (--d == 0) { ++c.pos; break; } }
        ++c.pos;
      }
      continue;
    }
    if (k == '<' && c.pos > start) {   // a partial-range suffix
      size_t gt = c.line.find('>', c.pos);
      if (gt != std::string::npos) { c.pos = gt + 1; continue; }
    }
    if (atom_stop(k)) break;
    ++c.pos;
  }
  if (c.pos == start) { ++c.pos; return Result<ImapVal>(Status::Corrupt, "an unexpected character in a server response"); }
  v.t = ImapVal::T::Atom;
  v.s = c.line.substr(start, c.pos - start);
  if (ieq(v.s, "NIL")) { v.t = ImapVal::T::Nil; v.s.clear(); }
  return v;
}

// --- modified UTF-7 --------------------------------------------------------------------------------

void utf8_to_u16(std::string_view in, std::vector<uint16_t>& out) {
  size_t i = 0;
  while (i < in.size()) {
    unsigned char c = static_cast<unsigned char>(in[i]);
    uint32_t cp;
    size_t n;
    if (c < 0x80) { cp = c; n = 1; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 4; }
    else { cp = 0xFFFD; n = 1; }
    if (i + n > in.size()) { cp = 0xFFFD; n = in.size() - i; }
    else for (size_t k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(in[i + k]) & 0x3F);
    i += n;
    if (cp >= 0x10000) {
      cp -= 0x10000;
      out.push_back(static_cast<uint16_t>(0xD800 + (cp >> 10)));
      out.push_back(static_cast<uint16_t>(0xDC00 + (cp & 0x3FF)));
    } else {
      out.push_back(static_cast<uint16_t>(cp));
    }
  }
}

void put_utf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) out += static_cast<char>(cp);
  else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
  else if (cp < 0x10000) {
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

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+,";

}  // namespace

std::string imap_quote(std::string_view s) { return quote(s); }
bool imap_needs_literal(std::string_view s) { return needs_literal(s); }

std::string imap_utf7_encode(std::string_view utf8) {
  std::string out;
  size_t i = 0;
  while (i < utf8.size()) {
    unsigned char c = static_cast<unsigned char>(utf8[i]);
    if (c >= 0x20 && c < 0x7F) {
      if (c == '&') out += "&-"; else out += static_cast<char>(c);
      ++i;
      continue;
    }
    size_t j = i;
    while (j < utf8.size()) {
      unsigned char d = static_cast<unsigned char>(utf8[j]);
      if (d >= 0x20 && d < 0x7F) break;
      ++j;
    }
    std::vector<uint16_t> u16;
    utf8_to_u16(utf8.substr(i, j - i), u16);
    std::string bytes;
    for (uint16_t u : u16) { bytes += static_cast<char>(u >> 8); bytes += static_cast<char>(u & 0xFF); }
    out += '&';
    uint32_t acc = 0;
    int bits = 0;
    for (unsigned char b : bytes) {
      acc = (acc << 8) | b;
      bits += 8;
      while (bits >= 6) { out += kB64[(acc >> (bits - 6)) & 0x3F]; bits -= 6; }
    }
    if (bits > 0) out += kB64[(acc << (6 - bits)) & 0x3F];
    out += '-';
    i = j;
  }
  return out;
}

std::string imap_utf7_decode(std::string_view wire) {
  std::string out;
  size_t i = 0;
  while (i < wire.size()) {
    if (wire[i] != '&') { out += wire[i++]; continue; }
    size_t dash = wire.find('-', i + 1);
    if (dash == std::string_view::npos) { out += wire.substr(i); break; }
    if (dash == i + 1) { out += '&'; i = dash + 1; continue; }
    uint32_t acc = 0;
    int bits = 0;
    std::vector<uint8_t> bytes;
    for (size_t k = i + 1; k < dash; ++k) {
      const char* p = std::strchr(kB64, wire[k]);
      if (!p || !wire[k]) continue;
      acc = (acc << 6) | static_cast<uint32_t>(p - kB64);
      bits += 6;
      if (bits >= 8) { bytes.push_back(static_cast<uint8_t>((acc >> (bits - 8)) & 0xFF)); bits -= 8; }
    }
    for (size_t k = 0; k + 1 < bytes.size(); k += 2) {
      uint32_t u = (static_cast<uint32_t>(bytes[k]) << 8) | bytes[k + 1];
      if (u >= 0xD800 && u < 0xDC00 && k + 3 < bytes.size()) {
        uint32_t lo = (static_cast<uint32_t>(bytes[k + 2]) << 8) | bytes[k + 3];
        if (lo >= 0xDC00 && lo < 0xE000) { put_utf8(out, 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00)); k += 2; continue; }
      }
      put_utf8(out, u);
    }
    i = dash + 1;
  }
  return out;
}

bool imap_parse_internaldate(std::string_view t, int64_t& epoch) {
  // "17-Jul-1996 02:44:25 -0700" (the day may be space-padded)
  while (!t.empty() && t.front() == ' ') t.remove_prefix(1);
  size_t d1 = t.find('-');
  if (d1 == std::string_view::npos || d1 + 4 >= t.size()) return false;
  int day = static_cast<int>(to_u64(t.substr(0, d1)));
  static const char* mons[] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
  int mon = 0;
  for (int i = 0; i < 12; ++i) if (ieq(t.substr(d1 + 1, 3), mons[i])) mon = i + 1;
  if (mon == 0 || t[d1 + 4] != '-') return false;
  size_t sp = t.find(' ', d1 + 5);
  if (sp == std::string_view::npos) return false;
  int year = static_cast<int>(to_u64(t.substr(d1 + 5, sp - d1 - 5)));
  std::string_view rest = t.substr(sp + 1);
  if (rest.size() < 8) return false;
  int hh = static_cast<int>(to_u64(rest.substr(0, 2))), mi = static_cast<int>(to_u64(rest.substr(3, 2))),
      ss = static_cast<int>(to_u64(rest.substr(6, 2)));
  int off = 0;
  if (rest.size() >= 14 && (rest[9] == '+' || rest[9] == '-')) {
    int v = static_cast<int>(to_u64(rest.substr(10, 2))) * 60 + static_cast<int>(to_u64(rest.substr(12, 2)));
    off = rest[9] == '-' ? -v : v;
  }
  // days from civil
  int64_t y = year - (mon <= 2);
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * static_cast<unsigned>(mon + (mon > 2 ? -3 : 9)) + 2) / 5 + static_cast<unsigned>(day) - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  int64_t days = era * 146097 + static_cast<int64_t>(doe) - 719468;
  epoch = days * 86400 + hh * 3600 + mi * 60 + ss - static_cast<int64_t>(off) * 60;
  return true;
}

uint64_t ImapVal::num() const { return is_text() ? to_u64(s) : 0; }

const ImapVal* ImapVal::item(std::string_view name) const {
  if (t != T::List) return nullptr;
  for (size_t i = 0; i + 1 < l.size(); i += 2)
    if (l[i].is_text() && ieq(l[i].s, name)) return &l[i + 1];
  return nullptr;
}

bool ImapMailbox::has_attr(std::string_view a) const {
  for (const auto& x : attrs)
    if (ieq(x, a)) return true;
  return false;
}

// --- the client ------------------------------------------------------------------------------------

ImapClient::ImapClient(hal::Host& host, std::string kernel_root, int timeout_ms, std::string trust_store)
    : stream_(host, std::move(kernel_root), timeout_ms, std::move(trust_store)) {}

std::string ImapClient::next_tag() {
  char b[16];
  std::snprintf(b, sizeof b, "A%04u", ++tag_ % 10000u);
  return b;
}

bool ImapClient::has_capability(std::string_view name) const {
  for (const auto& c : caps_)
    if (ieq(c, name)) return true;
  return false;
}

void ImapClient::note_caps(const ImapResult& r) {
  for (const auto& u : r.untagged) {
    if (u.kind == "CAPABILITY") {
      caps_.clear();
      for (const auto& a : u.args) if (a.is_text()) caps_.push_back(upper(a.s));
    } else if ((u.kind == "OK" || u.kind == "PREAUTH") && u.code == "CAPABILITY") {
      caps_.clear();
      if (u.code_arg.is_text()) caps_.push_back(upper(u.code_arg.s));
      for (const auto& a : u.args) if (a.is_text()) caps_.push_back(upper(a.s));
    }
  }
  if (r.code == "CAPABILITY") {
    caps_.clear();
    if (r.code_arg.is_text()) caps_.push_back(upper(r.code_arg.s));
  }
}

Result<ImapResponse> ImapClient::read_untagged(const std::string& line) {
  Cursor c{stream_, line, 2};   // past "* "
  ImapResponse r;
  c.skip_spaces();
  size_t s = c.pos;
  while (c.pos < c.line.size() && c.line[c.pos] != ' ') ++c.pos;
  std::string first = c.line.substr(s, c.pos - s);
  if (all_digits(first)) {
    r.has_number = true;
    r.number = static_cast<uint32_t>(to_u64(first));
    c.skip_spaces();
    s = c.pos;
    while (c.pos < c.line.size() && c.line[c.pos] != ' ') ++c.pos;
    r.kind = upper(c.line.substr(s, c.pos - s));
  } else {
    r.kind = upper(first);
  }
  const bool status_like = r.kind == "OK" || r.kind == "NO" || r.kind == "BAD" || r.kind == "PREAUTH" || r.kind == "BYE";
  if (status_like) {
    c.skip_spaces();
    if (c.pos < c.line.size() && c.line[c.pos] == '[') {
      ++c.pos;
      size_t cs = c.pos;
      while (c.pos < c.line.size() && c.line[c.pos] != ' ' && c.line[c.pos] != ']') ++c.pos;
      r.code = upper(c.line.substr(cs, c.pos - cs));
      bool first_arg = true;
      for (;;) {
        c.skip_spaces();
        if (c.pos >= c.line.size()) break;
        if (c.line[c.pos] == ']') { ++c.pos; break; }
        auto v = parse_value(c, 0);
        if (!v.ok()) return Result<ImapResponse>(v.error());
        if (first_arg) { r.code_arg = *v; first_arg = false; }
        else r.args.push_back(std::move(*v));
      }
    }
    c.skip_spaces();
    r.text = c.line.substr(std::min(c.pos, c.line.size()));
    return r;
  }
  // everything else: values until the line ends
  while (!c.at_end()) {
    auto v = parse_value(c, 0);
    if (!v.ok()) return Result<ImapResponse>(v.error());
    r.args.push_back(std::move(*v));
    if (r.args.size() > 4000000) return Result<ImapResponse>(Status::Exhausted, "a response too long to be real");
  }
  return r;
}

Result<ImapResult> ImapClient::run(const std::string& line, const std::string& payload, bool wait_for_continuation) {
  const std::string tag = next_tag();
  const bool have_payload = !payload.empty() || wait_for_continuation;
  if (wait_for_continuation || !have_payload) {
    if (auto w = stream_.write(tag + " " + line + "\r\n"); !w.ok()) return Result<ImapResult>(w.error());
  } else {
    // LITERAL+ / SASL-IR style: the payload goes in the same write, no round trip
    if (auto w = stream_.write(tag + " " + line + "\r\n" + payload + "\r\n"); !w.ok()) return Result<ImapResult>(w.error());
  }
  bool payload_sent = !wait_for_continuation;
  ImapResult res;
  for (;;) {
    auto ln = stream_.read_line();
    if (!ln.ok()) return Result<ImapResult>(ln.error());
    const std::string& l = *ln;
    if (!l.empty() && l[0] == '+') {
      if (!payload_sent) {
        payload_sent = true;
        if (auto w = stream_.write(payload + "\r\n"); !w.ok()) return Result<ImapResult>(w.error());
      } else {
        // The server wants more than we have (an SASL error blob): an empty line ends the exchange.
        if (auto w = stream_.write("\r\n"); !w.ok()) return Result<ImapResult>(w.error());
      }
      continue;
    }
    if (l.size() >= 2 && l[0] == '*' && l[1] == ' ') {
      auto u = read_untagged(l);
      if (!u.ok()) return Result<ImapResult>(u.error());
      res.untagged.push_back(std::move(*u));
      continue;
    }
    if (l.size() > tag.size() && l.compare(0, tag.size(), tag) == 0 && l[tag.size()] == ' ') {
      Cursor c{stream_, l, tag.size() + 1};
      size_t s = c.pos;
      while (c.pos < l.size() && l[c.pos] != ' ') ++c.pos;
      res.status = upper(l.substr(s, c.pos - s));
      c.skip_spaces();
      if (c.pos < l.size() && l[c.pos] == '[') {
        ++c.pos;
        size_t cs = c.pos;
        while (c.pos < l.size() && l[c.pos] != ' ' && l[c.pos] != ']') ++c.pos;
        res.code = upper(l.substr(cs, c.pos - cs));
        bool first_arg = true;
        for (;;) {
          c.skip_spaces();
          if (c.pos >= l.size() || l[c.pos] == ']') break;
          auto v = parse_value(c, 0);
          if (!v.ok()) break;
          if (first_arg) { res.code_arg = std::move(*v); first_arg = false; }
          else res.code_more.push_back(std::move(*v));
        }
        if (c.pos < l.size() && l[c.pos] == ']') ++c.pos;
      }
      c.skip_spaces();
      res.text = l.substr(std::min(c.pos, l.size()));
      last_text_ = res.text;
      note_caps(res);
      return res;
    }
    // an untagged line without "* ", a blank line, a stale tagged reply: not ours; skip it
  }
}

Result<ImapResult> ImapClient::command(const std::string& line) { return run(line); }

Result<void> ImapClient::check(const Result<ImapResult>& r, const char* what) {
  if (!r.ok()) return Result<void>(r.error());
  if (r->ok()) return Ok();
  Status st = r->status == "BAD" ? Status::Invalid : Status::IoError;
  return Result<void>(st, std::string(what) + ": " + r->status + " " + r->text);
}

Result<void> ImapClient::connect(const std::string& host, uint16_t port, Security security) {
  caps_.clear();
  if (auto r = stream_.connect(host, port, security); !r.ok()) return r;
  auto greet = stream_.read_line();
  if (!greet.ok()) return Result<void>(greet.error());
  if (greet->size() < 2 || (*greet)[0] != '*' || (*greet)[1] != ' ')
    return Result<void>(Status::Corrupt, "not an IMAP server: it greeted with '" + greet->substr(0, 60) + "'");
  auto g = read_untagged(*greet);
  if (!g.ok()) return Result<void>(g.error());
  if (g->kind == "BYE" || g->kind == "NO") return Result<void>(Status::Denied, "the server refused the connection: " + g->text);
  ImapResult tmp;
  tmp.untagged.push_back(*g);
  note_caps(tmp);
  if (caps_.empty()) {
    auto r = run("CAPABILITY");
    if (r.ok()) note_caps(*r);
  }
  if (security == Security::StartTls) {
    if (!has_capability("STARTTLS")) return Result<void>(Status::Unsupported, "the server does not offer STARTTLS");
    auto r = run("STARTTLS");
    if (auto c = check(r, "STARTTLS"); !c.ok()) return c;
    if (auto t = stream_.start_tls(); !t.ok()) return t;
    caps_.clear();
    auto cr = run("CAPABILITY");
    if (cr.ok()) note_caps(*cr);
  }
  return Ok();
}

Result<void> ImapClient::after_auth(ImapResult& r) {
  if (!r.ok()) {
    Status st = r.status == "BAD" ? Status::Invalid : Status::Denied;
    std::string text = r.text;
    // A failed XOAUTH2 carries a JSON blob in a continuation that run() has already answered; the
    // tagged line is what is left.
    return Result<void>(st, "the server refused the login: " + r.status + " " + text);
  }
  if (r.code != "CAPABILITY") {   // capabilities change after authentication
    auto cr = run("CAPABILITY");
    if (cr.ok()) note_caps(*cr);
  }
  return Ok();
}

Result<void> ImapClient::login(const std::string& user, const std::string& password) {
  if (has_capability("LOGINDISABLED") && !stream_.encrypted())
    return Result<void>(Status::Denied, "the server refuses LOGIN on an unencrypted connection");
  if (needs_literal(user) || needs_literal(password)) {
    if (has_capability("AUTH=PLAIN")) return authenticate_plain(user, password);
    return Result<void>(Status::Unsupported, "these credentials hold characters that LOGIN cannot carry and the server has no AUTH=PLAIN");
  }
  auto r = run("LOGIN " + quote(user) + " " + quote(password));
  if (!r.ok()) return Result<void>(r.error());
  return after_auth(*r);
}

Result<void> ImapClient::authenticate_plain(const std::string& user, const std::string& password) {
  std::string blob;
  blob += '\0';
  blob += user;
  blob += '\0';
  blob += password;
  const std::string enc = b64enc(blob);
  auto r = has_capability("SASL-IR") ? run("AUTHENTICATE PLAIN " + enc) : run("AUTHENTICATE PLAIN", enc, true);
  if (!r.ok()) return Result<void>(r.error());
  return after_auth(*r);
}

Result<void> ImapClient::authenticate_xoauth2(const std::string& user, const std::string& access_token) {
  if (!has_capability("AUTH=XOAUTH2") && !has_capability("AUTH=OAUTHBEARER") && !caps_.empty())
    return Result<void>(Status::Unsupported, "the server does not offer XOAUTH2");
  std::string blob = "user=" + user + "\x01" "auth=Bearer " + access_token + "\x01\x01";
  const std::string enc = b64enc(blob);
  auto r = has_capability("SASL-IR") ? run("AUTHENTICATE XOAUTH2 " + enc) : run("AUTHENTICATE XOAUTH2", enc, true);
  if (!r.ok()) return Result<void>(r.error());
  return after_auth(*r);
}

Result<ImapSelect> ImapClient::select(const std::string& mailbox_utf8, bool read_only) {
  auto r = run(std::string(read_only ? "EXAMINE " : "SELECT ") + quote(imap_utf7_encode(mailbox_utf8)));
  if (!r.ok()) return Result<ImapSelect>(r.error());
  if (!r->ok()) return Result<ImapSelect>(r->status == "BAD" ? Status::Invalid : Status::NotFound,
                                          "cannot open " + mailbox_utf8 + ": " + r->text);
  ImapSelect s;
  s.read_only = read_only || r->code == "READ-ONLY";
  for (const auto& u : r->untagged) {
    if (u.kind == "EXISTS" && u.has_number) s.exists = u.number;
    else if (u.kind == "RECENT" && u.has_number) s.recent = u.number;
    else if (u.kind == "FLAGS" && !u.args.empty() && u.args[0].is_list()) {
      for (const auto& f : u.args[0].l) if (f.is_text()) s.flags.push_back(f.s);
    } else if (u.kind == "OK") {
      if (u.code == "UIDVALIDITY") s.uidvalidity = static_cast<uint32_t>(u.code_arg.num());
      else if (u.code == "UIDNEXT") s.uidnext = static_cast<uint32_t>(u.code_arg.num());
      else if (u.code == "HIGHESTMODSEQ") s.highestmodseq = u.code_arg.num();
    }
  }
  return s;
}

Result<uint32_t> ImapClient::uidvalidity(const std::string& mailbox_utf8) {
  auto r = run("STATUS " + quote(imap_utf7_encode(mailbox_utf8)) + " (UIDVALIDITY)");
  if (!r.ok()) return Result<uint32_t>(r.error());
  if (!r->ok()) return Result<uint32_t>(Status::NotFound, "cannot read the status of " + mailbox_utf8 + ": " + r->text);
  for (const auto& u : r->untagged) {
    if (u.kind != "STATUS" || u.args.size() < 2 || !u.args[1].is_list()) continue;
    if (const ImapVal* v = u.args[1].item("UIDVALIDITY")) return static_cast<uint32_t>(v->num());
  }
  return Result<uint32_t>(Status::Corrupt, "the server did not report UIDVALIDITY");
}

Result<std::vector<uint32_t>> ImapClient::uid_search(const std::string& criteria) {
  auto r = run("UID SEARCH " + criteria);
  if (!r.ok()) return Result<std::vector<uint32_t>>(r.error());
  if (!r->ok()) return Result<std::vector<uint32_t>>(r->status == "BAD" ? Status::Invalid : Status::IoError, "search failed: " + r->text);
  std::vector<uint32_t> out;
  for (const auto& u : r->untagged) {
    if (u.kind != "SEARCH") continue;
    for (const auto& a : u.args) if (a.is_text() && all_digits(a.s)) out.push_back(static_cast<uint32_t>(to_u64(a.s)));
  }
  return out;
}

Result<std::vector<ImapFetched>> ImapClient::uid_fetch(const std::string& set, const std::string& items) {
  auto r = run("UID FETCH " + set + " " + items);
  if (!r.ok()) return Result<std::vector<ImapFetched>>(r.error());
  if (!r->ok()) return Result<std::vector<ImapFetched>>(r->status == "BAD" ? Status::Invalid : Status::IoError, "fetch failed: " + r->text);
  std::vector<ImapFetched> out;
  for (const auto& u : r->untagged) {
    if (u.kind != "FETCH" || u.args.empty() || !u.args[0].is_list()) continue;
    const ImapVal& d = u.args[0];
    ImapFetched f;
    f.seq = u.number;
    for (size_t i = 0; i + 1 < d.l.size(); i += 2) {
      if (!d.l[i].is_text()) continue;
      std::string name = upper(d.l[i].s);
      const ImapVal& v = d.l[i + 1];
      if (name == "UID") f.uid = static_cast<uint32_t>(v.num());
      else if (name == "FLAGS") { f.has_flags = true; for (const auto& x : v.l) if (x.is_text()) f.flags.push_back(x.s); }
      else if (name == "INTERNALDATE") f.internaldate = v.s;
      else if (name == "RFC822.SIZE") f.size = v.num();
      else if (name == "X-GM-MSGID") f.gm_msgid = v.s;
      else if (name == "X-GM-THRID") f.gm_thrid = v.s;
      else if (name == "X-GM-LABELS") { for (const auto& x : v.l) if (x.is_text()) f.gm_labels.push_back(x.s); }
      else if (name.compare(0, 4, "BODY") == 0 || name.compare(0, 6, "RFC822") == 0) {
        size_t lt = name.find('<');
        if (lt != std::string::npos) name.erase(lt);
        if (v.is_text()) f.sections[name] = v.s;
      }
    }
    out.push_back(std::move(f));
  }
  return out;
}

Result<void> ImapClient::uid_store(const std::string& set, const std::string& mode, const std::string& flags) {
  return check(run("UID STORE " + set + " " + mode + ".SILENT (" + flags + ")"), "store");
}

Result<uint32_t> ImapClient::append(const std::string& mailbox_utf8, const std::string& flags, const std::string& message) {
  const bool plus = has_capability("LITERAL+");
  std::string line = "APPEND " + quote(imap_utf7_encode(mailbox_utf8));
  if (!flags.empty()) line += " (" + flags + ")";
  line += " {" + std::to_string(message.size()) + (plus ? "+}" : "}");
  auto r = run(line, message, !plus);
  if (!r.ok()) return Result<uint32_t>(r.error());
  if (!r->ok()) return Result<uint32_t>(r->status == "BAD" ? Status::Invalid : Status::IoError, "append failed: " + r->text);
  if (r->code == "APPENDUID" && !r->code_more.empty()) return static_cast<uint32_t>(r->code_more[0].num());
  return static_cast<uint32_t>(0);
}

Result<void> ImapClient::uid_copy(const std::string& set, const std::string& mailbox_utf8) {
  return check(run("UID COPY " + set + " " + quote(imap_utf7_encode(mailbox_utf8))), "copy");
}

Result<void> ImapClient::uid_expunge(const std::string& set) {
  if (has_capability("UIDPLUS")) return check(run("UID EXPUNGE " + set), "expunge");
  return expunge();
}

Result<void> ImapClient::expunge() { return check(run("EXPUNGE"), "expunge"); }

Result<std::vector<ImapMailbox>> ImapClient::list(const std::string& reference, const std::string& pattern) {
  auto r = run("LIST " + quote(reference) + " " + quote(pattern));
  if (!r.ok()) return Result<std::vector<ImapMailbox>>(r.error());
  if (!r->ok()) return Result<std::vector<ImapMailbox>>(Status::IoError, "list failed: " + r->text);
  std::vector<ImapMailbox> out;
  for (const auto& u : r->untagged) {
    if (u.kind != "LIST" || u.args.size() < 3) continue;
    ImapMailbox m;
    if (u.args[0].is_list()) for (const auto& a : u.args[0].l) if (a.is_text()) m.attrs.push_back(a.s);
    m.delimiter = u.args[1].s;
    m.name = imap_utf7_decode(u.args[2].s);
    out.push_back(std::move(m));
  }
  return out;
}

Result<void> ImapClient::create_mailbox(const std::string& mailbox_utf8) {
  return check(run("CREATE " + quote(imap_utf7_encode(mailbox_utf8))), "create");
}

Result<void> ImapClient::noop() { return check(run("NOOP"), "noop"); }

Result<void> ImapClient::logout() {
  auto r = run("LOGOUT");
  stream_.close();
  if (!r.ok()) return Ok();   // a server that hangs up on LOGOUT has still logged us out
  return Ok();
}

}  // namespace aios::net
