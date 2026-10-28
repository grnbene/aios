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

#include "aios/hal/host.hpp"
#include "aios/net/line_stream.hpp"
#include "aios/net/mail_message.hpp"
#include "aios/net/smtp.hpp"
#include "aios/userland/grain.hpp"
#include "grain_builtins_internal.hpp"

namespace aios::userland::grain {

namespace {

bool has_break(const std::string& s) { return net::mail_has_break(s); }
bool plain_address(const std::string& a) { return net::mail_plain_address(a); }

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

  std::vector<net::MailAttachment> files;
  if (Value* v = m.find("attachments"); v && v->type != Type::Nil) {
    if (v->type != Type::List) { error = "mail_send(): attachments is a list of {filename, data, type}"; return Value::nil(); }
    for (const Value& it : v->list_items()) {
      if (it.type != Type::Map) { error = "mail_send(): each attachment is a map {filename, data, type}"; return Value::nil(); }
      MapObj& a = *static_cast<MapObj*>(it.obj.get());
      net::MailAttachment f;
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
  net::MailParts parts;
  parts.from = from;
  parts.from_name = from_name;
  parts.reply_to = reply_to;
  parts.subject = subject;
  parts.text = text;
  parts.html = html;
  parts.to = to;
  parts.cc = cc;
  parts.bcc = bcc;
  parts.attachments = std::move(files);
  std::string message_id, build_err;
  const std::string msg = net::mail_build_message(parts, &message_id, &build_err);
  if (msg.empty()) { error = "mail_send(): " + build_err; return Value::nil(); }
  const std::vector<std::string> rcpts = net::mail_recipients(parts);   // bcc delivered to, never named in a header

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
