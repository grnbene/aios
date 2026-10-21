#include "aios/storage/object_store.hpp"
#include "aios/crypto/sha256.hpp"
#include "aios/core/strbuf.hpp"
#include "aios/memory/hnsw_index.hpp"
#include "aios/hal/sync.hpp"
#include <cmath>
#include <algorithm>
#include <cctype>
#include <unordered_set>
#if !defined(AIOS_METAL) && defined(__GLIBC__)
#include <malloc.h>
#endif
#if defined(AIOS_HAVE_LIBDEFLATE)
#include <libdeflate.h>
#endif
#if defined(AIOS_HAVE_ZLIB)
#include "aios/net/gzip.hpp"
#endif

namespace aios {

// Freeing a container is not the same as giving the machine its memory back.
// glibc holds freed small chunks in its arena for reuse, so a vacuum that
// dropped hundreds of megabytes of dead objects showed up as no change at all
// in RSS -- which is exactly what "vacuum never frees RAM" meant on chi. This
// is the call that hands the arena's free pages back to the kernel. Not
// available (or meaningful) on the bare-metal substrate, which has no
// hosted allocator to trim.

static constexpr uint32_t OBJ_MAGIC = 0x4F534F41u;  // 'AOSO'
// A sealed block's index, persisted so a fresh boot can verify what it just
// re-derived from the log instead of trusting it blind. This does NOT let
// replay skip decoding a sealed block's bodies -- meta_key_index_/meta_index_/
// numeric_index_ are built from those bodies during the SAME decode pass, and
// sealing is a deterministic function of replay order, so a fresh process
// re-derives the identical SparseBlock before it ever reaches this footer.
// What the footer buys is durability: a crash-safe record of what sealing
// produced, checked against what replay produces on its own. A disagreement
// is corruption or a determinism bug, not something to paper over.
static constexpr uint32_t BLOCK_FOOTER_MAGIC = 0x5345414Cu;  // 'SEAL'
// The header dump that lets a restart rebuild order_/meta_key_index_/
// meta_index_/graph_/semantic_ for a sealed block without decoding its
// payload bytes. Written immediately after BLOCK_FOOTER_MAGIC at seal time.
static constexpr uint32_t META_FOOTER_MAGIC = 0x4D455441u;    // 'META'

std::string StoredObject::meta(const std::string& key) const { for (auto& [k, v] : metadata) if (k == key) return v; return {}; }

static uint64_t parse_u64_safe(const std::string& s) {
  if (s.empty()) return 0;
  const char* end = nullptr;
  unsigned long long v = aios::to_u64(s.c_str(), &end);
  return (end != nullptr && end != s.c_str()) ? static_cast<uint64_t>(v) : 0;
}

bool StoredObject::is_expired(uint64_t now_ns) const {
  if (now_ns == 0) return false;
  std::string exp_str = meta("expires_at_ns");
  if (!exp_str.empty()) {
    uint64_t exp = parse_u64_safe(exp_str);
    if (exp > 0 && now_ns >= exp) return true;
  }
  std::string exp_at = meta("expires_at");
  if (!exp_at.empty()) {
    uint64_t exp = parse_u64_safe(exp_at);
    if (exp > 0) {
      if (exp < 100000000000ULL) exp *= 1000000000ULL;
      if (now_ns >= exp) return true;
    }
  }
  std::string ttl_str = meta("ttl");
  if (!ttl_str.empty()) {
    uint64_t ttl_s = parse_u64_safe(ttl_str);
    if (ttl_s > 0 && timestamp_ns > 0 && now_ns >= (timestamp_ns + ttl_s * 1000000000ULL)) return true;
  }
  return false;
}

bool StoredObject::is_tombstone() const {
  std::string s = meta("status");
  if (s == "__aios_tombstone__") return true;
  std::string k = meta("kind");
  if (k == "tombstone" || k == "__aios_tombstone__") return true;
  std::string t = meta("__aios_tombstone__");
  if (t == "1" || t == "true") return true;
  std::string tb = meta("tombstone");
  if (tb == "1" || tb == "true") return true;
  std::string del = meta("deleted");
  if (del == "1" || del == "true") return true;
  if (payload.size() == 18 && std::string(reinterpret_cast<const char*>(payload.data()), payload.size()) == "__aios_tombstone__") return true;
  return false;
}

namespace {
// A payload stored compressed: "ASOZ", its plain size (u32), then a raw deflate stream (encode() writes one
// where it saves bytes).
constexpr uint32_t kPayloadZMagic = 0x5A4F5341u;

// The `plain` bytes a compressed payload's stream inflates to, into `out`; false when it does not inflate to
// exactly that many (a payload that only happens to start with the magic stays as it is).
bool inflate_payload(const uint8_t* z, size_t n, uint32_t plain, ByteBuffer& out) {
  if (plain == 0 || plain > (1u << 28)) return false;
#if defined(AIOS_HAVE_LIBDEFLATE)
  struct D {
    libdeflate_decompressor* d = libdeflate_alloc_decompressor();
    ~D() { if (d) libdeflate_free_decompressor(d); }
  };
  thread_local D dec;
  if (!dec.d) return false;
  out.resize(plain);
  size_t got = 0;
  return libdeflate_deflate_decompress(dec.d, z, n, out.data(), plain, &got) == LIBDEFLATE_SUCCESS && got == plain;
#elif defined(AIOS_HAVE_ZLIB)
  size_t consumed = 0;
  out.clear();
  return net::zlib_inflate_raw(ByteSpan(z, n), &out, &consumed, plain) && out.size() == plain;
#else
  (void)z; (void)n; (void)out;
  return false;
#endif
}

bool payload_is_compressed(const uint8_t* p, size_t n) {
  if (n < 8) return false;
  uint32_t magic = 0;
  std::memcpy(&magic, p, 4);
  return magic == kPayloadZMagic;
}

uint32_t compressed_plain_size(const uint8_t* p) {
  uint32_t plain = 0;
  std::memcpy(&plain, p + 4, 4);
  return plain;
}
}  // namespace

bool StoredObject::is_payload_compressed() const { return payload_is_compressed(payload.data(), payload.size()); }

ByteBuffer StoredObject::decompressed_payload() const {
  if (!is_payload_compressed()) return payload;
  ByteBuffer out;
  if (inflate_payload(payload.data() + 8, payload.size() - 8, compressed_plain_size(payload.data()), out)) return out;
  return payload;
}

StoredObject StoredObject::decompressed() const {
  if (!is_payload_compressed()) return *this;
  StoredObject c = *this;
  c.payload = decompressed_payload();
  return c;
}

std::string StoredObject::compute_hash() const {
  ByteBuffer buf;
  Writer w(buf);
  w.id(id); w.u64(timestamp_ns); w.id(provenance); w.id(derived_from); w.id(source_state);
  w.u32((uint32_t)embedding.size()); for (float f : embedding) w.f32(f);
  uint32_t meta_count = 0;
  for (auto& [k, v] : metadata) if (k != "crypto.hash") meta_count++;
  w.u32(meta_count);
  for (auto& [k, v] : metadata) { if (k != "crypto.hash") { w.str(k); w.str(v); } }
  w.u32((uint32_t)relations.size()); for (auto& r : relations) { w.id(r.target); w.u32(r.kind); }
  if (is_payload_compressed()) {
    ByteBuffer dec = decompressed_payload();
    w.bytes(dec);
  } else {
    w.bytes(payload);
  }
  auto digest = crypto::sha256(ByteSpan(buf.data(), buf.size()));
  return crypto::bytes_to_hex(ByteSpan(digest.data(), digest.size()));
}

void StoredObject::encode(Writer& w) const {
  w.id(id); w.u64(timestamp_ns); w.id(provenance); w.id(derived_from); w.id(source_state);
  w.u32((uint32_t)embedding.size()); for (float f : embedding) w.f32(f);
  w.u32((uint32_t)metadata.size()); for (auto& [k, v] : metadata) { w.str(k); w.str(v); }
  w.u32((uint32_t)relations.size()); for (auto& r : relations) { w.id(r.target); w.u32(r.kind); }
  if (is_payload_compressed()) {
    w.bytes(payload);
    return;
  }
#if defined(AIOS_HAVE_LIBDEFLATE)
  if (payload.size() >= 64) {
    struct C {
      libdeflate_compressor* c = libdeflate_alloc_compressor(5);
      ~C() { if (c) libdeflate_free_compressor(c); }
    };
    thread_local C comp;
    if (comp.c) {
      ByteBuffer comp_buf;
      comp_buf.resize(8 + payload.size());
      std::memcpy(comp_buf.data(), &kPayloadZMagic, 4);
      uint32_t orig_sz = (uint32_t)payload.size();
      std::memcpy(comp_buf.data() + 4, &orig_sz, 4);
      size_t stored = libdeflate_deflate_compress(comp.c, payload.data(), payload.size(), comp_buf.data() + 8, payload.size());
      if (stored > 0 && 8 + stored < payload.size()) {
        comp_buf.resize(8 + stored);
        w.bytes(ByteSpan(comp_buf.data(), comp_buf.size()));
        return;
      }
    }
  }
#endif
  w.bytes(payload);
}
Result<StoredObject> StoredObject::decode(Reader& r, bool decompress_payload) {
  StoredObject o;
  o.id = ObjectID{r.id()}; o.timestamp_ns = r.u64(); o.provenance = EntityID{r.id()}; o.derived_from = ObjectID{r.id()}; o.source_state = StateID{r.id()};
  uint32_t n = r.u32(); if (!r.ok() || n > (1u << 24)) return Result<StoredObject>(Status::Corrupt, "embedding");
  o.embedding.resize(n); for (uint32_t i = 0; i < n; ++i) o.embedding[i] = r.f32();
  n = r.u32(); if (!r.ok() || n > (1u << 20)) return Result<StoredObject>(Status::Corrupt, "metadata");
  for (uint32_t i = 0; i < n; ++i) { std::string k = r.str(); std::string v = r.str(); o.metadata.emplace_back(k, v); }
  n = r.u32(); if (!r.ok() || n > (1u << 20)) return Result<StoredObject>(Status::Corrupt, "relations");
  for (uint32_t i = 0; i < n; ++i) { ObjectRelation rel; rel.target = ObjectID{r.id()}; rel.kind = r.u32(); o.relations.push_back(rel); }
  auto p = r.bytes();
  if (!r.ok()) return Result<StoredObject>(Status::Corrupt, "object record");
  if (decompress_payload && payload_is_compressed(p.data(), p.size()) &&
      inflate_payload(p.data() + 8, p.size() - 8, compressed_plain_size(p.data()), o.payload))
    return o;
  o.payload.assign(p.begin(), p.end());
  if (!r.ok()) return Result<StoredObject>(Status::Corrupt, "object record");
  return o;
}

// The graph lives behind a pointer so the header does not have to carry the
// index's own types into everything that includes the store.
struct SemanticIndex::Approx {
  std::unique_ptr<memory::HNSWVectorIndex> index;
  size_t dimension = 0;
};

SemanticIndex::SemanticIndex() : approx_(std::make_unique<Approx>()) {}
SemanticIndex::~SemanticIndex() = default;

bool SemanticIndex::approx_ready() const { return approx_ && approx_->index != nullptr; }
std::vector<float> SemanticIndex::vector_at(size_t i) const { return i < rows_.size() ? rows_[i].second : std::vector<float>{}; }
size_t SemanticIndex::approx_size() const { return approx_ready() ? approx_->index->size() : 0; }

void SemanticIndex::add(const ObjectID& id, const std::vector<float>& v) {
  if (v.empty()) return;
  rows_.emplace_back(id, v);
  double vn = 0;
  for (float f : v) vn += double(f) * f;
  norms_.push_back(std::sqrt(vn));
  // A graph that exists is kept current; one that does not is built from
  // rows_ when first asked for.
  if (approx_ && approx_->index && v.size() == approx_->dimension) (void)approx_->index->insert(id, v);
}

void SemanticIndex::ensure_approx() const {
  if (!approx_ || approx_->index || rows_.empty()) return;
  // The dimension comes from the first vector; a machine's embeddings are
  // whatever its embedder produces. A vector of another width stays in the
  // exact rows, where the scan skips mismatches anyway.
  approx_->dimension = rows_.front().second.size();
  approx_->index = std::make_unique<memory::HNSWVectorIndex>(approx_->dimension);
  for (const auto& [id, v] : rows_)
    if (v.size() == approx_->dimension) (void)approx_->index->insert(id, v);
}

std::vector<SimilarHit> SemanticIndex::nearest(const std::vector<float>& q, size_t k) const {
  if (rows_.size() >= kApproxFrom) {
    ensure_approx();
    if (approx_ready() && q.size() == approx_->dimension) {
      auto hits = nearest_approx(q, k);
      // A graph that answers nothing is not an answer. Falling back to the scan
      // is slower and correct, which is the right way round.
      if (!hits.empty()) return hits;
    }
  }
  return nearest_exact(q, k);
}

std::vector<SimilarHit> SemanticIndex::nearest_approx(const std::vector<float>& q, size_t k) const {
  ensure_approx();
  std::vector<SimilarHit> hits;
  if (!approx_ready() || q.size() != approx_->dimension) return hits;
  for (const auto& [id, dist] : approx_->index->search_knn(q, k)) {
    // The graph works in cosine DISTANCE and the store reports cosine
    // SIMILARITY; one is one minus the other and mixing them up would sort the
    // answer backwards.
    hits.push_back({id, 1.0f - dist});
  }
  return hits;
}

std::vector<SimilarHit> SemanticIndex::nearest_exact(const std::vector<float>& q, size_t k) const {
  std::vector<SimilarHit> hits;
  double qn = 0; for (float f : q) qn += double(f) * f; qn = std::sqrt(qn);
  if (qn == 0 || k == 0) return hits;
  // Only the k best are kept, worst on top, instead of scoring every row into
  // a vector and sorting all of it. Equal scores go to the earlier row, so a
  // corpus full of repeated vectors answers the same way every time rather
  // than in whatever order the sort left the ties.
  struct Scored { float score; size_t row; };
  auto better = [](const Scored& a, const Scored& b) { return a.score > b.score || (a.score == b.score && a.row < b.row); };
  std::vector<Scored> top;   // a heap under `better`: the worst kept hit is top.front()
  top.reserve(k + 1);
  for (size_t r = 0; r < rows_.size(); ++r) {
    const auto& v = rows_[r].second;
    if (v.size() != q.size()) continue;
    const double vn = norms_[r];
    if (vn == 0) continue;
    // Four independent sums let the multiplies overlap instead of each
    // waiting on the last add. Different rounding order from one sum, but in
    // double and cast to float: the scores come out the same.
    const size_t d = v.size();
    const float* a = v.data();
    const float* b = q.data();
    double s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    size_t i = 0;
    for (; i + 4 <= d; i += 4) {
      s0 += double(a[i]) * b[i];
      s1 += double(a[i + 1]) * b[i + 1];
      s2 += double(a[i + 2]) * b[i + 2];
      s3 += double(a[i + 3]) * b[i + 3];
    }
    for (; i < d; ++i) s0 += double(a[i]) * b[i];
    const double dot = (s0 + s1) + (s2 + s3);
    const Scored s{(float)(dot / (qn * vn)), r};
    if (top.size() < k) { top.push_back(s); std::push_heap(top.begin(), top.end(), better); }
    else if (better(s, top.front())) {
      std::pop_heap(top.begin(), top.end(), better); top.back() = s; std::push_heap(top.begin(), top.end(), better);
    }
  }
  std::sort(top.begin(), top.end(), better);
  for (const auto& s : top) hits.push_back({rows_[s.row].first, s.score});
  return hits;
}

void SemanticIndex::clear() {
  // Swapped, not cleared: a cleared vector keeps its capacity, which is the
  // memory a vacuum is trying to give back.
  decltype(rows_) gone; gone.swap(rows_);
  decltype(norms_) gone_norms; gone_norms.swap(norms_);
  approx_ = std::make_unique<Approx>();
}

void RelationshipGraph::add(const ObjectID& from, const ObjectID& to, uint32_t kind) { out_[from].push_back({to, kind}); in_[to].push_back({from, kind}); }
void RelationshipGraph::clear() {
  decltype(out_) gone_out; gone_out.swap(out_);
  decltype(in_) gone_in;   gone_in.swap(in_);
}
std::vector<ObjectRelation> RelationshipGraph::out(const ObjectID& id) const { auto it = out_.find(id); return it == out_.end() ? std::vector<ObjectRelation>{} : it->second; }
std::vector<ObjectRelation> RelationshipGraph::in(const ObjectID& id) const { auto it = in_.find(id); return it == in_.end() ? std::vector<ObjectRelation>{} : it->second; }

ObjectStore::ObjectStore(hal::Host& host, IdGenerator& ids, std::string directory,
                         const ResidencyConfig& cfg)
    : host_(host), ids_(ids), dir_(std::move(directory)), config_(cfg) {}

ObjectStore::~ObjectStore() = default;

Result<StoredObject> ObjectStore::decode_locked(const Entry& e) const {
  // One record, read where it lies. A mapping was the wrong instrument: on
  // bare metal a persistent mapping is not demand-paged at all — it allocates
  // kernel pages and copies the whole file in — so mapping a 235MB log there
  // would have been worse than keeping every object decoded.
  auto body = host_.read_range(dir_ + "/objects.log", e.body_offset, e.body_len);
  if (!body.ok()) return body.error();
  if (body->size() < e.body_len) {
    return Result<StoredObject>(Status::NotFound, "object body is past the end of the log");
  }
  Reader br(ByteSpan(body->data(), body->size()));
  return StoredObject::decode(br, false);
}

size_t ObjectStore::footprint_of(const StoredObject& o) {
  size_t n = sizeof(StoredObject) + o.payload.size() + o.embedding.size() * sizeof(float) +
             o.relations.size() * sizeof(ObjectRelation);
  for (const auto& [k, v] : o.metadata) n += k.size() + v.size() + 2 * sizeof(std::string);
  return n;
}

void ObjectStore::evict_locked() const {
  if (hot_bytes_ <= config_.hot_budget) return;

  auto get_class = [this](const EntityID& prov) -> ResidencyClass {
    if (prov.is_null()) return ResidencyClass::Cool;
    auto it = entity_residency_.find(prov);
    if (it != entity_residency_.end()) return it->second;
    return ResidencyClass::Warm;
  };

  // Eviction priorities:
  // Pass 0: Evict Cool/Sealed objects from unpinned working sets (oldest LRU first)
  // Pass 1: Evict Warm objects from unpinned working sets (oldest LRU first)
  // Never purge Pinned working sets.
  for (int pass = 0; pass < 2 && hot_bytes_ > config_.hot_budget; ++pass) {
    auto it = lru_.end();
    while (it != lru_.begin() && hot_bytes_ > config_.hot_budget) {
      --it;
      const ObjectID victim = *it;
      auto hit = hot_.find(victim);
      if (hit == hot_.end()) {
        it = lru_.erase(it);
        continue;
      }

      ResidencyClass rc = get_class(hit->second.obj.provenance);
      if (rc == ResidencyClass::Pinned) {
        continue;
      }

      bool should_evict = false;
      if (pass == 0) {
        should_evict = (rc == ResidencyClass::Cool || rc == ResidencyClass::Sealed);
      } else {
        should_evict = (rc == ResidencyClass::Warm);
      }

      if (should_evict) {
        hot_bytes_ -= hit->second.bytes;
        hot_.erase(hit);
        it = lru_.erase(it);
      }
    }
  }
}

const StoredObject* ObjectStore::hot_ref_locked(const ObjectID& id, const Entry& e) const {
  auto it = hot_.find(id);
  if (it != hot_.end()) {
    // Touched: it goes back to the front of the queue.
    lru_.splice(lru_.begin(), lru_, it->second.lru);
    it->second.lru = lru_.begin();
    if (!it->second.obj.provenance.is_null()) {
      entity_last_access_ns_[it->second.obj.provenance] = host_.monotonic_ns();
    }
    return &it->second.obj;
  }
  auto decoded = decode_locked(e);
  if (!decoded.ok()) return nullptr;
  if (!decoded->provenance.is_null()) {
    entity_last_access_ns_[decoded->provenance] = host_.monotonic_ns();
  }
  HotEntry he;
  he.bytes = footprint_of(*decoded);
  he.obj = std::move(*decoded);
  lru_.push_front(id);
  he.lru = lru_.begin();
  hot_bytes_ += he.bytes;
  auto ins = hot_.emplace(id, std::move(he));
  evict_locked();
  // evict_locked never removes the front, so this reference stands.
  auto again = hot_.find(id);
  return again == hot_.end() ? nullptr : &again->second.obj;
  (void)ins;
}

void ObjectStore::set_hot_budget(size_t bytes) {
  hal::LockGuard g(mu_);
  config_.hot_budget = bytes;
  evict_locked();
}
size_t ObjectStore::hot_budget() const { hal::LockGuard g(mu_); return config_.hot_budget; }
size_t ObjectStore::hot_bytes() const { hal::LockGuard g(mu_); return hot_bytes_; }
size_t ObjectStore::hot_count() const { hal::LockGuard g(mu_); return hot_.size(); }

const ResidencyConfig& ObjectStore::residency_config() const { hal::LockGuard g(mu_); return config_; }
void ObjectStore::set_residency_config(const ResidencyConfig& cfg) {
  hal::LockGuard g(mu_);
  config_ = cfg;
  evict_locked();
}

void ObjectStore::pin(const EntityID& entity) {
  hal::LockGuard g(mu_);
  entity_residency_[entity] = ResidencyClass::Pinned;
}

void ObjectStore::demote(const EntityID& entity) {
  hal::LockGuard g(mu_);
  auto it = entity_residency_.find(entity);
  if (it == entity_residency_.end()) {
    entity_residency_[entity] = ResidencyClass::Cool;
  } else {
    switch (it->second) {
      case ResidencyClass::Pinned:
        it->second = ResidencyClass::Warm;
        break;
      case ResidencyClass::Warm:
        it->second = ResidencyClass::Cool;
        break;
      case ResidencyClass::Cool:
      case ResidencyClass::Sealed:
        it->second = ResidencyClass::Sealed;
        break;
    }
  }
  evict_locked();
}

void ObjectStore::set_residency_class(const EntityID& entity, ResidencyClass rclass) {
  hal::LockGuard g(mu_);
  entity_residency_[entity] = rclass;
  if (rclass == ResidencyClass::Cool || rclass == ResidencyClass::Sealed) {
    evict_locked();
  }
}

ResidencyClass ObjectStore::residency_class(const EntityID& entity) const {
  hal::LockGuard g(mu_);
  auto it = entity_residency_.find(entity);
  if (it != entity_residency_.end()) return it->second;
  return ResidencyClass::Warm;
}

size_t ObjectStore::evict_entity(const EntityID& entity) {
  hal::LockGuard g(mu_);
  size_t freed = 0;
  auto it = lru_.begin();
  while (it != lru_.end()) {
    const ObjectID id = *it;
    auto hit = hot_.find(id);
    if (hit != hot_.end() && hit->second.obj.provenance == entity) {
      freed += hit->second.bytes;
      hot_bytes_ -= hit->second.bytes;
      hot_.erase(hit);
      it = lru_.erase(it);
    } else {
      ++it;
    }
  }
  return freed;
}

size_t ObjectStore::hot_count(const EntityID& entity) const {
  hal::LockGuard g(mu_);
  size_t c = 0;
  for (const auto& [id, entry] : hot_) {
    if (entry.obj.provenance == entity) c++;
  }
  return c;
}

size_t ObjectStore::hot_bytes(const EntityID& entity) const {
  hal::LockGuard g(mu_);
  size_t b = 0;
  for (const auto& [id, entry] : hot_) {
    if (entry.obj.provenance == entity) b += entry.bytes;
  }
  return b;
}

uint64_t ObjectStore::entity_last_access(const EntityID& entity) const {
  hal::LockGuard g(mu_);
  auto it = entity_last_access_ns_.find(entity);
  return it != entity_last_access_ns_.end() ? it->second : 0;
}

size_t ObjectStore::cold_blocks_count() const { hal::LockGuard g(mu_); return cold_blocks_.size(); }
size_t ObjectStore::cold_objects_count() const { hal::LockGuard g(mu_); return cold_objects_count_; }
size_t ObjectStore::active_count() const { hal::LockGuard g(mu_); return entries_.size(); }

void ObjectStore::set_active_threshold(size_t max_active) {
  hal::LockGuard g(mu_);
  config_.max_active = std::max<uint32_t>(1, (uint32_t)max_active);
}

void ObjectStore::seal_cold_blocks() {
  hal::LockGuard g(mu_);
  seal_current_block_locked();
}

namespace {

// The block's index, wire-encoded. Everything a fresh process needs to
// reconstruct this SparseBlock without touching the block's own data bytes:
// the range, the bloom, and the full per-object locator table (without which
// a bloom hit has nowhere to look and the segment gets scanned anyway -- the
// failure this whole footer exists to rule out).
ByteBuffer encode_block_footer(const ObjectStore::SparseBlock& b) {
  ByteBuffer body;
  Writer w(body);
  w.u64(b.file_offset);
  w.u64(b.file_len);
  w.u32(b.object_count);
  w.u64(b.min_timestamp_ns);
  w.u64(b.max_timestamp_ns);
  w.u64(b.min_lo);
  w.u64(b.max_lo);
  w.u32((uint32_t)ObjectStore::SparseBlock::kBloomWords);
  for (size_t i = 0; i < ObjectStore::SparseBlock::kBloomWords; ++i) w.u64(b.bloom[i]);
  w.u32((uint32_t)b.locs.size());
  for (const auto& loc : b.locs) { w.u64(loc.lo); w.u64(loc.hi); w.u64(loc.offset); w.u32(loc.len); }
  const uint64_t crc = fnv1a(ByteSpan(body.data(), body.size()));

  ByteBuffer rec;
  Writer rw(rec);
  rw.u32(BLOCK_FOOTER_MAGIC);
  rw.u32((uint32_t)(body.size() + 8));   // bytes following: body + this crc
  rw.raw(ByteSpan(body.data(), body.size()));
  rw.u64(crc);
  return rec;
}

// The inverse. `body` is the footer's payload, magic and length already
// consumed by the caller and the trailing crc already verified.
Result<ObjectStore::SparseBlock> decode_block_footer(ByteSpan body) {
  Reader r(body);
  ObjectStore::SparseBlock b;
  b.file_offset = r.u64();
  b.file_len = r.u64();
  b.object_count = r.u32();
  b.min_timestamp_ns = r.u64();
  b.max_timestamp_ns = r.u64();
  b.min_lo = r.u64();
  b.max_lo = r.u64();
  uint32_t words = r.u32();
  if (!r.ok() || words != ObjectStore::SparseBlock::kBloomWords) {
    return Result<ObjectStore::SparseBlock>(Status::Corrupt, "block footer: bloom width mismatch");
  }
  for (size_t i = 0; i < words; ++i) b.bloom[i] = r.u64();
  uint32_t n = r.u32();
  if (!r.ok()) return Result<ObjectStore::SparseBlock>(Status::Corrupt, "block footer: truncated");
  b.locs.reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    ObjectStore::SparseBlock::Loc loc;
    loc.lo = r.u64(); loc.hi = r.u64(); loc.offset = r.u64(); loc.len = r.u32();
    b.locs.push_back(loc);
  }
  if (!r.ok()) return Result<ObjectStore::SparseBlock>(Status::Corrupt, "block footer: truncated locator table");
  return b;
}

// A full StoredObject minus its payload -- everything apply_metadata_locked
// needs to rebuild order_/graph_/semantic_/meta indexes without ever reading
// the (possibly large) payload bytes at restore time. Reuses
// StoredObject::decode unmodified: it just gets an empty payload back, which
// is correct -- the real payload is still in the log, read on demand exactly
// as any other cold object.
ByteBuffer encode_object_header(const StoredObject& o) {
  ByteBuffer buf;
  Writer w(buf);
  w.id(o.id); w.u64(o.timestamp_ns); w.id(o.provenance); w.id(o.derived_from); w.id(o.source_state);
  w.u32((uint32_t)o.embedding.size()); for (float f : o.embedding) w.f32(f);
  w.u32((uint32_t)o.metadata.size()); for (auto& [k, v] : o.metadata) { w.str(k); w.str(v); }
  w.u32((uint32_t)o.relations.size()); for (auto& r : o.relations) { w.id(r.target); w.u32(r.kind); }
  w.bytes(ByteSpan{});
  return buf;
}

// Version of the persisted header format itself. A reader that does not
// recognise this exact version refuses to trust the meta footer and falls
// back to decoding the block's real bodies -- forward/backward compatibility
// bought by doing more work, never by misreading.
constexpr uint32_t kMetaFooterVersion = 1;

ByteBuffer encode_meta_footer(const ObjectStore::SparseBlock& b, const std::vector<ByteBuffer>& headers) {
  ByteBuffer body;
  Writer w(body);
  w.u32(kMetaFooterVersion);
  w.u64(b.file_offset);
  w.u32((uint32_t)headers.size());
  for (const auto& h : headers) { w.u32((uint32_t)h.size()); w.raw(ByteSpan(h.data(), h.size())); }
  const uint64_t crc = fnv1a(ByteSpan(body.data(), body.size()));

  ByteBuffer rec;
  Writer rw(rec);
  rw.u32(META_FOOTER_MAGIC);
  rw.u32((uint32_t)(body.size() + 8));
  rw.raw(ByteSpan(body.data(), body.size()));
  rw.u64(crc);
  return rec;
}

struct DecodedMetaFooter {
  uint32_t version = 0;
  uint64_t file_offset = 0;
  std::vector<StoredObject> objects;
};

// `body` excludes magic/len (already consumed) and the trailing crc (already
// verified by the caller).
Result<DecodedMetaFooter> decode_meta_footer(ByteSpan body) {
  Reader r(body);
  DecodedMetaFooter out;
  out.version = r.u32();
  out.file_offset = r.u64();
  uint32_t n = r.u32();
  if (!r.ok()) return Result<DecodedMetaFooter>(Status::Corrupt, "meta footer: truncated header");
  out.objects.reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t hlen = r.u32();
    ByteSpan hbytes = r.raw(hlen);
    if (!r.ok()) return Result<DecodedMetaFooter>(Status::Corrupt, "meta footer: truncated object header");
    Reader hr(hbytes);
    auto o = StoredObject::decode(hr);
    if (!o.ok()) return Result<DecodedMetaFooter>(Status::Corrupt, "meta footer: bad object header");
    out.objects.push_back(std::move(*o));
  }
  return out;
}

