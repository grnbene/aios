#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "aios/core/result.hpp"

namespace aios { class Kernel; class Surface; }

namespace aios::channel {

// Stages a payload (a local file, or text given on the command line) into the
// kernel root's ingress/ directory, records it in <root>/ingress_manifest.state,
// and loads SQL payloads into the machine's SQL database. There is no network
// listener here: a payload arrives because someone on this machine handed it in.
//
// Status, each one only once it is true:
//   STAGED     written under <root>/ingress/
//   VERIFIED   read back from disk and its SHA-256 matches the one taken on receipt
//   INGESTED   (SQL only) the database accepted it
//   FAILED     the database refused it
//   CORRUPTED  the staged file no longer matches its recorded SHA-256
//   MISSING    the staged file is gone
struct IngressPayload {
  std::string payload_id;
  std::string filename;
  std::string customer_id;
  std::string content_type;
  std::string source;           // "file:<path>" or "inline"
  size_t size_bytes = 0;
  std::string sha256_checksum;
  std::string target_path;
  std::string status;
  std::string detail;           // why, for FAILED / CORRUPTED
  uint64_t timestamp_ns = 0;    // wall clock at receipt
  std::vector<uint8_t> data;    // this process only; never persisted
};

class IngressReceiver {
 public:
  // Everything lives under the kernel's root; nothing is written to the
  // working directory. With an actor, staging and ingesting are checked against
  // that actor's storage authority.
  explicit IngressReceiver(Kernel& kernel, Surface* actor = nullptr);

  Result<IngressPayload> receive_payload(const std::string& filename, const std::string& customer_id,
                                         const std::vector<uint8_t>& data, const std::string& target_path = "",
                                         const std::string& source = "inline");
  Result<IngressPayload> receive_string(const std::string& filename, const std::string& customer_id,
                                        const std::string& content, const std::string& target_path = "");
  Result<IngressPayload> receive_file(const std::string& source_filepath, const std::string& customer_id,
                                      const std::string& target_name = "");

  // Re-reads the staged file and checks it against the SHA-256 taken on receipt.
  Result<void> verify_payload(const std::string& payload_id);
  Result<void> auto_ingest(const std::string& payload_id);

  std::vector<IngressPayload> list_payloads(const std::string& customer_id = "") const;
  Result<IngressPayload> get_payload(const std::string& payload_id) const;
  std::string render_ingress_log(const std::string& customer_id = "") const;
  std::string manifest_path() const;
  std::string ingress_dir() const;

  void load_state();
  Result<void> save_state() const;

 private:
  Kernel& kernel_;
  Surface* actor_ = nullptr;
  std::vector<IngressPayload> payloads_;
  uint64_t counter_ = 0;
  Result<void> may_write() const;
};

}  // namespace aios::channel
