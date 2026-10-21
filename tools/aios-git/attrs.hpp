#pragma once
// gitattributes: which attributes a path has, from the global file, every .gitattributes down to the path's
// directory, and $GIT_DIR/info/attributes, with git's precedence and macros.
#include <map>
#include <set>
#include <string>
#include <vector>

#include "cli.hpp"

namespace aios_git {

struct AttrValue {
  enum Kind { Unspecified, Set, Unset, Text } kind = Unspecified;
  std::string text;
  // "set", "unset", "unspecified" or the text, as check-attr prints it.
  std::string shown() const;
};

class Attributes {
 public:
  // `cached`: .gitattributes from the index only (check-attr --cached); else the work tree, with the index
  // standing in for a file the work tree lacks. `source_tree`: read them from that tree instead.
  Attributes(Ctx& c, bool cached, const std::string& source_tree = {}) : c_(c), cached_(cached), source_tree_(source_tree) {}
  AttrValue get(const std::string& path, const std::string& name);
  // Every attribute with a value for the path, in the order git lists them (the order it first met their names).
  std::vector<std::pair<std::string, AttrValue>> all(const std::string& path);
  // Makes git's registry know `name` (a name asked for is numbered even when no file mentions it).
  bool known(const std::string& name) const { return registry_.count(name) != 0; }

 private:
  struct Line {
    std::string pattern, base;
    bool anchored = false, macro = false;
    std::string macro_name;
    std::vector<std::pair<std::string, AttrValue>> attrs;
  };
  const std::vector<Line>& file_lines(const std::string& dir);
  void parse_into(const std::string& text, const std::string& base, bool top, std::vector<Line>& out);
  void ensure_loaded();
  void note_names(const Line& l);
  void note_name(const std::string& n);
  std::string tree_blob(const std::string& tree, const std::string& path);
  bool line_matches(const Line& l, const std::string& path) const;
  void fill_list(const std::vector<std::pair<std::string, AttrValue>>& list, std::map<std::string, AttrValue>& st, int depth);
  void fill_macro(const std::string& name, std::map<std::string, AttrValue>& st, int depth);
  std::map<std::string, AttrValue> resolve(const std::string& path);
  Ctx& c_;
  bool cached_;
  std::string source_tree_;
  bool loaded_ = false;
  std::map<std::string, std::vector<Line>> by_dir_;
  std::vector<Line> global_, info_;
  std::set<std::string> registry_;
  std::vector<std::string> order_;
  std::map<std::string, std::vector<std::pair<std::string, AttrValue>>> macros_;
};

}  // namespace aios_git
