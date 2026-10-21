# Compute: graphs, accelerators, SAT, grain

This page covers four things that share one scheduler:

- **compute graphs**: tensors and operations that an entity owns. The scheduler
  places each operation on a device and runs it.
- **accelerators**: OpenCL, CUDA, Vulkan and Metal devices joining the same
  device fabric as the CPU.
- **SAT/UNSAT**: a CDCL solver. It checks a model before reporting SAT and
  checks a DRAT proof before reporting UNSAT. It ships as the standalone
  `aios-sat`, as the `sat` command, as `CALL SAT` in SQL, and as `sat()` in grain.
- **grain**: the machine's own scripting language. It has a bytecode VM and an
  AArch64 JIT.

Every example below was run against the binaries in `build/` (`build/aios`,
`build/aios-sat`), on an x86_64 host with 4 cores and a GTX 1080 Ti. Each run
used a throwaway root under `/tmp/aios-docs-compute/` and `AIOS_NO_ACCEL=1`,
except where the point was to show accelerators. Output is trimmed but not
edited.

Sources: `src/compute/`, `include/aios/compute/`, `cli/commands.cpp`
(`cmd_graph`, `cmd_compute`, `cmd_quota`, `cmd_devices`), `cli/sat_cmd.cpp`,
`tools/sat-cli.cpp` (aios-sat), `src/userland/sat_solver.cpp`,
`src/userland/sql_sat.cpp`, `cli/grain_cmd.cpp`, `src/userland/grain_*.cpp`,
`metal/arm64/grain_shell.cpp`, `src/channel/compute_serve.cpp`,
`tools/grain-jit/`, `tools/proof/sat/`.

---

## Running commands

All of these are console commands. Run them one-shot, from a script, or at the
prompt:

    aios --root DIR exec 'cmd' ['cmd' ...]    # run and exit
    aios --root DIR run FILE.aos              # a script; stops at the first unexpected failure
    aios --root DIR shell                     # interactive

`--root` defaults to `$AIOS_ROOT`, then `./aios-root`. In a script:

- `=> name` binds a command's produced id to `$name`.
- `set name` binds the last produced value.
- A leading `!` marks a line that is expected to fail.