// Headers are collected in arrival order; sort them alongside locs (same
// permutation), which is the order find() searches and the footers are written.
void sort_block_by_lo(ObjectStore::SparseBlock& b, std::vector<ByteBuffer>& headers) {
  std::vector<size_t> order(b.locs.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](size_t x, size_t y) { return b.locs[x].lo < b.locs[y].lo; });
  std::vector<ObjectStore::SparseBlock::Loc> sorted_locs(order.size());
  std::vector<ByteBuffer> sorted_headers(order.size());
  for (size_t i = 0; i < order.size(); ++i) {
    sorted_locs[i] = b.locs[order[i]];
    sorted_headers[i] = std::move(headers[order[i]]);
  }
  b.locs = std::move(sorted_locs);
  headers = std::move(sorted_headers);
}

// One blocks.idx record: where a sealed block's data and footers lie.
ByteBuffer encode_toc_record(uint64_t data_offset, uint64_t data_len, uint64_t footer_offset, uint64_t footer_len) {
  ByteBuffer toc;
  Writer tw(toc);
  tw.u64(data_offset);
  tw.u64(data_len);
  tw.u64(footer_offset);
  tw.u64(footer_len);
  tw.u64(fnv1a(ByteSpan(toc.data(), toc.size())));
  return toc;
}

}  // namespace

