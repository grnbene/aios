// The AOS freestanding C++ runtime: what the standard library implementation
// needs from "the system", provided here from AOS primitives instead of an OS.
// Route A: the containers stay standard; only the substrate changes.
#include <cstddef>
#include <cstdint>
#include <new>

extern "C" void metal_console_write(const char* s, size_t n);
extern "C" [[noreturn]] void metal_panic(const char* what);

// ---- memory and string primitives -------------------------------------------
extern "C" {
void* memcpy(void* d, const void* s, size_t n) {
  auto* a = (uint8_t*)d;
  auto* b = (const uint8_t*)s;
  while (n >= 8) {
    *(uint64_t*)a = *(const uint64_t*)b;
    a += 8; b += 8; n -= 8;
  }
  while (n--) *a++ = *b++;
  return d;
}
void* memmove(void* d, const void* s, size_t n) {
  auto* a = (uint8_t*)d;
  auto* b = (const uint8_t*)s;
  if (a < b) {
    while (n >= 8) {
      *(uint64_t*)a = *(const uint64_t*)b;
      a += 8; b += 8; n -= 8;
    }
    while (n--) *a++ = *b++;
  } else {
    a += n; b += n;
    while (n >= 8) {
      a -= 8; b -= 8; n -= 8;
      *(uint64_t*)a = *(const uint64_t*)b;
    }
    while (n--) *--a = *--b;
  }
  return d;
}
void* memset(void* d, int c, size_t n) {
  auto* a = (uint8_t*)d;
  uint8_t uc = (uint8_t)c;
  uint64_t c64 = (uint64_t)uc * 0x0101010101010101ull;
  while (n >= 8) {
    *(uint64_t*)a = c64;
    a += 8; n -= 8;
  }
  while (n--) *a++ = uc;
  return d;
}
int memcmp(const void* x, const void* y, size_t n) { auto* a = (const uint8_t*)x; auto* b = (const uint8_t*)y; for (; n; --n, ++a, ++b) if (*a != *b) return *a < *b ? -1 : 1; return 0; }
void* memchr(const void* s, int c, size_t n) { auto* p = (const uint8_t*)s; for (; n; --n, ++p) if (*p == (uint8_t)c) return (void*)p; return nullptr; }
size_t strlen(const char* s) { size_t n = 0; while (s[n]) ++n; return n; }
int strcmp(const char* a, const char* b) { while (*a && *a == *b) { ++a; ++b; } return (uint8_t)*a - (uint8_t)*b; }
int strncmp(const char* a, const char* b, size_t n) { for (; n; --n, ++a, ++b) { if (*a != *b) return (uint8_t)*a - (uint8_t)*b; if (!*a) return 0; } return 0; }
char* strchr(const char* s, int c) { for (;; ++s) { if (*s == (char)c) return (char*)s; if (!*s) return nullptr; } }
size_t strnlen(const char* s, size_t m) { size_t n = 0; while (n < m && s[n]) ++n; return n; }
// fortify-source variants, in case any object was built with checking on
void* __memcpy_chk(void* d, const void* s, size_t n, size_t dl) { if (n > dl) metal_panic("memcpy overflow"); return memcpy(d, s, n); }
void* __memmove_chk(void* d, const void* s, size_t n, size_t dl) { if (n > dl) metal_panic("memmove overflow"); return memmove(d, s, n); }
void* __memset_chk(void* d, int c, size_t n, size_t dl) { if (n > dl) metal_panic("memset overflow"); return memset(d, c, n); }

// ---- the heap: intrusive buddy allocator with split & coalesce -------------
//
// Size classes: c = 0..kMaxOrder, where block size is (16 << c).
// kMinBlockSize = 32 bytes (order 1, size_class 1) so every block can hold
// the intrusive free-list pointers: next and prev.
// Header format:
//   offset -16: uint64_t order
//   offset -8:  uint64_t is_free (0 = allocated, 1 = free)
// When free, block payload (offset 0..size-16) contains:
//   offset 0: BuddyNode* next
//   offset 8: BuddyNode* prev
//
// On malloc:
//   If free list at order has a block, take it.
//   Else, take a block from the smallest higher order > c and iteratively
//   split it down in half into buddies until reaching order c.
//   If no higher order has a free block, carve a new 2^k aligned block from
//   the uncommitted heap tail (heap_cur).
//
// On free:
//   Mark block free. Look up its buddy address: buddy = base + ((addr - base) ^ size).
//   If buddy is inside committed heap and marked free and at the same order,
//   remove buddy from free list, coalesce them into order+1, and repeat.
//   Then insert the coalesced block into free_lists[order].
// This eliminates fragmentation panic from mixed small AST node churn followed
// by large page allocations.

static constexpr int kMinOrder = 1;      // 32 bytes minimum block
static constexpr int kMaxOrder = 30;     // up to 16 << 30 = 16 GiB
static uint8_t* heap_base_ptr = nullptr;
static uint8_t* heap_cur = nullptr;
static uint8_t* heap_end = nullptr;
static size_t heap_used = 0, heap_peak = 0;
static size_t heap_size = 0;

struct BuddyHeader {
  uint64_t order;
  uint64_t is_free;
};
static_assert(sizeof(BuddyHeader) == 16, "BuddyHeader must be 16 bytes");

struct BuddyFreeNode {
  BuddyFreeNode* next;
  BuddyFreeNode* prev;
};

static BuddyFreeNode* free_lists[kMaxOrder + 1] = {};
static uint64_t heap_coalesced = 0;   // buddy pairs joined, for the acceptance check

// Every core can allocate (smp.cpp runs jobs on the application processors): one lock over the heap.
static volatile int heap_lock = 0;
struct HeapLock {
  HeapLock() {
    while (__atomic_test_and_set(&heap_lock, __ATOMIC_ACQUIRE)) {
#if defined(__x86_64__) || defined(__i386__)
      asm volatile("pause");
#elif defined(__aarch64__)
      asm volatile("yield");
#endif
    }
  }
  ~HeapLock() { __atomic_clear(&heap_lock, __ATOMIC_RELEASE); }
};

void metal_heap_init(void* base, size_t size) {
  // Align base to 32 bytes
  uintptr_t b = ((uintptr_t)base + 31) & ~(uintptr_t)31;
  uintptr_t end = (uintptr_t)base + size;
  if (end > b) {
    heap_base_ptr = (uint8_t*)b;
    heap_cur = heap_base_ptr;
    heap_end = (uint8_t*)end;
    heap_size = end - b;
  } else {
    heap_base_ptr = (uint8_t*)base;
    heap_cur = heap_base_ptr;
    heap_end = heap_cur;
    heap_size = 0;
  }
  for (int i = 0; i <= kMaxOrder; ++i) free_lists[i] = nullptr;
  heap_used = 0;
  heap_peak = 0;
}

static inline int size_class(size_t n) {
  int c = 0;
  size_t s = 16;
  while (s < n && c < kMaxOrder) { s <<= 1; ++c; }
  return c;
}

static void free_list_remove(int order, BuddyFreeNode* node) {
  if (!node) return;
  if (node->prev) node->prev->next = node->next;
  else free_lists[order] = node->next;
  if (node->next) node->next->prev = node->prev;
  node->next = nullptr;
  node->prev = nullptr;
}

static void free_list_insert(int order, BuddyFreeNode* node) {
  if (!node) return;
  node->next = free_lists[order];
  node->prev = nullptr;
  if (free_lists[order]) free_lists[order]->prev = node;
  free_lists[order] = node;
}

// [from, to) is address space the heap skipped to align a new block. It is
// handed back as free blocks, each the largest that is aligned (relative to the
// heap base, as the buddy arithmetic needs) and fits - left alone it was lost
// for good: a 60 KiB request after small-object churn skipped up to 64 KiB each
// time, and a free() could read the stale bytes in it as a block header.
static void gap_to_free_blocks(uint8_t* from, uint8_t* to) {
  while (from + ((size_t)16 << kMinOrder) <= to) {
    uintptr_t rel = (uintptr_t)(from - heap_base_ptr);
    int o = kMinOrder;
    while (o < kMaxOrder) {
      size_t next = (size_t)16 << (o + 1);
      if ((rel & (next - 1)) != 0 || from + next > to) break;
      ++o;
    }
    BuddyHeader* h = (BuddyHeader*)from;
    h->order = (uint64_t)o;
    h->is_free = 1;
    free_list_insert(o, (BuddyFreeNode*)(from + 16));
    from += (size_t)16 << o;
  }
}

void* malloc(size_t n) {
  HeapLock lock;
  if (n == 0) n = 1;
  if (n > (size_t)-1 - 16) return nullptr;   // n + 16 would wrap to a small block
  int c = size_class(n + 16);
  if (c < kMinOrder) c = kMinOrder;
  if (c > kMaxOrder) return nullptr;

  size_t sz = (size_t)16 << c;

  // 1. Check if free list for this order has a block
  BuddyFreeNode* chosen = free_lists[c];
  if (chosen) {
    free_list_remove(c, chosen);
    BuddyHeader* hdr = (BuddyHeader*)((uint8_t*)chosen - 16);
    hdr->is_free = 0;
    hdr->order = (uint64_t)c;
    heap_used += sz;
    if (heap_used > heap_peak) heap_peak = heap_used;
    return (uint8_t*)chosen;
  }

  // 2. Search for smallest available higher order to split
  int split_order = -1;
  for (int o = c + 1; o <= kMaxOrder; ++o) {
    if (free_lists[o]) {
      split_order = o;
      break;
    }
  }

  uint8_t* block_addr = nullptr;

  if (split_order != -1) {
    // Split down from split_order to c
    BuddyFreeNode* high = free_lists[split_order];
    free_list_remove(split_order, high);
    block_addr = (uint8_t*)high - 16;
    for (int curr = split_order; curr > c; --curr) {
      size_t half_sz = (size_t)16 << (curr - 1);
      uint8_t* buddy_addr = block_addr + half_sz;
      BuddyHeader* buddy_hdr = (BuddyHeader*)buddy_addr;
      buddy_hdr->order = (uint64_t)(curr - 1);
      buddy_hdr->is_free = 1;
      free_list_insert(curr - 1, (BuddyFreeNode*)(buddy_addr + 16));
    }
  } else {
    // 3. No block in free lists can satisfy: carve a 2^(c+4) block from heap_cur,
    // aligned to its size relative to the heap base - buddy() is an XOR of the
    // offset from the base, so an absolute alignment pairs the wrong blocks
    // whenever the base itself is not aligned that far.
    uintptr_t base_u = (uintptr_t)heap_base_ptr;
    uintptr_t rel_cur = (uintptr_t)heap_cur - base_u;
    uintptr_t a = base_u + ((rel_cur + (sz - 1)) & ~(uintptr_t)(sz - 1));
    if (a + sz > (uintptr_t)heap_end) {
      static char msg[160];
      auto put = [](char*& o, const char* t) { while (*t) *o++ = *t++; };
      auto num = [](char*& o, uint64_t v) { char b[24]; int k = 0; do { b[k++] = (char)('0' + v % 10); v /= 10; } while (v); while (k) *o++ = b[--k]; };
      char* o = msg;
      put(o, "heap exhausted: asked for "); num(o, n); put(o, " bytes, ");
      num(o, (uint64_t)((uintptr_t)heap_end - (uintptr_t)heap_cur)); put(o, " left of "); num(o, heap_size);
      put(o, ", in use "); num(o, heap_used); *o = 0;
      metal_panic(msg);
    }
    block_addr = (uint8_t*)a;
    gap_to_free_blocks(heap_cur, block_addr);
    heap_cur = (uint8_t*)(a + sz);
  }

  BuddyHeader* hdr = (BuddyHeader*)block_addr;
  hdr->order = (uint64_t)c;
  hdr->is_free = 0;
  heap_used += sz;
  if (heap_used > heap_peak) heap_peak = heap_used;
  return block_addr + 16;
}

void free(void* q) {
  if (!q) return;
  HeapLock lock;
  uint8_t* block_addr = (uint8_t*)q - 16;
  if (block_addr < heap_base_ptr || block_addr >= heap_cur) return;

  BuddyHeader* hdr = (BuddyHeader*)block_addr;
  int c = (int)hdr->order;
  if (c < kMinOrder || c > kMaxOrder) return;
  if (hdr->is_free != 0) return; // already free

  hdr->is_free = 1;
  size_t sz = (size_t)16 << c;
  heap_used -= sz;

  // Intrusive buddy coalescing
  while (c < kMaxOrder) {
    size_t block_sz = (size_t)16 << c;
    uintptr_t offset = (uintptr_t)(block_addr - heap_base_ptr);
    uintptr_t buddy_offset = offset ^ block_sz;
    uint8_t* buddy_addr = heap_base_ptr + buddy_offset;

    // Check if buddy is within currently carved heap range
    if (buddy_addr < heap_base_ptr || buddy_addr + block_sz > heap_cur) {
      break;
    }

    BuddyHeader* buddy_hdr = (BuddyHeader*)buddy_addr;
    // Coalesce only if buddy is free, not in use, and of the exact same order
    if (buddy_hdr->is_free != 1 || (int)buddy_hdr->order != c) {
      break;
    }

    // Buddy is free at same order: coalesce!
    ++heap_coalesced;
    BuddyFreeNode* buddy_node = (BuddyFreeNode*)(buddy_addr + 16);
    free_list_remove(c, buddy_node);

    // Merge into the lower address block
    if (buddy_addr < block_addr) {
      block_addr = buddy_addr;
    }
    ++c;
    BuddyHeader* merged_hdr = (BuddyHeader*)block_addr;
    merged_hdr->order = (uint64_t)c;
    merged_hdr->is_free = 1;
  }

  // Insert coalesced block into free list
  free_list_insert(c, (BuddyFreeNode*)(block_addr + 16));
}

void* calloc(size_t a, size_t b) {
  if (b != 0 && a > (size_t)-1 / b) return nullptr;
  void* p = malloc(a * b);
  if (p) memset(p, 0, a * b);
  return p;
}

void* realloc(void* p, size_t n) {
  if (!p) return malloc(n);
  BuddyHeader* hdr = (BuddyHeader*)((uint8_t*)p - 16);
  int c = (int)hdr->order;
  size_t have = ((size_t)16 << c) - 16;
  if (n <= have) return p;
  void* q = malloc(n);
  if (q) {
    memcpy(q, p, have);
    free(p);
  }
  return q;
}

size_t metal_heap_peak() { return heap_peak; }
size_t metal_heap_used() { return heap_used; }
size_t metal_heap_total() { return heap_size; }
size_t metal_heap_untouched() { return (size_t)(heap_end - heap_cur); }   // never yet cut into blocks
uint64_t metal_heap_coalesced() { return heap_coalesced; }
// Map a device's MMIO where the firmware actually put it.
//
// boot.S identity-maps the first 4 GiB with 1 GiB pages, which covers RAM and
// the 32-bit PCI hole — and nothing else. A modern virtio device's MMIO BAR is
// 64-bit and prefetchable, and QEMU puts it at 0xc000000000: three quarters of
// a terabyte up, with no page table entry behind it. Touching it does not
// return garbage, it faults, and a kernel with no fault handler simply stops —
// which is exactly what the virtio-gpu bring-up did.
//
// So: install a 1 GiB identity page for the region, uncacheable, walking the
// live table through CR3 rather than a linker symbol. The spare directories
// are static because this can be called before or after the heap exists.
extern "C" bool metal_map_device_1g(uint64_t phys) {
  alignas(4096) static uint64_t spare[4][512];
  static int spare_used = 0;
  uint64_t cr3 = 0;
  asm volatile("mov %%cr3, %0" : "=r"(cr3));
  uint64_t* pml4 = (uint64_t*)(uintptr_t)(cr3 & ~0xFFFull);
  unsigned i4 = (unsigned)((phys >> 39) & 0x1FF);
  unsigned i3 = (unsigned)((phys >> 30) & 0x1FF);
  uint64_t* pdpt;
  if (!(pml4[i4] & 1)) {
    if (spare_used >= 4) return false;
    pdpt = spare[spare_used++];
    memset(pdpt, 0, 4096);
    pml4[i4] = ((uint64_t)(uintptr_t)pdpt) | 0x3;          // present, writable
  } else {
    pdpt = (uint64_t*)(uintptr_t)(pml4[i4] & 0x000FFFFFFFFFF000ull);
  }
  // 1 GiB page, present, writable, and cache-disabled: this is a device, and a
  // write that sits in a cache line is a command the device never sees.
  pdpt[i3] = (phys & ~((1ull << 30) - 1)) | 0x83 | 0x10 | 0x08;
  asm volatile("mov %%cr3, %%rax\n\tmov %%rax, %%cr3" ::: "rax", "memory");
  return true;
}

void* metal_alloc_pages(size_t bytes) {   // page-aligned, zeroed, never freed (mappings)
  uintptr_t a = ((uintptr_t)heap_cur + 4095) & ~(uintptr_t)4095;
  if (a + bytes > (uintptr_t)heap_end) return nullptr;
  heap_cur = (uint8_t*)(a + bytes); memset((void*)a, 0, bytes); return (void*)a;
}

// ---- C++ runtime hooks ------------------------------------------------------------
void abort(void) { metal_panic("abort()"); }
void __stack_chk_fail(void) { metal_panic("stack check failed"); }
char __libc_single_threaded = 1;
void* __dso_handle = &__dso_handle;
int __cxa_atexit(void (*)(void*), void*, void*) { return 0; }
// A thread_local with a destructor — src/userland/sql_database.cpp keeps its
// outer-scope stack in one — makes libstdc++ register a teardown callback with
// glibc. There is no glibc here and no thread that ever exits: the machine
// runs until it is powered off. Registering nothing is the whole job, and
// without this symbol the link fails on an object libstdc++ pulls in itself,
// naming a function no line of AIOS ever wrote.
int __cxa_thread_atexit_impl(void (*)(void*), void*, void*) { return 0; }
void __cxa_pure_virtual() { metal_panic("pure virtual call"); }
// libstdc++'s verbose terminate handler talks to stdio; route it to the console.
struct _IO_FILE; typedef struct _IO_FILE FILE;
static int fake_stderr; FILE* stderr = (FILE*)&fake_stderr;
int fputs(const char* s, FILE*) { metal_console_write(s, strlen(s)); return 0; }
size_t fwrite(const void* p, size_t sz, size_t n, FILE*) { metal_console_write((const char*)p, sz * n); return n; }
int fputc(int c, FILE*) { char ch = (char)c; metal_console_write(&ch, 1); return c; }
int fflush(FILE*) { return 0; }   // the console is unbuffered: there is nothing to push
int putchar(int c) { char ch = (char)c; metal_console_write(&ch, 1); return c; }
int puts(const char* s) { metal_console_write(s, strlen(s)); metal_console_write("\n", 1); return 0; }
int dl_iterate_phdr(void*, void*) { return 0; }
int _dl_find_object(void*, void*) { return -1; }   // libgcc_eh asks glibc first; we have registered frames only
int sched_yield(void) { return 0; }
// glibc conveniences libstdc++.a's demangler and terminate handler were built against.
char* getenv(const char*) { return nullptr; }
char* secure_getenv(const char*) { return nullptr; }
/* No process environment on bare metal: setting one fails, honestly, so the
 * grain setenv() builtin reports an error instead of pretending. */
int setenv(const char*, const char*, int) { return -1; }
int unsetenv(const char*) { return -1; }
unsigned long __isoc23_strtoul(const char* s, char** end, int base) {
  unsigned long v = 0; if (base == 0) base = 10;
  while (*s == ' ') ++s;
  for (;; ++s) { int d; if (*s >= '0' && *s <= '9') d = *s - '0'; else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10; else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10; else break; if (d >= base) break; v = v * (unsigned long)base + (unsigned long)d; }
  if (end) *end = (char*)s;
  return v;
}
// ---- printf: enough of it to be the C library on this substrate -----------
// The standard library reaches for this in places that have nothing to do with
// files: std::to_string(double) is vsnprintf, and libstdc++'s demangler and
// terminate handler use sprintf. It is not glibc's printf and does not try to
// be — it is the conversions this kernel's callers actually use, formatted
// correctly, with the width and precision they ask for.
namespace {
struct Sink {
  char* out; size_t cap; size_t n;
  void put(char c) { if (out && n + 1 < cap) out[n] = c; ++n; }
  void puts_(const char* s) { while (*s) put(*s++); }
};

void emit_uint(Sink& s, unsigned long long v, unsigned base, bool upper, int min_digits) {
  char b[32]; int i = 0;
  const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  do { b[i++] = digits[v % base]; v /= base; } while (v && i < 32);
  while (i < min_digits && i < 32) b[i++] = '0';
  while (i) s.put(b[--i]);
}

#define AIOS_FLOAT_FORMAT 1
#include "float_common.inc"
#undef AIOS_FLOAT_FORMAT

int format_into(char* out, size_t cap, const char* fmt, __builtin_va_list ap) {
  Sink s{out, cap, 0};
  for (const char* f = fmt; *f; ++f) {
    if (*f != '%') { s.put(*f); continue; }
    ++f;
    bool left = false, zero = false, plus = false, space = false, alt = false;
    for (;; ++f) {
      if (*f == '-') left = true;
      else if (*f == '0') zero = true;
      else if (*f == '+') plus = true;
      else if (*f == ' ') space = true;
      else if (*f == '#') alt = true;
      else if (*f == '\'') {}
      else break;
    }
    int width = 0;
    if (*f == '*') { width = __builtin_va_arg(ap, int); ++f; if (width < 0) { left = true; width = -width; } }
    else while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
    int prec = -1;
    if (*f == '.') {
      ++f; prec = 0;
      if (*f == '*') { prec = __builtin_va_arg(ap, int); ++f; }
      else while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0');
    }
    int lng = 0;
    for (;; ++f) {
      if (*f == 'l') ++lng;
      else if (*f == 'z' || *f == 'j' || *f == 't') lng = 2;
      else if (*f == 'h' || *f == 'L' || *f == 'q') {}
      else break;
    }
    char conv = *f;
    if (!conv) break;
    if (conv == '%') { s.put('%'); continue; }

    // Strings pad without a scratch buffer; everything else formats into one so
    // the width applies to the finished item.
    if (conv == 's') {
      const char* t = __builtin_va_arg(ap, const char*);
      if (!t) t = "(null)";
      int len = 0; while (t[len] && (prec < 0 || len < prec)) ++len;
      for (int i = len; !left && i < width; ++i) s.put(' ');
      for (int i = 0; i < len; ++i) s.put(t[i]);
      for (int i = len; left && i < width; ++i) s.put(' ');
      continue;
    }

    char scratch[480];   // %f of a big double is ~310 digits before any precision
    Sink item{scratch, sizeof scratch, 0};
    switch (conv) {
      case 'c': item.put((char)__builtin_va_arg(ap, int)); break;
      case 'd': case 'i': {
        long long v = lng >= 2 ? __builtin_va_arg(ap, long long) : (lng == 1 ? (long long)__builtin_va_arg(ap, long) : (long long)__builtin_va_arg(ap, int));
        if (v < 0) { item.put('-'); emit_uint(item, (unsigned long long)(-(v + 1)) + 1ull, 10, false, prec < 0 ? 1 : prec); }
        else { if (plus) item.put('+'); else if (space) item.put(' '); emit_uint(item, (unsigned long long)v, 10, false, prec < 0 ? 1 : prec); }
        break;
      }
      case 'u': {
        unsigned long long v = lng >= 2 ? __builtin_va_arg(ap, unsigned long long) : (lng == 1 ? (unsigned long long)__builtin_va_arg(ap, unsigned long) : (unsigned long long)__builtin_va_arg(ap, unsigned int));
        emit_uint(item, v, 10, false, prec < 0 ? 1 : prec);
        break;
      }
      case 'x': case 'X': case 'o': {
        unsigned long long v = lng >= 2 ? __builtin_va_arg(ap, unsigned long long) : (lng == 1 ? (unsigned long long)__builtin_va_arg(ap, unsigned long) : (unsigned long long)__builtin_va_arg(ap, unsigned int));
        emit_uint(item, v, conv == 'o' ? 8u : 16u, conv == 'X', prec < 0 ? 1 : prec);
        break;
      }
      case 'p': {
        item.puts_("0x");
        emit_uint(item, (unsigned long long)(uintptr_t)__builtin_va_arg(ap, void*), 16, false, 1);
        break;
      }
      case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
        double v = __builtin_va_arg(ap, double);
        if (!(v < 0) && plus) item.put('+');
        else if (!(v < 0) && space) item.put(' ');
        emit_double(item, v, prec, conv, alt);
        break;
      }
      default: item.put('%'); item.put(conv); break;
    }
    size_t len = item.n < sizeof scratch ? item.n : sizeof scratch - 1;
    for (size_t i = len; !left && (int)i < width; ++i) s.put(zero && scratch[0] != '-' ? '0' : ' ');
    for (size_t i = 0; i < len; ++i) s.put(scratch[i]);
    for (size_t i = len; left && (int)i < width; ++i) s.put(' ');
  }
  if (out && cap) out[s.n < cap ? s.n : cap - 1] = 0;
  return (int)s.n;
}
}  // namespace

