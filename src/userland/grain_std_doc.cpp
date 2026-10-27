// grain standard library: document generation and compression (pdf_render, zip_create, unzip, xlsx_create)
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "aios/core/bytes.hpp"
#include "aios/net/deflate.hpp"
#include "aios/userland/archive.hpp"
#include "aios/userland/grain.hpp"
#include "aios/userland/web.hpp"
#include "grain_builtins_internal.hpp"

namespace aios::userland::grain {

namespace {

inline std::shared_ptr<MapObj> as_map(const Value& v) {
  return v.type == Type::Map && v.obj ? std::static_pointer_cast<MapObj>(v.obj) : nullptr;
}

// Escapes special characters for PDF text string literals
// Text for a PDF string in the standard fonts' WinAnsiEncoding: UTF-8 decoded, Latin-1 and the Windows-1252 extras
// (dashes, quotes, bullet, euro, ellipsis) as their single bytes, written as octal escapes; anything else '?'. A
// non-breaking space used to come out as '?' - every byte past ASCII did.
std::string pdf_escape_text(const std::string& t) {
  std::string o;
  auto byte = [&o](unsigned v) {
    char b[8];
    std::snprintf(b, sizeof b, "\\%03o", v & 0xFF);
    o += b;
  };
  for (size_t i = 0; i < t.size();) {
    unsigned char c = (unsigned char)t[i];
    uint32_t cp = c;
    size_t n = 1;
    if (c >= 0xF0 && i + 3 < t.size()) { cp = ((c & 0x07u) << 18) | ((t[i + 1] & 0x3Fu) << 12) | ((t[i + 2] & 0x3Fu) << 6) | (t[i + 3] & 0x3Fu); n = 4; }
    else if (c >= 0xE0 && i + 2 < t.size()) { cp = ((c & 0x0Fu) << 12) | ((t[i + 1] & 0x3Fu) << 6) | (t[i + 2] & 0x3Fu); n = 3; }
    else if (c >= 0xC0 && i + 1 < t.size()) { cp = ((c & 0x1Fu) << 6) | (t[i + 1] & 0x3Fu); n = 2; }
    i += n;
    if (cp == '(' || cp == ')' || cp == '\\') { o += '\\'; o += (char)cp; continue; }
    if (cp >= 0x20 && cp < 0x7F) { o += (char)cp; continue; }
    if (cp >= 0xA0 && cp <= 0xFF) { byte(cp); continue; }
    switch (cp) {
      case 0x20AC: byte(0x80); break; case 0x2026: byte(0x85); break; case 0x2018: byte(0x91); break;
      case 0x2019: byte(0x92); break; case 0x201C: byte(0x93); break; case 0x201D: byte(0x94); break;
      case 0x2022: byte(0x95); break; case 0x2013: byte(0x96); break; case 0x2014: byte(0x97); break;
      case 0x2122: byte(0x99); break;
      default: if (cp >= 0x80) o += '?'; break;
    }
  }
  return o;
}

std::string pdf_rgb(uint32_t c) {
  char b[48];
  std::snprintf(b, sizeof b, "%.3f %.3f %.3f", ((c >> 16) & 0xFF) / 255.0, ((c >> 8) & 0xFF) / 255.0, (c & 0xFF) / 255.0);
  return std::string(b);
}

// Convert 0-indexed column number to Excel column letters (0 -> "A", 25 -> "Z", 26 -> "AA")
std::string excel_col_name(int i) {
  std::string s;
  for (++i; i > 0; i = (i - 1) / 26) {
    s = (char)('A' + (i - 1) % 26) + s;
  }
  return s;
}

std::string xml_escape(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 16);
  for (char c : s) {
    switch (c) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '"': o += "&quot;"; break;
      case '\'': o += "&apos;"; break;
      default: o += c; break;
    }
  }
  return o;
}

int resolve_style_id(const Value& s_val) {
  if (s_val.type == Type::Int) return (int)s_val.as_int();
  if (s_val.type == Type::Str) {
    const std::string& s = s_val.str();
    if (s == "plain" || s == "normal") return 0;
    if (s == "section" || s == "section_header") return 1;
    if (s == "header" || s == "col_header") return 2;
    if (s == "contrib_header" || s == "header_contrib") return 3;
    if (s == "int" || s == "integer") return 4;
    if (s == "dec" || s == "decimal") return 5;
    if (s == "contrib_int" || s == "int_contrib") return 6;
    if (s == "contrib_dec" || s == "dec_contrib") return 7;
    if (s == "total_text" || s == "total") return 8;
    if (s == "total_int") return 9;
    if (s == "total_dec") return 10;
    if (s == "total_contrib_int" || s == "total_contrib") return 11;
    if (s == "total_contrib_dec") return 12;
    if (s == "title") return 13;
  }
  return 0;
}

