// Drives the editor and the database studio the way a person's hands do, and
// reports what the panes actually held afterwards.
//
// Every call here is one the SDL backend and the bare-metal PS/2 driver make
// too: keys, typed text, and the mouse. Nothing reaches around the input path
// to set state directly, because "the editor works" is a claim about typing
// into it and the only way to know is to type into it.
//
//   aios-workstation-drive <root> <out-dir>
#include "aios/kernel/kernel.hpp"
#include "aios/kernel/surface.hpp"
#include "aios/hal/clipboard.hpp"
#include "aios/hal/keys.hpp"
#include "aios/userland/compositor.hpp"
#include "aios/userland/source_lang.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace aios;

namespace {

int failures = 0;

void check(bool ok, const std::string& what, const std::string& saw) {
  std::printf("  [%s] %s%s%s\n", ok ? "ok" : "FAIL", what.c_str(),
              saw.empty() ? "" : "  -> ", saw.c_str());
  if (!ok) failures++;
}

void write_ppm(const userland::HardwareCompositor& c, uint32_t w, uint32_t h, const std::string& path) {
  const auto& fb = c.framebuffer();
  std::ofstream out(path, std::ios::binary);
  out << "P6\n" << w << " " << h << "\n255\n";
  for (size_t i = 0; i < fb.size(); ++i) {
    uint32_t p = fb[i];
    char rgb[3] = {(char)((p >> 16) & 0xFF), (char)((p >> 8) & 0xFF), (char)(p & 0xFF)};
    out.write(rgb, 3);
  }
}

// Type a line at whichever prompt has the keyboard and press Enter, which is
// exactly what a person does.
void run_line(userland::HardwareCompositor& d, const std::string& text) {
  d.input_text(text);
  d.input_key(hal::KeyEnter);
}

std::string file_bytes(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "usage: aios-workstation-drive <root> <out-dir>\n"); return 2; }
  const std::string root = argv[1];
  const std::string out = argv[2];

  KernelConfig cfg;
  cfg.root = root;
  auto k = Kernel::boot(cfg);
  if (!k.ok()) { std::fprintf(stderr, "boot: %s\n", k.error().str().c_str()); return 1; }
  Kernel& kernel = **k;

  AuthorityProfile profile;
  auto me = kernel.resident("workstation-drive", profile);
  if (!me.ok()) { std::fprintf(stderr, "resident: %s\n", me.error().str().c_str()); return 1; }
  Surface console = kernel.as(*me);

  const uint32_t W = 1600, H = 1000;
  userland::HardwareCompositor desk(W, H);
  desk.set_fullscreen(false);
  desk.set_console(&console);
  desk.set_dispatcher([](const std::string&, userland::OutputCallback) {});
  desk.init_default_workspace(kernel);
  desk.set_canvas_mode(false);

  // A file holding the two things the old editor destroyed on the way in: a
  // real tab, and a character the ASCII font has no glyph for.
  const std::string sample = root + "/drive-sample.txt";
  const std::string original = "int main() {\n\treturn 0;  // caf\xc3\xa9\n}\n";
  { std::ofstream f(sample, std::ios::binary); f << original; }

  std::printf("EDITOR (F4 raises it; Ctrl+L is its command line)\n");
  desk.input_key(hal::KeyF4);
  desk.input_key(hal::KeyCtrlL);
  desk.input_text("open " + sample);
  desk.input_key(hal::KeyEnter);

  auto ws = desk.workstation();
  check(ws.editor_lines == 3, "opens the file", std::to_string(ws.editor_lines) + " lines");

  // Byte fidelity: save it straight back and compare with what went in.
  desk.input_key(hal::KeyCtrlS);
  {
    std::string got = file_bytes(sample);
    check(got == original, "open+save is byte-for-byte identical",
          std::to_string(got.size()) + " bytes vs " + std::to_string(original.size()) +
              (got.find('\t') != std::string::npos ? ", tab kept" : ", TAB LOST") +
              (got.find("\xc3\xa9") != std::string::npos ? ", utf-8 kept" : ", UTF-8 LOST"));
  }

  // Typing goes into the FILE, with no command-line detour.
  desk.input_text("X");
  ws = desk.workstation();
  check(ws.editor_dirty && ws.editor_line.rfind("Xint", 0) == 0,
        "typing lands in the buffer", ws.editor_line);

  desk.input_key(hal::KeyCtrlZ);
  ws = desk.workstation();
  check(ws.editor_line == "int main() {", "Ctrl+Z undoes it", ws.editor_line);

  desk.input_key(hal::KeyCtrlY);
  ws = desk.workstation();
  check(ws.editor_line.rfind("Xint", 0) == 0, "Ctrl+Y redoes it", ws.editor_line);
  desk.input_key(hal::KeyCtrlZ);

  // Select all, copy: a multi-line clipboard round trip the old pane could not do.
  desk.input_key(hal::KeyCtrlA);
  ws = desk.workstation();
  check(ws.editor_selection, "Ctrl+A selects the buffer", "selection active");
  desk.input_key(hal::KeyCtrlC);
  auto clip = hal::clipboard_get();
  check(clip.ok() && clip->find('\n') != std::string::npos && clip->find('\t') != std::string::npos,
        "Ctrl+C copies every selected line, tab and all",
        clip.ok() ? std::to_string(clip->size()) + " bytes" : "clipboard failed");

  // Paste it back over the selection: the buffer should be unchanged in shape.
  desk.input_key(hal::KeyCtrlV);
  ws = desk.workstation();
  check(ws.editor_lines == 3, "Ctrl+V pastes all of it, not just the first line",
        std::to_string(ws.editor_lines) + " lines");

  // Shift+Down drags a selection out of nothing.
  desk.input_key(hal::KeyCtrlL);
  run_line(desk, "goto 1");
  desk.set_shift_modifier(true);
  desk.input_key(hal::KeyDown);
  desk.set_shift_modifier(false);
  ws = desk.workstation();
  check(ws.editor_selection, "Shift+Down extends a selection", "selection active");
  desk.input_key(hal::KeyRight);   // drop it again

  // Find, then replace, then undo the replace.
  desk.input_key(hal::KeyCtrlL);
  run_line(desk, "find return");
  ws = desk.workstation();
  check(ws.editor_cursor_line == 1, "find moves the caret to the match",
        "line " + std::to_string(ws.editor_cursor_line + 1));

  desk.input_key(hal::KeyCtrlL);
  run_line(desk, "replace return yield");
  ws = desk.workstation();
  check(ws.editor_line.find("yield") != std::string::npos, "replace rewrites it", ws.editor_line);
  desk.input_key(hal::KeyCtrlZ);
  ws = desk.workstation();
  check(ws.editor_line.find("return") != std::string::npos, "undo puts it back", ws.editor_line);

  // Ctrl+D duplicates the caret's line.
  size_t before = desk.workstation().editor_lines;
  desk.input_key(hal::KeyCtrlD);
  ws = desk.workstation();
  check(ws.editor_lines == before + 1, "Ctrl+D duplicates the line",
        std::to_string(before) + " -> " + std::to_string(ws.editor_lines));
  desk.input_key(hal::KeyCtrlZ);

  // A second file opens as its own tab and the first is still open.
  const std::string second = root + "/drive-second.txt";
  { std::ofstream f(second); f << "second file\n"; }
  desk.input_key(hal::KeyCtrlL);
  run_line(desk, "open " + second);
  ws = desk.workstation();
  check(ws.editor_tabs == 2, "the second file opens as its own tab",
        std::to_string(ws.editor_tabs) + " tabs");
  check(ws.editor_line == "second file", "the live tab shows the new file", ws.editor_line);

  // Drag a selection out with the mouse, at the text the paint actually drew.
  desk.render_desktop(kernel);
  auto eg = desk.editor_geom();
  check(eg.ok, "the editor reports where its text is", "");
  if (eg.ok) {
    desk.input_mouse_button(eg.text_x + eg.char_w, eg.text_y + 6, true, hal::MouseLeft);
    desk.input_mouse_motion(eg.text_x + eg.char_w * 6, eg.text_y + 6);
    desk.input_mouse_button(eg.text_x + eg.char_w * 6, eg.text_y + 6, false, hal::MouseLeft);
    ws = desk.workstation();
    check(ws.editor_selection, "click-drag selects a range of text", "selection active");
  }

  std::printf("\nLANGUAGES\n");
  struct Sample { const char* name; const char* body; const char* expect; };
  const Sample samples[] = {
    {"drive.php",
     "<?php\nclass Cart {\n  private array $items = [];\n"
     "  public function total(): float { return 0.0; } // sum\n}\n",
     "PHP"},
    {"drive.component.ts",
     "import { Component } from '@angular/core';\n@Component({ selector: 'app-root' })\n"
     "export class AppComponent { title: string = 'x'; }\n",
     "TypeScript"},
    {"drive.component.html",
     "<div class=\"row\" *ngIf=\"ready\" [value]=\"n\" (click)=\"go()\">{{ title }}</div>\n",
     "HTML"},
    {"drive.md",
     "# Release notes\n\nA paragraph with **bold text**, some *emphasis*, `inline_code()`\n"
     "and a [link to the docs](https://example.com) in it.\n\n## What changed\n\n"
     "- The editor reads **PHP 7.4**, TypeScript and Angular templates\n"
     "- Tabs keep each file's own language\n- `Ctrl+P` toggles this preview\n\n"
     "> A quoted line, set behind a rule.\n\n```cpp\nint main() { return 0; }  // fenced\n```\n\n"
     "### Smaller heading\n\n1. first\n2. second\n",
     "Markdown"},
    {"drive.py", "def go(n):\n    return n * 2  # doubled\n", "Python"},
    {"drive.sql", "SELECT * FROM t WHERE n > 1; -- a note\n", "SQL"},
    {"drive.yml", "name: build\non: [push]\n", "YAML"},
    {"drive.go", "package main\nfunc main() { println(\"hi\") }\n", "Go"},
  };
  for (const Sample& sp : samples) {
    std::string path = root + "/" + sp.name;
    { std::ofstream f(path); f << sp.body; }
    desk.input_key(hal::KeyCtrlL);
    run_line(desk, "open " + path);
    ws = desk.workstation();
    check(ws.editor_lang == sp.expect,
          std::string(sp.name) + " is read as " + sp.expect, ws.editor_lang);
  }

  std::printf("\nMARKDOWN PREVIEW\n");
  desk.input_key(hal::KeyCtrlL);
  run_line(desk, "open " + root + "/drive.md");
  ws = desk.workstation();
  check(ws.editor_preview, "a Markdown file opens rendered", ws.editor_lang);

  // Typing at a preview must not change the file underneath it.
  bool dirty_before = desk.workstation().editor_dirty;
  desk.input_text("ZZZ");
  ws = desk.workstation();
  check(ws.editor_dirty == dirty_before, "typing at a preview does not edit the file",
        ws.editor_dirty ? "modified" : "unchanged");

  desk.render_desktop(kernel);
  write_ppm(desk, W, H, out + "/markdown-preview.ppm");

  desk.input_key(hal::KeyCtrlP);
  ws = desk.workstation();
  check(!ws.editor_preview, "Ctrl+P goes back to the source", ws.editor_status);
  desk.input_text("Z");
  ws = desk.workstation();
  check(desk.workstation().editor_dirty, "and typing edits again", "modified");
  desk.input_key(hal::KeyCtrlZ);

  // The grammars themselves, on the constructs each language is made of.
  std::printf("\nGRAMMARS\n");
  {
    using namespace aios::userland;
    auto classes_in = [](SourceLang lang, const std::string& line) {
      HighlightState st;
      std::vector<SourceSpan> sp;
      highlight_line(lang, line, &st, &sp);
      return sp;
    };
    auto has = [](const std::vector<SourceSpan>& sp, TokenClass c) {
      for (const auto& s2 : sp) if (s2.cls == c) return true;
      return false;
    };
    // A PHP file is HTML until <?php says otherwise, so the state is threaded
    // exactly as the editor threads it down a real file.
    auto php_lines = [](std::vector<std::string> ls) {
      HighlightState st;
      std::vector<SourceSpan> sp;
      for (const auto& l : ls) highlight_line(SourceLang::Php, l, &st, &sp);
      return sp;
    };
    auto outside = classes_in(SourceLang::Php, "<div class=\"x\">before the tag</div>");
    check(has(outside, TokenClass::Tag),
          "PHP: the HTML before <?php is painted as HTML", "");
    auto php = php_lines({"<?php", "  private array $items = []; // note"});
    check(has(php, TokenClass::Keyword) && has(php, TokenClass::Variable) &&
              has(php, TokenClass::Comment),
          "PHP: keyword, $variable and comment are told apart", "");
    auto php74 = php_lines({"<?php", "$f = fn($x) => $x * 2;"});
    check(has(php74, TokenClass::Keyword), "PHP 7.4: fn is a keyword, not a stray word", "");
    auto ts = classes_in(SourceLang::TypeScript, "@Component({ selector: 'app' })");
    check(has(ts, TokenClass::Decorator) && has(ts, TokenClass::String),
          "TypeScript: a decorator and a string", "");
    auto ng = classes_in(SourceLang::Html, "<div *ngIf=\"a\" [v]=\"b\" (click)=\"c()\">{{ t }}</div>");
    check(has(ng, TokenClass::Tag) && has(ng, TokenClass::Decorator) &&
              has(ng, TokenClass::Variable),
          "Angular template: tag, binding and interpolation", "");
    auto md = classes_in(SourceLang::Markdown, "# Heading with **bold**");
    check(has(md, TokenClass::Heading), "Markdown: a heading line", "");
    // The thing the old highlighter got wrong every time.
    auto strk = classes_in(SourceLang::Cpp, "const char* s = \"return nothing\";");
    bool keyword_inside_string = false;
    for (const auto& s2 : strk)
      if (s2.cls == TokenClass::Keyword && s2.start > 16) keyword_inside_string = true;
    check(!keyword_inside_string, "a keyword inside a string stays a string", "");
  }

  // A frame of each of the three the request named, so the colouring can be
  // looked at rather than taken on trust.
  for (const char* f : {"drive.php", "drive.component.ts", "drive.component.html"}) {
    desk.input_key(hal::KeyCtrlL);
    run_line(desk, "open " + root + "/" + f);
    desk.render_desktop(kernel);
    write_ppm(desk, W, H, out + "/" + std::string(f) + ".ppm");
  }

  desk.render_desktop(kernel);
  write_ppm(desk, W, H, out + "/editor.ppm");

  std::printf("\nDATABASE STUDIO (F5 raises it; a statement runs on Enter)\n");
  desk.input_key(hal::KeyF5);
  // Start from nothing, so running this twice against one root cannot report a
  // failure that is only last run's rows still sitting there.
  run_line(desk, "DROP TABLE IF EXISTS drive_t");
  run_line(desk, "CREATE TABLE drive_t (id INT PRIMARY KEY, name TEXT, n INT)");
  run_line(desk, "INSERT INTO drive_t VALUES (1,'alice',5),(2,'bob',9),(3,'carol',7)");
  ws = desk.workstation();
  check(ws.db_connected && !ws.db_error, "the studio runs against the engine", ws.db_status);

  run_line(desk, "SELECT * FROM drive_t WHERE n > 6");
  ws = desk.workstation();
  check(ws.db_rows == 2 && !ws.db_error, "a > predicate returns the two matching rows",
        std::to_string(ws.db_rows) + " row(s); " + ws.db_status);

  run_line(desk, "SELECT * FROM drive_t WHERE nosuch = 1");
  ws = desk.workstation();
  check(ws.db_error, "an unknown column is an error, not every row", ws.db_status);

  run_line(desk, "SELECT name, n FROM drive_t ORDER BY n DESC LIMIT 2");
  ws = desk.workstation();
  check(ws.db_rows == 2 && ws.db_columns == 2, "ORDER BY and LIMIT reach the grid",
        std::to_string(ws.db_rows) + " x " + std::to_string(ws.db_columns));

  run_line(desk, "DELETE FROM drive_t WHERE n > 6");
  run_line(desk, "SELECT * FROM drive_t");
  ws = desk.workstation();
  check(ws.db_rows == 1, "DELETE with > removed exactly the two it matched",
        std::to_string(ws.db_rows) + " row(s) left");

  run_line(desk, "INSERT INTO drive_t VALUES (7,'zoe',1),(8,'adam',3)");
  run_line(desk, "INSERT INTO drive_t (id,name) VALUES (9,'nobody')");
  run_line(desk, "SELECT n FROM drive_t WHERE id = 9");
  check(desk.grid_cell(0, 0) == "NULL", "a NULL cell reaches the grid as the word NULL",
        "cell = '" + desk.grid_cell(0, 0) + "'");

  // Everything below is done with the mouse, at the rectangles the paint uses.
  desk.render_desktop(kernel);   // the geometry is measured off a painted frame
  auto g = desk.studio_geom();
  check(g.ok, "the studio reports where its controls are", "");

  // Click the first table in the explorer: that is what makes the grid a
  // table's rows rather than a statement's result.
  desk.input_mouse_button(g.tree_x, g.tree_y + 6, true, hal::MouseLeft);
  desk.render_desktop(kernel);
  ws = desk.workstation();
  check(ws.db_rows >= 1 && ws.db_grid == ws.db_selected, "clicking a table opens its rows",
        ws.db_selected + ": " + std::to_string(ws.db_rows) + " row(s)");

  // Sort by clicking a column header, then again to turn it around.
  g = desk.studio_geom();
  if (g.column_w.size() >= 2) {
    int col2_x = g.cell_x + g.column_w[0] + 4;
    desk.input_mouse_button(col2_x, g.header_y, true, hal::MouseLeft);
    desk.render_desktop(kernel);
    std::string first_asc = desk.grid_cell(0, 1);
    desk.input_mouse_button(col2_x, g.header_y, true, hal::MouseLeft);
    desk.render_desktop(kernel);
    std::string first_desc = desk.grid_cell(0, 1);
    check(!first_asc.empty() && first_asc != first_desc,
          "a header click sorts, and a second click reverses it",
          first_asc + " -> " + first_desc);
  }

  // Edit a cell in place: the first click picks the row, the second opens the
  // cell, then the value is typed and Enter writes the UPDATE.
  g = desk.studio_geom();
  int cell_x = g.cell_x + (g.column_w.empty() ? 40 : g.column_w[0] + 4);
  int cell_y = g.cell_y + 6;
  desk.input_mouse_button(cell_x, cell_y, true, hal::MouseLeft);
  desk.input_mouse_button(cell_x, cell_y, true, hal::MouseLeft);
  desk.render_desktop(kernel);
  desk.input_key(hal::KeyCtrlU);            // clear the field
  desk.input_text("edited");
  desk.input_key(hal::KeyEnter);
  desk.render_desktop(kernel);
  ws = desk.workstation();
  check(!ws.db_error && ws.db_status.find("updated") != std::string::npos,
        "typing in a cell and pressing Enter writes an UPDATE", ws.db_status);

  // And the engine really holds it now, not just the grid.
  run_line(desk, "SELECT * FROM drive_t WHERE name = 'edited'");
  ws = desk.workstation();
  check(ws.db_rows == 1, "the edited value is in the table, not just on screen",
        std::to_string(ws.db_rows) + " row(s) match");

  desk.render_desktop(kernel);
  write_ppm(desk, W, H, out + "/studio.ppm");

  std::printf("\n%s\n", failures ? "SOME CHECKS FAILED" : "every check passed");
  return failures ? 1 : 0;
}
