# AIOS — architecture notes

## Objects

Every object has a 128-bit immutable identity. Identity is the only way to name
anything across the kernel API; pointers, file descriptors and addresses stay
inside the kernel.

    Entity        id, label, status, generation
                  states[], memory[], capabilities[], relationships[]
                  execution graph + execution position
    MemoryObject  id, size, type, location, permissions, owner, generation
    Tensor        memory id, dtype, shape, offset            (a view, not data)
    StateObject   id, owner, kind, generation, payload, memory[]
    Capability    id, holder, resource{kind,id|*}, rights, scope, expiry, parent
    Channel       id, owner, region (a shared MemoryObject), cursors[]
    ComputeGraph  id, owner, operations[]
    Operation     id, opcode, requirement{device,memory,parallelism,priority}, tensors, deps
    StoredObject  id, timestamp, provenance, derived_from, embedding, metadata, relations, payload

## Invariants

- **Authority is capability.** `Surface` is the only path from an entity into
  the kernel, and every method checks `CapabilityManager`. A spawned entity
  receives: a scoped memory-allocation budget, authority over its own state,
  channel allocation, storage read. It does not receive compute execution or
  scheduling; a parent holding `grant` over the class delegates those.
  Delegation can only attenuate; revocation cascades through delegation.
- **Nothing on the Surface mints authority.** `Surface::grant(to, res, rights)`
  is a derivation: the kernel searches the caller's own capabilities for one
  that covers `res`, carries `grant`, and holds every requested right, and
  issues the new capability as its child. Otherwise it refuses — in words that
  name what the caller holds and what it asked for. There is no path by which
  an entity ends up holding more than it was given, itself included; the only
  minting is the kernel's own root authority at first boot. `request(res,
  rights)` is the same decision asked from the entity's side ("I need X"): the
  capability that already permits X, a narrower one derived from what is held,
  or a refusal. Every refusal is recorded.
- **No entity acts as the kernel.** `act_as(e)` needs `execute` over entity
  `e`; the kernel entity is excluded outright. The console acts as other
  entities only this way.
- **Authority is a persistent object.** Residents (the kernel entity, a
  console, later a storage or human-interface service) and every capability
  they hold live in an append-only ledger under the root (`authority.log`),
  together with every mint, derivation, revocation, request, refusal and
  restore-attenuation the kernel decided. It is replayed at boot: a resident
  wakes up as the same entity holding exactly what it held.
- **Authority does not travel in images.** A restore performed by any entity
  but the kernel re-derives each capability in the image from the restorer's
  authority (the entity keeps authority over the objects the image itself
  recreates); what cannot be derived is dropped, on the record.
- **Ownership grants object capabilities.** Allocating memory or a channel
  issues the owner `read,write,grant,destroy` (or `publish,consume,grant,
  destroy`) over that specific object, delegable to others via `share`/`admit`.
- **Identity survives.** Checkpoint images carry every object id; restore
  re-creates memory objects with the same ids, reinstalls states,
  capabilities, graphs and channel records, and rebinds the channel ring to
  its restored region. A restored entity's graph resumes at its recorded
  execution position.
- **Memory location is the kernel's problem.** `MemoryFabric::migrate` moves
  bytes across providers (system, shared, persistent, later device and remote)
  and bumps `generation`; the id does not change. The scheduler may migrate
  operands toward a device's native memory before executing.
- **Communication is state.** A frame in a channel carries the publisher, the
  state id, kind, generation and payload plus references to memory objects.
  Consumers hold cursors; an overrun is reported as `lost`, never hidden.
- **The scheduler decides where, when, how.** Placement scores capability
  (must hold `execute` over the device or the class), memory locality (bytes
  already resident in the device's native memory type), free lanes and
  throughput. Ready operations run as concurrent waves ordered by priority;
  data hazards (write→read, write→write, read→write on the same memory) are
  inferred from the tensors so an entity never has to declare them.
- **The intelligence is opaque.** The kernel drives perceive → think →
  (schedule graph) → act, then writes the result into the entity's canonical
  Context state and publishes it to the bound output channel. It never looks
  inside a mind, and three different minds can be bound to one entity in turn.
- **Storage is objects.** The store is an append-only log with a rebuilt
  in-memory index; a torn tail is dropped on open, never trusted.

## Encoding

Everything that leaves a register uses `core/bytes.hpp`: little-endian fixed
widths, length-prefixed byte strings, 128-bit ids. Checkpoint images, object
records, channel frames and the external-mind frame protocol all use it. There
is no text protocol anywhere in `src/`.

## Host boundary and the zero-host contract

The kernel core's dependency on the machine is enumerated, not assumed:
`scripts/zero-host-audit.sh` prints every external symbol the compiled core
needs, and `scripts/zero-host-audit.sh` says which are allowed. Threads and locks are
`hal::Mutex`/`hal::Thread`; the core never names pthread or `std::thread`.

## Host boundary

`hal::Host` is the whole surface the kernel needs from hardware: map memory
(private, shared, persistent), unmap, sync, read/write/append/list files,
monotonic and wall clocks, entropy, topology. `src/hal/linux_host.cpp` and
`src/hal/mac_host.cpp` are the only translation units that include a hardware
host's POSIX headers on the kernel side, and a build compiles exactly one of
them; `src/hal/posix_sync.cpp` serves either.
The intelligence adapters (`dlopen`, `fork`/`pipe`) are outside the kernel and
may be host-specific.

## Coherence across memory types

Device and Remote memory objects are mirrored: the fabric's `base` is a host
mapping the kernel reads and writes like any other; the accelerator or peer
holds the authoritative copy only while it computes. A device records the
generation it last uploaded and re-sends only when the object changed; after
an operation, outputs are downloaded into the mirror and the object is
touched. Migration between types copies the mirror, so a Device or Remote
object migrated back to System is exactly the bytes the device last produced.

## The console is an entity

There is no operator hole. The command surface (`cli/`) is bound by the
substrate to a resident entity — `console` — created once per root with a
deliberately bounded, explicit profile (`cli/console.cpp`):

    kernel:*   read, execute, checkpoint, schedule, grant     — access, not ownership
    entity:*   read, allocate, execute, destroy, grant
    memory:*   read, write, allocate, destroy, grant  (4 GiB budget)
    state:*    read, write, allocate, destroy, grant
    channel:*  allocate, publish, consume, destroy, grant
    compute:*  execute, schedule, grant
    storage:*  read, write, grant
    network:*  publish, consume, grant

`kernel:write`, `kernel:destroy`, `kernel:allocate` (minting), `kernel:publish`,
`kernel:consume` stay with the kernel entity. Whatever sits at the console — a
person, a mind — issues commands that are that entity's acts: `cap grant X
kernel * all` reaches `Surface::grant`, the kernel looks at what the console
holds over `kernel:*`, and says no. `grant` is not hidden from the mind; the
kernel enforces it. `scripts/authority-gate.sh` (a test) keeps the command
surface from reaching `Kernel::grant`/`spawn`/`as`/… directly: it may read the
kernel's registries (after `kernel:read`), and everything that acts goes through
`Surface`. Operating the machine beneath the kernel (`host …`) is
`kernel:execute`; wiping the volume the kernel lives on is `kernel:destroy`,
and the substrate asks for exactly that before formatting an existing volume.

## Guests

A peer kernel is admitted as an entity ("peer:<address>") with compute and
scheduling capabilities and the default allocation quota. Its memory, graphs
and channels are owned by that entity, so terminating it releases everything
the peer left behind. Frames a peer publishes carry the original publisher's
identity from the other kernel; identities are global, so they mean the same
thing on both sides.

## Extending

- A **new device** implements `Device` (id, type, capacity, `supports`,
  `execute`, `register_kernel`) and is added to `DeviceFabric`; a memory
  provider with `MemoryType::Device` lets the scheduler place and migrate
  toward it. Nothing above changes.
- A **new transport** implements `Transport` (`region_bytes`, `format`,
  `publish`, `consume`, `stats`) and registers with `ChannelManager`.
- A **new mind** is a `.so` exporting `aios_intelligence_create` (see
  `intelligence/plugins/reflect`) or any process speaking the frame protocol
  (see `intelligence/external/peer_main.cpp`, `intelligence/assay/bridge.cjs`).
