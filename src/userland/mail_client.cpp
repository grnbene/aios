#include "aios/userland/mail_client.hpp"
#include "aios/node/secure.hpp"
#include "aios/hal/sync.hpp"
#include "aios/core/bytes.hpp"
#include "aios/crypto/sha256.hpp"
#include "aios/core/base64.hpp"

#include <algorithm>
#include <sstream>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <charconv>
#include <unordered_map>
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <mutex>
#include <direct.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")
#ifndef S_ISREG
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif
static inline int mkdir(const char* path, int /*mode*/) { return ::_mkdir(path); }
#elif !defined(AIOS_METAL)
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <signal.h>
#endif
#include <sys/stat.h>

namespace aios::userland {

namespace {

// std::stoll/std::stoull/std::stoul throw on a bad field, and bare metal is
// built with -fno-exceptions; from_chars never throws by design.
template <typename T>
T parse_num(const std::string& s, T fallback = 0) {
  T v = fallback;
  auto res = std::from_chars(s.data(), s.data() + s.size(), v);
  if (res.ec != std::errc()) return fallback;
  return v;
}

char to_lower(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

bool str_contains_ci(const std::string& haystack, const std::string& needle) {
  if (needle.empty()) return true;
  if (haystack.size() < needle.size()) return false;
  auto it = std::search(
      haystack.begin(), haystack.end(),
      needle.begin(), needle.end(),
      [](char ch1, char ch2) { return to_lower(ch1) == to_lower(ch2); }
  );
  return it != haystack.end();
}

std::string trim_str(const std::string& s) {
  size_t start = 0;
  while (start < s.size() && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r' || s[start] == '\n')) {
    start++;
  }
  size_t end = s.size();
  while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n')) {
    end--;
  }
  return s.substr(start, end - start);
}

std::string format_time_now() {
  auto now = std::chrono::system_clock::now();
  std::time_t tt = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &tt);
#else
  localtime_r(&tt, &tm);
#endif
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
  return std::string(buf);
}

// How many of the newest INBOX messages a sync pulls down.
constexpr int kSyncFetchLimit = 30;

// True randomness for the keyring key/nonce. Bare metal has no /dev/urandom,
// so this mirrors hal_metal.cpp's own random(): RDRAND when the CPU has it,
// RDTSC-mixed as a fallback. MailStore only ever gets a root directory, never
// a hal::Host, so this can't just call hal::Host::random().
void secure_random_bytes(uint8_t* buf, size_t n) {
#if defined(AIOS_METAL) && defined(__aarch64__)
  // Armv8 counterpart of the x86 path below. The x86 side can probe for
  // RDRAND, because every core that lacks it still assembles the mnemonic.
  // That is not true of RNDR: the toolchain for this target is built for
  // -mcpu=cortex-a53, which predates Armv8.5-RNG, and GAS refuses to assemble
  // "mrs _, RNDR" at all ("selected processor does not support system register
  // name 'rndr'") -- so a runtime feature gate around it cannot work, the
  // failure is at assembly time, not execution. Since cortex-a53 has no RNDR
  // to read in any case, use the always-present virtual counter CNTVCT_EL0 --
  // the ARM equivalent of RDTSC -- through the same multiply/shift mixing step.
  for (size_t i = 0; i < n; i += 8) {
    uint64_t c; asm volatile("mrs %0, CNTVCT_EL0" : "=r"(c));
    uint64_t v = (c * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)i + 1u);
    v ^= v >> 30; v *= 0xBF58476D1CE4E5B9ull; v ^= v >> 27;
    for (size_t k = 0; k < 8 && i + k < n; ++k) buf[i + k] = (uint8_t)(v >> (8 * k));
  }
#elif defined(AIOS_METAL)
  uint32_t a, b, c, d;
  asm volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
  bool rdrand = c & (1u << 30);
  for (size_t i = 0; i < n; i += 8) {
    uint64_t v; unsigned char ok = 0;
    if (rdrand) asm volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
    if (!ok) {
      uint32_t lo, hi; asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
      v = (((uint64_t)hi << 32) | lo) * 0x9E3779B97F4A7C15ull ^ (uint64_t)i;
    }
    for (size_t k = 0; k < 8 && i + k < n; ++k) buf[i + k] = (uint8_t)(v >> (8 * k));
  }
#elif defined(_WIN32)
  size_t off = 0;
  if (::BCryptGenRandom(nullptr, buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0) off = n;
  for (; off < n; ++off) buf[off] = (uint8_t)std::chrono::steady_clock::now().time_since_epoch().count();
#else
  int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  size_t off = 0;
  if (fd >= 0) {
    while (off < n) {
      ssize_t r = ::read(fd, buf + off, n - off);
      if (r <= 0) break;
      off += (size_t)r;
    }
    ::close(fd);
  }
  for (; off < n; ++off) buf[off] = (uint8_t)std::chrono::steady_clock::now().time_since_epoch().count();
#endif
}

// The vault's real secret: 32 random bytes generated once per install and
// kept in their own file, never a literal in source. Losing keyring.key means
// the vault can't be opened again -- the correct failure mode for an actual
// key, unlike the one it replaces.
crypto::Key load_or_create_keyring_key(const std::string& root_dir) {
  std::string path = root_dir + "/keyring.key";
  crypto::Key key{};
  std::ifstream in(path, std::ios::binary);
  if (in.is_open()) {
    in.read(reinterpret_cast<char*>(key.data()), (std::streamsize)key.size());
    if (in.gcount() == (std::streamsize)key.size()) return key;
  }
  secure_random_bytes(key.data(), key.size());
  std::string tmp = path + ".tmp";
  std::ofstream out(tmp, std::ios::binary);
  if (out.is_open()) {
    out.write(reinterpret_cast<const char*>(key.data()), (std::streamsize)key.size());
    out.close();
#if !defined(AIOS_METAL) && !defined(_WIN32)
    ::chmod(tmp.c_str(), 0600);
#endif
    ::rename(tmp.c_str(), path.c_str());
  }
  return key;
}

// Legacy V1 vault: key, salt AND nonce were hardcoded literals in source,
// which handed decryption to anyone with the source tree and broke Poly1305
// authentication outright via nonce reuse. Kept only so load_accounts_and_vault
// can decrypt a vault written under that scheme once, then immediately
// re-encrypt it under load_or_create_keyring_key() with a random nonce so the
// broken ciphertext never survives past the next save. Never used for a write.
crypto::Key legacy_v1_key() {
  std::string salt_str = "aethermail-keyring-salt-2026";
  return crypto::derive_key("aethermail-host-keyring-vault-secret", span_of(salt_str));
}
crypto::Nonce legacy_v1_nonce() {
  return crypto::Nonce{0x41, 0x45, 0x54, 0x48, 0x45, 0x52, 0x56, 0x41, 0x55, 0x4c, 0x54, 0x01};
}

bool file_exists(const std::string& path) {
  if (path.empty()) return false;
#if defined(AIOS_METAL)
  // No stat() on bare metal; opening it is the only question we can ask, and
  // the only one this needs answered.
  std::ifstream probe(path, std::ios::binary);
  return probe.is_open();
#else
  struct stat st{};
  return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#endif
}

void ensure_dir(const std::string& path) {
  if (path.empty()) return;
#if !defined(AIOS_METAL)
  std::string cur;
  for (size_t i = 0; i < path.size(); ++i) {
    cur += path[i];
    if (path[i] == '/' || i + 1 == path.size()) {
      ::mkdir(cur.c_str(), 0755);
    }
  }
#endif
}

std::string escape_tab_field(const std::string& s) {
  std::string res;
  res.reserve(s.size() + 8);
  for (char c : s) {
    if (c == '\t') res += "\\t";
    else if (c == '\n') res += "\\n";
    else if (c == '\r') res += "\\r";
    else if (c == '\\') res += "\\\\";
    else res += c;
  }
  return res;
}

std::string unescape_tab_field(const std::string& s) {
  std::string res;
  res.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      char next = s[++i];
      if (next == 't') res += '\t';
      else if (next == 'n') res += '\n';
      else if (next == 'r') res += '\r';
      else if (next == '\\') res += '\\';
      else res += next;
    } else {
      res += s[i];
    }
  }
  return res;
}

std::vector<std::string> split_tab_line(const std::string& line) {
  std::vector<std::string> fields;
  size_t start = 0;
  while (start <= line.size()) {
    size_t pos = line.find('\t', start);
    if (pos == std::string::npos) {
      fields.push_back(unescape_tab_field(line.substr(start)));
      break;
    }
    fields.push_back(unescape_tab_field(line.substr(start, pos - start)));
    start = pos + 1;
  }
  return fields;
}

// Simple JSON helper for migrating legacy files if found
std::string json_unescape(const std::string& s) {
  std::string res;
  res.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      char next = s[++i];
      switch (next) {
        case '"': res += '"'; break;
        case '\\': res += '\\'; break;
        case '/': res += '/'; break;
        case 'b': res += '\b'; break;
        case 'f': res += '\f'; break;
        case 'n': res += '\n'; break;
        case 'r': res += '\r'; break;
        case 't': res += '\t'; break;
        default: res += next; break;
      }
    } else {
      res += s[i];
    }
  }
  return res;
}

std::string json_get_str(const std::string& obj, const std::string& key, const std::string& def = "") {
  std::string pattern = "\"" + key + "\"";
  size_t p = obj.find(pattern);
  if (p == std::string::npos) return def;
  p += pattern.size();
  while (p < obj.size() && (obj[p] == ' ' || obj[p] == '\t' || obj[p] == ':' || obj[p] == '\r' || obj[p] == '\n')) p++;
  if (p >= obj.size()) return def;
  if (obj[p] == '"') {
    p++;
    size_t end = p;
    bool esc = false;
    while (end < obj.size()) {
      if (esc) { esc = false; }
      else if (obj[end] == '\\') { esc = true; }
      else if (obj[end] == '"') { break; }
      end++;
    }
    return json_unescape(obj.substr(p, end - p));
  }
  size_t end = obj.find_first_of(",}\n\r", p);
  if (end == std::string::npos) end = obj.size();
  std::string val = obj.substr(p, end - p);
  while (!val.empty() && (val.back() == ' ' || val.back() == '\t')) val.pop_back();
  return val;
}

int64_t json_get_int(const std::string& obj, const std::string& key, int64_t def = 0) {
  std::string s = json_get_str(obj, key, "");
  if (s.empty()) return def;
  return parse_num<int64_t>(s, def);
}

bool json_get_bool(const std::string& obj, const std::string& key, bool def = false) {
  std::string s = json_get_str(obj, key, "");
  if (s == "true") return true;
  if (s == "false") return false;
  return def;
}

std::vector<std::string> json_get_array(const std::string& obj, const std::string& key) {
  std::vector<std::string> res;
  std::string pattern = "\"" + key + "\"";
  size_t p = obj.find(pattern);
  if (p == std::string::npos) return res;
  p = obj.find('[', p);
  if (p == std::string::npos) return res;
  size_t end = obj.find(']', p);
  if (end == std::string::npos) return res;
  std::string content = obj.substr(p + 1, end - p - 1);

  size_t idx = 0;
  while (idx < content.size()) {
    size_t q1 = content.find('"', idx);
    if (q1 == std::string::npos) break;
    size_t q2 = q1 + 1;
    bool esc = false;
    while (q2 < content.size()) {
      if (esc) esc = false;
      else if (content[q2] == '\\') esc = true;
      else if (content[q2] == '"') break;
      q2++;
    }
    if (q2 >= content.size()) break;
    res.push_back(json_unescape(content.substr(q1 + 1, q2 - q1 - 1)));
    idx = q2 + 1;
  }
  return res;
}

