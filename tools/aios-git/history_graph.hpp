#pragma once
// The ASCII history graph of `git log --graph`: which column each line of history sits in and the edge lines
// between commits, laid out and colored as git does (graph.c).
#include <functional>
#include <string>
#include <vector>

namespace aios_git {

class HistoryGraph {
 public:
  // `interesting` says whether a parent will be shown; with `first_parent_only` only the first parent is followed.
  HistoryGraph(std::function<bool(const std::string&)> interesting, bool first_parent_only);
  // The next commit to be drawn, its parents as the history shows them, and the character of its row.
  void update(const std::string& oid, const std::vector<std::string>& parents, char commit_char);
  // Lines before the commit's own, each ended with a newline, then the commit row's text (no newline).
  std::string show_commit();
  // The graph text for one more line of the commit's output (no newline).
  std::string show_oneline();
  // A line that keeps every branch line as it is (no newline).
  std::string padding_line();
  // The commit's message lines with the graph at the left of each after the first, and the graph lines still owed.
  std::string show_commit_msg(const std::string& message);
  bool finished() const { return state_ == State::Padding; }
  // the bytes of a padding row as the diff after a commit is prefixed with (git measures it with strlen)
  size_t padding_prefix_bytes() const;
  // git's column colors (graph.c: column_colors, log.graphColors), the reset last; empty for none
  void set_colors(std::vector<std::string> colors) {
    colors_ = std::move(colors);
    colors_max_ = colors_.empty() ? 0 : (unsigned short)(colors_.size() - 1);
    default_color_ = colors_max_ ? (unsigned short)(colors_max_ - 1) : 0;
  }

 private:
  enum class State { Padding, Skip, PreCommit, Commit, PostMerge, Collapsing };
  struct Column { std::string commit; unsigned short color = 0; };
  // a line of graph text and its width on screen (the color escapes take none)
  struct Line {
    std::string buf;
    int width = 0;
    void ch(char c) { buf += c; ++width; }
    void chars(char c, int n) { if (n > 0) { buf.append((size_t)n, c); width += n; } }
  };
  void write_column(Line& line, const Column& col, char c) const;
  unsigned short current_color() const { return colors_.empty() ? colors_max_ : default_color_; }
  unsigned short find_commit_color(const std::string& oid) const;
  bool interesting_parent(const std::string& oid) const { return interesting_(oid); }
  int num_expansion_rows() const { return (num_parents_ - 2) * 2; }
  bool needs_pre_commit_line() const;
  void update_columns();
  int find_new_column(const std::string& oid) const;
  void insert_into_new_columns(const std::string& oid, int idx);
  void set_state(State s) { prev_state_ = state_; state_ = s; }
  bool mapping_correct() const;
  void pad(Line& line) const;
  bool next_line(std::string& out);
  void commit_line(Line& line);
  void pre_commit_line(Line& line);
  void post_merge_line(Line& line);
  void collapsing_line(Line& line);
  void octopus_merge(Line& line) const;
  std::string remainder();

  std::function<bool(const std::string&)> interesting_;
  bool first_parent_only_;
  std::string commit_;
  char commit_char_ = '*';
  std::vector<std::string> parents_;
  int num_parents_ = 0, width_ = 0, expansion_row_ = 0;
  State state_ = State::Padding, prev_state_ = State::Padding;
  int commit_index_ = 0, prev_commit_index_ = 0, merge_layout_ = 0, edges_added_ = 0, prev_edges_added_ = 0;
  std::vector<Column> columns_, new_columns_;
  std::vector<int> mapping_, old_mapping_;
  size_t mapping_size_ = 0;
  std::vector<std::string> colors_;
  unsigned short colors_max_ = 0, default_color_ = 0;
};

}  // namespace aios_git
