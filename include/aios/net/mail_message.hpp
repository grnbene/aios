#pragma once
// An outgoing email as RFC 5322 / MIME text: plain text, an HTML alternative, attachments, UTF-8 subjects and
// names as RFC 2047 encoded words, filenames as RFC 2231. Nothing a caller passes can add a header line:
// addresses and one-line fields with a line break are refused. Shared by Grain's mail_send and ASQL's mailers.

#include <string>
#include <vector>

namespace aios::net {

struct MailAttachment {
  std::string filename, type, data;
};

struct MailParts {
  std::string from, from_name, reply_to, subject, text, html;
  std::vector<std::string> to, cc, bcc;
  std::vector<MailAttachment> attachments;
};

// A bare address: one @, something on both sides, no spaces, brackets, commas or line breaks.
bool mail_plain_address(const std::string& a);
bool mail_has_break(const std::string& s);

// to + cc + bcc, in that order: who the message is delivered to (bcc is never named in a header).
std::vector<std::string> mail_recipients(const MailParts& m);

// The message text for `m`, its Message-ID in *message_id. On a bad address, a line break in a one-line field,
// no recipient or no body: empty, with *err saying which.
std::string mail_build_message(const MailParts& m, std::string* message_id, std::string* err);

}  // namespace aios::net
