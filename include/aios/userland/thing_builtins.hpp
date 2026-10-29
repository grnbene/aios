#pragma once
// What a Thing's program may name (sql_things.cpp keeps a Thing's VM to these), shared with `grain app check` so a
// Thing that names anything else is refused on the developer's machine, not at deploy.
#include <string>
#include <vector>

namespace aios::userland {

// Builtins a Thing may call: pure functions of their arguments (the engine retains these globals and no others).
inline const std::vector<std::string>& thing_allowed_builtins() {
  static const std::vector<std::string> names = {
      "print", "len", "type", "str", "repr", "int", "real", "push", "pop", "range", "sort", "reverse", "map", "flat_map",
      "filter", "reduce", "each", "for_each", "any", "all", "find_if", "flatten", "concat", "insert", "remove_at",
      "zip", "enumerate", "take", "drop", "count", "unique", "dedup", "compact", "keys", "values", "has", "del", "merge",
      "entries", "from_entries", "split", "join", "upper", "lower", "find", "replace", "slice", "ord", "chr", "trim",
      "ltrim", "rtrim", "starts_with", "ends_with", "contains", "index_of", "last_index_of", "pad_left", "pad_right",
      "repeat", "char_at", "chars", "is_digit", "is_alpha", "is_alnum", "is_space", "abs", "min", "max", "floor",
      "ceil", "sqrt", "pow", "round", "trunc", "sign", "clamp", "log", "log2", "log10", "exp", "assert", "try_call",
      "pcall", "error", "panic", "is_err", "is_ok", "json_parse", "json_stringify",
      // exact decimals and text layout: pure functions of their arguments
      "decimal", "dec_div", "scale", "decimal_prefix", "round_sig", "number_format", "is_numeric", "to_number", "format", "sort_by", "group_by", "key_by",
      "sum_by", "idiv", "upper_rtrim",
      "chunk", "partition", "take_while", "drop_while", "sum", "min_by", "max_by", "find_index", "zip_with",
      // patterns: the same text and pattern give the same answer anywhere
      "regex_test", "regex_match", "regex_find_all", "regex_replace", "regex_split",
      // blanks, records made on first use, sets, calendar days and markup: functions of their arguments (get_or_put
      // and add_keys change the map they are given, as push changes its list)
      "is_blank", "first_present", "get", "get_in", "get_or_put", "add_keys", "add_into", "date_range", "day_add",
      "html_escape", "xml_escape", "el", "html_raw", "html_stringify", "xml_stringify",
      "sat"};   // deterministic: a reaction replayed after a crash gets the same answer
  return names;
}

// What the engine defines for a Thing's VM besides those (sql_things.cpp, after retain_globals): kept in step by hand.
inline const std::vector<std::string>& thing_native_names() {
  static const std::vector<std::string> names = {"read", "read_raw", "read_with", "read_raw_with", "claims", "write", "read_target"};
  return names;
}

}  // namespace aios::userland
