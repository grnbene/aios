#pragma once
// Kernel paths never throw. Every fallible operation returns a Status or a
// Result<T>. The status vocabulary is deliberately small.
#include <cstdint>
#include <string>
#include <utility>
#include <optional>

namespace aios {

enum class Status : uint8_t {
  Ok = 0,
  NotFound,
  Denied,       // capability check failed
  Exhausted,    // resource limit reached
  Invalid,      // malformed request
  Conflict,     // generation / ownership mismatch
  Unsupported,  // no device / transport / provider can satisfy this
  IoError,      // the host refused
  Corrupt,      // persisted image failed validation
  Busy,
  Expired,
};

const char* status_name(Status s);

struct Error {
  Status status = Status::Ok;
  std::string message;
  bool ok() const { return status == Status::Ok; }
  std::string str() const { return std::string(status_name(status)) + (message.empty() ? "" : ": " + message); }
};

template <typename T>
class Result {
 public:
  Result(T value) : value_(std::move(value)) {}                 // NOLINT implicit by design
  Result(Status s, std::string msg = {}) : err_{s, std::move(msg)} {}
  Result(Error e) : err_(std::move(e)) {}                        // NOLINT
  bool ok() const { return err_.ok(); }
  explicit operator bool() const { return ok(); }
  Status status() const { return err_.status; }
  const Error& error() const { return err_; }
  T& value() { return *value_; }
  const T& value() const { return *value_; }
  T& operator*() { return *value_; }
  const T& operator*() const { return *value_; }
  T* operator->() { return &*value_; }
  const T* operator->() const { return &*value_; }
 private:
  std::optional<T> value_;
  Error err_;
};

template <>
class Result<void> {
 public:
  Result() = default;
  Result(Status s, std::string msg = {}) : err_{s, std::move(msg)} {}
  Result(Error e) : err_(std::move(e)) {}  // NOLINT
  bool ok() const { return err_.ok(); }
  explicit operator bool() const { return ok(); }
  Status status() const { return err_.status; }
  const Error& error() const { return err_; }
 private:
  Error err_;
};

inline Result<void> Ok() { return Result<void>(); }

#define AIOS_TRY(expr)                                   \
  do {                                                   \
    auto&& _r = (expr);                                  \
    if (!_r.ok()) return ::aios::Error(_r.error());      \
  } while (0)

}  // namespace aios
