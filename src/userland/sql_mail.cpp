#include "aios/userland/sql_mail.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "aios/core/bytes.hpp"
#include "aios/hal/host.hpp"
#include "aios/net/smtp.hpp"
#include "aios/userland/oauth.hpp"

namespace aios::userland {

namespace {

std::string lower_s(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
std::string upper_s(std::string s) { for (auto& c : s) c = (char)std::toupper((unsigned char)c); return s; }
std::string trim_s(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace((unsigned char)s[b])) ++b;
  while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
  return s.substr(b, e - b);
}

bool ident_ok(const std::string& n) {
  if (n.empty() || n.size() > 64) return false;
  for (unsigned char c : n) if (!(std::isalnum(c) || c == '_')) return false;
  return true;
}

bool loopback(const std::string& h) { return h == "127.0.0.1" || h == "localhost" || h == "::1"; }

struct Tok {
  std::string text;
  bool quoted = false;
};

// Words and quoted strings (single or double quotes; the quote doubled or \<quote> is the quote itself).
bool tokens(const std::string& s, std::vector<Tok>* out, std::string* err) {
  size_t i = 0;
  while (i < s.size()) {
    if (std::isspace((unsigned char)s[i])) { ++i; continue; }
    Tok t;
    if (s[i] == '\'' || s[i] == '"') {
      const char q = s[i++];
      bool closed = false;
      for (; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '\\' && i + 1 < s.size() && (s[i + 1] == q || s[i + 1] == '\\')) { t.text += s[++i]; continue; }
        if (c == q) {
          if (i + 1 < s.size() && s[i + 1] == q) { t.text += q; ++i; continue; }
          ++i;
          closed = true;
          break;
        }
        t.text += c;
      }
      if (!closed) { *err = "a quoted string is not closed"; return false; }
      t.quoted = true;
    } else {
      while (i < s.size() && !std::isspace((unsigned char)s[i])) t.text += s[i++];
    }
    out->push_back(std::move(t));
  }
  return true;
}

std::string now_text() {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char b[32];
  std::strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &tm);
  return b;
}

}  // namespace

std::string MailerStore::Mailer::auth_text() const {
  if (!oauth_provider.empty()) return "oauth " + oauth_provider + " " + oauth_account;
  if (!user.empty()) return "password of " + user + " (secret " + password_secret + ")";
  return "none";
}

bool MailerStore::handles(const std::string& h) { return h == "CREATE MAILER" || h == "DROP MAILER" || h == "SHOW MAILERS"; }

