#!/usr/bin/env python3
"""Make this machine's Ethereum RPC surface speak to a client that checks it.

The Bitcoin side has done an inv/getdata/tx exchange
with a live testnet peer, and that "the Ethereum RPC surface has never spoken to
an execution client". Standing up a real client means either a package install
or an external endpoint, and neither is mine to decide — so this closes the part
that needs nothing: a JSON-RPC responder that answers the methods the client
calls and then INDEPENDENTLY verifies what it was sent.

The verification is the point. Keccak-256, RLP decoding and secp256k1 public-key
recovery are implemented here in pure Python, sharing no line of code with the
C++ under test. So when the recovered signer matches the address the wallet
says it is, that is two implementations agreeing — not one implementation
agreeing with itself.

What this does NOT do, and is not claimed: it is not an execution client. It
does not validate state, gas or nonces against a chain. It proves the codec, the
socket handling, and that the signature over the EIP-1559 payload is correct and
recoverable to our own account.
"""
import http.server, json, os, re, socket, subprocess, sys, threading

# ---------------------------------------------------------------- keccak-256
_RC = [0x0000000000000001, 0x0000000000008082, 0x800000000000808A, 0x8000000080008000,
       0x000000000000808B, 0x0000000080000001, 0x8000000080008081, 0x8000000000008009,
       0x000000000000008A, 0x0000000000000088, 0x0000000080008009, 0x000000008000000A,
       0x000000008000808B, 0x800000000000008B, 0x8000000000008089, 0x8000000000008003,
       0x8000000000008002, 0x8000000000000080, 0x000000000000800A, 0x800000008000000A,
       0x8000000080008081, 0x8000000000008080, 0x0000000080000001, 0x8000000080008008]
_ROT = [[0, 36, 3, 41, 18], [1, 44, 10, 45, 2], [62, 6, 43, 15, 61],
        [28, 55, 25, 21, 56], [27, 20, 39, 8, 14]]

def _keccak_f(a):
    for rnd in range(24):
        c = [a[x][0] ^ a[x][1] ^ a[x][2] ^ a[x][3] ^ a[x][4] for x in range(5)]
        d = [c[(x - 1) % 5] ^ (((c[(x + 1) % 5] << 1) | (c[(x + 1) % 5] >> 63)) & 0xFFFFFFFFFFFFFFFF)
             for x in range(5)]
        for x in range(5):
            for y in range(5):
                a[x][y] ^= d[x]
        b = [[0] * 5 for _ in range(5)]
        for x in range(5):
            for y in range(5):
                r = _ROT[x][y]
                v = a[x][y]
                b[y][(2 * x + 3 * y) % 5] = ((v << r) | (v >> (64 - r))) & 0xFFFFFFFFFFFFFFFF if r else v
        for x in range(5):
            for y in range(5):
                a[x][y] = b[x][y] ^ ((~b[(x + 1) % 5][y]) & b[(x + 2) % 5][y] & 0xFFFFFFFFFFFFFFFF)
        a[0][0] ^= _RC[rnd]
    return a

