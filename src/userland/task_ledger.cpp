#include "aios/userland/task_ledger.hpp"
#include <algorithm>
#include "aios/crypto/sha256.hpp"
#include "aios/hal/host.hpp"

namespace aios::userland {

std::string task_status_to_string(TaskStatus status) {
  switch (status) {
    case TaskStatus::Pending: return "PENDING";
    case TaskStatus::InProgress: return "IN_PROGRESS";
    case TaskStatus::Completed: return "COMPLETED";
    case TaskStatus::Failed: return "FAILED";
    case TaskStatus::Cancelled: return "CANCELLED";
  }
  return "UNKNOWN";
}

Result<TaskStatus> task_status_from_string(const std::string& str) {
  std::string s = str;
  for (auto& c : s) c = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
  if (s == "PENDING") return TaskStatus::Pending;
  if (s == "IN_PROGRESS" || s == "INPROGRESS" || s == "RUNNING") return TaskStatus::InProgress;
  if (s == "COMPLETED" || s == "DONE" || s == "SUCCESS") return TaskStatus::Completed;
  if (s == "FAILED" || s == "ERROR") return TaskStatus::Failed;
  if (s == "CANCELLED" || s == "CANCELED") return TaskStatus::Cancelled;
  return Result<TaskStatus>(Status::Invalid, "unknown task status: " + str);
}

TaskLedger::TaskLedger(Kernel& kernel)
    : kernel_(kernel), root_path_(kernel.config().root) {}

Result<void> TaskLedger::init(const std::string& ledger_dir) {
  root_path_ = kernel_.config().root + "/" + ledger_dir;
  kernel_.host().make_dir(root_path_);
  return load_state();
}

std::string TaskLedger::calculate_entry_hash(const LedgerEntry& entry) const {
  std::string data = std::to_string(entry.entry_index) + "|" +
                     entry.task_id + "|" +
                     entry.customer_id + "|" +
                     entry.action + "|" +
                     entry.prev_status + "|" +
                     entry.new_status + "|" +
                     std::to_string(entry.timestamp_ns) + "|" +
                     entry.details + "|" +
                     entry.prev_hash;
  auto hash_arr = crypto::sha256(ByteSpan((const uint8_t*)data.data(), data.size()));
  return crypto::bytes_to_hex(ByteSpan(hash_arr.data(), hash_arr.size()));
}

void TaskLedger::record_ledger_entry(const std::string& task_id,
                                     const std::string& customer_id,
                                     const std::string& action,
                                     const std::string& prev_status,
                                     const std::string& new_status,
                                     const std::string& details) {
  LedgerEntry entry;
  entry.entry_index = ledger_.size();
  entry.task_id = task_id;
  entry.customer_id = customer_id;
  entry.action = action;
  entry.prev_status = prev_status;
  entry.new_status = new_status;
  entry.timestamp_ns = kernel_.now();
  entry.details = details;
  entry.prev_hash = ledger_.empty() ? "0000000000000000000000000000000000000000000000000000000000000000" : ledger_.back().hash;
  entry.hash = calculate_entry_hash(entry);

  ledger_.push_back(entry);
}

Result<std::string> TaskLedger::create_task(const std::string& customer_id,
                                            const std::string& name,
                                            const std::string& payload,
                                            uint32_t priority) {
  std::string cid = customer_id.empty() ? "default" : customer_id;
  auto id128 = kernel_.ids().next();
  std::string task_id = "task_" + id128.short_str();

  TaskRecord task;
  task.id = task_id;
  task.customer_id = cid;
  task.name = name;
  task.payload = payload;
  task.status = TaskStatus::Pending;
  task.priority = priority;
  task.created_at_ns = kernel_.now();
  task.updated_at_ns = task.created_at_ns;

  tasks_.push_back(task);

  record_ledger_entry(task_id, cid, "CREATE", "", "PENDING", "Task created: " + name);
  save_state();

  return task_id;
}

Result<void> TaskLedger::update_status(const std::string& task_id,
                                       TaskStatus new_status,
                                       const std::string& details) {
  for (auto& task : tasks_) {
    if (task.id == task_id) {
      std::string old_status_str = task_status_to_string(task.status);
      std::string new_status_str = task_status_to_string(new_status);

      task.status = new_status;
      task.updated_at_ns = kernel_.now();

      record_ledger_entry(task_id, task.customer_id, "UPDATE_STATUS", old_status_str, new_status_str, details.empty() ? ("Status changed to " + new_status_str) : details);
      save_state();
      return Ok();
    }
  }
  return Result<void>(Status::NotFound, "task not found: " + task_id);
}

Result<TaskRecord> TaskLedger::get_task(const std::string& task_id) const {
  for (auto& task : tasks_) {
    if (task.id == task_id) {
      return task;
    }
  }
  return Result<TaskRecord>(Status::NotFound, "task not found: " + task_id);
}

std::vector<TaskRecord> TaskLedger::list_tasks(const std::string& customer_id,
                                                const std::string& status_filter) const {
  std::vector<TaskRecord> out;
  std::string target_status = status_filter;
  for (auto& c : target_status) c = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;

  for (auto& task : tasks_) {
    if (!customer_id.empty() && task.customer_id != customer_id) continue;
    if (!target_status.empty() && task_status_to_string(task.status) != target_status) continue;
    out.push_back(task);
  }
  return out;
}

std::vector<LedgerEntry> TaskLedger::get_ledger_history(const std::string& task_id,
                                                        const std::string& customer_id) const {
  std::vector<LedgerEntry> out;
  for (auto& entry : ledger_) {
    if (!task_id.empty() && entry.task_id != task_id) continue;
    if (!customer_id.empty() && entry.customer_id != customer_id) continue;
    out.push_back(entry);
  }
  return out;
}

bool TaskLedger::verify_ledger_integrity() const {
  std::string prev = "0000000000000000000000000000000000000000000000000000000000000000";
  for (size_t i = 0; i < ledger_.size(); ++i) {
    auto& entry = ledger_[i];
    if (entry.entry_index != i) return false;
    if (entry.prev_hash != prev) return false;
    if (calculate_entry_hash(entry) != entry.hash) return false;
    prev = entry.hash;
  }
  return true;
}

Result<void> TaskLedger::save_state() {
  if (root_path_.empty()) return Ok();
  kernel_.host().make_dir(root_path_);
  
  std::string tasks_file = root_path_ + "/tasks.db";
  Writer tw;
  tw.u32((uint32_t)tasks_.size());
  for (auto& t : tasks_) {
    tw.str(t.id);
    tw.str(t.customer_id);
    tw.str(t.name);
    tw.str(t.payload);
    tw.u32((uint32_t)t.status);
    tw.u32(t.priority);
    tw.u64(t.created_at_ns);
    tw.u64(t.updated_at_ns);
  }
  auto tr = kernel_.host().write_file(tasks_file, ByteSpan(tw.buf().data(), tw.buf().size()));
  if (!tr.ok()) return tr;

  std::string ledger_file = root_path_ + "/ledger.log";
  Writer lw;
  lw.u32((uint32_t)ledger_.size());
  for (auto& e : ledger_) {
    lw.u64(e.entry_index);
    lw.str(e.task_id);
    lw.str(e.customer_id);
    lw.str(e.action);
    lw.str(e.prev_status);
    lw.str(e.new_status);
    lw.str(e.details);
    lw.u64(e.timestamp_ns);
    lw.str(e.prev_hash);
    lw.str(e.hash);
  }
  return kernel_.host().write_file(ledger_file, ByteSpan(lw.buf().data(), lw.buf().size()));
}

Result<void> TaskLedger::load_state() {
  if (root_path_.empty()) return Ok();

  std::string tasks_file = root_path_ + "/tasks.db";
  auto tr = kernel_.host().read_file(tasks_file);
  if (tr.ok() && !tr->empty()) {
    Reader r(ByteSpan(tr->data(), tr->size()));
    uint32_t count = r.u32();
    tasks_.clear();
    for (uint32_t i = 0; i < count; ++i) {
      TaskRecord t;
      t.id = r.str();
      t.customer_id = r.str();
      t.name = r.str();
      t.payload = r.str();
      t.status = (TaskStatus)r.u32();
      t.priority = r.u32();
      t.created_at_ns = r.u64();
      t.updated_at_ns = r.u64();
      tasks_.push_back(t);
    }
  }

  std::string ledger_file = root_path_ + "/ledger.log";
  auto lr = kernel_.host().read_file(ledger_file);
  if (lr.ok() && !lr->empty()) {
    Reader r(ByteSpan(lr->data(), lr->size()));
    uint32_t count = r.u32();
    ledger_.clear();
    for (uint32_t i = 0; i < count; ++i) {
      LedgerEntry e;
      e.entry_index = r.u64();
      e.task_id = r.str();
      e.customer_id = r.str();
      e.action = r.str();
      e.prev_status = r.str();
      e.new_status = r.str();
      e.details = r.str();
      e.timestamp_ns = r.u64();
      e.prev_hash = r.str();
      e.hash = r.str();
      ledger_.push_back(e);
    }
  }

  return Ok();
}

}  // namespace aios::userland
