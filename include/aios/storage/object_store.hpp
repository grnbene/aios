#pragma once
// Storage is objects, not paths. An object carries identity, payload,
// embedding, metadata, relationships, timestamp and provenance, on an
// append-only log. Paths are one interface to storage; this is another. The
// SemanticIndex answers find(similar); the RelationshipGraph answers
// find(related) and find(history).
#include <string>
#include "aios/hal/sync.hpp"
#include <memory>
#include <functional>
#include <vector>
#include <unordered_map>
#include <list>
#include "aios/core/ids.hpp"
#include "aios/core/result.hpp"
#include "aios/core/bytes.hpp"
#include "aios/hal/host.hpp"
#include "aios/state/state.hpp"

namespace aios {

struct ObjectRelation { ObjectID target; uint32_t kind; };

struct StoredObject {
  ObjectID id;
  uint64_t timestamp_ns = 0;
  EntityID provenance;                 // who produced it
  ObjectID derived_from;               // history chain
  StateID source_state;                // if this object snapshots a state
  std::vector<float> embedding;
  std::vector<std::pair<std::string, std::string>> metadata;
  std::vector<ObjectRelation> relations;
  ByteBuffer payload;

  std::string meta(const std::string& key) const;
  std::string compute_hash() const;
  void encode(Writer& w) const;
  static Result<StoredObject> decode(Reader& r, bool decompress_payload = true);
  bool is_expired(uint64_t now_ns = 0) const;
  bool is_tombstone() const;
  bool is_payload_compressed() const;
  ByteBuffer decompressed_payload() const;
  StoredObject decompressed() const;
};

struct VacuumStats {
  size_t total_objects_scanned = 0;
  size_t live_objects_written = 0;
  size_t dead_objects_pruned = 0;
  size_t expired_objects_pruned = 0;
  size_t tombstones_pruned = 0;
  size_t blocks_sealed = 0;       // blocks re-sealed in the compacted image
  uint64_t bytes_before = 0;
  uint64_t bytes_after = 0;
  uint64_t bytes_copied = 0;      // payload bytes actually rewritten
};

struct SimilarHit { ObjectID id; float score; };

// Similarity over the store's embeddings, two ways.
//
// The exact scan is O(N) and is the ANSWER: every vector, cosine, sorted. The
// approximate index is a navigable small-world graph and is a TRADE — sublinear
// at the cost of sometimes missing a neighbour — and which one runs is decided
// by the corpus size, because under a few hundred vectors the graph is slower
// AND less exact, which is the worst of both.
//
// `src/memory/hnsw_index.cpp` had been written and wired to nothing: the store's
// find_similar was the linear scan and always had been. Both are here now, and
// `store index` measures the graph's RECALL against the exact answer rather than
// asserting that an approximate index is fine.
class SemanticIndex {
 public:
  SemanticIndex();
  ~SemanticIndex();
  SemanticIndex(const SemanticIndex&) = delete;
  SemanticIndex& operator=(const SemanticIndex&) = delete;

