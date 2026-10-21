// aios-boot: the hardware host's entry point into the AIOS.
//
// As PID 1 it is `init`: it prepares the host (mounts, drivers), finds or
// creates the persistent root, and keeps the AIOS command surface alive on
// the console. It also carries the installer that lays the system onto a
// disk: GPT written directly, an EFI system partition that boots on both UEFI
// (systemd-boot) and BIOS (syslinux), and an ext4 persistent root.
//
// Nothing here is part of the AIOS kernel; this is the host boundary's
// bootstrap, the piece that disappears when Linux does.
#include <sys/mount.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/reboot.h>
#include <sys/sysmacros.h>
#include <linux/fs.h>
#include <linux/reboot.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <termios.h>
#include <cerrno>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <random>
#include <chrono>
#include <thread>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

static const char* BANNER =
  "\n"
  "   ___   ____   ___  ____\n"
  "  / _ | /  _/  / _ \\/ __/      the machine on which intelligence exists\n"
  " / __ |_/ /   / // /\\ \\\n"
  "/_/ |_/___/   \\____/___/       phase 1 · Linux is the hardware host\n\n";

static void say(const std::string& s) { std::string m = "[aios-boot] " + s + "\n"; ssize_t r = ::write(1, m.data(), m.size()); (void)r; }
static std::string slurp(const std::string& p) { std::ifstream f(p); std::stringstream ss; ss << f.rdbuf(); std::string s = ss.str(); while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back(); return s; }
static bool exists(const std::string& p) { struct stat st{}; return ::stat(p.c_str(), &st) == 0; }
static int run(const std::vector<std::string>& argv, bool quiet = false) {
  pid_t pid = ::fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    if (quiet) { int n = ::open("/dev/null", O_WRONLY); ::dup2(n, 1); ::dup2(n, 2); }
    std::vector<char*> a; for (auto& s : argv) a.push_back(const_cast<char*>(s.c_str())); a.push_back(nullptr);
    ::execvp(a[0], a.data());
    _exit(127);
  }
  int st = 0;
  while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
static std::string cmdline_param(const std::string& key) {
  std::stringstream ss(slurp("/proc/cmdline"));
  std::string tok;
  while (ss >> tok) if (tok.rfind(key + "=", 0) == 0) return tok.substr(key.size() + 1);
  return {};
}

// ------------------------------------------------------------------ block devices
struct BlockDev {
  std::string name;        // sda, vda, nvme0n1, sr0
  std::string path;        // /dev/sda
  uint64_t bytes = 0;
  bool removable = false;
  bool partition = false;
  std::string model;
  std::string fs;          // detected: ext4:<label>, vfat, iso9660:<volid>, gpt, ""
  std::string label;
};

static std::string probe_fs(const std::string& dev, std::string& label) {
  int fd = ::open(dev.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return "";
  std::vector<uint8_t> b(0x8000 + 2048);
  ssize_t n = ::pread(fd, b.data(), b.size(), 0);
  ::close(fd);
  if (n < 0x1000) return "";
  if (n >= 0x8006 && std::memcmp(b.data() + 0x8001, "CD001", 5) == 0) { label = std::string((char*)b.data() + 0x8028, 32); while (!label.empty() && label.back() == ' ') label.pop_back(); return "iso9660"; }
  if (b[0x438] == 0x53 && b[0x439] == 0xEF) { label = std::string((char*)b.data() + 0x478, strnlen((char*)b.data() + 0x478, 16)); return "ext4"; }
  if (b[0x1FE] == 0x55 && b[0x1FF] == 0xAA && (std::memcmp(b.data() + 0x52, "FAT32", 5) == 0 || std::memcmp(b.data() + 0x36, "FAT1", 4) == 0)) {
    label = std::memcmp(b.data() + 0x52, "FAT32", 5) == 0 ? std::string((char*)b.data() + 0x47, 11) : std::string((char*)b.data() + 0x2B, 11);
    while (!label.empty() && label.back() == ' ') label.pop_back();
    return "vfat";
  }
  if (n >= 0x400 && std::memcmp(b.data() + 0x200, "EFI PART", 8) == 0) return "gpt";
  return "";
}

static std::vector<BlockDev> block_devices() {
  std::vector<BlockDev> out;
  if (!fs::exists("/sys/class/block")) return out;
  for (auto& e : fs::directory_iterator("/sys/class/block")) {
    BlockDev d;
    d.name = e.path().filename();
    if (d.name.rfind("loop", 0) == 0 || d.name.rfind("ram", 0) == 0 || d.name.rfind("zram", 0) == 0) continue;
    d.path = "/dev/" + d.name;
    d.bytes = std::strtoull(slurp(e.path() / "size").c_str(), nullptr, 10) * 512;
    d.removable = slurp(e.path() / "removable") == "1";
    d.partition = fs::exists(e.path() / "partition");
    if (fs::exists(e.path() / "device/model")) d.model = slurp(e.path() / "device/model");
    if (d.bytes == 0) continue;
    d.fs = probe_fs(d.path, d.label);
    out.push_back(d);
  }
  std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.name < b.name; });
  return out;
}