extern "C" {
int vsnprintf(char* out, size_t cap, const char* fmt, __builtin_va_list ap) { return format_into(out, cap, fmt, ap); }
int __vsnprintf_chk(char* out, size_t cap, int, size_t, const char* fmt, __builtin_va_list ap) { return format_into(out, cap, fmt, ap); }
int vsprintf(char* out, const char* fmt, __builtin_va_list ap) { return format_into(out, (size_t)-1, fmt, ap); }
int snprintf(char* out, size_t cap, const char* fmt, ...) {
  __builtin_va_list ap; __builtin_va_start(ap, fmt); int n = format_into(out, cap, fmt, ap); __builtin_va_end(ap); return n;
}
int sprintf(char* out, const char* fmt, ...) {
  __builtin_va_list ap; __builtin_va_start(ap, fmt); int n = format_into(out, (size_t)-1, fmt, ap); __builtin_va_end(ap); return n;
}
// Reading a formatted string back. NOT a general sscanf — it covers literals,
// whitespace, and %d/%u/%ld/%lld/%f/%c/%s, which is what this image actually
// asks for ("%d,%d", four call sites between the compositor and the face
// language). Anything else stops the scan and is reported as a short count,
// because a scanner that quietly skipped a directive it did not understand
// would hand its caller an uninitialised variable and no way to know.
namespace {
bool scan_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }

int scan_from(const char* s, const char* fmt, __builtin_va_list ap) {
  int assigned = 0;
  while (*fmt) {
    if (scan_space(*fmt)) { while (scan_space(*s)) ++s; ++fmt; continue; }
    if (*fmt != '%') {
      if (*s != *fmt) return assigned;
      ++s; ++fmt; continue;
    }
    ++fmt;                                   // past '%'
    if (*fmt == '%') { if (*s != '%') return assigned; ++s; ++fmt; continue; }
    bool suppress = false;
    if (*fmt == '*') { suppress = true; ++fmt; }
    int width = 0;
    while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); ++fmt; }
    int longs = 0;
    while (*fmt == 'l') { ++longs; ++fmt; }
    const char conv = *fmt ? *fmt++ : '\0';

    if (conv == 'c') {
      if (!*s) return assigned;
      if (!suppress) { *__builtin_va_arg(ap, char*) = *s; ++assigned; }
      ++s;
      continue;
    }
    if (conv != 's') while (scan_space(*s)) ++s;   // %s and the numbers skip leading space; %c does not

    if (conv == 's') {
      while (scan_space(*s)) ++s;
      if (!*s) return assigned;
      char* dst = suppress ? nullptr : __builtin_va_arg(ap, char*);
      int n = 0;
      while (*s && !scan_space(*s) && (width == 0 || n < width)) { if (dst) dst[n] = *s; ++s; ++n; }
      if (dst) { dst[n] = '\0'; ++assigned; }
      continue;
    }

    const char* start = s;
    bool neg = false;
    if (conv == 'd' || conv == 'f' || conv == 'e' || conv == 'g') {
      if (*s == '+' || *s == '-') { neg = *s == '-'; ++s; }
    }
    if (conv == 'd' || conv == 'u' || conv == 'x') {
      const int base = conv == 'x' ? 16 : 10;
      unsigned long long v = 0;
      int digits = 0;
      for (;;) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (base == 16 && (*s | 32) >= 'a' && (*s | 32) <= 'f') d = (*s | 32) - 'a' + 10;
        else break;
        if (width && digits >= width) break;
        v = v * (unsigned)base + (unsigned)d; ++s; ++digits;
      }
      if (!digits) { s = start; return assigned; }
      if (!suppress) {
        const long long sv = neg ? -(long long)v : (long long)v;
        if (longs >= 2) *__builtin_va_arg(ap, long long*) = sv;
        else if (longs == 1) *__builtin_va_arg(ap, long*) = (long)sv;
        else if (conv == 'd') *__builtin_va_arg(ap, int*) = (int)sv;
        else *__builtin_va_arg(ap, unsigned*) = (unsigned)v;
        ++assigned;
      }
      continue;
    }
    if (conv == 'f' || conv == 'e' || conv == 'g') {
      double v = 0.0; int digits = 0;
      while (*s >= '0' && *s <= '9') { v = v * 10.0 + (*s - '0'); ++s; ++digits; }
      if (*s == '.') {
        ++s;
        double scale = 0.1;
        while (*s >= '0' && *s <= '9') { v += (*s - '0') * scale; scale *= 0.1; ++s; ++digits; }
      }
      if (!digits) { s = start; return assigned; }
      if (neg) v = -v;
      if (!suppress) {
        if (longs) *__builtin_va_arg(ap, double*) = v;
        else *__builtin_va_arg(ap, float*) = (float)v;
        ++assigned;
      }
      continue;
    }
    return assigned;                          // a directive this scanner does not implement
  }
  return assigned;
}
}  // namespace

