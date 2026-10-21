// The mail store's network side, in this tree and nothing else: IMAP fetches (headers, whole messages,
// server-side search), SMTP delivery and the connection test all go through src/net/imap.cpp and
// src/net/smtp.cpp over the kernel's own sockets and TLS, signing in with a password or with an OAuth 2.0
// access token (src/userland/oauth.cpp). No other program is started, so this is the same code on a
// hosted machine and on bare metal.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sys/stat.h>

#include "aios/hal/net.hpp"
#include "aios/core/base64.hpp"
#include "aios/net/imap.hpp"
#include "aios/net/mime.hpp"
#include "aios/net/smtp.hpp"
#include "aios/userland/mail_client.hpp"
#include "aios/userland/oauth.hpp"

namespace aios::userland {

std::string mail_imap_folder_name(const MailAccount& acc, const std::string& local_folder) {
  if (local_folder == "inbox") return "INBOX";
  if (local_folder == "sent") {
    if (acc.provider == "gmail") return "[Gmail]/Sent Mail";
    if (acc.provider == "icloud") return "Sent Messages";
    if (acc.provider == "outlook") return "Sent Items";
    return "Sent";
  }
  return local_folder;
}

std::string mail_oauth_provider(const MailAccount& acc) {
  if (acc.provider == "gmail") return "google";
  if (acc.provider == "outlook") return "microsoft";
  return acc.provider;
}

namespace {

net::Security imap_security(uint16_t port, bool use_tls) {
  if (port == 993) return net::Security::Tls;
  return use_tls ? net::Security::StartTls : net::Security::None;
}

net::Security smtp_security(uint16_t port, bool use_tls) {
  if (port == 465) return net::Security::Tls;
  return use_tls ? net::Security::StartTls : net::Security::None;
}

std::string collapse_ws(const std::string& s) {
  std::string o;
  bool ws = false;
  for (char c : s) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { ws = true; continue; }
    if (ws && !o.empty()) o += ' ';
    ws = false;
    o += c;
  }
  return o;
}

// The first `n` code points of UTF-8 text.
std::string utf8_prefix(const std::string& s, size_t n) {
  size_t i = 0, count = 0;
  while (i < s.size() && count < n) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    i += len;
    ++count;
  }
  return s.substr(0, std::min(i, s.size()));
}

bool has_flag(const std::vector<std::string>& flags, const char* f) {
  for (const auto& x : flags) {
    if (x.size() != std::strlen(f)) continue;
    bool eq = true;
    for (size_t i = 0; i < x.size() && eq; ++i) eq = (x[i] | 0x20) == (f[i] | 0x20);
    if (eq) return true;
  }
  return false;
}

// What the old worker made of one message (envelope_from), from the in-tree MIME reader.
MailFetchRecord make_record(const net::mime::Parsed& p, uint32_t uid, const std::vector<std::string>& flags,
                            bool have_body, int64_t fallback_ts) {
  MailFetchRecord r;
  r.uid = uid;
  r.message_id = collapse_ws(p.header("Message-ID"));
  net::mime::Address from = net::mime::parse_address(p.decoded_header("From"));
  r.from_addr = from.addr;
  r.from_name = from.name.empty() ? from.addr : from.name;
  r.to = p.decoded_header("To");
  r.cc = p.decoded_header("Cc");
  r.subject = p.header("Subject").empty() ? std::string("(No Subject)") : p.decoded_header("Subject");
  int64_t ts = 0;
  int off = 0;
  if (net::mime::parse_date(p.header("Date"), ts, off)) {
    r.date_str = net::mime::format_date(ts, off);
  } else {
    ts = fallback_ts;
    off = 0;
    r.date_str = ts ? net::mime::format_date(ts, 0) : std::string("Unknown");
  }
  r.timestamp = ts > 0 ? static_cast<uint64_t>(ts) : 0;
  r.is_unread = !has_flag(flags, "\\Seen");
  r.is_flagged = has_flag(flags, "\\Flagged");
  if (have_body) {
    r.have_body = true;
    r.body = p.text;
    r.has_html = !p.html.empty();
    r.snippet = utf8_prefix(collapse_ws(p.text), 120);
    for (const auto& a : p.attachments) {
      MailAttachment att;
      att.filename = a.filename;
      att.size_bytes = a.size_bytes;
      att.mime_type = a.mime_type;
      r.attachments.push_back(std::move(att));
    }
  }
  return r;
}

