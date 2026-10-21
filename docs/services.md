# Built-in services: chain clients, sensing and feeds, voice, ingress

This page covers the console command groups not described elsewhere:

- [Chain clients](#chain-clients): `chain` (alias `btc`, `bitcoin`), `wallet`, `contract` (alias `solve`)
- [Sensing and feeds](#sensing-and-feeds): `sense` (`look`), `feed` (`rss`), `horizon`
- [Voice](#voice): `voice` (`speech`, `talk`, `listen`)
- [Ingress](#ingress): `ingress` (`receive`, `upload`)
- [Task ledger](#task-ledger): `task` (`taskledger`)
- [Supervised processes](#supervised-processes-daemon): `daemon`
- [Where state lands](#where-state-lands)

How to start the console, what a root is, and how `exec` works are in
[console.md](console.md). Standing directives (`directive`, `attend`) are in
[console.md, Standing directives](console.md#standing-directives-and-attend).
The TLS client that every `https://` fetch on this page uses is in
[network.md, TLS client](network.md#client-nettlsclient).

All output below is real, from `build/aios` with `AIOS_NO_ACCEL=1` on throwaway
roots under `/tmp/aios-docs-svc/`, trimmed where marked. Nothing on this page
sent a transaction, spent funds or mined against a real pool. Anything marked


```
A=/repos/aios/build/aios
export AIOS_NO_ACCEL=1
```

---

## Chain clients

There are four separate things here. They differ a lot in how real they are:

| Command | What it actually is |
|---|---|
| `chain` | A Bitcoin P2P client written in the tree (`src/channel/btc_p2p.cpp`): version/verack handshake, `getheaders`/`headers`, `inv`/`getdata`/`tx` for broadcast. Plus a JSON-RPC client (`src/channel/chain_rpc.cpp`) for Ethereum or Bitcoin Core nodes, and offline transaction decoding. |
| `wallet` | One secp256k1 key per root, giving a P2WPKH address, a P2PKH address and an Ethereum account. Builds and signs real Bitcoin transactions (BIP143 for P2WPKH) and EIP-1559 Ethereum transactions. Does not broadcast on its own. |
| `contract` / `solve` | A hash-locked contract built and solved here: a real HTLC script, a preimage search, and the preimage checked by the script interpreter. Reads a real chain height from a peer; nothing is funded or broadcast. |

The cryptography is in the tree: `src/crypto/secp256k1_*.cpp` (field, group,
scalar, ECDSA), `keccak256.cpp`, `bech32.cpp`, `base58check.cpp`,
`ripemd160.cpp`, `sha256.cpp`. Transaction serialization, sighash, scripts and
fee planning are in `src/userland/chain/` (`btc_tx.cpp`, `btc_sign.cpp`,
`btc_script.cpp`, `eth_tx.cpp`, `rlp.cpp`).

Networks: `mainnet` (default for `chain`), `testnet`, `signet`. Pass the name as
a bare word, for example `chain peers signet`. The wallet defaults to `signet`.

### `chain peers` — handshake with public nodes

`chain peers [network] [count=N]` (default 3). It resolves the network's DNS
seeds (`dns_seeds()` in `btc_p2p.cpp`), opens TCP to each, sends `version`
(protocol 70016, user agent `/aios:0.1/`), waits for the peer's `version` and
`verack`, then sends its own `verack`. No gateway or explorer is involved.

```
$ $A --root /tmp/aios-docs-svc/chain exec 'chain peers count=2'
Bitcoin mainnet peers
  58.96.68.62:8333                  : handshake protocol 70016 /Satoshi:31.0.0/ height 969496
  126.51.104.165:8333               : handshake protocol 70016 /Satoshi:31.1.0/ height 969496
  2 peer(s) reached over the wire protocol, no gateway involved
```

The height is the peer's own `start_height` from its `version` message. It is
not verified.

### `chain headers` — header sync from genesis

`chain headers [network] [batches=N]` (default 1). Each batch is one
`getheaders` with the previous tip as the locator. The peer returns up to 2000
headers. Each header is checked two ways: its double-SHA-256 meets the target
in its own `nBits`, and its `prev_block` is the hash of the header before it,
starting from the hard-coded genesis hash. If a peer drops the connection, the
walk reconnects to the next seed and carries on from the cursor.

```
$ $A --root /tmp/aios-docs-svc/chain exec 'chain headers batches=2'
Bitcoin mainnet headers from 67.250.1.176:8333
  Peer                  : /Satoshi:29.2.0(FutureBit-Apollo-Node)/ at height 969496
  Headers received      : 4000
  Proof of work valid   : 4000/4000
  Chain links           : 4000/4000
  Tip of this walk      : 00000000922e2aa9e84a474350a3555f49f06061fd49df50a9352f156692a842
  Peers lost mid-walk   : 1
```

Limits:

- Headers are kept in memory for the length of the command. Nothing is written
  to the root. Every run starts again from genesis.
- There is no check that `nBits` follows the difficulty-retarget rules, and
  no cumulative-work comparison between chains. A peer could serve a valid but
  low-difficulty fork and it would pass.
- Blocks and transactions are never downloaded (there is no `getdata` for
  blocks), so nothing is validated beyond headers. This is not an SPV wallet:
  balances come from a node or an explorer (see below).

`chain trace [network]` handshakes and then prints each message the peer sends
in response to one `getheaders`, up to 12.

### `chain mine` — measured hashrate against a real target

`chain mine [network] [seconds=N] [threads=N] [bits=<hex>]` (alias
`hashrate`). Without `bits=` it walks **every** header from genesis to the tip
to get the current `nBits`, which means close to 500 `getheaders` round trips.
With `bits=` it stays offline. Either way it then runs SHA-256d over that
header on all threads for `seconds` (default 5) and works out how long one
block would take. It never submits anything.

```
$ $A --root R exec 'chain mine bits=17034219 seconds=2 threads=2'
What this machine can do against that target
  Source                  : bits given on the command line
  Network nBits           : 19420317 (little endian)
  Network difficulty      : 86388558925171
  Threads                 : 2
  Hashes done             : 3133440 in 2s
  Measured rate           : 1.57 MH/s
  Best hash seen          : 000000e228808ae8369a57223399c8670ddfa04c58552aec9158108523db9142
  Hashes per block        : 3.71 x 10^23 expected
  Time to one block       : 7504482275 years at this rate
  Network finds one in    : 600 seconds
  This machine's share    : 0.000000000000 %
```

(`17034219` is an example value typed on the command line. It is not the
current network target.)

### `chain decode` — parse a raw transaction offline

`chain decode <raw-hex> [network]` deserializes a transaction (legacy or
segwit) and prints the txid, wtxid, vsize, inputs and outputs. Output addresses
are rendered for the network you name. See the wallet example below for real
output.

### `chain broadcast` and `chain submit`

- `chain broadcast <raw-hex> [network]` (alias `send`) handshakes with seed
  peers, announces the txid in an `inv`, and sends the `tx` only to a peer that
  asks for it with `getdata`. It stops after 3 peers have taken it. It needs
  `operate` authority.
- `chain submit <url> <raw-hex>` POSTs the raw hex as the body to a REST
  endpoint (the Esplora `/api/tx` style) and prints the node's answer.

`chain broadcast` sends a transaction. `chain submit`
takes `http://` or `https://` (over the kernel's TLS client and trust store).
Driven only against a local stand-in on `127.0.0.1:13613` (TLS, a throwaway CA
in `<root>/ssl/ca-certificates.crt`) that refuses everything:

```
$ $A --root R exec 'chain submit https://127.0.0.1:13613/api/tx 0200'
  node answered 400: sendrawtransaction RPC error: stand-in node refuses (test)
```

### `chain rpc` — JSON-RPC to any node

`chain rpc <url> <method> [params...]`. Each param is parsed as JSON if it can
be, otherwise it is sent as a string. `https://` works and uses the kernel's
trust store. A `user:pass@` in the URL is sent as HTTP Basic auth.

```
$ $A --root R exec 'chain rpc https://eth.drpc.org eth_blockNumber'
"0x18e42f4"
$ $A --root R exec 'chain rpc https://eth.drpc.org eth_getBalance 0x0000000000000000000000000000000000000000 latest'
"0x2ff32d84418151acec7"
```

A node that answers with a JSON-RPC error has its message passed through:

```
$ $A --root R exec 'chain rpc https://cloudflare-eth.com eth_blockNumber'
FAILED: invalid: eth_blockNumber: Cannot fulfill request
```

### `chain balance` — the compiled-in payout addresses

`chain balance [eth=<rpc-url>] [btc=<explorer-base-url>]` (alias `payout`).
It shows two addresses compiled into
`include/aios/userland/chain/payout.hpp`. AIOS holds no key for either, and
the output says so. `eth=` reads the balance and nonce over JSON-RPC.
`btc=` reads `<base>/api/address/<addr>` from an Esplora-style explorer, then
handshakes with one mainnet peer to print its height next to the result.

```
$ $A --root R exec 'chain balance eth=https://eth.drpc.org'
Payout accounts  receive-only: AIOS holds no key for either
  ETH       : 0xD6c833c7C424344Daf6F49342A11c08f3CE82c96
              balance 0.00000000 ETH  (0x0 wei)
              0 transactions sent from it
  BTC       : 35grv9BWGSHkQyGCk5uKfXj2wNWLpMXbsh
```

`btc=` takes `http://` or `https://`. Against the local TLS stand-in (which
serves made-up explorer numbers):

```
$ $A --root R exec 'chain balance btc=https://127.0.0.1:13613'
  BTC       : 35grv9BWGSHkQyGCk5uKfXj2wNWLpMXbsh
              balance 0.00122456 BTC  (122456 sats)
              3 confirmed transactions, 0 in the mempool
```

### `chain eth` — build and sign an Ethereum transfer

`chain eth <rpc-url> to=<addr> [value=<wei-hex>] [passphrase=<p>] [submit]`.
It reads the chain id, nonce, priority fee, gas price and balance from the
node. Then it builds an EIP-1559 transfer with 21000 gas, signs it with the
wallet key, recovers the sender from the signature as a check, and prints the
raw transaction. Without `submit`, nothing is sent. `value` defaults to
`0x2386f26fc10000` (0.01 ETH) and `to` defaults to the zero address.

Driven against mainnet with a throwaway key and **no `submit`**:

```
$ $A --root /tmp/aios-docs-svc/wallet2 exec 'chain eth https://eth.drpc.org to=0x0000000000000000000000000000000000000001 value=0x1 passphrase=…'
Ethereum transaction on chain 1
  from              : 0x6d832f6bA4bD28C1c102bD1A270E8990e9E29199
  nonce             : 0 (from the node)
  max fee           : 0x0daa240c wei/gas
  gas at that price : 0.00000481 ETH (21000 gas)
  sending           : 0.00000000 ETH
  account needs     : 0.00000481 ETH
  account holds     : 0.00000000 ETH
  verdict           : short by 0.00000481 ETH — the node will refuse until the balance covers it
  signed sender     : 0x6d832f6bA4bD28C1c102bD1A270E8990e9E29199
  tx hash           : 0x58fee5ffb603559784a4cec981a9d2f092c6a76068e43897fcbc403128e10a2d
  raw               : 02f8680180821b58840daa240c825208940000…   (trimmed)
  not submitted; add 'submit' to hand it to the node
```

The max fee is set to the node's current `eth_gasPrice`, with no headroom.
If the base fee rises before the transaction is mined, it will sit unmined.
`submit` needs `operate` authority.

### `wallet` — one key per root

```
wallet status | new [force=1] | import <wif> [force=1] | export | node [url] | watch <addr> | sync
       | utxo <txid> <vout> <sats> <scriptpubkey-hex> | send <addr> <sats> [feerate=<sat/vB>]
       | history | release <txid>
       [passphrase=<p>] [network=mainnet|testnet|signet|regtest]
```

`status` is the default. The subcommand is the first word that is not
`key=value`, so `wallet passphrase=x` is `wallet status passphrase=x`.

**What it holds.** There is one 32-byte secp256k1 secret per root, from the
host's randomness (`generate_new_keys`) or from a WIF (`import`). From it the
wallet derives a compressed public key, a bech32 P2WPKH address, a legacy
P2PKH address and a checksummed Ethereum account.

**Where, and how it is protected.** State goes to `<root>/wallet_default.state`
as `key=value` lines, file mode **0600**. The secret is written only when a
passphrase is given. It is sealed with ChaCha20-Poly1305 under a key from
**PBKDF2-HMAC-SHA256, 210 000 rounds** (`pbkdf2_hmac_sha256`,
`src/crypto/hmac_sha256.cpp`, checked against the RFC 7914 §11 vectors), with a
fresh 16-byte salt and a fresh 12-byte nonce on every save (`version=3`,
`kdf=`, `salt=`, `nonce=`, `key=`). A `version=2` file (the old 65 x
`derive_key` stretch, nonce = salt) is still read, and is re-sealed with
PBKDF2 the first time it is opened with its passphrase.

```
$ $A --root R exec 'wallet new passphrase=hunter2 network=regtest'
key created on regtest
  Address             : bcrt1qggy3htamc8d8pt0wgdu76tupax8j7gwtrmdtus
  Ethereum account    : 0x7A29E22a99eE3Ac25261Da16386195B5251939f9
  Key at rest         : sealed (pbkdf2-sha256:210000)
$ ls -l R/wallet_default.state
-rw------- 1 arch arch 553 Oct  1 19:28 R/wallet_default.state
```

What a process without the passphrase does to the sealed key: nothing. It
writes the sealed line back as it read it.

```
$ $A --root R exec 'wallet node http://127.0.0.1:13611'      # no passphrase
node set to http://127.0.0.1:13611
$ grep ^key= R/wallet_default.state | cut -c1-30
key=201e80371b54d6a8c8d3af15aa
```

A wrong passphrase is an error, and `Key at rest` says what is actually there:

```
$ $A --root R exec 'wallet status passphrase=WRONG'
FAILED: denied: wrong passphrase: it does not open the key sealed on file
$ $A --root R exec 'wallet passphrase=hunter2'
Wallet on regtest
  Address             : bcrt1qggy3htamc8d8pt0wgdu76tupax8j7gwtrmdtus
  ...
  Key at rest         : sealed on file (ChaCha20-Poly1305, pbkdf2-sha256:210000), opened with the given passphrase
```

`new` and `import` refuse to replace a key on file unless told to:

```
$ $A --root R exec 'wallet new passphrase=hunter2'
FAILED: conflict: a key is already on file for tb1qggy3…; 'wallet new' would replace it. Back it up ('wallet export passphrase=<p>') and pass force=1 to replace it.
```

`network=` applies to an existing wallet too (and is saved): the addresses are
re-derived from the public key on file. `wallet watch <addr>` refuses anything
that is not an address on the wallet's network.

Still true: the passphrase is a command-line argument, so it ends up in shell
history and the process list; the node URL, including any `user:pass@`, is
stored and printed in plain text.

**Balances come from a node, not from the chain walk.**
`wallet node <url>` stores a Bitcoin Core JSON-RPC URL, `http://` or
`https://` (over the kernel's TLS client, trust store `<root>/ssl/ca-certificates.crt`
or the substrate's). `wallet sync` calls
`scantxoutset start ["addr(<p2wpkh>)", "addr(<p2pkh>)"]` and replaces the
UTXO list with what comes back. `wallet status` re-syncs every time a node is
set. Driven against a local TLS stand-in on `127.0.0.1:13613` that answers
`scantxoutset` with one 50 000-sat output:

```
$ $A --root R exec 'wallet node https://127.0.0.1:13613/ passphrase=…' 'wallet sync passphrase=…'
node set to https://127.0.0.1:13613/
synced from node at 127.0.0.1:13613/
  50000 sats spendable across 1 outputs
# requests the stand-in received:
POST / {"jsonrpc":"2.0","id":1,"method":"scantxoutset","params":["start",["addr(tb1qpdp45…)","addr(…)"]]}
POST / {"jsonrpc":"2.0","id":2,"method":"scantxoutset","params":["start",["addr(35grv9BWGSHkQyGCk5uKfXj2wNWLpMXbsh)"]]}
```

The second call is the watched address. Its default is the mainnet payout
address from `payout.hpp`, even on a signet wallet.

**Building and signing.** `wallet send <addr> <sats> [feerate=N]` (default 2
sat/vB, needs `operate` authority) selects outputs from the stored UTXO list,
adds change back to the wallet's own P2WPKH address if the change is above the
dust limit, signs each input (BIP143 for witness inputs, legacy sighash for
P2PKH) and prints the raw transaction. **It does not broadcast.** The
transaction is recorded in the file's history (`tx=` lines) and the outputs it
spends are **reserved** (`utxo=…:<txid>`), so a second send cannot sign the
same coins. A reservation lasts until a node stops reporting the output (it was
spent for real) or `wallet release <txid>` drops it (a transaction that was
never sent). `wallet utxo` records an output by hand.

Driven offline; the UTXO is made up (txid `11…11`) and nothing was broadcast:

```
$ $A --root R exec 'wallet utxo 1111…1111 0 100000 001423855c0e50a3360af83b5fff2df156c84f796703 passphrase=…'
$ $A --root R exec 'wallet send tb1qw508d6qejxtdg4y5r3zarvary0c5xw7kxpjzsx 5000 passphrase=…'
signed, not broadcast
  txid                : f52d3f0f15176c0a20372b1cae804ef672684b29dadc7c59273a6c6dcd85d739
  recorded            : in history; its inputs are reserved ('wallet release f52d…' if it is never sent)
$ $A --root R exec 'wallet send tb1qw508d6qejxtdg4y5r3zarvary0c5xw7kxpjzsx 5000 passphrase=…'
FAILED: exhausted: every output is reserved by a transaction signed earlier; 'wallet history' lists them, 'wallet release <txid>' frees one that was never broadcast
$ $A --root R exec 'wallet history' 'wallet status passphrase=…'
  f52d3f0f…d739  5000 sats -> tb1qw508d6qejxtdg4y5r3zarvary0c5xw7kxpjzsx  fee 282  141 vB  1 input(s) reserved
  Spendable           : 0.00000000 (0 sats, 1 outputs on record)
  Reserved            : 100000 sats held by transactions signed here ('wallet history')
  History             : 1 transactions
$ $A --root R exec 'wallet release f52d…d739'
released 1 output(s) reserved by f52d3f0f15176c0a20372b1cae804ef672684b29dadc7c59273a6c6dcd85d739
```

`chain decode <raw> signet` parses the raw transaction offline.

### `contract` (`solve`)

`contract solve [watch N] [threads=N] [space=N] [gpu] [network=<net>]` (also
`run`, `btc`, `real`) handshakes with one peer on the wallet's network (signet
by default) for its claimed height, then builds a hash-locked puzzle: a secret
nonce `n < space` (default 100 000, drawn from the host's randomness), the lock
`sha256("preimage_nonce_<n>_htlc")`, and an HTLC script locked to the wallet's
public key:
`OP_IF OP_SHA256 <h> OP_EQUALVERIFY <pk> OP_ELSE <tip+144> OP_CHECKLOCKTIMEVERIFY OP_DROP <pk> OP_ENDIF OP_CHECKSIG`.
It searches the space on the threads asked for (CPU; there is no GPU SHA-256
kernel and `gpu` says so), and checks the preimage it found by running
`OP_SHA256 <h> OP_EQUAL` in the script interpreter. Nothing funds the script
and nothing is broadcast; the output says so. `contract create hash=<64 hex>
[lock=<height>]` prints the HTLC script for a hash you give.

```
$ $A --root R exec 'contract solve threads=2'
Hash-locked contract puzzle (local — not on any chain)
  Network               : signet (the wallet's)
  Chain tip             : height 324559, as claimed by Bitcoin signet peer 195.192.213.241:38333 /Satoshi:26.0.0/
  Funded by             : nothing — no transaction pays into this script, so solving it pays nobody
  Hash lock             : sha256(preimage) = 8a1eadfc9dc3bdfe2d855dfd7822b363e2c0104d7a336588293e74172503ea49
  HTLC script           : 63a8208a1e…ea4988210326cc…705067035ff404b1752103…705068ac
  Search                : 92018 of 100000 candidates in 22.27 ms (4.133 MH/s), CPU scalar SHA-256 on 2 thread(s)
  Preimage              : preimage_nonce_91949_htlc (nonce 91949)
  Script check          : OP_SHA256 <h> OP_EQUAL is true for it (the signature branch needs a spending transaction and was not run)
```

---

## Sensing and feeds

### `sense` (`look`) — read one page

`sense <url> [keep=0] [show=<chars>] [links=<n>] [max=<chars>] [timeout=<ms>] [as=$e]`.
It fetches over the kernel's own HTTP/TLS client, follows redirects, strips the
markup to text, and lists the page's links. With `keep` on (the default), the
text is stored as an object with `url`, `title` and a SHA-256 `digest`. The
same URL with the same text is not stored twice. It needs `network:consume`
authority, plus `storage:write` to keep the page.

```
$ $A --root R exec 'sense https://example.com/ keep=0 show=300 links=3'
[SENSE] Example Domain
  from          : https://example.com/ over TLS 1.3 TLS_CHACHA20_POLY1305_SHA256 — trusted via SSL.com TLS ECC Root CA 2022
  read          : 713 B on the wire, 167 B of text, in 466.68 ms
  | This domain is for use in documentation examples without needing permission. …
  | Learn more
  links         : 1
    -> Learn more  https://iana.org/help/example-domains
```

The client offers only ChaCha20-Poly1305 with x25519 (see
[network.md](network.md#client-nettlsclient)). Servers that will not
negotiate that refuse the handshake. Both of these did:

```
$ $A --root R exec 'feed ingest https://hnrss.org/frontpage'
FAILED: denied: denied: the server refused the handshake: handshake_failure (40)
$ $A --root R exec 'feed ingest https://xkcd.com/atom.xml'
FAILED: denied: denied: the server refused the handshake: handshake_failure (40)
```

`sense frame` (also `desktop`, `screen`, `canvas`) does not fetch anything. It
describes the kernel's own devices and runs `scrot` (no shell) to
`<root>/run/desktop_now.png`, or under `$AOS_RUN` when that is set. It needs an X
display and `scrot` on the host.

### `feed` (`rss`) — RSS/Atom ingest

`feed ingest <url> [as=$e]` (alias `fetch`) fetches the document itself
(`http://`, or `https://` over the kernel's TLS client; up to 5 redirects) and
parses the markup: RSS `<item>` (title, link, description, pubDate/dc:date,
guid) or Atom `<entry>` (title, the `rel="alternate"` or rel-less
`<link href="…"/>`, summary/content, updated/published, id). CDATA is
unwrapped and XML character references decoded. Each new item is kept as an
object `kind=rss_item` with `feed`, `title`, `link`, `guid`, `date` metadata;
an item already kept for the same feed (same guid, or link) is not stored
twice. The fetch is checked against the caller's `network` authority and the
keep against its `storage` authority — the console's, or `as=$e`'s.
`feed list [url]` lists what has been kept.

Driven against a local RSS and a local Atom file served on `127.0.0.1:13612`:

```
$ $A --root R exec 'feed ingest http://127.0.0.1:13612/rss.xml'
Feed: Local RSS & Test
Parsed 2 item(s); stored 2 new object(s)
  [1] First & item
      Link: http://127.0.0.1:13612/a
      Date: Wed, 01 Oct 2026 10:00:00 GMT
  [2] Second item
      Link: http://127.0.0.1:13612/b
$ $A --root R exec 'feed ingest http://127.0.0.1:13612/atom.xml'
Feed: Local Atom
Parsed 2 item(s); stored 2 new object(s)
  [1] Atom one
      Link: http://127.0.0.1:13612/x1          # the alternate link, not the enclosure beside it
      Date: 2026-10-01T10:00:00Z
  ...
$ $A --root R exec 'feed ingest http://127.0.0.1:13612/rss.xml'
Parsed 2 item(s); stored 0 new object(s), 2 already kept
$ $A --root R exec 'feed list'
Kept feed items (4)
  First & item
      http://127.0.0.1:13612/a  Wed, 01 Oct 2026 10:00:00 GMT  [http://127.0.0.1:13612/rss.xml]
  ...
```

Public `https://` feeds still depend on the TLS client's cipher offer (see
`sense` above).

### `horizon` — watched sources scored against your own commits

```
horizon show [max=N] | scan [keep=N] | sources | watch <name> <url> | unwatch <name>
        | interests | distil [k=N] [mind=<name>] [<url>...] ["<brief text>"] | brief [N]   [as=$e]
```

Horizon keeps a list of source URLs. It works out what this machine cares
about from its own git repositories. A scan fetches each source and keeps the
links whose text matches those interests.

- **Interests** (`horizon interests`) come from the names of repositories
  registered with `git` (weight 4) and from words in the subject lines of up
  to 120 commits each in the first three repositories. A commit word counts
  only if it appears at least twice. Nobody types them. With no repository
  that has commits, `scan` refuses.
- **Scan** reads each source with `sense` and looks at its `<a>` links. A
  link is considered if its text is 18–180 characters and at least 3 words. It
  is scored against the interests, and the best `keep` (default 6) per source
  are stored as objects of `kind=signal`. A URL that is already a signal is
  not stored again.
- Because a scan reads `<a>` links, it needs an HTML page. An RSS or Atom URL
  contributes 0 links.

Driven: a repository with two commits that mention "linux rust model data
open source" and two that mention "browser", then Hacker News's front page as
the source:

```
$ $A --root /tmp/aios-docs-svc/horizon exec 'horizon interests' 'horizon watch hn https://news.ycombinator.com'
...
[HORIZON] watching hn — https://news.ycombinator.com

$ $A --root /tmp/aios-docs-svc/horizon exec 'horizon scan' 'horizon sources'
[HORIZON] read 1 source(s), 27 considered, 5 new, 0 already known, in 1124 ms
  * 4.50   Show HN: Open-source model routing for coding agents at Astra-level performance
      hn — touches model, open, source
  * 3.00   Clef: Open-source decision models, and new RL fine-tuning platform
      hn — touches open, source
  * 3.00   Show HN: Rhun, an open-source code editor written in assembly
      hn — touches open, source
  * 1.50   Bez: Generating a browser engine from specs and tests
      hn — touches browser
  * 1.50   How to speed up the Rust compiler in September 2026
      hn — touches rust
Watched (register state 7c05d770f95d, adopted from this kernel)
  NAME            READS  LAST
  hn              1      5 new of 27 considered
      https://news.ycombinator.com
```

A second scan, after adding `https://lobste.rs/rss` as another source: the
RSS source added nothing, and the HN items were already known.

```
[HORIZON] read 2 source(s), 27 considered, 0 new, 5 already known, in 1374 ms
```

`horizon show` lists the stored signals by score:

```
$ $A --root R exec 'horizon show max=2'
The horizon — 2 thing(s) the world did that touch this machine's own work
  4.50  Show HN: Open-source model routing for coding agents at Astra-level performance
      hn — model, open, source
      https://news.ycombinator.com/item?id=49911500
  3.00  Show HN: Rhun, an open-source code editor written in assembly
      hn — open, source
      https://rhun.app/
```

**Briefs.** `horizon distil` hands the reading to the resident mind (see
[console.md, The intelligence slot](console.md#the-intelligence-slot-optional)).
It reads the watched sources, or by default Hacker News, Lobsters and the CISA
advisories page. The mind picks articles from the links on those pages, the
command opens them, and the mind writes `k` items (default 3) in the form
`TITLE :: what :: why :: URL`. An item whose URL is not one of the articles
read is thrown away, and if too few survive the mind is asked once more. The
result is stored as `kind=horizon-brief`. With the default `echo` engine bound,
the mind's answer cites nothing and the command fails:

```
$ $A --root R exec 'horizon distil'
...
FAILED: corrupt: horizon distil: the mind's brief cited nothing it had read
```

If you give the brief text yourself, it is stored as-is, without a mind:

```
$ $A --root R exec 'horizon distil "Rust compiler speedups :: rustc got faster in September :: the toolchain this repo builds with :: https://news.ycombinator.com"'
Brief · 2026-10-01 · kept as this machine's horizon  (object 2f6bf5fa7b51)
Brief · 2026-10-01 · perceived sources  (object 2f6bf5fa7b51)
  - Rust compiler speedups
      rustc got faster in September
      why us: the toolchain this repo builds with
      https://news.ycombinator.com
```

`horizon brief [N]` prints the newest N briefs. `horizon distil` needs a mind
bound to the resident (see [console.md](console.md)).

**On a schedule.** A scan is an ordinary console line, so a directive can run
it (see [console.md](console.md#standing-directives-and-attend)). It then runs
under `attend` or the shell:

```
$ $A --root R exec 'directive add look 6h as=console horizon scan' 'directive run look' 'directive list'
[DIRECTIVE] look — every 6h, as 3926e6e8a4c3
  horizon scan
  it runs whenever this machine is idle; nothing has to be typed again
[HORIZON] read 1 source(s), 27 considered, 0 new, 5 already known, in 960 ms
  ...
1 directive(s) ran
Standing directives (register state 2808e29007fa, adopted from this kernel)
  NAME            EVERY   RUNS  NEXT              AS            LAST
  look            6h      1     in 5h59m (01:30)  3926e6e8a4c3  [HORIZON] read 1 source(s), 27 considered, 0 new, 5 already known, in 960 ms
      horizon scan
```

The source list is saved to `<root>/horizon/register`, and also to a kernel
state object. Signals and briefs go to the object store.

`world-model` is an alias of `jepa`. It has nothing to do with feeds and is
not covered here.

---

## Voice

`voice` (aliases `speech`, `talk`, `listen`) has no audio engine of its own. It
uses programs on the host:

| Half | Looked for, in order (`src/hal/posix_speech.cpp`, `src/hal/linux_audio.cpp`) |
|---|---|
| synthesis | `say`, then `espeak-ng`/`espeak`, then `pico2wave` |
| recognition | `whisper-cli`/`whisper-cpp` (whisper.cpp, which needs `ggml-base.en.bin` in one of four fixed paths), then the Python `whisper` |
| capture / playback | `arecord` / `aplay` |

```
voice status | voices | speak <text...> [out=<f.wav>] | hear <f.wav> | listen [dur=<sec>]
      | converse [prompt=<text>] | shm
```

`status` reports what this machine actually has:

```
$ $A --root /tmp/aios-docs-svc/voice exec 'voice status'
Voice engine state  : Idle
Sample rate         : 16000 Hz
Synthesizer         : none — this machine has no speech synthesizer (looked for say, espeak-ng, pico2wave)
Recogniser          : whisper (/home/arch/.local/bin/whisper)
Audio hardware      : in  PCH / ALC887-VD Analog  (default)
Audio hardware      : out PCH / ALC887-VD Analog  (default)
Audio hardware      : out NVidia / HDMI 0
...
Shared state region : reading (seq 0)
```

`hear` transcribes a WAV file, so recognition can be tested without a
microphone. Driven with the stock ALSA test sample:

```
$ $A --root R exec 'voice hear /usr/share/sounds/alsa/Front_Center.wav'
  Recording         : 68545 frames at 48000 Hz (1.43s)
  Heard             : front, center.
      0-1560 ms  front, center.
  Took              : 34311 ms
```

The time is the Python `whisper` process, mostly model load on CPU. A pure
440 Hz tone came back as `Heard : .`, which is whisper's own output.

With no synthesizer installed, everything that speaks fails with the reason:

```
$ $A --root R exec 'voice speak hello out=/tmp/aios-docs-svc/hello.wav'
FAILED: unsupported: this machine has no speech synthesizer (looked for say, espeak-ng, pico2wave)
```

- `speak <text> out=<f.wav>` writes the synthesized audio to a WAV. Without
  `out=` it plays through `aplay`.
- `listen [dur=<sec>]` records with `arecord` (default 3 s) and transcribes.
- `converse [prompt=<text>]` sends the text (or 3 s of recording) to the
  resident mind with `ask`, and speaks the answer.
- `shm` reads the shared state-vector region.

`speak` needs a speech synthesizer on the host, `listen` a microphone, and
`converse` both.

---

## Ingress

`ingress` (aliases `receive`, `upload`) stages a payload and records it in a
manifest. **It does not listen on the network**: a payload is a local file
read, or text passed on the command line, and the headings say which
(`[STAGED FROM LOCAL FILE]`, `[STAGED INLINE]`).

```
ingress receive file=<path> [customer=<cid>] [target=<name>] [as=$e]   (also: file, upload)
ingress ingest customer=<cid> [filename=<name>] payload="<data>"        (also: data, payload)
ingress log | list | status [customer=<cid>]
ingress verify <payload_id>
receive file=<path> …     upload <path> …                               (the alias is the subcommand)
```

What happens to a payload (`src/channel/ingress_receiver.cpp`):

1. The caller's (or `as=$e`'s) `storage` write authority is checked.
2. Its SHA-256 is computed and the wall-clock time recorded. The content type
   is guessed from the extension (`.sql`, `.json`, `.aos`) or from
   `CREATE TABLE`/`INSERT INTO` in the first 256 bytes.
3. It is written to `<root>/ingress/<payload_id>_<filename>` (a name, never a
   path: `../` cannot leave the directory) — **under `--root`** — and the
   manifest to `<root>/ingress_manifest.state`. Status is `STAGED`, and
   `VERIFIED` only once the staged file has been read back and hashes to what
   arrived.
4. An SQL payload is then run against the root's SQL database
   (`<root>/sql_db`): `INGESTED` if it accepted it, `FAILED` with its reason
   if not. Other payloads stay staged; nothing claims they were ingested.

`verify` re-reads the staged file, so it works from any later process, and
marks `CORRUPTED` (with both hashes) or `MISSING` only when that is true:

```
$ cd /tmp/aios-fix-svc/gicwd
$ $A --root /tmp/aios-fix-svc/giroot exec 'ingress receive file=/tmp/aios-fix-svc/note.txt customer=acme'
[STAGED FROM LOCAL FILE]
  Source              : file:/tmp/aios-fix-svc/note.txt
  Status              : VERIFIED
  Staged Path         : /tmp/aios-fix-svc/giroot/ingress/ing_1_note.txt
$ $A --root /tmp/aios-fix-svc/giroot exec 'ingress verify ing_1'          # a later process
[VERIFIED] the staged file for ing_1 still hashes to its recorded SHA-256
$ $A --root /tmp/aios-fix-svc/giroot exec 'receive file=/tmp/aios-fix-svc/note.txt customer=beta' 'ingress log customer=beta'
  ...
  ing_2      beta            application/octet-st 14             VERIFIED     note.txt
$ $A --root /tmp/aios-fix-svc/giroot exec 'ingress ingest customer=acme filename=t.sql payload="CREATE TABLE t (a INT); INSERT INTO t VALUES (1);"'
[STAGED INLINE; SQL LOADED]
  Status              : INGESTED
$ echo tampered >> /tmp/aios-fix-svc/giroot/ingress/ing_1_note.txt
$ $A --root /tmp/aios-fix-svc/giroot exec 'ingress verify ing_1'
FAILED: corrupt: checksum mismatch for ing_1: on disk 06dd662c…, recorded 87dc89c2…
$ find /tmp/aios-fix-svc/gicwd -type f | wc -l       # nothing in the working directory
0
```

`customer=` is recorded and filters `log`; it does not namespace the SQL.

---

## Task ledger

`task` (alias `taskledger`) keeps tasks plus an append-only log. Each log
entry carries the SHA-256 of its fields and the previous entry's hash.
`task ledger` re-checks the chain. State is `<root>/tasks/tasks.db` and
`<root>/tasks/ledger.log`.

```
task create customer=<cid> name=<name> payload=<payload> [priority=<p>]
task list [customer=<cid>] [status=<st>]
task status <task_id> <PENDING|IN_PROGRESS|COMPLETED|FAILED|CANCELLED> [details=<text>]
task ledger [customer=<cid>] [task_id=<id>]
task show <task_id>
task speech-add [text=…] | speech-update <id> [text=…] | speak <id> | speak-list
```

```
$ $A --root /tmp/aios-docs-svc/task exec 'task create customer=acme name=reindex payload=nightly priority=2'
[TASK CREATED] ID: task_0d226ad374a5 (Customer: acme, Name: 'reindex')
$ $A --root R exec 'task status task_0d226ad374a5 IN_PROGRESS details=started' 'task status task_0d226ad374a5 COMPLETED' 'task ledger'
[SUCCESS] Updated task task_0d226ad374a5 status to IN_PROGRESS
[SUCCESS] Updated task task_0d226ad374a5 status to COMPLETED
Append-Only Task Ledger Log (3 entries, Cryptographic Integrity: VERIFIED VALID)
  Idx     Task ID             Action        State Trans   SHA256 Digest (Truncated)  Details
  0       task_0d226ad374a5   CREATE        PENDING       a656a830372e0f9f...  Task created: reindex
  1       task_0d226ad374a5   UPDATE_STATUS  PENDING -> IN_PROGRESS  3d80fa7f1fd05c46...  started
  2       task_0d226ad374a5   UPDATE_STATUS  IN_PROGRESS -> COMPLETED  8ee13f1b3080840a...  Status changed to COMPLETED
```

The hash chain has no key, so it catches accidental edits but not someone who
rewrites the file and recomputes the hashes.

The speech subcommands work on the ledger's own tasks and need a synthesizer
(see [Voice](#voice)). `speak <id>` reads out that ledger task (an unknown id
is `not-found`, before any voice is started); `speak-list [customer=]
[status=]` reads out the ledger's tasks (none: it says so and speaks nothing).
`speech-add text=…` has no microphone path: it synthesizes `text`, has the
recogniser hear it back, and creates a ledger task from what was heard.
`speech-update <id> text=…` does the same and, if what was heard names a status
(complete/done/finish, start/in progress, cancel), writes that status to the
ledger; otherwise it says nothing changed. With no synthesizer installed every
one of them fails with `no speech synthesizer` once it reaches the voice.

---

## Supervised processes (`daemon`)

`daemon` runs host processes as named services:

```
daemon register <name> "<command>" [after=a,b] [restart=never|on-failure|always] [dir=<path>]
       [log=<path>] [env=K=V ...] [unset=NAME,...] [limit=N] [delay=<ns>] [atboot]
daemon list | start <name> | stop <name> | restart <name> | status <name> | supervise [seconds] | forget <name>
```

The definition is stored as an object of `kind=service`, so it outlives the
process. The running child does not: it belongs to the `aios` process that
started it and stops when that process exits. The shell starts `atboot`
services when it comes up ([console.md](console.md#interactive-shell)). For
something that must keep running, keep a long-lived `aios shell` or `attend`
up, or use a systemd unit ([network.md](network.md#running-as-a-service)).

```
$ $A --root /tmp/aios-docs-svc/daemon exec 'daemon register ticker "sh -c \"while true; do date >> /tmp/aios-docs-svc/ticker.log; sleep 1; done\"" restart=always log=/tmp/aios-docs-svc/ticker.out' 'daemon start ticker' 'daemon list'
service ticker defined (object d2a5d06836ab): sh -c "while true; do date >> /tmp/aios-docs-svc/ticker.log; sleep 1; done"
service ticker running
SERVICE         STATUS      RESTART     RESTARTS  UPTIME      LAST EXIT COMMAND
ticker          running     always      0         874.6 µs   -         sh -c "while true; …"

$ $A --root /tmp/aios-docs-svc/daemon exec 'daemon status ticker'      # a new process
  Service           : ticker
  Status            : stopped
  Restart policy    : always (limit 5 per 60.000 s)
  ...
```

With `'sleep 2.5'` added in the same `exec`, the service wrote 3 lines to
`ticker.log` and then stopped writing when `aios` exited.

---

## Where state lands

| Command | Path (under `--root` unless noted) | Format |
|---|---|---|
| `wallet`, `chain eth`, `contract` (reads it) | `wallet_default.state` (mode 0600) | `key=value` text; key sealed (PBKDF2 + ChaCha20-Poly1305) only with a passphrase; history and reservations |
| `chain peers/headers/mine/rpc/decode` | nothing | — |
| `contract` | nothing | — |
| `sense` (`keep` on) | object store, `url`/`title`/`digest` metadata | object |
| `feed ingest` | object store, `kind=rss_item` with `feed`/`guid`/`link`/`date` | object |
| `horizon` | `horizon/register` + a kernel state object; signals `kind=signal` and briefs `kind=horizon-brief` in the object store | binary register, objects |
| `directive` | kernel state register (see console.md) | — |
| `task` | `tasks/tasks.db`, `tasks/ledger.log` | binary |
| `ingress` | `ingress/<id>_<name>`, `ingress_manifest.state`; SQL payloads into `sql_db/` | files, binary manifest |
| `daemon register` | object store, `kind=service` | object |
| `voice speak out=` | the path you give | WAV |

---
