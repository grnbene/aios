// Bare-metal entry after boot.S: bring up the substrate, boot the kernel, then
// either run the acceptance sequence (boot menu entry "acceptance") or offer
// the command surface on the console (serial and display/keyboard).
#include <algorithm>
#include <cstring>
#include "aios/kernel/kernel.hpp"
#include "aios/net/inet.hpp"
#include "aios/channel/btc_p2p.hpp"
#include "aios/hal/volume.hpp"
#include "aios/hal/fabric.hpp"
#include "aios/userland/compositor.hpp"
#include "aios/metal/process.hpp"
#include "aios/metal/trap.hpp"
#include "aios/userland/elf.hpp"
#include "aios/intelligence/world_model.hpp"
#include "../cli/cli.hpp"
#include <cstring>
#include <cstdlib>

extern "C" uint64_t metal_ticks();
extern "C" uint32_t metal_tick_hz();
extern "C" uint64_t metal_switches();
extern "C" uint64_t metal_task_ticks_total();
extern "C" void metal_set_scheduler_slice(uint32_t ticks);
extern "C" void metal_set_display_scene(uint32_t scene);
extern "C" uint32_t metal_ready_tasks();
extern "C" uint32_t metal_scheduler_slice();
extern "C" size_t metal_heap_used();
extern "C" size_t metal_heap_total();
extern "C" float metal_nic_rx_depth();
extern "C" bool metal_set_nic_prefetch(bool on);
extern "C" bool metal_nic_prefetch();
extern "C" uint32_t metal_nic_rx_window();
extern "C" uint32_t metal_nic_rx_posted();
extern "C" uint32_t metal_nic_rx_slots();
extern "C" uint32_t metal_nic_rx_max_batch();
extern "C" uint64_t metal_nic_rx_kicks();
extern "C" uint64_t metal_nic_rx_buffers_posted();
extern "C" uint64_t metal_nic_rx_starved();
extern "C" uint64_t metal_nic_rx_behind_mean();
extern "C" uint32_t metal_nic_rx_behind_max();
extern "C" uint64_t metal_nic_rx_window_changes();
extern "C" bool metal_nic_present();
extern "C" int metal_ap_participants();

namespace {
// ---- the machine's own instruments, handed to the world model --------------
// Every field here is read from a counter this kernel maintains. Rates are
// deltas between calls, so a value means "since you last looked", which is what
// a world model wants. Nothing here is a constant standing in for a device.
aios::intelligence::SubstrateTelemetry metal_telemetry() {
  static uint64_t last_ticks = 0, last_switches = 0, last_task_ticks = 0;
  aios::intelligence::SubstrateTelemetry t;

  uint64_t ticks = metal_ticks();
  uint64_t switches = metal_switches();
  uint64_t task_ticks = metal_task_ticks_total();
  uint64_t d_ticks = ticks - last_ticks;

  if (d_ticks) {
    float busy = (float)(task_ticks - last_task_ticks) / (float)d_ticks;
    t.cpu_load = busy > 1.0f ? 1.0f : busy;
    // Normalised against FOUR switches per tick, not one.
    //
    // One was what this scheduler did before it had a quantum at all, and it
    // saturates: this machine reschedules at least once per tick just idling, so
    // the ratio was clamped to 1.0 at every slice from 1 to 16 and state
    // dimension 2 could not see the lever at all. `jepa prove` on the same boot
    // measures the kernel's own counter going 359 -> 24 over the same workload,
    // and `jepa sweep` showed dimension 2 reading exactly 1 for both — so the
    // reward the policy is trained against was a constant, and the metal
    // learning run correctly reported a return that did not improve.
    //
    // Four gives the range headroom above the idle baseline. The raw rate is
    // reported beside it so the ceiling can be checked rather than trusted.
    float rate = (float)(switches - last_switches) / (float)d_ticks;
    t.ctx_switch_raw = rate;
    t.switch_events = switches - last_switches;
    t.interval_ticks = d_ticks;
    rate /= 4.0f;
    t.ctx_switch_rate = rate > 1.0f ? 1.0f : rate;
  }
  last_ticks = ticks; last_switches = switches; last_task_ticks = task_ticks;

  t.scheduler_slice_now = metal_scheduler_slice();
  t.nic_ring_depth = metal_nic_rx_depth();
  // Packet loss, on the one substrate where this machine owns the receive ring.
  // The driver counts every time the window drained — the device had a frame
  // and nowhere to put it — and that IS the loss the refill window causes and
  // the refill window can cure. Rate since the last read, over the buffers that
  // were successfully posted in the same interval.
  {
    static uint64_t last_bad = 0, last_posted = 0;
    // The window draining under our own nose is the only loss this driver can
    // OBSERVE. Frames the device dropped because it had no buffer are reported
    // only through virtio-net's control-queue statistics, which this driver does
    // not negotiate — so they are not in this number and `host jepa nic` says
    // as much. An instrument that guessed at them would be worse than one that
    // reports what it can see.
    const uint64_t bad = metal_nic_rx_starved();
    const uint64_t posted = metal_nic_rx_buffers_posted();
    const uint64_t d_bad = bad - last_bad;
    const uint64_t d_posted = posted - last_posted;
    last_bad = bad; last_posted = posted;
    if (d_bad + d_posted) {
      const float loss = (float)((double)d_bad / (double)(d_bad + d_posted));
      t.packet_loss = loss > 1.0f ? 1.0f : loss;
    }
  }
  // Runnable work per processor: the scheduler's own ready count over the cores
  // that joined. This machine knows both exactly.
  {
    const int cores = metal_ap_participants() > 0 ? metal_ap_participants() : 1;
    const float per = (float)metal_ready_tasks() / (float)cores;
    t.load_imbalance = per > 4.0f ? 1.0f : per / 4.0f;
  }
  size_t total = metal_heap_total();
  t.mem_pressure = total ? (float)((double)metal_heap_used() / (double)total) : 0.0f;
  int cores = metal_ap_participants();
  t.compute_occupancy = cores > 0 ? t.cpu_load : 0.0f;
  uint32_t hz = metal_tick_hz(); if (!hz) hz = 100;
  t.timestamp_ms = ticks * (1000ull / hz);
  return t;
}

// ---- the lever ------------------------------------------------------------
// The policy head's decoded action reaching the actual scheduler. This is the
// point where a prediction stops being a number and becomes machine behaviour.
bool metal_actuate(const aios::intelligence::SubstrateActuation& a) {
  metal_set_scheduler_slice(a.scheduler_slice_ticks);
  // a_t[2] reaches the compositor directly here: on bare metal the display IS
  // this process, so there is no file to write and no half-second poll — the
  // canvas is told which scene to draw and draws it on the next frame.
  metal_set_display_scene(a.display_scene);
  // a_t[1] reaches the virtio-net receive path: it sets how many rx buffers the
  // driver keeps posted to the device (4 lazy vs the full ring) and therefore
  // how many it refills behind one doorbell. `host jepa nic` reads the driver's
  // own counters back. Returns false with no NIC, and we do not pretend
  // otherwise — the scheduler lever still applied.
  (void)metal_set_nic_prefetch(a.nic_prefetch_enabled);
  return true;
}
}  // namespace

