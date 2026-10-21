#include "aios/userland/asql_binlog.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>

namespace aios::userland {
namespace {

// fdatasync where there is one. macOS has none, and its fsync does not reach the drive (F_FULLFSYNC does).
inline int datasync(int fd) {
#if defined(__APPLE__)
  if (::fcntl(fd, F_FULLFSYNC, 0) != -1) return 0;
  return ::fsync(fd);
#else
  return ::fdatasync(fd);
#endif
}

constexpr uint8_t kQuery = 2, kRotate = 4, kIntvar = 5, kFormatDescription = 15, kXid = 16;
constexpr uint32_t kHeader = 19;
// The FORMAT_DESCRIPTION a MySQL 5.7 server writes (the version this server
// answers to): post-header lengths of event types 1..38.
constexpr uint8_t kPostHeaderLens[38] = {56, 13, 0, 8, 0, 18, 0, 4, 4, 4, 4, 18, 0, 0, 95, 0, 4, 26, 8,
                                         0,  0,  0, 8, 8, 8, 2, 0, 0, 0, 10, 10, 10, 42, 42, 0, 18, 52, 0};
constexpr char kServerVersion[] = "5.7.44-aios-asql-log";

void put_u16(std::string* o, uint16_t v) { o->push_back((char)(v & 0xff)); o->push_back((char)(v >> 8)); }
void put_u32(std::string* o, uint32_t v) { for (int i = 0; i < 4; ++i) o->push_back((char)((v >> (8 * i)) & 0xff)); }
void put_u64(std::string* o, uint64_t v) { for (int i = 0; i < 8; ++i) o->push_back((char)((v >> (8 * i)) & 0xff)); }
uint32_t get_u32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

bool write_all(int fd, const char* p, size_t n) {
  while (n) {
    const ssize_t w = ::write(fd, p, n);
    if (w < 0) { if (errno == EINTR) continue; return false; }
    p += w;
    n -= (size_t)w;
  }
  return true;
}

uint64_t file_size(const std::string& path) {
  struct stat st;
  return ::stat(path.c_str(), &st) == 0 ? (uint64_t)st.st_size : 0;
}

// DDL commits on its own in MySQL and is logged outside BEGIN ... COMMIT.
bool is_ddl(const std::string& sql) {
  size_t i = 0;
  for (;;) {
    while (i < sql.size() && (unsigned char)sql[i] <= ' ') ++i;
    if (sql.compare(i, 2, "/*") == 0) {
      const size_t e = sql.find("*/", i + 2);
      if (e == std::string::npos) return false;
      i = e + 2;
      continue;
    }
    break;
  }
  size_t j = i;
  while (j < sql.size() && std::isalpha((unsigned char)sql[j])) ++j;
  std::string w = sql.substr(i, j - i);
  for (auto& c : w) c = (char)std::toupper((unsigned char)c);
  return w == "CREATE" || w == "ALTER" || w == "DROP" || w == "RENAME" || w == "TRUNCATE";
}

}  // namespace

std::string asql_binlog_name(uint64_t n) {
  char b[32];
  std::snprintf(b, sizeof b, "asql-bin.%06llu", (unsigned long long)n);
  return b;
}

AsqlBinlog::~AsqlBinlog() {
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  if (fd_ >= 0) { datasync(fd_); ::close(fd_); fd_ = -1; }
}

void AsqlBinlog::put_header(std::string* out, uint8_t type, uint32_t when, uint32_t body_len, uint16_t flags) const {
  // log_pos is where the event ENDS, in the file being written.
  const uint64_t at = end_ + out->size();
  put_u32(out, when);
  out->push_back((char)type);
  put_u32(out, server_id_);
  put_u32(out, kHeader + body_len);
  put_u32(out, (uint32_t)(at + kHeader + body_len));
  put_u16(out, flags);
}

void AsqlBinlog::put_query(std::string* out, uint32_t when, const std::string& db, const std::string& sql, bool ddl) const {
  std::string sv;
  sv.push_back(0);   // Q_FLAGS2_CODE: foreign keys are not checked again on the replica - this server already did
  put_u32(&sv, 0x04000000u);
  sv.push_back(1);   // Q_SQL_MODE_CODE: NO_ENGINE_SUBSTITUTION, what this server answers for @@sql_mode
  put_u64(&sv, 1ull << 30);
  sv.push_back(6);   // Q_CATALOG_NZ_CODE
  sv.push_back(3);
  sv += "std";
  sv.push_back(4);   // Q_CHARSET_CODE: utf8mb4_general_ci client, connection and server
  put_u16(&sv, 45);
  put_u16(&sv, 45);
  put_u16(&sv, 45);
  const std::string d = db.size() > 255 ? db.substr(0, 255) : db;
  const uint32_t body = 13 + (uint32_t)sv.size() + (uint32_t)d.size() + 1 + (uint32_t)sql.size();
  put_header(out, kQuery, when, body, 0);
  put_u32(out, 1);   // thread id: one session's statements, in commit order
  put_u32(out, 0);
  out->push_back((char)d.size());
  put_u16(out, 0);
  put_u16(out, (uint16_t)sv.size());
  *out += sv;
  *out += d;
  out->push_back(0);
  *out += sql;
  (void)ddl;
}

bool AsqlBinlog::open(const std::string& dir, uint32_t server_id, std::string* err) {
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  dir_ = dir;
  server_id_ = server_id ? server_id : 1;
  ::mkdir(dir_.c_str(), 0700);
  names_.clear();
  {
    std::ifstream in(dir_ + "/asql-bin.index");
    std::string line;
    while (std::getline(in, line)) {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
      const size_t slash = line.rfind('/');
      if (slash != std::string::npos) line = line.substr(slash + 1);
      if (!line.empty() && file_size(dir_ + "/" + line) >= 4) names_.push_back(line);
    }
  }
  if (!names_.empty()) {
    // The last file of the earlier run: cut back to its last whole group (a
    // crash can leave half an event, or a BEGIN with no XID after it), then
    // closed with a ROTATE so a reader goes on into the new file.
    const std::string path = dir_ + "/" + names_.back();
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) { *err = "cannot open " + path; return false; }
    const uint64_t size = file_size(path);
    uint64_t at = 4, good = 4;
    uint8_t last_type = 0;   // of the last whole event kept
    bool in_group = false;
    uint8_t h[kHeader];
    while (at + kHeader <= size) {
      if (::pread(fd, h, kHeader, (off_t)at) != (ssize_t)kHeader) break;
      const uint32_t len = get_u32(h + 9);
      if (len < kHeader || at + len > size) break;
      const uint8_t type = h[4];
      if (type == kQuery) {
        std::string body(len - kHeader, '\0');
        if (::pread(fd, body.data(), body.size(), (off_t)(at + kHeader)) != (ssize_t)body.size()) break;
        if (body.size() >= 13) {
          const size_t db_len = (uint8_t)body[8];
          const size_t sv_len = (uint8_t)body[11] | ((size_t)(uint8_t)body[12] << 8);
          const size_t q = 13 + sv_len + db_len + 1;
          const std::string text = q <= body.size() ? body.substr(q) : std::string();
          if (text == "BEGIN") in_group = true;
          else if (text == "COMMIT") in_group = false;
        }
      } else if (type == kXid) {
        in_group = false;
      }
      at += len;
      if (!in_group) { good = at; last_type = type; }
      if (type == kRotate) break;
    }
    if (good < size && ::ftruncate(fd, (off_t)good) != 0) { ::close(fd); *err = "cannot cut back " + path; return false; }
    fd_ = fd;
    cur_ = names_.back();
    end_ = good;
    if (last_type == kRotate) {   // closed already: the new file is the one it names
      ::close(fd_);
      fd_ = -1;
      const uint64_t n = std::strtoull(cur_.c_str() + cur_.find('.') + 1, nullptr, 10);
      cur_ = asql_binlog_name(n + 1);
      return start_file_locked(err);
    }
    ::lseek(fd_, 0, SEEK_END);
    rotate_locked();
    if (fd_ < 0) { *err = "cannot start the next binlog file in " + dir_; return false; }
    return true;
  }
  cur_ = asql_binlog_name(1);
  return start_file_locked(err);
}

