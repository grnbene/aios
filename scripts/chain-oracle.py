#!/usr/bin/env python3
"""Differential oracle for the chain crypto floor.

Reads lines of "privkey msg pubkey r s recid" produced by
`aios-chain-check --emit` and recomputes every field with the independent
python `ecdsa` implementation. A verifier written from the same mental model
as the signer agrees with the signer's bugs; this one was not.
"""
import hashlib
import sys

from ecdsa import SECP256k1, SigningKey
from ecdsa.util import sigencode_string

ORDER = SECP256k1.order


def main() -> int:
    checked = 0
    bad = 0
    for line in sys.stdin:
        parts = line.split()
        if len(parts) != 6:
            continue
        priv, msg, pub, r_hex, s_hex, recid = parts
        checked += 1

        sk = SigningKey.from_string(bytes.fromhex(priv), curve=SECP256k1)
        want_pub = sk.get_verifying_key().to_string("compressed").hex()
        if want_pub != pub:
            bad += 1
            print(f"pubkey mismatch for {priv}\n  ours {pub}\n  ref  {want_pub}")
            continue

        sig = sk.sign_digest_deterministic(
            bytes.fromhex(msg), hashfunc=hashlib.sha256, sigencode=sigencode_string
        )
        r = int.from_bytes(sig[:32], "big")
        s = int.from_bytes(sig[32:], "big")
        if s > ORDER // 2:
            s = ORDER - s

        if f"{r:064x}" != r_hex or f"{s:064x}" != s_hex:
            bad += 1
            print(
                f"signature mismatch for {priv}\n"
                f"  ours r={r_hex} s={s_hex}\n"
                f"  ref  r={r:064x} s={s:064x}"
            )
            continue

        if int(recid) not in (0, 1, 2, 3):
            bad += 1
            print(f"recovery id out of range for {priv}: {recid}")

    print(f"{checked} signatures compared against python-ecdsa, {bad} mismatches")
    return 1 if bad or checked == 0 else 0


if __name__ == "__main__":
    sys.exit(main())
