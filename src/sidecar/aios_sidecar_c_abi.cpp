#include "aios/hal/host.hpp"
#include "aios/sidecar/aios_sidecar_c_abi.h"
#include "aios/core/strbuf.hpp"
#include "aios/sidecar/unified_sidecar.hpp"
#include "aios/sidecar/capability_guard.hpp"
#include "aios/sidecar/binary_telemetry.hpp"
#include "aios/sidecar/git_telemetry_events.hpp"

// Crypto
#include "aios/crypto/secp256k1.hpp"
#include "aios/crypto/curve25519.hpp"
#include "aios/crypto/sha256.hpp"
#include "aios/crypto/keccak256.hpp"
#include "aios/crypto/ripemd160.hpp"

// Blockchain & Contracts
#include "aios/userland/chain/btc_tx.hpp"
#include "aios/userland/chain/eth_tx.hpp"
#include "aios/userland/chain/rlp.hpp"

// Communications & Feeds
#include "aios/userland/mail_client.hpp"
#include "aios/userland/calendar_client.hpp"
#include "aios/userland/contacts_client.hpp"
#include "aios/userland/feed_parser.hpp"

// Capability & Security
#include "aios/capability/authority.hpp"
#include "aios/capability/capability.hpp"

// Web, SVG, PDF
#include "aios/userland/web.hpp"

// Compute, TSP & Routing
#include "aios/compute/erp_kernels.hpp"

// Grain Bytecode VM
#include "aios/userland/grain.hpp"

// Patch Transactions
#include "aios/userland/patch_transaction.hpp"

#include <cstring>
#include <string>
#include <vector>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <algorithm>

namespace aios::sidecar {

// Helper: JSON Escaper
static std::string json_escape(const std::string& s) {
  aios::StrBuf o;
  for (char c : s) {
    if (c == '"') o << "\\\"";
    else if (c == '\\') o << "\\\\";
    else if (c == '\b') o << "\\b";
    else if (c == '\f') o << "\\f";
    else if (c == '\n') o << "\\n";
    else if (c == '\r') o << "\\r";
    else if (c == '\t') o << "\\t";
    else if ('\x00' <= c && c <= '\x1f') {
      char u[8]; std::snprintf(u, sizeof u, "\\u%04x", (unsigned)(unsigned char)c); o << u;
    } else {
      o << c;
    }
  }
  return o.str();
}

// The value of a top-level header in a raw RFC 5322 message (case-insensitive
// name, folded continuation lines joined), or "" when absent.
static std::string raw_header(const std::string& raw, const std::string& name) {
  std::string want = name;
  for (auto& ch : want) ch = (char)std::tolower((unsigned char)ch);
  size_t pos = 0;
  std::string found;
  bool in_it = false;
  while (pos < raw.size()) {
    size_t eol = raw.find('\n', pos);
    std::string line = raw.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
    pos = eol == std::string::npos ? raw.size() : eol + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) break;   // end of the header block
    if (line[0] == ' ' || line[0] == '\t') { if (in_it) found += " " + line.substr(line.find_first_not_of(" \t")); continue; }
    if (in_it) break;
    size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string n = line.substr(0, colon);
    for (auto& ch : n) ch = (char)std::tolower((unsigned char)ch);
    if (n == want) { in_it = true; found = line.substr(colon + 1); }
  }
  size_t a = found.find_first_not_of(" \t");
  return a == std::string::npos ? "" : found.substr(a);
}

Result<EmlMetadata> parse_eml_string(const std::string& eml_raw) {
#if defined(__wasi__)
  // The mail store (sockets, files) is not built into WebAssembly.
  (void)eml_raw;
  return Result<EmlMetadata>(Status::Unsupported, "mail is not built into WebAssembly");
#else
  userland::MailMessage msg;
  if (!userland::MailStore::parse_rfc5322(eml_raw, msg)) {
    return Result<EmlMetadata>(Status::Invalid, "failed to parse RFC 5322 EML");
  }
  EmlMetadata meta;
  meta.from = msg.from_addr.empty() ? msg.from_name : msg.from_addr;
  if (!msg.to.empty()) meta.to = msg.to[0];
  meta.subject = msg.subject;
  // The mail store's own messages carry X-AetherMail-DateStr; a message from
  // anywhere else carries only the standard Date: header, which the store's
  // parser does not keep.
  meta.date = !msg.date_str.empty() ? msg.date_str : raw_header(eml_raw, "Date");
  meta.body = msg.body;
  meta.has_html = msg.has_html;
  meta.attachment_count = msg.attachments.size();
  return meta;
#endif
}

Result<ByteBuffer> render_svg_to_png(const std::string& svg_xml, const SvgRenderOptions& opts) {
  auto dom = userland::web::parse_html(ByteSpan(reinterpret_cast<const uint8_t*>(svg_xml.data()), svg_xml.size()));
  if (!dom) return Result<ByteBuffer>(Status::Invalid, "empty or malformed SVG document");
  
  userland::web::Node* svg_node = nullptr;
  if (dom->tag == "svg") {
    svg_node = dom.get();
  } else {
    svg_node = dom->first("svg");
  }
  if (!svg_node) return Result<ByteBuffer>(Status::NotFound, "no <svg> element found in document");

  auto bm_res = userland::web::rasterise_svg(*svg_node, opts.width, opts.height, opts.current_color);
  if (!bm_res.ok()) return bm_res.error();

  auto png = userland::web::encode_png(bm_res->pixels.data(), bm_res->w, bm_res->h);
  return png;
}

// A PDF string literal: escape the delimiters, and keep to the bytes the
// standard Helvetica (WinAnsi) can show — anything else becomes '?'.
static std::string pdf_text(const std::string& t) {
  std::string o;
  for (size_t i = 0; i < t.size(); ++i) {
    unsigned char c = (unsigned char)t[i];
    if (c == '(' || c == ')' || c == '\\') { o += '\\'; o += (char)c; }
    else if (c >= 0x20 && c < 0x7F) o += (char)c;
    else if (c >= 0x80) {
      if ((c & 0xC0) == 0x80) continue;   // continuation bytes of the '?' already written
      o += '?';
    }
  }
  return o;
}