// ---- Native: pdf_render(html, [css], [options]) -----------------------------
Value bi_pdf_render(Vm&, std::vector<Value>& args, std::string& error) {
  if (args.empty()) {
    error = "pdf_render() requires at least 1 argument (html)";
    return Value::nil();
  }
  std::string html;
  if (args[0].type == Type::Str) {
    html = args[0].str();
  } else if (args[0].type == Type::Bytes) {
    const auto& d = args[0].bytes_data();
    html.assign(reinterpret_cast<const char*>(d.data()), d.size());
  } else {
    error = "pdf_render() argument 1 (html) must be string or bytes";
    return Value::nil();
  }

  std::string css;
  if (args.size() > 1 && args[1].type == Type::Str) {
    css = args[1].str();
  }

  // Default geometry: Letter Landscape 792 x 612 pt
  int page_w = 792;
  int page_h = 612;
  int margin_top = 24;
  int margin_bottom = 24;
  int margin_left = 24;
  int margin_right = 24;
  std::string header_text;
  std::string footer_text;

  if (args.size() > 2 && args[2].type == Type::Map) {
    auto m = as_map(args[2]);
    if (Value* w = m->find("width")) if (w->is_number()) page_w = (int)w->as_int();
    if (Value* h = m->find("height")) if (h->is_number()) page_h = (int)h->as_int();
    if (Value* sz = m->find("page_size")) {
      std::string s = sz->to_text();
      for (char& c : s) c = (char)std::tolower(c);
      if (s == "letter") { page_w = 612; page_h = 792; }
      else if (s == "a4") { page_w = 595; page_h = 842; }
      else if (s == "legal") { page_w = 612; page_h = 1008; }
    }
    bool landscape = true;  // default to landscape for warehouse reports unless overridden
    if (Value* ls = m->find("landscape")) {
      if (ls->type == Type::Bool) landscape = ls->b;
    } else if (Value* orien = m->find("orientation")) {
      std::string o = orien->to_text();
      for (char& c : o) c = (char)std::tolower(c);
      if (o == "portrait") landscape = false;
      else if (o == "landscape") landscape = true;
    }
    if (landscape && page_w < page_h) std::swap(page_w, page_h);
    else if (!landscape && page_w > page_h) std::swap(page_w, page_h);

    if (Value* mt = m->find("margin_top")) if (mt->is_number()) margin_top = (int)mt->as_int();
    if (Value* mb = m->find("margin_bottom")) if (mb->is_number()) margin_bottom = (int)mb->as_int();
    if (Value* ml = m->find("margin_left")) if (ml->is_number()) margin_left = (int)ml->as_int();
    if (Value* mr = m->find("margin_right")) if (mr->is_number()) margin_right = (int)mr->as_int();
    if (Value* hd = m->find("header")) header_text = hd->to_text();
    if (Value* ft = m->find("footer")) footer_text = ft->to_text();
  }

  int printable_w = page_w - margin_left - margin_right;
  int printable_h = page_h - margin_top - margin_bottom;
  if (printable_w < 100) printable_w = 100;
  if (printable_h < 100) printable_h = 100;

  // A fragment (a table, a few divs) is given a document around it: the layout styles from <body> down, and with no
  // body there was nothing for a page's or the paper's colours to apply to.
  {
    std::string lower_html = html.substr(0, 4096);
    for (char& ch : lower_html) ch = (char)std::tolower((unsigned char)ch);
    if (lower_html.find("<body") == std::string::npos && lower_html.find("<html") == std::string::npos)
      html = "<!doctype html><html><head><meta charset=\"utf-8\"></head><body>" + html + "</body></html>";
  }
  auto dom = userland::web::parse_html(ByteSpan(reinterpret_cast<const uint8_t*>(html.data()), html.size()));
  if (!dom) {
    error = "pdf_render() failed to parse HTML";
    return Value::nil();
  }

  // Paper: black text on white unless the document says otherwise. The engine's own defaults are a screen's, and a
  // page that set no colour came out in its pale default text.
  userland::web::Stylesheet sheet = userland::web::parse_css("html, body { color: #000000; background-color: #ffffff; }");
  sheet.append(userland::web::collect_author_css(*dom, "", {}));
  if (!css.empty()) sheet.append(userland::web::parse_css(css));

  userland::web::Document doc;
  doc.dom = std::move(dom);

  userland::web::Metrics m;
  m.measure = [](const std::string& t, const userland::web::FontKey& f) {
    int px = f.px > 0 ? (int)f.px : 12, n = 0;
    for (unsigned char c : t) if ((c & 0xC0) != 0x80) ++n;
    return (n * px * 52 + 99) / 100;
  };
  m.line_height = [](const userland::web::FontKey& f) { int px = f.px > 0 ? (int)f.px : 12; return (px * 12 + 9) / 10; };
  m.ascent = [](const userland::web::FontKey& f) { int px = f.px > 0 ? (int)f.px : 12; return (px * 8 + 9) / 10; };

  userland::web::layout(doc, sheet, m, printable_w, printable_h);

  // Content wider than the page (a wide table whose columns cannot shrink further) is laid out again at the width it
  // needs and the whole page scaled down to fit, fonts and all - what mPDF's shrink_tables_to_fit does for the PHP
  // reports. It used to run off the right edge. Content that fits is drawn 1:1, as before.
  auto right_edge = [&doc]() {
    int r = 0;
    for (const auto& f : doc.fragments)
      if (f.w > 0 && f.x + f.w > r) r = f.x + f.w;
    return r;
  };
  double scale = 1.0;
  int layout_w = printable_w;
  if (int r = right_edge(); r > printable_w + 1) {
    layout_w = r;
    for (int pass = 0; pass < 3; ++pass) {   // a wider layout can itself want a little more
      userland::web::layout(doc, sheet, m, layout_w, (int)((double)printable_h * layout_w / printable_w));
      const int r2 = right_edge();
      if (r2 <= layout_w + 1) break;
      layout_w = r2;
    }
    scale = (double)printable_w / layout_w;
  }
  // A page holds printable_h of paper: in layout units, that divided by the scale.
  const int page_span = std::max(1, (int)((double)printable_h / scale));

  int num_pages = (doc.content_height + page_span - 1) / page_span;
  if (num_pages < 1) num_pages = 1;
  auto num = [](double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.2f", v);
    return std::string(b);
  };

  std::vector<std::string> page_streams;
  page_streams.reserve(num_pages);

  using Kind = userland::web::Fragment::Kind;

  for (int p = 0; p < num_pages; ++p) {
    int y_start = p * page_span;
    int y_end = (p + 1) * page_span;

    std::ostringstream s;
    // Base white canvas
    s << "q\n1 1 1 rg\n0 0 " << page_w << " " << page_h << " re\nf\n";

    // Running header
    if (!header_text.empty()) {
      int baseline = page_h - (margin_top * 2 / 3);
      s << "BT\n/F2 9 Tf\n0.2 0.2 0.2 rg\n1 0 0 1 " << margin_left << " " << baseline
        << " Tm\n(" << pdf_escape_text(header_text) << ") Tj\nET\n";
      int rule_y = page_h - margin_top + 4;
      s << "0.75 0.75 0.75 rg\n" << margin_left << " " << rule_y << " " << printable_w << " 0.5 re\nf\n";
    }

    // Fragments for page p
    for (const auto& f : doc.fragments) {
      if (f.y < y_start || f.y >= y_end) continue;
      const double local_y = margin_top + (f.y - y_start) * scale;
      const double local_x = margin_left + f.x * scale;

      if (f.kind == Kind::Text && !f.text.empty()) {
        int lpx = f.font.px > 0 ? (int)f.font.px : 10;
        const double px = lpx * scale;
        const double baseline = page_h - (local_y + ((lpx * 8 + 9) / 10) * scale);
        std::string font_name = "/F1";
        if (f.font.bold && f.font.italic) font_name = "/F4";
        else if (f.font.bold) font_name = "/F2";
        else if (f.font.italic) font_name = "/F3";

        uint32_t text_col = f.color;
        // On white paper, unstyled or default engine text (0xFFDDDDDD, white, transparent) must render dark.
        if (text_col == 0xFFDDDDDD || text_col == 0xFFFFFFFF || (text_col >> 24) == 0) {
          text_col = 0xFF111111;
        }

        s << "BT\n" << font_name << " " << num(px) << " Tf\n" << pdf_rgb(text_col) << " rg\n";
        s << "1 0 0 1 " << num(local_x) << " " << num(baseline) << " Tm\n(" << pdf_escape_text(f.text) << ") Tj\nET\n";
      } else if ((f.kind == Kind::Rect || f.kind == Kind::Rule) && (f.color >> 24) != 0 && f.w > 0 && f.h > 0) {
        const double h = f.h * scale;
        const double pdf_y = page_h - local_y - h;
        s << pdf_rgb(f.color) << " rg\n" << num(local_x) << " " << num(pdf_y) << " " << num(f.w * scale) << " " << num(h) << " re\nf\n";
      } else if (f.kind == Kind::Border && (f.color >> 24) != 0 && f.w > 0 && f.h > 0 && f.text.size() >= 4) {
        // the four widths ride in the text field (top, right, bottom, left), as the screen painter reads them
        const double bt = (unsigned char)f.text[0] * scale, br = (unsigned char)f.text[1] * scale;
        const double bb = (unsigned char)f.text[2] * scale, bl = (unsigned char)f.text[3] * scale;
        const double w = f.w * scale, h = f.h * scale, top = page_h - local_y;
        s << pdf_rgb(f.color) << " rg\n";
        auto band = [&](double bx, double by, double bw, double bh) {
          if (bw > 0 && bh > 0) s << num(bx) << " " << num(by) << " " << num(bw) << " " << num(bh) << " re\nf\n";
        };
        band(local_x, top - bt, w, bt);              // top
        band(local_x, top - h, w, bb);               // bottom
        band(local_x, top - h, bl, h);               // left
        band(local_x + w - br, top - h, br, h);      // right
      }
    }

    // Running footer with page replacement
    if (!footer_text.empty()) {
      std::string ft = footer_text;
      size_t pos = 0;
      while ((pos = ft.find("{PAGENO}", pos)) != std::string::npos) {
        std::string rep = std::to_string(p + 1);
        ft.replace(pos, 8, rep);
        pos += rep.size();
      }
      pos = 0;
      while ((pos = ft.find("{nbpg}", pos)) != std::string::npos) {
        std::string rep = std::to_string(num_pages);
        ft.replace(pos, 6, rep);
        pos += rep.size();
      }
      int baseline = margin_bottom / 2;
      s << "BT\n/F1 8 Tf\n0.35 0.35 0.35 rg\n1 0 0 1 " << margin_left << " " << baseline
        << " Tm\n(" << pdf_escape_text(ft) << ") Tj\nET\n";
      int rule_y = margin_bottom + 4;
      s << "0.75 0.75 0.75 rg\n" << margin_left << " " << rule_y << " " << printable_w << " 0.5 re\nf\n";
    }

    s << "Q\n";
    page_streams.push_back(s.str());
  }

  // Assemble PDF document objects
  // 1: Catalog
  // 2: Pages
  // 3..2+N: Page objects
  // 3+N..2+2N: Content stream objects
  // Resources object
  // Fonts: Helvetica, Helvetica-Bold, Helvetica-Oblique, Helvetica-BoldOblique
  int res_id = 3 + 2 * num_pages;
  int f1_id = res_id + 1;
  int f2_id = res_id + 2;
  int f3_id = res_id + 3;
  int f4_id = res_id + 4;
  int total_objs = f4_id;

  std::vector<std::string> objs(total_objs + 1);
  objs[1] = "<< /Type /Catalog /Pages 2 0 R >>";

  std::string kids = "[";
  for (int p = 0; p < num_pages; ++p) {
    if (p > 0) kids += " ";
    kids += std::to_string(3 + p) + " 0 R";
  }
  kids += "]";
  objs[2] = "<< /Type /Pages /Kids " + kids + " /Count " + std::to_string(num_pages) + " >>";

  for (int p = 0; p < num_pages; ++p) {
    int page_id = 3 + p;
    int stream_id = 3 + num_pages + p;
    objs[page_id] = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " + std::to_string(page_w) + " " +
                    std::to_string(page_h) + "] /Contents " + std::to_string(stream_id) +
                    " 0 R /Resources " + std::to_string(res_id) + " 0 R >>";
    const std::string& st_data = page_streams[p];
    objs[stream_id] = "<< /Length " + std::to_string(st_data.size()) + " >>\nstream\n" + st_data + "\nendstream";
  }

  objs[res_id] = "<< /Font << /F1 " + std::to_string(f1_id) + " 0 R /F2 " + std::to_string(f2_id) +
                 " 0 R /F3 " + std::to_string(f3_id) + " 0 R /F4 " + std::to_string(f4_id) + " 0 R >> >>";
  objs[f1_id] = "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>";
  objs[f2_id] = "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Encoding /WinAnsiEncoding >>";
  objs[f3_id] = "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Oblique /Encoding /WinAnsiEncoding >>";
  objs[f4_id] = "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-BoldOblique /Encoding /WinAnsiEncoding >>";

  std::string pstr = "%PDF-1.4\n";
  std::vector<size_t> offsets(total_objs + 1, 0);
  for (int k = 1; k <= total_objs; ++k) {
    offsets[k] = pstr.size();
    pstr += std::to_string(k) + " 0 obj\n" + objs[k] + "\nendobj\n";
  }

  size_t xref_at = pstr.size();
  pstr += "xref\n0 " + std::to_string(total_objs + 1) + "\n0000000000 65535 f \n";
  for (int k = 1; k <= total_objs; ++k) {
    char line[32];
    std::snprintf(line, sizeof line, "%010zu 00000 n \n", offsets[k]);
    pstr += line;
  }
  pstr += "trailer\n<< /Size " + std::to_string(total_objs + 1) + " /Root 1 0 R >>\nstartxref\n" +
          std::to_string(xref_at) + "\n%%EOF\n";

  std::vector<uint8_t> out_bytes(pstr.begin(), pstr.end());
  return Value::object(Type::Bytes, std::make_shared<BytesObj>(std::move(out_bytes)));
}

