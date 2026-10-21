// AIOS — Native MySQL / MariaDB Binlog CDC Streaming Consumer Implementation

#include "aios/userland/mysql_binlog.hpp"

#include <cerrno>
#include <cstdlib>
#include "aios/userland/mysql_wire.hpp"
#include "aios/userland/archive.hpp"
#include "aios/core/bytes.hpp"
#include "aios/hal/host.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <sstream>

#if defined(_WIN32)
// gmtime_s takes (dest, source) and returns errno_t: the reverse order and a
// different success value from POSIX gmtime_r. This adapts the call, not the
// callers below.
static struct tm* gmtime_r(const time_t* t, struct tm* out) { return ::gmtime_s(out, t) == 0 ? out : nullptr; }
#endif

namespace aios::userland {

namespace {

// Helper: current timestamp in seconds
uint64_t now_seconds() {
  return (uint64_t)std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// Convert string to uppercase
std::string to_upper_str(std::string s) {
  for (char& c : s) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
  }
  return s;
}

// Convert string to lowercase
std::string to_lower_str(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  }
  return s;
}

// Trim whitespace
std::string trim_str(const std::string& s) {
  size_t start = 0;
  while (start < s.size() && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r' || s[start] == '\n')) {
    ++start;
  }
  size_t end = s.size();
  while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n' || s[end - 1] == ';')) {
    --end;
  }
  return s.substr(start, end - start);
}

// Binary decode helpers
uint16_t read_le16(const uint8_t* p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

uint32_t read_le24(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

uint32_t read_le32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// A binlog file name as the source names it, "<base>.<digits>". A position saved before the ROTATE checksum fix
// carried the event's CRC32 after the name ("mariadb-bin.000655" and four bytes): the source has no such file and
// every reconnect failed with "Could not find first log file name". The name is cut at the first byte no binlog
// name has, then after the sequence number's digits.
std::string clean_binlog_name(const std::string& raw) {
  std::string s;
  for (char ch : raw) {
    const unsigned char u = (unsigned char)ch;
    if (!(std::isalnum(u) || ch == '.' || ch == '_' || ch == '-')) break;
    s += ch;
  }
  const size_t dot = s.rfind('.');
  if (dot != std::string::npos && dot + 1 < s.size() && std::isdigit((unsigned char)s[dot + 1])) {
    size_t end = dot + 1;
    while (end < s.size() && std::isdigit((unsigned char)s[end])) ++end;
    s.resize(end);
  }
  return s;
}

uint64_t read_le48(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 6; ++i) {
    v |= ((uint64_t)p[i] << (8 * i));
  }
  return v;
}

uint64_t read_le64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= ((uint64_t)p[i] << (8 * i));
  }
  return v;
}

// Big-endian readers: MySQL/MariaDB's packed temporal types (TIME2,
// DATETIME2, TIMESTAMP2) are stored big-endian on the wire, unlike every
// other integer type in the row-event format.
uint32_t read_be24(const uint8_t* p) {
  return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

uint32_t read_be32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

uint64_t read_be40(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 5; ++i) v = (v << 8) | p[i];
  return v;
}

// "YYYY-MM-DD HH:MM:SS", zero-padded, no fractional part (fsp handling is
// appended by the caller when meta says the column carries one).
std::string format_datetime(int year, int month, int day, int hour, int minute, int second) {
  char buf[20];
  int n = std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", year, month, day, hour, minute, second);
  return std::string(buf, n > 0 ? (size_t)n : 0);
}

std::string format_date(int year, int month, int day) {
  char buf[12];
  int n = std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", year, month, day);
  return std::string(buf, n > 0 ? (size_t)n : 0);
}

// Number of metadata/fractional-seconds bytes a TIME2/DATETIME2/TIMESTAMP2
// value carries beyond its fixed part, per fsp (0-6).
size_t frac_bytes_for_fsp(uint8_t fsp) {
  return (fsp + 1) / 2;
}

// The fractional seconds that follow a DATETIME2 / TIMESTAMP2 value: 1, 2 or
// 3 big-endian bytes in hundredths, ten-thousandths or millionths of a
// second, printed to the column's own precision (".678" for DATETIME(3)).
// Empty for fsp 0. The bytes used to be skipped, so every replicated
// DATETIME(3) arrived whole-second where the seeded rows kept their fraction.
std::string fraction_suffix(const uint8_t* fp, uint8_t fsp) {
  if (fsp == 0) return std::string();
  const size_t fb = frac_bytes_for_fsp(fsp);
  uint32_t frac = 0;
  for (size_t i = 0; i < fb; ++i) frac = (frac << 8) | fp[i];
  uint32_t micros = fb == 1 ? frac * 10000u : fb == 2 ? frac * 100u : frac;
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%06u", micros);
  return "." + std::string(buf, fsp > 6 ? 6 : fsp);
}

// DATETIME2: 5 bytes big-endian packed (sign-biased) + frac_bytes_for_fsp(fsp).
// Packed layout (after removing the 0x8000000000 bias):
//   ymdhms = ((year*13+month)*32+day)*(1<<17) + (hour*64+minute)*64+second
std::string decode_datetime2(const uint8_t* p, uint8_t fsp = 0) {
  int64_t raw = (int64_t)read_be40(p) - 0x8000000000LL;
  int64_t ymd = raw >> 17;
  int64_t hms = raw - (ymd << 17);
  int64_t ym = ymd >> 5;
  int day = (int)(ymd - (ym << 5));
  int month = (int)(ym % 13);
  int year = (int)(ym / 13);
  int second = (int)(hms % 64);
  int minute = (int)((hms >> 6) % 64);
  int hour = (int)(hms >> 12);
  return format_datetime(year, month, day, hour, minute, second) + fraction_suffix(p + 5, fsp);
}

// Legacy DATETIME (pre-5.6.4): 8 bytes little-endian, YYYYMMDDHHMMSS packed
// as a decimal-weighted integer.
std::string decode_datetime_legacy(const uint8_t* p) {
  int64_t v = (int64_t)read_le64(p);
  int64_t d = v / 1000000;
  int64_t t = v % 1000000;
  int year = (int)(d / 10000);
  int month = (int)((d / 100) % 100);
  int day = (int)(d % 100);
  int hour = (int)(t / 10000);
  int minute = (int)((t / 100) % 100);
  int second = (int)(t % 100);
  return format_datetime(year, month, day, hour, minute, second);
}

// DATE: 3 bytes little-endian packed as year*16*32 + month*32 + day.
std::string decode_date(const uint8_t* p) {
  uint32_t v = read_le24(p);
  int day = (int)(v & 31);
  int month = (int)((v >> 5) & 15);
  int year = (int)(v >> 9);
  return format_date(year, month, day);
}

// TIMESTAMP2: 4 bytes big-endian unix epoch seconds (UTC) + fractional bytes.
std::string decode_timestamp2(const uint8_t* p, uint8_t fsp = 0) {
  uint32_t epoch = read_be32(p);
  time_t t = (time_t)epoch;
  struct tm tmv;
  gmtime_r(&t, &tmv);
  return format_datetime(tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec) +
         fraction_suffix(p + 4, fsp);
}

