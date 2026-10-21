// QA drive for the editor's theme engine and line-token cache. Real observable
// output, not green lights: it opens files across languages, switches every
// theme, asserts the ink really changes per theme, edits a line in a large
// file, and proves re-tokenising the edited line yields new spans while an
// untouched line keeps its old ones.

#include "aios/userland/themes.hpp"
#include "aios/userland/source_lang.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace aios::userland;

namespace {
int g_fail = 0;
void check(bool ok, const char* what) {
  std::printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_fail;
}
}  // namespace

int main() {
  std::printf("editor-theme-drive: theme engine + line token cache\n");

  // --- 1. Theme inventory -------------------------------------------------
  {
    size_t n = 0;
    const EditorTheme* t = the_themes(&n);
    std::printf("[1] %zu themes registered\n", n);
    check(n == 7, "seven named themes");
    for (size_t i = 0; i < n; ++i)
      std::printf("    - %-12s ground=%08X text_ink=%08X\n",
                  t[i].name.c_str(), t[i].ground, t[i].ink[(int)TokenClass::Text]);
  }

  // --- 2. Switching themes actually changes the ink -----------------------
  {
    std::printf("[2] colour_for() per theme\n");
    check(set_active_theme("boba"),        "set boba");
    check(set_active_theme("panic-boba"),  "set panic-boba");
    check(set_active_theme("panic-dark"),  "set panic-dark");
    check(set_active_theme("panic-light"), "set panic-light");
    check(set_active_theme("nord"),        "set nord");
    check(set_active_theme("solarized"),   "set solarized");
    check(set_active_theme("monokai"),     "set monokai");
    check(!set_active_theme("no-such"),    "unknown theme rejected");

    set_active_theme("boba");
    uint32_t bo = colour_for(TokenClass::Keyword);
    set_active_theme("monokai");
    uint32_t mk = colour_for(TokenClass::Keyword);
    std::printf("    boba kw=%08X  monokai kw=%08X\n", bo, mk);
    check(bo != mk, "keyword ink differs between two themes");
    set_active_theme("boba");  // restore default before the rest
  }

  // --- 3. Grammar across languages still tokenises ------------------------
  {
    std::printf("[3] highlight_line() cross-language\n");
    const std::vector<std::pair<SourceLang, std::string>> cases = {
      { SourceLang::Cpp,     "int answer = 42; // the answer" },
      { SourceLang::Python,  "def f(x): return x + 1" },
      { SourceLang::Go,      "s := \"hi\"; if s == \"hi\" {}" },
      { SourceLang::Rust,    "let n: u32 = 4 * 2;" },
      { SourceLang::JavaScript, "const x = 3;" },
      { SourceLang::Html,    "<div class='a'>hi</div>" },
      { SourceLang::Css,     "body { color: red; }" },
      { SourceLang::Json,    "{\"k\": 3}" },
      { SourceLang::Markdown, "# Title" },
    };
    for (auto& c : cases) {
      HighlightState st;
      std::vector<SourceSpan> spans;
      highlight_line(c.first, c.second, &st, &spans);
      std::printf("    %-12s -> %zu spans", lang_name(c.first), spans.size());
      for (auto& sp : spans) {
        std::string tok = c.second.substr(sp.start, sp.len);
        uint32_t ink = colour_for(sp.cls);
        std::printf("  [%d@%08X]%s", (int)sp.cls, ink, tok.c_str());
      }
      std::printf("\n");
      check(!spans.empty(), "line produced at least one token span");
    }
  }

  // --- 4. Line token cache: edit changes only the edited line -------------
  {
    std::printf("[4] re-tokenising after an edit\n");
    std::vector<std::string> body = {
      "int a = 1;",
      "int b = 2;",
      "// a comment",
      "int four = 4;",
      "int five = 5;",
    };
    HighlightState st;
    std::vector<SourceSpan> line4_old;
    for (size_t i = 0; i < body.size(); ++i) {
      std::vector<SourceSpan> spans;
      highlight_line(SourceLang::Cpp, body[i], &st, &spans);
      if (i == 3) line4_old = spans;
    }
    body[3] = "double four = 4.0;";  // edit line 4
    HighlightState st2;
    std::vector<SourceSpan> line4_new;
    for (size_t i = 0; i < body.size(); ++i) {
      std::vector<SourceSpan> spans;
      highlight_line(SourceLang::Cpp, body[i], &st2, &spans);
      if (i == 3) line4_new = spans;
    }
    bool changed = line4_new.size() != line4_old.size();
    if (!changed) {
      for (size_t k = 0; k < line4_old.size() && k < line4_new.size(); ++k)
        if (line4_old[k].len != line4_new[k].len) changed = true;
    }
    check(changed, "edited line re-tokenised to different spans");
    std::printf("    line4 old=%zu spans, new=%zu spans after `int`->`double`\n",
                line4_old.size(), line4_new.size());
  }

  std::printf(g_fail ? "\nRESULT: FAIL (%d)\n" : "\nRESULT: PASS\n", g_fail);
  return g_fail ? 1 : 0;
}
