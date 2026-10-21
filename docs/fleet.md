# Fleets: running several AIOS machines as one

This page is the fleet-level picture: how machines become members, how they
trust each other, how they find each other, how work is placed on another
machine, how a running entity moves, what the orchestrator decides, how SQL
and Things reach across machines, and how a fleet runs as services.

The building blocks (`node serve` / `connect` / `migrate`, the relay hub, the
gateway, `compute serve`) are described one by one in
[network.md](network.md). This page links to that and doesn't repeat it.

Everything marked **driven** was run on 2026-10-01 against `/repos/aios/build/aios`.
Each kernel was its own `aios shell` process with its own throwaway root under
`/tmp/aios-docs-fleet/`, on 127.0.0.1, ports 13480-13499, with `AIOS_NO_ACCEL=1`.
The shells were driven through a pty, the same way `scripts/drive-migrate.py`
does it. The output below is real, with some lines trimmed.

| You want to… | Use | Section |
|---|---|---|
| Make machines members of one fleet | `fleet hub` + `fleet join` (or `node serve` + `node connect`) | [Membership and trust](#membership-and-trust) |
| See who is in the fleet and how loaded they are | `fleet members`, `fleet status`, `fleet observe` | [Discovery](#discovery-and-the-roster) |
| Run an operation, keep memory, or open a channel on another machine | `dev=remote:…`, `device=remote:…`, `mem alloc … remote`, `transport=$transport` | [Placing work](#placing-work-on-another-machine) |
| Split a SAT problem over machines | `sat fleet FILE on=cpu,remote:…` | [sat fleet](#sat-fleet) |
| Move a running entity | `node migrate` | [Moving an entity](#moving-a-running-entity) |
| Let the fleet move work itself and recover a dead machine's jobs | `fleet plan / tick / watch / replicate` | [The orchestrator](#the-orchestrator) |
| Query another machine's SQL, or every machine's Things | `sql_on()` in grain, `… ON <machine>`, `… ON ALL`, `MIRROR`, `MACHINES` | [Fleet-wide SQL](#fleet-wide-sql-and-mirrored-things) |
| Health, attestation, the PCI bus | `watchdog`, `attest`, `bus` | [Health and attestation](#health-attestation-and-the-bus) |
| Keep a machine in the fleet across restarts | `AIOS_FLEET_HUB`, `deploy/aos-fleet/`, `packaging/asql-serve` | [Running a fleet as services](#running-a-fleet-as-services) |

---

## The two ways machines link

There are two topologies. Both carry the same node protocol (`src/node/protocol.hpp`)
over the same encrypted link (`src/node/secure.cpp`).

- **Direct links.** One kernel runs `node serve <port> <token>`, and others
  run `node connect <host> <port> <token>`. The connecting kernel needs the
  server's address. Links are one-way: the kernel that connected can use the
  server, but not the other way round unless the server also connects back.
- **The fleet relay.** One kernel runs `fleet hub <port> <cluster-key>`. Every
  member runs `fleet join <hub-host> <hub-port> <key|@FILE>`, which opens one
  outbound connection to the hub. Members find each other in the hub's roster
  and reach each other **by kernel identity**. The hub switches byte circuits
  between them and only sees ciphertext (`include/aios/fleet/relay.hpp`). A
  member needs no inbound port, and a peer reached this way shows up as
  `relay:<id>`.

Most fleet features work over either kind of link, because both end up in the
kernel's node list (`node list`). These features are **relay only**: the
roster, `fleet run`, `sql_on()` and the Things' `ON <machine>`/`ON ALL`,
mirrors, and automatic re-dial of a dropped link (`Kernel::fleet_sync`).

---

## Membership and trust

### What a key or token grants

A fleet has a **cluster key** that every member shares, and a hub can
additionally admit machines one by one: a members file gives each machine its
own attach key, and can require a TPM quote when it joins.

- `node serve` takes a **node token**. `fleet hub`/`fleet join` take a
  **cluster key**. Both must be at least 16 characters
  (`Kernel::serve`, `Kernel::relay_hub`, `Kernel::fleet_join`).
- **Attaching to the hub.** The hub sends a random 16-byte challenge. The
  member answers with a value derived from its key and that challenge, plus the
  kernel identity it claims. The hub does not prove anything back to the
  member. The identity is claimed, not proven, so the hub only lets a new attach
  take an id from a session that has been silent for 12 s (two missed
  heartbeats). While the holder is live, a second claimant is refused with
  `the hub refused: id already attached by a live session`.
- **A members file** (`fleet hub <port> <key> members=<file>`) makes the hub
  admit only listed machines. One line per member, `#` for comments:

  ```
  <member-id> <member-key> [ak=<hex>] [pcrs=<hex>]
  ```

  `<member-id>` is the 32-hex kernel id or any prefix of it (the 12 characters
  `fleet members` prints). The member joins with
  `fleet join <hub> <port> <cluster-key> member-key=<key|@FILE|env:NAME>`, and
  the hub checks the proof against that member's own key. An id that is not
  listed is refused with `not a listed member of this fleet`; a listed id with
  the wrong key, including the cluster key alone, with `bad member key`. The hub
  re-reads the file every 2 s, so deleting a line drops that machine's live
  session within seconds and it cannot attach again.
- **Attestation at join** (`attest=require` on the hub, with a members file).
  The member runs `fleet join ... attest=1`: its TPM signs a quote over PCRs
  0, 1, 2, 3 and 7 and the hub's challenge, and the quote travels with the
  attach. The hub verifies the signature and the nonce, reads the PCR digest
  from the signed bytes, and requires the signer to be the member's listed
  `ak=`. `attest quote` prints a machine's key as `ak=<hex>`. A listed `pcrs=`
  (the SHA-256 of the five PCR values concatenated) pins the machine's
  measured state. A member that sends no quote, a quote from another key, a
  quote that fails verification or a state that differs is refused. A machine
  with no TPM cannot attest (`attest=1, but this machine has no TPM to quote
  with`). Verifying needs no TPM, so the hub does not need one.
- **Reaching a peer through the hub.** Each circuit then runs the full node
  handshake end to end, keyed by the **cluster key** (`Kernel::reach`). So the
  hub can't read or forge traffic between members.
- **What a peer can do on your machine.** A peer that completes the handshake
  becomes a guest entity labelled `peer:<address>`. It is granted `execute` on
  compute devices and `schedule` on the kernel (`node_service.cpp:118-123`),
  and its allocations count against the spawn quota. Everything it asks for
  goes through that guest's capabilities. When the session ends, the guest is
  terminated and everything it held goes with it.
- These requests are **not** gated by the guest's capabilities. They are gated
  only by the token:
  - `Telemetry` (load report).
  - `Migrate`: the image is restored as shipped, including its capabilities
    (`node_service.cpp:286-293`).
  - `Replicate`: store a recovery point.
  - `Sql`, when the member has an SQL runner. See
    [Fleet-wide SQL](#fleet-wide-sql-and-mirrored-things): reads only, unless
    the member has run `fleet sql writes=on`.

  Treat everyone who holds the cluster key as fully trusted: the members file
  controls who may attach to the hub, and the cluster key is still the
  end-to-end key between members.

### Keeping the key off the command line

`fleet join`, `fleet observe`, `fleet hub` and `node serve` accept `@FILE`,
which reads the key from the first line of a file, and `env:NAME`, which reads
it from that environment variable (`fleet_key_arg`). Either keeps it out of
`ps`. `node connect` still takes the token only as an argument.

### Binding

`node serve` and `fleet hub` bind every interface by default. `bind=127.0.0.1`
binds loopback alone (for a hub or node reached only through an ssh tunnel or a
local proxy); `bind=0.0.0.0` is the explicit default. Any other address is
refused, because the substrate binds those two only:

```
node serve <port> <token|@FILE|env:NAME> [bind=127.0.0.1|0.0.0.0]
fleet hub <port> <key|@FILE|env:NAME> [bind=127.0.0.1|0.0.0.0]
```

### Driven: three processes, one hub

```
H> fleet hub 13482 docs-fleet-cluster-key-01
   fleet hub up on port 13482 — members attach here; it keeps the roster and relays circuits, seeing only ciphertext
A> fleet join 127.0.0.1 13482 @fleet.key
   joined the fleet via 127.0.0.1:13482 — roster: 0 peer(s) discovered
B> fleet join 127.0.0.1 13482 docs-fleet-cluster-key-01
   joined the fleet via 127.0.0.1:13482 — roster: 0 peer(s) discovered
A> fleet members
   Fleet roster (discovered via the hub, self excluded)
     09223501ef31
H> fleet members
     (not in a fleet — `fleet join <hub> <port> <key>` or `fleet hub <port> <key>`)
```

A hub is not a member by itself. To make it one, run `fleet join` against its
own port as well (driven: `H> fleet join 127.0.0.1 13487 …` worked, and H then
appeared in the other members' rosters).

A wrong key, a short key and a wrong token are covered in
[network.md](network.md#the-fleet-relay).

### `node serve` and `fleet join` on one machine

A kernel has one node service. `fleet join` on a kernel already serving a
`node serve` token adds the cluster key to the tokens that service accepts
(and `node serve` after `fleet join` gives the relay-only service a port and
its token), so each kind of peer presents its own secret: direct peers the node
token, peers through the hub the cluster key. Driven (2026-10-01, loopback):

```
A> node serve 13631 nodetoken-AAAAAAAAAAAAAAAA bind=127.0.0.1
A> fleet join 127.0.0.1 13630 clusterkey-fixcli-0123456789
B> fleet join 127.0.0.1 13630 clusterkey-fixcli-0123456789
B> fleet status
     * this  c30257751165  jobs 0/4 slots  pressure 0.51  free 4  (4 cpus)
     0  relay:40c48075e1d7  40c48075e1d7  jobs 0/4 slots  pressure 0.52  free 4     (A, through the hub)
C> node connect 127.0.0.1 13631 nodetoken-AAAAAAAAAAAAAAAA
   attached node 127.0.0.1:13631: 1 device(s) …                                    (the node token still works)
B> node serve 13633 nodetoken-BBBBBBBBBBBBBBBB bind=127.0.0.1                      (after fleet join)
   serving this kernel on 127.0.0.1:13633 …
```

Before, the service kept only the node token: B reached A with
`denied: node: node token rejected`, and `fleet status` / `fleet plan` left A
out with no error; `node serve` after `fleet join` failed with
`busy: already serving on port 0`. Holding either secret reaches the node
protocol on that machine.

---

## Discovery and the roster

### The hub's roster

The hub keeps the roster. Members send a heartbeat every 6 s, and the hub
evicts one it hasn't heard from in 20 s (`relay.cpp:37-38`). When a member's
process exits, its socket closes and it leaves the roster at once. Driven:
after B was killed, A's `fleet members` was empty 3 s later.

When the hub link drops, a member reconnects with backoff (0.5 s, doubling to
8 s) and re-attaches by itself (`relay.cpp:536-552`). Driven: with the hub
killed and restarted on the same port, B's roster came back about 6 s later.
While it's reconnecting, `fleet members` says so, with the reason:

```
B> fleet members                    (hub killed)
     (in a fleet, but not attached to the hub at 127.0.0.1:13630 right now — reconnecting: Connection refused)
```

`fleet join <host> <port> <key> retry` joins a hub that is down: it says the hub
is not reachable, keeps attaching in the background on the same backoff, and
`fleet members` shows the same "reconnecting" line until it attaches.

`fleet members` shows only identities. A member becomes a node-list entry
(`relay:<id>`) when something reaches it. `fleet plan`, `tick`, `watch`, `db`,
`observe` and `sat` call `Kernel::fleet_sync()`, which reaches every roster
member it doesn't hold a live link to:

```
A> fleet status
   Fleet load  (high-water 1.00, low-water 0.75)
     * this  17721a7ab2fa  jobs 0/4 slots  pressure 0.90  free 4  (4 cpus)
     (in a fleet; 1 peer(s) in the roster, reached on the next tick)
A> fleet plan
   Placement plan (nothing executed)
     · hold        — pressure 0.90 under high-water 1.00 — placed
A> fleet status
     * this  17721a7ab2fa  jobs 0/4 slots  pressure 0.90  free 4  (4 cpus)
     0  relay:09223501ef31  09223501ef31  jobs 0/4 slots  pressure 0.91  free 4
A> node list
   relay:09223501ef31 kernel 09223501ef31 calls=3 devices=1
B> node peers
   guest e56b6b055b81b75c000cd7276d12d43c from relay:1 kernel 17721a7ab2fa requests=3
```

### `fleet observe`: a status file for dashboards

```
fleet observe <hub-host> <hub-port> <key|@FILE> [status-file] [interval-ms]
```

This joins the hub and then loops forever. Each pass it calls `fleet_sync`,
polls every peer's telemetry, and rewrites the JSON file. The default file is
`/opt/aos/fleet-status.json` and the default interval 4000 ms. The observer
also joins as a member, so other members' rosters list it. Driven, every
2000 ms:

```
{"hub":"127.0.0.1:13487","updated_ns":1790894121859246875,"observer":"076195e91bb9","member_count":2,"reachable":2,
 "self":{"id":"076195e91bb9","cpus":4,"slots":4,"jobs":0,"pressure":0.790,"free":4,"memory_in_use":16524374016,"total_memory":20926529536,"hosted_databases":[]},
 "members":[{"reachable":true,"address":"relay:935fed9fd18f","telemetry":{"id":"935fed9fd18f",…,"pressure":0.783,…}},
            {"reachable":true,"address":"relay:7cfdd2935cec","telemetry":{"id":"7cfdd2935cec",…}}]}
```

`member_count` counts members reached, not the roster. A member that is in the
roster but can't be reached (as in the token case above) is missing, not
listed as unreachable.

### Telemetry: what each machine reports

A peer's `Telemetry` request answers with `NodeTelemetry`
(`include/aios/fleet/telemetry.hpp`), filled in by `Kernel::telemetry()`:

| Field | Source |
|---|---|
| `logical_cpus`, `total_memory`, `available_memory` | host topology |
| `slots` | `fleet slots N`, else `AIOS_FLEET_SLOTS`, else the CPU count |
| `memory_in_use` | host total − host available (the **whole machine's** memory, not just AIOS's) |
| `movable_entities` | alive entities that aren't the kernel, a resident, or a `peer:` guest |
| `heartbeat`, `wall_ns` | a counter that advances per report; the reporter's clock |
| `custom_kernels` | every named kernel a device here has registered (`sat.check`, `sat.solve`, `erp.*`, …), by name; one registered without a noted name shows as `kernel:<hash>`. `fleet status` prints them under each node |
| `hosted_databases` | set by `compute serve sql=1` (name, RPC port, MySQL port, table count, read-only: a cdc mirror, and fleet-read-only: peers' SQL may only read) |

`pressure()` is whichever is larger: jobs ÷ slots, or memory in use ÷ total.
Because memory is the whole host's, a busy machine with no AIOS jobs still
shows high pressure. Every kernel in these runs shared one host and read about
0.8–0.96 with 0 jobs. `free_slots()` is slots − jobs.

`fleet db` lists the `hosted_databases` of this kernel and of every reachable
peer:

```
C> fleet db
Peer 164bb975349f @ relay:164bb975349f
  aios_db  mysql=port 13603 (relayed: no dialable address; use sql_on/ON)  tables=1  [fleet sql: read-only]
```

The database list each member advertises is read when its `compute serve`
starts. A relayed peer has no address a MySQL client can dial, so `fleet db` says so
instead of printing one; a peer reached directly prints `mysql=<host>:<port>`,
and one with no MySQL wire `mysql=none`. `[ro: cdc mirror]` marks a mirror
whose local writes replication overwrites; `[fleet sql: read-only]` marks a
member whose engine refuses peers' writes (every `compute serve sql=1`
member, and any member that has not run `fleet sql writes=on`). The store's
own `tables/`, `things/`, `cells/` and `exports/` directories are not listed
as databases.

### LAN beacons and the local DHT (`p2p` / `dht` / `mesh`)

`p2p`, `dht` and `mesh` are the same command (`cmd_p2p`,
`src/channel/peer_discovery.cpp`). The node id is the SHA-derived 40-hex form
of this kernel's identity.

```
p2p status
p2p peers [wait=6000]                          # listen on udp/42423 for beacons, list who was heard
p2p mdns announce [port=5353] [every=S] [count=N] | parse <payload> | stop
p2p ping <node_id> <ip> [port]                 # a TCP connect; on success the peer is added to the routing table
p2p store key=<k> value=<v>  |  p2p find-value <key>  |  p2p find-node <target_id>
```

Discovery here is **advisory**. Nothing in `p2p` opens a node link or joins a
fleet, and the "DHT" is a map inside this one process:

- `store` writes to it.
- `find-value` reads it.
- `find-node` sorts the peers this process has heard by XOR distance.

No RPC goes to another machine, and nothing persists past the process.

Shown on loopback (`peers` and `mdns announce` listen on and broadcast to
udp/42423 on the LAN, so they reach whatever is on that network):

```
A> p2p store key=model value=v1
   [P2P STORE SUCCESS] Stored key='model' value='v1' in local DHT partition
A> p2p find-value model
   [DHT VALUE FOUND]   Key : model   Value : v1   Publisher : eb05de0acf81bf1a4e64467901a66689d6c47258
A> p2p mdns parse AIOS_P2P_MDNS_v1:0123456789abcdef0123456789abcdef01234567:127.0.0.1:13488:active
   [mDNS PEER DISCOVERED] Added peer 0123456789abcdef0123456789abcdef01234567 via subnet beacon
A> p2p ping 0123456789abcdef0123456789abcdef01234567 127.0.0.1 13488
   [P2P REACHABLE] 0123456789abcdef0123456789abcdef01234567 (127.0.0.1:13488) accepted a TCP connection; added to the routing table
A> p2p ping 0123456789abcdef0123456789abcdef01234568 127.0.0.1 13499
   error: io-error: [P2P UNREACHABLE] io-error: 127.0.0.1:13499 did not answer: io-error: Connection refused
```

`p2p ping` only proves that something accepts TCP on that port. Here it was a
`node serve` port, but the command doesn't speak the node protocol.

---

## Placing work on another machine

Once a peer is in the node list, it contributes three things to this kernel
(`attach_remote_node`, `src/node/remote.cpp:518`):

1. **Devices**: each peer device appears as `remote:<address>/<name>`, for
   example `remote:127.0.0.1:13480/cpu` or `remote:relay:09223501ef31/cpu`.
2. **A remote memory provider**, for `mem alloc <e> <size> remote`.
3. **A channel transport** `network:<address>`. After `node connect` it is in
   `$transport`.

### A graph operation on a peer (`dev=`)

`graph op … dev=<prefix>` keeps a device-name or id prefix as written and
resolves it at placement. Details are in
[compute.md](compute.md#dev-class-or-pin). Driven: a matmul pinned to the peer.

```
A> node connect 127.0.0.1 13480 docs-fleet-node-token-1
   attached node 127.0.0.1:13480: 1 device(s) [remote:127.0.0.1:13480/cpu], provides remote memory, transport network:127.0.0.1:13480
A> graph op $w $g matmul in=$X:f32:2x3 in=$Y:f32:3x2 out=$Z:f32:2x2 dev=remote:127.0.0.1:13480/cpu
A> graph run $w $g
   execution 52394db06f60: ran 1/1 ops, position 1 (complete) in 1.67 ms
       wave 0  op f0f1f7f7d0ab → remote:127.0.0.1:13480/cpu(3b3015d56556) lanes=1 526.8 µs  [locality=100% free-lanes=100% free-memory=100%, migrated 64 B]
A> tensor show $w $Z f32 2x2
   f32[2x2]@8e5641036ed4 = [58 64 139 154]
B> node peers
   guest 272523d1f221fe150005ce9860184e71 from 127.0.0.1:47140 kernel 2748c3d9f1e6 requests=8
```

Each remote op stages its operands, runs, and reads back. If the peer
advertises `ExecBatch`, writes, the op and reads go in one round trip
(`protocol.hpp`, `kFeatExecBatch`).

### Training on a peer (`lm train … device=`)

`lm train <steps> <lr> device=<prefix>` pins every operation of the training
graph to devices whose names start with that prefix (`commands.cpp:2805-2809`).
Driven over a direct loopback link:

```
A> lm train 1 0.003 device=remote:127.0.0.1:13490/cpu
     step 1/1  loss 2.87904
   loss 4.71517 -> 2.87904 over 1 step(s) at lr 0.003
   graph: 426 operation(s) in 224.3 ms on [remote:127.0.0.1:13490/cpu x426]
```

Over a real network each operation pays a round trip, so a remote step costs
far more than a local one.

### Remote memory and remote channels

These work as shown in [network.md](network.md#node-links-between-two-kernels):
`mem alloc $w 64 remote`, and `channel create $w 8 512 transport=$transport`.

Every attached peer can hold remote memory. New regions go to one chosen
peer: the first attached, until `node memory <#>` (the index in `node list`)
chooses another. A region stays with the peer that holds it: reads, writes and
its release go there whichever peer is chosen later. Driven (2026-10-01) from C
with A (#0) and B (#1) attached:

```
C> node memory
   new remote memory goes to 127.0.0.1:13631
     #0  127.0.0.1:13631  (chosen)
     #1  127.0.0.1:13633
C> mem alloc console 4096 remote          → A's fabric: 1 object, 4.0K; B: 0
C> node memory 1
   new remote memory now goes to node #1 (127.0.0.1:13633); regions already placed stay where they are
C> mem alloc console 8192 remote          → B: 1 object, 8.0K
C> mem write/read console <first region> 0 hello-from-C   → "hello-from-C"
C> mem release console <first region>     → A: 0 objects, B still 1
```

Before, only the first peer attached provided remote memory, and there was no
way to choose.

A `network:` transport is registered per peer, so you pick the peer for a
channel with `transport=network:<address>`.

### The compute service and the gateway

`compute serve` (length-framed JSON on 7311) and `gateway serve` (HTTP on
7310) are per-machine services. They are described in
[network.md](network.md#the-control-plane-gateway). The gateway's `/route`
returns a decision (admit here / route to peer / reject) and doesn't move the
job itself. Its `/fleet` endpoint reports the orchestrator's peers and
decisions, but some fields are hardcoded (see network.md's Defects).

### SHM fabric and P2P DMA: local only

- `shm-fabric` (`src/channel/shm_fabric_transport.cpp`) is a lock-free ring
  in a shared memory region. Every kernel registers it at boot
  (`kernel.cpp:77`). It is for processes on **one** machine, not between
  machines.
- `bus list` enumerates PCI functions and their memory windows. `bus peer
  <src> <dst> [bytes]` checks whether two functions could reach each other
  peer-to-peer, then refuses the transfer: *"this substrate will not let a
  program master the bus"* (`p2p_dma.cpp:95-110`). `bus list` prints
  this machine's functions (for example `0000:01:00.0 display 10de:1b06
  nvidia … bar1@0xc0000000/256.00 MiBp`). `bus peer` needs a peer on the bus.
- `ingress receive file=… | ingest payload=… | log | verify <id>`
  (`src/channel/ingress_receiver.cpp`) stages a **local** file or string and
  checksums it. Despite the `[NETWORK INGRESS …]` banner, nothing crosses
  machines. Driven: it staged into `./aios-root/ingress/` under the **current
  directory**, not under `--root` (see Defects).

---

## sat fleet

`sat fleet FILE on=SEL[@budget][!],SEL,… [budget=N] [depth=D]` splits a
formula into cubes and gives one worker to each selector. A worker's UNSAT
proof is checked on a different machine than the one that produced it. The
full description is in [compute.md](compute.md#sat-and-unsat). In a fleet,
`sat` calls `fleet_sync` and waits up to about 15 s for each named `remote…`
device to appear (`sat_cmd.cpp:432-455`).

Driven across the relay, with one local worker and one on B
(`php-6-5.cnf` is pigeonhole 6→5, 30 variables, 81 clauses, unsatisfiable):

```
A> sat devices
     cpu                                         b13a80bd3141  lanes 4  sat.solve  sat.check
     remote:relay:09223501ef31/cpu               3bbac47e1f8a  lanes 4  sat.solve  sat.check
A> sat fleet php-6-5.cnf on=cpu,remote:relay:09223501ef31/cpu budget=200
   c fleet: 2 worker(s), budget 200 conflicts a cube, first split depth 3, 0 further splits, wall 177.4 ms
   c   worker 0 cpu  on cpu  …  cubes 7 (sat 0, unsat 7, split 0)  checks 0 …
   c   worker 1 remote:relay:09223501ef31/cpu  …  cubes 1 (sat 0, unsat 1, split 0)  checks 7 …
   s UNSATISFIABLE
   c 8 leaf cube(s), every one refuted, 8 proof(s) verified by sat.check (7 checked on a different machine than solved it, 0 on the same machine by another worker, 1 on the machine that made it, never moved)
   c   proofs from this machine checked on relay:09223501ef31: 7
```

---

## Moving a running entity

`node migrate <entity> [node-index=0] [keep]` captures the entity and sends the
image to the peer at that index in `node list`. That covers states, memory and
the execution position. The peer restores it under the **same identity**.
Authority does not travel in the image: on the peer, every capability is
re-derived from that kernel's *admission* entity (`NodeService::admission`,
which holds compute execute, scheduling, memory within the spawn quota, and
state and channel allocation, all grantable) and dropped where the admission
entity holds no such delegable authority. The entity keeps authority over what
the image itself recreates (its own memory, states, channels). Each decision
is on the peer's `authority log`:

```
B> entity show a13debf44da7e73700038a7f18d14878
  label=worker status=active gen=8 states=0 memory=0 caps=3 restores=1
B> cap list a13debf44da7e73700038a7f18d14878
d3d7c93973f3  a13debf44da7  state:*    read,write,destroy,allocate  from=97a6c58c708e
ef44dcaa6d58  a13debf44da7  channel:*  allocate                     from=31ee4376f338
4d1f56e3f2ad  a13debf44da7  memory:*   allocate  scope=0 B/1.00 GiB from=f6797fc5f1d3
B> authority log
attenuated … kernel:* read      restore dropped it: the restorer holds no such delegable authority
attenuated … storage:* read,write restore dropped it: the restorer holds no such delegable authority
attenuated … memory:* allocate  restore re-derived it from the restorer's f6797fc5f1d3
```
(The entity had been granted `storage:* read,write` and `kernel:* read` on A;
neither arrives.) Without `keep`, the local copy is terminated. The basics,
including a graph that runs one op on A and finishes on B, are in
[network.md](network.md#migrating-a-running-entity). Residents and the kernel
entity refuse to move (`Kernel::migrate`).

Over the relay it's the same command. The peer's index comes from `node list`:

```
A> state create $t working "carried through the hub"
A> node list
   relay:09223501ef31 kernel 09223501ef31 calls=22 devices=1
A> node migrate $t 0
   migrated 0205a8cf5c80 to peer node #0 — it lives there as 0205a8cf5c805b81000621044a4609b4 and is gone here
B> entity show 0205a8cf5c805b81000621044a4609b4
     label=traveller status=active gen=8 states=1 memory=0 caps=5 checkpoints=0 restores=1
B> state list 0205a8cf5c805b81000621044a4609b4
   df4a832458c6  working       0205a8cf5c80  0     "carried through the hub" (23 bytes)
```

`keep` (driven) clones instead of moving:

```
A> node migrate $w 1 keep
   cloned b6f45de77f15 to peer node #1 — it lives there as b6f45de77f154b5e0009dd2ba2c162cb
C> entity show b6f45de77f154b5e0009dd2ba2c162cb
     label=w status=active gen=10 states=1 memory=1 caps=6 checkpoints=0 restores=1
```

After a `keep`, **two live entities share one identity** on two machines.
Nothing reconciles them.

---

## The orchestrator

`src/fleet/orchestrator.cpp` decides where jobs should live. Each kernel
process has one, and its decision log lasts for the life of the process
(`orch_for`). It rebalances and recovers jobs, driven below.

```
fleet status | telemetry | load       # this kernel and every node-list peer
fleet slots [n]                       # scheduling capacity (default: CPU count; also AIOS_FLEET_SLOTS)
fleet watermarks <high> <low>         # defaults 1.00 / 0.75
fleet plan                            # what a tick would decide; executes nothing
fleet tick                            # one pass, executed
fleet watch [ticks=10] [ms=500]       # repeated ticks; prints only the passes that acted
fleet replicate [idx=0]               # ship recovery points for every movable job to peer idx
fleet replicas                        # recovery points this kernel holds for others
fleet log                             # decisions executed so far
```

### What a pass decides

`Orchestrator::decide` (`orchestrator.cpp:49-97`) makes one pass:

1. **`fleet_sync()`**: reach any roster member that isn't linked yet.
2. **Recover.** For each recovery point this kernel holds whose source kernel
   once connected here and whose session has since dropped, restore the job
   from its image. Only a dropped session counts as the source being dead.
3. **Rebalance**, at most one job per pass. This happens only if **both**
   `pressure >= high` **and** `free_slots < 0` (more jobs than slots). The
   target is the reachable peer with `free_slots > 0`, pressure at or under the
   **low watermark**, the lowest pressure, and pressure below this kernel's. The job moved is the first movable entity in
   this kernel's entity list. The move goes through `Kernel::migrate`.
4. **Hold.** Otherwise it records a hold and says why.

What it doesn't do:

- It doesn't choose which job is best to move.
- It doesn't place new work. It only moves existing entities.

Memory pressure alone never triggers a move, because `free_slots < 0` is also
required.

### Driven: rebalance over a direct link

```
A> fleet slots 1
   scheduling capacity set to 1 slots
A> entity create job1
A> entity create job2
A> fleet status
     * this  2748c3d9f1e6  jobs 3/1 slots  pressure 3.00  free -2  (4 cpus)
     0  127.0.0.1:13480  139e75d48e91  jobs 0/4 slots  pressure 0.90  free 4
A> fleet plan
   Placement plan (nothing executed)
     → rebalance  job 59c89c1ce698  2748c3d9f1e6 → 139e75d48e91  — oversubscribed; would move to 127.0.0.1:13480 (4 free)
A> fleet tick
   Placement tick
     ✓ rebalance  job 59c89c1ce698  2748c3d9f1e6 → 139e75d48e91  — oversubscribed (3/1 slots); moved to peer with 4 free
A> fleet status
     * this  2748c3d9f1e6  jobs 2/1 slots  pressure 2.00  free -1  (4 cpus)
     0  127.0.0.1:13480  139e75d48e91  jobs 1/4 slots  pressure 0.90  free 3
B> entity list
   …
   59c89c1ce698  worker          active    17    1       3     9     exec=52394db06f60@1
```

The job moved was `worker`, the entity created first, which had a graph
execution in progress. The next tick would move one more.

The low watermark decides who may receive: a peer over it gets nothing, and
`fleet watermarks` refuses anything but `0 <= low < high <= 1`. Driven
(2026-10-01, direct loopback link, the peer at pressure 0.51):

```
A> fleet watermarks 0.2 0.9
   error: invalid: watermarks need 0 <= low < high <= 1 (got high 0.2, low 0.9): a job moves off at >=high and only to a peer at <=low
A> fleet slots 1 ; entity create job1 ; entity create job2
A> fleet watermarks 1.0 0.05
A> fleet tick
   · hold        — oversubscribed but no peer is at or under low-water 0.05 with a free slot — holding
A> fleet watermarks 1.0 0.95
A> fleet tick
   ✓ rebalance  job 959810454e61  89d0ce583af1 → ea0e3c185ed1  — oversubscribed (2/1 slots); moved to peer with 4 free
```

(Before this, with low at 0.05 a peer at 0.78 still received the job, and
`high 0.20, low 0.90` was accepted.)

### Driven: replicate, kill the source, recover

```
A> fleet replicate 0
   shipped 2 recovery point(s) to peer node #0 — a hard death is now recoverable off-box
B> fleet replicas
     recovery point: job b540e064d140 from kernel 2748c3d9f1e6 gen 5 (657 B)
     recovery point: job 5c7d83c455bc from kernel 2748c3d9f1e6 gen 5 (657 B)
          (process A killed)
B> node peers
   guest 272523d1f221fe150005ce9860184e71 from 127.0.0.1:47140 kernel 2748c3d9f1e6 requests=21 (gone)
B> fleet plan
     → recover    job b540e064d140  2748c3d9f1e6 → 139e75d48e91  — source 2748c3d9f1e6 dropped; would restore job here from recovery point
     → recover    job 5c7d83c455bc  2748c3d9f1e6 → 139e75d48e91  — source 2748c3d9f1e6 dropped; would restore job here from recovery point
     · hold        — pressure 0.90 under high-water 1.00 — placed
B> fleet tick
     ✓ recover    job b540e064d140  …  — source 2748c3d9f1e6 dropped; job restored here from its last recovery point
     ✓ recover    job 5c7d83c455bc  …  — source 2748c3d9f1e6 dropped; job restored here from its last recovery point
B> entity list
   …
   b540e064d140  job1            active    6     0       0     4
   5c7d83c455bc  job2            active    6     0       0     4
```

Limits of recovery, from the code:

- A recovery point is a snapshot taken when `fleet replicate` ran. Nothing
  re-ships it on a schedule, so later work is lost.
- Recovery points are held **in memory** (`Kernel::store_replica`). If the
  standby restarts, they are gone.
- Nothing runs `tick` on its own. Something has to call `fleet tick` or
  `fleet watch`, or POST the gateway's `/rebalance`.
- The source is judged dead only when its session to this kernel drops. A
  source that is alive but partitioned from the standby looks dead too. If both
  copies then run, two entities share one identity.

---

## Fleet-wide SQL and mirrored Things

`src/userland/machine_sql.cpp` gives each kernel one **machine SQL engine**,
opened on `sql_db` the first time something asks for it. A fleet member
answers peers' SQL from that engine, over the node link (opcode `Sql`), not
over the MySQL wire. A relayed peer has no address anyone can dial.

When a member answers:

- **`fleet join` installs the SQL runner** and the Things' fleet hook. A
  kernel linked only with `node serve`/`node connect` answers `this node does
  not serve SQL`. `sql_on()` only addresses fleet members by identity prefix
  (`kernel_reach`).
- **Peers may only read, unless the member opts in.** By default a peer can
  run `SELECT`, `SHOW`, `DESCRIBE`, `DESC`, `EXPLAIN`, `WITH`, Thing reports
  and controls, and `CALL SAT` (`reads_only`); anything else is refused with
  `this machine takes only reads from fleet peers (SELECT, SHOW, DESCRIBE,
  EXPLAIN); its operator can allow writes with `fleet sql writes=on``.
  `fleet sql writes=on|off` is the member's own setting, kept in
  `<root>/fleet_sql.conf` (`writes=1`); `fleet sql` shows it. `compute serve
  sql=1` registers its engine with `read_only=true`, which refuses writes
  whatever the setting (`this machine's SQL engine is a read-only mirror`).

### From grain: `sql()` and `sql_on()`

`sql(stmt)` runs on this machine's engine. `sql_on(peer-id-prefix, stmt)` runs
on a fleet member's. A result set comes back as a list of maps, one per row,
and anything else as `{affected: n}`.

Driven: B created a table, and A read it and wrote to it. C was a third member
running `compute serve sql=1`.

```
B> sql query "CREATE TABLE orders (id INT, item VARCHAR(20), qty INT)"
B> sql query "INSERT INTO orders VALUES (1, 'bolts', 40), (2, 'nuts', 15)"

$ cat q.gr
let rows = sql_on("09223501ef31", "SELECT id, item, qty FROM orders")
for r in rows { print(r["id"], r["item"], r["qty"]) }
print(sql_on("09223501ef31", "INSERT INTO orders VALUES (3, 'washers', 7)"))
print(sql_on("09223501ef31", "SELECT COUNT(*) AS n FROM orders"))

B> fleet sql writes=on          # B lets fleet peers write to it
   fleet sql: peers may read and write through this machine's SQL engine
A> grain run q.gr
   1 bolts 40
   2 nuts 15
   {affected: 1}
   [{n: 3}]

A> grain run a3.gr          # pcall around a write to B, and two statements to C (compute serve sql=1)
   {ok: true, value: {affected: 1}}
   {ok: false, error: "sql_on(79a65201bb7d): node relay:79a65201bb7d: this machine's SQL engine is a read-only mirror: SELECT, SHOW, DESCRIBE and EXPLAIN only"}
   {ok: true, value: [{Tables_in_aios_db: "tasks"}]}
```

### `… ON <machine>` and `SHOW … ON ALL`

`ThingEngine::execute` (`sql_things.cpp:2342-2410`) looks for a trailing
`ON <target>` on these verbs: `SHOW`, `CALL`, `PAUSE`, `RESUME`, `REFRESH`,
`FIRE`, `RETRY` and `SET`. `CREATE` is excluded, because a JOIN's `ON` could
end its query.

- `ON <id-prefix>` runs the statement on that member.
- `ON LOCAL`, or a prefix of this machine's own id, runs it here.
- `ON ALL` is for `SHOW` only. It runs here first, then on every member, and
  merges the columns by name. Every row gets a `machine` column. A member that
  fails gets an `unreachable` column instead of being skipped.
- Each fleet round is bounded: 8 s per member and 20 s in total (3 s and 4 s on
  the Pi). Each round starts one member further along the roster.

```
A> sql query "SHOW THINGS ON ALL"
   | thing    | things | version | … | host     | machine      |
   | item_qty | 2      | 2       | … | arch-qbc | 7d125b44f246 |
A> sql query "SHOW THINGS item_qty ON 7d125b44f246"
   | item  | qty | n | version | partitions | updated_at          | last_change | … | machine      |
   | bolts | 42  | 2 | 1       | 1          | 2026-10-01 18:33:50 | created     | … | 7d125b44f246 |
   | nuts  | 15  | 1 | 1       | 1          | 2026-10-01 18:33:50 | created     | … | 7d125b44f246 |
```

`sql query` runs on the machine engine (the one peers reach), and `fleet
join` installs the fleet hook, so these forms work straight after joining.
Driven on a freshly joined member: `sql query "SHOW THINGS ON ALL"` answered
with every member's rows, and `CREATE THING fleetm MACHINES EVERY 5 SECONDS`
followed by `SHOW THINGS fleetm` listed the machines.

### Mirrors: Things about another machine

```
CREATE THING <name> MIRROR <kind> ON <machine> [EVERY <n> SECONDS]
CREATE THING <name> MACHINES [EVERY <n> SECONDS]
```

(`ThingEngine::parse_mirror`, `sql_things.cpp:5310`.)

- A **MIRROR** pulls `SHOW THINGS <kind>` from one machine on each interval,
  keeps the rows as local Things, and drops the ones that disappear there.
  `ALL` and `LOCAL` aren't allowed as the machine.
- A **MACHINES** Thing has one row per member, this one included. The columns
  are `host`, `reachable`, `serves`, `thing_kinds` and `note`. A member that
  leaves the roster stays, marked `reachable: no`.
- Mirrors don't pull on the Pi (`kBoardNoPull`).

Create mirrors in the machine engine, for example from grain. The engine that
`sql query` builds is thrown away after the command, and it refused:
`a mirror needs the fleet, and this engine has no fleet link`.

Driven, from A:

```
$ cat a1.gr
print(sql("CREATE THING bqty MIRROR item_qty ON 7d125b44f246 EVERY 5 SECONDS"))
print(sql("CREATE THING fleetmap MACHINES EVERY 5 SECONDS"))

A> grain run a2.gr      # SHOW THINGS bqty, then SHOW THINGS fleetmap, ~7 s later
   {item: "bolts", qty: "42", n: "2", version: "1", …, last_change: "from 7d125b44f246 v1: created", machine: "bc0c0b2fac52"}
   {item: "nuts", qty: "15", n: "1", …}
   {machine: "79a65201bb7d", host: nil, reachable: "yes", serves: "things", thing_kinds: "0", …}
   {machine: "7d125b44f246", host: "arch-qbc", reachable: "yes", serves: "things", thing_kinds: "1", …}
   {machine: "bc0c0b2fac52", host: "arch-qbc", reachable: "yes", serves: "things", thing_kinds: "2", note: "this machine", …}

B> grain run b2.gr      # INSERT INTO orders VALUES (4, 'washers', 9)
A> grain run a2.gr      # next pull
   {item: "washers", qty: "9", n: "1", …, last_change: "from 7d125b44f246 v1: orders insert item washers", …}

          (process B killed, 25 s)
A> grain run a2.gr
   {machine: "7d125b44f246", host: "arch-qbc", reachable: "no", serves: "things", thing_kinds: "1", note: "not in the fleet roster", version: "2", last_change: "left the roster"}
```

The mirror kept B's last rows after B left. `host` comes from
`AIOS_MACHINE_NAME`, or else the host name.

---

## Health, attestation and the bus

### `watchdog`

`watchdog` runs `KernelWatchdog::check_health()` once and prints the result.
Nothing calls it periodically, and it reports nothing to peers. It does two
things:

- It checks the front door's pipe lock, `$AOS_RUN/exchange.lock` (the run
  directory is `AOS_RUN`, else `<root>/run`; `aos-session.sh` exports
  `AOS_RUN`, so it is the one `aos.sh` uses). It removes the lock only when the
  pid recorded in it is dead (`kill(pid, 0)` says no such process). A lock with
  a live owner, or with no owner recorded, is left alone (`Pipe Lock Status :
  held, no owner recorded (left to aos.sh)`). `aos think` records its waiter's
  pid, so its lock is not mistaken for an orphan. Hosted only.
- When the memory fabric holds more than 500 objects, it compacts memory owned
  by entities that no longer exist.

```
$ aios --root t0 exec watchdog
AIOS Self-Healing System Watchdog
  Checks Performed    : 1
  Recoveries Triggered: 0
  Heap Memory Used    : 0 B
  Pipe Lock Status    : OK
```

Fleet liveness itself comes from the hub's heartbeat and eviction (above), and
from each peer's `heartbeat` counter in telemetry.

### `attest` / `tpm`

```
attest status | attest pcrs [n...] | attest quote [n...] [nonce=<text>]
```

`src/capability/attestation.cpp` talks TPM 2.0 to `/dev/tpmrm0`, `/dev/tpm0`,
or a software TPM on `127.0.0.1:2321`:

- `pcrs` reads SHA-256 PCRs.
- `quote` asks for a signed quote over a fresh nonce. The signing key is an
  ECDSA P-256 attestation key created in the endorsement hierarchy. The command
  verifies the quote, then flips one bit and checks that the tampered copy is
  rejected.

Attestation is **not part of fleet membership**. No node or hub handshake asks
for a quote, and a peer's quote can't be requested over the link. Driven on a
machine with no TPM:

```
$ aios exec 'attest status'
  Root of trust     : no TPM on this machine (looked for /dev/tpmrm0, /dev/tpm0, and a software TPM on 127.0.0.1:2321)
```

`pcrs` and `quote` need a TPM; without one they say so.

### `bus` / `pci`

See [SHM fabric and P2P DMA](#shm-fabric-and-p2p-dma-local-only) above.

---

## Running a fleet as services

### Rejoin on every start: `AIOS_FLEET_HUB` / `AIOS_FLEET_KEY`

If both variables are set, `aios` runs `fleet join <host> <port> <key>` right
after boot, before the mode (`exec`, `shell`, …) starts (`cli/main.cpp:119-128`).
`AIOS_FLEET_HUB` is `host` or `host:port`, and the port defaults to 7420.
`AIOS_FLEET_SLOTS` sets the slot count. Driven:

```
$ AIOS_FLEET_HUB=127.0.0.1:13487 AIOS_FLEET_KEY=… aios --root s4E exec 'fleet members'
joined the fleet via 127.0.0.1:13487 — roster: 0 peer(s) discovered
Fleet roster (discovered via the hub, self excluded)
  935fed9fd18f
  e2386ef90ac8
  7cfdd2935cec
```

The boot join uses `retry`: if the hub is down at that moment the machine says
so and keeps attaching in the background (0.5 s backoff doubling to 8 s), and
the boot line is not in `history`. Driven (2026-10-01):

```
$ AIOS_FLEET_HUB=127.0.0.1:13635 AIOS_FLEET_KEY=… aios --root r-F shell      (no hub on 13635 yet)
fleet: hub 127.0.0.1:13635 unreachable (Connection refused) - retrying in the background
the hub at 127.0.0.1:13635 is not reachable (Connection refused); this machine keeps trying in the background — `fleet members` shows when it attaches
aios> fleet members
  (in a fleet, but not attached to the hub at 127.0.0.1:13635 right now — reconnecting: Connection refused)
G> fleet hub 13635 …            (the hub comes up)
aios> fleet members              (seconds later)
Fleet roster (discovered via the hub, self excluded)
M> fleet join 127.0.0.1 13635 … ; fleet members
  b6abf7c53db4                   (F, attached by its retry)
```

Before, the join failed silently and was never retried. The key is also in the
environment, so it is readable through `/proc/<pid>/environ` by the same user.

### `deploy/aos-fleet/`: a persistent member

These files were read, not run.

- `install.sh [user]`, run with sudo:
  - copies `build/aios` to `/opt/aos/bin/aios`,
  - creates `/opt/aos/fleet-root`,
  - installs `fleet-member.sh` and `aos-fleet.service` (`User=` substituted),
  - enables the unit.
- `aos-fleet.service` has `ConditionPathExists=/etc/aos/fleet.key`,
  `Restart=always` and `RestartSec=5`.
- `fleet-member.sh` runs
  `aios --root /opt/aos/fleet-root exec "fleet join ${AOS_HUB:?set AOS_HUB in /etc/aos/fleet.env} ${AOS_HUB_PORT:-7420} @/etc/aos/fleet.key" "sleep 4000000"`.
  The key is read by `@FILE`, so it never appears in `ps`. The trailing
  `sleep` keeps the member alive, and systemd restarts it when it ends. The
  default hub address is a specific production host, so set `AOS_HUB` for
  any other fleet.

The units `aos-fleet-sync.sh` restarts are in the tree too:

| Unit | Kind | Runs | Install |
|---|---|---|---|
| `aos-hub.service` | system | `/opt/aos/aios --root /opt/aos/hub-root exec "fleet hub ${AOS_HUB_PORT} @/etc/aos/fleet.key" "sleep 4000000"` | `sudo deploy/aos-fleet/install.sh <user> hub` |
| `aos-observer.service` | system | `… exec "fleet observe ${AOS_HUB} ${AOS_HUB_PORT} @/etc/aos/fleet.key /opt/aos/fleet-status.json 4000"` | `sudo deploy/aos-fleet/install.sh <user> observer` |
| `aos-member.service` | `systemd --user` | `%h/aos/aios --root %h/aos/fleet-root exec "fleet join ${AOS_HUB} ${AOS_HUB_PORT} @%h/aos/fleet.key retry" "sleep 4000000"` | copy to `~/.config/systemd/user/`, see its header |

Hub and observer run `/opt/aos/aios`, the path `aos-fleet-sync.sh` copies new
builds to. Defaults are `AOS_HUB=127.0.0.1`, `AOS_HUB_PORT=7420`; a host that
differs sets them in `/etc/aos/fleet.env` (`~/aos/fleet.env` for the user
unit). Every unit reads the key with `@FILE`, so it is never on a command line;
`install.sh` now says to copy `/etc/aos/fleet.key` from the hub host instead of
recovering it from a hub's `/proc/<pid>/cmdline`. `systemd-analyze verify`
passes on all three (with the binary paths pointed at a real build), and
`aos-member.service` was driven as a scratch user unit (`aos-fixcli-member`,
paths moved to `/tmp`, hub port 13637): `${AOS_HUB}`/`${AOS_HUB_PORT}` expand
from `fleet.env` into the quoted `exec` line, and with the hub started ten
seconds after the member, the member attached by itself (`retry`) and showed
in another member's roster. The unit `aos-member-roomy` that the script
restarts on arch itself is still only on arch.

### `deploy/aos-fleet-sync.sh`: roll a build to the fleet

This script was read, not run, as instructed. It runs from the build host, the
one with the fleet's ssh config.

| Mode | Does |
|---|---|
| `--status` | sha256 of each host's binary, and `reachable=n/m` from the hub host's `/opt/aos/fleet-status.json` (the `fleet observe` file) |
| `--dry-run` | prints the plan. It still runs `git fetch`. |
| no argument | 1. `git fetch` + `merge --ff-only origin/main` <br> 2. `cmake --build build` and `build-static` <br> 3. `sudo -n systemctl restart aos-member-roomy` <br> 4. `scp` the static binary to each roster host as `<bin>.new`, `mv` it into place, run that host's restart command <br> 5. after 6 s, read the roster count from the observer file |

The roster of hosts is hardcoded in the script (`ROSTER=(…)`).

### `deploy/aos-selfsync.sh`: a machine that syncs itself

It is for a source-build machine that the ssh-based sync can't reach. It:

1. fast-forwards `origin/main` (`AOS_SYNC_REMOTE`/`AOS_SYNC_BRANCH`) in the
   git checkout that holds the aios tree (`git rev-parse --show-toplevel`: the
   tree itself in a standalone clone, the monorepo around it otherwise),
2. runs `scripts/aos.sh build`,
3. runs `scripts/aos.sh restart`.

`--dry-run` prints those steps and runs nothing. Driven in this standalone
checkout: `pull origin/main into /repos/aios`, then the two `would:` lines
(before, `git -C /repos` failed: not a git repository). It relies on
`AIOS_FLEET_HUB` being set in the kernel's environment, so the restarted kernel
rejoins.

### `packaging/asql-serve`: an SQL node that is also a fleet member

The wrapper reads `/etc/aios/asql.conf`. If `FLEET_HOST`, `FLEET_PORT` and
`FLEET_KEY_FILE` are **all** set, it runs `fleet join … retry` before
`compute serve`. Fleet peers can then `sql_on()` this machine's tables,
read-only, over the node link, without opening the MySQL port. Only some of
the three set is reported and ignored. `--print` shows the command line
without running it:

```
$ ASQL_CONF=fl/s.conf packaging/asql-serve --print
/repos/aios/build/aios --root /tmp/aios-fix-sql/fl/r-s exec "fleet join 127.0.0.1 13601 @/tmp/aios-fix-sql/fl/key retry" "compute serve port=13602 lanes=2 sql=1 mysql_port=13603 mysql_bind=127.0.0.1"
```

The three keys are in `packaging/asql.conf.example`, empty. With `retry`, a
hub that is down does not hold SQL back. Driven with the hub stopped:

```
fleet: hub 127.0.0.1:13601 unreachable (Connection refused) - retrying in the background
the hub at 127.0.0.1:13601 is not reachable (Connection refused); this machine keeps trying in the background — `fleet members` shows when it attaches
compute sql op attached: sql_db (1 table(s), reads only)
compute mysql wire server listening on 127.0.0.1:13603 (loopback, no password)
```

The MySQL wire answered at once, and when the hub was started the member
appeared in the other members' rosters.

A driven `compute serve sql=1` member printed:

```
joined the fleet via 127.0.0.1:13483 — roster: 0 peer(s) discovered
compute sql op attached: sql_db (1 table(s), reads only)
compute mysql wire server: no mysql_user set, binding 127.0.0.1 only (set mysql_user= and mysql_pass_file= to accept remote clients)
compute service as entity 6c5ea80d14e2 over 1 cpu device(s); kernels: erp.tsp_exact, erp.route_improve, erp.pool_greedy; sql op: read-only; mysql wire: read-write
compute service listening on 127.0.0.1:13485 lanes=4 max_points=15
```

Unit files and the conf itself are covered in
[asql.md](asql.md#running-it-as-a-service).

---
