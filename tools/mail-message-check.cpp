// The outgoing message builder, read back by the in-tree MIME reader: what it writes must come back as it
// was given (recipients, subject, display name, filename, text), and no line it writes may exceed RFC 5322's
// 998 bytes; address lists and subjects fold at 78 where a comma or space allows (a 40-recipient To: and a
// long subject used to go out as one line, which a strict MTA refuses).
#include <cstdio>
#include <string>
#include <vector>

#include "aios/net/mail_message.hpp"
#include "aios/net/mime.hpp"

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
  if (ok) return;
  ++failures;
  std::printf("FAIL  %s\n", what.c_str());
}

// Longest header line (bytes before CRLF) in the message's own header block.
size_t longest_header_line(const std::string& msg) {
  size_t longest = 0, at = 0;
  while (at < msg.size()) {
    size_t e = msg.find("\r\n", at);
    if (e == std::string::npos) e = msg.size();
    if (e == at) break;   // the blank line: the body begins
    longest = std::max(longest, e - at);
    at = e + 2;
  }
  return longest;
}

std::string join(const std::vector<std::string>& v) {
  std::string o;
  for (const auto& x : v) { if (!o.empty()) o += ", "; o += x; }
  return o;
}

}  // namespace

int main() {
  using namespace aios::net;
  // 1. A notification to forty people, a long subject, a non-ASCII name and filename.
  {
    MailParts m;
    m.from = "orders@example.com";
    m.from_name = "Ørdér Désk";
    for (int i = 0; i < 40; ++i) m.to.push_back("warehouse-person-" + std::to_string(i) + "@example-company.com");
    for (int i = 0; i < 12; ++i) m.cc.push_back("cc-" + std::to_string(i) + "@example-company.com");
    m.subject = "Purchase order 48213 received and allocated to the Friday truck; please confirm the pallet count "
                "and the dock door, and note that the carrier has asked for the paperwork by noon on Thursday at the latest";
    m.text = "line one\n.leading dot\nline three\n";
    m.attachments.push_back({"pièce jointe.pdf", "application/pdf", std::string(5000, 'x')});
    std::string id, err;
    const std::string msg = mail_build_message(m, &id, &err);
    check(!msg.empty(), "built: " + err);
    const size_t longest = longest_header_line(msg);
    check(longest <= 998, "a header line of " + std::to_string(longest) + " bytes (RFC 5322 allows 998)");
    check(longest <= 78, "a header line of " + std::to_string(longest) + " bytes (78 wanted: To, Cc and Subject fold)");
    mime::Parsed p = mime::parse(msg);
    std::vector<std::string> to, cc;
    for (const auto& a : mime::parse_addresses(p.header("To"))) to.push_back(a.addr);
    for (const auto& a : mime::parse_addresses(p.header("Cc"))) cc.push_back(a.addr);
    check(to == m.to, "To read back: " + join(to));
    check(cc == m.cc, "Cc read back: " + join(cc));
    check(p.decoded_header("Subject") == m.subject, "Subject read back: " + p.decoded_header("Subject"));
    mime::Address from = mime::parse_address(p.decoded_header("From"));
    check(from.name == m.from_name && from.addr == m.from, "From read back: " + from.name + " <" + from.addr + ">");
    check(p.text == m.text, "text read back: [" + p.text + "]");
    check(p.attachments.size() == 1 && p.attachments[0].filename == "pièce jointe.pdf" && p.attachments[0].size_bytes == 5000,
          "attachment read back");
    check(p.header("Message-ID") == id, "Message-ID");
  }
  // 2. A short message stays as it was: one line per header, 7bit text.
  {
    MailParts m;
    m.from = "a@x.com";
    m.to = {"b@y.com"};
    m.subject = "hi";
    m.text = "hello\n";
    std::string id, err;
    const std::string msg = mail_build_message(m, &id, &err);
    check(msg.find("To: b@y.com\r\n") != std::string::npos, "short To on one line");
    check(msg.find("Subject: hi\r\n") != std::string::npos, "short Subject on one line");
    check(msg.find("Content-Transfer-Encoding: 7bit") != std::string::npos, "ASCII text is 7bit");
  }
  // 3. A subject with no space to fold at is still under the hard limit.
  {
    MailParts m;
    m.from = "a@x.com";
    m.to = {"b@y.com"};
    m.subject = std::string(1200, 'w');
    m.text = "hello\n";
    std::string id, err;
    const std::string msg = mail_build_message(m, &id, &err);
    const size_t longest = longest_header_line(msg);
    check(longest <= 998, "unbreakable subject: a header line of " + std::to_string(longest) + " bytes");
    mime::Parsed p = mime::parse(msg);
    check(p.decoded_header("Subject") == m.subject, "unbreakable subject reads back whole");
  }
  if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
  std::printf("mail message: ok\n");
  return 0;
}
