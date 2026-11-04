// The SQL engine's store on disk: the column-image format (format 4: MANIFEST, per-table generation files, parts of
// later writes), the row-per-cell formats before it, the view catalog, and where replication had got to. Split out of
// sql_database.cpp unchanged; the helpers it shares with that file are in sql_engine_shared.hpp.
#include "aios/userland/sql_database.hpp"
#include "aios/core/bytes.hpp"
#include "aios/hal/host.hpp"
#include "aios/hal/sync.hpp"
#include "aios/userland/asql_binlog.hpp"
#include "aios/userland/mysql_binlog.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "sql_engine_shared.hpp"

namespace aios::userland {
using namespace sqle;


// --- persistence ----------------------------------------------------------

// kStoreMagic, kStoreMagicV3: sql_engine_shared.hpp (load_tables, in sql_database.cpp, reads them too).

// ---------------------------------------------------------------------------
// Store format 4: the columns as they are encoded
//
// sql_db/tables/MANIFEST names one generation of per-table files
// (<hex key>.g<N>.tbl); each holds the table's columns in their encoded form -
// dictionary, bit-packed codes, frame-of-reference, RLE runs, FSST, text
// arenas, compressed blocks, NULL bitmap, zone maps - exactly as the column
// store keeps them in memory. Loading reads those structures back: nothing
// is re-parsed a row at a time, re-interned or re-compressed. The row-per-
// cell text format before it (schema_tables.db) made st's mirror spend 140 s
// rebuilding 22M rows and another 250 s re-compressing them on every start,
// peaking at 13.5 GB. A save writes every table under a new generation and
// then swaps the manifest; the previous generation stays complete until that
// swap, so a crash mid-save leaves a consistent set (the redo log covers the
// rest). A store in the old format loads the old way once and is converted
// by the next save.
// ---------------------------------------------------------------------------
namespace {
constexpr uint32_t kColStoreMagicV8 = 0x41515438; // "AQT8": a dictionary may be held in compressed blocks (DictArena::blocked)
constexpr uint32_t kColStoreMagicV7 = 0x41515437; // "AQT7": a codec byte per compressed block (LZ4 or deflate); a block column may carry an open tail
constexpr uint32_t kColStoreMagicV6 = 0x41515436; // "AQT6": compressed blocks keep lengths in the payload, compact FSST boundaries
constexpr uint32_t kColStoreMagicV5 = 0x41515435; // "AQT5": compact RLE runs
constexpr uint32_t kColStoreMagicV4 = 0x41515434; // "AQT4": AQT3 and each column's segment layout (SegmentFOR, ALP)
constexpr uint32_t kColStoreMagicV3 = 0x41515433; // "AQT3": zone maps keyed by general_ci weight
constexpr uint32_t kColStoreMagicV2 = 0x41515432; // "AQT2": keyed lower-cased, PAD SPACE
constexpr uint32_t kColStoreMagicV1 = 0x41515431; // "AQT1": keyed on lowered text
constexpr uint32_t kColStoreMagic = kColStoreMagicV8;
constexpr uint32_t kManifestMagic = 0x41514D31;   // "AQM1"
// After the columns: the table's secondary indices (UNIQUE ones enforced) and
// foreign keys. They were never written: a table kept them only while its
// CREATE was still in the redo log, and lost them - its UNIQUE keys with
// them - at the first restart after a checkpoint. Files without it load as
// before (no trailer, nothing to read).
constexpr uint32_t kIndexTrailerMagic = 0x41514958;   // "AQIX"
// Declared column types (varchar(20), char(3), ...): after the index trailer
// when there is one. An older build stops reading at an unknown tag, so a
// store written here still opens there (it reports the derived types).
constexpr uint32_t kDeclTypeTrailerMagic = 0x41514454;  // "AQDT"
// When the table was created (information_schema CREATE_TIME). A file written
// before it existed simply has none, and the table reports NULL.
constexpr uint32_t kCreateTimeTrailerMagic = 0x41514354;  // "AQCT"
constexpr uint32_t kRulesTrailerMagic = 0x41515255;       // "AQRU": generated columns and CHECK constraints
constexpr uint32_t kAutoIncTrailerMagic = 0x41514149;     // "AQAI": the AUTO_INCREMENT counter
constexpr uint32_t kCommentTrailerMagic = 0x4151434d;     // "AQCM": table/column/index comments, index prefix lengths and types
constexpr uint32_t kTextCodecTrailerMagic = 0x41515443;   // "AQTC": the TEXT_CODEC table option
// "AQTS", the image's last bytes: where each column starts (and the last ends), then the column count and this. A
// reader that knows it decodes a big table's columns at once, one per core; one that does not reads past it as a
// trailer it does not know. audittrail (31M rows, three columns) was a 2.2 s start on one core.
constexpr uint32_t kColumnSpansFooterMagic = 0x41515453;
// After the manifest's entries: where each entry's file validly ends (u64
// each, in entry order). A file is its packed image and then the parts
// appended to it; a part past this length is one a checkpoint wrote but
// never named (a crash before its manifest), and is not read. A loader from
// before parts stops at the entries and reads only whole images.
constexpr uint32_t kManifestLengthsMagic = 0x41514d4c;    // "AQML"
// After the lengths, a replica's binlog position as of this generation (the cdc_position text). Carried in the
// manifest so the two cannot disagree: written as a file after it, a crash between the two left the new generation
// with the old position, startup dropped the logs that would have moved it on, and replication applied again what
// the store already held - doubling rows in a table without a key. A manifest without it is read as before.
constexpr uint32_t kManifestCdcMagic = 0x41514d43;        // "AQMC"
// A part appended to a table file by a checkpoint whose only change to the
// table was appended rows (snapshot_tables_v4): the rows [from, to) as
// text, one length-prefixed cell after another, row-major, store_pack'ed.
//   u32 magic, u64 gen, u64 from, u64 to, u32 ncols, u64 payload bytes, u64 hash of the payload, payload
// The loader appends them to the table through the same path an INSERT
// takes (ColumnStore::push_back), so the columns come back encoded as they
// would be after those inserts.
constexpr uint32_t kPartMagic = 0x41514150;               // "AQAP"
// A part of a table's writes in their order (snapshot_tables_v4, SqlTable::ChangeJournal): rows erased, cells
// set, rows appended - the same header as kPartMagic (from and to the row counts before and after), its payload
// a list of ops: u8 'E', u64 n, n x u64 rows (ascending, numbered as they were) | u8 'S', u64 row, u32 col,
// str value | u8 'A', u64 n, n * ncols x str cells, row-major. The loader replays them in order. A loader from
// before these parts stops at the first one and does not load the table's writes after it: turn them off
// (AIOS_STORE_NO_OPS_PARTS=1) and let a checkpoint write the tables whole before going back to such a binary.
constexpr uint32_t kOpsPartMagic = 0x4151504f;            // "AQPO"
constexpr size_t kPartHeader = 4 + 8 + 8 + 8 + 4 + 8 + 8;
// Parts are appended while they stay a small share of the table; past this
// the next checkpoint writes the table whole again (the parts fold in).
constexpr size_t kPartMaxRowsFloor = 50000;    // appended rows a table may hold as parts, at least ...
constexpr uint32_t kPartMaxCount = 64;         // ... and at most this many parts
// ... and the file at most this much bigger than one plain copy of the table: parts bounded by rows alone let a
// table of a few large values, replaced again and again (a model's mesh blobs on every rebuild), grow its file by
// all of them each time - 10 rows were 38 MB for 5.8 MB live. Past it the table is written whole, the dead values
// left behind.
inline bool parts_within_bytes(uint64_t file_bytes, size_t plain_bytes) {
  return file_bytes <= (uint64_t)plain_bytes + std::max<uint64_t>((uint64_t)1 << 20, (uint64_t)plain_bytes / 4);
}
inline bool table_has_comments(const SqlTable& t) {
  if (!t.comment.empty()) return true;
  for (const auto& col : t.columns) if (!col.comment.empty()) return true;
  for (const auto& ix : t.indices) if (!ix.comment.empty() || !ix.sub_parts.empty() || (!ix.type.empty() && ix.type != "BTREE")) return true;
  return false;
}

template <typename T>
void put_pod_vec(Writer& w, const std::vector<T>& v) {
  w.bytes(ByteSpan(reinterpret_cast<const uint8_t*>(v.data()), v.size() * sizeof(T)));
}
template <typename T>
bool get_pod_vec(Reader& r, std::vector<T>& v) {
  ByteSpan b = r.bytes();
  if (!r.ok() || b.size() % sizeof(T) != 0) return false;
  v.resize(b.size() / sizeof(T));
  if (!b.empty()) std::memcpy(v.data(), b.data(), b.size());
  return true;
}
void put_str_vec(Writer& w, const std::vector<std::string>& v) {
  w.u64(v.size());
  for (const auto& x : v) w.str(x);
}
void put_str_vec(Writer& w, const DictArena& v) {
  w.u64(v.size());
  // Straight from the arena: a 100k-entry dictionary used to cost a string
  // allocation per entry and a buffer growth every doubling, under the lock.
  ByteBuffer& b = w.buf();
  size_t total = 0;
  for (size_t i = 0; i < v.size(); ++i) total += 8 + v.view(i).size();
  b.reserve(b.size() + total);
  for (size_t i = 0; i < v.size(); ++i) {
    const std::string_view sv = v.view(i);
    const uint64_t n = sv.size();
    uint8_t len[8];
    for (int k = 0; k < 8; ++k) len[k] = uint8_t(n >> (8 * k));
    b.insert(b.end(), len, len + 8);
    b.insert(b.end(), reinterpret_cast<const uint8_t*>(sv.data()), reinterpret_cast<const uint8_t*>(sv.data()) + sv.size());
  }
}
bool get_str_vec(Reader& r, DictArena& v) {
  uint64_t n = r.u64();
  if (!r.ok() || n > r.remaining()) return false;
  v.clear();
  v.reserve((size_t)n, 0);
  for (uint64_t i = 0; i < n && r.ok(); ++i) {
    ByteSpan b = r.bytes();
    if (!r.ok()) return false;
    v.push_back(std::string_view(reinterpret_cast<const char*>(b.data()), b.size()));
  }
  v.shrink_to_fit();
  return r.ok();
}
// A dictionary in AQT4: as put_str_vec while it is plain; a coded one is a
// marker count, its symbol table, the coded entries as stored, then its
// plain entries.
constexpr uint64_t kCodedDictMarker = ~0ull;
// AQT8: a blocked dictionary is this marker, the entries a block, the block count, each block (entries, raw bytes,
// codec, payload), then its plain entries.
constexpr uint64_t kBlockedDictMarker = ~0ull - 1;
void put_dict(Writer& w, const DictArena& d) {
  if (d.blocked()) {
    w.u64(kBlockedDictMarker);
    w.u32(d.per_block());
    w.u32((uint32_t)d.blocks().size());
    for (const DictArena::Block& b : d.blocks()) { w.u32(b.count); w.u32(b.raw_size); w.u8(b.codec); put_pod_vec(w, b.bytes); }
    w.u64(d.size() - d.coded_count());
    for (size_t k = d.coded_count(); k < d.size(); ++k) {
      const std::string_view v = d.view(k);
      w.bytes(ByteSpan(reinterpret_cast<const uint8_t*>(v.data()), v.size()));
    }
    return;
  }
  if (!d.coded()) { put_str_vec(w, d); return; }
  w.u64(kCodedDictMarker);
  const SymbolCoder& c = *d.coder();
  w.u32((uint32_t)c.len.size());
  for (size_t i = 0; i < c.len.size(); ++i) { w.u8(c.len[i]); w.u64(c.sym[i]); }
  put_pod_vec(w, d.coded_bytes());
  put_pod_vec(w, d.coded_offsets());
  w.u64(d.size() - d.coded_count());
  for (size_t k = d.coded_count(); k < d.size(); ++k) {
    const std::string_view v = d.view(k);
    w.bytes(ByteSpan(reinterpret_cast<const uint8_t*>(v.data()), v.size()));
  }
}
bool get_dict(Reader& r, DictArena& v, bool aqt4) {
  if (!aqt4) return get_str_vec(r, v);
  uint64_t n = r.u64();
  if (!r.ok()) return false;
  v.clear();
  if (n == kBlockedDictMarker) {
    const uint32_t per = r.u32();
    const uint32_t nb = r.u32();
    if (!r.ok() || per == 0 || nb > r.remaining()) return false;
    std::vector<DictArena::Block> blocks(nb);
    for (uint32_t i = 0; i < nb; ++i) {
      blocks[i].count = r.u32();
      blocks[i].raw_size = r.u32();
      blocks[i].codec = r.u8();
      if (!r.ok() || !get_pod_vec(r, blocks[i].bytes)) return false;
    }
    if (!v.set_blocked(per, std::move(blocks))) return false;
    n = r.u64();
  } else if (n == kCodedDictMarker) {
    const uint32_t nsym = r.u32();
    if (!r.ok() || nsym > 255) return false;
    auto c = std::make_shared<SymbolCoder>();
    for (uint32_t i = 0; i < nsym; ++i) {
      const uint8_t l = r.u8();
      const uint64_t bytes = r.u64();
      if (!r.ok() || l < 1 || l > 8 || (l < 8 && (bytes >> (8 * l)))) return false;
      c->len.push_back(l);
      c->sym.push_back(bytes);
    }
    c->index();
    std::vector<uint8_t> cb;
    std::vector<uint32_t> co;
    if (!get_pod_vec(r, cb) || !get_pod_vec(r, co)) return false;
    if (!v.set_coded(std::move(c), std::move(cb), std::move(co))) return false;
    n = r.u64();
  }
  if (!r.ok() || n > r.remaining()) return false;
  // The plain entries' bytes counted first (each is a length and its bytes), so the arena is sized once: grown
  // entry by entry it was copied over and over - 0.8 s of a 3.9M-entry dictionary's load.
  size_t total = 0, run = 0;
  {
    Reader peek(r.rest());
    for (uint64_t i = 0; i < n && peek.ok(); ++i) total += peek.bytes().size();
    if (!peek.ok() || total > UINT32_MAX) return false;
    run = peek.position();
  }
  const ByteSpan stored = r.raw(run);
  if (!r.ok()) return false;
  v.reserve(v.coded_count() + (size_t)n, total);
  v.push_back_stored(stored.data(), n, total);
  v.shrink_to_fit();
  return r.ok();
}
bool get_str_vec(Reader& r, std::vector<std::string>& v) {
  uint64_t n = r.u64();
  if (!r.ok() || n > r.remaining()) return false;
  v.clear(); v.reserve((size_t)n);
  for (uint64_t i = 0; i < n && r.ok(); ++i) v.push_back(r.str());
  return r.ok();
}

void put_column(Writer& w, const ColumnStore& c) {
  w.u8((uint8_t)c.encoding);
  w.u64(c.row_count_);
  w.u8((uint8_t)(c.bit_width_ | (c.seg_packed_ ? 0x80 : 0)));   // widths are 0..32: the top bit marks the per-segment layout
  put_pod_vec(w, c.null_bitmap);
  w.u64(c.zone_maps.size());
  for (size_t zi = 0; zi < c.zone_maps.size(); ++zi) {
    const auto& z = c.zone_maps[zi];
    w.u64((uint64_t)z.min_int64); w.u64((uint64_t)z.max_int64);
    w.f64(z.min_double); w.f64(z.max_double);
    w.str(z.min_str); w.str(z.max_str);
    // a loose segment's bounds are not exact: read back as having no stats
    w.u32(z.null_count); w.u8(z.all_null ? 1 : 0); w.u8(z.has_stats && !c.zone_loose(zi) ? 1 : 0);
    w.u64(z.bloom_filter);
    w.u8(z.str_cut ? 1 : 0);   // AQT4
  }
  w.str(c.constant_val);
  w.u64((uint64_t)c.base_int64);
  w.f64(c.scale_factor); w.f64(c.inv_scale_factor);
  w.u32(c.rle_num_runs);
  w.u8(c.rle_code_bits);
  put_pod_vec(w, c.rle_codes_data);
  put_pod_vec(w, c.rle_run_lengths);
  put_pod_vec(w, c.rle_escape_runs);
  put_pod_vec(w, c.rle_escape_lengths);
  put_pod_vec(w, c.rle_seg_base_rows);
  w.u8(c.fsst_table.trained ? 1 : 0);
  put_str_vec(w, c.fsst_table.symbols);
  put_pod_vec(w, c.fsst_data); put_pod_vec(w, c.fsst_offsets); put_pod_vec(w, c.fsst_off_base); put_pod_vec(w, c.fsst_off_rel);
  put_pod_vec(w, c.packed_data); put_pod_vec(w, c.data32);
  if (c.seg_packed_) { put_pod_vec(w, c.seg_hdr); put_pod_vec(w, c.seg_tab); }
  put_dict(w, c.shared_dict ? c.shared_dict->dict : c.dict);
  put_pod_vec(w, c.int32_data); put_pod_vec(w, c.int64_data); put_pod_vec(w, c.double_data);
  put_pod_vec(w, c.text_arena); put_pod_vec(w, c.arena_offsets);
  w.u64(c.compressed_blocks.size());
  for (const auto& b : c.compressed_blocks) {
    w.u64(b.start_row); w.u64(b.num_rows);
    put_pod_vec(w, b.compressed_bytes); put_pod_vec(w, b.offsets); w.u32(b.raw_size);
    w.u8(b.codec);   // AQT7
  }
  // AQT4
  put_pod_vec(w, c.seg_base); put_pod_vec(w, c.seg_bits); put_pod_vec(w, c.seg_byte_at);
  put_pod_vec(w, c.alp_exp); put_pod_vec(w, c.alp_exc_rows); put_pod_vec(w, c.alp_exc_vals);
}

// A SegmentFOR / ALP column read from a file is checked before any of it is
// trusted: every segment's offsets inside packed_data, widths at most 64,
// exponents in range, exceptions in rows that exist.
bool seg_layout_sound(const ColumnStore& c) {
  const size_t segs = (c.row_count_ + kSegRows - 1) / kSegRows;
  if (c.seg_base.size() != segs || c.seg_bits.size() != segs || c.seg_byte_at.size() != segs + 1) return false;
  if (c.seg_byte_at[0] != 0 || c.packed_data.size() < c.seg_byte_at.back() + 9) return false;
  for (size_t s = 0; s < segs; ++s) {
    if (c.seg_bits[s] > 64 || c.seg_byte_at[s + 1] < c.seg_byte_at[s] ||
        c.seg_byte_at[s + 1] - c.seg_byte_at[s] < seg_bytes(c.seg_bits[s]))
      return false;
  }
  if (c.encoding == ColumnEncoding::ALP) {
    if (c.alp_exp.size() != segs || c.alp_exc_rows.size() != c.alp_exc_vals.size()) return false;
    for (uint8_t e : c.alp_exp) if ((e & 0x7f) > kAlpMaxExp) return false;
    for (size_t i = 0; i < c.alp_exc_rows.size(); ++i)
      if (c.alp_exc_rows[i] >= c.row_count_ || (i && c.alp_exc_rows[i] <= c.alp_exc_rows[i - 1])) return false;
  }
  return true;
}

bool get_column(Reader& r, ColumnStore& c, uint32_t magic = kColStoreMagic) {
  const bool aqt4 = magic >= kColStoreMagicV4;   // the segment layout of AQT4 (SegmentFOR, ALP); AQT5 adds compact RLE runs
  c = ColumnStore();
  c.encoding = (ColumnEncoding)r.u8();
  c.row_count_ = (size_t)r.u64();
  const uint8_t width_byte = r.u8();
  c.bit_width_ = width_byte & 0x7f;
  const bool seg_layout = (width_byte & 0x80) != 0;
  if (!get_pod_vec(r, c.null_bitmap)) return false;
  uint64_t nz = r.u64();
  if (!r.ok() || nz > r.remaining()) return false;
  c.zone_maps.resize((size_t)nz);
  for (auto& z : c.zone_maps) {
    z.min_int64 = (int64_t)r.u64(); z.max_int64 = (int64_t)r.u64();
    z.min_double = r.f64(); z.max_double = r.f64();
    z.min_str = r.str(); z.max_str = r.str();
    z.null_count = r.u32(); z.all_null = r.u8() != 0; z.has_stats = r.u8() != 0;
    z.bloom_filter = r.u64();
    if (aqt4) z.str_cut = r.u8() != 0;
    zone_cut_bounds(z);   // bounds from before they were cut short, cut now
  }
  c.constant_val = r.str();
  c.base_int64 = (int64_t)r.u64();
  c.scale_factor = r.f64(); c.inv_scale_factor = r.f64();
  if (magic >= kColStoreMagicV5) {
    c.rle_num_runs = r.u32();
    c.rle_code_bits = r.u8();
    if (!get_pod_vec(r, c.rle_codes_data) || !get_pod_vec(r, c.rle_run_lengths) ||
        !get_pod_vec(r, c.rle_escape_runs) || !get_pod_vec(r, c.rle_escape_lengths) ||
        !get_pod_vec(r, c.rle_seg_base_rows)) return false;
  } else {
    std::vector<uint32_t> old_codes, old_ends;
    if (!get_pod_vec(r, old_codes) || !get_pod_vec(r, old_ends)) return false;
    if (c.encoding == ColumnEncoding::RLE && !old_codes.empty() && old_codes.size() == old_ends.size()) {
      c.rle_num_runs = static_cast<uint32_t>(old_codes.size());
      uint32_t max_code = 0;
      for (uint32_t code : old_codes) if (code > max_code) max_code = code;
      c.rle_code_bits = code_bits_for(max_code);
      if (c.rle_code_bits == 0) c.rle_code_bits = 1;
      c.rle_codes_data.assign(((c.rle_num_runs * c.rle_code_bits + 7) >> 3) + 8, 0);
      c.rle_run_lengths.resize(c.rle_num_runs);
      c.rle_escape_runs.clear();
      c.rle_escape_lengths.clear();
      c.rle_seg_base_rows.clear();
      c.rle_seg_base_rows.reserve((c.rle_num_runs + 31) / 32);
      uint32_t prev_end = 0;
      for (size_t i = 0; i < c.rle_num_runs; ++i) {
        if ((i & 31) == 0) c.rle_seg_base_rows.push_back(prev_end);
        c.rle_set_code(i, old_codes[i]);
        uint32_t len = (old_ends[i] >= prev_end) ? (old_ends[i] - prev_end) : 0;
        if (len >= 0xFFFF) {
          c.rle_run_lengths[i] = 0xFFFF;
          c.rle_escape_runs.push_back(static_cast<uint32_t>(i));
          c.rle_escape_lengths.push_back(len);
        } else {
          c.rle_run_lengths[i] = static_cast<uint16_t>(len);
        }
        prev_end = old_ends[i];
      }
    }
  }
  c.fsst_table.trained = r.u8() != 0;
  if (!get_str_vec(r, c.fsst_table.symbols)) return false;
  if (!get_pod_vec(r, c.fsst_data) || !get_pod_vec(r, c.fsst_offsets)) return false;   // (the symbol index is built when an append needs it)
  if (magic >= kColStoreMagicV6) {
    if (!get_pod_vec(r, c.fsst_off_base) || !get_pod_vec(r, c.fsst_off_rel)) return false;
    // exactly one form, and the compact one's bases cover its boundaries
    if (!c.fsst_offsets.empty() && !c.fsst_off_rel.empty()) return false;
    if (c.fsst_off_base.size() != (c.fsst_off_rel.size() + 15) >> 4) return false;
  }
  if (!get_pod_vec(r, c.packed_data) || !get_pod_vec(r, c.data32)) return false;
  c.trim_nulls();   // a store written before bitmaps ended at their last NULL
  if (!seg_layout && c.bit_width_ > 0 && c.bit_width_ < 32) {
    const size_t needed = ((c.row_count_ * c.bit_width_ + 7) >> 3) + 8;
    if (c.packed_data.size() < needed) c.packed_data.resize(needed, 0);
  }
  if (seg_layout) {
    if (!get_pod_vec(r, c.seg_hdr) || !get_pod_vec(r, c.seg_tab)) return false;
    // Every field a reader will touch must lie inside the buffers it was loaded with.
    constexpr size_t kSeg = 2048;
    if (c.encoding != ColumnEncoding::DictBitPacked || c.seg_hdr.size() != (c.row_count_ + kSeg - 1) / kSeg ||
        c.packed_data.size() < 8)
      return false;
    for (size_t sg = 0; sg < c.seg_hdr.size(); ++sg) {
      const auto& h = c.seg_hdr[sg];
      const size_t n = std::min(kSeg, c.row_count_ - sg * kSeg);
      if (h.width > 32 || h.mode > 1) return false;
      if ((size_t)h.byte_off + ((n * h.width + 7) >> 3) + 8 > c.packed_data.size()) return false;
      if (h.mode == 1 && ((size_t)h.tab_off + ((size_t)1 << std::min<unsigned>(h.width, 16)) > c.seg_tab.size() || h.width > 8)) return false;
    }
    c.seg_packed_ = true;
  }
  if (!get_dict(r, c.dict, aqt4)) return false;
  if (c.encoding == ColumnEncoding::DictBitPacked && c.dict.size() > 1 && !c.dict.is_front_coded() && !c.dict.coded()) {
    c.dict.front_code(nullptr, 16);   // in the entries' own order (the codes are saved against it); is_sorted() checks whether that is sorted
  }
  // id_index (value -> code) is only for writes: built by intern() when a
  // write first needs it, not held for every read-only dictionary.
  c.id_index.clear();
  c.id_index_count = 0;
  if (!get_pod_vec(r, c.int32_data) || !get_pod_vec(r, c.int64_data) || !get_pod_vec(r, c.double_data)) return false;
  if (!get_pod_vec(r, c.text_arena) || !get_pod_vec(r, c.arena_offsets)) return false;
  uint64_t nb = r.u64();
  if (!r.ok() || nb > r.remaining()) return false;
  c.compressed_blocks.resize((size_t)nb);
  for (auto& b : c.compressed_blocks) {
    b.start_row = (size_t)r.u64(); b.num_rows = (size_t)r.u64();
    if (!get_pod_vec(r, b.compressed_bytes) || !get_pod_vec(r, b.offsets)) return false;
    if (magic >= kColStoreMagicV6) b.raw_size = r.u32();
    else b.raw_size = b.offsets.empty() ? 0 : b.offsets.back();
    b.codec = magic >= kColStoreMagicV7 ? r.u8() : (uint8_t)ColumnStore::kBlockLz4;
    if (b.codec > ColumnStore::kBlockDeflate) return false;
  }
  if (c.encoding == ColumnEncoding::BlockCompressed) {
    // The blocks cover the rows from the first, in order; what is left is the open tail (AQT7), in the arena.
    size_t at = 0;
    for (const auto& b : c.compressed_blocks) { if (b.start_row != at) return false; at += b.num_rows; }
    if (at > c.row_count_) return false;
    const size_t tail = c.row_count_ - at;
    if (tail == 0 ? !c.arena_offsets.empty() : c.arena_offsets.size() != tail + 1) return false;
    if (!c.arena_offsets.empty() && (c.arena_offsets.front() != 0 || c.arena_offsets.back() != c.text_arena.size())) return false;
    for (size_t i = 1; i < c.arena_offsets.size(); ++i) if (c.arena_offsets[i] < c.arena_offsets[i - 1]) return false;
  }
  if (aqt4) {
    if (!get_pod_vec(r, c.seg_base) || !get_pod_vec(r, c.seg_bits) || !get_pod_vec(r, c.seg_byte_at)) return false;
    if (!get_pod_vec(r, c.alp_exp) || !get_pod_vec(r, c.alp_exc_rows) || !get_pod_vec(r, c.alp_exc_vals)) return false;
  }
  if (c.encoding == ColumnEncoding::SegmentFOR || c.encoding == ColumnEncoding::ALP) {
    if (!aqt4 || !seg_layout_sound(c)) return false;
  } else if (c.encoding == ColumnEncoding::TemporalDays || c.encoding == ColumnEncoding::TemporalSeconds) {
    // Days or seconds since base_int64, bit-packed like FOR (data32 at 32
    // bits). The reader refused these two as "unknown" while the writer
    // produced them: every table with a date column converted at a
    // checkpoint came back as "damaged column data" at the next start.
    if (c.bit_width_ < 1 || c.bit_width_ > 32) return false;
    if (c.bit_width_ == 32 ? c.data32.size() < c.row_count_
                           : c.packed_data.size() < ((c.row_count_ * c.bit_width_ + 7) >> 3) + 8)
      return false;
  } else if ((uint8_t)c.encoding > (uint8_t)ColumnEncoding::TemporalSeconds) {
    return false;
  }
  return r.ok();
}

std::string hex_of(const std::string& s) {
  static const char* hx = "0123456789abcdef";
  std::string o;
  for (unsigned char ch : s) { o.push_back(hx[ch >> 4]); o.push_back(hx[ch & 15]); }
  return o;
}
}  // namespace

// Columns, types and scales: a schema change rewrites the table file even
// when no row moved.
uint64_t table_shape_signature(const SqlTable& t) {
  std::string sig = t.name + '\x1f' + t.database;
  for (const auto& c : t.columns) {
    sig += '\x1e';
    sig += c.name;
    sig += (char)c.type; sig += (char)c.precision; sig += (char)c.scale;
    sig += (char)(c.is_primary_key | (c.not_null << 1) | (c.auto_increment << 2) | (c.binary << 3) | (c.temporal << 5));
    sig += c.default_value;
    sig += '\x1f'; sig += c.declared;
  }
  sig += '\x1d';
  sig += std::to_string(t.col_data.size());
  // Indices and foreign keys are part of the file: adding one is a change to save.
  if (!t.indices.empty() || !t.foreign_keys.empty()) {
    sig += '\x1c';
    for (const auto& ix : t.indices) {
      sig += ix.name; sig += (char)ix.is_unique; sig += ix.type;
      for (const auto& c : ix.columns) { sig += '\x1f'; sig += c; }
      sig += '\x1e';
    }
    for (const auto& fk : t.foreign_keys)
      sig += fk.constraint_name + '\x1f' + fk.column + '\x1f' + fk.ref_table + '\x1f' + fk.ref_column + '\x1f' + fk.on_delete + '\x1f' + fk.on_update + '\x1e';
  }
  if (t.has_rules()) {
    sig += '\x1b';
    for (const auto& c : t.columns) { sig += c.generated; sig += (char)('0' + c.generated_virtual); sig += '\x1f'; }
    for (const auto& ck : t.checks) { sig += ck.name; sig += '\x1f'; sig += ck.expr; sig += (char)('0' + ck.column_level); sig += '\x1e'; }
  }
  if (t.auto_increment_next > 0) { sig += '\x1a'; sig += std::to_string(t.auto_increment_next); }
  if (table_has_comments(t)) {
    sig += '\x19'; sig += t.comment;
    for (const auto& col : t.columns) { sig += '\x1f'; sig += col.comment; }
    for (const auto& ix : t.indices) { sig += '\x1e'; sig += ix.comment; sig += ix.type; for (uint16_t sp : ix.sub_parts) sig += std::to_string(sp) + ","; }
  }
  uint64_t h = 1469598103934665603ull;
  for (unsigned char ch : sig) { h ^= ch; h *= 1099511628211ull; }
  return h;
}

ColumnStore ColumnStore::save_copy() const {
  ColumnStore c;
  save_copy_into(c);
  return c;
}

void ColumnStore::save_copy_into(ColumnStore& c) const {
  c.encoding = encoding; c.row_count_ = row_count_; c.bit_width_ = bit_width_;
  c.seg_packed_ = seg_packed_; c.seg_hdr = seg_hdr; c.seg_tab = seg_tab;
  c.null_bitmap = null_bitmap;
  c.zone_maps = zone_maps;
  c.constant_val = constant_val;
  c.base_int64 = base_int64; c.scale_factor = scale_factor; c.inv_scale_factor = inv_scale_factor;
  c.rle_num_runs = rle_num_runs; c.rle_code_bits = rle_code_bits;
  c.rle_codes_data = rle_codes_data; c.rle_run_lengths = rle_run_lengths;
  c.rle_escape_runs = rle_escape_runs; c.rle_escape_lengths = rle_escape_lengths;
  c.rle_seg_base_rows = rle_seg_base_rows;
  c.fsst_table = fsst_table; c.fsst_data = fsst_data; c.fsst_offsets = fsst_offsets; c.fsst_off_base = fsst_off_base; c.fsst_off_rel = fsst_off_rel;
  c.packed_data = packed_data; c.data32 = data32;
  (shared_dict ? shared_dict->dict : dict).copy_into(c.dict);
  c.int32_data = int32_data; c.int64_data = int64_data; c.double_data = double_data;
  c.text_arena = text_arena; c.arena_offsets = arena_offsets;
  c.compressed_blocks = compressed_blocks;
  c.seg_base = seg_base; c.seg_bits = seg_bits; c.seg_byte_at = seg_byte_at;
  c.alp_exp = alp_exp; c.alp_exc_rows = alp_exc_rows; c.alp_exc_vals = alp_exc_vals;
}

size_t SqlDatabase::checkpoint_pool_cap() {
  static const size_t v = [] {
    const char* e = std::getenv("AIOS_CHECKPOINT_POOL_MB");
    return e && *e ? ((size_t)std::strtoull(e, nullptr, 10) << 20) : 0u;
  }();
  return v;
}

Result<void> SqlDatabase::save_tables_v4() {
  CheckpointImage img;
  auto r = snapshot_tables_v4(img);
  if (!r.ok()) return r;
  r = write_checkpoint(img);
  if (r.ok()) save_index_list();
  return r;
}

// The snapshot: every changed table serialized into memory and the manifest
// that names this generation. Under the exclusive statement lock; touches
// no file.
Result<void> SqlDatabase::snapshot_tables_v4(CheckpointImage& img) {
  const std::string dir = root_path_ + "/tables";
  const uint64_t gen = std::max(store_generation_, checkpoint_seq_) + 1;
  checkpoint_seq_ = gen;
  img.gen = gen;
  auto& entries = img.entries;
  auto& saved_now = img.saved;
  size_t& rewritten = img.rewritten;
  const uint64_t now_ns = kernel_.host().monotonic_ns();
  static const uint64_t cold_ns = [] {
    const char* e = std::getenv("AIOS_STORE_COLD_SECS");
    return (uint64_t)(e && *e ? std::max(0, std::atoi(e)) : 120) * 1000000000ull;
  }();
  size_t dense_budget = (size_t)256 << 20;   // bytes of table files deflated per checkpoint
  // Tables whose form changed and nothing else, rewritten whole under the lock this checkpoint, at most this many
  // bytes (AIOS_STORE_FORM_MB, 16 by default); the rest keep their file and wait for a later one.
  static const size_t form_budget_total = [] {
    const char* e = std::getenv("AIOS_STORE_FORM_MB");
    return (size_t)(e && *e ? std::max(0, std::atoi(e)) : 16) << 20;
  }();
  size_t form_budget = form_budget_total;
  static const bool snap_trace = std::getenv("AIOS_STORE_TRACE") != nullptr;
  uint64_t snap_small_ns = 0;   // tables under 5 ms each, added up (AIOS_STORE_TRACE)
  const uint64_t snap_loop_t0 = snap_trace ? kernel_.host().monotonic_ns() : 0;
  for (auto& kv : tables_) {
    SqlTable& t = kv.second;
    // AIOS_STORE_TRACE: a table that held the lock 5 ms or more here, and how much of it was folding its delta in -
    // whichever way below it leaves by (unchanged, a part, appended rows, whole).
    struct SnapTime {
      hal::Host& h; const std::string& name; uint64_t t0; uint64_t& small; uint64_t fold = 0;
      ~SnapTime() {
        if (!snap_trace) return;
        const uint64_t ns = h.monotonic_ns() - t0;
        if (ns >= 5000000ull) fprintf(stderr, "[store] snapshot %s: %.1f ms under the lock (delta fold %.1f)\n", name.c_str(), ns / 1e6, fold / 1e6);
        else small += ns;
      }
    } snap_time{kernel_.host(), kv.first, snap_trace ? kernel_.host().monotonic_ns() : 0, snap_small_ns};
    t.compact_delta_buffer();
    if (snap_trace) snap_time.fold = kernel_.host().monotonic_ns() - snap_time.t0;
    t.ensure_columns();
    // Unchanged since the file already on disk: the new manifest names that
    // file again. A checkpoint used to rewrite all 8 GB of st's store every
    // 2,000 replicated statements (~6.5 minutes) for a few changed rows.
    SavedTableFile state{std::string(), t.mutation_seq, t.row_count(), table_shape_signature(t)};
    state.still_ns = now_ns;
    auto prev = saved_tables_.find(kv.first);
    bool dense = false;
    // Form only: the counter moved and nothing in the table changed - a column re-encoded, a dictionary compacted,
    // segments packed (stamp_form_change), the journal running on from the file without a write in it. The file still
    // holds exactly these rows in this order, so it can stand; the new form is only smaller or faster to load. Written
    // whole under the lock it was every such table at once: 433 files and 2-11 s of every read waiting on a copy of
    // the Cleve mirror after a restart. Now within a budget a checkpoint; the rest keep their file, form pending.
    bool force_whole = false;
    if (prev != saved_tables_.end() && prev->second.shape == state.shape && prev->second.rows == state.rows &&
        !unpacked_tables_.count(kv.first)) {
      bool form_only = prev->second.seq == state.seq && prev->second.form_pending;
      if (prev->second.seq != state.seq) {
        std::lock_guard<aios::hal::Mutex> jl(*t.journal_mu);
        const SqlTable::ChangeJournal& j = t.journal;
        form_only = j.on && !j.broken && j.start_seq == prev->second.seq && j.last_seq == t.mutation_seq && j.ops.empty();
      }
      if (form_only) {
        const size_t cost = std::max<size_t>(prev->second.bytes, 1);
        if (exit_flush_ || form_budget >= cost) {
          form_budget -= std::min(form_budget, cost);
          force_whole = true;
        } else {
          entries.emplace_back(kv.first, prev->second.file);
          SavedTableFile kept = prev->second;
          kept.seq = state.seq;   // the file is the table as it stands; its journal goes on from here
          kept.form_pending = true;
          saved_now[kv.first] = kept;
          continue;
        }
      }
    }
    if (!force_whole && prev != saved_tables_.end() && prev->second.seq == state.seq && prev->second.rows == state.rows &&
        prev->second.shape == state.shape && kernel_.host().exists(dir + "/" + prev->second.file) &&
        !(prev->second.op_parts && !ops_parts_enabled())) {   // parts of writes, with them turned off: written whole
      // Unchanged. Written in the fast form while it was changing; once it
      // has been still for a while, once more deflated, a few tables a
      // checkpoint. Deflating every checkpoint of the tables being written
      // was 38% of the server's CPU under sysbench write_only, on the cores
      // the statements needed.
      const uint64_t still = prev->second.still_ns ? prev->second.still_ns : now_ns;
      if (prev->second.dense || now_ns - still < cold_ns || dense_budget == 0 || exit_flush_) {
        entries.emplace_back(kv.first, prev->second.file);
        saved_now[kv.first] = prev->second;
        if (!prev->second.still_ns) saved_now[kv.first].still_ns = now_ns;
        continue;
      }
      dense = true;
      state.still_ns = still;
      dense_budget -= std::min(dense_budget, std::max<size_t>(prev->second.bytes, 1));
      // Nothing has changed since its file was written, so the file is the table: deflated from it by the writer
      // thread, off the lock. Its columns were copied under the lock to be serialized again (atl's mirror: 3 s of
      // every read waiting at one checkpoint, a still table at a time, 15-60 ms each). A file with parts on its end
      // is rewritten from the columns below, the parts folding into it.
      if (prev->second.parts == 0 && !unpacked_tables_.count(kv.first)) {
        const std::string file = hex_of(kv.first) + ".g" + std::to_string(gen) + ".tbl";
        CheckpointImage::TableImage ti;
        ti.key = kv.first;
        ti.file = file;
        ti.dense = true;
        ti.repack_from = prev->second.file;
        img.tables.push_back(std::move(ti));
        entries.emplace_back(kv.first, file);
        SavedTableFile kept = prev->second;
        kept.file = file;
        kept.dense = true;
        kept.still_ns = still;
        saved_now[kv.first] = kept;
        continue;
      }
    }
    state.layout = t.layout_seq;
    // Rows erased or cells set since the file was written, every one of them in the table's journal: the writes go
    // to the file as a part, in their order, instead of the table whole. A 30M-row audit log with a few thousand
    // rows deleted was copied whole under the lock at every checkpoint (1.5 s of every read waiting, and the copy
    // tripled the process's memory). The same bounds as appended rows; past them it is written whole again.
    if (!force_whole && prev != saved_tables_.end() && !dense && !unpacked_tables_.count(kv.first) && ops_parts_enabled() &&
        prev->second.seq != state.seq && prev->second.shape == state.shape && prev->second.file_bytes > 0 &&
        prev->second.parts < kPartMaxCount && parts_within_bytes(prev->second.file_bytes, prev->second.bytes)) {
      std::lock_guard<aios::hal::Mutex> jl(*t.journal_mu);
      const SqlTable::ChangeJournal& j = t.journal;
      const size_t ncols = t.col_data.size();
      bool usable = j.on && !j.broken && j.start_seq == prev->second.seq && j.last_seq == t.mutation_seq && !j.ops.empty() && ncols > 0;
      bool writes = false;   // erased rows or set cells: appends alone take the append path below
      size_t rows = prev->second.rows, appended = 0;
      for (const auto& op : j.ops) {
        if (!usable) break;
        if (op.kind == 'E') {
          writes = true;
          size_t gone = 0;
          for (uint64_t r : op.rows) if (r < rows) ++gone;
          rows -= gone;
        } else if (op.kind == 'S') {
          writes = true;
          if (op.row >= rows || op.col >= ncols) usable = false;
        } else if (op.kind == 'A') {
          if (op.cells.size() % ncols) { usable = false; break; }
          rows += op.cells.size() / ncols;
          appended += op.cells.size() / ncols;
        } else {
          usable = false;
        }
      }
      const size_t bound = std::max(kPartMaxRowsFloor, prev->second.base_rows / 4);
      usable = usable && writes && rows == state.rows && prev->second.part_rows + appended <= bound &&
               prev->second.op_cells + j.cells <= bound;
      if (usable) {
        auto st = kernel_.host().stat(dir + "/" + prev->second.file);
        if (st.ok() && (uint64_t)st->size == prev->second.file_bytes) {
          CheckpointImage::TableImage ti;
          ti.key = kv.first;
          ti.file = prev->second.file;
          ti.dense = prev->second.dense;
          ti.ops = true;
          ti.rows_from = prev->second.rows;
          ti.rows_to = state.rows;
          ti.ncols = ncols;
          Writer w;
          for (const auto& op : j.ops) {
            w.u8(op.kind);
            if (op.kind == 'E') { w.u64(op.rows.size()); for (uint64_t r : op.rows) w.u64(r); }
            else if (op.kind == 'S') { w.u64(op.row); w.u32(op.col); w.str(op.val); }
            else { w.u64(op.cells.size() / ncols); for (const auto& c : op.cells) w.str(c); }
          }
          ti.ops_plain = w.take();
          img.bytes += ti.ops_plain.size();
          state.file = prev->second.file;
          state.dense = prev->second.dense;
          state.bytes = prev->second.bytes;
          state.base_rows = prev->second.base_rows;
          state.part_rows = prev->second.part_rows + appended;
          state.parts = prev->second.parts + 1;
          state.op_parts = prev->second.op_parts + 1;
          state.op_cells = prev->second.op_cells + j.cells;
          state.file_bytes = prev->second.file_bytes;   // write_checkpoint adds the part's bytes
          static const bool store_trace_o = std::getenv("AIOS_STORE_TRACE") != nullptr;
          if (store_trace_o) fprintf(stderr, "[store] %s: %zu write(s) (%zu row(s) erased or cell(s) set, %zu appended) added to %s as part %u\n", t.name.c_str(), j.ops.size(), j.cells, appended, ti.file.c_str(), state.parts);
          img.tables.push_back(std::move(ti));
          entries.emplace_back(kv.first, state.file);
          saved_now[kv.first] = state;
          continue;
        }
      }
    }
    // Only appended since the file was written (its layout stands, no row
    // it holds changed, rows only grew, same shape): the new rows go to the
    // file as a part. An append-only table (an audit log) was re-encoded and
    // rewritten whole - 2 GB of it - at every checkpoint that added rows.
    // The parts stay a bounded share of the table (kPartMaxRowsFloor,
    // kPartMaxCount, a quarter of the base rows); past that, or when a row
    // already stored changes, the table is written whole and the parts fold
    // in. A file whose size is not what the last manifest recorded (a part
    // a crash left unnamed) is written whole too, never appended behind.
    // The layout also moves when an append or maintenance re-encodes a column (a dictionary grown past its code
    // width): the rows stored keep their values, and the journal says so when it recorded nothing but appends since
    // the file. An audit log taking a new value in a few appended rows was written whole for it (320 MB).
    auto only_appended = [&] {
      std::lock_guard<aios::hal::Mutex> jl(*t.journal_mu);
      const SqlTable::ChangeJournal& j = t.journal;
      if (!j.on || j.broken || j.start_seq != prev->second.seq || j.last_seq != t.mutation_seq || j.ops.empty()) return false;
      for (const auto& op : j.ops) if (op.kind != 'A') return false;
      return true;
    };
    if (!force_whole && prev != saved_tables_.end() && !dense && !unpacked_tables_.count(kv.first) &&
        prev->second.seq != state.seq && state.rows > prev->second.rows && prev->second.shape == state.shape &&
        (prev->second.layout == t.layout_seq || only_appended()) &&
        prev->second.file_bytes > 0 && prev->second.parts < kPartMaxCount && parts_within_bytes(prev->second.file_bytes, prev->second.bytes) &&
        prev->second.part_rows + (state.rows - prev->second.rows) <= std::max(kPartMaxRowsFloor, prev->second.base_rows / 4)) {
      auto st = kernel_.host().stat(dir + "/" + prev->second.file);
      if (st.ok() && (uint64_t)st->size == prev->second.file_bytes) {
        CheckpointImage::TableImage ti;
        ti.key = kv.first;
        ti.file = prev->second.file;
        ti.dense = prev->second.dense;
        ti.append = true;
        ti.rows_from = prev->second.rows;
        ti.rows_to = state.rows;
        ti.ncols = t.col_data.size();
        ti.part_cells.reserve((ti.rows_to - ti.rows_from) * ti.ncols);
        for (size_t r = ti.rows_from; r < ti.rows_to; ++r)
          for (size_t c = 0; c < ti.ncols; ++c) ti.part_cells.push_back(t.col_data[c].get(r));
        for (const auto& cell : ti.part_cells) img.bytes += cell.size();
        state.file = prev->second.file;
        state.dense = prev->second.dense;
        state.bytes = prev->second.bytes;
        state.base_rows = prev->second.base_rows;
        state.part_rows = prev->second.part_rows + (ti.rows_to - ti.rows_from);
        state.parts = prev->second.parts + 1;
        state.op_parts = prev->second.op_parts;
        state.op_cells = prev->second.op_cells;
        state.file_bytes = prev->second.file_bytes;   // write_checkpoint adds the part's bytes
        static const bool store_trace_p = std::getenv("AIOS_STORE_TRACE") != nullptr;
        if (store_trace_p) fprintf(stderr, "[store] %s: %zu row(s) appended to %s as part %u (%zu of %zu rows in parts)\n", t.name.c_str(), ti.rows_to - ti.rows_from, ti.file.c_str(), state.parts, state.part_rows, state.rows);
        img.tables.push_back(std::move(ti));
        entries.emplace_back(kv.first, state.file);
        saved_now[kv.first] = state;
        continue;
      }
    }
    state.dense = dense;
    state.base_rows = state.rows;
    ++rewritten;
    if (snap_trace && prev != saved_tables_.end()) {
      // AIOS_STORE_TRACE: why a table that had a file is copied whole under the lock, not added to as a part
      std::lock_guard<aios::hal::Mutex> jl(*t.journal_mu);
      const SqlTable::ChangeJournal& j = t.journal;
      fprintf(stderr, "[store] %s: whole (rows %zu; dense %d unpacked %d parts %u/%u shape %s seq %s; journal on %d broken %d "
                      "from %s to %s, %zu ops, %zu cells; part rows %zu + cells %zu of bound %zu)\n",
              t.name.c_str(), state.rows, (int)dense, (int)unpacked_tables_.count(kv.first), prev->second.parts, kPartMaxCount,
              prev->second.shape == state.shape ? "same" : "changed", prev->second.seq == state.seq ? "same" : "moved",
              (int)j.on, (int)j.broken, j.start_seq == prev->second.seq ? "file" : "elsewhere",
              j.last_seq == t.mutation_seq ? "now" : "behind", j.ops.size(), j.cells, prev->second.part_rows,
              prev->second.op_cells, std::max(kPartMaxRowsFloor, prev->second.base_rows / 4));
    }
    unpacked_tables_.erase(kv.first);   // written packed now
    CheckpointImage::TableImage ti;
    ti.key = kv.first;
    ti.dense = dense;
    Writer w;
    w.u32(kColStoreMagic);
    w.str(t.name); w.str(t.database);
    w.u32((uint32_t)t.columns.size());
    for (const auto& col : t.columns) {
      w.str(col.name);
      w.u32((uint32_t)col.type | ((uint32_t)(col.binary & 3) << 6) | ((uint32_t)col.precision << 8) |
            ((uint32_t)col.scale << 16) | ((uint32_t)col.temporal << 24));
      w.u8(col.is_primary_key ? 1 : 0); w.u8(col.not_null ? 1 : 0); w.u8(col.auto_increment ? 1 : 0);
      w.str(col.default_value);
    }
    w.u32((uint32_t)t.col_data.size());
    ti.head = w.take();
    w = Writer();
    // Into the copies the last checkpoint of this table left, if any: their
    // buffers are warm (a fresh copy paid a first-touch fault per page).
    if (auto pooled = checkpoint_pool_.find(kv.first); pooled != checkpoint_pool_.end()) {
      ti.cols = std::move(pooled->second);
      for (const auto& c : ti.cols) checkpoint_pool_bytes_ -= std::min(checkpoint_pool_bytes_, c.estimate_memory_bytes());
      checkpoint_pool_.erase(pooled);
    }
    ti.cols.resize(t.col_data.size());
    for (size_t c = 0; c < t.col_data.size(); ++c) { t.col_data[c].save_copy_into(ti.cols[c]); img.bytes += ti.cols[c].estimate_memory_bytes(); }
    if (!t.indices.empty() || !t.foreign_keys.empty()) {
      w.u32(kIndexTrailerMagic);
      w.u32((uint32_t)t.indices.size());
      for (const auto& ix : t.indices) {
        w.str(ix.name); w.u8(ix.is_unique ? 1 : 0); w.str(ix.type);
        put_str_vec(w, ix.columns);
      }
      w.u32((uint32_t)t.foreign_keys.size());
      for (const auto& fk : t.foreign_keys) {
        w.str(fk.constraint_name); w.str(fk.column); w.str(fk.ref_table); w.str(fk.ref_column);
        w.str(fk.on_delete); w.str(fk.on_update);
      }
    }
    if (std::any_of(t.columns.begin(), t.columns.end(), [](const SqlColumn& c) { return !c.declared.empty(); })) {
      w.u32(kDeclTypeTrailerMagic);
      w.u32((uint32_t)t.columns.size());
      for (const auto& col : t.columns) w.str(col.declared);
    }
    if (!t.create_time.empty()) {
      w.u32(kCreateTimeTrailerMagic);
      w.str(t.create_time);
    }
    if (t.has_rules()) {
      w.u32(kRulesTrailerMagic);
      w.u32((uint32_t)t.columns.size());
      for (const auto& col : t.columns) { w.str(col.generated); w.u8(col.generated_virtual ? 1 : 0); }
      w.u32((uint32_t)t.checks.size());
      for (const auto& ck : t.checks) { w.str(ck.name); w.str(ck.expr); w.u8(ck.column_level ? 1 : 0); }
    }
    if (t.auto_increment_next > 0) {
      w.u32(kAutoIncTrailerMagic);
      w.u64((uint64_t)t.auto_increment_next);
    }
    if (t.text_codec != 0) {
      w.u32(kTextCodecTrailerMagic);
      w.u8(t.text_codec);
    }
    if (table_has_comments(t)) {
      w.u32(kCommentTrailerMagic);
      w.str(t.comment);
      w.u32((uint32_t)t.columns.size());
      for (const auto& col : t.columns) w.str(col.comment);
      w.u32((uint32_t)t.indices.size());
      for (const auto& ix : t.indices) {
        w.str(ix.comment); w.str(ix.type);
        w.u32((uint32_t)ix.sub_parts.size());
        for (uint16_t sp : ix.sub_parts) w.u32(sp);
      }
    }
    const std::string file = hex_of(kv.first) + ".g" + std::to_string(gen) + ".tbl";
    ti.tail = w.take();
    ti.file = file;
    if (prev != saved_tables_.end()) state.bytes = prev->second.bytes;   // the size to expect, give or take
    img.tables.push_back(std::move(ti));
    entries.emplace_back(kv.first, file);
    state.file = file;
    saved_now[kv.first] = state;
  }
  const uint64_t snap_loop_ns = snap_trace ? kernel_.host().monotonic_ns() - snap_loop_t0 : 0;
  // Every table as this snapshot holds it is what the next one starts from: its journal records from here.
  for (auto& kv : tables_) kv.second.journal_begin();
  if (snap_trace)
    fprintf(stderr, "[store] snapshot of %zu tables: loop %.1f ms (tables under 5 ms each: %.1f), journals begun %.1f ms\n", tables_.size(),
            snap_loop_ns / 1e6, snap_small_ns / 1e6, (kernel_.host().monotonic_ns() - snap_loop_t0 - snap_loop_ns) / 1e6);
  // The manifest is finished by write_checkpoint (manifest_bytes), once every
  // file's valid length is known; the databases it names are taken here, under the lock.
  img.databases.assign(databases_.begin(), databases_.end());
  return Ok();
}

// The manifest naming this generation: its entries, then where each entry's file validly ends (kManifestLengthsMagic).
std::vector<uint8_t> SqlDatabase::manifest_bytes(const CheckpointImage& img) const {
  Writer m;
  m.u32(kManifestMagic);
  m.u64(img.gen);
  m.u32((uint32_t)img.databases.size());
  for (const auto& d : img.databases) m.str(d);
  m.u32((uint32_t)img.entries.size());
  for (const auto& e : img.entries) { m.str(e.first); m.str(e.second); }
  m.u32(kManifestLengthsMagic);
  for (const auto& e : img.entries) {
    auto sv = img.saved.find(e.first);
    m.u64(sv == img.saved.end() ? 0 : sv->second.file_bytes);   // 0: the whole file (none of it is a part)
  }
  if (!img.cdc_text.empty()) {
    m.u32(kManifestCdcMagic);
    m.str(img.cdc_text);
  }
  return m.take();
}

// One part (kPartMagic) holding `cells` (rows * ncols, row-major) as rows [from, to) of the table, for generation gen.
static std::vector<uint8_t> store_part_bytes(uint64_t gen, size_t from, size_t to, size_t ncols, const std::vector<std::string>& cells, bool dense) {
  Writer w;
  for (const auto& cell : cells) w.str(cell);
  std::vector<uint8_t> plain = w.take();
  ByteBuffer packed;
  store_pack(plain.data(), plain.size(), packed, dense);
  std::vector<uint8_t> out;
  out.resize(kPartHeader + packed.size());
  uint8_t* p = out.data();
  store_put(p, kPartMagic, 4); p += 4;
  store_put(p, gen, 8); p += 8;
  store_put(p, from, 8); p += 8;
  store_put(p, to, 8); p += 8;
  store_put(p, ncols, 4); p += 4;
  store_put(p, packed.size(), 8); p += 8;
  store_put(p, store_hash64(packed.data(), packed.size()), 8); p += 8;
  std::memcpy(p, packed.data(), packed.size());
  return out;
}

static std::vector<uint8_t> store_ops_part_bytes(uint64_t gen, size_t from, size_t to, size_t ncols, const std::vector<uint8_t>& plain, bool dense) {
  ByteBuffer packed;
  store_pack(plain.data(), plain.size(), packed, dense);
  std::vector<uint8_t> out(kPartHeader + packed.size());
  uint8_t* p = out.data();
  store_put(p, kOpsPartMagic, 4); p += 4;
  store_put(p, gen, 8); p += 8;
  store_put(p, from, 8); p += 8;
  store_put(p, to, 8); p += 8;
  store_put(p, ncols, 4); p += 4;
  store_put(p, packed.size(), 8); p += 8;
  store_put(p, store_hash64(packed.data(), packed.size()), 8); p += 8;
  std::memcpy(p, packed.data(), packed.size());
  return out;
}

// The parts of a table file after its packed image, within [at, limit): each
// one checked (header, hash, generation at most `gen`, rows following on)
// and its rows appended to `t` the way an INSERT appends them. Stops at the
// first part that does not check out: what follows is not guessed at. False,
// with why, when a part that checks out cannot be applied (rows not following
// on, the wrong column count): the table is not loaded half.
// A kOpsPartMagic part's writes, replayed on `t` in their order. The rows parts before it appended are still in
// the delta buffer, after the columns' rows: a row past the columns is one of those, numbered on from them, as
// the running table numbered them - so they are erased or set there, without folding them into the columns.
static bool store_apply_ops(SqlTable& t, ByteSpan plain, uint32_t ncols, size_t* cells, size_t* appended, std::string* why) {
  // Through the table's own writes, as the statements that made them went (erase_rows, set_cell, append_row): what
  // they keep in step with the rows - the column indexes, the per-code verdicts, the zone maps - stays in step.
  Reader r(plain);
  while (r.remaining() > 0) {
    const uint8_t kind = r.u8();
    if (kind == 'E') {
      const uint64_t n = r.u64();
      if (!r.ok() || n > r.remaining() / 8) { if (why) *why = "an erase shorter than its rows"; return false; }
      const size_t total = t.row_count();
      std::vector<size_t> rows;
      rows.reserve((size_t)n);
      for (uint64_t i = 0; i < n; ++i) {
        const uint64_t row = r.u64();
        if (row < total) rows.push_back((size_t)row);   // as the running table's erase_rows: a row past the end is none
      }
      t.erase_rows(rows);
      *cells += rows.size();
    } else if (kind == 'S') {
      const uint64_t row = r.u64();
      const uint32_t col = r.u32();
      std::string val = r.str();
      if (!r.ok() || col >= ncols || row >= t.row_count()) { if (why) *why = "a set cell that does not read"; return false; }
      t.set_cell((size_t)row, col, val);
      *cells += 1;
    } else if (kind == 'A') {
      const uint64_t n = r.u64();
      if (!r.ok()) { if (why) *why = "an append that does not read"; return false; }
      std::vector<std::string> vals(ncols);
      for (uint64_t i = 0; i < n; ++i) {
        for (uint32_t c = 0; c < ncols; ++c) vals[c] = r.str();
        if (!r.ok()) { if (why) *why = "an append shorter than its rows"; return false; }
        t.append_row_canonical(vals);   // as stored: not normalised again
      }
      *appended += (size_t)n;
    } else {
      if (why) *why = "a write of an unknown kind";
      return false;
    }
    if (!r.ok()) { if (why) *why = "a part shorter than its writes"; return false; }
  }
  return true;
}

// A part that checks out (header, hash, generation at most `gen`): its length with the header, or 0.
static size_t store_part_checks(ByteSpan file, size_t at, size_t limit, uint64_t gen) {
  if (at + kPartHeader > limit) return 0;
  const uint8_t* p = file.data() + at;
  const uint32_t magic = (uint32_t)store_get(p, 4);
  if (magic != kPartMagic && magic != kOpsPartMagic) return 0;
  const uint64_t pgen = store_get(p + 4, 8), len = store_get(p + 32, 8), hash = store_get(p + 40, 8);
  if (pgen > gen || len > limit - at - kPartHeader) return 0;
  if (store_hash64(p + kPartHeader, (size_t)len) != hash) return 0;
  return kPartHeader + (size_t)len;
}

// `later`: from the first part of writes on, the parts are not applied but kept, and replayed once every table is
// loaded (store_replay_later), through the table's own writes.
static bool store_parts(ByteSpan file, size_t at, size_t limit, uint64_t gen, SqlTable& t, size_t* parts, size_t* part_rows, bool* recoded, std::string* why,
                        size_t* op_parts = nullptr, size_t* op_cells = nullptr, std::vector<std::vector<uint8_t>>* later = nullptr) {
  *parts = 0; *part_rows = 0; *recoded = false;
  if (op_parts) *op_parts = 0;
  if (op_cells) *op_cells = 0;
  t.ensure_columns();
  bool keeping = false;
  while (at + kPartHeader <= limit) {
    const uint8_t* p = file.data() + at;
    const uint32_t magic = (uint32_t)store_get(p, 4);
    if (later && (keeping || magic == kOpsPartMagic)) {
      const size_t n = store_part_checks(file, at, limit, gen);
      if (!n) { if (why) *why = "a part of writes that fails validation"; return false; }
      later->emplace_back(p, p + n);
      keeping = true;
      at += n;
      continue;
    }
    if (magic == kOpsPartMagic) {
      const uint64_t pgen = store_get(p + 4, 8), from = store_get(p + 12, 8), to = store_get(p + 20, 8);
      const uint32_t ncols = (uint32_t)store_get(p + 28, 4);
      const uint64_t len = store_get(p + 32, 8), hash = store_get(p + 40, 8);
      if (pgen > gen) { if (why) *why = "a part of writes whose manifest never landed"; return false; }
      if (len > limit - at - kPartHeader) { if (why) *why = "a part of writes truncated"; return false; }
      const uint8_t* payload = p + kPartHeader;
      if (store_hash64(payload, (size_t)len) != hash) { if (why) *why = "a part of writes with hash mismatch"; return false; }
      if (from != t.row_count()) { if (why) *why = "a part of writes for another row count"; return false; }
      if (ncols != t.col_data.size()) { if (why) *why = "a part of writes with the wrong column count"; return false; }
      ByteBuffer plain;
      std::string uw;
      if (!store_unpack(ByteSpan(payload, (size_t)len), plain, &uw)) { if (why) *why = "a part of writes that does not unpack: " + uw; return false; }
      size_t cells = 0, appended = 0;
      if (!store_apply_ops(t, ByteSpan(plain.data(), plain.size()), ncols, &cells, &appended, why)) return false;
      *part_rows += appended;
      if (t.row_count() != to) { if (why) *why = "a part of writes that leaves another row count"; return false; }
      ++*parts;
      if (op_parts) ++*op_parts;
      if (op_cells) *op_cells += cells;
      *recoded = true;   // rows moved or changed: what is keyed on the layout starts again
      at += kPartHeader + (size_t)len;
      continue;
    }
    if (magic != kPartMagic) {
      if (why) *why = "unrecognised part magic";
      return false;
    }
    const uint64_t pgen = store_get(p + 4, 8), from = store_get(p + 12, 8), to = store_get(p + 20, 8);
    const uint32_t ncols = (uint32_t)store_get(p + 28, 4);
    const uint64_t len = store_get(p + 32, 8), hash = store_get(p + 40, 8);
    if (pgen > gen) { if (why) *why = "a part whose manifest never landed"; return false; }
    if (len > limit - at - kPartHeader) { if (why) *why = "a part truncated"; return false; }
    const uint8_t* payload = p + kPartHeader;
    if (store_hash64(payload, (size_t)len) != hash) { if (why) *why = "a part with hash mismatch"; return false; }
    if (from != t.row_count() || to < from) { if (why) *why = "a part's rows do not follow the table's"; return false; }
    if (ncols != t.col_data.size()) { if (why) *why = "a part with the wrong column count"; return false; }
    ByteBuffer plain;
    std::string uw;
    if (!store_unpack(ByteSpan(payload, (size_t)len), plain, &uw)) { if (why) *why = "a part that does not unpack: " + uw; return false; }
    // The part's rows go to the table's delta buffer, as they are stored (a cell per column, row after row): the
    // columns take them when the table is next written or scanned (compact_delta_buffer). Appended to the columns
    // here, the first part made a text column build its value -> code index over its whole dictionary - 17M
    // entries for audittrail on chi, most of a 20 s restart - which the first replicated insert after a start
    // builds anyway, on its own thread, while the rest of the database answers.
    Reader r(ByteSpan(plain.data(), plain.size()));
    std::vector<SqlRow> rows((size_t)(to - from));
    for (auto& row : rows) {
      row.values.reserve(ncols);
      for (uint32_t c = 0; c < ncols; ++c) {
        row.values.push_back(r.str());
        if (!r.ok()) { if (why) *why = "a part shorter than its rows"; return false; }
      }
    }
    {
      std::lock_guard<aios::hal::Mutex> lk(*t.delta_buffer.mu);
      auto& d = t.delta_buffer.rows;
      if (d.empty()) d = std::move(rows);
      else d.insert(d.end(), std::make_move_iterator(rows.begin()), std::make_move_iterator(rows.end()));
      t.delta_buffer.count.store(d.size(), std::memory_order_relaxed);
    }
    ++*parts;
    *part_rows += (size_t)(to - from);
    at += kPartHeader + (size_t)len;
  }
  if (at != limit) {
    if (why) *why = "parts incomplete or damaged before valid length";
    return false;
  }
  return true;
}

// The parts store_parts kept for later, applied in their order, once the store is loaded (see store_parts).
static bool store_replay_later(SqlTable& t, const std::vector<std::vector<uint8_t>>& later, size_t* op_parts, size_t* op_cells, size_t* part_rows, std::string* why) {
  for (const auto& part : later) {
    size_t parts = 0, rows = 0, ops = 0, cells = 0;
    bool recoded = false;
    if (!store_parts(ByteSpan(part.data(), part.size()), 0, part.size(), ~0ull, t, &parts, &rows, &recoded, why, &ops, &cells, nullptr)) return false;
    if (parts != 1) { if (why) *why = "a kept part that no longer reads"; return false; }
    *op_parts += ops;
    *op_cells += cells;
    *part_rows += rows;
  }
  // The rows erased and set leave segments without stats (has_stats false, empty bounds) until maintenance refreshes
  // them; read straight after a start, `doc = 'x'` skipped such a segment by its empty bounds. Built again here,
  // as a whole file's are when it is loaded.
  t.compact_delta_buffer();
  for (auto& cd : t.col_data) cd.build_zone_maps();
  return true;
}

// The files of a snapshot, then the manifest that names them: on the
// checkpoint thread while statements run, or inline for a caller that needs
// the store on the medium before it returns. Touches nothing of the tables.
Result<void> SqlDatabase::write_checkpoint(CheckpointImage& img) {
  auto& h = kernel_.host();
  const std::string dir = root_path_ + "/tables";
  h.make_dir(dir);
  static const bool trace = std::getenv("AIOS_STORE_TRACE") != nullptr;
  const uint64_t w0 = h.monotonic_ns();
  ByteBuffer packed;   // one table file's packed bytes, reused across the tables
  // The serialization buffer is reused across the tables of this snapshot, but
  // its capacity is the largest table's: past a threshold it is not kept
  // between checkpoints.
  struct ShedWbuf {
    std::vector<uint8_t>& b;
    ~ShedWbuf() { if (b.capacity() > (size_t)32 << 20) std::vector<uint8_t>().swap(b); }
  } shed_wbuf{checkpoint_wbuf_};
  for (auto& ti : img.tables) {
    auto sv = img.saved.find(ti.key);
    if (!ti.repack_from.empty()) {
      // A still table, deflated from its own file. A file that does not unpack goes under the new name as it is -
      // the table all the same, deflated another time.
      auto rd = h.read_file(dir + "/" + ti.repack_from);
      if (!rd.ok()) return Result<void>(Status::IoError, ti.repack_from + ": " + rd.error().message);
      ByteBuffer plain;
      std::string why;
      const bool unpacked = store_unpack(ByteSpan(rd->data(), rd->size()), plain, &why);
      if (unpacked) store_pack(plain.data(), plain.size(), packed, true);
      const ByteSpan bytes = unpacked ? ByteSpan(packed.data(), packed.size()) : ByteSpan(rd->data(), rd->size());
      auto wr = h.write_file(dir + "/" + ti.file, bytes);
      if (!wr.ok()) return wr;
      if (sv != img.saved.end()) {
        sv->second.file_bytes = bytes.size();
        if (unpacked) sv->second.bytes = plain.size();
        else sv->second.dense = false;
      }
      continue;
    }
    if (ti.ops) {
      // The writes since the file was written, as one part on its end.
      const std::vector<uint8_t> part = store_ops_part_bytes(img.gen, ti.rows_from, ti.rows_to, ti.ncols, ti.ops_plain, ti.dense);
      auto ar = h.append_file(dir + "/" + ti.file, ByteSpan(part.data(), part.size()));
      if (!ar.ok()) return ar;
      if (sv != img.saved.end()) sv->second.file_bytes += part.size();
      std::vector<uint8_t>().swap(ti.ops_plain);
      continue;
    }
    if (ti.append) {
      // The rows appended since the file was written, as one part on its end.
      const std::vector<uint8_t> part = store_part_bytes(img.gen, ti.rows_from, ti.rows_to, ti.ncols, ti.part_cells, ti.dense);
      auto ar = h.append_file(dir + "/" + ti.file, ByteSpan(part.data(), part.size()));
      if (!ar.ok()) return ar;
      if (sv != img.saved.end()) sv->second.file_bytes += part.size();
      std::vector<std::string>().swap(ti.part_cells);
      continue;
    }
    checkpoint_wbuf_.clear();
    Writer w(checkpoint_wbuf_);
    if (sv != img.saved.end() && sv->second.bytes) w.buf().reserve(sv->second.bytes + sv->second.bytes / 8);
    w.raw(ByteSpan(ti.head.data(), ti.head.size()));
    std::vector<uint64_t> spans;
    spans.reserve(ti.cols.size() + 1);
    for (const auto& c : ti.cols) { spans.push_back(w.buf().size()); put_column(w, c); }
    spans.push_back(w.buf().size());
    w.raw(ByteSpan(ti.tail.data(), ti.tail.size()));
    for (uint64_t at : spans) w.u64(at);
    w.u32((uint32_t)ti.cols.size());
    w.u32(kColumnSpansFooterMagic);
    if (sv != img.saved.end()) sv->second.bytes = w.buf().size();
    // On the medium compressed (store_pack): st's 2.4 GB of table files were
    // 1.0 GB once LZ4 had been over them.
    store_pack(w.buf().data(), w.buf().size(), packed, ti.dense);
    auto wr = h.write_file(dir + "/" + ti.file, ByteSpan(packed.data(), packed.size()));
    if (!wr.ok()) return wr;
    if (sv != img.saved.end()) sv->second.file_bytes = packed.size();   // the whole file is the image: parts start here
    // The copies stay for the next snapshot of this table, within the pool's
    // budget (AIOS_CHECKPOINT_POOL_MB); beyond it they go now.
    size_t sz = 0;
    for (const auto& c : ti.cols) sz += c.estimate_memory_bytes();
    if (checkpoint_pool_cap() > 0 && checkpoint_pool_bytes_ + sz <= checkpoint_pool_cap()) {
      checkpoint_pool_bytes_ += sz;
      checkpoint_pool_[ti.key] = std::move(ti.cols);
    }
    ti.cols.clear();
  }
  if (checkpoint_pool_cap() == 0 && !checkpoint_pool_.empty()) {
    checkpoint_pool_.clear();
    checkpoint_pool_bytes_ = 0;
  }
  const uint64_t w1 = h.monotonic_ns();
  img.manifest = manifest_bytes(img);
  auto mw = h.write_file(dir + "/MANIFEST", ByteSpan(img.manifest.data(), img.manifest.size()));
  if (!mw.ok()) return mw;
  // The renames into tables/ (each file's tmp -> name, MANIFEST last) on disk: until the directory is flushed a
  // power cut can bring back the previous MANIFEST beside files it does not name, or lose a renamed table file.
  if (auto ds = h.sync_dir(dir); !ds.ok()) return ds;
  const uint64_t w2 = h.monotonic_ns();
  store_generation_ = img.gen;
  saved_tables_ = std::move(img.saved);
  if (std::getenv("AIOS_STORE_TRACE"))
    std::fprintf(stderr, "sql store: generation %llu - %zu of %zu table file(s) rewritten\n",
                 (unsigned long long)img.gen, img.rewritten, img.entries.size());
  // The manifest now names this generation: everything else in the directory
  // is a previous one (or an interrupted save's leftovers).
  std::unordered_set<std::string> keep;
  for (const auto& e : img.entries) keep.insert(e.second);
  keep.insert("MANIFEST");
  if (auto ls = h.list_dir(dir); ls.ok())
    for (const auto& f : *ls) if (!keep.count(f)) h.remove_gently(dir + "/" + f);
  // The old single-file store is superseded - and 7.5 GB on st.
  const std::string legacy = root_path_ + "/schema_tables.db";
  if (h.exists(legacy)) h.remove(legacy);
  const uint64_t w3 = h.monotonic_ns();
  // Also in the manifest now (kManifestCdcMagic); kept as a file for the tools that read it.
  if (!img.cdc_text.empty()) {
    h.write_file(root_path_ + "/cdc_position", ByteSpan((const uint8_t*)img.cdc_text.data(), img.cdc_text.size()));
    h.sync_dir(root_path_);
  }
  // The rotated logs this generation covers (its own, and any an earlier
  // write left behind) are redundant now that the manifest names it.
  if (auto ls = h.list_dir(root_path_); ls.ok())
    for (const auto& f : *ls) {
      if (f.compare(0, 6, "redo.g") != 0 || f.size() < 12 || f.compare(f.size() - 5, 5, ".prev") != 0) continue;
      const uint64_t g = std::strtoull(f.c_str() + 6, nullptr, 10);
      if (g > 0 && g <= img.gen) h.remove_gently(root_path_ + "/" + f);
    }
  if (trace)
    std::fprintf(stderr, "sql store: generation %llu write phases at %.3f: files %.1f ms, manifest %.1f, cleanup %.1f, redo %.1f\n",
                 (unsigned long long)img.gen, h.wall_ns() / 1e9, (w1 - w0) / 1e6, (w2 - w1) / 1e6, (w3 - w2) / 1e6,
                 (h.monotonic_ns() - w3) / 1e6);
  return Ok();
}

bool SqlDatabase::load_tables_v4() {
  const std::string dir = root_path_ + "/tables";
  auto mf = kernel_.host().read_file(dir + "/MANIFEST");
  if (!mf.ok() || mf->empty()) return false;
  Reader m(ByteSpan(mf->data(), mf->size()));
  if (m.u32() != kManifestMagic) return false;
  const uint64_t gen = m.u64();
  uint32_t ndb = m.u32();
  for (uint32_t i = 0; i < ndb && m.ok(); ++i) databases_.insert(m.str());
  uint32_t nt = m.u32();
  std::unordered_map<std::string, SqlTable> loaded;
  // A table whose file cannot be read is left out - and its file moved aside below, not left for the next checkpoint
  // to sweep away as "not in the manifest": that deleted the only copy of a damaged table without a word.
  std::vector<std::pair<std::string, std::string>> damaged;   // file, why
  // The entries, then (kManifestLengthsMagic) where each one's file validly
  // ends: a manifest from before parts has no lengths, and its files are
  // whole images (0: all of the file).
  std::vector<std::pair<std::string, std::string>> manifest_entries;
  for (uint32_t i = 0; i < nt && m.ok(); ++i) { std::string key = m.str(), file = m.str(); manifest_entries.emplace_back(std::move(key), std::move(file)); }
  std::vector<uint64_t> valid_lengths(manifest_entries.size(), 0);
  if (m.ok() && m.remaining() >= 4 && m.u32() == kManifestLengthsMagic)
    for (size_t i = 0; i < manifest_entries.size() && m.ok(); ++i) { const uint64_t n = m.u64(); if (m.ok()) valid_lengths[i] = n; }
  // The position this generation was taken at is where the stream resumes from - before the redo logs after it are
  // replayed, which move it on with every commit mark they hold. The separate file may be older (written after the
  // manifest, a crash between left it behind) and is brought up to the manifest's.
  if (m.ok() && m.remaining() >= 4 && m.u32() == kManifestCdcMagic) {
    const std::string cdc = m.str();
    if (m.ok() && !cdc.empty()) {
      auto cur = kernel_.host().read_file(root_path_ + "/cdc_position");
      const bool same = cur.ok() && std::string((const char*)cur->data(), cur->size()) == cdc;
      if (!same) {
        kernel_.host().write_file(root_path_ + "/cdc_position", ByteSpan((const uint8_t*)cdc.data(), cdc.size()));
        kernel_.host().sync_dir(root_path_);
      }
    }
  }
  set_startup_phase("loading tables", 0, manifest_entries.size());
  const auto load_t0 = std::chrono::steady_clock::now();
  // The tables are independent: each is read, unpacked and decoded on whichever thread takes it, biggest first,
  // and what each found is put together afterwards in the manifest's order. Decoding the columns was most of a
  // start (3 s of 4.6 on chi-il-1), one table after another.
  struct Slot {
    std::vector<std::pair<std::string, std::string>> damaged;
    bool has = false;
    SqlTable t;
    bool saved = false;
    SavedTableFile st;
    bool unpacked = false;
    double read = 0, unpack = 0, decode = 0, parts = 0, total = 0;
    std::vector<std::vector<uint8_t>> later;   // parts from the first part of writes on: replayed once all are loaded
  };
  std::vector<Slot> slots(manifest_entries.size());
  std::atomic<size_t> loaded_n{0};
  auto lap = [](std::chrono::steady_clock::time_point& at) {
    const auto now = std::chrono::steady_clock::now();
    const double d = std::chrono::duration<double>(now - at).count();
    at = now;
    return d;
  };
  auto load_one = [&](size_t ei) {
    Slot& slot = slots[ei];
    const std::string& file = manifest_entries[ei].second;
    auto lap_at = std::chrono::steady_clock::now();
    const auto slot_t0 = lap_at;
    auto tf = kernel_.host().read_file(dir + "/" + file);
    slot.read += lap(lap_at);
    if (!tf.ok()) { slot.damaged.push_back({file, "unreadable: " + tf.error().message}); return; }
    ByteBuffer plain;
    ByteSpan body(tf->data(), tf->size());
    const bool packed = store_is_packed(body);
    size_t image_end = tf->size();   // where the packed image stops: parts, if any, follow
    bool image_deflated = false;     // written in the dense form: not deflated again once it is still
    std::shared_ptr<ByteBuffer> file_bytes_kept;   // the file while its parts are read
    if (packed) {
      std::string why;
      if (!store_unpack(body, plain, &why, &image_end, &image_deflated)) {
        slot.damaged.push_back({file, why});
        return;
      }
      if (image_end < tf->size()) file_bytes_kept = std::make_shared<ByteBuffer>(std::move(*tf));
      ByteBuffer().swap(*tf);
      body = ByteSpan(plain.data(), plain.size());
    }
    slot.unpack += lap(lap_at);
    const size_t image_plain = body.size();   // what the image holds unpacked (SavedTableFile::bytes)
    const uint64_t valid_end = valid_lengths[ei] ? std::min<uint64_t>(valid_lengths[ei], file_bytes_kept ? file_bytes_kept->size() : image_end) : (file_bytes_kept ? file_bytes_kept->size() : image_end);
    Reader r(body);
    const uint32_t magic = r.u32();
    if (magic != kColStoreMagic && magic != kColStoreMagicV7 && magic != kColStoreMagicV6 && magic != kColStoreMagicV5 && magic != kColStoreMagicV4 && magic != kColStoreMagicV3 && magic != kColStoreMagicV2 && magic != kColStoreMagicV1) {
      slot.damaged.push_back({file, "not a table file this version reads"});
      return;
    }
    SqlTable t;
    t.name = r.str(); t.database = r.str();
    uint32_t ncol = r.u32();
    for (uint32_t c = 0; c < ncol && r.ok(); ++c) {
      SqlColumn col;
      col.name = r.str();
      uint32_t type_word = r.u32();
      col.type = (SqlDataType)(type_word & 0x3f);
      col.binary = (uint8_t)((type_word >> 6) & 3);
      col.precision = (uint8_t)((type_word >> 8) & 0xff);
      col.scale = (uint8_t)((type_word >> 16) & 0xff);
      col.temporal = (uint8_t)((type_word >> 24) & 0xff);
      col.is_primary_key = r.u8() != 0; col.not_null = r.u8() != 0; col.auto_increment = r.u8() != 0;
      col.default_value = r.str();
      t.columns.push_back(col);
    }
    uint32_t ncd = r.u32();
    t.col_data.resize(ncd);
    bool good = r.ok();
    uint32_t bad_col = 0;
    // Where each column lies, from the footer (kColumnSpansFooterMagic) if the image has one that fits it.
    std::vector<uint64_t> spans;
    if (good && ncd > 1 && body.size() >= 8 + 8 * ((size_t)ncd + 1)) {
      Reader f(body.subspan(body.size() - 8));
      const uint32_t fn = f.u32(), fm = f.u32();
      if (fm == kColumnSpansFooterMagic && fn == ncd) {
        Reader o(body.subspan(body.size() - 8 - 8 * ((size_t)ncd + 1)));
        for (uint32_t c = 0; c <= ncd; ++c) spans.push_back(o.u64());
        bool fits = spans[0] == r.position();
        for (uint32_t c = 0; c < ncd && fits; ++c) fits = spans[c] <= spans[c + 1];
        if (!fits || spans[ncd] > body.size() - 8 - 8 * ((size_t)ncd + 1)) spans.clear();
      }
    }
    if (!spans.empty() && spans[ncd] - spans[0] >= ((size_t)32 << 20)) {
      // a big table: its columns at once, each from its own span, each read to the end of it exactly
      std::vector<char> col_ok(ncd, 0);
      std::vector<std::thread> threads;
      for (uint32_t c = 0; c < ncd; ++c)
        threads.emplace_back([&, c] {
          Reader cr(body.subspan(spans[c], spans[c + 1] - spans[c]));
          col_ok[c] = get_column(cr, t.col_data[c], magic) && cr.remaining() == 0;
        });
      for (auto& th : threads) th.join();
      for (uint32_t c = 0; c < ncd && good; ++c) { good = col_ok[c] != 0; if (!good) bad_col = c; }
      static const bool spans_trace = std::getenv("AIOS_STORE_TRACE") != nullptr;
      if (spans_trace) std::fprintf(stderr, "[store] %s: %u column(s) decoded at once (%.1f MB)\n", t.name.c_str(), ncd, (spans[ncd] - spans[0]) / 1048576.0);
      if (good) r.raw((size_t)(spans[ncd] - spans[0]));
    } else {
      for (uint32_t c = 0; c < ncd && good; ++c) { good = get_column(r, t.col_data[c], magic); if (!good) bad_col = c; }
    }
    if (!good) {   // skipped, not guessed at; the column and its encoding named so the cause can be found
      const std::string cname = bad_col < t.columns.size() ? t.columns[bad_col].name : std::to_string(bad_col);
      slot.damaged.push_back({file, "damaged column data: " + t.database + "." + t.name + "." + cname + " encoding " +
                                   std::to_string((int)t.col_data[bad_col].encoding)});
      return;
    }
    uint32_t trailer = r.remaining() >= 4 ? r.u32() : 0;
    if (trailer == kIndexTrailerMagic) {
      trailer = 0;
      const uint32_t nix = r.u32();
      for (uint32_t i = 0; i < nix && r.ok(); ++i) {
        SqlIndex ix;
        ix.name = r.str(); ix.is_unique = r.u8() != 0; ix.type = r.str();
        if (!get_str_vec(r, ix.columns)) break;
        // a copy of an index already read (ADD INDEX once added one per run of the same migration): left out
        bool dup = false;
        for (const auto& have : t.indices)
          dup = dup || (lower_of(have.name) == lower_of(ix.name) && have.columns == ix.columns && have.is_unique == ix.is_unique);
        if (!dup) t.indices.push_back(std::move(ix));
      }
      const uint32_t nfk = r.ok() ? r.u32() : 0;
      for (uint32_t i = 0; i < nfk && r.ok(); ++i) {
        SqlForeignKey fk;
        fk.constraint_name = r.str(); fk.column = r.str(); fk.ref_table = r.str(); fk.ref_column = r.str();
        fk.on_delete = r.str(); fk.on_update = r.str();
        t.foreign_keys.push_back(std::move(fk));
      }
      if (!r.ok()) { t.indices.clear(); t.foreign_keys.clear(); }
      else if (r.remaining() >= 4) trailer = r.u32();
    }
    if (trailer == kDeclTypeTrailerMagic && r.ok()) {
      const uint32_t nd = r.u32();
      std::vector<std::string> decl;
      for (uint32_t i = 0; i < nd && r.ok(); ++i) decl.push_back(r.str());
      if (r.ok() && decl.size() == t.columns.size())
        for (size_t i = 0; i < decl.size(); ++i) t.columns[i].declared = decl[i];
      trailer = r.ok() && r.remaining() >= 4 ? r.u32() : 0;
    }
    if (trailer == kCreateTimeTrailerMagic && r.ok()) {
      std::string ct = r.str();
      if (r.ok()) t.create_time = std::move(ct);
      trailer = r.ok() && r.remaining() >= 4 ? r.u32() : 0;
    }
    if (trailer == kRulesTrailerMagic && r.ok()) {
      const uint32_t nc = r.u32();
      std::vector<std::pair<std::string, bool>> generated;
      for (uint32_t i = 0; i < nc && r.ok(); ++i) { std::string g = r.str(); const bool v = r.u8() != 0; generated.push_back({std::move(g), v}); }
      const uint32_t nk = r.ok() ? r.u32() : 0;
      std::vector<SqlCheck> checks;
      for (uint32_t i = 0; i < nk && r.ok(); ++i) { SqlCheck ck; ck.name = r.str(); ck.expr = r.str(); ck.column_level = r.u8() != 0; checks.push_back(std::move(ck)); }
      if (r.ok() && generated.size() == t.columns.size()) {
        for (size_t i = 0; i < generated.size(); ++i) { t.columns[i].generated = generated[i].first; t.columns[i].generated_virtual = generated[i].second; }
        t.checks = std::move(checks);
      }
      trailer = r.ok() && r.remaining() >= 4 ? r.u32() : 0;
    }
    if (trailer == kAutoIncTrailerMagic && r.ok()) {
      const uint64_t next = r.u64();
      if (r.ok()) t.auto_increment_next = (long long)next;
      trailer = r.ok() && r.remaining() >= 4 ? r.u32() : 0;
    }
    if (trailer == kCommentTrailerMagic && r.ok()) {
      std::string tc = r.str();
      const uint32_t nc = r.u32();
      std::vector<std::string> cc;
      for (uint32_t i = 0; i < nc && r.ok(); ++i) cc.push_back(r.str());
      const uint32_t ni = r.ok() ? r.u32() : 0;
      struct IxMeta { std::string comment, type; std::vector<uint16_t> sub; };
      std::vector<IxMeta> im;
      for (uint32_t i = 0; i < ni && r.ok(); ++i) {
        IxMeta meta; meta.comment = r.str(); meta.type = r.str();
        const uint32_t ns = r.ok() ? r.u32() : 0;
        for (uint32_t k = 0; k < ns && r.ok(); ++k) meta.sub.push_back((uint16_t)r.u32());
        im.push_back(std::move(meta));
      }
      if (r.ok() && cc.size() == t.columns.size()) {
        t.comment = std::move(tc);
        for (size_t i = 0; i < cc.size(); ++i) t.columns[i].comment = cc[i];
        if (im.size() == t.indices.size())
          for (size_t i = 0; i < im.size(); ++i) { t.indices[i].comment = im[i].comment; if (!im[i].type.empty()) t.indices[i].type = im[i].type; t.indices[i].sub_parts = im[i].sub; }
      }
      trailer = r.ok() && r.remaining() >= 4 ? r.u32() : 0;
    }
    if (trailer == kTextCodecTrailerMagic && r.ok()) {
      const uint8_t codec = r.u8();
      if (r.ok() && codec <= ColumnStore::kBlockDeflate) t.text_codec = codec;
      trailer = r.ok() && r.remaining() >= 4 ? r.u32() : 0;
    }
    for (auto& cd : t.col_data) cd.block_codec_pref = t.text_codec;
    // A TEMPORARY table of a session that ended with the process: never loaded.
    if (hidden_table_name(t.name)) return;
    // An older file's zone maps were keyed by an older collation (no PAD
    // SPACE, or lower-cased ASCII only): pruning on them could skip a segment
    // that matches. Rebuild them from the data; the table is rewritten at the
    // next checkpoint.
    if (magic != kColStoreMagic && magic != kColStoreMagicV7 && magic != kColStoreMagicV6 && magic != kColStoreMagicV5 && magic != kColStoreMagicV4 && magic != kColStoreMagicV3)
      for (auto& cd : t.col_data) cd.build_zone_maps();
    t.ensure_columns();
    const size_t base_rows = t.row_count();
    // The parts appended since the image was written: their rows go on the
    // table as an INSERT's would. A part a crash left unnamed by any
    // manifest (past valid_end, or of a later generation) is not read.
    slot.decode += lap(lap_at);
    size_t parts = 0, part_rows = 0, op_parts = 0, op_cells = 0;
    bool part_recoded = false;
    if (file_bytes_kept && image_end < valid_end) {
      std::string why;
      if (!store_parts(ByteSpan(file_bytes_kept->data(), file_bytes_kept->size()), image_end, (size_t)valid_end, gen, t, &parts, &part_rows, &part_recoded, &why, &op_parts, &op_cells, &slot.later)) {
        slot.damaged.push_back({file, "parts not loaded: " + why});
        return;
      }
      if (parts || !slot.later.empty()) {
        t.mutation_seq = sql_next_mutation_seq();
        if (part_recoded) t.layout_seq = t.mutation_seq;
        static const bool store_trace_l = std::getenv("AIOS_STORE_TRACE") != nullptr;
        if (store_trace_l) fprintf(stderr, "[store] %s.%s: %zu row(s) from %zu part(s) appended to the %zu of its image\n", t.database.c_str(), t.name.c_str(), part_rows, parts, base_rows);
      }
    }
    file_bytes_kept.reset();
    slot.parts += lap(lap_at);
    // Re-saved as-is later only if its zone maps are current and it is
    // packed (an AQT1 file, or one written before store_pack, gets rewritten
    // at the next checkpoint).
    if (magic == kColStoreMagic || magic == kColStoreMagicV6) {   // an AQT6 file reads back complete: re-saved only when the table changes
      SavedTableFile st{file, t.mutation_seq, t.row_count(), table_shape_signature(t)};
      st.layout = t.layout_seq;
      st.base_rows = base_rows;
      st.part_rows = part_rows;
      st.parts = (uint32_t)parts;
      st.op_parts = (uint32_t)op_parts;
      st.op_cells = op_cells;
      st.file_bytes = packed ? valid_end : 0;   // the image, then the parts read: where the next part goes
      // A start forgot that a file was already deflated, and the first checkpoint two minutes on copied every big
      // still table whole under the lock to deflate it again (a 30M-row audit log: a 13 s stall after each restart).
      st.dense = image_deflated;
      st.bytes = image_plain;
      slot.st = st;
      slot.saved = true;
      if (!packed) slot.unpacked = true;   // converted per checkpoint (maintain_storage)
    } else {
      slot.unpacked = true;   // legacy format: upgrade to AQT6 and pack
    }
    slot.total = std::chrono::duration<double>(std::chrono::steady_clock::now() - slot_t0).count();
    slot.t = std::move(t);
    slot.has = true;
  };
  std::vector<size_t> order(manifest_entries.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return valid_lengths[a] > valid_lengths[b]; });
  for_each_parallel(order.size(), [&](size_t i) {
    load_one(order[i]);
    startup_progress(loaded_n.fetch_add(1) + 1);
  });
  double t_read = 0, t_unpack = 0, t_decode = 0, t_parts = 0;
  if (std::getenv("AIOS_STORE_TRACE")) {   // the tables that took longest: what a start waits for
    std::vector<size_t> by_time(slots.size());
    for (size_t i = 0; i < by_time.size(); ++i) by_time[i] = i;
    std::sort(by_time.begin(), by_time.end(), [&](size_t a, size_t b) { return slots[a].total > slots[b].total; });
    for (size_t i = 0; i < by_time.size() && i < 8; ++i) {
      const Slot& sl = slots[by_time[i]];
      std::fprintf(stderr, "[store] %s: %.2f s (read %.2f, unpack %.2f, decode %.2f)\n", manifest_entries[by_time[i]].first.c_str(), sl.total, sl.read, sl.unpack, sl.decode);
    }
  }
  for (size_t ei = 0; ei < slots.size(); ++ei) {
    Slot& slot = slots[ei];
    for (auto& d : slot.damaged) damaged.push_back(std::move(d));
    t_read += slot.read; t_unpack += slot.unpack; t_decode += slot.decode; t_parts += slot.parts;
    if (!slot.has) continue;
    const std::string& key = manifest_entries[ei].first;
    if (!slot.later.empty()) {
      // The table's parts of writes (and any after them), here on the loading thread (store_parts says why).
      size_t op_parts = 0, op_cells = 0, rows = 0;
      std::string why;
      if (!store_replay_later(slot.t, slot.later, &op_parts, &op_cells, &rows, &why)) {
        damaged.push_back({manifest_entries[ei].second, "parts of writes not loaded: " + why});
        continue;
      }
      slot.t.mutation_seq = sql_next_mutation_seq();
      slot.t.layout_seq = slot.t.mutation_seq;   // rows moved or changed
      slot.st.seq = slot.t.mutation_seq;
      slot.st.rows = slot.t.row_count();
      slot.st.layout = slot.t.layout_seq;
      slot.st.parts += (uint32_t)slot.later.size();
      slot.st.op_parts += (uint32_t)op_parts;
      slot.st.op_cells += op_cells;
      slot.st.part_rows += rows;
      static const bool store_trace_r = std::getenv("AIOS_STORE_TRACE") != nullptr;
      if (store_trace_r) fprintf(stderr, "[store] %s: %zu part(s) replayed after the load (%zu of writes, %zu row(s) erased or cell(s) set)\n", key.c_str(), slot.later.size(), op_parts, op_cells);
      std::vector<std::vector<uint8_t>>().swap(slot.later);
    }
    if (slot.saved) saved_tables_[key] = slot.st;
    if (slot.unpacked) unpacked_tables_.insert(key);
    // Writes from here on are what the file lacks: the first checkpoint after a start can add them as a part too.
    if (slot.saved) slot.t.journal_begin();
    loaded[key] = std::move(slot.t);
  }
  if (!damaged.empty()) {
    const std::string aside = root_path_ + "/tables-damaged";
    kernel_.host().make_dir(aside);
    for (const auto& [file, why] : damaged) {
      const std::string kept = aside + "/" + file;
      const bool moved = kernel_.host().exists(dir + "/" + file) && kernel_.host().rename_file(dir + "/" + file, kept).ok();
      const std::string line = "table file " + file + " not loaded (" + why + ")" + (moved ? "; kept as " + kept : "");
      std::fprintf(stderr, "sql store: %s\n", line.c_str());
      damaged_tables_.push_back(line);
    }
    std::fprintf(stderr, "sql store: %zu table(s) not loaded: a mirror seeds them again from its master; "
                         "otherwise restore them from a snapshot (EXPORT SNAPSHOT)\n", damaged.size());
  }
  tables_ = std::move(loaded);
  store_generation_ = gen;
  std::fprintf(stderr, "asql: %zu table(s) loaded in %.2f s (thread time: read %.2f, unpack %.2f, decode %.2f, parts %.2f)\n", tables_.size(),
               std::chrono::duration<double>(std::chrono::steady_clock::now() - load_t0).count(), t_read, t_unpack, t_decode, t_parts);
#if defined(__GLIBC__)
  // Where the process's memory stands against the tables' own estimate right after the load: what the estimate
  // misses shows here, before indexes and queries add theirs.
  if (std::getenv("AIOS_STORE_TRACE")) {
    size_t est = 0;
    for (const auto& kv : tables_) est += kv.second.estimated_memory_bytes();
    std::fprintf(stderr, "[store] memory after the load: allocator live %.1f MB (%.1f in blocks past the mmap threshold), tables' estimate %.1f MB, %zu columns\n",
                 malloc_live_bytes() / 1048576.0, malloc_mmap_bytes() / 1048576.0, est / 1048576.0,
                 [&] { size_t n = 0; for (const auto& kv : tables_) n += kv.second.col_data.size(); return n; }());
  }
#endif
  return true;
}