// A new file `cur_`: the magic and its FORMAT_DESCRIPTION, named in the index.
bool AsqlBinlog::start_file_locked(std::string* err) {
  const std::string path = dir_ + "/" + cur_;
  fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd_ < 0) { if (err) *err = "cannot create " + path; return false; }
  std::string b = "\xfe" "bin";
  std::string fde;
  end_ = 4;   // where the FORMAT_DESCRIPTION starts
  const uint32_t body = 2 + 50 + 4 + 1 + sizeof kPostHeaderLens + 1 + 4;
  put_header(&fde, kFormatDescription, (uint32_t)std::time(nullptr), body, 0);
  put_u16(&fde, 4);
  std::string ver(50, '\0');
  std::memcpy(ver.data(), kServerVersion, sizeof kServerVersion - 1);
  fde += ver;
  put_u32(&fde, 0);   // not "created at startup": a replica keeps its temporary tables
  fde.push_back((char)kHeader);
  fde.append((const char*)kPostHeaderLens, sizeof kPostHeaderLens);
  fde.push_back(0);   // checksum algorithm: none (the 4 bytes after are there regardless)
  put_u32(&fde, 0);
  b += fde;
  if (!write_all(fd_, b.data(), b.size())) { if (err) *err = "cannot write " + path; ::close(fd_); fd_ = -1; return false; }
  end_ = b.size();
  if (names_.empty() || names_.back() != cur_) names_.push_back(cur_);
  write_index_locked();
  cv_.notify_all();
  return true;
}

