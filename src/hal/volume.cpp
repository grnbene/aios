#include "aios/hal/volume.hpp"
#include <cstring>

namespace aios::hal {

namespace {
constexpr char SB_MAGIC[8] = {'A', 'O', 'S', 'V', 'O', 'L', '0', '1'};
constexpr uint32_t REC_MAGIC = 0x52534F41u;   // 'AOSR'
constexpr uint32_t REC_HDR = 48;

std::string dir_of(const std::string& p) { auto i = p.find_last_of('/'); return i == std::string::npos ? "" : p.substr(0, i); }
std::string norm(const std::string& p) { std::string s; for (char c : p) { if (c == '/' && (s.empty() || s.back() == '/')) continue; s.push_back(c); } if (!s.empty() && s.back() == '/') s.pop_back(); if (s.empty() || s[0] != '/') s = "/" + s; return s; }
}  // namespace

bool Volume::probe(BlockDevice& dev, std::string* label) {
  uint8_t sb[SECTOR];
  if (!dev.read(0, 1, sb).ok()) return false;
  if (std::memcmp(sb, SB_MAGIC, 8) != 0) return false;
  if (label) { Reader r(ByteSpan(sb + 8, SECTOR - 8)); r.u32(); r.u32(); r.u64(); r.u64(); r.u32(); r.u64(); *label = r.str(); }
  return true;
}

Result<void> Volume::format(BlockDevice& dev, const std::string& label) {
  if (dev.sectors() < 64) return Result<void>(Status::Invalid, "device too small for a volume");
  uint64_t half = (dev.sectors() - 1) / 2;
  ByteBuffer sb(SECTOR, 0);
  std::memcpy(sb.data(), SB_MAGIC, 8);
  { Writer w; w.u32(1); w.u32(SECTOR); w.u64(dev.sectors()); w.u64(half); w.u32(0); w.u64(1); w.str(label.substr(0, 32)); auto b = w.take(); std::memcpy(sb.data() + 8, b.data(), b.size()); }
  uint64_t sum = fnv1a(ByteSpan(sb.data(), SECTOR - 8)); std::memcpy(sb.data() + SECTOR - 8, &sum, 8);
  // Invalidate the first record slot of both halves so a stale log is never replayed.
  ByteBuffer zero(SECTOR, 0);
  AIOS_TRY(dev.write(1, 1, zero.data()));
  AIOS_TRY(dev.write(1 + half, 1, zero.data()));
  AIOS_TRY(dev.write(0, 1, sb.data()));
  return dev.flush();
}

Result<void> Volume::write_superblock() {
  ByteBuffer sb(SECTOR, 0);
  std::memcpy(sb.data(), SB_MAGIC, 8);
  { Writer w; w.u32(1); w.u32(SECTOR); w.u64(info_.sectors); w.u64(info_.half_sectors); w.u32(info_.active_half); w.u64(info_.generation); w.str(info_.label); auto b = w.take(); std::memcpy(sb.data() + 8, b.data(), b.size()); }
  uint64_t sum = fnv1a(ByteSpan(sb.data(), SECTOR - 8)); std::memcpy(sb.data() + SECTOR - 8, &sum, 8);
  AIOS_TRY(dev_->write(0, 1, sb.data()));
  return dev_->flush();
}

Result<void> Volume::mount(BlockDevice& dev) {
  uint8_t sb[SECTOR];
  AIOS_TRY(dev.read(0, 1, sb));
  if (std::memcmp(sb, SB_MAGIC, 8) != 0) return Result<void>(Status::NotFound, "no AOS volume on " + dev.name());
  uint64_t sum; std::memcpy(&sum, sb + SECTOR - 8, 8);
  if (sum != fnv1a(ByteSpan(sb, SECTOR - 8))) return Result<void>(Status::Corrupt, "volume superblock checksum");
  Reader r(ByteSpan(sb + 8, SECTOR - 8));
  uint32_t version = r.u32(); uint32_t ssz = r.u32();
  info_ = VolumeInfo{};
  info_.sectors = r.u64(); info_.half_sectors = r.u64(); info_.active_half = r.u32(); info_.generation = r.u64(); info_.label = r.str();
  if (version != 1 || ssz != SECTOR || !r.ok()) return Result<void>(Status::Unsupported, "volume format version");
  dev_ = &dev;
  files_.clear(); dirs_.clear(); dirs_.insert("/");
  // Replay the active half.
  uint64_t lba = half_start(info_.active_half), end = lba + info_.half_sectors;
  std::vector<uint8_t> hdr(SECTOR);
  while (lba < end) {
    if (!dev.read(lba, 1, hdr.data()).ok()) break;
    Reader h(hdr);
    if (h.u32() != REC_MAGIC) break;
    uint8_t type = h.u8(); h.u8(); uint16_t plen = h.u16(); uint32_t dlen = h.u32(); uint64_t gen = h.u64(); uint64_t sum2 = h.u64(); h.u64();
    uint64_t body = REC_HDR + plen + dlen, sectors = (body + SECTOR - 1) / SECTOR;
    if (lba + sectors > end) break;
    std::vector<uint8_t> rec(sectors * SECTOR);
    if (!dev.read(lba, (uint32_t)sectors, rec.data()).ok()) break;
    ByteSpan payload(rec.data() + REC_HDR, plen + dlen);
    if (fnv1a(payload) != sum2) break;   // torn or stale record: the log ends here
    std::string path((const char*)payload.data(), plen);
    ByteSpan data = payload.subspan(plen);
    if (type == Set) { files_[path].assign(data.begin(), data.end()); for (std::string d = dir_of(path); !d.empty(); d = dir_of(d)) dirs_.insert(d); }
    else if (type == Append) { auto& f = files_[path]; f.insert(f.end(), data.begin(), data.end()); for (std::string d = dir_of(path); !d.empty(); d = dir_of(d)) dirs_.insert(d); }
    else if (type == Dir) { dirs_.insert(path); for (std::string d = dir_of(path); !d.empty(); d = dir_of(d)) dirs_.insert(d); }
    else if (type == Del) { files_.erase(path); dirs_.erase(path); }
    info_.records++; if (gen > info_.generation) info_.generation = gen;
    lba += sectors;
  }
  write_sector_ = lba;
  info_.log_used_sectors = lba - half_start(info_.active_half);
  info_.files = files_.size();
  info_.bytes_live = 0; for (auto& [k, v] : files_) info_.bytes_live += v.size();
  return Ok();
}

Result<void> Volume::append_record(RecType t, const std::string& path, ByteSpan data) {
  if (!dev_) return Result<void>(Status::Unsupported, "no volume mounted");
  if (path.size() > 60000) return Result<void>(Status::Invalid, "path too long");
  uint64_t body = REC_HDR + path.size() + data.size(), sectors = (body + SECTOR - 1) / SECTOR;
  uint64_t end = half_start(info_.active_half) + info_.half_sectors;
  if (write_sector_ + sectors + 1 > end) {   // +1: keep one sector so the next header slot can be invalidated
    AIOS_TRY(compact());
    end = half_start(info_.active_half) + info_.half_sectors;
    if (write_sector_ + sectors + 1 > end) return Result<void>(Status::Exhausted, "volume full");
  }
  std::vector<uint8_t> rec(sectors * SECTOR, 0);
  ByteBuffer payload; payload.insert(payload.end(), path.begin(), path.end()); payload.insert(payload.end(), data.begin(), data.end());
  { Writer w; w.u32(REC_MAGIC); w.u8(t); w.u8(0); w.u16((uint16_t)path.size()); w.u32((uint32_t)data.size()); w.u64(++info_.generation); w.u64(fnv1a(payload)); w.u64(0); auto h = w.take(); std::memcpy(rec.data(), h.data(), h.size()); }
  std::memcpy(rec.data() + REC_HDR, payload.data(), payload.size());
  // The sector after this record must not look like a record (old log data).
  std::vector<uint8_t> zero(SECTOR, 0);
  AIOS_TRY(dev_->write(write_sector_ + sectors, 1, zero.data()));
  AIOS_TRY(dev_->write(write_sector_, (uint32_t)sectors, rec.data()));
  AIOS_TRY(dev_->flush());
  write_sector_ += sectors;
  info_.records++;
  info_.log_used_sectors = write_sector_ - half_start(info_.active_half);
  return Ok();
}

Result<void> Volume::compact() {
  // Rewrite the live set into the other half, then flip the superblock.
  uint32_t other = 1 - info_.active_half;
  uint64_t lba = half_start(other), end = lba + info_.half_sectors;
  auto emit = [&](RecType t, const std::string& path, ByteSpan data) -> Result<void> {
    uint64_t body = REC_HDR + path.size() + data.size(), sectors = (body + SECTOR - 1) / SECTOR;
    if (lba + sectors + 1 > end) return Result<void>(Status::Exhausted, "live set does not fit the volume");
    std::vector<uint8_t> rec(sectors * SECTOR, 0);
    ByteBuffer payload; payload.insert(payload.end(), path.begin(), path.end()); payload.insert(payload.end(), data.begin(), data.end());
    { Writer w; w.u32(REC_MAGIC); w.u8(t); w.u8(0); w.u16((uint16_t)path.size()); w.u32((uint32_t)data.size()); w.u64(++info_.generation); w.u64(fnv1a(payload)); w.u64(0); auto h = w.take(); std::memcpy(rec.data(), h.data(), h.size()); }
    std::memcpy(rec.data() + REC_HDR, payload.data(), payload.size());
    AIOS_TRY(dev_->write(lba, (uint32_t)sectors, rec.data()));
    lba += sectors;
    return Ok();
  };
  for (auto& d : dirs_) if (d != "/") AIOS_TRY(emit(Dir, d, {}));
  for (auto& [path, data] : files_) AIOS_TRY(emit(Set, path, data));
  std::vector<uint8_t> zero(SECTOR, 0);
  AIOS_TRY(dev_->write(lba, 1, zero.data()));
  AIOS_TRY(dev_->flush());
  info_.active_half = other;
  AIOS_TRY(write_superblock());
  write_sector_ = lba;
  info_.records = dirs_.size() - 1 + files_.size();
  info_.log_used_sectors = lba - half_start(other);
  info_.compactions++;
  return Ok();
}

Result<void> Volume::write_file(const std::string& p, ByteSpan data) {
  std::string path = norm(p);
  AIOS_TRY(append_record(Set, path, data));
  files_[path].assign(data.begin(), data.end());
  for (std::string d = dir_of(path); !d.empty(); d = dir_of(d)) dirs_.insert(d);
  info_.files = files_.size(); info_.bytes_live = 0; for (auto& [k, v] : files_) info_.bytes_live += v.size();
  return Ok();
}
Result<void> Volume::append_file(const std::string& p, ByteSpan data) {
  std::string path = norm(p);
  AIOS_TRY(append_record(Append, path, data));
  auto& f = files_[path]; f.insert(f.end(), data.begin(), data.end());
  for (std::string d = dir_of(path); !d.empty(); d = dir_of(d)) dirs_.insert(d);
  info_.files = files_.size(); info_.bytes_live += data.size();
  return Ok();
}
Result<ByteBuffer> Volume::read_file(const std::string& p) const {
  auto it = files_.find(norm(p));
  if (it == files_.end()) return Result<ByteBuffer>(Status::NotFound, p);
  return it->second;
}
Result<std::vector<std::string>> Volume::list_dir(const std::string& p) const {
  std::string path = norm(p);
  if (!dirs_.count(path)) return Result<std::vector<std::string>>(Status::NotFound, p);
  std::set<std::string> names;
  std::string prefix = path == "/" ? "/" : path + "/";
  for (auto& [f, v] : files_) if (f.rfind(prefix, 0) == 0) { std::string rest = f.substr(prefix.size()); names.insert(rest.substr(0, rest.find('/'))); }
  for (auto& d : dirs_) if (d != path && d.rfind(prefix, 0) == 0) { std::string rest = d.substr(prefix.size()); names.insert(rest.substr(0, rest.find('/'))); }
  return std::vector<std::string>(names.begin(), names.end());
}
Result<void> Volume::make_dir(const std::string& p) {
  std::string path = norm(p);
  if (dirs_.count(path)) return Ok();
  AIOS_TRY(append_record(Dir, path, {}));
  for (std::string d = path; !d.empty(); d = dir_of(d)) dirs_.insert(d);
  return Ok();
}
bool Volume::exists(const std::string& p) const { std::string path = norm(p); return files_.count(path) || dirs_.count(path); }
bool Volume::is_dir(const std::string& p) const { return dirs_.count(norm(p)) > 0; }
Result<uint64_t> Volume::size(const std::string& p) const { auto it = files_.find(norm(p)); if (it == files_.end()) return Result<uint64_t>(Status::NotFound, p); return (uint64_t)it->second.size(); }
Result<void> Volume::remove(const std::string& p) {
  std::string path = norm(p);
  if (!files_.count(path) && !dirs_.count(path)) return Result<void>(Status::NotFound, p);
  if (dirs_.count(path)) { std::string prefix = path + "/"; for (auto& [f, v] : files_) if (f.rfind(prefix, 0) == 0) return Result<void>(Status::Invalid, "directory not empty"); for (auto& d : dirs_) if (d != path && d.rfind(prefix, 0) == 0) return Result<void>(Status::Invalid, "directory not empty"); }
  AIOS_TRY(append_record(Del, path, {}));
  files_.erase(path); dirs_.erase(path);
  info_.files = files_.size(); info_.bytes_live = 0; for (auto& [k, v] : files_) info_.bytes_live += v.size();
  return Ok();
}

}  // namespace aios::hal