int vsscanf(const char* s, const char* fmt, __builtin_va_list ap) { return scan_from(s, fmt, ap); }
int sscanf(const char* s, const char* fmt, ...) {
  __builtin_va_list ap; __builtin_va_start(ap, fmt); int n = scan_from(s, fmt, ap); __builtin_va_end(ap); return n;
}
// glibc 2.38+ redirects sscanf in <stdio.h> to this name, so the plain symbol
// above is never the one the call sites reference on this host. Both exist:
// the image must link the same whichever glibc's headers chose.
int __isoc23_sscanf(const char* s, const char* fmt, ...) {
  __builtin_va_list ap; __builtin_va_start(ap, fmt); int n = scan_from(s, fmt, ap); __builtin_va_end(ap); return n;
}
int __isoc99_sscanf(const char* s, const char* fmt, ...) {
  __builtin_va_list ap; __builtin_va_start(ap, fmt); int n = scan_from(s, fmt, ap); __builtin_va_end(ap); return n;
}

// There is no working directory on this substrate — the machine has one root
// and everything is named from it. Answering "/" is the truth here, not a stub:
// the caller (the face language absolutising a path) wants a prefix, and "/" is
// the correct prefix for every path this machine has.
char* getcwd(char* buf, size_t size) {
  if (!buf || size < 2) return nullptr;
  buf[0] = '/'; buf[1] = '\0';
  return buf;
}