// Legacy TIMESTAMP: 4 bytes little-endian unix epoch seconds.
std::string decode_timestamp_legacy(const uint8_t* p) {
  uint32_t epoch = read_le32(p);
  time_t t = (time_t)epoch;
  struct tm tmv;
  gmtime_r(&t, &tmv);
  return format_datetime(tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

// YEAR: 1 byte, stored as (year - 1900), 0 means the zero-year.
std::string decode_year(uint8_t b) {
  if (b == 0) return "0000";
  return std::to_string(1900 + (int)b);
}

// Legacy TIME: 3 bytes little-endian signed, packed as sign*(HH*10000+MM*100+SS).
std::string decode_time_legacy(const uint8_t* p) {
  int32_t v = (int32_t)read_le24(p);
  if (v & 0x00800000) v |= (int32_t)0xFF000000;  // sign-extend the 24-bit value
  bool neg = v < 0;
  if (neg) v = -v;
  int h = v / 10000, m = (v / 100) % 100, s = v % 100;
  char buf[16];
  int n = std::snprintf(buf, sizeof(buf), "%s%02d:%02d:%02d", neg ? "-" : "", h, m, s);
  return std::string(buf, n > 0 ? (size_t)n : 0);
}

// TIME2: 3 bytes big-endian packed (bias 0x800000) + fractional bytes. The
// integer part and the fraction are one signed number (my_time_packed_from_
// binary): a negative time's fraction bytes count down from the next whole
// second, so "-05:06:07.250" is stored as -(5:06:08) with fraction 750000.
// Read as the same single number, then split - as the server does.
std::string decode_time2(const uint8_t* p, uint8_t fsp = 0) {
  const size_t fb = frac_bytes_for_fsp(fsp);
  int64_t packed;   // (hms << 24) + microseconds, signed
  if (fb == 0) {
    packed = ((int64_t)read_be24(p) - 0x800000) << 24;
  } else if (fb == 1) {
    int64_t intpart = (int64_t)read_be24(p) - 0x800000;
    int64_t frac = p[3];
    if (intpart < 0 && frac) { intpart++; frac -= 0x100; }
    packed = (intpart << 24) + frac * 10000;
  } else if (fb == 2) {
    int64_t intpart = (int64_t)read_be24(p) - 0x800000;
    int64_t frac = ((int64_t)p[3] << 8) | p[4];
    if (intpart < 0 && frac) { intpart++; frac -= 0x10000; }
    packed = (intpart << 24) + frac * 100;
  } else {
    int64_t v = 0;
    for (int i = 0; i < 6; ++i) v = (v << 8) | p[i];
    packed = v - 0x800000000000LL;
  }
  const bool neg = packed < 0;
  if (neg) packed = -packed;
  const int64_t hms = packed >> 24;
  const uint32_t micros = (uint32_t)(packed & 0xFFFFFF);
  int second = (int)(hms % 64);
  int minute = (int)((hms >> 6) % 64);
  int hour = (int)(hms >> 12);
  char buf[32];
  int n = std::snprintf(buf, sizeof(buf), "%s%02d:%02d:%02d", neg ? "-" : "", hour, minute, second);
  std::string out(buf, n > 0 ? (size_t)n : 0);
  if (fsp > 0) {
    char fr[8];
    std::snprintf(fr, sizeof(fr), "%06u", micros);
    out += "." + std::string(fr, fsp > 6 ? 6 : fsp);
  }
  return out;
}

// NEWDECIMAL: MySQL's packed-BCD fixed point format. meta = (precision<<8)|scale.
// Digits are stored in big-endian 9-digit groups (4 bytes each), with any
// remainder group (< 9 digits) using a narrower width from dig2bytes, placed
// at the outer edge of the integer/fraction split. The sign is folded into
// the leading byte: positive numbers have their top bit set; negative
// numbers are fully bit-complemented (two's-complement-style) with the top
// bit then clear.
size_t new_decimal_byte_length(uint16_t meta) {
  static const int dig2bytes[10] = {0, 1, 1, 2, 2, 3, 3, 4, 4, 4};
  int precision = (int)(meta >> 8);
  int scale = (int)(meta & 0xff);
  int intg = precision - scale;
  int intg0 = intg / 9, frac0 = scale / 9;
  int intg0x = intg - intg0 * 9, frac0x = scale - frac0 * 9;
  if (intg0x < 0 || intg0x > 9 || frac0x < 0 || frac0x > 9) return 0;
  return (size_t)(intg0 * 4 + dig2bytes[intg0x] + frac0 * 4 + dig2bytes[frac0x]);
}

// FLOAT and DOUBLE as the shortest text that reads back as the same value -
// what MariaDB itself prints. std::to_string gave six fixed places:
// 3.14159265 arrived as 3.141593, 1e-7 as 0.000000, 1e20 with 21 digits.
// FLOAT and DOUBLE printed the way MariaDB prints them (FLOAT to 6
// significant digits, DOUBLE shortest round-trip), so a replicated value reads
// exactly as the source shows it. std::to_string gave six fixed places:
// 3.14159265 arrived as 3.141593, 1e-7 as 0.000000, 1e20 with 21 digits.
std::string shortest_float_text(float f) { return sql_format_double((double)f, 6); }
std::string shortest_double_text(double d) { return sql_format_double(d, 0); }

// Parse ENUM or SET member values from column type definition like "enum('ftp','api')"
std::vector<std::string> parse_mysql_enum_values(const std::string& type_str) {
  std::vector<std::string> values;
  size_t start = type_str.find('(');
  size_t end = type_str.rfind(')');
  if (start == std::string::npos || end == std::string::npos || end <= start) {
    return values;
  }
  std::string inner = type_str.substr(start + 1, end - start - 1);
  bool in_quote = false;
  std::string cur;
  for (size_t i = 0; i < inner.size(); ++i) {
    char ch = inner[i];
    if (!in_quote) {
      if (ch == '\'') {
        in_quote = true;
        cur.clear();
      }
    } else {
      if (ch == '\\' && i + 1 < inner.size()) {
        cur += inner[++i];
      } else if (ch == '\'' && i + 1 < inner.size() && inner[i + 1] == '\'') {
        cur += '\'';
        ++i;
      } else if (ch == '\'') {
        in_quote = false;
        values.push_back(cur);
      } else {
        cur += ch;
      }
    }
  }
  return values;
}

std::string decode_new_decimal(const uint8_t* p, uint16_t meta, size_t* consumed) {
  static const int dig2bytes[10] = {0, 1, 1, 2, 2, 3, 3, 4, 4, 4};
  int precision = (int)(meta >> 8);
  int scale = (int)(meta & 0xff);
  int intg = precision - scale;
  int intg0 = intg / 9, frac0 = scale / 9;
  int intg0x = intg - intg0 * 9, frac0x = scale - frac0 * 9;
  size_t total = new_decimal_byte_length(meta);
  *consumed = total;
  if (total == 0) return "0";

  std::vector<uint8_t> buf(p, p + total);
  bool positive = (buf[0] & 0x80) != 0;
  buf[0] ^= 0x80;
  if (!positive) {
    for (auto& b : buf) b = (uint8_t)~b;
  }

  size_t pos = 0;
  std::string out;
  if (!positive) out += '-';

  bool any_digit = false;
  if (dig2bytes[intg0x] > 0) {
    uint32_t v = 0;
    for (int i = 0; i < dig2bytes[intg0x]; ++i) v = (v << 8) | buf[pos++];
    if (v != 0 || intg0 > 0) { out += std::to_string(v); any_digit = true; }
  }
  for (int i = 0; i < intg0; ++i) {
    uint32_t v = read_be32(&buf[pos]);
    pos += 4;
    if (any_digit) {
      char tmp[16];
      std::snprintf(tmp, sizeof(tmp), "%09u", v);
      out += tmp;
    } else if (v != 0 || i == intg0 - 1) {
      out += std::to_string(v);
      any_digit = true;
    }
  }
  if (!any_digit) out += '0';

  if (scale > 0) {
    out += '.';
    for (int i = 0; i < frac0; ++i) {
      uint32_t v = read_be32(&buf[pos]);
      pos += 4;
      char tmp[16];
      std::snprintf(tmp, sizeof(tmp), "%09u", v);
      out += tmp;
    }
    if (dig2bytes[frac0x] > 0) {
      uint32_t v = 0;
      for (int i = 0; i < dig2bytes[frac0x]; ++i) v = (v << 8) | buf[pos++];
      char tmp[16];
      std::snprintf(tmp, sizeof(tmp), "%0*u", frac0x, v);
      out += tmp;
    }
  }
  return out;
}

uint64_t read_lenenc(const uint8_t* p, size_t max_len, size_t* bytes_read) {
  if (max_len == 0) {
    if (bytes_read) *bytes_read = 0;
    return 0;
  }
  uint8_t first = p[0];
  if (first < 0xfb) {
    if (bytes_read) *bytes_read = 1;
    return first;
  }
  if (first == 0xfc) {
    if (max_len < 3) {
      if (bytes_read) *bytes_read = max_len;
      return 0;
    }
    if (bytes_read) *bytes_read = 3;
    return read_le16(p + 1);
  }
  if (first == 0xfd) {
    if (max_len < 4) {
      if (bytes_read) *bytes_read = max_len;
      return 0;
    }
    if (bytes_read) *bytes_read = 4;
    return read_le24(p + 1);
  }
  if (first == 0xfe) {
    if (max_len < 9) {
      if (bytes_read) *bytes_read = max_len;
      return 0;
    }
    if (bytes_read) *bytes_read = 9;
    return read_le64(p + 1);
  }
  if (bytes_read) *bytes_read = 1;
  return 0;
}

void write_le16(std::vector<uint8_t>* v, uint16_t x) {
  v->push_back((uint8_t)(x & 0xff));
  v->push_back((uint8_t)((x >> 8) & 0xff));
}

void write_le24(std::vector<uint8_t>* v, uint32_t x) {
  v->push_back((uint8_t)(x & 0xff));
  v->push_back((uint8_t)((x >> 8) & 0xff));
  v->push_back((uint8_t)((x >> 16) & 0xff));
}

void write_le32(std::vector<uint8_t>* v, uint32_t x) {
  v->push_back((uint8_t)(x & 0xff));
  v->push_back((uint8_t)((x >> 8) & 0xff));
  v->push_back((uint8_t)((x >> 16) & 0xff));
  v->push_back((uint8_t)((x >> 24) & 0xff));
}

void write_le64(std::vector<uint8_t>* v, uint64_t x) {
  for (int i = 0; i < 8; ++i) {
    v->push_back((uint8_t)((x >> (8 * i)) & 0xff));
  }
}

void write_bytes(std::vector<uint8_t>* v, const std::string& s) {
  v->insert(v->end(), s.begin(), s.end());
}

}  // namespace

// ============================================================================
// BinlogConsumer Implementation
// ============================================================================

BinlogConsumer::BinlogConsumer(Kernel& kernel, SqlDatabase& db,
                               const BinlogConsumerConfig& config)
    : kernel_(kernel), db_(db), config_(config) {
  config_.binlog_filename = clean_binlog_name(config_.binlog_filename);
  active_binlog_file_ = config_.binlog_filename;
  active_binlog_pos_ = config_.binlog_position;
}

BinlogConsumer::~BinlogConsumer() {
  stop();
}

void BinlogConsumer::configure(const BinlogConsumerConfig& config) {
  hal::LockGuard lock(mu_);
  config_ = config;
  config_.binlog_filename = clean_binlog_name(config_.binlog_filename);
  if (!config_.binlog_filename.empty()) {
    active_binlog_file_ = config_.binlog_filename;
  }
  active_binlog_pos_ = config_.binlog_position;
}

void BinlogConsumer::set_table_whitelist(const std::vector<std::string>& tables) {
  hal::LockGuard lock(mu_);
  config_.table_whitelist.clear();
  for (const auto& t : tables) {
    config_.table_whitelist.insert(to_lower_str(trim_str(t)));
  }
}

void BinlogConsumer::set_column_whitelist(const std::string& table,
                                          const std::vector<std::string>& columns) {
  hal::LockGuard lock(mu_);
  std::string tbl_low = to_lower_str(trim_str(table));
  std::unordered_set<std::string> cols;
  for (const auto& c : columns) {
    cols.insert(to_lower_str(trim_str(c)));
  }
  config_.column_whitelists[tbl_low] = cols;
}

void BinlogConsumer::set_table_max_rows(const std::string& table, size_t max_rows) {
  hal::LockGuard lock(mu_);
  std::string tbl_low = to_lower_str(trim_str(table));
  config_.table_max_rows[tbl_low] = max_rows;
}

// A transaction that changed the mirror has committed: put the position
// just past it into the redo log, beside the statements it produced, so a
// restart that replays those statements also resumes after them - not from
// the last store flush (up to five minutes back), which re-applied every
// change since and doubled rows in any table without a key. Positions go in
// only at commits: resuming inside a transaction would skip its TABLE_MAP.
void BinlogConsumer::note_commit() {
  {
    hal::LockGuard lock(mu_);
    committed_file_ = active_binlog_file_;
    committed_pos_ = active_binlog_pos_;
  }
  if (!applied_since_commit_) return;
  applied_since_commit_ = false;
  db_.record_cdc_commit(active_binlog_file_, active_binlog_pos_);
}

bool BinlogConsumer::committed_position(std::string* file, uint64_t* pos) const {
  hal::LockGuard lock(mu_);
  if (committed_file_.empty() || committed_pos_ == 0) return false;
  *file = committed_file_; *pos = committed_pos_;
  return true;
}

void BinlogConsumer::set_table_seed_position(const std::string& table, const std::string& file, uint64_t pos) {
  hal::LockGuard lock(mu_);
  config_.table_seed_positions[to_lower_str(trim_str(table))] = {file, pos};
}

BinlogConsumerStats BinlogConsumer::stats() const {
  hal::LockGuard lock(mu_);
  BinlogConsumerStats st;
  st.is_running = running_;
  st.is_connected = connected_;
  st.current_binlog_file = active_binlog_file_;
  st.current_binlog_pos = active_binlog_pos_;
  st.total_events_received = total_events_received_;
  st.total_events_applied = total_events_applied_;
  st.total_events_skipped = total_events_skipped_;
  st.total_apply_failures = total_apply_failures_;
  st.total_inserts = total_inserts_;
  st.total_updates = total_updates_;
  st.total_deletes = total_deletes_;
  st.total_ddl_executed = total_ddl_;
  st.total_row_evictions = total_evictions_;
  st.last_event_timestamp = last_event_ts_;
  st.seconds_behind_master = seconds_behind_master_;
  st.last_error = last_error_;
  return st;
}

// Which table a DDL statement acts on: CREATE [TEMPORARY] TABLE [IF NOT
// EXISTS] t, ALTER [ONLINE|IGNORE] TABLE t, DROP [TEMPORARY] TABLE [IF EXISTS]
// t, TRUNCATE [TABLE] t - each t possibly `db`.`t`. False for anything else
// (databases, views, routines, grants, RENAME, a DROP of several tables),
// which a mirror of chosen tables has no business running.
bool BinlogConsumer::parse_table_ddl(const std::string& q, std::string* verb, std::string* db, std::string* table) {
  size_t i = 0;
  auto skip_ws = [&] {
    for (;;) {
      while (i < q.size() && (q[i] == ' ' || q[i] == '\t' || q[i] == '\n' || q[i] == '\r')) ++i;
      if (i + 1 < q.size() && q[i] == '/' && q[i + 1] == '*') {        // /* comment */ or /*!version ... */
        size_t e = q.find("*/", i + 2);
        if (e == std::string::npos) return;
        i = e + 2;
        continue;
      }
      return;
    }
  };
  auto word = [&]() -> std::string {
    skip_ws();
    size_t b = i;
    while (i < q.size() && (std::isalnum((unsigned char)q[i]) || q[i] == '_')) ++i;
    return to_upper_str(q.substr(b, i - b));
  };
  auto ident = [&]() -> std::string {
    skip_ws();
    std::string out;
    if (i < q.size() && q[i] == '`') {
      ++i;
      while (i < q.size()) {
        if (q[i] == '`') { if (i + 1 < q.size() && q[i + 1] == '`') { out += '`'; i += 2; continue; } ++i; break; }
        out += q[i++];
      }
    } else {
      while (i < q.size() && (std::isalnum((unsigned char)q[i]) || q[i] == '_' || q[i] == '$')) out += q[i++];
    }
    return out;
  };
  std::string w = word();
  *verb = w;
  if (w == "CREATE") {
    std::string n = word();
    if (n == "OR") { word(); n = word(); }                           // CREATE OR REPLACE TABLE
    if (n == "TEMPORARY") return false;                               // session-scoped, never in the mirror
    if (n != "TABLE") return false;
    size_t save = i;
    if (word() == "IF") { word(); word(); } else i = save;            // IF NOT EXISTS
  } else if (w == "ALTER") {
    std::string n = word();
    while (n == "ONLINE" || n == "IGNORE") n = word();
    if (n != "TABLE") return false;
  } else if (w == "DROP") {
    std::string n = word();
    if (n == "TEMPORARY") return false;
    if (n != "TABLE") return false;
    size_t save = i;
    if (word() == "IF") word(); else i = save;                        // IF EXISTS
  } else if (w == "TRUNCATE") {
    size_t save = i;
    if (word() != "TABLE") i = save;
  } else {
    return false;
  }
  std::string a = ident();
  if (a.empty()) return false;
  skip_ws();
  if (i < q.size() && q[i] == '.') { ++i; *db = a; *table = ident(); }
  else { db->clear(); *table = a; }
  if (table->empty()) return false;
  skip_ws();
  if (w == "DROP" && i < q.size() && q[i] == ',') return false;       // several tables in one DROP
  return true;
}

bool BinlogConsumer::parse_view_ddl(const std::string& q, std::string* verb,
                                    std::vector<std::pair<std::string, std::string>>* names) {
  size_t i = 0;
  auto skip_ws = [&] { while (i < q.size() && (q[i] == ' ' || q[i] == '\t' || q[i] == '\n' || q[i] == '\r')) ++i; };
  auto word = [&]() -> std::string {
    skip_ws();
    size_t b = i;
    while (i < q.size() && (std::isalnum((unsigned char)q[i]) || q[i] == '_')) ++i;
    return to_upper_str(q.substr(b, i - b));
  };
  auto ident = [&]() -> std::string {
    skip_ws();
    std::string out;
    if (i < q.size() && q[i] == '`') {
      ++i;
      while (i < q.size()) {
        if (q[i] == '`') { if (i + 1 < q.size() && q[i + 1] == '`') { out += '`'; i += 2; continue; } ++i; break; }
        out += q[i++];
      }
    } else {
      while (i < q.size() && (std::isalnum((unsigned char)q[i]) || q[i] == '_' || q[i] == '$')) out += q[i++];
    }
    return out;
  };
  auto qualified = [&]() -> std::pair<std::string, std::string> {
    std::string a = ident();
    skip_ws();
    if (!a.empty() && i < q.size() && q[i] == '.') { ++i; return {a, ident()}; }
    return {std::string(), a};
  };
  // DEFINER = `user`@`host` | CURRENT_USER[()]: read past, quotes and all.
  auto skip_definer_value = [&] {
    skip_ws();
    bool quoted = false;
    while (i < q.size() && (quoted || !(q[i] == ' ' || q[i] == '\t' || q[i] == '\n' || q[i] == '\r'))) {
      if (q[i] == '`') quoted = !quoted;
      ++i;
    }
  };
  const std::string w = word();
  *verb = w;
  if (w == "DROP") {
    if (word() != "VIEW") return false;
    size_t save = i;
    if (word() == "IF") word(); else i = save;                        // IF EXISTS
    for (;;) {
      auto n = qualified();
      if (n.second.empty()) return false;
      names->push_back(n);
      skip_ws();
      if (i < q.size() && q[i] == ',') { ++i; continue; }
      break;
    }
    return !names->empty();
  }
  if (w != "CREATE" && w != "ALTER") return false;
  std::string n = word();
  if (n == "OR") { word(); n = word(); }                              // CREATE OR REPLACE
  for (int guard = 0; guard < 6 && n != "VIEW"; ++guard) {
    if (n == "ALGORITHM") { skip_ws(); if (i < q.size() && q[i] == '=') ++i; word(); }
    else if (n == "DEFINER") { skip_ws(); if (i < q.size() && q[i] == '=') ++i; skip_definer_value(); }
    else if (n == "SQL") { word(); word(); }                          // SECURITY DEFINER|INVOKER
    else return false;
    n = word();
  }
  if (n != "VIEW") return false;
  size_t save = i;
  if (word() == "IF") { word(); word(); } else i = save;              // IF NOT EXISTS
  auto nm = qualified();
  if (nm.second.empty()) return false;
  names->push_back(nm);
  return true;
}

// The source database this mirror follows: SOURCE_DATABASE / cdc_db. Empty,
// or the engine's own default name, means none was configured.
bool BinlogConsumer::in_source_schema(const std::string& schema) const {
  if (config_.database.empty() || to_lower_str(config_.database) == "aios_db") return true;
  return to_lower_str(schema) == to_lower_str(config_.database);
}

bool BinlogConsumer::is_whitelisted(const std::string& schema, const std::string& table) const {
  // A mirror follows ONE source database. The binlog carries every database
  // on the server: two tenants each with a `salesman` table were one table
  // here, whichever wrote last.
  if (!in_source_schema(schema)) return false;
  if (config_.table_whitelist.empty()) return true;
  std::string tbl_low = to_lower_str(table);
  if (config_.table_whitelist.count(tbl_low)) return true;
  if (!schema.empty()) {
    std::string full_low = to_lower_str(schema + "." + table);
    if (config_.table_whitelist.count(full_low)) return true;
  }
  return false;
}

void BinlogConsumer::enforce_table_limits(const std::string& table_name) {
  std::string tbl_low = to_lower_str(table_name);
  auto it = config_.table_max_rows.find(tbl_low);
  if (it == config_.table_max_rows.end() || it->second == 0) return;

  size_t max_r = it->second;
  const SqlTable* tbl = db_.find_table(table_name);
  if (!tbl || tbl->row_count() <= max_r) return;

  size_t excess = tbl->row_count() - max_r;
  if (excess > 0) {
    // FIFO trim: delete oldest rows
    std::string sql = "DELETE FROM " + table_name + " LIMIT " + std::to_string(excess) + ";";
    db_.execute_local_query(sql);
    total_evictions_ += excess;
  }
}

Result<void> BinlogConsumer::write_packet(const uint8_t* data, size_t n, uint8_t seq) {
  if (!conn_ || !conn_->alive()) return Error(Status::IoError, "Connection closed");
  std::vector<uint8_t> header;
  header.reserve(4);
  write_le24(&header, (uint32_t)n);
  header.push_back(seq);

  if (!conn_->send_all(ByteSpan(header.data(), 4))) {
    return Error(Status::IoError, "failed writing packet header");
  }
  if (n > 0 && data) {
    if (!conn_->send_all(ByteSpan(data, n))) {
      return Error(Status::IoError, "failed writing packet body");
    }
  }
  return Ok();
}

Result<size_t> BinlogConsumer::read_packet(std::vector<uint8_t>* out, uint8_t* seq) {
  if (!conn_ || !conn_->alive()) return Error(Status::IoError, "Connection closed");
  uint8_t h[4];
  if (!conn_->recv_all(MutableByteSpan(h, 4), 10000)) {
    return Error(Status::IoError, "timed out reading packet header");
  }

  size_t len = read_le24(h);
  if (seq) *seq = h[3];
  out->resize(len);
  if (len > 0) {
    if (!conn_->recv_all(MutableByteSpan(out->data(), len), 10000)) {
      return Error(Status::IoError, "timed out reading packet payload");
    }
  }
  // In a COM_BINLOG_DUMP stream MariaDB prefixes EVERY packet with a one-byte
  // status marker, and it is NOT always 0x00: the live stream carries 0x1f
  // (SERVER_MORE_RESULTS_EXISTS|SERVER_STATUS_IN_TRANS) and 0x01
  // (SERVER_STATUS_IN_TRANS) just as often. Measured on st: of 610 packets, 405
  // began with 0x1f, 133 with 0x00, 35 with 0x01. Stripping only the 0x00 case
  // handed the real events to handle_event() with the marker still on front, so
  // event_data[4] read a timestamp byte instead of the event type and every event
  // decoded as type 0 — silently skipped, no TABLE_MAP, no ROWS applied, cursor
  // frozen forever (inserts=0, read=111, table_maps=1, pos 67254).
  // A real event payload starts with a 4-byte LE timestamp whose high byte is
  // 0x00 for any date before 2106, so a genuine header has out->at(3) == 0x00
  // followed by a plausible event type. That distinguishes the two cases.
  // Only in the dump stream. Applied to every packet, it also ate the first
  // byte of the server's greeting (0x0a, protocol 10) and of every reply
  // during login, so after 3e736b13 the consumer could never connect:
  // "Expected HandshakeV10 protocol packet", retried forever.
  packet_kind_ = PacketKind::Event;
  if (in_dump_stream_ && len > 0) {
    // ERR (0xff + code + '#' + state + message) and EOF (0xfe, under 9
    // bytes) are told by the byte as sent; everything else is an event.
    if ((*out)[0] == 0xff) packet_kind_ = PacketKind::Err;
    else if ((*out)[0] == 0xfe && len < 9) packet_kind_ = PacketKind::Eof;
  }
  if (in_dump_stream_ && len > 4 && packet_kind_ == PacketKind::Event) {
    bool looks_like_header = ((*out)[3] == 0x00 && (*out)[4] > 0x00 &&
                              (*out)[4] <= 0x27 && out->at(5) <= 0x27);
    if (!looks_like_header) {
      out->erase(out->begin());
      len--;
    }
  }
  return len;
}

Result<void> BinlogConsumer::connect_and_authenticate() {
  in_dump_stream_ = false;                       // login and setup replies are plain packets
  // A host that is a path is the master's unix socket (/run/mysqld/mysqld.sock),
  // as MariaWireSession reads it; it used to go to getaddrinfo and never connect.
  auto conn_res = (!config_.host.empty() && config_.host[0] == '/')
                      ? hal::net_connect_unix(config_.host, 10000)
                      : hal::net_connect(config_.host, config_.port, 10000);
  if (!conn_res.ok()) return conn_res.status();
  conn_ = std::move(*conn_res);

  // Read HandshakeV10
  std::vector<uint8_t> pkt;
  uint8_t seq = 0;
  auto pr = read_packet(&pkt, &seq);
  if (!pr.ok()) return pr.status();

  if (pkt.empty() || pkt[0] != 0x0a) {
    return Error(Status::Unsupported, "Expected HandshakeV10 protocol packet");
  }

  size_t idx = 1;
  while (idx < pkt.size() && pkt[idx] != 0) idx++;
  idx++; // skip null
  if (idx + 4 > pkt.size()) return Error(Status::Corrupt, "Handshake truncated");
  uint32_t conn_id = read_le32(pkt.data() + idx);
  (void)conn_id;
  idx += 4;

  uint8_t salt[64];
  size_t salt_len = 0;
  if (idx + 8 <= pkt.size()) {
    std::memcpy(salt, pkt.data() + idx, 8);
    salt_len = 8;
    idx += 8;
  }
  idx += 1; // filter 0x00

  [[maybe_unused]] uint32_t caps = 0;
  if (idx + 2 <= pkt.size()) {
    caps |= read_le16(pkt.data() + idx);
    idx += 2;
  }
  if (idx < pkt.size()) idx++; // charset
  if (idx + 2 <= pkt.size()) idx += 2; // status
  if (idx + 2 <= pkt.size()) {
    caps |= ((uint32_t)read_le16(pkt.data() + idx) << 16);
    idx += 2;
  }
  uint8_t auth_plugin_data_len = 0;
  if (idx < pkt.size()) {
    auth_plugin_data_len = pkt[idx++];
  }
  idx += 10; // reserved

  if (auth_plugin_data_len > 0 && idx < pkt.size()) {
    size_t rest_salt = (auth_plugin_data_len > 8) ? (auth_plugin_data_len - 8) : 12;
    if (idx + rest_salt <= pkt.size()) {
      size_t copy_n = std::min(rest_salt, sizeof(salt) - salt_len);
      std::memcpy(salt + salt_len, pkt.data() + idx, copy_n);
      salt_len += copy_n;
      idx += rest_salt;
    }
  }

  std::string plugin_name = "mysql_native_password";
  if (idx < pkt.size()) {
    const char* p = (const char*)pkt.data() + idx;
    size_t pl = std::strlen(p);
    if (pl > 0) plugin_name = std::string(p, pl);
  }

  // Scramble password
  std::vector<uint8_t> scrambled;
  if (!config_.password.empty()) {
    if (plugin_name == "caching_sha2_password") {
      scrambled = caching_sha2_scramble(config_.password, salt, salt_len);
    } else {
      scrambled = mysql_native_scramble(config_.password, salt, salt_len);
    }
  }

  // HandshakeResponse41
  std::vector<uint8_t> resp;
  uint32_t client_caps = 0x00000001 | 0x00000200 | 0x00008000 | 0x00080000;
  if (!config_.database.empty()) client_caps |= 0x00000008; // CAP_CONNECT_WITH_DB

  write_le32(&resp, client_caps);
  write_le32(&resp, 0x01000000); // 16MB max packet
  resp.push_back(33); // utf8_general_ci
  resp.insert(resp.end(), 23, 0); // filler

  write_bytes(&resp, config_.user);
  resp.push_back(0); // null terminator

  if (scrambled.empty()) {
    resp.push_back(0);
  } else {
    resp.push_back((uint8_t)scrambled.size());
    resp.insert(resp.end(), scrambled.begin(), scrambled.end());
  }

  if (!config_.database.empty()) {
    write_bytes(&resp, config_.database);
    resp.push_back(0);
  }

  write_bytes(&resp, plugin_name);
  resp.push_back(0);

  auto wr = write_packet(resp.data(), resp.size(), seq + 1);
  if (!wr.ok()) return wr;

  // Read server auth result
  pkt.clear();
  pr = read_packet(&pkt, &seq);
  if (!pr.ok()) return pr.status();

  if (!pkt.empty() && pkt[0] == 0xff) {
    std::string err_msg = (pkt.size() > 9) ? std::string((const char*)pkt.data() + 9, pkt.size() - 9) : "Auth failed";
    last_error_ = err_msg;
    return Error(Status::Denied, err_msg);
  }

  return Ok();
}

Result<void> BinlogConsumer::discover_master_pos(std::string* out_file) {
  if (!config_.binlog_filename.empty()) {
    active_binlog_file_ = config_.binlog_filename;
    active_binlog_pos_ = config_.binlog_position;
    if (out_file) *out_file = active_binlog_file_;
    return Ok();
  }

  // Query master status using MariaWireSession
  MariaWireSession sess(config_.host, config_.port);
  auto cr = sess.connect(config_.user, config_.password, config_.database);
  if (!cr.ok()) return cr.status();

  auto qr = sess.query("SHOW MASTER STATUS;");
  if (qr.ok() && qr->ok && !qr->rows.empty() && qr->rows[0].size() >= 2) {
    active_binlog_file_ = qr->rows[0][0].bytes;
    // strtoull, not stoull: a bad position is reported here by falling back to
    // 4, and stoull reports it by throwing -- which this file cannot do, being
    // compiled with exceptions off in the bare-metal image.
    const std::string& pos_text = qr->rows[0][1].bytes;
    const char* pos_begin = pos_text.c_str();
    char* pos_end = nullptr;
    errno = 0;
    const unsigned long long pos = std::strtoull(pos_begin, &pos_end, 10);
    active_binlog_pos_ = (pos_end == pos_begin || errno == ERANGE) ? 4 : pos;
  } else {
    // Fallback: check SHOW BINARY LOGS
    auto blr = sess.query("SHOW BINARY LOGS;");
    if (blr.ok() && blr->ok && !blr->rows.empty()) {
      active_binlog_file_ = blr->rows.back()[0].bytes;
      active_binlog_pos_ = 4;
    } else {
      active_binlog_file_ = "mysql-bin.000001";
      active_binlog_pos_ = 4;
    }
  }
  if (out_file) *out_file = active_binlog_file_;
  return Ok();
}

Result<void> BinlogConsumer::send_register_slave() {
  std::vector<uint8_t> payload;
  payload.push_back(0x15); // COM_REGISTER_SLAVE
  write_le32(&payload, config_.server_id);

  std::string rep_host = "aios-replica";
  payload.push_back((uint8_t)rep_host.size());
  write_bytes(&payload, rep_host);

  payload.push_back((uint8_t)config_.user.size());
  write_bytes(&payload, config_.user);

  payload.push_back((uint8_t)config_.password.size());
  write_bytes(&payload, config_.password);

  write_le16(&payload, config_.port);
  write_le32(&payload, 0); // rpl_recovery_rank
  write_le32(&payload, 0); // master_id

  auto wr = write_packet(payload.data(), payload.size(), 0);
  if (!wr.ok()) return wr;

  std::vector<uint8_t> reply;
  uint8_t seq = 0;
  auto rr = read_packet(&reply, &seq);
  if (!rr.ok()) return rr.status();

  if (!reply.empty() && reply[0] == 0xff) {
    std::string err = (reply.size() > 9) ? std::string((const char*)reply.data() + 9, reply.size() - 9) : "COM_REGISTER_SLAVE rejected";
    last_error_ = err;
    return Error(Status::Invalid, err);
  }
  return Ok();
}

Result<void> BinlogConsumer::send_binlog_dump() {
  std::vector<uint8_t> payload;
  payload.push_back(0x12); // COM_BINLOG_DUMP
  // COM_BINLOG_DUMP is: command, position as 4 bytes LE, flags (2), server id
  // (4), then the file name to the end of the packet - the same on MySQL and
  // MariaDB. An 8-byte position (3e736b13) pushed our server id into the file
  // name, and the server answered 1236 "Could not find first log file name in
  // binary log index file" on every attempt. (The FORMAT_DESCRIPTION event
  // with next_pos=4 that motivated it is the synthetic header every dump
  // starts with; it only looked stuck because read_packet was also eating
  // the first byte of each event.)
  write_le32(&payload, (uint32_t)active_binlog_pos_);
  write_le16(&payload, 0x0000); // flags (BINLOG_DUMP_NON_BLOCK = 0x0000 for continuous)
  write_le32(&payload, config_.server_id);
  write_bytes(&payload, active_binlog_file_);

  auto wr = write_packet(payload.data(), payload.size(), 0);
  if (!wr.ok()) return wr;

  return Ok();
}

Result<void> BinlogConsumer::send_checksum_config() {
  std::vector<uint8_t> payload;
  payload.push_back(0x03); // COM_QUERY
  std::string sql = "SET @master_binlog_checksum='NONE'";
  write_bytes(&payload, sql);

  auto wr = write_packet(payload.data(), payload.size(), 0);
  if (!wr.ok()) return wr;

  std::vector<uint8_t> reply;
  uint8_t seq = 0;
  read_packet(&reply, &seq);

  // A heartbeat every 5 s while the source has nothing to send. Without one a
  // quiet database (st's summitholding, most of the day) looked like a dead
  // link: read_packet's 10 s timeout fired, the stream "failed", reconnected,
  // and did it again every ~10 s. With it, only a link that is really gone
  // goes quiet long enough to trip the timeout.
  std::vector<uint8_t> hb;
  hb.push_back(0x03);  // COM_QUERY
  write_bytes(&hb, std::string("SET @master_heartbeat_period = 5000000000"));   // nanoseconds
  auto hw = write_packet(hb.data(), hb.size(), 0);
  if (!hw.ok()) return hw;
  read_packet(&reply, &seq);
  return Ok();
}

Result<void> BinlogConsumer::connect_and_dump() {
  if (config_.auto_discover_position && active_binlog_file_.empty()) {
    discover_master_pos();
  }

  auto cr = connect_and_authenticate();
  if (!cr.ok()) return cr;

  auto sr = send_register_slave();
  if (!sr.ok()) return sr;

  send_checksum_config();

  auto dr = send_binlog_dump();
  if (!dr.ok()) return dr;

  in_dump_stream_ = true;
  connected_ = true;
  return Ok();
}

Result<size_t> BinlogConsumer::step(int timeout_ms) {
  if (!connected_ || !conn_ || !conn_->alive()) {
    auto cr = connect_and_dump();
    if (!cr.ok()) return cr.error();          // the message, not just the code
  }

  std::vector<uint8_t> pkt;
  uint8_t seq = 0;
  auto rr = read_packet(&pkt, &seq);
  if (!rr.ok()) {
    connected_ = false;
    return rr.error();
  }

  if (pkt.empty()) return 0;

  // UNCONDITIONAL diag for the first packets: prove whether ANY event arrives.
  {
    static uint64_t boot_read = 0;
    if (boot_read < 6) {
      ++boot_read;
      std::string hexs; char b[8];
      for (size_t i = 0; i < pkt.size() && i < 20; ++i) { snprintf(b, sizeof(b), "%02x ", (unsigned)pkt[i]); hexs += b; }
      fprintf(stderr, "[cdc-boot] pkt#%llu first=0x%02x len=%zu head=%s\n",
              (unsigned long long)boot_read, (unsigned)pkt[0], pkt.size(), hexs.c_str());
      fflush(stderr);
    }
  }

  {
    static uint64_t diag_read = 0, diag_00 = 0, diag_ff = 0, diag_fe = 0, diag_other = 0, diag_other_sample = 0;
    ++diag_read;
    if (packet_kind_ == PacketKind::Err) ++diag_ff;
    else if (packet_kind_ == PacketKind::Eof) ++diag_fe;
    else if (pkt[0] == 0x00) ++diag_00;
    else ++diag_other;
    if (packet_kind_ == PacketKind::Event && pkt[0] != 0x00 && diag_other_sample < 3) {
      ++diag_other_sample;
      std::string hexs;
      char b[8];
      for (size_t i = 0; i < pkt.size() && i < 24; ++i) { snprintf(b, sizeof(b), "%02x ", (unsigned)pkt[i]); hexs += b; }
      fprintf(stderr, "[cdc-diag] other-pkt len=%zu head=%s\n", pkt.size(), hexs.c_str());
    }
    if ((diag_read % 20) == 0 || diag_read < 6) {
      fprintf(stderr, "[cdc-diag] read=%llu ok=%llu err=%llu eof=%llu other=%llu pos=%s/%u maps=%zu skipped=%llu\n",
              (unsigned long long)diag_read, (unsigned long long)diag_00, (unsigned long long)diag_ff,
              (unsigned long long)diag_fe, (unsigned long long)diag_other, active_binlog_file_.c_str(),
              (unsigned)active_binlog_pos_, table_maps_.size(), (unsigned long long)total_events_skipped_);
    }
    fflush(stderr);
  }

  // Replication packet framing. MariaDB's COM_BINLOG_DUMP stream prefixes the
  // event header with a 1-byte status marker:
  //   0x00 = OK (binlog event data follows)
  //   0xFF = error
  //   0xFE = EOF
  // A 0x00 marker byte is also a valid first byte of the 4-byte LE event
  // timestamp, so this byte alone cannot tell an event from a keepalive packet.
  // The header scan below decides; pkt[0] is only used for the 0xff/0xfe cases.
  // The old code assumed the marker is always at offset 0 and never appears in
  // the middle of the payload. In practice MariaDB also emits a 5-byte OK packet
  // (0x00 <affected rows> <last insert id> <status> <warnings>) ahead of the
  // event stream, so the marker can sit further in. Locate the real event header
  // instead of trusting the first byte: a valid header is >= 19 bytes with an
  // LE timestamp we could plausibly have produced and an event type in 1..0x27.
  if (packet_kind_ == PacketKind::Err) {
    std::string err = (pkt.size() > 9) ? std::string((const char*)pkt.data() + 9, pkt.size() - 9) : "Binlog stream error";
    last_error_ = err;
    connected_ = false;
    return Error(Status::Invalid, err);
  } else if (packet_kind_ == PacketKind::Eof) {
    // End of binlog stream or rotate
    return 0;
  }

  // Locate the true event header. Scan the leading bytes for the first offset
  // whose 5th following byte (event type) is a real type and whose preceding
  // 4 bytes decode to a sane LE timestamp. A false positive is harmless: the
  // CRC32 checksum of the event validates the whole packet downstream.
  size_t hdr = SIZE_MAX;
  const uint32_t now_s = (uint32_t)std::time(nullptr);
  for (size_t off = 0; off + 19 <= pkt.size() && off < 8; ++off) {
    const uint8_t et = pkt[off + 4];
    if (et < 1 || et > 0x27) continue;
    const uint32_t ts = (uint32_t)pkt[off] | ((uint32_t)pkt[off + 1] << 8) |
                        ((uint32_t)pkt[off + 2] << 16) | ((uint32_t)pkt[off + 3] << 24);
    // Timestamps are unix epoch seconds: must be after 2001 and not far in the
    // future. This rejects the 0x00000000 padding of an OK packet.
    // The one event that legitimately has timestamp 0 is the ROTATE the master
    // makes up (flag 0x20, artificial): how it tells a client that the log it
    // asked for has ended and the next one begins. After a master restart the
    // old log ends in a STOP event and this is the only notice, so dropping it
    // left the file name stale while offsets came from the new file, and every
    // event was then skipped as "already in the seed snapshot".
    const bool artificial_rotate = ts == 0 && et == binlog_event::ROTATE_EVENT && (pkt[off + 17] & 0x20) != 0;
    if (!artificial_rotate && (ts < 1000000000u || ts > now_s + 86400u)) continue;
    hdr = off;
    break;
  }

  if (hdr == SIZE_MAX) {
    // Not recognisable as an event (OK/EOF/keepalive packet) — ignore it but
    // keep the stream alive rather than tearing the connection down.
    ++total_keepalives_;
    return 0;
  }

  // A FORMAT_DESCRIPTION event is only legal at the head of a binlog, where its
  // next-position is 4 (right after the magic). Seeing one again while we asked
  // for a high offset means the server rejected our COM_BINLOG_DUMP position and
  // restarted the stream from the file head: replaying it forever is what left
  // cdc_position pinned and every row dropped. Re-dump from the live master
  // position instead.
  if (pkt[hdr + 4] == binlog_event::FORMAT_DESCRIPTION_EVENT) {
    const uint32_t fde_next = read_le32(pkt.data() + hdr + 13);
    if (fde_next > 0 && fde_next <= 4 && active_binlog_pos_ > 64) {
      std::string mfile = active_binlog_file_;
      auto dres = discover_master_pos(&mfile);
      if (dres.ok() && active_binlog_pos_ > 0) {
        if (!mfile.empty()) active_binlog_file_ = mfile;
        auto cr = connect_and_dump();
        if (!cr.ok()) return cr.status();
        return 0;
      }
    }
  }

  // The dump stream can pack several events into one replication packet (and a
  // single event can also be split across packets). Drain every event header
  // present in this packet rather than decoding only the first one — otherwise
  // the TABLE_MAP/ROWS pair for a transaction is silently halved and rows are
  // dropped at decode for a missing table_id.
  size_t events_here = 0;
  while (hdr != SIZE_MAX) {
    auto hr = handle_event(pkt.data() + hdr, pkt.size() - hdr);
    if (!hr.ok()) return hr.status();
    ++events_here;

    // Advance to the next event header inside this packet, if the previous
    // event's declared length lands on one.
    const uint32_t ev_len = read_le32(pkt.data() + hdr + 9);
    if (ev_len < 19) break;
    size_t next = hdr + ev_len;
    if (next + 19 > pkt.size()) break;
    size_t nh = SIZE_MAX;
    const uint8_t net = pkt[next + 4];
    if (net >= 1 && net <= 0x27) {
      const uint32_t nts = (uint32_t)pkt[next] | ((uint32_t)pkt[next + 1] << 8) |
                           ((uint32_t)pkt[next + 2] << 16) | ((uint32_t)pkt[next + 3] << 24);
      if (nts >= 1000000000u && nts <= now_s + 86400u) nh = next;
    }
    hdr = nh;
  }
  return 1;
}

Result<void> BinlogConsumer::ingest_raw_event(const uint8_t* data, size_t len) {
  return handle_event(data, len);
}

Result<void> BinlogConsumer::handle_event(const uint8_t* event_data, size_t event_len) {
  if (event_len < 19) {
    return Error(Status::Corrupt, "Binlog event header truncated (<19 bytes)");
  }

  total_events_received_++;

  uint32_t ts = read_le32(event_data);
  uint8_t type = event_data[4];
  uint32_t ev_len = read_le32(event_data + 9);
  uint32_t next_pos = read_le32(event_data + 13);
  (void)ev_len;

  last_event_ts_ = ts;
  uint64_t now_s = now_seconds();
  // A heartbeat means the master has sent everything it has: nothing behind.
  // (Measured from the event's timestamp, an idle master's lag only grew:
  // SHOW REPLICA STATUS on a caught-up mirror said it was minutes behind.)
  seconds_behind_master_ = type == binlog_event::HEARTBEAT_LOG_EVENT ? 0
                           : (now_s >= ts && ts > 0) ? (now_s - ts) : 0;
  // A heartbeat is not an event in the log: it never moves our place in it.
  if (next_pos > 0 && type != binlog_event::HEARTBEAT_LOG_EVENT) {
    active_binlog_pos_ = next_pos;
  }

  // Every event after a FORMAT_DESCRIPTION that announces CRC32 ends in a
  // 4-byte checksum. The FDE says so in the byte just before its own
  // checksum (1 = CRC32). Left in, the checksum was decoded as one more row
  // of every ROWS event: an all-NULL insert that a NOT NULL key refused and a
  // keyless table took as a junk row.
  if (type == binlog_event::FORMAT_DESCRIPTION_EVENT && event_len >= 19 + 57 + 5) {
    checksum_len_ = event_data[event_len - 5] == 1 ? 4 : 0;
  }
  // The artificial ROTATE (ts == 0) the source sends when a dump starts carries no checksum from MySQL, but MariaDB's
  // does: its last four bytes are taken off only when they are the CRC32 of the rest. Read as part of the name, they
  // made "mariadb-bin.000655" plus four bytes, saved as the position, and chi-il-1 stopped replicating on Oct 8.
  size_t eff_checksum = checksum_len_;
  if (type == binlog_event::ROTATE_EVENT && ts == 0) {
    eff_checksum = 0;
    if (event_len >= 19 + 8 + 4 &&
        crc32(ByteSpan(event_data, event_len - 4)) == read_le32(event_data + event_len - 4))
      eff_checksum = 4;
  }
  const size_t payload_len = event_len >= 19 + eff_checksum ? event_len - eff_checksum : event_len;
  const uint8_t* body = event_data + 19;
  size_t body_len = (payload_len >= 19) ? (payload_len - 19) : 0;

  switch (type) {
    case binlog_event::ROTATE_EVENT:
      return handle_rotate_event(body, body_len);
    case binlog_event::TABLE_MAP_EVENT:
      return handle_table_map(body, body_len, ts);
    case binlog_event::WRITE_ROWS_EVENT_V1:
    case binlog_event::WRITE_ROWS_EVENT_V2:
    case binlog_event::UPDATE_ROWS_EVENT_V1:
    case binlog_event::UPDATE_ROWS_EVENT_V2:
    case binlog_event::DELETE_ROWS_EVENT_V1:
    case binlog_event::DELETE_ROWS_EVENT_V2:
      return handle_rows_event(type, body, body_len, ts);
    case binlog_event::QUERY_EVENT:
      return handle_query_event(body, body_len, ts);
    case binlog_event::XID_EVENT:
      note_commit();
      total_events_applied_++;
      return Ok();
    case binlog_event::HEARTBEAT_LOG_EVENT:
    case binlog_event::FORMAT_DESCRIPTION_EVENT:
    default:
      total_events_applied_++;
      return Ok();
  }
}

Result<void> BinlogConsumer::handle_rotate_event(const uint8_t* p, size_t len) {
  if (len < 8) return Error(Status::Corrupt, "Truncated ROTATE_EVENT");
  uint64_t next_pos = read_le64(p);
  std::string next_file((const char*)p + 8, len - 8);
  next_file = clean_binlog_name(trim_str(next_file));
  if (!next_file.empty()) {
    active_binlog_file_ = next_file;
  }
  active_binlog_pos_ = next_pos;
  total_events_applied_++;
  return Ok();
}

Result<void> BinlogConsumer::introspect_table_columns(BinlogTableMap& map) {
  // First check if table exists in local ASQL database
  std::string db_name = map.schema_name.empty() ? config_.database : map.schema_name;
  const SqlTable* local_tbl = db_.find_table(map.table_name, db_name);
  if (local_tbl && !local_tbl->columns.empty()) {
    for (size_t i = 0; i < local_tbl->columns.size() && i < map.columns.size(); ++i) {
      map.columns[i].name = local_tbl->columns[i].name;
      map.columns[i].is_pk = local_tbl->columns[i].is_primary_key;
    }
  }

  // Check if remote introspection is needed: missing column names or ENUM/SET columns lacking enum_values
  bool need_remote = false;
  for (size_t i = 0; i < map.columns.size(); ++i) {
    if (map.columns[i].name.empty()) { need_remote = true; break; }
    uint8_t ctype = map.columns[i].type;
    uint8_t real_t = (uint8_t)(map.columns[i].meta >> 8);
    if ((ctype == binlog_type::ENUM || ctype == binlog_type::SET ||
         real_t == binlog_type::ENUM || real_t == binlog_type::SET) &&
        map.columns[i].enum_values.empty()) {
      need_remote = true;
      break;
    }
  }

  // Next query master if auto_schema_sync is true and we have connection details
  if (need_remote && config_.auto_schema_sync && !config_.host.empty()) {
    MariaWireSession sess(config_.host, config_.port);
    auto cr = sess.connect(config_.user, config_.password, map.schema_name.empty() ? config_.database : map.schema_name);
    if (cr.ok()) {
      auto qr = sess.query("SHOW COLUMNS FROM `" + map.table_name + "`;");
      if (qr.ok() && qr->ok && !qr->rows.empty()) {
        for (size_t i = 0; i < qr->rows.size() && i < map.columns.size(); ++i) {
          if (map.columns[i].name.empty()) {
            map.columns[i].name = qr->rows[i][0].bytes;
          }
          if (qr->rows[i].size() >= 2 && map.columns[i].enum_values.empty()) {
            map.columns[i].enum_values = parse_mysql_enum_values(qr->rows[i][1].bytes);
          }
          if (qr->rows[i].size() >= 4 && qr->rows[i][3].bytes == "PRI") {
            map.columns[i].is_pk = true;
          }
        }
        return Ok();
      }
    }
  }

  if (local_tbl && !local_tbl->columns.empty()) {
    return Ok();
  }

  // Fallback: generate default column names col1, col2, ...
  for (size_t i = 0; i < map.columns.size(); ++i) {
    if (map.columns[i].name.empty()) {
      map.columns[i].name = "col" + std::to_string(i + 1);
    }
  }
  return Ok();
}

Result<void> BinlogConsumer::handle_table_map(const uint8_t* p, size_t len, uint32_t ts) {
  if (len < 8) return Error(Status::Corrupt, "Truncated TABLE_MAP_EVENT");
  uint64_t table_id = read_le48(p); // 6 bytes table id
  // 2 bytes flags
  size_t idx = 8;

  if (idx >= len) return Error(Status::Corrupt, "TABLE_MAP_EVENT truncated schema len");
  uint8_t schema_len = p[idx++];
  if (idx + schema_len + 1 > len) return Error(Status::Corrupt, "TABLE_MAP_EVENT truncated schema name");
  std::string schema_name((const char*)p + idx, schema_len);
  idx += schema_len + 1; // skip null byte

  if (idx >= len) return Error(Status::Corrupt, "TABLE_MAP_EVENT truncated table len");
  uint8_t table_len = p[idx++];
  if (idx + table_len + 1 > len) return Error(Status::Corrupt, "TABLE_MAP_EVENT truncated table name");
  std::string table_name((const char*)p + idx, table_len);
  idx += table_len + 1; // skip null byte

  size_t bytes_read = 0;
  uint64_t col_count = read_lenenc(p + idx, len - idx, &bytes_read);
  idx += bytes_read;

  BinlogTableMap tmap;
  tmap.table_id = table_id;
  tmap.schema_name = schema_name;
  tmap.table_name = table_name;
  tmap.whitelisted = is_whitelisted(schema_name, table_name);

  // Load column whitelist if defined
  std::string tbl_low = to_lower_str(table_name);
  auto it_col = config_.column_whitelists.find(tbl_low);
  if (it_col != config_.column_whitelists.end()) {
    tmap.column_whitelist = it_col->second;
  }

  // Parse column type bytes
  if (idx + col_count <= len) {
    tmap.columns.resize(col_count);
    for (size_t i = 0; i < col_count; ++i) {
      tmap.columns[i].type = p[idx + i];
    }
    idx += col_count;
  }

  // Parse metadata: per-column bytes whose count and meaning depend on the
  // column's type (see Table_map_log_event::save_field_metadata upstream).
  // This used to be skipped outright, which left every column's `meta` at
  // its default of 0 -- fatal for NEWDECIMAL (precision/scale), VARCHAR
  // (length-prefix width) and the *2 temporal types (fractional precision).
  if (idx < len) {
    uint64_t meta_len = read_lenenc(p + idx, len - idx, &bytes_read);
    idx += bytes_read;
    size_t meta_end = idx + meta_len;
    if (meta_end > len) meta_end = len;

    for (size_t i = 0; i < col_count && i < tmap.columns.size() && idx < meta_end; ++i) {
      uint8_t t = tmap.columns[i].type;
      switch (t) {
        case binlog_type::FLOAT:
        case binlog_type::DOUBLE:
        case binlog_type::BLOB:
        case binlog_type::TINY_BLOB:
        case binlog_type::MEDIUM_BLOB:
        case binlog_type::LONG_BLOB:
        case binlog_type::JSON:
        case binlog_type::GEOMETRY:
          if (idx < meta_end) { tmap.columns[i].meta = p[idx]; idx += 1; }
          break;
        case binlog_type::VARCHAR:
          if (idx + 2 <= meta_end) { tmap.columns[i].meta = read_le16(p + idx); idx += 2; }
          break;
        case binlog_type::VAR_STRING:
        case binlog_type::STRING:
        case binlog_type::ENUM:
        case binlog_type::SET:
          if (idx + 2 <= meta_end) {
            uint8_t b1 = p[idx];
            uint8_t b2 = p[idx + 1];
            if (t == binlog_type::STRING && (b1 == binlog_type::ENUM || b1 == binlog_type::SET)) {
              tmap.columns[i].type = b1;
            }
            tmap.columns[i].meta = (uint16_t)((b1 << 8) | b2);
            idx += 2;
          }
          break;
        case binlog_type::NEWDECIMAL:
          if (idx + 2 <= meta_end) { tmap.columns[i].meta = (uint16_t)((p[idx] << 8) | p[idx + 1]); idx += 2; }
          break;
        case binlog_type::BIT:
          if (idx + 2 <= meta_end) { tmap.columns[i].meta = (uint16_t)((p[idx] << 8) | p[idx + 1]); idx += 2; }
          break;
        case binlog_type::TIME2:
        case binlog_type::DATETIME2:
        case binlog_type::TIMESTAMP2:
          if (idx < meta_end) { tmap.columns[i].meta = p[idx]; idx += 1; }
          break;
        default:
          // TINY, SHORT, LONG, LONGLONG, INT24, TIMESTAMP, DATE, TIME,
          // DATETIME, YEAR, NULL_TYPE: no metadata bytes.
          break;
      }
    }
    idx = meta_end;
  }

  if (tmap.whitelisted) {
    introspect_table_columns(tmap);
  }

  fprintf(stderr, "[cdc-diag] TABLE_MAP id=%llu schema=%s table=%s cols=%zu whitelisted=%d\n",
          (unsigned long long)table_id, tmap.schema_name.c_str(), tmap.table_name.c_str(),
          tmap.columns.size(), (int)tmap.whitelisted);
  fflush(stderr);

  table_maps_[table_id] = std::move(tmap);
  total_events_applied_++;
  return Ok();
}

Result<void> BinlogConsumer::handle_query_event(const uint8_t* p, size_t len, uint32_t ts) {
  if (len < 13) return Error(Status::Corrupt, "Truncated QUERY_EVENT");
  // 4b slave_proxy_id, 4b exec_time, 1b schema_len, 2b err_code, 2b status_vars_len
  uint8_t schema_len = p[8];
  uint16_t status_vars_len = read_le16(p + 11);
  size_t idx = 13 + status_vars_len;

  std::string schema;
  if (idx + schema_len <= len) {
    schema = std::string((const char*)p + idx, schema_len);
    idx += schema_len + 1; // null-terminated
  }

  if (idx < len) {
    std::string query((const char*)p + idx, len - idx);
    query = trim_str(query);
    std::string qupper = to_upper_str(query);

    // Skip transaction control queries (BEGIN, COMMIT) as ASQL autocommits
    if (qupper == "BEGIN" || qupper == "COMMIT" || qupper == "ROLLBACK") {
      if (qupper == "COMMIT") note_commit();   // a non-transactional table's commit
      total_events_applied_++;
      return Ok();
    }

    if (config_.replicate_ddl) {
      // Only table DDL, only for a mirrored table of the source database,
      // and run inside that database. It used to execute every statement the
      // binlog carried, for every database on the server - another tenant's
      // CREATE TABLE landed in this mirror, and a DROP DATABASE would have
      // been attempted here too.
      // View DDL is applied to the views of the source database. A mirror that
      // ignored it kept every view as it was seeded, and a changed view kept
      // answering from its old join.
      {
        std::string vverb;
        std::vector<std::pair<std::string, std::string>> vnames;
        if (parse_view_ddl(query, &vverb, &vnames)) {
          auto bt = [](const std::string& s) {
            std::string o = "`";
            for (char c : s) { if (c == '`') o += '`'; o += c; }
            return o + "`";
          };
          for (const auto& vn : vnames) {
            const std::string vdb = vn.first.empty() ? schema : vn.first;
            if (!in_source_schema(vdb)) continue;
            std::string stmt;
            if (vverb == "DROP") {
              stmt = "DROP VIEW IF EXISTS " + bt(vn.second);
            } else {
              // `CREATE ... VIEW v ...` and `ALTER ... VIEW v ...` both leave
              // the view as the statement defines it: run it as a CREATE OR
              // REPLACE, keeping ALGORITHM / DEFINER / SQL SECURITY as written.
              const std::string up = to_upper_str(query);
              const size_t at = up.find(" VIEW ");
              if (at == std::string::npos) continue;
              const size_t orr = up.find("OR REPLACE");
              if (vverb == "CREATE" && orr != std::string::npos && orr < at) {
                stmt = query;
              } else {
                const size_t sp = query.find_first_of(" \t\r\n");
                stmt = "CREATE OR REPLACE" + (sp == std::string::npos ? std::string() : query.substr(sp));
              }
            }
            auto vr = db_.execute_local_query(stmt, vdb);
            if (vr.success) {
              total_ddl_++;
              total_events_applied_++;
              applied_since_commit_ = true;
              fprintf(stderr, "[cdc] view %s.%s %s\n", vdb.c_str(), vn.second.c_str(), vverb == "DROP" ? "dropped" : "replaced from the binlog");
            } else {
              last_error_ = vr.error_message;
              fprintf(stderr, "[cdc] view %s.%s: %s not applied: %s\n", vdb.c_str(), vn.second.c_str(), vverb.c_str(), vr.error_message.c_str());
            }
          }
          return Ok();
        }
      }
      std::string verb, target_db, target_table;
      if (!parse_table_ddl(query, &verb, &target_db, &target_table)) return Ok();   // not table DDL
      if (target_db.empty()) target_db = schema;
      if (!is_whitelisted(target_db, target_table)) return Ok();
      // Replaying past a table's seed point re-reads the CREATE that made it;
      // the table being here already is the state that CREATE describes.
      if (verb == "CREATE" && db_.find_table(target_table, target_db)) return Ok();
      auto q_res = db_.execute_local_query(query, target_db);
      if (q_res.success) {
        total_ddl_++;
        total_events_applied_++;
        applied_since_commit_ = true;
      } else {
        last_error_ = q_res.error_message;
        fprintf(stderr, "[cdc] %s.%s: %s not applied: %s\n", target_db.c_str(), target_table.c_str(), verb.c_str(), q_res.error_message.c_str());
      }
    }
  }

  return Ok();
}

Result<void> BinlogConsumer::handle_rows_event(uint8_t type, const uint8_t* p,
                                              size_t len, uint32_t ts) {
  if (len < 8) return Error(Status::Corrupt, "Truncated ROWS_EVENT");
  uint64_t table_id = read_le48(p);
  size_t idx = 8; // 6b table_id + 2b flags

  bool is_v2 = (type == binlog_event::WRITE_ROWS_EVENT_V2 ||
                type == binlog_event::UPDATE_ROWS_EVENT_V2 ||
                type == binlog_event::DELETE_ROWS_EVENT_V2);

  if (is_v2) {
    if (idx + 2 > len) return Error(Status::Corrupt, "Truncated ROWS_EVENT_V2 extra data");
    uint16_t extra_len = read_le16(p + idx);
    idx += extra_len;
  }

  auto it = table_maps_.find(table_id);
  if (it == table_maps_.end() || !it->second.whitelisted) {
    total_events_skipped_++;
    return Ok(); // Filtered: 0 RAM allocated!
  }

  const BinlogTableMap& tmap = it->second;
  // Already in the table's seed snapshot? handle_event() has moved the cursor
  // to this event's end, so an event ending at or before the snapshot point
  // was committed before it. Binlog names are fixed-width, so they order as
  // strings.
  if (!config_.table_seed_positions.empty()) {
    auto sp = config_.table_seed_positions.find(to_lower_str(tmap.schema_name + "." + tmap.table_name));
    if (sp == config_.table_seed_positions.end()) sp = config_.table_seed_positions.find(to_lower_str(tmap.table_name));
    if (sp != config_.table_seed_positions.end()) {
      const std::string& sf = sp->second.first;
      if (active_binlog_file_ < sf || (active_binlog_file_ == sf && active_binlog_pos_ <= sp->second.second)) {
        total_events_skipped_++;
        return Ok();
      }
    }
  }
  // AIOS_CDC_TRACE=1: the raw row image of every applied ROWS event, for
  // debugging a decoder against a real server. Off by default - it is the
  // customer's data.
  static const bool trace = [] { const char* e = getenv("AIOS_CDC_TRACE"); return e && e[0] == '1'; }();
  if (trace) {
    std::string hx; char b[4];
    for (size_t i = idx; i < len && i < idx + 96; ++i) { snprintf(b, sizeof(b), "%02x ", (unsigned)p[i]); hx += b; }
    fprintf(stderr, "[cdc-trace] type=0x%02x %s pos=%llu len=%zu cols:", (unsigned)type, tmap.table_name.c_str(),
            (unsigned long long)active_binlog_pos_, len - idx);
    for (const auto& c : tmap.columns) fprintf(stderr, " %s/0x%02x/m%u", c.name.c_str(), (unsigned)c.type, (unsigned)c.meta);
    fprintf(stderr, "\n[cdc-trace]   %s\n", hx.c_str());
  }
  return decode_rows(tmap, p + idx, (len >= idx) ? (len - idx) : 0, type, ts);
}

Result<void> BinlogConsumer::decode_rows(const BinlogTableMap& map, const uint8_t* p,
                                        size_t len, uint8_t type, uint32_t ts) {
  if (len == 0) return Ok();
  size_t idx = 0;

  size_t bytes_read = 0;
  uint64_t col_count = read_lenenc(p + idx, len - idx, &bytes_read);
  idx += bytes_read;

  size_t bitmap_bytes = (col_count + 7) / 8;
  if (idx + bitmap_bytes > len) return Error(Status::Corrupt, "Truncated column bitmap");
  const uint8_t* cols_present = p + idx;
  idx += bitmap_bytes;

  const uint8_t* cols_present_after = nullptr;
  bool is_update = (type == binlog_event::UPDATE_ROWS_EVENT_V1 ||
                    type == binlog_event::UPDATE_ROWS_EVENT_V2);
  bool is_delete = (type == binlog_event::DELETE_ROWS_EVENT_V1 ||
                    type == binlog_event::DELETE_ROWS_EVENT_V2);

  if (is_update) {
    if (idx + bitmap_bytes > len) return Error(Status::Corrupt, "Truncated update after column bitmap");
    cols_present_after = p + idx;
    idx += bitmap_bytes;
  }

  // Row decoder lambda
  auto decode_one_row = [&](const uint8_t* present_bmp, std::vector<std::pair<std::string, std::string>>* out_row) -> bool {
    if (idx >= len) return false;

    // Count present columns
    size_t num_present = 0;
    for (size_t c = 0; c < col_count; ++c) {
      if (present_bmp[c / 8] & (1 << (c % 8))) num_present++;
    }

    size_t null_bytes = (num_present + 7) / 8;
    if (idx + null_bytes > len) return false;
    const uint8_t* null_bmp = p + idx;
    idx += null_bytes;

    size_t null_bit = 0;
    out_row->clear();

    for (size_t c = 0; c < col_count && c < map.columns.size(); ++c) {
      bool is_present = (present_bmp[c / 8] & (1 << (c % 8))) != 0;
      if (!is_present) continue;

      bool is_null = (null_bmp[null_bit / 8] & (1 << (null_bit % 8))) != 0;
      null_bit++;

      std::string col_name = map.columns[c].name;
      if (col_name.empty()) col_name = "col" + std::to_string(c + 1);

      // Column projection: check if column is in whitelist
      if (!map.column_whitelist.empty() && !map.column_whitelist.count(to_lower_str(col_name))) {
        // Skip decoding unwhitelisted column bytes to save CPU & RAM
        uint8_t ctype = map.columns[c].type;
        if (ctype == binlog_type::STRING) {
          uint8_t real_t = (uint8_t)(map.columns[c].meta >> 8);
          if (real_t == binlog_type::ENUM || real_t == binlog_type::SET) {
            ctype = real_t;
          }
        }
        if (!is_null) {
          switch (ctype) {
            case binlog_type::TINY: idx += 1; break;
            case binlog_type::SHORT: idx += 2; break;
            case binlog_type::INT24: idx += 3; break;
            case binlog_type::LONG:
            case binlog_type::FLOAT: idx += 4; break;
            case binlog_type::LONGLONG:
            case binlog_type::DOUBLE: idx += 8; break;
            case binlog_type::VARCHAR: {
              size_t lp = map.columns[c].meta >= 256 ? 2 : 1;
              if (idx + lp <= len) {
                uint32_t slen = lp == 2 ? read_le16(p + idx) : p[idx];
                idx += lp + slen;
              }
              break;
            }
            case binlog_type::VAR_STRING:
            case binlog_type::STRING: {
              if (idx < len) {
                uint8_t slen = p[idx++];
                idx += slen;
              }
              break;
            }
            case binlog_type::ENUM: {
              size_t nbytes = (map.columns[c].meta != 0) ? ((map.columns[c].meta & 0xff) > 1 ? 2 : 1) : 1;
              idx += nbytes;
              break;
            }
            case binlog_type::SET: {
              size_t nbytes = map.columns[c].meta != 0 ? (map.columns[c].meta & 0xff) : 8;
              if (nbytes == 0 || nbytes > 8) nbytes = 8;
              idx += nbytes;
              break;
            }
            case binlog_type::BLOB:
            case binlog_type::TINY_BLOB:
            case binlog_type::MEDIUM_BLOB:
            case binlog_type::LONG_BLOB:
            case binlog_type::JSON: {
              uint8_t pack_len = map.columns[c].meta ? (uint8_t)map.columns[c].meta : 2;
              uint32_t blob_len = 0;
              if (idx + pack_len <= len) {
                for (uint8_t bi = 0; bi < pack_len; ++bi) blob_len |= ((uint32_t)p[idx + bi]) << (8 * bi);
                idx += pack_len + blob_len;
              }
              break;
            }
            case binlog_type::DATE:
            case binlog_type::TIME: idx += 3; break;
            case binlog_type::TIME2: idx += 3 + frac_bytes_for_fsp((uint8_t)map.columns[c].meta); break;
            case binlog_type::DATETIME: idx += 8; break;
            case binlog_type::DATETIME2: idx += 5 + frac_bytes_for_fsp((uint8_t)map.columns[c].meta); break;
            case binlog_type::TIMESTAMP: idx += 4; break;
            case binlog_type::TIMESTAMP2: idx += 4 + frac_bytes_for_fsp((uint8_t)map.columns[c].meta); break;
            case binlog_type::YEAR: idx += 1; break;
            case binlog_type::NEWDECIMAL:
              idx += new_decimal_byte_length(map.columns[c].meta);
              break;
            case binlog_type::BIT: {
              size_t nbytes = map.columns[c].meta != 0 ? (map.columns[c].meta & 0xff) : 1;
              idx += nbytes;
              break;
            }
            default:
              idx += 4;
              break;
          }
        }
        continue;
      }

      if (is_null) {
        out_row->push_back({col_name, sql_null()});
        continue;
      }

      uint8_t ctype = map.columns[c].type;
      if (ctype == binlog_type::STRING) {
        uint8_t real_t = (uint8_t)(map.columns[c].meta >> 8);
        if (real_t == binlog_type::ENUM || real_t == binlog_type::SET) {
          ctype = real_t;
        }
      }
      std::string val_str;

      switch (ctype) {
        case binlog_type::TINY:
          if (idx < len) {
            val_str = std::to_string((int8_t)p[idx++]);
          }
          break;
        case binlog_type::SHORT:
          if (idx + 2 <= len) {
            val_str = std::to_string((int16_t)read_le16(p + idx));
            idx += 2;
          }
          break;
        case binlog_type::INT24:
          if (idx + 3 <= len) {
            val_str = std::to_string((int32_t)read_le24(p + idx));
            idx += 3;
          }
          break;
        case binlog_type::LONG:
          if (idx + 4 <= len) {
            val_str = std::to_string((int32_t)read_le32(p + idx));
            idx += 4;
          }
          break;
        case binlog_type::LONGLONG:
          if (idx + 8 <= len) {
            val_str = std::to_string((int64_t)read_le64(p + idx));
            idx += 8;
          }
          break;
        case binlog_type::FLOAT:
          if (idx + 4 <= len) {
            float f;
            std::memcpy(&f, p + idx, 4);
            val_str = shortest_float_text(f);
            idx += 4;
          }
          break;
        case binlog_type::DOUBLE:
          if (idx + 8 <= len) {
            double d;
            std::memcpy(&d, p + idx, 8);
            val_str = shortest_double_text(d);
            idx += 8;
          }
          break;
        case binlog_type::VARCHAR: {
          // Length-prefix width is driven by the schema's declared max
          // length (metadata), not a guess from the data: >= 256 needs a
          // 2-byte prefix, otherwise 1 byte.
          size_t lp = map.columns[c].meta >= 256 ? 2 : 1;
          if (idx + lp <= len) {
            uint32_t str_len = lp == 2 ? read_le16(p + idx) : p[idx];
            idx += lp;
            if (idx + str_len <= len) {
              val_str = std::string((const char*)p + idx, str_len);
              idx += str_len;
            }
          }
          break;
        }
        case binlog_type::VAR_STRING:
        case binlog_type::STRING: {
          if (idx < len) {
            uint16_t str_len = p[idx++];
            if (str_len > 128 && idx < len) {
              str_len = (str_len & 0x7f) | ((uint16_t)p[idx++] << 7);
            }
            if (idx + str_len <= len) {
              val_str = std::string((const char*)p + idx, str_len);
              idx += str_len;
            }
          }
          break;
        }
        case binlog_type::BLOB:
        case binlog_type::TINY_BLOB:
        case binlog_type::MEDIUM_BLOB:
        case binlog_type::LONG_BLOB:
        case binlog_type::JSON: {
          // Pack length comes from metadata (1-4 bytes), not a fixed 2.
          uint8_t pack_len = map.columns[c].meta ? (uint8_t)map.columns[c].meta : 2;
          uint32_t blob_len = 0;
          if (idx + pack_len <= len) {
            for (uint8_t bi = 0; bi < pack_len; ++bi) blob_len |= ((uint32_t)p[idx + bi]) << (8 * bi);
            idx += pack_len;
            if (idx + blob_len <= len) {
              val_str = std::string((const char*)p + idx, blob_len);
              idx += blob_len;
            }
          }
          break;
        }
        case binlog_type::DATE:
          if (idx + 3 <= len) { val_str = decode_date(p + idx); idx += 3; }
          break;
        case binlog_type::TIME:
          if (idx + 3 <= len) { val_str = decode_time_legacy(p + idx); idx += 3; }
          break;
        case binlog_type::TIME2: {
          uint8_t fsp = (uint8_t)map.columns[c].meta;
          size_t fb = frac_bytes_for_fsp(fsp);
          if (idx + 3 + fb <= len) { val_str = decode_time2(p + idx, fsp); idx += 3 + fb; }
          break;
        }
        case binlog_type::DATETIME:
          if (idx + 8 <= len) { val_str = decode_datetime_legacy(p + idx); idx += 8; }
          break;
        case binlog_type::DATETIME2: {
          uint8_t fsp = (uint8_t)map.columns[c].meta;
          size_t fb = frac_bytes_for_fsp(fsp);
          if (idx + 5 + fb <= len) { val_str = decode_datetime2(p + idx, fsp); idx += 5 + fb; }
          break;
        }
        case binlog_type::TIMESTAMP:
          if (idx + 4 <= len) { val_str = decode_timestamp_legacy(p + idx); idx += 4; }
          break;
        case binlog_type::TIMESTAMP2: {
          uint8_t fsp = (uint8_t)map.columns[c].meta;
          size_t fb = frac_bytes_for_fsp(fsp);
          if (idx + 4 + fb <= len) { val_str = decode_timestamp2(p + idx, fsp); idx += 4 + fb; }
          break;
        }
        case binlog_type::YEAR:
          if (idx < len) { val_str = decode_year(p[idx]); idx += 1; }
          break;
        case binlog_type::NEWDECIMAL: {
          size_t need = new_decimal_byte_length(map.columns[c].meta);
          if (need > 0 && idx + need <= len) {
            size_t consumed = 0;
            val_str = decode_new_decimal(p + idx, map.columns[c].meta, &consumed);
            idx += consumed;
          }
          break;
        }
        case binlog_type::BIT: {
          size_t nbytes = (size_t)((map.columns[c].meta >> 8) > 0 ? (map.columns[c].meta & 0xff) + 1
                                                                   : (map.columns[c].meta & 0xff));
          if (nbytes == 0) nbytes = 1;
          if (idx + nbytes <= len) {
            uint64_t v = 0;
            for (size_t bi = 0; bi < nbytes && bi < 8; ++bi) v = (v << 8) | p[idx + bi];
            val_str = std::to_string(v);
            idx += nbytes;
          }
          break;
        }
        case binlog_type::ENUM: {
          size_t nbytes = (map.columns[c].meta != 0) ? ((map.columns[c].meta & 0xff) > 1 ? 2 : 1) : 1;
          if (idx + nbytes <= len) {
            uint32_t v = 0;
            for (size_t bi = 0; bi < nbytes; ++bi) v |= ((uint32_t)p[idx + bi]) << (8 * bi);
            if (v >= 1 && v <= map.columns[c].enum_values.size()) {
              val_str = map.columns[c].enum_values[v - 1];
            } else if (v == 0) {
              val_str = "";
            } else {
              val_str = std::to_string(v);
            }
            idx += nbytes;
          }
          break;
        }
        case binlog_type::SET: {
          size_t nbytes = map.columns[c].meta != 0 ? (map.columns[c].meta & 0xff) : 8;
          if (nbytes == 0 || nbytes > 8) nbytes = 8;
          if (idx + nbytes <= len) {
            uint64_t v = 0;
            for (size_t bi = 0; bi < nbytes; ++bi) v |= ((uint64_t)p[idx + bi]) << (8 * bi);
            if (!map.columns[c].enum_values.empty()) {
              std::string s;
              for (size_t bi = 0; bi < map.columns[c].enum_values.size() && bi < 64; ++bi) {
                if (v & (1ULL << bi)) {
                  if (!s.empty()) s += ",";
                  s += map.columns[c].enum_values[bi];
                }
              }
              val_str = std::move(s);
            } else {
              val_str = std::to_string(v);
            }
            idx += nbytes;
          }
          break;
        }
        default: {
          // Generic fallback for anything not explicitly handled above.
          if (idx + 4 <= len) {
            val_str = std::to_string((int32_t)read_le32(p + idx));
            idx += 4;
          }
          break;
        }
      }

      out_row->push_back({col_name, val_str});
    }

    return true;
  };

  // Decode rows and apply mutations
  while (idx < len) {
    BinlogRowMutation mut;
    mut.schema = map.schema_name;
    mut.table = map.table_name;
    mut.timestamp = ts;

    if (is_delete) {
      mut.kind = BinlogRowMutation::Kind::Delete;
      if (!decode_one_row(cols_present, &mut.values)) break;
      apply_mutation(mut);
      total_deletes_++;
    } else if (is_update) {
      mut.kind = BinlogRowMutation::Kind::Update;
      if (!decode_one_row(cols_present, &mut.before_values)) break;
      if (!decode_one_row(cols_present_after, &mut.values)) break;
      apply_mutation(mut);
      total_updates_++;
    } else {
      // WRITE_ROWS (Insert)
      mut.kind = BinlogRowMutation::Kind::Insert;
      if (!decode_one_row(cols_present, &mut.values)) break;
      apply_mutation(mut);
      total_inserts_++;
    }
  }

  enforce_table_limits(map.table_name);
  total_events_applied_++;
  return Ok();
}

Result<void> BinlogConsumer::apply_mutation(const BinlogRowMutation& mut) {
  if (mut.values.empty() && mut.before_values.empty()) return Ok();

  std::string table = mut.table;
  std::string db_name = mut.schema.empty() ? config_.database : mut.schema;
  // Only the table of that name in the source's own database - never a
  // same-named table elsewhere. The old fallback to the engine's default
  // database is exactly where two tenants' `salesman` tables would have met.
  SqlTable* tbl = db_.resolve_table(table, db_name);
  if (!tbl) {
    std::string key = to_lower_str(db_name + "." + table);
    if (missing_tables_logged_.insert(key).second)
      fprintf(stderr, "[cdc] %s.%s: changes arrive but the mirror holds no such table (not seeded?) - skipped\n",
              db_name.c_str(), table.c_str());
    return Ok();
  }
  std::vector<std::string> pk_names;
  if (tbl) {
    for (const auto& col : tbl->columns) {
      if (col.is_primary_key) pk_names.push_back(to_lower_str(col.name));
    }
  }

  // Every value is a quoted, escaped literal and every name a quoted
  // identifier. They used to go in raw: a value holding an apostrophe (a
  // customer called O'Brien) made the statement unparseable, and the failure
  // was ignored, so that change vanished from the mirror without a word.
  auto lit = [](const std::string& v) {
    if (sql_is_null(v)) return std::string("NULL");
    std::string o = "'";
    for (char ch : v) { if (ch == '\'') o += "''"; else if (ch == '\\') o += "\\\\"; else o += ch; }
    return o + "'";
  };
  auto ident = [](const std::string& n) {
    std::string o = "`";
    for (char ch : n) { if (ch == '`') o += "``"; else o += ch; }
    return o + "`";
  };
  // Which row a before-image names: its primary key when the table has one,
  // otherwise every column of the (FULL) before image - never a guess from
  // column names, which could update or delete a different row.
  auto match = [&](const std::vector<std::pair<std::string, std::string>>& image) {
    std::string w;
    auto add = [&](const std::pair<std::string, std::string>& kv) {
      if (!w.empty()) w += " AND ";
      w += sql_is_null(kv.second) ? ident(kv.first) + " IS NULL" : ident(kv.first) + " = " + lit(kv.second);
    };
    if (!pk_names.empty())
      for (const auto& kv : image)
        for (const auto& pkn : pk_names)
          if (to_lower_str(kv.first) == pkn) { add(kv); break; }
    if (w.empty()) for (const auto& kv : image) add(kv);
    return w;
  };

  std::string sql;
  if (mut.kind == BinlogRowMutation::Kind::Insert) {
    std::string cols, vals, updates;
    for (size_t i = 0; i < mut.values.size(); ++i) {
      if (i > 0) { cols += ", "; vals += ", "; updates += ", "; }
      cols += ident(mut.values[i].first);
      vals += lit(mut.values[i].second);
      updates += ident(mut.values[i].first) + " = " + lit(mut.values[i].second);
    }
    sql = "INSERT INTO " + ident(table) + " (" + cols + ") VALUES (" + vals + ") ON DUPLICATE KEY UPDATE " + updates + ";";
  } else if (mut.kind == BinlogRowMutation::Kind::Update) {
    std::string sets;
    for (size_t i = 0; i < mut.values.size(); ++i) {
      if (i > 0) sets += ", ";
      sets += ident(mut.values[i].first) + " = " + lit(mut.values[i].second);
    }
    const auto& before = mut.before_values.empty() ? mut.values : mut.before_values;
    sql = "UPDATE " + ident(table) + " SET " + sets + " WHERE " + match(before) + ";";
  } else if (mut.kind == BinlogRowMutation::Kind::Delete) {
    const auto& before = mut.before_values.empty() ? mut.values : mut.before_values;
    sql = "DELETE FROM " + ident(table) + " WHERE " + match(before) + ";";
  }
  if (!sql.empty()) {
    static const bool trace = [] { const char* e = getenv("AIOS_CDC_TRACE"); return e && e[0] == '1'; }();
    if (trace) fprintf(stderr, "[cdc-trace]   apply %s: %s\n", db_name.c_str(), sql.c_str());
    auto r = db_.execute_local_query(sql, db_name);
    if (r.success) applied_since_commit_ = true;
    if (!r.success) {
      total_apply_failures_++;
      // Column names, never values: the values are the customer's data.
      std::string names;
      for (const auto& kv : (mut.values.empty() ? mut.before_values : mut.values)) names += (names.empty() ? "" : ",") + kv.first;
      fprintf(stderr, "[cdc] %s.%s: %s not applied (columns %s): %s\n", db_name.c_str(), table.c_str(),
              mut.kind == BinlogRowMutation::Kind::Insert ? "insert" : (mut.kind == BinlogRowMutation::Kind::Update ? "update" : "delete"),
              names.c_str(), r.error_message.c_str());
    }
  }

  return Ok();
}

Result<size_t> BinlogConsumer::sync_views() {
  const std::string dbn = config_.database;
  if (config_.host.empty() || dbn.empty() || to_lower_str(dbn) == "aios_db")
    return Error(Status::Invalid, "no source database is configured to take views from");
  MariaWireSession sess(config_.host, config_.port);
  auto cr = sess.connect(config_.user, config_.password, dbn);
  if (!cr.ok()) return Error(Status::IoError, "view sync: cannot reach the source: " + cr.error().message);
  auto lst = sess.query("SHOW FULL TABLES WHERE Table_type = 'VIEW'");
  if (!lst.ok() || !lst->ok) return Error(Status::IoError, "view sync: listing views failed: " + (lst.ok() ? lst->error_message : lst.error().message));
  auto bt = [](const std::string& s) {
    std::string o = "`";
    for (char c : s) { if (c == '`') o += '`'; o += c; }
    return o + "`";
  };
  // The body after `AS` in a CREATE VIEW text, to tell a changed definition
  // from the same one: what this engine keeps as a view's query.
  auto body_of = [](const std::string& stmt) -> std::string {
    std::string up = to_upper_str(stmt);
    size_t v = up.find(" VIEW ");
    if (v == std::string::npos) return std::string();
    size_t i = v + 6;
    // name, possibly db.name, backtick-quoted
    for (int part = 0; part < 2; ++part) {
      while (i < stmt.size() && stmt[i] == ' ') ++i;
      if (i < stmt.size() && stmt[i] == '`') {
        ++i;
        while (i < stmt.size()) { if (stmt[i] == '`') { if (i + 1 < stmt.size() && stmt[i + 1] == '`') { i += 2; continue; } ++i; break; } ++i; }
      } else {
        while (i < stmt.size() && (std::isalnum((unsigned char)stmt[i]) || stmt[i] == '_' || stmt[i] == '$')) ++i;
      }
      if (i < stmt.size() && stmt[i] == '.') { ++i; continue; }
      break;
    }
    while (i < stmt.size() && stmt[i] == ' ') ++i;
    if (i < stmt.size() && stmt[i] == '(') {                          // column aliases
      int depth = 0;
      for (; i < stmt.size(); ++i) { if (stmt[i] == '(') ++depth; else if (stmt[i] == ')' && --depth == 0) { ++i; break; } }
    }
    while (i < stmt.size() && stmt[i] == ' ') ++i;
    if (up.compare(i, 2, "AS") != 0) return std::string();
    std::string body = trim_str(stmt.substr(i + 2));
    while (!body.empty() && body.back() == ';') body.pop_back();
    return trim_str(body);
  };
  size_t changed = 0, checked = 0;
  for (const auto& row : lst->rows) {
    if (row.empty()) continue;
    const std::string name = row[0].bytes;
    auto sc = sess.query("SHOW CREATE VIEW " + bt(name));
    if (!sc.ok() || !sc->ok || sc->rows.empty() || sc->rows[0].size() < 2) continue;
    const std::string create = sc->rows[0][1].bytes;
    ++checked;
    std::string have_body;
    const bool have = db_.view_body(name, dbn, &have_body);
    const std::string want = body_of(create);
    if (have && !want.empty() && trim_str(have_body) == want) continue;   // already the source's
    std::string stmt = create;
    if (to_upper_str(stmt).rfind("CREATE OR REPLACE", 0) != 0) stmt = "CREATE OR REPLACE" + stmt.substr(6);
    auto vr = db_.execute_local_query(stmt, dbn);
    if (vr.success) {
      ++changed;
      fprintf(stderr, "[cdc] view %s.%s %s from the source\n", dbn.c_str(), name.c_str(), have ? "replaced" : "created");
    } else {
      last_error_ = vr.error_message;
      fprintf(stderr, "[cdc] view %s.%s not synced: %s\n", dbn.c_str(), name.c_str(), vr.error_message.c_str());
    }
  }
  return changed;
}

Result<void> BinlogConsumer::start() {
  hal::LockGuard lock(mu_);
  if (running_) return Ok();
  running_ = true;

  worker_thread_ = std::make_unique<hal::Thread>([this]() {
    sql_set_cdc_thread(true);   // the Things react at each transaction's commit
    worker_loop();
  });
  return Ok();
}

void BinlogConsumer::stop() {
  running_ = false;
  connected_ = false;
  if (conn_) {
    conn_->close();
  }
  if (worker_thread_ && worker_thread_->joinable()) {
    worker_thread_->join();
    worker_thread_.reset();
  }
}

namespace {
// How long replicated changes may sit only in memory. A restart replays at
// most this much binlog, which is cheap; the store write is not, so it is not
// done per event -- save_tables serialises the whole store into one buffer
// before writing it, so on chi-il-1 each flush is a transient 1.35 GB against
// a 4.8 GB resident set. Five minutes of binlog replays in seconds; five
// minutes of that allocation every minute is 22 MB/s of disk for nothing.
constexpr uint64_t kCdcFlushIntervalMs = 300000;
uint64_t cdc_now_ms() {
  return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

void BinlogConsumer::worker_loop() {
  // Replicated rows only ever existed in memory: nothing on this path wrote
  // the store or the binlog position, so a restart came back to whenever a
  // local statement last happened to checkpoint and then resumed from the
  // master's current offset, dropping the difference. Writing both together on
  // an interval bounds that to one interval of binlog, which replays quickly.
  uint64_t last_flush_ms = cdc_now_ms();
  uint64_t applied_at_flush = total_events_applied_;
  std::string saved_pos_file;
  uint64_t saved_pos = 0;
  std::string last_logged;
  // Views are re-read from the source at start and on an interval
  // (AIOS_VIEW_SYNC_SECS, default 300; 0 = never), so a definition that
  // changed while this mirror was down, or whose DDL event never applied,
  // does not stay wrong.
  const uint64_t view_sync_ms = [] {
    const char* e = getenv("AIOS_VIEW_SYNC_SECS");
    return (e && e[0]) ? (uint64_t)strtoull(e, nullptr, 10) * 1000 : (uint64_t)300000;
  }();
  bool views_synced_once = false;
  while (running_) {
    if (view_sync_ms > 0 && connected_ && (!views_synced_once || cdc_now_ms() - last_view_sync_ms_ >= view_sync_ms)) {
      views_synced_once = true;
      last_view_sync_ms_ = cdc_now_ms();
      auto vs = sync_views();
      if (!vs.ok()) fprintf(stderr, "[cdc] view sync: %s\n", vs.error().message.c_str());
    }
    auto r = step(config_.poll_interval_ms);
    db_.publish_table_lists();
    if (!r.ok()) {
      // Said once per distinct failure. It used to be swallowed: a replica
      // that could not connect sat at "Connecting to master" indefinitely
      // with an empty Last_IO_Error, looking healthy.
      if (r.error().message != last_logged) {
        last_logged = r.error().message;
        fprintf(stderr, "[cdc] stream error (retrying): %s\n", last_logged.c_str());
        fflush(stderr);
      }
      if (config_.auto_reconnect && running_) {
        hal::sleep_ms(config_.reconnect_backoff_ms);
      } else {
        break;
      }
    }
    uint64_t now_ms = cdc_now_ms();
    if (now_ms - last_flush_ms >= kCdcFlushIntervalMs) {
      if (total_events_applied_ != applied_at_flush) {
        db_.flush_now();
        applied_at_flush = total_events_applied_;
      } else {
        // Nothing applied since the store was last written, so it already
        // matches the newest commit read: move the resume point up to it.
        // Without this a mirror whose tables are quiet (while the source's
        // other tables are busy) kept restarting from hours-old positions
        // and re-reading every event since.
        std::string file;
        uint64_t pos = 0;
        if (committed_position(&file, &pos) && (file != saved_pos_file || pos != saved_pos)) {
          if (db_.save_cdc_position().ok()) { saved_pos_file = file; saved_pos = pos; }
        }
        db_.save_index_list();
      }
      // Time passes for the Things too: a window keyed to the date moves at
      // midnight whether or not anything was written.
      db_.things_tick();
      last_flush_ms = now_ms;
    }
  }
  if (running_ || total_events_applied_ != applied_at_flush) {
    db_.flush_now();
  }
  connected_ = false;
}

}  // namespace aios::userland