extern "C" void metal_console_init();
extern "C" void metal_console_write(const char* s, size_t n);
extern "C" int metal_console_getc();
extern "C" void metal_vga_clear();
extern "C" void metal_display_init(uint64_t addr, uint32_t pitch, uint32_t width, uint32_t height, uint8_t bpp, uint8_t type);
extern "C" bool metal_display_best_mode(uint32_t* w, uint32_t* h, uint32_t* pitch);
extern "C" bool metal_display_describe(uint32_t* width, uint32_t* height, uint32_t* bpp, uint32_t* text_cols, uint32_t* text_rows);
extern "C" void metal_display_adopt(uint64_t addr, uint32_t pitch, uint32_t width, uint32_t height, void (*present)());
extern "C" bool metal_virtio_gpu_init(uint32_t want_w, uint32_t want_h, uint64_t* base, uint32_t* pitch, uint32_t* w, uint32_t* h);
extern "C" void metal_virtio_gpu_present();
extern "C" bool metal_display_describe(uint32_t* width, uint32_t* height, uint32_t* bpp, uint32_t* text_cols, uint32_t* text_rows);
extern "C" void metal_clock_init();
extern "C" void metal_idt_init();
extern "C" void metal_ps2_irq_enable();
extern "C" uint64_t metal_ticks();
extern "C" uint32_t metal_tick_hz();
// The programs this machine ships with, linked in as the bytes of real ELF
// files and written into its filesystem at boot so that running one goes
// through the same path any other program would.
extern "C" char user_hello_start[], user_hello_end[];
extern "C" char user_probe_start[], user_probe_end[];
extern "C" char user_crash_start[], user_crash_end[];
extern "C" char user_priv_start[], user_priv_end[];
extern "C" char user_peek_start[], user_peek_end[];
extern "C" char user_spin_start[], user_spin_end[];
extern "C" char user_fs_start[], user_fs_end[];
extern "C" char user_mem_start[], user_mem_end[];
extern "C" char user_forker_start[], user_forker_end[];
extern "C" char user_execer_start[], user_execer_end[];
extern "C" char user_sig_start[], user_sig_end[];
extern "C" char user_fpu_start[], user_fpu_end[];
extern "C" char user_edges_start[], user_edges_end[];
extern "C" void metal_set_memory(uint64_t total, uint32_t cpus);
extern "C" int metal_smp_init();
extern "C" void metal_smp_set_rsdp(const void* rsdp);
extern "C" uint64_t metal_tsc_hz();
extern "C" void metal_heap_init(void* base, size_t size);
extern "C" size_t metal_heap_used();
extern "C" char __kernel_end[];
extern "C" char __eh_frame_start[];
extern "C" char __tdata_start[], __tdata_end[], __tbss_start[], __tbss_end[];
extern "C" void* metal_alloc_pages(size_t);
extern "C" [[noreturn]] void metal_panic(const char* what);
// x86-64 static TLS, variant II: FS points at a thread control block whose
// first word is its own address, and the TLS image sits immediately below it,
// the segment's size rounded up to its alignment (a page: link.ld, and some
// input sections ask for one). Built on the
// boot core for each core before it runs anything: the page allocator has no
// lock.
extern "C" void* metal_make_tls_block() {
  const size_t image = (size_t)(__tdata_end - __tdata_start);
  const size_t span = ((size_t)(__tbss_end - __tdata_start) + 4095) & ~(size_t)4095;
  const size_t bytes = (span + 64 + 4095) & ~(size_t)4095;
  auto* block = (uint8_t*)metal_alloc_pages(bytes);   // zeroed: that is .tbss
  if (!block) metal_panic("no memory for a TLS block");
  for (size_t i = 0; i < image; ++i) block[i] = (uint8_t)__tdata_start[i];
  uint8_t* tp = block + span;
  *(uint64_t*)tp = (uint64_t)tp;
  return tp;
}
extern "C" void metal_set_fs_base(void* tp) {
  const uint64_t v = (uint64_t)tp;
  asm volatile("wrmsr" : : "c"(0xC0000100u), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)) : "memory");   // IA32_FS_BASE
}
extern "C" void (*__init_array_start[])();
extern "C" void (*__init_array_end[])();
extern "C" void __register_frame_info(const void*, void*);
int run_acceptance(aios::Kernel& k);
// metal/desktop.cpp: the framebuffer desktop, PS/2 mouse and keyboard included.
aios::Result<void> metal_run_desktop(aios::Kernel& k, aios::userland::CommandDispatcher dispatcher, void (*idle)());
// metal/canvas.cpp: the machine's own lifecycle on the glass — no windows.
#include "aios/userland/canvas.hpp"
using aios::userland::CanvasFeed;
aios::Result<void> metal_run_canvas(aios::Kernel& k, aios::Surface& console, CanvasFeed (*feed)(), void (*idle)());
std::unique_ptr<aios::hal::BlockDevice> metal_probe_virtio_blk();
// The NVM Express driver in metal/nvme.cpp. Null when the bus carries no
// controller, which is the machine saying it has none.
extern "C" aios::hal::BlockDevice* metal_nvme_probe();
std::unique_ptr<aios::hal::FrameDevice> metal_probe_virtio_net();
std::unique_ptr<aios::hal::FrameDevice> metal_probe_e1000();
extern aios::hal::Fabric* metal_fabric;
extern "C" uint32_t metal_rand32();
aios::net::NetStack* metal_net = nullptr;   // AOS's own TCP/IP over the NIC, when brought up
static std::unique_ptr<aios::net::NetStack> g_net;
static std::unique_ptr<aios::hal::FrameDevice> g_nic;
static std::unique_ptr<aios::hal::Fabric> g_fabric;
static aios::Kernel* g_kernel = nullptr;
static uint64_t metal_now_ns() { extern uint64_t metal_tsc_hz(); static uint64_t hz = metal_tsc_hz(); uint32_t lo, hi; asm volatile("rdtsc" : "=a"(lo), "=d"(hi)); uint64_t t = ((uint64_t)hi << 32) | lo; return hz ? (t / (hz / 1000000ull)) * 1000ull : t; }
// Everything that must advance while the console waits for a key.
static aios::cli::Session* g_session = nullptr;

// The recorder needs work to have HAPPENED between applying an action and
// measuring what followed. Two telemetry reads taken microseconds apart on an
// idle machine differ by nothing, so every counter delta — and therefore the
// reward — is 0 regardless of what the policy chose. This runs the same
// workload `jepa prove` uses (two copies of the shipped /bin/spin, linked into
// the kernel image so no filesystem is needed) under whatever slice the action
// just set, and waits for it. The switches the scheduler counts while that runs
// are what the next telemetry read sees.
void metal_settle() {
  if (!g_kernel || !g_session || !g_session->me) return;
  aios::Surface actor = g_session->me->kernel().as(g_session->me->self());
  aios::ByteSpan img((const uint8_t*)user_spin_start, (size_t)(user_spin_end - user_spin_start));
  // Only its OWN two tasks: waiting for and reaping every task would eat the exit
  // status of whatever the user started (`host spawn`, a fork's child) while idle.
  std::vector<int> mine;
  for (int i = 0; i < 2; ++i) { auto pid = aios::metal::process_spawn("/bin/spin", img, actor, /*quiet=*/true); if (pid.ok()) mine.push_back(*pid); }
  (void)aios::metal::process_wait(mine);
  aios::metal::process_reap(mine);
}
// Everything that must advance while the console waits for a key — including
// the standing directives, which is what makes this machine do things nobody is
// typing. The console is a task now, so this really is idle time.
// What only the substrate knows, for the canvas: the clock, the scheduler, the
// page pool and the wire.
static CanvasFeed canvas_feed() {
  CanvasFeed f;
  f.ticks = metal_ticks();
  f.tick_hz = metal_tick_hz();
  aios::metal::process_memory(&f.pages_total, &f.pages_live, &f.switches);
  f.heap_kib = (uint64_t)(metal_heap_used() >> 10);
  // Whichever wire is carrying traffic: the fabric when it is raw Ethernet, the
  // machine's own TCP/IP once it is up.
  f.net_up = g_net && g_net->up();
  if (f.net_up) { f.rx_frames = g_net->stats().rx_frames; f.tx_frames = g_net->stats().tx_frames; }
  else if (g_fabric) { f.rx_frames = g_fabric->stats().frames_in; f.tx_frames = g_fabric->stats().frames_out; }
  auto console_task = aios::metal::process_console();
  f.tasks.push_back("0     console        running  " + std::to_string(console_task.ticks) + " ticks");
  for (const auto& p : aios::metal::process_list())
    f.tasks.push_back(std::to_string(p.pid) + "     " + p.name.substr(0, 14) + "  " + p.state + "  " +
                      std::to_string(p.ticks) + " ticks");
  return f;
}

// The machine learning about itself, while it has nothing else to do.
//
// This is the only place it may run. idle_pump is reached when the console has
// no key waiting and nothing else is pending, so a learning iteration never
// takes the CPU from work somebody asked for — which matters more here than it
// looks: the policy's reward is computed from this machine's own context-switch
// rate and run-queue length, so a learner that ran alongside real work would be
// scoring its own interference as though the policy had caused it. The gradient
// step also falls OUTSIDE metal_settle's measurement window (see learn_once),
// so the part that costs the most is not the part being measured.
//
// Paced, not run flat out. An iteration spawns two /bin/spin processes and
// waits for them; doing that on every idle pass would mean the machine is never
// actually idle, and the baseline the policy learns against would be a machine
// permanently busy learning.
static bool g_jepa_learning = true;
static uint64_t g_jepa_iters = 0;
static uint64_t g_jepa_refusals = 0;
static float g_jepa_last_reward = 0.0f;
static constexpr uint64_t kLearnIntervalTicks = 200;

static void jepa_learn_pump() {
  if (!g_jepa_learning) return;
  // A program somebody started is running: that is not idle. An iteration
  // here blocks the console until its own two spins finish (seconds under
  // emulation) while the person's program runs on unwatched, and its reward
  // would score that program's load as the policy's doing. Wait for quiet.
  if (aios::metal::process_any_live()) return;
  static uint64_t next_at = 0;
  const uint64_t now = metal_ticks();
  if (now < next_at) return;
  next_at = now + kLearnIntervalTicks;
  const auto st = aios::intelligence::learn_once(64);
  if (st.ran) { ++g_jepa_iters; g_jepa_last_reward = st.reward; }
  else {
    // Refusing forever is a fact about this machine, not a transient: stop
    // asking rather than burning an idle pass on the same refusal.
    ++g_jepa_refusals;
    if (g_jepa_refusals > 3) g_jepa_learning = false;
  }
}

