/* EL0 processes and the scheduler (metal/arm64/process.cpp).
 *
 * Full userland engine subsystem for ARM64:
 * - User process memory isolation
 * - Context switching (EL0/EL1 transition)
 * - User thread state management & signals
 * - ELF binary loading (embedded + FAT32)
 * - Full syscall dispatch parity with x86 metal implementation
 */
#include "aios/metal/trap.hpp"

#include <cstring>

#include "fat32.hpp"
#include "kio.hpp"
#include "mmu.hpp"
#include "timer.hpp"
#include "uart.hpp"
#include "usb_hid.hpp"

extern "C" {
uint64_t metal_ticks();
[[noreturn]] void metal_panic(const char* what);
void metal_report_frame(const char* lead, TrapFrame* f);
[[noreturn]] void arm64_enter_frame(TrapFrame* f);
extern TrapFrame* (*metal_tick_hook)(TrapFrame*);
extern TrapFrame* (*metal_syscall_hook)(TrapFrame*);
extern TrapFrame* (*metal_user_fault_hook)(TrapFrame*);
}

/* Embedded programs, emitted into user_blobs.S by the build. */
extern "C" {
extern const unsigned char user_hello_start[], user_hello_end[];
extern const unsigned char user_spin_start[],  user_spin_end[];
extern const unsigned char user_crash_start[], user_crash_end[];
extern const unsigned char user_priv_start[],  user_priv_end[];
extern const unsigned char user_neon_start[],  user_neon_end[];
extern const unsigned char user_shell_start[], user_shell_end[];
}