void ObjectStore::seal_current_block_locked() {
  if (cur_block_.object_count == 0) return;
  // Headers in arrival order (cur_block_.locs order), read back from the log.
  // A record that will not decode leaves the block without a footer: replay
  // re-derives it from the data records, exactly as for an out-of-position seal.
  bool headers_ok = true;
  cur_block_headers_.clear();
  cur_block_headers_.reserve(cur_block_.locs.size());
  for (const auto& l : cur_block_.locs) {
    Entry e;
    e.body_offset = l.offset;
    e.body_len = l.len;
    auto o = decode_locked(e);
    if (!o.ok()) { headers_ok = false; break; }
    cur_block_headers_.push_back(encode_object_header(*o));
  }
  if (headers_ok) sort_block_by_lo(cur_block_, cur_block_headers_);
  else {
    cur_block_headers_.clear();
    std::sort(cur_block_.locs.begin(), cur_block_.locs.end(), [](const auto& x, const auto& y) { return x.lo < y.lo; });
  }

  const std::string log = dir_ + "/objects.log";

  // Written before the in-memory transition, so a crash between "wrote the
  // footer" and "erased the active entries" is recoverable: replay will
  // re-derive the same block from the data records regardless, and the footer
  // is there to confirm it, not required for it to happen.
  ByteBuffer block_footer = encode_block_footer(cur_block_);
  uint64_t footer_write_offset = 0;
  { auto st = host_.stat(log); if (st.ok()) footer_write_offset = st->size; }

  // A footer is only meaningful where it belongs: IMMEDIATELY after the
  // records of the block it describes. Appending happens at the end of the
  // log, so that holds only while the block being sealed is itself the tail.
  // When a block is sealed out of history instead -- which is exactly what
  // the reindex after a vacuum does, replaying a compacted log that has no
  // footers in it -- the append lands after every later record, and the next
  // boot reaches it having already sealed a DIFFERENT block: "sealed block
  // footer disagrees with replay-derived block", and open() fails. A store
  // that cannot be reopened after a vacuum is worse than one that never
  // vacuums. Sealing is re-derivable from the data records either way, so
  // when the position would be wrong the footer is simply not written.
  const bool footer_in_position = headers_ok &&
      footer_write_offset ==cur_block_.file_offset + cur_block_.file_len;
  if (!footer_in_position) {
    cold_blocks_.push_back(cur_block_);
    cold_objects_count_ += cur_block_.object_count;
    for (const auto& id : cur_block_ids_) entries_.erase(id);
    cur_block_ = SparseBlock{};
    cur_block_ids_.clear();
    cur_block_headers_.clear();
    return;
  }

  auto w1 = host_.append_file(log, ByteSpan(block_footer.data(), block_footer.size()));

  // The header dump: everything a restore needs to rebuild the metadata,
  // graph, and semantic indexes for this block WITHOUT decoding a single
  // payload. This is the durability record Option B needed -- without it,
  // "sealed" only ever meant "out of the hot decode cache", never "the WHERE
  // clause still works after a restart without reading every body again."
  ByteBuffer meta_footer = encode_meta_footer(cur_block_, cur_block_headers_);
  auto w2 = host_.append_file(log, ByteSpan(meta_footer.data(), meta_footer.size()));

  if (!w1.ok() || !w2.ok()) {
    // A durability record that failed to write is worth knowing about, but a
    // failed footer must never block sealing itself -- the tier still has to
    // work (via re-derivation on the next replay) even when this failed.
    host_.log_error("aios: block footer write failed: " + (!w1.ok() ? w1.error().message : w2.error().message));
  } else {
    // The table of contents: what makes the skip-decode restore possible at
    // all. Without it, a fresh boot has no way to know a footer exists
    // further into the file until it has ALREADY decoded every byte leading
    // up to it -- by which point decoding was not skipped, it already
    // happened. This is looked up BEFORE a span is decoded, not after.
    ByteBuffer toc = encode_toc_record(cur_block_.file_offset, cur_block_.file_len, footer_write_offset,
                                       block_footer.size() + meta_footer.size());
    auto w3 = host_.append_file(dir_ + "/blocks.idx", ByteSpan(toc.data(), toc.size()));
    if (!w3.ok()) {
      host_.log_error("aios: blocks.idx write failed: " + w3.error().message);
    } else {
      known_spans_.push_back(SkipSpan{cur_block_.file_offset, cur_block_.file_len,
                                      footer_write_offset, block_footer.size() + meta_footer.size()});
    }
  }

  cold_blocks_.push_back(cur_block_);
  cold_objects_count_ += cur_block_.object_count;
  for (const auto& id : cur_block_ids_) {
    entries_.erase(id);
  }
  cur_block_ = SparseBlock{};
  cur_block_ids_.clear();
  cur_block_headers_.clear();
}

size_t ObjectStore::cold_block_for_locked(const ObjectID& id) const {
  // Blocks are sealed in log order and ids rise with that order, so the first
  // block whose max_lo reaches this id is the only candidate.
  size_t lo = 0, hi = cold_blocks_.size();
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (cold_blocks_[mid].max_lo < id.lo) lo = mid + 1;
    else hi = mid;
  }
  for (size_t i = lo; i < cold_blocks_.size() && cold_blocks_[i].min_lo <= id.lo; ++i) {
    if (cold_blocks_[i].may_contain(id)) return i;
  }
  return cold_blocks_.size();   // not placed by the ordering
}

const ObjectStore::SparseBlock::Loc* ObjectStore::locate_cold_locked(const ObjectID& id,
                                                                    size_t* block_out) const {
  const size_t bi = cold_block_for_locked(id);
  if (bi < cold_blocks_.size()) {
    if (const auto* l = cold_blocks_[bi].find(id.lo)) {
      if (block_out) *block_out = bi;
      return l;
    }
  }
  // The ordering did not place it (a log spanning a reboot); ask each block.
  for (size_t i = 0; i < cold_blocks_.size(); ++i) {
    if (!cold_blocks_[i].may_contain(id)) continue;
    if (const auto* l = cold_blocks_[i].find(id.lo)) {
      if (block_out) *block_out = i;
      return l;
    }
  }
  return nullptr;
}

Result<StoredObject> ObjectStore::find_in_cold_blocks_locked(const ObjectID& id) const {
  // Exact placement: read the one record, not the block around it.
  if (const auto* l = locate_cold_locked(id, nullptr)) {
    Entry e;
    e.body_offset = l->offset;
    e.body_len = l->len;
    auto o = decode_locked(e);
    if (o.ok() && o->id == id) return o;
  }
  return find_in_cold_blocks_scan_locked(id);
}

Result<StoredObject> ObjectStore::find_in_cold_blocks_scan_locked(const ObjectID& id) const {
  if (cold_blocks_.empty()) return Result<StoredObject>(Status::NotFound, "no cold segments");
  const std::string log = dir_ + "/objects.log";

  // Blocks are sealed in log order and ids rise with that order within a boot,
  // so `min_lo` is sorted and the candidates can be found without touching the
  // rest. A reboot breaks the ordering, so the search below is a hint: if the
  // narrowed range does not hold the object, every block is still checked.
  auto try_block = [&](const SparseBlock& b) -> Result<StoredObject> {
    if (!b.may_contain(id)) return Result<StoredObject>(Status::NotFound, "filtered");
    auto buf = host_.read_range(log, b.file_offset, b.file_len);
    if (!buf.ok() || buf->size() < 16) return Result<StoredObject>(Status::NotFound, "unreadable block");
    Reader r(ByteSpan(buf->data(), buf->size()));
    while (r.remaining() >= 16) {
      uint32_t magic = r.u32();
      uint32_t len = r.u32();
      if (magic != OBJ_MAGIC || 16 + (uint64_t)len > r.remaining() + 8) break;
      ByteSpan body = r.raw(len);
      uint64_t sum = r.u64();
      if (!r.ok() || sum != fnv1a(body)) break;
      if (body.size() >= 16) {
        Reader br(body);
        ObjectID obj_id{br.id()};
        if (obj_id == id) {
          Reader full_br(body);
          return StoredObject::decode(full_br);
        }
      }
    }
    return Result<StoredObject>(Status::NotFound, "not in this block");
  };

  // The first block whose max_lo could cover this id.
  size_t lo = 0, hi = cold_blocks_.size();
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (cold_blocks_[mid].max_lo < id.lo) lo = mid + 1;
    else hi = mid;
  }
  for (size_t i = lo; i < cold_blocks_.size() && cold_blocks_[i].min_lo <= id.lo; ++i) {
    auto r = try_block(cold_blocks_[i]);
    if (r.ok()) return r;
  }

  // Not where the ordering said it would be. That happens when the log spans a
  // reboot, so fall back to every block rather than report a stored object
  // missing — the filter still skips most of them without a read.
  for (const auto& b : cold_blocks_) {
    auto r = try_block(b);
    if (r.ok()) return r;
  }
  return Result<StoredObject>(Status::NotFound, "object " + id.short_str() + " not found in cold segments");
}

