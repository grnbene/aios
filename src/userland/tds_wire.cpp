// AIOS — Native Microsoft TDS 7.4 wire client implementation ([MS-TDS]).
#include "aios/userland/tds_wire.hpp"
#include "aios/userland/sql_database.hpp"
#include "aios/hal/host.hpp"
#include "aios/net/tls.hpp"
#include "aios/net/tls_conn.hpp"
#include <iconv.h>
#include <unistd.h>
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <map>

namespace aios::userland {

namespace {

constexpr uint8_t kPktSqlBatch = 0x01, kPktReply = 0x04, kPktLogin7 = 0x10, kPktPrelogin = 0x12;
constexpr uint8_t kEncOff = 0x00, kEncOn = 0x01, kEncNotSup = 0x02, kEncReq = 0x03;
constexpr size_t kMaxReply = size_t(1) << 30;   // a reply past this is not one the gateway can hold

uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
uint64_t le64(const uint8_t* p) { return uint64_t(le32(p)) | (uint64_t(le32(p + 4)) << 32); }
uint64_t le_n(const uint8_t* p, size_t n) { uint64_t v = 0; for (size_t i = 0; i < n; ++i) v |= uint64_t(p[i]) << (8 * i); return v; }
uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
void put16(std::vector<uint8_t>* v, uint16_t x) { v->push_back(uint8_t(x)); v->push_back(uint8_t(x >> 8)); }
void put32(std::vector<uint8_t>* v, uint32_t x) { for (int i = 0; i < 4; ++i) v->push_back(uint8_t(x >> (8 * i))); }
void put64(std::vector<uint8_t>* v, uint64_t x) { for (int i = 0; i < 8; ++i) v->push_back(uint8_t(x >> (8 * i))); }
void set16(uint8_t* p, uint16_t x) { p[0] = uint8_t(x); p[1] = uint8_t(x >> 8); }
void set32(uint8_t* p, uint32_t x) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(x >> (8 * i)); }

std::vector<uint8_t> utf8_to_utf16le(const std::string& s) {
  std::vector<uint8_t> out;
  out.reserve(s.size() * 2);
  for (size_t i = 0; i < s.size();) {
    uint32_t cp;
    const uint8_t c = static_cast<uint8_t>(s[i]);
    if (c < 0x80) { cp = c; i += 1; }
    else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) { cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F); i += 2; }
    else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) { cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F); i += 3; }
    else if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
      cp = ((c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F);
      i += 4;
    } else { cp = 0xFFFD; i += 1; }
    if (cp <= 0xFFFF) { out.push_back(uint8_t(cp)); out.push_back(uint8_t(cp >> 8)); }
    else {
      cp -= 0x10000;
      const uint16_t hi = uint16_t(0xD800 + (cp >> 10)), lo = uint16_t(0xDC00 + (cp & 0x3FF));
      out.push_back(uint8_t(hi)); out.push_back(uint8_t(hi >> 8));
      out.push_back(uint8_t(lo)); out.push_back(uint8_t(lo >> 8));
    }
  }
  return out;
}

void put_utf8(std::string* s, uint32_t cp) {
  if (cp < 0x80) s->push_back(char(cp));
  else if (cp < 0x800) { s->push_back(char(0xC0 | (cp >> 6))); s->push_back(char(0x80 | (cp & 0x3F))); }
  else if (cp < 0x10000) { s->push_back(char(0xE0 | (cp >> 12))); s->push_back(char(0x80 | ((cp >> 6) & 0x3F))); s->push_back(char(0x80 | (cp & 0x3F))); }
  else {
    s->push_back(char(0xF0 | (cp >> 18))); s->push_back(char(0x80 | ((cp >> 12) & 0x3F)));
    s->push_back(char(0x80 | ((cp >> 6) & 0x3F))); s->push_back(char(0x80 | (cp & 0x3F)));
  }
}

// UTF-16LE to UTF-8, surrogate pairs joined (an emoji is one character, not two broken halves).
std::string utf16le_to_utf8(const uint8_t* p, size_t byte_len) {
  std::string s;
  s.reserve(byte_len / 2);
  for (size_t i = 0; i + 1 < byte_len; i += 2) {
    uint32_t u = le16(p + i);
    if (u >= 0xD800 && u <= 0xDBFF && i + 3 < byte_len) {
      const uint32_t lo = le16(p + i + 2);
      if (lo >= 0xDC00 && lo <= 0xDFFF) { u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00); i += 2; }
      else u = 0xFFFD;
    } else if (u >= 0xD800 && u <= 0xDFFF) {
      u = 0xFFFD;
    }
    put_utf8(&s, u);
  }
  return s;
}

// The Windows ANSI code page a non-Unicode (varchar/char/text) column's bytes
// are in, from its collation's locale: the code page SQL Server itself uses.
int code_page_of(uint32_t collation, uint8_t sort_id) {
  // SQL collations from SQL Server 2000 (SortId set) whose code page is not their locale's.
  if (sort_id >= 30 && sort_id <= 34) return 437;
  if (sort_id >= 40 && sort_id <= 49) return 850;
  const uint32_t lcid = collation & 0xFFFFF;
  switch (lcid & 0x3FF) {   // the primary language
    case 0x05: case 0x0E: case 0x15: case 0x18: case 0x1A: case 0x1B: case 0x1C: case 0x24: return 1250;
    case 0x02: case 0x19: case 0x22: case 0x23: case 0x2F: case 0x3F: case 0x44: case 0x50: return 1251;
    case 0x08: return 1253;
    case 0x1F: case 0x2C: case 0x43: return 1254;
    case 0x0D: return 1255;
    case 0x01: case 0x20: case 0x29: return 1256;
    case 0x25: case 0x26: case 0x27: return 1257;
    case 0x2A: return 1258;
    case 0x1E: return 874;
    case 0x11: return 932;
    case 0x12: return 949;
    case 0x04: return (lcid == 0x0404 || lcid == 0x0C04 || lcid == 0x1404) ? 950 : 936;
    default: return 1252;
  }
}