static void idle_pump() {
  if (g_net && g_net->up()) g_net->poll(); else if (g_fabric) g_fabric->pump();
  if (g_kernel && g_kernel->service()) g_kernel->service()->poll();
  if (g_session) aios::cli::run_due_directives(*g_session);
  jepa_learn_pump();
}
aios::Result<void> metal_adopt_volume();
extern aios::hal::Volume* metal_volume;
extern aios::hal::BlockDevice* metal_disk;
static std::unique_ptr<aios::hal::BlockDevice> g_disk;       // the whole disk
static std::unique_ptr<aios::hal::BlockDevice> g_part;       // the AOS partition on it, if the disk is partitioned
static aios::hal::Volume g_volume;
static aios::hal::BlockDevice* g_volume_dev = nullptr;        // where the volume lives (partition or whole disk)

// A block device that is a range of another: the AOS partition of a shipped disk image.
class PartitionDevice final : public aios::hal::BlockDevice {
 public:
  PartitionDevice(aios::hal::BlockDevice& base, uint64_t first, uint64_t count, std::string name) : base_(base), first_(first), count_(count), name_(std::move(name)) {}
  const std::string& name() const override { return name_; }
  uint64_t sectors() const override { return count_; }
  aios::Result<void> read(uint64_t lba, uint32_t n, uint8_t* out) override { if (lba + n > count_) return aios::Result<void>(aios::Status::Invalid, "past end of partition"); return base_.read(first_ + lba, n, out); }
  aios::Result<void> write(uint64_t lba, uint32_t n, const uint8_t* d) override { if (lba + n > count_) return aios::Result<void>(aios::Status::Invalid, "past end of partition"); return base_.write(first_ + lba, n, d); }
  aios::Result<void> flush() override { return base_.flush(); }
 private:
  aios::hal::BlockDevice& base_; uint64_t first_, count_; std::string name_;
};
constexpr uint8_t AOS_PARTITION_TYPE = 0x7F;   // MBR type byte claimed for AOS volumes

// Finds an AOS partition (type 0x7F) in the disk's MBR; null when the disk is unpartitioned or has none.
static std::unique_ptr<aios::hal::BlockDevice> find_aos_partition(aios::hal::BlockDevice& disk) {
  uint8_t mbr[512];
  if (!disk.read(0, 1, mbr).ok() || mbr[510] != 0x55 || mbr[511] != 0xAA) return nullptr;
  for (int i = 0; i < 4; ++i) {
    const uint8_t* e = mbr + 0x1BE + 16 * i;
    if (e[4] != AOS_PARTITION_TYPE) continue;
    uint32_t first = (uint32_t)e[8] | ((uint32_t)e[9] << 8) | ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
    uint32_t count = (uint32_t)e[12] | ((uint32_t)e[13] << 8) | ((uint32_t)e[14] << 16) | ((uint32_t)e[15] << 24);
    if (count == 0 || (uint64_t)first + count > disk.sectors()) continue;
    return std::make_unique<PartitionDevice>(disk, first, count, disk.name() + "p" + std::to_string(i + 1));
  }
  return nullptr;
}

// What a disk with no AOS partition and no AOS volume already carries, in words;
// empty when it is blank (every byte of its first MiB and its last 34 sectors is
// zero — where a partition table, a GPT and its backup, a boot record and the
// superblock of any common filesystem live). Formatting a blank disk destroys
// nothing; formatting one that answers here wipes someone's data.
static std::string disk_contents(aios::hal::BlockDevice& disk) {
  uint8_t buf[64 * 512];
  const uint64_t n = disk.sectors();
  auto nonzero = [&](uint64_t lba, uint32_t count) -> int {   // -1 unreadable, 1 data, 0 blank
    if (!disk.read(lba, count, buf).ok()) return -1;
    for (uint32_t i = 0; i < count * 512; ++i) if (buf[i]) return 1;
    return 0;
  };
  if (!disk.read(0, 2, buf).ok()) return "an unreadable first sector";
  if (memcmp(buf + 512, "EFI PART", 8) == 0) return "a GPT partition table";
  if (buf[510] == 0x55 && buf[511] == 0xAA) {
    std::string types;
    for (int i = 0; i < 4; ++i) { uint8_t t = buf[0x1BE + 16 * i + 4]; if (t) { static const char* hx = "0123456789abcdef"; types += std::string(types.empty() ? "" : ",") + "0x" + hx[t >> 4] + hx[t & 15]; } }
    return types.empty() ? "a boot record" : "an MBR partition table (types " + types + ")";
  }
  for (uint64_t lba = 0; lba < 2048 && lba < n; lba += 64) {
    int r = nonzero(lba, (uint32_t)std::min<uint64_t>(64, n - lba));
    if (r) return r < 0 ? "unreadable sectors" : "data in its first MiB";
  }
  if (n > 2048) { uint64_t tail = n >= 34 ? n - 34 : 0; int r = nonzero(tail, (uint32_t)(n - tail)); if (r) return r < 0 ? "unreadable sectors" : "data in its last sectors"; }
  return "";
}

using namespace aios;

static void out(const std::string& s) { metal_console_write(s.data(), s.size()); }
// An address, written the way a machine writes one (the console's Out is not a
// std::ostream, so the manipulators do not apply to it).
static std::string aios_hex(uint64_t v) {
  static const char* d = "0123456789abcdef";
  std::string s = "0x";
  bool lead = true;
  for (int i = 60; i >= 0; i -= 4) {
    uint8_t nib = (uint8_t)((v >> i) & 0xF);
    if (nib || !lead || i == 0) { s += d[nib]; lead = false; }
  }
  return s;
}
static void outc(const char* s) { metal_console_write(s, strlen(s)); }   // before the heap exists
[[noreturn]] static void qemu_exit(uint8_t code) { asm volatile("outb %0, %1" : : "a"(code), "Nd"((uint16_t)0xF4)); for (;;) asm volatile("hlt"); }
extern "C" [[noreturn]] void metal_panic(const char* what) { outc("\n[metal] PANIC: "); outc(what); outc("\n"); qemu_exit(0x20); }   // must not allocate
[[noreturn]] static void reboot() { out("[metal] rebooting\n"); for (int i = 0; i < 100000; ++i) asm volatile("pause"); asm volatile("outb %0, %1" : : "a"((uint8_t)0xFE), "Nd"((uint16_t)0x64)); for (;;) asm volatile("hlt"); }
[[noreturn]] static void poweroff() {
  out("[metal] powering off\n");
  // isa-debug-exit port (QEMU immediate clean exit)
  asm volatile("outb %0, %1" : : "a"((uint8_t)0), "Nd"((uint16_t)0xF4));
  // ACPI S5 poweroff (PIIX4 / Bochs / Q35)
  asm volatile("outw %0, %1" : : "a"((uint16_t)0x2000), "Nd"((uint16_t)0x604));
  asm volatile("outw %0, %1" : : "a"((uint16_t)0x2000), "Nd"((uint16_t)0xB004));
  asm volatile("outw %0, %1" : : "a"((uint16_t)0x3400), "Nd"((uint16_t)0x4004));
  asm volatile("outw %0, %1" : : "a"((uint16_t)0x3C00), "Nd"((uint16_t)0x604));
  for (;;) asm volatile("hlt");
}

struct __attribute__((packed)) MbTag { uint32_t type, size; };
struct __attribute__((packed)) MbMmapEntry { uint64_t base, len; uint32_t type, zero; };
static char eh_object[256];

// Commands typed at the desktop run on the SAME command surface the serial
// console uses: one session, one console entity, one set of capabilities. Its
// output is captured into the window instead of being written to the screen the
// desktop is composited onto — and stripped of the colour escapes the command
// surface emits, which mean nothing to a bitmap font.
static aios::userland::CommandDispatcher desktop_dispatcher(cli::Session& s) {
  return [&s](const std::string& cmd, aios::userland::OutputCallback emit) {
    if (cmd == "exit" || cmd == "quit" || cmd == "poweroff" || cmd == "shutdown" ||
        cmd == "host poweroff" || cmd == "host exit" || cmd == "host shutdown") {
      poweroff();
    }
    std::string acc;
    {
      cli::Out captured([&acc](const std::string& t) { acc += t; });
      captured.set_colour(false);
      cli::Out* prev = s.out;
      s.out = &captured;
      auto r = cli::run_line(s, cmd);
      captured.flush();
      s.out = prev;
      if (!r.ok()) acc += "error: " + r.error().str() + "\n";
    }
    std::string line;
    for (size_t i = 0; i < acc.size(); ++i) {
      char c = acc[i];
      if (c == 27) {   // CSI ... final byte: an escape sequence, not text
        ++i;
        if (i < acc.size() && acc[i] == '[') { while (++i < acc.size() && !(acc[i] >= 0x40 && acc[i] <= 0x7E)) {} }
        continue;
      }
      if (c == '\n') { emit(line); line.clear(); continue; }
      if (c == '\r' || c == '\t') { line.push_back(' '); continue; }
      line.push_back(c);
    }
    if (!line.empty()) emit(line);
  };
}