const std::string* find_section(const net::ImapFetched& f, const char* prefix) {
  const size_t n = std::strlen(prefix);
  for (const auto& kv : f.sections)
    if (kv.first.compare(0, n, prefix) == 0) return &kv.second;
  return nullptr;
}

// A very large message keeps its html and text but not its raw copy: the body cache is size-capped and one
// 40MB attachment would evict everything else.
constexpr size_t kMaxRawBytes = 4 * 1024 * 1024;

void write_parts(const std::string& raw_dir, uint32_t uid, const std::string& raw, const std::string& html) {
  if (raw_dir.empty()) return;
  const std::string base = raw_dir + "/" + std::to_string(uid);
  if (raw.size() <= kMaxRawBytes) {
    std::ofstream f(base + ".eml", std::ios::binary);
    if (f.is_open()) f.write(raw.data(), static_cast<std::streamsize>(raw.size()));
  }
  if (!html.empty()) {
    std::ofstream f(base + ".html", std::ios::binary);
    if (f.is_open()) f.write(html.data(), static_cast<std::streamsize>(html.size()));
  }
}

std::string uid_list(const std::vector<uint32_t>& uids, size_t from, size_t to) {
  std::string s;
  for (size_t i = from; i < to && i < uids.size(); ++i) {
    if (!s.empty()) s += ',';
    s += std::to_string(uids[i]);
  }
  return s;
}

// What to say when a sign-in fails: the provider's own reason, with the one fix people actually need.
std::string explain_login_failure(const MailSyncJob& job, const std::string& text) {
  if (text.find("Application-specific password required") != std::string::npos || text.find("185833") != std::string::npos)
    return "Google App Password required (https://support.google.com/accounts/answer/185833), or sign in with `aios google auth`";
  if (job.oauth && (text.find("AUTHENTICATIONFAILED") != std::string::npos || text.find("Invalid credentials") != std::string::npos))
    return "the server refused the access token: sign in again with `aios google auth` (" + text + ")";
  return text;
}

}  // namespace

void MailStore::set_host(hal::Host* host, const std::string& kernel_root) {
  host_ = host;
  if (!kernel_root.empty()) kernel_root_ = kernel_root;
  else if (kernel_root_.empty()) {
    size_t slash = root_dir_.find_last_of('/');
    kernel_root_ = slash == std::string::npos || slash == 0 ? std::string(".") : root_dir_.substr(0, slash);
  }
}

Result<std::string> MailStore::oauth_access_token(const std::string& provider, const std::string& account) {
  if (!host_) return Result<std::string>(Status::Unsupported, "this mail store has no network host to sign in through");
  std::lock_guard<aios::hal::Mutex> lk(oauth_mu_);
  OAuth o(*host_, kernel_root_);
  return o.access_token(provider, account);
}