int __snprintf_chk(char* out, size_t cap, int, size_t, const char* fmt, ...) {
  __builtin_va_list ap; __builtin_va_start(ap, fmt); int n = format_into(out, cap, fmt, ap); __builtin_va_end(ap); return n;
}
int __sprintf_chk(char* out, int, size_t cap, const char* fmt, ...) {
  __builtin_va_list ap; __builtin_va_start(ap, fmt); int n = format_into(out, cap, fmt, ap); __builtin_va_end(ap); return n;
}
int fprintf(FILE* f, const char* fmt, ...) {
  char b[512];
  __builtin_va_list ap; __builtin_va_start(ap, fmt); int n = format_into(b, sizeof b, fmt, ap); __builtin_va_end(ap);
  fputs(b, f);
  return n;
}
int printf(const char* fmt, ...) {
  char b[512];
  __builtin_va_list ap; __builtin_va_start(ap, fmt); int n = format_into(b, sizeof b, fmt, ap); __builtin_va_end(ap);
  metal_console_write(b, strlen(b));
  return n;
}
// The signed twin of __isoc23_strtoul above; std::atol is this.
long __isoc23_strtol(const char* s, char** end, int base) {
  while (*s == ' ' || *s == '\t') ++s;
  bool neg = false;
  if (*s == '+' || *s == '-') { neg = *s == '-'; ++s; }
  unsigned long v = __isoc23_strtoul(s, end, base);
  return neg ? -(long)v : (long)v;
}
long strtol(const char* s, char** end, int base) { return __isoc23_strtol(s, end, base); }
unsigned long strtoul(const char* s, char** end, int base) { return __isoc23_strtoul(s, end, base); }
long long strtoll(const char* s, char** end, int base) { return __isoc23_strtol(s, end, base); }
int atoi(const char* s) { return (int)__isoc23_strtol(s, nullptr, 10); }
long atol(const char* s) { return __isoc23_strtol(s, nullptr, 10); }
// glibc 2.38 headers redirect strtoll/strtoull to these, so the plain names
// above are never the ones the object files ask for.
long long __isoc23_strtoll(const char* s, char** end, int base) { return __isoc23_strtol(s, end, base); }
unsigned long long __isoc23_strtoull(const char* s, char** end, int base) { return __isoc23_strtoul(s, end, base); }
unsigned long long strtoull(const char* s, char** end, int base) { return __isoc23_strtoul(s, end, base); }
// `strtod` existed here only as `__strtod_l` returning 0.0 — a stub that turned
// every number the SQL engine and the grid sorter parsed into zero rather than
// failing where anyone could see it. Freestanding, but real.
#define AIOS_FLOAT_PARSE 1
#include "float_common.inc"
#undef AIOS_FLOAT_PARSE
float strtof(const char* s, char** end) { return (float)strtod(s, end); }
double atof(const char* s) { return strtod(s, nullptr); }
}