void ObjectStore::load_known_spans_locked() {
  const std::string idx_path = dir_ + "/blocks.idx";
  auto info = host_.stat(idx_path);
  if (!info.ok() || info->size <= blocks_idx_consumed_) return;
  auto data = host_.read_range(idx_path, blocks_idx_consumed_, info->size - blocks_idx_consumed_);
  if (!data.ok()) return;
  Reader r(ByteSpan(data->data(), data->size()));
  constexpr size_t kRecSize = 8 * 5;   // 4 u64 fields + a trailing u64 crc
  while (r.remaining() >= kRecSize) {
    const size_t start = r.position();
    uint64_t data_offset = r.u64();
    uint64_t data_len = r.u64();
    uint64_t footer_offset = r.u64();
    uint64_t footer_len = r.u64();
    uint64_t crc_read = r.u64();
    if (!r.ok()) break;
    ByteSpan body(data->data() + start, 32);
    if (fnv1a(body) != crc_read) break;   // torn or corrupt tail: stop, retry on the next call
    known_spans_.push_back(SkipSpan{data_offset, data_len, footer_offset, footer_len});
    span_consumed_.push_back(false);
    blocks_idx_consumed_ += kRecSize;
  }
}

Result<bool> ObjectStore::try_skip_restore_locked(uint64_t data_offset) {
  load_known_spans_locked();   // pick up spans another process may have sealed
  for (size_t i = 0; i < known_spans_.size(); ++i) {
    if (span_consumed_[i] || known_spans_[i].data_offset != data_offset) continue;
    const SkipSpan span = known_spans_[i];
    const std::string log = dir_ + "/objects.log";

    // The skip ends by moving consumed_ to the far side of the FOOTERS, which
    // is only the far side of this block if the footers sit immediately after
    // its data. An older build appended footers at the end of the log when it
    // sealed a block out of history (its post-vacuum reindex did exactly
    // that), and trusting such a span here jumped consumed_ to the end of the
    // file: one block restored, every record after it silently dropped --
    // 16,384 objects recovered out of 50,000. A span whose footers are not
    // adjacent is not skippable; its records get decoded the ordinary way.
    if (span.footer_offset != span.data_offset + span.data_len) {
      span_consumed_[i] = true;   // never usable: stop re-checking it every pass
      return false;
    }

    auto raw = host_.read_range(log, span.footer_offset, span.footer_len);
    if (!raw.ok() || raw->size() < span.footer_len) return false;   // not fully written yet
    Reader r(ByteSpan(raw->data(), raw->size()));

    uint32_t magic1 = r.u32(), len1 = r.u32();
    if (!r.ok() || magic1 != BLOCK_FOOTER_MAGIC || len1 < 8) return false;
    ByteSpan body1 = r.raw(len1 - 8);
    uint64_t crc1 = r.u64();
    if (!r.ok() || fnv1a(body1) != crc1) return false;
    auto block = decode_block_footer(body1);
    if (!block.ok()) return false;

    uint32_t magic2 = r.u32(), len2 = r.u32();
    if (!r.ok() || magic2 != META_FOOTER_MAGIC || len2 < 8) return false;
    ByteSpan body2 = r.raw(len2 - 8);
    uint64_t crc2 = r.u64();
    if (!r.ok() || fnv1a(body2) != crc2) return false;
    auto meta = decode_meta_footer(body2);
    if (!meta.ok() || meta->version != kMetaFooterVersion) return false;   // unrecognised format: decode instead
    if (meta->objects.size() != block->object_count || block->file_offset != span.data_offset) return false;

    // Spot check: decode a few real records and confirm the persisted headers
    // agree with them exactly. Cheap (a handful of reads, not the whole
    // block), and it is what keeps a skip from being blind trust -- any
    // disagreement here falls all the way back to a normal per-record decode
    // of this span, the same as if no footer existed at all.
    if (block->locs.empty()) return false;
    std::vector<size_t> sample = {0, block->locs.size() / 2, block->locs.size() - 1};
    size_t checked = 0;
    for (size_t si : sample) {
      const auto& loc = block->locs[si];
      auto rawbody = host_.read_range(log, loc.offset, loc.len);
      if (!rawbody.ok() || rawbody->size() < loc.len) return false;
      Reader br(ByteSpan(rawbody->data(), rawbody->size()));
      auto obj = StoredObject::decode(br);
      if (!obj.ok()) return false;
      bool found = false, agrees = false;
      for (auto& h : meta->objects) {
        if (h.id.lo == loc.lo && h.id.hi == loc.hi) {
          found = true;
          agrees = h.metadata == obj->metadata && h.timestamp_ns == obj->timestamp_ns &&
                  h.provenance == obj->provenance && h.derived_from == obj->derived_from &&
                  h.source_state == obj->source_state && h.relations.size() == obj->relations.size() &&
                  h.embedding == obj->embedding;
          break;
        }
      }
      if (!found || !agrees) return false;
      checked++;
    }
    if (checked == 0) return false;

    // The spot check passed: commit the whole block from the footers, with
    // no further decoding.
    cold_blocks_.push_back(*block);
    cold_objects_count_ += block->object_count;
    for (auto& h : meta->objects) apply_metadata_locked(h);
    span_consumed_[i] = true;
    consumed_ = span.footer_offset + span.footer_len;
    return true;
  }
  return false;
}

void ObjectStore::recover_compacting_locked() {
  const std::string log = dir_ + "/objects.log";
  const std::string tmp = log + ".compacting";
  const std::string tmp_idx = dir_ + "/blocks.idx.compacting";
  const std::string tmp_ready = log + ".compacting.ready";

  if (host_.exists(tmp_ready)) {
    auto tinfo = host_.stat(tmp);
    if (tinfo.ok() && tinfo->size > 0) {
      const uint64_t tsize = tinfo->size;
      uint64_t copied = 0;
      bool log_started = false;
      static constexpr uint64_t kWriteChunk = 8ull << 20;
      bool copy_ok = true;
      while (copied < tsize) {
        const uint64_t want = std::min<uint64_t>(kWriteChunk, tsize - copied);
        auto buf = host_.read_range(tmp, copied, want);
        if (!buf.ok() || buf->empty() || buf->size() > want) {
          copy_ok = false;
          break;
        }
        Result<void> wr;
        if (!log_started) {
          wr = host_.write_file(log, ByteSpan(buf->data(), buf->size()));
          log_started = true;
        } else {
          wr = host_.append_file(log, ByteSpan(buf->data(), buf->size()));
        }
        if (!wr.ok()) {
          copy_ok = false;
          break;
        }
        copied += buf->size();
      }
      if (!log_started && copy_ok) {
        ByteBuffer empty;
        (void)host_.write_file(log, ByteSpan(empty.data(), 0));
      }
      if (copy_ok && copied == tsize) {
        if (host_.exists(tmp_idx)) {
          auto idx_buf = host_.read_file(tmp_idx);
          if (idx_buf.ok()) {
            (void)host_.write_file(dir_ + "/blocks.idx", ByteSpan(idx_buf->data(), idx_buf->size()));
          }
        }
        (void)host_.remove(tmp_ready);
        (void)host_.remove(tmp_idx);
        (void)host_.remove(tmp);
        return;
      }
    }
  }

  (void)host_.remove(tmp_ready);
  (void)host_.remove(tmp_idx);
  (void)host_.remove(tmp);
}

Result<void> ObjectStore::open() {
  AIOS_TRY(host_.make_dir(dir_));
  hal::LockGuard g(mu_);
  consumed_ = 0;
  recover_compacting_locked();
  load_known_spans_locked();
  auto r = refresh_locked();
  if (!r.ok()) return r.error();
  return Ok();
}

Result<size_t> ObjectStore::refresh() {
  hal::LockGuard g(mu_);
  return refresh_locked();
}

