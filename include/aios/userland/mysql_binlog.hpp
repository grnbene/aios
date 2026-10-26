// AIOS — Native MySQL / MariaDB Binlog CDC Streaming Consumer
//
// Streams binary replication logs from MySQL/MariaDB in real-time over native
// replication wire protocol (COM_REGISTER_SLAVE + COM_BINLOG_DUMP) directly
// into AIOS ASQL tables.
//
// Features:
// 1. Zero-RAM-Bloat Streaming: Ingress filtering at TABLE_MAP and ROW_EVENT
//    headers drops unwhitelisted tables and columns with O(1) memory overhead.
// 2. Column Projection: Whitelist specific columns to strip large BLOBs/TEXTs.
// 3. Hot Working Set & Max Row Eviction: Enforce per-table capacity bounds.
// 4. Binary Row Decoders: Supports MySQL Binlog v4 (TableMap, WriteRows v1/v2,
//    UpdateRows v1/v2, DeleteRows v1/v2, Query DDL, Rotate, XID, Heartbeat).
// 5. Background Streaming Daemon: Resilient auto-reconnect, position tracking,
//    and statistics inspection.

#pragma once

#include "aios/core/result.hpp"
#include "aios/hal/net.hpp"
#include "aios/hal/sync.hpp"
#include "aios/kernel/kernel.hpp"
#include "aios/userland/sql_database.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace aios::userland {

// Binlog Event Type codes (MySQL / MariaDB replication protocol v4)
namespace binlog_event {
constexpr uint8_t UNKNOWN_EVENT = 0x00;
constexpr uint8_t START_EVENT_V3 = 0x01;
constexpr uint8_t QUERY_EVENT = 0x02;
constexpr uint8_t STOP_EVENT = 0x03;
constexpr uint8_t ROTATE_EVENT = 0x04;
constexpr uint8_t INTVAR_EVENT = 0x05;
constexpr uint8_t LOAD_EVENT = 0x06;
constexpr uint8_t SLAVE_EVENT = 0x07;
constexpr uint8_t CREATE_FILE_EVENT = 0x08;
constexpr uint8_t APPEND_BLOCK_EVENT = 0x09;
constexpr uint8_t EXEC_LOAD_EVENT = 0x0a;
constexpr uint8_t DELETE_FILE_EVENT = 0x0b;
constexpr uint8_t NEW_LOAD_EVENT = 0x0c;
constexpr uint8_t RAND_EVENT = 0x0d;
constexpr uint8_t USER_VAR_EVENT = 0x0e;
constexpr uint8_t FORMAT_DESCRIPTION_EVENT = 0x0f;
constexpr uint8_t XID_EVENT = 0x10;
constexpr uint8_t BEGIN_LOAD_QUERY_EVENT = 0x11;
constexpr uint8_t EXECUTE_LOAD_QUERY_EVENT = 0x12;
constexpr uint8_t TABLE_MAP_EVENT = 0x13;
constexpr uint8_t PRE_GA_WRITE_ROWS_EVENT = 0x14;
constexpr uint8_t PRE_GA_UPDATE_ROWS_EVENT = 0x15;
constexpr uint8_t PRE_GA_DELETE_ROWS_EVENT = 0x16;
constexpr uint8_t WRITE_ROWS_EVENT_V1 = 0x17;
constexpr uint8_t UPDATE_ROWS_EVENT_V1 = 0x18;
constexpr uint8_t DELETE_ROWS_EVENT_V1 = 0x19;
constexpr uint8_t INCIDENT_EVENT = 0x1a;
constexpr uint8_t HEARTBEAT_LOG_EVENT = 0x1b;
constexpr uint8_t IGNORABLE_LOG_EVENT = 0x1c;
constexpr uint8_t ROWS_QUERY_LOG_EVENT = 0x1d;
constexpr uint8_t WRITE_ROWS_EVENT_V2 = 0x1e;
constexpr uint8_t UPDATE_ROWS_EVENT_V2 = 0x1f;
constexpr uint8_t DELETE_ROWS_EVENT_V2 = 0x20;
constexpr uint8_t GTID_LOG_EVENT = 0x21;
constexpr uint8_t ANONYMOUS_GTID_LOG_EVENT = 0x22;
constexpr uint8_t PREVIOUS_GTIDS_LOG_EVENT = 0x23;

// MariaDB specific extensions
constexpr uint8_t MARIADB_ANNOTATE_ROWS_EVENT = 0xa0;
constexpr uint8_t MARIADB_BINLOG_CHECKPOINT_EVENT = 0xa1;
constexpr uint8_t MARIADB_GTID_EVENT = 0xa2;
constexpr uint8_t MARIADB_GTID_LIST_EVENT = 0xa3;
constexpr uint8_t MARIADB_START_ENCRYPTION_EVENT = 0xa4;
}  // namespace binlog_event

