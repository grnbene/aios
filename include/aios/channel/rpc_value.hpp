#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "aios/core/bytes.hpp"
#include "aios/core/result.hpp"

namespace aios::channel {

// The document model the chain's remote procedure surface speaks. Numbers are
// kept as their original text: an Ethereum balance is a 256-bit quantity and a
// satoshi amount must survive round-tripping, neither of which a double can do.
class RpcValue {
 public:
  enum class Kind { Null, Bool, Number, Text, List, Object };

  RpcValue() = default;
  static RpcValue boolean(bool v);
  static RpcValue number(const std::string& digits);
  static RpcValue number(uint64_t v);
  static RpcValue text(const std::string& v);
  static RpcValue list();
  static RpcValue object();

  Kind kind() const { return kind_; }
  bool is_null() const { return kind_ == Kind::Null; }
  bool truthy() const;

  const std::string& raw() const { return scalar_; }
  std::string as_text() const;
  uint64_t as_u64(uint64_t fallback = 0) const;
  double as_double(double fallback = 0.0) const;
  // Ethereum quantities arrive as "0x…" of arbitrary width; this hands back the
  // big-endian bytes so nothing is truncated on the way in.
  ByteBuffer as_quantity() const;

  void push(RpcValue v);
  void set(const std::string& key, RpcValue v);
  // Append a field the caller knows is not there yet: set() without the scan.
  void set_new(std::string key, RpcValue v);
  bool has(const std::string& key) const;

  size_t size() const;
  const RpcValue& operator[](size_t i) const;
  const RpcValue& operator[](const std::string& key) const;
  const std::vector<RpcValue>& items() const { return list_; }
  const std::vector<std::pair<std::string, RpcValue>>& fields() const { return object_; }

  std::string render() const;

 private:
  Kind kind_ = Kind::Null;
  std::string scalar_;
  std::vector<RpcValue> list_;
  std::vector<std::pair<std::string, RpcValue>> object_;
};

Result<RpcValue> rpc_parse(ByteSpan document);
Result<RpcValue> rpc_parse(const std::string& document);

// The remote procedure envelope both bitcoind and every execution client speak.
std::string rpc_request(const std::string& method, const std::vector<RpcValue>& params, uint64_t id = 1);

}  // namespace aios::channel