Result<size_t> ObjectStore::refresh_locked() {
  const std::string log = dir_ + "/objects.log";
  auto info = host_.stat(log);
  if (!info.ok() || info->size <= consumed_) return 0;   // no log yet, or nothing new
  const uint64_t end = info->size;
  size_t added = 0;

  // Replayed in windows. It used to read the WHOLE log into one buffer on
  // every refresh — a 235MB allocation to notice a handful of new records, and
  // on a machine with a gigabyte that is most of the machine.
  static constexpr uint64_t kWindow = 4ull << 20;
  while (consumed_ < end) {
    // A span always begins at a record boundary, which is exactly where
    // `consumed_` sits at the top of this loop. If blocks.idx knows a fully
    // sealed, footer-verified block starts right here, its whole data span
    // (which could be megabytes) is skipped without decoding a single record
    // -- this is the entire point of Option B. Any failure of the spot check
    // inside falls through to the ordinary per-record decode below, exactly
    // as if no footer existed.
    auto skip = try_skip_restore_locked(consumed_);
    if (skip.ok() && *skip) continue;

    uint64_t want = std::min<uint64_t>(kWindow, end - consumed_);
    auto win = host_.read_range(log, consumed_, want);
    if (!win.ok()) return win.error();
    if (win->empty()) break;

    // Where this window starts in the log. consumed_ moves as records are
    // taken, so offsets within the window must be measured from a base that
    // does not move — computing them from consumed_ made every record after
    // the first skip forward, and the replay silently dropped most of the log.
    const uint64_t win_base = consumed_;
    Reader r(ByteSpan(win->data(), win->size()));
    bool progressed = false;
    while (r.remaining() >= 16) {
      const size_t rec_start = r.position();
      uint32_t magic = r.u32();
      uint32_t len = r.u32();
      if (magic == BLOCK_FOOTER_MAGIC) {
        // A durability record, not a data record. It must be consumed and
        // stepped over -- treating it like an unrecognized magic and bailing
        // would silently stop replay here forever, dropping every record a
        // live process appends after its first sealed block.
        const uint64_t footer_total = 8 + (uint64_t)len;   // magic+len already read
        ByteSpan raw_body;
        ByteBuffer owned;
        uint64_t crc_read = 0;
        bool have_it = false;
        if (footer_total <= win->size() - rec_start) {
          ByteSpan body = r.raw(len - 8);
          crc_read = r.u64();
          if (!r.ok()) return Result<size_t>(Status::Corrupt, "torn block footer");
          owned.assign(body.begin(), body.end());
          have_it = true;
        } else {
          auto one = host_.read_range(log, win_base + rec_start, footer_total);
          if (!one.ok()) return one.error();
          if (one->size() < footer_total) return (size_t)added;   // torn tail: stop cleanly
          Reader fr(ByteSpan(one->data(), one->size()));
          fr.u32(); fr.u32();
          ByteSpan body = fr.raw(len - 8);
          crc_read = fr.u64();
          if (!fr.ok()) return Result<size_t>(Status::Corrupt, "torn block footer");
          owned.assign(body.begin(), body.end());
          have_it = true;
        }
        if (have_it) {
          if (fnv1a(ByteSpan(owned.data(), owned.size())) != crc_read) {
            return Result<size_t>(Status::Corrupt, "block footer checksum mismatch");
          }
          auto decoded = decode_block_footer(ByteSpan(owned.data(), owned.size()));
          if (!decoded.ok()) return decoded.error();
          // Sealing is deterministic replay of the same bytes: by the time
          // this footer is reached, THIS process has already independently
          // sealed the identical block from the data records that precede it.
          // The footer is not trusted in place of that -- it is checked
          // against it. A disagreement means the footer is not describing the
          // block replay just built (see below): it is dropped, never used to
          // override what the data records say.
          // A footer that does not sit where its block ends was written by a
          // replay that sealed a block out of history (older builds did this
          // after every vacuum). It describes a real block, just not the one
          // replay is standing in front of, and the block itself has already
          // been re-derived from the data records. Stepping over it is the
          // recovery; refusing to open the store is not.
          if (cold_blocks_.empty()) {
            host_.log_error("aios: stale block footer at " + std::to_string(win_base + rec_start) + " with no preceding sealed block -- skipped");
            consumed_ = win_base + rec_start + footer_total;
            progressed = true;
            if (footer_total > win->size() - rec_start) break;
            continue;
          }
          const SparseBlock& derived = cold_blocks_.back();
          bool locs_match = derived.locs.size() == decoded->locs.size();
          if (locs_match) {
            for (size_t i = 0; i < derived.locs.size(); ++i) {
              if (derived.locs[i].lo != decoded->locs[i].lo ||
                  derived.locs[i].offset != decoded->locs[i].offset ||
                  derived.locs[i].len != decoded->locs[i].len) { locs_match = false; break; }
            }
          }
          bool bloom_match = std::equal(derived.bloom, derived.bloom + SparseBlock::kBloomWords, decoded->bloom);
          const bool matches = derived.file_offset == decoded->file_offset &&
                               derived.object_count == decoded->object_count &&
                               derived.min_lo == decoded->min_lo && derived.max_lo == decoded->max_lo &&
                               locs_match && bloom_match;
          if (!matches) {
            // Same case as above: an out-of-position footer left behind by an
            // older build's post-vacuum reindex. Replay's own derivation is
            // authoritative and already done, so the footer is skipped with a
            // note rather than taking the whole store down with it.
            host_.log_error("aios: block footer at " + std::to_string(win_base + rec_start) + " disagrees with the replay-derived block -- skipped");
            consumed_ = win_base + rec_start + footer_total;
            progressed = true;
            if (footer_total > win->size() - rec_start) break;
            continue;
          }
          consumed_ = win_base + rec_start + footer_total;
          progressed = true;
          if (footer_total > win->size() - rec_start) break;   // read outside the window: refill it
          continue;
        }
      }
      if (magic == META_FOOTER_MAGIC) {
        // Reached only via the fallback path -- either this block's skip
        // attempt already used this meta footer (and consumed_ jumped past
        // it, so we would not be here), or it failed its own validation and
        // we are decoding the block's data records one at a time instead.
        // Either way there is nothing left to do with these bytes but step
        // over them: without this case, hitting them here would look exactly
        // like the "not a record: stop" bail below and silently truncate
        // replay of everything appended after this footer.
        const uint64_t footer_total = 8 + (uint64_t)len;
        if (footer_total <= win->size() - rec_start) {
          r.raw(len - 8); r.u64();
          if (!r.ok()) return Result<size_t>(Status::Corrupt, "torn meta footer");
          consumed_ = win_base + rec_start + footer_total;
          progressed = true;
          continue;
        } else {
          auto one = host_.read_range(log, win_base + rec_start, footer_total);
          if (!one.ok()) return one.error();
          if (one->size() < footer_total) return (size_t)added;   // torn tail: stop cleanly
          consumed_ = win_base + rec_start + footer_total;
          progressed = true;
          break;   // read outside the window: refill it
        }
      }
      if (magic != OBJ_MAGIC) { return (size_t)added; }        // not a record: stop
      if (16 + (uint64_t)len > win->size() - rec_start) {
        // The record straddles this window. Read it whole on the next pass;
        // if it is larger than a window, ask for exactly its length.
        if ((uint64_t)len + 16 > kWindow) {
          auto one = host_.read_range(log, win_base + rec_start, 16 + (uint64_t)len);
          if (!one.ok()) return one.error();
          if (one->size() < 16 + (size_t)len) return (size_t)added;   // torn tail
          Reader r1(ByteSpan(one->data(), one->size()));
          r1.u32(); r1.u32();
          ByteSpan body1 = r1.raw(len);
          uint64_t sum1 = r1.u64();
          if (!r1.ok() || sum1 != fnv1a(body1)) return (size_t)added;
          Reader br1(body1);
          auto o1 = StoredObject::decode(br1, false);
          if (!o1.ok()) return (size_t)added;
          const uint64_t body_at = win_base + rec_start + 8;
          if (!entries_.count(o1->id)) {
            index(*o1, body_at, len, win_base + rec_start, 16 + len);
            added++;
          }
          consumed_ = win_base + rec_start + 16 + len;
          progressed = true;
        }
        break;
      }
      const size_t body_at_in_win = r.position();
      ByteSpan body = r.raw(len);
      uint64_t sum = r.u64();
      if (!r.ok() || sum != fnv1a(body)) return (size_t)added;   // torn tail: keep what was whole
      Reader br(body);
      auto o = StoredObject::decode(br, false);
      if (!o.ok()) return (size_t)added;
      // The decoded object builds the indexes and is then dropped; what stays
      // is where to find it again.
      if (!entries_.count(o->id)) {
        index(*o, win_base + body_at_in_win, len, win_base + rec_start, 16 + len);
        added++;
      }
      consumed_ = win_base + rec_start + 16 + len;
      progressed = true;
    }
    if (!progressed) break;   // nothing whole in this window
  }
  return added;
}

void ObjectStore::apply_metadata_locked(const StoredObject& o) {
  // Everything index() derives from an object EXCEPT where its bytes live.
  // Factored out so a restored header (no body decode, no payload) can drive
  // the same order_/meta_key_index_/meta_index_/graph_/semantic_ population
  // that a live decode does -- this is what makes a persisted index actually
  // equivalent to having decoded, rather than a second, drifting structure.
  const uint32_t ord = (uint32_t)order_.size();
  order_.push_back(o.id);
  index_revision_++;   // any ordering built before this is stale
  semantic_.add(o.id, o.embedding);
  for (auto& rel : o.relations) graph_.add(o.id, rel.target, rel.kind);
  if (!o.derived_from.is_null()) graph_.add(o.id, o.derived_from, 0xD0);  // derivation edge
  // An object may carry the same key twice; find_meta counts it once, so the
  // index must too.
  for (auto& [k, v] : o.metadata) {
    auto& by_key = meta_key_index_[k];
    if (by_key.empty() || by_key.back() != ord) by_key.push_back(ord);

    ValueIndex& vi = meta_index_[k];
    if (!vi.active) continue;
    if (v.size() > kMaxIndexedValue) {
      // One long value is enough to say this key is not an equality target.
      // Drop whatever was built for it and stop paying for it.
      vi.active = false;
      vi.by_value.clear();
      continue;
    }
    auto& bucket = vi.by_value[v];
    if (bucket.empty() || bucket.back() != ord) bucket.push_back(ord);
    // Near-unique: the index has become a second copy of the column and
    // narrows a search to one row. The scan can do that.
    if (vi.by_value.size() > kJudgeCardinalityFrom && vi.by_value.size() * 2 > (entries_.size() + cold_objects_count_)) {
      vi.active = false;
      vi.by_value.clear();
    }
  }
}

void ObjectStore::drop_superseded_hot_locked(const StoredObject& o) {
  if (hot_.empty()) return;
  const std::string key = o.meta("key");
  if (key.empty()) return;
  // The prior versions of this key, from the value index when it has one.
  // When the index has judged "key" near-unique and dropped itself there is
  // no cheap way to find them, and a scan of the whole store on every write
  // would cost far more than the cache it would trim; the vacuum still
  // retires them.
  auto kit = meta_index_.find("key");
  if (kit == meta_index_.end() || !kit->second.active) return;
  auto bit = kit->second.by_value.find(key);
  if (bit == kit->second.by_value.end()) return;
  for (const uint32_t old_ord : bit->second) {
    const ObjectID& old_id = order_[old_ord];
    if (old_id == o.id) continue;
    auto hit = hot_.find(old_id);
    if (hit == hot_.end()) continue;
    hot_bytes_ -= hit->second.bytes;
    lru_.erase(hit->second.lru);
    hot_.erase(hit);
  }
}

Result<void> ObjectStore::index(const StoredObject& o, uint64_t body_offset, uint32_t body_len,
                                uint64_t rec_offset, uint32_t rec_len) {
  Entry e;
  e.body_offset = body_offset;
  e.body_len = body_len;
  e.timestamp_ns = o.timestamp_ns;
  entries_[o.id] = e;
  // A new version of a key — an overwrite, or the tombstone a DELETE writes —
  // makes every older version of that key dead data. The next vacuum drops
  // them from the log, but there is no reason to go on holding their decoded
  // payloads in RAM until then: a console DELETE should give the memory back
  // at the moment of the delete.
  drop_superseded_hot_locked(o);
  apply_metadata_locked(o);

  // Update current sparse block under construction
  if (cur_block_.object_count == 0) {
    cur_block_.file_offset = rec_offset;
    cur_block_.first_id = o.id;
    cur_block_.min_timestamp_ns = o.timestamp_ns;
    cur_block_.max_timestamp_ns = o.timestamp_ns;
  }
  cur_block_.file_len = (rec_offset + rec_len) - cur_block_.file_offset;
  cur_block_.last_id = o.id;
  cur_block_.min_timestamp_ns = std::min(cur_block_.min_timestamp_ns, o.timestamp_ns);
  cur_block_.max_timestamp_ns = std::max(cur_block_.max_timestamp_ns, o.timestamp_ns);
  cur_block_.add_bloom(o.id);
  cur_block_.locs.push_back(SparseBlock::Loc{o.id.lo, o.id.hi, body_offset, body_len});
  // No header is kept per object while the block fills (~350 B each, resident
  // until the seal): the seal re-derives them from the records by their Loc.
  cur_block_.object_count++;
  cur_block_ids_.push_back(o.id);

  if (cur_block_.object_count >= config_.block_objects && entries_.size() >= config_.max_active) {
    seal_current_block_locked();
  }

  return Ok();
}

Result<ObjectID> ObjectStore::put(StoredObject obj) {
  if (obj.id.is_null()) obj.id = ids_.next_as<ObjectID>();
  if (obj.timestamp_ns == 0) obj.timestamp_ns = host_.wall_ns();
  if (obj.meta("crypto.hash").empty()) {
    obj.metadata.emplace_back("crypto.hash", obj.compute_hash());
  }
  ByteBuffer body;
  { Writer w(body); obj.encode(w); }
  ByteBuffer rec;
  { Writer w(rec); w.u32(OBJ_MAGIC); w.u32((uint32_t)body.size()); w.raw(body); w.u64(fnv1a(body)); }
  hal::LockGuard g(mu_);
  if (entries_.count(obj.id)) return Result<ObjectID>(Status::Conflict, "object identity already stored");
  AIOS_TRY(host_.append_file(dir_ + "/objects.log", rec));
  // Indexed by replaying the tail rather than by arithmetic on a size read
  // before the write. The log is shared — another kernel process on the same
  // root appends to it too — so the only offset that can be trusted is the one
  // found by reading what is actually there.
  auto added = refresh_locked();
  if (!added.ok()) return added.error();
  if (!obj.provenance.is_null()) {
    entity_last_access_ns_[obj.provenance] = host_.monotonic_ns();
  }
  return obj.id;
}

Result<std::vector<ObjectID>> ObjectStore::put_many(std::vector<StoredObject> objs) {
  std::vector<ObjectID> ids;
  if (objs.empty()) return ids;
  ids.reserve(objs.size());

  ByteBuffer batch;
  Writer bw(batch);
  for (auto& obj : objs) {
    if (obj.id.is_null()) obj.id = ids_.next_as<ObjectID>();
    if (obj.timestamp_ns == 0) obj.timestamp_ns = host_.wall_ns();
    if (obj.meta("crypto.hash").empty()) {
      obj.metadata.emplace_back("crypto.hash", obj.compute_hash());
    }
    ByteBuffer body;
    { Writer w(body); obj.encode(w); }
    bw.u32(OBJ_MAGIC); bw.u32((uint32_t)body.size()); bw.raw(body); bw.u64(fnv1a(body));
    ids.push_back(obj.id);
  }

  hal::LockGuard g(mu_);
  // Nothing is written until every identity is known to be new: a batch that
  // would half-apply is worse than one that is refused.
  for (const auto& obj : objs) {
    if (entries_.count(obj.id)) {
      return Result<std::vector<ObjectID>>(Status::Conflict, "object identity already stored");
    }
  }
  AIOS_TRY(host_.append_file(dir_ + "/objects.log", batch));
  // As in put(): the tail is replayed to find where these actually landed.
  auto added = refresh_locked();
  if (!added.ok()) return added.error();
  const uint64_t now_acc = host_.monotonic_ns();
  for (const auto& obj : objs) {
    if (!obj.provenance.is_null()) {
      entity_last_access_ns_[obj.provenance] = now_acc;
    }
  }
  return ids;
}

Result<StoredObject> ObjectStore::get(const ObjectID& id) const {
  hal::LockGuard g(mu_);
  const uint64_t now_wall = host_.wall_ns();
  auto hit = hot_.find(id);
  if (hit != hot_.end()) {
    if (hit->second.obj.is_expired(now_wall)) {
      return Result<StoredObject>(Status::NotFound, "object " + id.short_str() + " expired");
    }
    lru_.splice(lru_.begin(), lru_, hit->second.lru);
    hit->second.lru = lru_.begin();
    if (!hit->second.obj.provenance.is_null()) {
      entity_last_access_ns_[hit->second.obj.provenance] = host_.monotonic_ns();
    }
    return hit->second.obj.decompressed();
  }
  auto it = entries_.find(id);
  if (it != entries_.end()) {
    const StoredObject* p = hot_ref_locked(id, it->second);
    if (!p) return Result<StoredObject>(Status::Corrupt, "object " + id.short_str() + " could not be read from the log");
    if (p->is_expired(now_wall)) {
      return Result<StoredObject>(Status::NotFound, "object " + id.short_str() + " expired");
    }
    return p->decompressed();
  }
  auto cold_obj = find_in_cold_blocks_locked(id);
  if (cold_obj.ok()) {
    if (cold_obj->is_expired(now_wall)) {
      return Result<StoredObject>(Status::NotFound, "object " + id.short_str() + " expired");
    }
    if (!cold_obj->provenance.is_null()) {
      entity_last_access_ns_[cold_obj->provenance] = host_.monotonic_ns();
    }
    HotEntry he;
    he.bytes = footprint_of(*cold_obj);
    he.obj = *cold_obj;
    lru_.push_front(id);
    he.lru = lru_.begin();
    hot_bytes_ += he.bytes;
    hot_.emplace(id, std::move(he));
    evict_locked();
    return cold_obj->decompressed();
  }
  return Result<StoredObject>(Status::NotFound, "object " + id.short_str());
}