// Exception globals live in TLS inside libstdc++; there is one thread here.
static char cxa_globals[64];
void* __cxa_get_globals() { return cxa_globals; }
void* __cxa_get_globals_fast() { return cxa_globals; }
void* __tls_get_addr(void*) { metal_panic("thread-local storage is not available on this substrate"); }
// Threads: none on this substrate.
int pthread_mutex_lock(void*) { return 0; }
int pthread_mutex_unlock(void*) { return 0; }
// Missing glibc / libstdc++ stubs for freestanding metal link
wchar_t* wmemcpy(wchar_t* d, const wchar_t* s, size_t n) { auto* a = d; auto* b = s; while (n--) *a++ = *b++; return d; }
char* __nl_langinfo_l(int, void*) { static char empty[] = ""; return empty; }
char* bind_textdomain_codeset(const char*, const char*) { return nullptr; }
void* __uselocale(void*) { return nullptr; }
char* dgettext(const char*, const char* msgid) { return (char*)msgid; }
char* gettext(const char* msgid) { return (char*)msgid; }
size_t __strftime_l(char*, size_t, const char*, const void*, void*) { return 0; }
size_t __wcsftime_l(wchar_t*, size_t, const wchar_t*, const void*, void*) { return 0; }
wchar_t* wmemmove(wchar_t* d, const wchar_t* s, size_t n) { auto* a = d; auto* b = s; if (a < b) { while (n--) *a++ = *b++; } else { a += n; b += n; while (n--) *--a = *--b; } return d; }
char* strerror(int errnum) {
  if (errnum == 0) return (char*)"Success";
  if (errnum == 2) return (char*)"No such file or directory";
  if (errnum == 12) return (char*)"Out of memory";
  if (errnum == 13) return (char*)"Permission denied";
  if (errnum == 22) return (char*)"Invalid argument";
  return (char*)"Unknown error";
}
int strerror_r(int errnum, char* buf, size_t buflen) {
  const char* msg = strerror(errnum);
  size_t len = strlen(msg);
  if (len >= buflen) len = buflen ? buflen - 1 : 0;
  if (buflen) { memcpy(buf, msg, len); buf[len] = '\0'; }
  return 0;
}
wchar_t* wmemchr(const wchar_t* s, wchar_t c, size_t n) { for (; n; --n, ++s) if (*s == c) return (wchar_t*)s; return nullptr; }
size_t wcslen(const wchar_t* s) { size_t n = 0; while (s[n]) ++n; return n; }
long syscall(long, ...) { return -1; }
float __strtof_l(const char* s, char** end, void*) { return (float)strtod(s, end); }
double __strtod_l(const char* s, char** end, void*) { return strtod(s, end); }
long double strtold_l(const char* s, char** end, void*) { return (long double)strtod(s, end); }
long double strtold(const char* s, char** end) { return (long double)strtod(s, end); }
void* __newlocale(int, const char*, void*) { return nullptr; }
void __freelocale(void*) {}
void* __duplocale(void*) { return nullptr; }
char* strdup(const char* s) { size_t n = strlen(s) + 1; char* p = (char*)malloc(n); if (p) memcpy(p, s, n); return p; }
size_t wcsnrtombs(char*, const wchar_t**, size_t, size_t, void*) { return (size_t)-1; }
size_t wcrtomb(char*, wchar_t, void*) { return (size_t)-1; }
size_t mbsnrtowcs(wchar_t*, const char**, size_t, size_t, void*) { return (size_t)-1; }
size_t mbrtowc(wchar_t*, const char*, size_t, void*) { return (size_t)-1; }
size_t __ctype_get_mb_cur_max(void) { return 1; }
size_t __mbsnrtowcs_chk(wchar_t*, const char**, size_t, size_t, void*, size_t) { return (size_t)-1; }
int __strcoll_l(const char* a, const char* b, void*) { return strcmp(a, b); }
char* strncpy(char* d, const char* s, size_t n) { char* r = d; while (n && (*d++ = *s++)) --n; while (n--) *d++ = 0; return r; }
size_t __strxfrm_l(char* d, const char* s, size_t n, void*) { size_t l = strlen(s); if (d && n) { strncpy(d, s, n); d[n-1] = 0; } return l; }
int __wcscoll_l(const wchar_t*, const wchar_t*, void*) { return 0; }
size_t __wcsxfrm_l(wchar_t*, const wchar_t*, size_t, void*) { return 0; }
int pthread_once(int*, void (*)(void)) { return 0; }
int wcscmp(const wchar_t* a, const wchar_t* b) { while (*a && *a == *b) { ++a; ++b; } return *a - *b; }
wchar_t* wmemset(wchar_t* s, wchar_t c, size_t n) { auto* p = s; while (n--) *p++ = c; return s; }
wchar_t* __wmemset_chk(wchar_t* s, wchar_t c, size_t n, size_t) { return wmemset(s, c, n); }
wchar_t* __wmemcpy_chk(wchar_t* d, const wchar_t* s, size_t n, size_t) { return wmemcpy(d, s, n); }
void* iconv_open(const char*, const char*) { return (void*)-1; }
size_t iconv(void*, char**, size_t*, char**, size_t*) { return (size_t)-1; }
int iconv_close(void*) { return 0; }
typedef uint32_t wint_t;
unsigned long __wctype_l(const char*, void*) { return 0; }
wint_t __towupper_l(wint_t c, void*) { return c; }
wint_t __towlower_l(wint_t c, void*) { return c; }
int __iswctype_l(wint_t, unsigned long, void*) { return 0; }
int wctob(wint_t c) { return c < 128 ? (int)c : -1; }
wint_t btowc(int c) { return c >= 0 && c < 128 ? (wint_t)c : (wint_t)-1; }
// The unwinder (libgcc_eh) is linked for the command surface's exceptions; it
// finds frames through __register_frame_info, never through a dynamic loader.
}

