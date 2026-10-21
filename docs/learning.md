# Learning: training and inference on the compute fabric

This page covers the model code that ships inside `aios`:

- **`lm`**: a small transformer decoder with a byte-level BPE tokenizer.
  Both are written in this repo. Training runs either as a scalar CPU loop or
  as compute graphs that the scheduler places on any device, remote peers
  included.
- **`jepa`** (alias `world-model`): a predictor that maps the machine's
  measured state plus an action to the next state, and a policy head that
  picks actions. It trains on transitions recorded from the machine's own
  counters.
- **`infer`**: runs a Qwen2-architecture checkpoint (safetensors weights, an
  optional LoRA adapter) on this machine's cores, and serves it as an
  OpenAI-compatible chat endpoint.
- **`qwen`** (aliases `llm`, `spark`) and **`aios models`**: HTTP clients for
  model servers, `aios infer serve` among them. `qwen local` runs the `infer`
  model in-process.

Graphs, tensors, devices, `dev=` selectors, accelerators and `AIOS_NO_ACCEL`
are covered in [compute.md](compute.md). This page assumes them. The `tensor`
command is documented there under [Compute graphs](compute.md#compute-graphs).

Every example below was run against `build/aios` on an x86_64 host with 4
cores and a GTX 1080 Ti. Each run used a throwaway root under
`/tmp/aios-docs-learn/`, was pinned with `taskset -c 3`, and had
`AIOS_NO_ACCEL=1` set, except where a GPU run is labelled as one. Output is
trimmed but not edited.

Sources: `cli/commands.cpp` (`cmd_lm`, `cmd_jepa`, `cmd_qwen`, `cmd_node`),
`cli/model_controller.{h,cpp}`, `cli/main.cpp` (`models` mode, start-up
resume), `include/aios/intelligence/language_model.hpp`,
`src/intelligence/language_model.cpp`,
`include/aios/intelligence/world_model.hpp`, `src/intelligence/world_model.cpp`,
`src/hal/linux_host.cpp` (hosted telemetry and workload),
`include/aios/intelligence/qwen_mind.hpp`, `src/channel/qwen_mind.cpp`,
`include/aios/userland/llm.hpp`, `src/userland/llm.cpp`,
`src/userland/llm_kernels.inc`, `include/aios/userland/bpe.hpp`,
`src/userland/bpe.cpp`, `include/aios/channel/llm_serve.hpp`,
`src/channel/llm_serve.cpp`, `tools/llm-drive.cpp`.

---

## Running commands

All of these are console commands, run as in compute.md:

    aios --root DIR exec 'cmd' ['cmd' ...]
    aios --root DIR shell

`aios models` is the exception. It is a separate mode of the binary (see
[`aios models`](#aios-models)).

---

## `lm`: the built-in language model

### What it is

| | |
|---|---|
| tokenizer | byte-level BPE: 256 byte tokens plus 256 learned merges, so 512 tokens. Every input decomposes into bytes, so there is no unknown token. |
| model | pre-LN causal decoder: `d_model 64`, 2 layers, 4 heads, `d_ff 128`, context 64 tokens, GELU, learned position embeddings (`TinyLmConfig` in `language_model.hpp`) |
| parameters | 137,216 |
| optimizer | AdamW, β1 0.9, β2 0.999, default lr 3e-3, batch of 4 sequences |
| corpus | 107 lines compiled into the binary (`kPairs` and `kFacts` in `language_model.cpp`) |
| init seed | 20260907, fixed, so a fresh model is reproducible |

The corpus is fixed. 91 lines pair a request with the console command it
means, written as `request >>> command`, for example
`show me the ocean at dusk >>> display show ocean at dusk`. The other 16 are
sentences describing the system. There is no command or flag that trains on
your own text, and the model shape cannot be changed from the console. To
change either you edit `kPairs`/`kFacts` or `TinyLmConfig` and rebuild.

What the model is used for: it picks one of the trained commands for a
request by likelihood, and prints a greedy continuation. `lm status` says
this itself.

### Commands

| command | what it does |
|---|---|
| `lm status` | shape, vocabulary, steps, last loss, whether weights persist. Trains first if the root has no weights. |
| `lm train [steps] [lr] [steps=N] [lr=X] [save_every=N] [corpus=<file>] [engine=graph\|scalar] [device=<prefix>]` | `steps` defaults to 200 and `lr` to 3e-3, given as the first two non-`key=value` words or as `steps=`/`lr=`. Saves every `save_every` steps (default 50; 0 = only at the end) and at the end. `corpus=<file>` trains on your own text: one sequence per non-empty line (longer lines split into 64-token windows), same tokenizer. |
| `lm tokenize <text>` | token ids and the bytes each one stands for, plus a round-trip check |
| `lm check [n] [sabotage=<slot>]` | the backward pass against central finite differences (n sampled parameters, default 12). `sabotage=` negates one parameter tensor's analytic gradient first, to show the check fails on a wrong backward pass. |
| `lm ask <request>` / `lm say <request>` | inference: the command chosen, its NLL, and the raw greedy continuation |

### First use, and what lands in the root

The first command that needs the model builds the corpus, trains the
tokenizer, runs 60 scalar steps, and writes the result:

```
$ aios --root r1 exec 'lm status'
The machine's own language model
  state           : ready
  trained here: 512 token vocabulary, 137216 parameters, 60 steps, loss 4.715167
  vocabulary      : 512 tokens (256 learned merges over 256 bytes)
  shape           : d_model 64, 2 layer(s), 4 head(s), d_ff 128, context 64
  parameters      : 137216
  trained for     : 60 step(s), last loss 4.71517
  corpus          : 107 line(s) this machine wrote about itself
  weights persist : yes, under the kernel root
real	0m1.129s

$ ls -la r1
-rw-r--r-- 1 arch arch 555563 language-model.bin
...
```

`<root>/language-model.bin` is the only checkpoint. It holds, in order:

1. a 64-bit FNV-1a fingerprint of the corpus;
2. the tokenizer's merges;
3. the model: magic `TLM2`, the shape, the step count, the last loss,
   every parameter as float32, then the AdamW moments `m` and `v`. (A `TLM1`
   file, without moments, still loads; its moments start at zero.)

On load, a fingerprint that does not match the compiled-in corpus causes a
retrain instead of a load. `lm train` rewrites the file every
`save_every` steps and at the end, moments included, so a run resumed in a
new process continues the same optimisation. Driven: `lm train 5` then
`lm train 5` in one process, and the same two runs as two processes, both
end at loss 2.52797 (before this fix the two-process run ended at 3.44485).

### `engine=scalar` and `engine=graph`

`engine=graph` is the default. Each step is built as `ComputeGraph`s and
handed to the kernel scheduler: one graph per sequence for the forward and
backward passes (matmul, bias-add, layernorm, causal-attention, gelu,
softmax-ce-loss, and their `-grad` ops), then one graph of `adam-step` ops,
one per parameter tensor. The host does only the embedding gather and its
gradient scatter. With this model that is 426 operations per step.

`engine=scalar` is the hand-written CPU loop the graph is checked against.

Same root, same 25 steps, both engines:

```
$ aios --root r1 exec 'lm train 25 3e-3 engine=scalar'
loaded 137216 parameters trained for 60 steps (loss 4.715167)
  step 25/25  loss 5.49672
loss 4.71517 -> 5.49672 over 25 step(s) at lr 0.003
total steps 85, 137216 parameters
engine: the scalar CPU pass (no graph, no device)
weights written under the kernel root
real	0m0.414s

$ aios --root r2 exec 'lm train 25'          # r2 is a copy of r1 before the run
  step 25/25  loss 5.49656
loss 4.71517 -> 5.49656 over 25 step(s) at lr 0.003
graph: 10650 operation(s) in 412.9 ms on [cpu x10650]
weights written under the kernel root
real	0m0.692s
```

From one starting point, 2 steps gave these losses:

| run | loss after 2 steps |
|---|---|
| `engine=scalar` | 1.30613 |
| `engine=graph device=cpu` | 1.30608 |
| `engine=graph device=cuda` (GPU run) | 1.30608 |

On the CPU, the graph engine costs about 1.7x the scalar loop's wall time at
this size.

**How to read the loss.** The printed number is the mean loss of the batch
for that step. Each `lm train` restarts its walk through the corpus at line
0. A short run can therefore print a loss higher than the one it started
from. The "before" figure is from a different batch, so it is not a
regression. A longer run does go down:

```
$ aios --root r5 exec 'lm train 100 3e-3 engine=scalar'     # fresh root: 60 warm-up steps, then 100
  step 25/100  loss 5.10806
  step 50/100  loss 4.06131
  step 75/100  loss 2.39262
  step 100/100  loss 1.80696
loss 4.71517 -> 1.80696 over 100 step(s) at lr 0.003
total steps 160, 137216 parameters
```

(Before the checkpoint carried the moments, the same 100 steps run after a
reload ended at 1.88041 instead of 1.80696.)

A step is counted, and its loss kept, only once the optimizer has actually
run: a graph that fails leaves the step count and the loss where they were.

### Devices: `device=`

`device=<prefix>` pins every operation in the step to devices whose name
starts with the prefix. Without it, the scheduler places each operation
itself. A prefix that matches nothing stops the run before any weights
change:

```
$ aios --root r6 exec 'lm train 2 3e-3 device=cuda'           # AIOS_NO_ACCEL=1
  step 1: the graph did not run — not-found: no device answering to 'cuda' in this kernel's fabric
FAILED: io-error: training stopped: not-found: no device answering to 'cuda' in this kernel's fabric

$ aios --root r6 exec 'lm train 2 3e-3 device=cpu'
graph: 852 operation(s) in 24.3 ms on [cpu x852]
```

GPU run, with `AIOS_NO_ACCEL` unset (device names come from `devices`, see
[compute.md](compute.md#devices-and-placement)):

```
$ aios --root g1 exec 'devices' 'lm train 2 3e-3 device=cuda' 'lm train 2 3e-3 device=vulkan'
fc19758033a2  gpu       3584    0       10.91 GiB     device    cuda:NVIDIA GeForce GTX 1080 Ti cc6.1 [0]
62d1f3ffb26d  gpu       16      0       11.24 GiB     device    vulkan:NVIDIA GeForce GTX 1080 Ti api1.3 [0]
...
graph: 852 operation(s) in 120.0 ms on [cuda:NVIDIA GeForce GTX 1080 Ti cc6.1 [0] x852]
weights written under the kernel root
  step 1: the graph did not run — unsupported: no candidate device has a kernel for layernorm on 'vulkan' (2 candidate(s))
FAILED: io-error: training stopped: unsupported: no candidate device has a kernel for layernorm on 'vulkan' (2 candidate(s))
real	0m0.923s
```

CUDA has kernels for every operation the step uses. Vulkan has no
`layernorm`, so a Vulkan-pinned step is refused. Unpinned, on the same
machine, the scheduler kept nearly everything on the CPU:

```
graph: 852 operation(s) in 71.4 ms on [cpu x850] [cuda:NVIDIA GeForce GTX 1080 Ti cc6.1 [0] x2]
```

At this model size, pinning to CUDA (120 ms for 2 steps) is slower than the
CPU graph (24 ms), because each operation is small and pays for its own
transfers.

### Remote pools: `device=remote:…`

A peer kernel's devices join the fabric with the name
`remote:<host>:<port>/<device>` once you connect to it. Here a second root
served its kernel on loopback, and the training root connected to it:

```
$ aios --root srv attend 120 "node serve 13510 docs-learn-token-0001"
attending — kernel c555ac323372, console 3546e4b7b9d0, for 120s
serving this kernel on port 13510 (peers act here as guest entities; link authenticated and encrypted with the token)
```

The token must be at least 16 characters, because it keys the encrypted
link. A shorter one is refused.

```
$ aios --root r7 exec 'node connect 127.0.0.1 13510 docs-learn-token-0001' 'devices' \
                      'lm train 1 3e-3 device=remote:'
attached node 127.0.0.1:13510: 1 device(s) [remote:127.0.0.1:13510/cpu], provides remote memory, transport network:127.0.0.1:13510
DEVICE ID     TYPE      LANES   BUSY    MEMORY        NATIVE    NAME
1f3af05c4a14  cpu       4       0       19.49 GiB     system    cpu
79830301c474  remote-cpu4       0       19.49 GiB     remote    remote:127.0.0.1:13510/cpu
loaded 137216 parameters trained for 160 steps (loss 1.806958)
  step 1/1  loss 1.07708
graph: 426 operation(s) in 191.2 ms on [remote:127.0.0.1:13510/cpu x426]
weights written under the kernel root
```

A full device name pins to that one peer. An unknown peer fails like any
other selector:

```
$ aios --root r7 exec 'node connect 127.0.0.1 13510 docs-learn-token-0001' 'lm train 2' \
       'lm train 2 3e-3 device=remote:127.0.0.1:13510/cpu' 'lm train 2 3e-3 device=remote:nosuch'
graph: 852 operation(s) in 42.5 ms on [cpu x852]
graph: 852 operation(s) in 402.2 ms on [remote:127.0.0.1:13510/cpu x852]
  step 1: the graph did not run — not-found: no device answering to 'remote:nosuch' in this kernel's fabric
```

Unpinned, the scheduler left everything on the local CPU. Pinned to the
remote device over loopback, a step costs about 0.45 ms per operation,
against about 0.03 ms locally. Every operation is a round trip. Across a real
network it will cost more.

The connection lasts only as long as the process that made it, so
`node connect` has to run in the same `exec` (or shell) as the training.

### The gradient check

```
$ aios --root r5 exec 'lm check 6'
Gradient check against central differences (h = 1e-3):
  L0.bq[27]  analytic 0.00475645  numeric 0.00441074  rel err 0.072681
  L1.ln1_b[51]  analytic -0.00574833  numeric -0.00584125  rel err 0.0159082
  L1.W1[5302]  analytic -0.00430059  numeric -0.00411272  rel err 0.0436847
  L0.W1[5154]  analytic 0.0102609  numeric 0.0103116  rel err 0.00491221
  resolution floor: 0.000550471 (the loss is a float32; a central difference cannot see a gradient under this)
  skipped as unmeasurable: 2
  worst relative error over the rest: 0.072681
Directional derivative over ALL 137216 parameters at once (h = 1e-2) - ...
  analytic g.d 0.0163432  numeric dL/da 0.0163317  rel err 0.000707984
  analytic g.d 0.00790082  numeric dL/da 0.00789762  rel err 0.00040526
  analytic g.d 0.0345774  numeric dL/da 0.0345767  rel err 0.0000227326
  worst over 3 random direction(s): 0.000707984
the derivatives agree with the function they are supposed to differentiate
```

Per-parameter errors of a few percent are float32 noise. The pass/fail is
the directional test over three random directions across all parameters.
Each numeric derivative is taken at step `h` and at `h/2`, and a direction
agrees when `|numeric - analytic| <= 2% of the magnitude + the float32 floor
+ 2 x |numeric(h) - numeric(h/2)|`. (A plain 2% relative test reported
`THE DERIVATIVES DO NOT AGREE` on a correct pass whenever `g·d` was ~0.0008.)
`sabotage=` shows it still catches a wrong one:

```
$ aios --root r exec 'lm check sabotage=4'
SABOTAGED: parameter tensor 4's analytic gradient is negated on purpose
  analytic g.d -0.0120589  numeric dL/da -0.00854731 (half step -0.00849366)  |diff| 0.00351156 vs tolerance 0.000729935  DISAGREES
  ...
THE DERIVATIVES DO NOT AGREE - the backward pass is wrong
```

### Tokenizing and inference

```
$ aios --root r5 exec 'lm tokenize show me the ocean at dusk'
3 token(s) for 25 byte(s):
  295  "show me the "
  362  "ocean at dus"
  107  "k"
round trip: identical
```

`lm ask` scores `<request> >>> <command>\n` under the model for every
command in the corpus and takes the one with the lowest mean NLL. It then
prints a greedy continuation of `<request> >>> `, up to 24 tokens, cut at
the first newline (every token whose bytes contain `\n` ends it). In
`generate()`, seed 0 is greedy and any other seed samples from the model's
distribution with that seed; `lm ask` uses 0. The console acts on the
model's mapping only under 1.0 nats/token, and `lm ask` reports against the
same bar: above it, `answered by` says nobody answered with confidence and
prints the best guess beside the keyword table's answer. The `state` line
reads the hosted telemetry.

```
$ aios --root r5 exec 'lm ask show me the ocean at dusk' 'lm ask how many packets are we dropping'
request     : show me the ocean at dusk
answered by : the model
command     : display show ocean at dusk
chosen at   : 1.24842 nats/token
generated   : " >>> display show ocean at dusk
deserdevices
kocean at dusk
 >>> display show ert >>> display show pyramid
 ..."
request     : how many packets are we dropping
answered by : the model
command     : checkpoint console
chosen at   : 5.73379 nats/token
generated   : "e
 >>> houstentioe list
 >>> oes ouihnet and ant ti >>> ing  >>> er"
```

The first request is a line of the corpus, so it is matched. The second is
not, and the model picks `checkpoint console` at 5.7 nats/token. The model
has no notion of "none of these". The NLL is the only signal you get, and a
high one means the mapping should not be trusted. `lm ask` only prints the
command. It does not run it. With no usable weights, `answered by` reads
`the keyword fallback` and a fixed keyword table picks the command.

(The `generated` lines above are from before the newline fix; now the
reply ends at its first newline, e.g. `generated   : "ware you >>> identity"`.)

---

## `jepa` / `world-model`: the machine-state predictor and policy

### What it is

- **State `s_t`**: a 1024-float vector. The first 8 entries are measured.
  Hosted, they come from Linux's counters (`linux_telemetry` in
  `src/hal/linux_host.cpp`): cpu load, NIC rx depth, context-switch rate,
  heap pressure, compute occupancy, a live flag, packet loss, and load
  imbalance.
- **Action `a_t`**: a 128-float vector. Four entries are decoded into
  settings: `a_t[0]` scheduler slice, `a_t[1]` NIC refill window,
  `a_t[2]` display scene, `a_t[3]` compute quota percentage. `a_t[4..127]`
  are read by nothing.
- **Predictor** (`JEPAWorldModel`): a residual 2-layer MLP,
  1152 → 512 → 1024, plus a skip from `s_t`. It is trained on the CPU with
  AdamW (decoupled weight decay 1e-4 on the weights, none on biases) on MSE
  against the recorded `s_{t+1}`. The backward pass uses the exact derivative
  of the activation the forward computes (a rational-tanh GELU), at the
  pre-activation. `evaluate_loss` adds 0.1 x a VICReg-style variance hinge
  (no covariance term); training does not use it.
- **Policy head** (`ActionPolicyHead`): an MLP, 1024 → 256 → 128, with
  Gaussian exploration (σ 0.15). It is trained by REINFORCE against
  `measured_reward`: context-switch cost, plus 4x packet loss, plus 2x load
  over saturation, negated. Optimiser: plain Adam (no decay).
- **Trace buffer** (`ExecutionTraceLogger`): holds up to 1000
  `(s_t, a_t, s_{t+1}, reward)` samples.

Hosted, only `a_t[3]` is applied: it becomes a quota on every non-kernel
entity. On bare metal `metal_actuate` applies `a_t[0]`, `a_t[1]` and `a_t[2]`
and not the quota. `jepa status`, `tick` and `daemon` say per lever what the
sink bound in this process did (APPLIED / DECLINED here), never assuming it.
Losses read `not trained yet` until a step has run, and the trace count is
split into recorded and synthetic (the state's live flag, kept through a
checkpoint).

### Commands

| command | what it does |
|---|---|
| `jepa status` | steps, losses, return, trace count, backend, live telemetry, and what each lever maps to |
| `jepa synthesize [count=N]` (`gen`) | adds generated transitions. They are labelled synthetic. |
| `jepa record [count=N] [pressure=0..3]` | real transitions: read the state, act, run a workload, read again, score. Refuses without a telemetry provider. |
| `jepa train [steps=N] [batch=N]` | default 50 steps, batch 64, over the trace buffer. If the buffer is empty it synthesizes 500 first and says so; a return over synthetic traces is labelled as such. CPU, plus whatever matmul offload took products (it reports how many). Writes checkpoints. |
| `jepa daemon [iters=N] [batch=N] [checkpoint=N]` | the online loop: record one transition and take one step, repeated. Checkpoints every N iterations (default 50) and at the end. |
| `jepa tick [count=N]` | perceive, act, predict, publish to the shared-state region; no training |
| `jepa predict`, `jepa policy` | one forward pass on a constant 0.1 state |
| `jepa bench [steps] [batch]` | the predictor's training step timed on CPU and then on the GPU matmul offload. It also trains the in-memory model; that training is not saved. |
| `jepa latency [count=N]` | the predictor's forward pass timed N times (default 1000), with a verdict computed from that measurement |
| `jepa sweep [repeats]` | holds `a_t[0]` at five settings and reports whether the reward responds |
| `jepa gpu on\|off` | binds or unbinds the predictor's matmul offload |
| `jepa export <path>` / `jepa import <path>` | copies `<path>` and `<path>.policy`; export makes missing directories, import writes the root's checkpoints |
| `jepa shm`, `jepa layout` | shared-region dimensions; a fixed action vector pushed through the predictor and published to the shared region (nothing is drawn, and it says so) |
| `jepa reboot-resume` | load checkpoints. `cli/main.cpp` runs it at every start. |

The hosted workload (`linux_set_settle_pressure`) has four levels:

| level | load |
|---|---|
| 0 | one thread, 10 ms |
| 1 | one thread per CPU, 20 ms |
| 2 | 3 per CPU, 30 ms |
| 3 | 8 per CPU plus a loopback UDP stream with a small receive buffer, so drops occur |

`record` cycles through all four unless you pass `pressure=`. The `daemon`
uses whatever level was set last; the default is 1.

### Synthesize, train, and the checkpoint files

```
$ aios --root j1 exec 'jepa status' 'jepa synthesize 200' 'jepa train steps=3 batch=16'
JEPA Substrate World Model Status:
Latent State Dimension: 1024 (R^1024)
Action Vector Dimension: 128 (R^128)
Predictor Training Steps: 0
...
Predictor Backend: scalar CPU (freestanding kernel core)
Telemetry: LIVE (substrate provider registered)
  s_t[0] cpu load        0.17973
  s_t[3] heap pressure   0.90372
  s_t[7] load imbalance  0.25  (runnable processes per processor, 1.0 = four times oversubscribed)
Actuation: LIVE — sink: hosted (this CLI's sink: quota only)
  a_t[0] -> scheduler slice  10 tick(s)  [DECLINED here: Linux owns the timer]
  a_t[1] -> nic refill window 17 buffer(s) posted, prefetch off  [DECLINED here: Linux owns the receive ring]
  a_t[2] -> display scene    1 (cyber)  [DECLINED here: only an explicit `display` request writes j1/run/scene]
  a_t[3] -> compute quota    39% of measured device time per period  [APPLIED here]
[JEPA TRACE SYNTHESIZER] WARNING: synthetic, not measured — use `jepa record` on metal.
Generated 200 synthetic operational state transition triples (s_t, a_t, s_{t+1}).
[JEPA TRAIN] Completed 3 backpropagation step(s): all on the CPU (no matrix product was taken by an offload).
Checkpoint written: 13387904 B predictor + 3543696 B policy + 1749632 B traces (200 transition(s))
World Model Prediction Loss (MSE): 0.00114081
Action Policy Surrogate Objective (-A*log pi): 0.0360624
Total Cumulative World Steps: 3
real	0m0.677s

$ ls -la j1
-rw-r--r-- 1 arch arch  3543696 jepa.policy
-rw-r--r-- 1 arch arch 13387904 jepa.predictor
-rw-r--r-- 1 arch arch  1749632 jepa.traces
```

The three files sit directly under the root: weights with their AdamW
moments, the policy with its moments and return baseline, and the trace
buffer. A checkpoint that exists but does not match the model's shapes is
refused by name, not loaded silently. Every later start of that root
restores them:

```
jepa restored — predictor at 3 step(s), policy at 3 step(s), 200 transition(s): 0 recorded, 200 synthetic
```

### Recording real transitions and the online loop

```
$ aios --root j1 exec 'jepa record count=8 pressure=0' 'jepa daemon iters=4 batch=16 checkpoint=2' 'jepa tick'
[JEPA RECORD] Logged 8 MEASURED (s_t, a_t, s_t+1) transition(s) from this machine.
Trace buffer: 208 sample(s) (source: live substrate telemetry, not the synthesizer)
Measured reward (switch cost + 4x packet loss + 2x load over saturation, negated): mean -1.19396, best -0.835634
State space visited (peak over 8 transition(s)):
  cpu load        up to 1
  switch rate     up to 1
  packet loss     up to 0   (0 of 8 transition(s) had loss in them, mean 0)
  load imbalance  up to 0.5   (8 of 8 past one runnable process per processor, mean 0.46875)
  NO loss was seen. The reward's loss term cannot teach anything from this
  recording, and saying so beats training on it and calling the result learning.
[JEPA DAEMON] 4 live learning iteration(s) — record, act, settle, score, step.
Measured reward: first -1.625 -> last -1.12005
World Model Prediction Loss (MSE): 0.00138435
Checkpoint written: 13387904 B predictor + 3543696 B policy + 1854608 B traces (212 transition(s))
Total Cumulative World Steps: 7
[JEPA TICK] Ran 1 perceive -> policy -> ACT -> predict -> publish cycle(s).
Telemetry: live   Actuation: applied to the substrate
s_t[0..4]   cpu=0.983051 nic=0.0000659943 ctx=0.724728 mem=0.925611
a_t[0..3]   -0.162079 -0.248631 -0.479797 0.224026
s_hat[0..3] 0.991771 0.00286142 0.726701 0.925851
real	0m0.585s
```

Four iterations are far too few to say the policy learned anything. The
change from -1.625 to -1.12 is within noise. On this shared 4-core host the
machine was already loaded (load imbalance 0.5 throughout), and that
dominates the reward.

### Export and import

```
$ aios --root j1 exec 'jepa export /tmp/aios-docs-learn/jexp/wm'
[JEPA EXPORT] /tmp/aios-docs-learn/jexp/wm  13387904 B  (predictor, 7 step(s), weights + AdamW moments)
[JEPA EXPORT] /tmp/aios-docs-learn/jexp/wm.policy  3543696 B  (policy head, 7 step(s), weights + moments + return baseline)

$ aios --root j2 exec 'jepa import /tmp/aios-docs-learn/jexp/wm'
[JEPA IMPORT] predictor restored to 7 step(s), loss 0.00138435
[JEPA IMPORT] policy head restored to 7 step(s), baseline -0.266965
```

`export` makes the directory it names if it is missing. `import` also writes
`jepa.*` into the root, so the next start resumes from it:

```
$ aios --root ji exec 'jepa import /tmp/aios-fix-svc/expg/m.bin'
...
[JEPA IMPORT] checkpoint written: 13387904 B predictor + 3543696 B policy + 32 B traces (0 transition(s))
$ aios --root ji exec 'jepa status' | grep 'Predictor Training Steps'
Predictor Training Steps: 3
```

### Where the predictor's arithmetic runs

`jepa train`, `daemon` and `record` run in scalar C++ on the CPU. The one
exception is the optional matmul offload. The first `jepa` command binds a
GPU device that supports `matmul` and prefers Vulkan. The offload takes only
products over 48M multiply-accumulates (`kOffloadWorthIt` in
`cli/commands.cpp`) and sends the rest back to the CPU. With
`AIOS_NO_ACCEL=1` there is no device to bind:

```
$ aios --root j1 exec 'jepa bench 2 16'
  cpu   : 2 step(s) of batch 16 in 38.4657 ms  (19.2329 ms/step), last loss 0.00110092, products offloaded 0, declined 0
no device took them, so there is nothing to compare
```

GPU run (`AIOS_NO_ACCEL` unset), batch 128:

```
$ aios --root j3 exec 'jepa bench 2 128' 'jepa status'
  cpu   : 2 step(s) of batch 128 in 156.394 ms  (78.197 ms/step), last loss 0.00134496, products offloaded 0, declined 0
  device: 2 step(s) of batch 128 in 105.026 ms  (52.5131 ms/step), last loss 0.00140781, products offloaded 10, declined 0
the device is faster: 1.48909x (78.197 ms/step on the cpu, 52.5131 ms/step on the device)
Predictor Backend: vulkan:NVIDIA GeForce GTX 1080 Ti api1.3 [0] through the compute fabric
GPU Offload: registered - 10 product(s) offloaded, 0 declined, 704M multiply-accumulates on the device
real	0m1.049s
```

A forward pass of the predictor alone (`jepa latency`) took 1.17 ms on the
CPU:

```
$ aios --root j1 exec 'jepa latency count=200'
Total Iterations: 200 calls
Total Execution Time: 233.846 ms
Mean Latency per Call: 1169.23 us (1.16923 ms)
Predictor Throughput: 855.264 ops/sec
Verdict: mean 1.16923 ms per prediction is NOT under 0.1 ms on this machine
```

### As a mind

`mind bind <e> jepa` (also `world-model`, `jepa-cuda`; the intelligence
is named `jepa-world-model` — there is no CUDA path) binds the predictor to
an entity as its `Intelligence`, through
`make_jepa_world_model_intelligence()`. `mind cycle <e>` then runs it.

---

## `infer`: Qwen2 checkpoints in-tree

`infer` loads a Hugging Face checkpoint of the Qwen2 architecture and runs it
in the `aios` process: the tokenizer, the forward pass, the KV cache and the
sampler are all in this repo. Nothing is called out to. The runs in this
section use all four cores, not `taskset -c 3`.

| | |
|---|---|
| checkpoint | a directory with `config.json` (`model_type` `qwen2`), one or more `*.safetensors` files (BF16, F16 or F32) and `tokenizer.json`; `tokenizer_config.json` and `generation_config.json` are read when present |
| adapter | a PEFT LoRA directory (`adapter_config.json`, `adapter_model.safetensors`), applied unmerged as `y += (alpha/r)·B(Ax)` on every projection it names; rsLoRA scaling when `use_rslora` is set. An adapter's own `tokenizer.json` and `tokenizer_config.json` take precedence over the base's |
| tokenizer | byte-level BPE from `tokenizer.json`: the vocabulary, the merge ranks, the added tokens, and Qwen2's pre-tokenizer split, with Unicode letter and number classes generated by `scripts/gen-unicode-classes.py` |
| weights | linear layers kept in BF16 and widened to F32 inside the kernels; norms, biases and the adapter in F32 |
| arithmetic | F32 throughout; RMSNorm, rotary position embedding, grouped-query attention, SwiGLU, tied or separate output head |
| kernels | 8-wide vector code built twice, portable and AVX2+FMA, picked per machine at load (`llm_kernels.inc`); a prompt runs three weight rows against four tokens at a time |
| chat template | the checkpoint's: its default system turn, then `<\|im_start\|>role\ncontent<\|im_end\|>` per turn, the assistant turn left open |
| sampling | `generation_config.json` supplies the defaults (temperature, top_p, top_k, repetition_penalty); temperature 0 is greedy; generation ends at `<\|im_end\|>` or `<\|endoftext\|>` |
| context | 4096 tokens by default (`context=`), capped by `max_position_embeddings`; the cache keeps the prefix a new prompt shares with the last one |

    infer load [model=<dir>] [adapter=<dir>] [context=N]
    infer status
    infer ask <text> [temperature=T] [top_p=P] [max_tokens=N] [seed=S]
    infer serve [port=8000] [name=spark] [public=1 key_file=<file>] [model=<dir>] [adapter=<dir>]

Without `model=` the checkpoint is `AIOS_LLM_MODEL`, else
`<root>/models/default`; without `adapter=`, `AIOS_LLM_ADAPTER`. One model is
resident per process and `infer`, `infer serve`, `qwen local` and every mind
bound with `mind bind <e> local` share it.

```
$ export AIOS_LLM_MODEL=~/models/qwen2.5-0.5b-instruct AIOS_LLM_ADAPTER=~/models/spark-adapter
$ aios --root l1 exec 'infer status' 'infer ask What is 17 times 3?'
model    : /home/arch/models/qwen2.5-0.5b-instruct + adapter /home/arch/models/spark-adapter
shape    : 24 layers, hidden 896, 14 heads (2 kv), ffn 4864, vocab 151936
context  : 4096 tokens
weights  : 1009.28 MiB
system   : You are a helpful assistant.
The answer is 51.
(28 prompt tokens in 113 ms, 7 generated in 406 ms, 17.26 tok/s)
```

With no checkpoint where it looks:

```
FAILED: not-found: no model at l1/models/default: put a Qwen2 checkpoint there (config.json, *.safetensors, tokenizer.json) or set AIOS_LLM_MODEL
```

### Agreement with the reference implementation

On Qwen2.5-0.5B-Instruct, the five likeliest next tokens after
`The capital of France is` and their logits, from `aios-llm-drive <dir> logits`
and from Hugging Face transformers in F32 on the same checkpoint:

| token | aios | transformers |
|---|---|---|
| ` Paris` | 17.2173 | 17.217 |
| ` ______` | 16.3196 | 16.320 |
| `:\n` | 15.6955 | 15.695 |
| `:\n\n` | 15.5711 | 15.571 |
| ` __` | 15.3869 | 15.387 |

With the spark adapter, greedy replies to five prompts were compared with the
spark server (transformers + PEFT in F16 on a GTX 1060). Four were identical
to the character. The fifth (`Write a haiku about the sea.`) opens on a
near-tie, `Wh` 14.976 against `Sil` 14.973 in F32; F16 picks `Sil`, F32 and
`aios` pick `Wh`, and the F32 transformers reply is identical to the `aios`
one:

```
Whispering waves dance,
Sea's embrace holds its breath,
Ocean's heart beats deep.
```

The tokenizer was compared with `AutoTokenizer.encode` on the same text (ASCII,
contractions, digits, runs of spaces and newlines, CJK, accents, emoji, code,
literal special tokens); `tokens` mode prints the ids and checks the decode
round trip.

### Speed

On an i5-7600K (4 cores, AVX2+FMA), Qwen2.5-0.5B with the spark adapter:

| | |
|---|---|
| load | 1.1 s for 1.0 GB of weights |
| prompt | 1,744 tokens in 10.7 s, about 160 tokens/s |
| generation | 17 tokens/s; each token reads every weight once, so this is the memory bandwidth |

### `infer serve`: the chat endpoint

`infer serve` answers the OpenAI chat-completions protocol, so `qwen`,
`aios models`, `mind bind <e> qwen <url> <model>` and ordinary OpenAI clients
use it unchanged:

| path | |
|---|---|
| `GET /v1/models` | the one model, under `name=` (default `spark`) |
| `GET /health` | `{"status":"ok"}` |
| `POST /v1/chat/completions` | `messages` (string content, or a list of `text` parts), `temperature`, `top_p`, `top_k`, `repetition_penalty`, `seed`, `max_tokens` / `max_completion_tokens`, `stop`, `stream` |

It binds 127.0.0.1. `public=1` binds every interface and needs a bearer key
from `key_file=` (or `AIOS_LLM_KEY`); it refuses to start without one:

```
FAILED: denied: refusing to listen on every interface with no API key
```

Requests take turns on the one model. A caller that closes its connection
stops its generation, so the next request does not wait for text nobody will
read.

```
$ aios --root s1 exec 'infer serve model=~/models/qwen2.5-0.5b-instruct adapter=~/models/spark-adapter port=8011'
loaded /home/arch/models/qwen2.5-0.5b-instruct + adapter /home/arch/models/spark-adapter: 24 layers, hidden 896, 14/2 heads, vocab 151936, context 4096, 1009.28 MiB of weights, 1.17 s
llm endpoint listening on 127.0.0.1:8011 model=spark (no key)
chat 28+7 tokens, prefill 110 ms, decode 489 ms
chat abandoned by the caller after 48 tokens
chat 2078+10 tokens, prefill 15066 ms, decode 791 ms

$ curl -s localhost:8011/v1/chat/completions -d '{"messages":[{"role":"user","content":"What is 17 times 3?"}],"temperature":0}'
{"id":"chatcmpl-e1150ff5986c","object":"chat.completion","created":1790948605,"model":"spark","choices":[{"index":0,"message":{"role":"assistant","content":"The answer is 51."},"finish_reason":"stop"}],"usage":{"prompt_tokens":28,"completion_tokens":7,"total_tokens":35}}
```

With `"stream": true` the reply is server-sent events: a `role` chunk, one
`content` chunk per decoded piece, a chunk carrying `finish_reason`, then
`data: [DONE]`. Errors come back as `{"error":{"message":…,"type":…}}` with
400 (bad JSON, no messages, a role other than system/user/assistant, a prompt
longer than the context), 401 (missing or wrong key), 404 and 405.

---

## Model servers: `qwen`, `llm`, `spark`, `aios models`

These are HTTP clients for an OpenAI-compatible endpoint, such as
`infer serve`:

- `GET <base>/v1/models` lists models.
- `POST <base>/v1/chat/completions` sends
  `{"model":…, "messages":[{"role":"user","content":…}], "max_tokens":16384}`
  and reads `choices[0].message.content`, or `choices[0].text`.
- `aios models` also speaks to ollama's `GET /api/tags` and `/api/ps`.

Where they look:

- `QwenClient` (and every network mind made without a URL) takes
  `AIOS_MIND_URL` from the environment; with none, a call fails with
  `no model endpoint configured: set AIOS_MIND_URL (e.g. http://<host>:8000/v1/chat/completions)`
  (`src/channel/qwen_mind.cpp`). The model name defaults to `spark`. The url
  `local` runs the `infer` model in this process instead.
- `AIOS_MIND_KIND=qwen` without `AIOS_MIND_URL` refuses to bind, saying so.
- The `aios models` roster is `ollama http://127.0.0.1:11434`, plus the base of
  `AIOS_MIND_URL` when it is set (`cli/model_controller.cpp`). Set
  `AIOS_MODEL_ENDPOINTS` (`kind=base,...`) to replace the roster.

For the runs below, a stand-in server on `127.0.0.1:13511` listed one model
id, `stub-echo`, and echoed the last user message back, to show exactly what
the client sends and what it parses.

### `aios models`

```
$ AIOS_MODEL_ENDPOINTS=openai=http://127.0.0.1:13511,ollama=http://127.0.0.1:13512 aios --root q1 models stub-echo
models this machine can reach right now:
  stub-echo  via openai on this machine  [resident]
endpoints:
  openai http://127.0.0.1:13511 (this machine): up, 1 model(s)
  ollama http://127.0.0.1:13512 (this machine): unreachable — Connection refused
stub-echo: served by this machine at http://127.0.0.1:13511/v1/chat/completions
```

`AIOS_MODEL_ENDPOINTS` is a comma-separated list of `kind=base`, where kind
is `openai` or `ollama`. The exit code is 0 if any endpoint answered. With a
model name given, it is 1 if no endpoint serves that model.

### `qwen` / `llm` / `spark`

    qwen status [model=<m>] [url=<chat-completions URL>]
    qwen [ask|prompt|query] <text...> [model=<m>] [url=<URL>]
    qwen local status | qwen local <text>

`model` defaults to `spark`. Without `url=`, the client asks the
`aios models` roster which endpoint lists that model. `llm` and `spark` are
the same command. A first word that is not a subcommand is treated as the
start of the prompt.

```
$ aios --root q1 exec 'qwen status model=stub-echo' 'qwen ask model=stub-echo what is two plus two'
Qwen LLM Endpoint: http://127.0.0.1:13511/v1/chat/completions
Target Model: stub-echo
Status: Online (HTTP 200)
Models Payload: {"object": "list", "data": [{"id": "stub-echo", "object": "model"}]}
Querying Qwen LLM (stub-echo @ http://127.0.0.1:13511/v1/chat/completions)...

[Qwen Response]
echo: what is two plus two

$ aios --root q1 exec 'spark hello there url=http://127.0.0.1:13511/v1/chat/completions'
Querying Qwen LLM (spark @ http://127.0.0.1:13511/v1/chat/completions)...
[Qwen Response]
echo: hello there
```

The request the stand-in server received:

```
POST /v1/chat/completions {"model":"spark","messages":[{"role":"user","content":"hello there"}],"max_tokens":16384} auth=None
```

With no endpoint listing the model:

```
FAILED: not-found: no reachable endpoint lists a model named 'spark' — `aios models` lists what is reachable, or give url=
```

`qwen local` is the in-process path: the `infer` model, loaded on first use.

```
$ aios --root l1 exec 'qwen local status' 'qwen local Who are you?'
Qwen in-process endpoint: the in-tree model (infer)
Model: /home/arch/models/qwen2.5-0.5b-instruct + adapter /home/arch/models/spark-adapter
Status: Online (24 layers, context 4096)
Querying Qwen LLM (spark @ local)...

[Qwen Response]
I'm Spark, an AI assistant developed by QBC.
```

A mind takes the same model with `mind bind <e> local [<name>]`:

```
$ aios --root l1 exec 'mind bind mind local' 'mind cycle mind'
in-tree model /home/arch/models/qwen2.5-0.5b-instruct + adapter /home/arch/models/spark-adapter (24 layers)
bound local:spark to 687cbd7e70b6
cycle 1 [local:spark] context c035aa8843c0 → perceived be7d34cfcc9b → no computation → result 1c84bf054d6d  771.82 ms
        result: "HELLO FROM AIOS" (15 bytes)
```

### Credentials

`mind bind <e> qwen <url> <model>` reads a bearer token from
`<root>/mind.key`, falling back to `AIOS_MIND_KEY`, and sends it as
`Authorization: Bearer …`. The `qwen` command sends the same credential
(`qwen status` says whether it has one, never what it is). Driven against a
stand-in on `127.0.0.1:13614` with `<root>/mind.key` present: it received
`auth=Bearer ***K3Y9`.

---