def keccak256(data: bytes) -> bytes:
    rate = 136
    a = [[0] * 5 for _ in range(5)]
    padded = bytearray(data)
    padded.append(0x01)
    while len(padded) % rate != 0:
        padded.append(0x00)
    padded[-1] ^= 0x80
    for off in range(0, len(padded), rate):
        blk = padded[off:off + rate]
        for i in range(rate // 8):
            lane = int.from_bytes(blk[i * 8:(i + 1) * 8], "little")
            a[i % 5][i // 5] ^= lane
        a = _keccak_f(a)
    out = bytearray()
    for i in range(4):
        out += a[i % 5][i // 5].to_bytes(8, "little")
    return bytes(out[:32])

# ---------------------------------------------------------------- secp256k1
P = 2**256 - 2**32 - 977
N = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
GX = 0x79BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798
GY = 0x483ADA7726A3C4655DA4FBFC0E1108A8FD17B448A68554199C47D08FFB10D4B8

def _inv(a, m):
    return pow(a, m - 2, m)

def _add(p, q):
    if p is None: return q
    if q is None: return p
    if p[0] == q[0] and (p[1] + q[1]) % P == 0: return None
    if p == q:
        lam = (3 * p[0] * p[0]) * _inv(2 * p[1] % P, P) % P
    else:
        lam = (q[1] - p[1]) * _inv((q[0] - p[0]) % P, P) % P
    x = (lam * lam - p[0] - q[0]) % P
    return (x, (lam * (p[0] - x) - p[1]) % P)

def _mul(k, p):
    r = None
    while k:
        if k & 1: r = _add(r, p)
        p = _add(p, p)
        k >>= 1
    return r

def recover_address(msg_hash: bytes, r: int, s: int, v: int):
    """The signer's address, recovered independently of the code under test."""
    if not (1 <= r < N and 1 <= s < N) or v not in (0, 1):
        return None
    x = r
    y_sq = (pow(x, 3, P) + 7) % P
    y = pow(y_sq, (P + 1) // 4, P)
    if pow(y, 2, P) != y_sq:
        return None
    if (y % 2) != v:
        y = P - y
    R = (x, y)
    e = int.from_bytes(msg_hash, "big")
    r_inv = _inv(r, N)
    # Q = r^-1 (sR - eG)
    Q = _add(_mul(s % N, R), _mul((N - e) % N, (GX, GY)))
    Q = _mul(r_inv, Q)
    if Q is None:
        return None
    pub = Q[0].to_bytes(32, "big") + Q[1].to_bytes(32, "big")
    return "0x" + keccak256(pub)[-20:].hex()

# ---------------------------------------------------------------- RLP
def rlp_decode(b: bytes):
    def item(i):
        p = b[i]
        if p < 0x80: return b[i:i + 1], i + 1
        if p < 0xB8:
            n = p - 0x80
            return b[i + 1:i + 1 + n], i + 1 + n
        if p < 0xC0:
            ln = p - 0xB7
            n = int.from_bytes(b[i + 1:i + 1 + ln], "big")
            return b[i + 1 + ln:i + 1 + ln + n], i + 1 + ln + n
        if p < 0xF8:
            n = p - 0xC0
            end = i + 1 + n
            out, j = [], i + 1
            while j < end:
                v, j = item(j)
                out.append(v)
            return out, end
        ln = p - 0xF7
        n = int.from_bytes(b[i + 1:i + 1 + ln], "big")
        end = i + 1 + ln + n
        out, j = [], i + 1 + ln
        while j < end:
            v, j = item(j)
            out.append(v)
        return out, end
    v, _ = item(0)
    return v

def rlp_encode(x):
    if isinstance(x, int):
        x = b"" if x == 0 else x.to_bytes((x.bit_length() + 7) // 8, "big")
    if isinstance(x, bytes):
        if len(x) == 1 and x[0] < 0x80: return x
        if len(x) < 56: return bytes([0x80 + len(x)]) + x
        ln = len(x).to_bytes((len(x).bit_length() + 7) // 8, "big")
        return bytes([0xB7 + len(ln)]) + ln + x
    body = b"".join(rlp_encode(i) for i in x)
    if len(body) < 56: return bytes([0xC0 + len(body)]) + body
    ln = len(body).to_bytes((len(body).bit_length() + 7) // 8, "big")
    return bytes([0xF7 + len(ln)]) + ln + body

# ---------------------------------------------------------------- the responder
CHAIN_ID = 31337
STATE = {"seen": [], "verified": [], "problems": []}

def verify_raw(raw_hex: str, expect_from: str):
    raw = bytes.fromhex(raw_hex[2:] if raw_hex.startswith("0x") else raw_hex)
    if not raw or raw[0] != 0x02:
        STATE["problems"].append("not an EIP-1559 typed transaction (first byte %02x)" % (raw[0] if raw else 0))
        return None
    fields = rlp_decode(raw[1:])
    if not isinstance(fields, list) or len(fields) != 12:
        STATE["problems"].append("EIP-1559 payload has %s fields, expected 12" % (len(fields) if isinstance(fields, list) else "?"))
        return None
    unsigned = fields[:9]
    v = int.from_bytes(fields[9], "big") if fields[9] else 0
    r = int.from_bytes(fields[10], "big")
    s = int.from_bytes(fields[11], "big")
    sighash = keccak256(b"\x02" + rlp_encode(unsigned))
    who = recover_address(sighash, r, s, v)
    tx_hash = "0x" + keccak256(raw).hex()
    chain_id = int.from_bytes(fields[0], "big") if fields[0] else 0
    nonce = int.from_bytes(fields[1], "big") if fields[1] else 0
    STATE["verified"].append({
        "hash": tx_hash, "recovered": who, "expected": expect_from.lower(),
        "chain_id": chain_id, "nonce": nonce, "fields": len(fields),
    })
    if who is None:
        STATE["problems"].append("the signature did not recover to any public key")
    elif who.lower() != expect_from.lower():
        STATE["problems"].append("recovered %s but the wallet says it is %s" % (who, expect_from))
    if chain_id != CHAIN_ID:
        STATE["problems"].append("signed for chain %d, this node is %d" % (chain_id, CHAIN_ID))
    return tx_hash

ACCOUNT = {"address": None}

class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args): pass
    def do_POST(self):
        n = int(self.headers.get("content-length", 0))
        req = json.loads(self.rfile.read(n) or b"{}")
        method, params = req.get("method"), req.get("params") or []
        STATE["seen"].append(method)
        if method == "eth_chainId":            result = hex(CHAIN_ID)
        elif method == "eth_getTransactionCount":
            ACCOUNT["address"] = params[0]
            result = "0x7"
        elif method == "eth_gasPrice":         result = "0x2540be400"
        elif method == "eth_maxPriorityFeePerGas": result = "0x3b9aca00"
        elif method == "eth_getBalance":       result = "0xde0b6b3a7640000"   # 1 ETH
        elif method == "eth_estimateGas":      result = "0x5208"
        elif method == "eth_sendRawTransaction":
            result = verify_raw(params[0], ACCOUNT["address"] or "0x0")
            if result is None: result = "0x" + "00" * 32
        elif method == "eth_getTransactionReceipt":
            result = {"transactionHash": params[0], "status": "0x1", "blockNumber": "0x1"}
        else:                                  result = None
        body = json.dumps({"jsonrpc": "2.0", "id": req.get("id", 1), "result": result}).encode()
        self.send_response(200)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "/tmp/eth-rpc-root"
    aios = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "aios")
    srv = http.server.HTTPServer(("127.0.0.1", 0), Handler)
    port = srv.server_address[1]
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    print("a JSON-RPC responder on 127.0.0.1:%d that decodes and verifies what it is sent\n" % port)

    subprocess.run([aios, "--root", root, "exec", "wallet new passphrase=drive-eth"],
                   capture_output=True, timeout=300)
    out = subprocess.run(
        [aios, "--root", root, "exec",
         "chain eth http://127.0.0.1:%d/ to=0x00112233445566778899aabbccddeeff00112233 value=0x2386f26fc10000 passphrase=drive-eth submit" % port],
        capture_output=True, text=True, timeout=600).stdout
    print(out)

    checks = [
        ("the client read the chain id from this node", "eth_chainId" in STATE["seen"]),
        ("it asked this node for the nonce", "eth_getTransactionCount" in STATE["seen"]),
        ("it asked this node what gas costs", "eth_gasPrice" in STATE["seen"] or "eth_maxPriorityFeePerGas" in STATE["seen"]),
        ("it asked this node for the balance", "eth_getBalance" in STATE["seen"]),
        ("it submitted a raw transaction", "eth_sendRawTransaction" in STATE["seen"]),
        ("the payload decoded as 12-field EIP-1559 RLP", any(v["fields"] == 12 for v in STATE["verified"])),
        ("it signed for the chain id this node reported", all(v["chain_id"] == CHAIN_ID for v in STATE["verified"]) and STATE["verified"] != []),
        ("it used the nonce this node gave it", any(v["nonce"] == 7 for v in STATE["verified"])),
        ("the signature recovers to the wallet's own account, verified by an independent implementation",
         any(v["recovered"] and v["recovered"].lower() == v["expected"] for v in STATE["verified"])),
        ("the client reported this node's transaction hash back",
         any(v["hash"][2:10] in out for v in STATE["verified"])),
    ]
    for v in STATE["verified"]:
        print("  transaction %s" % v["hash"])
        print("    signed for chain %d, nonce %d, %d RLP fields" % (v["chain_id"], v["nonce"], v["fields"]))
        print("    recovered signer : %s" % v["recovered"])
        print("    wallet says it is: %s" % v["expected"])
    for p in STATE["problems"]:
        print("  PROBLEM: %s" % p)
    print()
    bad = 0
    for name, ok in checks:
        print(("  PASS  " if ok else "  FAIL  ") + name)
        bad += 0 if ok else 1
    print("\n%d check(s), %d failed" % (len(checks), bad))
    print("Not claimed: this is not an execution client. It does not validate state,")
    print("gas or nonces against a chain. What it proves is the codec, the socket")
    print("handling, and a signature that an independent implementation recovers to")
    print("the account this wallet says it holds.")
    sys.exit(1 if bad else 0)

main()