// ---- Native: zip_create(entries, [level]) -----------------------------------
Value bi_zip_create(Vm&, std::vector<Value>& args, std::string& error) {
  if (args.empty()) {
    error = "zip_create() requires at least 1 argument (entries)";
    return Value::nil();
  }
  int level = 6;
  if (args.size() > 1 && args[1].is_number()) {
    level = (int)args[1].as_int();
    if (level < 0) level = 0;
    if (level > 9) level = 9;
  }

  std::vector<ArchiveEntry> entries;
  if (args[0].type == Type::Map) {
    auto m = as_map(args[0]);
    for (const auto& [name, val] : m->entries) {
      ArchiveEntry e;
      e.path = name;
      if (val.type == Type::Bytes) {
        const auto& d = val.bytes_data();
        e.data.assign(d.begin(), d.end());
      } else {
        std::string s = val.str();
        e.data.assign(s.begin(), s.end());
      }
      entries.push_back(std::move(e));
    }
  } else if (args[0].type == Type::List) {
    const auto& list = args[0].list_items();
    for (const auto& item : list) {
      if (item.type != Type::Map) continue;
      auto m = as_map(item);
      Value* p = m->find("path");
      if (!p) p = m->find("name");
      if (!p) continue;
      ArchiveEntry e;
      e.path = p->to_text();
      Value* d = m->find("data");
      if (d) {
        if (d->type == Type::Bytes) {
          const auto& bd = d->bytes_data();
          e.data.assign(bd.begin(), bd.end());
        } else {
          std::string s = d->to_text();
          e.data.assign(s.begin(), s.end());
        }
      }
      Value* md = m->find("mode");
      if (md && md->is_number()) e.mode = (uint32_t)md->as_int();
      entries.push_back(std::move(e));
    }
  } else {
    error = "zip_create() expects a map of path->content or a list of entry maps";
    return Value::nil();
  }

  ByteBuffer zip = zip_create(entries, level);
  std::vector<uint8_t> out(zip.begin(), zip.end());
  return Value::object(Type::Bytes, std::make_shared<BytesObj>(std::move(out)));
}