static std::string human(uint64_t b) {
  char buf[32];
  if (b >= (1ull << 30)) snprintf(buf, sizeof buf, "%.1f GiB", b / double(1ull << 30));
  else snprintf(buf, sizeof buf, "%.0f MiB", b / double(1ull << 20));
  return buf;
}

static int cmd_disks() {
  auto devs = block_devices();
  printf("%-10s %-10s %-6s %-18s %s\n", "device", "size", "type", "contents", "model");
  for (auto& d : devs) {
    std::string type = d.partition ? "part" : (d.name.rfind("sr", 0) == 0 ? "cdrom" : (d.removable ? "usb" : "disk"));
    std::string contents = d.fs.empty() ? "-" : d.fs + (d.label.empty() ? "" : ":" + d.label);
    if (d.fs == "iso9660" && d.label == "AIOS") contents += "  (boot media)";
    if (d.fs == "ext4" && d.label == "AIOS") contents += "  (AIOS root)";
    printf("%-10s %-10s %-6s %-18s %s\n", d.path.c_str(), human(d.bytes).c_str(), type.c_str(), contents.c_str(), d.model.c_str());
  }
  return 0;
}

// ------------------------------------------------------------------ GPT writer
static uint32_t crc32(const uint8_t* p, size_t n, uint32_t crc = 0) {
  crc = ~crc;
  for (size_t i = 0; i < n; ++i) { crc ^= p[i]; for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1))); }
  return ~crc;
}
struct Guid { uint8_t b[16]; };
static Guid guid_parse(const char* s) {
  // "C12A7328-F81F-11D2-BA4B-00A0C93EC93B" → mixed-endian on-disk layout
  unsigned v[10];
  sscanf(s, "%2x%2x%2x%2x-%2x%2x-%2x%2x-%2x%2x-", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]);
  Guid g{};
  g.b[0] = v[3]; g.b[1] = v[2]; g.b[2] = v[1]; g.b[3] = v[0];
  g.b[4] = v[5]; g.b[5] = v[4]; g.b[6] = v[7]; g.b[7] = v[6];
  g.b[8] = v[8]; g.b[9] = v[9];
  const char* tail = s + 24;
  for (int i = 0; i < 6; ++i) { unsigned x; sscanf(tail + 2 * i, "%2x", &x); g.b[10 + i] = (uint8_t)x; }
  return g;
}
static Guid guid_random() { Guid g; std::random_device rd; for (auto& x : g.b) x = (uint8_t)rd(); g.b[7] = (g.b[7] & 0x0F) | 0x40; g.b[8] = (g.b[8] & 0x3F) | 0x80; return g; }
static void put64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = uint8_t(v >> (8 * i)); }
static void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); }
static void put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }

struct PartSpec { const char* type; uint64_t first, last; uint64_t attrs; const char* name; };

