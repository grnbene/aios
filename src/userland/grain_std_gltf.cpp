// gltf_*: reading glTF 2.0 models (the Khronos format: a JSON description, binary buffers, images) natively, so a
// program can bring a model's meshes, skeleton, animations and textures into its own store (apps/sqldoom keeps
// them in ASQL) without any other program or library.
//
//   gltf_read(bytes)          -> {json, model}: the description as a map (json_parse's), and the model (its buffers)
//       bytes: a .glb (the binary container: the JSON chunk and the BIN chunk), or a .gltf's JSON text whose
//       buffers are embedded as data: URIs. A .gltf naming buffers in other files is refused: read those and
//       embed them first.
//   gltf_accessor(model, i)   -> {count, type, comps, kind, data}: accessor i's elements, packed - kind "float"
//       (float32 each: floats as they are, normalized integers mapped to 0..1 or -1..1) or "uint" (uint32 each:
//       indices, joint numbers); type is the accessor's ("SCALAR", "VEC3", "MAT4" ..), comps how many numbers an
//       element has; data little-endian, element after element, with no padding. Sparse accessors are refused.
//   gltf_image(model, i)      -> {w, h, rgba, data}: image i decoded (PNG or JPEG), 4 bytes a pixel, top row first;
//       data: the image as stored in the file (its PNG or JPEG bytes, to keep it small; image_decode reads it back)
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "aios/userland/grain.hpp"
#include "aios/userland/web.hpp"
#include "grain_builtins_internal.hpp"