std::vector<std::string> split_json_objects(const std::string& json_array_str) {
  std::vector<std::string> objs;
  int depth = 0;
  size_t start = 0;
  bool in_str = false;
  bool esc = false;
  for (size_t i = 0; i < json_array_str.size(); ++i) {
    char c = json_array_str[i];
    if (in_str) {
      if (esc) esc = false;
      else if (c == '\\') esc = true;
      else if (c == '"') in_str = false;
    } else {
      if (c == '"') in_str = true;
      else if (c == '{') {
        if (depth == 0) start = i;
        depth++;
      } else if (c == '}') {
        depth--;
        if (depth == 0) {
          objs.push_back(json_array_str.substr(start, i - start + 1));
        }
      }
    }
  }
  return objs;
}

}  // namespace

// ============================================================================
// RFC 5322 MIME & EML Serialization / Deserialization
// ============================================================================

std::string MailStore::serialize_rfc5322(const MailMessage& m) {
  std::ostringstream ss;
  if (!m.from_name.empty()) {
    ss << "From: \"" << m.from_name << "\" <" << m.from_addr << ">\r\n";
  } else {
    ss << "From: <" << m.from_addr << ">\r\n";
  }

  if (!m.to.empty()) {
    ss << "To: ";
    for (size_t i = 0; i < m.to.size(); ++i) {
      if (i > 0) ss << ", ";
      ss << m.to[i];
    }
    ss << "\r\n";
  }

  if (!m.cc.empty()) {
    ss << "Cc: ";
    for (size_t i = 0; i < m.cc.size(); ++i) {
      if (i > 0) ss << ", ";
      ss << m.cc[i];
    }
    ss << "\r\n";
  }

  if (!m.bcc.empty()) {
    ss << "Bcc: ";
    for (size_t i = 0; i < m.bcc.size(); ++i) {
      if (i > 0) ss << ", ";
      ss << m.bcc[i];
    }
    ss << "\r\n";
  }

  ss << "Subject: " << (m.subject.empty() ? "(No Subject)" : m.subject) << "\r\n";

  // Format RFC 2822 date
  std::time_t tt = m.timestamp ? (std::time_t)m.timestamp : std::time(nullptr);
  std::tm tm_buf{};
#if defined(_WIN32)
  gmtime_s(&tm_buf, &tt);
#else
  gmtime_r(&tt, &tm_buf);
#endif
  char date_buf[128];
  std::strftime(date_buf, sizeof(date_buf), "%a, %d %b %Y %H:%M:%S +0000", &tm_buf);
  ss << "Date: " << date_buf << "\r\n";

  if (!m.message_id.empty()) {
    ss << "Message-ID: " << m.message_id << "\r\n";
  } else {
    ss << "Message-ID: <" << m.id << "@aios.org>\r\n";
  }

  // Envelope Metadata Headers (Apple Mail / RFC 5322 extension)
  ss << "X-AetherMail-ID: " << m.id << "\r\n";
  ss << "X-AetherMail-Account: " << m.account << "\r\n";
  ss << "X-AetherMail-Folder: " << m.folder << "\r\n";
  ss << "X-AetherMail-DateStr: " << m.date_str << "\r\n";

  std::string flags;
  if (!m.is_unread) flags += "\\Seen ";
  if (m.is_flagged) flags += "\\Flagged ";
  if (m.is_draft) flags += "\\Draft ";
  if (!flags.empty()) {
    flags.pop_back();
    ss << "X-AetherMail-Flags: " << flags << "\r\n";
  }
  ss << "MIME-Version: 1.0\r\n";

  if (m.attachments.empty()) {
    ss << "Content-Type: text/plain; charset=utf-8\r\n";
    ss << "Content-Transfer-Encoding: 8bit\r\n\r\n";
    ss << m.body;
  } else {
    std::string boundary = "----=_Part_" + m.id + "_AetherMailBoundary";
    ss << "Content-Type: multipart/mixed; boundary=\"" << boundary << "\"\r\n\r\n";
    ss << "--" << boundary << "\r\n";
    ss << "Content-Type: text/plain; charset=utf-8\r\n";
    ss << "Content-Transfer-Encoding: 8bit\r\n\r\n";
    ss << m.body << "\r\n\r\n";

    for (const auto& att : m.attachments) {
      ss << "--" << boundary << "\r\n";
      ss << "Content-Type: " << att.mime_type << "; name=\"" << att.filename << "\"\r\n";
      ss << "Content-Disposition: attachment; filename=\"" << att.filename << "\"\r\n";
      ss << "X-Attachment-Size: " << att.size_bytes << "\r\n\r\n";
      ss << "[Attachment payload: " << att.filename << " (" << att.size_bytes << " bytes)]\r\n";
    }
    ss << "--" << boundary << "--\r\n";
  }

  return ss.str();
}

bool MailStore::parse_rfc5322(const std::string& raw_eml, MailMessage& out) {
  if (raw_eml.empty()) return false;

  std::istringstream stream(raw_eml);
  std::string line;
  std::string current_header;
  std::string current_val;
  std::unordered_map<std::string, std::string> headers;

  // 1. Read headers
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) {
      // Empty line marks end of headers
      if (!current_header.empty()) {
        headers[current_header] = current_val;
      }
      break;
    }

    if (line[0] == ' ' || line[0] == '\t') {
      // Header continuation (folding)
      current_val += " " + trim_str(line);
    } else {
      if (!current_header.empty()) {
        headers[current_header] = current_val;
      }
      size_t colon = line.find(':');
      if (colon != std::string::npos) {
        current_header = line.substr(0, colon);
        for (char& c : current_header) c = to_lower(c);
        current_val = trim_str(line.substr(colon + 1));
      } else {
        current_header.clear();
      }
    }
  }

  // 2. Read body
  std::ostringstream body_ss;
  while (std::getline(stream, line)) {
    body_ss << line << "\n";
  }
  std::string full_body = body_ss.str();

  // Populate fields from headers
  if (headers.count("x-aethermail-id")) out.id = headers["x-aethermail-id"];
  if (headers.count("message-id")) out.message_id = headers["message-id"];
  if (headers.count("x-aethermail-account")) out.account = headers["x-aethermail-account"];
  if (headers.count("x-aethermail-folder")) out.folder = headers["x-aethermail-folder"];
  if (headers.count("x-aethermail-datestr")) out.date_str = headers["x-aethermail-datestr"];
  if (headers.count("subject")) out.subject = headers["subject"];

  if (headers.count("x-aethermail-flags")) {
    const std::string& fl = headers["x-aethermail-flags"];
    out.is_unread = (fl.find("\\Seen") == std::string::npos);
    out.is_flagged = (fl.find("\\Flagged") != std::string::npos);
    out.is_draft = (fl.find("\\Draft") != std::string::npos);
  }

  // Parse From
  if (headers.count("from")) {
    const std::string& f = headers["from"];
    size_t lt = f.find('<');
    size_t gt = f.find('>');
    if (lt != std::string::npos && gt != std::string::npos && gt > lt) {
      out.from_addr = f.substr(lt + 1, gt - lt - 1);
      std::string name_part = trim_str(f.substr(0, lt));
      if (name_part.size() >= 2 && name_part.front() == '"' && name_part.back() == '"') {
        name_part = name_part.substr(1, name_part.size() - 2);
      }
      out.from_name = name_part;
    } else {
      out.from_addr = trim_str(f);
      out.from_name = out.from_addr;
    }
  }

  // Parse To
  if (headers.count("to")) {
    out.to.clear();
    std::string to_hdr = headers["to"];
    size_t start = 0;
    while (start < to_hdr.size()) {
      size_t comma = to_hdr.find(',', start);
      if (comma == std::string::npos) {
        out.to.push_back(trim_str(to_hdr.substr(start)));
        break;
      }
      out.to.push_back(trim_str(to_hdr.substr(start, comma - start)));
      start = comma + 1;
    }
  }

  // Parse multipart boundary if present
  std::string ctype = headers.count("content-type") ? headers["content-type"] : "";
  size_t bpos = ctype.find("boundary=");
  if (bpos != std::string::npos) {
    std::string bound = ctype.substr(bpos + 9);
    if (bound.size() >= 2 && bound.front() == '"' && bound.back() == '"') {
      bound = bound.substr(1, bound.size() - 2);
    }
    std::string delimiter = "--" + bound;
    size_t p1 = full_body.find(delimiter);
    if (p1 != std::string::npos) {
      size_t p2 = full_body.find(delimiter, p1 + delimiter.size());
      if (p2 != std::string::npos) {
        std::string part1 = full_body.substr(p1 + delimiter.size(), p2 - p1 - delimiter.size());
        size_t part_hdr_end = part1.find("\n\n");
        if (part_hdr_end == std::string::npos) part_hdr_end = part1.find("\r\n\r\n");
        if (part_hdr_end != std::string::npos) {
          out.body = trim_str(part1.substr(part_hdr_end + 2));
        } else {
          out.body = trim_str(part1);
        }
      }
    }
  } else {
    out.body = trim_str(full_body);
  }

  out.snippet = out.body.substr(0, 100);
  out.raw_size = raw_eml.size();
  return true;
}

// ============================================================================
// MailAccount Preset Generation
// ============================================================================

