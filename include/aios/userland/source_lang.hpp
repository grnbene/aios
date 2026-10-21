#pragma once
// What language a file is in, and how one of its lines breaks into coloured
// pieces.
//
// The editor used to colour a line by asking whether the word "return"
// appeared anywhere in it, with one keyword set for everything. That reads a
// C++ file badly and every other kind of file wrongly: a PHP variable, an
// Angular binding, a YAML key and a Markdown heading are not the same thing
// and must not be painted as though they were.
//
// Highlighting says what a piece IS, not what colour to make it. The
// compositor owns the palette; this owns the grammar.
#include <cstdint>
#include <string>
#include <vector>

namespace aios::userland {

enum class SourceLang : uint8_t {
  PlainText,
  C, Cpp, CSharp, Java, Kotlin, Swift, Go, Rust,
  Python, Ruby, Lua, Php,
  JavaScript, TypeScript,
  Html, Xml, Css, Json, Yaml, Toml,
  Markdown, Shell, Sql, Cmake, Makefile, Dockerfile, Diff
};

// What a run of characters is. The editor maps these to colours; nothing here
// knows what colour anything is.
enum class TokenClass : uint8_t {
  Text,
  Keyword,      // if, class, function
  Type,         // int, string, a Capitalised name in a typed language
  String,       // "..." '...' `...`
  Comment,
  Number,
  Preproc,      // #include, <?php, a shebang
  Variable,     // $x in PHP or the shell, {{ x }} in a template
  Function,     // a name with a '(' straight after it
  Decorator,    // @Component, an attribute
  Tag,          // <div>
  Attribute,    // class=, [ngModel], (click)
  Heading,      // Markdown # line
  Emphasis,     // **bold**, *italic*
  Link,         // [text](url)
  Code,         // `inline code`, a fenced block
  Rule          // ---, a table pipe, a list bullet, a blockquote mark
};

struct SourceSpan {
  uint32_t start = 0;
  uint32_t len = 0;
  TokenClass cls = TokenClass::Text;
};

// What carries from one line to the next: a /* */ that has not closed, a
// Markdown fence that is still open, and whether a PHP file is currently
// inside <?php or back out in the HTML around it.
struct HighlightState {
  bool in_block_comment = false;
  bool in_fence = false;
  bool in_php_block = false;
};

// How a single line of source breaks into coloured pieces. `st` carries the
// state in from the previous line and returns the state to pass into the
// next line, so multi-line constructs (/* */, Markdown fences, <?php blocks)
// stay coloured correctly across newlines.
void highlight_line(const std::string& line, std::vector<SourceSpan>* out,
                    HighlightState* st);

// The language a path names, from its extension or its whole filename.
SourceLang lang_for_path(const std::string& path);
// How that language is written down for a person to read.
const char* lang_name(SourceLang lang);
// Markdown is the one the editor can also show rendered rather than as source.
bool lang_has_preview(SourceLang lang);

// One line, split into spans. `state` is threaded through the file in order:
// a line is not highlightable on its own when a block comment or a fence may
// be open above it.
void highlight_line(SourceLang lang, const std::string& line, HighlightState* state,
                    std::vector<SourceSpan>* out);

// --- Markdown, rendered ----------------------------------------------------

enum class MarkdownBlock : uint8_t {
  Paragraph, Heading1, Heading2, Heading3, Bullet, Numbered, Quote, Code, Rule, TableRow, Blank
};

// A piece of a rendered line, with its markers already taken off: a preview
// that still shows the asterisks around a bold word is not a preview.
struct MarkdownRun {
  std::string text;
  TokenClass cls = TokenClass::Text;   // Text, Emphasis, Code or Link
};

// One line of a Markdown document, as something to draw rather than as source.
struct MarkdownLine {
  MarkdownBlock block = MarkdownBlock::Paragraph;
  std::string text;                // the whole line, markers removed
  int indent = 0;                  // nesting level for lists and quotes
  std::string marker;              // what a list item is numbered or bulleted with
  std::vector<MarkdownRun> runs;   // that same text, split by how it is styled
};

// A whole document, turned into lines to draw.
void markdown_render(const std::vector<std::string>& lines, std::vector<MarkdownLine>* out);

}  // namespace aios::userland
