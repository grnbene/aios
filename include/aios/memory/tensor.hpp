#pragma once
// A Tensor is a typed, shaped view over a MemoryObject. It carries no data of
// its own; it names memory the way a pointer names bytes.
#include <vector>
#include <string>
#include "aios/core/ids.hpp"
#include "aios/core/result.hpp"
#include "aios/core/bytes.hpp"

namespace aios {

enum class DType : uint8_t { F32 = 0, F64 = 1, I32 = 2, I64 = 3, U8 = 4 };
const char* dtype_name(DType t);
bool parse_dtype(const std::string& s, DType& out);
uint64_t dtype_size(DType t);

struct Tensor {
  MemoryID memory;
  DType dtype = DType::F32;
  std::vector<uint64_t> shape;
  uint64_t offset = 0;      // byte offset inside the memory object

  uint64_t elements() const { uint64_t n = 1; for (auto d : shape) n *= d; return shape.empty() ? 0 : n; }
  uint64_t bytes() const { return elements() * dtype_size(dtype); }
  uint64_t rank() const { return shape.size(); }
  std::string describe() const;

  void encode(Writer& w) const;
  static Result<Tensor> decode(Reader& r);
};

}  // namespace aios
