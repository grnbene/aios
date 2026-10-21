#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "aios/core/result.hpp"
#include "aios/kernel/kernel.hpp"

namespace aios::userland {

enum class TaskStatus {
  Pending,
  InProgress,
  Completed,
  Failed,
  Cancelled
};

std::string task_status_to_string(TaskStatus status);
Result<TaskStatus> task_status_from_string(const std::string& str);

struct TaskRecord {
  std::string id;
  std::string customer_id;
  std::string name;
  std::string payload;
  TaskStatus status = TaskStatus::Pending;
  uint32_t priority = 1;
  uint64_t created_at_ns = 0;
  uint64_t updated_at_ns = 0;
};

struct LedgerEntry {
  uint64_t entry_index = 0;
  std::string task_id;
  std::string customer_id;
  std::string action;       // CREATE, UPDATE_STATUS, PULL_SYNC, PUSH_SYNC, DELETE
  std::string prev_status;
  std::string new_status;
  uint64_t timestamp_ns = 0;
  std::string details;
  std::string prev_hash;
  std::string hash;         // SHA256 integrity digest of ledger entry + link to prev_hash
};

class TaskLedger {
 public:
  explicit TaskLedger(Kernel& kernel);

  Result<void> init(const std::string& ledger_dir = "tasks");
  Result<std::string> create_task(const std::string& customer_id, const std::string& name, const std::string& payload, uint32_t priority = 1);
  Result<void> update_status(const std::string& task_id, TaskStatus new_status, const std::string& details = "");
  Result<TaskRecord> get_task(const std::string& task_id) const;
  std::vector<TaskRecord> list_tasks(const std::string& customer_id = "", const std::string& status_filter = "") const;
  std::vector<LedgerEntry> get_ledger_history(const std::string& task_id = "", const std::string& customer_id = "") const;

  Result<void> save_state();
  Result<void> load_state();
  bool verify_ledger_integrity() const;
  size_t task_count() const { return tasks_.size(); }
  size_t ledger_entry_count() const { return ledger_.size(); }

 private:
  Kernel& kernel_;
  std::string root_path_;
  std::vector<TaskRecord> tasks_;
  std::vector<LedgerEntry> ledger_;

  std::string calculate_entry_hash(const LedgerEntry& entry) const;
  void record_ledger_entry(const std::string& task_id, const std::string& customer_id, const std::string& action, const std::string& prev_status, const std::string& new_status, const std::string& details);
};

}  // namespace aios::userland
