#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "aios/hal/sync.hpp"

namespace aios::userland {

// One statement of a committed group, as the redo log recorded it.
struct BinlogStatement {
  std::string db;
  std::string sql;
  uint32_t when = 0;        // the clock it ran on (NOW() on a replica), 0 = the time it is written
  uint64_t insert_id = 0;   // the first AUTO_INCREMENT value it generated, 0 = none
};

// ASQL's own binary log: what makes it a binlog master. Statement-based,
// MySQL binlog v4 files (asql-bin.000001 ...) with an index file, checksum
// NONE, written from the same committed statements the redo log keeps. A
// MySQL or MariaDB replica pointed at ASQL (CHANGE MASTER TO ... MASTER_LOG_FILE
// = 'asql-bin.000001', MASTER_LOG_POS = 4) reads them over COM_BINLOG_DUMP.
class AsqlBinlog {
 public:
  ~AsqlBinlog();
  // Opens (or starts) the log under dir. A file left by an earlier run is
  // cut back to its last whole group, closed with a ROTATE, and a new file
  // started, as MySQL starts a new binlog at every restart.
  bool open(const std::string& dir, uint32_t server_id, std::string* err);
  // A committed group: autocommit statements are one group each, a
  // transaction's statements one group. DDL is written on its own, outside
  // BEGIN ... XID, as a server writes it.
  void append_group(const std::vector<BinlogStatement>& stmts);
  // Closes the current file with a ROTATE and starts the next (FLUSH BINARY LOGS).
  void rotate();
  // Deletes the files before `file` (PURGE BINARY LOGS TO 'file').
  bool purge_to(const std::string& file, std::string* err);

  struct FileInfo { std::string name; uint64_t size; };
  std::vector<FileInfo> files() const;
  std::string current_file() const;
  uint64_t current_pos() const;
  uint32_t server_id() const { return server_id_; }
  const std::string& dir() const { return dir_; }
  // For a reader: how far `file` may be read (the end of its last whole
  // event), whether it is the file being written, and the name of the file
  // after it ("" for the current one). False when the index has no `file`.
  bool readable_end(const std::string& file, uint64_t* end, bool* active, std::string* next) const;
  // Waits up to ms for anything written past (file, pos), or a new file.
  void wait_past(const std::string& file, uint64_t pos, uint32_t ms) const;

  uint64_t max_file_bytes = 256ull << 20;   // rotate past this
  uint64_t keep_bytes = 2ull << 30;         // oldest files purged past this, at a rotation

 private:
  std::string dir_;
  uint32_t server_id_ = 0;
  int fd_ = -1;
  std::vector<std::string> names_;   // the index, oldest first
  std::string cur_;
  uint64_t end_ = 0;
  uint64_t xid_ = 0;
  mutable aios::hal::Mutex mu_;
  mutable aios::hal::CondVar cv_;
  bool start_file_locked(std::string* err);
  void rotate_locked();
  void write_index_locked();
  void purge_old_locked();
  void put_header(std::string* out, uint8_t type, uint32_t when, uint32_t body_len, uint16_t flags) const;
  void put_query(std::string* out, uint32_t when, const std::string& db, const std::string& sql, bool ddl) const;
};

// asql-bin.NNNNNN
std::string asql_binlog_name(uint64_t n);

}  // namespace aios::userland