  void add(const ObjectID& id, const std::vector<float>& v);
  void clear();
  // Whichever path the corpus size calls for.
  std::vector<SimilarHit> nearest(const std::vector<float>& q, size_t k) const;
  // Named explicitly, so a caller that needs the truth can have it and a caller
  // measuring the trade can compare them.
  std::vector<SimilarHit> nearest_exact(const std::vector<float>& q, size_t k) const;
  std::vector<SimilarHit> nearest_approx(const std::vector<float>& q, size_t k) const;
  bool approx_ready() const;
  size_t approx_size() const;
  // The exact rows, so a caller measuring recall can draw its probes from the
  // corpus itself rather than from vectors the index has never seen.
  std::vector<float> vector_at(size_t i) const;
  // Force building the graph now if not already built.
  void ensure_approx() const;
  // Above this many vectors the graph is used. The number is MEASURED, not
  // chosen — `store index bench` on this host, 64-d vectors, recall@5 against
  // the exact answer:
  //
  //     N        recall@5   exact scan   graph      graph/scan
  //     128      100%         5.2 us       4.3 us     1.2x   graph wins
  //     1024     100%        39.2 us       8.9 us     4.4x   graph wins
  //     8192      99.4%     374.1 us      38.4 us     9.7x   graph wins
  //     32768     98.8%    1517.5 us      56.1 us    27.0x   graph wins
  //
  // The scan is O(N) and the graph is not, so the crossover is real and it sits
  // at ~128-256 vectors. Below it the scan is faster AND exact, which makes
  // using the graph there unnecessary.
  static constexpr size_t kApproxFrom = 256;
  size_t size() const { return rows_.size(); }
 private:
  std::vector<std::pair<ObjectID, std::vector<float>>> rows_;
  std::vector<double> norms_;   // |row|, as the scan computes it, once per row instead of per query
  struct Approx;
  // Built on the first question that needs it, then kept current by add().
  // Building it eagerly was 85-95% of opening a store: 44k HNSW inserts on
  // every boot, 1.4-4.4 s, for a graph most boots never query.
  mutable std::unique_ptr<Approx> approx_;
};

class RelationshipGraph {
 public:
  void add(const ObjectID& from, const ObjectID& to, uint32_t kind);
  void clear();
  std::vector<ObjectRelation> out(const ObjectID& id) const;
  std::vector<ObjectRelation> in(const ObjectID& id) const;
 private:
  std::unordered_map<ObjectID, std::vector<ObjectRelation>, TypedIdHash<9>> out_, in_;
};

enum class ResidencyClass : uint8_t {
  Pinned = 0,
  Warm = 1,
  Cool = 2,
  Sealed = 3
};

struct ResidencyConfig {
  uint32_t block_objects = 2048;  // Object threshold per sealed segment
  uint32_t max_active = 16384;    // Max active index entries
  uint64_t hot_budget = 512ull << 20;     // Memory cap before eviction

  static constexpr ResidencyConfig micro_harness() {
    return ResidencyConfig{
      .block_objects = 8,
      .max_active = 16,
      .hot_budget = 64 * 1024  // 64 KB
    };
  }
};

inline constexpr ResidencyConfig test_config {
  .block_objects = 8,
  .max_active = 16,
  .hot_budget = 64 * 1024  // 64 KB
};

class ObjectStore {
 public:
  ObjectStore(hal::Host& host, IdGenerator& ids, std::string directory,
              const ResidencyConfig& cfg = {});
  ~ObjectStore();
  ObjectStore(const ObjectStore&) = delete;
  ObjectStore& operator=(const ObjectStore&) = delete;
  Result<void> open();
  // The log is shared: other kernel processes on the same root append to it
  // while this one runs (the resident writes, the desktop reads). Consume any
  // whole records that have landed since open()/the last refresh, so a reader
  // sees them without reopening. Returns how many new objects were indexed.
  Result<size_t> refresh();
  Result<ObjectID> put(StoredObject obj);   // id assigned if null
  // Many objects, ONE append and ONE fdatasync. put() pays a full durability
  // barrier per object (~585us on a real disk), so a 20k-row ERP import cost
  // 11.7s of almost pure fsync; the same import through here is one barrier.
  // The batch is atomic in the sense that matters to a reader: a torn tail is
  // discarded whole by refresh(), so either a record is complete or unseen.
  Result<std::vector<ObjectID>> put_many(std::vector<StoredObject> objs);
  Result<StoredObject> get(const ObjectID& id) const;
  // Read many objects in place, under ONE lock, without copying them. get()
  // copies the whole object — every metadata string — and takes the lock per
  // call, so a filter over 350k candidates spent its time copying rows it was
  // about to reject. `fn` must not call back into the store.
  void visit(const std::vector<ObjectID>& ids,
             const std::function<void(const StoredObject&)>& fn) const;
  std::vector<SimilarHit> find_similar(const std::vector<float>& embedding, size_t k) const;
  // The index itself, so a caller can measure the approximate path against the
  // exact one rather than take the trade on trust.
  size_t semantic_size() const;
  size_t semantic_approx_size() const;
  bool semantic_approx_ready() const;
  std::vector<SimilarHit> semantic_exact(const std::vector<float>& q, size_t k) const;
  std::vector<SimilarHit> semantic_approx(const std::vector<float>& q, size_t k) const;
  std::vector<float> semantic_vector_at(size_t i) const;
  std::vector<ObjectRelation> find_related(const ObjectID& id) const;   // both directions
  std::vector<ObjectID> find_history(const ObjectID& id) const;         // derived_from chain, newest first
  Result<void> verify_provenance_chain(const ObjectID& id) const;       // cryptographic tamper-evidence verification across parentage
  std::vector<ObjectID> find_meta(const std::string& key, const std::string& value) const;
  // Whether this key has a usable value index. A key whose values are long
  // (a query string, a narrative) carries no index, because indexing it costs
  // a full copy of every value to answer an equality nobody asks for. Callers
  // that SEED a scan from the index must ask this first: with no index an
  // empty result means "scan instead", not "nothing matched".
  bool meta_indexed(const std::string& key) const;
  // An UPPER BOUND on how many objects `key`=`value` matches: the size of the
  // hash bucket, without verifying any of it. A query planner choosing which
  // predicate to seed from needs to compare selectivities, not to be right
  // about them — and verification costs a pass over the bucket, which is the
  // very work the planner is trying to decide whether to do.
  size_t count_meta_approx(const std::string& key, const std::string& value) const;