// Writes protective MBR, primary + backup GPT for a 512-byte-sector disk.
static bool write_gpt(int fd, uint64_t disk_bytes, const std::vector<PartSpec>& parts, const uint8_t* mbr_code) {
  const uint64_t S = 512;
  uint64_t sectors = disk_bytes / S;
  std::vector<uint8_t> mbr(S, 0);
  if (mbr_code) std::memcpy(mbr.data(), mbr_code, 440);
  // protective partition
  uint8_t* pe = mbr.data() + 0x1BE;
  pe[1] = 0x00; pe[2] = 0x02; pe[3] = 0x00; pe[4] = 0xEE; pe[5] = 0xFF; pe[6] = 0xFF; pe[7] = 0xFF;
  put32(pe + 8, 1); put32(pe + 12, (uint32_t)std::min<uint64_t>(sectors - 1, 0xFFFFFFFFull));
  mbr[0x1FE] = 0x55; mbr[0x1FF] = 0xAA;
  // entries
  const uint32_t nent = 128, esz = 128;
  std::vector<uint8_t> entries(nent * esz, 0);
  for (size_t i = 0; i < parts.size() && i < nent; ++i) {
    uint8_t* e = entries.data() + i * esz;
    Guid t = guid_parse(parts[i].type); std::memcpy(e, t.b, 16);
    Guid u = guid_random(); std::memcpy(e + 16, u.b, 16);
    put64(e + 32, parts[i].first); put64(e + 40, parts[i].last); put64(e + 48, parts[i].attrs);
    for (size_t k = 0; parts[i].name[k] && k < 36; ++k) put16(e + 56 + 2 * k, (uint16_t)parts[i].name[k]);
  }
  uint32_t ecrc = crc32(entries.data(), entries.size());
  uint64_t ent_sectors = entries.size() / S;   // 32
  uint64_t primary_lba = 1, primary_ent = 2, backup_lba = sectors - 1, backup_ent = sectors - 1 - ent_sectors;
  uint64_t first_usable = primary_ent + ent_sectors, last_usable = backup_ent - 1;
  Guid disk_guid = guid_random();
  auto header = [&](uint64_t my, uint64_t alt, uint64_t ent_lba) {
    std::vector<uint8_t> h(S, 0);
    std::memcpy(h.data(), "EFI PART", 8);
    put32(h.data() + 8, 0x00010000); put32(h.data() + 12, 92);
    put64(h.data() + 24, my); put64(h.data() + 32, alt);
    put64(h.data() + 40, first_usable); put64(h.data() + 48, last_usable);
    std::memcpy(h.data() + 56, disk_guid.b, 16);
    put64(h.data() + 72, ent_lba); put32(h.data() + 80, nent); put32(h.data() + 84, esz); put32(h.data() + 88, ecrc);
    put32(h.data() + 16, crc32(h.data(), 92));
    return h;
  };
  auto ph = header(primary_lba, backup_lba, primary_ent), bh = header(backup_lba, primary_lba, backup_ent);
  auto wr = [&](uint64_t lba, const std::vector<uint8_t>& d) { return ::pwrite(fd, d.data(), d.size(), (off_t)(lba * S)) == (ssize_t)d.size(); };
  // Wipe any old filesystem signatures in the first MiB.
  std::vector<uint8_t> zero(1 << 20, 0);
  if (::pwrite(fd, zero.data(), zero.size(), 0) < 0) return false;
  return wr(0, mbr) && wr(primary_lba, ph) && wr(primary_ent, entries) && wr(backup_ent, entries) && wr(backup_lba, bh) && ::fsync(fd) == 0;
}