void* operator new(size_t n) { return malloc(n); }
void* operator new[](size_t n) { return malloc(n); }
void* operator new(size_t n, const std::nothrow_t&) noexcept { return malloc(n); }
void* operator new(size_t n, std::align_val_t a) { size_t al = (size_t)a; if (al <= 16) return malloc(n); void* p = malloc(n + al); uintptr_t q = ((uintptr_t)p + al) & ~(al - 1); ((void**)q)[-1] = p; return (void*)q; }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }
void operator delete(void* p, std::align_val_t a) noexcept { if ((size_t)a <= 16) free(p); else free(((void**)p)[-1]); }
void operator delete(void* p, size_t, std::align_val_t a) noexcept { if ((size_t)a <= 16) free(p); else free(((void**)p)[-1]); }

// The standard library's throw points. The core is compiled without
// exceptions; a container that would throw is a kernel invariant failure.
namespace std {
[[noreturn]] void __throw_bad_alloc() { metal_panic("bad_alloc"); }
[[noreturn]] void __throw_bad_array_new_length() { metal_panic("bad_array_new_length"); }
[[noreturn]] void __throw_bad_function_call() { metal_panic("bad_function_call"); }
[[noreturn]] void __throw_length_error(const char* w) { metal_panic(w); }
[[noreturn]] void __throw_logic_error(const char* w) { metal_panic(w); }
[[noreturn]] void __throw_out_of_range(const char* w) { metal_panic(w); }
[[noreturn]] void __throw_out_of_range_fmt(const char* w, ...) { metal_panic(w); }
[[noreturn]] void __throw_invalid_argument(const char* w) { metal_panic(w); }
[[noreturn]] void __throw_bad_cast() { metal_panic("bad_cast"); }
[[noreturn]] void __throw_runtime_error(const char* w) { metal_panic(w); }
[[noreturn]] void __throw_domain_error(const char* w) { metal_panic(w); }
[[noreturn]] void __throw_overflow_error(const char* w) { metal_panic(w); }
[[noreturn]] void __throw_underflow_error(const char* w) { metal_panic(w); }
}

