#pragma once
// The Go checksum database, as this machine's own client.
//
// A module proxy is not trusted, and neither is this database: what is trusted
// is an Ed25519 key this kernel carries. sum.golang.org publishes an append-only
// transparency log; a lookup answers with the record for a module version and a
// SIGNED tree head. The signature covers the tree head and nothing else, so a
// record is worth exactly as much as the proof that it is IN that tree — this
// client fetches the log tiles and checks the inclusion proof itself, folding
// the record's leaf hash up to the root the key signed.
//
// Without that proof the record lines are unauthenticated text, which is why
// they are not accepted on their own here.
#include <cstdint>
#include <map>
#include <string>

#include "aios/core/bytes.hpp"
#include "aios/core/result.hpp"
#include "aios/kernel/kernel.hpp"

namespace aios::userland {

// The default: the database the Go distribution itself pins.
extern const char* const kGoSumDatabaseKey;

struct SumRecord {
  std::string module;
  std::string version;
  std::string hash;        // h1:... of the module zip, as the log recorded it
  std::string mod_hash;    // h1:... of its go.mod
  int64_t id = -1;         // the record's index in the log
};

// What the machine checked the record against, so a report can say it.
struct SumTree {
  std::string origin;      // "go.sum database tree"
  int64_t size = 0;
  std::string root;        // base64, as signed
  std::string signer;      // the key name that signed it
  size_t tiles_read = 0;   // how much of the log this machine had to read
};

// A signed note (Ed25519 over the note's text, RFC-less but fully specified by
// the format Go publishes). Returns the signed text; `signer` names the key.
Result<std::string> note_verify(const std::string& note, const std::string& verifier_key, std::string& signer);

class ChecksumDatabase {
 public:
  ChecksumDatabase(Kernel& kernel, std::string base, std::string verifier_key, int timeout_ms);

  // Look a module version up, verify the tree head's signature, and prove the
  // record is in that tree before returning it.
  Result<SumRecord> lookup(const std::string& module, const std::string& version, SumTree* tree);

 private:
  Result<ByteBuffer> fetch(const std::string& path);
  Result<ByteBuffer> tile(int level, int64_t n, int width);
  Result<void> node_hash(int level, int64_t index, int64_t tree_size, uint8_t out[32]);
  Result<void> subtree_hash(int64_t lo, int64_t hi, int64_t tree_size, uint8_t out[32]);

  Kernel& kernel_;
  std::string base_;
  std::string key_;
  int timeout_ms_;
  std::map<std::string, ByteBuffer> tiles_;
};

}  // namespace aios::userland
