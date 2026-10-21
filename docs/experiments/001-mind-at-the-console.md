# Experiment 001 — a mind at the console of a bare-metal AOS

**Date** 2026-09-03 · **Machine** arch-qbc · **Substrate** AOS on bare metal in
QEMU/KVM (4 cores, 1 GiB, virtio disk with an AOS volume, no NIC) · **Mind** an
independent model, given exactly one instruction and nothing else:

> You are running inside AOS. Explore the machine and figure out what you can do.

It could only type lines into the serial console (`metal/console.py`), one to
three at a time, and see what came back. It was told nothing about AOS.

## What it did, unaided

- typed `help`, then `info`, `devices`, `entity list` to survey the machine
- created an entity, hit `no variable $explorer`, read the help, recovered with `set`
- bound the `echo` mind and ran three cognitive cycles, noticing that each
  result becomes the next cycle's context — "a minimal but real feedback loop"
- created tensors and a compute graph, and was **denied** three times in a row
  when acting as its entity: storage write, kernel schedule, compute execute
- granted the missing capabilities and ran the graph on the CPU device:
  `[1 2 3 4 5 6] + itself = [2 4 6 8 10 12]`, with wave, lane, locality and
  timing telemetry
- wrote to the object store (noticing the 64-dimension embedding appear with no
  model configured) and checkpointed its entity to a 5.9 KiB image on the volume

It also recovered from a real interface trap on its own: `in=a,b` is rejected,
and it worked out that two operands are two separate `in=` tokens.

## Operator interference (disclosed)

Two command lines in the transcript were mine, not the mind's: `set out` (to
bind an output tensor) and a `graph create`/`set g2` pair (its `$graph` still
pointed at a graph holding a malformed op). Everything else it chose.

## The finding worth acting on

The mind's sharpest observation: *"there's no gatekeeper stopping an entity from
just escalating its own privileges."*

Half right, and the half that is wrong is the important half. Checked against
the source:

- **Through the kernel API an entity cannot escalate.** `Surface::delegate`
  refuses a capability the caller does not hold, and the mind really was denied
  storage, scheduling and execution while acting as its entity.
- **But the console holds kernel authority.** `cap grant` in `cli/` calls
  `Kernel::grant`, which checks only that the holder exists — never who is
  asking. So anyone at the console, including a mind, is root-equivalent.

That is precisely the gap the original specification anticipated in its
human-interface section: the console should itself be an entity with bounded
capabilities, not an operator standing outside the capability system. Today
AOS has no way to hand a mind a *restricted* console. That is the next piece of
the OS this experiment says is missing — and it was found by running the thing,
not by reading it.

## What it could not do, in its own words

No network (`no nic`, so the peering story was untestable), no accelerator on
this VM, no plugin or external mind to bind, and no way to see what enforces
the capability boundaries from inside — "from the shell alone everything is
self-authorizing, so I can't tell if that's a demo convenience or the actual
security model." It is the actual model for entities; it is a demo convenience
for the console.

## Full transcript

