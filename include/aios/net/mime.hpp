#pragma once
// MIME for mail: what Python's `email` package gave the old IMAP worker, written down once and in
// this tree, so reading a message needs no other program on the machine. No exceptions, no regex,
// nothing off the heap that a bare-metal build does not have.
//
// Supported: RFC 5322 headers (folding), RFC 2047 encoded words, RFC 2045 transfer encodings
// (base64, quoted-printable, 7bit/8bit/binary), RFC 2231 parameters (continuations and charsets),
// multipart nesting, RFC 5322 dates and addresses. Charsets: UTF-8, US-ASCII, ISO-8859-1/2/15,
// windows-1250/1251/1252, KOI8-R. A message in any other charset has its non-ASCII bytes replaced
// with U+FFFD rather than guessed at; there is no table for it in this tree.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aios::net::mime {

struct Address {
  std::string name;   // decoded to UTF-8
  std::string addr;
};

struct AttachmentInfo {
  std::string filename;     // decoded to UTF-8
  std::string mime_type;
  std::string content_id;
  size_t size_bytes = 0;    // decoded size
};

struct Parsed {
  std::vector<std::pair<std::string, std::string>> headers;   // names as written, values unfolded, undecoded
  std::string text;          // the first text part: text/plain as is, text/html as plain text (UTF-8)
  std::string html;          // the first text/html part (UTF-8) with cid: images folded in as data: urls
  std::vector<AttachmentInfo> attachments;

  // First header called `name` (case-insensitive), raw and unfolded; empty if absent.
  std::string header(std::string_view name) const;
  // The same with RFC 2047 words decoded and whitespace collapsed.
  std::string decoded_header(std::string_view name) const;
};

Parsed parse(std::string_view raw);

// --- pieces, exposed because they are useful alone and testable alone ------------------------------

std::string decode_base64(std::string_view in);                                 // lenient: skips whitespace and junk
std::string decode_quoted_printable(std::string_view in, bool header_mode = false);   // header_mode: '_' is a space
// `data` in `charset` to UTF-8. An unknown charset or an invalid sequence gives U+FFFD, never an error.
std::string to_utf8(std::string_view data, std::string_view charset);
bool charset_supported(std::string_view charset);
// RFC 2047: "=?utf-8?B?...?=" words decoded, whitespace between adjacent words dropped, runs of
// whitespace collapsed to one space, ends trimmed.
std::string decode_words(std::string_view value);

// Every address in a header value ("A <a@x>, b@y, \"C, D\" <c@z>", group syntax flattened).
std::vector<Address> parse_addresses(std::string_view value);
Address parse_address(std::string_view value);   // the first, or empty

// RFC 5322 date to seconds since the epoch (UTC). `offset_minutes` is the zone the date was written
// in. Returns false when the text is not a date.
bool parse_date(std::string_view text, int64_t& epoch, int& offset_minutes);
// "Fri, 16 Oct 2026 16:50:49 +0000": the Date: header for `epoch`, in UTC. Written out by hand because a bare-metal
// libc has neither strftime nor gmtime_r.
std::string format_rfc5322_date(int64_t epoch);
// "Oct 16 2026", the calendar date in the zone the message was written in.
std::string format_date(int64_t epoch, int offset_minutes);

// Plain text out of html: style and script dropped, <br> and </p> as line breaks, tags removed,
// the common entities decoded.
std::string html_to_text(std::string_view html);

// "Content-Type"-style value into the bare type and its parameters (names lower-cased, RFC 2231
// continuations joined and decoded).
struct TypedValue {
  std::string value;   // lower-cased, e.g. "text/plain"
  std::vector<std::pair<std::string, std::string>> params;
  std::string param(std::string_view name) const;
};
TypedValue parse_typed_value(std::string_view header_value);

}  // namespace aios::net::mime
