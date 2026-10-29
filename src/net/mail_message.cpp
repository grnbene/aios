#include "aios/net/mail_message.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>

#include "aios/core/base64.hpp"
#include "aios/hal/host.hpp"

namespace aios::net {

namespace {

bool is_ascii(const std::string& s) {
  for (unsigned char c : s) if (c >= 0x80) return false;
  return true;
}

std::string b64(const std::string& s) { return base64_encode(ByteSpan(reinterpret_cast<const uint8_t*>(s.data()), s.size())); }
std::string b64_lines(const std::string& s) {
  const std::string e = b64(s);
  std::string out;
  for (size_t i = 0; i < e.size(); i += 76) out += e.substr(i, 76) + "\r\n";
  return out;
}

// RFC 5322 §2.1.1: a line of at most 78 characters where it can be had, never more than 998.
constexpr size_t kSoftLine = 78;
constexpr size_t kHardLine = 998;

// `s` folded at spaces so that no line, the first of them `prefix` characters longer, exceeds 78 where a
// space allows it; a run with no space in it goes on a line of its own. Empty when some run would still
// exceed the hard limit (the caller then encodes instead).
std::string fold_at_spaces(const std::string& s, size_t prefix) {
  std::string out;
  size_t line = prefix, at = 0;
  while (at < s.size()) {
    size_t sp = s.find(' ', at);
    if (sp == std::string::npos) sp = s.size();
    const size_t len = sp - at;
    if (len + 1 > kHardLine) return {};
    if (!out.empty()) {
      if (line + 1 + len > kSoftLine) { out += "\r\n "; line = 1; }
      else { out += ' '; ++line; }
    }
    out.append(s, at, len);
    line += len;
    at = sp + 1;
  }
  return out;
}

// A header value: as it is when ASCII and short, folded at spaces when ASCII and long, else RFC 2047
// encoded words of at most 45 bytes (split on UTF-8 boundaries). `prefix` is the length of "Name: ".
std::string header_text(const std::string& s, size_t prefix = 0) {
  if (is_ascii(s)) {
    if (prefix + s.size() <= kSoftLine) return s;
    std::string folded = fold_at_spaces(s, prefix);
    if (!folded.empty()) return folded;
  }
  std::string out;
  size_t i = 0;
  while (i < s.size()) {
    size_t j = std::min(s.size(), i + 45);
    while (j < s.size() && (static_cast<unsigned char>(s[j]) & 0xC0) == 0x80) --j;
    if (!out.empty()) out += "\r\n ";
    out += "=?UTF-8?B?" + b64(s.substr(i, j - i)) + "?=";
    i = j;
  }
  return out;
}

// `"Name" <addr>` (the name quoted or encoded), or the bare address.
std::string mailbox(const std::string& name, const std::string& addr) {
  if (name.empty()) return addr;
  if (!is_ascii(name)) return header_text(name) + " <" + addr + ">";
  std::string q = "\"";
  for (char c : name) { if (c == '"' || c == '\\') q += '\\'; q += c; }
  return q + "\" <" + addr + ">";
}

std::string rfc5322_date(std::time_t t) {
  static const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  std::tm g{};
#if defined(_WIN32)
  gmtime_s(&g, &t);
#else
  gmtime_r(&t, &g);
#endif
  char buf[64];
  std::snprintf(buf, sizeof buf, "%s, %02d %s %04d %02d:%02d:%02d +0000", days[g.tm_wday], g.tm_mday, months[g.tm_mon], g.tm_year + 1900,
                g.tm_hour, g.tm_min, g.tm_sec);
  return buf;
}

std::string random_hex(size_t bytes) {
  std::vector<uint8_t> r(bytes);
  hal::process_host().random(MutableByteSpan(r.data(), r.size()));
  static const char* digits = "0123456789abcdef";
  std::string out;
  for (uint8_t b : r) { out += digits[b >> 4]; out += digits[b & 15]; }
  return out;
}

// A text part: 7bit when ASCII with SMTP-safe lines, else base64.
std::string text_part(const std::string& type, const std::string& body) {
  bool seven = is_ascii(body);
  size_t line = 0;
  for (char c : body) { line = (c == '\n') ? 0 : line + 1; if (line > 900) { seven = false; break; } }
  std::string out = "Content-Type: " + type + "; charset=utf-8\r\n";
  if (seven) return out + "Content-Transfer-Encoding: 7bit\r\n\r\n" + body + "\r\n";
  return out + "Content-Transfer-Encoding: base64\r\n\r\n" + b64_lines(body);
}

// RFC 2231 for a non-ASCII filename (filename*=UTF-8''...), a quoted one otherwise.
std::string filename_param(const std::string& name) {
  if (is_ascii(name)) {
    std::string q;
    for (char c : name) { if (c == '"' || c == '\\') q += '\\'; q += c; }
    return "filename=\"" + q + "\"";
  }
  std::string out = "filename*=UTF-8''";
  static const char* hexd = "0123456789ABCDEF";
  for (unsigned char c : name) {
    if (std::isalnum(c) || c == '.' || c == '-' || c == '_') out += static_cast<char>(c);
    else { out += '%'; out += hexd[c >> 4]; out += hexd[c & 15]; }
  }
  return out;
}

}  // namespace

bool mail_has_break(const std::string& s) { return s.find_first_of("\r\n") != std::string::npos; }

bool mail_plain_address(const std::string& a) {
  const size_t at = a.find('@');
  if (at == std::string::npos || at == 0 || at + 1 >= a.size() || a.find('@', at + 1) != std::string::npos) return false;
  for (unsigned char c : a) if (c <= ' ' || c == '<' || c == '>' || c == ',' || c == ';' || c == '"' || c == 0x7F) return false;
  return true;
}

std::vector<std::string> mail_recipients(const MailParts& m) {
  std::vector<std::string> r = m.to;
  r.insert(r.end(), m.cc.begin(), m.cc.end());
  r.insert(r.end(), m.bcc.begin(), m.bcc.end());
  return r;
}

std::string mail_build_message(const MailParts& m, std::string* message_id, std::string* err) {
  if (!mail_plain_address(m.from)) { *err = "from must be a plain address (name@domain); from_name gives the display name"; return {}; }
  if (!m.reply_to.empty() && !mail_plain_address(m.reply_to)) { *err = "reply_to must be a plain address"; return {}; }
  for (const auto* list : {&m.to, &m.cc, &m.bcc})
    for (const std::string& a : *list)
      if (!mail_plain_address(a)) { *err = "\"" + a + "\" is not a plain address (name@domain)"; return {}; }
  if (m.to.empty() && m.cc.empty() && m.bcc.empty()) { *err = "no recipients (to, cc or bcc)"; return {}; }
  if (m.text.empty() && m.html.empty()) { *err = "no body (text and/or html)"; return {}; }
  if (mail_has_break(m.from_name) || mail_has_break(m.subject)) { *err = "from_name and subject cannot hold a line break"; return {}; }

  const std::string domain = m.from.substr(m.from.find('@') + 1);
  *message_id = "<" + random_hex(16) + "@" + domain + ">";
  // An address list is folded after a comma: each line under 78 where the addresses allow it.
  auto list_header = [](const char* name, const std::vector<std::string>& v) {
    std::string o = std::string(name) + ": ";
    size_t line = o.size();
    for (size_t i = 0; i < v.size(); ++i) {
      const std::string item = v[i] + (i + 1 < v.size() ? "," : "");
      if (i > 0) {
        if (line + 1 + item.size() > kSoftLine) { o += "\r\n "; line = 1; }
        else { o += ' '; ++line; }
      }
      o += item;
      line += item.size();
    }
    return o + "\r\n";
  };
  std::string msg;
  msg += "From: " + mailbox(m.from_name, m.from) + "\r\n";
  if (!m.to.empty()) msg += list_header("To", m.to);
  if (!m.cc.empty()) msg += list_header("Cc", m.cc);
  if (!m.reply_to.empty()) msg += "Reply-To: " + m.reply_to + "\r\n";
  msg += "Subject: " + header_text(m.subject, 9) + "\r\n";
  msg += "Date: " + rfc5322_date(std::time(nullptr)) + "\r\n";
  msg += "Message-ID: " + *message_id + "\r\n";
  msg += "MIME-Version: 1.0\r\n";
  // The readable part: text, html, or both as alternatives (text first: the last is the preferred).
  std::string body_part;
  if (!m.text.empty() && !m.html.empty()) {
    const std::string b = "alt-" + random_hex(12);
    body_part = "Content-Type: multipart/alternative; boundary=\"" + b + "\"\r\n\r\n";
    body_part += "--" + b + "\r\n" + text_part("text/plain", m.text);
    body_part += "--" + b + "\r\n" + text_part("text/html", m.html);
    body_part += "--" + b + "--\r\n";
  } else {
    body_part = m.html.empty() ? text_part("text/plain", m.text) : text_part("text/html", m.html);
  }
  if (m.attachments.empty()) {
    msg += body_part;
  } else {
    const std::string b = "mix-" + random_hex(12);
    msg += "Content-Type: multipart/mixed; boundary=\"" + b + "\"\r\n\r\n";
    msg += "--" + b + "\r\n" + body_part;
    for (const auto& f : m.attachments) {
      msg += "--" + b + "\r\n";
      msg += "Content-Type: " + f.type + "\r\n";
      msg += "Content-Disposition: attachment; " + filename_param(f.filename) + "\r\n";
      msg += "Content-Transfer-Encoding: base64\r\n\r\n" + b64_lines(f.data);
    }
    msg += "--" + b + "--\r\n";
  }
  return msg;
}

}  // namespace aios::net
