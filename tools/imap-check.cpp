// Drives the in-tree IMAP client against a server (scripts/mock-imap.py) and checks what comes back:
// authentication (LOGIN, XOAUTH2 right and wrong), SELECT, UID SEARCH, UID FETCH of headers and of
// whole messages byte-for-byte, APPEND, STORE, COPY, EXPUNGE, LIST and modified UTF-7 mailbox names.
//   aios-imap-check <port> <seed-dir> <access-token> [none|tls|starttls [trust-store.pem]]
// Exits 0 only if every check holds. Prints one line per check.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include "aios/hal/host.hpp"
#include "aios/net/imap.hpp"
#include "aios/net/mime.hpp"

using namespace aios;
using namespace aios::net;

static int g_fail = 0;
static void check(bool ok, const std::string& what) {
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
  if (!ok) ++g_fail;
}

static std::string slurp(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

int main(int argc, char** argv) {
  if (argc < 4) { std::fprintf(stderr, "usage: %s <port> <seed-dir> <access-token> [none|tls|starttls [trust.pem]]\n", argv[0]); return 2; }
  const uint16_t port = static_cast<uint16_t>(std::atoi(argv[1]));
  const std::string seed = argv[2];
  const std::string token = argv[3];
  const std::string mode = argc > 4 ? argv[4] : "none";
  const std::string trust = argc > 5 ? argv[5] : "";
  const Security sec = mode == "tls" ? Security::Tls : mode == "starttls" ? Security::StartTls : Security::None;
  auto host = hal::make_host();
  auto fresh = [&]() { return std::make_unique<ImapClient>(*host, "/tmp", 10000, trust); };

  // --- authentication ---
  {
    auto c = fresh();
    check(c->connect("localhost", port, sec).ok(), "connect, greeting and capabilities");
    check(c->has_capability("IMAP4rev1") && c->has_capability("AUTH=XOAUTH2"), "capabilities include IMAP4rev1 and AUTH=XOAUTH2");
    auto bad = c->login("test@example.com", "wrong");
    check(!bad.ok() && bad.error().status == Status::Denied, "LOGIN with a wrong password is Denied");
    c->close();
  }
  {
    auto c = fresh();
    (void)c->connect("localhost", port, sec);
    auto bad = c->authenticate_xoauth2("test@example.com", "ya29.expired");
    check(!bad.ok() && bad.error().status == Status::Denied, "XOAUTH2 with a bad token is Denied (and the SASL error round trip does not hang)");
    // the connection must still be usable after the failed exchange
    auto good = c->authenticate_xoauth2("test@example.com", token);
    check(good.ok(), "XOAUTH2 with the right token succeeds on the same connection afterwards");
    c->close();
  }
  {
    auto c = fresh();
    (void)c->connect("localhost", port, sec);
    check(c->authenticate_plain("test@example.com", "secret").ok(), "AUTHENTICATE PLAIN");
    c->close();
  }

  auto c = fresh();
  if (!c->connect("localhost", port, sec).ok() || !c->login("test@example.com", "secret").ok()) {
    std::printf("FAIL  cannot log in for the rest of the checks\n");
    return 1;
  }
  check(true, "LOGIN with the right password");

  // --- select, search ---
  auto sel = c->select("INBOX", true);
  check(sel.ok() && sel->read_only && sel->uidvalidity == 1000 && sel->exists > 5, "EXAMINE INBOX: UIDVALIDITY 1000, read-only, messages present");
  const uint32_t n = sel.ok() ? sel->exists : 0;
  auto all = c->uid_search("ALL");
  check(all.ok() && all->size() == n && (*all)[0] == 1 && all->back() == n, "UID SEARCH ALL returns every UID in order");
  auto tail = c->uid_search("UID " + std::to_string(n) + ":*");
  check(tail.ok() && tail->size() == 1 && (*tail)[0] == n, "UID SEARCH UID n:* returns the newest");
  auto gm = c->uid_search("X-GM-RAW \"cid\"");
  check(gm.ok() && !gm->empty(), "UID SEARCH X-GM-RAW finds a message by text");
  auto none = c->uid_search("TEXT \"zzzz-no-such-text-zzzz\"");
  check(none.ok() && none->empty(), "a search with no hits is an empty list, not an error");
  check(c->uidvalidity("INBOX").ok() && *c->uidvalidity("INBOX") == 1000, "STATUS UIDVALIDITY");

  // --- fetch headers (server answers items in its own order, FLAGS after literals for even uids) ---
  auto hdrs = c->uid_fetch("1:*", "(UID FLAGS INTERNALDATE BODY.PEEK[HEADER.FIELDS (FROM TO CC SUBJECT DATE MESSAGE-ID)])");
  bool hdr_ok = hdrs.ok() && hdrs->size() == n;
  bool every_has = hdr_ok;
  if (hdr_ok)
    for (const auto& f : *hdrs)
      if (f.uid == 0 || !f.has_flags || f.internaldate.empty() || !f.sections.count("BODY[HEADER.FIELDS (FROM TO CC SUBJECT DATE MESSAGE-ID)]")) every_has = false;
  check(hdr_ok && every_has, "UID FETCH headers for all messages: uid, flags, internaldate and the header block each time");
  if (hdr_ok) {
    int64_t t = 0;
    check(imap_parse_internaldate((*hdrs)[0].internaldate, t) && t == 1026899065, "INTERNALDATE parses (17-Jul-2002 02:44:25 -0700)");
  }

  // --- fetch whole messages, byte for byte ---
  bool bytes_ok = true;
  size_t compared = 0;
  for (uint32_t u = 1; u <= n; ++u) {
    auto one = c->uid_fetch(std::to_string(u), "(UID BODY.PEEK[])");
    if (!one.ok() || one->size() != 1) { bytes_ok = false; break; }
    // the seed files sort by name; uid i is the i-th file
    std::string want;
    {
      std::vector<std::string> names;
      std::string cmd = "LC_ALL=C ls " + seed + "/INBOX/*.eml";
      FILE* p = popen(cmd.c_str(), "r");
      char line[4096];
      while (p && std::fgets(line, sizeof line, p)) { std::string s(line); while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back(); names.push_back(s); }
      if (p) pclose(p);
      if (u - 1 < names.size()) want = slurp(names[u - 1]);
    }
    if ((*one)[0].sections["BODY[]"] != want) { bytes_ok = false; std::printf("      uid %u differs (%zu vs %zu bytes)\n", u, (*one)[0].sections["BODY[]"].size(), want.size()); }
    ++compared;
  }
  check(bytes_ok && compared == n, "UID FETCH BODY.PEEK[] returns every message byte-for-byte (literals with CRLF, 8-bit, NUL-free binary)");
  auto many = c->uid_fetch("1:*", "(UID BODY.PEEK[])");
  check(many.ok() && many->size() == n, "one FETCH of every whole message in a single response");

  // --- the parsed message through the MIME reader ---
  if (hdr_ok) {
    auto one = c->uid_fetch("1", "(UID BODY.PEEK[])");
    auto p = mime::parse((*one)[0].sections["BODY[]"]);
    check(!p.header("Subject").empty() || !p.header("From").empty(), "a fetched message parses with the MIME reader");
  }

  // --- LIST, create, utf-7, append ---
  auto boxes = c->list();
  bool sent = false;
  if (boxes.ok()) for (const auto& b : *boxes) if (b.name == "Sent" && b.has_attr("\\Sent")) sent = true;
  check(boxes.ok() && sent, "LIST returns mailboxes with their special-use attributes");
  const std::string uni = "Entw\xc3\xbcrfe/\xd0\xa2\xd0\xb5\xd1\x81\xd1\x82";   // Entwürfe/Тест
  check(imap_utf7_decode(imap_utf7_encode(uni)) == uni, "modified UTF-7 round-trips a non-ASCII mailbox name");
  check(imap_utf7_encode("a&b") == "a&-b" && imap_utf7_encode("Entw\xc3\xbcrfe") == "Entw&APw-rfe", "modified UTF-7 matches RFC 3501's examples");
  check(c->create_mailbox(uni).ok(), "CREATE a mailbox with a non-ASCII name");
  auto boxes2 = c->list();
  bool found_uni = false;
  if (boxes2.ok()) for (const auto& b : *boxes2) if (b.name == uni) found_uni = true;
  check(found_uni, "LIST shows it back decoded to the same UTF-8");
  check(c->select(uni, false).ok(), "SELECT the non-ASCII mailbox");

  const std::string note = "From: me@example.com\r\nSubject: a note\r\nDate: Fri, 16 Oct 2026 12:00:00 +0000\r\n\r\nline one\r\n\r\nline three with \xc3\xa9\r\n";
  auto up = c->append(uni, "\\Seen", note);
  check(up.ok() && *up > 0, "APPEND returns the new UID (APPENDUID)");
  auto back = c->uid_fetch(std::to_string(up.ok() ? *up : 0), "(UID FLAGS BODY.PEEK[])");
  check(back.ok() && back->size() == 1 && (*back)[0].sections["BODY[]"] == note, "the appended message comes back unchanged");
  check(back.ok() && back->size() == 1 && (*back)[0].flags.size() == 1 && (*back)[0].flags[0] == "\\Seen", "with the flags it was appended with");

  // --- store, copy, expunge ---
  check(c->uid_store("1", "+FLAGS", "\\Flagged \\Deleted").ok(), "UID STORE +FLAGS");
  auto fl = c->uid_fetch("1", "(UID FLAGS)");
  check(fl.ok() && fl->size() == 1 && (*fl)[0].flags.size() == 3, "the flags are there afterwards (Seen, Flagged, Deleted)");
  check(c->uid_copy("1", "INBOX").ok(), "UID COPY into another mailbox");
  check(c->uid_expunge("1").ok(), "UID EXPUNGE");
  auto after = c->uid_search("ALL");
  check(after.ok() && after->empty(), "the mailbox is empty after expunging its only message");

  // --- errors are errors ---
  auto nope = c->select("No Such Mailbox", true);
  check(!nope.ok() && nope.error().status == Status::NotFound, "SELECT of a missing mailbox is NotFound");
  auto bad = c->command("FROBNICATE");
  check(bad.ok() && bad->status == "BAD", "an unknown command is a BAD reply, not a crash");
  check(c->noop().ok(), "the session still works after errors");
  check(c->logout().ok(), "LOGOUT");

  std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
  return g_fail ? 1 : 0;
}