MailSyncOutcome MailStore::run_job(const MailSyncJob& job) {
  MailSyncOutcome r;
  r.mode = job.mode;
  r.account_id = job.account_id;
  r.folder = job.local_folder;
  auto fail = [&](const std::string& why) {
    r.ok = false;
    r.status = why;
    return r;
  };

  // An account that cannot possibly sync says so, out loud: a half-configured account must not look like
  // a freshly synced one.
  if (job.server.empty()) return fail("Not syncing - no IMAP server set");
  if (!job.oauth && (job.username.empty() || job.password.empty())) return fail("Not syncing - no password set");
  if (!host_) return fail("Sync failed: this mail store was opened without a network host");

  r.attempted = true;
  std::string secret = job.password;
  if (job.oauth) {
    auto tok = oauth_access_token(job.oauth_provider, job.oauth_account);
    if (!tok.ok()) {
      if (tok.error().status == Status::Expired || tok.error().status == Status::NotFound)
        return fail("Sync failed: sign in again with `aios google auth` (" + tok.error().message + ")");
      return fail("Sync failed: could not get an access token (" + tok.error().message + ")");
    }
    secret = *tok;
  }

  net::ImapClient imap(*host_, kernel_root_, 30000);
  if (auto c = imap.connect(job.server, job.port, static_cast<net::Security>(job.security)); !c.ok())
    return fail("Sync failed: " + c.error().message);
  auto au = job.oauth ? imap.authenticate_xoauth2(job.username, secret) : imap.login(job.username, secret);
  if (!au.ok()) return fail("Sync failed: " + explain_login_failure(job, au.error().message));

  // ---- one whole message, for a body the vacuum has since evicted ----
  if (job.mode == "body") {
    auto sel = imap.select(job.imap_folder, true);
    if (!sel.ok()) return fail("Sync failed: " + sel.error().message);
    auto got = imap.uid_fetch(job.body_uid, "(UID FLAGS BODY.PEEK[])");
    if (!got.ok()) return fail("Sync failed: " + got.error().message);
    const std::string* raw = got->empty() ? nullptr : find_section((*got)[0], "BODY[]");
    if (!raw) return fail("Sync failed: message " + job.body_uid + " is no longer on the server");
    net::mime::Parsed p = net::mime::parse(*raw);
    const uint32_t uid = (*got)[0].uid ? (*got)[0].uid : static_cast<uint32_t>(std::strtoul(job.body_uid.c_str(), nullptr, 10));
    r.records.push_back(make_record(p, uid, (*got)[0].flags, true, 0));
    write_parts(job.raw_dir, uid, *raw, p.html);
    r.ok = true;
    r.fetched = 1;
    r.max_uid = uid;
    (void)imap.logout();
    return r;
  }

  auto sel = imap.select(job.imap_folder, true);
  if (!sel.ok()) return fail("Sync failed: cannot open " + job.imap_folder + " (" + sel.error().message + ")");
  r.uidvalidity = sel->uidvalidity;

  std::vector<uint32_t> uids;
  bool want_bodies = true;
  if (job.mode == "search") {
    want_bodies = false;
    const std::string q = net::imap_quote(job.query);
    auto s = imap.uid_search("X-GM-RAW " + q);
    if (!s.ok()) s = imap.uid_search("TEXT " + q);   // not Gmail: the server's own text search
    if (!s.ok()) return fail("Sync failed: " + s.error().message);
    uids = std::move(*s);
    const size_t lim = static_cast<size_t>(job.limit > 0 ? job.limit : 200);
    if (uids.size() > lim) uids.erase(uids.begin(), uids.end() - static_cast<long>(lim));
  } else if (job.last_uid > 0) {
    // everything the server has gained since we last looked
    auto s = imap.uid_search("UID " + std::to_string(job.last_uid + 1) + ":*");
    if (!s.ok()) return fail("Sync failed: " + s.error().message);
    for (uint32_t u : *s) if (u > job.last_uid) uids.push_back(u);
  } else {
    auto s = imap.uid_search("ALL");
    if (!s.ok()) return fail("Sync failed: " + s.error().message);
    uids = std::move(*s);
    if (job.limit > 0 && uids.size() > static_cast<size_t>(job.limit)) uids.erase(uids.begin(), uids.end() - job.limit);
  }

  // Headers for everything; whole messages only for the recent slice. HEADER.FIELDS, not HEADER: a full Gmail
  // header block runs 5-17KB (every Received: hop, DKIM, ARC seal), and these six fields are exactly what the
  // envelope index keeps. No BODY.PEEK[TEXT]<0.N> snippet peek either: a partial TEXT range makes Gmail
  // materialise and decode the whole message, measured at 0.3s without it and 18.8s with it for 200 messages.
  std::vector<MailFetchRecord> records;
  uint32_t max_uid = 0;
  for (size_t i = 0; i < uids.size(); i += 200) {
    auto got = imap.uid_fetch(uid_list(uids, i, i + 200), "(UID FLAGS INTERNALDATE BODY.PEEK[HEADER.FIELDS (FROM TO CC SUBJECT DATE MESSAGE-ID)])");
    if (!got.ok()) return fail("Sync failed: " + got.error().message);
    for (const auto& f : *got) {
      const std::string* hdr = find_section(f, "BODY[HEADER");
      if (!hdr || f.uid == 0) continue;
      int64_t idate = 0;
      net::imap_parse_internaldate(f.internaldate, idate);
      records.push_back(make_record(net::mime::parse(*hdr), f.uid, f.flags, false, idate));
      max_uid = std::max(max_uid, f.uid);
    }
  }

  if (want_bodies && job.body_since > 0) {
    std::vector<size_t> recent;
    for (size_t i = 0; i < records.size(); ++i)
      if (records[i].timestamp >= job.body_since) recent.push_back(i);
    for (size_t i = 0; i < recent.size(); i += 50) {
      std::string set;
      for (size_t k = i; k < recent.size() && k < i + 50; ++k) { if (!set.empty()) set += ','; set += std::to_string(records[recent[k]].uid); }
      auto got = imap.uid_fetch(set, "(UID BODY.PEEK[])");
      if (!got.ok()) continue;   // the envelopes are good; the bodies can be fetched on demand
      for (const auto& f : *got) {
        const std::string* raw = find_section(f, "BODY[]");
        if (!raw) continue;
        for (size_t idx : recent) {
          MailFetchRecord& rec = records[idx];
          if (rec.uid != f.uid) continue;
          net::mime::Parsed p = net::mime::parse(*raw);
          rec.body = p.text;
          rec.have_body = true;
          rec.has_html = !p.html.empty();
          rec.snippet = utf8_prefix(collapse_ws(p.text), 120);
          rec.attachments.clear();
          for (const auto& a : p.attachments) {
            MailAttachment att;
            att.filename = a.filename;
            att.size_bytes = a.size_bytes;
            att.mime_type = a.mime_type;
            rec.attachments.push_back(std::move(att));
          }
          write_parts(job.raw_dir, f.uid, *raw, p.html);
          break;
        }
      }
    }
  }

  std::stable_sort(records.begin(), records.end(), [](const MailFetchRecord& a, const MailFetchRecord& b) { return a.timestamp > b.timestamp; });
  r.records = std::move(records);
  r.ok = true;
  r.fetched = static_cast<int>(r.records.size());
  r.max_uid = max_uid;
  (void)imap.logout();
  return r;
}