static std::string read_line() {
  std::string line;
  int esc_state = 0;
  int x11_mouse_bytes = 0;
  for (;;) {
    int c = metal_console_getc();
    if (c < 0) { idle_pump(); asm volatile("hlt"); continue; }
    if (esc_state == 0) {
      if (c == 27) { esc_state = 1; continue; }
    } else if (esc_state == 1) {
      if (c == '[') { esc_state = 2; continue; }
      if (c == 'O') { esc_state = 3; continue; }
      esc_state = 0;
      continue;
    } else if (esc_state == 2) {
      if (c == 'M') { esc_state = 4; x11_mouse_bytes = 3; continue; }
      if (c >= 0x20 && c <= 0x3F) { continue; }   // parameter/intermediate bytes
      esc_state = 0;
      continue;   // final byte (0x40-0x7E)
    } else if (esc_state == 3) {
      esc_state = 0;
      continue;
    } else if (esc_state == 4) {
      if (--x11_mouse_bytes <= 0) esc_state = 0;
      continue;
    }
    if (c == '\r' || c == '\n') { out("\n"); return line; }
    if (c == 8 || c == 127) { if (!line.empty()) { line.pop_back(); out("\b \b"); } continue; }
    if (c == 3) { out("^C\n"); line.clear(); continue; }
    if (c >= 32 && c < 127) { line.push_back((char)c); metal_console_write((const char*)&c, 1); }
  }
}

