#pragma once
// The two archive shapes a package arrives in. tar.gz is already here for npm;
// ZIP is what a Python wheel and a Go module are, so it lives beside it rather
// than inside whichever installer happened to need it first.
#include <cstdint>
#include <string>
#include <vector>

#include "aios/core/bytes.hpp"
#include "aios/core/result.hpp"

namespace aios::userland {

struct ArchiveEntry {
  std::string path;
  ByteBuffer data;
  uint32_t mode = 0644;
  bool is_dir = false;
};

// A ZIP read the way the format says to: from the central directory at the end,
// not by scanning for local headers, so an entry the directory does not list is
// not extracted. Stored and deflated entries; every one's CRC-32 is checked.
Result<std::vector<ArchiveEntry>> unzip(ByteSpan zip);

// Creates a standard PKZIP archive (deflated with net::deflate_raw or stored).
ByteBuffer zip_create(const std::vector<ArchiveEntry>& entries, int compression_level = 6);

uint32_t crc32(ByteSpan data);

}  // namespace aios::userland
