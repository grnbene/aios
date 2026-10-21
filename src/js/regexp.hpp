#pragma once
// A backtracking regular-expression engine for ECMAScript patterns (ES2024 syntax: named groups, lookbehind,
// unicode property escapes, the d/g/i/m/s/u/v/y flags, and the Annex B legacy grammar for non-unicode patterns).
// It matches over UTF-16 code units, with an explicit backtrack stack so that long inputs do not recurse.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace aios::js::re {

struct ClassItem {
  uint32_t lo = 0, hi = 0;
  int prop = -1;            // >= 0: a predicate (see regexp.cpp) instead of a range
  uint32_t mask = 0;        // general-category mask for category predicates
  bool negated = false;     // a negated predicate (\P{...}, \D, \W, \S)
};

struct CharClass {
  std::vector<ClassItem> items;
  bool negate = false;
  // v-mode: nested class operations are flattened at parse time into this list of alternatives
  std::vector<std::vector<uint32_t>> strings;   // \q{abc} alternatives, longest first
  // set operations (v flag): the class is (items) minus/and with these
  std::vector<std::shared_ptr<CharClass>> subtract, intersect;
};

struct Inst {
  uint8_t op = 0;
  uint8_t flag = 0;
  int32_t a = 0, b = 0, c = 0, d = 0;
};

struct Program {
  std::vector<Inst> code;
  std::vector<CharClass> classes;
  int ncaps = 1;                          // including group 0
  int ncounters = 0;
  int nposregs = 0;
  std::vector<std::u16string> group_names;   // index -> name ("" for unnamed); size ncaps
  bool has_named = false;
  bool icase = false, multiline = false, dotall = false, unicode = false, unicode_sets = false, sticky = false, global = false, has_indices = false;
  std::u16string source, flags;
  // search acceleration
  int first_char = -1;                    // every match starts with this code unit (case-sensitive patterns only)
  bool anchored_start = false;            // starts with ^ in non-multiline mode
};

// Compile. On failure returns null and sets *error (a SyntaxError message, without the "Invalid regular expression:" prefix).
std::unique_ptr<Program> compile(const std::u16string& pattern, const std::u16string& flags, std::string* error);

enum class Result { Match, NoMatch, TooComplex };

// Match at the first position >= start (only at `start` when `sticky`). `caps` receives 2 * ncaps indexes (-1 for unset).
Result exec(const Program& p, const std::u16string& input, size_t start, bool sticky, std::vector<int>* caps);

}  // namespace aios::js::re