// CREATE MAILER <name> followed by keyword/value pairs in any order.
bool MailerStore::parse(const std::string& sql, Mailer* m, std::string* err) {
  std::string q = trim_s(sql);
  while (!q.empty() && q.back() == ';') q = trim_s(q.substr(0, q.size() - 1));
  std::vector<Tok> t;
  if (!tokens(q, &t, err)) return false;
  const char* usage =
      "CREATE MAILER <name> OAUTH '<provider>' ACCOUNT '<email>' [HOST '<host>'] [PORT <n>] [SECURITY '<mode>'] [FROM '<address>'] [FROM_NAME '<name>'] "
      "| USER '<user>' PASSWORD SECRET <secret> HOST '<host>' FROM '<address>' | HOST '127.0.0.1' FROM '<address>'";
  if (t.size() < 3 || upper_s(t[0].text) != "CREATE" || upper_s(t[1].text) != "MAILER") { *err = usage; return false; }
  m->name = t[2].text;
  if (t[2].quoted || !ident_ok(m->name)) { *err = "a mailer's name is letters, digits and _ (up to 64)"; return false; }
  m->declaration = q;
  bool port_set = false;
  for (size_t i = 3; i < t.size();) {
    const std::string kw = upper_s(t[i].text);
    if (t[i].quoted) { *err = "unexpected '" + t[i].text + "'; " + usage; return false; }
    if (kw == "PASSWORD") {
      if (i + 2 >= t.size() || upper_s(t[i + 1].text) != "SECRET" || t[i + 2].quoted) { *err = "PASSWORD SECRET <secret name>: a password is never written in the statement"; return false; }
      m->password_secret = t[i + 2].text;
      i += 3;
      continue;
    }
    if (i + 1 >= t.size()) { *err = kw + " needs a value"; return false; }
    const std::string v = t[i + 1].text;
    if (kw == "HOST") m->host = v;
    else if (kw == "PORT") {
      char* end = nullptr;
      const long p = std::strtol(v.c_str(), &end, 10);
      if (v.empty() || *end || p < 1 || p > 65535) { *err = "PORT is 1 to 65535"; return false; }
      m->port = (int)p;
      port_set = true;
    } else if (kw == "SECURITY") m->security = lower_s(v);
    else if (kw == "OAUTH") m->oauth_provider = lower_s(v);
    else if (kw == "ACCOUNT") m->oauth_account = v;
    else if (kw == "USER") m->user = v;
    else if (kw == "FROM") m->from = v;
    else if (kw == "FROM_NAME") m->from_name = v;
    else { *err = "unknown '" + t[i].text + "'; " + usage; return false; }
    i += 2;
  }
  if (!m->security.empty() && m->security != "starttls" && m->security != "tls" && m->security != "none") { *err = "SECURITY is 'starttls', 'tls' or 'none'"; return false; }
  if (net::mail_has_break(m->from_name)) { *err = "FROM_NAME cannot hold a line break"; return false; }

  if (!m->oauth_provider.empty()) {
    if (!m->user.empty() || !m->password_secret.empty()) { *err = "OAUTH and USER/PASSWORD are two ways to sign in: use one"; return false; }
    if (!net::mail_plain_address(m->oauth_account)) { *err = "OAUTH '<provider>' needs ACCOUNT '<email>' (the signed-in account)"; return false; }
    m->user = m->oauth_account;
    if (m->host.empty() && m->oauth_provider == "google") m->host = "smtp.gmail.com";
    if (m->host.empty()) { *err = "OAUTH '" + m->oauth_provider + "' needs HOST '<smtp host>'"; return false; }
    if (m->from.empty()) m->from = m->oauth_account;
  } else if (!m->user.empty() || !m->password_secret.empty()) {
    if (m->user.empty() || m->password_secret.empty()) { *err = "a password sign-in needs both USER '<user>' and PASSWORD SECRET <secret>"; return false; }
    if (m->host.empty()) { *err = "USER/PASSWORD needs HOST '<smtp host>'"; return false; }
    if (!ident_ok(m->password_secret)) { *err = "PASSWORD SECRET takes a secret's name"; return false; }
  } else {
    if (m->host.empty() || !loopback(m->host)) { *err = "a mailer with no sign-in sends only through this machine (HOST '127.0.0.1'); give OAUTH or USER/PASSWORD for a server"; return false; }
  }
  if (!net::mail_plain_address(m->from)) { *err = "FROM '<address>' must be a plain address (name@domain)"; return false; }
  if (m->security.empty()) {
    if (m->user.empty()) m->security = "none";
    else m->security = (port_set && m->port == 465) ? "tls" : "starttls";
  }
  if (!port_set) m->port = m->security == "tls" ? 465 : (m->user.empty() ? 25 : 587);
  if (m->security == "none" && !m->user.empty() && !loopback(m->host)) { *err = "a password or token is never sent without TLS to another machine (SECURITY 'none')"; return false; }
  m->created = now_text();
  return true;
}

void MailerStore::save(const Mailer& m) {
  host_.make_dir(dir_);
  std::string text = m.created + "\n" + m.declaration + "\n";
  host_.write_file(dir_ + "/" + m.name + ".mailer", aios::span_of(text));
}

void MailerStore::load() {
  std::lock_guard<std::mutex> lk(mu_);
  mailers_.clear();
  if (!host_.exists(dir_)) return;
  auto names = host_.list_dir(dir_);
  if (!names.ok()) return;
  for (const std::string& n : *names) {
    if (n.size() < 9 || n.compare(n.size() - 7, 7, ".mailer") != 0) continue;
    auto f = host_.read_file(dir_ + "/" + n);
    if (!f.ok()) continue;
    const std::string text = aios::string_of(aios::ByteSpan(f->data(), f->size()));
    const size_t nl = text.find('\n');
    if (nl == std::string::npos) continue;
    std::string decl = text.substr(nl + 1);
    while (!decl.empty() && std::isspace((unsigned char)decl.back())) decl.pop_back();
    Mailer m;
    std::string err;
    if (!parse(decl, &m, &err)) {
      std::fprintf(stderr, "[mailers] %s: %s\n", n.c_str(), err.c_str());
      continue;
    }
    m.created = text.substr(0, nl);
    mailers_.push_back(std::move(m));
  }
}