// A varchar's bytes as UTF-8: as they are in a UTF-8 collation (SQL Server
// 2019's _UTF8), else converted from the column's code page.
std::string ansi_to_utf8(const uint8_t* p, size_t n, uint32_t collation, uint8_t sort_id) {
  bool ascii = true;
  for (size_t i = 0; i < n && ascii; ++i) ascii = p[i] < 0x80;
  if (ascii || (collation >> 26) & 1) return std::string(reinterpret_cast<const char*>(p), n);
  const std::string from = "CP" + std::to_string(code_page_of(collation, sort_id));
  iconv_t cd = iconv_open("UTF-8", from.c_str());
  if (cd == (iconv_t)-1) {   // no converter for it here: each byte past ASCII is U+FFFD, never passed off as UTF-8
    std::string s;
    for (size_t i = 0; i < n; ++i) { if (p[i] < 0x80) s.push_back(char(p[i])); else put_utf8(&s, 0xFFFD); }
    return s;
  }
  std::string out(n * 4 + 8, '\0');
  char* in = const_cast<char*>(reinterpret_cast<const char*>(p));
  size_t in_left = n, out_left = out.size();
  char* o = out.data();
  while (in_left > 0) {
    if (iconv(cd, &in, &in_left, &o, &out_left) != (size_t)-1) break;
    if (errno == EILSEQ || errno == EINVAL) {   // a byte with no character: U+FFFD and on
      if (out_left < 3) break;
      *o++ = char(0xEF); *o++ = char(0xBF); *o++ = char(0xBD); out_left -= 3;
      ++in; --in_left;
    } else {
      break;
    }
  }
  iconv_close(cd);
  out.resize(out.size() - out_left);
  return out;
}

const char* type_name(uint8_t t) {
  switch (t) {
    case 0x1F: return "null";
    case 0x30: return "tinyint";
    case 0x32: return "bit";
    case 0x34: return "smallint";
    case 0x38: return "int";
    case 0x7F: return "bigint";
    case 0x26: return "int";
    case 0x68: return "bit";
    case 0x3B: return "real";
    case 0x3E: return "float";
    case 0x6D: return "float";
    case 0x3C: case 0x7A: case 0x6E: return "money";
    case 0x3D: case 0x6F: return "datetime";
    case 0x3A: return "smalldatetime";
    case 0x28: return "date";
    case 0x29: return "time";
    case 0x2A: return "datetime2";
    case 0x2B: return "datetimeoffset";
    case 0x6A: case 0x37: return "decimal";
    case 0x6C: case 0x3F: return "numeric";
    case 0x24: return "uniqueidentifier";
    case 0xA7: return "varchar";
    case 0xAF: return "char";
    case 0xE7: return "nvarchar";
    case 0xEF: return "nchar";
    case 0xA5: return "varbinary";
    case 0xAD: return "binary";
    case 0x23: return "text";
    case 0x63: return "ntext";
    case 0x22: return "image";
    case 0xF1: return "xml";
    case 0x62: return "sql_variant";
    default: return "?";
  }
}

// Bounds-checked reading of a token stream: any read past the end marks it
// failed, and the caller treats the reply as corrupt rather than guessing.
struct Rd {
  const uint8_t* p;
  size_t n, pos = 0;
  bool bad = false;
  bool has(size_t k) { if (bad || pos + k > n) { bad = true; return false; } return true; }
  uint8_t u8() { return has(1) ? p[pos++] : 0; }
  uint16_t u16() { if (!has(2)) return 0; uint16_t v = le16(p + pos); pos += 2; return v; }
  uint32_t u32() { if (!has(4)) return 0; uint32_t v = le32(p + pos); pos += 4; return v; }
  uint64_t u64() { if (!has(8)) return 0; uint64_t v = le64(p + pos); pos += 8; return v; }
  const uint8_t* take(size_t k) { if (!has(k)) return nullptr; const uint8_t* q = p + pos; pos += k; return q; }
  void skip(size_t k) { if (has(k)) pos += k; }
  std::string b_varchar() { const uint8_t k = u8(); const uint8_t* q = take(size_t(k) * 2); return q ? utf16le_to_utf8(q, size_t(k) * 2) : std::string(); }
  std::string us_varchar() { const uint16_t k = u16(); const uint8_t* q = take(size_t(k) * 2); return q ? utf16le_to_utf8(q, size_t(k) * 2) : std::string(); }
};