// ------------------------------------------------------------------ installer
static std::string part_path(const std::string& disk, int n) {
  bool digit = !disk.empty() && std::isdigit((unsigned char)disk.back());
  return disk + (digit ? "p" : "") + std::to_string(n);
}
static bool wait_for(const std::string& path, int ms) {
  for (int i = 0; i < ms / 50; ++i) { if (exists(path)) return true; std::this_thread::sleep_for(50ms); }
  return exists(path);
}
static bool copy_tree(const fs::path& from, const fs::path& to) {
  std::error_code ec;
  fs::create_directories(to, ec);
  fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
  if (ec) { say("copy " + from.string() + " → " + to.string() + ": " + ec.message()); return false; }
  return true;
}
static std::string find_media() {
  for (auto& d : block_devices()) if (d.fs == "iso9660" && d.label == "AIOS") return d.path;
  return {};
}

static int cmd_install(const std::string& disk) {
  auto devs = block_devices();
  const BlockDev* target = nullptr;
  for (auto& d : devs) if (d.path == disk) target = &d;
  if (!target) { say("no such block device: " + disk + " (see `host disks`)"); return 2; }
  if (target->partition) { say(disk + " is a partition; install to the whole disk"); return 2; }
  if (target->fs == "iso9660") { say(disk + " holds the boot media; pick another disk"); return 2; }
  if (target->bytes < (1ull << 30)) { say(disk + " is smaller than 1 GiB"); return 2; }

  std::string media = find_media();
  if (media.empty()) { say("cannot find the AIOS boot media (an ISO9660 volume labelled AIOS)"); return 3; }
  fs::create_directories("/media");
  if (::mount(media.c_str(), "/media", "iso9660", MS_RDONLY, "") != 0 && errno != EBUSY) { say(std::string("mount media: ") + strerror(errno)); return 3; }
  for (const char* f : {"/media/boot/vmlinuz", "/media/boot/initramfs.img", "/media/boot/systemd-bootx64.efi", "/media/boot/gptmbr.bin", "/media/boot/ldlinux.c32", "/media/boot/syslinux-disk.cfg", "/media/boot/loader-disk"})
    if (!exists(f)) { say(std::string("boot media is missing ") + f); return 3; }

  say("installing to " + disk + " (" + human(target->bytes) + " " + target->model + ") — this erases it");
  int fd = ::open(disk.c_str(), O_RDWR | O_CLOEXEC);
  if (fd < 0) { say(std::string("open: ") + strerror(errno)); return 4; }
  uint64_t bytes = target->bytes;
  std::vector<uint8_t> mbr(440);
  { std::ifstream m("/media/boot/gptmbr.bin", std::ios::binary); m.read((char*)mbr.data(), 440); }
  const uint64_t esp_first = 2048, esp_last = 2048 + (512ull << 20) / 512 - 1;   // 512 MiB ESP
  const uint64_t root_first = esp_last + 1, root_last = bytes / 512 - 34;
  std::vector<PartSpec> parts = {
    {"C12A7328-F81F-11D2-BA4B-00A0C93EC93B", esp_first, esp_last, 0x4ull /* legacy BIOS bootable */, "AIOS BOOT"},
    {"0FC63DAF-8483-4772-8E79-3D69D16ADBFC", root_first, root_last, 0, "AIOS"},
  };
  if (!write_gpt(fd, bytes, parts, mbr.data())) { say("writing GPT failed"); ::close(fd); return 4; }
  ::ioctl(fd, BLKRRPART);
  ::close(fd);
  say("GPT written: ESP 512 MiB + AIOS root " + human((root_last - root_first + 1) * 512));
  std::string p1 = part_path(disk, 1), p2 = part_path(disk, 2);
  if (!wait_for(p1, 5000) || !wait_for(p2, 5000)) { say("partitions did not appear (" + p1 + ", " + p2 + ")"); return 4; }

  say("formatting " + p1 + " (FAT32) and " + p2 + " (ext4, label AIOS)");
  if (run({"/aios/bin/mkfs.fat", "-F", "32", "-n", "AIOSBOOT", p1}, true) != 0) { say("mkfs.fat failed"); return 5; }
  if (run({"/aios/bin/mke2fs", "-q", "-F", "-t", "ext4", "-L", "AIOS", p2}, true) != 0) { say("mke2fs failed"); return 5; }

  fs::create_directories("/mnt/esp"); fs::create_directories("/mnt/root");
  if (::mount(p1.c_str(), "/mnt/esp", "vfat", 0, "") != 0) { say(std::string("mount ESP: ") + strerror(errno)); return 5; }
  bool ok = true;
  ok &= copy_tree("/media/boot/loader-disk", "/mnt/esp/loader");
  fs::create_directories("/mnt/esp/EFI/BOOT");
  std::error_code ec;
  fs::copy_file("/media/boot/systemd-bootx64.efi", "/mnt/esp/EFI/BOOT/BOOTX64.EFI", fs::copy_options::overwrite_existing, ec); ok &= !ec;
  fs::copy_file("/media/boot/vmlinuz", "/mnt/esp/vmlinuz", fs::copy_options::overwrite_existing, ec); ok &= !ec;
  fs::copy_file("/media/boot/initramfs.img", "/mnt/esp/initramfs.img", fs::copy_options::overwrite_existing, ec); ok &= !ec;
  fs::create_directories("/mnt/esp/syslinux");
  fs::copy_file("/media/boot/ldlinux.c32", "/mnt/esp/syslinux/ldlinux.c32", fs::copy_options::overwrite_existing, ec); ok &= !ec;
  fs::copy_file("/media/boot/syslinux-disk.cfg", "/mnt/esp/syslinux/syslinux.cfg", fs::copy_options::overwrite_existing, ec); ok &= !ec;
  ::sync();
  if (!ok) { ::umount("/mnt/esp"); say("copying boot files failed"); return 5; }
  // BIOS path: extlinux writes ldlinux.sys into the mounted ESP and patches the
  // partition's boot sector; gptmbr.bin at LBA 0 chains to the bootable partition.
  if (run({"/aios/bin/extlinux", "--install", "/mnt/esp/syslinux"}) != 0) say("warning: extlinux install failed; disk boots on UEFI only");
  else say("BIOS boot installed (syslinux/extlinux) alongside UEFI (systemd-boot)");
  ::sync();
  ::umount("/mnt/esp");

  if (::mount(p2.c_str(), "/mnt/root", "ext4", 0, "") != 0) { say(std::string("mount root: ") + strerror(errno)); return 5; }
  fs::create_directories("/mnt/root/root");
  // Carry the live session's state across: the installed system continues it.
  if (exists("/aios/root/kernel.identity")) { say("carrying the live session's persistent root onto the disk"); copy_tree("/aios/root", "/mnt/root/root"); }
  ::sync();
  ::umount("/mnt/root");
  ::umount("/media");
  say("installed. remove the media and reboot: `host reboot`");
  return 0;
}