The console tokenizer treats a backslash inside quotes as an escape for the
next character, so `\n` arrives as a plain `n`. To pass a literal backslash
sequence, write `\\n` (see [CALL SAT](#call-sat-in-sql)).

---

## Compute graphs

An entity needs two capabilities to run a graph: `schedule` on the kernel and
`execute` on compute. Tensors are memory objects with a dtype and a shape. A
graph is a list of operations that read and write those tensors.

### Commands

| command | what it does |
|---|---|
| `tensor create <e> <dtype> <shape> [values...]` | dtype `f32 f64 i32 i64 u8`; shape like `2x3`. A single value fills every element. |
| `tensor show <e> <mem> <dtype> <shape>` | read a tensor back |
| `graph create <e>` | a new, empty graph owned by `<e>` |
| `graph op <e> <g> <opcode> [attrs...]` | add an operation (attributes below) |
| `graph show <g>` | the operations as written, plus the dependency order |
| `graph plan <e> <g>` | where each operation would be placed, and why |
| `graph partition <e> <g> [link=<bytes/s>]` | waves, per-device cost estimates, critical path |
| `graph run <e> <g> [steps]` | execute; with `steps`, stop after that many operations (resumable) |
| `quota set <e> budget=<ns> [slice=<ns>] [period=<ns>]` / `quota list` / `quota clear <e>` / `quota reset` | device-time budgets that preempt a run |

Operation attributes (`cmd_graph` in `cli/commands.cpp`):

- `in=<mem>:<dtype>:<shape>` and `out=<mem>:<dtype>:<shape>`: operands. Repeat
  them for more than one.
- `scalar=x`: the scalar for `fill` and `scale`. Custom kernels also read it.
- `par=n`, `prio=n`, `mem=bytes`: requirements given to the scheduler.
- `dev=...`: a device class or a pin. See [Devices and placement](#devices-and-placement).
- `after=<op>`: an explicit dependency. Data dependencies through shared
  tensors are ordered for you.
- `kernel=<name>`: a named custom kernel, used with opcode `custom`.
- `flops=n`: override the work estimate. By default it is computed from the
  opcode and shapes.

Opcodes: `nop copy fill add mul scale matmul reduce-sum relu softmax histogram
normalize gelu gelu-grad relu-grad matmul-grad-a matmul-grad-b bias-add
bias-add-grad layernorm layernorm-grad softmax-ce-loss softmax-ce-grad
adam-step causal-attention causal-attention-grad custom`.

### Build one, run it partway, finish it

```
entity create worker => w
cap grant $w kernel * schedule
cap grant $w compute * execute
tensor create $w f32 2x3 1 2 3 4 5 6          => A
tensor create $w f32 3x2 7 8 9 10 11 12       => B
tensor create $w f32 2x2                      => C
tensor create $w f32 2x2                      => D
graph create $w                               => g
graph op $w $g matmul in=$A:f32:2x3 in=$B:f32:3x2 out=$C:f32:2x2 => m
graph op $w $g relu in=$C:f32:2x2 out=$D:f32:2x2 after=$m
graph op $w $g softmax in=$D:f32:2x2 out=$D:f32:2x2
graph show $g
graph plan $w $g
graph partition $w $g
graph run $w $g 1
tensor show $w $C f32 2x2
graph run $w $g
tensor show $w $D f32 2x2
```

```
graph b69afd1f54b60af80005ce62641914eb owner=f15a1f3cc251 ops=3 flops≈40 footprint=80 B
    d4d263a4d5f9  matmul      dev=any par=1 prio=100  in f32[2x3]@a4c6ab2b5f5a  in f32[3x2]@6155d7b658f7  out f32[2x2]@2da4c405598c
    72e9ea6a5643  relu        dev=any par=1 prio=100  in f32[2x2]@2da4c405598c  out f32[2x2]@ea0bf0905b19  after d4d263a4d5f9
    91209033d696  softmax     dev=any par=1 prio=100  in f32[2x2]@ea0bf0905b19  out f32[2x2]@ea0bf0905b19
    order: d4d263a4d5f9 72e9ea6a5643 91209033d696
› graph plan $w $g
    op d4d263a4d5f9 → cpu lanes=1 [locality=100% free-lanes=100% free-memory=75%]
    ...
› graph partition $w $g
  wave 0  1 op(s), 1.5e-06 ms on the slowest lane, busiest device cpu
      d4d263a4d5f9  matmul      -> cpu                compute 1.5e-06 ms  (24 flop)
  ...
  3 wave(s) over 1 device(s); critical path 2.5e-06 ms; graph cost 0.000128 ms on the 1 GFLOP/s reference machine
› graph run $w $g 1
execution b69afd1f54b6: ran 1/3 ops, position 1 (resumable) in 192.4 µs
    wave 0  op d4d263a4d5f9 → cpu(46da4378349e) lanes=1 4.1 µs  [locality=100% free-lanes=100% free-memory=75%]
› tensor show $w $C f32 2x2
f32[2x2]@2da4c405598c = [58 64 139 154]
› graph run $w $g
execution b69afd1f54b6: ran 2/3 ops, position 3 (complete) in 13.9 µs
    wave 0  op 72e9ea6a5643 → cpu(46da4378349e) lanes=1 3.2 µs  [...]
    wave 1  op 91209033d696 → cpu(46da4378349e) lanes=1 773 ns  [...]
› tensor show $w $D f32 2x2
f32[2x2]@ea0bf0905b19 = [0.0024726 0.99753 3.059e-07 1]
```

The execution keeps its **position**. A second `graph run` continues from the
position where the first one stopped; it does not start over.

### Checkpoint and resume mid-graph

The position belongs to the entity, so it survives a checkpoint, a
termination, and a restore. The entity also needs `kernel * checkpoint`:

```
cap grant $w kernel * checkpoint
...
graph run $w $g 1
checkpoint $w
set ckpt
entity terminate $w
restore $ckpt
graph run $w $g
tensor show $w $D f32 2x2
```

```
execution 35abb5ef6931: ran 1/3 ops, position 1 (resumable) in 279.7 µs
checkpoint /tmp/aios-docs-compute/ck/root/checkpoints/16221147648908a20008d8d82b021143/1.aoc
terminated 162211476489 (memory released, capabilities revoked, channels destroyed)
restored entity 16221147648908a20008d8d82b021143 (worker) states=1 memory=4 caps=11 restores=1 execution=35abb5ef6931@1
execution 35abb5ef6931: ran 2/3 ops, position 3 (complete) in 20.8 µs
f32[2x2]@f15ab9606e9a = [0.0024726 0.99753 3.059e-07 1]
```

`execution=35abb5ef6931@1` is the resumable position, carried in the
checkpoint. To resume on another machine, connect to it with `node connect`
and move the entity with `node migrate`. The top-level `README.md` covers
this.

### Budgets: preemption mid-graph

A quota caps an entity's device time. When the budget runs out, the run stops
at an operation boundary and stays resumable:

```
quota set $w budget=1000
graph run $w $g
quota list
quota clear $w
graph run $w $g
```

```
entity 3526c353d4e8 may spend 1.0 µs of device time every 1.000 s, in stretches of at most 1.0 µs
execution 3ef1b126a3b0: ran 1/3 ops, position 1 (resumable) in 436.5 µs
    wave 0  op 5c292b6822c5 → cpu(83a63f0ec233) lanes=1 140.8 µs  [...]
    PREEMPTED: the owner's execution budget ran out after 140.8 µs of device time; resume from position 1
ENTITY        LABEL             SLICE       BUDGET      USED        PERIOD    STOPS   STATE
3526c353d4e8  worker            1.0 µs     1.0 µs     140.8 µs   1.000 s   1       out of budget
quota cleared
execution 3ef1b126a3b0: ran 2/3 ops, position 3 (complete) in 14.6 µs
```

Budgets are checked between operations. A single operation that runs past the
budget is still allowed to finish (140.8 µs against a 1 µs budget above).

---

## Devices and placement

`devices` lists the fabric. With `AIOS_NO_ACCEL=1` the only device is the CPU:

```
DEVICE ID     TYPE      LANES   BUSY    MEMORY        NATIVE    NAME
46da4378349e  cpu       4       0       19.49 GiB     system    cpu
                in use 4.81 GiB, free 14.68 GiB (host-available)
```

There is no `device` or `dev` command group. The listing is `devices`, and
`dev=` is an operation attribute.

### How accelerators are detected

When the kernel boots (`src/kernel/kernel.cpp`), it registers one CPU device
and registers the SAT and ERP kernels on it. Unless `AIOS_NO_ACCEL` is set,
it then tries each runtime in turn. Each is loaded with `dlopen` at run time,
so there is no vendor library dependency at link time:

| backend | source | runtime it opens | device name |
|---|---|---|---|
| OpenCL | `opencl_device.cpp` | `libOpenCL.so.1` | `gpu:<name>`, `npu:<name>` (accelerator), `ocl-cpu:<name>` (CPU runtime such as pocl) |
| CUDA | `cuda_backend.cpp` | `libcuda.so.1` / `libcuda.so` / `nvcuda.dll` | `cuda:<name> cc<maj.min> [<index>]` |
| Vulkan | `vulkan_backend.cpp` | `libvulkan.so.1` / `libvulkan.so` / MoltenVK | `vulkan:<name> api<ver> [<index>]` (shaders in `src/compute/shaders/`) |
| Metal | `metal_backend.cpp` | Darwin only | elsewhere it registers nothing and says so |

The same machine without `AIOS_NO_ACCEL` (one GPU, seen through three
runtimes, plus Mesa's llvmpipe):

```
$ aios --root /tmp/aios-docs-compute/acc exec devices info
DEVICE ID     TYPE      LANES   BUSY    MEMORY        NATIVE    NAME
12eb9a1ef5b3  cpu       4       0       19.49 GiB     system    cpu
709200d74a05  gpu       28      0       10.91 GiB     device    gpu:NVIDIA GeForce GTX 1080 Ti
                in use 1.06 GiB, free 9.94 GiB (nvml); tenants: pid 5281 aios 283.64 MiB ...
d65a896dcb48  gpu       3584    0       10.91 GiB     device    cuda:NVIDIA GeForce GTX 1080 Ti cc6.1 [0]
34050f2a4b92  gpu       16      0       11.24 GiB     device    vulkan:NVIDIA GeForce GTX 1080 Ti api1.3 [0]
95cc75e0c8e5  gpu       1       0       19.49 GiB     system    vulkan:llvmpipe (LLVM 20.1.2, 256 bits) api1.4 [1]
...
  Accelerators    : gpu:NVIDIA GeForce GTX 1080 Ti (28 units, 11169 MiB, fp64), cuda:NVIDIA GeForce GTX 1080 Ti cc6.1 [0],
                    vulkan:NVIDIA GeForce GTX 1080 Ti api1.3 [0], vulkan:llvmpipe (LLVM 20.1.2, 256 bits) api1.4 [1]
                    (Metal: Metal compute runtime is only available on Darwin/macOS)
```

`info` keeps a backend's failure note even when other devices registered, for
example "device 1: cuCtxCreate: out of memory".

### `AIOS_NO_ACCEL`

Any non-empty value other than `0` makes the node CPU-only. No OpenCL, CUDA,
Vulkan or Metal runtime is opened. Use it:

- for a statically linked build, which cannot `dlopen` safely;
- for a compute server with no card;
- whenever you time something. Bringing the GPUs up costs about 0.65 s and
  hundreds of MB on every CLI start.

`info` then reports `accelerators disabled (AIOS_NO_ACCEL)`.

`AIOS_NO_OPENCL`, `AIOS_NO_CUDA` and `AIOS_NO_VULKAN` turn off one backend
each (`info` names which), so a machine whose card all three can reach can be
run on the one being tested.

### `dev=`: class, or pin

- `dev=any` is the default. The others are `dev=cpu`, `dev=gpu`, `dev=npu`,
  `dev=remote-cpu`, `dev=remote-gpu` and `dev=remote-npu`. A class name asks
  for any device of that class; the scheduler chooses by locality, free
  lanes, free memory and estimated cost.
- Anything else is a **pin**: a prefix of a device name (`cuda`,
  `vulkan:NVIDIA`, `ocl-cpu`, `gpu:`, `remote:<addr>/...`) or of a device id.
  The selector is stored as written and resolved when the operation is
  placed, so a graph that moves to another kernel still names a real device.
  A selector that matches nothing is refused when you add the operation.

```
graph op $w $g matmul ... dev=cuda
graph op $w $g matmul ... dev=vulkan:NVIDIA
graph op $w $g matmul ... dev=gpu
graph op $w $g matmul ... dev=cpu
graph run $w $g
! graph op $w $g matmul ... dev=tpu9
```

```
execution a1fa91e9eef0: ran 4/4 ops, position 4 (complete) in 116.16 ms
    wave 0  op c7051f526d3a → cuda:NVIDIA GeForce GTX 1080 Ti cc6.1 [0](81e3201efbb5) lanes=1 1.37 ms  [...]
    wave 1  op 654da69cec67 → vulkan:NVIDIA GeForce GTX 1080 Ti api1.3 [0](272bae477aff) lanes=1 3.12 ms  [...]
    wave 2  op 8a942cc56cd1 → vulkan:llvmpipe (LLVM 20.1.2, 256 bits) api1.4 [1](45723581f928) lanes=1 51.6 µs
            [estimated 45.0440 us (gpu:NVIDIA G 164.663, cuda:NVIDIA  132.912, vulkan:NVIDI 768.970, vulkan:llvmp 45.0440 us)]
    wave 3  op 28dcaa0feb1a → cpu(dd90136bfc1e) lanes=1 1.9 µs  [...]
f32[2x2]@e26b8c66ef5d = [58 64 139 154]
  refused as expected: not-found: no device answering to 'tpu9' in this kernel's fabric
```

The `dev=gpu` matmul landed on llvmpipe. For a 2x2 matmul, the cost
estimate in brackets put the CPU-backed Vulkan device ahead of the real GPU,
because of the GPU's transfer and launch overhead.

With `AIOS_NO_ACCEL=1`, the two forms fail at different times:

- `dev=gpu` is accepted by `graph op` but fails at plan or run time with
  `unsupported: no gpu device in the fabric`.
- `dev=cuda` is refused by `graph op` immediately with
  `not-found: no device answering to 'cuda'`.

An operation is placed only on a device whose `supports()` accepts it:

- **OpenCL**: `nop copy fill add mul scale relu matmul reduce-sum softmax
  normalize histogram`. No integer tensors, and f64 only if the device has
  fp64.
- **CUDA**: the elementwise and matmul set, plus the training ops (gelu, the
  gradients, layernorm, softmax-ce, adam-step, causal-attention).
- **Vulkan**: `fill add mul scale relu copy matmul` only.
- **Custom kernels**: only where that kernel is registered.

When an operation runs on a device, its operands move into device memory; the
memory object keeps its identity. `scripts/drive-accelerator.aos` exercises
this: re-uploads tracked by generation, and `mem migrate ... system`.

### Device memory

`mem migrate <e> <mem> device` (or allocating `device` memory) puts an object
in the first accelerator backend's memory, in boot order: OpenCL, then CUDA,
then Vulkan, then Metal. Each keeps a host mirror, which is what the fabric and
the CPU read and write, plus the copy its kernels work on:

- **OpenCL**: a buffer on the device.
- **CUDA**: a `cuMemAlloc` allocation on the card.
- **Vulkan**: a host-visible storage buffer the shaders bind directly. On a
  discrete card this can be system memory the GPU reads over the bus rather than
  VRAM. It is bound in place only for a tensor that starts its object.
- **Metal**: a buffer on the device; its kernels do not read it yet.

The device copy is rewritten from the mirror only when the mirror's
generation has moved past it, and an operation that writes it in place leaves
it current, so data that stays on the card between operations is not copied
each time. A backend uses only device memory it made itself; any other operand
goes through a temporary buffer for the call. On a GTX 1080 Ti, with the other
backends switched off so each held the device slot in turn, CUDA and Vulkan
gave the same answers, card to card, CPU write then card read, and card write
then CPU read:

```
$ AIOS_NO_OPENCL=1 aios --root R exec ... 'mem migrate $w $C device' 'mem migrate $w $D device' \
    'graph op $w $g1 fill out=$C:f32:2x2 scalar=3 dev=cuda => f' 'graph op $w $g1 scale in=$C:f32:2x2 out=$D:f32:2x2 scalar=2 after=$f dev=cuda' ... \
    'graph op $w $g2 fill out=$C:f32:2x2 scalar=5 dev=cpu => f2' 'graph op $w $g2 scale in=$C:f32:2x2 out=$D:f32:2x2 scalar=2 after=$f2 dev=cuda' ... \
    'graph op $w $g3 fill out=$C:f32:2x2 scalar=7 dev=cuda => f3' 'graph op $w $g3 scale in=$C:f32:2x2 out=$D:f32:2x2 scalar=10 after=$f3 dev=cpu' ...
f32[2x2]@5647ad7f578f = [6 6 6 6]
f32[2x2]@5647ad7f578f = [10 10 10 10]
f32[2x2]@5647ad7f578f = [70 70 70 70]
```

With all backends on, OpenCL holds the slot; a matmul on OpenCL, a scale on
CUDA and a relu on Vulkan over the same OpenCL device memory give
`[116 128 278 308]`.

### A custom kernel in a graph

The ERP kernels are registered on every CPU device at boot, so a hand-built
graph can call one with `custom ... kernel=<name>`:

```
tensor create $w f64 4x2 42.886 -78.878 42.95 -78.70 42.80 -78.80 42.99 -78.85 => P
tensor create $w i32 4 => O
tensor create $w f64 4 => M
graph create $w => g
graph op $w $g custom in=$P:f64:4x2 out=$O:i32:4 out=$M:f64:4 kernel=erp.tsp_exact scalar=1
graph run $w $g
tensor show $w $O i32 4
tensor show $w $M f64 4
```

```
execution 2ce15daef862: ran 1/1 ops, position 1 (complete) in 238.0 µs
i32[4]@a4034650fb34 = [0 2 1 3]
f64[4]@68705123fadf = [26.742 6 0 0]
```

`erp.tsp_exact` is laid out in `include/aios/compute/erp_kernels.hpp`:

- `in[0]`: an `[n,2]` tensor of lat,lng; row 0 is the start.
- `out[0]`: the visiting order.
- `out[1]`: distance in miles, paths evaluated, paths pruned.
- `scalar != 0`: turns on pruning.

The route is an open path (no return to the depot). Distances are haversine
miles with radius 3958.8.

---

## `compute serve`: the compute service

```
compute kernels
compute serve [port=7311] [lanes=N] [max_points=15] [token=T | token_file=F] [public=1] [shm_ring=NAME]
              [sql=1 [sql_db=DIR] [cdc_*=...] [mysql_*=...]]
```

`compute kernels` registers `erp.tsp_exact`, `erp.route_improve` and
`erp.pool_greedy` on the CPU devices.

`compute serve` runs as a resident entity named `erp-compute`. That entity's
authority is limited to schedule, execute, at most 1 GiB of memory, state,
and the object store. The service listens on 127.0.0.1 unless `public=1` is
given, and requires the token when one is set.

**Framing.** Each request and each answer is a 4-byte big-endian length
followed by that many bytes of JSON.

**Operations.**

- `ping`
- `stats`
- `tsp_exact`: `start` and `stops`, each `{id, lat, lng}`; optional
  `deadline_ms` and `prune`.
- `route_improve`: optional `end_mode` and `end`.
- `pool_greedy`
- `sql`: only with `sql=1`, and read-only.

A binary TSP form also exists (`compute_wire.hpp`).

**Deadlines and caching.** If the deadline passes before the answer is ready,
the request gets `"status":"pending"`. The work continues, and the same
request asked again answers from cache.

Drive (port 13450, loopback, token set):

```
$ AIOS_NO_ACCEL=1 aios --root .../serve/root exec 'compute serve port=13450 token=docs-token-0123456789' &
compute service as entity f3fadf759062 over 1 cpu device(s); kernels: erp.tsp_exact, erp.route_improve, erp.pool_greedy
compute service listening on 127.0.0.1:13450 lanes=4 max_points=15 token=required
```

The client used for this drive:

```python
import socket, struct, json
def call(req):
    s = socket.create_connection(("127.0.0.1", 13450), timeout=10)
    b = json.dumps(req).encode()
    s.sendall(struct.pack(">I", len(b)) + b)
    hdr = b""
    while len(hdr) < 4: hdr += s.recv(4 - len(hdr))
    n = struct.unpack(">I", hdr)[0]; body = b""
    while len(body) < n: body += s.recv(n - len(body))
    return json.loads(body)
```

```
{"op":"ping"}                         -> {'ok': False, 'op': 'ping', 'error': 'unauthorized'}
{"op":"ping","token":...}             -> {'ok': True, 'op': 'ping', 'kernel': '87ad15700c92',
                                          'devices': [{'name': 'cpu', 'type': 'cpu', 'lanes': 4}],
                                          'ops': ['ping', 'stats', 'tsp_exact', 'route_improve', 'pool_greedy'], 'max_exact_points': 15}
{"op":"tsp_exact", start + 5 stops}   -> {'ok': True, 'status': 'done', 'order': ['depot', 'e', 'c', 'a', 'd', 'b'], 'n': 6,
                                          'distance_miles': 35.666829, 'evaluated': 16, 'pruned': 99, 'elapsed_ms': 0.474,
                                          'device': 'cpu', 'lanes': 4, 'cached': False, 'record': 'b53af916...'}
(same request again)                  -> ... 'cached': True ...
{"op":"route_improve", same points}   -> {'ok': True, 'status': 'done', 'order': ['e', 'c', 'a', 'd', 'b'],
                                          'total_distance_miles': 42.802889, 'evaluated': 62, 'passes': 1, 'end_mode': 'home', ...}
{"op":"stats","token":...}            -> {'requests': 6, 'solved': 2, 'served_cached': 1, 'pending': 0, 'errors': 1, ...}
```

The service log, for the same requests:

```
tsp_exact n=6 prune=1 -> 35.667 mi, evaluated=16 pruned=99 in 0.47 ms on cpu lanes=4
route_improve n=5 end=home -> 42.803 mi, evaluated=62 passes=1 in 0.04 ms on cpu
```

`tsp_exact` takes at most `max_points` points: start plus stops, default 15.
Beyond that the caller should use `pool_greedy` or `route_improve`.

**Not driven here:**

- `sql=1`, which also starts a MySQL-wire server, defaulting to port 3307;
- the CDC/binlog options;
- `shm_ring`.

---

## SAT and UNSAT

There is one solver (`src/userland/sat_solver.cpp`, CDCL with preprocessing)
and one independent checker (`sat::check_rup`, a RUP/RAT checker for
**binary DRAT**). Four entry points use them:

| entry point | where it runs | proof checked by |
|---|---|---|
| `aios-sat FILE.cnf` | this process, a racing portfolio | `aios-sat --check` (separate run) |
| `sat solve FILE` | a graph op `sat.solve` on a device, as its own entity | `sat.check` op on a device, automatically |
| `sat fleet FILE on=...` | cube-and-conquer over several devices or machines | `sat.check`, on another worker than the one that solved the cube |
| `CALL SAT(...)` (SQL), `sat(...)` (grain) | in-process `solve_verified` | the RUP checker, automatically |

**Answers and exit codes.** SAT is reported only after the model has been
checked against every clause. UNSAT carries a proof. The SAT Competition
codes are used: 10 SAT, 20 UNSAT, 0 unknown.

The two example formulas used below:

```
$ cat small-sat.cnf
c three variables, satisfiable
p cnf 3 4
1 2 0
-1 3 0
-2 -3 0
2 3 0
$ head -4 php-4-3.cnf          # pigeonhole: 4 pigeons, 3 holes
c pigeonhole 4 pigeons -> 3 holes (UNSAT)
p cnf 12 22
1 2 3 0
4 5 6 0
```

### `aios-sat`

```
aios-sat FILE.cnf [--proof=FILE] [--binary] [--threads=N] [--no-elim] [--conflicts=N] [--seconds=N] [--quiet] [--dump=FILE]
aios-sat --check FILE.cnf PROOF.bin
```

- `--threads=N`: a racing portfolio of differently configured solvers; the
  first answer wins. The default is every hardware thread. Use
  `--threads=1` when you time it.
- `--proof=FILE`: writes the DRAT proof, in text form unless `--binary` is
  given. `--check` reads binary only.
- `--conflicts=N`, `--seconds=N`: budgets. When a budget runs out the answer
  is `s UNKNOWN`, exit 0.
- `--no-elim`: turns off variable elimination.
- `--dump=FILE`: writes the preprocessed formula and stops.

A satisfiable formula: the model is printed in `v` lines.

```
$ aios-sat small-sat.cnf --threads=1; echo exit=$?
c vars=3 clauses=4 conflicts=0 decisions=1 propagations=1 restarts=0 ... time=0.0ms
s SATISFIABLE
v -1 2 -3 0
exit=10
```

UNSAT with a binary DRAT proof, then an independent check of that proof:

```
$ aios-sat php-4-3.cnf --threads=1 --proof=php.drat --binary; echo exit=$?
c vars=12 clauses=22 conflicts=0 ... eliminated=10 gates=1 resolvents=63 ... time=0.1ms
s UNSATISFIABLE
exit=20
$ aios-sat --check php-4-3.cnf php.drat; echo exit=$?
c lemmas=62 (rat 0) time=0.9ms
s VERIFIED
exit=0
```

A harder pigeonhole (8 into 7, 56 variables, 204 clauses). Shown below: a
budget running out, a full run with a proof and its check, and a truncated
proof being rejected:

```
$ aios-sat php-8-7.cnf --threads=1 --no-elim --conflicts=200; echo exit=$?
c vars=56 clauses=204 conflicts=202 decisions=353 propagations=2923 restarts=3 ...
s UNKNOWN
exit=0
$ aios-sat php-8-7.cnf --threads=1 --proof=php87.drat --binary --quiet
s UNSATISFIABLE
$ aios-sat --check php-8-7.cnf php87.drat
c lemmas=1151 (rat 126) time=7.2ms
s VERIFIED
$ head -c 300 php87.drat > cut.drat; aios-sat --check php-8-7.cnf cut.drat; echo exit=$?
c lemmas=27 (rat 27) time=0.9ms the proof never reaches the empty clause
s NOT VERIFIED
exit=1
```

A text proof (without `--binary`) is written fine, but `--check` rejects it:
`not binary DRAT at byte 0`. Check text proofs with an external tool such as
drat-trim (`tools/proof/sat/proofcheck.sh`, which needs drat-trim installed).

The portfolio header appears when there is more than one thread:

```
$ aios-sat php-8-7.cnf --threads=2 | head -1
c portfolio: 2 members, member 1 answered first
```

### `sat` inside aios

```
sat devices
sat solve FILE [on=SEL] [conflicts=N] [check_on=SEL]
sat fleet FILE on=SEL[@budget][!],SEL,... [budget=N] [depth=D]
```

**How a solve runs.** Each solve or check is a graph with one `custom`
operation (`sat.solve` or `sat.check`). It is run by a worker entity spawned
for the purpose. The worker holds schedule rights, `execute` on just the
named device, and write access to the store for its receipts.

**Selectors.** `on=` and `check_on=` take a device-name or id prefix,
`remote:<peer>/...` included. Only CPU devices register the SAT kernels, so
`sat devices` lists only `cpu`, with or without accelerators.

```
$ AIOS_NO_ACCEL=1 aios --root .../sat/root exec 'sat devices' 'sat solve small-sat.cnf' 'sat solve php-8-7.cnf' 'sat solve php-8-7.cnf conflicts=50'
  cpu                                         a5a65f49234e  lanes 4  sat.solve  sat.check
1 device(s) run the solver
c solved on cpu: 0 conflicts, 1 decisions, kernel 0.0 ms, wall 16.3 ms
c fingerprint: proof cbf29ce484222325 (0 bytes), propagations 1, model 9a27d17e6e2685bd
s SATISFIABLE
c model checked here: every clause satisfied
c solved on cpu: 981 conflicts, 1205 decisions, kernel 5.3 ms, wall 19.5 ms
c fingerprint: proof 9b0a8f11b4fdf71c (16241 bytes), propagations 17231
s UNSATISFIABLE
c proof: 16241 bytes binary DRAT
c proof checked by sat.check on cpu: VERIFIED (7.1 ms)
c solved on cpu: 50 conflicts, 80 decisions, kernel 0.5 ms, wall 14.6 ms
c fingerprint: proof ace4d41e5ff6a807 (3197 bytes), propagations 965
s UNKNOWN (budget spent)
```

`sat solve` does not print the model. Use `aios-sat` or `sat()` when you need
the assignment.

**Fingerprints.** The fingerprint line is there to prove determinism. The
same formula with the same budget must give the same proof bytes on every
machine, x86 or ARM: the solver decides nothing by clock, randomness or
address. In this drive, a second process gave `proof 9b0a8f11b4fdf71c
(16241 bytes)` again, and that is also the byte size of the `aios-sat`
binary proof above.

**Fleet: cube-and-conquer.**

1. The formula is split on its most frequent variables to depth `D` (default
   `ceil(log2(4 × workers))`).
2. Each worker takes cubes from a shared queue and solves each one with a
   conflict budget (default 20000).
3. A cube that exhausts its budget is split in two and goes back on the
   queue.

The result is never taken on a worker's word:

- **SAT**: the model is checked by the coordinator against the whole formula.
- **UNSAT**: every leaf proof must pass `sat.check` on a different worker
  than the one that produced it. With remote workers, a check runs on a
  different machine whenever one exists.

Two per-worker options go in the selector:

- `SEL@N`: that worker's own conflict budget.
- `SEL!`: a worker that solves but never checks, for a slow link or a small
  box.

Each run is written to the machine's ASQL as `sat.runs`, `sat.cubes` and
`sat.workers`, with two live Things on top (`sat_run`, `sat_machine`). Every
cube also leaves a receipt in the object store, under the entity of the
worker that produced it.

Local drive, two workers on the same CPU:

```
$ AIOS_NO_ACCEL=1 aios --root .../sat/root exec 'sat fleet php-8-7.cnf on=cpu,cpu budget=200' 'sat fleet small-sat.cnf on=cpu,cpu'
c fleet: 2 worker(s), budget 200 conflicts a cube, first split depth 3, 50 further splits, wall 312.3 ms
c   worker 0 cpu   on cpu   entity 9059513dfe8d  cubes 42 (sat 0, unsat 22, split 20)  checks 36  conflicts 6128  busy 222 ms (solving 71, checking 73, the rest moving data: 79)  165 KB back
c   worker 1 cpu   on cpu   entity 2cf7dcbfecbf  cubes 66 (sat 0, unsat 36, split 30)  checks 22  conflicts 8934  busy 250 ms (solving 109, checking 49, the rest moving data: 93)  252 KB back
c ledger: run 18da86c2583c8205 in ASQL (sat.runs, 108 rows in sat.cubes, sat.workers) in 17 transaction(s), 0 failed statement(s); Things sat_run, sat_machine
c receipts: 58 cube receipt(s) under the workers' entities; run receipt 73c54913bda39c6b00055f20aa32787c
s UNSATISFIABLE
c 58 leaf cube(s), every one refuted, 58 proof(s) verified by sat.check (0 checked on a different machine than solved it, 58 on the same machine by another worker, 0 on the machine that made it, never moved)
c   proofs from this machine checked on this machine: 58
c fleet: 2 worker(s), budget 20000 conflicts a cube, first split depth 3, 0 further splits, wall 13.9 ms
...
s SATISFIABLE
c model checked here against every clause
```

A SAT run raises no alert: once SAT is found the run stops, and the cubes it
had already refuted are never checked, because the model (checked against every
clause) is the proof. `sat_run` alarms `finished UNSAT with leaves not
verified` only for an UNSAT run with a refuted leaf no proof check confirmed. A
root that declared the older `sat_run` gets it replaced on the next `sat fleet`
(version kept in `sat.ledger_meta`).

The ledger is ordinary SQL:

```
$ aios --root .../sat/root exec 'sql query "SELECT run, result, leaves, verified, wall_ms FROM sat.runs"'
| run              | result | leaves | verified | wall_ms |
| 18da86c2583c8205 | 20     | 58     | 58       | 312     |
| 18da86c26cfbbf56 | 10     | 2      | 0        | 13      |
```

**Across machines.** A cross-machine fleet run
(`on=remote:<peer>/cpu,...`) needs peers; [fleet.md](fleet.md) shows one over a
hub, with the other member checking the proofs. Peers are reached either through
`node connect` or through the fleet hub (`AIOS_FLEET_HUB`/`AIOS_FLEET_KEY`).
For `remote...` selectors, `sat` waits up to about 15 s for each named peer's
device to appear. A remote worker keeps three proofs in four on the machine
that made them and checks them there ("never moved"). The fourth comes back
to be checked on a different machine.

### `CALL SAT` in SQL

```
CALL SAT('<DIMACS CNF>' [, conflict_limit])
CALL SAT FROM (SELECT <clause column> FROM ...) [LIMIT n]
```

The result has one row with these columns: `status`, `verified`, `evidence`,
`vars`, `clauses`, `conflicts`, `solve_ms`, `check_ms`, `model`, `core`,
`core_evidence`.

**Why UNSAT: the core.** On UNSAT, `core` lists the clauses (1-based, in input
order) that are contradictory on their own. It is a minimal unsatisfiable
subset. The solver gives each clause a selector literal and solves under all
of the selectors as assumptions; the failed assumptions it returns (`analyze_final`)
are a first core. That core is then shrunk by deletion: each clause is dropped
if the rest is still UNSAT. The core is checked, not trusted. The subset alone
is refuted again with a DRAT proof that the independent RUP checker verifies.
Each clause is shown to be needed by a model of the others that is checked
against every one of them (up to 256 clauses):

```
CALL SAT('p cnf 3 6\\n1 2 0\\n-1 2 0\\n1 -2 0\\n-1 -2 0\\n3 0\\n-3 1 0\\n')
| UNSAT | 1 | ... | core = 1 2 3 4 | minimal core of 4 clauses: refuted (proof verified), and each one needed (a checked model of the rest) |
```

A pigeonhole 5 -> 4 hidden among 40 satisfiable noise clauses came back as a
core of exactly its 45 clauses (21..65). `core` is empty on SAT. When the
conflict limit stops the shrinking, `core_evidence` says the core is
unsatisfiable but not proven minimal.

`CALL SAT` unescapes `\n` itself. The console tokenizer has already consumed
one backslash, though, so from a script or `exec` you must write `\\n`:

```
sql query "CALL SAT('p cnf 2 3\\n1 2 0\\n-1 0\\n-2 0\\n')"
sql query "CALL SAT('p cnf 3 2\\n1 -2 0\\n2 3 0\\n')"
```

```
| UNSAT  | 1 | proof verified (0 lemmas, independent RUP/RAT checker) | 2 | 3 | 0 | 0.050 | 1.098 | NULL    |
| SAT    | 1 | model checked against every clause                     | 3 | 2 | 0 | 0.016 | 0.000 | -1 -2 3 |
```

**Check the `clauses` column.** With a single `\n` the CNF reaches the parser
as one line. The parser drops everything on the `p` line after the header,
so the solver sees zero clauses, and an UNSAT formula comes back
`SAT, verified=1, clauses=0`. See Defects.

### The benchmark standing

The numbers in `tools/proof/sat/results/` are aios-sat against kissat. They
were reproduced here with the repo's report scripts from the TSVs on disk;
no benchmark was re-run.

- **Fair paired comparison** (`pair-v4.tsv`, `bench-pair.py`): 70 SAT
  Competition 2023 main-track instances, each in its original order plus two
  shuffles. kissat ran on core 2 and aios on core 3 at the same time, one
  thread each, with a 60 s limit, scored by PAR-2:

      runs=210 instances=70 | PAR-2 kissat 5803 aios 9449 (aios/kissat 1.63) | solved kissat 177 aios 154 | disagreements 0
      instances where aios median < kissat median: 25 of 70

  kissat is ahead overall (1.63× on PAR-2) and solves 23 more runs. aios is
  faster on 25 of the 70 instances, and never disagreed on an answer.

- **One core each, sequential** (`bench.py`, `bench-report.py LIMIT files`):

  | set | instances | kissat PAR-2 | aios PAR-2 | wins aios/kissat | disagreements |
  |---|---|---|---|---|---|
  | SATLIB uf250 (SAT) | 100 | 73.7 s | 26.1 s | 61 / 39 | 0 |
  | SATLIB uuf250 (UNSAT) | 100 | 498.6 s | 329.0 s | 98 / 2 | 0 |
  | SATLIB structured | 36 | 8.4 s | 7.8 s | 17 / 19 | 0 |
  | SC2023 subset, 100 s (`bench-sc`, `-sc2`, `-sc3`) | 70 | 462–557 s | 3429–6790 s | 26–31 / 39–44 | 0 |

  On small random 3-SAT, aios-sat beats kissat. On competition instances it
  is several times behind, and the gap is mostly timeouts.

The data can be rebuilt with `tools/proof/sat/fetch-benchmarks.sh [satlib]
[refs] [sc2023]`. It fetches the SATLIB sets (sha256-pinned), kissat and
drat-trim at the commits that were measured, and the SC2023 instances.
**Not driven here**: it downloads from the internet.

---

## grain

grain is the machine's own language: a lexer, a parser, a bytecode compiler
and a VM in `src/userland/grain_*.cpp`. It has no kernel include; the console
hands it a `sys` map and SQL builtins (`cli/grain_cmd.cpp`).

### Running it

```
grain help
grain run <file.gr>        compile a file and run it (step limit 10,000,000)
grain eval <source...>     one line; prints its value
grain dis <file.gr>        the bytecode
grain serve <file.gr>      no step limit, and the only mode that gets sockets
grain repl                 one VM across lines, until EOF or .quit
```

The examples in `examples/grain/`, run with
`AIOS_NO_ACCEL=1 aios --root R exec 'grain run examples/grain/<f>.gr'`:

```
=== tour            ... tour: every check agreed           (42 checks)
=== selftest        ... selftest: 131 checks, all agreed
=== closures
shared upvalue: 3 (want 3)
independent: 3 1 (want 3 1)
per-iteration capture: [0, 1, 4, 9, 16] (want [0, 1, 4, 9, 16])
two levels up: 70 (want 70)
=== fib
fib(9) = 34
fib(24) = 46368 in 12 ms
=== words
distinct words: 13
primes under 40: [2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37]
aios on x86_64 with 4 cores
```

Other examples need more than a terminal:

- `httpd.gr` needs `grain serve` and a port.
- `calculator.gr`, `navicat.gr` and `test_windowing.gr` need the desktop or
  window builtins.
- `host.gr` and `bench.gr`.

### The language

Keywords: `let fn if else while for in return break continue and or not
true false nil`.

Types: `int`, `real`, `string`, `bool`, `nil`, `list`, `map`, and functions,
which are closures and first-class values. Blocks use `{ }`, statements need
no separators, and `#` starts a comment.

```
let failures = 0
fn check(label, got, want) {
  if got == want { print("  ok   ", label, "=", repr(got)) }
  else { failures = failures + 1  print("  WRONG", label, "got", repr(got)) }
}
check("division is real", 7 / 2, 3.5)
check("negative index", "grain"[-1], "n")
let m = {a: 1, b: 2}
m.c = 3                                     # field set; keys keep insertion order
for k in {x: 1, y: 2} { print(k) }          # for over a map walks its keys
check("closure captures", (fn () { let k = 10 return fn (x) { return x + k } })()(5), 15)
check("ternary", 10 > 5 ? "a" : "b", "a")
check("f-string", f"calc: {10 + 20 * 2}", "calc: 50")
check("pcall catches", pcall(fn () { error("boom") }).ok, false)
```

Semantics checked by `tour.gr` and `selftest.gr`:

- `/` always gives a real; `int(7/2)` truncates.
- `0` and `""` are truthy; only `nil` and `false` are falsy.
- Integers are 64-bit.
- Strings index by byte: `len` of a UTF-8 string is its byte count.

The builtins are in `src/userland/grain_builtins.cpp`, about 300 of them,
covering:

- strings, lists and maps;
- math, JSON, base64, hex and URL encoding;
- `pcall`, `try_call`, `error`;
- `clock`, `time_ms`, `rand`, `seed`;
- files (`read_file`, `write_file`, `list_dir`, ...), on host paths;
- the `canvas_*` and `draw_*` drawing set;
- `sql()`, `sql_on()`, `asql_query`;
- `sat()`.

Under `serve`, sockets are added: `listen`, `accept`, `read_until`, `udp_*`,
`tcp_*`. `sys` carries `arch`, `host`, `cpus`, `memory`, `kernel`,
`uptime_ms`, `entities`, `memory_objects` and `memory_bytes`.

### `sat()` from grain

`sat(clauses [, conflict_limit])` takes either a list of lists of non-zero
ints or DIMACS text. It returns `{status, verified, evidence, conflicts,
model?}`, and it is deterministic, so a Thing can call it. On UNSAT it also
returns `core` (0-based clause indices: the verified minimal core described
under `CALL SAT`), `core_verified` and `core_evidence`.

```
let php = [[1, 2], [3, 4], [5, 6], [-1, -3], [-1, -5], [-3, -5], [-2, -4], [-2, -6], [-4, -6]]
let r = sat(php)
print(f"php 3->2: {r.status}, verified={r.verified}: {r.evidence}")
let s = sat("p cnf 3 2\n1 -2 0\n2 3 0\n")
print(s.status, s.model, s.evidence)
```

```
php 3->2: UNSAT, verified=true: proof verified (1 lemmas, independent RUP/RAT checker)
SAT [-1, -2, 3] model checked against every clause
```

### Errors, the step limit, bytecode, the REPL

A runtime error prints a stack trace, and the command fails with exit 1:

```
$ cat bad.gr
fn inner(x) { return x.nope + 1 }
fn outer() { return inner(5) }
outer()
$ aios ... exec 'grain run bad.gr'
bad.gr:1: a int has no field 'nope'
  in inner (line 1)
  from outer (line 2)
  from bad.gr (line 3)
FAILED: invalid: bad.gr did not run to completion
```

A runaway program stops at the step limit; it does not hang the console:

```
$ echo 'let i = 0 while true { i = i + 1 }' > spin.gr; aios ... exec 'grain run spin.gr'
spin.gr:1: step limit of 10000000 reached
  in spin.gr (line 1)
```

`grain dis` shows the compiled bytecode:

```
$ echo 'fn add(a, b) { return a + b } print(add(2, 3))' > dis.gr; aios ... exec 'grain dis dis.gr'
== fn dis.gr (0 param(s), 25 bytes, 5 constant(s)) ==
0000    1 CLOSURE 0  ; fn add
0003    | DEFGLOBAL 1  ; "add"
0006    | GETGLOBAL 2  ; "print"
...
== fn add (2 param(s), 8 bytes, 0 constant(s)) ==
0000    1 GETLOCAL 1
0002    | GETLOCAL 2
0004    | ADD
0005    | RET
```

The REPL keeps one VM across lines:

```
$ printf 'let n = 5\nn * 2\n.quit\n' | aios ... exec 'grain repl'
grain — end with .quit or EOF
10
```

### The AArch64 JIT

`src/userland/grain_jit_arm64.cpp` is a template compiler. Each bytecode
instruction becomes a short run of AArch64 code that works on the VM's own
value stack.

- **Compiled inline**: locals, constants, integer arithmetic and comparison,
  branches, and loop back-edges.
- **Handed back to the interpreter, one instruction at a time**
  (`Vm::step_one`): everything else, including strings, lists, maps,
  globals, natives, calls, returns and closures.

This means native and interpreted frames interleave, and errors and traces
are identical in both modes.

Where it applies:

- **Any AArch64 build.** On the Pi (metal) the heap is executable. On arm64
  Linux the code pages come from `mmap`.
- **Nowhere else.** On x86_64, `jit::compile` always returns null and
  everything is interpreted. That includes the `build/aios` used for this
  page.
- **On by default.** `Vm::jit_enabled` defaults to true. The hosted `grain`
  command has no switch for it.

The Pi console (`metal/arm64/grain_shell.cpp`) adds these commands:

```
grain <source...>          run one line
grain run FILE [OUTFILE]   a program from the SD card (step limit 200,000,000 with OUTFILE)
grain repl <source>        the persistent session;  grain load FILE / grain reset
grain jit on|off           native code (default) or the interpreter
grain prof                 JIT profile since the last prof, then reset: slow calls (natives, first calls,
                           arity/depth errors), cycles per return / get_global / interpreter step, and which
                           opcodes fell back to the interpreter
```

**Not driven here.** `grain jit` and `grain prof` exist only on the Pi
board's console; `prof` reads the PMU cycle counter at EL1. To compare the
JIT with the interpreter without a board, use `tools/grain-jit/check.sh`. It
cross-builds grain with `aarch64-linux-gnu-g++` and runs each listed example
plus the 11 programs in `tools/grain-jit/cases/` under `qemu-aarch64-static`,
twice: native, then with `GRAIN_NOJIT=1`. Outputs must match line for line,
and both runs must end with zero live allocations (`GRAIN_LEAKS=1`). Both
tools are installed here, but the script compiles a fresh tree, so it was not
run for this page (this page's rule was no rebuilding).

`tools/proof/grain/grain-bench.cpp` is a standalone runner with no step limit,
for profiling a program with `-pg`.

---