// MySQL Column Data Types in Binlog payloads
namespace binlog_type {
constexpr uint8_t DECIMAL = 0x00;
constexpr uint8_t TINY = 0x01;
constexpr uint8_t SHORT = 0x02;
constexpr uint8_t LONG = 0x03;
constexpr uint8_t FLOAT = 0x04;
constexpr uint8_t DOUBLE = 0x05;
constexpr uint8_t NULL_TYPE = 0x06;
constexpr uint8_t TIMESTAMP = 0x07;
constexpr uint8_t LONGLONG = 0x08;
constexpr uint8_t INT24 = 0x09;
constexpr uint8_t DATE = 0x0a;
constexpr uint8_t TIME = 0x0b;
constexpr uint8_t DATETIME = 0x0c;
constexpr uint8_t YEAR = 0x0d;
constexpr uint8_t NEWDATE = 0x0e;
constexpr uint8_t VARCHAR = 0x0f;
constexpr uint8_t BIT = 0x10;
constexpr uint8_t TIMESTAMP2 = 0x11;
constexpr uint8_t DATETIME2 = 0x12;
constexpr uint8_t TIME2 = 0x13;
constexpr uint8_t JSON = 0xf5;
constexpr uint8_t NEWDECIMAL = 0xf6;
constexpr uint8_t ENUM = 0xf7;
constexpr uint8_t SET = 0xf8;
constexpr uint8_t TINY_BLOB = 0xf9;
constexpr uint8_t MEDIUM_BLOB = 0xfa;
constexpr uint8_t LONG_BLOB = 0xfb;
constexpr uint8_t BLOB = 0xfc;
constexpr uint8_t VAR_STRING = 0xfd;
constexpr uint8_t STRING = 0xfe;
constexpr uint8_t GEOMETRY = 0xff;
}  // namespace binlog_type

// Metadata for a column in a mapped table
struct BinlogColumnMeta {
  std::string name;
  uint8_t type = binlog_type::VARCHAR;
  uint16_t meta = 0;
  bool nullable = true;
  bool is_pk = false;
  std::vector<std::string> enum_values;
  // A latin1 (cp1252) text column: the binlog carries its bytes as stored, and
  // the store is UTF-8, so they are converted as MariaDB converts them for a
  // utf8 client. The binlog itself does not say (binlog_row_metadata=NO_LOG).
  bool latin1 = false;
  // FLOAT(M,D) / DOUBLE(M,D): the D decimals MariaDB shows the value with;
  // -1 for a plain FLOAT / DOUBLE.
  int shown_decimals = -1;
};

// Schema and column structure recorded in TABLE_MAP_EVENT
struct BinlogTableMap {
  uint64_t table_id = 0;
  std::string schema_name;
  std::string table_name;
  std::vector<BinlogColumnMeta> columns;
  bool whitelisted = true;
  std::unordered_set<std::string> column_whitelist;
};

// One row mutation (Insert, Update, Delete)
struct BinlogRowMutation {
  enum class Kind { Insert, Update, Delete };
  Kind kind = Kind::Insert;
  std::string schema;
  std::string table;
  std::vector<std::pair<std::string, std::string>> values;         // (column_name, value)
  std::vector<std::pair<std::string, std::string>> before_values;  // For UPDATE / DELETE
  uint64_t timestamp = 0;
};

// Configuration for Binlog Replication Consumer
struct BinlogConsumerConfig {
  std::string host = "127.0.0.1";
  uint16_t port = 3306;
  std::string user = "root";
  std::string password;
  std::string database = "aios_db";
  uint32_t server_id = 8901;  // Unique replica server-id

  std::string binlog_filename;  // Start binlog filename (empty = auto-discover)
  uint32_t binlog_position = 4; // Start offset in binlog file (default: 4)
  bool auto_discover_position = true; // Query SHOW MASTER STATUS if binlog_filename empty

  // Filtering & Working-Set RAM Controls
  std::unordered_set<std::string> table_whitelist; // Tables to sync (empty = sync all)
  std::unordered_map<std::string, std::unordered_set<std::string>> column_whitelists; // Table -> columns
  std::unordered_map<std::string, size_t> table_max_rows; // Enforce FIFO/LRU eviction
  // Table -> (binlog file, position) of the consistent snapshot it was seeded
  // from. Row events for that table ending at or before it are already in the
  // seeded rows and are skipped; a table added to the whitelist while the
  // stream sits behind its snapshot would otherwise take them twice.
  std::unordered_map<std::string, std::pair<std::string, uint64_t>> table_seed_positions;

