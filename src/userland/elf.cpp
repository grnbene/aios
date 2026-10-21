#include "aios/userland/elf.hpp"

namespace aios::userland {
namespace {

std::string dec(uint64_t v) {
  if (!v) return "0";
  std::string s;
  while (v) { s.insert(s.begin(), (char)('0' + v % 10)); v /= 10; }
  return s;
}

template <typename T>
T read_at(ByteSpan b, size_t off) {
  T v = 0;
  for (size_t i = 0; i < sizeof(T); ++i) v |= (T)b[off + i] << (8 * i);
  return v;
}

constexpr uint32_t PT_LOAD = 1, PT_INTERP = 3;
constexpr uint32_t PF_X = 1, PF_W = 2, PF_R = 4;

}  // namespace

Result<ElfImage> elf_read(ByteSpan bytes) {
  if (bytes.size() < 64)
    return Result<ElfImage>(Status::Invalid, "too small to be an ELF file: " + dec(bytes.size()) + " bytes");
  if (!(bytes[0] == 0x7F && bytes[1] == 'E' && bytes[2] == 'L' && bytes[3] == 'F'))
    return Result<ElfImage>(Status::Invalid, "not an ELF file: it does not begin with \\x7fELF");
  if (bytes[4] != 2) return Result<ElfImage>(Status::Unsupported, "this is a 32-bit ELF; this machine runs 64-bit code");
  if (bytes[5] != 1) return Result<ElfImage>(Status::Unsupported, "big-endian ELF: this machine is little-endian");
  if (bytes[6] != 1) return Result<ElfImage>(Status::Invalid, "unknown ELF version in the identification");

  ElfImage img;
  img.bytes = bytes.size();
  uint16_t type = read_at<uint16_t>(bytes, 16);
  uint16_t machine = read_at<uint16_t>(bytes, 18);
  if (machine != 62)
    return Result<ElfImage>(Status::Unsupported, "this ELF is for machine type " + dec(machine) + ", not x86-64");
  img.machine = "x86-64";
  if (type == 2) img.kind = "executable";
  else if (type == 3) img.kind = "shared object";
  else return Result<ElfImage>(Status::Unsupported, "ELF type " + dec(type) + " is not something to run");

  img.entry = read_at<uint64_t>(bytes, 24);
  uint64_t phoff = read_at<uint64_t>(bytes, 32);
  uint16_t phentsize = read_at<uint16_t>(bytes, 54);
  uint16_t phnum = read_at<uint16_t>(bytes, 56);
  if (phentsize < 56) return Result<ElfImage>(Status::Invalid, "program headers are too small to be program headers");
  if (!phnum) return Result<ElfImage>(Status::Invalid, "this ELF has no program headers: there is nothing to load");
  if (phoff + (uint64_t)phentsize * phnum > bytes.size())
    return Result<ElfImage>(Status::Invalid, "the program headers run past the end of the file");

  bool first = true;
  for (uint16_t i = 0; i < phnum; ++i) {
    size_t at = (size_t)(phoff + (uint64_t)phentsize * i);
    uint32_t ptype = read_at<uint32_t>(bytes, at);
    uint32_t flags = read_at<uint32_t>(bytes, at + 4);
    uint64_t offset = read_at<uint64_t>(bytes, at + 8);
    uint64_t vaddr = read_at<uint64_t>(bytes, at + 16);
    uint64_t filesz = read_at<uint64_t>(bytes, at + 32);
    uint64_t memsz = read_at<uint64_t>(bytes, at + 40);
    if (ptype == PT_INTERP) {
      img.static_linked = false;
      if (offset + filesz <= bytes.size())
        for (uint64_t j = 0; j + 1 < filesz; ++j) img.interpreter += (char)bytes[(size_t)(offset + j)];
      continue;
    }
    if (ptype != PT_LOAD) continue;
    if (offset + filesz > bytes.size())
      return Result<ElfImage>(Status::Invalid, "a loadable segment claims bytes past the end of the file");
    if (memsz < filesz)
      return Result<ElfImage>(Status::Invalid, "a loadable segment is larger on disk than it is in memory");
    ElfSegment s;
    s.vaddr = vaddr;
    s.file_offset = offset;
    s.file_size = filesz;
    s.mem_size = memsz;
    s.readable = (flags & PF_R) != 0;
    s.writable = (flags & PF_W) != 0;
    s.executable = (flags & PF_X) != 0;
    if (first || vaddr < img.lowest) img.lowest = vaddr;
    if (first || vaddr + memsz > img.highest) img.highest = vaddr + memsz;
    first = false;
    img.segments.push_back(s);
  }
  if (img.segments.empty())
    return Result<ElfImage>(Status::Invalid, "this ELF has no loadable segments");
  if (img.entry < img.lowest || img.entry >= img.highest)
    return Result<ElfImage>(Status::Invalid, "the entry point is outside every segment this file loads");
  return img;
}

Result<ElfFile> elf_open(Kernel& kernel, Surface* actor, const std::string& path) {
  if (actor) {
    auto may = actor->may(ResourceRef{ResourceKind::StorageObject, {}}, R_Read);
    if (!may.ok())
      return Result<ElfFile>(Status::Denied, "entity " + actor->self().short_str() +
                                                 " may not read programs from this machine: " + may.error().str());
  }
  auto bytes = kernel.host().read_file(path);
  if (!bytes.ok()) return Result<ElfFile>(bytes.error());
  ElfFile f;
  f.path = path;
  f.bytes = *bytes;
  auto img = elf_read(ByteSpan(f.bytes.data(), f.bytes.size()));
  if (!img.ok()) return Result<ElfFile>(Status(img.error().status), path + ": " + img.error().message);
  f.image = *img;
  return f;
}

}  // namespace aios::userland
