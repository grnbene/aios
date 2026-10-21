// Known-answer vectors for the chain crypto floor, plus an emitter the
// differential drive feeds to an independent implementation.
//
//   chain-check            run the vectors, exit non-zero on any mismatch
//   chain-check --emit N   print N deterministic signing cases for the oracle
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "aios/crypto/base58check.hpp"
#include "aios/crypto/bech32.hpp"
#include "aios/crypto/hmac_sha256.hpp"
#include "aios/crypto/keccak256.hpp"
#include "aios/crypto/ripemd160.hpp"
#include "aios/crypto/secp256k1.hpp"
#include "aios/crypto/sha256.hpp"
#include <unistd.h>
#include <unordered_set>
#include "aios/kernel/kernel.hpp"
#include "aios/storage/object_store.hpp"
#include "aios/userland/chain/btc_tx.hpp"
#include "aios/userland/chain/eth_tx.hpp"
#include "aios/userland/chain/rlp.hpp"

using namespace aios;
using namespace aios::crypto;
namespace chain = aios::userland::chain;
using aios::userland::chain::SIGHASH_ALL;
using aios::userland::chain::SIGHASH_NONE;

namespace {

int failures = 0;
int checks = 0;

void expect(const std::string& name, const std::string& got, const std::string& want) {
  ++checks;
  if (got == want) return;
  ++failures;
  std::printf("  MISMATCH %s\n    got  %s\n    want %s\n", name.c_str(), got.c_str(), want.c_str());
}

void expect_true(const std::string& name, bool ok) {
  ++checks;
  if (ok) return;
  ++failures;
  std::printf("  MISMATCH %s (expected true)\n", name.c_str());
}

void expect_false(const std::string& name, bool ok) {
  ++checks;
  if (!ok) return;
  ++failures;
  std::printf("  MISMATCH %s (expected false)\n", name.c_str());
}

std::string hex_of(ByteSpan b) { return bytes_to_hex(b); }

template <size_t N>
std::string hex_of(const std::array<uint8_t, N>& a) {
  return bytes_to_hex(ByteSpan(a.data(), a.size()));
}

ByteSpan text(const std::string& s) {
  return ByteSpan(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

void check_hashes() {
  std::printf("RIPEMD-160 and Keccak-256\n");
  expect("ripemd160('')", hex_of(ripemd160(text(""))), "9c1185a5c5e9fc54612808977ee8f548b2258d31");
  expect("ripemd160('abc')", hex_of(ripemd160(text("abc"))), "8eb208f7e05d987a9b044a8e98c6b087f15a0bfc");
  expect("ripemd160('message digest')", hex_of(ripemd160(text("message digest"))),
         "5d0689ef49d2fae572b881b123a85ffa21595f36");
  expect("ripemd160(a..z)", hex_of(ripemd160(text("abcdefghijklmnopqrstuvwxyz"))),
         "f71c27109c692c1b56bbdceb5b9d2865b3708dbc");
  expect("ripemd160(8x '1234567890')",
         hex_of(ripemd160(text("12345678901234567890123456789012345678901234567890123456789012345678901234567890"))),
         "9b752e45573d4b39f4dbd3323cab82bf63326bfb");

  expect("keccak256('')", hex_of(keccak256(text(""))),
         "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470");
  expect("keccak256('abc')", hex_of(keccak256(text("abc"))),
         "4e03657aea45a94fc7d47ba826c8d667c0d1e6e33a64a036ec44f58fa12d6c45");

  std::string long_input(200, 'a');
  ++checks;
  if (keccak256(text(long_input)).size() != 32) ++failures;
}

void check_hmac() {
  std::printf("HMAC-SHA256 (RFC 4231)\n");
  std::vector<uint8_t> key1(20, 0x0b);
  expect("rfc4231 case 1", hex_of(hmac_sha256(ByteSpan(key1.data(), key1.size()), text("Hi There"))),
         "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
  expect("rfc4231 case 2",
         hex_of(hmac_sha256(text("Jefe"), text("what do ya want for nothing?"))),
         "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
  std::vector<uint8_t> key3(20, 0xaa);
  std::vector<uint8_t> data3(50, 0xdd);
  expect("rfc4231 case 3",
         hex_of(hmac_sha256(ByteSpan(key3.data(), key3.size()), ByteSpan(data3.data(), data3.size()))),
         "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe");
}

void check_keys() {
  std::printf("secp256k1 key derivation and addresses\n");
  uint8_t one[32] = {0};
  one[31] = 1;

  auto compressed = ec_pubkey_create(one, true);
  expect_true("pubkey(1) derives", compressed.ok());
  if (!compressed.ok()) return;
  expect("pubkey(1) compressed", hex_of(ByteSpan(compressed->data(), compressed->size())),
         "0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798");

  auto uncompressed = ec_pubkey_create(one, false);
  expect("pubkey(1) uncompressed", hex_of(ByteSpan(uncompressed->data(), uncompressed->size())),
         "0479be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"
         "483ada7726a3c4655da4fbfc0e1108a8fd17b448a68554199c47d08ffb10d4b8");

  auto h160 = hash160(ByteSpan(compressed->data(), compressed->size()));
  expect("hash160(pubkey(1))", hex_of(h160), "751e76e8199196d454941c45d1b3a323f1433bd6");
  expect("p2pkh(1) compressed", base58check_encode(0x00, ByteSpan(h160.data(), h160.size())),
         "1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH");

  auto h160u = hash160(ByteSpan(uncompressed->data(), uncompressed->size()));
  expect("p2pkh(1) uncompressed", base58check_encode(0x00, ByteSpan(h160u.data(), h160u.size())),
         "1EHNa6Q4Jz2uvNExL497mE43ikXhwF6kZm");

  ByteBuffer wif_payload(one, one + 32);
  wif_payload.push_back(0x01);
  expect("wif(1) compressed", base58check_encode(0x80, ByteSpan(wif_payload.data(), wif_payload.size())),
         "KwDiBf89QgGbjEhKnhXJuH7LrciVrZi3qYjgd9M7rFU73sVHnoWn");
  expect("wif(1) uncompressed", base58check_encode(0x80, ByteSpan(one, 32)),
         "5HpHagT65TZzG1PH3CSu63k8DbpvD8s5ip4nEB3kEsreAnchuDf");

  uint8_t version = 0xff;
  auto decoded = base58check_decode("KwDiBf89QgGbjEhKnhXJuH7LrciVrZi3qYjgd9M7rFU73sVHnoWn", version);
  expect_true("wif decodes", decoded.ok());
  if (decoded.ok()) {
    expect_true("wif version is 0x80", version == 0x80);
    expect("wif round trip", hex_of(ByteSpan(decoded->data(), 32)), hex_of(ByteSpan(one, 32)));
  }
  auto corrupt = base58check_decode("KwDiBf89QgGbjEhKnhXJuH7LrciVrZi3qYjgd9M7rFU73sVHnoWm", version);
  expect_false("corrupted wif is rejected", corrupt.ok());

  expect_false("private key 0 rejected", ec_seckey_valid([] {
                 static uint8_t z[32] = {0};
                 return z;
               }()));
  uint8_t over_n[32];
  std::memset(over_n, 0xff, 32);
  expect_false("private key >= n rejected", ec_seckey_valid(over_n));
}

void check_bech32() {
  std::printf("bech32 and bech32m\n");
  auto program = hex_to_bytes("751e76e8199196d454941c45d1b3a323f1433bd6");
  expect("p2wpkh(1)", segwit_encode("bc", 0, ByteSpan(program.data(), program.size())),
         "bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4");

  uint8_t version = 0xff;
  auto back = segwit_decode("bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4", "bc", version);
  expect_true("p2wpkh decodes", back.ok());
  expect_true("p2wpkh version 0", version == 0);
  if (back.ok()) expect("p2wpkh program", hex_of(ByteSpan(back->data(), back->size())),
                        "751e76e8199196d454941c45d1b3a323f1433bd6");

  auto upper = segwit_decode("BC1QW508D6QEJXTDG4Y5R3ZARVARY0C5XW7KV8F3T4", "bc", version);
  expect_true("uppercase form decodes", upper.ok());

  auto bad = segwit_decode("bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t5", "bc", version);
  expect_false("one flipped character fails the checksum", bad.ok());

  auto taproot = hex_to_bytes("79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798");
  std::string tr = segwit_encode("bc", 1, ByteSpan(taproot.data(), taproot.size()));
  auto tr_back = segwit_decode(tr, "bc", version);
  expect_true("v1 round trip", tr_back.ok());
  expect_true("v1 version is 1", version == 1);
  if (tr_back.ok()) expect("v1 program", hex_of(ByteSpan(tr_back->data(), tr_back->size())),
                           "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798");

  // A v1 payload carrying a bech32 checksum instead of bech32m must not decode:
  // that separation is the whole point of BIP-350.
  ByteBuffer data5;
  data5.push_back(1);
  std::string wrong_variant;
  {
    ByteBuffer conv;
    uint32_t acc = 0;
    int bits = 0;
    for (uint8_t b : taproot) {
      acc = (acc << 8) | b;
      bits += 8;
      while (bits >= 5) {
        bits -= 5;
        conv.push_back((uint8_t)((acc >> bits) & 31));
      }
    }
    if (bits) conv.push_back((uint8_t)((acc << (5 - bits)) & 31));
    data5.insert(data5.end(), conv.begin(), conv.end());
    wrong_variant = bech32_encode("bc", ByteSpan(data5.data(), data5.size()), Bech32Variant::Bech32);
  }
  auto wrong = segwit_decode(wrong_variant, "bc", version);
  expect_false("v1 with bech32 checksum rejected", wrong.ok());
}

void check_signing() {
  std::printf("ECDSA signing, verification and recovery\n");
  uint8_t sec[32] = {0};
  sec[31] = 0x2a;
  auto pub = ec_pubkey_create(sec, true);
  expect_true("test key derives", pub.ok());
  if (!pub.ok()) return;

  auto digest = sha256(text("drive the actual thing"));
  auto sig = ecdsa_sign(sec, digest.data());
  expect_true("signs", sig.ok());
  if (!sig.ok()) return;

  auto sig2 = ecdsa_sign(sec, digest.data());
  expect("deterministic nonce", hex_of(sig->r) + hex_of(sig->s), hex_of(sig2->r) + hex_of(sig2->s));

  expect_true("verifies", ecdsa_verify(ByteSpan(pub->data(), pub->size()), digest.data(), *sig));

  auto other = sha256(text("drive the actual thinh"));
  expect_false("wrong message fails", ecdsa_verify(ByteSpan(pub->data(), pub->size()), other.data(), *sig));

  EcSignature tampered = *sig;
  tampered.s[31] ^= 0x01;
  expect_false("tampered s fails", ecdsa_verify(ByteSpan(pub->data(), pub->size()), digest.data(), tampered));

  auto recovered = ecdsa_recover(*sig, digest.data(), true);
  expect_true("recovers", recovered.ok());
  if (recovered.ok()) {
    expect("recovered key matches", hex_of(ByteSpan(recovered->data(), recovered->size())),
           hex_of(ByteSpan(pub->data(), pub->size())));
  }

  auto der = ecdsa_der_encode(*sig);
  auto der_back = ecdsa_der_decode(ByteSpan(der.data(), der.size()));
  expect_true("der round trip", der_back.ok());
  if (der_back.ok()) {
    expect("der r", hex_of(der_back->r), hex_of(sig->r));
    expect("der s", hex_of(der_back->s), hex_of(sig->s));
  }

  auto uncompressed = ec_pubkey_create(sec, false);
  auto eth = eth_address_from_pubkey(ByteSpan(uncompressed->data(), uncompressed->size()));
  expect_true("eth address derives", eth.ok());
  ++checks;
  if (eth.ok() && eth->size() != 20) ++failures;

  // Every signature this build produces must be low-S; a high-S signature is
  // non-standard and relayed by nobody.
  for (int i = 1; i <= 32; ++i) {
    uint8_t k[32] = {0};
    k[31] = (uint8_t)i;
    auto d = sha256(ByteSpan(k, 32));
    auto s = ecdsa_sign(k, d.data());
    ++checks;
    if (!s.ok()) { ++failures; continue; }
    bool high = false;
    static const uint8_t HALF_N[32] = {0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                       0xff, 0xff, 0xff, 0xff, 0xff, 0x5d, 0x57, 0x6e, 0x73, 0x57, 0xa4,
                                       0x50, 0x1d, 0xdf, 0xe9, 0x2f, 0x46, 0x68, 0x1b, 0x20, 0xa0};
    for (int j = 0; j < 32; ++j) {
      if (s->s[(size_t)j] != HALF_N[j]) { high = s->s[(size_t)j] > HALF_N[j]; break; }
    }
    if (high) { ++failures; std::printf("  MISMATCH high-S signature for key %d\n", i); }

    auto p = ec_pubkey_create(k, true);
    ++checks;
    if (!ecdsa_verify(ByteSpan(p->data(), p->size()), d.data(), *s)) {
      ++failures;
      std::printf("  MISMATCH self-verify failed for key %d\n", i);
    }
    auto rec = ecdsa_recover(*s, d.data(), true);
    ++checks;
    if (!rec.ok() || hex_of(ByteSpan(rec->data(), rec->size())) != hex_of(ByteSpan(p->data(), p->size()))) {
      ++failures;
      std::printf("  MISMATCH recovery failed for key %d\n", i);
    }
  }
}

// Three real mainnet transactions, captured with their prevout details. They
// are the strongest oracle available without a node: the signatures inside them
// were accepted by the network, so a sighash implementation that disagrees with
// consensus fails to verify them.
const char* LEGACY_TX =
    "0100000001c997a5e56e104102fa209c6a852dd90660a20b2d9c352423edce25857fcd370400000000484730440220"
    "4e45e16932b8af514961a1d3a1a25fdf3f4f7732e9d624c6c61548ab5fb8cd410220181522ec8eca07de4860a4acdd"
    "12909d831cc56cbbac4622082221a8768d1d0901ffffffff0200ca9a3b00000000434104ae1a62fe09c5f51b13905f"
    "07f06b99a2f7159b2225f374cd378d71302fa28414e7aab37397f554a7df5f142c21c1b7303b8a0626f1baded5c72a"
    "704f7e6cd84cac00286bee0000000043410411db93e1dcdb8a016b49840f8c53bc1eb68a382e97b1482ecad7b148a6"
    "909a5cb2e0eaddfb84ccf9744464f82e160bfa9b8b64f9d4c03f999b8643f656b412a3ac00000000";
const char* LEGACY_TXID = "f4184fc596403b9d638783cf57adfe4c75c605f6356fbc91338530e9831e9e16";
const char* LEGACY_PREV_SPK =
    "410411db93e1dcdb8a016b49840f8c53bc1eb68a382e97b1482ecad7b148a6909a5cb2e0eaddfb84ccf9744464f82e"
    "160bfa9b8b64f9d4c03f999b8643f656b412a3ac";

const char* SEGWIT_TX =
    "020000000001018eacf1a954a04ada078e574bba683c67c693a162e58fd7c1dad94bfb9715bd620100000000fdffff"
    "ff02ac320100000000001976a91463a7577fb68fc9b245ff15ab8f3eac7de6a84bd488ac741f0a0000000000160014"
    "940a14ef14c61cd86ae3961b8ce2b71512711ba702483045022100ace56eebe3f4c7df75e946f15ec2aad379ff9532"
    "182f33de4d393cf9423a5448022026bbafd922618608e3edc960022938e9f3345787034ef725c75efecf0131c4de01"
    "21022685e6ff6aeb01f40979113bc44395d8d3b758ac0ef29cb5bd933ad1a118149d00000000";
const char* SEGWIT_TXID = "9992e8caccb73e6c4ac9c34b86454922eac136c69d687475b538b64fa775528d";
const char* SEGWIT_PREV_PROGRAM = "69509ef6b16c95179855eb995f9493166c22f38c";
const uint64_t SEGWIT_PREV_AMOUNT = 742493;

const char* NESTED_TX =
    "010000000001014659eac7bbe2a9007257936d8eb7817eccb45d284eef3e0c7b6f81503b431b840000000023220020"
    "ded933f2945040d020e59a75c65569c1e909274d4f22fec80cc762e0957ac8b0fdffffff02e891e5000000000022"
    "00201e29c34c76a21e9a4f1d3ec2913535e3078142016be4c96747ef0cf760a9ae75005a62020000000017a9149a41"
    "f56f84b513ed9a11d73fe2732fb43422918c8704004830450221008a4ecb0e0b90050f96651a14236ae36f9031419e"
    "0fd6a863afc9c1495f14ac3a02204711f8f654728f6420267c00a4b9a9cefee292f849d0a7f69ad99ffc63e4f0eb01"
    "47304402200dd25f7951c45920a5f1615ad82b6dbcaa433725f6cba8bb33e368b4676a4ac002201921f6ecbeca4e41"
    "c1743484575f3c0be4d0d25c3145f3127bda65c72bcd83550169522102b4a601ad13a6432f734f8b7f48cee9b28924"
    "0bd1aa27ed47fb8da685be0dc4fd2103dd9221709d91b3d80b6b9dbc49917d5947b56f3d59a15a38d7b6796f8c6ebd"
    "e821031486368c253ada7a15e2a58238a2a2e8779ac4039fbf531e2c7c21897634840b53ae00000000";
const char* NESTED_TXID = "409516c72b7c62cfcab314e48f369cd9c0afcddfadae17e36617b63c84777602";

void round_trip(const std::string& name, const char* hex, const char* want_txid) {
  auto raw = hex_to_bytes(hex);
  auto tx = chain::deserialize_tx(ByteSpan(raw.data(), raw.size()));
  expect_true(name + " parses", tx.ok());
  if (!tx.ok()) return;

  auto again = chain::serialize_tx(*tx, true);
  expect(name + " re-serializes byte for byte", bytes_to_hex(ByteSpan(again.data(), again.size())), hex);
  expect(name + " txid", chain::hash_to_display(chain::txid_of(*tx)), want_txid);
}

void check_transactions() {
  std::printf("real mainnet transactions\n");
  round_trip("legacy tx", LEGACY_TX, LEGACY_TXID);
  round_trip("segwit tx", SEGWIT_TX, SEGWIT_TXID);
  round_trip("nested p2sh-p2wsh tx", NESTED_TX, NESTED_TXID);

  auto raw = hex_to_bytes(SEGWIT_TX);
  auto segwit = chain::deserialize_tx(ByteSpan(raw.data(), raw.size()));
  if (segwit.ok()) {
    ++checks;
    if (chain::hash_to_display(chain::wtxid_of(*segwit)) == std::string(SEGWIT_TXID)) {
      ++failures;
      std::printf("  MISMATCH wtxid equals txid for a witness transaction\n");
    }
    expect_true("segwit vsize below total size", chain::vsize_of(*segwit) < chain::serialize_tx(*segwit, true).size());
  }
}

void check_consensus_signatures() {
  std::printf("signatures the network already accepted\n");

  auto raw = hex_to_bytes(LEGACY_TX);
  auto tx = chain::deserialize_tx(ByteSpan(raw.data(), raw.size()));
  expect_true("legacy tx parses for sighash", tx.ok());
  if (tx.ok()) {
    auto spk = hex_to_bytes(LEGACY_PREV_SPK);
    const auto& script_sig = tx->inputs[0].script_sig;
    expect_true("legacy scriptSig is one push", !script_sig.empty() && script_sig[0] == script_sig.size() - 1);
    ByteBuffer sig_with_type(script_sig.begin() + 1, script_sig.end());
    uint8_t type = sig_with_type.back();
    auto sig = ecdsa_der_decode(ByteSpan(sig_with_type.data(), sig_with_type.size() - 1));
    expect_true("legacy signature parses as der", sig.ok());

    ByteBuffer pubkey(spk.begin() + 1, spk.begin() + 1 + spk[0]);
    auto digest = chain::sighash_legacy(*tx, 0, ByteSpan(spk.data(), spk.size()), type);
    expect_true("legacy sighash verifies against the network's own signature",
                sig.ok() && ecdsa_verify(ByteSpan(pubkey.data(), pubkey.size()), digest.data(), *sig));

    auto wrong = chain::sighash_legacy(*tx, 0, ByteSpan(spk.data(), spk.size()), SIGHASH_NONE);
    expect_false("a different sighash type does not verify",
                 sig.ok() && ecdsa_verify(ByteSpan(pubkey.data(), pubkey.size()), wrong.data(), *sig));
  }

  auto raw2 = hex_to_bytes(SEGWIT_TX);
  auto stx = chain::deserialize_tx(ByteSpan(raw2.data(), raw2.size()));
  expect_true("segwit tx parses for sighash", stx.ok());
  if (stx.ok()) {
    expect_true("segwit input carries two witness items", stx->inputs[0].witness.size() == 2);
    const auto& sig_with_type = stx->inputs[0].witness[0];
    const auto& pubkey = stx->inputs[0].witness[1];

    auto program = hash160(ByteSpan(pubkey.data(), pubkey.size()));
    expect("witness key hashes to the prevout program", hex_of(program), SEGWIT_PREV_PROGRAM);

    ByteBuffer script_code = chain::script_p2pkh(program.data());
    uint8_t type = sig_with_type.back();
    auto sig = ecdsa_der_decode(ByteSpan(sig_with_type.data(), sig_with_type.size() - 1));
    expect_true("witness signature parses as der", sig.ok());

    auto digest = chain::sighash_witness_v0(*stx, 0, ByteSpan(script_code.data(), script_code.size()),
                                            SEGWIT_PREV_AMOUNT, type);
    expect_true("bip143 sighash verifies against the network's own signature",
                sig.ok() && ecdsa_verify(ByteSpan(pubkey.data(), pubkey.size()), digest.data(), *sig));

    auto wrong_amount = chain::sighash_witness_v0(*stx, 0, ByteSpan(script_code.data(), script_code.size()),
                                                  SEGWIT_PREV_AMOUNT + 1, type);
    expect_false("bip143 commits to the input amount",
                 sig.ok() && ecdsa_verify(ByteSpan(pubkey.data(), pubkey.size()), wrong_amount.data(), *sig));
  }
}

void check_addresses_and_build() {
  std::printf("addresses, payment building and script evaluation\n");
  auto p2pkh_script = hex_to_bytes("76a91463a7577fb68fc9b245ff15ab8f3eac7de6a84bd488ac");
  auto addr = chain::address_for_script(ByteSpan(p2pkh_script.data(), p2pkh_script.size()), chain::Network::Mainnet);
  expect_true("p2pkh output has an address", addr.ok());
  if (addr.ok()) {
    auto back = chain::script_for_address(*addr, chain::Network::Mainnet);
    expect_true("address maps back to a script", back.ok());
    if (back.ok()) expect("p2pkh address round trip", hex_of(ByteSpan(back->data(), back->size())),
                          hex_of(ByteSpan(p2pkh_script.data(), p2pkh_script.size())));
  }

  auto p2wpkh_script = hex_to_bytes("0014940a14ef14c61cd86ae3961b8ce2b71512711ba7");
  auto waddr = chain::address_for_script(ByteSpan(p2wpkh_script.data(), p2wpkh_script.size()),
                                         chain::Network::Mainnet);
  expect_true("p2wpkh output has an address", waddr.ok());
  if (waddr.ok()) {
    auto back = chain::script_for_address(*waddr, chain::Network::Mainnet);
    expect_true("bech32 address maps back", back.ok());
    if (back.ok()) expect("p2wpkh address round trip", hex_of(ByteSpan(back->data(), back->size())),
                          hex_of(ByteSpan(p2wpkh_script.data(), p2wpkh_script.size())));
  }

  uint8_t sec[32] = {0};
  sec[31] = 0x77;
  auto pub = ec_pubkey_create(sec, true);
  auto self = chain::address_p2wpkh(ByteSpan(pub->data(), pub->size()), chain::Network::Signet);
  expect_true("signet address derives", self.ok());

  std::vector<chain::SpendableOutput> utxos;
  for (int i = 0; i < 3; ++i) {
    chain::SpendableOutput u;
    u.prevout.txid[0] = (uint8_t)(i + 1);
    u.prevout.index = (uint32_t)i;
    u.amount = 100000;
    auto spk = chain::script_for_address(*self, chain::Network::Signet);
    u.script_pubkey = *spk;
    utxos.push_back(u);
  }

  uint64_t fee_rate = 5;
  auto plan = chain::build_payment(utxos, *self, 150000, *self, fee_rate, chain::Network::Signet);
  expect_true("payment builds", plan.ok());
  if (plan.ok()) {
    expect_true("payment selects two inputs", plan->tx.inputs.size() == 2);
    expect_true("payment has a change output", plan->tx.outputs.size() == 2);
    auto signed_ok = chain::sign_payment(*plan, sec);
    expect_true("payment signs", signed_ok.ok());

    uint64_t input_total = 0;
    for (const auto& s : plan->spent) input_total += s.amount;
    uint64_t output_total = 0;
    for (const auto& o : plan->tx.outputs) output_total += o.value;
    expect_true("inputs cover outputs plus fee", input_total == output_total + plan->fee);
    expect_true("fee matches the rate and size",
                plan->fee >= (uint64_t)chain::vsize_of(plan->tx) * fee_rate);

    auto program = hash160(ByteSpan(pub->data(), pub->size()));
    ByteBuffer script_code = chain::script_p2pkh(program.data());
    const auto& w = plan->tx.inputs[0].witness;
    expect_true("signed input has a witness", w.size() == 2);
    if (w.size() == 2) {
      auto sig = ecdsa_der_decode(ByteSpan(w[0].data(), w[0].size() - 1));
      auto digest = chain::sighash_witness_v0(plan->tx, 0, ByteSpan(script_code.data(), script_code.size()),
                                              plan->spent[0].amount, w[0].back());
      expect_true("our own signature verifies against our own sighash",
                  sig.ok() && ecdsa_verify(ByteSpan(w[1].data(), w[1].size()), digest.data(), *sig));
    }
  }

  std::string secret = "the preimage nobody guessed";
  auto target = sha256(text(secret));
  auto lock = chain::script_hashlock(target.data(), ByteSpan(pub->data(), pub->size()));

  chain::Transaction claim;
  claim.inputs.push_back(chain::TxIn{});
  claim.outputs.push_back(chain::TxOut{1000, ByteBuffer{0x6a}});
  chain::ScriptContext ctx;
  ctx.tx = &claim;
  ctx.input_index = 0;
  ctx.amount = 2000;
  ctx.witness = true;

  auto digest = chain::sighash_witness_v0(claim, 0, ByteSpan(lock.data(), lock.size()), 2000, SIGHASH_ALL);
  auto sig = ecdsa_sign(sec, digest.data());
  ByteBuffer sig_bytes = ecdsa_der_encode(*sig);
  sig_bytes.push_back(SIGHASH_ALL);

  std::vector<ByteBuffer> good = {sig_bytes, ByteBuffer(secret.begin(), secret.end())};
  auto ran = chain::evaluate_script_stack(good, ByteSpan(lock.data(), lock.size()), ctx);
  expect_true("hashlock evaluates", ran.ok());
  expect_true("correct preimage and signature unlock", ran.ok() && *ran);

  std::string wrong_secret = "the preimage nobody guessee";
  std::vector<ByteBuffer> bad = {sig_bytes, ByteBuffer(wrong_secret.begin(), wrong_secret.end())};
  auto ran_bad = chain::evaluate_script_stack(bad, ByteSpan(lock.data(), lock.size()), ctx);
  expect_false("wrong preimage does not unlock", ran_bad.ok() && *ran_bad);

  std::vector<ByteBuffer> no_sig = {ByteBuffer{}, ByteBuffer(secret.begin(), secret.end())};
  auto ran_nosig = chain::evaluate_script_stack(no_sig, ByteSpan(lock.data(), lock.size()), ctx);
  expect_false("correct preimage without a signature does not unlock", ran_nosig.ok() && *ran_nosig);
}

void check_ethereum() {
  std::printf("Ethereum RLP, EIP-155 and EIP-1559\n");

  expect("rlp('dog')", hex_of(ByteSpan(chain::rlp_bytes(text("dog")).data(), 4)), "83646f67");
  auto empty_string = chain::rlp_bytes(ByteSpan());
  expect("rlp('')", hex_of(ByteSpan(empty_string.data(), empty_string.size())), "80");
  auto empty_list = chain::rlp_list({});
  expect("rlp([])", hex_of(ByteSpan(empty_list.data(), empty_list.size())), "c0");
  auto zero = chain::rlp_number(0);
  expect("rlp(0) is the empty string", hex_of(ByteSpan(zero.data(), zero.size())), "80");
  auto fifteen = chain::rlp_number(15);
  expect("rlp(15)", hex_of(ByteSpan(fifteen.data(), fifteen.size())), "0f");
  auto big = chain::rlp_number(1024);
  expect("rlp(1024)", hex_of(ByteSpan(big.data(), big.size())), "820400");
  auto cat_dog = chain::rlp_list({chain::rlp_bytes(text("cat")), chain::rlp_bytes(text("dog"))});
  expect("rlp(['cat','dog'])", hex_of(ByteSpan(cat_dog.data(), cat_dog.size())), "c88363617483646f67");

  std::string lorem = "Lorem ipsum dolor sit amet, consectetur adipisicing elit";
  auto long_string = chain::rlp_bytes(text(lorem));
  expect_true("rlp long string uses the 0xb8 form", long_string.size() > 2 && long_string[0] == 0xb8);

  auto round = chain::rlp_decode(ByteSpan(cat_dog.data(), cat_dog.size()));
  expect_true("rlp list decodes", round.ok() && round->is_list && round->items.size() == 2);
  if (round.ok() && round->items.size() == 2) {
    expect("rlp decode item 0", std::string(round->items[0].data.begin(), round->items[0].data.end()), "cat");
    expect("rlp decode item 1", std::string(round->items[1].data.begin(), round->items[1].data.end()), "dog");
  }

  // The EIP-155 specification's own worked example. Reproducing its signed
  // bytes exactly exercises RLP, keccak, the v derivation and the deterministic
  // signature in one shot; nothing here is our own invention to check against.
  chain::EthTransaction tx;
  tx.chain_id = 1;
  tx.nonce = 9;
  tx.gas_price = hex_to_bytes("04a817c800");
  tx.gas_limit = 21000;
  tx.to = *chain::eth_parse_address("0x3535353535353535353535353535353535353535");
  tx.value = hex_to_bytes("0de0b6b3a7640000");

  // This digest is pinned by the signed bytes below rather than by anyone's
  // recollection: the specification's own r and s verify against it under an
  // independent implementation, and against nothing else.
  auto payload = chain::eth_signing_payload_legacy(tx);
  auto digest = keccak256(ByteSpan(payload.data(), payload.size()));
  expect("eip-155 signing hash", hex_of(digest),
         "daf5a779ae972f972197303d7b574746c7ef83eadac0f2791ad23db92e4c8e53");

  auto key = hex_to_bytes("4646464646464646464646464646464646464646464646464646464646464646");
  auto signed_tx = chain::eth_sign_legacy(tx, key.data());
  expect_true("eip-155 signs", signed_tx.ok());
  if (signed_tx.ok()) {
    expect("eip-155 signed transaction bytes", hex_of(ByteSpan(signed_tx->data(), signed_tx->size())),
           "f86c098504a817c800825208943535353535353535353535353535353535353535880de0b6b3a764000080"
           "25a028ef61340bd939bc2195fe537567866003e1a15d3c71ff63e1590620aa636276a067cbe9d8997f761ae"
           "cb703304b3800ccf555c9f3dc64214b297fb1966a3b6d83");

    auto sender = chain::eth_recover_sender(ByteSpan(signed_tx->data(), signed_tx->size()));
    auto expected = chain::eth_address_of_key(key.data());
    expect_true("eip-155 sender recovers", sender.ok() && expected.ok());
    if (sender.ok() && expected.ok()) {
      expect("recovered sender is the signer", chain::eth_checksum_address(*sender),
             chain::eth_checksum_address(*expected));
    }
  }

  // EIP-55 mixed-case checksums, from the specification's examples.
  auto a1 = chain::eth_parse_address("0x5aaeb6053f3e94c9b9a09f33669435e7ef1beaed");
  expect("eip-55 checksum 1", chain::eth_checksum_address(*a1),
         "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed");
  auto a2 = chain::eth_parse_address("0xfb6916095ca1df60bb79ce92ce3ea74c37c5d359");
  expect("eip-55 checksum 2", chain::eth_checksum_address(*a2),
         "0xfB6916095ca1df60bB79Ce92cE3Ea74c37c5d359");
  auto a3 = chain::eth_parse_address("0xde709f2102306220921060314715629080e2fb77");
  expect("eip-55 checksum 3", chain::eth_checksum_address(*a3),
         "0xde709f2102306220921060314715629080e2fb77");

  // A type 2 transaction the network actually carried, rebuilt from its fields.
  // Matching its canonical hash proves the encoding is byte-exact, and
  // recovering the sender the network reports proves the signing payload is the
  // one that was signed. Neither can be satisfied by a plausible-looking guess.
  chain::EthTransaction live;
  live.chain_id = 1;
  live.nonce = 0x55085;
  live.max_priority_fee_per_gas = hex_to_bytes("");
  live.max_fee_per_gas = hex_to_bytes("04a817c800");
  live.gas_limit = 0xf424a;
  live.to = *chain::eth_parse_address("0x27ad186bb115a2b41fb64553efd4ba5a74b83b08");
  live.value = hex_to_bytes("043cd4ebc202b6");
  live.data = hex_to_bytes(
      "dd9d4fa00000000000000000000000000000000000000000000000000000000000000020000000000000000000000000"
      "000000000000000000000000000000000000000100000000000000000000000000000000000000000000000000000000"
      "00000020000000000000000000000000000000000000000000000000000000000000012a001800000000000000000000"
      "000000000000000000000000000000043cd4ebc202b627ad186bb115a2b41fb64553efd4ba5a74b83b0827ad186bb115"
      "a2b41fb64553efd4ba5a74b83b08b1d1eae60eea9525032a6dcb4c1ce336a1de71be0000000000000000000000000000"
      "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
      "0050efad0cb1a9f500000000000000000000000000000000000000000000000000000155a88aa52f6c92003a9800012c"
      "010100000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
      "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
      "0000000000000000000000000000000000000000");

  auto live_r = hex_to_bytes("4d173f18c6471e08d74669f0765c67233353fec9f8c9349f168ccd58cc9c0663");
  auto live_s = hex_to_bytes("6138250a0ec152eda3887edac43f25dcb0114427704c30b5905f278593ccf44c");
  auto live_raw = chain::eth_encode_signed_1559(live, 0, ByteSpan(live_r.data(), live_r.size()),
                                                ByteSpan(live_s.data(), live_s.size()));
  expect("mainnet type 2 transaction hash", hex_of(chain::eth_transaction_hash(
                                                ByteSpan(live_raw.data(), live_raw.size()))),
         "3d91c39646513a1714eab0c7c4e7f811a288f2a071bcf32ade2b699e611a1756");

  auto live_sender = chain::eth_recover_sender(ByteSpan(live_raw.data(), live_raw.size()));
  expect_true("mainnet type 2 sender recovers", live_sender.ok());
  if (live_sender.ok()) {
    std::string got = chain::eth_checksum_address(*live_sender);
    std::string want_lower = "0xfe0c760cbcb9da239b9ba805f0aeaed3be84f65a";
    std::string got_lower;
    for (char ch : got) got_lower.push_back((ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch);
    expect("mainnet type 2 sender is the address the network reports", got_lower, want_lower);
  }

  chain::EthTransaction modern;
  modern.chain_id = 11155111;
  modern.nonce = 3;
  modern.max_priority_fee_per_gas = hex_to_bytes("3b9aca00");
  modern.max_fee_per_gas = hex_to_bytes("0ba43b7400");
  modern.gas_limit = 21000;
  modern.to = *chain::eth_parse_address("0x0000000000000000000000000000000000001234");
  modern.value = hex_to_bytes("0de0b6b3a7640000");

  auto typed = chain::eth_sign_1559(modern, key.data());
  expect_true("type 2 signs", typed.ok());
  if (typed.ok()) {
    expect_true("type 2 carries its envelope byte", (*typed)[0] == 0x02);
    auto sender = chain::eth_recover_sender(ByteSpan(typed->data(), typed->size()));
    auto expected = chain::eth_address_of_key(key.data());
    expect_true("type 2 sender recovers", sender.ok());
    if (sender.ok() && expected.ok()) {
      expect("type 2 recovered sender is the signer", chain::eth_checksum_address(*sender),
             chain::eth_checksum_address(*expected));
    }
  }
}

int emit(int count) {
  for (int i = 1; i <= count; ++i) {
    uint8_t seed[8];
    for (int j = 0; j < 8; ++j) seed[j] = (uint8_t)((unsigned)i >> (8 * (7 - j)));
    auto sec = sha256(ByteSpan(seed, 8));
    if (!ec_seckey_valid(sec.data())) continue;
    auto msg = sha256(ByteSpan(sec.data(), 32));
    auto pub = ec_pubkey_create(sec.data(), true);
    auto sig = ecdsa_sign(sec.data(), msg.data());
    if (!pub.ok() || !sig.ok()) return 1;
    std::printf("%s %s %s %s %s %d\n", hex_of(ByteSpan(sec.data(), 32)).c_str(),
                hex_of(ByteSpan(msg.data(), 32)).c_str(),
                hex_of(ByteSpan(pub->data(), pub->size())).c_str(), hex_of(sig->r).c_str(),
                hex_of(sig->s).c_str(), sig->recovery_id);
  }
  return 0;
}

void check_chain_storage_residency() {
  std::printf("Chain Transaction Storage & Residency Micro-Harness\n");
  char root_dir[] = "/tmp/aios-chain-check-XXXXXX";
  if (!mkdtemp(root_dir)) {
    std::perror("mkdtemp");
    ++failures;
    return;
  }

  KernelConfig kcfg;
  kcfg.root = root_dir;
  kcfg.residency = test_config;
  auto k_res = Kernel::boot(kcfg);
  expect_true("Kernel::boot with test_config", k_res.ok());
  if (!k_res.ok()) return;
  Kernel& kernel = **k_res;

  expect_true("ResidencyConfig block_objects == 8", kernel.store().residency_config().block_objects == 8);
  expect_true("ResidencyConfig max_active == 16", kernel.store().residency_config().max_active == 16);
  expect_true("ResidencyConfig hot_budget == 64KB", kernel.store().residency_config().hot_budget == 64 * 1024);

  // Store 25 chain transaction records to cross the 8/16 thresholds
  std::vector<ObjectID> tx_ids;
  for (int i = 1; i <= 25; ++i) {
    StoredObject obj;
    obj.metadata.emplace_back("kind", "chain.tx_record");
    obj.metadata.emplace_back("chain_id", "1");
    obj.metadata.emplace_back("nonce", std::to_string(i));
    obj.metadata.emplace_back("tx_type", (i % 2 == 0) ? "eip-1559" : "legacy");
    // 3KB payload per tx to drive memory pressure past 64KB budget
    obj.payload = ByteBuffer(3072, static_cast<uint8_t>(0x55 + (i % 16)));
    auto put_r = kernel.store().put(obj);
    expect_true("store().put chain tx", put_r.ok());
    if (put_r.ok()) tx_ids.push_back(*put_r);
  }

  // Verify auto-sealing occurred: cold_blocks_count() > 0
  expect_true("cold_blocks_count() > 0 after 25 tx stores", kernel.store().cold_blocks_count() > 0);

  // Query meta across tiers
  auto legacy_txs = kernel.store().find_meta("tx_type", "legacy");
  expect_true("find_meta resolves legacy txs across active & sealed tiers", legacy_txs.size() == 13);

  // Exactly-once resolution
  std::unordered_set<ObjectID, TypedIdHash<9>> seen;
  for (const auto& id : legacy_txs) {
    seen.insert(id);
  }
  expect_true("exactly-once resolution across tiers", seen.size() == legacy_txs.size());

  // Point get and round-trip fidelity
  for (size_t i = 0; i < tx_ids.size(); ++i) {
    auto got = kernel.store().get(tx_ids[i]);
    expect_true("get(tx_id) resolves", got.ok());
    if (got.ok()) {
      expect("metadata nonce matches", got->meta("nonce"), std::to_string(i + 1));
      expect_true("payload size exact", got->payload.size() == 3072);
      expect_true("payload byte exact", got->payload[0] == static_cast<uint8_t>(0x55 + ((i + 1) % 16)));
    }
  }

  // Count and collection identity
  expect_true("count() == active + cold",
              kernel.store().count() == kernel.store().active_count() + kernel.store().cold_objects_count());
  expect_true("list().size() == count()", kernel.store().list().size() == kernel.store().count());

  // Budget integrity
  expect_true("hot_bytes() <= hot_budget()", kernel.store().hot_bytes() <= kernel.store().hot_budget());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 3 && std::strcmp(argv[1], "--emit") == 0) {
    return emit(std::atoi(argv[2]));
  }

  check_hashes();
  check_hmac();
  check_keys();
  check_bech32();
  check_signing();
  check_transactions();
  check_consensus_signatures();
  check_addresses_and_build();
  check_ethereum();
  check_chain_storage_residency();

  std::printf("\n%d checks, %d mismatches\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
