#include "aios/memory/tensor.hpp"

namespace aios {
const char* dtype_name(DType t) {
  switch (t) { case DType::F32: return "f32"; case DType::F64: return "f64"; case DType::I32: return "i32"; case DType::I64: return "i64"; case DType::U8: return "u8"; }
  return "?";
}
bool parse_dtype(const std::string& s, DType& out) {
  if (s == "f32") out = DType::F32; else if (s == "f64") out = DType::F64; else if (s == "i32") out = DType::I32;
  else if (s == "i64") out = DType::I64; else if (s == "u8") out = DType::U8; else return false;
  return true;
}
uint64_t dtype_size(DType t) {
  switch (t) { case DType::F32: return 4; case DType::F64: return 8; case DType::I32: return 4; case DType::I64: return 8; case DType::U8: return 1; }
  return 0;
}
std::string Tensor::describe() const {
  std::string s = std::string(dtype_name(dtype)) + "[";
  for (size_t i = 0; i < shape.size(); ++i) { if (i) s += "x"; s += std::to_string(shape[i]); }
  return s + "]@" + memory.short_str();
}
void Tensor::encode(Writer& w) const {
  w.id(memory); w.u8((uint8_t)dtype); w.u64(offset); w.u32((uint32_t)shape.size());
  for (auto d : shape) w.u64(d);
}
Result<Tensor> Tensor::decode(Reader& r) {
  Tensor t;
  t.memory = MemoryID{r.id()};
  t.dtype = (DType)r.u8();
  t.offset = r.u64();
  uint32_t n = r.u32();
  if (!r.ok() || n > 64) return Result<Tensor>(Status::Corrupt, "tensor shape");
  for (uint32_t i = 0; i < n; ++i) t.shape.push_back(r.u64());
  if (!r.ok()) return Result<Tensor>(Status::Corrupt, "tensor");
  return t;
}
}  // namespace aios
