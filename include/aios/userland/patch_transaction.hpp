#pragma once
#include <functional>
// Multi-File Edit Transactions & Deterministic Atomic Patch Engine.
//
// Replaces single-file shotgun writes with staged, cryptographic, all-or-nothing
// git-style transactions.
//
// 1. Intent Declaration: Manifest of target files, op types ([M], [+], [-], [R]),
//    and per-file rationale before any disk or buffer is touched.
// 2. Deterministic Patch Engine: Strict pre-flight dry-run. If a patch fails to apply
//    cleanly to any file in the manifest, the entire batch fails before touching disk.
// 3. Unified Atomic Review Buffer: Viewport renders all modified files in a single,
//    scrollable buffer with syntax highlighting, inline diffs, and hunk-level toggles.
// 4. Keyboard-Driven Verdict:
//    - Tab / Shift+Tab: Jump between hunks across all modified files.
//    - Space: Toggle accept/reject on individual hunks or entire files.
//    - Ctrl+Enter: Commit transaction atomically in one tick.
//    - Esc: Abort transaction; staging vanishes and tree remains pristine.

#include "aios/core/result.hpp"
#include "aios/kernel/kernel.hpp"
#include "aios/userland/source_lang.hpp"
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <utility>

namespace aios::userland {

enum class FileOpType : uint8_t {
  Modify,   // [M]
  Add,      // [+]
  Delete,   // [-]
  Rename    // [R]
};

const char* file_op_symbol(FileOpType op);
const char* file_op_name(FileOpType op);

enum class DiffLineType : uint8_t {
  Context,      // ' '
  Addition,     // '+'
  Deletion,     // '-'
  HunkHeader,   // '@@ ... @@'
  FileHeader    // '---', '+++', 'diff --git'
};

struct DiffLine {
  DiffLineType type = DiffLineType::Context;
  std::string content;      // text without the leading prefix (+/-/ )
  std::string raw_line;     // original raw line including prefix
  int old_lineno = -1;      // 1-based line number in original file (-1 for additions)
  int new_lineno = -1;      // 1-based line number in new file (-1 for deletions)
};

struct DiffHunk {
  int old_start = 0;
  int old_count = 0;
  int new_start = 0;
  int new_count = 0;
  std::string header;               // e.g. "@@ -15,7 +15,9 @@ void foo()"
  std::string section_heading;      // optional trailing heading from hunk header
  std::vector<DiffLine> lines;
  bool accepted = true;             // Space toggles accept/reject for this hunk
  int global_hunk_id = 0;           // global 0-based hunk index across entire transaction
};

struct DiffFileEntry {
  std::string old_path;
  std::string new_path;
  FileOpType op_type = FileOpType::Modify;
  std::string reason;               // rationale declared in plan manifest
  std::vector<DiffHunk> hunks;
  bool accepted = true;             // Space toggles accept/reject for entire file
  bool collapsed = false;           // Click or shortcut collapses/expands in review buffer
  SourceLang lang = SourceLang::PlainText;

  // Cached file lines for staging and diff generation
  std::vector<std::string> original_lines;
  std::vector<std::string> staged_lines;

  int additions_count = 0;
  int deletions_count = 0;

  int active_additions() const;
  int active_deletions() const;
};

enum class TransactionStatus : uint8_t {
  Draft,
  Staged,
  Committed,
  Aborted,
  Failed
};

const char* transaction_status_name(TransactionStatus status);

struct TransactionManifest {
  uint64_t tx_id = 0;
  std::string title;                // e.g. "Refactor Capability Attenuation"
  std::string author = "ai-agent";
  uint64_t timestamp = 0;
  TransactionStatus status = TransactionStatus::Draft;
  std::string error_message;
  std::vector<DiffFileEntry> files;

  int total_hunks() const;
  int total_additions() const;
  int total_deletions() const;
  int active_hunks_count() const;

  // Global hunk index lookup: returns {file_idx, hunk_idx} or {-1, -1}
  std::pair<int, int> find_hunk_location(int global_idx) const;
  int global_hunk_index(int file_idx, int hunk_idx) const;
};

// Hit-test item in the unified atomic review viewport
struct TransactionReviewHit {
  int y_top = 0;
  int y_bottom = 0;
  int file_idx = -1;
  int hunk_idx = -1;
  bool is_file_header = false;
  bool is_hunk_header = false;
  bool is_commit_btn = false;
  bool is_abort_btn = false;
};

class PatchEngine {
public:
  // Parse unified diff format (standard diff -u or git diff output) into a TransactionManifest
  static Result<TransactionManifest> parse_unified_diff(
      const std::string& diff_text,
      const std::string& default_title = "Atomic Code Transaction",
      uint64_t tx_id = 402);

  // Format the Intent Declaration Plan manifest for console / terminal output
  static std::string format_plan_manifest(const TransactionManifest& manifest);

  // Generate a unified diff representation from a TransactionManifest
  static std::string generate_unified_diff(const TransactionManifest& manifest, bool accepted_only = false);

  // Perform deterministic pre-flight check.
  // Reads files from disk/kernel, verifies all context and deletion lines match exactly.
  // Returns error on first mismatch; touches NO files on disk.
  static Result<void> dry_run(
      Kernel& k,
      TransactionManifest& manifest,
      const std::string& root_dir = ".");

  // The same check with the caller's file reader (path -> contents; false when
  // the file cannot be read), for code that has no Kernel — the sidecar C ABI.
  static Result<void> dry_run_with(
      const std::function<bool(const std::string&, std::string&)>& read_file,
      TransactionManifest& manifest,
      const std::string& root_dir = ".");

  // Stage the transaction in memory.
  // Computes staged file contents based on accepted hunks. Main disk tree remains untouched.
  static Result<void> stage(
      Kernel& k,
      TransactionManifest& manifest,
      const std::string& root_dir = ".");

  // Re-compute staged contents when hunks are toggled (Space key)
  static Result<void> restage_with_hunk_selection(
      TransactionManifest& manifest);

  // Commit all staged files to disk in one atomic tick.
  // If any file write fails, rolls back all files immediately and returns error.
  static Result<void> commit(
      Kernel& k,
      TransactionManifest& manifest,
      const std::string& root_dir = ".");

  // Abort transaction: clears memory staging; tree remains pristine.
  static void abort(TransactionManifest& manifest);

  // Helper to apply patch string directly in an all-or-nothing transaction
  static Result<void> apply_patch_atomic(
      Kernel& k,
      const std::string& patch_text,
      const std::string& title = "Atomic Patch",
      const std::string& root_dir = ".");

  // Global transaction singleton for editor & CLI coordination
  static TransactionManifest& active_transaction();
  static bool has_active_transaction();
  static void set_active_transaction(const TransactionManifest& tx);
  static void clear_active_transaction();
};

} // namespace aios::userland
