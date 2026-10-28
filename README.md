# AIOS: All-In-One System

AIOS is a unified C++20 systems architecture and runtime designed for high-performance state, reactive data, and sandboxed application execution. Built with zero external runtime dependencies, it runs hosted on Linux and macOS or directly on bare-metal hardware (x86-64 and ARM64 / Raspberry Pi 3) from the same kernel codebase.

---

## Core Architecture

### 1. High-Performance Relational State (ASQL & Things)
* **In-Memory Relational Engine**: Speaks the MySQL wire protocol with concurrent transaction execution (`AIOS_TXN_CONCURRENT=1`), a 16-way sharded query cache, columnar dictionary and delta bit-packing, and zero-copy wire streaming with zlib/zstd protocol compression.
* **Real-Time CDC Replication**: Ingests live MariaDB/MySQL binary replication streams (binlog) into memory with sub-millisecond propagation.
* **Reactive Materialized Views (Things)**: Incremental view maintenance (IVM) engine that updates derived state, triggers reactive side effects (`WHEN ... DO ... SHADOW`), and supports historical queries (`AS OF`), predictive previews (`WHAT IF`), and chained materialized pipelines (`INTO`).
* **Operational Tooling**: Live performance inspection via `asql-top` and `asql-console`.

### 2. Application Runtime & Scripting (Grain)
* **Sandboxed Scripting Language**: Built-in language designed for high-throughput operational scripts, web APIs, and application logic.
* **JIT Compilation**: Native code generation (including AArch64 JIT) for tight compute loops.
* **App Lifecycle Management**: Integrated manifest-based packaging, hot deployment, route handling, workers, and atomic rollbacks.
* **Extensive Builtin Library**: 540+ builtins spanning I/O, networking, data structures, and UI layout.

### 3. Capability Kernel & Security Model
* **Pure Capability Security**: No ambient root privileges. All operations are mediated by 128-bit entity identities and attenuable, revocable capability tokens.
* **State & Memory Migration**: Objects and memory regions migrate transparently across RAM, shared memory, persistent NVRAM, accelerator devices, or remote nodes while preserving identity.
* **Process & Entity Checkpoints**: Full checkpoint and restore of running entities across process restarts or machine migrations.

---

## Integrated Subsystems & Tooling

* **Networking & Cryptography**: Native TLS 1.3/1.2 client and server, SSH client/server with agent forwarding, HTTP/HTTPS web server (`aios-webserver`), FastCGI, and an independent bare-metal TCP/IP stack. Cryptographic primitives implemented in-tree: AES-GCM, ChaCha20-Poly1305, X25519, P-256, RSA, SHA-2/3, Keccak, and secp256k1.
* **Developer Environment**:
  * `aios-git`: Git-compatible version control featuring working-copy commits (`@`), stacked change management, evolutionary logs (`git change evolog`), and AST-aware structural diffing/merging across 13 language grammars.
  * `aios-sh`: Native POSIX/Bourne shell implementation with streaming built-ins (awk, sed, grep).
* **Distributed Compute & Fleet Fabric**:
  * Fleet clustering over authenticated, encrypted links with automatic node discovery, remote memory access, and cross-machine SQL (`sql_on()`).
  * Heterogeneous compute graph scheduler targeting multi-lane CPU, OpenCL, Vulkan, CUDA, and Metal devices.
  * In-tree SAT solver verifying both satisfiable models and unsat certificates, splittable across fleet nodes.
* **Intelligence ABI Slot**: Optional extension interface for language model inference (`infer`), supporting local GGUF/safetensors checkpoints (such as Qwen2) with LoRA adapters, AVX2+FMA acceleration, and an OpenAI-compatible HTTP endpoint.
* **Bare Metal & Boot**: Self-contained OS layers with bootable ISOs, UEFI/BIOS bootloaders, virtio drivers, and SD/network boot support for embedded boards (Raspberry Pi 3).