  // --- ordered access over numeric metadata ---------------------------------
  // The hash index answers equality and nothing else, so `ORDER BY meta.price
  // DESC LIMIT 10` had to materialise every object, parse every price, sort,
  // and discard all but ten — 73ms where MariaDB walks a B-tree backwards and
  // stops after ten rows in 0.26ms. These give the same shape of access.
  //
  // The ordering is built from the value index, which already holds every
  // distinct value and its ids, so building it reads no objects at all. It is
  // built on first use per key and rebuilt only after new objects land.
  bool numeric_ordered(const std::string& key) const;
  // Ids in value order. `desc` walks from the largest. Stops once `limit` ids
  // have been produced (0 means all). False when the key has no usable order.
  bool order_by_meta(const std::string& key, bool desc, size_t limit,
                     std::vector<ObjectID>& out) const;
  // Ids whose value satisfies `op` against `bound` ("<", "<=", ">", ">=").
  bool range_meta(const std::string& key, const std::string& op, double bound,
                  std::vector<ObjectID>& out) const;
  // How many objects a meta equality would match, WITHOUT materialising them.
  // A query planner needs this to choose which of several predicates to seed
  // the scan from: seeding on the rarest one turns an N-object scan into a
  // scan of just its matches.
  size_t count_meta(const std::string& key, const std::string& value) const;
  // find_meta/count_meta treat an empty `value` as "any value for this key",
  // which is the right thing for a wildcard and the WRONG thing for a query
  // asking for the rows whose value IS empty. In real ERP data that is not an
  // edge case — `debtorno = ''` covers most of a stock ledger — and the
  // planner, unable to see an exact count for it, fell back to scanning the
  // whole corpus. These say what they mean: exact match, empty included.
  std::vector<ObjectID> find_meta_exact(const std::string& key, const std::string& value) const;
  size_t count_meta_exact(const std::string& key, const std::string& value) const;
  std::vector<ObjectID> find_state(const StateID& state) const;         // snapshots of a state
  std::vector<ObjectID> list() const;
  size_t count() const;
  // How much the store may keep decoded in memory, and how much it is using.
  // A machine with room says so; a machine without one still answers every
  // query, out of the log.
  void set_hot_budget(size_t bytes);
  size_t hot_budget() const;
  size_t hot_bytes() const;
  size_t hot_count() const;
  const std::string& directory() const { return dir_; }

  const ResidencyConfig& residency_config() const;
  void set_residency_config(const ResidencyConfig& cfg);
  void pin(const EntityID& entity);
  void demote(const EntityID& entity);
  void set_residency_class(const EntityID& entity, ResidencyClass rclass);
  ResidencyClass residency_class(const EntityID& entity) const;
  size_t evict_entity(const EntityID& entity);
  size_t hot_count(const EntityID& entity) const;
  size_t hot_bytes(const EntityID& entity) const;
  uint64_t entity_last_access(const EntityID& entity) const;

  // Sparse block index for cold segments.
  // When active entries exceed active threshold (or seal_cold_blocks() is called),
  // historical segments are sealed into compact SparseBlock descriptors in RAM
  // (~64 bytes per 2,048 objects), dropping per-object entries_ map nodes.
  struct SparseBlock {
    uint64_t file_offset = 0;
    uint64_t file_len = 0;
    uint32_t object_count = 0;
    uint64_t min_timestamp_ns = 0;
    uint64_t max_timestamp_ns = 0;
    ObjectID first_id;
    ObjectID last_id;
    // The id range this block covers. IdGenerator puts a monotonic counter in
    // the high bits of `lo`, so within a boot `lo` rises with allocation order
    // and the log is written in that order — which makes this range an almost
    // exact filter for free.
    //
    // These are the TRUE min and max as the block fills, not the first and
    // last id seen. Across a reboot the counter restarts under fresh entropy,
    // so a block straddling that boundary is not ordered; first/last would
    // then describe a range that EXCLUDES ids the block really holds, and the
    // lookup would answer "not found" for an object that is right there.
    // Widening to min/max keeps the filter conservative: it may admit a block
    // that lacks the id (a wasted read) but never rejects one that has it.
    uint64_t min_lo = ~0ull;
    uint64_t max_lo = 0;