MailAccount MailAccount::make_preset(const std::string& provider_name,
                                     const std::string& email_addr,
                                     const std::string& pass,
                                     const std::string& disp_name) {
  MailAccount acc;
  std::string prov = provider_name;
  for (char& c : prov) c = to_lower(c);

  acc.email = email_addr;
  acc.password = pass;
  acc.use_tls = true;
  acc.online = true;
  acc.is_default = false;

  std::string user_prefix = email_addr;
  size_t at_pos = email_addr.find('@');
  if (at_pos != std::string::npos) user_prefix = email_addr.substr(0, at_pos);

  if (prov == "icloud" || prov == "apple" || prov == "me.com") {
    acc.id = "icloud_" + (user_prefix.empty() ? "user" : user_prefix);
    acc.provider = "icloud";
    acc.display_name = disp_name.empty() ? "iCloud (" + email_addr + ")" : disp_name;
    acc.incoming_server = "imap.mail.me.com";
    acc.incoming_port = 993;
    acc.incoming_protocol = "imap";
    acc.outgoing_server = "smtp.mail.me.com";
    acc.outgoing_port = 587;
    acc.outgoing_protocol = "smtp";
    acc.username = user_prefix;
  } else if (prov == "gmail" || prov == "google") {
    acc.id = "gmail_" + (user_prefix.empty() ? "user" : user_prefix);
    acc.provider = "gmail";
    acc.display_name = disp_name.empty() ? "Google Mail (" + email_addr + ")" : disp_name;
    acc.incoming_server = "imap.gmail.com";
    acc.incoming_port = 993;
    acc.incoming_protocol = "imap";
    acc.outgoing_server = "smtp.gmail.com";
    acc.outgoing_port = 587;
    acc.outgoing_protocol = "smtp";
    acc.username = email_addr;
  } else if (prov == "outlook" || prov == "office365" || prov == "hotmail" || prov == "live") {
    acc.id = "outlook_" + (user_prefix.empty() ? "user" : user_prefix);
    acc.provider = "outlook";
    acc.display_name = disp_name.empty() ? "Outlook Mail (" + email_addr + ")" : disp_name;
    acc.incoming_server = "outlook.office365.com";
    acc.incoming_port = 993;
    acc.incoming_protocol = "imap";
    acc.outgoing_server = "smtp.office365.com";
    acc.outgoing_port = 587;
    acc.outgoing_protocol = "smtp";
    acc.username = email_addr;
  } else if (prov == "fastmail") {
    acc.id = "fastmail_" + (user_prefix.empty() ? "user" : user_prefix);
    acc.provider = "fastmail";
    acc.display_name = disp_name.empty() ? "Fastmail (" + email_addr + ")" : disp_name;
    acc.incoming_server = "imap.fastmail.com";
    acc.incoming_port = 993;
    acc.incoming_protocol = "imap";
    acc.outgoing_server = "smtp.fastmail.com";
    acc.outgoing_port = 587;
    acc.outgoing_protocol = "smtp";
    acc.username = email_addr;
  } else if (prov == "yahoo") {
    acc.id = "yahoo_" + (user_prefix.empty() ? "user" : user_prefix);
    acc.provider = "yahoo";
    acc.display_name = disp_name.empty() ? "Yahoo Mail (" + email_addr + ")" : disp_name;
    acc.incoming_server = "imap.mail.yahoo.com";
    acc.incoming_port = 993;
    acc.incoming_protocol = "imap";
    acc.outgoing_server = "smtp.mail.yahoo.com";
    acc.outgoing_port = 587;
    acc.outgoing_protocol = "smtp";
    acc.username = email_addr;
  } else {
    acc.id = "account_" + (user_prefix.empty() ? "custom" : user_prefix);
    acc.provider = "custom";
    acc.display_name = disp_name.empty() ? "Mail (" + email_addr + ")" : disp_name;
    acc.incoming_server = (at_pos != std::string::npos) ? "imap." + email_addr.substr(at_pos + 1) : "imap.example.com";
    acc.incoming_port = 993;
    acc.incoming_protocol = "imap";
    acc.outgoing_server = (at_pos != std::string::npos) ? "smtp." + email_addr.substr(at_pos + 1) : "smtp.example.com";
    acc.outgoing_port = 587;
    acc.outgoing_protocol = "smtp";
    acc.username = email_addr;
  }

  return acc;
}

// ============================================================================
// MailStore Initialization & Storage Management
// ============================================================================

MailStore::MailStore(const std::string& root_dir, hal::Host* host, const std::string& kernel_root) : root_dir_(root_dir) {
  set_host(host, kernel_root);
  reload();
}

void MailStore::ensure_store_hierarchy() {
  if (root_dir_.empty()) return;
  ensure_dir(root_dir_);
  ensure_dir(root_dir_ + "/store");
  ensure_dir(root_dir_ + "/store/tmp");
  for (const auto& acc : accounts_) {
    ensure_dir(root_dir_ + "/store/" + acc.id);
    ensure_dir(root_dir_ + "/store/" + acc.id + "/inbox");
    ensure_dir(root_dir_ + "/store/" + acc.id + "/sent");
    ensure_dir(root_dir_ + "/store/" + acc.id + "/drafts");
    ensure_dir(root_dir_ + "/store/" + acc.id + "/archive");
    ensure_dir(root_dir_ + "/store/" + acc.id + "/trash");
  }
}

void MailStore::write_eml_file(const MailMessage& m) {
  if (root_dir_.empty()) return;
  ensure_store_hierarchy();
  std::string acc = m.account.empty() ? "aios_core" : m.account;
  std::string fld = m.folder.empty() ? "inbox" : m.folder;
  std::string fld_dir = root_dir_ + "/store/" + acc + "/" + fld;
  ensure_dir(fld_dir);

  std::string tmp_path = root_dir_ + "/store/tmp/" + m.id + ".eml.tmp";
  std::string target_path = fld_dir + "/" + m.id + ".eml";

  std::string raw = serialize_rfc5322(m);
  std::ofstream out(tmp_path, std::ios::binary);
  if (out.is_open()) {
    out.write(raw.data(), (std::streamsize)raw.size());
    out.close();
    ::rename(tmp_path.c_str(), target_path.c_str());
  }
}

bool MailStore::read_eml_file(const std::string& path, MailMessage& m) const {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) return false;
  std::stringstream ss;
  ss << in.rdbuf();
  return parse_rfc5322(ss.str(), m);
}

void MailStore::save_envelope_index() {
  if (root_dir_.empty()) return;
  std::string db_path = envelope_db_path();
  std::string tmp_path = db_path + ".tmp";

  std::ofstream out(tmp_path, std::ios::binary);
  if (!out.is_open()) return;

  out << "# AE_ENVELOPE_DB_V4\n";
  out << "# ID\tMessage-ID\tFolder\tAccount\tFromName\tFromAddr\tTo\tCc\tBcc\tSubject\tDateStr\tTimestamp\tSnippet\tUnread\tFlagged\tDraft\tEMLPath\tAttachments\tUID\tHasHTML\tRawEML\n";

  for (const auto& m : messages_) {
    std::string to_s, cc_s, bcc_s, att_s;
    for (size_t i = 0; i < m.to.size(); ++i) {
      if (i > 0) to_s += ";";
      to_s += m.to[i];
    }
    for (size_t i = 0; i < m.cc.size(); ++i) {
      if (i > 0) cc_s += ";";
      cc_s += m.cc[i];
    }
    for (size_t i = 0; i < m.bcc.size(); ++i) {
      if (i > 0) bcc_s += ";";
      bcc_s += m.bcc[i];
    }
    for (size_t i = 0; i < m.attachments.size(); ++i) {
      if (i > 0) att_s += ";";
      att_s += m.attachments[i].filename + ":" + std::to_string(m.attachments[i].size_bytes) + ":" + m.attachments[i].mime_type;
    }

    std::string eml = m.eml_path;
    if (eml.empty()) {
      eml = "store/" + (m.account.empty() ? "aios_core" : m.account) + "/" + (m.folder.empty() ? "inbox" : m.folder) + "/" + m.id + ".eml";
    }

    out << escape_tab_field(m.id) << "\t"
        << escape_tab_field(m.message_id) << "\t"
        << escape_tab_field(m.folder) << "\t"
        << escape_tab_field(m.account) << "\t"
        << escape_tab_field(m.from_name) << "\t"
        << escape_tab_field(m.from_addr) << "\t"
        << escape_tab_field(to_s) << "\t"
        << escape_tab_field(cc_s) << "\t"
        << escape_tab_field(bcc_s) << "\t"
        << escape_tab_field(m.subject) << "\t"
        << escape_tab_field(m.date_str) << "\t"
        << m.timestamp << "\t"
        << escape_tab_field(m.snippet) << "\t"
        << (m.is_unread ? "1" : "0") << "\t"
        << (m.is_flagged ? "1" : "0") << "\t"
        << (m.is_draft ? "1" : "0") << "\t"
        << escape_tab_field(eml) << "\t"
        << escape_tab_field(att_s) << "\t"
        << m.uid << "\t"
        << (m.has_html ? "1" : "0") << "\t"
        << (m.raw_on_disk ? "1" : "0") << "\n";
  }

  out.close();
  ::rename(tmp_path.c_str(), db_path.c_str());
}

bool MailStore::load_envelope_index() {
  std::string db_path = envelope_db_path();
  std::ifstream in(db_path);
  if (!in.is_open()) return false;

  std::string line;
  messages_.clear();

  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    auto cols = split_tab_line(line);
    if (cols.size() < 17) continue;

    MailMessage m;
    m.id = cols[0];
    m.message_id = cols[1];
    m.folder = cols[2];
    m.account = cols[3];
    m.from_name = cols[4];
    m.from_addr = cols[5];

    // Split To
    if (!cols[6].empty()) {
      std::string to_s = cols[6];
      size_t st = 0;
      while (st < to_s.size()) {
        size_t semi = to_s.find(';', st);
        if (semi == std::string::npos) {
          m.to.push_back(to_s.substr(st));
          break;
        }
        m.to.push_back(to_s.substr(st, semi - st));
        st = semi + 1;
      }
    }

    // Split Cc
    if (!cols[7].empty()) {
      std::string cc_s = cols[7];
      size_t st = 0;
      while (st < cc_s.size()) {
        size_t semi = cc_s.find(';', st);
        if (semi == std::string::npos) {
          m.cc.push_back(cc_s.substr(st));
          break;
        }
        m.cc.push_back(cc_s.substr(st, semi - st));
        st = semi + 1;
      }
    }

    // Split Bcc
    if (!cols[8].empty()) {
      std::string bcc_s = cols[8];
      size_t st = 0;
      while (st < bcc_s.size()) {
        size_t semi = bcc_s.find(';', st);
        if (semi == std::string::npos) {
          m.bcc.push_back(bcc_s.substr(st));
          break;
        }
        m.bcc.push_back(bcc_s.substr(st, semi - st));
        st = semi + 1;
      }
    }

    m.subject = cols[9];
    m.date_str = cols[10];
    m.timestamp = parse_num<uint64_t>(cols[11], 0);
    m.snippet = cols[12];
    m.is_unread = (cols[13] == "1");
    m.is_flagged = (cols[14] == "1");
    m.is_draft = (cols[15] == "1");
    m.eml_path = cols[16];

    // V3 appended the IMAP UID. A V2 row reads back with uid 0, which simply
    // means "we do not know where this lives on the server" -- it cannot be
    // re-fetched by uid until the next sync matches it up by Message-ID.
    if (cols.size() >= 19) m.uid = (uint32_t)parse_num<uint64_t>(cols[18], 0);
    // V4 records whether the sender's own html is on disk beside the message,
    // and whether the .eml is the server's bytes rather than our own writing.
    if (cols.size() >= 20) m.has_html = (cols[19] == "1");
    if (cols.size() >= 21) m.raw_on_disk = (cols[20] == "1");

    // Split attachments
    if (cols.size() >= 18 && !cols[17].empty()) {
      std::string att_s = cols[17];
      size_t st = 0;
      while (st < att_s.size()) {
        size_t semi = att_s.find(';', st);
        std::string single = (semi == std::string::npos) ? att_s.substr(st) : att_s.substr(st, semi - st);
        size_t c1 = single.find(':');
        size_t c2 = (c1 != std::string::npos) ? single.find(':', c1 + 1) : std::string::npos;
        if (c1 != std::string::npos && c2 != std::string::npos) {
          MailAttachment att;
          att.filename = single.substr(0, c1);
          att.size_bytes = parse_num<uint64_t>(single.substr(c1 + 1, c2 - c1 - 1), 0);
          att.mime_type = single.substr(c2 + 1);
          m.attachments.push_back(att);
        }
        if (semi == std::string::npos) break;
        st = semi + 1;
      }
    }

    // The body stays on disk until something actually wants to read it.
    // Loading every .eml here cost one open+parse per message and kept every
    // body resident: a year of mail (12.6k messages) took 1.55s and 476 MB to
    // print a status line, and the full 49.5k inbox would not fit the budget at
    // all. ensure_body() pulls it in on demand; the snippet in the index is what
    // the message list draws.
    messages_.push_back(m);
  }

  uint64_t max_id = 1000;
  for (const auto& m : messages_) {
    if (m.id.rfind("msg-", 0) == 0) {
      uint64_t num = parse_num<uint64_t>(m.id.substr(4), 0);
      if (num >= max_id) max_id = num + 1;
    }
  }
  next_id_num_ = max_id;

  return !messages_.empty();
}

