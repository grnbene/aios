#include "aios/userland/archive.hpp"

#include <cstring>

#include "aios/net/deflate.hpp"

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
      std::string why;
      if (!net::inflate_raw_tree(zip, data_at, e.data, nullptr, raw_size, &why)) {
        return Result<std::vector<ArchiveEntry>>(Status::Corrupt, why);
      }
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

ByteBuffer zip_create(const std::vector<ArchiveEntry>& entries, int compression_level) {
  ByteBuffer zip;
  struct EntryMeta {
    std::string path;
    uint32_t crc = 0;
    uint32_t comp_size = 0;
    uint32_t raw_size = 0;
    uint16_t method = 0;
    uint32_t local_offset = 0;
    uint32_t external_attr = 0;
  };
  std::vector<EntryMeta> meta;
  meta.reserve(entries.size());

  auto write16 = [&zip](uint16_t val) {
    zip.push_back(static_cast<uint8_t>(val & 0xFF));
    zip.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
  };
  auto write32 = [&zip](uint32_t val) {
    zip.push_back(static_cast<uint8_t>(val & 0xFF));
    zip.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    zip.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    zip.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
  };

  for (const auto& e : entries) {
    EntryMeta m;
    m.path = e.path;
    m.raw_size = static_cast<uint32_t>(e.data.size());
    m.crc = crc32(ByteSpan(e.data.data(), e.data.size()));
    m.local_offset = static_cast<uint32_t>(zip.size());
    m.external_attr = e.is_dir ? (0040755u << 16) : ((e.mode ? (e.mode & 0777) : 0644u) | 0100000u) << 16;

    ByteBuffer comp_data;
    if (!e.is_dir && !e.data.empty() && compression_level > 0) {
      comp_data = net::deflate_raw(ByteSpan(e.data.data(), e.data.size()), compression_level);
    }
    const bool use_deflate = !comp_data.empty() && comp_data.size() < e.data.size();
    m.method = use_deflate ? 8 : 0;
    const uint8_t* payload_ptr = use_deflate ? comp_data.data() : e.data.data();
    m.comp_size = use_deflate ? static_cast<uint32_t>(comp_data.size()) : m.raw_size;

    // Local Header
    write32(0x04034b50);
    write16(20);               // version needed
    write16(0x0800);           // UTF-8 filename flag
    write16(m.method);
    write16(0);                // mod time
    write16(0);                // mod date
    write32(m.crc);
    write32(m.comp_size);
    write32(m.raw_size);
    write16(static_cast<uint16_t>(m.path.size()));
    write16(0);                // extra field length
    zip.insert(zip.end(), m.path.begin(), m.path.end());
    if (m.comp_size > 0 && payload_ptr) {
      zip.insert(zip.end(), payload_ptr, payload_ptr + m.comp_size);
    }
    meta.push_back(m);
  }

  // Central Directory
  size_t cd_start = zip.size();
  for (const auto& m : meta) {
    write32(0x02014b50);
    write16(0x031E);           // UNIX, 3.0
    write16(20);               // version needed 2.0
    write16(0x0800);           // UTF-8 flag
    write16(m.method);
    write16(0);                // mod time
    write16(0);                // mod date
    write32(m.crc);
    write32(m.comp_size);
    write32(m.raw_size);
    write16(static_cast<uint16_t>(m.path.size()));
    write16(0);                // extra len
    write16(0);                // comment len
    write16(0);                // disk start
    write16(0);                // internal attr
    write32(m.external_attr);
    write32(m.local_offset);
    zip.insert(zip.end(), m.path.begin(), m.path.end());
  }

  size_t cd_size = zip.size() - cd_start;

  // End of Central Directory Record
  write32(0x06054b50);
  write16(0);                  // disk number
  write16(0);                  // disk with central dir
  write16(static_cast<uint16_t>(meta.size()));
  write16(static_cast<uint16_t>(meta.size()));
  write32(static_cast<uint32_t>(cd_size));
  write32(static_cast<uint32_t>(cd_start));
  write16(0);                  // comment length

  return zip;
}

}  // namespace aios::userland