  bool replicate_ddl = true; // Replicate CREATE/ALTER/TRUNCATE from QUERY_EVENT
  bool auto_schema_sync = true; // Query remote master for table column names on first map
  size_t batch_commit_size = 50;
  uint32_t poll_interval_ms = 10;
  bool auto_reconnect = true;
  uint32_t reconnect_backoff_ms = 1000;
};

// Real-time metrics for binlog CDC stream
struct BinlogConsumerStats {
  bool is_running = false;
  bool is_connected = false;
  std::string current_binlog_file;
  uint64_t current_binlog_pos = 0;
  uint64_t total_events_received = 0;
  uint64_t total_events_applied = 0;
  uint64_t total_events_skipped = 0;
  uint64_t total_apply_failures = 0;
  uint64_t total_inserts = 0;
  uint64_t total_updates = 0;
  uint64_t total_deletes = 0;
  uint64_t total_ddl_executed = 0;
  uint64_t total_row_evictions = 0;
  uint64_t last_event_timestamp = 0;
  uint64_t seconds_behind_master = 0;
  std::string last_error;
};

class BinlogConsumer {
 public:
  BinlogConsumer(Kernel& kernel, SqlDatabase& db, const BinlogConsumerConfig& config = {});
  ~BinlogConsumer();

  // No copies
  BinlogConsumer(const BinlogConsumer&) = delete;
  BinlogConsumer& operator=(const BinlogConsumer&) = delete;

  // Brings the mirror's views to the source's current definitions: every view
  // the source database holds, by SHOW CREATE VIEW, is created or replaced
  // here when it is missing or reads differently. Returns how many changed.
  // The binlog carries view DDL too (applied as it arrives), but a mirror
  // seeded from rows alone, restarted past a change, or fed a statement it
  // could not run drifts otherwise - st's stocktrans lost a join that way and
  // its sales totals dropped a line per item with two manufacturer rows.
  // Safe from any thread: each statement it runs takes the database's own lock.
  Result<size_t> sync_views();

  // Lifecycle control
  Result<void> start();
  void stop();
  bool is_running() const { return running_; }
  bool is_connected() const { return connected_; }

  // Step-by-step or polled processing
  Result<size_t> step(int timeout_ms = 20);

  // Configuration modifiers
  void configure(const BinlogConsumerConfig& config);
  const BinlogConsumerConfig& config() const { return config_; }

  void set_table_whitelist(const std::vector<std::string>& tables);
  void set_column_whitelist(const std::string& table, const std::vector<std::string>& columns);
  void set_table_max_rows(const std::string& table, size_t max_rows);
  void set_table_seed_position(const std::string& table, const std::string& file, uint64_t pos);
  // The position just past the last whole transaction seen - the only safe
  // place to resume (inside a transaction its TABLE_MAP would be skipped).
  bool committed_position(std::string* file, uint64_t* pos) const;

  // Status & Telemetry
  BinlogConsumerStats stats() const;

  // Direct packet injector for testing & synthetic streams
  Result<void> ingest_raw_event(const uint8_t* data, size_t len);

  // Manually apply row mutation
  Result<void> apply_mutation(const BinlogRowMutation& mut);
  // DELETEs by a single-column key, held back and erased together (flush_pending_deletes): a delete rewrites every
  // column of the table, so one pass for a transaction's worth costs what one row's delete did. Anything else that
  // changes the mirror, a commit and DDL flush it first, so the order of changes is the binlog's.
  void flush_pending_deletes();
  std::string pending_del_db_, pending_del_table_, pending_del_column_;
  std::vector<std::string> pending_del_keys_;
  // DELETEs from a table without a primary key, held back the same way: each row named by its whole before-image
  // (in the table's column order), erased by one DELETE ... MATCHING - one row per image, found in one pass. Each
  // used to be its own DELETE, a scan of the table and a rewrite of every column: a purge of audittrail (31M
  // rows, no key) ran at twenty rows a second and left the mirror a day behind.
  std::vector<std::vector<std::string>> pending_img_rows_;
  // INSERTs, held back the same way and written as one multi-row INSERT per table (the same columns, in the
  // binlog's order): a statement per row was most of what a bulk load at the source cost here.
  std::vector<std::string> pending_ins_cols_;
  std::vector<std::vector<std::string>> pending_ins_rows_;
  void flush_pending_inserts();
  size_t pending_img_bytes_ = 0;
  bool defer_deletes_ = false;           // set while the stream's row events are applied; a direct apply_mutation() is immediate
  uint64_t pending_del_since_ms_ = 0;    // when the first held delete arrived: held no longer than a moment past that

 private:
  Kernel& kernel_;
  SqlDatabase& db_;
  BinlogConsumerConfig config_;

  std::unique_ptr<hal::Connection> conn_;
  std::unique_ptr<hal::Thread> worker_thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> connected_{false};
  mutable hal::Mutex mu_;