    // Where each object in this block lies, sorted by id.lo. Sealing used to
    // drop every per-object offset, so the only way to reach one record was to
    // read the whole block and scan it: fetching the ten dearest rows, which
    // are scattered across every block, read ~50MB to return 10 objects and
    // cost 25ms against MariaDB's 0.26ms. Twenty bytes an object buys exact
    // placement back — a fifth of what the entries_ map node cost, and the
    // reason a sealed store can still answer a top-N.
    struct Loc {
      uint64_t lo;        // id.lo, the sort key
      uint64_t hi;        // id.hi -- without this a locator can find a SLOT by
                          // lo but cannot hand back a full ObjectID on its own;
                          // every caller so far already had the id it was
                          // searching for and only needed the slot. A
                          // persisted index has no such caller to fall back on.
      uint64_t offset;    // body offset in objects.log
      uint32_t len;       // body length
    };
    std::vector<Loc> locs;

    const Loc* find(uint64_t id_lo) const {
      size_t a = 0, b = locs.size();
      while (a < b) {
        const size_t m = a + (b - a) / 2;
        if (locs[m].lo < id_lo) a = m + 1; else b = m;
      }
      return (a < locs.size() && locs[a].lo == id_lo) ? &locs[a] : nullptr;
    }

    // A 2048-bit filter, 4 bits per id. The previous one was 64 bits for 2048
    // objects — 8192 bit-sets into 64 slots, which saturates every bit after
    // the first few dozen ids and answers "maybe" to everything. Measured at
    // 2048 objects/block: 64 of 64 bits set, 100% false positives, so every
    // cold read scanned every sealed block. A sampled readback of 300 objects
    // then read 1TB from a 235MB log without finishing.
    static constexpr size_t kBloomWords = 32;          // 2048 bits
    uint64_t bloom[kBloomWords] = {};

    void mark(uint64_t h) { bloom[(h >> 6) % kBloomWords] |= (1ull << (h & 63)); }
    bool marked(uint64_t h) const { return bloom[(h >> 6) % kBloomWords] & (1ull << (h & 63)); }
    static void hashes(const ObjectID& id, uint64_t out[4]) {
      const uint64_t a = id.lo * 0x9E3779B97F4A7C15ull;
      const uint64_t b = id.hi * 0xC2B2AE3D27D4EB4Full;
      out[0] = a ^ (a >> 29);
      out[1] = b ^ (b >> 31);
      out[2] = (a + b) * 0xD6E8FEB86659FD93ull;
      out[3] = (a ^ b) * 0xA24BAED4963EE407ull;
    }

    void add_bloom(const ObjectID& id) {
      if (id.lo < min_lo) min_lo = id.lo;
      if (id.lo > max_lo) max_lo = id.lo;
      uint64_t h[4];
      hashes(id, h);
      for (uint64_t x : h) mark(x);
    }

    bool may_contain(const ObjectID& id) const {
      // The range rejects most blocks outright, before any hashing.
      if (id.lo < min_lo || id.lo > max_lo) return false;
      uint64_t h[4];
      hashes(id, h);
      for (uint64_t x : h) if (!marked(x)) return false;
      return true;
    }
  };

  size_t cold_blocks_count() const;
  size_t cold_objects_count() const;
  size_t active_count() const;
  void seal_cold_blocks();
  void set_active_threshold(size_t max_active);
  Result<VacuumStats> vacuum();

 private:
  // Where a record's body sits in objects.log, and the two fields a query
  // needs before it has decided to read the body at all. THIS is what stays
  // resident per object — about 40 bytes — instead of the whole decoded
  // object, whose metadata strings and payload ran to ~1.9KB on a real ERP
  // row. The log is mapped, so reading a body is a decode out of the page
  // cache rather than an allocation that never goes away.
  struct Entry {
    uint64_t body_offset = 0;
    uint32_t body_len = 0;
    uint64_t timestamp_ns = 0;
  };