void MailStore::save_accounts_and_vault() {
  if (root_dir_.empty()) return;
  std::string idx_path = accounts_idx_path();
  std::string vault_path = keyring_vault_path();
  std::string tmp_idx = idx_path + ".tmp";
  std::string tmp_vault = vault_path + ".tmp";

  // 1. Write Accounts Index (non-sensitive server info)
  {
    std::ofstream out(tmp_idx);
    if (out.is_open()) {
      out << "# AE_ACCOUNTS_IDX_V3\n";
      out << "# ID\tDisplayName\tEmail\tProvider\tIncomingServer\tIncomingPort\tIncomingProto\tOutgoingServer\tOutgoingPort\tOutgoingProto\tUseTLS\tUsername\tIsDefault\tOnline\tSyncStatus\tLastSyncAt\tAuth\n";
      for (const auto& a : accounts_) {
        out << escape_tab_field(a.id) << "\t"
            << escape_tab_field(a.display_name) << "\t"
            << escape_tab_field(a.email) << "\t"
            << escape_tab_field(a.provider) << "\t"
            << escape_tab_field(a.incoming_server) << "\t"
            << a.incoming_port << "\t"
            << escape_tab_field(a.incoming_protocol) << "\t"
            << escape_tab_field(a.outgoing_server) << "\t"
            << a.outgoing_port << "\t"
            << escape_tab_field(a.outgoing_protocol) << "\t"
            << (a.use_tls ? "1" : "0") << "\t"
            << escape_tab_field(a.username) << "\t"
            << (a.is_default ? "1" : "0") << "\t"
            << (a.online ? "1" : "0") << "\t"
            << escape_tab_field(a.last_sync_status) << "\t"
            << a.last_sync_at << "\t"
            << escape_tab_field(a.auth) << "\n";
      }
      out.close();
      ::rename(tmp_idx.c_str(), idx_path.c_str());
    }
  }

  // 2. Write Encrypted Keyring Vault (RFC 8439 ChaCha20-Poly1305 AEAD)
  {
    std::ostringstream creds_ss;
    for (const auto& a : accounts_) {
      if (!a.password.empty()) {
        creds_ss << escape_tab_field(a.id) << "\t" << escape_tab_field(a.password) << "\n";
      }
    }
    std::string creds_plain = creds_ss.str();

    // Key is real per-install randomness (see load_or_create_keyring_key), and
    // the nonce is fresh random bytes every write, stored alongside the
    // ciphertext -- a nonce isn't secret, it just can't repeat under a given
    // key, and a random 96-bit value satisfies that with overwhelming odds.
    crypto::Key key = load_or_create_keyring_key(root_dir_);
    crypto::Nonce nonce{};
    secure_random_bytes(nonce.data(), nonce.size());
    uint8_t aad_bytes[8] = {'A', 'E', '_', 'K', 'E', 'Y', 'R', '2'};

    ByteBuffer ct_and_tag = crypto::aead_encrypt(key, nonce, ByteSpan(aad_bytes, 8), span_of(creds_plain));

    std::ofstream vault_out(tmp_vault, std::ios::binary);
    if (vault_out.is_open()) {
      vault_out << "AE_KEYRING_VAULT_V2\n";
      vault_out.write(reinterpret_cast<const char*>(nonce.data()), (std::streamsize)nonce.size());
      uint32_t payload_len = (uint32_t)ct_and_tag.size();
      vault_out.write(reinterpret_cast<const char*>(&payload_len), sizeof(payload_len));
      vault_out.write(reinterpret_cast<const char*>(ct_and_tag.data()), (std::streamsize)ct_and_tag.size());
      vault_out.close();
      ::rename(tmp_vault.c_str(), vault_path.c_str());
    }
  }
}

bool MailStore::load_accounts_and_vault() {
  std::string idx_path = accounts_idx_path();
  std::string vault_path = keyring_vault_path();

  std::ifstream in(idx_path);
  if (!in.is_open()) return false;

  accounts_.clear();
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    auto cols = split_tab_line(line);
    if (cols.size() < 15) continue;

    MailAccount acc;
    acc.id = cols[0];
    acc.display_name = cols[1];
    acc.email = cols[2];
    acc.provider = cols[3];
    acc.incoming_server = cols[4];
    acc.incoming_port = (uint16_t)parse_num<uint32_t>(cols[5], 993);
    acc.incoming_protocol = cols[6];
    acc.outgoing_server = cols[7];
    acc.outgoing_port = (uint16_t)parse_num<uint32_t>(cols[8], 587);
    acc.outgoing_protocol = cols[9];
    acc.use_tls = (cols[10] == "1");
    acc.username = cols[11];
    acc.is_default = (cols[12] == "1");
    acc.online = (cols[13] == "1");
    acc.last_sync_status = cols[14];
    // V2 indexes have no LastSyncAt column; they read back as "never synced",
    // which is the honest answer for a file written before we tracked it.
    if (cols.size() >= 16) acc.last_sync_at = parse_num<uint64_t>(cols[15], 0);
    // Older indexes have no Auth column: those accounts log in with their password.
    if (cols.size() >= 17 && !cols[16].empty()) acc.auth = cols[16];

    accounts_.push_back(acc);
  }

  // Load and decrypt Keyring Vault
  auto apply_decrypted_credentials = [&](const std::string& plain) {
    std::istringstream pss(plain);
    std::string pline;
    while (std::getline(pss, pline)) {
      if (pline.empty()) continue;
      auto pcols = split_tab_line(pline);
      if (pcols.size() >= 2) {
        std::string acc_id = pcols[0];
        std::string pass = pcols[1];
        for (auto& a : accounts_) {
          if (a.id == acc_id) {
            a.password = pass;
            break;
          }
        }
      }
    }
  };

  std::ifstream vault_in(vault_path, std::ios::binary);
  if (vault_in.is_open()) {
    std::string header;
    std::getline(vault_in, header);
    if (header == "AE_KEYRING_VAULT_V2\r" || header == "AE_KEYRING_VAULT_V2") {
      crypto::Nonce nonce{};
      vault_in.read(reinterpret_cast<char*>(nonce.data()), (std::streamsize)nonce.size());
      uint32_t payload_len = 0;
      vault_in.read(reinterpret_cast<char*>(&payload_len), sizeof(payload_len));
      if (payload_len > 0 && payload_len < 1024 * 1024) {
        ByteBuffer ct_and_tag(payload_len);
        vault_in.read(reinterpret_cast<char*>(ct_and_tag.data()), payload_len);

        crypto::Key key = load_or_create_keyring_key(root_dir_);
        uint8_t aad_bytes[8] = {'A', 'E', '_', 'K', 'E', 'Y', 'R', '2'};

        auto pt_res = crypto::aead_decrypt(key, nonce, ByteSpan(aad_bytes, 8), ct_and_tag);
        if (pt_res.ok()) apply_decrypted_credentials(string_of(*pt_res));
      }
    } else if (header == "AE_KEYRING_VAULT_V1\r" || header == "AE_KEYRING_VAULT_V1") {
      // One-shot migration off the broken scheme (see legacy_v1_key/nonce):
      // decrypt with the old, still-known-to-anyone-with-source key, then
      // immediately rewrite under a real random key so this branch never
      // fires again for this install.
      uint32_t payload_len = 0;
      vault_in.read(reinterpret_cast<char*>(&payload_len), sizeof(payload_len));
      if (payload_len > 0 && payload_len < 1024 * 1024) {
        ByteBuffer ct_and_tag(payload_len);
        vault_in.read(reinterpret_cast<char*>(ct_and_tag.data()), payload_len);
        uint8_t aad_bytes[8] = {'A', 'E', '_', 'K', 'E', 'Y', 'R', '1'};

        auto pt_res = crypto::aead_decrypt(legacy_v1_key(), legacy_v1_nonce(), ByteSpan(aad_bytes, 8), ct_and_tag);
        if (pt_res.ok()) {
          apply_decrypted_credentials(string_of(*pt_res));
          save_accounts_and_vault();
        }
      }
    }
  }

  return !accounts_.empty();
}

void MailStore::migrate_legacy_json() {
  if (root_dir_.empty()) return;
  std::string acc_json = root_dir_ + "/accounts.json";
  std::string msg_json = root_dir_ + "/messages.json";

  bool had_legacy = false;

  // Migrate accounts.json if present
  std::ifstream f_acc(acc_json);
  if (f_acc.is_open()) {
    std::stringstream buf;
    buf << f_acc.rdbuf();
    f_acc.close();
    auto objs = split_json_objects(buf.str());
    if (!objs.empty()) {
      accounts_.clear();
      for (const auto& o : objs) {
        MailAccount acc;
        acc.id = json_get_str(o, "id", "acc");
        acc.display_name = json_get_str(o, "display_name", "Mail Account");
        acc.email = json_get_str(o, "email", "");
        acc.provider = json_get_str(o, "provider", "custom");
        acc.incoming_server = json_get_str(o, "incoming_server", "");
        acc.incoming_port = (uint16_t)json_get_int(o, "incoming_port", 993);
        acc.incoming_protocol = json_get_str(o, "incoming_protocol", "imap");
        acc.outgoing_server = json_get_str(o, "outgoing_server", "");
        acc.outgoing_port = (uint16_t)json_get_int(o, "outgoing_port", 587);
        acc.outgoing_protocol = json_get_str(o, "outgoing_protocol", "smtp");
        acc.use_tls = json_get_bool(o, "use_tls", true);
        acc.username = json_get_str(o, "username", "");
        acc.password = json_get_str(o, "password", "");
        acc.is_default = json_get_bool(o, "is_default", false);
        acc.online = json_get_bool(o, "online", true);
        acc.last_sync_status = json_get_str(o, "last_sync_status", "Ready");
        if (!acc.email.empty()) {
          accounts_.push_back(acc);
        }
      }
      had_legacy = true;
    }
    ::unlink(acc_json.c_str());
  }

  // Migrate messages.json if present
  std::ifstream f_msg(msg_json);
  if (f_msg.is_open()) {
    std::stringstream buf;
    buf << f_msg.rdbuf();
    f_msg.close();
    auto objs = split_json_objects(buf.str());
    if (!objs.empty()) {
      messages_.clear();
      for (const auto& o : objs) {
        MailMessage m;
        m.id = json_get_str(o, "id", "");
        m.message_id = json_get_str(o, "message_id", "");
        m.folder = json_get_str(o, "folder", "inbox");
        m.account = json_get_str(o, "account", "aios_core");
        m.from_name = json_get_str(o, "from_name", "");
        m.from_addr = json_get_str(o, "from_addr", "");
        m.to = json_get_array(o, "to");
        m.cc = json_get_array(o, "cc");
        m.bcc = json_get_array(o, "bcc");
        m.subject = json_get_str(o, "subject", "");
        m.date_str = json_get_str(o, "date_str", "");
        m.timestamp = (uint64_t)json_get_int(o, "timestamp", 0);
        m.snippet = json_get_str(o, "snippet", "");
        m.body = json_get_str(o, "body", "");
        m.is_unread = json_get_bool(o, "is_unread", false);
        m.is_flagged = json_get_bool(o, "is_flagged", false);
        m.is_draft = json_get_bool(o, "is_draft", false);
        if (!m.id.empty()) {
          messages_.push_back(m);
        }
      }
      had_legacy = true;
    }
    ::unlink(msg_json.c_str());
  }

  if (had_legacy) {
    save_to_disk();
  }
}