## Get started

    scripts/aios-setup.sh                  # checks tools, builds, makes ~/aos/root and its key, boots it
    build/aios --root ~/aos/root shell     # the console; `help` lists everything

`aios-setup.sh` can be re-run: it keeps the key, root and profile it already made. `--dry-run` shows every step first. Other options:

- `--upstream host:port --upstream-user U` adds a MariaDB/MySQL connection profile.
- `--key-from FILE` gives machines that must trust each other one shared key.
- `--console` keeps a resident console supervised.
- `--asql-server` installs ASQL as a system service.

Building by hand:

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j
    scripts/drive.sh                       # clean root -> full kernel drive -> restart in a new process

The main binary is `build/aios` (CMake target `aios-cli`); `--target aios`
builds only the library. You need CMake 3.20+, GCC 10+ or Clang 13+, and the
OpenCL headers (`CL/cl.h`, needed to compile; the library is loaded at run time
only if present).

For concurrent or resource-managed environments, helper build scripts are available:
`scripts/stream/tree-build.sh <target>` performs incremental builds in a shared tree
(`build-agents/`), and `scripts/build-slot.sh` runs compiles at idle priority across
allocated CPU slots.

## Guides

| guide | covers |
|---|---|
| [docs/console.md](docs/console.md) | the `aios` console, roots, the resident machine, the familiar shell, entities and capabilities, the optional mind |
| [docs/asql.md](docs/asql.md) | running ASQL, the MySQL wire server, replicating MariaDB, write modes, Things and Cells, running it as a service |
| [docs/things.md](docs/things.md) | Things, the practical guide: one worked example from a first Thing to acting on it safely (shadow, then live), looking back (`AS OF`, `EXPLAIN`), previewing a write (`WHAT IF`) and building one Thing on another (`INTO`) |
| [docs/network.md](docs/network.md) | the web server, TLS, SSH, linking two machines, moving a running workload, the fleet relay and gateway, the native TCP/IP stack |
| [docs/fleet.md](docs/fleet.md) | several machines as one: membership and keys, discovery, remote devices and memory, moving running work, fleet-wide SQL, the orchestrator, running a fleet |
| [docs/resources.md](docs/resources.md) | devices, memory types and migration, tensors, quotas, the object store and AQL, checkpoints, the sidecars and their C ABI |
| [docs/compute.md](docs/compute.md) | compute graphs and accelerators, the SAT solver (SAT and UNSAT, verification, fleet solving), grain |
| [docs/learning.md](docs/learning.md) | model training and inference on the compute fabric (`lm`, `jepa`, `world-model`), on local or remote devices |
| [docs/services.md](docs/services.md) | Bitcoin and Ethereum clients, wallet, feeds and sensing, voice, ingress |
| [docs/apps.md](docs/apps.md) | the desktop and browse pane, git, mail, contacts, calendar, the editor, package clients |
| [docs/metal.md](docs/metal.md) | bare metal on x86-64 and the Raspberry Pi 3, the ISO, disk images, dual boot, SD and network boot |
| [docs/vcs.md](docs/vcs.md) | `aios-git`: working-copy commits (`@`), stacked changes, evolutionary log, structural AST diffs, and fsmonitor |
| [docs/asql-benchmarks.md](docs/asql-benchmarks.md) | ASQL sysbench and ERP workload benchmarks against MariaDB |
| [README-ASQL.md](README-ASQL.md) | ASQL's internals and its feature list |
| [grain/README.md](grain/README.md) | `grain`: the language, syntax, JIT compiler, and core runtime |
| [docs/grain-reference.md](docs/grain-reference.md) | Grain standard library reference: 540+ builtins (and the `ui.*` and `doc.*` modules) across 18 domains |
| [docs/grain-apps.md](docs/grain-apps.md) | running and managing Grain applications, deployments, and rollbacks |
| [docs/grain-app-writing.md](docs/grain-app-writing.md) | writing Grain applications: manifests, routes, workers, and hooks |
| [ARCHITECTURE.md](ARCHITECTURE.md) | the object model and the invariants each layer keeps |
| [docs/reference.md](docs/reference.md) | every command and subcommand and what it does (generated by `scripts/gen-reference.py` from the dispatcher and `cli/command_docs.cpp`, which `help <cmd>` prints too) |
| [docs/tools.md](docs/tools.md) | every program the build produces: drives, checks, benches and servers (generated) |