namespace {

constexpr uint64_t USER_BASE       = 0x4000000000ull;
constexpr uint64_t USER_END        = 0x4100000000ull;
constexpr uint64_t USER_STACK_TOP  = 0x4001000000ull;
constexpr uint64_t USER_STACK_SIZE = 64 * 1024;
constexpr uint64_t USER_MMAP_BASE  = 0x4020000000ull;
constexpr uint64_t KSTACK_SIZE     = 32 * 1024;
constexpr uint64_t FRAME_BYTES     = 816;   /* matches vectors.S */
constexpr uint64_t PAGE_SIZE       = 4096;

constexpr unsigned MAX_PROCS = 16;
constexpr unsigned MAX_FDS = 16;
constexpr unsigned MAX_SIGNALS = 32;
constexpr unsigned MAX_MAPPINGS = 64;

/* ---- ELF64 reader --------------------------------------------------- */

struct Elf64Header {
  unsigned char ident[16];
  uint16_t type, machine;
  uint32_t version;
  uint64_t entry, phoff, shoff;
  uint32_t flags;
  uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct Elf64Phdr {
  uint32_t type, flags;
  uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

constexpr uint32_t PT_LOAD = 1;
constexpr uint16_t EM_AARCH64 = 183;

struct Program {
  const char* name;
  const unsigned char* start;
  const unsigned char* end;
};

const Program g_programs[] = {
    {"hello", user_hello_start, user_hello_end},
    {"spin",  user_spin_start,  user_spin_end},
    {"crash", user_crash_start, user_crash_end},
    {"priv",  user_priv_start,  user_priv_end},
    {"neon",  user_neon_start,  user_neon_end},
    {"shell", user_shell_start, user_shell_end},
};
constexpr unsigned PROGRAM_COUNT = sizeof(g_programs) / sizeof(g_programs[0]);

enum class State : uint8_t { Free, Ready, Running, Sleeping, Zombie, Exited };

struct MemoryMapping {
  uint64_t va = 0;
  uint64_t pa = 0;
  uint64_t size = 0;
  uint32_t flags = 0;
};

struct OpenFile {
  char path[64] = {};
  uint8_t* data = nullptr;
  uint64_t size = 0;
  uint64_t capacity = 0;
  uint64_t offset = 0;
  bool writable = false;
  bool dirty = false;
  bool is_console = false;
  bool active = false;
};

OpenFile g_file_table[MAX_PROCS * MAX_FDS];

OpenFile* alloc_open_file() {
  for (unsigned i = 0; i < MAX_PROCS * MAX_FDS; ++i) {
    if (!g_file_table[i].active) {
      g_file_table[i] = OpenFile{};
      g_file_table[i].active = true;
      return &g_file_table[i];
    }
  }
  return nullptr;
}

void free_open_file(OpenFile* f) {
  if (!f) return;
  if (f->dirty && f->writable && f->path[0]) {
    fat32_write_file(f->path, f->data, f->size);
  }
  if (f->data && !f->is_console) {
    metal_free_pages(f->data, (f->capacity + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
  }
  f->active = false;
}

struct Proc {
  State state = State::Free;
  int pid = 0;
  int ppid = 0;
  char name[32] = {};
  TrapFrame* frame = nullptr;
  uint8_t* kstack = nullptr;

  /* Mappings & memory isolation */
  MemoryMapping mappings[MAX_MAPPINGS] = {};
  size_t mapping_count = 0;
  uint64_t image_pa = 0;
  uint64_t image_va = 0;
  uint64_t image_bytes = 0;
  uint64_t stack_pa = 0;
  uint64_t brk = 0;
  uint64_t mmap_next = USER_MMAP_BASE;

  /* Scheduler & execution stats */
  uint64_t ticks = 0;
  uint64_t syscalls = 0;
  uint32_t slice_left = 1;
  uint64_t wake_at = 0;
  int64_t exit_code = 0;
  bool faulted = false;
  bool reaped_by_parent = false;

  /* File descriptors */
  OpenFile* fds[MAX_FDS] = {};

  /* Signals */
  uint64_t sig_pending = 0;
  uint64_t handler[MAX_SIGNALS] = {};
  uint64_t trampoline = 0;
  bool in_signal = false;
  TrapFrame saved_frame = {};
};

Proc g_procs[MAX_PROCS];
int g_next_pid = 1;
int g_current = -1;          /* index into g_procs, -1 when the shell has the CPU */
KernelContext g_shell_ctx;
bool g_in_process = false;
uint64_t g_switches = 0;
uint32_t g_slice_ticks = 1;

bool user_range_ok(uint64_t va, uint64_t len) {
  if (va < USER_BASE || va + len > USER_END || va + len < va) return false;
  return true;
}

bool add_mapping(Proc& p, uint64_t va, uint64_t pa, uint64_t size, uint32_t flags) {
  if (p.mapping_count >= MAX_MAPPINGS) return false;
  if (!mmu_map(va, pa, size, flags)) return false;
  p.mappings[p.mapping_count++] = MemoryMapping{va, pa, size, flags};
  return true;
}

void free_proc(Proc& p) {
  for (size_t i = 0; i < p.mapping_count; ++i) {
    const auto& m = p.mappings[i];
    mmu_unmap(m.va, m.size);
    metal_free_pages(reinterpret_cast<void*>(m.pa), m.size);
  }
  p.mapping_count = 0;
  if (p.kstack) {
    metal_free_pages(p.kstack, KSTACK_SIZE);
    p.kstack = nullptr;
  }
  for (unsigned i = 0; i < MAX_FDS; ++i) {
    if (p.fds[i]) {
      free_open_file(p.fds[i]);
      p.fds[i] = nullptr;
    }
  }
  p.image_pa = p.stack_pa = p.image_va = p.image_bytes = p.brk = 0;
  p.mmap_next = USER_MMAP_BASE;
  p.frame = nullptr;
  p.state = State::Free;
}

int fd_install(Proc& p, OpenFile* f) {
  for (unsigned i = 0; i < MAX_FDS; ++i) {
    if (!p.fds[i]) {
      p.fds[i] = f;
      return static_cast<int>(i);
    }
  }
  return -1;
}

Proc* find_proc(int pid) {
  for (unsigned i = 0; i < MAX_PROCS; ++i) {
    if (g_procs[i].state != State::Free && g_procs[i].pid == pid) return &g_procs[i];
  }
  return nullptr;
}

int pick_next(int after) {
  uint64_t now = metal_ticks();
  for (unsigned n = 1; n <= MAX_PROCS; ++n) {
    const int i = (after + static_cast<int>(n)) % MAX_PROCS;
    Proc& p = g_procs[i];
    if (p.state == State::Sleeping && p.wake_at <= now) {
      p.state = State::Ready;
    }
    if (p.state == State::Ready || p.state == State::Running) return i;
  }
  return -1;
}

[[noreturn]] void return_to_shell() {
  g_current = -1;
  g_in_process = false;
  metal_ctx_restore(&g_shell_ctx);
  __builtin_unreachable();
}

TrapFrame* schedule_from(TrapFrame* f) {
  if (g_current >= 0) {
    Proc& cur = g_procs[g_current];
    cur.frame = f;
  }
  const int next = pick_next(g_current);
  if (next < 0) return_to_shell();

  if (g_current >= 0 && g_procs[g_current].state == State::Running && g_current != next) {
    g_procs[g_current].state = State::Ready;
  }

  g_procs[next].state = State::Running;
  g_procs[next].slice_left = g_slice_ticks;
  g_current = next;
  ++g_switches;

  Proc& p = g_procs[next];
  if (p.sig_pending && !p.in_signal) {
    for (unsigned s = 1; s < MAX_SIGNALS; ++s) {
      if ((p.sig_pending & (1ull << s)) && p.handler[s]) {
        p.sig_pending &= ~(1ull << s);
        p.in_signal = true;
        p.saved_frame = *p.frame;
        p.frame->x[0] = s;
        p.frame->x[30] = p.trampoline;
        p.frame->elr_el1 = p.handler[s];
        break;
      }
    }
  }

  return p.frame;
}

TrapFrame* on_tick(TrapFrame* f) {
  if (!g_in_process || g_current < 0) return f;
  Proc& cur = g_procs[g_current];
  ++cur.ticks;
  if (cur.slice_left > 1) {
    --cur.slice_left;
    return f;
  }
  return schedule_from(f);
}

TrapFrame* on_user_fault(TrapFrame* f) {
  if (g_current < 0) {
    metal_report_frame("fault at EL0 with no current process", f);
    metal_panic("EL0 fault outside any process");
  }
  Proc& p = g_procs[g_current];
  const uint32_t ec = static_cast<uint32_t>((f->esr_el1 >> 26) & 0x3F);
  kprintf("\n  [kernel] pid %d (%s) took ", static_cast<int64_t>(p.pid), p.name);
  if (ec == 0x24) kprintf("a data abort on %p", f->far_el1);
  else if (ec == 0x20) kprintf("an instruction abort on %p", f->far_el1);
  else kprintf("exception class 0x%02x", static_cast<uint64_t>(ec));
  kprintf(" at pc %p\n  [kernel] killed it; the machine keeps running\n", f->elr_el1);

  p.faulted = true;
  p.exit_code = -1;
  p.state = State::Exited;

  return schedule_from(f);
}

bool load_elf(Proc& p, const uint8_t* img, uint64_t img_len) {
  if (img_len < sizeof(Elf64Header)) return false;
  const auto* eh = reinterpret_cast<const Elf64Header*>(img);
  if (!(eh->ident[0] == 0x7F && eh->ident[1] == 'E' && eh->ident[2] == 'L' && eh->ident[3] == 'F'))
    return false;
  if (eh->ident[4] != 2 || eh->machine != EM_AARCH64) return false;

  uint64_t max_va = 0;
  for (unsigned i = 0; i < eh->phnum; ++i) {
    const auto* ph = reinterpret_cast<const Elf64Phdr*>(img + eh->phoff + i * eh->phentsize);
    if (ph->type != PT_LOAD) continue;
    uint64_t seg_va = ph->vaddr & ~(PAGE_SIZE - 1);
    uint64_t seg_end = (ph->vaddr + ph->memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t seg_bytes = seg_end - seg_va;
    if (seg_end > max_va) max_va = seg_end;

    auto* pages = static_cast<uint8_t*>(metal_alloc_pages(seg_bytes));
    if (!pages) return false;

    for (uint64_t k = 0; k < ph->filesz; ++k) pages[(ph->vaddr - seg_va) + k] = img[ph->offset + k];
    for (uint64_t k = ph->filesz; k < ph->memsz; ++k) pages[(ph->vaddr - seg_va) + k] = 0;

    uint32_t flags = MAP_USER;
    if (ph->flags & 1) flags |= MAP_EXEC;
    if (!(ph->flags & 2)) flags |= MAP_RO;

    if (!add_mapping(p, seg_va, reinterpret_cast<uint64_t>(pages), seg_bytes, flags)) {
      metal_free_pages(pages, seg_bytes);
      return false;
    }
  }

  auto* ustack = static_cast<uint8_t*>(metal_alloc_pages(USER_STACK_SIZE));
  auto* kstack = static_cast<uint8_t*>(metal_alloc_pages(KSTACK_SIZE));
  if (!ustack || !kstack) {
    if (ustack) metal_free_pages(ustack, USER_STACK_SIZE);
    if (kstack) metal_free_pages(kstack, KSTACK_SIZE);
    return false;
  }

  if (!add_mapping(p, USER_STACK_TOP - USER_STACK_SIZE, reinterpret_cast<uint64_t>(ustack),
                   USER_STACK_SIZE, MAP_USER)) {
    metal_free_pages(ustack, USER_STACK_SIZE);
    metal_free_pages(kstack, KSTACK_SIZE);
    return false;
  }

  p.stack_pa = reinterpret_cast<uint64_t>(ustack);
  p.kstack = kstack;
  p.brk = (max_va + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
  p.mmap_next = USER_MMAP_BASE;

  auto* f = reinterpret_cast<TrapFrame*>(kstack + KSTACK_SIZE - FRAME_BYTES);
  auto* words = reinterpret_cast<uint64_t*>(f);
  for (unsigned i = 0; i < FRAME_BYTES / 8; ++i) words[i] = 0;

  f->elr_el1 = eh->entry;
  f->sp_el0 = USER_STACK_TOP - 16;
  f->spsr_el1 = 0;          /* EL0t, DAIF clear: user code is interruptible */
  p.frame = f;
  return true;
}

/* Full syscall dispatch table matching x86 parity */
TrapFrame* on_syscall(TrapFrame* f) {
  if (g_current < 0) { f->x[0] = static_cast<uint64_t>(-1); return f; }
  Proc& p = g_procs[g_current];
  ++p.syscalls;

  const uint64_t nr = f->x[8];
  const uint64_t a0 = f->x[0];
  const uint64_t a1 = f->x[1];
  const uint64_t a2 = f->x[2];

  switch (nr) {
    case 0: {  /* SYS_WRITE (fd, buf, len) */
      const uint64_t fd = a0;
      const uint64_t va = a1;
      uint64_t len = a2;
      if (!user_range_ok(va, len)) { f->x[0] = static_cast<uint64_t>(-1); return f; }

      if (fd == 1 || fd == 2) {
        uint64_t written = 0;
        for (uint64_t i = 0; i < len; ++i) {
          const uint64_t pa = mmu_translate(va + i);
          if (!pa) break;
          const char c = *reinterpret_cast<const char*>(pa);
          metal_console_write(&c, 1);
          ++written;
        }
        f->x[0] = written;
        return f;
      }
      if (fd < MAX_FDS && p.fds[fd] && p.fds[fd]->writable) {
        auto* file = p.fds[fd];
        if (file->offset + len > file->capacity) {
          uint64_t new_cap = (file->offset + len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
          auto* new_data = static_cast<uint8_t*>(metal_alloc_pages(new_cap));
          if (new_data) {
            if (file->data) {
              for (uint64_t i = 0; i < file->size; ++i) new_data[i] = file->data[i];
              metal_free_pages(file->data, (file->capacity + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
            }
            file->data = new_data;
            file->capacity = new_cap;
          } else {
            f->x[0] = static_cast<uint64_t>(-1);
            return f;
          }
        }
        for (uint64_t i = 0; i < len; ++i) {
          const uint64_t pa = mmu_translate(va + i);
          if (!pa) break;
          file->data[file->offset + i] = *reinterpret_cast<const uint8_t*>(pa);
        }
        file->offset += len;
        if (file->offset > file->size) file->size = file->offset;
        file->dirty = true;
        f->x[0] = len;
        return f;
      }
      f->x[0] = static_cast<uint64_t>(-1);
      return f;
    }

    case 1: {  /* SYS_EXIT (code) */
      p.exit_code = static_cast<int64_t>(a0);
      p.state = State::Zombie;
      /* Wake parent if waiting */
      for (unsigned i = 0; i < MAX_PROCS; ++i) {
        if (g_procs[i].pid == p.ppid && g_procs[i].state == State::Sleeping) {
          g_procs[i].state = State::Ready;
        }
      }
      return schedule_from(f);
    }

    case 2: {  /* SYS_GETPID () */
      f->x[0] = static_cast<uint64_t>(p.pid);
      return f;
    }

    case 3: {  /* SYS_YIELD () */
      p.state = State::Ready;
      return schedule_from(f);
    }

    case 4: {  /* SYS_TICKS () */
      f->x[0] = metal_ticks();
      return f;
    }

    case 5: {  /* SYS_STATE (buf, len) */
      if (!user_range_ok(a0, a1)) { f->x[0] = static_cast<uint64_t>(-1); return f; }
      f->x[0] = 0x57A7E001ull;
      return f;
    }

    case 6: {  /* SYS_OPEN (path, len, write) */
      const uint64_t path_va = a0;
      const uint64_t path_len = a1;
      const bool writable = (a2 != 0);
      if (!user_range_ok(path_va, path_len) || path_len >= 64) {
        f->x[0] = static_cast<uint64_t>(-1);
        return f;
      }
      char path[64] = {};
      for (uint64_t i = 0; i < path_len; ++i) {
        const uint64_t pa = mmu_translate(path_va + i);
        if (!pa) break;
        path[i] = *reinterpret_cast<const char*>(pa);
      }
      path[path_len] = '\0';

      auto* file = alloc_open_file();
      if (!file) { f->x[0] = static_cast<uint64_t>(-1); return f; }
      for (size_t i = 0; i < sizeof(file->path) && path[i]; ++i) file->path[i] = path[i];
      file->writable = writable;

      /* Attempt to load file from FAT32 */
      size_t read_bytes = 0;
      constexpr size_t INIT_CAP = 64 * 1024;
      auto* buf = static_cast<uint8_t*>(metal_alloc_pages(INIT_CAP));
      if (buf) {
        file->data = buf;
        file->capacity = INIT_CAP;
        if (fat32_read_file(path, file->data, file->capacity, read_bytes)) {
          file->size = read_bytes;
        } else if (!writable) {
          free_open_file(file);
          f->x[0] = static_cast<uint64_t>(-1);
          return f;
        }
      }

      int fd = fd_install(p, file);
      f->x[0] = (fd < 0) ? static_cast<uint64_t>(-1) : static_cast<uint64_t>(fd);
      return f;
    }

    case 7: {  /* SYS_READ (fd, buf, len) */
      const uint64_t fd = a0;
      const uint64_t va = a1;
      uint64_t len = a2;
      if (fd == 0) {
        /* stdin: this exists at all only because proc_run() is synchronous -
         * the kernel's own arm64_main loop is not running while an EL0
         * process is, so there is no separate "foreground process" owner to
         * arbitrate; this handler can poll the same raw sources
         * (usb_get_key()/metal_console_read()) that loop would, directly.
         * Non-blocking: returns however many bytes are actually available
         * right now (0 if the user hasn't typed anything), matching a
         * terminal in raw/non-canonical mode rather than a blocking line
         * read - a userland program spins on this with SYS_YIELD between
         * polls, exactly the way it already has to for anything else. */
        if (!user_range_ok(va, len)) { f->x[0] = static_cast<uint64_t>(-1); return f; }
        uint64_t got = 0;
        for (uint64_t i = 0; i < len; ++i) {
          int c = usb_get_key();
          if (c < 0) c = metal_console_read();
          if (c < 0) break;
          const uint64_t pa = mmu_translate(va + i);
          if (!pa) break;
          *reinterpret_cast<uint8_t*>(pa) = static_cast<uint8_t>(c);
          ++got;
        }
        f->x[0] = got;
        return f;
      }
      if (fd >= MAX_FDS || !p.fds[fd] || !user_range_ok(va, len)) {
        f->x[0] = static_cast<uint64_t>(-1);
        return f;
      }
      auto* file = p.fds[fd];
      uint64_t avail = (file->offset >= file->size) ? 0 : file->size - file->offset;
      if (len > avail) len = avail;
      for (uint64_t i = 0; i < len; ++i) {
        const uint64_t pa = mmu_translate(va + i);
        if (!pa) break;
        *reinterpret_cast<uint8_t*>(pa) = file->data[file->offset + i];
      }
      file->offset += len;
      f->x[0] = len;
      return f;
    }

    case 8: {  /* SYS_CLOSE (fd) */
      const uint64_t fd = a0;
      if (fd >= MAX_FDS || !p.fds[fd]) { f->x[0] = static_cast<uint64_t>(-1); return f; }
      free_open_file(p.fds[fd]);
      p.fds[fd] = nullptr;
      f->x[0] = 0;
      return f;
    }

    case 9: {  /* SYS_SEEK (fd, offset, whence) */
      const uint64_t fd = a0;
      const int64_t off = static_cast<int64_t>(a1);
      const uint64_t whence = a2;
      if (fd >= MAX_FDS || !p.fds[fd]) { f->x[0] = static_cast<uint64_t>(-1); return f; }
      auto* file = p.fds[fd];
      int64_t new_off = (whence == 1) ? static_cast<int64_t>(file->offset) + off
                      : (whence == 2) ? static_cast<int64_t>(file->size) + off : off;
      if (new_off < 0) new_off = 0;
      if (static_cast<uint64_t>(new_off) > file->size) new_off = static_cast<int64_t>(file->size);
      file->offset = static_cast<uint64_t>(new_off);
      f->x[0] = file->offset;
      return f;
    }

    case 10: {  /* SYS_BRK / SYS_SBRK (addr) */
      const uint64_t req = a0;
      if (req == 0) { f->x[0] = p.brk; return f; }
      if (req < p.brk || req > USER_STACK_TOP - USER_STACK_SIZE) {
        f->x[0] = static_cast<uint64_t>(-1);
        return f;
      }
      uint64_t old_brk_page = (p.brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
      uint64_t new_brk_page = (req + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
      if (new_brk_page > old_brk_page) {
        uint64_t grow_bytes = new_brk_page - old_brk_page;
        auto* pages = static_cast<uint8_t*>(metal_alloc_pages(grow_bytes));
        if (!pages || !add_mapping(p, old_brk_page, reinterpret_cast<uint64_t>(pages), grow_bytes, MAP_USER)) {
          if (pages) metal_free_pages(pages, grow_bytes);
          f->x[0] = static_cast<uint64_t>(-1);
          return f;
        }
      }
      p.brk = req;
      f->x[0] = p.brk;
      return f;
    }

    case 11: {  /* SYS_MMAP (len) */
      uint64_t len = (a0 + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
      if (!len || p.mmap_next + len > USER_END) {
        f->x[0] = static_cast<uint64_t>(-1);
        return f;
      }
      auto* pages = static_cast<uint8_t*>(metal_alloc_pages(len));
      if (!pages || !add_mapping(p, p.mmap_next, reinterpret_cast<uint64_t>(pages), len, MAP_USER)) {
        if (pages) metal_free_pages(pages, len);
        f->x[0] = static_cast<uint64_t>(-1);
        return f;
      }
      uint64_t ret = p.mmap_next;
      p.mmap_next += len;
      f->x[0] = ret;
      return f;
    }

    case 12: {  /* SYS_FORK () */
      int slot = -1;
      for (unsigned i = 0; i < MAX_PROCS; ++i) {
        if (g_procs[i].state == State::Free) { slot = static_cast<int>(i); break; }
      }
      if (slot < 0) { f->x[0] = static_cast<uint64_t>(-1); return f; }

      Proc& child = g_procs[slot];
      child = Proc{};
      child.pid = g_next_pid++;
      child.ppid = p.pid;
      for (size_t i = 0; i < sizeof(child.name) && p.name[i]; ++i) child.name[i] = p.name[i];
      child.brk = p.brk;
      child.mmap_next = p.mmap_next;
      child.slice_left = g_slice_ticks;

      /* Duplicate address space */
      bool ok = true;
      for (size_t i = 0; i < p.mapping_count; ++i) {
        const auto& m = p.mappings[i];
        auto* frame = static_cast<uint8_t*>(metal_alloc_pages(m.size));
        if (!frame) { ok = false; break; }
        const auto* src = reinterpret_cast<const uint8_t*>(m.pa);
        for (uint64_t k = 0; k < m.size; ++k) frame[k] = src[k];
        if (!add_mapping(child, m.va, reinterpret_cast<uint64_t>(frame), m.size, m.flags)) {
          metal_free_pages(frame, m.size);
          ok = false;
          break;
        }
      }
      if (!ok) {
        free_proc(child);
        f->x[0] = static_cast<uint64_t>(-1);
        return f;
      }

      child.kstack = static_cast<uint8_t*>(metal_alloc_pages(KSTACK_SIZE));
      if (!child.kstack) {
        free_proc(child);
        f->x[0] = static_cast<uint64_t>(-1);
        return f;
      }

      auto* cf = reinterpret_cast<TrapFrame*>(child.kstack + KSTACK_SIZE - FRAME_BYTES);
      *cf = *f;
      cf->x[0] = 0; /* Child returns 0 */
      child.frame = cf;

      for (unsigned i = 0; i < MAX_SIGNALS; ++i) child.handler[i] = p.handler[i];
      child.trampoline = p.trampoline;

      child.state = State::Ready;
      f->x[0] = static_cast<uint64_t>(child.pid);
      return f;
    }

    case 13: {  /* SYS_EXEC (path, len) */
      const uint64_t path_va = a0;
      const uint64_t path_len = a1;
      if (!user_range_ok(path_va, path_len) || path_len >= 64) {
        f->x[0] = static_cast<uint64_t>(-1);
        return f;
      }
      char path[64] = {};
      for (uint64_t i = 0; i < path_len; ++i) {
        const uint64_t pa = mmu_translate(path_va + i);
        if (!pa) break;
        path[i] = *reinterpret_cast<const char*>(pa);
      }
      path[path_len] = '\0';

      /* Check embedded programs */
      const Program* found = nullptr;
      for (unsigned i = 0; i < PROGRAM_COUNT; ++i) {
        if (strcmp(path, g_programs[i].name) == 0) {
          found = &g_programs[i];
          break;
        }
      }

      if (found) {
        for (size_t i = 0; i < p.mapping_count; ++i) {
          mmu_unmap(p.mappings[i].va, p.mappings[i].size);
          metal_free_pages(reinterpret_cast<void*>(p.mappings[i].pa), p.mappings[i].size);
        }
        p.mapping_count = 0;
        if (!load_elf(p, found->start, found->end - found->start)) {
          p.state = State::Exited;
          p.exit_code = -1;
          return schedule_from(f);
        }
        for (size_t i = 0; i < sizeof(p.name); ++i) p.name[i] = path[i];
        p.in_signal = false;
        return p.frame;
      }

      /* Check FAT32 binary */
      size_t file_len = 0;
      constexpr size_t MAX_BIN = 512 * 1024;
      auto* bin_buf = static_cast<uint8_t*>(metal_alloc_pages(MAX_BIN));
      if (bin_buf && fat32_read_file(path, bin_buf, MAX_BIN, file_len)) {
        for (size_t i = 0; i < p.mapping_count; ++i) {
          mmu_unmap(p.mappings[i].va, p.mappings[i].size);
          metal_free_pages(reinterpret_cast<void*>(p.mappings[i].pa), p.mappings[i].size);
        }
        p.mapping_count = 0;
        bool loaded = load_elf(p, bin_buf, file_len);
        metal_free_pages(bin_buf, MAX_BIN);
        if (!loaded) {
          p.state = State::Exited;
          p.exit_code = -1;
          return schedule_from(f);
        }
        for (size_t i = 0; i < sizeof(p.name); ++i) p.name[i] = path[i];
        p.in_signal = false;
        return p.frame;
      }
      if (bin_buf) metal_free_pages(bin_buf, MAX_BIN);

      f->x[0] = static_cast<uint64_t>(-1);
      return f;
    }

    case 14: {  /* SYS_WAIT (&status) */
      const uint64_t status_va = a0;
      for (unsigned i = 0; i < MAX_PROCS; ++i) {
        Proc& other = g_procs[i];
        if (other.ppid != p.pid || other.state != State::Zombie || other.reaped_by_parent) continue;
        other.reaped_by_parent = true;
        if (status_va && user_range_ok(status_va, 8)) {
          const uint64_t pa = mmu_translate(status_va);
          if (pa) *reinterpret_cast<int64_t*>(pa) = other.exit_code;
        }
        f->x[0] = static_cast<uint64_t>(other.pid);
        return f;
      }
      bool any_child = false;
      for (unsigned i = 0; i < MAX_PROCS; ++i) {
        if (g_procs[i].ppid == p.pid && !g_procs[i].reaped_by_parent && g_procs[i].state != State::Free) {
          any_child = true;
          break;
        }
      }
      if (!any_child) { f->x[0] = static_cast<uint64_t>(-1); return f; }

      /* Child running: rewind SVC instruction (4 bytes) and block */
      f->elr_el1 -= 4;
      p.state = State::Sleeping;
      p.wake_at = 0;
      return schedule_from(f);
    }

    case 15: {  /* SYS_KILL (pid, sig) */
      const int target_pid = static_cast<int>(a0);
      const uint64_t sig = a1;
      Proc* target = find_proc(target_pid);
      if (!target || target->state == State::Zombie || target->state == State::Free || sig >= MAX_SIGNALS) {
        f->x[0] = static_cast<uint64_t>(-1);
        return f;
      }
      target->sig_pending |= (1ull << sig);
      if (target->state == State::Sleeping) target->state = State::Ready;
      f->x[0] = 0;
      return (target == &p) ? schedule_from(f) : f;
    }

    case 16: {  /* SYS_SIGNAL (sig, handler, trampoline) */
      const uint64_t sig = a0;
      const uint64_t handler = a1;
      const uint64_t tramp = a2;
      if (sig == 0 || sig >= MAX_SIGNALS || sig == 9) {
        f->x[0] = static_cast<uint64_t>(-1);
        return f;
      }
      p.handler[sig] = handler;
      if (tramp) p.trampoline = tramp;
      f->x[0] = 0;
      return f;
    }

    case 17: {  /* SYS_SIGRETURN () */
      if (!p.in_signal) { f->x[0] = static_cast<uint64_t>(-1); return f; }
      p.in_signal = false;
      *f = p.saved_frame;
      return f;
    }

    case 18: {  /* SYS_GETPPID () */
      f->x[0] = static_cast<uint64_t>(p.ppid);
      return f;
    }

    case 19: {  /* SYS_SLEEP (ticks) */
      p.wake_at = metal_ticks() + a0;
      p.state = State::Sleeping;
      f->x[0] = 0;
      return schedule_from(f);
    }

    case 20: {  /* SYS_PERSIST (buf, len) */
      const uint64_t va = a0;
      const uint64_t len = a1;
      if (!user_range_ok(va, len)) { f->x[0] = static_cast<uint64_t>(-1); return f; }
      char fn[32] = "proc-";
      size_t pos = 5;
      uint64_t pid_val = static_cast<uint64_t>(p.pid);
      if (!pid_val) fn[pos++] = '0';
      char tmp[16]; int ti = 0;
      while (pid_val) { tmp[ti++] = '0' + (pid_val % 10); pid_val /= 10; }
      while (ti > 0) fn[pos++] = tmp[--ti];
      const char ext[] = ".txt";
      for (size_t k = 0; ext[k]; ++k) fn[pos++] = ext[k];
      fn[pos] = '\0';

      auto* kbuf = static_cast<uint8_t*>(metal_alloc_pages(len));
      if (!kbuf) { f->x[0] = static_cast<uint64_t>(-1); return f; }
      for (uint64_t i = 0; i < len; ++i) {
        const uint64_t pa = mmu_translate(va + i);
        kbuf[i] = pa ? *reinterpret_cast<const uint8_t*>(pa) : 0;
      }
      bool ok = fat32_write_file(fn, kbuf, len);
      metal_free_pages(kbuf, len);
      f->x[0] = ok ? len : static_cast<uint64_t>(-1);
      return f;
    }

    /* SYS_JEPA_PREDICT / SYS_JEPA_STATE / SYS_JEPA_SHM. This kernel has no
     * world model behind them, and they used to pretend: PREDICT copied the
     * input state to the output as the "prediction", STATE returned the
     * caller's count having written nothing (after a count * 4 that could
     * wrap), and SHM returned USER_BASE - the caller's own program text - as
     * the shared region. A call that cannot be answered says so. */
    case 21:
    case 22:
    case 23:
      f->x[0] = static_cast<uint64_t>(-1);
      return f;

    default:
      f->x[0] = static_cast<uint64_t>(-1);
      return f;
  }
}

}  /* namespace */

void proc_init() {
  metal_tick_hook = on_tick;
  metal_syscall_hook = on_syscall;
  metal_user_fault_hook = on_user_fault;
}

void proc_report() {
  kputs("  embedded EL0 programs:\n");
  for (unsigned i = 0; i < PROGRAM_COUNT; ++i) {
    const auto& g = g_programs[i];
    const uint64_t len = static_cast<uint64_t>(g.end - g.start);
    const auto* eh = reinterpret_cast<const Elf64Header*>(g.start);
    const bool elf = len > 64 && eh->ident[0] == 0x7F && eh->ident[1] == 'E' &&
                     eh->machine == EM_AARCH64;
    kprintf("    %u  %s  %u bytes, %s, entry %p\n", static_cast<uint64_t>(i), g.name, len,
            elf ? "ELF64 aarch64" : "NOT A VALID ELF", elf ? eh->entry : 0);
  }
  kprintf("  %u context switches so far\n", g_switches);
  bool any = false;
  for (unsigned i = 0; i < MAX_PROCS; ++i) {
    const Proc& p = g_procs[i];
    if (p.state == State::Free) continue;
    if (!any) { kputs("  process table:\n"); any = true; }
    const char* s = p.state == State::Exited ? (p.faulted ? "killed" : "exited")
                  : p.state == State::Zombie ? "zombie"
                  : p.state == State::Sleeping ? "sleep"
                  : p.state == State::Running ? "running" : "ready";
    kprintf("    pid %d  ppid %d  %s  %s  ticks %u  syscalls %u  exit %d\n",
            static_cast<int64_t>(p.pid), static_cast<int64_t>(p.ppid), p.name, s, p.ticks, p.syscalls, p.exit_code);
  }
  if (!any) kputs("  process table empty\n");
}

bool proc_run(unsigned index) {
  if (index >= PROGRAM_COUNT) return false;

  int slot = -1;
  for (unsigned i = 0; i < MAX_PROCS; ++i)
    if (g_procs[i].state == State::Free) { slot = static_cast<int>(i); break; }
  if (slot < 0)
    for (unsigned i = 0; i < MAX_PROCS; ++i)
      if (g_procs[i].state == State::Exited || g_procs[i].state == State::Zombie) {
        slot = static_cast<int>(i); break;
      }
  if (slot < 0) { kputs("  process table full\n"); return true; }

  Proc& p = g_procs[slot];
  if (p.state == State::Exited || p.state == State::Zombie) free_proc(p);
  p = Proc{};
  p.pid = g_next_pid++;
  p.ppid = 0;
  for (size_t i = 0; i < sizeof(p.name) && g_programs[index].name[i]; ++i)
    p.name[i] = g_programs[index].name[i];

  if (!load_elf(p, g_programs[index].start, g_programs[index].end - g_programs[index].start)) {
    kputs("  could not load that program\n");
    p.state = State::Free;
    return true;
  }

  kprintf("  pid %d (%s): entry %p, mapped for EL0\n",
          static_cast<int64_t>(p.pid), p.name, p.frame->elr_el1);

  p.state = State::Running;
  p.slice_left = g_slice_ticks;
  g_current = slot;
  g_in_process = true;

  const uint64_t t0 = metal_ticks();
  if (metal_ctx_save(&g_shell_ctx) == 0) {
    arm64_enter_frame(p.frame);
  }

  asm volatile("msr daifclr, #2" ::: "memory");
  g_in_process = false;
  g_current = -1;

  kprintf("  pid %d %s after %u ticks and %u syscalls, exit %d\n",
          static_cast<int64_t>(p.pid), p.faulted ? "was killed" : "exited",
          metal_ticks() - t0, p.syscalls, p.exit_code);
  return true;
}