Result<ByteBuffer> render_html_to_pdf(const std::string& html, const std::string& css, int viewport_w) {
  auto dom = userland::web::parse_html(ByteSpan(reinterpret_cast<const uint8_t*>(html.data()), html.size()));
  if (!dom) return Result<ByteBuffer>(Status::Invalid, "failed to parse HTML");

  // The page's own <style> blocks, then the caller's css on top.
  userland::web::Stylesheet sheet = userland::web::collect_author_css(*dom, "", {});
  if (!css.empty()) sheet.append(userland::web::parse_css(css));
  userland::web::Document doc;
  doc.dom = std::move(dom);
  // Layout needs text metrics; without them it called an empty std::function
  // (std::bad_function_call). These are Helvetica's: average advance ~0.52em,
  // line 1.2em, ascent 0.8em — the font the page is written in below.
  userland::web::Metrics m;
  m.measure = [](const std::string& t, const userland::web::FontKey& f) {
    int px = f.px > 0 ? (int)f.px : 12, n = 0;
    for (unsigned char c : t) if ((c & 0xC0) != 0x80) ++n;
    return (n * px * 52 + 99) / 100;
  };
  m.line_height = [](const userland::web::FontKey& f) { int px = f.px > 0 ? (int)f.px : 12; return (px * 12 + 9) / 10; };
  m.ascent = [](const userland::web::FontKey& f) { int px = f.px > 0 ? (int)f.px : 12; return (px * 8 + 9) / 10; };
  const int min_h = 1100;
  userland::web::layout(doc, sheet, m, viewport_w, min_h);

  int page_w = viewport_w;
  int page_h = std::max(min_h, doc.content_height);   // one page, as tall as the content
  aios::StrBuf stream;
  auto rgb = [](uint32_t c) {
    char b[48];
    std::snprintf(b, sizeof b, "%.3f %.3f %.3f", ((c >> 16) & 0xFF) / 255.0, ((c >> 8) & 0xFF) / 255.0, (c & 0xFF) / 255.0);
    return std::string(b);
  };
  stream << "q\n1 1 1 rg\n0 0 " << page_w << " " << page_h << " re\nf\n";
  using Kind = userland::web::Fragment::Kind;
  for (const auto& f : doc.fragments) {
    if (f.kind == Kind::Text && !f.text.empty()) {
      int px = f.font.px > 0 ? (int)f.font.px : 12;
      int baseline = page_h - (f.y + (px * 8 + 9) / 10);
      stream << "BT\n/F1 " << px << " Tf\n" << rgb(f.color) << " rg\n";
      stream << "1 0 0 1 " << f.x << " " << baseline << " Tm\n(" << pdf_text(f.text) << ") Tj\nET\n";
    } else if ((f.kind == Kind::Rect || f.kind == Kind::Rule) && (f.color >> 24) != 0 && f.w > 0 && f.h > 0) {
      int pdf_y = page_h - f.y - f.h;
      stream << rgb(f.color) << " rg\n" << f.x << " " << pdf_y << " " << f.w << " " << f.h << " re\nf\n";
    }
  }
  stream << "Q\n";
  std::string s_data = stream.str();

  // Objects with a real cross-reference table: every offset is where the object starts.
  std::vector<std::string> objs = {
    "<< /Type /Catalog /Pages 2 0 R >>",
    "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " + std::to_string(page_w) + " " + std::to_string(page_h) + "] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>",
    "<< /Length " + std::to_string(s_data.size()) + " >>\nstream\n" + s_data + "\nendstream",
    "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>",
  };
  std::string pstr = "%PDF-1.4\n";
  std::vector<size_t> offsets;
  for (size_t k = 0; k < objs.size(); ++k) {
    offsets.push_back(pstr.size());
    pstr += std::to_string(k + 1) + " 0 obj\n" + objs[k] + "\nendobj\n";
  }
  size_t xref_at = pstr.size();
  pstr += "xref\n0 " + std::to_string(objs.size() + 1) + "\n0000000000 65535 f \n";
  for (size_t off : offsets) {
    char line[24];
    std::snprintf(line, sizeof line, "%010zu 00000 n \n", off);
    pstr += line;
  }
  pstr += "trailer\n<< /Size " + std::to_string(objs.size() + 1) + " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref_at) + "\n%%EOF\n";

  ByteBuffer out(pstr.begin(), pstr.end());
  return out;
}

Result<CheckpointMetadata> inspect_checkpoint(ByteSpan bytes) {
  if (bytes.size() < 24) return Result<CheckpointMetadata>(Status::Invalid, "checkpoint buffer too small");
  Reader r(bytes);
  uint32_t magic = r.u32();
  uint32_t version = r.u32();
  uint64_t body_len = r.u64();
  uint64_t expected_sum = r.u64();
  if (!r.ok()) {
    return Result<CheckpointMetadata>(Status::Invalid, "corrupt checkpoint header");
  }
  if (magic != 0x434B4F41u) {
    return Result<CheckpointMetadata>(Status::Invalid, "invalid checkpoint magic");
  }
  if (bytes.size() < 24 + body_len) {
    return Result<CheckpointMetadata>(Status::Invalid, "truncated checkpoint body");
  }
  ByteSpan body = bytes.subspan(24, body_len);
  uint64_t actual_sum = fnv1a(body);
  CheckpointMetadata meta;
  meta.magic = magic;
  meta.version = version;
  meta.body_size = body_len;
  meta.checksum = expected_sum;
  meta.valid = (actual_sum == expected_sum);
  return meta;
}

}  // namespace aios::sidecar