bool MailerStore::create(const std::string& sql, std::string* err) {
  Mailer m;
  if (!parse(sql, &m, err)) return false;
  if (!m.password_secret.empty() && !secrets_.has(m.password_secret)) {
    *err = "no secret named " + m.password_secret + " (CREATE SECRET " + m.password_secret + " AS '...' first)";
    return false;
  }
  std::lock_guard<std::mutex> lk(mu_);
  for (const Mailer& o : mailers_) if (o.name == m.name) { *err = "mailer " + m.name + " already exists (DROP MAILER first)"; return false; }
  save(m);
  mailers_.push_back(std::move(m));
  return true;
}

bool MailerStore::drop(const std::string& name, std::string* err) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = std::find_if(mailers_.begin(), mailers_.end(), [&](const Mailer& m) { return m.name == name; });
  if (it == mailers_.end()) { *err = "no mailer named " + name; return false; }
  host_.remove(dir_ + "/" + name + ".mailer");
  mailers_.erase(it);
  return true;
}

bool MailerStore::has(const std::string& name) {
  std::lock_guard<std::mutex> lk(mu_);
  for (const Mailer& m : mailers_) if (m.name == name) return true;
  return false;
}

std::vector<MailerStore::Mailer> MailerStore::list() {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<Mailer> out = mailers_;
  std::sort(out.begin(), out.end(), [](const Mailer& a, const Mailer& b) { return a.name < b.name; });
  return out;
}

bool MailerStore::send(const std::string& name, aios::net::MailParts parts, std::string* message_id, std::string* err) {
  Mailer m;
  {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = std::find_if(mailers_.begin(), mailers_.end(), [&](const Mailer& x) { return x.name == name; });
    if (it == mailers_.end()) { *err = "no mailer named " + name; return false; }
    m = *it;
  }
  const std::string at = "MAILER " + name + ": ";
  parts.from = m.from;
  if (parts.from_name.empty()) parts.from_name = m.from_name;
  std::string build_err;
  const std::string msg = net::mail_build_message(parts, message_id, &build_err);
  if (msg.empty()) { *err = at + build_err; return false; }
  const std::vector<std::string> rcpts = net::mail_recipients(parts);

  // The credential first, so a sign-in that has lapsed says so before any connection is made.
  std::string secret;
  if (!m.oauth_provider.empty()) {
    OAuth oauth(host_, kernel_root_);
    auto tok = oauth.access_token(m.oauth_provider, m.oauth_account);
    if (!tok.ok()) {
      *err = at + "no usable " + m.oauth_provider + " sign-in for " + m.oauth_account + " (run `aios google auth " + m.oauth_account + "` on this machine): " + tok.error().message;
      return false;
    }
    secret = *tok;
  } else if (!m.password_secret.empty()) {
    std::string e;
    if (!secrets_.get(m.password_secret, &secret, &e)) { *err = at + e; return false; }
  }

  const net::Security sec = m.security == "tls" ? net::Security::Tls : m.security == "none" ? net::Security::None : net::Security::StartTls;
  net::SmtpClient smtp(host_, kernel_root_, 30000);
  auto fail = [&](const char* step, const std::string& why) {
    smtp.close();
    *err = at + step + " " + m.host + ":" + std::to_string(m.port) + ": " + why;
    return false;
  };
  if (auto c = smtp.connect(m.host, (uint16_t)m.port, sec, m.from.substr(m.from.find('@') + 1)); !c.ok()) return fail("connecting to", c.error().message);
  if (!m.oauth_provider.empty()) {
    if (auto a = smtp.authenticate_xoauth2(m.user, secret); !a.ok()) return fail("signing in at", a.error().message);
  } else if (!m.user.empty()) {
    if (auto a = smtp.authenticate(m.user, secret); !a.ok()) return fail("signing in at", a.error().message);
  }
  if (auto s = smtp.send(m.from, rcpts, msg); !s.ok()) return fail("sending through", s.error().message);
  (void)smtp.quit();
  smtp.close();
  return true;
}

}  // namespace aios::userland
