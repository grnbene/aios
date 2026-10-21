#pragma once
// Block storage: an object store laid out by column, beyond what a packfile does.
//
// A pack is a list of objects, each zlib'd on its own or as a delta against another; reading a commit's parents
// means inflating the commit, and walking a long history inflates every commit in it. A block file
// (objects/blocks/<id>.blk) keeps the same objects by kind and by column instead:
//
//   - commits: their tree, parents and commit time as fixed-width columns, read with no inflation at all (a history
//     walk, a merge base, an ancestry check), and their full text in compressed frames;
//   - trees: entries encoded against two dictionaries (every file name and every mode, once), so the names repeated
//     across every version of every directory are stored once;
//   - blobs: split into content-defined chunks (a gear hash picks the cut points, so an insertion moves only the cuts
//     near it); a chunk stored once however many blobs hold it, so the versions of a big file share what they share;
//   - frames: objects of one kind, versions of one path next to each other, compressed together;
//   - outlines: for every blob of a language the structural merge reads, its named units (scope, place, a digest of
//     the unit and of the unit without what it holds), so which functions, classes or keys a commit touched is read
//     from the file instead of parsed (git_structural_outline, every version of a path parsed incrementally from
//     the one before it).
//
// Every object reads back byte for byte (the build reads each one back and checks its SHA-1 before the file is
// kept). Only aios reads block files: `git repack -a -d` turns a repository back into an ordinary pack.
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "aios/core/result.hpp"
#include "aios/userland/git.hpp"
#include "aios/userland/git_structural.hpp"

namespace aios::userland {

struct GitBlockStats {
  uint64_t objects = 0, commits = 0, trees = 0, blobs = 0, tags = 0;
  uint64_t chunks = 0, chunk_refs = 0;   // distinct chunks stored, chunk references from blobs
  uint64_t deltas = 0;                   // blobs stored as a delta against a newer version of their path
  uint64_t names = 0;                    // distinct file names in the tree dictionary
  uint64_t outlines = 0;                 // source blobs whose syntax outline the file keeps
  uint64_t frames = 0;
  uint64_t raw_bytes = 0;                // the objects' payloads, uncompressed
  uint64_t file_bytes = 0;               // the block file
  std::string path;
};

// Writes `oids` (objects of `repo`) into one new block file under objects/blocks, reads every object back and checks
// it, and only then keeps the file. With `outlines`, source blobs' syntax outlines go in too.
Result<GitBlockStats> git_build_blocks(Git& repo, const std::vector<std::string>& oids, bool outlines = true);
// What a block file holds, and whether every object in it reads back to its own id.
Result<GitBlockStats> git_verify_blocks(const Git& repo, const std::string& path, bool check_every_object);

// One open block file (Git keeps these for objects/blocks/*.blk).
class GitBlockFile {
 public:
  static Result<std::shared_ptr<GitBlockFile>> open(hal::Host& host, const std::string& path);
  bool find(const std::string& oid, uint32_t& idx) const;
  uint32_t count() const { return n_; }
  std::string oid_at(uint32_t idx) const;
  Result<GitObject> read(uint32_t idx) const;
  // A commit's tree, parents and commit time straight from the columns (false: not a commit in this file).
  bool commit_meta(uint32_t idx, std::string& tree, std::vector<std::string>& parents, uint64_t& commit_time) const;
  const std::string& path() const { return path_; }
  GitBlockStats stats() const;
  void ids_with_prefix(const std::string& prefix, std::vector<std::string>& out) const;
  const ByteBuffer& data_for_check() const { return data_; }
  // Object numbers in the order their bytes lie in the file (reading them so inflates each frame once).
  std::vector<uint32_t> storage_order() const;
  // A source blob's syntax outline, kept when the file was built (nullopt: not kept, or not a source file).
  std::optional<GitSyntaxOutline> outline(uint32_t idx) const;

 private:
  struct Frame { uint64_t off = 0; uint32_t clen = 0, ulen = 0; };
  std::string path_;
  ByteBuffer data_;
  uint32_t n_ = 0, nchunks_ = 0, nframes_ = 0, next_ = 0, ncommits_ = 0;
  const uint8_t *oids_ = nullptr, *fanout_ = nullptr, *kinds_ = nullptr, *loc_ = nullptr, *rows_ = nullptr,
                *commits_ = nullptr, *parents_ = nullptr, *chunk_lists_ = nullptr, *chunks_ = nullptr, *frames_ = nullptr,
                *ext_ = nullptr, *frames_data_ = nullptr;
  uint64_t frames_data_len_ = 0;
  std::vector<std::string> modes_, names_;
  mutable std::vector<std::pair<uint32_t, std::shared_ptr<ByteBuffer>>> cache_;   // recently inflated frames
  mutable std::shared_ptr<ByteBuffer> last_blob_;                                  // the last blob read, a likely delta base
  mutable uint32_t last_blob_idx_ = UINT32_MAX;
  std::string_view syntax_;                                                          // the section, compressed
  mutable std::once_flag syntax_once_;
  mutable std::string syntax_bytes_;
  mutable std::unordered_map<uint32_t, std::pair<size_t, size_t>> syntax_at_;       // object -> (offset, length)
  Result<std::shared_ptr<ByteBuffer>> frame(uint32_t f) const;
  std::string ref_oid(uint32_t ref) const;
  const uint8_t* ref_raw(uint32_t ref) const;
  Result<std::string> slice(uint32_t frame_idx, uint32_t off, uint32_t len) const;
};

}  // namespace aios::userland