/* -------------------------------------------------------------------------
 * C-ABI Implementation
 * ------------------------------------------------------------------------- */
extern "C" {

using namespace aios;
using namespace aios::userland;
using namespace aios::userland::chain;
using namespace aios::sidecar;

int aios_chain_eval_script(const uint8_t* script_sig, size_t sig_len,
                           const uint8_t* script_pubkey, size_t pk_len,
                           int* out_is_valid) {
  if (!script_sig || !script_pubkey || !out_is_valid) return AIOS_ERR_INVALID;
  ScriptContext ctx;
  auto res = evaluate_script(ByteSpan(script_sig, sig_len), ByteSpan(script_pubkey, pk_len), ctx);
  if (!res.ok()) {
    *out_is_valid = 0;
    return AIOS_ERR_EXECUTION;
  }
  *out_is_valid = *res ? 1 : 0;
  return AIOS_OK;
}

int aios_chain_btc_p2wpkh_address(const uint8_t pubkey[33], int testnet,
                                  char* out_addr, size_t max_len) {
  if (!pubkey || !out_addr || max_len == 0) return AIOS_ERR_INVALID;
  Network net = testnet ? Network::Testnet : Network::Mainnet;
  auto res = address_p2wpkh(ByteSpan(pubkey, 33), net);
  if (!res.ok()) return AIOS_ERR_EXECUTION;
  if (res->size() + 1 > max_len) return AIOS_ERR_BUFFER_TOO_SMALL;
  std::memcpy(out_addr, res->c_str(), res->size() + 1);
  return AIOS_OK;
}

int aios_chain_btc_create_p2wpkh_tx(const uint8_t prev_txid[32], uint32_t prev_vout,
                                    uint64_t amount_sats, const char* dest_addr,
                                    uint64_t fee_rate_sat_per_vb, const uint8_t privkey[32],
                                    int testnet, uint8_t* out_tx, size_t* out_len) {
  if (!prev_txid || !dest_addr || !privkey || !out_tx || !out_len) return AIOS_ERR_INVALID;
  Network net = testnet ? Network::Testnet : Network::Mainnet;
  auto pub = crypto::ec_pubkey_create(privkey, true);
  if (!pub.ok()) return AIOS_ERR_INVALID;
  auto h160 = crypto::hash160(ByteSpan(pub->data(), pub->size()));
  auto script_pub = script_p2wpkh(h160.data());

  SpendableOutput utxo;
  std::memcpy(utxo.prevout.txid.data(), prev_txid, 32);
  utxo.prevout.index = prev_vout;
  utxo.amount = amount_sats;
  utxo.script_pubkey = script_pub;
  utxo.witness = true;

  auto change_addr_res = address_p2wpkh(ByteSpan(pub->data(), pub->size()), net);
  if (!change_addr_res.ok()) return AIOS_ERR_EXECUTION;

  uint64_t fee_rate = (fee_rate_sat_per_vb > 0) ? fee_rate_sat_per_vb : 2;
  uint64_t send_amount = (amount_sats > 500) ? (amount_sats / 2) : (amount_sats - 200);
  if (send_amount == 0) return AIOS_ERR_INVALID;

  auto plan_res = build_payment({utxo}, dest_addr, send_amount, *change_addr_res, fee_rate, net);
  if (!plan_res.ok()) return AIOS_ERR_EXECUTION;

  auto sign_res = sign_payment(*plan_res, privkey);
  if (!sign_res.ok()) return AIOS_ERR_EXECUTION;

  auto raw = serialize_tx(plan_res->tx, true);
  if (raw.size() > *out_len) {
    *out_len = raw.size();
    return AIOS_ERR_BUFFER_TOO_SMALL;
  }
  std::memcpy(out_tx, raw.data(), raw.size());
  *out_len = raw.size();
  return AIOS_OK;
}

int aios_chain_eth_sign_tx(uint64_t nonce, uint64_t gas_limit, const uint8_t to[20],
                           const uint8_t* value_be, size_t val_len,
                           const uint8_t* data, size_t data_len, uint64_t chain_id,
                           const uint8_t privkey[32], uint8_t* out_raw_tx, size_t* out_len,
                           char* out_txhash_hex) {
  if (!privkey || !out_raw_tx || !out_len) return AIOS_ERR_INVALID;
  EthTransaction tx;
  tx.chain_id = chain_id;
  tx.nonce = nonce;
  tx.gas_limit = gas_limit;
  if (to) {
    std::memcpy(tx.to.data(), to, 20);
    tx.creates_contract = false;
  } else {
    tx.creates_contract = true;
  }
  if (value_be && val_len > 0) tx.value.assign(value_be, value_be + val_len);
  if (data && data_len > 0) tx.data.assign(data, data_len + data);
  tx.max_fee_per_gas = {0x04, 0xa8, 0x17, 0xc8, 0x00}; // 20 Gwei
  tx.max_priority_fee_per_gas = {0x3b, 0x9a, 0xca, 0x00}; // 1 Gwei

  auto signed_res = eth_sign_1559(tx, privkey);
  if (!signed_res.ok()) return AIOS_ERR_EXECUTION;

  if (signed_res->size() > *out_len) {
    *out_len = signed_res->size();
    return AIOS_ERR_BUFFER_TOO_SMALL;
  }
  std::memcpy(out_raw_tx, signed_res->data(), signed_res->size());
  *out_len = signed_res->size();

  if (out_txhash_hex) {
    auto h = eth_transaction_hash(*signed_res);
    std::string hex_str = "0x" + eth_hex_of(ByteSpan(h.data(), h.size()));
    std::memcpy(out_txhash_hex, hex_str.c_str(), hex_str.size() + 1);
  }
  return AIOS_OK;
}

int aios_chain_eth_derive_address(const uint8_t privkey[32], char* out_addr, size_t max_len) {
  if (!privkey || !out_addr || max_len < 43) return AIOS_ERR_INVALID;
  auto addr_res = eth_address_of_key(privkey);
  if (!addr_res.ok()) return AIOS_ERR_EXECUTION;
  std::string cs = eth_checksum_address(*addr_res);
  if (cs.size() + 1 > max_len) return AIOS_ERR_BUFFER_TOO_SMALL;
  std::memcpy(out_addr, cs.c_str(), cs.size() + 1);
  return AIOS_OK;
}

int aios_comms_parse_eml_json(const char* raw_eml, char* out_json, size_t max_len) {
  if (!raw_eml || !out_json || max_len == 0) return AIOS_ERR_INVALID;
  auto meta_res = parse_eml_string(raw_eml);
  if (!meta_res.ok()) return AIOS_ERR_EXECUTION;
  
  aios::StrBuf o;
  o << "{\"from\":\"" << json_escape(meta_res->from) << "\","
    << "\"to\":\"" << json_escape(meta_res->to) << "\","
    << "\"subject\":\"" << json_escape(meta_res->subject) << "\","
    << "\"date\":\"" << json_escape(meta_res->date) << "\","
    << "\"body\":\"" << json_escape(meta_res->body) << "\","
    << "\"has_html\":" << (meta_res->has_html ? "true" : "false") << ","
    << "\"attachment_count\":" << meta_res->attachment_count << "}";
  std::string s = o.str();
  if (s.size() + 1 > max_len) return AIOS_ERR_BUFFER_TOO_SMALL;
  std::memcpy(out_json, s.c_str(), s.size() + 1);
  return AIOS_OK;
}

// RFC 5545 / 6350 content lines: unfold continuation lines (a leading space or
// tab), then NAME[;PARAM=..]:VALUE. Returns (upper-case name, unescaped value).
static std::vector<std::pair<std::string, std::string>> content_lines(const char* data) {
  std::vector<std::string> lines;
  for (std::string line : aios::split_lines(std::string(data))) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty() && (line[0] == ' ' || line[0] == '\t') && !lines.empty()) { lines.back() += line.substr(1); continue; }
    lines.push_back(line);
  }
  std::vector<std::pair<std::string, std::string>> out;
  for (auto& l : lines) {
    size_t colon = l.find(':');
    if (colon == std::string::npos) continue;
    std::string head = l.substr(0, colon);
    std::string name = head.substr(0, head.find(';'));
    for (auto& ch : name) ch = (char)std::toupper((unsigned char)ch);
    std::string v, raw = l.substr(colon + 1);
    for (size_t k = 0; k < raw.size(); ++k) {
      if (raw[k] == '\\' && k + 1 < raw.size()) {
        char e = raw[++k];
        v += (e == 'n' || e == 'N') ? '\n' : e;
      } else v += raw[k];
    }
    while (!v.empty() && v.back() == ' ') v.pop_back();
    out.emplace_back(std::move(name), std::move(v));
  }
  return out;
}

