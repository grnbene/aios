#include "aios/channel/ingress_receiver.hpp"
#include <algorithm>
#include "aios/core/strbuf.hpp"
#include <cstdlib>
#include "aios/core/bytes.hpp"
#include "aios/crypto/sha256.hpp"
#include "aios/hal/host.hpp"
#include "aios/kernel/kernel.hpp"
#include "aios/kernel/surface.hpp"
#include "aios/userland/sql_database.hpp"

namespace aios::channel {

namespace {

std::string detect_content_type(const std::string& filename, const std::vector<uint8_t>& data) {
  auto ends = [&](const char* ext) {
    std::string e(ext);
    return filename.size() >= e.size() && filename.compare(filename.size() - e.size(), e.size(), e) == 0;
  };
  if (ends(".sql")) return "text/x-sql";
  if (ends(".json")) return "application/json";
  if (ends(".aos")) return "application/x-aos";
  std::string snippet(data.begin(), data.begin() + (long)std::min<size_t>(data.size(), 256));
  if (snippet.find("CREATE TABLE") != std::string::npos || snippet.find("INSERT INTO") != std::string::npos)
    return "text/x-sql";
  return "application/octet-stream";
}

std::string sha256_hex(const uint8_t* p, size_t n) {
  auto h = crypto::sha256(ByteSpan(p, n));
  return crypto::bytes_to_hex(ByteSpan(h.data(), h.size()));
}

// A filename is a name, not a path: no directories, no way out of ingress/.
std::string safe_name(const std::string& name) {
  std::string base = name;
  size_t slash = base.find_last_of("/\\");
  if (slash != std::string::npos) base = base.substr(slash + 1);
  if (base.empty() || base == "." || base == "..") base = "payload.bin";
  return base;
}

}  // namespace

IngressReceiver::IngressReceiver(Kernel& kernel, Surface* actor) : kernel_(kernel), actor_(actor) { load_state(); }

std::string IngressReceiver::ingress_dir() const { return kernel_.config().root + "/ingress"; }
std::string IngressReceiver::manifest_path() const { return kernel_.config().root + "/ingress_manifest.state"; }

Result<void> IngressReceiver::may_write() const {
  if (!actor_) return Ok();
  auto may = actor_->may(ResourceRef{ResourceKind::StorageObject, {}}, R_Write);
  if (!may.ok())
    return Result<void>(Status::Denied, "entity " + actor_->self().short_str() +
                                            " may not stage payloads on this machine: " + may.error().str());
  return Ok();
}

Result<IngressPayload> IngressReceiver::receive_payload(const std::string& filename, const std::string& customer_id,
                                                        const std::vector<uint8_t>& data,
                                                        const std::string& target_path, const std::string& source) {
  if (auto ok = may_write(); !ok.ok()) return Result<IngressPayload>(ok.error());
  auto& host = kernel_.host();

  IngressPayload payload;
  payload.payload_id = "ing_" + std::to_string(++counter_);
  payload.filename = safe_name(filename);
  payload.customer_id = customer_id.empty() ? "global" : customer_id;
  payload.source = source;
  payload.data = data;
  payload.size_bytes = data.size();
  payload.content_type = detect_content_type(payload.filename, data);
  payload.timestamp_ns = host.wall_ns();
  payload.sha256_checksum = sha256_hex(data.data(), data.size());
  // One file per payload: two payloads with the same filename must not
  // overwrite each other's staged bytes (and with them each other's checksum).
  payload.target_path = ingress_dir() + "/" + payload.payload_id + "_" +
                        (target_path.empty() ? payload.filename : safe_name(target_path));

  (void)host.make_dir(kernel_.config().root);
  (void)host.make_dir(ingress_dir());
  auto wrote = host.write_file(payload.target_path, ByteSpan(data.data(), data.size()));
  if (!wrote.ok()) return Result<IngressPayload>(wrote.error());
  payload.status = "STAGED";

  // VERIFIED means the bytes on disk were read back and hash to what arrived.
  auto back = host.read_file(payload.target_path);
  if (back.ok() && sha256_hex(back->data(), back->size()) == payload.sha256_checksum) payload.status = "VERIFIED";

  payloads_.push_back(payload);
  if (auto saved = save_state(); !saved.ok()) return Result<IngressPayload>(saved.error());

  if (payload.content_type == "text/x-sql" && payload.status == "VERIFIED") {
    (void)auto_ingest(payload.payload_id);
    for (auto& p : payloads_)
      if (p.payload_id == payload.payload_id) { payload.status = p.status; payload.detail = p.detail; break; }
  }
  return payload;
}

Result<IngressPayload> IngressReceiver::receive_string(const std::string& filename, const std::string& customer_id,
                                                       const std::string& content, const std::string& target_path) {
  std::vector<uint8_t> data(content.begin(), content.end());
  return receive_payload(filename, customer_id, data, target_path, "inline");
}

Result<IngressPayload> IngressReceiver::receive_file(const std::string& source_filepath,
                                                     const std::string& customer_id, const std::string& target_name) {
  auto read_res = kernel_.host().read_file(source_filepath);
  if (!read_res.ok())
    return Result<IngressPayload>(Status::NotFound, source_filepath + ": " + read_res.error().message);
  std::vector<uint8_t> data(read_res->begin(), read_res->end());
  std::string fname = target_name.empty() ? safe_name(source_filepath) : target_name;
  return receive_payload(fname, customer_id, data, "", "file:" + source_filepath);
}

Result<void> IngressReceiver::verify_payload(const std::string& payload_id) {
  for (auto& p : payloads_) {
    if (p.payload_id != payload_id) continue;
    // From the staged file, so a later process can verify what an earlier one staged.
    auto on_disk = kernel_.host().read_file(p.target_path);
    if (!on_disk.ok()) {
      p.status = "MISSING";
      p.detail = p.target_path + ": " + on_disk.error().message;
      (void)save_state();
      return Result<void>(Status::NotFound, "staged file for " + payload_id + " is gone: " + p.detail);
    }
    std::string computed = sha256_hex(on_disk->data(), on_disk->size());
    if (computed != p.sha256_checksum) {
      p.status = "CORRUPTED";
      p.detail = "on disk " + computed + ", recorded " + p.sha256_checksum;
      (void)save_state();
      return Result<void>(Status::Corrupt, "checksum mismatch for " + payload_id + ": " + p.detail);
    }
    // A payload already ingested stays INGESTED; its file still matches.
    if (p.status != "INGESTED") p.status = "VERIFIED";
    p.detail.clear();
    return save_state();
  }
  return Result<void>(Status::NotFound, "payload not found: " + payload_id);
}

Result<void> IngressReceiver::auto_ingest(const std::string& payload_id) {
  IngressPayload* target = nullptr;
  for (auto& p : payloads_) if (p.payload_id == payload_id) { target = &p; break; }
  if (!target) return Result<void>(Status::NotFound, "payload not found: " + payload_id);
  if (target->content_type != "text/x-sql")
    return Result<void>(Status::Unsupported, "only SQL payloads are ingested; " + payload_id + " stays staged");
  if (auto ok = may_write(); !ok.ok()) return ok;

  std::string sql_str;
  if (!target->data.empty()) {
    sql_str.assign(target->data.begin(), target->data.end());
  } else {
    auto on_disk = kernel_.host().read_file(target->target_path);
    if (!on_disk.ok()) return Result<void>(on_disk.error());
    sql_str.assign(on_disk->begin(), on_disk->end());
  }
  userland::TaskLedger ledger(kernel_);
  ledger.init();
  userland::SqlDatabase db(kernel_, &ledger);
  db.init();
  auto res = db.pull_mariadb_sql(target->customer_id, sql_str);
  if (res.success) {
    target->status = "INGESTED";
    target->detail.clear();
    return save_state();
  }
  target->status = "FAILED";
  target->detail = res.error_message;
  (void)save_state();
  return Result<void>(Status::Invalid, "SQL ingestion error: " + res.error_message);
}

std::vector<IngressPayload> IngressReceiver::list_payloads(const std::string& customer_id) const {
  if (customer_id.empty()) return payloads_;
  std::vector<IngressPayload> filtered;
  for (const auto& p : payloads_) if (p.customer_id == customer_id) filtered.push_back(p);
  return filtered;
}

Result<IngressPayload> IngressReceiver::get_payload(const std::string& payload_id) const {
  for (const auto& p : payloads_) if (p.payload_id == payload_id) return p;
  return Result<IngressPayload>(Status::NotFound, "payload not found: " + payload_id);
}

static std::string pad(const std::string& s, size_t width) {
  if (s.length() >= width) return s.substr(0, width);
  return s + std::string(width - s.length(), ' ');
}

std::string IngressReceiver::render_ingress_log(const std::string& customer_id) const {
  auto shown = list_payloads(customer_id);
  std::string out = "Ingress log, " + manifest_path() + " (" + std::to_string(shown.size()) + " payload(s)" +
                    (customer_id.empty() ? std::string() : " for customer " + customer_id) + ")\n";
  out += "  ID         Customer        Type                 Size (Bytes)   Status       Filename\n";
  out += "  -------------------------------------------------------------------------------------\n";
  for (const auto& p : shown) {
    out += "  " + pad(p.payload_id, 10) + " " + pad(p.customer_id, 15) + " " + pad(p.content_type, 20) + " " +
           pad(std::to_string(p.size_bytes), 14) + " " + pad(p.status, 12) + " " + p.filename + "\n";
    if (!p.detail.empty()) out += "             " + p.detail + "\n";
  }
  return out;
}

void IngressReceiver::load_state() {
  payloads_.clear();
  counter_ = 0;
  auto read_res = kernel_.host().read_file(manifest_path());
  if (!read_res.ok() || read_res->empty()) return;
  Reader r(ByteSpan(read_res->data(), read_res->size()));
  uint32_t version = r.u32();
  // Version 2 manifests start with this marker; older ones started with the count.
  const bool v2 = version == 0x49474D32u;   // "IGM2"
  uint32_t count = v2 ? r.u32() : version;
  for (uint32_t i = 0; i < count && r.ok(); ++i) {
    IngressPayload p;
    p.payload_id = r.str();
    p.customer_id = r.str();
    p.content_type = r.str();
    p.size_bytes = r.u64();
    p.status = r.str();
    p.filename = r.str();
    p.sha256_checksum = r.str();
    p.target_path = r.str();
    if (v2) {
      p.source = r.str();
      p.detail = r.str();
      p.timestamp_ns = r.u64();
    }
    if (!r.ok()) break;
    payloads_.push_back(p);
    uint64_t n = 0;
    if (p.payload_id.rfind("ing_", 0) == 0) n = aios::to_u64(p.payload_id.c_str() + 4);
    counter_ = std::max(counter_, n);
  }
}

Result<void> IngressReceiver::save_state() const {
  Writer w;
  w.u32(0x49474D32u);
  w.u32(static_cast<uint32_t>(payloads_.size()));
  for (const auto& p : payloads_) {
    w.str(p.payload_id);
    w.str(p.customer_id);
    w.str(p.content_type);
    w.u64(p.size_bytes);
    w.str(p.status);
    w.str(p.filename);
    w.str(p.sha256_checksum);
    w.str(p.target_path);
    w.str(p.source);
    w.str(p.detail);
    w.u64(p.timestamp_ns);
  }
  (void)kernel_.host().make_dir(kernel_.config().root);
  return kernel_.host().write_file(manifest_path(), ByteSpan(w.buf().data(), w.buf().size()));
}

}  // namespace aios::channel