  // The order_/graph_/semantic_/meta-index side effects of indexing an
  // object, with no opinion about where its bytes live. Shared by a live
  // decode (index()) and a persisted-header restore, so both paths produce
  // identical index state.
  void apply_metadata_locked(const StoredObject& o);

  // Drops the hot-cache copies of any older version of o's key. Caller holds
  // the lock. Cache maintenance only: the entries and indexes still resolve
  // those ids, so nothing a reader can ask for changes -- only what is
  // resident.
  void drop_superseded_hot_locked(const StoredObject& o);

  Result<void> index(const StoredObject& o, uint64_t body_offset, uint32_t body_len,
                     uint64_t rec_offset, uint32_t rec_len);
  Result<size_t> refresh_locked();
  // The raw decode of one record, read from the log by range. No caching.
  Result<StoredObject> decode_locked(const Entry& e) const;
  Result<StoredObject> find_in_cold_blocks_locked(const ObjectID& id) const;
  // Last resort when no locator places the id: read blocks and scan them.
  Result<StoredObject> find_in_cold_blocks_scan_locked(const ObjectID& id) const;
  // Which sealed block holds `id`, or cold_blocks_.size() when none does.
  size_t cold_block_for_locked(const ObjectID& id) const;
  void seal_current_block_locked();
  // The object itself: from the hot set when it is there, decoded out of the
  // mapping and kept when it is not. Returns a pointer INTO the hot set, so a
  // reader that only needs to look at an object copies nothing. Null when the
  // record cannot be read. Caller holds the lock; the pointer is good until
  // the next call that can evict.
  const StoredObject* hot_ref_locked(const ObjectID& id, const Entry& e) const;
  // Roughly what an object costs to keep, for the hot set's budget.
  static size_t footprint_of(const StoredObject& o);
  void evict_locked() const;

  hal::Host& host_;
  IdGenerator& ids_;
  std::string dir_;
  uint64_t consumed_ = 0;   // bytes of objects.log already replayed into memory
  uint64_t blocks_idx_consumed_ = 0;   // bytes of blocks.idx already loaded into known_spans_
  mutable hal::Mutex mu_;
  std::unordered_map<ObjectID, Entry, TypedIdHash<9>> entries_;

  // Residency and tiering configuration
  ResidencyConfig config_;
  std::unordered_map<EntityID, ResidencyClass, TypedIdHash<1>> entity_residency_;
  mutable std::unordered_map<EntityID, uint64_t, TypedIdHash<1>> entity_last_access_ns_;

  // Sparse block index for cold historical segments
  std::vector<SparseBlock> cold_blocks_;
  size_t cold_objects_count_ = 0;
  // list() used to re-read, re-decode and re-checksum every sealed block in
  // objects.log on EVERY call. The desktop calls it twice a frame, so a 20 MB
  // store meant ~2.4 GB/s of fnv1a and the compositor sat at 100% of a core
  // painting a still screen. Sealed blocks are immutable and append-only, so
  // their id list is cached and extended, never rebuilt; vacuum() is the only
  // thing that rewrites them and it bumps the epoch to drop the cache.
  mutable std::vector<ObjectID> cold_ids_cache_;
  mutable size_t cold_ids_cached_blocks_ = 0;
  mutable uint64_t cold_ids_cache_epoch_ = 0;
  uint64_t cold_epoch_ = 0;
  // Ids of the sealed blocks from `first` onward, appended to `out`.
  void append_cold_ids_locked(std::vector<ObjectID>& out) const;
  SparseBlock cur_block_;
  std::vector<ObjectID> cur_block_ids_;
  // One header per object in cur_block_, same order as cur_block_ids_ before
  // the seal-time sort. Persisted so a restart can rebuild the metadata,
  // graph, and semantic indexes without decoding a payload.
  std::vector<ByteBuffer> cur_block_headers_;
  // Where `id` sits in a sealed block, or nullptr when no block holds it.
  const SparseBlock::Loc* locate_cold_locked(const ObjectID& id, size_t* block_out) const;

  void recover_compacting_locked();