void MailStore::reload() {
  generation_++;
  if (!root_dir_.empty()) {
    load_from_disk();
  }
}

void MailStore::save() {
  if (!root_dir_.empty()) {
    save_to_disk();
  }
}

void MailStore::load_from_disk() {
  ensure_dir(root_dir_);
  migrate_legacy_json();

  bool acc_ok = load_accounts_and_vault();
  bool msg_ok = load_envelope_index();
  load_sync_state();

  if (acc_ok && msg_ok) {
    loaded_from_disk_ = true;
  }
}

void MailStore::save_to_disk() {
  generation_++;
  if (root_dir_.empty()) return;
  ensure_dir(root_dir_);
  ensure_store_hierarchy();

  // Write the .eml only for messages whose body is actually in hand. With
  // bodies loaded lazily, most messages carry an empty body that is merely
  // still on disk -- serializing those would truncate every one of their .eml
  // files to a headers-only stub and destroy the mail. It also keeps a save
  // proportional to what changed rather than to the size of the mailbox: this
  // loop used to rewrite every message's file on every mark-read.
  for (auto& m : messages_) {
    std::string path = "store/" + (m.account.empty() ? "aios_core" : m.account) + "/"
                     + (m.folder.empty() ? "inbox" : m.folder) + "/" + m.id + ".eml";
    // A message whose .eml is the server's own bytes is never rewritten: the
    // serialiser would replace the real MIME -- html part, attachments and all
    // -- with a flattened copy of the few fields the index keeps, which is how
    // the original got destroyed in the first place.
    if (m.raw_on_disk) {
      m.eml_path = path;
      continue;
    }
    if (!m.body.empty()) {
      write_eml_file(m);                       // body in hand: authoritative
    } else if (!file_exists(root_dir_ + "/" + path)) {
      // No body loaded and nothing at the target. If this message just moved
      // folders its bytes are still at the old path -- move the file rather
      // than writing a stub over a body we never read.
      std::string old_full = m.eml_path.empty() ? std::string() : root_dir_ + "/" + m.eml_path;
      if (!old_full.empty() && m.eml_path != path && file_exists(old_full)) {
        ensure_dir(root_dir_ + "/store/" + (m.account.empty() ? "aios_core" : m.account) + "/"
                   + (m.folder.empty() ? "inbox" : m.folder));
        ::rename(old_full.c_str(), (root_dir_ + "/" + path).c_str());
      }
      // Otherwise write NOTHING. A message we hold only an envelope for has no
      // file at all, and that absence is the signal ensure_body/fetch_body act
      // on. Writing a headers-only stub instead cost a 4KB filesystem block
      // each: a 71.7k backfill produced 61.7k stubs holding 48MB of real data
      // in 293MB of disk, and made every evicted body look present.
    }
    m.eml_path = path;
  }

  // Save envelope database & encrypted keyring vault
  save_accounts_and_vault();
  save_envelope_index();
}


// ============================================================================
// Message Queries & Mailbox Operations
// ============================================================================

std::vector<MailFolder> MailStore::get_folders(const std::string& account_id) const {
  std::vector<MailFolder> folders;

  auto add_f = [&](const std::string& id, const std::string& name, const std::string& icon) {
    MailFolder f;
    f.id = id;
    f.name = name;
    f.icon_name = icon;
    f.unread_count = folder_unread(id, account_id);
    f.total_count = 0;
    for (const auto& m : messages_) {
      if (account_id != "all" && !account_id.empty()) {
        if (m.account != account_id) continue;
      }
      if (id == "flagged") {
        if (m.is_flagged && m.folder != "trash") f.total_count++;
      } else if (m.folder == id) {
        f.total_count++;
      }
    }
    folders.push_back(f);
  };

  add_f("inbox", "Inbox", "inbox");
  add_f("flagged", "Flagged", "flag");
  add_f("drafts", "Drafts", "draft");
  add_f("sent", "Sent", "sent");
  add_f("archive", "Archive", "archive");
  add_f("trash", "Trash", "trash");

  return folders;
}

MailSearchQuery MailSearchQuery::parse(const std::string& query) {
  MailSearchQuery q;
  q.raw = query;

  std::vector<std::string> tokens;
  std::string cur;
  bool in_quote = false;

  for (size_t i = 0; i < query.size(); ++i) {
    char c = query[i];
    if (c == '"') {
      in_quote = !in_quote;
      cur += c;
    } else if (std::isspace(static_cast<unsigned char>(c)) && !in_quote) {
      if (!cur.empty()) {
        tokens.push_back(cur);
        cur.clear();
      }
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) tokens.push_back(cur);

  auto strip_quotes = [](const std::string& s) -> std::string {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
      return s.substr(1, s.size() - 2);
    }
    return s;
  };

  for (const auto& tok : tokens) {
    if (tok.empty()) continue;
    std::string lower_tok = tok;
    for (char& c : lower_tok) c = std::tolower(static_cast<unsigned char>(c));

    if (lower_tok.rfind("from:", 0) == 0) {
      std::string val = strip_quotes(tok.substr(5));
      if (!val.empty()) q.from_terms.push_back(val);
    } else if (lower_tok.rfind("to:", 0) == 0) {
      std::string val = strip_quotes(tok.substr(3));
      if (!val.empty()) q.to_terms.push_back(val);
    } else if (lower_tok.rfind("subject:", 0) == 0) {
      std::string val = strip_quotes(tok.substr(8));
      if (!val.empty()) q.subject_terms.push_back(val);
    } else if (lower_tok.rfind("subj:", 0) == 0) {
      std::string val = strip_quotes(tok.substr(5));
      if (!val.empty()) q.subject_terms.push_back(val);
    } else if (lower_tok.rfind("body:", 0) == 0) {
      std::string val = strip_quotes(tok.substr(5));
      if (!val.empty()) q.body_terms.push_back(val);
    } else if (lower_tok.rfind("in:", 0) == 0) {
      q.folder = strip_quotes(lower_tok.substr(3));
    } else if (lower_tok.rfind("folder:", 0) == 0) {
      q.folder = strip_quotes(lower_tok.substr(7));
    } else if (lower_tok.rfind("account:", 0) == 0) {
      q.account = strip_quotes(tok.substr(8));
    } else if (lower_tok.rfind("acc:", 0) == 0) {
      q.account = strip_quotes(tok.substr(4));
    } else if (lower_tok == "has:attachment" || lower_tok == "has:attach" || lower_tok == "has:file" || lower_tok == "has:files") {
      q.has_attachment = 1;
    } else if (lower_tok == "has:noattachment" || lower_tok == "-has:attachment" || lower_tok == "-has:attach" || lower_tok == "-has:file") {
      q.has_attachment = 0;
    } else if (lower_tok == "is:unread") {
      q.is_unread = 1;
    } else if (lower_tok == "is:read") {
      q.is_unread = 0;
    } else if (lower_tok == "is:flagged" || lower_tok == "is:starred" || lower_tok == "is:star" || lower_tok == "is:flag") {
      q.is_flagged = 1;
    } else if (lower_tok == "is:unflagged" || lower_tok == "is:unstarred") {
      q.is_flagged = 0;
    } else {
      std::string val = strip_quotes(tok);
      if (!val.empty()) q.any_terms.push_back(val);
    }
  }

  return q;
}

bool MailSearchQuery::is_empty() const {
  return any_terms.empty() && from_terms.empty() && to_terms.empty() &&
         subject_terms.empty() && body_terms.empty() &&
         has_attachment == -1 && is_unread == -1 && is_flagged == -1 &&
         folder.empty() && account.empty();
}

bool MailSearchQuery::matches(const MailMessage& m) const {
  if (is_unread == 1 && !m.is_unread) return false;
  if (is_unread == 0 && m.is_unread) return false;
  if (is_flagged == 1 && !m.is_flagged) return false;
  if (is_flagged == 0 && m.is_flagged) return false;
  if (has_attachment == 1 && m.attachments.empty()) return false;
  if (has_attachment == 0 && !m.attachments.empty()) return false;

  for (const auto& t : from_terms) {
    if (!str_contains_ci(m.from_name, t) && !str_contains_ci(m.from_addr, t)) return false;
  }
  for (const auto& t : to_terms) {
    bool matched = false;
    for (const auto& addr : m.to) { if (str_contains_ci(addr, t)) { matched = true; break; } }
    if (!matched) {
      for (const auto& addr : m.cc) { if (str_contains_ci(addr, t)) { matched = true; break; } }
    }
    if (!matched) {
      for (const auto& addr : m.bcc) { if (str_contains_ci(addr, t)) { matched = true; break; } }
    }
    if (!matched) return false;
  }
  for (const auto& t : subject_terms) {
    if (!str_contains_ci(m.subject, t)) return false;
  }
  for (const auto& t : body_terms) {
    if (!str_contains_ci(m.body, t) && !str_contains_ci(m.snippet, t)) return false;
  }
  for (const auto& t : any_terms) {
    bool matched = str_contains_ci(m.subject, t) ||
                   str_contains_ci(m.from_name, t) ||
                   str_contains_ci(m.from_addr, t) ||
                   str_contains_ci(m.snippet, t);
    if (!matched) {
      for (const auto& addr : m.to) { if (str_contains_ci(addr, t)) { matched = true; break; } }
    }
    if (!matched) {
      for (const auto& addr : m.cc) { if (str_contains_ci(addr, t)) { matched = true; break; } }
    }
    if (!matched) {
      for (const auto& att : m.attachments) { if (str_contains_ci(att.filename, t)) { matched = true; break; } }
    }
    if (!matched) return false;
  }
  return true;
}