int aios_comms_parse_ical_json(const char* ics_data, char* out_json, size_t max_len) {
  if (!ics_data || !out_json || max_len == 0) return AIOS_ERR_INVALID;
  std::vector<std::string> summaries;
  std::string cur_summary, cur_uid, cur_dtstart, cur_dtend, cur_location, cur_description;
  bool in_vevent = false;
  int depth = 0;   // a VALARM inside a VEVENT has its own DESCRIPTION; keep the event's

  for (auto& [name, value] : content_lines(ics_data)) {
    if (name == "BEGIN" && value == "VEVENT") {
      in_vevent = true; depth = 0;
      cur_summary.clear(); cur_uid.clear(); cur_dtstart.clear(); cur_dtend.clear(); cur_location.clear(); cur_description.clear();
    } else if (name == "END" && value == "VEVENT") {
      if (in_vevent) {
        aios::StrBuf ev;
        ev << "{\"uid\":\"" << json_escape(cur_uid) << "\",\"summary\":\"" << json_escape(cur_summary)
           << "\",\"dtstart\":\"" << json_escape(cur_dtstart) << "\",\"dtend\":\"" << json_escape(cur_dtend)
           << "\",\"location\":\"" << json_escape(cur_location) << "\",\"description\":\"" << json_escape(cur_description) << "\"}";
        summaries.push_back(ev.str());
      }
      in_vevent = false;
    } else if (in_vevent && name == "BEGIN") {
      ++depth;
    } else if (in_vevent && name == "END") {
      if (depth > 0) --depth;
    } else if (in_vevent && depth == 0) {
      if (name == "SUMMARY") cur_summary = value;
      else if (name == "UID") cur_uid = value;
      else if (name == "DTSTART") cur_dtstart = value;
      else if (name == "DTEND") cur_dtend = value;
      else if (name == "LOCATION") cur_location = value;
      else if (name == "DESCRIPTION") cur_description = value;
    }
  }
  aios::StrBuf o;
  o << "{\"events\":[";
  for (size_t i = 0; i < summaries.size(); ++i) {
    if (i > 0) o << ",";
    o << summaries[i];
  }
  o << "],\"count\":" << summaries.size() << "}";
  std::string s = o.str();
  if (s.size() + 1 > max_len) return AIOS_ERR_BUFFER_TOO_SMALL;
  std::memcpy(out_json, s.c_str(), s.size() + 1);
  return AIOS_OK;
}

