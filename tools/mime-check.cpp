// Prints, for each .eml named, the record the old Python IMAP worker built from it (subject, sender,
// recipients, date, body, html, attachments) as one JSON object per line, so scripts/mime-diff.py can
// hold this tree's MIME reader against Python's `email` package on the same bytes.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "aios/net/mime.hpp"

namespace {

std::string jstr(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += static_cast<char>(c);
    }
  }
  return o + "\"";
}

std::string collapse(const std::string& s) {
  std::string o;
  bool ws = false;
  for (char c : s) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { ws = true; continue; }
    if (ws && !o.empty()) o += ' ';
    ws = false;
    o += c;
  }
  return o;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace aios::net::mime;
  if (argc >= 3 && std::string(argv[1]) == "--date") {   // --date EPOCH...: the Date: header for each
    for (int i = 2; i < argc; ++i) std::printf("%s\n", format_rfc5322_date(std::atoll(argv[i])).c_str());
    return 0;
  }
  for (int i = 1; i < argc; ++i) {
    std::ifstream f(argv[i], std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string raw = ss.str();
    Parsed p = parse(raw);
    Address from = parse_address(p.decoded_header("From"));
    std::string subject = p.decoded_header("Subject");
    if (p.header("Subject").empty()) subject = "(No Subject)";
    int64_t ts = 0;
    int off = 0;
    bool have_date = parse_date(p.header("Date"), ts, off);
    std::string date_str = have_date ? format_date(ts, off) : "Unknown";
    std::string snippet = collapse(p.text).substr(0, 120);
    std::string atts = "[";
    for (size_t k = 0; k < p.attachments.size(); ++k) {
      if (k) atts += ",";
      atts += "{\"filename\":" + jstr(p.attachments[k].filename) + ",\"size_bytes\":" +
              std::to_string(p.attachments[k].size_bytes) + ",\"mime_type\":" + jstr(p.attachments[k].mime_type) +
              ",\"content_id\":" + jstr(p.attachments[k].content_id) + "}";
    }
    atts += "]";
    std::printf("{\"file\":%s,\"message_id\":%s,\"from_name\":%s,\"from_addr\":%s,\"to\":%s,\"cc\":%s,"
                "\"subject\":%s,\"timestamp\":%lld,\"date_str\":%s,\"snippet\":%s,\"body\":%s,\"html\":%s,"
                "\"attachments\":%s}\n",
                jstr(argv[i]).c_str(), jstr(collapse(p.header("Message-ID"))).c_str(),
                jstr(from.name.empty() ? from.addr : from.name).c_str(), jstr(from.addr).c_str(),
                jstr(p.decoded_header("To")).c_str(), jstr(p.decoded_header("Cc")).c_str(), jstr(subject).c_str(),
                static_cast<long long>(have_date ? ts : 0), jstr(date_str).c_str(), jstr(snippet).c_str(),
                jstr(p.text).c_str(), jstr(p.html).c_str(), atts.c_str());
  }
  return 0;
}