// ---- Native: unzip(zip_data) ------------------------------------------------
Value bi_unzip(Vm&, std::vector<Value>& args, std::string& error) {
  if (args.empty()) {
    error = "unzip() requires 1 argument (zip_data)";
    return Value::nil();
  }
  ByteSpan span;
  if (args[0].type == Type::Bytes) {
    const auto& d = args[0].bytes_data();
    span = ByteSpan(d.data(), d.size());
  } else if (args[0].type == Type::Str) {
    const std::string& s = args[0].str();
    span = ByteSpan(reinterpret_cast<const uint8_t*>(s.data()), s.size());
  } else {
    error = "unzip() requires string or bytes";
    return Value::nil();
  }

  auto res = unzip(span);
  if (!res.ok()) {
    error = "unzip() failed: " + res.error().str();
    return Value::nil();
  }

  auto out_map = std::make_shared<MapObj>();
  for (const auto& e : *res) {
    std::vector<uint8_t> data(e.data.begin(), e.data.end());
    out_map->put(e.path, Value::object(Type::Bytes, std::make_shared<BytesObj>(std::move(data))));
  }
  return Value::object(Type::Map, out_map);
}

// ---- Native: doc.csv(data) / doc.csv(columns, rows) -------------------------
Value bi_doc_csv(Vm&, std::vector<Value>& args, std::string& error) {
  if (args.empty()) {
    error = "doc.csv() requires at least 1 argument";
    return Value::nil();
  }

  std::vector<std::string> cols;
  std::vector<std::vector<Value>> rows;

  auto format_cell = [](std::string& out, const Value& v) {
    if (v.type == Type::Nil) return;
    if (v.type == Type::Int) {
      out += std::to_string(v.as_int());
      return;
    }
    if (v.type == Type::Real) {
      char b[64];
      std::snprintf(b, sizeof b, "%.10g", v.as_double());
      out += b;
      return;
    }
    if (v.type == Type::Bool) {
      out += v.b ? "true" : "false";
      return;
    }
    std::string s = v.type == Type::Str ? v.str() : v.to_text();
    bool needs = false;
    for (char c : s) {
      if (c == ',' || c == '"' || c == '\n' || c == '\r') {
        needs = true;
        break;
      }
    }
    if (!needs) {
      out += s;
    } else {
      out += '"';
      for (char c : s) {
        if (c == '"') out += "\"\"";
        else out += c;
      }
      out += '"';
    }
  };

  if (args.size() >= 2) {
    if (!want_list(args[0], error, "doc.csv (columns)")) return Value::nil();
    if (!want_list(args[1], error, "doc.csv (rows)")) return Value::nil();
    for (const auto& c : args[0].list_items()) cols.push_back(c.to_text());
    for (const auto& r : args[1].list_items()) {
      if (r.type == Type::List) {
        rows.push_back(r.list_items());
      } else if (r.type == Type::Map) {
        auto rm = as_map(r);
        std::vector<Value> row_vals;
        for (const auto& cn : cols) {
          Value* f = rm->find(cn);
          row_vals.push_back(f ? *f : Value::nil());
        }
        rows.push_back(std::move(row_vals));
      }
    }
  } else if (args[0].type == Type::Map) {
    auto m = as_map(args[0]);
    if (Value* c = m->find("columns"); c && c->type == Type::List) {
      for (const auto& ci : c->list_items()) cols.push_back(ci.to_text());
    }
    if (Value* r = m->find("rows"); r && r->type == Type::List) {
      for (const auto& ri : r->list_items()) {
        if (ri.type == Type::List) rows.push_back(ri.list_items());
      }
    } else if (Value* recs = m->find("records"); recs && recs->type == Type::List) {
      for (const auto& ri : recs->list_items()) {
        if (ri.type != Type::Map) continue;
        auto rm = as_map(ri);
        if (cols.empty()) {
          for (const auto& kv : rm->entries) cols.push_back(kv.first);
        }
        std::vector<Value> row_vals;
        for (const auto& cn : cols) {
          Value* f = rm->find(cn);
          row_vals.push_back(f ? *f : Value::nil());
        }
        rows.push_back(std::move(row_vals));
      }
    }
  } else if (args[0].type == Type::List) {
    const auto& list = args[0].list_items();
    if (!list.empty()) {
      if (list[0].type == Type::Map) {
        auto first_m = as_map(list[0]);
        for (const auto& kv : first_m->entries) cols.push_back(kv.first);
        for (const auto& item : list) {
          if (item.type != Type::Map) continue;
          auto rm = as_map(item);
          std::vector<Value> row_vals;
          for (const auto& cn : cols) {
            Value* f = rm->find(cn);
            row_vals.push_back(f ? *f : Value::nil());
          }
          rows.push_back(std::move(row_vals));
        }
      } else if (list[0].type == Type::List) {
        for (size_t i = 0; i < list.size(); ++i) {
          if (list[i].type == Type::List) rows.push_back(list[i].list_items());
        }
      }
    }
  } else {
    error = "doc.csv() argument must be (columns, rows) or a dataset map/list";
    return Value::nil();
  }

  std::string csv;
  if (!cols.empty()) {
    for (size_t i = 0; i < cols.size(); ++i) {
      if (i > 0) csv += ",";
      format_cell(csv, Value::string(cols[i]));
    }
    csv += "\r\n";
  }

  for (const auto& row : rows) {
    for (size_t i = 0; i < row.size(); ++i) {
      if (i > 0) csv += ",";
      format_cell(csv, row[i]);
    }
    csv += "\r\n";
  }

  return Value::string(csv);
}