int aios_comms_parse_vcard_json(const char* vcf_data, char* out_json, size_t max_len) {
  if (!vcf_data || !out_json || max_len == 0) return AIOS_ERR_INVALID;
  std::vector<std::string> cards;
  std::string cur_fn, cur_email, cur_org, cur_tel;
  bool in_vcard = false;

  for (auto& [name, value] : content_lines(vcf_data)) {
    if (name == "BEGIN" && value == "VCARD") {
      in_vcard = true;
      cur_fn.clear(); cur_email.clear(); cur_org.clear(); cur_tel.clear();
    } else if (name == "END" && value == "VCARD") {
      if (in_vcard) {
        aios::StrBuf c;
        c << "{\"fn\":\"" << json_escape(cur_fn) << "\",\"email\":\"" << json_escape(cur_email) << "\",\"org\":\"" << json_escape(cur_org) << "\",\"tel\":\"" << json_escape(cur_tel) << "\"}";
        cards.push_back(c.str());
      }
      in_vcard = false;
    } else if (in_vcard) {
      // EMAIL;TYPE=work:... counts as EMAIL; the first of each is kept.
      if (name == "FN" && cur_fn.empty()) cur_fn = value;
      else if (name == "EMAIL" && cur_email.empty()) cur_email = value;
      else if (name == "ORG" && cur_org.empty()) cur_org = value;
      else if (name == "TEL" && cur_tel.empty()) cur_tel = value;
    }
  }
  aios::StrBuf o;
  o << "{\"contacts\":[";
  for (size_t i = 0; i < cards.size(); ++i) {
    if (i > 0) o << ",";
    o << cards[i];
  }
  o << "],\"count\":" << cards.size() << "}";
  std::string s = o.str();
  if (s.size() + 1 > max_len) return AIOS_ERR_BUFFER_TOO_SMALL;
  std::memcpy(out_json, s.c_str(), s.size() + 1);
  return AIOS_OK;
}

int aios_comms_parse_rss_json(const char* xml_data, char* out_json, size_t max_len) {
  if (!xml_data || !out_json || max_len == 0) return AIOS_ERR_INVALID;
  auto items = parse_feed_xml(xml_data);
  aios::StrBuf o;
  o << "{\"items\":[";
  for (size_t i = 0; i < items.size(); ++i) {
    if (i > 0) o << ",";
    o << "{\"title\":\"" << json_escape(items[i].title) << "\","
      << "\"link\":\"" << json_escape(items[i].link) << "\","
      << "\"description\":\"" << json_escape(items[i].description) << "\","
      << "\"pub_date\":\"" << json_escape(items[i].pub_date) << "\","
      << "\"guid\":\"" << json_escape(items[i].guid) << "\"}";
  }
  o << "],\"count\":" << items.size() << "}";
  std::string s = o.str();
  if (s.size() + 1 > max_len) return AIOS_ERR_BUFFER_TOO_SMALL;
  std::memcpy(out_json, s.c_str(), s.size() + 1);
  return AIOS_OK;
}