std::vector<MailMessage> MailStore::get_messages(const std::string& folder_id,
                                                const std::string& search_query,
                                                int filter_mode,
                                                const std::string& account_id) const {
  MailSearchQuery q = MailSearchQuery::parse(search_query);
  std::vector<MailMessage> res;
  for (const auto& m : messages_) {
    // Account match
    std::string eff_acc = (!q.account.empty()) ? q.account : account_id;
    if (eff_acc != "all" && !eff_acc.empty()) {
      if (m.account != eff_acc && !str_contains_ci(m.account, eff_acc)) continue;
    }

    // Folder match
    std::string eff_folder = (!q.folder.empty()) ? q.folder : folder_id;
    if (eff_folder == "flagged") {
      if (!m.is_flagged || m.folder == "trash") continue;
    } else if (eff_folder == "all" || (!q.folder.empty() && q.folder == "all")) {
      if (m.folder == "trash" && q.folder != "trash" && folder_id != "trash") continue;
    } else if (!eff_folder.empty()) {
      if (m.folder != eff_folder) continue;
    }

    // Filter mode (0: All, 1: Unread only, 2: Flagged only, 3: Has Attachment)
    if (filter_mode == 1 && !m.is_unread) continue;
    if (filter_mode == 2 && !m.is_flagged) continue;
    if (filter_mode == 3 && m.attachments.empty()) continue;

    // Search query match. Only a query with an explicit body: term pays for
    // reading bodies off disk, and only for the messages that got this far --
    // the account, folder and filter tests have already thinned the set.
    if (!q.is_empty()) {
      if (q.needs_body()) ensure_body(m);
      if (!q.matches(m)) continue;
    }

    res.push_back(m);
  }

  // Sort descending by timestamp
  std::sort(res.begin(), res.end(), [](const MailMessage& a, const MailMessage& b) {
    return a.timestamp > b.timestamp;
  });

  return res;
}

// Pull a message's body in off disk if it is not already here. Cheap to call
// repeatedly: it is a no-op once the body is loaded. Const because the body is
// a cache of what the .eml already holds, not a change to the mailbox.
void MailStore::ensure_body(const MailMessage& cm) const {
  MailMessage& m = const_cast<MailMessage&>(cm);
  if (!m.body.empty() || m.eml_path.empty() || root_dir_.empty()) return;

  // For a raw message the .eml is the server's MIME and parse_rfc5322 cannot
  // read it -- the display text sits beside it, extracted at fetch time.
  std::string txt = sidecar_path(m, ".txt");
  if (!txt.empty() && file_exists(txt)) {
    std::ifstream in(txt, std::ios::binary);
    std::stringstream buf;
    buf << in.rdbuf();
    m.body = buf.str();
    if (!m.body.empty()) return;
  }

  MailMessage eml_msg;
  if (read_eml_file(root_dir_ + "/" + m.eml_path, eml_msg)) {
    m.body = eml_msg.body;
    if (m.attachments.empty()) m.attachments = eml_msg.attachments;
  }
}

// The .html / .txt that sit beside a message's .eml, by extension.
std::string MailStore::sidecar_path(const MailMessage& m, const std::string& ext) const {
  if (m.eml_path.empty() || root_dir_.empty()) return std::string();
  std::string p = root_dir_ + "/" + m.eml_path;
  size_t dot = p.rfind(".eml");
  if (dot == std::string::npos) return std::string();
  return p.substr(0, dot) + ext;
}

std::string MailStore::html_body(const MailMessage& m) const {
  if (!m.has_html) return std::string();
  std::string path = sidecar_path(m, ".html");
  if (path.empty() || !file_exists(path)) return std::string();
  std::ifstream in(path, std::ios::binary);
  std::stringstream buf;
  buf << in.rdbuf();
  return buf.str();
}

const MailMessage* MailStore::get_message(const std::string& id) const {
  for (const auto& m : messages_) {
    if (m.id == id) { ensure_body(m); return &m; }
  }
  return nullptr;
}

MailMessage* MailStore::get_message_mut(const std::string& id) {
  for (auto& m : messages_) {
    if (m.id == id) { ensure_body(m); return &m; }
  }
  return nullptr;
}

bool MailStore::mark_read(const std::string& id, bool read) {
  MailMessage* m = get_message_mut(id);
  if (!m) return false;
  m->is_unread = !read;
  save_to_disk();
  return true;
}

bool MailStore::toggle_flag(const std::string& id) {
  MailMessage* m = get_message_mut(id);
  if (!m) return false;
  m->is_flagged = !m->is_flagged;
  save_to_disk();
  return true;
}

bool MailStore::move_to_folder(const std::string& id, const std::string& target_folder) {
  MailMessage* m = get_message_mut(id);
  if (!m) return false;
  m->folder = target_folder;
  save_to_disk();
  return true;
}

bool MailStore::delete_message(const std::string& id) {
  for (size_t i = 0; i < messages_.size(); ++i) {
    if (messages_[i].id == id) {
      if (messages_[i].folder == "trash") {
        if (!root_dir_.empty() && !messages_[i].eml_path.empty()) {
          ::unlink((root_dir_ + "/" + messages_[i].eml_path).c_str());
        }
        messages_.erase(messages_.begin() + (long)i);
      } else {
        messages_[i].folder = "trash";
      }
      save_to_disk();
      return true;
    }
  }
  return false;
}

bool MailStore::archive_message(const std::string& id) {
  return move_to_folder(id, "archive");
}

std::string MailStore::send_message(const std::string& from_addr,
                                    const std::vector<std::string>& to_addrs,
                                    const std::vector<std::string>& cc_addrs,
                                    const std::string& subject,
                                    const std::string& body,
                                    const std::vector<MailAttachment>& attachments,
                                    const std::string& account_id) {
  const MailAccount& acc = !account_id.empty() ? (get_account(account_id) ? *get_account(account_id) : primary_account()) : primary_account();

  MailMessage m;
  m.id = "msg-" + std::to_string(next_id_num_++);
  m.message_id = "<" + m.id + "@" + (acc.email.find('@') != std::string::npos ? acc.email.substr(acc.email.find('@') + 1) : "aios.org") + ">";
  m.folder = "sent";
  m.account = acc.id;
  m.from_name = acc.display_name;
  m.from_addr = from_addr.empty() ? acc.email : from_addr;
  m.to = to_addrs;
  m.cc = cc_addrs;
  m.subject = subject.empty() ? "(No Subject)" : subject;
  m.date_str = format_time_now();
  m.timestamp = (uint64_t)std::time(nullptr);
  m.body = body;
  m.snippet = body.substr(0, 100);
  m.is_unread = false;
  m.is_flagged = false;
  m.is_draft = false;
  m.attachments = attachments;

  messages_.push_back(m);
  save_to_disk();

  transient_status_ = deliver(acc, m);

  return m.id;
}

std::string MailStore::save_draft(const std::string& draft_id,
                                  const std::string& to_str,
                                  const std::string& cc_str,
                                  const std::string& subject,
                                  const std::string& body,
                                  const std::string& account_id) {
  const MailAccount& acc = !account_id.empty() ? (get_account(account_id) ? *get_account(account_id) : primary_account()) : primary_account();

  MailMessage* existing = !draft_id.empty() ? get_message_mut(draft_id) : nullptr;
  if (existing) {
    existing->to = {to_str};
    if (!cc_str.empty()) existing->cc = {cc_str};
    existing->subject = subject;
    existing->body = body;
    existing->snippet = body.substr(0, 100);
    existing->date_str = format_time_now();
    existing->timestamp = (uint64_t)std::time(nullptr);
    transient_status_ = "Draft saved";
    save_to_disk();
    return existing->id;
  }

  MailMessage m;
  m.id = "msg-" + std::to_string(next_id_num_++);
  m.message_id = "<draft-" + m.id + "@aios.org>";
  m.folder = "drafts";
  m.account = acc.id;
  m.from_name = acc.display_name;
  m.from_addr = acc.email;
  m.to = {to_str};
  if (!cc_str.empty()) m.cc = {cc_str};
  m.subject = subject.empty() ? "Draft" : subject;
  m.date_str = format_time_now();
  m.timestamp = (uint64_t)std::time(nullptr);
  m.body = body;
  m.snippet = body.substr(0, 100);
  m.is_unread = false;
  m.is_flagged = false;
  m.is_draft = true;

  messages_.push_back(m);
  transient_status_ = "Draft saved";
  save_to_disk();
  return m.id;
}

size_t MailStore::total_unread(const std::string& account_id) const {
  // The status line asks for this every frame, and it walks the whole mailbox.
  // Memoise the common "all" case against the generation; a per-account count
  // is rare enough to just compute.
  const bool all = (account_id == "all" || account_id.empty());
  if (all && unread_cache_generation_ == generation_) return unread_cache_;

  size_t count = 0;
  for (const auto& m : messages_) {
    if (!all && m.account != account_id) continue;
    if (m.is_unread && m.folder != "trash") count++;
  }
  if (all) {
    unread_cache_generation_ = generation_;
    unread_cache_ = count;
  }
  return count;
}

size_t MailStore::folder_unread(const std::string& folder_id, const std::string& account_id) const {
  size_t count = 0;
  for (const auto& m : messages_) {
    if (account_id != "all" && !account_id.empty() && m.account != account_id) continue;
    if (folder_id == "flagged") {
      if (m.is_flagged && m.is_unread && m.folder != "trash") count++;
    } else if (m.folder == folder_id && m.is_unread) {
      count++;
    }
  }
  return count;
}

// ============================================================================
// Account Management
// ============================================================================

const MailAccount& MailStore::primary_account() const {
  for (const auto& acc : accounts_) {
    if (acc.is_default) return acc;
  }
  if (!accounts_.empty()) return accounts_.front();
  static MailAccount fallback;
  return fallback;
}

const MailAccount* MailStore::get_account(const std::string& id_or_email) const {
  for (const auto& acc : accounts_) {
    if (acc.id == id_or_email || acc.email == id_or_email) return &acc;
  }
  return nullptr;
}

MailAccount* MailStore::get_account_mut(const std::string& id_or_email) {
  for (auto& acc : accounts_) {
    if (acc.id == id_or_email || acc.email == id_or_email) return &acc;
  }
  return nullptr;
}

bool MailStore::add_account(const MailAccount& acc) {
  for (auto& existing : accounts_) {
    if (existing.id == acc.id || existing.email == acc.email) {
      existing = acc;
      save_to_disk();
      return true;
    }
  }
  accounts_.push_back(acc);
  if (accounts_.size() == 1 || acc.is_default) {
    set_default_account(acc.id);
  }
  save_to_disk();
  return true;
}

bool MailStore::update_account(const MailAccount& acc) {
  for (auto& existing : accounts_) {
    if (existing.id == acc.id) {
      existing = acc;
      save_to_disk();
      return true;
    }
  }
  return false;
}

bool MailStore::remove_account(const std::string& id_or_email) {
  for (size_t i = 0; i < accounts_.size(); ++i) {
    if (accounts_[i].id == id_or_email || accounts_[i].email == id_or_email) {
      bool was_default = accounts_[i].is_default;
      accounts_.erase(accounts_.begin() + (long)i);
      if (was_default && !accounts_.empty()) {
        accounts_.front().is_default = true;
      }
      save_to_disk();
      return true;
    }
  }
  return false;
}

bool MailStore::set_default_account(const std::string& id_or_email) {
  bool found = false;
  for (auto& acc : accounts_) {
    if (acc.id == id_or_email || acc.email == id_or_email) {
      acc.is_default = true;
      found = true;
    } else {
      acc.is_default = false;
    }
  }
  if (found) save_to_disk();
  return found;
}

// ============================================================================
// Network Diagnostics & Mail Syncing
// ============================================================================