## How it is put together

Measured from this tree (lines of C/C++, the generated font atlas excluded):

| part | where | lines | what it is |
|---|---|---:|---|
| object kernel | `src/kernel`, `entity`, `capability`, `state`, `memory`, `include/aios/core` | ~4.3k | entities with 128-bit identities, capabilities (grant, attenuate, revoke, budgets; there is no root user), state objects, memory that migrates between types keeping its identity, checkpoint and restore of a whole running entity |
| hardware layers | `src/hal`, `metal/`, `metal/arm64/` | ~3.6k + 7.0k + 26.1k | Linux and macOS hosts; bare-metal x86-64 (Multiboot2, UEFI and BIOS, SMP, virtio disk and network, framebuffer, PS/2); Raspberry Pi 3 (SD, USB, Wi-Fi over SDIO, four cores, framebuffer, netboot) |
| compute | `src/compute` | ~16.6k | compute graphs and their scheduler; CPU, OpenCL, Vulkan, CUDA and Metal backends; SAT and routing kernels |
| channels and wire | `src/channel`, `src/node`, `src/fleet` | ~6.8k | shared-memory state channels, encrypted links between machines, the compute wire, the gateway, the fleet relay, HTTP and chain RPC clients |
| network and crypto | `src/net`, `src/crypto` | ~14.9k | TCP/IP, TLS, SSH, FastCGI, gzip; AES-GCM, ChaCha20-Poly1305, X25519, P-256, RSA, X.509, SHA-1/256/512, Keccak, secp256k1 |
| storage | `src/storage` | ~3.1k | the append-only object store: similarity, relations, history |
| ASQL | `src/userland/sql_*`, `mysql_*` | ~63.3k | the relational engine, MySQL wire server, binlog replication, Things and Cells |
| display | `src/userland/compositor*`, `canvas`, `face`, `gpu_*`, `scene` | ~30k | the window system and desktop, the face language and layout, GPU presentation |
| web engine | `src/userland/web_*` | ~18.6k | HTML, CSS, JS and SVG for the browse pane |
| grain | `src/userland/grain_*` | ~10.6k | the scripting language and its JIT |
| applications | `src/userland/git*`, `mail*`, `contacts`, `calendar`, `lsp`, `npm`, `pypi`, `gomod`, … | ~19.6k | git, mail, contacts, calendar, the editor's LSP, package registry clients |
| intelligence slot | `src/intelligence`, `intelligence/` | ~3.5k | the optional model slot: a C plugin ABI, an out-of-process adapter, a bridge to Assay |
| command surface | `cli/` | ~13.8k | `aios`, a console that is itself an entity, so every command is checked by the kernel |
| servers and tools | `tools/` | ~114k | `aios-git`, `aios-sh`, `aios-webserver`, `aios-engine`, `aios-sat`, `asql-top`, `asql-console`, and subsystem drivers |

The kernel proper is about 4.3k lines; most of the tree is system software built
on it. Layering checks audit the boundaries: `authority-gate` ensures the
command surface acts only as the console entity (0 violations), while
`zero-host.gate` and `harness-smell` audit the core symbol surface and vocabulary.

## Fleet trust

Fleet members share a cluster key, and a hub can admit machines one by one: a
members file gives each its own attach key (delete the line to revoke it) and can
require a TPM quote at join, pinned to the machine's attestation key and measured
state. A migrated entity arrives with the authority the receiving machine admits
(docs/fleet.md).

## What a root holds

A root holds a machine's identity (`kernel.identity`), its capability key, its
object store, mail and keyring, and its connection profiles with their
passwords. Running `aios` creates them, and `.gitignore` keeps roots, build trees
and keys out of version control.