// Capability entry points, on the real CapabilityGuard: a capability id is the
// hex of a signed 48-byte CapabilityToken (96 chars). The token is bound to
// subject + resource (the guard's resource key is "<subject>\x1f<resource>"),
// carries the rights mask, and is HMAC-signed with the process key:
// AIOS_CAPABILITY_KEY when set (so tokens verify across processes that share
// it, as aios-engine's do), else 32 random bytes chosen once per process — then
// a token is only good inside the process that minted it.
//
// The actor is recorded nowhere and checked against nothing (this layer has no
// entity ledger): the authority boundary is the key — only code holding it can
// mint — so actor_id is required but advisory.
//
// No key (no env, no /dev/urandom) means no guard: every call then fails
// rather than signing with an empty key anyone could reproduce.
static aios::sidecar::CapabilityGuard* abi_guard() {
  static aios::sidecar::CapabilityGuard* g = [] () -> aios::sidecar::CapabilityGuard* {
    std::string key;
    std::string state_dir;   // revocations are kept only for a key that outlives this process
    if (std::string env = aios::hal::process_host().env("AIOS_CAPABILITY_KEY"); !env.empty()) {
      key = env;
      state_dir = aios::hal::process_host().env("AIOS_CAPABILITY_DIR");
    } else {
      unsigned char raw[32] = {0};
      aios::hal::process_host().random(aios::MutableByteSpan(raw, sizeof raw));
      static const char* hex = "0123456789abcdef";
      for (unsigned char b : raw) { key += hex[b >> 4]; key += hex[b & 15]; }
    }
    return new aios::sidecar::CapabilityGuard(key, state_dir);
  }();
  return g;
}
static std::string cap_key(const char* subject, const char* resource) {
  return std::string(subject) + '\x1f' + resource;
}
static std::string token_hex(const aios::sidecar::CapabilityToken& t) {
  static const char* hex = "0123456789abcdef";
  const uint8_t* b = reinterpret_cast<const uint8_t*>(&t);
  std::string o;
  for (size_t k = 0; k < sizeof t; ++k) { o += hex[b[k] >> 4]; o += hex[b[k] & 15]; }
  return o;
}
static bool token_parse(const char* s, aios::sidecar::CapabilityToken& t) {
  if (!s || std::strlen(s) != sizeof(t) * 2) return false;
  uint8_t* b = reinterpret_cast<uint8_t*>(&t);
  for (size_t k = 0; k < sizeof t; ++k) {
    auto nib = [](char c) -> int { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    int hi = nib(s[2 * k]), lo = nib(s[2 * k + 1]);
    if (hi < 0 || lo < 0) return false;
    b[k] = (uint8_t)(hi << 4 | lo);
  }
  return true;
}
static int put_str(const std::string& v, char* out, size_t max_len) {
  if (v.size() + 1 > max_len) return AIOS_ERR_BUFFER_TOO_SMALL;
  std::memcpy(out, v.c_str(), v.size() + 1);
  return AIOS_OK;
}

int aios_cap_mint(const char* actor_id, const char* subject_id, const char* resource,
                  uint16_t rights, char* out_cap_id, size_t max_len) {
  if (!actor_id || !*actor_id || !subject_id || !*subject_id || !resource || !*resource || !out_cap_id) return AIOS_ERR_INVALID;
  if (rights == 0 || (rights & ~static_cast<uint16_t>(CapabilityRight::ALL)) != 0) return AIOS_ERR_INVALID;
  if (max_len < AIOS_CAP_ID_LEN + 1) return AIOS_ERR_BUFFER_TOO_SMALL;
  auto* guard = abi_guard();
  if (!guard) return AIOS_ERR_EXECUTION;
  auto tok = guard->mint_for_resource(cap_key(subject_id, resource), static_cast<CapabilityRight>(rights));
  return put_str(token_hex(tok), out_cap_id, max_len);
}

int aios_cap_verify(const char* cap_id, const char* subject_id, const char* resource,
                    uint16_t required_rights, int* out_valid) {
  if (!cap_id || !subject_id || !resource || !out_valid) return AIOS_ERR_INVALID;
  *out_valid = 0;
  CapabilityToken tok;
  if (!token_parse(cap_id, tok)) return AIOS_ERR_INVALID;
  if (required_rights == 0 || (required_rights & ~static_cast<uint16_t>(CapabilityRight::ALL)) != 0) return AIOS_ERR_INVALID;
  auto* guard = abi_guard();
  if (!guard) return AIOS_ERR_EXECUTION;
  *out_valid = guard->validate_for_resource(tok, cap_key(subject_id, resource), static_cast<CapabilityRight>(required_rights)) ? 1 : 0;
  return AIOS_OK;
}

int aios_cap_attenuate(const char* parent_cap_id, uint16_t narrowed_rights,
                       char* out_child_cap_id, size_t max_len) {
  if (!parent_cap_id || !out_child_cap_id) return AIOS_ERR_INVALID;
  if (max_len < AIOS_CAP_ID_LEN + 1) return AIOS_ERR_BUFFER_TOO_SMALL;
  CapabilityToken parent;
  if (!token_parse(parent_cap_id, parent)) return AIOS_ERR_INVALID;
  if (narrowed_rights == 0) return AIOS_ERR_INVALID;
  auto* guard = abi_guard();
  if (!guard) return AIOS_ERR_EXECUTION;
  // Only a capability that may be delegated can be attenuated, and only to a subset.
  if (!(parent.rights & static_cast<uint32_t>(CapabilityRight::DELEGATE))) return AIOS_ERR_DENIED;
  auto child = guard->attenuate(parent, static_cast<CapabilityRight>(narrowed_rights));
  if (!child) return AIOS_ERR_DENIED;   // forged, or asks for rights the parent lacks
  return put_str(token_hex(*child), out_child_cap_id, max_len);
}

int aios_cap_revoke(const char* subject_id, const char* resource) {
  if (!subject_id || !*subject_id || !resource || !*resource) return AIOS_ERR_INVALID;
  auto* guard = abi_guard();
  if (!guard) return AIOS_ERR_EXECUTION;
  // False: revoked in this process, but the store could not be written.
  if (!guard->revoke_resource(cap_key(subject_id, resource))) return AIOS_ERR_EXECUTION;
  return AIOS_OK;
}

int aios_checkpoint_serialize(const char* entity_id, const uint8_t* payload, size_t len,
                              uint8_t* out_ckpt, size_t* out_len) {
  if (!entity_id || !payload || !out_ckpt || !out_len) return AIOS_ERR_INVALID;
  size_t total_needed = 24 + len;
  if (*out_len < total_needed) {
    *out_len = total_needed;
    return AIOS_ERR_BUFFER_TOO_SMALL;
  }
  uint32_t magic = 0x434B4F41u;
  uint32_t version = 2;
  uint64_t body_len = len;
  uint64_t sum = fnv1a(ByteSpan(payload, len));

  ByteBuffer hdr;
  Writer w(hdr);
  w.u32(magic);
  w.u32(version);
  w.u64(body_len);
  w.u64(sum);

  std::memcpy(out_ckpt, hdr.data(), 24);
  std::memcpy(out_ckpt + 24, payload, len);
  *out_len = total_needed;
  return AIOS_OK;
}

int aios_checkpoint_validate(const uint8_t* ckpt_data, size_t len,
                             int* out_valid, uint32_t* out_version, size_t* out_body_len) {
  if (!ckpt_data || len < 24 || !out_valid || !out_version || !out_body_len) return AIOS_ERR_INVALID;
  auto meta_res = inspect_checkpoint(ByteSpan(ckpt_data, len));
  if (!meta_res.ok()) {
    *out_valid = 0;
    return AIOS_ERR_INVALID;
  }
  *out_valid = meta_res->valid ? 1 : 0;
  *out_version = meta_res->version;
  *out_body_len = (size_t)meta_res->body_size;
  return AIOS_OK;
}

int aios_svg_render_png(const char* svg_xml, int width, int height,
                        uint32_t current_color_argb, uint8_t* out_png, size_t* out_len) {
  if (!svg_xml || width <= 0 || height <= 0 || !out_png || !out_len) return AIOS_ERR_INVALID;
  SvgRenderOptions opts;
  opts.width = width;
  opts.height = height;
  opts.current_color = current_color_argb;
  auto res = render_svg_to_png(svg_xml, opts);
  if (!res.ok()) return AIOS_ERR_EXECUTION;
  if (res->size() > *out_len) {
    *out_len = res->size();
    return AIOS_ERR_BUFFER_TOO_SMALL;
  }
  std::memcpy(out_png, res->data(), res->size());
  *out_len = res->size();
  return AIOS_OK;
}

int aios_svg_render_rgba(const char* svg_xml, int width, int height,
                         uint32_t current_color_argb, uint32_t* out_argb, size_t max_pixels) {
  if (!svg_xml || width <= 0 || height <= 0 || !out_argb || max_pixels < (size_t)(width * height)) {
    return AIOS_ERR_INVALID;
  }
  auto dom = web::parse_html(ByteSpan(reinterpret_cast<const uint8_t*>(svg_xml), std::strlen(svg_xml)));
  if (!dom) return AIOS_ERR_INVALID;
  web::Node* svg = dom->tag == "svg" ? dom.get() : dom->first("svg");
  if (!svg) return AIOS_ERR_NOT_FOUND;
  auto bm = web::rasterise_svg(*svg, width, height, current_color_argb);
  if (!bm.ok()) return AIOS_ERR_EXECUTION;
  std::memcpy(out_argb, bm->pixels.data(), bm->pixels.size() * sizeof(uint32_t));
  return AIOS_OK;
}

int aios_pdf_render_html(const char* html, const char* css, int viewport_w,
                         uint8_t* out_pdf, size_t* out_len) {
  if (!html || !out_pdf || !out_len) return AIOS_ERR_INVALID;
  auto res = render_html_to_pdf(html, css ? css : "", viewport_w > 0 ? viewport_w : 800);
  if (!res.ok()) return AIOS_ERR_EXECUTION;
  if (res->size() > *out_len) {
    *out_len = res->size();
    return AIOS_ERR_BUFFER_TOO_SMALL;
  }
  std::memcpy(out_pdf, res->data(), res->size());
  *out_len = res->size();
  return AIOS_OK;
}

double aios_haversine_distance(double lat1, double lng1, double lat2, double lng2) {
  return erp::haversine_miles(lat1, lng1, lat2, lng2) * 1609.344;
}

int aios_tsp_solve_2opt(const double* coords_latlng, int count,
                        int* out_order, double* out_distance_meters) {
  if (!coords_latlng || count <= 0 || !out_order || !out_distance_meters) return AIOS_ERR_INVALID;
  out_order[0] = 0;
  *out_distance_meters = 0;
  if (count == 1) return AIOS_OK;
  // Point 0 is the start; the other count-1 points are the stops, and the tour
  // closes back at the start. route_improve is the nearest-neighbour seed plus
  // 2-opt / Or-opt passes (pool_greedy, used before, is the seed alone and
  // returns count-1 entries — the last slot was filled with a made-up index).
  auto res = erp::route_improve(coords_latlng + 2, count - 1, coords_latlng, erp::RouteEnd::Home, nullptr);
  if (!res.ok()) return AIOS_ERR_EXECUTION;
  if ((int)res->order.size() != count - 1) return AIOS_ERR_EXECUTION;
  std::vector<char> seen(count, 0);
  seen[0] = 1;
  for (int i = 0; i < count - 1; ++i) {
    int idx = res->order[i] + 1;   // stop index -> input index
    if (idx < 1 || idx >= count || seen[idx]) return AIOS_ERR_EXECUTION;   // never hand back a non-permutation
    seen[idx] = 1;
    out_order[i + 1] = idx;
  }
  *out_distance_meters = res->distance * 1609.344;
  return AIOS_OK;
}

int aios_grain_run(const char* source_code, uint64_t step_limit,
                   int64_t* out_int_result, double* out_real_result,
                   char* out_output_buf, size_t max_out_len,
                   char* out_err_buf, size_t max_err_len) {
  if (!source_code) return AIOS_ERR_INVALID;
  auto compiled = grain::compile_source(source_code, "sidecar", true);
  if (!compiled.ok()) {
    if (out_err_buf && max_err_len > 0) {
      std::string err = compiled.errors.empty() ? "compile error" : compiled.errors[0].message;
      std::strncpy(out_err_buf, err.c_str(), max_err_len - 1);
      out_err_buf[max_err_len - 1] = '\0';
    }
    return AIOS_ERR_EXECUTION;
  }
  grain::Vm vm;
  grain::install_builtins(vm);
  std::vector<std::string> output;
  vm.sink = &output;
  vm.step_limit = step_limit;
  auto res = vm.run(compiled.script);
  if (!res.ok) {
    if (out_err_buf && max_err_len > 0) {
      std::strncpy(out_err_buf, res.error.c_str(), max_err_len - 1);
      out_err_buf[max_err_len - 1] = '\0';
    }
    return AIOS_ERR_EXECUTION;
  }
  if (out_int_result) *out_int_result = res.value.as_int();
  if (out_real_result) *out_real_result = res.value.as_double();
  if (out_output_buf && max_out_len > 0) {
    aios::StrBuf os;
    for (size_t i = 0; i < output.size(); ++i) {
      if (i > 0) os << "\n";
      os << output[i];
    }
    std::string s = os.str();
    std::strncpy(out_output_buf, s.c_str(), max_out_len - 1);
    out_output_buf[max_out_len - 1] = '\0';
  }
  return AIOS_OK;
}

int aios_patch_check_dry_run_ex(const char* unified_diff, const char* root_dir,
                                int* out_hunk_count, int* out_file_count, int* out_can_apply,
                                char* out_err, size_t max_err_len) {
  if (!unified_diff || !out_hunk_count || !out_file_count || !out_can_apply) return AIOS_ERR_INVALID;
  *out_can_apply = 0;
  *out_hunk_count = 0;
  *out_file_count = 0;
  auto put_err = [&](const std::string& e) {
    if (!out_err || max_err_len == 0) return;
    std::strncpy(out_err, e.c_str(), max_err_len - 1);
    out_err[max_err_len - 1] = '\0';
  };
  put_err("");
  auto tx_res = PatchEngine::parse_unified_diff(unified_diff);
  if (!tx_res.ok()) { put_err(tx_res.error().message); return AIOS_ERR_INVALID; }
  if (tx_res->files.empty()) { put_err("no file changes in the diff"); return AIOS_ERR_INVALID; }
  *out_file_count = (int)tx_res->files.size();
  int hunks = 0;
  for (const auto& f : tx_res->files) hunks += (int)f.hunks.size();
  *out_hunk_count = hunks;
  // "Can apply" is answered by checking every hunk's context and deleted lines
  // against the files as they are on disk now — never assumed from a parse.
  auto read = [](const std::string& path, std::string& out) {
    auto b = aios::hal::process_host().read_file(path);
    if (!b.ok()) return false;
    out.assign(b->begin(), b->end());
    return true;
  };
  auto dr = PatchEngine::dry_run_with(read, *tx_res, root_dir && *root_dir ? root_dir : ".");
  if (!dr.ok()) { put_err(dr.error().message); return AIOS_OK; }
  *out_can_apply = 1;
  return AIOS_OK;
}

int aios_patch_check_dry_run(const char* unified_diff, int* out_hunk_count,
                             int* out_file_count, int* out_can_apply) {
  return aios_patch_check_dry_run_ex(unified_diff, ".", out_hunk_count, out_file_count, out_can_apply, nullptr, 0);
}

void aios_crypto_sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
  auto d = crypto::sha256(ByteSpan(data, len));
  std::memcpy(out, d.data(), 32);
}