// ------------------------------------------------------------------ init
static volatile sig_atomic_t g_child_exit = 0;
static void on_chld(int) { g_child_exit = 1; }

static void mount_or_die(const char* src, const char* dst, const char* type, unsigned long flags, const char* data = "") {
  ::mkdir(dst, 0755);
  if (::mount(src, dst, type, flags, data) != 0 && errno != EBUSY) say(std::string("mount ") + dst + ": " + strerror(errno));
}

static std::string find_persistent_root() {
  for (auto& d : block_devices()) if (d.fs == "ext4" && d.label == "AIOS") return d.path;
  return {};
}

static pid_t spawn_console(const std::string& tty, const std::vector<std::string>& argv) {
  pid_t pid = ::fork();
  if (pid != 0) return pid;
  ::setsid();
  int fd = ::open(tty.c_str(), O_RDWR | O_NOCTTY);
  if (fd < 0) { fd = ::open("/dev/console", O_RDWR | O_NOCTTY); }
  ::ioctl(fd, TIOCSCTTY, 1);
  ::dup2(fd, 0); ::dup2(fd, 1); ::dup2(fd, 2);
  if (fd > 2) ::close(fd);
  termios t{};
  if (::tcgetattr(0, &t) == 0) { t.c_lflag |= ICANON | ECHO | ECHOE | ISIG; t.c_iflag |= ICRNL; t.c_oflag |= OPOST | ONLCR; ::tcsetattr(0, TCSANOW, &t); }
  std::vector<char*> a; for (auto& s : argv) a.push_back(const_cast<char*>(s.c_str())); a.push_back(nullptr);
  ::execv(a[0], a.data());
  _exit(127);
}