struct tm { int tm_sec; int tm_min; int tm_hour; int tm_mday; int tm_mon; int tm_year; int tm_wday; int tm_yday; int tm_isdst; };
struct timespec { long tv_sec; long tv_nsec; };
extern "C" {
// The wall clock is the CMOS real-time clock read at boot plus the time since
// (metal/hal_metal.cpp); monotonic time is the TSC once it is calibrated and
// the PIT tick before that. std::chrono's clocks bottom out in clock_gettime.
#include "time_common.inc"
uint64_t metal_ticks();
uint32_t metal_tick_hz();
size_t __mbsrtowcs_chk(wchar_t*, const char**, size_t, void*, size_t) { return (size_t)-1; }
}

// ---- libm: what the core actually uses (expf, log2, sqrt) and their kin -----------
extern "C" {
double sqrt(double x) { double r; asm("sqrtsd %1, %0" : "=x"(r) : "x"(x)); return r; }
float sqrtf(float x) { float r; asm("sqrtss %1, %0" : "=x"(r) : "x"(x)); return r; }
#include "libm_portable.inc"
// ctype, ASCII only — there is no locale on this substrate and nothing here
// wants one. The web engine's cascade folds case with these, and until
// something else in the image referenced that object --gc-sections dropped it
// and the missing symbol never showed up at link time.
int tolower(int c) { return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c; }
int toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c; }
int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isprint(int c) { return c >= 32 && c < 127; }
int isxdigit(int c) { return isdigit(c) || ((c | 32) >= 'a' && (c | 32) <= 'f'); }
// rand, because the face language offers a random() to the scenes a person
// describes. xorshift64*, seeded to a fixed non-zero constant: this machine has
// no entropy source at this point in boot, and a generator that silently
// returned 0 forever would be worse than one that is openly deterministic.
// srand replaces the seed for anyone who has something better to offer.
namespace { unsigned long long g_rand_state = 0x853C49E6748FEA9Bull; }
int rand(void) {
  g_rand_state ^= g_rand_state >> 12;
  g_rand_state ^= g_rand_state << 25;
  g_rand_state ^= g_rand_state >> 27;
  return (int)((g_rand_state * 0x2545F4914F6CDD1Dull) >> 33);   // [0, 2^31)
}
void srand(unsigned int seed) { g_rand_state = seed ? (unsigned long long)seed : 0x853C49E6748FEA9Bull; }

// Full libc/pthread/POSIX shims for bare metal x86
static int g_errno_val = 0;
int* __errno_location(void) { return &g_errno_val; }
char* strcpy(char* d, const char* s) { char* r = d; while ((*d++ = *s++)) {} return r; }
unsigned long __getauxval(unsigned long) { return 0; }

int ungetc(int, void*) { return -1; }
int fseeko64(void*, int64_t, int) { return -1; }
int64_t ftello64(void*) { return -1; }
int getwc(void*) { return -1; }
int ungetwc(int, void*) { return -1; }
int putwc(int, void*) { return -1; }
int putc(int, void*) { return -1; }
size_t fread(void*, size_t, size_t, void*) { return 0; }
int unlink(const char*) { return -1; }
int rename(const char*, const char*) { return -1; }
int mkdir(const char*, unsigned int) { return -1; }
void* fdopen(int, const char*) { return nullptr; }
int setvbuf(void*, char*, int, size_t) { return 0; }
void* fopen64(const char*, const char*) { return nullptr; }
// No stdio files on bare metal: an open fails, so callers take their no-file path.
void* fopen(const char*, const char*) { return nullptr; }
void* popen(const char*, const char*) { return nullptr; }   // no host to run a command on; execute_host_command reports it
int pclose(void*) { return -1; }
int ferror(void*) { return 1; }
int fileno(void*) { return -1; }
int fclose(void*) { return 0; }
long read(int, void*, size_t) { return -1; }
long write(int, const void*, size_t) { return -1; }
long writev(int, const void*, int) { return -1; }
int64_t lseek64(int, int64_t, int) { return -1; }
int ioctl(int, unsigned long, ...) { return -1; }
int poll(void*, unsigned long, int) { return 0; }
int fstat64(int, void*) { return -1; }
int getc(void*) { return -1; }
void* stdout = nullptr;
void* stdin = nullptr;

// Time and thread execution
int nanosleep(const struct timespec* req, struct timespec* rem) {
  const uint64_t us = req ? (uint64_t)req->tv_sec * 1000000ull + (uint64_t)req->tv_nsec / 1000ull : 0;
  if (us) {
    uint32_t hz = metal_tick_hz(); if (!hz) hz = 100;
    uint64_t ticks_needed = (us * hz + 999999ull) / 1000000ull;
    uint64_t start = metal_ticks();
    while (metal_ticks() - start < ticks_needed) asm volatile("pause");
  }
  if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
  return 0;
}

int get_nprocs(void) { return 4; }
long sysconf(int name) {
  if (name == 30 /* _SC_PAGESIZE */) return 4096;
  if (name == 83 /* _SC_NPROCESSORS_CONF */ || name == 84 /* _SC_NPROCESSORS_ONLN */) return 4;
  if (name == 2 /* _SC_CLK_TCK */) return 100;
  return -1;
}

static uint64_t rdtsc_val() {
  uint32_t lo, hi;
  asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}

int getentropy(void* buf, size_t n) {
  auto* p = (uint8_t*)buf;
  uint64_t x = rdtsc_val() ^ 0x9E3779B97F4A7C15ull;
  for (size_t i = 0; i < n; ++i) {
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    p[i] = (uint8_t)x;
  }
  return 0;
}
uint32_t arc4random(void) {
  uint32_t v; (void)getentropy(&v, sizeof v); return v;
}