void aios_crypto_keccak256(const uint8_t* data, size_t len, uint8_t out[32]) {
  auto d = crypto::keccak256(ByteSpan(data, len));
  std::memcpy(out, d.data(), 32);
}

int aios_crypto_secp256k1_sign(const uint8_t privkey[32], const uint8_t msg_hash[32],
                               uint8_t out_sig[64], int* out_recid) {
  if (!privkey || !msg_hash || !out_sig || !out_recid) return AIOS_ERR_INVALID;
  auto sig = crypto::ecdsa_sign(privkey, msg_hash);
  if (!sig.ok()) return AIOS_ERR_EXECUTION;
  std::memcpy(out_sig, sig->r.data(), 32);
  std::memcpy(out_sig + 32, sig->s.data(), 32);
  *out_recid = sig->recovery_id;
  return AIOS_OK;
}

int aios_crypto_secp256k1_recover(const uint8_t sig[64], int recid,
                                  const uint8_t msg_hash[32], uint8_t out_pubkey[65]) {
  if (!sig || !msg_hash || !out_pubkey) return AIOS_ERR_INVALID;
  crypto::EcSignature s;
  std::memcpy(s.r.data(), sig, 32);
  std::memcpy(s.s.data(), sig + 32, 32);
  s.recovery_id = recid;
  auto pub = crypto::ecdsa_recover(s, msg_hash, false);
  if (!pub.ok() || pub->size() != 65) return AIOS_ERR_EXECUTION;
  std::memcpy(out_pubkey, pub->data(), 65);
  return AIOS_OK;
}

