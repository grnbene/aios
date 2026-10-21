// hal sync primitives shared by the x86-64 and AArch64 bare-metal kernels.
// Included inside namespace aios::hal by metal/hal_metal.cpp and
// metal/arm64/hal_metal_arm64.cpp after they define AIOS_CPU_RELAX() (the
// core's spin hint: x86 `pause`, AArch64 `yield`) and Mutex itself (a ticket
// lock: next ticket at storage_[0], now serving at storage_[4]).
//
// A thread on bare metal runs on an idle core, or inline on the caller when
// none is free; these spin rather than sleep, which is correct in both cases.

bool Mutex::try_lock() {
  auto* next = reinterpret_cast<volatile uint32_t*>(storage_);
  auto* serving = reinterpret_cast<volatile uint32_t*>(storage_ + 4);
  uint32_t now = __atomic_load_n(serving, __ATOMIC_ACQUIRE);
  uint32_t expected = now;
  return __atomic_compare_exchange_n(next, &expected, now + 1, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
}

// Owner is the core that took it (metal_cpu_token()), with a depth count.
struct MetalRecursive { Mutex m; volatile uint64_t owner = ~0ull; uint32_t depth = 0; };
RecursiveMutex::RecursiveMutex() : impl_(new MetalRecursive) {}
RecursiveMutex::~RecursiveMutex() { delete static_cast<MetalRecursive*>(impl_); }
void RecursiveMutex::lock() {
  auto* r = static_cast<MetalRecursive*>(impl_);
  const uint64_t me = metal_cpu_token();
  if (__atomic_load_n(&r->owner, __ATOMIC_ACQUIRE) == me) { r->depth++; return; }
  r->m.lock();
  __atomic_store_n(&r->owner, me, __ATOMIC_RELEASE);
  r->depth = 1;
}
bool RecursiveMutex::try_lock() {
  auto* r = static_cast<MetalRecursive*>(impl_);
  const uint64_t me = metal_cpu_token();
  if (__atomic_load_n(&r->owner, __ATOMIC_ACQUIRE) == me) { r->depth++; return true; }
  if (!r->m.try_lock()) return false;
  __atomic_store_n(&r->owner, me, __ATOMIC_RELEASE);
  r->depth = 1;
  return true;
}
void RecursiveMutex::unlock() {
  auto* r = static_cast<MetalRecursive*>(impl_);
  if (--r->depth == 0) { __atomic_store_n(&r->owner, ~0ull, __ATOMIC_RELEASE); r->m.unlock(); }
}

// state: -1 = a writer holds it, 0 = free, n > 0 = n readers.
struct MetalShared { volatile int32_t state = 0; };
SharedMutex::SharedMutex() : impl_(new MetalShared) {}
SharedMutex::~SharedMutex() { delete static_cast<MetalShared*>(impl_); }
bool SharedMutex::try_lock() {
  int32_t expected = 0;
  return __atomic_compare_exchange_n(&static_cast<MetalShared*>(impl_)->state, &expected, -1, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
}
void SharedMutex::lock() { while (!try_lock()) AIOS_CPU_RELAX(); }
void SharedMutex::unlock() { __atomic_store_n(&static_cast<MetalShared*>(impl_)->state, 0, __ATOMIC_RELEASE); }
bool SharedMutex::try_lock_shared() {
  auto* s = &static_cast<MetalShared*>(impl_)->state;
  int32_t cur = __atomic_load_n(s, __ATOMIC_ACQUIRE);
  return cur >= 0 && __atomic_compare_exchange_n(s, &cur, cur + 1, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
}
void SharedMutex::lock_shared() { while (!try_lock_shared()) AIOS_CPU_RELAX(); }
void SharedMutex::unlock_shared() { __atomic_fetch_sub(&static_cast<MetalShared*>(impl_)->state, 1, __ATOMIC_RELEASE); }

// A generation count: a waiter drops the lock, spins until the count moves,
// and takes the lock back. A notify that lands between the drop and the spin
// is not lost, because the waiter read the generation while it held the lock.
struct MetalCond { volatile uint64_t gen = 0; };
CondVar::CondVar() : impl_(new MetalCond) {}
CondVar::~CondVar() { delete static_cast<MetalCond*>(impl_); }
void CondVar::notify_one() { __atomic_fetch_add(&static_cast<MetalCond*>(impl_)->gen, 1, __ATOMIC_RELEASE); }
void CondVar::notify_all() { __atomic_fetch_add(&static_cast<MetalCond*>(impl_)->gen, 1, __ATOMIC_RELEASE); }
void CondVar::wait_raw(Mutex& m) {
  auto* g = &static_cast<MetalCond*>(impl_)->gen;
  const uint64_t seen = __atomic_load_n(g, __ATOMIC_ACQUIRE);
  m.unlock();
  while (__atomic_load_n(g, __ATOMIC_ACQUIRE) == seen) AIOS_CPU_RELAX();
  m.lock();
}
bool CondVar::wait_raw_ms(Mutex& m, uint32_t ms) {
  auto* g = &static_cast<MetalCond*>(impl_)->gen;
  const uint64_t seen = __atomic_load_n(g, __ATOMIC_ACQUIRE);
  m.unlock();
  bool moved = false;
  for (uint32_t t = 0; t <= ms && !moved; ++t) {
    moved = __atomic_load_n(g, __ATOMIC_ACQUIRE) != seen;
    if (!moved && t < ms) sleep_ms(1);
  }
  m.lock();
  return moved;
}

void yield() { AIOS_CPU_RELAX(); }
