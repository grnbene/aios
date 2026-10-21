#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "aios/core/result.hpp"
#include "aios/userland/task_ledger.hpp"
#include "aios/media/audio.hpp"
#include "aios/media/stt.hpp"
#include "aios/media/tts.hpp"

namespace aios::userland {

enum class TaskPriority : uint8_t {
  Low = 0,
  Medium = 1,
  High = 2,
  Critical = 3
};

enum class TaskCategory : uint8_t {
  General = 0,
  Compute = 1,
  IO = 2,
  System = 3,
  User = 4
};

const char* task_priority_to_string(TaskPriority priority);
Result<TaskPriority> task_priority_from_string(const std::string& str);

const char* task_category_to_string(TaskCategory category);
Result<TaskCategory> task_category_from_string(const std::string& str);

struct TaskItem {
  std::string id;
  std::string title;
  std::string description;
  TaskPriority priority = TaskPriority::Medium;
  TaskStatus status = TaskStatus::Pending;
  TaskCategory category = TaskCategory::General;
  std::string assignee;
  std::vector<std::string> tags;
  std::vector<std::string> dependencies;
  uint64_t due_date_ns = 0;
  uint64_t created_at_ns = 0;
  uint64_t updated_at_ns = 0;
  uint64_t completed_at_ns = 0;
  int progress_percent = 0;
};

struct TaskListFilter {
  std::optional<TaskStatus> status;
  std::optional<TaskPriority> priority;
  std::optional<TaskCategory> category;
  std::string tag;
  std::string assignee;
  std::string query;
};

enum class TaskListSortField : uint8_t {
  CreatedAt = 0,
  Priority = 1,
  DueDate = 2,
  Title = 3,
  Status = 4
};

enum class TaskListSortOrder : uint8_t {
  Ascending = 0,
  Descending = 1
};

class TaskList {
 public:
  TaskList() = default;
  ~TaskList() = default;

  Result<std::string> add_task(const TaskItem& item);
  Result<void> update_task(const TaskItem& item);
  Result<void> remove_task(const std::string& task_id);

  Result<TaskItem> get_task(const std::string& task_id) const;
  bool has_task(const std::string& task_id) const;

  Result<void> set_status(const std::string& task_id, TaskStatus status);
  Result<void> set_progress(const std::string& task_id, int progress_percent);

  std::vector<TaskItem> filter(const TaskListFilter& opts = {}) const;
  std::vector<TaskItem> get_sorted(TaskListSortField field, TaskListSortOrder order = TaskListSortOrder::Ascending) const;
  std::vector<TaskItem> get_executable_tasks() const;
  bool can_execute(const std::string& task_id) const;

  // Speech-to-Text (STT) input processing
  Result<TaskItem> add_task_from_speech(media::STTEngine& stt, const media::AudioBuffer& audio, TaskPriority priority = TaskPriority::Medium, TaskCategory category = TaskCategory::General);
  Result<TaskItem> update_task_from_speech(media::STTEngine& stt, const std::string& task_id, const media::AudioBuffer& audio);
  Result<std::string> process_speech_command(media::STTEngine& stt, media::TTSEngine& tts, const media::AudioBuffer& audio);

  // Text-to-Speech (TTS) audio feedback
  Result<media::TTSResult> synthesize_task_audio_feedback(media::TTSEngine& tts, const std::string& task_id) const;
  Result<media::TTSResult> synthesize_task_list_audio_feedback(media::TTSEngine& tts, const TaskListFilter& filter_opts = {}) const;
  Result<media::TTSResult> synthesize_action_feedback(media::TTSEngine& tts, const std::string& action_message) const;

  size_t size() const { return tasks_.size(); }
  bool empty() const { return tasks_.empty(); }
  void clear() { tasks_.clear(); task_index_.clear(); }

 private:
  std::vector<TaskItem> tasks_;
  std::unordered_map<std::string, size_t> task_index_;

  std::string generate_id() const;
};

}  // namespace aios::userland