void ObjectStore::visit(const std::vector<ObjectID>& ids,
                        const std::function<void(const StoredObject&)>& fn) const {
  hal::LockGuard g(mu_);

  // Anything not already in memory is fetched FIRST, in log order, in bulk.
  // One record per read meant one syscall per record: a filter matching 14k
  // rows spent 1.1 seconds doing 14,000 reads of ~470 bytes. Records that sit
  // near each other in the log come back in the same read.
  struct Cold { uint64_t off; uint32_t len; ObjectID id; };
  std::vector<Cold> cold;
  cold.reserve(ids.size());
  for (const auto& id : ids) {
    if (hot_.count(id)) continue;
    auto it = entries_.find(id);
    if (it == entries_.end()) continue;
    cold.push_back(Cold{it->second.body_offset, it->second.body_len, id});
  }
  // Sealed objects have no entries_ record; they are grouped by the block that
  // holds them so each block is read once for ALL the objects wanted out of it.
  // Read per id, a query touching 90k cold objects re-read the same 470KB
  // blocks 90,000 times — 293GB for one query over a 235MB log.
  // Sealed objects now carry exact offsets, so they go through the same
  // offset-sorted windowed read as everything else instead of dragging in a
  // whole block each.
  if (!cold_blocks_.empty()) {
    for (const auto& id : ids) {
      if (hot_.count(id) || entries_.count(id)) continue;
      if (const auto* l = locate_cold_locked(id, nullptr))
        cold.push_back(Cold{l->offset, l->len, id});
    }
  }

  if (false) {
    std::vector<std::pair<size_t, ObjectID>> sealed;   // block index -> id
    for (const auto& id : ids) {
      if (hot_.count(id) || entries_.count(id)) continue;
      const size_t bi = cold_block_for_locked(id);
      if (bi < cold_blocks_.size()) sealed.emplace_back(bi, id);
    }
    if (!sealed.empty()) {
      std::sort(sealed.begin(), sealed.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
      const std::string log = dir_ + "/objects.log";
      size_t i = 0;
      while (i < sealed.size()) {
        const size_t bi = sealed[i].first;
        size_t j = i;
        while (j < sealed.size() && sealed[j].first == bi) ++j;
        const SparseBlock& b = cold_blocks_[bi];
        // Wanted ids as a set. Scanning them linearly per record made this
        // quadratic — a block of 16k records against 16k wanted ids is 256
        // million comparisons, and a full-corpus query spun on CPU with no
        // I/O at all.
        std::unordered_set<ObjectID, TypedIdHash<9>> want;
        want.reserve((j - i) * 2);
        for (size_t k = i; k < j; ++k) want.insert(sealed[k].second);
        auto buf = host_.read_range(log, b.file_offset, b.file_len);
        if (buf.ok() && buf->size() >= 16) {
          Reader r(ByteSpan(buf->data(), buf->size()));
          while (r.remaining() >= 16) {
            uint32_t magic = r.u32();
            uint32_t len = r.u32();
            if (magic != OBJ_MAGIC || 16 + (uint64_t)len > r.remaining() + 8) break;
            ByteSpan body = r.raw(len);
            uint64_t sum = r.u64();
            if (!r.ok() || sum != fnv1a(body)) break;
            if (body.size() < 16) continue;
            Reader idr(body);
            ObjectID rec_id{idr.id()};
            if (!want.count(rec_id) || hot_.count(rec_id)) continue;
            Reader full(body);
            auto o = StoredObject::decode(full);
            if (!o.ok()) continue;
            HotEntry he;
            he.bytes = footprint_of(*o);
            he.obj = std::move(*o);
            lru_.push_front(rec_id);
            he.lru = lru_.begin();
            hot_bytes_ += he.bytes;
            hot_.emplace(rec_id, std::move(he));
          }
          evict_locked();
        }
        i = j;
      }
    }
  }

  if (!cold.empty()) {
    std::sort(cold.begin(), cold.end(), [](const Cold& a, const Cold& b) { return a.off < b.off; });
    static constexpr uint64_t kFetchWindow = 1ull << 20;
    const std::string log = dir_ + "/objects.log";
    size_t i = 0;
    while (i < cold.size()) {
      const uint64_t start = cold[i].off;
      size_t j = i;
      // Take as many as share a window. A record larger than the window is
      // fetched on its own.
      while (j < cold.size() && (cold[j].off + cold[j].len) - start <= kFetchWindow) ++j;
      if (j == i) ++j;
      const uint64_t span = (cold[j - 1].off + cold[j - 1].len) - start;
      auto buf = host_.read_range(log, start, span);
      if (!buf.ok()) { i = j; continue; }
      for (size_t k = i; k < j; ++k) {
        const uint64_t rel = cold[k].off - start;
        if (rel + cold[k].len > buf->size()) continue;
        Reader br(ByteSpan(buf->data() + rel, cold[k].len));
        auto o = StoredObject::decode(br, false);
        if (!o.ok()) continue;
        HotEntry he;
        he.bytes = footprint_of(*o);
        he.obj = std::move(*o);
        lru_.push_front(cold[k].id);
        he.lru = lru_.begin();
        hot_bytes_ += he.bytes;
        hot_.emplace(cold[k].id, std::move(he));
      }
      evict_locked();
      i = j;
    }
  }

  // Now serve them in the order asked for, from memory or cold blocks.
  const uint64_t now_wall = host_.wall_ns();
  for (const auto& id : ids) {
    auto hit = hot_.find(id);
    if (hit != hot_.end()) {
      if (hit->second.obj.is_expired(now_wall)) continue;
      lru_.splice(lru_.begin(), lru_, hit->second.lru);
      hit->second.lru = lru_.begin();
      fn(hit->second.obj.decompressed());
      continue;
    }
    auto it = entries_.find(id);
    if (it != entries_.end()) {
      const StoredObject* p = hot_ref_locked(id, it->second);
      if (p && !p->is_expired(now_wall)) fn(p->decompressed());
      continue;
    }
    auto cold_obj = find_in_cold_blocks_locked(id);
    if (cold_obj.ok()) {
      if (cold_obj->is_expired(now_wall)) continue;
      HotEntry he;
      he.bytes = footprint_of(*cold_obj);
      he.obj = *cold_obj;
      lru_.push_front(id);
      he.lru = lru_.begin();
      hot_bytes_ += he.bytes;
      hot_.emplace(id, std::move(he));
      evict_locked();
      fn(cold_obj->decompressed());
    }
  }
}
std::vector<SimilarHit> ObjectStore::find_similar(const std::vector<float>& e, size_t k) const { hal::LockGuard g(mu_); return semantic_.nearest(e, k); }

// The index, exposed. Each one takes the store's lock for the same reason
// find_similar does: `put` mutates the index from another thread.
size_t ObjectStore::semantic_size() const { hal::LockGuard g(mu_); return semantic_.size(); }
size_t ObjectStore::semantic_approx_size() const { hal::LockGuard g(mu_); return semantic_.approx_size(); }
bool ObjectStore::semantic_approx_ready() const { hal::LockGuard g(mu_); return semantic_.approx_ready(); }
std::vector<SimilarHit> ObjectStore::semantic_exact(const std::vector<float>& q, size_t k) const { hal::LockGuard g(mu_); return semantic_.nearest_exact(q, k); }
std::vector<SimilarHit> ObjectStore::semantic_approx(const std::vector<float>& q, size_t k) const { hal::LockGuard g(mu_); return semantic_.nearest_approx(q, k); }
std::vector<float> ObjectStore::semantic_vector_at(size_t i) const { hal::LockGuard g(mu_); return semantic_.vector_at(i); }
std::vector<ObjectRelation> ObjectStore::find_related(const ObjectID& id) const {
  hal::LockGuard g(mu_);
  auto v = graph_.out(id);
  for (auto& r : graph_.in(id)) v.push_back(r);
  return v;
}
std::vector<ObjectID> ObjectStore::find_history(const ObjectID& id) const {
  hal::LockGuard g(mu_);
  std::vector<ObjectID> chain;
  ObjectID cur = id;
  size_t guard = 0;
  while (!cur.is_null() && guard++ < 100000) {
    auto hit = hot_.find(cur);
    if (hit != hot_.end()) {
      chain.push_back(cur);
      cur = hit->second.obj.derived_from;
      continue;
    }
    auto it = entries_.find(cur);
    if (it != entries_.end()) {
      const StoredObject* o = hot_ref_locked(cur, it->second);
      if (!o) break;
      chain.push_back(cur);
      cur = o->derived_from;
      continue;
    }
    auto cold_obj = find_in_cold_blocks_locked(cur);
    if (cold_obj.ok()) {
      chain.push_back(cur);
      cur = cold_obj->derived_from;
      continue;
    }
    break;
  }
  return chain;
}

Result<void> ObjectStore::verify_provenance_chain(const ObjectID& id) const {
  hal::LockGuard g(mu_);
  ObjectID cur = id;
  size_t guard = 0;
  while (!cur.is_null() && guard++ < 100000) {
    StoredObject obj;
    bool found = false;
    auto hit = hot_.find(cur);
    if (hit != hot_.end()) {
      obj = hit->second.obj;
      found = true;
    } else {
      auto it = entries_.find(cur);
      if (it != entries_.end()) {
        const StoredObject* odp = hot_ref_locked(cur, it->second);
        if (odp) { obj = *odp; found = true; }
      } else {
        auto cold = find_in_cold_blocks_locked(cur);
        if (cold.ok()) { obj = *cold; found = true; }
      }
    }
    if (!found) {
      return Result<void>(Status::NotFound, "broken provenance chain: missing parent object ID " + cur.str());
    }
    std::string expected_hash = obj.compute_hash();
    std::string stored_hash = obj.meta("crypto.hash");
    if (!stored_hash.empty() && stored_hash != expected_hash) {
      return Result<void>(Status::Corrupt, "provenance tamper detected at object ID " + cur.str() + ": cryptographic hash mismatch");
    }
    cur = obj.derived_from;
  }
  return Result<void>();
}
const ObjectStore::NumericIndex* ObjectStore::ensure_numeric_locked(const std::string& key) const {
  auto built_it = numeric_built_at_.find(key);
  auto ni_it = numeric_index_.find(key);
  const bool fresh = ni_it != numeric_index_.end() && built_it != numeric_built_at_.end() &&
                     built_it->second == index_revision_;
  if (fresh) return ni_it->second.built ? &ni_it->second : nullptr;

  // Only a key the value index covers can be ordered this way: that index is
  // the source of the values, and reading it costs nothing.
  auto vi = meta_index_.find(key);
  if (vi == meta_index_.end() || !vi->second.active) {
    numeric_index_[key] = NumericIndex{};
    numeric_built_at_[key] = index_revision_;
    return nullptr;
  }

  NumericIndex ni;
  size_t numeric = 0, total = 0;
  for (const auto& [val, ids] : vi->second.by_value) {
    total++;
    if (val.empty()) continue;
    const char* end = nullptr;
    const double d = aios::to_f64(val.c_str(), &end);
    if (end == val.c_str()) continue;              // not a number at all
    while (*end && std::isspace((unsigned char)*end)) end++;
    if (*end != '\0') continue;                    // trailing junk: not numeric
    numeric++;
    for (const uint32_t o : ids) ni.sorted.emplace_back(d, order_[o]);
  }
  // A key whose values are mostly not numbers cannot be ordered numerically;
  // say so rather than order a fraction of the rows and look correct.
  if (total == 0 || numeric * 2 < total) {
    numeric_index_[key] = NumericIndex{};
    numeric_built_at_[key] = index_revision_;
    return nullptr;
  }
  std::sort(ni.sorted.begin(), ni.sorted.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  ni.built = true;
  numeric_index_[key] = std::move(ni);
  numeric_built_at_[key] = index_revision_;
  auto out = numeric_index_.find(key);
  return out->second.built ? &out->second : nullptr;
}

bool ObjectStore::numeric_ordered(const std::string& key) const {
  hal::LockGuard g(mu_);
  return ensure_numeric_locked(key) != nullptr;
}

bool ObjectStore::order_by_meta(const std::string& key, bool desc, size_t limit,
                                std::vector<ObjectID>& out) const {
  hal::LockGuard g(mu_);
  const NumericIndex* ni = ensure_numeric_locked(key);
  if (!ni) return false;
  out.clear();
  const size_t want = limit == 0 ? ni->sorted.size() : std::min(limit, ni->sorted.size());
  out.reserve(want);
  if (desc) {
    for (auto it = ni->sorted.rbegin(); it != ni->sorted.rend() && out.size() < want; ++it)
      out.push_back(it->second);
  } else {
    for (auto it = ni->sorted.begin(); it != ni->sorted.end() && out.size() < want; ++it)
      out.push_back(it->second);
  }
  return true;
}

bool ObjectStore::range_meta(const std::string& key, const std::string& op, double bound,
                             std::vector<ObjectID>& out) const {
  hal::LockGuard g(mu_);
  const NumericIndex* ni = ensure_numeric_locked(key);
  if (!ni) return false;
  out.clear();
  const auto& v = ni->sorted;
  auto at = [](const std::pair<double, ObjectID>& p) { return p.first; };
  (void)at;
  if (op == ">" || op == ">=") {
    auto first = op == ">" ? std::upper_bound(v.begin(), v.end(), bound,
                    [](double b, const auto& e) { return b < e.first; })
                           : std::lower_bound(v.begin(), v.end(), bound,
                    [](const auto& e, double b) { return e.first < b; });
    for (auto it = first; it != v.end(); ++it) out.push_back(it->second);
    return true;
  }
  if (op == "<" || op == "<=") {
    auto last = op == "<" ? std::lower_bound(v.begin(), v.end(), bound,
                    [](const auto& e, double b) { return e.first < b; })
                          : std::upper_bound(v.begin(), v.end(), bound,
                    [](double b, const auto& e) { return b < e.first; });
    for (auto it = v.begin(); it != last; ++it) out.push_back(it->second);
    return true;
  }
  return false;
}

bool ObjectStore::meta_indexed(const std::string& key) const {
  hal::LockGuard g(mu_);
  auto it = meta_index_.find(key);
  return it != meta_index_.end() && it->second.active;
}

size_t ObjectStore::count_meta_approx(const std::string& key, const std::string& value) const {
  hal::LockGuard g(mu_);
  auto vi = meta_index_.find(key);
  if (vi == meta_index_.end() || !vi->second.active || value.size() > kMaxIndexedValue) {
    auto ki = meta_key_index_.find(key);
    return ki == meta_key_index_.end() ? 0 : ki->second.size();
  }
  auto b = vi->second.by_value.find(value);
  return b == vi->second.by_value.end() ? 0 : b->second.size();
}

std::vector<ObjectID> ObjectStore::find_meta_locked(const std::string& key, const std::string& value,
                                                    bool count_only, size_t* out_count) const {
  std::vector<ObjectID> out;
  size_t total_matches = 0;

  // 1. Check active set
  auto vi = meta_index_.find(key);
  // The value index is NOT pruned when a block is sealed: it keeps the ids of
  // cold objects too, which is the whole reason a query can still find them
  // without reading the log. So when it answers, it answers for every tier and
  // the cold scan below must not run — it would return each sealed match a
  // second time. It did: 93,295 matching objects came back as 186,590 rows.
  const bool indexed = vi != meta_index_.end() && vi->second.active && value.size() <= kMaxIndexedValue;
  if (indexed) {
    // Indexed: the bucket IS the answer. Nothing is decoded.
    auto b = vi->second.by_value.find(value);
    if (b != vi->second.by_value.end()) {
      total_matches += b->second.size();
      if (!count_only) {
        out.reserve(b->second.size());
        for (const uint32_t o : b->second) out.push_back(order_[o]);
      }
    }
  } else {
    // Not indexed: walk the objects that carry the key at all in active set
    auto ki = meta_key_index_.find(key);
    if (ki != meta_key_index_.end()) {
      for (const uint32_t ord : ki->second) {
        const ObjectID& id = order_[ord];
        auto eit = entries_.find(id);
        if (eit == entries_.end()) continue;
        const StoredObject* od = hot_ref_locked(id, eit->second);
        if (!od) continue;
        for (const auto& [k, v] : od->metadata) {
          if (k == key && (value.empty() || v == value)) {
            if (!count_only) out.push_back(id);
            total_matches++;
            break;
          }
        }
      }
    }
  }

  // 2. Cold blocks, ONLY for a key the index cannot answer. The branch above
  // skips sealed ids (they have no entries_ record), so this is what finds
  // them — and for an indexed key it is pure duplication.
  if (!indexed && !cold_blocks_.empty()) {
    const std::string log = dir_ + "/objects.log";
    for (const auto& b : cold_blocks_) {
      auto buf = host_.read_range(log, b.file_offset, b.file_len);
      if (!buf.ok() || buf->size() < 16) continue;
      Reader r(ByteSpan(buf->data(), buf->size()));
      while (r.remaining() >= 16) {
        uint32_t magic = r.u32();
        uint32_t len = r.u32();
        if (magic != OBJ_MAGIC || 16 + (uint64_t)len > r.remaining() + 8) break;
        ByteSpan body = r.raw(len);
        uint64_t sum = r.u64();
        if (!r.ok() || sum != fnv1a(body)) break;
        Reader br(body);
        auto obj = StoredObject::decode(br);
        if (!obj.ok()) continue;
        for (const auto& [k, v] : obj->metadata) {
          if (k == key && (value.empty() || v == value)) {
            if (!count_only) out.push_back(obj->id);
            total_matches++;
            break;
          }
        }
      }
    }
  }

  if (out_count) *out_count = total_matches;
  return out;
}

std::vector<ObjectID> ObjectStore::find_meta(const std::string& key, const std::string& value) const {
  hal::LockGuard g(mu_);
  if (value.empty()) {
    auto it = meta_key_index_.find(key);
    std::vector<ObjectID> all;
    if (it != meta_key_index_.end()) {
      all.reserve(it->second.size());
      for (const uint32_t o : it->second) all.push_back(order_[o]);
    }
    return all;
  }
  return find_meta_locked(key, value, false, nullptr);
}

size_t ObjectStore::count_meta(const std::string& key, const std::string& value) const {
  hal::LockGuard g(mu_);
  if (value.empty()) {
    auto it = meta_key_index_.find(key);
    return it == meta_key_index_.end() ? 0 : it->second.size();
  }
  size_t n = 0;
  find_meta_locked(key, value, true, &n);
  return n;
}

std::vector<ObjectID> ObjectStore::find_meta_exact(const std::string& key, const std::string& value) const {
  hal::LockGuard g(mu_);
  return find_meta_locked(key, value, false, nullptr);
}

size_t ObjectStore::count_meta_exact(const std::string& key, const std::string& value) const {
  hal::LockGuard g(mu_);
  size_t n = 0;
  find_meta_locked(key, value, true, &n);
  return n;
}

std::vector<ObjectID> ObjectStore::find_state(const StateID& state) const {
  hal::LockGuard g(mu_);
  std::vector<ObjectID> out;
  for (auto& id : order_) {
    auto it = entries_.find(id);
    if (it == entries_.end()) continue;
    const StoredObject* o = hot_ref_locked(id, it->second);
    if (o && o->source_state == state) out.push_back(id);
  }
  if (!cold_blocks_.empty()) {
    const std::string log = dir_ + "/objects.log";
    for (const auto& b : cold_blocks_) {
      auto buf = host_.read_range(log, b.file_offset, b.file_len);
      if (!buf.ok() || buf->size() < 16) continue;
      Reader r(ByteSpan(buf->data(), buf->size()));
      while (r.remaining() >= 16) {
        uint32_t magic = r.u32();
        uint32_t len = r.u32();
        if (magic != OBJ_MAGIC || 16 + (uint64_t)len > r.remaining() + 8) break;
        ByteSpan body = r.raw(len);
        uint64_t sum = r.u64();
        if (!r.ok() || sum != fnv1a(body)) break;
        Reader br(body);
        auto obj = StoredObject::decode(br);
        if (!obj.ok()) continue;
        if (obj->source_state == state) out.push_back(obj->id);
      }
    }
  }
  return out;
}
// Sealed blocks never change: once a block is in cold_blocks_ its bytes in
// objects.log are immutable, so the ids it contributes are read from disk once
// and then remembered. Only blocks the cache has not seen yet are decoded; a
// vacuum() rewrites the log and bumps cold_epoch_, which throws the cache away.
void ObjectStore::append_cold_ids_locked(std::vector<ObjectID>& out) const {
  if (cold_ids_cache_epoch_ != cold_epoch_) {
    cold_ids_cache_.clear();
    cold_ids_cached_blocks_ = 0;
    cold_ids_cache_epoch_ = cold_epoch_;
  }
  if (cold_ids_cached_blocks_ > cold_blocks_.size()) {   // cannot happen without a clear
    cold_ids_cache_.clear();
    cold_ids_cached_blocks_ = 0;
  }
  if (cold_ids_cached_blocks_ < cold_blocks_.size()) {
    // A sealed block already carries every object's id and offset. Reading
    // them back out of the log instead stopped at the first record that was
    // not an object, and a replay-derived block can enclose footers an older
    // build left in the middle of history: on a live 44,486-object store that
    // hid 15,870 objects from list() and from short-id lookups. Offset order
    // is log order, which is the order the scan produced.
    for (size_t i = cold_ids_cached_blocks_; i < cold_blocks_.size(); ++i) {
      std::vector<const SparseBlock::Loc*> by_offset;
      by_offset.reserve(cold_blocks_[i].locs.size());
      for (const auto& loc : cold_blocks_[i].locs) by_offset.push_back(&loc);
      std::sort(by_offset.begin(), by_offset.end(),
                [](const SparseBlock::Loc* x, const SparseBlock::Loc* y) { return x->offset < y->offset; });
      for (const auto* loc : by_offset) {
        Id128 id;
        id.hi = loc->hi;
        id.lo = loc->lo;
        cold_ids_cache_.push_back(ObjectID{id});
      }
    }
    cold_ids_cached_blocks_ = cold_blocks_.size();
  }
  out.insert(out.end(), cold_ids_cache_.begin(), cold_ids_cache_.end());
}

std::vector<ObjectID> ObjectStore::list() const {
  hal::LockGuard g(mu_);
  std::vector<ObjectID> out;
  out.reserve(cold_ids_cache_.size() + order_.size());
  append_cold_ids_locked(out);
  for (const auto& id : order_) {
    if (entries_.count(id)) out.push_back(id);
  }
  return out;
}

size_t ObjectStore::count() const {
  hal::LockGuard g(mu_);
  return cold_objects_count_ + entries_.size();
}

Result<VacuumStats> ObjectStore::vacuum() {
  hal::LockGuard g(mu_);
  VacuumStats stats;

  const std::string log = dir_ + "/objects.log";
  auto info = host_.stat(log);
  if (!info.ok() || info->size == 0) {
    return stats;
  }
  stats.bytes_before = info->size;

  const uint64_t end = info->size;
  const uint64_t now_wall = host_.wall_ns();

  static constexpr uint64_t kWindow = 4ull << 20;
  static constexpr uint64_t kWriteChunk = 8ull << 20;

  // PASS 1 -- decide what survives, and remember only WHERE it lives.
  //
  // This used to decode every live object into a vector, then encode a second
  // complete copy of all of them into one buffer before writing. On a 180MB
  // log that is ~200MB of transient allocation, and since the allocator never
  // handed it back the vacuum finished with the process holding MORE memory
  // than before it ran -- the compaction was paid for in the leak it exists
  // to fix. A surviving record is kept verbatim, so all that has to be
  // remembered about it is its offset and length: 12 bytes, not a copy.
  struct Rec { uint64_t offset = 0; uint32_t len = 0; };
  std::vector<Rec> recs;
  std::vector<bool> alive;
  std::unordered_map<std::string, size_t> key_to_index;

  ByteBuffer win;
  uint64_t win_base = 0;
  bool win_valid = false;
  auto ensure = [&](uint64_t at, uint64_t need) -> bool {
    if (win_valid && at >= win_base && at + need <= win_base + win.size()) return true;
    if (at + need > end) return false;
    uint64_t want = std::max<uint64_t>(need, kWindow);
    if (at + want > end) want = end - at;
    auto buf = host_.read_range(log, at, want);
    if (!buf.ok() || buf->size() < need) return false;
    win = std::move(*buf);
    win_base = at;
    win_valid = true;
    return true;
  };

  uint64_t offset = 0;
  while (offset + 8 <= end) {
    if (!ensure(offset, 8)) break;
    Reader hr(ByteSpan(win.data() + (offset - win_base), 8));
    uint32_t magic = hr.u32();
    uint32_t len = hr.u32();

    if (magic == BLOCK_FOOTER_MAGIC || magic == META_FOOTER_MAGIC) {
      // Footers describe the old layout; pass 2 writes new ones for the
      // compacted image, so these are dropped.
      offset += 8 + (uint64_t)len;
      continue;
    }
    if (magic != OBJ_MAGIC) break;

    const uint64_t rec_len = 16 + (uint64_t)len;
    if (offset + rec_len > end) break;
    if (!ensure(offset, rec_len)) break;

    Reader r(ByteSpan(win.data() + (offset - win_base), (size_t)rec_len));
    r.u32(); r.u32();
    ByteSpan body = r.raw(len);
    uint64_t sum = r.u64();
    if (!r.ok() || sum != fnv1a(body)) break;   // torn tail: keep what was whole
    Reader br(body);
    auto o = StoredObject::decode(br, false);
    if (!o.ok()) break;

    stats.total_objects_scanned++;
    const std::string key = o->meta("key");
    if (o->is_expired(now_wall)) {
      stats.expired_objects_pruned++;
    } else if (o->is_tombstone()) {
      // The delete marker retires WITH the row it deletes: neither the
      // tombstone nor any older version of that key is written out.
      stats.tombstones_pruned++;
      if (!key.empty()) {
        auto kit = key_to_index.find(key);
        if (kit != key_to_index.end()) {
          alive[kit->second] = false;
          stats.dead_objects_pruned++;
          key_to_index.erase(kit);
        }
      }
    } else {
      const size_t idx = recs.size();
      recs.push_back(Rec{offset, (uint32_t)rec_len});
      alive.push_back(true);
      if (!key.empty()) {
        auto kit = key_to_index.find(key);
        if (kit != key_to_index.end()) {
          alive[kit->second] = false;
          stats.dead_objects_pruned++;
        }
        key_to_index[key] = idx;
      }
    }

    offset += rec_len;
  }

  // The scan's own scratch goes back now, before the rewrite allocates.
  { std::unordered_map<std::string, size_t> gone; gone.swap(key_to_index); }
  { ByteBuffer gone; gone.swap(win); win_valid = false; }

  // PASS 2 -- copy the survivors, in log order, into a side file. Records are
  // byte-identical to what is already on disk, so this is a copy, not a
  // re-encode: adjacent survivors (the common case) come across in one read.
  //
  // The copy is sealed as it is written: blocks close by the same rule
  // index() applies during a replay (block_objects, and max_active objects
  // left unsealed), each block's footers land directly after its records, and
  // blocks.idx is rewritten to match. A bare copy left no footers and no
  // index, and the append path never seals history again, so every boot
  // after a VACUUM decoded the whole log.
  const std::string tmp = log + ".compacting";
  (void)host_.remove(tmp);
  bool tmp_started = false;
  uint64_t write_off = 0;   // length of the side file so far
  size_t live = 0;
  auto put_bytes = [&](const uint8_t* p, size_t n) -> Result<void> {
    if (n == 0) return Ok();
    if (!tmp_started) {
      AIOS_TRY(host_.write_file(tmp, ByteSpan(p, n)));
      tmp_started = true;
    } else {
      AIOS_TRY(host_.append_file(tmp, ByteSpan(p, n)));
    }
    write_off += n;
    return Ok();
  };

  SparseBlock blk;
  std::vector<ByteBuffer> blk_headers;
  ByteBuffer toc;
  size_t unsealed = 0;   // survivors written and not yet in a sealed block: what entries_ holds on replay
  auto seal = [&]() -> Result<void> {
    sort_block_by_lo(blk, blk_headers);
    const ByteBuffer bf = encode_block_footer(blk);
    const ByteBuffer mf = encode_meta_footer(blk, blk_headers);
    const uint64_t footer_at = write_off;
    AIOS_TRY(put_bytes(bf.data(), bf.size()));
    AIOS_TRY(put_bytes(mf.data(), mf.size()));
    const ByteBuffer rec = encode_toc_record(blk.file_offset, blk.file_len, footer_at, bf.size() + mf.size());
    toc.insert(toc.end(), rec.begin(), rec.end());
    unsealed -= blk.object_count;
    stats.blocks_sealed++;
    blk = SparseBlock{};
    blk_headers.clear();
    return Ok();
  };

  for (size_t i = 0; i < recs.size(); ) {
    if (!alive[i]) { ++i; continue; }
    const uint64_t run_start = recs[i].offset;
    uint64_t run_len = recs[i].len;   // a record larger than a chunk is a run of one
    size_t j = i + 1;
    while (j < recs.size() && alive[j] && recs[j].offset == run_start + run_len &&
           run_len + recs[j].len <= kWriteChunk) {
      run_len += recs[j].len;
      ++j;
    }
    auto buf = host_.read_range(log, run_start, run_len);
    if (!buf.ok() || buf->size() < run_len) {
      (void)host_.remove(tmp);
      return Result<VacuumStats>(Status::Corrupt, "a record vanished from objects.log mid-vacuum");
    }
    for (size_t k = i; k < j; ++k) {
      const size_t at = (size_t)(recs[k].offset - run_start);
      const uint32_t body_len = recs[k].len - 16;
      Reader br(ByteSpan(buf->data() + at + 8, body_len));
      auto o = StoredObject::decode(br, false);
      if (!o.ok()) {
        (void)host_.remove(tmp);
        return Result<VacuumStats>(Status::Corrupt, "a record changed in objects.log mid-vacuum");
      }
      ByteBuffer body;
      { Writer w(body); o->encode(w); }
      const uint32_t new_body_len = (uint32_t)body.size();
      const uint32_t new_rec_len = 16 + new_body_len;
      ByteBuffer rec;
      { Writer rw(rec); rw.u32(OBJ_MAGIC); rw.u32(new_body_len); rw.raw(body); rw.u64(fnv1a(body)); }

      const uint64_t rec_at = write_off;
      if (blk.object_count == 0) {
        blk.file_offset = rec_at;
        blk.first_id = o->id;
        blk.min_timestamp_ns = blk.max_timestamp_ns = o->timestamp_ns;
      }
      blk.file_len = rec_at + new_rec_len - blk.file_offset;
      blk.last_id = o->id;
      blk.min_timestamp_ns = std::min(blk.min_timestamp_ns, o->timestamp_ns);
      blk.max_timestamp_ns = std::max(blk.max_timestamp_ns, o->timestamp_ns);
      blk.add_bloom(o->id);
      blk.locs.push_back(SparseBlock::Loc{o->id.lo, o->id.hi, rec_at + 8, new_body_len});
      blk_headers.push_back(encode_object_header(*o));
      blk.object_count++;
      ++live;
      ++unsealed;
      AIOS_TRY(put_bytes(rec.data(), rec.size()));
      if (blk.object_count >= config_.block_objects && unsealed >= config_.max_active) {
        AIOS_TRY(seal());
      }
      stats.bytes_copied += new_rec_len;
    }
    i = j;
  }
  { std::vector<Rec> gone; gone.swap(recs); }
  { std::vector<bool> gone; gone.swap(alive); }
  // The unsealed tail stays bare records, as on the append path.
  { std::vector<ByteBuffer> gone; gone.swap(blk_headers); }
  { std::vector<SparseBlock::Loc> gone; gone.swap(blk.locs); }
  blk = SparseBlock{};

  if (!tmp_started) {
    ByteBuffer empty;
    AIOS_TRY(host_.write_file(tmp, ByteSpan(empty.data(), 0)));
  }

  const std::string tmp_idx = dir_ + "/blocks.idx.compacting";
  const std::string tmp_ready = log + ".compacting.ready";
  (void)host_.remove(tmp_ready);
  (void)host_.remove(tmp_idx);
  if (!toc.empty()) {
    (void)host_.write_file(tmp_idx, ByteSpan(toc.data(), toc.size()));
  } else {
    ByteBuffer empty_idx;
    (void)host_.write_file(tmp_idx, ByteSpan(empty_idx.data(), 0));
  }
  uint8_t rbyte = 1;
  (void)host_.write_file(tmp_ready, ByteSpan(&rbyte, 1));

  // The old index names offsets in the old log: gone before the log changes,
  // so a crash mid-swap leaves no index (a full decode) rather than a wrong one.
  (void)host_.remove(dir_ + "/blocks.idx");

  // Swap the compacted image in. There is no rename on the host boundary, so
  // the copy back is chunked for the same reason the write out was: the whole
  // store must never sit in one buffer. The side file is only removed once
  // the copy has completed, so a crash mid-copy leaves the compacted image
  // next to the log rather than nothing at all.
  auto tinfo = host_.stat(tmp);
  const uint64_t tsize = tinfo.ok() ? tinfo->size : 0;
  uint64_t copied = 0;
  bool log_started = false;
  while (copied < tsize) {
    const uint64_t want = std::min<uint64_t>(kWriteChunk, tsize - copied);
    auto buf = host_.read_range(tmp, copied, want);
    // A short read is NOT a successful copy. It used to break out of this loop
    // and fall through to the removal of the compacted image, which is silent
    // data loss: objects.log had already been truncated and rewritten with
    // part of the new image, and the one complete copy was then deleted. The
    // side image is deliberately left in place so the swap can be retried or
    // recovered by hand.
    if (!buf.ok() || buf->empty() || buf->size() > want) {
      return Result<VacuumStats>(Status::IoError,
          "short read copying the compacted image back over objects.log; the "
          "complete compacted image is retained at " + tmp);
    }
    if (!log_started) {
      AIOS_TRY(host_.write_file(log, ByteSpan(buf->data(), buf->size())));
      log_started = true;
    } else {
      AIOS_TRY(host_.append_file(log, ByteSpan(buf->data(), buf->size())));
    }
    copied += buf->size();
  }
  if (!log_started) {
    ByteBuffer empty;
    AIOS_TRY(host_.write_file(log, ByteSpan(empty.data(), 0)));
  }
  // Prove the destination is the whole image before anything is deleted. A
  // truncated objects.log is a store that cannot reopen, so this is the last
  // point at which the good side image is still the only complete copy.
  if (copied != tsize) {
    return Result<VacuumStats>(Status::IoError,
        "objects.log is " + std::to_string(copied) + " of " + std::to_string(tsize) +
        " bytes after the copy; the complete compacted image is retained at " + tmp);
  }
  auto lcheck = host_.stat(log);
  if (!lcheck.ok() || lcheck->size != tsize) {
    return Result<VacuumStats>(Status::IoError,
        "objects.log did not verify at " + std::to_string(tsize) +
        " bytes after the copy; the complete compacted image is retained at " + tmp);
  }
  if (!toc.empty()) {
    // Fatal, not a warning. This index is the entire point of the rewrite:
    // without it the next boot cannot skip-decode and falls back to a full
    // decode of the log. Reporting a successful vacuum that silently lost it
    // is worse than failing the vacuum.
    auto w = host_.write_file(dir_ + "/blocks.idx", ByteSpan(toc.data(), toc.size()));
    if (!w.ok()) {
      return Result<VacuumStats>(w.error().status,
          "writing the regenerated blocks.idx after vacuum failed: " +
          w.error().message + "; the compacted image is retained at " + tmp);
    }
  }
  { ByteBuffer gone; gone.swap(toc); }

  // Reset all in-memory indexes and state.
  //
  // clear() is NOT enough here and that is the whole bug: an unordered_map
  // keeps its bucket array, a vector keeps its capacity, so every structure
  // stayed exactly as large as it was at peak and the dead objects' index
  // memory was never released at all. Swapping against a fresh empty
  // container is what actually frees it.
  { std::unordered_map<ObjectID, Entry, TypedIdHash<9>> gone; gone.swap(entries_); }
  { std::vector<SparseBlock> gone; gone.swap(cold_blocks_); }
  cold_objects_count_ = 0;
  { std::vector<ObjectID> gone; gone.swap(cold_ids_cache_); }
  cold_ids_cached_blocks_ = 0;
  ++cold_epoch_;   // the log has been rewritten; the cached id list is stale
  cur_block_ = SparseBlock{};
  { std::vector<ObjectID> gone; gone.swap(cur_block_ids_); }
  { std::vector<ByteBuffer> gone; gone.swap(cur_block_headers_); }
  { std::vector<SkipSpan> gone; gone.swap(known_spans_); }
  { std::vector<bool> gone; gone.swap(span_consumed_); }
  { std::unordered_map<ObjectID, HotEntry, TypedIdHash<9>> gone; gone.swap(hot_); }
  { std::list<ObjectID> gone; gone.swap(lru_); }
  hot_bytes_ = 0;
  { std::vector<ObjectID> gone; gone.swap(order_); }
  { std::unordered_map<std::string, std::vector<uint32_t>> gone; gone.swap(meta_key_index_); }
  { std::unordered_map<std::string, ValueIndex> gone; gone.swap(meta_index_); }
  { std::unordered_map<std::string, NumericIndex> gone; gone.swap(numeric_index_); }
  { std::unordered_map<std::string, uint64_t> gone; gone.swap(numeric_built_at_); }
  semantic_.clear();
  graph_.clear();
  consumed_ = 0;
  blocks_idx_consumed_ = 0;
  index_revision_ = 0;

  // The freed index memory goes back to the OS now, not whenever the allocator decides to.
#if !defined(AIOS_METAL) && defined(__GLIBC__)   // malloc_trim is glibc's (see the <malloc.h> include)
  malloc_trim(0);
#endif

  // Replay & reindex the compacted log
  load_known_spans_locked();
  auto ref_res = refresh_locked();
  if (!ref_res.ok()) return ref_res.error();
  // The replay builds the new indexes through a high-water of temporaries: trim again once they are gone.
#if !defined(AIOS_METAL) && defined(__GLIBC__)   // malloc_trim is glibc's (see the <malloc.h> include)
  malloc_trim(0);
#endif

  // Only now, with the new log in place, its index installed, and the replay
  // of that log proven, is the side image redundant. Removing it earlier left
  // no recovery path if the replay below failed.
  (void)host_.remove(tmp_ready);
  (void)host_.remove(tmp_idx);
  (void)host_.remove(tmp);

  auto st_after = host_.stat(log);
  stats.bytes_after = st_after.ok() ? st_after->size : tsize;
  stats.live_objects_written = live;

  // Everything above only returned memory to the allocator. This returns it
  // to the machine -- the point of the exercise.
  host_.release_free_memory();

  return stats;
}

}  // namespace aios