Result<void> SqlDatabase::save_tables() {
  if (root_path_.empty()) return Ok();
  save_views();
  return save_tables_v4();
}

// The row-per-cell format (store v3): kept to document what old stores hold;
// load_tables still reads it, save_tables no longer writes it.
Result<void> SqlDatabase::save_tables_v3_unused() {
  std::string file = root_path_ + "/schema_tables.db";
  Writer w;
  w.u32(kStoreMagicV3);
  w.u32((uint32_t)databases_.size());
  for (const auto& d : databases_) w.str(d);
  w.u32((uint32_t)tables_.size());
  for (auto& kv : tables_) {
    const SqlTable& t = kv.second;
    w.str(t.name);
    w.str(t.database);
    w.u32((uint32_t)t.columns.size());
    for (auto& col : t.columns) {
      w.str(col.name);
      // The type in the low byte; a DECIMAL's precision and scale in the next
      // two (the field was always 32 bits, so older stores read unchanged).
      w.u32((uint32_t)col.type | ((uint32_t)(col.binary & 3) << 6) | ((uint32_t)col.precision << 8) |
            ((uint32_t)col.scale << 16) | ((uint32_t)col.temporal << 24));
      w.u8(col.is_primary_key ? 1 : 0);
      w.u8(col.not_null ? 1 : 0);
      w.u8(col.auto_increment ? 1 : 0);
      w.str(col.default_value);
    }
    size_t rc = t.row_count();
    w.u32((uint32_t)rc);
    for (size_t ri = 0; ri < rc; ++ri) {
      w.u32((uint32_t)t.columns.size());
      for (size_t ci = 0; ci < t.columns.size(); ++ci) {
        w.str(t.cell(ri, ci));
      }
    }
  }

  return kernel_.host().write_file(file, ByteSpan(w.buf().data(), w.buf().size()));
}