char* setlocale(int, const char*) { static char c[] = "C"; return c; }
char* bindtextdomain(const char*, const char*) { return nullptr; }
size_t mbsrtowcs(wchar_t* dst, const char** src, size_t len, void*) {
  if (!src || !*src) return 0;
  const char* s = *src;
  size_t n = 0;
  while (s[n] && (!dst || n < len)) { if (dst) dst[n] = (wchar_t)(unsigned char)s[n]; ++n; }
  if (dst && n < len) dst[n] = 0;
  *src = s[n] ? s + n : nullptr;
  return n;
}
int wmemcmp(const wchar_t* a, const wchar_t* b, size_t n) {
  for (size_t i = 0; i < n; ++i) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
  return 0;
}

int atexit(void (*)()) { return 0; }
int malloc_trim(size_t) { return 0; }
int __fprintf_chk(FILE*, int, const char* fmt, ...) {
  return fmt ? (int)strlen(fmt) : 0;
}

// Memory mapping (anonymous heap backed)
#ifndef MAP_FAILED
#define MAP_FAILED ((void*)-1)
#endif
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS 0x20
#endif

void* mmap(void*, size_t length, int, int flags, int fd, long) {
  if (!(flags & MAP_ANONYMOUS) || fd != -1) { g_errno_val = 19 /* ENODEV */; return MAP_FAILED; }
  void* p = malloc(length);
  if (!p) { g_errno_val = 12 /* ENOMEM */; return MAP_FAILED; }
  memset(p, 0, length);
  return p;
}
int munmap(void* addr, size_t) {
  free(addr);
  return 0;
}

// File / directory / shm stubs
int open(const char*, int, ...) { g_errno_val = 2 /* ENOENT */; return -1; }
int openat(int, const char*, int, ...) { g_errno_val = 2 /* ENOENT */; return -1; }
// The fortified forms libstdc++'s own objects call (random_device reads,
// filesystem::directory_iterator opens) - the x86 image stopped linking when
// the SQL engine and grain's shell began using them.
long __read_chk(int fd, void* buf, size_t n, size_t) { return read(fd, buf, n); }
int __openat_2(int fd, const char* path, int flags) { return openat(fd, path, flags); }
int close(int) { return 0; }
int ftruncate(int, long) { g_errno_val = 9 /* EBADF */; return -1; }
int truncate(const char*, long) { g_errno_val = 2 /* ENOENT */; return -1; }
void* opendir(const char*) { g_errno_val = 2 /* ENOENT */; return nullptr; }
void* fdopendir(int) { g_errno_val = 9 /* EBADF */; return nullptr; }
int dirfd(void*) { return -1; }
void* readdir(void*) { return nullptr; }
int closedir(void*) { return 0; }
int __lxstat(int, const char*, void*) { g_errno_val = 2 /* ENOENT */; return -1; }
int lstat(const char* path, void* buf) { return __lxstat(1, path, buf); }
int stat(const char* path, void* buf) { return __lxstat(1, path, buf); }
int __xstat(int, const char* path, void* buf) { return __lxstat(1, path, buf); }
int remove(const char*) { g_errno_val = 2 /* ENOENT */; return -1; }
int rmdir(const char*) { g_errno_val = 2 /* ENOENT */; return -1; }
int access(const char*, int) { g_errno_val = 2 /* ENOENT */; return -1; }
int chmod(const char*, unsigned) { g_errno_val = 2 /* ENOENT */; return -1; }
int symlink(const char*, const char*) { g_errno_val = 30 /* EROFS */; return -1; }
long readlink(const char*, char*, size_t) { g_errno_val = 2 /* ENOENT */; return -1; }
int isatty(int fd) { return (fd == 0 || fd == 1 || fd == 2) ? 1 : 0; }
int fcntl(int, int, ...) { return 0; }
int shm_open(const char*, int, unsigned) { g_errno_val = 38 /* ENOSYS */; return -1; }
int shm_unlink(const char*) { g_errno_val = 38 /* ENOSYS */; return -1; }

// Threads & synchronization
int pthread_create(unsigned long* out, const void*, void* (*start)(void*), void* arg) {
  void* result = start(arg);
  if (out) *out = (unsigned long)result;
  return 0;
}
int pthread_join(unsigned long t, void** ret) {
  if (ret) *ret = (void*)t;
  return 0;
}
int pthread_detach(unsigned long) { return 0; }
int pthread_mutex_trylock(void*) { return 0; }

int pthread_rwlock_rdlock(void* rw) {
  auto* w = (volatile int*)rw;
  for (;;) {
    int cur = __atomic_load_n(w, __ATOMIC_RELAXED);
    if (cur >= 0 && __atomic_compare_exchange_n(w, &cur, cur + 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 0;
    asm volatile("pause");
  }
}
int pthread_rwlock_wrlock(void* rw) {
  auto* w = (volatile int*)rw;
  int expect;
  do { expect = 0; } while (!__atomic_compare_exchange_n(w, &expect, -1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED));
  return 0;
}
int pthread_rwlock_unlock(void* rw) {
  auto* w = (volatile int*)rw;
  int cur = __atomic_load_n(w, __ATOMIC_RELAXED);
  __atomic_store_n(w, cur > 0 ? cur - 1 : 0, __ATOMIC_RELEASE);
  return 0;
}

int pthread_cond_wait(void* c, void* m) {
  auto* gen = (volatile uint32_t*)c;
  uint32_t seen = __atomic_load_n(gen, __ATOMIC_ACQUIRE);
  pthread_mutex_unlock(m);
  while (__atomic_load_n(gen, __ATOMIC_ACQUIRE) == seen) asm volatile("pause");
  pthread_mutex_lock(m);
  return 0;
}
int pthread_cond_signal(void* c) {
  __atomic_add_fetch((volatile uint32_t*)c, 1, __ATOMIC_RELEASE);
  return 0;
}
int pthread_cond_broadcast(void* c) { return pthread_cond_signal(c); }
int pthread_cond_destroy(void*) { return 0; }

// Thread-local storage
namespace {
constexpr int MAX_KEYS = 32;
constexpr int MAX_CORES = 16;
void* g_tls[MAX_CORES][MAX_KEYS] = {};
int g_next_key = 0;
inline int this_core() {
  uint32_t ebx = 0;
  asm volatile("cpuid" : "=b"(ebx) : "a"(1) : "rcx", "rdx");
  return (int)((ebx >> 24) % MAX_CORES);
}
}

int pthread_key_create(unsigned int* key, void (*)(void*)) {
  if (g_next_key >= MAX_KEYS) return 11 /* EAGAIN */;
  if (key) *key = (unsigned int)(g_next_key++);
  return 0;
}
int pthread_key_delete(unsigned int) { return 0; }
void* pthread_getspecific(unsigned int key) {
  return key < MAX_KEYS ? g_tls[this_core()][key] : nullptr;
}
int pthread_setspecific(unsigned int key, const void* value) {
  if (key >= MAX_KEYS) return 22 /* EINVAL */;
  g_tls[this_core()][key] = const_cast<void*>(value);
  return 0;
}

}