// ---------------------------------------------------------------------------------------------------
// Outbound
// ---------------------------------------------------------------------------------------------------

namespace {

bool is_ascii(const std::string& s) {
  for (unsigned char c : s) if (c >= 0x80 || c == 0) return false;
  return true;
}

std::string b64(const std::string& s) {
  return aios::base64_encode(aios::ByteSpan(reinterpret_cast<const uint8_t*>(s.data()), s.size()));
}

// RFC 2047 encoded words, each at most 45 bytes of text so the encoded word stays under 75 characters,
// split on UTF-8 boundaries.
std::string encode_words(const std::string& s) {
  if (is_ascii(s)) {
    for (char c : s) if (c == '\r' || c == '\n') return "";   // a header value cannot carry a line break
    return s;
  }
  std::string out;
  size_t i = 0;
  while (i < s.size()) {
    size_t j = std::min(s.size(), i + 45);
    while (j < s.size() && (static_cast<unsigned char>(s[j]) & 0xC0) == 0x80) --j;
    std::string chunk = s.substr(i, j - i);
    for (auto& c : chunk) if (c == '\r' || c == '\n') c = ' ';
    if (!out.empty()) out += "\r\n ";
    out += "=?UTF-8?B?" + b64(chunk) + "?=";
    i = j;
  }
  return out;
}

std::string quoted_display_name(const std::string& name) {
  if (name.empty()) return "";
  if (!is_ascii(name)) return encode_words(name);
  std::string o = "\"";
  for (char c : name) {
    if (c == '\r' || c == '\n') continue;
    if (c == '"' || c == '\\') o += '\\';
    o += c;
  }
  return o + "\"";
}

// Addresses out of header-ish strings ("A <a@x>; b@y"), as bare addresses.
std::vector<std::string> bare_addresses(const std::vector<std::string>& in) {
  std::vector<std::string> out;
  for (std::string s : in) {
    for (auto& c : s) if (c == ';') c = ',';
    for (const auto& a : net::mime::parse_addresses(s))
      if (!a.addr.empty()) out.push_back(a.addr);
  }
  return out;
}

std::string join(const std::vector<std::string>& v) {
  std::string o;
  for (const auto& x : v) { if (!o.empty()) o += ", "; o += x; }
  return o;
}

}  // namespace