Result<void> SqlDatabase::save_views() {
  std::string vfile = root_path_ + "/schema_views.db";
  Writer vw;
  vw.u32(kStoreMagicV3);
  vw.u32((uint32_t)views_.size());
  for (const auto& kv : views_) {
    const SqlView& v = kv.second;
    vw.str(v.name);
    vw.str(v.database);
    vw.str(v.query_sql);
    vw.str(v.definer);
    vw.str(v.algorithm);
    vw.str(v.security);
    vw.u32((uint32_t)v.column_aliases.size());
    for (const auto& ca : v.column_aliases) vw.str(ca);
  }
  return kernel_.host().write_file(vfile, ByteSpan(vw.buf().data(), vw.buf().size()));
}


// Where replication had got to when this store was last written. Without it a
// restart auto-discovers the CURRENT position, so every change made while the
// engine was down is skipped and never comes back: on chi-il-1 that left
// purchdata serving two-day-old supplier prices while its row count still
// matched, which reads as correct data right up until someone quotes from it.
Result<void> SqlDatabase::save_cdc_position() {
  if (root_path_.empty() || !binlog_consumer_) return Ok();
  // The last committed transaction, not the stream's cursor: a flush can land
  // between a transaction's row events and its commit, and resuming there
  // would skip the rest of it.
  std::string file; uint64_t pos = 0;
  if (!binlog_consumer_->committed_position(&file, &pos)) return Ok();
  std::string text = file + "\n" + std::to_string(pos) + "\n";
  return kernel_.host().write_file(root_path_ + "/cdc_position",
                                   ByteSpan((const uint8_t*)text.data(), text.size()));
}

bool SqlDatabase::load_cdc_position(std::string* out_file, uint64_t* out_pos) const {
  if (root_path_.empty()) return false;
  auto r = kernel_.host().read_file(root_path_ + "/cdc_position");
  if (!r.ok() || r->empty()) return false;
  std::string text((const char*)r->data(), r->size());
  size_t nl = text.find('\n');
  if (nl == std::string::npos) return false;
  *out_file = text.substr(0, nl);
  *out_pos = std::strtoull(text.c_str() + nl + 1, nullptr, 10);
  return !out_file->empty() && *out_pos > 0;
}

}  // namespace aios::userland