// ---- Native: doc.parse_csv(text, [options]) ---------------------------------
Value bi_doc_parse_csv(Vm&, std::vector<Value>& args, std::string& error) {
  if (args.empty()) {
    error = "doc.parse_csv() requires at least 1 argument (csv text)";
    return Value::nil();
  }
  std::string text;
  if (args[0].type == Type::Str) {
    text = args[0].str();
  } else if (args[0].type == Type::Bytes) {
    const auto& b = args[0].bytes_data();
    text.assign(reinterpret_cast<const char*>(b.data()), b.size());
  } else {
    error = "doc.parse_csv() requires string or bytes";
    return Value::nil();
  }

  bool header = true;
  bool types = true;
  bool as_table = false;
  if (args.size() > 1 && args[1].type == Type::Map) {
    auto m = as_map(args[1]);
    if (Value* h = m->find("header"); h && h->type == Type::Bool) header = h->b;
    if (Value* t = m->find("types"); t && t->type == Type::Bool) types = t->b;
    if (Value* tbl = m->find("table"); tbl && tbl->type == Type::Bool) as_table = tbl->b;
    if (Value* tbl2 = m->find("as_table"); tbl2 && tbl2->type == Type::Bool) as_table = tbl2->b;
  }

  std::vector<std::vector<std::string>> raw_rows;
  std::vector<std::string> cur_row;
  std::string cur_cell;
  bool in_quotes = false;
  size_t i = 0;
  const size_t n = text.size();

  while (i < n) {
    char c = text[i];
    if (in_quotes) {
      if (c == '"') {
        if (i + 1 < n && text[i + 1] == '"') {
          cur_cell += '"';
          i += 2;
          continue;
        } else {
          in_quotes = false;
          i++;
          continue;
        }
      } else {
        cur_cell += c;
        i++;
      }
    } else {
      if (c == '"') {
        in_quotes = true;
        i++;
      } else if (c == ',') {
        cur_row.push_back(cur_cell);
        cur_cell.clear();
        i++;
      } else if (c == '\r') {
        if (i + 1 < n && text[i + 1] == '\n') i++;
        cur_row.push_back(cur_cell);
        cur_cell.clear();
        raw_rows.push_back(std::move(cur_row));
        cur_row.clear();
        i++;
      } else if (c == '\n') {
        cur_row.push_back(cur_cell);
        cur_cell.clear();
        raw_rows.push_back(std::move(cur_row));
        cur_row.clear();
        i++;
      } else {
        cur_cell += c;
        i++;
      }
    }
  }
  if (!cur_cell.empty() || !cur_row.empty()) {
    cur_row.push_back(cur_cell);
    raw_rows.push_back(std::move(cur_row));
  }

  if (raw_rows.empty()) {
    if (as_table) {
      auto tm = std::make_shared<MapObj>();
      tm->put("columns", Value::list({}));
      tm->put("rows", Value::list({}));
      return Value::object(Type::Map, tm);
    }
    return Value::list({});
  }

  auto to_val = [&](const std::string& s) -> Value {
    if (!types) return Value::string(s);
    if (s.empty()) return Value::nil();
    if (s == "true") return Value::boolean(true);
    if (s == "false") return Value::boolean(false);
    if (s == "null" || s == "nil") return Value::nil();
    bool is_int = true;
    size_t start = 0;
    if (s[0] == '-' || s[0] == '+') start = 1;
    if (start >= s.size()) is_int = false;
    for (size_t k = start; k < s.size() && is_int; ++k) {
      if (s[k] < '0' || s[k] > '9') is_int = false;
    }
    if (is_int) {
      try {
        return Value::integer(std::stoll(s));
      } catch (...) {}
    }
    bool has_dot = false;
    bool is_real = true;
    for (size_t k = start; k < s.size() && is_real; ++k) {
      if (s[k] == '.') {
        if (has_dot) is_real = false;
        else has_dot = true;
      } else if (s[k] < '0' || s[k] > '9') {
        is_real = false;
      }
    }
    if (is_real && has_dot && s.size() > (start + 1)) {
      try {
        return Value::real(std::stod(s));
      } catch (...) {}
    }
    return Value::string(s);
  };

  if (as_table) {
    auto tm = std::make_shared<MapObj>();
    std::vector<Value> cols_val;
    size_t row_start = 0;
    if (header && !raw_rows.empty()) {
      for (const auto& c : raw_rows[0]) cols_val.push_back(Value::string(c));
      row_start = 1;
    }
    std::vector<Value> rows_val;
    for (size_t r = row_start; r < raw_rows.size(); ++r) {
      std::vector<Value> r_cells;
      for (const auto& cell_str : raw_rows[r]) {
        r_cells.push_back(to_val(cell_str));
      }
      rows_val.push_back(Value::list(std::move(r_cells)));
    }
    tm->put("columns", Value::list(std::move(cols_val)));
    tm->put("rows", Value::list(std::move(rows_val)));
    return Value::object(Type::Map, tm);
  }

  if (header) {
    const auto& col_names = raw_rows[0];
    std::vector<Value> records;
    for (size_t r = 1; r < raw_rows.size(); ++r) {
      auto m = std::make_shared<MapObj>();
      for (size_t c = 0; c < col_names.size(); ++c) {
        std::string val_str = c < raw_rows[r].size() ? raw_rows[r][c] : "";
        m->put(col_names[c], to_val(val_str));
      }
      records.push_back(Value::object(Type::Map, m));
    }
    return Value::list(std::move(records));
  } else {
    std::vector<Value> rows_val;
    for (size_t r = 0; r < raw_rows.size(); ++r) {
      std::vector<Value> r_cells;
      for (const auto& cell_str : raw_rows[r]) {
        r_cells.push_back(to_val(cell_str));
      }
      rows_val.push_back(Value::list(std::move(rows_val)));
    }
    return Value::list(std::move(rows_val));
  }
}

