#include "aios/userland/task_list.hpp"
#include <algorithm>
#include <chrono>
#include <sstream>

namespace aios::userland {

const char* task_priority_to_string(TaskPriority priority) {
  switch (priority) {
    case TaskPriority::Low:      return "Low";
    case TaskPriority::Medium:   return "Medium";
    case TaskPriority::High:     return "High";
    case TaskPriority::Critical: return "Critical";
  }
  return "Medium";
}

Result<TaskPriority> task_priority_from_string(const std::string& str) {
  if (str == "Low" || str == "low") return TaskPriority::Low;
  if (str == "Medium" || str == "medium") return TaskPriority::Medium;
  if (str == "High" || str == "high") return TaskPriority::High;
  if (str == "Critical" || str == "critical") return TaskPriority::Critical;
  return Result<TaskPriority>(Status::Invalid, "Unknown task priority: " + str);
}

const char* task_category_to_string(TaskCategory category) {
  switch (category) {
    case TaskCategory::General: return "General";
    case TaskCategory::Compute: return "Compute";
    case TaskCategory::IO:      return "IO";
    case TaskCategory::System:  return "System";
    case TaskCategory::User:    return "User";
  }
  return "General";
}

Result<TaskCategory> task_category_from_string(const std::string& str) {
  if (str == "General" || str == "general") return TaskCategory::General;
  if (str == "Compute" || str == "compute") return TaskCategory::Compute;
  if (str == "IO" || str == "io") return TaskCategory::IO;
  if (str == "System" || str == "system") return TaskCategory::System;
  if (str == "User" || str == "user") return TaskCategory::User;
  return Result<TaskCategory>(Status::Invalid, "Unknown task category: " + str);
}

std::string TaskList::generate_id() const {
  static uint64_t counter = 1000;
  auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  std::ostringstream ss;
  ss << "task-" << now << "-" << counter++;
  return ss.str();
}

Result<std::string> TaskList::add_task(const TaskItem& item) {
  TaskItem task = item;
  if (task.id.empty()) {
    task.id = generate_id();
  }

  if (task_index_.find(task.id) != task_index_.end()) {
    return Result<std::string>(Status::Conflict, "Task with ID already exists: " + task.id);
  }

  uint64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();

  if (task.created_at_ns == 0) task.created_at_ns = now_ns;
  task.updated_at_ns = now_ns;

  size_t idx = tasks_.size();
  tasks_.push_back(task);
  task_index_[task.id] = idx;

  return task.id;
}

Result<void> TaskList::update_task(const TaskItem& item) {
  auto it = task_index_.find(item.id);
  if (it == task_index_.end()) {
    return Result<void>(Status::NotFound, "Task not found: " + item.id);
  }

  uint64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();

  size_t idx = it->second;
  tasks_[idx] = item;
  tasks_[idx].updated_at_ns = now_ns;

  if (item.status == TaskStatus::Completed && tasks_[idx].completed_at_ns == 0) {
    tasks_[idx].completed_at_ns = now_ns;
    tasks_[idx].progress_percent = 100;
  }

  return Ok();
}

Result<void> TaskList::remove_task(const std::string& task_id) {
  auto it = task_index_.find(task_id);
  if (it == task_index_.end()) {
    return Result<void>(Status::NotFound, "Task not found: " + task_id);
  }

  size_t idx = it->second;
  tasks_.erase(tasks_.begin() + idx);

  task_index_.clear();
  for (size_t i = 0; i < tasks_.size(); ++i) {
    task_index_[tasks_[i].id] = i;
  }

  return Ok();
}

Result<TaskItem> TaskList::get_task(const std::string& task_id) const {
  auto it = task_index_.find(task_id);
  if (it == task_index_.end()) {
    return Result<TaskItem>(Status::NotFound, "Task not found: " + task_id);
  }
  return tasks_[it->second];
}

bool TaskList::has_task(const std::string& task_id) const {
  return task_index_.find(task_id) != task_index_.end();
}

Result<void> TaskList::set_status(const std::string& task_id, TaskStatus status) {
  auto it = task_index_.find(task_id);
  if (it == task_index_.end()) {
    return Result<void>(Status::NotFound, "Task not found: " + task_id);
  }

  uint64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();

  size_t idx = it->second;
  tasks_[idx].status = status;
  tasks_[idx].updated_at_ns = now_ns;

  if (status == TaskStatus::Completed) {
    tasks_[idx].completed_at_ns = now_ns;
    tasks_[idx].progress_percent = 100;
  }

  return Ok();
}

Result<void> TaskList::set_progress(const std::string& task_id, int progress_percent) {
  auto it = task_index_.find(task_id);
  if (it == task_index_.end()) {
    return Result<void>(Status::NotFound, "Task not found: " + task_id);
  }

  size_t idx = it->second;
  tasks_[idx].progress_percent = std::clamp(progress_percent, 0, 100);
  tasks_[idx].updated_at_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();

  if (tasks_[idx].progress_percent == 100) {
    tasks_[idx].status = TaskStatus::Completed;
    tasks_[idx].completed_at_ns = tasks_[idx].updated_at_ns;
  } else if (tasks_[idx].progress_percent > 0 && tasks_[idx].status == TaskStatus::Pending) {
    tasks_[idx].status = TaskStatus::InProgress;
  }

  return Ok();
}

std::vector<TaskItem> TaskList::filter(const TaskListFilter& opts) const {
  std::vector<TaskItem> result;

  for (const auto& task : tasks_) {
    if (opts.status.has_value() && task.status != opts.status.value()) {
      continue;
    }
    if (opts.priority.has_value() && task.priority != opts.priority.value()) {
      continue;
    }
    if (opts.category.has_value() && task.category != opts.category.value()) {
      continue;
    }
    if (!opts.assignee.empty() && task.assignee != opts.assignee) {
      continue;
    }
    if (!opts.tag.empty()) {
      auto tag_it = std::find(task.tags.begin(), task.tags.end(), opts.tag);
      if (tag_it == task.tags.end()) continue;
    }
    if (!opts.query.empty()) {
      bool title_match = task.title.find(opts.query) != std::string::npos;
      bool desc_match = task.description.find(opts.query) != std::string::npos;
      if (!title_match && !desc_match) continue;
    }
    result.push_back(task);
  }

  return result;
}

std::vector<TaskItem> TaskList::get_sorted(TaskListSortField field, TaskListSortOrder order) const {
  std::vector<TaskItem> sorted_tasks = tasks_;

  auto comparator = [field](const TaskItem& a, const TaskItem& b) -> bool {
    switch (field) {
      case TaskListSortField::CreatedAt:
        return a.created_at_ns < b.created_at_ns;
      case TaskListSortField::Priority:
        return static_cast<uint8_t>(a.priority) < static_cast<uint8_t>(b.priority);
      case TaskListSortField::DueDate:
        return a.due_date_ns < b.due_date_ns;
      case TaskListSortField::Title:
        return a.title < b.title;
      case TaskListSortField::Status:
        return static_cast<uint8_t>(a.status) < static_cast<uint8_t>(b.status);
    }
    return a.created_at_ns < b.created_at_ns;
  };

  if (order == TaskListSortOrder::Ascending) {
    std::sort(sorted_tasks.begin(), sorted_tasks.end(), comparator);
  } else {
    std::sort(sorted_tasks.begin(), sorted_tasks.end(), [comparator](const TaskItem& a, const TaskItem& b) {
      return comparator(b, a);
    });
  }

  return sorted_tasks;
}

bool TaskList::can_execute(const std::string& task_id) const {
  auto it = task_index_.find(task_id);
  if (it == task_index_.end()) return false;

  const auto& task = tasks_[it->second];
  if (task.status == TaskStatus::Completed || task.status == TaskStatus::Cancelled) {
    return false;
  }

  for (const auto& dep_id : task.dependencies) {
    auto dep_it = task_index_.find(dep_id);
    if (dep_it == task_index_.end()) {
      return false; // missing dependency
    }
    if (tasks_[dep_it->second].status != TaskStatus::Completed) {
      return false; // dependency not finished
    }
  }

  return true;
}

std::vector<TaskItem> TaskList::get_executable_tasks() const {
  std::vector<TaskItem> executable;
  for (const auto& task : tasks_) {
    if (can_execute(task.id)) {
      executable.push_back(task);
    }
  }
  return executable;
}

Result<TaskItem> TaskList::add_task_from_speech(media::STTEngine& stt, const media::AudioBuffer& audio, TaskPriority priority, TaskCategory category) {
  if (stt.state() == media::STTState::Uninitialized) {
    media::STTConfig stt_cfg;
    stt_cfg.sample_rate = audio.spec.sample_rate > 0 ? audio.spec.sample_rate : 16000;
    (void)stt.initialize(stt_cfg);
  }

  auto stt_res = stt.transcribe(audio);
  if (!stt_res.ok()) {
    return Result<TaskItem>(stt_res.error());
  }
  if (stt_res->transcript.empty()) {
    return Result<TaskItem>(Status::Invalid, "Speech transcription produced empty text");
  }

  TaskItem item;
  item.title = stt_res->transcript;
  item.description = "Created from speech input (confidence: " + std::to_string(stt_res->confidence) + ")";
  item.priority = priority;
  item.category = category;

  auto add_res = add_task(item);
  if (!add_res.ok()) {
    return Result<TaskItem>(add_res.error());
  }

  item.id = *add_res;
  return item;
}

Result<TaskItem> TaskList::update_task_from_speech(media::STTEngine& stt, const std::string& task_id, const media::AudioBuffer& audio) {
  auto task_res = get_task(task_id);
  if (!task_res.ok()) {
    return task_res;
  }

  if (stt.state() == media::STTState::Uninitialized) {
    media::STTConfig stt_cfg;
    stt_cfg.sample_rate = audio.spec.sample_rate > 0 ? audio.spec.sample_rate : 16000;
    (void)stt.initialize(stt_cfg);
  }

  auto stt_res = stt.transcribe(audio);
  if (!stt_res.ok()) {
    return Result<TaskItem>(stt_res.error());
  }
  if (stt_res->transcript.empty()) {
    return Result<TaskItem>(Status::Invalid, "Speech transcription produced empty text");
  }

  TaskItem updated = *task_res;
  std::string transcript = stt_res->transcript;

  if (transcript.find("complete") != std::string::npos || transcript.find("done") != std::string::npos || transcript.find("finish") != std::string::npos) {
    updated.status = TaskStatus::Completed;
    updated.progress_percent = 100;
  } else if (transcript.find("in progress") != std::string::npos || transcript.find("start") != std::string::npos) {
    updated.status = TaskStatus::InProgress;
  } else if (transcript.find("cancel") != std::string::npos) {
    updated.status = TaskStatus::Cancelled;
  } else {
    updated.description = transcript;
  }

  auto up_res = update_task(updated);
  if (!up_res.ok()) {
    return Result<TaskItem>(up_res.error());
  }

  return get_task(task_id);
}

Result<std::string> TaskList::process_speech_command(media::STTEngine& stt, media::TTSEngine& tts, const media::AudioBuffer& audio) {
  if (stt.state() == media::STTState::Uninitialized) {
    media::STTConfig stt_cfg;
    stt_cfg.sample_rate = audio.spec.sample_rate > 0 ? audio.spec.sample_rate : 16000;
    (void)stt.initialize(stt_cfg);
  }
  if (tts.state() == media::TTSState::Uninitialized) {
    (void)tts.initialize();
  }

  auto stt_res = stt.transcribe(audio);
  if (!stt_res.ok()) {
    return Result<std::string>(stt_res.error());
  }
  std::string text = stt_res->transcript;
  if (text.empty()) {
    return Result<std::string>(Status::Invalid, "Empty speech transcript");
  }

  std::string response;
  if (text.rfind("add ", 0) == 0 || text.rfind("create ", 0) == 0) {
    std::string task_title = text.substr(text.find(' ') + 1);
    TaskItem item;
    item.title = task_title;
    auto res = add_task(item);
    if (res.ok()) {
      response = "Created task: " + task_title + " with ID " + *res;
    } else {
      response = "Failed to create task: " + res.error().message;
    }
  } else if (text.rfind("complete ", 0) == 0) {
    std::string task_id = text.substr(9);
    auto res = set_status(task_id, TaskStatus::Completed);
    if (res.ok()) {
      response = "Marked task " + task_id + " as completed";
    } else {
      response = "Failed to complete task: " + res.error().message;
    }
  } else {
    TaskItem item;
    item.title = text;
    auto res = add_task(item);
    if (res.ok()) {
      response = "Added task from speech: " + text;
    } else {
      response = "Failed to process speech command: " + res.error().message;
    }
  }

  (void)tts.synthesize(response);

  return response;
}

Result<media::TTSResult> TaskList::synthesize_task_audio_feedback(media::TTSEngine& tts, const std::string& task_id) const {
  auto task_res = get_task(task_id);
  if (!task_res.ok()) {
    return Result<media::TTSResult>(task_res.error());
  }

  const auto& task = *task_res;
  std::string summary = "Task " + task.id + ": " + task.title + ". Status is " +
                        task_status_to_string(task.status) + ", priority " +
                        task_priority_to_string(task.priority) + ", progress " +
                        std::to_string(task.progress_percent) + " percent.";
  if (!task.description.empty()) {
    summary += " Description: " + task.description;
  }

  return tts.synthesize(summary);
}

Result<media::TTSResult> TaskList::synthesize_task_list_audio_feedback(media::TTSEngine& tts, const TaskListFilter& filter_opts) const {
  auto filtered_tasks = filter(filter_opts);
  std::string text_summary = "Task list contains " + std::to_string(filtered_tasks.size()) + " matching tasks. ";
  for (size_t i = 0; i < filtered_tasks.size() && i < 5; ++i) {
    const auto& t = filtered_tasks[i];
    text_summary += "Task " + std::to_string(i + 1) + ": " + t.title + ", status " + task_status_to_string(t.status) + ". ";
  }
  if (filtered_tasks.size() > 5) {
    text_summary += "And " + std::to_string(filtered_tasks.size() - 5) + " more tasks.";
  }

  return tts.synthesize(text_summary);
}

Result<media::TTSResult> TaskList::synthesize_action_feedback(media::TTSEngine& tts, const std::string& action_message) const {
  if (action_message.empty()) {
    return Result<media::TTSResult>(Status::Invalid, "Action message is empty");
  }
  return tts.synthesize("Task notification: " + action_message);
}

}  // namespace aios::userland
