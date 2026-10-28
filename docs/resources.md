# Resources: devices, memory, storage, budgets, sidecars

This page covers what the machine has and how it is handed out:

- **what exists**: CPUs, accelerators, RAM, NUMA nodes, volumes, PCI functions,
  and how to list them;
- **memory**: memory objects, the five memory types, migration that keeps an
  object's identity, sharing, and tensors;
- **budgets**: allocation scopes on capabilities, device-time quotas, and what
  a refusal looks like;
- **the object store**: `store` and AQL queries (metadata, similarity,
  relations, history, time travel), plus `kv` and `state`;
- **checkpoints**: what an entity checkpoint holds;
- **the sidecars**: `aios-engine`, `aios-sidecar`, the C ABI for other
  languages, and the sidecar subsystems (shared-memory ring, event fabric,
  telemetry, filesystem mirror, module vault, capability guard).

Every example was run against the binaries in `build/` on an x86_64 host with
4 cores, 19.5 GiB of RAM and a GTX 1080 Ti. Each run used a throwaway root
under `/tmp/aios-docs-res/` and `AIOS_NO_ACCEL=1`, except where the point was to
show accelerators. Network listeners were bound to 127.0.0.1. Output is trimmed
but not edited.

Sources: `cli/commands.cpp` (`cmd_info`, `cmd_sysstat`, `cmd_devices`,
`cmd_storage`, `cmd_bus`, `cmd_subsystems`, `cmd_mem`, `cmd_tensor`, `cmd_cap`,
`cmd_quota`, `cmd_kv`, `cmd_state`, `cmd_store`, `cmd_checkpoint`,
`cmd_restore`), `src/memory/`, `include/aios/memory/`, `src/capability/`,
`src/kernel/surface.cpp`, `src/kernel/kernel.cpp`, `src/kernel/checkpoint.cpp`,
`src/storage/`, `include/aios/storage/aql.hpp`, `src/hal/`,
`src/compute/gpu_telemetry.cpp`, `src/compute/p2p_dma.cpp`, `src/sidecar/`,
`include/aios/sidecar/`, `tools/aios-engine.cpp`, `tools/aios-sidecar.cpp`, and
the check tools listed in [Check tools](#check-tools).

How to run console commands (`exec`, `run`, `shell`, `=> name`, `!`) is covered
in [console.md](console.md#starting-the-console). Compute graphs, accelerator
back ends and `compute serve` are in [compute.md](compute.md).

---

## Contents

- [What the machine has](#what-the-machine-has)
- [Memory objects](#memory-objects)
- [Tensors](#tensors)
- [Budgets and refusals](#budgets-and-refusals)
- [The object store and AQL](#the-object-store-and-aql)
- [kv and state](#kv-and-state)
- [What a checkpoint holds](#what-a-checkpoint-holds)
- [The sidecars](#the-sidecars)
- [Check tools](#check-tools)

---

## What the machine has

Five commands answer "what is here". All of them only read (they need the
console's inspect right and nothing else).

| Command | Shows |
|---|---|
| `info` | kernel and console ids, root, host topology, memory fabric totals by type, device list, accelerator note |
| `sysstat` | the same identifiers, plus a resource table (entities, states, memory, channels, graphs, capabilities, store objects), per-provider allocation, and the authority audit count |
| `devices` | every compute device in the fabric: lanes, busy lanes, memory, native memory type, and live memory use with its source and tenants |
| `storage` | every volume the host reports, with the one holding the root marked |
| `pci` (`bus`) | PCI functions: slot, class, vendor:device, driver, upstream bridge, memory windows |
| `subsystems` (`tasks`) | yes/no per capability of the machine, asked live; `subsystems -v` lists the devices behind each answer |

```
$ AIOS_NO_ACCEL=1 build/aios --root /tmp/aios-docs-res/r1 exec info devices storage
System Overview
  Kernel ID       : f85a07a2204fc9bb0002295878ef57e8
  Console ID      : 8847cf249a206c7e00022958797f57e8  (console entity)
  Root            : /tmp/aios-docs-res/r1
  Uptime          : 56.74 ms
  Host            : x86_64 · arch-qbc · 4 cpus · 19.49 GiB · 1 NUMA node(s)

Substrate & Fabric Resources
  Memory Fabric   : 0 objects, 0 B (system 0 B, device 0 B, shared 0 B, persistent 0 B, remote 0 B), 0 migrations
  Entities        : 3 active (states: 1, channels: 0, graphs: 0, capabilities: 25, store objects: 0)
  ...
  Devices         : cpu(66128e5ba119, 4 lanes, 4.29 GiB/19.49 GiB ram)
  Accelerators    : accelerators disabled (AIOS_NO_ACCEL)
DEVICE ID     TYPE      LANES   BUSY    MEMORY        NATIVE    NAME
66128e5ba119  cpu       4       0       19.49 GiB     system    cpu
                in use 4.29 GiB, free 15.20 GiB (host-available)
VOLUME                KIND        CAPACITY    USED        BLOCK   PATH
/dev/nvme0n1p2        filesystem  456.35 GiB  411.51 GiB  4096    /  ← the kernel's root
tmpfs                 filesystem  1.95 GiB    6.09 MiB    4096    /run
tmpfs                 filesystem  9.74 GiB    2.91 MiB    4096    /dev/shm
/dev/sdb2             filesystem  7391.51 GiB 405.49 GiB  4096    /data
/dev/sdb1             filesystem  1832.33 GiB 470.58 GiB  4096    /repos
...
```

The host topology (`src/hal/linux_host.cpp`) records logical CPUs, total and
available memory, the architecture, and one entry per NUMA node with its memory
and CPU list. `info` and `sysstat` print the node count. No command prints the
per-node CPU lists.

`storage` reports three kinds of volume: `filesystem` (the host's), `volume`
(an AOS volume, see [volume-check](#check-tools)) and `device` (raw).

### Accelerators and live device memory

Device detection (OpenCL, CUDA, Vulkan, Metal) is covered in
[compute.md](compute.md#how-accelerators-are-detected). Without
`AIOS_NO_ACCEL`, `devices` also prints what is in use on each card and who
holds it. On NVIDIA cards the numbers come from NVML, loaded at run time
(`src/compute/gpu_telemetry.cpp`), so memory held by other processes shows up
as tenants:

```
$ build/aios --root /tmp/aios-docs-res/acc exec devices
DEVICE ID     TYPE      LANES   BUSY    MEMORY        NATIVE    NAME
67749a8b9200  cpu       4       0       19.49 GiB     system    cpu
                in use 4.24 GiB, free 15.24 GiB (host-available)
c9ac044c117f  gpu       28      0       10.91 GiB     device    gpu:NVIDIA GeForce GTX 1080 Ti
                in use 1.03 GiB, free 9.97 GiB (nvml); tenants: pid 5281 aios 283.64 MiB pid 47565 aios 283.64 MiB ... pid 2096 Xorg 73.3...
2bdb8e1690b5  gpu       3584    0       10.91 GiB     device    cuda:NVIDIA GeForce GTX 1080 Ti cc6.1 [0]
                in use 958.00 MiB, free 9.97 GiB (cuMemGetInfo+nvml); tenants: ...
8a1309df1fe0  gpu       16      0       11.24 GiB     device    vulkan:NVIDIA GeForce GTX 1080 Ti api1.3 [0]
ec4af3a19f5e  gpu       1       0       19.49 GiB     system    vulkan:llvmpipe (LLVM 20.1.2, 256 bits) api1.4 [1]
                in use 4.26 GiB, free 15.23 GiB (vulkan-budget)
```

The source in parentheses says where the number came from: `nvml`,
`cuMemGetInfo+nvml`, `vulkan-budget` or `host-available`.

### Health and capability summary

```
$ AIOS_NO_ACCEL=1 build/aios --root /tmp/aios-docs-res/r1 exec sysstat subsystems
...
Machine Resources
  Compute           : cpu(4 lanes)
  Storage           : /dev/nvme0n1p2 456.35 GiB (411.51 GiB used) ← holds the root · 15 volume(s)
  Network           : br0 10.60.60.215 · virbr0 192.168.122.1 · ... · 5 of 10 up

Entity & Substrate Resource Table
RESOURCE        ACTIVE      TOTAL BYTES   DETAILS
Entities        3           -             3 resident(s)
States          1           -             Entity context & execution state
Memory Fabric   0           0 B           0 migrations
...
Memory Fabric Provider Allocation
PROVIDER      ALLOCATED       DESCRIPTION
System RAM    0 B             Private kernel mapped RAM
Device        0 B             GPU / NPU accelerator memory
Shared        0 B             memfd channel ring regions
Persistent    0 B             AOS volume disk-backed memory
Remote        0 B             Fabric peer node memory
...
What this machine can do, asked of the machine
  no  compute devices           1 in the fabric (0 gpu): cpu [4.31 GiB/19.49 GiB loaded]
                                accelerators disabled (AIOS_NO_ACCEL)
  yes storage                   15 volume(s)
  yes bus enumeration           20 function(s) on the bus
  yes network interfaces        4 up, 10 total
  ...
  yes execution quotas          0 entity(ies) bounded; the scheduler charges measured device time
  no  hardware attestation      no TPM on this machine (looked for /dev/tpmrm0, /dev/tpm0, and a software TPM on 127.0.0.1:2321)
```

"compute devices" reads `no` whenever the CPU is the only device.

### PCI

`pci` (or `pci list`) reads the bus from sysfs. `pci peer <src-slot>
<dst-slot> [bytes]` checks whether two functions can reach each other's memory
windows directly. It never moves data: a hosted process is not allowed to
master the bus, so the transfer line always reports why not
(`src/compute/p2p_dma.cpp`).

```
$ build/aios --root R exec pci
SLOT            KIND        VENDOR:DEV  DRIVER        UPSTREAM      WINDOWS
0000:00:17.0    storage     8086:a282   ahci          pci0000:00    bar0@0xdf348000/8.0 KiB bar1@0xdf34c000/256 B bar5@0xdf34b000/2.0 KiB
0000:01:00.0    display     10de:1b06   nvidia        0000:00:01.0  bar0@0xde000000/16.00 MiB bar1@0xc0000000/256.00 MiBp bar3@0xd0000000/32.00 MiBp
0000:05:00.0    storage     15b7:501a   nvme          0000:00:1d.0  bar0@0xdf100000/16.0 KiB bar4@0xdf104000/256 B
...
$ build/aios --root R exec 'pci peer 0000:01:00.0 0000:05:00.0 4096'
  Source            : 0000:01:00.0 answers at 0xc0000000 (256.00 MiB)
  Target            : 0000:05:00.0 answers at 0xdf100000 (16.0 KiB)
  Fabric            : 0000:01:00.0 is under 0000:00:01.0 and 0000:05:00.0 is under 0000:00:1d.0; a transfer between them crosses the root complex
  Peer capable      : no
  Transfer          : these two cannot reach each other: ...
$ build/aios --root R exec 'pci peer 0000:00:01.0 0000:05:00.0'
FAILED: unsupported: 0000:00:01.0 exposes no memory window
```

A `p` after a window size means prefetchable.

---

## Memory objects

An entity never holds an address. It holds a **MemoryID**, and the kernel
holds the mapping (`include/aios/memory/memory_object.hpp`). Each object has a
size, a type, an owner, permissions, and a **generation** that goes up on every
write and every migration.

### The five types

| Type | Provider | Backed by | Registered when |
|---|---|---|---|
| `system` | `make_system_ram_provider` | private anonymous `mmap` | always |
| `shared` | `make_shared_memory_provider` | `memfd_create` + shared `mmap` (channel rings use this) | always |
| `persistent` | `make_persistent_memory_provider` | a file `<root>/memory/<id>.mem`, mapped shared and synced on each write | always |
| `device` | `DeviceMemoryProvider` (`src/compute/opencl_device.cpp`) | an OpenCL device buffer | an OpenCL device registers (not with `AIOS_NO_ACCEL`) |
| `remote` | `RemoteMemoryProvider` (`src/node/remote.cpp`) | memory on a peer kernel | `node connect` attaches a peer |

Asking for a type with no provider fails with `unsupported: no provider for
<type>`.

### Commands

| Command | What it does | Needs |
|---|---|---|
| `mem alloc <e> <bytes> [type]` | allocate; charges the entity's allocation scope | `memory:* allocate` |
| `mem write <e> <mem> <offset> <text\|hex:..>` | write bytes | `write` on the object |
| `mem read <e> <mem> <offset> <n>` | read bytes | `read` on the object |
| `mem migrate <e> <mem> <type>` | move the contents to another type, same identity | `write` on the object |
| `mem share <e> <mem> <to> <rights>` | delegate rights on the object to another entity | `grant` on the object |
| `mem release <e> <mem>` | destroy the object | `destroy` on the object |
| `mem list [<e>]` | objects, with type, size, owner, generation, backing file | inspect |
| `mem compact` (`mem gc`) | release objects whose owner no longer exists | inspect |

`mem alloc`'s usage line lists `system|shared|persistent`, but it accepts any
of the five type names (see below for `device`).

The entity that allocates an object gets `read,write,grant,destroy` on that
object. Nobody else gets anything until it is shared.

### Allocate, migrate, share

```
$ AIOS_NO_ACCEL=1 build/aios --root /tmp/aios-docs-res/mem run mem.aos
mem.aos:3 › mem alloc $w 4096 => m
memory 3cf91993a0ccdb460008c2f903a48345 (4.0 KiB system)
mem.aos:4 › mem write $w $m 0 hello-fabric
wrote 12 bytes
mem.aos:6 › mem migrate $w $m shared
memory 3cf91993a0cc now shared gen=2
mem.aos:7 › mem migrate $w $m persistent
memory 3cf91993a0cc now persistent gen=3 backing=/tmp/aios-docs-res/mem/memory/3cf91993a0ccdb460008c2f903a48345.mem
mem.aos:8 › mem list $w
MEMORY ID     TYPE        SIZE        OWNER         GEN   BACKING
3cf91993a0cc  persistent  4.0 KiB     42c65b50a7d4  3     /tmp/aios-docs-res/mem/memory/3cf91993a0ccdb460008c2f903a48345.mem
mem.aos:9 › mem read $w $m 0 12
68656c6c6f2d666162726963  "hello-fabric" (12 bytes)
mem.aos:10 › mem migrate $w $m system
memory 3cf91993a0cc now system gen=4
mem.aos:12 › ! mem migrate $w $m device
  refused as expected: unsupported: no provider for device
mem.aos:13 › ! mem migrate $w $m remote
  refused as expected: unsupported: no provider for remote
mem.aos:14 › ! mem read $r $m 0 5
  refused as expected: denied: entity 2bd3ba332458 holds no read capability over memory:3cf91993a0cc
mem.aos:15 › mem share $w $m $r read
capability ff6fe6215f51d3680008c2f903c48345 (shared read)
mem.aos:16 › mem read $r $m 0 5
68656c6c6f  "hello" (5 bytes)
mem.aos:17 › ! mem write $r $m 0 x
  refused as expected: denied: entity 2bd3ba332458 holds no write capability over memory:3cf91993a0cc
mem.aos:18 › sysstat
...
Memory Fabric   1           4.0 KiB       3 migrations
...
mem.aos:19 › mem release $w $m
released
```

The id stays the same through every migration. Only `gen` and the type
change. Migration copies the bytes into a new mapping from the target provider
and gives the old mapping back (`MemoryFabric::migrate`,
`src/memory/memory_fabric.cpp`).

When a persistent object migrates away, the persistent provider is destroyed
and its `.mem` file under `<root>/memory/` is removed.

### Device memory

With accelerators on, an OpenCL device registers the `device` provider:

```
$ build/aios --root /tmp/aios-docs-res/acc exec 'entity create w => w' 'mem alloc $w 4096 => m' \
    'mem write $w $m 0 on-the-card' 'mem migrate $w $m device' 'mem read $w $m 0 11' 'mem list $w' \
    'mem migrate $w $m system' 'mem read $w $m 0 11' 'mem alloc $w 1024 device'
memory 4fae3e02f2d74b5b000f5356094cd285 (4.0 KiB system)
wrote 11 bytes
memory 4fae3e02f2d7 now device gen=2
6f6e2d7468652d63617264  "on-the-card" (11 bytes)
MEMORY ID     TYPE        SIZE        OWNER         GEN   BACKING
4fae3e02f2d7  device      4.0 KiB     58839da26e5b  2
memory 4fae3e02f2d7 now system gen=3
6f6e2d7468652d63617264  "on-the-card" (11 bytes)
memory 8c3f29bff1a24331000f5356096cd285 (1.0 KiB device)
```

Compute graphs migrate tensors onto a device by themselves when an operation
is placed there. See [compute.md](compute.md#devices-and-placement).

### Remote memory

`node connect` attaches a peer kernel and registers the `remote` provider.
Below, the peer ran on the same host on port 13503:

```
$ build/aios --root /tmp/aios-docs-res/peer exec 'node serve 13503 docs-token-0123456789' 'sleep 12' &
$ build/aios --root /tmp/aios-docs-res/local exec 'node connect 127.0.0.1 13503 docs-token-0123456789' \
    'entity create w => w' 'mem alloc $w 4096 => m' 'mem write $w $m 0 far-away' \
    'mem migrate $w $m remote' 'mem read $w $m 0 8' 'mem list $w' info
attached node 127.0.0.1:13503: 1 device(s) [remote:127.0.0.1:13503/cpu], provides remote memory, transport network:127.0.0.1:13503
memory 76cbd601a18f now remote gen=2
6661722d61776179  "far-away" (8 bytes)
MEMORY ID     TYPE        SIZE        OWNER         GEN   BACKING
76cbd601a18f  remote      4.0 KiB     9fe037e12673  2
...
  Memory Fabric   : 1 objects, 4.0 KiB (system 0 B, device 0 B, shared 0 B, persistent 0 B, remote 4.0 KiB), 1 migrations
  Devices         : cpu(e6c61e8f2b2d, 4 lanes, ...) remote:127.0.0.1:13503/cpu(3d18adb8a5ce, 4 lanes, 0 B/19.49 GiB vram)
```

The node token must be at least 16 characters, because it keys the encrypted
link. Peers, `node serve` and migration of whole entities are covered in
[network.md](network.md).

### Garbage collection

`mem gc` releases objects whose owner no longer exists. `entity terminate`
already releases an entity's memory, so on a healthy root it finds nothing:

```
Memory Fabric Garbage Collection: checked 1 object(s), released 0 orphaned object(s) (0 B reclaimed)
```

### Residency (not a console command)

`include/aios/kernel/residency_controller.hpp` classifies each entity as
Pinned (it has a running graph), Warm, or Sealed (idle under memory pressure),
and the object store evicts a sealed entity's decoded objects from its hot
cache. No console command exposes this. It is exercised by
`aios-residency-check` and `aios-unified-memory-store-bench`
([check tools](#check-tools)).

---

## Tensors

A tensor is a memory object read with a dtype and a shape. The dtype and shape
are not stored with the object: `tensor show` takes them again each time and
reinterprets the bytes.

| Command | |
|---|---|
| `tensor create <e> <dtype> <shape> [values...]` | allocate `elements × sizeof(dtype)` bytes of system memory and write the values. One value fills every element. |
| `tensor show <e> <mem> <dtype> <shape>` | read and print up to 64 values |

dtypes are `f32`, `f64`, `i32`, `i64` and `u8`. Shapes are written `2x3`.

```
tensor.aos:2 › tensor create $w f32 2x3 1 2 3 4 5 6 => t
tensor f32[2x3]@c2381c5d6379 memory c2381c5d6379a96a000f1de70ad68624
tensor.aos:3 › tensor show $w $t f32 2x3
f32[2x3]@c2381c5d6379 = [1 2 3 4 5 6]
tensor.aos:4 › tensor create $w i32 4 7 => u
tensor.aos:5 › tensor show $w $u i32 4
i32[4]@3e8929ca60c6 = [7 7 7 7]
tensor.aos:6 › mem list $w
3e8929ca60c6  system      16 B        eb0d7dfce7cc  1
c2381c5d6379  system      24 B        eb0d7dfce7cc  1
tensor.aos:7 › ! tensor create $w f16 2
  refused as expected: invalid: dtype: f32|f64|i32|i64|u8
tensor.aos:8 › mem migrate $w $t persistent
memory c2381c5d6379 now persistent gen=2 backing=.../memory/c2381c5d6379a96a000f1de70ad68624.mem
tensor.aos:9 › tensor show $w $t f32 3x2
f32[3x2]@c2381c5d6379 = [1 2 3 4 5 6]
tensor.aos:10 › tensor show $w $t i32 2x3
i32[2x3]@c2381c5d6379 = [1065353216 1073741824 1077936128 1082130432 1084227584 1086324736]
```

A tensor counts against the entity's allocation scope like any other memory.
Graphs that read and write tensors are in [compute.md](compute.md#compute-graphs).

---

## Budgets and refusals

Two different budgets apply:

- **Allocation scope**: how many bytes an entity may allocate. It is stored on
  a `memory:* allocate` capability as `scope_limit` and `scope_used`
  (`include/aios/capability/capability.hpp`).
- **Device-time quota**: how much device time an entity may spend running
  graphs. It is kept by the scheduler and managed with `quota`. See
  [compute.md](compute.md#budgets-preemption-mid-graph).

### Where scope comes from

- `entity create` gives the new entity `memory:* allocate` with a scope of
  `default_memory_quota`, which is 1 GiB (`include/aios/kernel/kernel.hpp`),
  plus `state:*`, `channel:* allocate` and `storage:* read`
  (`Kernel::spawn`, `src/kernel/kernel.cpp`).
- `cap grant <e> <kind> <id|*> <rights> [ttl_s] [scope_bytes]` grants a
  capability with a TTL (0 = none) and a scope (0 = unbounded).
- The kernel's self-improvement policy never mints an unbounded allocate
  capability. A petition for memory allocate gets the default quota, or 4 GiB
  if the quota is set to 0.

`cap list <e>` shows the scope as `scope=<used>/<limit>`:

```
budget.aos:2 › cap list $w
CAP ID        HOLDER        RESOURCE              RIGHTS                DETAILS
b5835d52ee2f  2d7d34a8eb75  storage:*             read
cb2ace6e6808  2d7d34a8eb75  memory:*              allocate                scope=0 B/1.00 GiB
17dbc39b6995  2d7d34a8eb75  channel:*             allocate
69124825e8c2  2d7d34a8eb75  state:*               read,write,destroy,allocate
```

### How a refusal reads

Below, the baseline 1 GiB capability is revoked and replaced by an 8 KiB one:

```
budget.aos:3 › cap find $w memory *
capability cb2ace6e6808965d00040c6f42ed5f4c memory:* allocate
budget.aos:5 › cap revoke $base
revoked (and every capability delegated from it)
budget.aos:6 › ! mem alloc $w 4096
  refused as expected: denied: entity 2d7d34a8eb75 holds no allocate capability over memory:*
budget.aos:7 › cap grant $w memory * allocate 0 8192
capability 5448d7086ee2998100040c6f432d5f4c (derived from the console's authority)
budget.aos:9 › mem alloc $w 4096 => a
budget.aos:10 › mem alloc $w 4096 => b
budget.aos:11 › ! mem alloc $w 1
  refused as expected: denied: entity 2d7d34a8eb75 holds no allocate capability over memory:*
budget.aos:12 › mem release $w $a
released
budget.aos:13 › mem alloc $w 4096
memory 2a90fcf7af12ecb500030aaa50bc3669 (4.0 KiB system)
```

There are two refusal shapes:

| When | Status and message |
|---|---|
| no capability covers it, or the only one is used up | `denied: entity X holds no allocate capability over memory:*` |
| a capability has scope left, but less than the request | `exhausted: no capability of X over memory:* has N scope remaining` |

```
budget2.aos:8 › mem alloc $w 4096
budget2.aos:9 › ! mem alloc $w 8192
  refused as expected: exhausted: no capability of 949b0b258bea over memory:* has 8192 scope remaining
```

The scope counts **bytes currently held**. Releasing memory gives its bytes
back to the capability that paid for them and to every ancestor that paid with
it (lines 12 and 13 above), and so does terminating the entity that held it
(`CapabilityManager::record_charge` / `refund_object`). A failed allocation is
not charged. The capability on a released object still stays in the holder's
list.

When an entity holds several allocate capabilities, one that is unbounded all
the way up its delegation chain satisfies the request without accounting.
Otherwise the request is charged to the one with the most room, where a
capability's room is the smallest remaining scope along it and its ancestors
(`CapabilityManager::charge`, `src/capability/capability.cpp`).

### Attenuation and revocation

`cap delegate <e> <cap> <to> <rights> [ttl_s]` passes on a subset of a
capability that `<e>` holds with `grant`. A delegation can only narrow: extra
rights, a longer expiry or a larger scope are refused. `cap revoke` removes a
capability and everything delegated from it:

```
budget2.aos:6 › cap grant $w memory * allocate,grant 0 8192
budget2.aos:10 › ! cap delegate $w $g $h allocate,write
  refused as expected: denied: delegation may only attenuate: requested write not held
budget2.aos:11 › cap delegate $w $g $h allocate 60
capability e3c85ac900b2c6120002b3b50022fb11 (delegated)
budget2.aos:12 › cap list $h
e3c85ac900b2  fd8c68850f5e  memory:*              allocate                scope=0 B/8.0 KiB  expires=+59.999 s  from=c6a6c9fa82d3
...
budget2.aos:13 › cap revoke $g
revoked (and every capability delegated from it)
budget2.aos:14 › cap list $h
(the delegated memory:* line is gone; the helper's own baseline remains)
```

A delegated budget is spent out of its parent's. At delegation the child's
scope is capped at what the parent has **left** (and a parent with nothing left
cannot delegate scope at all); afterwards every byte the child allocates is
charged to the child and to each bounded ancestor, and every release refunds
both. So an entity whose budget is used up cannot mint itself a fresh one, and
a helper cannot spend more than its delegator has:

```
$ build/aios --root /tmp/aios-fix-res/d1g exec ... \
    'cap grant $w memory * allocate,grant 0 8192 => c' 'mem alloc $w 8192 => a' '! mem alloc $w 1' \
    '! cap delegate $w $c $w allocate' 'mem release $w $a' 'mem alloc $w 4096 => b' \
    'cap delegate $w $c $h allocate => hc' 'mem alloc $h 4096 => hb' '! mem alloc $w 1' '! mem alloc $h 1' \
    'mem release $h $hb' 'mem alloc $w 4096'
memory ee07e100ae85d4ef… (8.0 KiB system)
  refused as expected: denied: entity 2261ae18265a holds no allocate capability over memory:*
  refused as expected: exhausted: delegation refused: capability 504f6fdb2d33 has no scope remaining (8192/8192 used, including what it has delegated)
released
memory 2a90fcf7af12… (4.0 KiB system)
capability 6721887ab1bf… (delegated)                      # $h gets scope=0 B/4.0 KiB: what $w had left
memory 05fa01a032f2… (4.0 KiB system)                     # $h's 4 KiB is charged to $w as well
  refused as expected: denied: entity 2261ae18265a holds no allocate capability over memory:*
  refused as expected: denied: entity 3954cebbaac6 holds no allocate capability over memory:*
released                                                  # $h releases: both get it back
memory 4e9cc6ded376… (4.0 KiB system)
```

(`$w`'s and `$h`'s baseline `memory:*` capabilities were revoked first; with
its own 1 GiB baseline, `$h` is charged to that, the one with most room.)

`cap check <e> <kind> <id|*> <right>` answers `permitted` or `denied` and
fails on `denied`, so a script can assert with it:

```
/dev/stdin:3 › cap check $w memory * allocate
permitted
/dev/stdin:4 › ! cap check $w storage * write
denied
  refused as expected: denied: capability check failed
```

Grants, refusals and petitions are recorded in `<root>/authority.log`. See
[console.md](console.md#entities-capabilities-and-authority) for the rest of
the authority model.

### Device-time quotas

```
/dev/stdin:5 › quota set $w budget=5000000
entity 957c56addd11 may spend 5.00 ms of device time every 1.000 s, in stretches of at most 5.00 ms
/dev/stdin:6 › quota list
ENTITY        LABEL             SLICE       BUDGET      USED        PERIOD    STOPS   STATE
957c56addd11  w                 5.00 ms     5.00 ms     0 ns        1.000 s   0       runnable
```

`quota set`, `clear` and `reset` need the console's operate right. A graph
that runs out of quota stops with `PREEMPTED` and can be resumed; the full run
is in [compute.md](compute.md#budgets-preemption-mid-graph).

---

## The object store and AQL

The store lives in `<root>/store/objects.log`, an append-only log that every
process opening the root reads back. An object carries an id, a timestamp, its
provenance (the entity that wrote it), an optional `derived_from` parent, an
optional source state, a 64-dimension embedding, metadata pairs, typed
relations to other objects, and the payload (`include/aios/storage/object_store.hpp`).
`store put` adds a `crypto.hash` metadata entry.

The embedding is a deterministic hashed byte-trigram vector
(`text_embedding`, `src/storage/aql.cpp`), with no model involved. Text that
shares character trigrams lands near itself.

### Commands

| Command | What it does | Needs |
|---|---|---|
| `store put <e> <text> [derived=<obj>] [rel=<obj>:<kind>] [k=v]...` | write an object as `<e>`; `kind` in `rel=` is a number | `storage:* write` |
| `store get [<e>] <obj> [show=<chars>]` | the object and its payload | `read` on the object |
| `store list` | every object | inspect |
| `store similar <text…> [k]` | the k nearest by embedding, with scores | inspect |
| `store related <obj>` | the objects `<obj>` points to, with relation kinds | inspect |
| `store history <obj>` | the `derived_from` chain, newest first | inspect |
| `store meta <key> [value]` | objects with that metadata key (and value) | inspect |
| `store snapshot <e> <state>` | store a copy of a state object | `read` on the state |
| `store query [<e>] <AQL>` | an AQL statement (below) | read is scoped per object |
| `store status` | dense/cold object counts, sealed blocks, hot-cache use | inspect |
| `store seal` | seal active objects into cold sparse blocks | |
| `store index` / `store index bench [n] [k]` | similarity index state; recall and speed on synthetic vectors | inspect |

A new entity holds only `storage:* read`. Grant `write` before it can `store put`:

```
store.aos:2 › cap grant $c storage * read,write
store.aos:3 › store put $c "invoice 1001 for pallets of bricks" kind=invoice customer=acme total=420 => inv
object 235198a681c6a537000522f272484713
store.aos:4 › store put $c "invoice 1002 for cement bags" kind=invoice customer=bolt total=95 => inv2
store.aos:5 › store put $c "invoice 1001 corrected: pallets of red bricks" derived=$inv kind=invoice customer=acme total=440 => fix
store.aos:6 › store put $c "delivery note for acme bricks" kind=delivery rel=$fix:7 => dn
store.aos:7 › store list
OBJECT ID     TIMESTAMP   PROVENANCE    EMBED   PAYLOAD / DETAILS
235198a681c6  1790893890s 95a2c2cf8581  64d     "invoice 1001 for pallets of bricks" (34 bytes) kind=invoice customer=acme total=420 crypto.hash=9b7f…
...
05ae0a517fe6  1790893890s 95a2c2cf8581  64d     "delivery note for acme bricks" (29 bytes) kind=delivery crypto.hash=8428… rel(7)→e7e68c1bff2d
store.aos:8 › store similar pallets of bricks 3
0.779  235198a681c6  ... "invoice 1001 for pallets of bricks" ...
0.660  e7e68c1bff2d  ... "invoice 1001 corrected: pallets of red bricks" ...
0.162  05ae0a517fe6  ... "delivery note for acme bricks" ...
store.aos:9 › store related $dn
kind 7  e7e68c1bff2d  ... "invoice 1001 corrected: pallets of red bricks" ...
store.aos:10 › store history $fix
e7e68c1bff2d  ... "invoice 1001 corrected: pallets of red bricks" (45 bytes) [derived=235198a681c6] ...
235198a681c6  ... "invoice 1001 for pallets of bricks" (34 bytes) ...
store.aos:11 › store meta customer acme
(the two acme invoices)
```

### AQL

`store query` runs one statement through the capability boundary: the result
only includes objects the calling entity may read. The grammar
(`parse_aql`, `src/storage/aql.cpp`):

```
SELECT * | COUNT(*) [FROM objects]
  [WHERE <field> <op> <value> [AND|OR [NOT] <field> <op> <value>]...]
  [NEAR <text> | NEAR embedding_of(<text>) | NEAR [v1, v2, ...]]
  [RELATED TO <obj> [KIND <n|name>]]
  [HISTORY OF <obj>]
  [AS OF <timestamp_ns> | AS OF TIMESTAMP(<ns>) | AS OF CHECKPOINT(<entity>, <n>) | AS OF <path>]
  [ORDER BY timestamp|id|meta.<key> [ASC|DESC]]
  [LIMIT n]

PUT | INSERT [INTO objects] <payload-word> [key=value]... [derived=<obj>]
```

- Fields: `meta.<key>`, `id`, `timestamp`, `derived_from`, `provenance`,
  `source_state`.
- Operators: `=` `==` `!=` `<` `<=` `>` `>=` `LIKE` `CONTAINS`. Ordering
  comparisons are numeric when both sides parse as numbers.
- `OR` splits the `WHERE` into alternatives. `AND` binds tighter.
- The default order is `timestamp DESC`, except that `NEAR` without an
  `ORDER BY` keeps the similarity ranking (nearest first), so `LIMIT` keeps the
  nearest.
- `KIND` takes a number, a hex number, `derived_from`, or a name (hashed).
- In the console, the statement must start with an upper-case `SELECT`, `PUT`
  or `INSERT`. Otherwise the first word is read as an entity name.
- The statement is taken from the line as typed, quotes included, so
  `NEAR "red bricks"`, `WHERE meta.kind = 'b c'` and `PUT "via aql" kind=x`
  mean what they say. `$name` outside quotes is a console variable, so
  `RELATED TO $b` works, and `=> name` binds the first row's id.
- Object ids are full 32-hex-digit ids (an `obj:` prefix is accepted). A word
  is a number only when all of it is one, so an id that starts with a digit is
  an id. `RELATED TO` / `HISTORY OF` with something that is not an id is an
  error. `RELATED TO` lists each related object once.
- `PUT` goes through the caller's Surface: it needs `storage write`, and the
  object's provenance is the caller.

From a new process on the same root (the store is persistent; the entity that
wrote the objects is not):

```
$ build/aios --root /tmp/aios-docs-res/store exec \
   'store query SELECT * FROM objects WHERE meta.kind = "invoice" ORDER BY meta.total DESC' \
   'store query SELECT COUNT(*) FROM objects WHERE meta.customer = "acme"' \
   'store query SELECT * WHERE meta.total > 100 AND NOT meta.customer = "bolt" OR meta.kind = "delivery"'
AQL Capability-Scoped Results (3 object(s)):
  [e7e68c1bff2d] ts=1790893890903231593 meta={kind=invoice, customer=acme, total=440, crypto.hash=…}
  [235198a681c6] ts=1790893890902077407 meta={kind=invoice, customer=acme, total=420, crypto.hash=…}
  [411916dc007b] ts=1790893890902691260 meta={kind=invoice, customer=bolt, total=95, crypto.hash=…}
COUNT(*) = 2
AQL Capability-Scoped Results (3 object(s)):
  [05ae0a517fe6] ts=1790893890903754017 meta={kind=delivery, crypto.hash=…}
  [e7e68c1bff2d] ts=1790893890903231593 meta={kind=invoice, customer=acme, total=440, crypto.hash=…}
  [235198a681c6] ts=1790893890902077407 meta={kind=invoice, customer=acme, total=420, crypto.hash=…}

$ build/aios --root /tmp/aios-docs-res/store exec 'store query SELECT * HISTORY OF e7e68c1bff2d9edd000522f272684713'
AQL Capability-Scoped Results (2 object(s)):
  [e7e68c1bff2d] ts=1790893890903231593 meta={kind=invoice, customer=acme, total=440, ...}
  [235198a681c6] ts=1790893890902077407 meta={kind=invoice, customer=acme, total=420, ...}
```

Time travel by timestamp. The first object was written before a checkpoint,
the second after:

```
/dev/stdin:3 › store put $c before kind=t
/dev/stdin:4 › checkpoint $c
checkpoint /tmp/aios-docs-res/asof/checkpoints/8b7bfd4486b6bfa2000caf99d570e1d5/1.aoc
/dev/stdin:6 › store put $c after kind=t
$ build/aios --root /tmp/aios-docs-res/asof exec 'store query SELECT * WHERE meta.kind = t AS OF 1790893974550000000'
AQL Capability-Scoped Results (1 object(s)):
  [468edbfd9af4] ts=1790893974530349263 meta={kind=t, ...}
```

`AS OF CHECKPOINT(<entity>, <n>)` reads the timestamp from
`<root>/checkpoints/<entity>/<n>.aoc`. An `AS OF` that resolves to nothing — no
such file, a file that does not verify, a path that is not a checkpoint — is an
error naming what was tried; it never runs unbounded:

```
store query SELECT * WHERE meta.kind = t AS OF CHECKPOINT($c, 1)     # $c = 5d873ed3…: 1 object (before)
! store query SELECT * AS OF CHECKPOINT(nosuch, 3)
  refused as expected: invalid: AQL error: AS OF names no time this machine can resolve — no readable, verified checkpoint at /tmp/aios-fix-res/aql4/checkpoints/nosuch/3.aoc, /tmp/aios-fix-res/aql4/checkpoints/nosuch.aoc
! store query SELECT * RELATED TO 12ab
  refused as expected: invalid: AQL error: invalid: RELATED TO needs a full object id (32 hex digits), got '12ab'
```

Errors look like this:

```
FAILED: invalid: AQL error: invalid: incomplete predicate in WHERE near 'meta.total' — expected <field> <operator> <value>
FAILED: not-found: no identity matches 'DELETE'        (store query DELETE FROM objects)
FAILED: not-found: no identity matches 'select'        (a lower-case verb)
```

There is no delete verb.

#### Quoting, ids, `NEAR` order and `PUT` authority

Quoted literals keep their spaces, ids that start with a digit resolve, `NEAR`
keeps its similarity order, and `PUT` needs write authority:

```
! store query $w PUT payload=sneaky kind=x          # $w holds only storage:* read
  refused as expected: invalid: AQL error: denied: entity 2ef29f3c3932 holds no write capability over storage:*
store query $w PUT 'via aql two words' kind=x => o   # after cap grant $w storage * read,write
Inserted object 6327b8c535714f020009583bc061123e
store get $o
6327b8c53571  1790897418s 2ef29f3c3932  64d     "via aql two words" (17 bytes) kind=x ...
store query SELECT * HISTORY OF 70348e75dc41a5270008c93a035d3d39
AQL Capability-Scoped Results (2 object(s)): ...
```

### Similarity index

Under 256 vectors, similarity is an exact scan. Above that it uses an HNSW
graph (`src/memory/hnsw_index.cpp`), built by the first query that needs it.
`store index` reports which path a query takes. `store index bench <n>` builds a
scratch index of synthetic vectors and measures recall and speed. This was run
at n=512, a small setting:

```
$ build/aios --root /tmp/aios-docs-res/store exec 'store status' 'store index' 'store index bench 512'
ObjectStore Tiering & Sparse Block Index Status
  total objects       : 5
  active (dense map)  : 5
  cold (sparse blocks): 0 objects across 0 sealed block(s)
  sparse index memory : 0 bytes in RAM
  decoded hot cache   : 0 KiB / 512 MiB (0 objects resident)
Similarity index
  vectors held        : 5
  in the graph        : 0  (not built yet: built by the first query that uses it)
  path a query takes  : the exact linear scan
  ...
too few vectors to measure recall against; add objects with embeddings first
Scratch index of 512 synthetic 64-d vectors
  built in            : 18.743 ms
  in the graph        : 512
Measured over 32 queries, k=5:
  recall@5          : 100%
  exact scan          : 22.5664 us/query
  graph               : 18.112 us/query
  the graph is 1.24594x the scan's speed - FASTER
```

---

## kv and state

A **state object** is a small typed payload owned by an entity, with a
generation. Kinds are `working`, `persistent`, `context`, `relationship` and
`execution`.

| Command | |
|---|---|
| `state create <e> <kind> [text]` | new state object |
| `state set <e> <s> <text>` | replace the payload (bumps `gen`) |
| `state get <e> <s>` | read it |
| `state attach <e> <s> <mem>` | link a memory object to it |
| `state keep <e> <s>` | keep it across boots (the owner must be a resident) |
| `state list [<e>]` | all states |

`kv` is a key/value view over the console entity's `working` states. Each key
is a state whose payload is `kv:<key>=<value>`.

| Command | |
|---|---|
| `kv set <key> [value...]` | create, or update in place (bumps `gen`) |
| `kv get <key>` | print the value; `not-found` if absent |
| `kv del <key>` (`delete`, `remove`, `rm`) | empty the state |
| `kv list` (`ls`) | keys, values, state ids, generations |
| `kv clear` | empty every kv state |

```
$ build/aios --root /tmp/aios-docs-res/kv exec 'kv set colour deep blue' 'kv set size 3' 'kv set size 4' \
    'kv get colour' 'kv list' 'kv del size' 'kv list' 'state list'
kv set: colour = deep blue (state 6ea99a6c3046)
kv set: size = 3 (state 8c761025b08c)
kv updated: size = 4 (state 8c761025b08c)
deep blue
KEY             VALUE                   STATE ID        GEN
colour          deep blue               6ea99a6c3046    0
size            4                       8c761025b08c    1
deleted key 'size'
KEY             VALUE                   STATE ID        GEN
colour          deep blue               6ea99a6c3046    0
STATE ID      KIND          OWNER         GEN   PAYLOAD
51186ef936ad  context       210fa577a80b  0     "" (0 bytes)
6ea99a6c3046  working       b1031df22268  0     "kv:colour=deep blue" (19 bytes)
8c761025b08c  working       b1031df22268  2     "" (0 bytes)

$ build/aios --root /tmp/aios-docs-res/kv exec 'kv list' 'kv get colour'
(the entries are there: see below)
```

**Kept states.** A state can be kept across boots, in
`<root>/states/<id>.state` (`Surface::keep_state`, restored in `Kernel::init`).
Only a resident's state can be kept, and only by its owner, because no other
entity outlives the process. `kv` keeps every entry it writes (the console is
a resident). `state create <e> persistent ...` keeps the state when `<e>` is a
resident and says so when it is not; `state keep <e> <s>` keeps any state of a
resident. Memory references are dropped on restore, since memory objects belong
to the process that made them. Other states (a mind's per-cycle states, any
entity's working state) still live only in the process.

`kv del` empties the state rather than destroying it, so the empty state
remains in `state list` and on disk.

---

## What a checkpoint holds

`checkpoint <e>` writes `<root>/checkpoints/<entity>/<n>.aoc`, numbered past
whatever is already on disk. `restore <path>` brings the entity back under the
same id. `rewind <path>` puts a live entity back to the image.

The file is a 24-byte header (`AOKC`, version 2, body length, FNV-1a checksum)
followed by a body in the kernel's own encoding (`src/kernel/checkpoint.cpp`):

| In the image | Notes |
|---|---|
| the kernel it was taken on, and when | used by `AS OF CHECKPOINT` |
| the entity record | id, label, relationships, execution position |
| its state objects | kind, generation, payload |
| every memory object it owns, **with contents** | id, size, type, permissions, generation, the bytes |
| the capabilities it holds | |
| graphs it owns | operations keep their device requirement as written, so a graph stays runnable on a kernel with different devices |
| channels it owns | ring metadata and subscriber cursors; the ring bytes are in its shared memory region |

It is not a dump of process memory, and authority does not travel in it. On
restore, the entity keeps authority over what the image recreates (its own
memory, states and channels). Everything else is re-issued as a delegation from
the restorer's authority, or dropped.

```
$ build/aios --root /tmp/aios-docs-res/ckpt exec 'entity create job => e' 'mem alloc $e 64 persistent => m' \
    'mem write $e $m 0 survives' 'state create $e working step-3 => s' 'state attach $e $s $m' \
    'channel create $e 4 64' 'checkpoint $e' 'mem list $e'
entity b8e1532002fe864f000757826f234bb4 (job)
memory a3faac81857239d6000757826f734bb4 (64 B persistent)
wrote 8 bytes
state ec65bf0e87e721f8000757826f934bb4 (working)
attached
channel 28d48b9bb8182902000757826fb34bb4 (4 frames × 64 B, transport shared-memory, region 8e1d01540752)
checkpoint /tmp/aios-docs-res/ckpt/checkpoints/b8e1532002fe864f000757826f234bb4/1.aoc
MEMORY ID     TYPE        SIZE        OWNER         GEN   BACKING
8e1d01540752  shared      584 B       b8e1532002fe  0
a3faac818572  persistent  64 B        b8e1532002fe  1     /tmp/aios-docs-res/ckpt/memory/a3faac81857239d6000757826f734bb4.mem
```

A new process: the entity is gone until it is restored, and then it comes back
with its memory, state, capabilities and channel:

```
$ build/aios --root /tmp/aios-docs-res/ckpt exec 'mem list' 'restore $P' 'entity show b8e1532002fe…' \
    'mem list b8e1…' 'cap list b8e1…' 'state list b8e1…' 'channel list' 'mem read b8e1… a3faac818572 0 8'
MEMORY ID     TYPE        SIZE        OWNER         GEN   BACKING
restored entity b8e1532002fe864f000757826f234bb4 (job) states=1 memory=2 caps=6 restores=1
b8e1532002fe (b8e1532002fe864f000757826f234bb4)
  label=job status=active gen=12 states=1 memory=2 caps=6 checkpoints=1 restores=1
    parent → 5c0fd19e1488
MEMORY ID     TYPE        SIZE        OWNER         GEN   BACKING
a3faac818572  persistent  64 B        b8e1532002fe  2     /tmp/aios-docs-res/ckpt/memory/a3faac81857239d6000757826f734bb4.mem
8e1d01540752  shared      584 B       b8e1532002fe  1
CAP ID        HOLDER        RESOURCE              RIGHTS                DETAILS
f3c00d94ab20  b8e1532002fe  channel:*             allocate                from=472132ff971d
1218835e2bd7  b8e1532002fe  storage:*             read                    from=83903d8c89b6
b051f927ac8d  b8e1532002fe  memory:*              allocate                scope=0 B/1.00 GiB  from=3ab6267296e8
d6a67ee92d40  b8e1532002fe  state:*               read,write,destroy,allocate  from=a569a8b81656
ca8c0de139c7  b8e1532002fe  channel:28d48b9bb818  grant,destroy,publish,consume
4db236c70639  b8e1532002fe  memory:a3faac818572   read,write,grant,destroy
STATE ID      KIND          OWNER         GEN   PAYLOAD
ec65bf0e87e7  working       b8e1532002fe  1     "step-3" (6 bytes)
28d48b9bb818  owner=b8e1532002fe capacity=4 slot=64 B transport=shared-memory head=0 tail=0 dropped=0 subscribers=1
7375727669766573  "survives" (8 bytes)

$ xxd .../1.aoc | head -1
00000000: 414f 4b43 0200 0000 4a07 0000 0000 0000  AOKC....J.......
```

The re-issued allocate capability starts at `scope=0 B` again. The scope used
before the checkpoint is not carried over.

Checkpointing in the middle of a graph run is in
[compute.md](compute.md#checkpoint-and-resume-mid-graph), and checkpoint files
as roots of a machine in [console.md](console.md#what-a-root-is).

---

## The sidecars

The sidecar code (`src/sidecar/`, `include/aios/sidecar/`) is a set of
in-process subsystems linked into `libaios.a`, plus two programs that use
them. None of them is a console command.

| Subsystem | Header | What it is |
|---|---|---|
| shared-memory ring | `shm_abi.hpp` | `ShmRingChannel`: a single-producer ring in a POSIX shm segment (`/dev/shm/<name>`) or an anonymous mapping (`anon`), with 32-byte frame headers. The creator unlinks the segment when it closes. `compute serve shm_ring=NAME` uses it ([compute.md](compute.md#compute-serve-the-compute-service)). |
| event fabric | `event_fabric.hpp` | topics with partitions; 256-byte messages (232-byte inline payload) in fixed rings; consumers poll with a cursor |
| binary telemetry | `binary_telemetry.hpp` | a RAM ring of 64-byte events; query by trace id or time window; p50/p95/p99 per service |
| filesystem mirror | `fs_topology.hpp` | an in-memory radix tree of paths with `fast_lstat`, `fast_du`, `fast_find`, `ingest_host_tree` |
| module vault | `module_vault.hpp` | content-addressed blobs by SHA-256, package manifests, dependency resolution, dedup accounting |
| capability guard | `capability_guard.hpp` | 48-byte tokens (object id, rights, generation, expiry, truncated HMAC-SHA256) signed with a master key; attenuate, validate, revoke by bumping a resource's generation |
| C ABI | `aios_sidecar_c_abi.h` | flat C functions for other languages (below) |

These token rights (`READ WRITE EXECUTE DELETE_OP DELEGATE ADMIN`) are separate
from the kernel's capabilities in [Budgets](#budgets-and-refusals). A token is
checked only by the `CapabilityGuard` that holds the same key.

### `aios-engine`

`aios-engine` boots a kernel on `--root`, starts the ASQL database and its
MySQL-wire server, and constructs the sidecar subsystems
(`tools/aios-engine.cpp`). The SQL side is documented in [asql.md](asql.md).
Its flags, all `--name=value`:

| Flag | Default | Used for |
|---|---|---|
| `--root=` | `/tmp/aios-engine-data` | kernel root; holds `capability.key` |
| `--bind=` / `--port=` | `0.0.0.0` / `3307` | MySQL-wire listener |
| `--sql-user=`, `--sql-pass=`, `--sql-pass-file=` | none | an account on the wire server (not in `--help`) |
| `--trust-loopback` | off | let loopback clients in without an account (not in `--help`) |
| `--upstream=`, `--upstream-user=`, `--upstream-pass=`, `--upstream-db=`, `--tables=` | none, `root`, none, `appdb`, all | CDC from a MariaDB/MySQL binlog |
| `--mode=` | `local` | `write_behind` or `write_through` sync to upstream |
| `--shm-ring=` | `aios_ring` | name of the 4 MB shm ring it creates (nothing in the engine writes to it) |
| `--telemetry-slots=` | 131072 | size of the telemetry ring |
| `--benchmark` | off | time 50,000 ring, telemetry and event-fabric operations at start |
| `--watch-fs=` | none | mirror every file and directory under it into the FS topology; rescanned every 60 s |

What the engine does with the sidecars: it builds them and says they are idle,
because nothing in the run loop feeds the telemetry ring, the event fabric, the
module vault or the shm ring, and no token is minted or checked. Only
`--benchmark` exercises them. `--shm=` was never used and is gone (it is now an
unknown option). `--watch-fs` really walks the tree (size, mode, owner, mtime
per entry) at start and every 60 s, dropping entries that disappeared:

```
    - Capability Guard       : key loaded; idle (the engine mints and checks no tokens)
    - Binary Telemetry Core  : 131072 slots allocated; idle (no query is recorded into it)
    - Event Fabric           : 3 topics registered; idle (nothing publishes)
    - SHM Ring Channel       : /aios_fixres_ring created (4 MB); idle (the engine writes nothing to it)
    - FS Topology Mirror     : 2 file(s), 2 dir(s), 9 bytes under /tmp/aios-fix-res/wfs (rescanned every 60 s)
...
[aios-fs] /tmp/aios-fix-res/wfs: 2 file(s), 2 dir(s), 11 bytes, 1 gone since the last scan
$ build/aios-engine --watch-fs=/nonexistent ...
[!] --watch-fs=/nonexistent: /nonexistent is not a readable directory
```

#### The capability master key

There is no built-in key. At start the engine takes:

1. `AIOS_CAPABILITY_KEY` from the environment, if set and non-empty; else
2. the first line of `<root>/capability.key`; else
3. 32 random bytes from `/dev/urandom`, written as hex to
   `<root>/capability.key` with mode 0600, and announced once.

If none of these works, it exits with `No capability key: set
AIOS_CAPABILITY_KEY or make <root>/capability.key readable and writable`.
Machines that must accept each other's tokens need the same key file
(`scripts/aios-setup.sh --key-from`).

The first start on a fresh root, then stopped with SIGTERM (driven before the
sidecar lines were made true; they now read as above, and the `POSIX SHM
Fabric` line is gone):

```
$ build/aios-engine --root=/tmp/aios-docs-res/engine --bind=127.0.0.1 --port=13500 \
    --shm-ring=aios_docs_ring --trust-loopback
  [Configuration Summary]
  • Root Directory       : /tmp/aios-docs-res/engine
  • MySQL Wire Server    : 127.0.0.1:13500 (5.7.44-aios-asql)
  • Local UNIX Socket    : /run/aios/compute.sock
  • POSIX SHM Fabric     : aios_fabric (Lockless Multi-Partition Rings)
  • Zero-Copy SHM Ring   : aios_docs_ring
  • Binary Telemetry     : 131072 64-byte slots in RAM (Zero Disk IO)
  • Sync Mode            : Local-Only (write-behind OFF)
  ✓ AIOS Microsecond Kernel booted successfully.
  • Capability key       : generated /tmp/aios-docs-res/engine/capability.key (keep it private; share it only between machines that must trust each other)
    - Capability Guard       : Active (128-bit MAC tokens initialized)
    - Binary Telemetry Core  : Active (131072 ring slots, p50/p95/p99 ready)
    - Event Fabric           : Active (3 topics initialized)
    - SHM Ring Channel       : Active (4 MB linear ring buffer)
  ✓ In-Memory Multi-Tenant ASQL Engine online.
  ! No SQL account set: only loopback clients will be let in.
  ✓ MySQL Native Wire Gateway listening on 127.0.0.1:13500
...
[aios-engine] Initiating graceful shutdown...
  ✓ MySQL Wire Server stopped.
  ✓ Store checkpointed to disk.
[aios-engine] Shutdown complete. Goodbye.

$ ls -la /tmp/aios-docs-res/engine/capability.key       # while it ran
-rw------- 1 arch arch 65 Oct  1 18:34 /tmp/aios-docs-res/engine/capability.key
$ ls -la /dev/shm | grep aios_docs                       # while it ran; gone after SIGTERM
-rw-rw----  1 arch arch 4194368 Oct  1 18:34 aios_docs_ring
```

On a second start the key file is reused unchanged, and no "generated" line is
printed. With `AIOS_CAPABILITY_KEY` set, no file is written. A client can reach
the wire server:

```
$ mysql -h127.0.0.1 -P13500 -uroot -e 'SELECT 1+1 AS two'
two
2
```

Without an account and without `--trust-loopback`, it refuses to listen:

```
[!] Refusing to open 127.0.0.1:13500 with no account: pass --sql-user= and --sql-pass-file=, or --trust-loopback for a local-only engine.
```

`--benchmark` (fixed at 50,000 operations each; it ran before the refusal
above):

```
  [Executing Self-Diagnostic Microsecond Benchmark]
    • SHM Ring Channel       : 11420518.33 ops/sec (Zero Copy)
    • Binary Telemetry Core  : 26110737.73 events/sec (0 disk IO)
    • Event Fabric Pub/Sub   : 10372621.92 msgs/sec
```

Use `--sql-pass-file=` rather than `--sql-pass=`, because a password on the
command line is visible in `ps`.

### `aios-sidecar`

`aios-sidecar` is a command-line front end to the C ABI. It takes a subcommand
and its arguments, with no flags (`tools/aios-sidecar.cpp`). Input files can be
`-` for stdin.

| Subcommand | Result here |
|---|---|
| `status` (`report`) | one check: the key `0x…01` must derive the known address `0x7E5F…5Bdf`; exit 1 otherwise |
| `sidecar-check` | the same single check (not in the usage text) |
| `chain eth-addr <privkey_hex>` | checksummed Ethereum address |
| `chain btc-addr <pubkey_hex> [1]` | P2WPKH address from a 33-byte compressed key; `1` = testnet |
| `comms eml\|ical\|vcard\|feed [file\|-]` | parse to JSON. `eml` falls back to the `Date:` header; `ical` gives `uid summary dtstart dtend location description` and unfolds lines and `NAME;PARAM=…:` properties; `vcard` reads `EMAIL;TYPE=…:` and friends |
| `svg <in.svg> <out.png> [w] [h]` | rasterize to PNG |
| `pdf <in.html> <out.pdf> [width]` | one-page PDF 1.4 (Helvetica metrics, the page's own `<style>`, escaped text, a real xref table) |
| `tsp <lat,lng;lat,lng;...>` | closed tour from point 0: nearest-neighbour seed plus 2-opt / Or-opt (`erp::route_improve`); the order is a permutation starting at 0 |
| `grain <code\|file.grain>` | run grain with a 50,000-step limit |
| `patch-check <diff\|-> [root]` | check every hunk's context and deleted lines against the files under `root`; exit 0 only when it applies; nothing is written |
| `rpc` | JSON-RPC 2.0 on stdio, one request per line; `methods` lists `sha256 keccak256 eth_addr btc_addr comms.* tsp grain patch_check cap.mint cap.verify cap.attenuate cap.revoke`; unknown methods and bad params are JSON-RPC errors |

```
$ build/aios-sidecar pdf a.html a.pdf && pdftotext a.pdf -
Rendered PDF to a.pdf (760 bytes)
Invoice (draft)
Total: 42 dollars
$ build/aios-sidecar tsp '40.71,-74.00;34.05,-118.24;41.87,-87.62;29.76,-95.36'
Optimized route: [0, 2, 1, 3] Distance: 8.43632e+06 meters        # the brute-force optimum
$ build/aios-sidecar patch-check bad.patch
does not apply (1 file(s), 1 hunk(s)): Dry-run mismatch in f.txt at line 2:
  Expected: "WRONG"
  Actual:   "line2"
$ build/aios-sidecar patch-check good.patch
applies cleanly: 1 file(s), 1 hunk(s), checked against . (nothing written)
$ build/aios-sidecar comms eml a.eml
{"from":"alice@example.com","to":"bob@example.com","subject":"Hi","date":"Wed, 01 Oct 2026 09:30:00 -0500",...}
$ build/aios-sidecar comms ical a.ics
{"events":[{"uid":"e1","summary":"Standup","dtstart":"20261001T090000Z","dtend":"20261001T091500Z","location":"Room 4","description":""}],"count":1}
$ printf '%s\n' 'not json' '{"jsonrpc":"2.0","id":1,"method":"tsp","params":{"points":[[40.71,-74.0],[34.05,-118.24],[41.87,-87.62],[29.76,-95.36]]}}' \
    '{"jsonrpc":"2.0","id":2,"method":"nosuch"}' | build/aios-sidecar rpc
{"jsonrpc":"2.0","id":null,"error":{"code":-32700,"message":"parse error: unexpected character at offset 0"}}
{"jsonrpc":"2.0","id":1,"result":{"order":[0,2,1,3],"distance_m":8436317.4}}
{"jsonrpc":"2.0","id":2,"error":{"code":-32601,"message":"method not found: nosuch (try \"methods\")"}}
```

A capability minted by `rpc` lives as long as that `rpc` process (or anywhere
`AIOS_CAPABILITY_KEY` is shared); see the C ABI below.

### The C ABI from other languages

`include/aios/sidecar/aios_sidecar_c_abi.h` is plain C with `extern "C"`.
Functions return `AIOS_OK` (0), `AIOS_ERR_INVALID`, `AIOS_ERR_NOT_FOUND`,
`AIOS_ERR_DENIED`, `AIOS_ERR_BUFFER_TOO_SMALL` or `AIOS_ERR_EXECUTION`, and
write into caller-provided buffers. The groups are:

| Group | Functions |
|---|---|
| chain | `aios_chain_eval_script`, `aios_chain_btc_p2wpkh_address`, `aios_chain_btc_create_p2wpkh_tx`, `aios_chain_eth_sign_tx`, `aios_chain_eth_derive_address` |
| comms | `aios_comms_parse_eml_json`, `_ical_json`, `_vcard_json`, `_rss_json` |
| capabilities | `aios_cap_mint`, `aios_cap_verify`, `aios_cap_attenuate`, `aios_cap_revoke` (see below) |
| checkpoints | `aios_checkpoint_serialize`, `aios_checkpoint_validate` (the 24-byte `AOKC` header) |
| rendering | `aios_svg_render_png`, `aios_svg_render_rgba`, `aios_pdf_render_html` |
| routing | `aios_haversine_distance` (metres), `aios_tsp_solve_2opt` |
| grain | `aios_grain_run` |
| patches | `aios_patch_check_dry_run`, `aios_patch_check_dry_run_ex` |
| crypto | `aios_crypto_sha256`, `_keccak256`, `_secp256k1_sign`, `_secp256k1_recover`, `_ed25519_sign`, `_ed25519_verify` |

The build also produces `build/libaios_sidecar.so` (CMake target
`aios_sidecar`): the C ABI with the rest of libaios linked in behind it. Only
the `aios_*` functions are exported (29 of them). From C:

```
$ gcc -I include t.c -L build -laios_sidecar -Wl,-rpath,$PWD/build -o t && ./t
sha256(abc)=ba7816bf...
mint rc=0 len=96 | verify write=1 other-subject=0 | attenuate rc=0 child read=1 child write=0 | re-attenuate rc=3 (3=denied) | after revoke=0
tsp order=[0,2,1,3] 8436317 m
```

From Python `ctypes`:

```python
lib = ctypes.CDLL("build/libaios_sidecar.so")
cap = ctypes.create_string_buffer(97)
lib.aios_cap_mint(b"admin", b"worker", b"table:orders", ctypes.c_uint16(1 | 16), cap, 97)   # 0
ok = ctypes.c_int()
lib.aios_cap_verify(cap, b"worker", b"table:orders", ctypes.c_uint16(1), ctypes.byref(ok))  # ok = 1
# write (not granted) -> 0; one hex digit changed -> 0; after aios_cap_revoke -> 0
```

The capability calls run on the real `CapabilityGuard`. A cap id is the hex of
the signed 48-byte token (`AIOS_CAP_ID_LEN` = 96 characters, so buffers need
97), bound to subject and resource, with the guard's rights bits (1 read,
2 write, 4 execute, 8 delete, 16 delegate, 32 admin). `aios_cap_attenuate`
needs the parent to hold delegate and returns `AIOS_ERR_DENIED` otherwise;
`aios_cap_revoke(subject, resource)` invalidates every token minted so far for
the pair, children included. Tokens are signed with `AIOS_CAPABILITY_KEY` when
it is set, else with a random per-process key, so without the variable a token
is only good inside the process that minted it. The actor argument is required
but advisory: the key is the authority. `aios_patch_check_dry_run` checks
the hunks against the files in the working directory (`_ex` takes a root and
returns the reason).

---

## Check tools

These are standalone programs in `build/`. Each prints PASS/FAIL lines and
exits non-zero on failure. Those that make their own root with `mkdtemp` under
`/tmp` (`residency-check`, `unified-memory-store-bench`, `sidecars-check`,
`parallel-scan-verify`, `aql-check`) remove it when they finish, and
`volume-check` removes its default `/tmp/aios-volume.img` (an image path you
name is left alone).

| Tool | What it drives | Result here |
|---|---|---|
| `aios-sidecars-check` | shm ring (anonymous), filesystem mirror (5,000 files), telemetry (50,000 events), module vault, event fabric, capability guard, multi-tenant ASQL | 10026/10026 passed. 10,000 of the checks are one repeated `fast_lstat` lookup. |
| `aios-module-vault-bench` | blob store/retrieve by SHA-256, dedup of 1,000 copies, package file resolution, 3-level dependency tree | all passed |
| `aios-residency-check` | Pinned/Warm/Sealed classification, eviction under a 32 KB hot budget, compositor focus signals | all passed, 0.36 s |
| `aios-unified-memory-store-bench` | system/shared/persistent allocation and migration, store batch insert (5,000), meta hash/numeric/range indexes, exact vs HNSW search, residency tiering, cold sealing | all passed, 0.11 s |
| `aios-volume-check [image]` | the AOS volume on a 4 MiB file-backed device: format, write, append, remount, compaction, torn tail | `VOLUME: PASS` |
| `aios-parallel-scan-verify` | 20,000 SQL rows, `GROUP BY` and filtered aggregation | passed |
| `aios-shm-fabric-grain-integration-bench` | shm-fabric channel transport, `compute_serve` over a zero-copy SHM ring, and grain's sidecar builtins | `ALL SHM FABRIC & USERLAND SIDECAR INTEGRATION TESTS PASSED` |

Excerpts:

```
$ build/aios-sidecars-check
[Step 1] Testing Zero-Copy Shared Memory ABI Ring Channel...
  [PASS] SHM Ring: 100000 push/pop ops in 7.20 ms (13895563 ops/sec, zero-copy!)
[Step 2] Testing In-Memory Filesystem Radix Topology Mirror...
  [PASS] FS topology node count: 5014 nodes indexed in RAM
  [PASS] Fast lstat latency: 404.1 ns/lookup (in RAM)
[Step 3] Testing Ring-Buffer Binary Telemetry Core...
  [PASS] Binary telemetry p50/p99 ordering: p50=300us p99=496us
[Step 6] Testing 128-Bit Capability-Secured Object Handles...
  [PASS] Capability tamper resistance: 1-byte MAC flip rejected
  [PASS] Capability read-only enforcement: read allowed, write denied
  [PASS] Capability revocation: admin token invalid after revoke
[Step 7] Testing Transparent In-Memory CDC Multi-Tenant ASQL Engine...
  [PASS] ASQL tenant isolation (tenant_a): session sees row 101 only
  ALL 10026/10026 SIDECAR CHECKS PASSED

$ build/aios-unified-memory-store-bench
  [PASS] Migrated MemoryObject: System RAM -> Persistent NVMe
  [PASS] Payload bit-identical after RAM -> Persistent migration
  [PASS] Migrated MemoryObject: Persistent NVMe -> Shared SHM
  Atomic Batch Ingestion (5,000 objects): 42.83 ms (116731 objects/sec)
  Numeric Index Range Query 'total >= 49000'        : 1.60 µs (matched 90 records)
  [PASS] Memory Pressure demoted idle entity to ResidencyClass::Sealed
  Sealed Cold Blocks Count                         : 157 blocks (5000 objects)

$ build/aios-volume-check /tmp/aios-docs-res/vol.img
  ok    format
  ok    remount
  ok    3000 writes accepted
  ok    compaction happened
  compactions=2 active_half=0 records=303 used=893 sectors
  ok    torn record dropped, everything before it kept
VOLUME: PASS
```

"Persistent NVMe" in the bench is the `persistent` provider, which is a file
under the root, wherever the root is.

---