// ---- Native: xlsx_create(spec) ----------------------------------------------
Value bi_xlsx_create(Vm&, std::vector<Value>& args, std::string& error) {
  if (args.empty()) {
    error = "xlsx_create() requires 1 argument (spec map or rows list)";
    return Value::nil();
  }

  std::string sheet_name = "Sheet1";
  std::string freeze_pane = "";
  std::vector<std::string> merges;
  std::vector<double> col_widths;
  std::vector<Value> rows;
  std::vector<std::string> cols;

  if (args[0].type == Type::List) {
    const auto& list = args[0].list_items();
    if (!list.empty() && list[0].type == Type::Map) {
      auto first_m = as_map(list[0]);
      for (const auto& kv : first_m->entries) cols.push_back(kv.first);
      for (const auto& item : list) {
        if (item.type != Type::Map) continue;
        auto rm = as_map(item);
        std::vector<Value> row_cells;
        for (const auto& cn : cols) {
          Value* f = rm->find(cn);
          row_cells.push_back(f ? *f : Value::nil());
        }
        rows.push_back(Value::list(std::move(row_cells)));
      }
    } else {
      rows = list;
    }
  } else if (args[0].type == Type::Map) {
    auto m = as_map(args[0]);
    if (Value* s = m->find("sheet")) sheet_name = s->to_text();
    else if (Value* t = m->find("title")) sheet_name = t->to_text();
    else if (Value* n = m->find("name")) sheet_name = n->to_text();
    if (Value* f = m->find("freeze_pane")) freeze_pane = f->to_text();
    if (Value* mg = m->find("merges")) {
      if (mg->type == Type::List) {
        for (const auto& it : mg->list_items()) merges.push_back(it.to_text());
      }
    }
    if (Value* cw = m->find("col_widths")) {
      if (cw->type == Type::List) {
        for (const auto& it : cw->list_items()) {
          col_widths.push_back(it.is_number() ? it.as_double() : 11.0);
        }
      }
    }
    if (Value* c = m->find("columns"); c && c->type == Type::List) {
      for (const auto& ci : c->list_items()) cols.push_back(ci.to_text());
    }
    if (Value* rw = m->find("rows"); rw && rw->type == Type::List) {
      rows = rw->list_items();
    } else if (Value* recs = m->find("records"); recs && recs->type == Type::List) {
      for (const auto& it : recs->list_items()) {
        if (it.type != Type::Map) continue;
        auto rm = as_map(it);
        if (cols.empty()) {
          for (const auto& kv : rm->entries) cols.push_back(kv.first);
        }
        std::vector<Value> row_cells;
        for (const auto& cn : cols) {
          Value* f = rm->find(cn);
          row_cells.push_back(f ? *f : Value::nil());
        }
        rows.push_back(Value::list(std::move(row_cells)));
      }
    }
  } else {
    error = "xlsx_create() argument must be a map or a list of rows";
    return Value::nil();
  }

  // Prepend styled header row if columns specified
  if (!cols.empty()) {
    std::vector<Value> header_cells;
    for (const auto& col_name : cols) {
      auto cm = std::make_shared<MapObj>();
      cm->put("v", Value::string(col_name));
      cm->put("s", Value::string("header"));
      header_cells.push_back(Value::object(Type::Map, cm));
    }
    rows.insert(rows.begin(), Value::list(std::move(header_cells)));
  }

  // Build sheetData XML
  std::string sheet_data;
  int ri = 1;
  int max_col = 1;
  std::vector<int> col_max_chars;

  for (const auto& row_val : rows) {
    if (row_val.type != Type::List) { ++ri; continue; }
    const auto& cells = row_val.list_items();
    if (cells.empty()) { ++ri; continue; }

    std::string row_xml = "<row r=\"" + std::to_string(ri) + "\">";
    int ci = 0;
    for (const auto& cell_val : cells) {
      std::string addr = excel_col_name(ci) + std::to_string(ri);
      int style_id = 0;
      Value v = cell_val;

      // Cell can be a map with {"v": ..., "s": ...}
      if (cell_val.type == Type::Map) {
        auto cm = as_map(cell_val);
        if (Value* s_val = cm->find("s")) style_id = resolve_style_id(*s_val);
        else if (Value* st_val = cm->find("style")) style_id = resolve_style_id(*st_val);
        if (Value* val_ptr = cm->find("v")) v = *val_ptr;
        else if (Value* val_ptr2 = cm->find("value")) v = *val_ptr2;
      }

      int char_len = 0;
      if (v.type == Type::Int || v.type == Type::Real) {
        char num_buf[64];
        if (v.type == Type::Int) {
          std::snprintf(num_buf, sizeof num_buf, "%lld", (long long)v.as_int());
        } else {
          std::snprintf(num_buf, sizeof num_buf, "%.4g", v.as_double());
        }
        char_len = (int)std::strlen(num_buf);
        row_xml += "<c r=\"" + addr + "\" s=\"" + std::to_string(style_id) + "\"><v>" + num_buf + "</v></c>";
      } else if (v.type == Type::Bool) {
        char_len = 5;
        row_xml += "<c r=\"" + addr + "\" t=\"b\" s=\"" + std::to_string(style_id) + "\"><v>" + (v.b ? "1" : "0") + "</v></c>";
      } else if (v.type == Type::Nil) {
        if (style_id > 0) {
          row_xml += "<c r=\"" + addr + "\" s=\"" + std::to_string(style_id) + "\"/>";
        }
      } else {
        std::string text = v.to_text();
        char_len = (int)text.size();
        row_xml += "<c r=\"" + addr + "\" t=\"inlineStr\" s=\"" + std::to_string(style_id) +
                   "\"><is><t xml:space=\"preserve\">" + xml_escape(text) + "</t></is></c>";
      }
      if (ci >= (int)col_max_chars.size()) col_max_chars.resize(ci + 1, 0);
      if (char_len > col_max_chars[ci]) col_max_chars[ci] = char_len;
      ci++;
      if (ci > max_col) max_col = ci;
    }
    row_xml += "</row>";
    sheet_data += row_xml;
    ri++;
  }

  // Cols XML
  std::string cols_xml;
  if (!col_widths.empty()) {
    cols_xml += "<cols>";
    for (size_t i = 0; i < col_widths.size(); ++i) {
      char b[128];
      std::snprintf(b, sizeof b, "<col min=\"%zu\" max=\"%zu\" width=\"%.1f\" customWidth=\"1\"/>", i + 1, i + 1, col_widths[i]);
      cols_xml += b;
    }
    cols_xml += "</cols>";
  } else if (max_col > 0) {
    cols_xml += "<cols>";
    for (int c = 0; c < max_col; ++c) {
      int ml = (c < (int)col_max_chars.size()) ? col_max_chars[c] : 8;
      double w = std::max(10.0, std::min(50.0, (double)ml + 3.0));
      char b[128];
      std::snprintf(b, sizeof b, "<col min=\"%d\" max=\"%d\" width=\"%.1f\" customWidth=\"1\"/>", c + 1, c + 1, w);
      cols_xml += b;
    }
    cols_xml += "</cols>";
  }

  // Views / Frozen Panes
  std::string views_xml;
  if (!freeze_pane.empty()) {
    // e.g. "B5" -> xSplit="1" ySplit="4"
    int x_split = 1;
    int y_split = 4;
    if (freeze_pane.size() >= 2) {
      char col_char = (char)std::toupper(freeze_pane[0]);
      if (col_char >= 'A' && col_char <= 'Z') x_split = col_char - 'A';
      y_split = std::atoi(freeze_pane.c_str() + 1) - 1;
      if (y_split < 0) y_split = 0;
    }
    views_xml = "<sheetViews><sheetView workbookViewId=\"0\"><pane xSplit=\"" + std::to_string(x_split) +
                "\" ySplit=\"" + std::to_string(y_split) + "\" topLeftCell=\"" + freeze_pane +
                "\" activePane=\"bottomRight\" state=\"frozen\"/></sheetView></sheetViews>";
  }

  // Merges
  std::string merges_xml;
  if (!merges.empty()) {
    merges_xml = "<mergeCells count=\"" + std::to_string(merges.size()) + "\">";
    for (const auto& m_ref : merges) {
      merges_xml += "<mergeCell ref=\"" + m_ref + "\"/>";
    }
    merges_xml += "</mergeCells>";
  }

  std::string sheet1_xml =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
      "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
      "<sheetPr><pageSetUpPr fitToPage=\"1\"/></sheetPr>" +
      views_xml + cols_xml + "<sheetData>" + sheet_data + "</sheetData>" + merges_xml +
      "<pageSetup orientation=\"landscape\" fitToWidth=\"1\" fitToHeight=\"0\"/></worksheet>";

  // OpenXML Stylesheet
  const char* kStylesXml =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
      "<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">\n"
      "  <numFmts count=\"1\"><numFmt numFmtId=\"164\" formatCode=\"#,##0.0\"/></numFmts>\n"
      "  <fonts count=\"4\">\n"
      "    <font><sz val=\"10\"/><name val=\"Arial\"/></font>\n"
      "    <font><b/><sz val=\"10\"/><name val=\"Arial\"/></font>\n"
      "    <font><b/><sz val=\"10\"/><color rgb=\"FFFFFFFF\"/><name val=\"Arial\"/></font>\n"
      "    <font><b/><sz val=\"13\"/><name val=\"Arial\"/></font>\n"
      "  </fonts>\n"
      "  <fills count=\"7\">\n"
      "    <fill><patternFill patternType=\"none\"/></fill>\n"
      "    <fill><patternFill patternType=\"gray125\"/></fill>\n"
      "    <fill><patternFill patternType=\"solid\"><fgColor rgb=\"FF1E3A5F\"/><bgColor indexed=\"64\"/></patternFill></fill>\n"
      "    <fill><patternFill patternType=\"solid\"><fgColor rgb=\"FFFFD966\"/><bgColor indexed=\"64\"/></patternFill></fill>\n"
      "    <fill><patternFill patternType=\"solid\"><fgColor rgb=\"FFFFF2CC\"/><bgColor indexed=\"64\"/></patternFill></fill>\n"
      "    <fill><patternFill patternType=\"solid\"><fgColor rgb=\"FFEEEEEE\"/><bgColor indexed=\"64\"/></patternFill></fill>\n"
      "    <fill><patternFill patternType=\"solid\"><fgColor rgb=\"FFFFE699\"/><bgColor indexed=\"64\"/></patternFill></fill>\n"
      "  </fills>\n"
      "  <borders count=\"2\">\n"
      "    <border><left/><right/><top/><bottom/><diagonal/></border>\n"
      "    <border><left/><right/><top style=\"thin\"><color rgb=\"FF888888\"/></top><bottom/><diagonal/></border>\n"
      "  </borders>\n"
      "  <cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>\n"
      "  <cellXfs count=\"14\">\n"
      "    <xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/>\n"                                                // 0 plain
      "    <xf numFmtId=\"0\" fontId=\"2\" fillId=\"2\" borderId=\"0\" applyFont=\"1\" applyFill=\"1\" applyAlignment=\"1\"><alignment horizontal=\"center\" vertical=\"center\"/></xf>\n" // 1 section
      "    <xf numFmtId=\"0\" fontId=\"1\" fillId=\"5\" borderId=\"0\" applyFont=\"1\" applyFill=\"1\" applyAlignment=\"1\"><alignment horizontal=\"center\"/></xf>\n"                   // 2 header
      "    <xf numFmtId=\"0\" fontId=\"1\" fillId=\"3\" borderId=\"0\" applyFont=\"1\" applyFill=\"1\" applyAlignment=\"1\"><alignment horizontal=\"center\"/></xf>\n"                   // 3 contrib_header
      "    <xf numFmtId=\"3\" fontId=\"0\" fillId=\"0\" borderId=\"0\" applyNumberFormat=\"1\"/>\n"                          // 4 int
      "    <xf numFmtId=\"164\" fontId=\"0\" fillId=\"0\" borderId=\"0\" applyNumberFormat=\"1\"/>\n"                        // 5 dec
      "    <xf numFmtId=\"3\" fontId=\"0\" fillId=\"4\" borderId=\"0\" applyNumberFormat=\"1\" applyFill=\"1\"/>\n"          // 6 contrib_int
      "    <xf numFmtId=\"164\" fontId=\"0\" fillId=\"4\" borderId=\"0\" applyNumberFormat=\"1\" applyFill=\"1\"/>\n"        // 7 contrib_dec
      "    <xf numFmtId=\"0\" fontId=\"1\" fillId=\"5\" borderId=\"1\" applyFont=\"1\" applyFill=\"1\" applyBorder=\"1\"/>\n" // 8 total text
      "    <xf numFmtId=\"3\" fontId=\"1\" fillId=\"5\" borderId=\"1\" applyNumberFormat=\"1\" applyFont=\"1\" applyFill=\"1\" applyBorder=\"1\"/>\n" // 9 total int
      "    <xf numFmtId=\"164\" fontId=\"1\" fillId=\"5\" borderId=\"1\" applyNumberFormat=\"1\" applyFont=\"1\" applyFill=\"1\" applyBorder=\"1\"/>\n" // 10 total dec
      "    <xf numFmtId=\"3\" fontId=\"1\" fillId=\"6\" borderId=\"1\" applyNumberFormat=\"1\" applyFont=\"1\" applyFill=\"1\" applyBorder=\"1\"/>\n" // 11 total contrib int
      "    <xf numFmtId=\"164\" fontId=\"1\" fillId=\"6\" borderId=\"1\" applyNumberFormat=\"1\" applyFont=\"1\" applyFill=\"1\" applyBorder=\"1\"/>\n" // 12 total contrib dec
      "    <xf numFmtId=\"0\" fontId=\"3\" fillId=\"0\" borderId=\"0\" applyFont=\"1\"/>\n"                                // 13 title
      "  </cellXfs>\n"
      "  <cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/></cellStyles>\n"
      "</styleSheet>";

  const char* kContentTypes =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
      "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">\n"
      "  <Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>\n"
      "  <Default Extension=\"xml\" ContentType=\"application/xml\"/>\n"
      "  <Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>\n"
      "  <Override PartName=\"/xl/worksheets/sheet1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>\n"
      "  <Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>\n"
      "</Types>";

  const char* kRels =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
      "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n"
      "  <Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/>\n"
      "</Relationships>";

  std::string workbook_xml =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
      "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">\n"
      "  <sheets><sheet name=\"" + xml_escape(sheet_name) + "\" sheetId=\"1\" r:id=\"rId1\"/></sheets>\n"
      "</workbook>";

  const char* kWorkbookRels =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
      "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n"
      "  <Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet1.xml\"/>\n"
      "  <Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>\n"
      "</Relationships>";

  auto str_to_buf = [](const std::string& str) {
    return ByteBuffer(str.begin(), str.end());
  };

  std::vector<ArchiveEntry> entries = {
      {"[Content_Types].xml", str_to_buf(kContentTypes), 0644, false},
      {"_rels/.rels", str_to_buf(kRels), 0644, false},
      {"xl/workbook.xml", str_to_buf(workbook_xml), 0644, false},
      {"xl/_rels/workbook.xml.rels", str_to_buf(kWorkbookRels), 0644, false},
      {"xl/worksheets/sheet1.xml", str_to_buf(sheet1_xml), 0644, false},
      {"xl/styles.xml", str_to_buf(kStylesXml), 0644, false},
  };

  ByteBuffer zip = zip_create(entries, 6);
  std::vector<uint8_t> out(zip.begin(), zip.end());
  return Value::object(Type::Bytes, std::make_shared<BytesObj>(std::move(out)));
}

}  // namespace

