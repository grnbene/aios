#include "history_graph.hpp"

#include <algorithm>

namespace aios_git {

HistoryGraph::HistoryGraph(std::function<bool(const std::string&)> interesting, bool first_parent_only)
    : interesting_(std::move(interesting)), first_parent_only_(first_parent_only) {}

// graph_line_write_column: a column's character in its color (none when colors are off)
void HistoryGraph::write_column(Line& line, const Column& col, char c) const {
  const bool colored = !colors_.empty() && col.color < colors_max_;
  if (colored) line.buf += colors_[col.color];
  line.ch(c);
  if (colored) line.buf += colors_[colors_max_];
}

// graph_find_commit_color: the color of the column already holding the commit, or the current default
unsigned short HistoryGraph::find_commit_color(const std::string& oid) const {
  for (auto& col : columns_)
    if (col.commit == oid) return col.color;
  return current_color();
}

bool HistoryGraph::needs_pre_commit_line() const {
  return num_parents_ >= 3 && commit_index_ < (int)columns_.size() - 1 && expansion_row_ < num_expansion_rows();
}

int HistoryGraph::find_new_column(const std::string& oid) const {
  for (size_t i = 0; i < new_columns_.size(); ++i)
    if (new_columns_[i].commit == oid) return (int)i;
  return -1;
}

void HistoryGraph::insert_into_new_columns(const std::string& oid, int idx) {
  int i = find_new_column(oid);
  if (i < 0) {
    i = (int)new_columns_.size();
    new_columns_.push_back(Column{oid, find_commit_color(oid)});
  }
  int mapping_idx;
  if (num_parents_ > 1 && idx > -1 && merge_layout_ == -1) {
    // the first parent of a merge picks the layout of the merge line: whether it sits left of the merge or not
    int dist = idx - i;
    int shift = dist > 1 ? 2 * dist - 3 : 1;
    merge_layout_ = dist > 0 ? 0 : 1;
    edges_added_ = num_parents_ + merge_layout_ - 2;
    mapping_idx = width_ + (merge_layout_ - 1) * shift;
    width_ += 2 * merge_layout_;
  } else if (edges_added_ > 0 && i == mapping_[width_ - 2]) {
    mapping_idx = width_ - 2;
    edges_added_ = -1;
  } else {
    mapping_idx = width_;
    width_ += 2;
  }
  mapping_[mapping_idx] = i;
}

void HistoryGraph::update_columns() {
  std::swap(columns_, new_columns_);
  new_columns_.clear();
  int num_columns = (int)columns_.size();
  int max_new_columns = num_columns + num_parents_;
  // the two mapping arrays only grow and keep what they held; the commit row reads the previous commit's final mapping
  size_t need = (size_t)(2 * max_new_columns + 2);
  if (mapping_.size() < need) mapping_.resize(need, -1);
  if (old_mapping_.size() < need) old_mapping_.resize(need, -1);
  mapping_size_ = (size_t)(2 * max_new_columns);
  std::fill(mapping_.begin(), mapping_.begin() + (ptrdiff_t)mapping_size_, -1);
  width_ = 0;
  prev_edges_added_ = edges_added_;
  edges_added_ = 0;
  bool seen_this = false, in_columns = true;
  for (int i = 0; i <= num_columns; ++i) {
    std::string col_commit;
    if (i == num_columns) {
      if (seen_this) break;
      in_columns = false;
      col_commit = commit_;
    } else {
      col_commit = columns_[(size_t)i].commit;
    }
    if (col_commit == commit_) {
      seen_this = true;
      commit_index_ = i;
      merge_layout_ = -1;
      for (auto& p : parents_) {
        // a merge, or the start of a new childless column, takes the next color
        if ((num_parents_ > 1 || !in_columns) && colors_max_) default_color_ = (unsigned short)((default_color_ + 1) % colors_max_);
        insert_into_new_columns(p, i);
      }
      if (num_parents_ == 0) width_ += 2;
    } else {
      insert_into_new_columns(col_commit, -1);
    }
  }
  while (mapping_size_ > 1 && mapping_[mapping_size_ - 1] < 0) --mapping_size_;
}

void HistoryGraph::update(const std::string& oid, const std::vector<std::string>& parents, char commit_char) {
  commit_ = oid;
  commit_char_ = commit_char;
  parents_.clear();
  if (first_parent_only_) {
    if (!parents.empty() && interesting_parent(parents[0])) parents_.push_back(parents[0]);
  } else {
    for (auto& p : parents) if (interesting_parent(p)) parents_.push_back(p);
  }
  num_parents_ = (int)parents_.size();
  prev_commit_index_ = commit_index_;
  update_columns();
  expansion_row_ = 0;
  if (state_ != State::Padding) state_ = State::Skip;
  else if (needs_pre_commit_line()) state_ = State::PreCommit;
  else state_ = State::Commit;
}

bool HistoryGraph::mapping_correct() const {
  for (size_t i = 0; i < mapping_size_; ++i) {
    int target = mapping_[i];
    if (target < 0) continue;
    if (target == (int)(i / 2)) continue;
    return false;
  }
  return true;
}

void HistoryGraph::pad(Line& line) const { line.chars(' ', width_ - line.width); }

void HistoryGraph::octopus_merge(Line& line) const {
  int dashless_parents = 3 - merge_layout_;
  int dashful_parents = num_parents_ - dashless_parents;
  for (int i = 0; i < dashful_parents; ++i) {
    // each dash in the color of the column its edge collapses to
    const int j = mapping_[(size_t)((commit_index_ + i + 2) * 2)];
    const Column& col = new_columns_[(size_t)j];
    write_column(line, col, '-');
    write_column(line, col, i == dashful_parents - 1 ? '.' : '-');
  }
}

void HistoryGraph::pre_commit_line(Line& line) {
  bool seen_this = false;
  for (size_t i = 0; i < columns_.size(); ++i) {
    const Column& col = columns_[i];
    if (col.commit == commit_) {
      seen_this = true;
      write_column(line, col, '|');
      line.chars(' ', expansion_row_);
    } else if (seen_this && expansion_row_ == 0) {
      write_column(line, col, (prev_state_ == State::PostMerge && prev_commit_index_ < (int)i) ? '\\' : '|');
    } else if (seen_this && expansion_row_ > 0) {
      write_column(line, col, '\\');
    } else {
      write_column(line, col, '|');
    }
    line.ch(' ');
  }
  ++expansion_row_;
  if (!needs_pre_commit_line()) set_state(State::Commit);
}

void HistoryGraph::commit_line(Line& line) {
  bool seen_this = false;
  int num_columns = (int)columns_.size();
  const Column none{};
  for (int i = 0; i <= num_columns; ++i) {
    const Column& col = i < num_columns ? columns_[(size_t)i] : none;
    const std::string& col_commit = i < num_columns ? col.commit : commit_;
    if (i == num_columns && seen_this) break;
    if (col_commit == commit_) {
      seen_this = true;
      line.ch(commit_char_);
      if (num_parents_ > 2) octopus_merge(line);
    } else if (seen_this && edges_added_ > 1) {
      write_column(line, col, '\\');
    } else if (seen_this && edges_added_ == 1) {
      write_column(line, col, (prev_state_ == State::PostMerge && prev_edges_added_ > 0 && prev_commit_index_ < i) ? '\\' : '|');
    } else if (prev_state_ == State::Collapsing && old_mapping_[(size_t)(2 * i + 1)] == i && mapping_[(size_t)(2 * i)] < i) {
      write_column(line, col, '/');
    } else {
      write_column(line, col, '|');
    }
    line.ch(' ');
  }
  if (num_parents_ > 1) set_state(State::PostMerge);
  else if (mapping_correct()) set_state(State::Padding);
  else set_state(State::Collapsing);
}

void HistoryGraph::post_merge_line(Line& line) {
  static const char merge_chars[] = {'/', '|', '\\'};
  bool seen_this = false;
  const Column* parent_col = nullptr;
  int num_columns = (int)columns_.size();
  const Column none{};
  for (int i = 0; i <= num_columns; ++i) {
    const Column& col = i < num_columns ? columns_[(size_t)i] : none;
    const std::string& col_commit = i < num_columns ? col.commit : commit_;
    if (i == num_columns && seen_this) break;
    if (col_commit == commit_) {
      int idx = merge_layout_;
      seen_this = true;
      for (size_t k = 0; k < parents_.size(); ++k) {
        const int par_column = find_new_column(parents_[k]);
        write_column(line, new_columns_[(size_t)std::max(0, par_column)], merge_chars[idx]);
        if (idx == 2) {
          if (edges_added_ > 0 || k + 1 < parents_.size()) line.ch(' ');
        } else {
          ++idx;
        }
      }
      if (edges_added_ == 0) line.ch(' ');
    } else if (seen_this) {
      write_column(line, col, edges_added_ > 0 ? '\\' : '|');
      line.ch(' ');
    } else {
      write_column(line, col, '|');
      if (merge_layout_ != 0 || i != commit_index_ - 1) {
        if (parent_col) write_column(line, *parent_col, '_');
        else line.ch(' ');
      }
    }
    if (!parents_.empty() && col_commit == parents_[0]) parent_col = &col;
  }
  if (mapping_correct()) set_state(State::Padding);
  else set_state(State::Collapsing);
}

void HistoryGraph::collapsing_line(Line& line) {
  bool used_horizontal = false;
  int horizontal_edge = -1, horizontal_edge_target = -1;
  std::swap(mapping_, old_mapping_);
  std::fill(mapping_.begin(), mapping_.begin() + (ptrdiff_t)mapping_size_, -1);
  size_t mapping_size = mapping_size_;
  for (size_t i = 0; i < mapping_size; ++i) {
    int target = old_mapping_[i];
    if (target < 0) continue;
    if (target * 2 == (int)i) {
      mapping_[i] = target;
    } else if (mapping_[i - 1] < 0) {
      mapping_[i - 1] = target;
      if (horizontal_edge == -1) {
        horizontal_edge = (int)i;
        horizontal_edge_target = target;
        for (int j = target * 2 + 3; j < (int)i - 2; j += 2) mapping_[(size_t)j] = target;
      }
    } else if (mapping_[i - 1] == target) {
    } else {
      mapping_[i - 2] = target;
      if (horizontal_edge == -1) {
        horizontal_edge = (int)i - 1;
        horizontal_edge_target = target;
        for (int j = target * 2 + 3; j < (int)i - 2; j += 2) mapping_[(size_t)j] = target;
      }
    }
  }
  std::copy(mapping_.begin(), mapping_.begin() + (ptrdiff_t)mapping_size, old_mapping_.begin());
  if (mapping_[mapping_size - 1] < 0) --mapping_size;
  mapping_size_ = mapping_size;
  for (size_t i = 0; i < mapping_size; ++i) {
    int target = mapping_[i];
    if (target < 0) {
      line.ch(' ');
    } else if (target * 2 == (int)i) {
      write_column(line, new_columns_[(size_t)target], '|');
    } else if (target == horizontal_edge_target && (int)i != horizontal_edge - 1) {
      if ((int)i != target * 2 + 3) mapping_[i] = -1;
      used_horizontal = true;
      write_column(line, new_columns_[(size_t)target], '_');
    } else {
      if (used_horizontal && (int)i < horizontal_edge) mapping_[i] = -1;
      write_column(line, new_columns_[(size_t)target], '/');
    }
  }
  if (mapping_correct()) set_state(State::Padding);
}

bool HistoryGraph::next_line(std::string& out) {
  Line line;
  bool shown = false;
  switch (state_) {
    case State::Padding:
      if (commit_.empty()) return false;
      for (auto& col : new_columns_) { write_column(line, col, '|'); line.ch(' '); }
      break;
    case State::Skip:
      line.buf += "...";
      line.width += 3;
      if (needs_pre_commit_line()) set_state(State::PreCommit);
      else set_state(State::Commit);
      break;
    case State::PreCommit: pre_commit_line(line); break;
    case State::Commit: commit_line(line); shown = true; break;
    case State::PostMerge: post_merge_line(line); break;
    case State::Collapsing: collapsing_line(line); break;
  }
  pad(line);
  out += line.buf;
  return shown;
}

size_t HistoryGraph::padding_prefix_bytes() const {
  Line line;
  for (auto& col : new_columns_) { write_column(line, col, '|'); line.ch(' '); }
  pad(line);
  return line.buf.size();
}

std::string HistoryGraph::padding_line() {
  std::string out;
  if (state_ != State::Commit) {
    next_line(out);
    return out;
  }
  Line line;
  for (auto& col : columns_) {
    write_column(line, col, '|');
    if (col.commit == commit_ && num_parents_ > 2) line.chars(' ', (num_parents_ - 2) * 2);
    else line.ch(' ');
  }
  pad(line);
  prev_state_ = State::Padding;
  return line.buf;
}

std::string HistoryGraph::show_commit() {
  std::string out;
  bool shown_commit_line = false;
  if (finished()) {
    out += padding_line();
    shown_commit_line = true;
  }
  while (!shown_commit_line && !finished()) {
    std::string line;
    shown_commit_line = next_line(line);
    out += line;
    if (!shown_commit_line) out += '\n';
  }
  return out;
}

std::string HistoryGraph::show_oneline() {
  std::string out;
  next_line(out);
  return out;
}

std::string HistoryGraph::remainder() {
  std::string out;
  if (finished()) return out;
  for (;;) {
    std::string line;
    next_line(line);
    out += line;
    if (finished()) break;
    out += '\n';
  }
  return out;
}

std::string HistoryGraph::show_commit_msg(const std::string& message) {
  std::string out;
  size_t p = 0;
  while (p < message.size()) {
    size_t nl = message.find('\n', p);
    size_t next = nl == std::string::npos ? message.size() : nl + 1;
    out.append(message, p, next - p);
    if (nl != std::string::npos && next < message.size()) out += show_oneline();
    p = next;
  }
  if (!finished()) {
    bool newline_terminated = !message.empty() && message.back() == '\n';
    if (!newline_terminated) out += '\n';
    out += remainder();
    if (newline_terminated) out += '\n';
  }
  return out;
}

}  // namespace aios_git