// 0001-01-01 + days, as y-m-d (proleptic Gregorian, as SQL Server's date is).
void civil_from_days(int64_t days_since_0001, int* y, unsigned* m, unsigned* d) {
  int64_t z = days_since_0001 - 719162 + 719468;   // to days since 1970-01-01, then the civil algorithm's epoch
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = unsigned(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t yy = int64_t(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  *d = doy - (153 * mp + 2) / 5 + 1;
  *m = mp < 10 ? mp + 3 : mp - 9;
  *y = int(yy + (*m <= 2));
}

std::string date_text(int64_t days_since_0001) {
  int y; unsigned m, d;
  civil_from_days(days_since_0001, &y, &m, &d);
  char b[16];
  std::snprintf(b, sizeof b, "%04d-%02u-%02u", y, m, d);
  return b;
}

// Seconds-and-fraction since midnight, `scale` digits of fraction.
std::string time_text(uint64_t units, int scale) {
  uint64_t div = 1;
  for (int i = 0; i < scale; ++i) div *= 10;
  const uint64_t secs = units / div, frac = units % div;
  char b[40];
  int k = std::snprintf(b, sizeof b, "%02u:%02u:%02u", unsigned(secs / 3600), unsigned(secs / 60 % 60), unsigned(secs % 60));
  if (scale > 0) std::snprintf(b + k, sizeof b - k, ".%0*llu", scale, (unsigned long long)frac);
  return b;
}

int time_bytes(int scale) { return scale <= 2 ? 3 : scale <= 4 ? 4 : 5; }

std::string hex2(unsigned v) {
  char b[8];
  std::snprintf(b, sizeof b, "%02X", v & 0xFF);
  return b;
}

// The shortest text that reads back as the same double, as the SQL engine
// prints a DOUBLE. (std::to_chars for floating point needs GCC 11; the el7
// build's manylinux2014 image has GCC 10.)
std::string num_text(double v) { return sql_format_double(v, 0); }

// An exact decimal: a magnitude of `len` little-endian bytes, `scale` digits after the point.
std::string decimal_text(bool positive, const uint8_t* mag, size_t len, int scale) {
  unsigned __int128 v = 0;
  for (size_t i = len; i-- > 0;) v = (v << 8) | mag[i];
  std::string digits;
  do { digits.push_back(char('0' + int(v % 10))); v /= 10; } while (v != 0);
  while ((int)digits.size() <= scale) digits.push_back('0');
  std::reverse(digits.begin(), digits.end());
  if (scale > 0) digits.insert(digits.end() - scale, '.');
  bool zero = true;
  for (char c : digits) if (c != '0' && c != '.') zero = false;
  return (positive || zero ? "" : "-") + digits;
}

std::string money_text(int64_t v) {   // ten-thousandths, printed with four places as SQL Server does
  const bool neg = v < 0;
  const uint64_t a = neg ? uint64_t(0) - uint64_t(v) : uint64_t(v);
  char b[40];
  std::snprintf(b, sizeof b, "%s%llu.%04llu", neg ? "-" : "", (unsigned long long)(a / 10000), (unsigned long long)(a % 10000));
  return b;
}

std::string guid_text(const uint8_t* g) {   // the first three groups little-endian, the rest as stored
  char b[40];
  std::snprintf(b, sizeof b, "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", le32(g), le16(g + 4), le16(g + 6), g[8], g[9], g[10], g[11],
                g[12], g[13], g[14], g[15]);
  return b;
}

// A (MAX) value's chunks: total length (or NULL / unknown), then length-prefixed parts to a zero one.
bool read_plp(Rd& r, std::string* out, bool* is_null) {
  const uint64_t total = r.u64();
  if (r.bad) return false;
  if (total == 0xFFFFFFFFFFFFFFFFull) { *is_null = true; return true; }
  *is_null = false;
  if (total != 0xFFFFFFFFFFFFFFFEull && total < kMaxReply) out->reserve(size_t(total));
  for (;;) {
    const uint32_t k = r.u32();
    if (r.bad) return false;
    if (k == 0) return true;
    const uint8_t* q = r.take(k);
    if (!q) return false;
    out->append(reinterpret_cast<const char*>(q), k);
  }
}

// One column's value from a ROW / NBCROW, as the text a MySQL client reads.
bool read_value(Rd& r, const TdsWireSession::Col& c, SqlWireValue* v) {
  v->is_null = false;
  auto fixed_int = [&](size_t k, bool is_signed) {
    const uint8_t* q = r.take(k);
    if (!q) return;
    const uint64_t u = le_n(q, k);
    if (!is_signed) { v->bytes = std::to_string(u); return; }
    const int64_t s = k == 8 ? int64_t(u) : k == 4 ? int64_t(int32_t(uint32_t(u))) : int64_t(int16_t(uint16_t(u)));
    v->bytes = std::to_string(s);
  };
  auto datetime8 = [&](const uint8_t* q) {   // days since 1900-01-01, 300ths of a second since midnight
    const int32_t days = int32_t(le32(q));
    const uint32_t ticks = le32(q + 4);
    const uint64_t ms = (uint64_t(ticks) * 1000 + 150) / 300;   // SQL Server rounds .xx3/.xx7 to the millisecond like this
    v->bytes = date_text(int64_t(days) + 693595) + " " + time_text(ms, 3);
  };
  auto smalldatetime = [&](const uint8_t* q) {
    v->bytes = date_text(int64_t(le16(q)) + 693595) + " " + time_text(uint64_t(le16(q + 2)) * 60, 0);
  };
  switch (c.type) {
    case 0x1F: v->is_null = true; return true;
    case 0x30: fixed_int(1, false); break;
    case 0x32: { const uint8_t* q = r.take(1); if (q) v->bytes = *q ? "1" : "0"; break; }
    case 0x34: fixed_int(2, true); break;
    case 0x38: fixed_int(4, true); break;
    case 0x7F: fixed_int(8, true); break;
    case 0x3B: { const uint8_t* q = r.take(4); if (q) { float f; uint32_t u = le32(q); std::memcpy(&f, &u, 4); v->bytes = num_text(f); } break; }
    case 0x3E: { const uint8_t* q = r.take(8); if (q) { double d; uint64_t u = le64(q); std::memcpy(&d, &u, 8); v->bytes = num_text(d); } break; }
    case 0x3C: { const uint8_t* q = r.take(8); if (q) v->bytes = money_text(int64_t((uint64_t(le32(q)) << 32) | le32(q + 4))); break; }
    case 0x7A: { const uint8_t* q = r.take(4); if (q) v->bytes = money_text(int32_t(le32(q))); break; }
    case 0x3D: { const uint8_t* q = r.take(8); if (q) datetime8(q); break; }
    case 0x3A: { const uint8_t* q = r.take(4); if (q) smalldatetime(q); break; }
    case 0x26: case 0x68: case 0x6D: case 0x6E: case 0x6F: case 0x24: case 0x6A: case 0x6C: case 0x37: case 0x3F:
    case 0x28: case 0x29: case 0x2A: case 0x2B: {
      const uint8_t k = r.u8();
      if (r.bad) return false;
      if (k == 0) { v->is_null = true; return true; }
      const uint8_t* q = r.take(k);
      if (!q) return false;
      switch (c.type) {
        case 0x26: {
          const uint64_t u = le_n(q, k);
          v->bytes = k == 1 ? std::to_string(u) : std::to_string(k == 8 ? int64_t(u) : k == 4 ? int64_t(int32_t(uint32_t(u))) : int64_t(int16_t(uint16_t(u))));
          break;
        }
        case 0x68: v->bytes = q[0] ? "1" : "0"; break;
        case 0x6D:
          if (k == 4) { float f; uint32_t u = le32(q); std::memcpy(&f, &u, 4); v->bytes = num_text(f); }
          else if (k == 8) { double d; uint64_t u = le64(q); std::memcpy(&d, &u, 8); v->bytes = num_text(d); }
          else return false;
          break;
        case 0x6E:
          if (k == 8) v->bytes = money_text(int64_t((uint64_t(le32(q)) << 32) | le32(q + 4)));
          else if (k == 4) v->bytes = money_text(int32_t(le32(q)));
          else return false;
          break;
        case 0x6F:
          if (k == 8) datetime8(q);
          else if (k == 4) smalldatetime(q);
          else return false;
          break;
        case 0x24:
          if (k != 16) return false;
          v->bytes = guid_text(q);
          break;
        case 0x6A: case 0x6C: case 0x37: case 0x3F:
          if (k < 2 || k > 17) return false;
          v->bytes = decimal_text(q[0] == 1, q + 1, k - 1, c.scale);
          break;
        case 0x28:
          if (k != 3) return false;
          v->bytes = date_text(int64_t(le_n(q, 3)));
          break;
        case 0x29:
          if (k != time_bytes(c.scale)) return false;
          v->bytes = time_text(le_n(q, k), c.scale);
          break;
        case 0x2A: case 0x2B: {
          const int tb = time_bytes(c.scale);
          if (k != tb + 3 + (c.type == 0x2B ? 2 : 0)) return false;
          uint64_t units = le_n(q, tb);
          int64_t days = int64_t(le_n(q + tb, 3));
          int16_t offset = 0;
          if (c.type == 0x2B) {
            // Stored in UTC; shown at its own offset, as SQL Server shows it.
            offset = int16_t(le16(q + tb + 3));
            uint64_t div = 1;
            for (int i = 0; i < c.scale; ++i) div *= 10;
            const int64_t day_units = int64_t(86400) * int64_t(div);
            int64_t t = int64_t(units) + int64_t(offset) * 60 * int64_t(div);
            while (t < 0) { t += day_units; --days; }
            while (t >= day_units) { t -= day_units; ++days; }
            units = uint64_t(t);
          }
          v->bytes = date_text(days) + " " + time_text(units, c.scale);
          if (c.type == 0x2B) {
            char b[12];
            const int a = offset < 0 ? -offset : offset;
            std::snprintf(b, sizeof b, " %c%02d:%02d", offset < 0 ? '-' : '+', a / 60, a % 60);
            v->bytes += b;
          }
          break;
        }
      }
      break;
    }
    case 0xA7: case 0xAF: case 0xE7: case 0xEF: case 0xA5: case 0xAD: case 0xF1: {
      std::string raw;
      if (c.plp || c.type == 0xF1) {
        if (!read_plp(r, &raw, &v->is_null)) return false;
        if (v->is_null) return true;
      } else {
        const uint16_t k = r.u16();
        if (r.bad) return false;
        if (k == 0xFFFF) { v->is_null = true; return true; }
        const uint8_t* q = r.take(k);
        if (!q) return false;
        raw.assign(reinterpret_cast<const char*>(q), k);
      }
      const uint8_t* q = reinterpret_cast<const uint8_t*>(raw.data());
      if (c.type == 0xE7 || c.type == 0xEF || c.type == 0xF1) v->bytes = utf16le_to_utf8(q, raw.size());
      else if (c.type == 0xA7 || c.type == 0xAF) v->bytes = ansi_to_utf8(q, raw.size(), c.collation, c.sort_id);
      else v->bytes = std::move(raw);   // binary: the bytes themselves
      break;
    }
    case 0x23: case 0x63: case 0x22: {   // text / ntext / image: a text pointer, a timestamp, then the data
      const uint8_t ptr_len = r.u8();
      if (r.bad) return false;
      if (ptr_len == 0) { v->is_null = true; return true; }
      r.skip(ptr_len);
      r.skip(8);
      const uint32_t k = r.u32();
      const uint8_t* q = r.take(k);
      if (!q) return false;
      if (c.type == 0x63) v->bytes = utf16le_to_utf8(q, k);
      else if (c.type == 0x23) v->bytes = ansi_to_utf8(q, k, c.collation, c.sort_id);
      else v->bytes.assign(reinterpret_cast<const char*>(q), k);
      break;
    }
    case 0x62: {   // sql_variant: its base type and properties, then that type's value
      const uint32_t k = r.u32();
      if (r.bad) return false;
      if (k == 0) { v->is_null = true; return true; }
      const uint8_t* q = r.take(k);
      if (!q || k < 2) return false;
      const uint8_t base = q[0], props = q[1];
      if (2u + props > k) return false;
      TdsWireSession::Col inner;
      inner.type = base;
      const uint8_t* pr = q + 2;
      const uint8_t* data = q + 2 + props;
      const size_t data_len = k - 2 - props;
      std::vector<uint8_t> framed;   // the value in the form read_value reads the base type in
      switch (base) {
        case 0x30: case 0x32: case 0x34: case 0x38: case 0x7F: case 0x3B: case 0x3E: case 0x3C: case 0x7A: case 0x3D: case 0x3A:
          framed.assign(data, data + data_len);
          break;
        case 0x6A: case 0x6C:
          if (props < 2) return false;
          inner.precision = pr[0];
          inner.scale = pr[1];
          framed.push_back(uint8_t(data_len));
          framed.insert(framed.end(), data, data + data_len);
          break;
        case 0x24: case 0x28:
          framed.push_back(uint8_t(data_len));
          framed.insert(framed.end(), data, data + data_len);
          break;
        case 0x29: case 0x2A: case 0x2B:
          if (props < 1) return false;
          inner.scale = pr[0];
          framed.push_back(uint8_t(data_len));
          framed.insert(framed.end(), data, data + data_len);
          break;
        case 0xA7: case 0xAF: case 0xE7: case 0xEF:
          if (props < 7) return false;
          inner.collation = le32(pr);
          inner.sort_id = pr[4];
          put16(&framed, uint16_t(data_len));
          framed.insert(framed.end(), data, data + data_len);
          break;
        case 0xA5: case 0xAD:
          put16(&framed, uint16_t(data_len));
          framed.insert(framed.end(), data, data + data_len);
          break;
        default:
          return false;
      }
      Rd ir{framed.data(), framed.size()};
      if (!read_value(ir, inner, v) || ir.bad) return false;
      break;
    }
    default:
      return false;   // a type this reader does not know: corrupt, never guessed at
  }
  return !r.bad;
}

// TYPE_INFO after a column's UserType and Flags.
bool read_type_info(Rd& r, TdsWireSession::Col* c) {
  c->type = r.u8();
  switch (c->type) {
    case 0x1F: case 0x30: case 0x32: case 0x34: case 0x38: case 0x7F: case 0x3B: case 0x3E: case 0x3C: case 0x7A: case 0x3D: case 0x3A:
      break;   // fixed length: nothing more
    case 0x26: case 0x68: case 0x6D: case 0x6E: case 0x6F: case 0x24:
      c->max_len = r.u8();
      break;
    case 0x6A: case 0x6C: case 0x37: case 0x3F:
      c->max_len = r.u8();
      c->precision = r.u8();
      c->scale = r.u8();
      break;
    case 0x28:
      break;
    case 0x29: case 0x2A: case 0x2B:
      c->scale = r.u8();
      if (c->scale > 7) return false;
      break;
    case 0xA7: case 0xAF: case 0xE7: case 0xEF:
      c->max_len = r.u16();
      c->collation = r.u32();
      c->sort_id = r.u8();
      c->plp = c->max_len == 0xFFFF;
      break;
    case 0xA5: case 0xAD:
      c->max_len = r.u16();
      c->plp = c->max_len == 0xFFFF;
      break;
    case 0x23: case 0x63: case 0x22: {
      c->max_len = r.u32();
      if (c->type != 0x22) { c->collation = r.u32(); c->sort_id = r.u8(); }
      const uint8_t parts = r.u8();   // the table's name, in parts
      for (uint8_t i = 0; i < parts && !r.bad; ++i) (void)r.us_varchar();
      break;
    }
    case 0xF1: {   // xml: maybe a schema collection
      if (r.u8()) { (void)r.b_varchar(); (void)r.b_varchar(); (void)r.us_varchar(); }
      c->plp = true;
      break;
    }
    case 0x62:
      c->max_len = r.u32();
      break;
    default:
      return false;
  }
  return !r.bad;
}

// What an ERROR / INFO token says: "Msg 2627, Level 14, State 1: Violation of PRIMARY KEY ..."
std::string message_text(Rd& r, size_t end, int* number) {
  const uint32_t num = r.u32();
  const uint8_t state = r.u8();
  const uint8_t level = r.u8();
  const std::string msg = r.us_varchar();
  if (number) *number = int(num);
  if (r.pos > end || end > r.n) r.bad = true;   // the message ran past its own token
  else r.pos = end;
  return "Msg " + std::to_string(num) + ", Level " + std::to_string(level) + ", State " + std::to_string(state) + ": " + msg;
}

// The TLS handshake of TDS 7.x travels inside PRELOGIN packets; once it is
// done the TLS records go on the socket as they are. This is the socket the
// TLS client sees: framed while `framing`, bare after.
class TdsTlsShim : public hal::Connection {
 public:
  explicit TdsTlsShim(std::unique_ptr<hal::Connection> raw) : raw_(std::move(raw)) {}
  bool framing = true;
  bool send_all(ByteSpan data) override {
    if (!framing) return raw_->send_all(data);
    size_t off = 0;
    while (off < data.size()) {
      const size_t k = std::min<size_t>(data.size() - off, 4096 - 8);
      uint8_t hdr[8] = {kPktPrelogin, uint8_t(off + k == data.size() ? 1 : 0), uint8_t((k + 8) >> 8), uint8_t(k + 8), 0, 0, seq_++, 0};
      if (!raw_->send_all(ByteSpan(hdr, 8)) || !raw_->send_all(ByteSpan(data.data() + off, k))) return false;
      off += k;
    }
    return true;
  }
  bool recv_all(MutableByteSpan data, int timeout_ms) override {
    if (!framing && pos_ == buf_.size()) return raw_->recv_all(data, timeout_ms);
    size_t off = 0;
    while (off < data.size()) {
      if (pos_ == buf_.size()) {
        if (!framing) return raw_->recv_all(MutableByteSpan(data.data() + off, data.size() - off), timeout_ms);
        if (!fill(timeout_ms)) return false;
        continue;
      }
      const size_t k = std::min(data.size() - off, buf_.size() - pos_);
      std::memcpy(data.data() + off, buf_.data() + pos_, k);
      pos_ += k; off += k;
    }
    return true;
  }
  long recv_some(MutableByteSpan data, int timeout_ms) override {
    if (data.size() == 0) return 0;
    if (pos_ == buf_.size()) {
      if (!framing) return raw_->recv_some(data, timeout_ms);
      if (!fill(timeout_ms)) return raw_->alive() ? 0 : -1;
    }
    const size_t k = std::min(data.size(), buf_.size() - pos_);
    std::memcpy(data.data(), buf_.data() + pos_, k);
    pos_ += k;
    return long(k);
  }
  std::string peer() const override { return raw_->peer(); }
  void close() override { raw_->close(); }
  bool alive() const override { return raw_->alive(); }

 private:
  bool fill(int timeout_ms) {   // one PRELOGIN packet's payload
    uint8_t hdr[8];
    if (!raw_->recv_all(MutableByteSpan(hdr, 8), timeout_ms)) return false;
    const uint16_t len = be16(hdr + 2);
    if (len < 8) return false;
    buf_.assign(len - 8, 0);
    pos_ = 0;
    return len == 8 || raw_->recv_all(MutableByteSpan(buf_.data(), buf_.size()), timeout_ms);
  }
  std::unique_ptr<hal::Connection> raw_;
  std::vector<uint8_t> buf_;
  size_t pos_ = 0;
  uint8_t seq_ = 1;
};

}  // namespace

TdsWireSession::TdsWireSession(const std::string& host, uint16_t port)
    : host_(host.empty() ? "127.0.0.1" : host), port_(port ? port : 1433) {}

TdsWireSession::~TdsWireSession() {
  close();
}

void TdsWireSession::close() {
  if (conn_) {
    conn_->close();
    conn_.reset();
  }
  txn_descriptor_ = 0;
  tls_active_ = false;
}

Result<void> TdsWireSession::read_exact(uint8_t* buf, size_t n) {
  if (!conn_ || !conn_->alive()) return Result<void>(Status::IoError, "no open MS SQL (TDS) connection");
  if (!conn_->recv_all(MutableByteSpan(buf, n), read_timeout_ms_))
    return Result<void>(Status::IoError, conn_->alive() ? "timed out reading from MS SQL Server" : "MS SQL Server closed the connection");
  return Ok();
}

Result<void> TdsWireSession::send_exact(const uint8_t* buf, size_t n) {
  if (!conn_ || !conn_->alive()) return Result<void>(Status::IoError, "no open MS SQL (TDS) connection");
  if (!conn_->send_all(ByteSpan(buf, n))) return Result<void>(Status::IoError, "failed to send to MS SQL Server");
  return Ok();
}

Result<void> TdsWireSession::send_message(uint8_t type, const uint8_t* payload, size_t len) {
  // Every packet but the last is the negotiated size; the header's length is 16 bits.
  const size_t room = std::min<size_t>(packet_size_, 32767) - 8;
  size_t off = 0;
  packet_seq_ = 1;
  do {
    const size_t k = std::min(room, len - off);
    std::vector<uint8_t> pkt(8 + k);
    pkt[0] = type;
    pkt[1] = off + k == len ? 0x01 : 0x00;   // EOM on the last
    pkt[2] = uint8_t((k + 8) >> 8);
    pkt[3] = uint8_t(k + 8);
    pkt[6] = packet_seq_++;
    if (k) std::memcpy(pkt.data() + 8, payload + off, k);
    auto s = send_exact(pkt.data(), pkt.size());
    if (!s.ok()) return s;
    off += k;
  } while (off < len);
  return Ok();
}

Result<std::vector<uint8_t>> TdsWireSession::read_message(uint8_t* out_type) {
  std::vector<uint8_t> body;
  for (;;) {
    uint8_t hdr[8];
    auto r = read_exact(hdr, 8);
    if (!r.ok()) return Result<std::vector<uint8_t>>(r.error());
    const uint16_t len = be16(hdr + 2);
    if (len < 8) return Result<std::vector<uint8_t>>(Status::Corrupt, "corrupt TDS packet length");
    if (out_type) *out_type = hdr[0];
    const size_t at = body.size();
    if (at + len > kMaxReply) return Result<std::vector<uint8_t>>(Status::Corrupt, "a TDS reply larger than 1 GiB");
    body.resize(at + len - 8);
    if (len > 8) {
      r = read_exact(body.data() + at, len - 8);
      if (!r.ok()) return Result<std::vector<uint8_t>>(r.error());
    }
    if (hdr[1] & 0x01) return body;   // EOM
  }
}

Result<void> TdsWireSession::connect(const std::string& host, uint16_t port, const std::string& user, const std::string& pass,
                                     const std::string& db) {
  host_ = host.empty() ? "127.0.0.1" : host;
  port_ = port ? port : 1433;
  return connect(user, pass, db);
}

// TCP, then PRELOGIN: what this client offers for encryption and what the server answers.
Result<void> TdsWireSession::open_transport(uint8_t encryption, uint8_t* server_encryption) {
  close();
  packet_size_ = 4096;
  auto c = hal::net_connect(host_, port_, 5000);
  if (!c.ok())
    return Result<void>(c.status(), "failed to connect to MS SQL Server at " + host_ + ":" + std::to_string(port_) + ": " + c.error().message);
  conn_ = std::move(*c);

  // Option descriptors (token, offset, length; big-endian), the terminator, then the data.
  std::vector<uint8_t> pkt;
  const uint16_t data_at = 5 * 5 + 1;
  uint16_t off = data_at;
  auto opt = [&](uint8_t token, uint16_t len) {
    pkt.push_back(token);
    pkt.push_back(uint8_t(off >> 8)); pkt.push_back(uint8_t(off));
    pkt.push_back(uint8_t(len >> 8)); pkt.push_back(uint8_t(len));
    off = uint16_t(off + len);
  };
  opt(0x00, 6);   // VERSION
  opt(0x01, 1);   // ENCRYPTION
  opt(0x02, 1);   // INSTOPT
  opt(0x03, 4);   // THREADID
  opt(0x04, 1);   // MARS
  pkt.push_back(0xFF);
  const uint8_t version[6] = {0x0B, 0x00, 0x00, 0x00, 0x00, 0x00};   // the client library's version: 11.0.0.0
  pkt.insert(pkt.end(), version, version + 6);
  pkt.push_back(encryption);
  pkt.push_back(0x00);   // instance: the default
  put32(&pkt, uint32_t(::getpid()));
  pkt.push_back(0x00);   // no MARS
  auto s = send_message(kPktPrelogin, pkt.data(), pkt.size());
  if (!s.ok()) return s;

  uint8_t type = 0;
  auto resp = read_message(&type);
  if (!resp.ok()) return Result<void>(resp.error());
  if (type != kPktReply) return Result<void>(Status::Corrupt, "MS SQL Server did not answer PRELOGIN");
  *server_encryption = kEncNotSup;
  const auto& b = *resp;
  for (size_t i = 0; i + 5 <= b.size() && b[i] != 0xFF; i += 5) {
    const uint16_t o = be16(&b[i + 1]), l = be16(&b[i + 3]);
    if (size_t(o) + l > b.size()) return Result<void>(Status::Corrupt, "corrupt PRELOGIN answer");
    if (b[i] == 0x00 && l >= 6) {
      char v[32];
      std::snprintf(v, sizeof v, "%u.%u.%u", b[o], b[o + 1], unsigned(be16(&b[o + 2])));
      server_version_ = std::string("Microsoft SQL Server ") + v;
    }
    if (b[i] == 0x01 && l >= 1) *server_encryption = b[o];
  }
  return Ok();
}

Result<void> TdsWireSession::start_tls() {
  auto shim = std::make_unique<TdsTlsShim>(std::move(conn_));
  TdsTlsShim* raw = shim.get();
  hal::Host& host = hal::process_host();
  const std::string store = tls_ca_.empty() ? net::default_trust_store(host, "/nonexistent") : tls_ca_;
  auto tls = std::make_unique<net::TlsClient>(host, store);
  const std::string peer = raw->peer();
  auto h = tls->connect_over(std::move(shim), host_, port_, read_timeout_ms_);
  if (!h.ok()) return Result<void>(h.status(), "TLS with MS SQL Server: " + h.error().message);
  raw->framing = false;   // the handshake is done: records go bare from here
  tls_note_ = tls->info().version + " " + tls->info().cipher + ", certificate of " + tls->info().subject;
  conn_ = std::make_unique<net::TlsClientConnection>(std::move(tls), peer);
  tls_active_ = true;
  return Ok();
}

Result<void> TdsWireSession::connect(const std::string& user, const std::string& pass, const std::string& db) {
  user_ = user.empty() ? "sa" : user;
  pass_ = pass;
  db_ = db.empty() ? "master" : db;
  tls_note_.clear();

  // disable: no encryption offered (a server that insists says so). Otherwise
  // the whole session encrypted, the certificate checked; under prefer a
  // server that has none, or one whose certificate does not verify, is used
  // in the clear (unless it requires encryption).
  const bool want_tls = tls_mode_ != "disable";
  uint8_t server_enc = kEncNotSup;
  auto o = open_transport(want_tls ? kEncOn : kEncNotSup, &server_enc);
  if (!o.ok()) { close(); return o; }
  if (!want_tls) {
    if (server_enc == kEncReq) { close(); return Result<void>(Status::Denied, "MS SQL Server requires encryption and the session's TLS mode is disable"); }
    tls_note_ = "sslmode=disable";
  } else if (server_enc == kEncNotSup) {
    if (tls_required()) { close(); return Result<void>(Status::Denied, "MS SQL Server does not support encryption and the session's TLS mode is " + tls_mode_); }
    tls_note_ = "the server does not support encryption";
  } else {
    auto t = start_tls();
    if (!t.ok()) {
      close();
      if (tls_required() || server_enc == kEncReq) return t;
      // prefer: the same login in the clear, on a connection that says so from the start.
      const std::string why = t.error().message;
      o = open_transport(kEncNotSup, &server_enc);
      if (!o.ok()) { close(); return o; }
      if (server_enc == kEncReq) { close(); return Result<void>(Status::Denied, "MS SQL Server requires encryption: " + why); }
      tls_note_ = "in the clear: " + why;
    }
  }
  auto l = do_login7();
  if (!l.ok()) { close(); return l; }
  return Ok();
}

void TdsWireSession::env_change(const uint8_t* p, size_t len) {
  if (len < 1) return;
  const uint8_t kind = p[0];
  Rd r{p + 1, len - 1};
  if (kind == 1 || kind == 2 || kind == 4) {   // database, language, packet size: new value, old value (B_VARCHAR)
    const std::string v = r.b_varchar();
    if (r.bad) return;
    if (kind == 1) db_ = v;
    if (kind == 4) {
      const long n = std::strtol(v.c_str(), nullptr, 10);
      if (n >= 512 && n <= 32767) packet_size_ = uint32_t(n);
    }
  } else if (kind == 8) {   // BEGIN TRANSACTION: the descriptor every batch in it carries
    const uint8_t k = r.u8();
    const uint8_t* q = r.take(k);
    if (q && k == 8) txn_descriptor_ = le64(q);
  } else if (kind == 9 || kind == 10 || kind == 17) {   // COMMIT / ROLLBACK / transaction ended
    txn_descriptor_ = 0;
  }
}

Result<void> TdsWireSession::do_login7() {
  // The fixed part (94 bytes), then the strings it points at, all UTF-16LE.
  const uint16_t kFixed = 94;
  std::vector<uint8_t> body(kFixed, 0);
  const std::string client_host = "AIOS";
  const auto u_host = utf8_to_utf16le(client_host);
  const auto u_user = utf8_to_utf16le(user_);
  auto u_pass = utf8_to_utf16le(pass_);
  const auto u_app = utf8_to_utf16le("AIOS ASQL");
  const auto u_server = utf8_to_utf16le(host_);
  const auto u_lib = utf8_to_utf16le("AIOS TDS");
  const auto u_db = utf8_to_utf16le(db_);
  // The password is not encrypted, only obscured: each byte's nibbles swapped, then XOR 0xA5.
  for (auto& b : u_pass) b = uint8_t((((b << 4) & 0xF0) | ((b >> 4) & 0x0F)) ^ 0xA5);

  std::vector<uint8_t> strings;
  auto entry = [&](size_t at, const std::vector<uint8_t>& s) {
    set16(body.data() + at, uint16_t(kFixed + strings.size()));
    set16(body.data() + at + 2, uint16_t(s.size() / 2));
    strings.insert(strings.end(), s.begin(), s.end());
  };
  set32(body.data() + 4, 0x74000004);   // TDS 7.4
  set32(body.data() + 8, packet_size_);
  set32(body.data() + 12, 0x0B000000);  // client program version
  set32(body.data() + 16, uint32_t(::getpid()));
  body[24] = 0xE0;   // fUseDB, fDatabase, fSetLang: a database that cannot be used fails the login, as the caller named it
  body[25] = 0x02 | 0x01;   // fODBC (ANSI defaults on: ANSI_NULLS, QUOTED_IDENTIFIER, ...) and fLanguage
  set32(body.data() + 32, 0x0409);   // LCID en-US
  entry(36, u_host);
  entry(40, u_user);
  entry(44, u_pass);
  entry(48, u_app);
  entry(52, u_server);
  entry(60, u_lib);   // CltIntName
  entry(68, u_db);
  const uint8_t client_id[6] = {0x00, 0x50, 0x56, 0xC0, 0x00, 0x08};
  std::memcpy(body.data() + 72, client_id, 6);
  set16(body.data() + 78, uint16_t(kFixed + strings.size()));   // SSPI: none, at the end
  set16(body.data() + 82, uint16_t(kFixed + strings.size()));   // AtchDBFile: none
  set16(body.data() + 86, uint16_t(kFixed + strings.size()));   // ChangePassword: none
  body.insert(body.end(), strings.begin(), strings.end());
  set32(body.data(), uint32_t(body.size()));

  auto s = send_message(kPktLogin7, body.data(), body.size());
  if (!s.ok()) return s;
  uint8_t type = 0;
  auto resp = read_message(&type);
  if (!resp.ok()) return Result<void>(resp.error());
  if (type != kPktReply) return Result<void>(Status::Corrupt, "MS SQL Server did not answer LOGIN7");

  Rd r{resp->data(), resp->size()};
  bool login_ok = false;
  std::string err;
  while (r.pos < r.n && !r.bad) {
    const uint8_t token = r.u8();
    if (token == 0xAD) {   // LOGINACK: interface, TDS version, program name, program version
      const uint16_t k = r.u16();
      const size_t end = r.pos + k;
      r.skip(1);
      const uint8_t* tv = r.take(4);   // the TDS version, most significant byte first
      const uint32_t tds = tv ? (uint32_t(tv[0]) << 24) | (uint32_t(tv[1]) << 16) | (uint32_t(tv[2]) << 8) | tv[3] : 0;
      const std::string prog = r.b_varchar();
      const uint8_t* ver = r.take(4);
      if (r.bad || r.pos > end) break;
      r.pos = end;
      char v[48];
      if (ver) std::snprintf(v, sizeof v, " %u.%u.%u", ver[0], ver[1], unsigned(be16(ver + 2)));
      server_version_ = prog + (ver ? std::string(v) : std::string()) + (tds >= 0x74000004 ? " (TDS 7.4)" : " (TDS 7.x)");
      login_ok = true;
    } else if (token == 0xAA || token == 0xAB) {
      const uint16_t k = r.u16();
      const size_t end = r.pos + k;
      int num = 0;
      const std::string m = message_text(r, end, &num);
      if (token == 0xAA) err = m;
    } else if (token == 0xE3) {
      const uint16_t k = r.u16();
      const uint8_t* q = r.take(k);
      if (q) env_change(q, k);
    } else if (token == 0xAE) {   // FEATUREEXTACK: features we did not ask for, to its terminator
      for (;;) {
        const uint8_t f = r.u8();
        if (r.bad || f == 0xFF) break;
        r.skip(r.u32());
      }
    } else if (token == 0xFD || token == 0xFE || token == 0xFF) {
      r.skip(12);
    } else if (token == 0xE4) {   // SESSIONSTATE
      r.skip(r.u32());
    } else {
      return Result<void>(Status::Corrupt, "unexpected token 0x" + hex2(token) + " in the LOGIN7 answer");
    }
  }
  if (r.bad) return Result<void>(Status::Corrupt, "corrupt LOGIN7 answer");
  if (!login_ok) return Result<void>(Status::Denied, err.empty() ? "MS SQL Server rejected the login" : "MS SQL Server login: " + err);
  return Ok();
}

Result<void> TdsWireSession::select_db(const std::string& db) {
  if (db.empty() || db == db_) return Ok();
  std::string q = "USE [";
  for (char c : db) { if (c == ']') q += ']'; q += c; }   // ] doubled inside [...]
  q += "]";
  auto r = execute(q);
  if (!r.ok()) return r;
  db_ = db;   // ENVCHANGE has already said so; this is its name as asked for
  return Ok();
}

Result<void> TdsWireSession::ping() {
  return execute("SELECT 1");
}

Result<SqlWireResult> TdsWireSession::query(const std::string& sql, const SqlWireRowSink* sink) {
  if (!conn_ || !conn_->alive()) return Result<SqlWireResult>(Status::IoError, "no open MS SQL (TDS) connection");

  // ALL_HEADERS: the transaction descriptor header (the open transaction's,
  // 0 outside one) and one outstanding request; then the batch's text.
  std::vector<uint8_t> payload;
  put32(&payload, 22);   // total length of the headers
  put32(&payload, 18);   // this header's length
  put16(&payload, 2);    // transaction descriptor
  put64(&payload, txn_descriptor_);
  put32(&payload, 1);    // outstanding request count
  const auto u_sql = utf8_to_utf16le(sql);
  payload.insert(payload.end(), u_sql.begin(), u_sql.end());
  auto s = send_message(kPktSqlBatch, payload.data(), payload.size());
  if (!s.ok()) { close(); return Result<SqlWireResult>(s.error()); }

  uint8_t type = 0;
  auto resp = read_message(&type);
  if (!resp.ok()) { close(); return Result<SqlWireResult>(resp.error()); }
  if (type != kPktReply) { close(); return Result<SqlWireResult>(Status::Corrupt, "MS SQL Server's reply is not a tabular result"); }

  SqlWireResult res;
  res.ok = true;
  std::vector<Col> cols;
  bool sink_ok = true;
  bool statement_rows = false;   // the statement this DONE ends returned rows (its count is not rows changed)
  Rd r{resp->data(), resp->size()};
  // A reply this reader cannot follow to its end is not passed off as a
  // shorter one: the session is closed (the stream is out of step) and the
  // statement fails.
  auto corrupt = [&](const std::string& what) {
    close();
    return Result<SqlWireResult>(Status::Corrupt, "MS SQL reply: " + what);
  };
  auto emit = [&](std::vector<SqlWireValue>& row) {
    if (sink) { if (sink_ok) sink_ok = (*sink)(res.columns, row); }
    else res.rows.push_back(std::move(row));
  };
  while (r.pos < r.n) {
    const uint8_t token = r.u8();
    switch (token) {
      case 0x81: {   // COLMETADATA: a new result set (the last of several is the one kept)
        const uint16_t n = r.u16();
        if (n == 0xFFFF) break;   // none
        cols.clear();
        res.columns.clear();
        if (!sink) res.rows.clear();
        res.has_result_set = true;
        statement_rows = true;
        for (uint16_t i = 0; i < n; ++i) {
          Col c;
          r.skip(4);   // UserType
          r.skip(2);   // Flags
          if (!read_type_info(r, &c)) return corrupt("a column of a type this reader does not know (0x" + hex2(c.type) + ")");
          c.name = r.b_varchar();
          if (r.bad) return corrupt("short COLMETADATA");
          SqlWireColumn w;
          w.name = c.name;
          w.type = type_name(c.type);
          w.length = c.max_len == 0xFFFF ? 0 : c.max_len;
          w.decimals = c.scale;
          res.columns.push_back(w);
          cols.push_back(std::move(c));
        }
        break;
      }
      case 0xD1: {   // ROW
        std::vector<SqlWireValue> row(cols.size());
        for (size_t i = 0; i < cols.size(); ++i)
          if (!read_value(r, cols[i], &row[i])) return corrupt("a value of column " + cols[i].name);
        emit(row);
        break;
      }
      case 0xD2: {   // NBCROW: a bitmap of the NULL columns, then the others' values
        const size_t bm = (cols.size() + 7) / 8;
        const uint8_t* bits = r.take(bm);
        if (!bits) return corrupt("short NBCROW");
        std::vector<SqlWireValue> row(cols.size());
        for (size_t i = 0; i < cols.size(); ++i) {
          if (bits[i / 8] & (1u << (i % 8))) { row[i].is_null = true; continue; }
          if (!read_value(r, cols[i], &row[i])) return corrupt("a value of column " + cols[i].name);
        }
        emit(row);
        break;
      }
      case 0xAA: case 0xAB: {   // ERROR / INFO
        const uint16_t k = r.u16();
        const size_t end = r.pos + k;
        int num = 0;
        const std::string m = message_text(r, end, &num);
        if (r.bad) return corrupt("short ERROR token");
        if (token == 0xAA) {
          res.ok = false;
          if (res.error_message.empty()) res.error_message = m;   // the first: later ones usually follow from it
        }
        break;
      }
      case 0xE3: {   // ENVCHANGE
        const uint16_t k = r.u16();
        const uint8_t* q = r.take(k);
        if (!q) return corrupt("short ENVCHANGE");
        env_change(q, k);
        break;
      }
      case 0xA9:   // ORDER
      case 0xA5:   // COLINFO
      case 0xA4: { // TABNAME
        r.skip(r.u16());
        break;
      }
      case 0xE4:   // SESSIONSTATE
        r.skip(r.u32());
        break;
      case 0x79:   // RETURNSTATUS
        r.skip(4);
        break;
      case 0xFD: case 0xFE: case 0xFF: {   // DONE / DONEPROC / DONEINPROC: status, command, row count
        const uint16_t st = r.u16();
        r.skip(2);
        const uint64_t count = r.u64();
        if (r.bad) return corrupt("short DONE token");
        if ((st & 0x10) && !statement_rows) res.affected_rows += count;   // DONE_COUNT of an INSERT/UPDATE/DELETE
        statement_rows = false;
        if ((st & 0x02) && res.ok) {   // DONE_ERROR without an ERROR token before it
          res.ok = false;
          res.error_message = "the batch failed on MS SQL Server";
        }
        break;
      }
      default:
        return corrupt("an unexpected token 0x" + hex2(token));
    }
    if (r.bad) return corrupt("a token cut short");
  }
  return res;
}

}  // namespace aios::userland