void install_doc_builtins(Vm& vm) {
  // Top-level builtins
  vm.define_native("pdf_render", 1, 3, bi_pdf_render);
  vm.define_native("zip_create", 1, 2, bi_zip_create);
  vm.define_native("unzip", 1, 1, bi_unzip);
  vm.define_native("xlsx_create", 1, 1, bi_xlsx_create);
  vm.define_native("csv_format", 1, 2, bi_doc_csv);
  vm.define_native("csv_parse", 1, 2, bi_doc_parse_csv);

  // doc module namespace
  auto doc = std::make_shared<MapObj>();
  auto add = [&](const char* name, int min_args, int max_args, NativeFn fn) {
    doc->put(name, Value::object(Type::Native, std::make_shared<NativeObj>(std::string("doc.") + name, min_args, max_args, fn)));
  };
  add("pdf", 1, 3, bi_pdf_render);
  add("pdf_render", 1, 3, bi_pdf_render);
  add("xlsx", 1, 1, bi_xlsx_create);
  add("xlsx_create", 1, 1, bi_xlsx_create);
  add("csv", 1, 2, bi_doc_csv);
  add("parse_csv", 1, 2, bi_doc_parse_csv);
  add("zip", 1, 2, bi_zip_create);
  add("unzip", 1, 1, bi_unzip);

  vm.define_global("doc", Value::object(Type::Map, doc));
}

}  // namespace aios::userland::grain