namespace aios::userland::grain {

namespace {

struct GltfModel {
  Value json;
  std::vector<std::vector<uint8_t>> buffers;
};

std::shared_ptr<GltfModel> get_model(const Value& v, std::string& error, const char* who) {
  if (v.type == Type::Extern) {
    auto ext = static_cast<ExternObj*>(v.obj.get());
    if (ext->kind_name == "gltf_model" && ext->ptr) return std::static_pointer_cast<GltfModel>(ext->ptr);
  }
  error = std::string(who) + "() wants a glTF model (gltf_read's) as its first argument, got a " + type_name(v.type);
  return nullptr;
}

uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

MapObj* as_map(const Value& v) { return v.type == Type::Map ? static_cast<MapObj*>(v.obj.get()) : nullptr; }
const Value* member(const Value& v, const char* k) {
  MapObj* m = as_map(v);
  return m ? m->find(k) : nullptr;
}
bool member_int(const Value& v, const char* k, int64_t* out) {
  const Value* f = member(v, k);
  if (!f || !f->is_number()) return false;
  *out = f->as_int();
  return true;
}
// element i of the list member k of v, or null
const Value* item(const Value& v, const char* k, int64_t i) {
  const Value* l = member(v, k);
  if (!l || l->type != Type::List || i < 0 || (size_t)i >= l->list_items().size()) return nullptr;
  return &l->list_items()[(size_t)i];
}

// a data: URI's bytes (base64 only: what glTF writers embed)
bool data_uri(const std::string& uri, std::vector<uint8_t>* out, std::string& error) {
  const size_t comma = uri.find(',');
  if (uri.rfind("data:", 0) != 0 || comma == std::string::npos || uri.substr(0, comma).find(";base64") == std::string::npos) {
    error = "the URI is not a base64 data: URI (a buffer or image in another file: read it and embed it)";
    return false;
  }
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
  };
  uint32_t acc = 0;
  int bits = 0;
  for (size_t i = comma + 1; i < uri.size(); ++i) {
    const char c = uri[i];
    if (c == '=') break;
    const int v = val(c);
    if (v < 0) { if (c == '\n' || c == '\r' || c == ' ') continue; error = "the data: URI is not valid base64"; return false; }
    acc = (acc << 6) | (uint32_t)v;
    bits += 6;
    if (bits >= 8) { bits -= 8; out->push_back((uint8_t)((acc >> bits) & 0xFF)); }
  }
  return true;
}

Value bi_gltf_read(Vm&, std::vector<Value>& args, std::string& error) {
  if (args[0].type != Type::Bytes) { error = "gltf_read() wants the file's bytes (read_bytes), got a " + std::string(type_name(args[0].type)); return Value::nil(); }
  const std::vector<uint8_t>& b = args[0].bytes_data();
  auto model = std::make_shared<GltfModel>();
  std::string json_text;
  std::vector<uint8_t> bin;
  bool has_bin = false;
  if (b.size() >= 12 && std::memcmp(b.data(), "glTF", 4) == 0) {
    const uint32_t version = le32(&b[4]), length = le32(&b[8]);
    if (version != 2) { error = "gltf_read(): a .glb of version " + std::to_string(version) + " (only glTF 2.0 is read)"; return Value::nil(); }
    if (length > b.size()) { error = "gltf_read(): the .glb says it is " + std::to_string(length) + " bytes and is " + std::to_string(b.size()); return Value::nil(); }
    size_t at = 12;
    while (at + 8 <= length) {
      const uint32_t clen = le32(&b[at]), ctype = le32(&b[at + 4]);
      if (at + 8 + (size_t)clen > length) { error = "gltf_read(): a chunk runs past the end of the .glb"; return Value::nil(); }
      const uint8_t* cd = &b[at + 8];
      if (ctype == 0x4E4F534Au) json_text.assign((const char*)cd, clen);
      else if (ctype == 0x004E4942u && !has_bin) { bin.assign(cd, cd + clen); has_bin = true; }
      at += 8 + clen;
    }
    if (json_text.empty()) { error = "gltf_read(): the .glb has no JSON chunk"; return Value::nil(); }
  } else {
    json_text.assign((const char*)b.data(), b.size());
  }
  std::string perr;
  model->json = grain_json_parse(json_text, perr);
  if (!perr.empty()) { error = "gltf_read(): the description is not JSON: " + perr; return Value::nil(); }
  if (!as_map(model->json)) { error = "gltf_read(): the description is not a JSON object"; return Value::nil(); }
  const Value* asset = member(model->json, "asset");
  const Value* ver = asset ? member(*asset, "version") : nullptr;
  if (!ver || ver->type != Type::Str || ver->str().rfind("2", 0) != 0) { error = "gltf_read(): not a glTF 2.0 description (asset.version)"; return Value::nil(); }
  if (const Value* bufs = member(model->json, "buffers"); bufs && bufs->type == Type::List) {
    for (size_t i = 0; i < bufs->list_items().size(); ++i) {
      const Value& bv = bufs->list_items()[i];
      const Value* uri = member(bv, "uri");
      std::vector<uint8_t> data;
      if (!uri) {
        if (!has_bin) { error = "gltf_read(): buffer " + std::to_string(i) + " has no URI and the file no BIN chunk"; return Value::nil(); }
        data = bin;
      } else {
        if (uri->type != Type::Str || !data_uri(uri->str(), &data, error)) { error = "gltf_read(): buffer " + std::to_string(i) + ": " + error; return Value::nil(); }
      }
      int64_t blen = 0;
      if (member_int(bv, "byteLength", &blen) && (int64_t)data.size() < blen) {
        error = "gltf_read(): buffer " + std::to_string(i) + " has " + std::to_string(data.size()) + " bytes of its " + std::to_string(blen);
        return Value::nil();
      }
      model->buffers.push_back(std::move(data));
    }
  }
  auto out = std::make_shared<MapObj>();
  out->put("json", model->json);
  out->put("model", Value::object(Type::Extern, std::make_shared<ExternObj>("gltf_model", model)));
  return Value::object(Type::Map, out);
}

// The bytes a buffer view covers, and its stride (0: tightly packed).
bool view_bytes(const GltfModel& m, int64_t view, const uint8_t** data, size_t* len, int64_t* stride, std::string& error) {
  const Value* bv = item(m.json, "bufferViews", view);
  if (!bv) { error = "no bufferView " + std::to_string(view); return false; }
  int64_t buf = 0, off = 0, blen = 0;
  member_int(*bv, "buffer", &buf);
  member_int(*bv, "byteOffset", &off);
  if (!member_int(*bv, "byteLength", &blen)) { error = "bufferView " + std::to_string(view) + " has no byteLength"; return false; }
  *stride = 0;
  member_int(*bv, "byteStride", stride);
  if (buf < 0 || (size_t)buf >= m.buffers.size()) { error = "bufferView " + std::to_string(view) + " names buffer " + std::to_string(buf) + ", which there is not"; return false; }
  const auto& B = m.buffers[(size_t)buf];
  if (off < 0 || blen < 0 || (size_t)(off + blen) > B.size()) { error = "bufferView " + std::to_string(view) + " runs past its buffer"; return false; }
  *data = B.data() + off;
  *len = (size_t)blen;
  return true;
}

Value bi_gltf_accessor(Vm&, std::vector<Value>& args, std::string& error) {
  auto m = get_model(args[0], error, "gltf_accessor");
  if (!m) return Value::nil();
  if (!want_number(args[1], error, "gltf_accessor")) return Value::nil();
  const int64_t ai = args[1].as_int();
  const Value* acc = item(m->json, "accessors", ai);
  if (!acc) { error = "gltf_accessor(): no accessor " + std::to_string(ai); return Value::nil(); }
  if (member(*acc, "sparse")) { error = "gltf_accessor(): accessor " + std::to_string(ai) + " is sparse, which is not read"; return Value::nil(); }
  int64_t count = 0, ctype = 0, off = 0;
  if (!member_int(*acc, "count", &count) || !member_int(*acc, "componentType", &ctype)) { error = "gltf_accessor(): accessor " + std::to_string(ai) + " needs count and componentType"; return Value::nil(); }
  member_int(*acc, "byteOffset", &off);
  const Value* tv = member(*acc, "type");
  const std::string type = tv && tv->type == Type::Str ? tv->str() : "";
  int comps = 0;
  if (type == "SCALAR") comps = 1; else if (type == "VEC2") comps = 2; else if (type == "VEC3") comps = 3; else if (type == "VEC4") comps = 4;
  else if (type == "MAT2") comps = 4; else if (type == "MAT3") comps = 9; else if (type == "MAT4") comps = 16;
  else { error = "gltf_accessor(): accessor " + std::to_string(ai) + " has type \"" + type + "\""; return Value::nil(); }
  int csize = 0;
  switch (ctype) { case 5120: case 5121: csize = 1; break; case 5122: case 5123: csize = 2; break; case 5125: case 5126: csize = 4; break;
    default: error = "gltf_accessor(): accessor " + std::to_string(ai) + " has componentType " + std::to_string(ctype); return Value::nil(); }
  const Value* nv = member(*acc, "normalized");
  const bool normalized = nv && nv->type == Type::Bool && nv->b;
  const bool as_float = ctype == 5126 || normalized;
  if (count < 0 || count > (int64_t)64 * 1024 * 1024) { error = "gltf_accessor(): accessor " + std::to_string(ai) + " has count " + std::to_string(count); return Value::nil(); }
  std::vector<uint8_t> out((size_t)count * comps * 4, 0);
  int64_t view = -1;
  if (member_int(*acc, "bufferView", &view)) {
    const uint8_t* data = nullptr;
    size_t len = 0;
    int64_t stride = 0;
    if (!view_bytes(*m, view, &data, &len, &stride, error)) { error = "gltf_accessor(): accessor " + std::to_string(ai) + ": " + error; return Value::nil(); }
    // matrices of 1- and 2-byte components pad each column to 4 bytes (the spec's alignment rule)
    const int rows = type == "MAT2" ? 2 : type == "MAT3" ? 3 : comps;
    const int cols = type == "MAT2" ? 2 : type == "MAT3" ? 3 : 1;
    const int col_bytes = (type == "MAT2" || type == "MAT3") ? ((rows * csize + 3) & ~3) : rows * csize;
    const int64_t elem = (int64_t)col_bytes * cols;
    if (stride == 0) stride = elem;
    if (count > 0 && (size_t)(off + (count - 1) * stride + elem) > len) { error = "gltf_accessor(): accessor " + std::to_string(ai) + " runs past its bufferView"; return Value::nil(); }
    for (int64_t e = 0; e < count; ++e) {
      const uint8_t* ep = data + off + e * stride;
      for (int c = 0; c < cols; ++c)
        for (int r = 0; r < rows; ++r) {
          const uint8_t* p = ep + c * col_bytes + r * csize;
          const size_t o = ((size_t)e * comps + (size_t)c * rows + r) * 4;
          if (as_float) {
            float f = 0;
            switch (ctype) {
              case 5126: std::memcpy(&f, p, 4); break;
              case 5120: f = std::max(-1.0f, (int8_t)p[0] / 127.0f); break;
              case 5121: f = p[0] / 255.0f; break;
              case 5122: f = std::max(-1.0f, (int16_t)(p[0] | (p[1] << 8)) / 32767.0f); break;
              case 5123: f = (uint16_t)(p[0] | (p[1] << 8)) / 65535.0f; break;
              case 5125: f = (float)(le32(p) / 4294967295.0); break;
            }
            std::memcpy(&out[o], &f, 4);
          } else {
            uint32_t u = 0;
            switch (ctype) {
              case 5120: u = (uint32_t)(int32_t)(int8_t)p[0]; break;
              case 5121: u = p[0]; break;
              case 5122: u = (uint32_t)(int32_t)(int16_t)(p[0] | (p[1] << 8)); break;
              case 5123: u = (uint16_t)(p[0] | (p[1] << 8)); break;
              case 5125: u = le32(p); break;
            }
            out[o] = (uint8_t)u; out[o + 1] = (uint8_t)(u >> 8); out[o + 2] = (uint8_t)(u >> 16); out[o + 3] = (uint8_t)(u >> 24);
          }
        }
    }
  }
  // (no bufferView: all zeros, as the spec says)
  auto res = std::make_shared<MapObj>();
  res->put("count", Value::integer(count));
  res->put("type", Value::string(type));
  res->put("comps", Value::integer(comps));
  res->put("kind", Value::string(as_float ? "float" : "uint"));
  res->put("data", Value::bytes(std::move(out)));
  return Value::object(Type::Map, res);
}

// an image's bytes decoded into {w, h, rgba}, or nil and the error
Value decoded(ByteSpan bytes, const std::string& who, std::string& error) {
  auto bm = web::decode_image(bytes);
  if (!bm.ok()) { error = who + ": " + bm.error().str(); return Value::nil(); }
  const web::Bitmap& B = bm.value();
  std::vector<uint8_t> rgba((size_t)B.w * B.h * 4);
  for (size_t i = 0; i < (size_t)B.w * B.h; ++i) {
    const uint32_t p = B.pixels[i];
    rgba[i * 4] = (uint8_t)(p >> 16); rgba[i * 4 + 1] = (uint8_t)(p >> 8); rgba[i * 4 + 2] = (uint8_t)p; rgba[i * 4 + 3] = (uint8_t)(p >> 24);
  }
  auto res = std::make_shared<MapObj>();
  res->put("w", Value::integer(B.w));
  res->put("h", Value::integer(B.h));
  res->put("rgba", Value::bytes(std::move(rgba)));
  return Value::object(Type::Map, res);
}

Value bi_gltf_image(Vm&, std::vector<Value>& args, std::string& error) {
  auto m = get_model(args[0], error, "gltf_image");
  if (!m) return Value::nil();
  if (!want_number(args[1], error, "gltf_image")) return Value::nil();
  const int64_t ii = args[1].as_int();
  const Value* img = item(m->json, "images", ii);
  if (!img) { error = "gltf_image(): no image " + std::to_string(ii); return Value::nil(); }
  std::vector<uint8_t> own;
  const uint8_t* data = nullptr;
  size_t len = 0;
  int64_t view = -1;
  if (member_int(*img, "bufferView", &view)) {
    int64_t stride = 0;
    if (!view_bytes(*m, view, &data, &len, &stride, error)) { error = "gltf_image(): image " + std::to_string(ii) + ": " + error; return Value::nil(); }
  } else if (const Value* uri = member(*img, "uri"); uri && uri->type == Type::Str) {
    if (!data_uri(uri->str(), &own, error)) { error = "gltf_image(): image " + std::to_string(ii) + ": " + error; return Value::nil(); }
    data = own.data();
    len = own.size();
  } else {
    error = "gltf_image(): image " + std::to_string(ii) + " has neither a bufferView nor a URI";
    return Value::nil();
  }
  Value out = decoded(ByteSpan(data, len), "gltf_image(): image " + std::to_string(ii), error);
  if (out.type == Type::Nil) return out;
  static_cast<MapObj*>(out.obj.get())->put("data", Value::bytes(std::vector<uint8_t>(data, data + len)));
  return out;
}


}  // namespace

void install_gltf_builtins(Vm& vm) {
  vm.define_native("gltf_read", 1, 1, bi_gltf_read);
  vm.define_native("gltf_accessor", 2, 2, bi_gltf_accessor);
  vm.define_native("gltf_image", 2, 2, bi_gltf_image);
}

}  // namespace aios::userland::grain
