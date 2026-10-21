// Drives the in-tree SMTP client against scripts/mock-smtp.py and checks the wire: authentication
// (PLAIN, LOGIN, XOAUTH2 right and wrong), what the server receives byte-for-byte (dot-stuffing, bare LF
// turned into CRLF, 8-bit text), recipients refused one by one, a size limit, and addresses that try
// to break out of the command line.
//   aios-smtp-check <port> <out-dir> <access-token> [none|tls|starttls [trust-store.pem [no8bit]]]
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "aios/hal/host.hpp"
#include "aios/net/smtp.hpp"

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
  if (argc < 4) { std::fprintf(stderr, "usage: %s <port> <out-dir> <token> [none|tls|starttls [trust.pem [no8bit]]]\n", argv[0]); return 2; }
  const uint16_t port = static_cast<uint16_t>(std::atoi(argv[1]));
  const std::string out = argv[2];
  const std::string token = argv[3];
  const std::string mode = argc > 4 ? argv[4] : "none";
  const std::string trust = argc > 5 ? argv[5] : "";
  const bool no8 = argc > 6 && std::string(argv[6]) == "no8bit";
  const Security sec = mode == "tls" ? Security::Tls : mode == "starttls" ? Security::StartTls : Security::None;
  auto host = hal::make_host();
  auto fresh = [&]() { return std::make_unique<SmtpClient>(*host, "/tmp", 10000, trust); };

  {
    auto c = fresh();
    check(c->connect("localhost", port, sec).ok(), "connect, greeting, EHLO" + std::string(sec == Security::StartTls ? ", STARTTLS, EHLO again" : ""));
    check(c->has_extension("8BITMIME") == !no8, "8BITMIME is advertised exactly when the server has it");
    check(c->has_mechanism("PLAIN") || c->has_mechanism("LOGIN"), "AUTH mechanisms are parsed from the EHLO reply");
    check(c->has_mechanism("XOAUTH2"), "XOAUTH2 is among them");
    auto nomail = c->send("a@example.com", {"b@example.com"}, "Subject: x\r\n\r\ny\r\n");
    check(!nomail.ok(), "MAIL before AUTH is refused (530)");
    auto bad = c->authenticate_plain("test@example.com", "wrong");
    check(!bad.ok() && bad.error().status == Status::Denied, "AUTH PLAIN with a wrong password is Denied");
    c->close();
  }
  {
    auto c = fresh();
    (void)c->connect("localhost", port, sec);
    auto bad = c->authenticate_xoauth2("test@example.com", "ya29.expired");
    check(!bad.ok() && bad.error().status == Status::Denied, "XOAUTH2 with a bad token is Denied (the 334 error round trip is answered)");
    check(c->authenticate_xoauth2("test@example.com", token).ok(), "XOAUTH2 with the right token succeeds on the same connection afterwards");
    c->close();
  }
  {
    auto c = fresh();
    (void)c->connect("localhost", port, sec);
    check(c->authenticate_login("test@example.com", "secret").ok(), "AUTH LOGIN");
    c->close();
  }

  auto c = fresh();
  if (!c->connect("localhost", port, sec).ok() || !c->authenticate("test@example.com", "secret").ok()) {
    std::printf("FAIL  cannot authenticate for the rest of the checks\n");
    return 1;
  }
  check(true, "authenticate() picks a password mechanism the server has");

  // 1. a plain message
  const std::string m1 = "From: a@example.com\r\nTo: b@example.com\r\nSubject: one\r\n\r\nhello\r\n";
  check(c->send("a@example.com", {"b@example.com"}, m1).ok(), "send a plain message");
  check(slurp(out + "/1.eml") == m1, "the server received it byte-for-byte");
  check(slurp(out + "/1.env") == "{\"from\": \"a@example.com\", \"to\": [\"b@example.com\"]}", "with the right envelope");

  // 2. dot-stuffing and a message that ends without a newline
  const std::string m2 = "Subject: dots\r\n\r\n.leading dot\r\n..two dots\r\nfine\r\n.\r\nafter a lone dot line\r\nno final newline";
  check(c->send("a@example.com", {"b@example.com"}, m2).ok(), "send a message whose lines start with dots, and has no final newline");
  check(slurp(out + "/2.eml") == m2 + "\r\n", "dots survive the round trip and the missing final CRLF is supplied");

  // 3. bare LF becomes CRLF
  check(c->send("a@example.com", {"b@example.com"}, "Subject: lf\n\nline1\nline2\n").ok(), "send a message written with bare LF");
  check(slurp(out + "/3.eml") == "Subject: lf\r\n\r\nline1\r\nline2\r\n", "every line ends in CRLF on the wire");

  // 4. 8-bit text
  const std::string m4 = "Subject: caf\xc3\xa9\r\nContent-Type: text/plain; charset=utf-8\r\n\r\ncaf\xc3\xa9 \xe2\x82\xac\r\n";
  auto r4 = c->send("a@example.com", {"b@example.com"}, m4);
  if (no8) {
    check(!r4.ok() && r4.error().status == Status::Unsupported, "8-bit text is refused, not mangled, when the server lacks 8BITMIME");
  } else {
    check(r4.ok() && slurp(out + "/4.eml") == m4, "8-bit UTF-8 text goes through under 8BITMIME unchanged");
  }
  const int base = no8 ? 3 : 4;

  // 5. one recipient refused, one accepted: delivered, and the refusal is reported
  auto r5 = c->send("a@example.com", {"ok@example.com", "gone@refuse.example"}, m1);
  check(!r5.ok() && r5.error().status == Status::Conflict && r5.error().message.find("gone@refuse.example") != std::string::npos,
        "a refused recipient is reported by name (Conflict) while the message goes to the others");
  check(slurp(out + "/" + std::to_string(base + 1) + ".env") == "{\"from\": \"a@example.com\", \"to\": [\"ok@example.com\"]}", "and the envelope holds only the accepted one");
  auto r6 = c->send("a@example.com", {"gone@refuse.example"}, m1);
  check(!r6.ok() && r6.error().status == Status::Invalid, "every recipient refused is Invalid, and nothing is sent");
  check(c->send("a@example.com", {"ok@example.com"}, m1).ok(), "the session is still good after a refused transaction (RSET)");

  // 6. injection
  auto inj = c->send("a@example.com", {"b@example.com>\r\nRCPT TO:<evil@example.com"}, m1);
  check(!inj.ok() && inj.error().status == Status::Invalid, "a recipient carrying CRLF is refused before it reaches the wire");
  auto inj2 = c->send("a@example.com\r\nMAIL FROM:<x@y", {"b@example.com"}, m1);
  check(!inj2.ok() && inj2.error().status == Status::Invalid, "so is a sender");

  // 7. size
  std::string big = "Subject: big\r\n\r\n";
  big.append(6 * 1000 * 1000, 'x');
  big += "\r\n";
  auto rb = c->send("a@example.com", {"b@example.com"}, big);
  check(!rb.ok() && rb.error().status == Status::Exhausted, "a message over the server's SIZE limit is refused up front");
  std::string mid = "Subject: mid\r\n\r\n";
  for (int i = 0; i < 40000; ++i) mid += "a line of text that is long enough to add up to a few megabytes of data, line " + std::to_string(i) + "\r\n";
  auto rm = c->send("a@example.com", {"b@example.com"}, mid);
  check(rm.ok(), "a " + std::to_string(mid.size() / 1000000) + " MB message is sent");
  std::string got = slurp(out + "/" + std::to_string(base + 3) + ".eml");
  check(got == mid, "and received intact");

  check(c->quit().ok(), "QUIT");
  std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
  return g_fail ? 1 : 0;
}