extern "C" void metal_start(uint32_t magic, uint32_t mbi) {
  metal_console_init();
  // The screen first: under UEFI the only screen is the framebuffer the loader
  // set up (Multiboot2 tag 8), so find it before printing anything.
  if (magic == 0x36d76289u) {
    uint8_t* p = (uint8_t*)(uintptr_t)mbi + 8;
    for (;;) {
      MbTag* t = (MbTag*)p;
      if (t->type == 0) break;
      if (t->type == 8) {
        uint64_t addr = *(uint64_t*)(p + 8); uint32_t pitch = *(uint32_t*)(p + 16), w = *(uint32_t*)(p + 20), h = *(uint32_t*)(p + 24);
        uint8_t bpp = p[28], type = p[29];
        metal_display_init(addr, pitch, w, h, bpp, type);
        // The loader gives whatever the firmware's mode list held — under OVMF
        // that tops out at 1024x768. Ask the adapter itself for better, which
        // is a mode the kernel sets rather than one it was handed.
        // No heap yet, so the report is assembled by hand.
        uint32_t mw = 0, mh = 0, mp = 0;
        if (metal_display_best_mode(&mw, &mh, &mp)) {
          char msg[72];
          size_t at = 0;
          const char* lead = "[metal] display: adapter mode set to ";
          while (lead[at]) { msg[at] = lead[at]; ++at; }
          auto put_u = [&](uint32_t v) {
            char d[8];
            int n = 0;
            do { d[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 8);
            while (n) msg[at++] = d[--n];
          };
          put_u(mw); msg[at++] = 'x'; put_u(mh);
          const char* tail = "x32\n";
          for (int i = 0; tail[i]; ++i) msg[at++] = tail[i];
          msg[at] = 0;
          outc(msg);
        }
      }
      p += (t->size + 7) & ~7u;
    }
  }
  metal_vga_clear();
  outc("\n[metal] AIOS bare-metal entry; multiboot2 magic ");
  outc(magic == 0x36d76289u ? "ok\n" : "MISSING\n");
  uint64_t total = 0, heap_base = 0, heap_len = 0;
  const char* cmdline = "";
  const uint8_t* rsdp = nullptr;
  uint64_t kend = ((uint64_t)(uintptr_t)__kernel_end + 0xFFFFF) & ~0xFFFFFull;
  if (magic == 0x36d76289u) {
    uint8_t* p = (uint8_t*)(uintptr_t)mbi + 8;
    for (;;) {
      MbTag* t = (MbTag*)p;
      if (t->type == 0) break;
      if (t->type == 1) cmdline = (const char*)(p + 8);
      // ACPI RSDP from the loader: essential under UEFI, where it is not in
      // the legacy BIOS area. Tag 15 (ACPI 2.0+) wins over tag 14 (1.0).
      if (t->type == 14 && !rsdp) rsdp = p + 8;
      if (t->type == 15) rsdp = p + 8;
      if (t->type == 6) {
        uint32_t esz = *(uint32_t*)(p + 8);
        for (uint8_t* e = p + 16; e < p + t->size; e += esz) {
          MbMmapEntry* m = (MbMmapEntry*)e;
          if (m->type != 1) continue;
          total += m->len;
          uint64_t b = m->base < kend ? kend : m->base, end = m->base + m->len;
          if (end > (1ull << 30)) end = 1ull << 30;
          if (end > b && end - b > heap_len) { heap_base = b; heap_len = end - b; }
        }
      }
      p += (t->size + 7) & ~7u;
    }
  }
  // The multiboot info (which cmdline points into) lives in low memory the heap
  // will reuse — so decide 'accept' NOW, from the string, before heap init.
  bool want_accept = false;
  for (const char* q = cmdline; q && *q; ++q)
    if (q[0]=='a'&&q[1]=='c'&&q[2]=='c'&&q[3]=='e'&&q[4]=='p'&&q[5]=='t') { want_accept = true; break; }
  if (!heap_len) { heap_base = kend; heap_len = 64ull << 20; total = heap_base + heap_len; }
  metal_heap_init((void*)(uintptr_t)heap_base, (size_t)heap_len);
  metal_set_fs_base(metal_make_tls_block());   // before anything touches a thread_local

  // A virtio-gpu has no VGA compatibility and no DISPI registers, and the
  // firmware here may have no GOP for it either — in which case the loader
  // handed over nothing and the machine has an 80x25 text console on a
  // machine with a perfectly good display. The kernel brings it up itself.
  // This has to wait for the heap: the queues and the framebuffer are pages.
  {
    uint32_t gw = 0, gh = 0, gp = 0;
    uint64_t gbase = 0;
    uint32_t have_w = 0, have_h = 0, have_bpp = 0, tc = 0, tr = 0;
    bool had = metal_display_describe(&have_w, &have_h, &have_bpp, &tc, &tr);
    if (!had || have_w < 1280) {
      if (metal_virtio_gpu_init(1920, 1080, &gbase, &gp, &gw, &gh)) {
        metal_display_adopt(gbase, gp, gw, gh, metal_virtio_gpu_present);
        outc((std::string("[metal] display: virtio-gpu scanout ") + std::to_string(gw) + "x" +
              std::to_string(gh) + "x32 brought up by this kernel\n").c_str());
      }
    }
  }
  { uint32_t w, h, bpp, tc, tr; bool have = metal_display_describe(&w, &h, &bpp, &tc, &tr);
    std::string d = have ? "[metal] display: framebuffer " + std::to_string(w) + "x" + std::to_string(h) + "x" + std::to_string(bpp) + ", text " + std::to_string(tc) + "x" + std::to_string(tr) + "\n" : std::string("[metal] display: VGA text 80x25 (no framebuffer from the loader)\n");
    outc(d.c_str()); }
  metal_clock_init();
  // The IDT, and with it the first thing on this machine that can take the CPU
  // back: faults report instead of stopping the machine dead, the timer ticks,
  // and the keyboard and mouse arrive because the controller raised a line.
  metal_idt_init();
  metal_ps2_irq_enable();
  out("[metal] interrupts: IDT live, PIC remapped to 32-47, timer at " + std::to_string(metal_tick_hz()) +
      " Hz, PS/2 on IRQ1 and IRQ12\n");
  if (rsdp) metal_smp_set_rsdp(rsdp);
  int cpus = metal_smp_init();
  // ACPI has been read by now, so the machine can leave the 8259s behind and
  // take its interrupts through the controller it actually has.
  if (metal_ioapic_init())
    out("[metal] interrupts: moved to the IOAPIC, both 8259s masked, acknowledged at the local APIC\n");
  else
    out("[metal] interrupts: no IOAPIC in the tables — staying on the legacy PIC\n");
  metal_set_memory(total, (uint32_t)cpus);
  out("[metal] cpus online: " + std::to_string(cpus) + "\n");
  __register_frame_info(__eh_frame_start, eh_object);   // C++ exceptions for the command surface
  for (auto** f = __init_array_start; f < __init_array_end; ++f) (*f)();
  std::set_terminate([] { metal_panic("std::terminate"); });
  out("[metal] heap " + std::to_string(heap_len >> 20) + " MiB, tsc " + std::to_string(metal_tsc_hz() / 1000000) + " MHz, usable memory " + std::to_string(total >> 20) + " MiB\n\n");

  // Storage: the first virtio disk, or an NVM Express controller when the
  // machine has one and no virtio disk. Either way, if it carries an AOS
  // volume the persistent root lives there.
  g_disk = metal_probe_virtio_blk();
  if (!g_disk) {
    if (aios::hal::BlockDevice* nvme = metal_nvme_probe()) g_disk.reset(nvme);
  }
  if (g_disk) {
    metal_disk = g_disk.get();
    out("[metal] disk " + g_disk->name() + " " + std::to_string((g_disk->sectors() * 512) >> 20) + " MiB");
    g_part = find_aos_partition(*g_disk);
    g_volume_dev = g_part ? g_part.get() : g_disk.get();
    if (g_part) {
      out(" · AOS partition " + g_part->name() + " " + std::to_string((g_part->sectors() * 512) >> 20) + " MiB");
      // A partition marked as ours but still blank is formatted on first boot: it exists for nothing else.
      if (!hal::Volume::probe(*g_part)) { auto f = hal::Volume::format(*g_part, "aios"); out(f.ok() ? " · formatted on first boot" : " · format failed: " + f.error().str()); }
    }
    if (hal::Volume::probe(*g_volume_dev)) { auto m = g_volume.mount(*g_volume_dev); if (m.ok()) { metal_volume = &g_volume; out(" · AOS volume '" + g_volume.info().label + "' mounted (" + std::to_string(g_volume.info().records) + " records, generation " + std::to_string(g_volume.info().generation) + ")\n"); } else out(" · volume mount failed: " + m.error().str() + "\n"); }
    else out(" · no AOS volume (host volume format)\n");
  } else out("[metal] no disk: state lives in memory\n");

  // Network: the first virtio NIC carries the AOS fabric (raw Ethernet; no IP, no host stack).
  g_nic = metal_probe_virtio_net();
  if (!g_nic) g_nic = metal_probe_e1000();   // real silicon: an Intel 8254x on the bus
  if (g_nic) { g_fabric = std::make_unique<hal::Fabric>(*g_nic, metal_now_ns); metal_fabric = g_fabric.get(); g_net = std::make_unique<aios::net::NetStack>(*g_nic, metal_now_ns, metal_rand32); metal_net = g_net.get(); out("[metal] nic " + g_nic->name() + " " + hal::mac_str(g_nic->mac()) + " · AOS fabric + TCP/IP ready\n"); }
  else out("[metal] no nic\n");

  KernelConfig cfg; cfg.root = "/aios/root";
  auto kr = Kernel::boot(cfg);
  if (!kr.ok()) { out("boot failed: " + kr.error().str() + "\n"); qemu_exit(0x20); }
  Kernel& k = **kr;
  g_kernel = &k;

  // Page tables, a ring-3 descriptor pair, a task-state segment and the system
  // call gate. Before this the machine had one address space and one privilege
  // level; after it, a program can be given neither.
  metal_process_init(32ull << 20);

  // The world model gets real eyes and one real hand. Before this line it was
  // reading a sine wave and driving nothing.
  aios::intelligence::set_telemetry_provider(&metal_telemetry);
  aios::intelligence::set_actuation_sink(&metal_actuate);
  aios::intelligence::set_substrate_settle(&metal_settle);
  out("[metal] world model: telemetry live (cpu, ctx-switch rate, heap, nic ring), actuators bound: scheduler slice + virtio-net rx prefetch window, settler bound: 2 x /bin/spin\n");
  {
    struct { const char* name; char* begin; char* end; } shipped[] = {
        {"hello", user_hello_start, user_hello_end}, {"probe", user_probe_start, user_probe_end},
        {"crash", user_crash_start, user_crash_end}, {"priv", user_priv_start, user_priv_end},
        {"peek", user_peek_start, user_peek_end}, {"spin", user_spin_start, user_spin_end},
        {"fs", user_fs_start, user_fs_end},           {"mem", user_mem_start, user_mem_end},
        {"forker", user_forker_start, user_forker_end}, {"execer", user_execer_start, user_execer_end},
        {"sig", user_sig_start, user_sig_end},        {"fpu", user_fpu_start, user_fpu_end},
        {"edges", user_edges_start, user_edges_end},
    };
    k.host().make_dir(cfg.root + "/fs");
    k.host().make_dir(cfg.root + "/fs/bin");
    size_t bytes = 0;
    for (auto& u : shipped) {
      k.host().write_file(cfg.root + "/fs/bin/" + u.name, ByteSpan((const uint8_t*)u.begin, (size_t)(u.end - u.begin)));
      bytes += (size_t)(u.end - u.begin);
    }
    uint64_t pool = 0, live = 0;
    aios::metal::process_memory(&pool, &live, nullptr);
    out("[metal] processes: ring 3 ready, " + std::to_string(pool) + " pages of user memory, /bin holds " +
        std::to_string(sizeof(shipped) / sizeof(shipped[0])) + " programs (" + std::to_string(bytes >> 10) + " KiB)\n");
  }

  if (want_accept) {
    int failures = run_acceptance(k);
    qemu_exit(failures == 0 ? 0x10 : 0x20);
  }

  out("   ___   ____   ___  ____\n  / _ | /  _/  / _ \\/ __/      the machine on which intelligence exists\n / __ |_/ /   / // /\\ \\\n/_/ |_/___/   \\____/___/       no host: this is the OS on the metal\n\n");
  cli::Session s;
  s.kernel = std::move(*kr);
  s.polled_services = true;
  s.colour = true;
  if (auto b = cli::bind_console(s); !b.ok()) { out("console entity: " + b.error().str() + "\n"); for (;;) {} }
  // From here the console is a task in the scheduler like any other: the timer
  // takes the CPU off it too, which is what lets a spawned program keep running
  // while the prompt sits waiting for the next thing typed at it.
  aios::metal::process_become_console(k, *s.me);
  g_session = &s;
  cli::Out o([](const std::string& t) { metal_console_write(t.data(), t.size()); });
  o.set_colour(true);
  s.out = &o;
  s.host = [&s](const std::vector<std::string>& args, cli::Out& po, Surface& me) -> Result<void> {
    if (args.empty() || args[0] == "help") {
      po << po.style(cli::Header()) << "Host Substrate Commands:" << po.style(cli::Reset()) << "\n"
         << "  dash / status  · live interactive system dashboard\n"
         << "  desktop / gui  · the graphical desktop on the framebuffer (mouse + keyboard)\n"
         << "  disks          · storage device info\n"
         << "  volume [...]   · AOS partition volume stats or format\n"
         << "  net            · fabric network status and session peers\n"
         << "  heap           · memory allocator usage\n"
         << "  irq            · interrupts, the tick, and what has been taken\n"
         << "  run <p> [...]  · load ELF programs into ring 3 and wait for them\n"
         << "  spawn <p>      · start one and come straight back to this prompt\n"
         << "  wait           · wait for everything still running\n"
         << "  kill <pid> [n] · send a signal\n"
         << "  ps             · the task table\n"
         << "  canvas         · the machine's own lifecycle on the screen (no windows)\n"
         << "  reboot         · restart machine\n"
         << "  poweroff       · shut down machine\n";
      return Ok();
    }
    if (args[0] == "dash" || args[0] == "status") {
      po << "\033[1;36m┌── \033[1;35mAIOS Bare-Metal Interactive Dashboard\033[1;36m ─────────────────────────────┐\033[0m\n";
      po << "\033[1;36m│\033[0m  \033[1;32mKernel Identity:\033[0m  " << me.kernel().self().str() << "\n";
      po << "\033[1;36m│\033[0m  \033[1;32mConsole Entity:\033[0m   " << me.self().str() << "\n";
      po << "\033[1;36m│\033[0m  \033[1;33mHeap Memory:\033[0m      " << (uint64_t)(metal_heap_used() >> 10) << " KiB used\n";
      po << "\033[1;36m│\033[0m  \033[1;33mStorage Root:\033[0m     " << (metal_volume ? ("Mounted volume '" + metal_volume->info().label + "'") : "In-memory RAM root") << "\n";
      if (g_nic) {
        auto& st = g_fabric->stats();
        po << "\033[1;36m│\033[0m  \033[1;34mNetwork Fabric:\033[0m   " << g_nic->name() << " [" << hal::mac_str(g_nic->mac()) << "] (In: " << st.frames_in << ", Out: " << st.frames_out << ")\n";
      } else {
        po << "\033[1;36m│\033[0m  \033[1;34mNetwork Fabric:\033[0m   Offline / No NIC\n";
      }
      {
        uint64_t pool = 0, live = 0, sw = 0;
        aios::metal::process_memory(&pool, &live, &sw);
        po << "\033[1;36m│\033[0m  \033[1;32mProcesses:\033[0m        ring 3 live · " << aios::metal::process_list().size()
           << " in the table · " << live << "/" << pool << " user pages held · " << metal_ticks()
           << " ticks at " << metal_tick_hz() << " Hz · " << sw << " context switches\n";
      }
      uint32_t dw, dh, dbpp, tc, tr;
      if (metal_display_describe(&dw, &dh, &dbpp, &tc, &tr)) {
        po << "\033[1;36m│\033[0m  \033[1;35mDisplay Surface:\033[0m  " << dw << "x" << dh << "@" << (int)dbpp << "bpp (" << tc << "x" << tr << " text grid)\n";
      }
      po << "\033[1;36m└─────────────────────────────────────────────────────────────────┘\033[0m\n";
      return Ok();
    }
    if (args[0] == "canvas" || args[0] == "lifecycle") {
      auto r = metal_run_canvas(me.kernel(), me, canvas_feed, idle_pump);
      if (!r.ok()) return r;
      return Ok();
    }
    if (args[0] == "irq") {
      uint64_t t0 = metal_ticks();
      po << "IDT live, timer " << metal_tick_hz() << " Hz, PS/2 keyboard and mouse on their own lines\n";
      po << "delivered by: " << (metal_ioapic_live() ? "the IOAPIC, acknowledged at the local APIC (both 8259s masked)"
                                                     : "the legacy 8259 pair, remapped to vectors 32-47") << "\n";
      po << "interrupt flag: " << (metal_interrupts_live() ? "set — the CPU is taking them" : "clear") << "\n";
      po << "ticks: " << t0; po.flush();
      // Stop the CPU entirely. If it ever runs again, something interrupted it.
      for (int i = 0; i < 25; ++i) metal_halt_until_interrupt();
      po << " → " << metal_ticks() << " after halting 25 times ("
         << ((metal_ticks() - t0) * 1000 / metal_tick_hz()) << " ms of real time, and the CPU was stopped for it)\n";
      uint64_t pool = 0, live = 0, sw = 0;
      aios::metal::process_memory(&pool, &live, &sw);
      po << "context switches so far: " << sw << "\n";
      return Ok();
    }
    if (args[0] == "ps") {
      auto pad = [&po](const std::string& text, size_t width) {
        po << text;
        for (size_t i = text.size(); i < width; ++i) po << " ";
      };
      po << "  PID  PPID  NAME              STATE      TICKS  CALLS  PAGES  EXIT  NOTE\n";
      auto row = [&](const aios::metal::ProcessReport& p) {
        po << "  ";
        pad(std::to_string(p.pid), 5);
        pad(p.pid == 0 ? std::string("-") : std::to_string(p.ppid), 6);
        pad(p.name, 18);
        pad(p.state, 11);
        pad(std::to_string(p.ticks), 7);
        pad(std::to_string(p.syscalls), 7);
        pad(std::to_string(p.pages), 7);
        pad(p.pid == 0 ? std::string("-") : std::to_string(p.exit_code), 6);
        po << p.ended << "\n";
      };
      row(aios::metal::process_console());
      for (auto& p : aios::metal::process_list()) row(p);
      return Ok();
    }
    if (args[0] == "kill") {
      if (args.size() < 2) return Result<void>(Status::Invalid, "host kill <pid> [signal]");
      int pid = 0, sig = 15;
      for (char ch : args[1]) if (ch >= '0' && ch <= '9') pid = pid * 10 + (ch - '0');
      if (args.size() > 2) { sig = 0; for (char ch : args[2]) if (ch >= '0' && ch <= '9') sig = sig * 10 + (ch - '0'); }
      auto r = aios::metal::process_signal(pid, sig);
      if (!r.ok()) return r;
      po << "signal " << sig << " sent to pid " << pid << "\n";
      return Ok();
    }
    if (args[0] == "wait") {
      bool live = aios::metal::process_any_live();
      uint64_t switches = live ? aios::metal::process_wait_all() : 0;
      auto finished = aios::metal::process_list();
      if (finished.empty()) { po << "nothing to wait for\n"; return Ok(); }
      if (live) po << "everything finished; the scheduler switched " << switches << " time(s) while this console waited\n";
      else po << "everything had already finished\n";
      for (auto& p : aios::metal::process_list())
        po << "  pid " << p.pid << " " << p.name << ": " << p.state << ", exit " << p.exit_code << ", " << p.ticks
           << " tick(s), " << p.syscalls << " system call(s)"
           << (p.ended.empty() ? std::string() : std::string(" — ") + p.ended) << "\n";
      aios::metal::process_reap();
      return Ok();
    }
    if (args[0] == "run" || args[0] == "spawn") {
      bool background = args[0] == "spawn";
      if (args.size() < 2) return Result<void>(Status::Invalid, "host run|spawn <program> [<program> ...] [bare]");
      bool bare = false;
      std::vector<std::string> paths;
      for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "bare") { bare = true; continue; }
        paths.push_back(args[i]);
      }
      // The authority a process runs as: the console's own, or a freshly made
      // entity that holds nothing — the same program, two different answers.
      EntityID as_entity = me.self();
      if (bare) {
        auto e = me.spawn("bare-process");
        if (!e.ok()) return Result<void>(e.error());
        as_entity = *e;
        po << "running as a bare entity " << e->short_str() << ", which holds no authority of its own\n";
      }
      Surface actor = me.kernel().as(as_entity);
      std::vector<int> started;
      for (const auto& raw : paths) {
        std::string path = raw.size() && raw[0] == '/' ? me.kernel().config().root + "/fs" + raw : raw;
        // Read through the Surface: an entity that may not read this machine's
        // storage cannot get a program out of it, let alone run one.
        auto file = aios::userland::elf_open(me.kernel(), &actor, path);
        if (!file.ok()) { po << raw << ": " << file.error().str() << "\n"; continue; }
        auto pid = aios::metal::process_spawn(raw, ByteSpan(file->bytes.data(), file->bytes.size()), actor);
        if (!pid.ok()) { po << raw << ": " << pid.error().str() << "\n"; continue; }
        started.push_back(*pid);
        po << "loaded " << raw << " as pid " << *pid << " (" << file->bytes.size() << " bytes of ELF, entry "
           << aios_hex(file->image.entry) << ", " << file->image.segments.size() << " segments)\n";
      }
      po.flush();
      if (background) {
        po << "running; this prompt is a task too, so it comes straight back — `host ps` to watch them\n";
        return Ok();
      }
      uint64_t switches = aios::metal::process_wait(started);
      auto list = aios::metal::process_list();
      po << "\nthe scheduler took the CPU back " << switches << " time(s); ticks below are timer interrupts each task was holding it for\n";
      for (auto& p : list)
        if (std::find(started.begin(), started.end(), p.pid) != started.end()) po << "  pid " << p.pid << " " << p.name << ": " << p.state << ", exit " << p.exit_code << ", "
           << p.ticks << " tick(s), " << p.syscalls << " system call(s)"
           << (p.ended.empty() ? std::string() : std::string(" — ") + p.ended) << "\n";
      aios::metal::process_reap(started);
      return Ok();
    }
    // `jepa nic` — the second lever, read back from the driver itself.
    //
    // a_t[1] sets the virtio-net rx prefetch window. Everything printed here is
    // a counter the receive path maintains (buffers posted, doorbell kicks,
    // largest refill batch, times the window drained), so "the driver consumed
    // the flag" is something you can see rather than something this text
    // claims. `jepa nic on|off` drives the lever by hand for comparison.
    // `jepa learn` — what the machine has been teaching itself while idle.
    //
    // The loop runs on its own in idle_pump; this is how you see that it is
    // running and stop it. It reports iterations rather than a status string,
    // because "learning: enabled" is true of a loop that has refused every
    // pass, and that is exactly the failure worth catching.
    if (args[0] == "jepa" && args.size() > 1 && args[1] == "learn") {
      if (args.size() > 2 && (args[2] == "on" || args[2] == "off")) {
        g_jepa_learning = args[2] == "on";
        if (g_jepa_learning) g_jepa_refusals = 0;
      }
      po << "idle learner   : " << (g_jepa_learning ? "running" : "stopped")
         << "   one iteration per " << kLearnIntervalTicks << " ticks, idle only\n";
      po << "iterations     : " << g_jepa_iters << " completed, " << g_jepa_refusals << " refused\n";
      po << "last reward    : " << g_jepa_last_reward << "\n";
      po << "predictor      : " << aios::intelligence::get_global_jepa_world_model()->total_steps()
         << " step(s), loss " << aios::intelligence::get_global_jepa_world_model()->current_loss() << "\n";
      po << "policy         : " << aios::intelligence::get_global_action_policy_head()->total_steps()
         << " step(s), baseline " << aios::intelligence::get_global_action_policy_head()->current_baseline() << "\n";
      po << "recorded        : " << aios::intelligence::get_global_trace_logger()->size() << " transition(s)\n";
      if (g_jepa_iters == 0 && g_jepa_refusals > 0)
        po << "NOTHING HAS BEEN LEARNED — every pass refused; telemetry or the settler is not bound\n";
      return Ok();
    }
    if (args[0] == "jepa" && args.size() > 1 && args[1] == "nic") {
      if (!metal_nic_present()) { po << "no virtio-net device on this machine; the rx prefetch lever has nothing to set\n"; return Ok(); }
      if (args.size() > 2 && (args[2] == "on" || args[2] == "off")) {
        bool want = args[2] == "on";
        uint64_t kicks_before = metal_nic_rx_kicks();
        uint32_t window_before = metal_nic_rx_window();
        (void)metal_set_nic_prefetch(want);
        po << "rx prefetch " << (want ? "ON" : "OFF") << ": window " << window_before << " -> "
           << metal_nic_rx_window() << " buffer(s), " << (metal_nic_rx_kicks() - kicks_before)
           << " doorbell kick(s) to apply it\n";
      }
      uint64_t kicks = metal_nic_rx_kicks(), posted_total = metal_nic_rx_buffers_posted();
      po << "virtio-net rx posting:\n";
      po << "  prefetch flag (a_t[1])   " << (metal_nic_prefetch() ? "on" : "off") << "\n";
      po << "  window / slots           " << metal_nic_rx_window() << " of " << metal_nic_rx_slots()
         << " buffer(s) kept posted\n";
      po << "  posted right now         " << metal_nic_rx_posted() << "\n";
      po << "  buffers posted (total)   " << posted_total << "\n";
      po << "  doorbell kicks           " << kicks << "\n";
      po << "  buffers per kick         " << (kicks ? posted_total / kicks : 0ull)
         << " (largest single batch " << metal_nic_rx_max_batch() << ")\n";
      po << "  window drained           " << metal_nic_rx_starved() << " time(s)\n";
      po << "  driver behind by         " << metal_nic_rx_behind_mean() << " frame(s) on average, "
         << metal_nic_rx_behind_max() << " at worst (backlog waiting to be taken - NOT loss)\n";
      po << "  frames the device dropped: NOT OBSERVABLE from this driver. virtio-net\n";
      po << "    reports them only over the control queue's statistics, which this driver\n";
      po << "    does not negotiate. `window drained` above is the loss it CAN see.\n";
      po << "  window changed by policy " << metal_nic_rx_window_changes() << " time(s)\n";
      return Ok();
    }
    // `jepa prove` — does the world model's action actually change this machine?
    //
    // Runs the SAME workload twice under two scheduler slices the policy head is
    // allowed to pick, and reports the context switches the kernel counted. If
    // the lever is real the switch count moves; if it is decoration, it will not.
    // Nothing here reads a status string — process_wait_all() returns the
    // kernel's own count of times it took the CPU away.
    if (args[0] == "jepa" && args.size() > 1 && args[1] == "prove") {
      const char* prog = args.size() > 2 ? args[2].c_str() : "/bin/spin";
      int copies = args.size() > 3 ? atoi(args[3].c_str()) : 2;
      if (copies < 1) copies = 1;
      Surface actor = me.kernel().as(me.self());
      std::string path = me.kernel().config().root + "/fs" + std::string(prog);
      auto file = aios::userland::elf_open(me.kernel(), &actor, path);
      if (!file.ok()) { po << prog << ": " << file.error().str() << "\n"; return Ok(); }

      uint32_t slices[] = {1, 8, 16};
      uint64_t results[3] = {0, 0, 0};
      uint64_t ticks_used[3] = {0, 0, 0};
      po << "workload: " << copies << " x " << prog << ", identical each run\n\n";
      for (int r = 0; r < 3; ++r) {
        metal_set_scheduler_slice(slices[r]);
        std::vector<int> run;
        for (int i = 0; i < copies; ++i) {
          auto pid = aios::metal::process_spawn(prog, ByteSpan(file->bytes.data(), file->bytes.size()), actor);
          if (pid.ok()) run.push_back(*pid);
        }
        uint64_t t0 = metal_ticks();
        results[r] = aios::metal::process_wait(run);
        ticks_used[r] = metal_ticks() - t0;
        aios::metal::process_reap(run);
        po << "  slice " << slices[r] << " tick(s): the scheduler took the CPU back "
           << results[r] << " time(s) over " << ticks_used[r] << " timer tick(s)\n";
      }
      metal_set_scheduler_slice(1);
      po << "\n";
      if (results[0] > results[2]) {
        po << "the lever is REAL: a larger slice measurably reduced context switches ("
           << results[0] << " -> " << results[2] << ").\n"
           << "a_t[0] from the policy head sets this value, so the world model's output\n"
           << "now changes how this machine schedules. that is the loop closed.\n";
      } else {
        po << "no measurable difference (" << results[0] << " vs " << results[2]
           << ") — the workload may be too short to preempt. try `jepa prove /bin/spin 4`.\n";
      }
      return Ok();
    }
    if (args[0] == "net") {
      if (!g_nic) { po << "no network device\n"; return Ok(); }
      std::string sub = args.size() > 1 ? args[1] : "";
      // AOS's own TCP/IP over this NIC — no host stack underneath.
      if (sub == "up") {
        aios::net::NetConfig cfg{};   // zero ip → DHCP
        if (args.size() > 2) { aios::net::Ipv4 v; if (aios::net::parse_ipv4(args[2], v)) cfg.ip = v; if (args.size() > 3) parse_ipv4(args[3], cfg.netmask); if (args.size() > 4) parse_ipv4(args[4], cfg.gateway); if (args.size() > 5) parse_ipv4(args[5], cfg.dns); }
        po << "bringing the interface up" << (cfg.ip.is_zero() ? " (DHCP)…\n" : "…\n"); po.flush();
        auto r = metal_net->configure(cfg);
        if (!r.ok()) return Result<void>(Status::IoError, r.error().message);
        auto& c = metal_net->config();
        po << "up: ip " << c.ip.str() << " mask " << c.netmask.str() << " gw " << c.gateway.str() << " dns " << c.dns.str() << "\n";
        return Ok();
      }
      if (sub == "status") {
        if (!metal_net->up()) { po << "TCP/IP down (host net up). fabric frames in " << g_fabric->stats().frames_in << " out " << g_fabric->stats().frames_out << "\n"; return Ok(); }
        auto& c = metal_net->config(); auto& n = metal_net->stats();
        po << "ip " << c.ip.str() << " gw " << c.gateway.str() << " dns " << c.dns.str() << "  ·  rx " << n.rx_frames << " tx " << n.tx_frames << " arp(" << n.arp_rx << "/" << n.arp_tx << ") ip(" << n.ip_rx << "/" << n.ip_tx << ") tcp_rx " << n.tcp_rx << " rexmit " << n.tcp_retransmits << " dropped " << n.dropped << "\n";
        return Ok();
      }
      if (sub == "ping") {
        if (!metal_net->up() || args.size() < 3) return Result<void>(Status::Invalid, "net up first, then: host net ping <ip>");
        auto ip = metal_net->resolve(args[2]); if (!ip.ok()) return Result<void>(Status::IoError, ip.error().message);
        auto r = metal_net->ping(*ip); if (!r.ok()) return Result<void>(Status::IoError, r.error().message);
        po << "reply from " << ip->str() << ": " << (*r) << " µs\n"; return Ok();
      }
      if (sub == "resolve") {
        if (!metal_net->up() || args.size() < 3) return Result<void>(Status::Invalid, "net up first, then: host net resolve <host>");
        auto ip = metal_net->resolve(args[2]); if (!ip.ok()) return Result<void>(Status::IoError, ip.error().message);
        po << args[2] << " is " << ip->str() << "\n"; return Ok();
      }
      if (sub == "get") {
        if (!metal_net->up() || args.size() < 3) return Result<void>(Status::Invalid, "net up first, then: host net get <host> [path] [port]");
        std::string host = args[2], path = args.size() > 3 ? args[3] : "/"; uint16_t port = args.size() > 4 ? (uint16_t)({ uint32_t v=0; for(char ch:args[4]) if(ch>='0'&&ch<='9') v=v*10+(ch-'0'); v; }) : 80;
        auto sock = metal_net->connect(host, port); if (!sock.ok()) return Result<void>(Status::IoError, sock.error().message);
        std::string req = "GET " + path + " HTTP/1.0\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
        if (!(*sock)->send_all(ByteSpan((const uint8_t*)req.data(), req.size()))) return Result<void>(Status::IoError, "send failed");
        std::string body; uint8_t buf[512];
        for (int i = 0; i < 400; ++i) { aios::MutableByteSpan one(buf, 1); if (!(*sock)->recv_all(one, 4000)) break; body.push_back((char)buf[0]); if (body.size() > 8000) break; }
        (*sock)->close();
        size_t nl = body.find("\r\n"); po << host << ":" << port << path << " → " << (nl == std::string::npos ? body.substr(0, 80) : body.substr(0, nl)) << "  (" << body.size() << " bytes)\n";
        return Ok();
      }
      if (sub == "btc") {
        // A REAL Bitcoin peer, over AOS's own TCP/IP: resolve a DNS seed, open
        // TCP to a full node on 8333, and complete the version/verack handshake.
        if (!metal_net->up()) return Result<void>(Status::Invalid, "net up first");
        const uint32_t MAINNET = 0xD9B4BEF9u;
        std::vector<std::string> seeds; if (args.size() > 2) seeds.push_back(args[2]); else seeds = aios::channel::dns_seeds(MAINNET);
        for (auto& seed : seeds) {
          auto ip = metal_net->resolve(seed, 5000);
          if (!ip.ok()) { po << "  " << seed << ": DNS failed\n"; continue; }
          po << "  " << seed << " → " << ip->str() << " :8333 … "; po.flush();
          aios::channel::BitcoinPeer peer;
          if (!peer.connect(ip->str(), 8333, MAINNET, 8000).ok()) { po << "no TCP\n"; continue; }
          auto info = peer.handshake(0, 15000);
          if (!info.ok()) { po << "handshake failed: " << info.error().message << "\n"; continue; }
          po << "\n  ✓ handshake complete with a real Bitcoin node:\n";
          po << "    protocol version " << info->version << "\n    user agent      " << info->subversion << "\n    block height    " << info->start_height << "\n    services        0x";
          { uint64_t v = info->services; const char* hx = "0123456789abcdef"; char b[16]; for (int i = 15; i >= 0; --i) { b[15 - i] = hx[(v >> (i * 4)) & 0xF]; } po << std::string(b, 16); }
          po << "\n";
          return Ok();
        }
        return Result<void>(Status::IoError, "no Bitcoin seed node answered");
      }
      auto& st = g_fabric->stats();
      po << g_nic->name() << "  mac " << hal::mac_str(g_nic->mac()) << "  fabric frames in " << st.frames_in << " out " << st.frames_out << "\n";
      po << "AOS TCP/IP: " << (metal_net->up() ? "up (host net status)" : "down — host net up [ip mask gw dns]") << "  ·  host net ping|resolve|get|btc <host>\n";
      po << "fabric peers: node connect " << hal::mac_str(g_nic->mac()) << " <port> <token>\n";
      return Ok();
    }
    // The graphical desktop. It is the substrate's to run: the screen is this
    // machine's framebuffer and the pointer is a device the HAL drives.
    if (args[0] == "desktop" || args[0] == "gui") {
      po.flush();
      return metal_run_desktop(*s.kernel, desktop_dispatcher(s), idle_pump);
    }
    if (args[0] == "reboot") reboot();
    if (args[0] == "poweroff" || args[0] == "exit" || args[0] == "quit" || args[0] == "shutdown") poweroff();
    if (args[0] == "heap") { po << "heap in use " << (uint64_t)(metal_heap_used() >> 10) << " KiB\n"; return Ok(); }
    if (args[0] == "disks") {
      if (!g_disk) { po << "no disks\n"; return Ok(); }
      std::string label; bool isvol = hal::Volume::probe(*g_volume_dev, &label);
      po << g_disk->name() << "  " << (uint64_t)((g_disk->sectors() * 512) >> 20) << " MiB  " << (g_part ? "partitioned, AOS partition " + g_part->name() + " " + std::to_string((g_part->sectors() * 512) >> 20) + " MiB: " : std::string("whole disk: ")) << (isvol ? "AOS volume '" + label + "'" + (metal_volume ? " (mounted: persistent root)" : "") : std::string("no volume")) << (!isvol && !g_part ? [] { std::string h = disk_contents(*g_disk); return h.empty() ? std::string(", blank") : ", not an AOS disk: holds " + h; }() : std::string()) << "\n";
      return Ok();
    }
    if (args[0] == "volume") {
      if (!g_disk) return Result<void>(Status::NotFound, "no disk");
      if (args.size() > 1 && args[1] == "format") {
        std::string existing;
        // `host volume format [label] [confirm=<disk>]`: the confirm word names the disk being wiped.
        std::string label = "aios", confirm;
        for (size_t i = 2; i < args.size(); ++i) { if (args[i].rfind("confirm=", 0) == 0) confirm = args[i].substr(8); else label = args[i]; }
        if (metal_volume || hal::Volume::probe(*g_volume_dev, &existing)) {
          std::string vname = g_volume_dev->name();
          std::string target = confirm.empty() ? vname : confirm;
          auto own = me.own(target);
          if (!own.ok()) return Result<void>(Status::Denied, "the kernel refuses: " + vname + " already holds an AOS volume" + (metal_volume ? " (the persistent root)" : "") + "; wiping it needs kernel:destroy over it: cap request kernel " + target + " destroy confirm=" + target);
        } else if (!g_part) {
          // No AOS partition: the format target is the WHOLE disk. A blank disk is
          // claimed freely; one that carries anything else is someone's data, and
          // wiping it takes kernel:destroy plus the disk named back explicitly.
          std::string holds = disk_contents(*g_disk);
          if (!holds.empty()) {
            if (confirm != g_disk->name()) return Result<void>(Status::Denied, g_disk->name() + " holds " + holds + "; formatting erases the whole disk — repeat with confirm=" + g_disk->name() + " to wipe it");
            auto own = me.own(g_disk->name());
            if (!own.ok()) return Result<void>(Status::Denied, "the kernel refuses: " + g_disk->name() + " is not an AOS disk (it holds " + holds + ") and formatting would erase the whole disk; that needs kernel:destroy over it: cap request kernel " + g_disk->name() + " destroy confirm=" + g_disk->name());
          }
        }
        AIOS_TRY(hal::Volume::format(*g_volume_dev, label));
        AIOS_TRY(g_volume.mount(*g_volume_dev));
        metal_volume = &g_volume;
        AIOS_TRY(metal_adopt_volume());
        po << "volume '" << label << "' formatted on " << g_volume_dev->name() << "; the persistent root now lives on it (" << (uint64_t)g_volume.info().records << " records carried over)\n";
        return Ok();
      }
      if (!metal_volume) { po << "no volume mounted (host volume format)\n"; return Ok(); }
      auto& i = g_volume.info();
      po << "volume '" << i.label << "' on " << g_volume_dev->name() << ": " << (uint64_t)((i.sectors * 512) >> 20) << " MiB, half " << (uint64_t)((i.half_sectors * 512) >> 20) << " MiB, active half " << i.active_half << ", generation " << i.generation << ", " << i.records << " records, " << i.files << " files, " << (uint64_t)(i.bytes_live >> 10) << " KiB live, " << (uint64_t)((i.log_used_sectors * 512) >> 10) << " KiB of log used, " << i.compactions << " compactions\n";
      return Ok();
    }
    return Result<void>(Status::Invalid, "host dash | disks | volume [format [label] [confirm=<disk>]] | net | heap | reboot | poweroff");
  };

  out("\033[1;36m  ___   ____   ___  ____\n / _ | /  _/  / _ \\/ __/      \033[1;35mthe machine on which intelligence exists\033[0m\n\033[1;36m/ __ |_/ /   / // /\\ \\\n/_/ |_/___/   \\____/___/       \033[1;33mno host: this is the OS on the metal\033[0m\n\n");
  out("\033[1;34m[AIOS]\033[0m \033[1;32mKernel:\033[0m " + s.kernel->self().short_str() + " · \033[1;32mConsole:\033[0m " + s.me->self().short_str() + " · " + (metal_volume ? "\033[1;33mpersistent volume mounted\033[0m" : "\033[1;30min-memory storage\033[0m") + " · type help, canvas, dash, or desktop\n\n");
  for (;;) {
    out("aios> ");
    std::string line = read_line();
    if (line == "quit" || line == "exit" || line == "poweroff" || line == "shutdown") poweroff();
    if (line == "dash" || line == "dashboard" || line == "status") {
      s.host({"dash"}, o, *s.me);
      o.flush();
      continue;
    }
    if (line == "desktop" || line == "gui") {
      auto d = s.host({"desktop"}, o, *s.me);
      if (!d.ok()) out("\033[31mdesktop: " + d.error().str() + "\033[0m\n");
      o.flush();
      continue;
    }
    if (line == "canvas" || line == "lifecycle") {
      // The machine's own face on the display — the same shortcut the help
      // text promises, so nobody has to know it lives under `host`.
      auto d = s.host({"canvas"}, o, *s.me);
      if (!d.ok()) out("\033[31mcanvas: " + d.error().str() + "\033[0m\n");
      o.flush();
      continue;
    }
    if (line == "clear" || line == "cls") {
      out("\033[2J\033[H");
      o.flush();
      continue;
    }
    auto r = cli::run_line(s, line);
    if (!r.ok()) out("\033[31merror: " + r.error().str() + "\033[0m\n");
    o.flush();
  }
}

