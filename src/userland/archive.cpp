#include "aios/userland/archive.hpp"

#include <cstring>

#include "aios/userland/git.hpp"   // inflate_raw: the same deflate reader, again

namespace aios::userland {
namespace {

std::string dec(uint64_t v) {
  if (!v) return "0";
  std::string s;
  while (v) { s.insert(s.begin(), (char)('0' + v % 10)); v /= 10; }
  return s;
}

uint32_t le32(ByteSpan b, size_t at) {
  return (uint32_t)b[at] | ((uint32_t)b[at + 1] << 8) | ((uint32_t)b[at + 2] << 16) | ((uint32_t)b[at + 3] << 24);
}
uint16_t le16(ByteSpan b, size_t at) { return (uint16_t)((uint32_t)b[at] | ((uint32_t)b[at + 1] << 8)); }

}  // namespace

uint32_t crc32(ByteSpan data) {
  static uint32_t table[256];
  static bool ready = false;
  if (!ready) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
      table[i] = c;
    }
    ready = true;
  }
  uint32_t c = 0xffffffffu;
  for (uint8_t b : data) c = table[(c ^ b) & 0xff] ^ (c >> 8);
  return c ^ 0xffffffffu;
}

Result<std::vector<ArchiveEntry>> unzip(ByteSpan zip) {
  if (zip.size() < 22) return Result<std::vector<ArchiveEntry>>(Status::Corrupt, "too short to be a zip");

  // The end-of-central-directory record is at the end, possibly behind a
  // comment, so it is searched for backwards.
  size_t eocd = 0;
  bool found = false;
  size_t limit = zip.size() > 66000 ? zip.size() - 66000 : 0;
  for (size_t i = zip.size() - 22 + 1; i-- > limit;) {
    if (le32(zip, i) == 0x06054b50) { eocd = i; found = true; break; }
  }
  if (!found) return Result<std::vector<ArchiveEntry>>(Status::Corrupt, "no end-of-central-directory record");

  uint32_t count = le16(zip, eocd + 10);
  uint32_t dir_size = le32(zip, eocd + 12);
  uint32_t dir_at = le32(zip, eocd + 16);
  if ((size_t)dir_at + dir_size > zip.size())
    return Result<std::vector<ArchiveEntry>>(Status::Corrupt, "the central directory is not inside the file");

  std::vector<ArchiveEntry> out;
  size_t p = dir_at;
  for (uint32_t i = 0; i < count; ++i) {
    if (p + 46 > zip.size() || le32(zip, p) != 0x02014b50)
      return Result<std::vector<ArchiveEntry>>(Status::Corrupt, "central directory entry " + dec(i) + " is malformed");
    uint16_t method = le16(zip, p + 10);
    uint32_t crc = le32(zip, p + 16);
    uint32_t comp_size = le32(zip, p + 20);
    uint32_t raw_size = le32(zip, p + 24);
    uint16_t name_len = le16(zip, p + 28);
    uint16_t extra_len = le16(zip, p + 30);
    uint16_t comment_len = le16(zip, p + 32);
    uint32_t external = le32(zip, p + 38);
    uint32_t local_at = le32(zip, p + 42);
    if (p + 46 + name_len > zip.size())
      return Result<std::vector<ArchiveEntry>>(Status::Corrupt, "a name that runs past the directory");
    std::string name((const char*)zip.data() + p + 46, name_len);
    p += 46 + name_len + extra_len + comment_len;

    ArchiveEntry e;
    e.path = name;
    e.is_dir = !name.empty() && name.back() == '/';
    // The upper sixteen bits of the external attributes are the unix mode when
    // the archive was made on a unix; zero means "whatever the reader defaults to".
    uint32_t unix_mode = external >> 16;
    e.mode = unix_mode ? (unix_mode & 0777) : 0644;
    if (e.is_dir) { out.push_back(std::move(e)); continue; }

    if ((size_t)local_at + 30 > zip.size() || le32(zip, local_at) != 0x04034b50)
      return Result<std::vector<ArchiveEntry>>(Status::Corrupt, name + ": no local header where the directory says");
    uint16_t l_name = le16(zip, local_at + 26);
    uint16_t l_extra = le16(zip, local_at + 28);
    size_t data_at = (size_t)local_at + 30 + l_name + l_extra;
    if (data_at + comp_size > zip.size())
      return Result<std::vector<ArchiveEntry>>(Status::Corrupt, name + ": the entry runs past the end of the file");

    if (method == 0) {
      e.data.assign(zip.begin() + (long)data_at, zip.begin() + (long)(data_at + comp_size));
    } else if (method == 8) {
      auto raw = inflate_raw(zip, data_at, nullptr);
      if (!raw.ok()) return Result<std::vector<ArchiveEntry>>(raw.error());
      e.data = *raw;
    } else {
      return Result<std::vector<ArchiveEntry>>(Status::Unsupported,
                                              name + ": compression method " + dec(method));
    }
    if (e.data.size() != raw_size)
      return Result<std::vector<ArchiveEntry>>(Status::Corrupt,
                                              name + ": the directory says " + dec(raw_size) + " bytes, the entry held " +
                                                  dec(e.data.size()));
    // The checksum is in the archive for a reason; a reader that skips it turns
    // a damaged download into damaged files on disk.
    if (crc32(ByteSpan(e.data.data(), e.data.size())) != crc)
      return Result<std::vector<ArchiveEntry>>(Status::Corrupt, name + ": its CRC-32 does not match the archive's");
    out.push_back(std::move(e));
  }
  if (out.empty()) return Result<std::vector<ArchiveEntry>>(Status::Corrupt, "no entries in this archive");
  return out;
}

}  // namespace aios::userland
