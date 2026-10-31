// The SQL engine's own helpers that more than one of its source files uses: sql_database.cpp and the files split out
// of it (sql_storage.cpp). Internal to the engine - nothing outside src/userland/sql_*.cpp includes this.
//
// Most are defined where they always were, in sql_database.cpp's file-local namespaces, and reach the other files
// through the thin sqle:: functions at that file's end, so calls inside sql_database.cpp still go straight to the
// file-local definition and compile as before. The few that are trivial (a constant, a byte loop) are defined here
// once and sql_database.cpp takes them with a using-declaration.
#pragma once
#include "aios/userland/sql_database.hpp"
#include "aios/core/bytes.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace aios::userland::sqle {

// Little-endian fixed-width integers in the store's files.
inline void store_put(uint8_t* at, uint64_t v, int bytes) { for (int k = 0; k < bytes; ++k) at[k] = (uint8_t)(v >> (8 * k)); }
inline uint64_t store_get(const uint8_t* at, int bytes) {
  uint64_t v = 0;
  for (int k = 0; k < bytes; ++k) v |= (uint64_t)at[k] << (8 * k);
  return v;
}

// The store gained per-column flags and NULL-bearing cells. A file that opens
// with this magic carries them; one that does not is the original layout and
// is read as all-non-null with bare columns, so an existing store still opens.
constexpr uint32_t kStoreMagic = 0x51534C32;  // "QSL2"
// QSL3 carries each table's database and the list of schemas that exist. QSL2
// did not, so every table came back in aios_db after a restart and the
// separation between two tenants only lasted until the process did.
constexpr uint32_t kStoreMagicV3 = 0x51534C33;  // "QSL3"

// A column store's segments: rows per segment, and the bytes a segment of w-bit codes takes.
constexpr size_t kSegRows = 2048;
constexpr int kAlpMaxExp = 18;   // the largest decimal exponent an ALP-encoded double column uses
inline size_t seg_bytes(unsigned w) { return (kSegRows * w + 7) / 8; }

// The store's file codec (LZ4, or deflate when dense) and its checksum.
uint64_t store_hash64(const uint8_t* p, size_t n);
bool store_is_packed(ByteSpan in);
void store_pack(const uint8_t* raw, size_t n, ByteBuffer& out, bool dense);
bool store_unpack(ByteSpan in, ByteBuffer& out, std::string* why, size_t* end = nullptr, bool* deflated = nullptr);
// Whether checkpoints may write a table's later writes as parts (AIOS_STORE_NO_OPS_PARTS=1 turns it off).
bool ops_parts_enabled();

// Column encoding helpers.
void zone_cut_bounds(SegmentZoneMap& zm);
uint8_t code_bits_for(uint64_t v);

// fn(0..n-1) spread over the machine's cores; returns when every call has.
void for_each_parallel(size_t n, const std::function<void(size_t)>& fn);

std::string lower_of(const std::string& s);
// The engine's own scratch tables ("\x01tmp..."), never saved or listed.
bool hidden_table_name(const std::string& n);

#if defined(__GLIBC__)
// glibc's view of the heap (AIOS_STORE_TRACE): bytes handed out and still held, and those in mmapped blocks.
size_t malloc_live_bytes();
size_t malloc_mmap_bytes();
#endif

}  // namespace aios::userland::sqle
