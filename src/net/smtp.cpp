// SMTP submission client. See include/aios/net/smtp.hpp.
#include "aios/net/smtp.hpp"

#include <cstring>

#include "aios/core/base64.hpp"
#include "aios/core/bytes.hpp"

namespace aios::net {

namespace {

char uc(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }

std::string upper(std::string_view s) {
  std::string o(s);
  for (auto& c : o) c = uc(c);
  return o;
}

std::string b64enc(std::string_view s) {
  return aios::base64_encode(aios::ByteSpan(reinterpret_cast<const uint8_t*>(s.data()), s.size()));
}

std::string b64dec(std::string_view s) {
  auto r = aios::base64_decode(std::string(s));
  return r.ok() ? std::string(r->begin(), r->end()) : std::string();
}

// An address with anything that would break out of <...> or the command line is refused, not escaped:
// a header-injected "a@b>\r\nRCPT TO:<evil" is the classic way this protocol is abused.
bool address_ok(std::string_view a) {
  if (a.empty() || a.size() > 254) return false;
  for (unsigned char c : a)
    if (c < 0x21 || c == '<' || c == '>' || c == 0x7F) return false;
  return true;
}

}  // namespace

SmtpClient::SmtpClient(hal::Host& host, std::string kernel_root, int timeout_ms, std::string trust_store)
    : stream_(host, std::move(kernel_root), timeout_ms, std::move(trust_store)) {}

bool SmtpClient::has_extension(std::string_view name) const {
  const std::string want = upper(name);
  for (const auto& e : ext_) {
    if (e == want) return true;
    if (e.size() > want.size() && e.compare(0, want.size(), want) == 0 && (e[want.size()] == ' ' || e[want.size()] == '=')) return true;
  }
  return false;
}

Result<SmtpReply> SmtpClient::read_reply() {
  SmtpReply r;
  for (int guard = 0; guard < 1000; ++guard) {
    auto ln = stream_.read_line(8192);
    if (!ln.ok()) return Result<SmtpReply>(ln.error());
    const std::string& l = *ln;
    if (l.size() < 3 || l[0] < '0' || l[0] > '9' || l[1] < '0' || l[1] > '9' || l[2] < '0' || l[2] > '9')
      return Result<SmtpReply>(Status::Corrupt, "not an SMTP reply: '" + l.substr(0, 60) + "'");
    r.code = (l[0] - '0') * 100 + (l[1] - '0') * 10 + (l[2] - '0');
    if (!r.text.empty()) r.text += '\n';
    r.text += l.size() > 4 ? l.substr(4) : std::string();
    if (l.size() == 3 || l[3] == ' ') { last_ = r; return r; }
    if (l[3] != '-') return Result<SmtpReply>(Status::Corrupt, "malformed SMTP reply line: '" + l.substr(0, 60) + "'");
  }
  return Result<SmtpReply>(Status::Exhausted, "an SMTP reply with a thousand lines");
}

Result<SmtpReply> SmtpClient::command(const std::string& line) {
  if (auto w = stream_.write(line + "\r\n"); !w.ok()) return Result<SmtpReply>(w.error());
  return read_reply();
}

Result<void> SmtpClient::ehlo() {
  auto r = command("EHLO " + client_name_);
  if (!r.ok()) return Result<void>(r.error());
  if (r->code != 250) {
    r = command("HELO " + client_name_);   // an old server
    if (!r.ok()) return Result<void>(r.error());
    if (r->code != 250) return Result<void>(Status::IoError, "the server refused EHLO and HELO: " + std::to_string(r->code) + " " + r->text);
    ext_.clear();
    return Ok();
  }
  ext_.clear();
  bool first = true;
  size_t pos = 0;
  while (pos <= r->text.size()) {
    size_t nl = r->text.find('\n', pos);
    std::string line = r->text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    if (!first && !line.empty()) ext_.push_back(upper(line));
    first = false;
    if (nl == std::string::npos) break;
    pos = nl + 1;
  }
  return Ok();
}

Result<void> SmtpClient::connect(const std::string& host, uint16_t port, Security security, const std::string& client_name) {
  ext_.clear();
  client_name_ = client_name;
  if (auto r = stream_.connect(host, port, security); !r.ok()) return r;
  auto greet = read_reply();
  if (!greet.ok()) return Result<void>(greet.error());
  if (greet->code != 220) return Result<void>(Status::Denied, "the server did not accept the connection: " + std::to_string(greet->code) + " " + greet->text);
  if (auto e = ehlo(); !e.ok()) return e;
  if (security == Security::StartTls) {
    if (!has_extension("STARTTLS")) return Result<void>(Status::Unsupported, "the server does not offer STARTTLS");
    auto r = command("STARTTLS");
    if (!r.ok()) return Result<void>(r.error());
    if (r->code != 220) return Result<void>(Status::IoError, "STARTTLS refused: " + std::to_string(r->code) + " " + r->text);
    if (auto t = stream_.start_tls(); !t.ok()) return t;
    if (auto e = ehlo(); !e.ok()) return e;
  }
  return Ok();
}

Result<void> SmtpClient::auth_result(const Result<SmtpReply>& r, const char* what) {
  if (!r.ok()) return Result<void>(r.error());
  if (r->code == 235) return Ok();
  return Result<void>(r->code >= 500 && r->code < 600 ? Status::Denied : Status::IoError,
                      std::string(what) + " refused: " + std::to_string(r->code) + " " + r->text);
}

Result<void> SmtpClient::authenticate_plain(const std::string& user, const std::string& password) {
  std::string blob;
  blob += '\0';
  blob += user;
  blob += '\0';
  blob += password;
  return auth_result(command("AUTH PLAIN " + b64enc(blob)), "AUTH PLAIN");
}

Result<void> SmtpClient::authenticate_login(const std::string& user, const std::string& password) {
  auto r = command("AUTH LOGIN");
  if (!r.ok()) return Result<void>(r.error());
  if (r->code != 334) return auth_result(r, "AUTH LOGIN");
  r = command(b64enc(user));
  if (!r.ok()) return Result<void>(r.error());
  if (r->code != 334) return auth_result(r, "AUTH LOGIN");
  return auth_result(command(b64enc(password)), "AUTH LOGIN");
}

Result<void> SmtpClient::authenticate_xoauth2(const std::string& user, const std::string& access_token) {
  std::string blob = "user=" + user + "\x01" "auth=Bearer " + access_token + "\x01\x01";
  auto r = command("AUTH XOAUTH2 " + b64enc(blob));
  if (!r.ok()) return Result<void>(r.error());
  if (r->code == 334) {
    // The server explains the failure in a JSON blob and waits for an empty line to end the exchange.
    std::string why = b64dec(r->text);
    auto fin = command("");
    if (!fin.ok()) return Result<void>(fin.error());
    return Result<void>(Status::Denied, "XOAUTH2 refused: " + std::to_string(fin->code) + " " + fin->text + (why.empty() ? "" : " (" + why + ")"));
  }
  return auth_result(r, "AUTH XOAUTH2");
}

Result<void> SmtpClient::authenticate(const std::string& user, const std::string& password) {
  if (has_mechanism("PLAIN")) return authenticate_plain(user, password);
  if (has_mechanism("LOGIN")) return authenticate_login(user, password);
  return Result<void>(Status::Unsupported, "the server offers no password mechanism (PLAIN or LOGIN)");
}

bool SmtpClient::has_mechanism(std::string_view mech) const {
  const std::string want = upper(mech);
  for (const auto& e : ext_) {
    // "AUTH PLAIN LOGIN XOAUTH2" and the old "AUTH=PLAIN LOGIN"
    if (e.compare(0, 4, "AUTH") != 0 || e.size() < 6 || (e[4] != ' ' && e[4] != '=')) continue;
    size_t pos = 5;
    while (pos <= e.size()) {
      size_t sp = e.find_first_of(" =", pos);
      std::string m = e.substr(pos, sp == std::string::npos ? std::string::npos : sp - pos);
      if (m == want) return true;
      if (sp == std::string::npos) break;
      pos = sp + 1;
    }
  }
  return false;
}

Result<void> SmtpClient::send(const std::string& from, const std::vector<std::string>& recipients, const std::string& message) {
  if (!address_ok(from)) return Result<void>(Status::Invalid, "the sender address is not one SMTP can carry");
  if (recipients.empty()) return Result<void>(Status::Invalid, "no recipients");
  for (const auto& r : recipients)
    if (!address_ok(r)) return Result<void>(Status::Invalid, "a recipient address is not one SMTP can carry: " + r.substr(0, 60));

  bool eight_bit = false;
  for (unsigned char c : message)
    if (c >= 0x80) { eight_bit = true; break; }
  if (eight_bit && !has_extension("8BITMIME") && !has_extension("SMTPUTF8"))
    return Result<void>(Status::Unsupported, "the message has 8-bit text and the server does not take 8BITMIME");
  if (has_extension("SIZE")) {
    for (const auto& e : ext_) {
      if (e.compare(0, 4, "SIZE") != 0) continue;
      size_t sp = e.find_first_of(" =");
      if (sp != std::string::npos) {
        unsigned long long lim = 0;
        for (size_t i = sp + 1; i < e.size() && e[i] >= '0' && e[i] <= '9'; ++i) lim = lim * 10 + static_cast<unsigned>(e[i] - '0');
        if (lim && message.size() > lim) return Result<void>(Status::Exhausted, "the message is larger than the server accepts");
      }
    }
  }

  std::string mail = "MAIL FROM:<" + from + ">";
  if (eight_bit && has_extension("8BITMIME")) mail += " BODY=8BITMIME";
  if (has_extension("SIZE")) mail += " SIZE=" + std::to_string(message.size());
  auto r = command(mail);
  if (!r.ok()) return Result<void>(r.error());
  if (r->code != 250) return Result<void>(r->code >= 500 ? Status::Invalid : Status::Busy, "MAIL FROM refused: " + std::to_string(r->code) + " " + r->text);
  size_t accepted = 0;
  std::string rejected;
  for (const auto& rc : recipients) {
    r = command("RCPT TO:<" + rc + ">");
    if (!r.ok()) return Result<void>(r.error());
    if (r->code == 250 || r->code == 251) ++accepted;
    else rejected += (rejected.empty() ? "" : "; ") + rc + ": " + std::to_string(r->code) + " " + r->text;
  }
  if (accepted == 0) {
    (void)command("RSET");
    return Result<void>(Status::Invalid, "every recipient was refused: " + rejected);
  }
  r = command("DATA");
  if (!r.ok()) return Result<void>(r.error());
  if (r->code != 354) return Result<void>(Status::IoError, "DATA refused: " + std::to_string(r->code) + " " + r->text);

  // Normalise to CRLF, double a leading dot on any line, end with CRLF "." CRLF.
  std::string wire;
  wire.reserve(message.size() + message.size() / 40 + 8);
  bool line_start = true;
  for (size_t i = 0; i < message.size(); ++i) {
    char c = message[i];
    if (c == '\r') {
      if (i + 1 < message.size() && message[i + 1] == '\n') ++i;
      wire += "\r\n";
      line_start = true;
      continue;
    }
    if (c == '\n') { wire += "\r\n"; line_start = true; continue; }
    if (line_start && c == '.') wire += '.';
    wire += c;
    line_start = false;
  }
  if (wire.size() < 2 || wire.compare(wire.size() - 2, 2, "\r\n") != 0) wire += "\r\n";
  wire += ".\r\n";
  if (auto w = stream_.write(wire); !w.ok()) return Result<void>(w.error());
  r = read_reply();
  if (!r.ok()) return Result<void>(r.error());
  if (r->code != 250) return Result<void>(r->code >= 500 ? Status::Invalid : Status::Busy, "the server did not accept the message: " + std::to_string(r->code) + " " + r->text);
  if (!rejected.empty()) return Result<void>(Status::Conflict, "sent, but some recipients were refused: " + rejected);
  return Ok();
}

Result<void> SmtpClient::quit() {
  auto r = command("QUIT");
  stream_.close();
  (void)r;
  return Ok();
}

}  // namespace aios::net
