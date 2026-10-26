# Fleet identity keys — design (stages 1–2 implemented)

Status: proposal, 2026-10-26. Stage 1 (ce1813e) and stage 2 are in: `tools/fleet-identity-check.cpp`,
`tools/fleet-signed-attach-check.cpp`. Stage 2 wire format: the hub's challenge is 16 random bytes plus a flags byte
(bit 0: signatures taken, set when the hub has a members file); a member that sees it appends a 64-byte Ed25519
signature over `"aios fleet attach v1\0" || challenge || member id` to its 16-byte key proof. A members-file line with
`ed25519=` admits by that signature only. Upgrade the hub first, then a member, then add its `ed25519=` (from
`fleet identity` on that machine). Replaces the shared cluster key with one key pair per machine, in stages that keep the
fleet running throughout.

## Today

| What | How | Weakness |
|---|---|---|
| Joining the hub | the cluster key, or with a members file (`fleet hub ... members=`) a per-member key the hub also holds (`src/fleet/relay.cpp`) | the hub's members file holds every member's secret |
| Member ↔ member links | `secure_handshake_*` (`src/node/secure.cpp`): `derive_key(cluster key)`, session key = that key mixed with the two nonces (`session_key`) | one key is every machine; whoever has it can pose as any member; no forward secrecy (a recorded session opens later with the key); revoking one machine means rekeying all |
| What a peer may do | anything the node serves: commands (now refused unless `AIOS_FLEET_SHELL=1`, commit f6984cc), SQL, compute, `app ...` | no per-peer permissions |

## Goals

1. Each machine proves who it is with its own key; nothing secret is shared between machines.
2. Removing a machine is one line, on the hub, effective within seconds; nobody else rekeys.
3. A recorded session cannot be opened later, even with every machine's key (forward secrecy).
4. Each node decides which peers may ask it for what.
5. No third-party code: Ed25519, X25519, HMAC-SHA-256 and ChaCha20-Poly1305 are already in-tree
   (`include/aios/crypto/curve25519.hpp`, `hmac_sha256.hpp`, `src/node/secure.cpp`).

## Design

**Identity.** Each kernel root gets an Ed25519 key pair at first start: `<root>/node.identity` (mode 600, the service
user's). `fleet identity` prints the public key (hex). The kernel id stays as it is; the key is what proves it.

**Roster.** The hub's members file gains a public-key column and roles:

    <member-id> ed25519=<hex> roles=app-route,app-admin   # cleve-ny-1 app host

Attaching to the hub becomes a signature over the hub's challenge (no secret on the hub). The hub hands every
attached member the roster (ids, public keys, roles), signed with the hub's own Ed25519 key, which members pin at
`fleet join` (`hub-key=<hex>`). Optionally the roster is signed by an offline admin key instead, so a compromised hub
cannot add members (see Tradeoffs).

**Link handshake v2** (member ↔ member, end to end through the relay as now):

1. Each side sends a fresh X25519 public key and a nonce.
2. Each side signs the transcript (both X25519 keys, both nonces, both kernel ids, the protocol version) with its
   Ed25519 identity key and sends the signature.
3. Each side checks the peer's signature against the peer's key in the roster; an id not in the roster, or a bad
   signature, ends the link.
4. Session keys = HKDF-SHA-256(X25519 shared secret, transcript hash), one key each way. The X25519 keys are thrown
   away after the handshake: forward secrecy.

**Permissions.** A node checks the peer's roles per request: `app-route` for `app.route` operations, `app-admin` for
`app deploy/set/restart`, `sql` for SQL, `compute` for other compute work, `shell` only where `AIOS_FLEET_SHELL=1`
also allows it. Roles live in the roster, so they are changed in one place.

## Stages (each deployable on its own, the fleet working between them)

1. **Identity keys.** Every kernel creates `node.identity`; `fleet identity` prints the public key. Nothing uses it
   yet.
2. **Hub attach by signature.** The members file takes `ed25519=`; a member with a key line attaches by signature,
   one without uses its member key as today.
3. **Roster distribution + handshake v2.** Peers advertise v2 in the existing feature word (`link_feature_payload`);
   two v2 peers use the new handshake, anything else falls back to v1 (cluster key). A node can be set v2-only.
4. **Roles.** Nodes enforce roles from the roster; without roles, a member keeps today's access.
5. **Retire v1.** Once every member runs v2: the hub drops member keys and the cluster key, nodes go v2-only.

Each stage gets a check (a tools/ program, as the asql ones): attach and handshake with good keys, bad keys,
a removed member (dropped within seconds), a replayed handshake, and v1/v2 mixed.

## Tradeoffs and open questions

- **Who vouches for the roster.** Signed by the hub key: simple, but a compromised hub can add a member. Signed by an
  offline admin key (kept off every server, used when the roster changes): stronger, one more step per change.
  Proposal: hub key now, admin key as an option later.
- **Hub as single point.** Unchanged from today: the hub still sees only ciphertext between members; it gates who
  can attach.
- **Rotation.** A machine's key is rotated by adding its new public key to the roster, restarting it, then removing
  the old one.
- **The Pi firmware** (`metal/arm64`) has its own fleet client; it stays on v1 until it gets the same code.
- **Cost.** One X25519 and two Ed25519 operations per link (links are long-lived, reused per member): negligible.