// Host services: executables in /etc/aios/rc.d, started in name order once
// the host is up, each in its own session with the console as its output.
// This is how a machine image adds the work it exists for (a VM that serves a
// platform, say) without a second init: aios-boot stays PID 1, reaps them
// with everything else, and the command surface keeps the console.
static void start_host_services() {
  const char* dir = "/etc/aios/rc.d";
  if (!fs::exists(dir)) return;
  std::vector<fs::path> svcs;
  for (auto& e : fs::directory_iterator(dir)) {
    struct stat st{};
    if (::stat(e.path().c_str(), &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0111)) svcs.push_back(e.path());
  }
  std::sort(svcs.begin(), svcs.end());
  for (auto& p : svcs) {
    pid_t pid = ::fork();
    if (pid == 0) {
      ::setsid();
      int in = ::open("/dev/null", O_RDONLY), out = ::open("/dev/console", O_WRONLY | O_NOCTTY);
      if (in >= 0) ::dup2(in, 0);
      if (out >= 0) { ::dup2(out, 1); ::dup2(out, 2); }
      ::execl(p.c_str(), p.c_str(), (char*)nullptr);
      _exit(127);
    }
    say("service " + p.filename().string() + (pid > 0 ? " started, pid " + std::to_string(pid) : std::string(" failed to start")));
  }
}

