// mail_send: email from a grain program through aios's own SMTP client (src/net/smtp.cpp: the kernel's sockets and
// TLS, STARTTLS, AUTH PLAIN/LOGIN, dot-stuffing). The program gives the message as a map; this builds the RFC 5322 /
// MIME text: plain text, an HTML alternative, attachments, UTF-8 subjects and names as RFC 2047 encoded words,
// filenames as RFC 2231. Nothing a caller passes can add a header line: addresses and header values with a line
// break are refused.
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include "aios/core/base64.hpp"
#include "aios/hal/host.hpp"
#include "aios/net/line_stream.hpp"
#include "aios/net/smtp.hpp"
#include "aios/userland/grain.hpp"
#include "grain_builtins_internal.hpp"

namespace aios::userland::grain {

namespace {

bool is_ascii(const std::string& s) {
  for (unsigned char c : s) if (c >= 0x80) return false;
  return true;
}
bool has_break(const std::string& s) { return s.find_first_of("\r\n") != std::string::npos; }

std::string b64(const std::string& s) { return base64_encode(ByteSpan(reinterpret_cast<const uint8_t*>(s.data()), s.size())); }
std::string b64_lines(const std::string& s) {
  const std::string e = b64(s);
  std::string out;
  for (size_t i = 0; i < e.size(); i += 76) out += e.substr(i, 76) + "\r\n";
  return out;
}

// A header value: as it is when ASCII, else RFC 2047 encoded words of at most 45 bytes (split on UTF-8 boundaries).
std::string header_text(const std::string& s) {
  if (is_ascii(s)) return s;
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

// A bare address: one @, something on both sides, no spaces, brackets or line breaks.
bool plain_address(const std::string& a) {
  const size_t at = a.find('@');
  if (at == std::string::npos || at == 0 || at + 1 >= a.size() || a.find('@', at + 1) != std::string::npos) return false;
  for (unsigned char c : a) if (c <= ' ' || c == '<' || c == '>' || c == ',' || c == ';' || c == '"' || c == 0x7F) return false;
  return true;
}

// to / cc / bcc: a string or a list of strings, each a bare address.
bool addresses(MapObj& m, const char* key, std::vector<std::string>& out, std::string& error) {
  Value* v = m.find(key);
  if (!v || v->type == Type::Nil) return true;
  std::vector<Value> items;
  if (v->type == Type::Str) items.push_back(*v);
  else if (v->type == Type::List) items = v->list_items();
  else { error = std::string("mail_send(): ") + key + " is an address or a list of them"; return false; }
  for (const Value& it : items) {
    if (it.type != Type::Str || !plain_address(it.str())) {
      error = std::string("mail_send(): ") + key + " has \"" + it.to_text() + "\", not a plain address (name@domain)";
      return false;
    }
    out.push_back(it.str());
  }
  return true;
}

bool text_field(MapObj& m, const char* key, std::string& out, std::string& error, bool one_line) {
  Value* v = m.find(key);
  if (!v || v->type == Type::Nil) return true;
  if (v->type != Type::Str) { error = std::string("mail_send(): ") + key + " is a string"; return false; }
  if (one_line && has_break(v->str())) { error = std::string("mail_send(): ") + key + " cannot hold a line break"; return false; }
  out = v->str();
  return true;
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

struct Attachment { std::string filename, type, data; };

// mail_send({host, port, security, user, password, from, from_name, to, cc, bcc, reply_to, subject, text, html,
//            attachments: [{filename, data, type}], timeout_ms})
Value bi_mail_send(Vm&, std::vector<Value>& args, std::string& error) {
  if (args[0].type != Type::Map) { error = "mail_send() wants one map: {host, from, to, subject, text, ...}"; return Value::nil(); }
  MapObj& m = *static_cast<MapObj*>(args[0].obj.get());

  std::string host, security_s, user, password, from, from_name, reply_to, subject, text, html;
  if (!text_field(m, "host", host, error, true) || !text_field(m, "security", security_s, error, true) || !text_field(m, "user", user, error, true) ||
      !text_field(m, "password", password, error, true) || !text_field(m, "from", from, error, true) ||
      !text_field(m, "from_name", from_name, error, true) || !text_field(m, "reply_to", reply_to, error, true) ||
      !text_field(m, "subject", subject, error, true) || !text_field(m, "text", text, error, false) || !text_field(m, "html", html, error, false))
    return Value::nil();
  if (host.empty()) { error = "mail_send(): host is the SMTP server to send through"; return Value::nil(); }
  if (!plain_address(from)) { error = "mail_send(): from must be a plain address (name@domain); from_name gives the display name"; return Value::nil(); }
  if (!reply_to.empty() && !plain_address(reply_to)) { error = "mail_send(): reply_to must be a plain address"; return Value::nil(); }
  std::vector<std::string> to, cc, bcc;
  if (!addresses(m, "to", to, error) || !addresses(m, "cc", cc, error) || !addresses(m, "bcc", bcc, error)) return Value::nil();
  if (to.empty() && cc.empty() && bcc.empty()) { error = "mail_send(): no recipients (to, cc or bcc)"; return Value::nil(); }
  if (text.empty() && html.empty()) { error = "mail_send(): no body (text and/or html)"; return Value::nil(); }

  int port = 587;
  if (Value* v = m.find("port")) {
    if (!want_number(*v, error, "mail_send")) return Value::nil();
    if (v->as_int() < 1 || v->as_int() > 65535) { error = "mail_send(): port is 1 to 65535"; return Value::nil(); }
    port = static_cast<int>(v->as_int());
  }
  // The port's usual security unless told: 465 TLS from the start, anything else STARTTLS.
  net::Security security = port == 465 ? net::Security::Tls : net::Security::StartTls;
  if (security_s == "tls") security = net::Security::Tls;
  else if (security_s == "starttls") security = net::Security::StartTls;
  else if (security_s == "none") security = net::Security::None;
  else if (!security_s.empty()) { error = "mail_send(): security is \"starttls\", \"tls\" or \"none\""; return Value::nil(); }
  const bool loopback = host == "127.0.0.1" || host == "localhost" || host == "::1";
  if (security == net::Security::None && !password.empty() && !loopback) {
    error = "mail_send(): a password is never sent unencrypted (security \"none\") to another machine";
    return Value::nil();
  }
  int timeout_ms = 30000;
  if (Value* v = m.find("timeout_ms")) {
    if (!want_number(*v, error, "mail_send")) return Value::nil();
    if (v->as_int() <= 0) { error = "mail_send(): timeout_ms must be positive"; return Value::nil(); }
    timeout_ms = static_cast<int>(std::min<int64_t>(v->as_int(), 600000));
  }

  std::vector<Attachment> files;
  if (Value* v = m.find("attachments"); v && v->type != Type::Nil) {
    if (v->type != Type::List) { error = "mail_send(): attachments is a list of {filename, data, type}"; return Value::nil(); }
    for (const Value& it : v->list_items()) {
      if (it.type != Type::Map) { error = "mail_send(): each attachment is a map {filename, data, type}"; return Value::nil(); }
      MapObj& a = *static_cast<MapObj*>(it.obj.get());
      Attachment f;
      if (!text_field(a, "filename", f.filename, error, true) || !text_field(a, "type", f.type, error, true)) return Value::nil();
      Value* d = a.find("data");
      if (!d) { error = "mail_send(): attachment " + f.filename + " has no data"; return Value::nil(); }
      if (d->type == Type::Str) f.data = d->str();
      else if (d->type == Type::Bytes) f.data.assign(d->bytes_data().begin(), d->bytes_data().end());
      else { error = "mail_send(): an attachment's data is a string or bytes"; return Value::nil(); }
      if (f.filename.empty()) f.filename = "attachment.bin";
      if (f.type.empty()) f.type = "application/octet-stream";
      files.push_back(std::move(f));
    }
  }

  // The message.
  const std::string domain = from.substr(from.find('@') + 1);
  const std::string message_id = "<" + random_hex(16) + "@" + domain + ">";
  auto join = [](const std::vector<std::string>& v) { std::string o; for (const auto& x : v) { if (!o.empty()) o += ", "; o += x; } return o; };
  std::string msg;
  msg += "From: " + mailbox(from_name, from) + "\r\n";
  if (!to.empty()) msg += "To: " + join(to) + "\r\n";
  if (!cc.empty()) msg += "Cc: " + join(cc) + "\r\n";
  if (!reply_to.empty()) msg += "Reply-To: " + reply_to + "\r\n";
  msg += "Subject: " + header_text(subject) + "\r\n";
  msg += "Date: " + rfc5322_date(std::time(nullptr)) + "\r\n";
  msg += "Message-ID: " + message_id + "\r\n";
  msg += "MIME-Version: 1.0\r\n";
  // The readable part: text, html, or both as alternatives (text first: the last is the preferred).
  std::string body_part;
  if (!text.empty() && !html.empty()) {
    const std::string b = "alt-" + random_hex(12);
    body_part = "Content-Type: multipart/alternative; boundary=\"" + b + "\"\r\n\r\n";
    body_part += "--" + b + "\r\n" + text_part("text/plain", text);
    body_part += "--" + b + "\r\n" + text_part("text/html", html);
    body_part += "--" + b + "--\r\n";
  } else {
    body_part = html.empty() ? text_part("text/plain", text) : text_part("text/html", html);
  }
  if (files.empty()) {
    msg += body_part;
  } else {
    const std::string b = "mix-" + random_hex(12);
    msg += "Content-Type: multipart/mixed; boundary=\"" + b + "\"\r\n\r\n";
    msg += "--" + b + "\r\n" + body_part;
    for (const auto& f : files) {
      msg += "--" + b + "\r\n";
      msg += "Content-Type: " + f.type + "\r\n";
      msg += "Content-Disposition: attachment; " + filename_param(f.filename) + "\r\n";
      msg += "Content-Transfer-Encoding: base64\r\n\r\n" + b64_lines(f.data);
    }
    msg += "--" + b + "--\r\n";
  }

  std::vector<std::string> rcpts = to;
  rcpts.insert(rcpts.end(), cc.begin(), cc.end());
  rcpts.insert(rcpts.end(), bcc.begin(), bcc.end());   // delivered to, never named in a header

  net::SmtpClient smtp(hal::process_host(), http_trust_root(), timeout_ms);
  auto fail = [&](const char* step, const Error& e) {
    smtp.close();
    error = "mail_send(): " + std::string(step) + " " + host + ":" + std::to_string(port) + ": " + e.message;
    return Value::nil();
  };
  if (auto c = smtp.connect(host, static_cast<uint16_t>(port), security, domain); !c.ok()) return fail("connecting to", c.error());
  if (!user.empty()) {
    if (auto a = smtp.authenticate(user, password); !a.ok()) return fail("signing in at", a.error());
  }
  if (auto s = smtp.send(from, rcpts, msg); !s.ok()) return fail("sending through", s.error());
  (void)smtp.quit();
  smtp.close();

  auto out = std::make_shared<MapObj>();
  out->put("ok", Value::boolean(true));
  out->put("message_id", Value::string(message_id));
  out->put("recipients", Value::integer(static_cast<int64_t>(rcpts.size())));
  return Value::object(Type::Map, out);
}

}  // namespace

void install_mail_builtins(Vm& vm) {
  vm.define_native("mail_send", 1, 1, bi_mail_send);
}

}  // namespace aios::userland::grain