std::string MailStore::deliver(const MailAccount& acc, const MailMessage& m) {
  const std::vector<std::string> to = bare_addresses(m.to), cc = bare_addresses(m.cc);
  std::vector<std::string> rcpts = to;
  rcpts.insert(rcpts.end(), cc.begin(), cc.end());
  if (acc.outgoing_server.empty() || acc.username.empty() || (acc.auth != "oauth2" && acc.password.empty()))
    return "Saved to Sent, not delivered (no SMTP server, user or password on " + (acc.email.empty() ? std::string("any account") : acc.email) + ")";
  if (!host_) return "Saved to Sent, not delivered (this mail store has no network host)";
  if (rcpts.empty()) return "Saved to Sent, not delivered (no recipients)";

  std::string secret = acc.password;
  const bool oauth = acc.auth == "oauth2";
  if (oauth) {
    auto tok = oauth_access_token(mail_oauth_provider(acc), acc.email);
    if (!tok.ok()) return "Sent locally (SMTP warning: sign in again with `aios google auth`: " + tok.error().message + ")";
    secret = *tok;
  }

  net::SmtpClient smtp(*host_, kernel_root_, 30000);
  if (auto c = smtp.connect(acc.outgoing_server, acc.outgoing_port, smtp_security(acc.outgoing_port, acc.use_tls)); !c.ok())
    return "Sent locally (SMTP warning: " + c.error().message + ")";
  auto au = oauth ? smtp.authenticate_xoauth2(acc.username, secret) : smtp.authenticate(acc.username, secret);
  if (!au.ok()) return "Sent locally (SMTP warning: " + au.error().message + ")";

  const bool eight = smtp.has_extension("8BITMIME");
  std::string from_addr = m.from_addr.empty() ? acc.email : m.from_addr;
  std::string msg;
  msg += "From: " + (quoted_display_name(acc.display_name).empty() ? "" : quoted_display_name(acc.display_name) + " ") + "<" + from_addr + ">\r\n";
  msg += "To: " + join(to) + "\r\n";
  if (!cc.empty()) msg += "Cc: " + join(cc) + "\r\n";
  msg += "Subject: " + encode_words(m.subject) + "\r\n";
  msg += "Date: " + net::mime::format_rfc5322_date(static_cast<int64_t>(m.timestamp ? m.timestamp : static_cast<uint64_t>(std::time(nullptr)))) + "\r\n";
  msg += "Message-ID: " + (m.message_id.empty() ? std::string("<" + m.id + "@aios.org>") : m.message_id) + "\r\n";
  msg += "MIME-Version: 1.0\r\n";
  msg += "Content-Type: text/plain; charset=utf-8\r\n";
  if (is_ascii(m.body)) {
    msg += "Content-Transfer-Encoding: 7bit\r\n\r\n" + m.body;
  } else if (eight) {
    msg += "Content-Transfer-Encoding: 8bit\r\n\r\n" + m.body;
  } else {
    msg += "Content-Transfer-Encoding: base64\r\n\r\n";
    std::string enc = b64(m.body);
    for (size_t i = 0; i < enc.size(); i += 76) msg += enc.substr(i, 76) + "\r\n";
  }
  if (msg.size() < 2 || msg.compare(msg.size() - 2, 2, "\r\n") != 0) msg += "\r\n";

  auto sent = smtp.send(from_addr, rcpts, msg);
  (void)smtp.quit();
  if (!sent.ok() && sent.error().status != Status::Conflict) return "Sent locally (SMTP warning: " + sent.error().message + ")";
  std::string status = "Message sent to " + (to.empty() ? rcpts[0] : to[0]) + " [SMTP Delivered]";
  if (!sent.ok()) status += " (some recipients were refused: " + sent.error().message + ")";
  // The compose model carries each attachment's name and size, not its bytes, so there is nothing to send.
  if (!m.attachments.empty()) status += " - attachments were not sent: this message holds their names, not their contents";
  return status;
}

