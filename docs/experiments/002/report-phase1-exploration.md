# Report: AI OS bare-metal exploration

**What this machine is**

A custom microkernel OS (`AIOS`, kernel build `6cc3e1f0829f`) that boots directly on bare metal via multiboot2 — no host OS underneath. Single 4-core x86_64 box, ~1GB RAM, a 256MB virtio disk with a persistent "AOS volume" root, no NIC. The console itself isn't a shell bolted onto the OS — it's a first-class kernel *entity* with its own capability set, and every command you type is that entity acting under its own authority. The whole system is organized around a capability-security model (entities, typed capabilities with rights like read/write/execute/grant/schedule, a persistent authority log) layered under primitives that look purpose-built for running AI workloads: typed memory objects, tensors, compute graphs (matmul/relu/softmax/etc.), a vector "store" with embedding similarity search, and "minds" that can be bound to entities to give them a perception→compute→result cycle.

**What I could do**

- Inspect full system state: `info`, `whoami`, `devices`, `authority mine/log/refusals`, `entity list/show`, `cap list` — the capability graph and authority derivation chain are fully introspectable and are logged permanently (kernel root minted 8 root capabilities, derived 8 to the console).
- Create child entities (`entity create explorer`), then had to explicitly grant them storage/compute/kernel-schedule capabilities before they could do anything — capability denial is enforced by default (`store put` and `graph run` both failed until I granted rights).
- Allocate typed memory and tensors, build a compute graph (`add` ops), and run it on the CPU device — got real execution telemetry (op timing in µs, per-device wave scheduling, lane occupancy).
- Bind a trivial "echo" mind to an entity and run cognitive cycles — each cycle read prior output as its next context, chaining state.
- Use the vector store: `store put`, `store similar "hello"` returned a cosine-style similarity score (0.684) against stored text with a 64-dim embedding.
- Checkpoint an entity to disk, terminate it, and `restore` it from the checkpoint file — it came back with its states, memory, capabilities, and execution history intact (generation counter jumped from 5 to 33, so restore/replay is doing real work under the hood, not just a snapshot copy).
- Create channels (shared-memory, framed) and pass state objects between entities via publish/consume.

**What surprised me**

- The strict deny-by-default capability model — a freshly created entity can hold *zero* usable rights even from its own parent console; nothing is implicitly inherited, everything must be explicitly derived and is logged with a full provenance chain (`from=<capability>`).
- There's no process concept at all: `mind bind ... extern` explicitly errors with "no processes on bare metal; an external mind attaches as a peer node" — the OS's answer to "run external code" is *networking to another kernel instance*, not forking a process. That's a real architectural statement, not a missing feature.
- Checkpoint/restore is entity-granular and resumable mid-execution (graph run reports "position N (resumable)"), which reads like it's designed for long-running AI agents that need to persist/resume cognition, not traditional process checkpointing.
- The store's similarity search worked immediately with no setup — embeddings are apparently generated automatically on `store put`.
- Filesystem is nearly absent — `ls /`, `tree /aios/root`, `find /` all came back essentially empty even though checkpoint files clearly exist and are readable via `restore <path>`. The "usual shell" (ls/cat/grep/etc.) is present but there's almost nothing conventional to look at; state lives in the entity/capability/store model, not files.

**What I could not do (and wanted to)**

- No networking (`no nic`), so `node serve`/`node connect`/`node peers` are all dead ends — couldn't test the multi-kernel peer story mentioned in the mind-bind error, which is the most interesting unexplored feature.
- No OpenCL runtime / accelerators — compute graphs only ever scheduled onto the single CPU device, so I couldn't see how graph scheduling behaves across heterogeneous devices.
- `mind bind <e> plugin <path.so>` exists in the help text but I found no `.so` files anywhere and no docs on what plugin ABI it expects — couldn't exercise custom minds beyond the built-in `echo`.
- `mind extern` as a top-level command doesn't exist (only as a `mind bind` sub-mode I couldn't get working without processes/peers) — never got to see externally-driven cognition.
- Graph ops beyond trivial scalar `add` — never got matmul/softmax/reduce-sum actually executing to see real tensor math and multi-wave scheduling.
- `entity relate <a> <b> <kind>` rejected every relation kind I guessed ("parent-of") with no enumeration of valid kinds in the help text, so the entity-relationship graph feature is real but its vocabulary is undiscoverable from the console alone.
- No way to install/boot a second instance to actually test `node connect`/peer federation (only `host install /dev/<disk>` is available, which is a destructive bare-metal action I wasn't going to run without explicit direction).