size_t MailStore::merge_records(const MailAccount& acc,
                                const std::string& folder,
                                const std::vector<MailFetchRecord>& records,
                                const std::string& raw_dir) {
  // One pass to index what we already hold for this (account, folder), so each incoming record is an O(1)
  // lookup rather than a walk of the whole mailbox.
  std::unordered_map<uint32_t, size_t> by_uid;
  std::unordered_map<std::string, size_t> by_mid;
  for (size_t i = 0; i < messages_.size(); ++i) {
    const MailMessage& m = messages_[i];
    if (m.account != acc.id || m.folder != folder) continue;
    if (m.uid) by_uid.emplace(m.uid, i);
    if (!m.message_id.empty()) by_mid.emplace(m.message_id, i);
  }

  size_t added = 0;
  for (const auto& o : records) {
    // A record without the fields a message has is skipped.
    if (o.message_id.empty() && o.subject.empty() && o.uid == 0) continue;

    // UID first: it is the server's own identity for this message in this mailbox, and unlike Message-ID
    // it is guaranteed present and unique. Message-ID is the fallback for rows written before uids were
    // stored. Both come from the index built above -- matching by scanning messages_ for every record
    // made a 71.7k backfill quadratic.
    size_t found = SIZE_MAX;
    if (o.uid) {
      auto it = by_uid.find(o.uid);
      if (it != by_uid.end()) found = it->second;
    }
    if (found == SIZE_MAX && !o.message_id.empty()) {
      auto it = by_mid.find(o.message_id);
      if (it != by_mid.end()) found = it->second;
    }
    bool existing = (found != SIZE_MAX);

    MailMessage fresh;
    MailMessage& m = existing ? messages_[found] : fresh;
    if (!existing) {
      m.id = "msg-" + std::to_string(next_id_num_++);
      m.folder = folder;
      m.account = acc.id;
      m.message_id = o.message_id;
    }
    if (o.uid) m.uid = o.uid;
    if (!o.from_name.empty() || !m.from_name.empty()) m.from_name = o.from_name.empty() ? m.from_name : o.from_name;
    if (!o.from_addr.empty() || !m.from_addr.empty()) m.from_addr = o.from_addr.empty() ? m.from_addr : o.from_addr;
    if (!o.subject.empty()) m.subject = o.subject;
    if (!o.date_str.empty()) m.date_str = o.date_str;
    if (o.timestamp) m.timestamp = o.timestamp;
    m.is_unread = o.is_unread;
    m.is_flagged = o.is_flagged;
    if (o.has_html) m.has_html = true;

    // A headers-only record carries no body and an empty snippet. It must not blank out a snippet we
    // already have, nor pretend the body is empty -- that would be indistinguishable from a body of zero
    // length and the .eml writer would truncate the file.
    if (o.have_body) {
      m.body = o.body;
      if (!o.snippet.empty()) m.snippet = o.snippet;
      m.attachments = o.attachments;
    }
    if (!o.to.empty()) { m.to.clear(); m.to.push_back(o.to); }
    if (!o.cc.empty()) { m.cc.clear(); m.cc.push_back(o.cc); }

    // Adopt the raw message and its html part, which the worker wrote under the uid because message ids
    // are assigned here. The .eml is the ORIGINAL -- headers, MIME structure, attachments -- not a
    // re-serialisation of the fields the index happens to keep.
    if (m.uid && !raw_dir.empty()) {
      std::string acc_dir = root_dir_ + "/store/" + acc.id + "/" + folder;
      ensure_dir(acc_dir);
      std::string from_eml = raw_dir + "/" + std::to_string(m.uid) + ".eml";
      if (file_exists(from_eml)) {
        ::rename(from_eml.c_str(), (acc_dir + "/" + m.id + ".eml").c_str());
        m.eml_path = "store/" + acc.id + "/" + folder + "/" + m.id + ".eml";
        m.raw_on_disk = true;
      }
      std::string from_html = raw_dir + "/" + std::to_string(m.uid) + ".html";
      if (file_exists(from_html)) {
        ::rename(from_html.c_str(), (acc_dir + "/" + m.id + ".html").c_str());
        m.has_html = true;
      }
      // The display text beside them: the raw MIME cannot be read back by parse_rfc5322 (multipart,
      // base64, quoted-printable), and the text was already extracted properly, so keep it.
      if (m.raw_on_disk && !m.body.empty()) {
        std::ofstream tf(acc_dir + "/" + m.id + ".txt", std::ios::binary);
        if (tf.is_open()) tf.write(m.body.data(), (std::streamsize)m.body.size());
      }
    }

    if (!existing) {
      if (m.uid) by_uid.emplace(m.uid, messages_.size());
      if (!m.message_id.empty()) by_mid.emplace(m.message_id, messages_.size());
      messages_.push_back(m);
      added++;
    }
  }
  return added;
}

// ---------------------------------------------------------------------------
// Sync state: where each (account, folder) got to on the server
// ---------------------------------------------------------------------------

void MailStore::load_sync_state() {
  sync_state_.clear();
  std::ifstream in(root_dir_ + "/sync.state");
  if (!in.is_open()) return;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    auto cols = split_tab_line(line);
    if (cols.size() < 4) continue;
    MailFolderSync st;
    st.account_id = cols[0];
    st.folder = cols[1];
    st.uidvalidity = parse_num<uint64_t>(cols[2], 0);
    st.last_uid = (uint32_t)parse_num<uint64_t>(cols[3], 0);
    if (cols.size() >= 5) st.backfilled = (cols[4] == "1");
    sync_state_.push_back(st);
  }
}

void MailStore::save_sync_state() const {
  if (root_dir_.empty()) return;
  std::string path = root_dir_ + "/sync.state";
  std::string tmp = path + ".tmp";
  std::ofstream out(tmp);
  if (!out.is_open()) return;
  out << "# AE_SYNC_STATE_V1\n# Account\tFolder\tUIDValidity\tLastUID\tBackfilled\n";
  for (const auto& st : sync_state_) {
    out << escape_tab_field(st.account_id) << "\t" << escape_tab_field(st.folder) << "\t"
        << st.uidvalidity << "\t" << st.last_uid << "\t" << (st.backfilled ? "1" : "0") << "\n";
  }
  out.close();
  ::rename(tmp.c_str(), path.c_str());
}

MailFolderSync& MailStore::sync_state_for(const std::string& account_id, const std::string& folder) {
  for (auto& st : sync_state_) {
    if (st.account_id == account_id && st.folder == folder) return st;
  }
  MailFolderSync st;
  st.account_id = account_id;
  st.folder = folder;
  sync_state_.push_back(st);
  return sync_state_.back();
}

// ---------------------------------------------------------------------------
// Vacuum: envelopes are cheap and stay; bodies are what costs and get evicted
// ---------------------------------------------------------------------------