void AsqlBinlog::write_index_locked() {
  const std::string path = dir_ + "/asql-bin.index", tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    for (const auto& n : names_) out << "./" << n << "\n";
  }
  std::rename(tmp.c_str(), path.c_str());
}

void AsqlBinlog::rotate_locked() {
  if (fd_ < 0) return;
  const uint64_t n = std::strtoull(cur_.c_str() + cur_.find('.') + 1, nullptr, 10);
  const std::string next = asql_binlog_name(n + 1);
  std::string ev;
  put_header(&ev, kRotate, (uint32_t)std::time(nullptr), 8 + (uint32_t)next.size(), 0);
  put_u64(&ev, 4);
  ev += next;
  write_all(fd_, ev.data(), ev.size());
  datasync(fd_);
  ::close(fd_);
  fd_ = -1;
  cur_ = next;
  start_file_locked(nullptr);
  purge_old_locked();
}

void AsqlBinlog::rotate() {
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  rotate_locked();
}

void AsqlBinlog::purge_old_locked() {
  uint64_t total = 0;
  for (const auto& n : names_) total += file_size(dir_ + "/" + n);
  bool changed = false;
  while (names_.size() > 1 && total > keep_bytes) {
    const std::string path = dir_ + "/" + names_.front();
    total -= file_size(path);
    ::unlink(path.c_str());   // a reader still on it keeps reading its open file
    names_.erase(names_.begin());
    changed = true;
  }
  if (changed) write_index_locked();
}

bool AsqlBinlog::purge_to(const std::string& file, std::string* err) {
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  size_t i = 0;
  while (i < names_.size() && names_[i] != file) ++i;
  if (i == names_.size()) { *err = "Target log not found in binlog index"; return false; }
  for (size_t k = 0; k < i; ++k) ::unlink((dir_ + "/" + names_[k]).c_str());
  names_.erase(names_.begin(), names_.begin() + (long)i);
  write_index_locked();
  return true;
}

void AsqlBinlog::append_group(const std::vector<BinlogStatement>& stmts) {
  if (stmts.empty()) return;
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  if (fd_ < 0) return;
  const uint32_t now = (uint32_t)std::time(nullptr);
  std::string out;
  bool open_txn = false;
  std::string txn_db;
  uint32_t last_when = now;
  auto commit = [&] {
    if (!open_txn) return;
    put_header(&out, kXid, last_when, 8, 0);
    put_u64(&out, ++xid_);
    open_txn = false;
  };
  for (const auto& s : stmts) {
    const uint32_t when = s.when ? s.when : now;
    if (is_ddl(s.sql)) {
      commit();
      put_query(&out, when, s.db, s.sql, true);
      continue;
    }
    if (!open_txn) {
      put_query(&out, when, s.db, "BEGIN", false);
      open_txn = true;
    }
    if (s.insert_id) {
      put_header(&out, kIntvar, when, 9, 0);
      out.push_back(2);   // INSERT_ID
      put_u64(&out, s.insert_id);
    }
    put_query(&out, when, s.db, s.sql, false);
    last_when = when;
  }
  commit();
  if (!write_all(fd_, out.data(), out.size())) {
    // Never half a group: what did get written is cut off again.
    if (::ftruncate(fd_, (off_t)end_) != 0) {}
    ::lseek(fd_, (off_t)end_, SEEK_SET);
    std::fprintf(stderr, "asql binlog: write to %s failed; a group was not logged\n", cur_.c_str());
    return;
  }
  end_ += out.size();
  if (end_ >= max_file_bytes) rotate_locked();
  cv_.notify_all();
}

std::vector<AsqlBinlog::FileInfo> AsqlBinlog::files() const {
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  std::vector<FileInfo> r;
  for (const auto& n : names_) r.push_back({n, n == cur_ ? end_ : file_size(dir_ + "/" + n)});
  return r;
}

std::string AsqlBinlog::current_file() const {
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  return cur_;
}

uint64_t AsqlBinlog::current_pos() const {
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  return end_;
}

bool AsqlBinlog::readable_end(const std::string& file, uint64_t* end, bool* active, std::string* next) const {
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  for (size_t i = 0; i < names_.size(); ++i) {
    if (names_[i] != file) continue;
    *active = file == cur_;
    *end = *active ? end_ : file_size(dir_ + "/" + file);
    *next = i + 1 < names_.size() ? names_[i + 1] : std::string();
    return true;
  }
  return false;
}

void AsqlBinlog::wait_past(const std::string& file, uint64_t pos, uint32_t ms) const {
  std::unique_lock<aios::hal::Mutex> lk(mu_);
  cv_.wait_for_ms(lk, ms, [&] { return cur_ != file || end_ > pos; });
}

}  // namespace aios::userland