  // A sealed block's location in objects.log, known BEFORE its data has been
  // decoded -- the thing that makes skipping the decode possible at all. Read
  // once from blocks.idx at open(), consulted before a record at a given
  // offset is decoded.
  struct SkipSpan {
    uint64_t data_offset = 0;
    uint64_t data_len = 0;
    uint64_t footer_offset = 0;
    uint64_t footer_len = 0;
  };
  std::vector<SkipSpan> known_spans_;
  // Which known_spans_ entries this process has already restored, so a
  // live-writing process's own later refresh_locked() calls do not re-apply a
  // block's metadata a second time.
  std::vector<bool> span_consumed_;
  void load_known_spans_locked();
  // Attempts to skip-restore the span starting at `data_offset` using its
  // persisted footers. Returns true and advances `consumed_` past the whole
  // span (data + both footers) on success; returns false and touches nothing
  // if the span is unknown, invalid, or fails its spot-check -- the ordinary
  // per-record decode path is always the fallback.
  Result<bool> try_skip_restore_locked(uint64_t data_offset);

  // RAM first, disk second. Objects that are actually being read stay decoded
  // in here and cost nothing to look at again; the rest live in the log and
  // are decoded on demand. Without this every read of every candidate decoded
  // afresh, which is the whole store on disk and none of it in memory — the
  // opposite mistake from keeping all of it resident.
  struct HotEntry {
    StoredObject obj;
    size_t bytes = 0;
    std::list<ObjectID>::iterator lru;
  };
  mutable std::unordered_map<ObjectID, HotEntry, TypedIdHash<9>> hot_;
  mutable std::list<ObjectID> lru_;         // front = most recently read
  mutable size_t hot_bytes_ = 0;
  std::vector<ObjectID> order_;
  // find_meta used to walk every object and every one of its metadata pairs.
  // On 20k ERP rows carrying 9 keys each that is 180k string compares for a
  // point lookup, which is what made WHERE meta.x = y cost milliseconds. These
  // two maps make it a hash lookup; both keep insertion order, so the results
  // come back in the same order the scan produced them.
  // Both indexes hold 4-byte ordinals into order_ (the object's position in
  // arrival order), not 16-byte ids: an object in N indexed keys cost 16*N
  // bytes of repeated id here, now 4*N. order_ is the one table of ids.
  std::unordered_map<std::string, std::vector<uint32_t>> meta_key_index_;
  // The value index. Keyed by the value itself, so a hit is exact and needs no
  // second look at the object — which matters now that objects live in the
  // mapped log and "look at the object" means decoding one.
  //
  // What made this expensive was never short values; it was columns where
  // nearly every row has its own (a crypto hash, a timestamp to the second, an
  // autoincrement id). There the index is one bucket per row: a full copy of
  // the column, to answer a lookup that returns a single row. Those keys are
  // dropped and left to the scan.
  struct ValueIndex {
    bool active = true;
    std::unordered_map<std::string, std::vector<uint32_t>> by_value;   // ordinals into order_
  };
  std::unordered_map<std::string, ValueIndex> meta_index_;

  // value -> ids, in ascending value order. Sorted vector rather than a tree:
  // 24 bytes an entry against a map node's 48+, and it is only ever rebuilt
  // wholesale, never point-updated.
  struct NumericIndex {
    std::vector<std::pair<double, ObjectID>> sorted;
    bool built = false;
  };
  mutable std::unordered_map<std::string, NumericIndex> numeric_index_;
  // Bumped whenever an object is indexed; a numeric index built at an older
  // revision is stale and gets rebuilt on next use.
  uint64_t index_revision_ = 0;
  mutable std::unordered_map<std::string, uint64_t> numeric_built_at_;
  const NumericIndex* ensure_numeric_locked(const std::string& key) const;
  // Values longer than this are not equality-filter targets; indexing them
  // costs far more than the scan it would save.
  static constexpr size_t kMaxIndexedValue = 64;
  // Below this many distinct values a key is left alone: a small index is
  // cheap whatever the shape of the data. Past it, a key whose values are
  // more than half distinct is judged near-unique and dropped.
  static constexpr size_t kJudgeCardinalityFrom = 50000;
  // The ids for `key`=`value`, from the index when the key has one and by
  // scanning the objects that carry the key when it does not. Caller holds
  // the lock.
  std::vector<ObjectID> find_meta_locked(const std::string& key, const std::string& value,
                                         bool count_only, size_t* out_count) const;
  SemanticIndex semantic_;
  RelationshipGraph graph_;
};

}  // namespace aios