static int init_main() {
  say("host boot");
  mount_or_die("proc", "/proc", "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV);
  mount_or_die("sysfs", "/sys", "sysfs", MS_NOSUID | MS_NOEXEC | MS_NODEV);
  mount_or_die("devtmpfs", "/dev", "devtmpfs", MS_NOSUID, "mode=0755");
  mount_or_die("devpts", "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC, "gid=5,mode=620");
  mount_or_die("tmpfs", "/run", "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755");
  mount_or_die("tmpfs", "/tmp", "tmpfs", MS_NOSUID | MS_NODEV, "");
  if (::sethostname("aios", 4) != 0) say("sethostname failed");
  { std::ofstream("/proc/sys/kernel/printk") << "3 4 1 3\n"; }
  ::setenv("PATH", "/aios/bin:/bin", 1);
  // Storage and input drivers the VM may present as modules.
  run({"/bin/busybox", "modprobe", "-a", "-q", "ahci", "isofs", "nls_iso8859-1", "vmw_pvscsi", "mptspi", "mptsas", "mpt3sas", "hv_vmbus", "hv_storvsc", "hyperv_keyboard", "xhci_pci", "usb_storage", "uas", "usbhid", "hid_generic"}, true);
  // Give the host a moment to surface block devices.
  for (int i = 0; i < 40; ++i) { if (!block_devices().empty()) break; std::this_thread::sleep_for(100ms); }
  std::this_thread::sleep_for(500ms);

  // Networking: every interface up, DHCP where offered (for node-to-node links).
  if (fs::exists("/sys/class/net")) {
    for (auto& e : fs::directory_iterator("/sys/class/net")) {
      std::string ifc = e.path().filename();
      if (ifc == "lo") { run({"/bin/busybox", "ip", "link", "set", "lo", "up"}, true); continue; }
      run({"/bin/busybox", "ip", "link", "set", ifc, "up"}, true);
      pid_t pid = ::fork();
      if (pid == 0) { int n = ::open("/dev/null", O_WRONLY); ::dup2(n, 1); ::dup2(n, 2); ::execl("/bin/busybox", "busybox", "udhcpc", "-i", ifc.c_str(), "-b", "-q", "-t", "5", "-s", "/etc/udhcpc.sh", (char*)nullptr); _exit(127); }
    }
  }
  ::mkdir("/aios", 0755);
  std::string root_dev = find_persistent_root();
  bool persistent = false;
  if (!root_dev.empty()) {
    // Check and repair the persistent root before trusting it (journal replay,
    // orphan cleanup); exit 0/1 = clean or repaired, anything else is reported.
    int fsck = run({"/aios/bin/e2fsck", "-p", root_dev}, true);
    if (fsck >= 2) say("e2fsck reported status " + std::to_string(fsck) + " on " + root_dev + " — inspect with `host shell`");
    else say(std::string("persistent root checked (") + (fsck == 0 ? "clean" : "repaired") + ")");
    if (::mount(root_dev.c_str(), "/aios/mnt", "ext4", 0, "") == 0 || (::mkdir("/aios/mnt", 0755) == 0 && ::mount(root_dev.c_str(), "/aios/mnt", "ext4", 0, "") == 0)) {
      ::mkdir("/aios/mnt/root", 0755);
      ::umount("/aios/root");
      ::mkdir("/aios/root", 0755);
      if (::mount("/aios/mnt/root", "/aios/root", nullptr, MS_BIND, nullptr) == 0) persistent = true;
      else say(std::string("bind root: ") + strerror(errno));
    } else say("persistent root " + root_dev + " found but mount failed: " + strerror(errno));
  }
  if (!persistent) { ::mkdir("/aios/root", 0755); mount_or_die("tmpfs", "/aios/root", "tmpfs", 0, ""); }

  signal(SIGCHLD, on_chld);
  std::string console = cmdline_param("console");
  std::string tty = "/dev/console";
  (void)console;
  ssize_t r = ::write(1, BANNER, strlen(BANNER)); (void)r;
  say(persistent ? "persistent root: " + root_dev + " mounted at /aios/root" : "no persistent root — state lives in memory until `host install <disk>`");
  say("boot media: " + (find_media().empty() ? std::string("none detected") : find_media()));
  say("commands: help · host disks · host install /dev/sdX · host net · host shell · host reboot · host poweroff");

  ::setenv("AIOS_HOST_TOOL", "/aios/bin/aios-boot", 1);
  start_host_services();
  for (;;) {
    pid_t shell = spawn_console(tty, {"/aios/bin/aios", "--root", "/aios/root", "shell"});
    int st = 0;
    for (;;) {
      pid_t w = ::waitpid(-1, &st, 0);
      if (w == shell) break;
      if (w < 0 && errno != EINTR) break;
    }
    ::sync();
    say("command surface exited; restarting");
    std::this_thread::sleep_for(1s);
  }
}

static int cmd_power(bool reboot_) {
  ::sync();
  ::umount("/aios/root"); ::umount("/aios/mnt"); ::umount("/media");
  ::sync();
  ::reboot(reboot_ ? RB_AUTOBOOT : RB_POWER_OFF);
  return 0;
}

int main(int argc, char** argv) {
  if (::getpid() == 1) return init_main();
  std::string cmd = argc > 1 ? argv[1] : "";
  if (cmd == "disks") return cmd_disks();
  if (cmd == "install") { if (argc < 3) { say("usage: host install /dev/<disk>   (see `host disks`)"); return 2; } return cmd_install(argv[2]); }
  if (cmd == "reboot") return cmd_power(true);
  if (cmd == "poweroff") return cmd_power(false);
  if (cmd == "net") { run({"/bin/busybox", "ip", "-brief", "addr"}); return 0; }
  if (cmd == "shell") { say("host shell (busybox); `exit` returns to the AIOS"); return run({"/bin/busybox", "sh"}); }
  if (cmd == "media") { std::string m = find_media(); printf("%s\n", m.empty() ? "none" : m.c_str()); return m.empty() ? 1 : 0; }
  say("host commands: disks | install /dev/<disk> | reboot | poweroff | media");
  return 2;
}