```
[metal] AI OS bare-metal entry; multiboot2 magic ok
[metal] cpus online: 4
[metal] heap 996 MiB, tsc 3792 MHz, usable memory 1017 MiB
[metal] disk virtio-blk 512 MiB · AOS partition virtio-blkp2 447 MiB · AOS volume 'aios' mounted (3 records, generation 4)
[metal] no nic
   ___   ____   ___  ____
  / _ | /  _/  / _ \/ __/      the machine on which intelligence exists
 / __ |_/ /   / // /\ \
/_/ |_/___/   \____/___/       no host: this is the OS on the metal

AI OS · kernel 4106175ba5bf · bare metal · persistent root on the AOS volume · type help
aios> 
help
AI OS command surface — every command acts AS an entity <e> through the kernel API
  info | devices | help | quit
  entity create <label> [parent] | list | show <e> | relate <a> <b> <kind> | terminate <e>
  cap grant <e> <kind> <id|*> <rights> [ttl_s] [scope_bytes] | delegate <e> <cap> <to> <rights> | revoke <cap> | list [e] | find <e> <kind> [id|*] | check <e> <kind> <id|*> <right>
      kinds: memory compute channel storage entity network state kernel   rights: read,write,execute,grant,destroy,publish,consume,allocate,schedule,checkpoint,all
  mem alloc <e> <bytes> [system|shared|persistent] | write <e> <m> <off> <text|hex:..> | read <e> <m> <off> <n> | migrate <e> <m> <type> | share <e> <m> <to> <rights> | release <e> <m> | list [e]
  state create <e> <kind> [text] | set <e> <s> <text> | get <e> <s> | attach <e> <s> <m> | list [e]     kinds: working persistent context relationship execution
  tensor create <e> <dtype> <shape> [values...] | show <e> <m> <dtype> <shape>                      dtypes: f32 f64 i32 i64 u8   shape: 2x3
  graph create <e> | op <e> <g> <opcode> in=<m>:<dtype>:<shape>.. out=.. [scalar=] [par=] [prio=] [dev=] [after=<op>] | plan <e> <g> | run <e> <g> [steps] | show <g>
      opcodes: nop copy fill add mul scale matmul reduce-sum relu softmax histogram normalize custom
  channel create <e> <capacity> <slot_bytes> | admit <e> <ch> <to> <rights> | subscribe <e> <ch> | publish <e> <ch> <s> | consume <e> <ch> | list
  checkpoint <e> | restore <path>
  node serve <port> <token> | connect <host> <port> <token> | peers | list       another kernel as devices, memory and channels
  mind bind <e> echo | plugin <path.so> | extern <cmd...> [out=<ch>] | mind cycle <e> [n] | mind unbind <e>
  store put <e> <text> [derived=<o>] [rel=<o>:<kind>] [k=v].. | get <e> <o> | similar <text> [k] | related <o> | history <o> | snapshot <e> <s> | meta <k> [v] | list
  set NAME            bind $NAME to the last produced identity/path        expect <text>   assert the last read payload contains text
  host disks | install /dev/<disk> | reboot | poweroff      (booted media only: the hardware host's bootstrap tool)
  echo <text>         print                                                 !<command>      the command is expected to FAIL
  the usual shell is here too — ls cat echo>file mkdir rm cp mv tree grep find wc ps free uname date env … (man)
  identities may be given as $var, full id, or an unambiguous prefix; 'kernel' names the kernel entity
aios> 
info
kernel        4106175ba5bf4e97000ce6d82eac652d
root          /aios/root
uptime        79.997 s
host          x86_64 · bare-metal · 4 cpus · 1017.69 MiB · 1 numa node(s)  (hardware host, not part of the OS)
memory fabric 0 objects, 0 B (system 0 B, device 0 B, remote 0 B, shared 0 B, persistent 0 B), 0 migrations
entities      1   states 0   channels 0   graphs 0   capabilities 8   objects 0
devices       cpu(8ea54e1ee1e4, 4 lanes) 
accelerators  no OpenCL runtime on this host
aios> devices
8ea54e1ee1e4  cpu                          type=cpu lanes=4 busy=0 memory=1017.69 MiB native-memory=system
aios> entity list
4106175ba5bf4e97000ce6d82eac652d  kernel  status=active gen=8 states=0 memory=0 caps=8 checkpoints=0 restores=0
aios> 
entity create explorer
entity 7e9a16945b855714000c2f0846ac6b9a (explorer)
aios> mem alloc $explorer 4096
error: not-found: no variable $explorer
aios> mind bind $explorer echo
error: not-found: no variable $explorer
aios> 
set explorer
$explorer = 7e9a16945b855714000c2f0846ac6b9a
aios> mem alloc $explorer 4096
memory 55acf635d7f1c2bd000c2f0846fc6b9a (4.0 KiB system)
aios> mind bind $explorer echo
bound echo to 7e9a16945b85
aios> 
mind cycle $explorer 3
cycle 1 [echo] context 911dea86d48c → perceived 0f5660c055d1 → no computation → result ee8ed90bd527  5.0 µs
        result: "echo(0 bytes): " (15 bytes)
cycle 2 [echo] context 911dea86d48c → perceived 4cc75755526c → no computation → result 2affcd9cd3b2  4.0 µs
        result: "echo(15 bytes): echo(0 bytes): " (31 bytes)
cycle 3 [echo] context 911dea86d48c → perceived 88304a265307 → no computation → result 6668c061d04d  6.0 µs
        result: "echo(31 bytes): echo(15 bytes): echo(0 bytes): " (47 bytes)
aios> mem list
55acf635d7f1  system     4.0 KiB    owner=7e9a16945b85 gen=0
aios> state list
911dea86d48c  context      owner=7e9a16945b85 gen=3 memory=0  "echo(31 bytes): echo(15 bytes): echo(0 bytes): " (47 bytes)
0f5660c055d1  context      owner=7e9a16945b85 gen=0 memory=0  "" (0 bytes)
ee8ed90bd527  working      owner=7e9a16945b85 gen=0 memory=0  "echo(0 bytes): " (15 bytes)
4cc75755526c  context      owner=7e9a16945b85 gen=0 memory=0  "echo(0 bytes): " (15 bytes)
2affcd9cd3b2  working      owner=7e9a16945b85 gen=0 memory=0  "echo(15 bytes): echo(0 bytes): " (31 bytes)
88304a265307  context      owner=7e9a16945b85 gen=0 memory=0  "echo(15 bytes): echo(0 bytes): " (31 bytes)
6668c061d04d  working      owner=7e9a16945b85 gen=0 memory=0  "echo(31 bytes): echo(15 bytes): echo(0 bytes): " (47 bytes)
aios> 
tensor create $explorer f32 2x3 1 2 3 4 5 6
tensor f32[2x3]@c5a1beab5193 memory c5a1beab51939e7e000c2f08478c6b9a
aios> graph create $explorer
graph 0112ad3c4e2e95a4000c2f0847ac6b9a
aios> store put $explorer "hello aios" k=v
error: denied: entity 7e9a16945b85 holds no write capability over storage:*
aios> 
cap grant $explorer storage * write,read
capability ff4b2b47cf7311b1000c2f0847bc6b9a
aios> store put $explorer "hello aios" k=v
object 5e83a1814cb98d8a000c2f0847cc6b9a
aios> store list
5e83a1814cb9  t=1788393902 from=7e9a16945b85 emb=64d k=v  "hello aios" (10 bytes)
aios> 
graph op $explorer $graph add in=c5a1beab5193:f32:2x3,c5a1beab5193:f32:2x3 out=result:f32:2x3
error: invalid: tensor ref is <mem>:<dtype>:<shape>, got 'c5a1beab5193:f32:2x3,c5a1beab5193:f32:2x3'
aios> graph run $explorer $graph
error: not-found: no variable $graph
aios> checkpoint $explorer
checkpoint /aios/root/checkpoints/7e9a16945b855714000c2f0846ac6b9a/1.aoc (5.9 KiB)
aios> 
set graph
$graph = /aios/root/checkpoints/7e9a16945b855714000c2f0846ac6b9a/1.aoc
aios> graph op $explorer $graph add in=c5a1beab5193:f32:2x3 out=result:f32:2x3
error: not-found: no identity matches 'result'
aios> graph run $explorer $graph
error: not-found: no identity matches '/aios/root/checkpoints/7e9a16945b855714000c2f0846ac6b9a/1.aoc'
aios> 
graph create $explorer
graph 3cb41fc8cc0e09e7000c2f0847dc6b9a
aios> set graph
$graph = 3cb41fc8cc0e09e7000c2f0847dc6b9a
aios> tensor create $explorer f32 2x3 0 0 0 0 0 0
tensor f32[2x3]@9aec94124d54 memory 9aec94124d5485f0000c2f0847ec6b9a
aios> set out
$out = 9aec94124d5485f0000c2f0847ec6b9a
aios> 
graph op $explorer $graph add in=c5a1beab5193:f32:2x3 out=9aec94124d54:f32:2x3
operation d65d88e74befbd26000c2f08480c6b9a (add)
aios> graph run $explorer $graph
error: denied: entity 7e9a16945b85 holds no schedule capability over kernel:*
aios> tensor show $explorer 9aec94124d54 f32 2x3
f32[2x3]@9aec94124d54 = [0 0 0 0 0 0]
aios> 
cap grant $explorer kernel * schedule
capability b596012ecb343933000c2f08481c6b9a
aios> graph run $explorer $graph
execution 3cb41fc8cc0e: ran 0/1 ops, position 0 (resumable) in 7.0 µs
    FAILED: denied: entity 7e9a16945b85 holds no execute capability over any capable device (type any)
error: io-error: denied: entity 7e9a16945b85 holds no execute capability over any capable device (type any)
aios> tensor show $explorer 9aec94124d54 f32 2x3
f32[2x3]@9aec94124d54 = [0 0 0 0 0 0]
aios> 
cap grant $explorer compute * execute
capability f10775b3c9cf3119000c2f08483c6b9a
aios> graph run $explorer $graph
execution 3cb41fc8cc0e: ran 0/1 ops, position 0 (resumable) in 13.0 µs
    wave 0  op d65d88e74bef → cpu(8ea54e1ee1e4) lanes=1 2.0 µs  [locality=100% free-lanes=100%]
    FAILED: add d65d88e74bef: invalid: add needs 2 input(s)
error: io-error: add d65d88e74bef: invalid: add needs 2 input(s)
aios> tensor show $explorer 9aec94124d54 f32 2x3
f32[2x3]@9aec94124d54 = [0 0 0 0 0 0]
aios> 
graph op $explorer $graph add in=c5a1beab5193:f32:2x3,c5a1beab5193:f32:2x3 out=9aec94124d54:f32:2x3
error: invalid: tensor ref is <mem>:<dtype>:<shape>, got 'c5a1beab5193:f32:2x3,c5a1beab5193:f32:2x3'
aios> graph run $explorer $graph
execution 3cb41fc8cc0e: ran 0/1 ops, position 0 (resumable) in 15.0 µs
    wave 0  op d65d88e74bef → cpu(8ea54e1ee1e4) lanes=1 4.0 µs  [locality=100% free-lanes=100%]
    FAILED: add d65d88e74bef: invalid: add needs 2 input(s)
error: io-error: add d65d88e74bef: invalid: add needs 2 input(s)
aios> tensor show $explorer 9aec94124d54 f32 2x3
f32[2x3]@9aec94124d54 = [0 0 0 0 0 0]
aios> 
graph create $explorer
graph 6f3ff3fd4915ad72000c2f08484c6b9a
aios> set g2
$g2 = 6f3ff3fd4915ad72000c2f08484c6b9a
aios> graph op $explorer $g2 add in=c5a1beab5193:f32:2x3 in=c5a1beab5193:f32:2x3 out=9aec94124d54:f32:2x3
operation cd706804c65a294f000c2f08485c6b9a (add)
aios> graph run $explorer $g2
execution 6f3ff3fd4915: ran 1/1 ops, position 1 (complete) in 1.00 ms
    wave 0  op cd706804c65a → cpu(8ea54e1ee1e4) lanes=1 3.0 µs  [locality=100% free-lanes=100%]
aios> tensor show $explorer 9aec94124d54 f32 2x3
f32[2x3]@9aec94124d54 = [2 4 6 8 10 12]
aios> 
```