void aios_crypto_ed25519_sign(const uint8_t privkey[32], const uint8_t* data, size_t len,
                              uint8_t out_sig[64]) {
  crypto::Scalar32 sec;
  std::memcpy(sec.data(), privkey, 32);
  auto pub = crypto::ed25519_public_from_seed(sec);
  auto sig = crypto::ed25519_sign(ByteSpan(data, len), sec, pub);
  std::memcpy(out_sig, sig.data(), 64);
}

int aios_crypto_ed25519_verify(const uint8_t pubkey[32], const uint8_t* data, size_t len,
                                const uint8_t sig[64]) {
  crypto::Point32 pub;
  std::memcpy(pub.data(), pubkey, 32);
  crypto::Sig64 s;
  std::memcpy(s.data(), sig, 64);
  return crypto::ed25519_verify(ByteSpan(data, len), s, pub) ? 1 : 0;
}

} // extern "C"

namespace aios::sidecar {
BinaryTelemetryCore& get_global_telemetry() {
  static BinaryTelemetryCore telemetry(131072); // 128k event ring buffer (~8MB RAM)
  return telemetry;
}
}

extern "C" {

int aios_telemetry_record(uint64_t trace_id, uint64_t span_id, uint32_t service_id,
                          uint16_t event_type, uint16_t status_code, uint32_t duration_us,
                          uint32_t payload_int, const char* tag) {
  aios::sidecar::get_global_telemetry().record(trace_id, span_id, service_id, event_type,
                                              status_code, duration_us, payload_int, tag);
  return AIOS_OK;
}

int aios_telemetry_get_trace_json(uint64_t trace_id, char* out_json, size_t max_len) {
  if (!out_json || max_len == 0) return AIOS_ERR_INVALID;

  auto events = aios::sidecar::get_global_telemetry().query_trace(trace_id);
  std::string s = "{\"trace_id\":" + std::to_string(trace_id) + ",\"events\":[";
  for (size_t i = 0; i < events.size(); ++i) {
    if (i > 0) s += ",";
    const auto& ev = events[i];
    s += "{\"span_id\":" + std::to_string(ev.span_id) + ",\"type\":" + std::to_string(ev.event_type) +
         ",\"status\":" + std::to_string(ev.status_code) + ",\"duration_us\":" + std::to_string(ev.duration_us) +
         ",\"payload_int\":" + std::to_string(ev.payload_int) + ",\"tag\":\"" + json_escape(ev.tag) + "\"}";
  }
  s += "]}";
  if (s.size() + 1 > max_len) return AIOS_ERR_BUFFER_TOO_SMALL;
  std::memcpy(out_json, s.c_str(), s.size() + 1);
  return AIOS_OK;
}

int aios_telemetry_git_stats(uint64_t window_seconds, 
                             uint64_t* out_ops_count, 
                             uint64_t* out_errors,
                             double* out_avg_us, 
                             uint32_t* out_p99_us) {
  uint64_t now_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  uint64_t start_ns = (window_seconds == 0) ? 0 : (now_ns - (window_seconds * 1000000000ULL));

  auto stats = aios::sidecar::get_global_telemetry().compute_stats(aios::sidecar::kServiceGitAgent, start_ns);

  if (out_ops_count) *out_ops_count = stats.count;
  if (out_errors)    *out_errors = stats.error_count;
  if (out_avg_us)     *out_avg_us = stats.avg_duration_us;
  if (out_p99_us)    *out_p99_us = stats.p99_duration_us;

  return AIOS_OK;
}

} // extern "C"
