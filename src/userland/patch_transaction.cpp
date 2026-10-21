#include "aios/userland/patch_transaction.hpp"
#include "aios/hal/host.hpp"
#include <algorithm>
#include <sstream>
#include <fstream>
#include <iostream>
#include <unordered_map>
#include <chrono>

namespace aios::userland {

namespace {
static TransactionManifest g_active_tx;
static bool g_has_active_tx = false;

std::vector<std::string> split_lines(const std::string& str) {
  std::vector<std::string> lines;
  std::string cur;
  for (size_t i = 0; i < str.size(); ++i) {
    char c = str[i];
    if (c == '\r') {
      if (i + 1 < str.size() && str[i + 1] == '\n') {
        i++;
      }
      lines.push_back(cur);
      cur.clear();
    } else if (c == '\n') {
      lines.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty() || (!str.empty() && (str.back() == '\n' || str.back() == '\r'))) {
    lines.push_back(cur);
  }
  return lines;
}

std::string trim(const std::string& s) {
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

std::string clean_diff_path(const std::string& path) {
  std::string p = trim(path);
  if (p.rfind("a/", 0) == 0 || p.rfind("b/", 0) == 0) {
    p = p.substr(2);
  }
  return p;
}

bool read_file_lines(Kernel& k, const std::string& full_path, std::vector<std::string>& out_lines) {
  auto res = k.host().read_file(full_path);
  if (!res.ok()) {
    return false;
  }
  std::string content(reinterpret_cast<const char*>(res->data()), res->size());
  out_lines = split_lines(content);
  return true;
}

bool write_file_lines(Kernel& k, const std::string& full_path, const std::vector<std::string>& lines) {
  std::string content;
  for (size_t i = 0; i < lines.size(); ++i) {
    content += lines[i];
    if (i + 1 < lines.size() || !lines[i].empty()) {
      content += "\n";
    }
  }
  std::vector<uint8_t> bytes(content.begin(), content.end());
  auto res = k.host().write_file(full_path, bytes);
  return res.ok();
}

std::string resolve_path(const std::string& root_dir, const std::string& rel_path) {
  if (rel_path.empty()) return root_dir;
  if (rel_path[0] == '/') return rel_path;
  if (root_dir == "." || root_dir.empty()) return rel_path;
  if (root_dir.back() == '/') return root_dir + rel_path;
  return root_dir + "/" + rel_path;
}

} // namespace

const char* file_op_symbol(FileOpType op) {
  switch (op) {
    case FileOpType::Modify: return "[M]";
    case FileOpType::Add:    return "[+]";
    case FileOpType::Delete: return "[-]";
    case FileOpType::Rename: return "[R]";
  }
  return "[?]";
}

const char* file_op_name(FileOpType op) {
  switch (op) {
    case FileOpType::Modify: return "Modified";
    case FileOpType::Add:    return "Added";
    case FileOpType::Delete: return "Deleted";
    case FileOpType::Rename: return "Renamed";
  }
  return "Unknown";
}

const char* transaction_status_name(TransactionStatus status) {
  switch (status) {
    case TransactionStatus::Draft:     return "DRAFT";
    case TransactionStatus::Staged:    return "STAGED";
    case TransactionStatus::Committed: return "COMMITTED";
    case TransactionStatus::Aborted:   return "ABORTED";
    case TransactionStatus::Failed:    return "FAILED";
  }
  return "UNKNOWN";
}

int DiffFileEntry::active_additions() const {
  int count = 0;
  for (const auto& h : hunks) {
    if (h.accepted) {
      for (const auto& l : h.lines) {
        if (l.type == DiffLineType::Addition) count++;
      }
    }
  }
  return count;
}

int DiffFileEntry::active_deletions() const {
  int count = 0;
  for (const auto& h : hunks) {
    if (h.accepted) {
      for (const auto& l : h.lines) {
        if (l.type == DiffLineType::Deletion) count++;
      }
    }
  }
  return count;
}

int TransactionManifest::total_hunks() const {
  int count = 0;
  for (const auto& f : files) {
    count += static_cast<int>(f.hunks.size());
  }
  return count;
}

int TransactionManifest::total_additions() const {
  int count = 0;
  for (const auto& f : files) {
    count += f.additions_count;
  }
  return count;
}

int TransactionManifest::total_deletions() const {
  int count = 0;
  for (const auto& f : files) {
    count += f.deletions_count;
  }
  return count;
}

int TransactionManifest::active_hunks_count() const {
  int count = 0;
  for (const auto& f : files) {
    for (const auto& h : f.hunks) {
      if (h.accepted) count++;
    }
  }
  return count;
}

std::pair<int, int> TransactionManifest::find_hunk_location(int global_idx) const {
  if (global_idx < 0) return {-1, -1};
  int cur = 0;
  for (size_t f_idx = 0; f_idx < files.size(); ++f_idx) {
    for (size_t h_idx = 0; h_idx < files[f_idx].hunks.size(); ++h_idx) {
      if (cur == global_idx) {
        return {static_cast<int>(f_idx), static_cast<int>(h_idx)};
      }
      cur++;
    }
  }
  return {-1, -1};
}

int TransactionManifest::global_hunk_index(int file_idx, int hunk_idx) const {
  if (file_idx < 0 || file_idx >= static_cast<int>(files.size())) return -1;
  int cur = 0;
  for (int f = 0; f < file_idx; ++f) {
    cur += static_cast<int>(files[f].hunks.size());
  }
  if (hunk_idx < 0 || hunk_idx >= static_cast<int>(files[file_idx].hunks.size())) return -1;
  return cur + hunk_idx;
}

Result<TransactionManifest> PatchEngine::parse_unified_diff(
    const std::string& diff_text,
    const std::string& default_title,
    uint64_t tx_id) {
  TransactionManifest manifest;
  manifest.tx_id = tx_id;
  manifest.title = default_title;
  manifest.timestamp = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  manifest.status = TransactionStatus::Draft;

  auto lines = split_lines(diff_text);
  if (lines.empty()) {
    return Result<TransactionManifest>(Status::Invalid, "Empty patch diff content");
  }

  // Pre-parse intent header if present
  // Format:
  // Transaction #402: Refactor Capability Attenuation
  //   [M] kernel/capability.hpp  (Add TTL field to Cap struct)
  //   ...
  std::unordered_map<std::string, std::string> declared_reasons;
  std::unordered_map<std::string, FileOpType> declared_ops;

  size_t idx = 0;
  while (idx < lines.size()) {
    const std::string& l = lines[idx];
    if (l.rfind("Transaction #", 0) == 0) {
      size_t colon = l.find(':');
      if (colon != std::string::npos) {
        std::string num_str = l.substr(13, colon - 13);
        char* end_ptr = nullptr;
        unsigned long long v = std::strtoull(num_str.c_str(), &end_ptr, 10);
        if (end_ptr != nullptr && end_ptr != num_str.c_str()) manifest.tx_id = v;
        manifest.title = trim(l.substr(colon + 1));
      }
      idx++;
      continue;
    }

    std::string trimmed = trim(l);
    if (trimmed.rfind("[M]", 0) == 0 || trimmed.rfind("[+]", 0) == 0 ||
        trimmed.rfind("[-]", 0) == 0 || trimmed.rfind("[R]", 0) == 0) {
      FileOpType op = FileOpType::Modify;
      if (trimmed.rfind("[+]", 0) == 0) op = FileOpType::Add;
      else if (trimmed.rfind("[-]", 0) == 0) op = FileOpType::Delete;
      else if (trimmed.rfind("[R]", 0) == 0) op = FileOpType::Rename;

      std::string rest = trim(trimmed.substr(3));
      std::string path, reason;
      size_t paren_open = rest.find('(');
      if (paren_open != std::string::npos) {
        path = trim(rest.substr(0, paren_open));
        size_t paren_close = rest.find(')', paren_open);
        if (paren_close != std::string::npos) {
          reason = rest.substr(paren_open + 1, paren_close - paren_open - 1);
        } else {
          reason = rest.substr(paren_open + 1);
        }
      } else {
        path = rest;
      }
      if (!path.empty()) {
        declared_reasons[path] = reason;
        declared_ops[path] = op;
      }
      idx++;
      continue;
    }

    if (l.rfind("--- ", 0) == 0 || l.rfind("diff --git ", 0) == 0) {
      break; // Unified diff body starts
    }
    idx++;
  }

  // Parse unified diff hunks
  DiffFileEntry cur_file;
  bool in_file = false;
  int global_hunk_counter = 0;

  auto flush_file = [&]() {
    if (in_file) {
      if (!cur_file.hunks.empty() || cur_file.op_type == FileOpType::Delete || cur_file.op_type == FileOpType::Add) {
        // Compute additions / deletions counts
        cur_file.additions_count = 0;
        cur_file.deletions_count = 0;
        for (const auto& h : cur_file.hunks) {
          for (const auto& dl : h.lines) {
            if (dl.type == DiffLineType::Addition) cur_file.additions_count++;
            else if (dl.type == DiffLineType::Deletion) cur_file.deletions_count++;
          }
        }
        std::string lookup_p = cur_file.new_path.empty() ? cur_file.old_path : cur_file.new_path;
        if (cur_file.reason.empty() && declared_reasons.count(lookup_p)) {
          cur_file.reason = declared_reasons[lookup_p];
        }
        if (declared_ops.count(lookup_p)) {
          cur_file.op_type = declared_ops[lookup_p];
        }
        cur_file.lang = lang_for_path(lookup_p);
        manifest.files.push_back(cur_file);
      }
      cur_file = DiffFileEntry();
      in_file = false;
    }
  };

  while (idx < lines.size()) {
    const std::string& line = lines[idx];

    if (line.rfind("diff --git ", 0) == 0) {
      flush_file();
      in_file = true;
      std::string rest = line.substr(11);
      size_t sp = rest.find(' ');
      if (sp != std::string::npos) {
        cur_file.old_path = clean_diff_path(rest.substr(0, sp));
        cur_file.new_path = clean_diff_path(rest.substr(sp + 1));
      }
      idx++;
      continue;
    }

    if (line.rfind("--- ", 0) == 0) {
      if (in_file && (!cur_file.hunks.empty() || !cur_file.new_path.empty() || cur_file.op_type == FileOpType::Add || cur_file.op_type == FileOpType::Delete)) {
        flush_file();
      }
      in_file = true;
      std::string path = line.substr(4);
      if (path == "/dev/null") {
        cur_file.old_path = "";
        cur_file.op_type = FileOpType::Add;
      } else {
        cur_file.old_path = clean_diff_path(path);
      }
      idx++;
      continue;
    }

    if (line.rfind("+++ ", 0) == 0) {
      if (!in_file) {
        in_file = true;
      }
      std::string path = line.substr(4);
      if (path == "/dev/null") {
        cur_file.new_path = "";
        cur_file.op_type = FileOpType::Delete;
      } else {
        cur_file.new_path = clean_diff_path(path);
      }
      idx++;
      continue;
    }

    if (line.rfind("@@ ", 0) == 0) {
      if (!in_file) {
        in_file = true;
      }
      // Parse hunk header: @@ -old_start,old_count +new_start,new_count @@ [section]
      DiffHunk hunk;
      hunk.header = line;
      hunk.global_hunk_id = global_hunk_counter++;

      size_t at_close = line.find("@@", 2);
      if (at_close != std::string::npos) {
        std::string range_part = line.substr(3, at_close - 3);
        if (at_close + 2 < line.size()) {
          hunk.section_heading = trim(line.substr(at_close + 2));
        }

        std::istringstream iss(range_part);
        std::string old_range, new_range;
        if (iss >> old_range >> new_range) {
          if (old_range.rfind("-", 0) == 0) old_range = old_range.substr(1);
          if (new_range.rfind("+", 0) == 0) new_range = new_range.substr(1);

          size_t c1 = old_range.find(',');
          if (c1 != std::string::npos) {
            hunk.old_start = std::atoi(old_range.substr(0, c1).c_str());
            hunk.old_count = std::atoi(old_range.substr(c1 + 1).c_str());
          } else {
            hunk.old_start = std::atoi(old_range.c_str());
            hunk.old_count = 1;
          }

          size_t c2 = new_range.find(',');
          if (c2 != std::string::npos) {
            hunk.new_start = std::atoi(new_range.substr(0, c2).c_str());
            hunk.new_count = std::atoi(new_range.substr(c2 + 1).c_str());
          } else {
            hunk.new_start = std::atoi(new_range.c_str());
            hunk.new_count = 1;
          }
        }
      }

      int cur_old_lineno = hunk.old_start;
      int cur_new_lineno = hunk.new_start;

      idx++;
      while (idx < lines.size()) {
        const std::string& hline = lines[idx];
        if (hline.rfind("@@ ", 0) == 0 || hline.rfind("diff --git ", 0) == 0 ||
            hline.rfind("--- ", 0) == 0) {
          break; // Next hunk or file
        }

        DiffLine dl;
        dl.raw_line = hline;
        if (!hline.empty()) {
          char prefix = hline[0];
          dl.content = hline.substr(1);
          if (prefix == '+') {
            dl.type = DiffLineType::Addition;
            dl.old_lineno = -1;
            dl.new_lineno = cur_new_lineno++;
          } else if (prefix == '-') {
            dl.type = DiffLineType::Deletion;
            dl.old_lineno = cur_old_lineno++;
            dl.new_lineno = -1;
          } else if (prefix == ' ') {
            dl.type = DiffLineType::Context;
            dl.old_lineno = cur_old_lineno++;
            dl.new_lineno = cur_new_lineno++;
          } else if (prefix == '\\') {
            // "\ No newline at end of file"
            idx++;
            continue;
          } else {
            // Unprefixed context line fallback
            dl.type = DiffLineType::Context;
            dl.content = hline;
            dl.old_lineno = cur_old_lineno++;
            dl.new_lineno = cur_new_lineno++;
          }
        } else {
          // Empty line treated as context
          dl.type = DiffLineType::Context;
          dl.content = "";
          dl.old_lineno = cur_old_lineno++;
          dl.new_lineno = cur_new_lineno++;
        }

        hunk.lines.push_back(dl);
        idx++;
      }

      cur_file.hunks.push_back(hunk);
      continue;
    }

    idx++;
  }

  flush_file();

  if (manifest.files.empty()) {
    return Result<TransactionManifest>(Status::Invalid, "No file diffs parsed from input patch");
  }

  return manifest;
}

std::string PatchEngine::format_plan_manifest(const TransactionManifest& manifest) {
  std::ostringstream oss;
  oss << "Transaction #" << manifest.tx_id << ": " << manifest.title << "\n";
  for (const auto& f : manifest.files) {
    oss << "  " << file_op_symbol(f.op_type) << " " << (f.new_path.empty() ? f.old_path : f.new_path);
    if (!f.reason.empty()) {
      oss << "  (" << f.reason << ")";
    }
    oss << "\n";
  }
  return oss.str();
}

std::string PatchEngine::generate_unified_diff(const TransactionManifest& manifest, bool accepted_only) {
  std::ostringstream oss;
  for (const auto& f : manifest.files) {
    if (accepted_only && !f.accepted) continue;
    std::string op_p = f.new_path.empty() ? f.old_path : f.new_path;
    std::string old_p = f.old_path.empty() ? "/dev/null" : "a/" + f.old_path;
    std::string new_p = f.new_path.empty() ? "/dev/null" : "b/" + f.new_path;

    oss << "--- " << old_p << "\n";
    oss << "+++ " << new_p << "\n";

    for (const auto& h : f.hunks) {
      if (accepted_only && !h.accepted) continue;
      oss << h.header << "\n";
      for (const auto& l : h.lines) {
        if (l.type == DiffLineType::Addition) oss << "+" << l.content << "\n";
        else if (l.type == DiffLineType::Deletion) oss << "-" << l.content << "\n";
        else oss << " " << l.content << "\n";
      }
    }
  }
  return oss.str();
}

Result<void> PatchEngine::dry_run(
    Kernel& k,
    TransactionManifest& manifest,
    const std::string& root_dir) {
  return dry_run_with([&k](const std::string& path, std::string& out) {
    auto res = k.host().read_file(path);
    if (!res.ok()) return false;
    out.assign(reinterpret_cast<const char*>(res->data()), res->size());
    return true;
  }, manifest, root_dir);
}

Result<void> PatchEngine::dry_run_with(
    const std::function<bool(const std::string&, std::string&)>& read_file,
    TransactionManifest& manifest,
    const std::string& root_dir) {
  for (auto& file_entry : manifest.files) {
    std::string target_path = file_entry.new_path.empty() ? file_entry.old_path : file_entry.new_path;
    std::string full_path = resolve_path(root_dir, target_path);

    std::vector<std::string> orig_lines;
    if (file_entry.op_type == FileOpType::Add) {
      orig_lines.clear();
      file_entry.original_lines = orig_lines;
      continue;
    } else {
      std::string content;
      if (!read_file(full_path, content)) {
        manifest.status = TransactionStatus::Failed;
        manifest.error_message = "Dry-run failed: Target file not found on substrate: " + target_path;
        return Result<void>(Status::NotFound, manifest.error_message);
      }
      orig_lines = split_lines(content);
    }

    file_entry.original_lines = orig_lines;

    // Strict deterministic hunk matching
    int line_offset = 0;
    for (size_t h_idx = 0; h_idx < file_entry.hunks.size(); ++h_idx) {
      const auto& hunk = file_entry.hunks[h_idx];
      int target_start_idx = hunk.old_start - 1 + line_offset;

      // Extract expected old lines from hunk
      std::vector<std::string> expected_old;
      for (const auto& dl : hunk.lines) {
        if (dl.type == DiffLineType::Context || dl.type == DiffLineType::Deletion) {
          expected_old.push_back(dl.content);
        }
      }

      // Check boundary
      if (target_start_idx < 0 || target_start_idx + static_cast<int>(expected_old.size()) > static_cast<int>(orig_lines.size())) {
        manifest.status = TransactionStatus::Failed;
        manifest.error_message = "Dry-run failed: Hunk #" + std::to_string(h_idx + 1) +
                                 " out of bounds in " + target_path +
                                 " (expected line " + std::to_string(hunk.old_start) + ")";
        return Result<void>(Status::Invalid, manifest.error_message);
      }

      // Verify exact line matching
      for (size_t i = 0; i < expected_old.size(); ++i) {
        const std::string& actual = orig_lines[target_start_idx + i];
        const std::string& expected = expected_old[i];
        if (actual != expected) {
          manifest.status = TransactionStatus::Failed;
          manifest.error_message = "Dry-run mismatch in " + target_path + " at line " +
                                   std::to_string(target_start_idx + i + 1) + ":\n" +
                                   "  Expected: \"" + expected + "\"\n" +
                                   "  Actual:   \"" + actual + "\"";
          return Result<void>(Status::Invalid, manifest.error_message);
        }
      }

      // Update offset for subsequent hunks in original file space
      int hunk_old_count = static_cast<int>(expected_old.size());
      int hunk_new_count = 0;
      for (const auto& dl : hunk.lines) {
        if (dl.type == DiffLineType::Context || dl.type == DiffLineType::Addition) {
          hunk_new_count++;
        }
      }
      line_offset += (hunk_new_count - hunk_old_count);
    }
  }

  return Ok();
}

Result<void> PatchEngine::restage_with_hunk_selection(TransactionManifest& manifest) {
  for (auto& file_entry : manifest.files) {
    if (!file_entry.accepted) {
      // Revert completely to original lines
      file_entry.staged_lines = file_entry.original_lines;
      continue;
    }

    if (file_entry.op_type == FileOpType::Delete) {
      file_entry.staged_lines.clear();
      continue;
    }

    if (file_entry.op_type == FileOpType::Add) {
      std::vector<std::string> staged;
      for (const auto& hunk : file_entry.hunks) {
        if (!hunk.accepted) continue;
        for (const auto& dl : hunk.lines) {
          if (dl.type == DiffLineType::Addition || dl.type == DiffLineType::Context) {
            staged.push_back(dl.content);
          }
        }
      }
      file_entry.staged_lines = staged;
      continue;
    }

    std::vector<std::string> staged = file_entry.original_lines;
    int line_offset = 0;

    for (const auto& hunk : file_entry.hunks) {
      if (!hunk.accepted) {
        // Skip rejected hunk: keeps original content in staged buffer
        continue;
      }

      int target_start_idx = hunk.old_start - 1 + line_offset;
      if (target_start_idx < 0) target_start_idx = 0;

      int old_consumed = 0;
      std::vector<std::string> replacement_lines;

      for (const auto& dl : hunk.lines) {
        if (dl.type == DiffLineType::Context) {
          replacement_lines.push_back(dl.content);
          old_consumed++;
        } else if (dl.type == DiffLineType::Addition) {
          replacement_lines.push_back(dl.content);
        } else if (dl.type == DiffLineType::Deletion) {
          old_consumed++;
        }
      }

      if (target_start_idx <= static_cast<int>(staged.size())) {
        int erase_end = target_start_idx + old_consumed;
        if (erase_end > static_cast<int>(staged.size())) {
          erase_end = static_cast<int>(staged.size());
        }

        staged.erase(staged.begin() + target_start_idx, staged.begin() + erase_end);
        staged.insert(staged.begin() + target_start_idx, replacement_lines.begin(), replacement_lines.end());

        line_offset += static_cast<int>(replacement_lines.size()) - old_consumed;
      }
    }

    file_entry.staged_lines = staged;
  }
  return Ok();
}

Result<void> PatchEngine::stage(
    Kernel& k,
    TransactionManifest& manifest,
    const std::string& root_dir) {
  // 1. Dry run pre-flight check
  auto check = dry_run(k, manifest, root_dir);
  if (!check.ok()) {
    return check.error();
  }

  // 2. Synthesize in-memory staged buffers
  auto restage_res = restage_with_hunk_selection(manifest);
  if (!restage_res.ok()) {
    return restage_res.error();
  }

  manifest.status = TransactionStatus::Staged;
  set_active_transaction(manifest);
  return Ok();
}

Result<void> PatchEngine::commit(
    Kernel& k,
    TransactionManifest& manifest,
    const std::string& root_dir) {
  if (manifest.status != TransactionStatus::Staged) {
    auto st_res = stage(k, manifest, root_dir);
    if (!st_res.ok()) return st_res.error();
  }

  // Atomic write pass: keep rollback backup map of original file contents
  std::unordered_map<std::string, std::vector<std::string>> backups;
  std::vector<std::string> written_paths;

  for (const auto& file_entry : manifest.files) {
    if (!file_entry.accepted) continue;

    std::string target_path = file_entry.new_path.empty() ? file_entry.old_path : file_entry.new_path;
    std::string full_path = resolve_path(root_dir, target_path);

    // Save backup if file existed
    if (file_entry.op_type != FileOpType::Add) {
      std::vector<std::string> prev_lines;
      if (read_file_lines(k, full_path, prev_lines)) {
        backups[full_path] = prev_lines;
      }
    }

    if (file_entry.op_type == FileOpType::Delete) {
      // In AIOS substrate, deleting a file can be empty write or remove
      std::vector<uint8_t> empty;
      auto res = k.host().write_file(full_path, empty);
      if (!res.ok()) {
        // Rollback all written files
        for (const auto& wp : written_paths) {
          if (backups.count(wp)) {
            write_file_lines(k, wp, backups[wp]);
          }
        }
        manifest.status = TransactionStatus::Failed;
        manifest.error_message = "Atomic commit failed deleting: " + full_path;
        return Result<void>(Status::IoError, manifest.error_message);
      }
      written_paths.push_back(full_path);
    } else {
      bool ok = write_file_lines(k, full_path, file_entry.staged_lines);
      if (!ok) {
        // Rollback all written files immediately!
        for (const auto& wp : written_paths) {
          if (backups.count(wp)) {
            write_file_lines(k, wp, backups[wp]);
          }
        }
        manifest.status = TransactionStatus::Failed;
        manifest.error_message = "Atomic commit failed writing: " + full_path;
        return Result<void>(Status::IoError, manifest.error_message);
      }
      written_paths.push_back(full_path);
    }
  }

  manifest.status = TransactionStatus::Committed;
  set_active_transaction(manifest);
  return Ok();
}

void PatchEngine::abort(TransactionManifest& manifest) {
  manifest.status = TransactionStatus::Aborted;
  for (auto& file_entry : manifest.files) {
    file_entry.staged_lines = file_entry.original_lines;
  }
  clear_active_transaction();
}

Result<void> PatchEngine::apply_patch_atomic(
    Kernel& k,
    const std::string& patch_text,
    const std::string& title,
    const std::string& root_dir) {
  auto parsed = parse_unified_diff(patch_text, title);
  if (!parsed.ok()) return parsed.error();

  TransactionManifest manifest = *parsed;
  return commit(k, manifest, root_dir);
}

TransactionManifest& PatchEngine::active_transaction() {
  return g_active_tx;
}

bool PatchEngine::has_active_transaction() {
  return g_has_active_tx;
}

void PatchEngine::set_active_transaction(const TransactionManifest& tx) {
  g_active_tx = tx;
  g_has_active_tx = true;
}

void PatchEngine::clear_active_transaction() {
  g_has_active_tx = false;
  g_active_tx = TransactionManifest();
}

} // namespace aios::userland
