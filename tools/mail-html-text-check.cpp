// The text a reader sees of an HTML message: what html_to_text makes of the markup senders actually
// produce. A table of rows (the shape of most notification mail) reads as lines, not one run; a list
// is one item per line; a comment's text does not leak; the Latin-1 named entities (&eacute;, &ntilde;,
// &uuml;) and the typographic ones (&rsquo;, &mdash;, &hellip;) are the characters, not left as names.
#include <cstdio>
#include <string>

#include "aios/net/mime.hpp"

namespace {

int failures = 0;

void expect(const char* what, const std::string& html, const std::string& want) {
  const std::string got = aios::net::mime::html_to_text(html);
  if (got == want) return;
  ++failures;
  std::printf("FAIL  %s\n  want [%s]\n  got  [%s]\n", what, want.c_str(), got.c_str());
}

}  // namespace

int main() {
  expect("rows become lines",
         "<table><tr><td>Order</td><td>48213</td></tr><tr><td>Status</td><td>Shipped</td></tr></table>",
         "Order 48213\n\nStatus Shipped");
  expect("divs become lines", "<div>one</div><div>two</div>", "one\n\ntwo");
  expect("list items on their own lines", "<ul><li>apples</li><li>pears</li></ul>", "apples\n\npears");
  expect("headings too", "<h1>Title</h1><p>Body text.</p>", "Title\n\nBody text.");
  expect("a comment's text does not leak", "before<!-- hidden preheader text -->after", "beforeafter");
  expect("an unclosed comment eats to the end", "shown<!-- not shown", "shown");
  expect("br and p as before", "a<br>b<br/>c</p>d", "a\nb\nc\n\nd");
  expect("inline tags vanish", "<b>bold</b> and <a href=\"x\">link</a>", "bold and link");
  expect("style and script go whole", "<style>p{}</style>x<script>var y;</script>", "x");
  expect("Latin-1 names", "caf&eacute; se&ntilde;or M&uuml;ller &Eacute;cole", "café señor Müller École");
  expect("typographic names", "it&rsquo;s &mdash; wait&hellip; &copy; 2026", "it\xE2\x80\x99s \xE2\x80\x94 wait\xE2\x80\xA6 \xC2\xA9 2026");
  expect("the markup five and nbsp", "a&amp;b &lt;c&gt; &quot;d&quot; &apos;e&apos; f&nbsp;g", "a&b <c> \"d\" 'e' f g");
  expect("numeric references", "&#233; &#xE9; &#8217;", "\xC3\xA9 \xC3\xA9 \xE2\x80\x99");
  expect("an unknown name stays as written", "&foo; &bar", "&foo; &bar");
  expect("tag names are case-insensitive", "<DIV>a</DIV><TR><TD>b</TD></TR>", "a\n\nb");
  expect("trimmed at the ends", "  <p>x</p>  ", "x");
  if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
  std::printf("mail html text: ok\n");
  return 0;
}
