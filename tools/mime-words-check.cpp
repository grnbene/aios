// RFC 2047 encoded words in headers: what decode_words makes of the forms senders actually
// produce. Adjacent words of one charset are one byte stream (a multi-byte character may be
// cut between them); words of different charsets are not joined; whitespace between two
// encoded words is dropped and kept beside plain text; a language tag on the charset is ignored.
#include <cstdio>
#include <string>

#include "aios/net/mime.hpp"

namespace {

int failures = 0;

void expect(const char* input, const std::string& want) {
  const std::string got = aios::net::mime::decode_words(input);
  if (got == want) return;
  ++failures;
  std::printf("FAIL  %s\n  want [%s]\n  got  [%s]\n", input, want.c_str(), got.c_str());
}

}  // namespace

int main() {
  // "é" is C3 A9. Cut at the character boundary, and cut in the middle of it, in both encodings.
  expect("=?UTF-8?B?w6k=?= =?UTF-8?B?dGU=?=", "\xC3\xA9te");
  expect("=?UTF-8?B?ww==?= =?UTF-8?B?qXQ=?=", "\xC3\xA9t");
  expect("=?UTF-8?Q?=C3?= =?UTF-8?Q?=A9?=", "\xC3\xA9");
  expect("=?UTF-8?Q?=C3?=\r\n =?UTF-8?Q?=A9?=", "\xC3\xA9");          // across a folded line
  expect("=?UTF-8?Q?=C3?=\t=?utf-8?B?qQ==?=", "\xC3\xA9");              // charset name case and encoding differ
  expect("=?UTF-8*en?Q?=C3?= =?utf-8*fr?Q?=A9?=", "\xC3\xA9");         // RFC 2231 language tags
  // Different charsets stay separate: a lone C3 is not UTF-8 (marked), A9 in Latin-1 is ©.
  expect("=?UTF-8?B?ww==?= =?ISO-8859-1?B?qQ==?=", "\xEF\xBF\xBD\xC2\xA9");
  // Plain text between words keeps its spaces; a word run flushes before the text.
  expect("=?UTF-8?B?w6k=?= and =?UTF-8?B?w6k=?=", "\xC3\xA9 and \xC3\xA9");
  expect("=?UTF-8?Q?a?= =?UTF-8?Q?b?= c =?UTF-8?Q?d?=", "ab c d");
  expect("=?utf-8?q?a_b?=", "a b");
  expect("plain =?UTF-8?Q?x?= tail", "plain x tail");
  expect("=?UTF-8?Q?=C3?= plain =?UTF-8?Q?=A9?=", "\xEF\xBF\xBD plain \xEF\xBF\xBD");   // text in between: two streams
  expect("=?UTF-8?X?zz?= =?UTF-8?Q?a?=", "=?UTF-8?X?zz?= a");                           // unknown encoding stays literal
  expect("no words here", "no words here");
  if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
  std::printf("mime words: ok\n");
  return 0;
}