  // Protocol state
  uint8_t seq_ = 0;
  std::string active_binlog_file_;
  uint64_t active_binlog_pos_ = 4;
  std::unordered_map<uint64_t, BinlogTableMap> table_maps_;

  // Stats counters
  std::atomic<uint64_t> total_events_received_{0};
  std::atomic<uint64_t> total_events_applied_{0};
  std::atomic<uint64_t> total_events_skipped_{0};
  std::atomic<uint64_t> total_apply_failures_{0};   // changes the store refused (logged to stderr)
  bool in_dump_stream_ = false;   // read_packet strips the stream's status byte only after COM_BINLOG_DUMP
  // What the last dump-stream packet was, decided from the byte the server
  // sent first - BEFORE any stripping. After the status byte comes off, the
  // event's own first byte is its timestamp's low byte, which is 0xff one
  // second in 256: read as an ERR marker, that event failed forever.
  enum class PacketKind { Event, Err, Eof } packet_kind_ = PacketKind::Event;
  size_t checksum_len_ = 0;       // 4 once a FORMAT_DESCRIPTION announces CRC32 (trimmed off every event)
  bool applied_since_commit_ = false;   // a change reached the store since the last commit record
  std::unordered_set<std::string> missing_tables_logged_;   // "db.table" already reported as not held here
  bool in_source_schema(const std::string& schema) const;
  static bool parse_table_ddl(const std::string& q, std::string* verb, std::string* db, std::string* table);
  // CREATE [OR REPLACE] [ALGORITHM=..] [DEFINER=..] [SQL SECURITY ..] VIEW v ...,
  // ALTER [..] VIEW v ..., DROP VIEW [IF EXISTS] a[, b...]. `names` are the
  // (database, view) pairs, database empty when the statement does not say.
  static bool parse_view_ddl(const std::string& q, std::string* verb,
                             std::vector<std::pair<std::string, std::string>>* names);
  uint64_t last_view_sync_ms_ = 0;
  std::string committed_file_;          // just past the last transaction the stream finished
  uint64_t committed_pos_ = 0;
  void note_commit();
  // Replication packets carrying no event header at all (MariaDB heartbeat/OK
  // padding). Without this, a stall where every real event is discarded looks
  // identical to an idle source.
  std::atomic<uint64_t> total_keepalives_{0};
  std::atomic<uint64_t> total_inserts_{0};
  std::atomic<uint64_t> total_updates_{0};
  std::atomic<uint64_t> total_deletes_{0};
  std::atomic<uint64_t> total_ddl_{0};
  std::atomic<uint64_t> total_evictions_{0};
  std::atomic<uint64_t> last_event_ts_{0};
  std::atomic<uint64_t> seconds_behind_master_{0};
  std::string last_error_;

  // Internal connection & replication wire commands
  Result<void> connect_and_authenticate();
  Result<void> connect_and_dump();
  Result<void> send_register_slave();
  Result<void> send_checksum_config();
  Result<void> send_binlog_dump();
  Result<void> discover_master_pos(std::string* out_file = nullptr);
  Result<void> introspect_table_columns(BinlogTableMap& map);
  // Each mirrored table's column charsets, read from the master's
  // information_schema once per table ("db.table" -> latin1 by ordinal) and
  // dropped when DDL arrives. Only successful reads are kept.
  std::unordered_map<std::string, std::vector<bool>> latin1_columns_;
  bool tz_checked_ = false;   // the master's and this host's zones were compared
  bool master_tz_fixed_ = false;   // the master's @@time_zone is a fixed offset, not SYSTEM
  int master_tz_offset_ = 0;       // that offset, seconds east of UTC
  void check_master_time_zone();

  // Packet IO helpers
  Result<void> write_packet(const uint8_t* data, size_t n, uint8_t seq);
  Result<size_t> read_packet(std::vector<uint8_t>* out, uint8_t* seq = nullptr);

  // Event dispatchers
  Result<void> handle_event(const uint8_t* event_data, size_t event_len);
  Result<void> handle_table_map(const uint8_t* p, size_t len, uint32_t ts);
  Result<void> handle_rows_event(uint8_t type, const uint8_t* p, size_t len, uint32_t ts);
  Result<void> handle_query_event(const uint8_t* p, size_t len, uint32_t ts);
  Result<void> handle_rotate_event(const uint8_t* p, size_t len);

  // Row decoding
  Result<void> decode_rows(const BinlogTableMap& map, const uint8_t* p, size_t len,
                           uint8_t type, uint32_t ts);

  // Table filtering & RAM bounding
  bool is_whitelisted(const std::string& schema, const std::string& table) const;
  void enforce_table_limits(const std::string& table_name);

  void worker_loop();
};

}  // namespace aios::userland
