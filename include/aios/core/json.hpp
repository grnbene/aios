#pragma once
// JSON (RFC 8259): a value tree, a strict parser and a compact writer. No exceptions, bounded depth,
// and the same on every substrate: the OAuth tokens, Google's REST answers and the sync bookkeeping all
// go through it.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aios/core/result.hpp"

namespace aios::json {

class Value {
 public:
  enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };

  Value() = default;
  static Value null() { return Value(); }
  static Value boolean(bool b) { Value v; v.type_ = Type::Bool; v.bool_ = b; return v; }
  static Value number(double d) { Value v; v.type_ = Type::Number; v.num_ = d; v.is_int_ = false; return v; }
  static Value integer(int64_t i) { Value v; v.type_ = Type::Number; v.int_ = i; v.num_ = static_cast<double>(i); v.is_int_ = true; return v; }
  static Value string(std::string s) { Value v; v.type_ = Type::String; v.str_ = std::move(s); return v; }
  static Value array() { Value v; v.type_ = Type::Array; return v; }
  static Value object() { Value v; v.type_ = Type::Object; return v; }

  Type type() const { return type_; }
  bool is_null() const { return type_ == Type::Null; }
  bool is_bool() const { return type_ == Type::Bool; }
  bool is_number() const { return type_ == Type::Number; }
  bool is_string() const { return type_ == Type::String; }
  bool is_array() const { return type_ == Type::Array; }
  bool is_object() const { return type_ == Type::Object; }

  // Reads with a default: a missing key, or a value of the wrong type, gives the default. Nothing throws.
  const std::string& str() const;                      // "" unless a string
  std::string str_or(std::string def) const { return is_string() ? str_ : def; }
  double num(double def = 0) const { return is_number() ? num_ : def; }
  int64_t i64(int64_t def = 0) const;                  // a number truncated toward zero, or def
  bool boolean_or(bool def = false) const { return is_bool() ? bool_ : def; }

  // Objects: the first member called `key`, or a shared null when absent (so chains are safe: v["a"]["b"]).
  const Value& operator[](std::string_view key) const;
  bool has(std::string_view key) const;
  const std::vector<std::pair<std::string, Value>>& members() const { return obj_; }
  // Arrays.
  const Value& operator[](size_t i) const;
  size_t size() const { return type_ == Type::Array ? arr_.size() : type_ == Type::Object ? obj_.size() : 0; }
  const std::vector<Value>& items() const { return arr_; }

  // Building.
  Value& set(std::string key, Value v);                // replaces an existing member of that name
  Value& push(Value v);
  void erase(std::string_view key);

  // Compact text. Strings are escaped; non-ASCII stays UTF-8 as it is.
  std::string dump() const;

 private:
  Type type_ = Type::Null;
  bool bool_ = false;
  bool is_int_ = false;
  double num_ = 0;
  int64_t int_ = 0;
  std::string str_;
  std::vector<Value> arr_;
  std::vector<std::pair<std::string, Value>> obj_;
  friend class Parser;
};

// Strict: one value, then only whitespace. Depth over 64 and text over 64 MB are refused.
Result<Value> parse(std::string_view text);

// The writer's escaping, for callers that build JSON by hand.
std::string quote(std::string_view s);

}  // namespace aios::json