size_t MailStore::vacuum_bodies() {
#if defined(AIOS_METAL)
  return 0;
#else
  if (root_dir_.empty()) return 0;
  struct Cached { std::string path; uint64_t mtime; uint64_t size; uint64_t msg_time; };
  std::vector<Cached> cached;
  uint64_t total = 0;

  for (const auto& m : messages_) {
    // A message whose body we could never fetch again has to keep its file:
    // drafts and anything we composed live nowhere else, and a row with no uid
    // has no way back to the server.
    if (m.is_draft || m.uid == 0 || m.folder == "drafts") continue;
    if (m.eml_path.empty()) continue;
    std::string full = root_dir_ + "/" + m.eml_path;
    size_t dot = full.rfind(".eml");
    if (dot == std::string::npos) continue;
    const std::string base = full.substr(0, dot);

    // A cached body is the .eml AND its sidecars -- the sender's html and the
    // extracted text -- and it is keyed on the MESSAGE, not on the .eml being
    // there. Two rounds of this: counting only the .eml let 1,177 .html and
    // 1,245 .txt files sit outside the cap that is supposed to bound them; then
    // requiring the .eml to exist did it again, because a message whose raw
    // copy is over the 4MB limit has no .eml at all -- 28 such messages held
    // 31MB of html that was invisible to the accounting AND could never be
    // evicted, which is a leak and not merely a miscount.
    uint64_t bytes = 0, newest = 0;
    bool any = false;
    for (const char* ext : {".eml", ".html", ".txt"}) {
      struct stat st{};
      if (::stat((base + ext).c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
      any = true;
      bytes += (uint64_t)st.st_size;
      if ((uint64_t)st.st_mtime > newest) newest = (uint64_t)st.st_mtime;
    }
    if (!any) continue;
    cached.push_back({full, newest, bytes, m.timestamp});
    total += bytes;
  }

  uint64_t now = (uint64_t)std::time(nullptr);
  uint64_t age_cutoff = (body_retain_days_ == 0) ? 0 : now - (uint64_t)body_retain_days_ * 86400;

  // Oldest-accessed first, so anything recently opened survives the size pass.
  std::sort(cached.begin(), cached.end(),
            [](const Cached& a, const Cached& b) { return a.mtime < b.mtime; });

  size_t removed = 0;
  for (const auto& c : cached) {
    bool too_old = age_cutoff && c.msg_time && c.msg_time < age_cutoff && c.mtime < age_cutoff;
    bool over_cap = body_cache_cap_bytes_ && total > body_cache_cap_bytes_;
    if (!too_old && !over_cap) continue;
    // The .eml, the html and the display text are one cached body and go
    // together. Dropping only some of them would leave a message looking
    // present while the part the renderer keys off had gone -- and a body with
    // no .eml (too large to keep raw) still has the other two to release.
    size_t dot = c.path.rfind(".eml");
    const std::string base = (dot == std::string::npos) ? c.path : c.path.substr(0, dot);
    bool gone = false;
    for (const char* ext : {".eml", ".html", ".txt"}) {
      if (::unlink((base + ext).c_str()) == 0) gone = true;
    }
    if (gone) {
      total -= c.size;
      removed++;
    }
  }

  // Drop the bodies of evicted messages from memory too, or the next save
  // writes the file straight back.
  if (removed) {
    for (auto& m : messages_) {
      if (m.eml_path.empty() || m.is_draft) continue;
      if (file_exists(root_dir_ + "/" + m.eml_path)) continue;
      m.body.clear();
      // Say the html is gone as well, or the pane offers to render a file that
      // is no longer there. A refetch sets both again.
      m.has_html = false;
      m.raw_on_disk = false;
    }
  }
  return removed;
#endif
}

std::vector<MailSyncJob> MailStore::build_sync_jobs(const std::string& account_id) {
  std::string tmp_dir = root_dir_ + "/tmp";
  ensure_dir(tmp_dir);
  std::vector<MailSyncJob> jobs;
  uint64_t body_since = (uint64_t)std::time(nullptr) - (uint64_t)body_retain_days_ * 86400;

  for (const auto& acc : accounts_) {
    if (account_id != "all" && acc.id != account_id && acc.email != account_id) continue;
    for (const char* local : {"inbox", "sent"}) {
      MailFolderSync& st = sync_state_for(acc.id, local);
      MailSyncJob j;
      j.mode = "headers";
      j.account_id = acc.id;
      j.local_folder = local;
      j.imap_folder = mail_imap_folder_name(acc, local);
      j.server = acc.incoming_server;
      j.port = acc.incoming_port;
      j.security = acc.incoming_port == 993 ? 1 : (acc.use_tls ? 2 : 0);
      j.username = acc.username;
      j.password = acc.password;
      j.oauth = acc.auth == "oauth2";
      j.oauth_provider = mail_oauth_provider(acc);
      j.oauth_account = acc.email;
      j.raw_dir = tmp_dir;
      j.body_since = body_since;
      // A finished backfill only ever asks for what is new. An unfinished one walks the whole mailbox in
      // one pass -- headers alone, which measured ~10s per 5000 messages, so even a 50k mailbox is a
      // minute and a half.
      j.last_uid = st.backfilled ? st.last_uid : 0;
      j.limit = st.backfilled ? 0 : backfill_limit_;
      jobs.push_back(j);
    }
  }
  return jobs;
}

MailSyncJob MailStore::job_for_message(const MailAccount& acc, const MailMessage& m) const {
  MailSyncJob j;
  j.mode = "body";
  j.account_id = acc.id;
  j.local_folder = m.folder;
  j.imap_folder = mail_imap_folder_name(acc, m.folder);
  j.server = acc.incoming_server;
  j.port = acc.incoming_port;
  j.security = acc.incoming_port == 993 ? 1 : (acc.use_tls ? 2 : 0);
  j.username = acc.username;
  j.password = acc.password;
  j.oauth = acc.auth == "oauth2";
  j.oauth_provider = mail_oauth_provider(acc);
  j.oauth_account = acc.email;
  j.body_uid = std::to_string(m.uid);
  j.raw_dir = root_dir_ + "/tmp";
  return j;
}

void MailStore::apply_sync_outcomes(const std::vector<MailSyncOutcome>& outcomes) {
  size_t total_added = 0;
  int total_fetched = 0;
  bool any_ok = false;
  std::string first_problem;

  for (const auto& r : outcomes) {
    MailAccount* acc = get_account_mut(r.account_id);
    if (!acc) continue;
    if (r.mode == "body") {
      // One message's body arriving says nothing about how far the folder has been synced, so it touches
      // neither the sync state nor last_sync_at.
      if (r.ok) merge_records(*acc, r.folder, r.records, root_dir_ + "/tmp");
      continue;
    }
    if (r.ok) {
      MailFolderSync& st = sync_state_for(r.account_id, r.folder);
      // UIDVALIDITY changing means the server renumbered the mailbox and every uid we hold is
      // meaningless. Start the folder over rather than matching new messages onto old rows.
      if (st.uidvalidity && r.uidvalidity && st.uidvalidity != r.uidvalidity) {
        for (auto& m : messages_) {
          if (m.account == r.account_id && m.folder == r.folder) m.uid = 0;
        }
        st.last_uid = 0;
        st.backfilled = false;
      }
      if (r.uidvalidity) st.uidvalidity = r.uidvalidity;
      if (r.max_uid > st.last_uid) st.last_uid = r.max_uid;
      st.backfilled = true;

      total_added += merge_records(*acc, r.folder, r.records, root_dir_ + "/tmp");
      total_fetched += r.fetched;
      any_ok = true;
      acc->online = true;
      acc->last_sync_at = (uint64_t)std::time(nullptr);
    } else {
      if (r.attempted) acc->online = false;
      if (first_problem.empty()) first_problem = r.status;
    }
  }

  for (auto& acc : accounts_) {
    bool touched = false;
    for (const auto& r : outcomes) if (r.account_id == acc.id) touched = true;
    if (!touched) continue;
    if (any_ok) {
      acc.last_sync_status = "Synced " + std::to_string(total_fetched) + " message"
                           + (total_fetched == 1 ? "" : "s")
                           + (total_added ? " (" + std::to_string(total_added) + " new)" : "");
    } else if (!first_problem.empty()) {
      acc.last_sync_status = first_problem;
    }
  }

  // Bodies are the only thing here that grows without bound, so the vacuum runs on the way out of every
  // sync rather than waiting to be asked.
  vacuum_bodies();

  // Persist messages, accounts, sync state AND the per-account status in one pass. (reload() here would
  // clear accounts_ and re-read them from disk, throwing away every status the sync just wrote.)
  save_to_disk();
  save_sync_state();
}

#if !defined(AIOS_METAL)
bool MailStore::begin_background_sync(const std::string& account_id) {
  bool expected = false;
  if (!sync_running_.compare_exchange_strong(expected, true)) return false;

  auto jobs = build_sync_jobs(account_id);
  if (jobs.empty()) { sync_running_.store(false); return false; }

  aios::hal::Thread([this, jobs]() {
    std::vector<MailSyncOutcome> results;
    results.reserve(jobs.size());
    for (const auto& j : jobs) results.push_back(run_job(j));
    {
      std::lock_guard<aios::hal::Mutex> lk(sync_mu_);
      sync_results_ = std::move(results);
    }
    sync_done_.store(true);
    sync_running_.store(false);
  }).detach();
  return true;
}

bool MailStore::poll_background_sync() {
  bool expected = true;
  if (!sync_done_.compare_exchange_strong(expected, false)) return false;
  std::vector<MailSyncOutcome> results;
  {
    std::lock_guard<aios::hal::Mutex> lk(sync_mu_);
    results.swap(sync_results_);
  }
  apply_sync_outcomes(results);
  return true;
}

bool MailStore::background_body_pending(const std::string& message_id) const {
  std::lock_guard<aios::hal::Mutex> lk(sync_mu_);
  return body_fetch_id_ == message_id;
}

bool MailStore::begin_background_body_fetch(const std::string& message_id) {
  const MailMessage* m = nullptr;
  for (const auto& msg : messages_) {
    if (msg.id == message_id) { m = &msg; break; }
  }
  if (!m || !m->body.empty() || m->uid == 0) return false;
  const MailAccount* acc = get_account(m->account);
  if (!acc || (acc->password.empty() && acc->auth != "oauth2")) return false;

  bool expected = false;
  if (!sync_running_.compare_exchange_strong(expected, true)) return false;

  MailSyncJob j = job_for_message(*acc, *m);
  {
    std::lock_guard<aios::hal::Mutex> lk(sync_mu_);
    body_fetch_id_ = message_id;
  }
  aios::hal::Thread([this, j]() {
    MailSyncOutcome r = run_job(j);
    {
      std::lock_guard<aios::hal::Mutex> lk(sync_mu_);
      sync_results_ = {r};
      body_fetch_id_.clear();
    }
    sync_done_.store(true);
    sync_running_.store(false);
  }).detach();
  return true;
}
#endif

Result<size_t> MailStore::fetch_body(const std::string& message_id) {
  MailMessage* m = nullptr;
  for (auto& msg : messages_) {
    if (msg.id == message_id) { m = &msg; break; }
  }
  if (!m) return Result<size_t>(Status::NotFound, "no message " + message_id);
  if (!m->body.empty()) return (size_t)m->body.size();
  ensure_body(*m);
  if (!m->body.empty()) return (size_t)m->body.size();
  if (m->uid == 0) return Result<size_t>(Status::NotFound, "message has no server uid to fetch by");

  const MailAccount* acc = get_account(m->account);
  if (!acc) return Result<size_t>(Status::NotFound, "no account " + m->account);

  ensure_dir(root_dir_ + "/tmp");
  MailSyncJob j = job_for_message(*acc, *m);
  MailSyncOutcome r = run_job(j);
  if (!r.ok) return Result<size_t>(Status::IoError, r.status.empty() ? "body fetch failed" : r.status);
  merge_records(*acc, m->folder, r.records, root_dir_ + "/tmp");
  save_to_disk();

  for (auto& msg : messages_) {
    if (msg.id == message_id) return (size_t)msg.body.size();
  }
  return (size_t)0;
}

Result<std::vector<MailMessage>> MailStore::search_server(const std::string& query,
                                                          const std::string& account_id,
                                                          int limit) {
  const MailAccount* acc = get_account(account_id.empty() ? primary_account().id : account_id);
  if (!acc) return Result<std::vector<MailMessage>>(Status::NotFound, "no such account");

  MailSyncJob j = job_for_message(*acc, MailMessage{});
  j.mode = "search";
  j.local_folder = "inbox";
  // Gmail's All Mail is the only place a server search sees archived mail too.
  j.imap_folder = (acc->provider == "gmail") ? "[Gmail]/All Mail" : "INBOX";
  j.body_uid.clear();
  j.raw_dir.clear();
  j.query = query;
  j.limit = limit;

  MailSyncOutcome r = run_job(j);
  if (!r.ok) {
    return Result<std::vector<MailMessage>>(Status::IoError,
             r.status.empty() ? "server search failed" : r.status);
  }

  // Results are shown, not merged: they are a view of the server, and folding archived mail into the
  // local inbox would be a lie about where it lives.
  std::vector<MailMessage> out;
  for (const auto& o : r.records) {
    if (o.subject.empty() && o.uid == 0) continue;
    MailMessage m;
    m.uid = o.uid;
    m.account = acc->id;
    m.folder = "inbox";
    m.id = "server-" + std::to_string(o.uid);
    m.message_id = o.message_id;
    m.from_name = o.from_name;
    m.from_addr = o.from_addr;
    m.subject = o.subject;
    m.date_str = o.date_str;
    m.timestamp = o.timestamp;
    m.snippet = o.snippet;
    out.push_back(m);
  }
  return out;
}

Result<void> MailStore::check_new_mail(const std::string& account_id) {
  ensure_dir(root_dir_ + "/tmp");
  auto jobs = build_sync_jobs(account_id);
  std::vector<MailSyncOutcome> results;
  results.reserve(jobs.size());
  for (const auto& j : jobs) results.push_back(run_job(j));
  apply_sync_outcomes(results);
  transient_status_.clear();
  return Ok();
}


std::string MailStore::status_line() const {
  if (!transient_status_.empty()) return transient_status_;

  uint64_t newest = 0;
  bool any_syncable = false;
  std::string problem;
  for (const auto& a : accounts_) {
    if (a.last_sync_at > newest) newest = a.last_sync_at;
    if ((!a.password.empty() || a.auth == "oauth2") && !a.incoming_server.empty()) any_syncable = true;
    if (problem.empty() && a.last_sync_status.rfind("Sync failed", 0) == 0) problem = a.last_sync_status;
    if (problem.empty() && a.last_sync_status.rfind("Not syncing", 0) == 0) problem = a.last_sync_status;
  }

  std::string head;
  if (newest == 0) {
    head = accounts_.empty() ? "No mail account" : (any_syncable ? "Never synced" : "Not configured to sync");
  } else {
    uint64_t now = (uint64_t)std::time(nullptr);
    uint64_t age = (now > newest) ? (now - newest) : 0;
    if (age < 60) head = "Updated just now";
    else if (age < 3600) head = "Updated " + std::to_string(age / 60) + "m ago";
    else if (age < 86400) head = "Updated " + std::to_string(age / 3600) + "h ago";
    else head = "Updated " + std::to_string(age / 86400) + "d ago";
  }

  std::string s = head + " • " + std::to_string(total_unread()) + " unread";
  if (!problem.empty()) s += " • " + problem;
  return s;
}

}  // namespace aios::userland