// ---------------------------------------------------------------------------------------------------
// The connection test
// ---------------------------------------------------------------------------------------------------

Result<std::string> MailStore::probe_endpoint(const std::string& host, uint16_t port, int timeout_ms) {
  if (host.empty()) return Result<std::string>(Status::Invalid, "empty host");
  auto start = std::chrono::steady_clock::now();
  auto c = hal::net_connect(host, port, timeout_ms);
  if (!c.ok()) return Result<std::string>(c.error());
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
  (*c)->close();
  return std::string("Connected in " + std::to_string(ms) + "ms");
}

Result<std::string> MailStore::test_account_connection(const MailAccount& acc) {
  std::string log;
  bool imap_ok = false, smtp_ok = false, auth_tried = false, auth_ok = false;
  auto r_imap = probe_endpoint(acc.incoming_server, acc.incoming_port, 2500);
  if (!r_imap.ok()) {
    log += "Incoming " + acc.incoming_server + ":" + std::to_string(acc.incoming_port) + " FAILED (" + r_imap.error().str() + ")";
  } else {
    imap_ok = true;
    log += "IMAP " + acc.incoming_server + ":" + std::to_string(acc.incoming_port) + " [Connected " + *r_imap + "]";
  }

  // With credentials, and a server that answered, try the real login.
  const bool oauth = acc.auth == "oauth2";
  if (imap_ok && host_ && !acc.username.empty() && (oauth || !acc.password.empty())) {
    auth_tried = true;
    MailSyncJob job;
    job.oauth = oauth;
    std::string secret = acc.password;
    bool have_secret = true;
    if (oauth) {
      auto tok = oauth_access_token(mail_oauth_provider(acc), acc.email);
      if (!tok.ok()) { log += " [Auth Failed: " + tok.error().message + "]"; have_secret = false; }
      else secret = *tok;
    }
    if (have_secret) {
      net::ImapClient imap(*host_, kernel_root_, 15000);
      auto c = imap.connect(acc.incoming_server, acc.incoming_port, imap_security(acc.incoming_port, acc.use_tls));
      auto au = c.ok() ? (oauth ? imap.authenticate_xoauth2(acc.username, secret) : imap.login(acc.username, secret)) : c;
      if (au.ok()) {
        auth_ok = true;
        log += " [Login Verified]";
        (void)imap.logout();
      } else {
        log += " [Auth Failed: " + explain_login_failure(job, au.error().message) + "]";
      }
    }
  }

  log += " • ";

  auto r_smtp = probe_endpoint(acc.outgoing_server, acc.outgoing_port, 2500);
  if (!r_smtp.ok()) {
    log += "Outgoing " + acc.outgoing_server + ":" + std::to_string(acc.outgoing_port) + " FAILED (" + r_smtp.error().str() + ")";
  } else {
    smtp_ok = true;
    log += "SMTP " + acc.outgoing_server + ":" + std::to_string(acc.outgoing_port) + " [OK " + *r_smtp + "]";
  }

  // The verdict follows what happened: two refused connections must not print [OK].
  if (!imap_ok || !smtp_ok || (auth_tried && !auth_ok)) return Result<std::string>(Status::IoError, log);
  return log;
}

}  // namespace aios::userland
