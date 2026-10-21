#include "aios/userland/chain/btc_tx.hpp"
#include <cstring>
#include "aios/crypto/base58check.hpp"
#include "aios/crypto/bech32.hpp"
#include "aios/crypto/ripemd160.hpp"
#include "aios/crypto/secp256k1.hpp"
#include "aios/crypto/sha256.hpp"

namespace aios::userland::chain {

using namespace aios::crypto;

namespace {

const NetworkParams MAINNET = {0x00, 0x05, 0x80, "bc", 0xd9b4bef9u, 8333, "mainnet"};
const NetworkParams TESTNET = {0x6f, 0xc4, 0xef, "tb", 0x0709110bu, 18333, "testnet"};
const NetworkParams SIGNET = {0x6f, 0xc4, 0xef, "tb", 0x40cf030au, 38333, "signet"};
const NetworkParams REGTEST = {0x6f, 0xc4, 0xef, "bcrt", 0xdab5bffau, 18444, "regtest"};

void write_u16(ByteBuffer& out, uint16_t v) {
  out.push_back((uint8_t)v);
  out.push_back((uint8_t)(v >> 8));
}

void write_u32(ByteBuffer& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back((uint8_t)(v >> (8 * i)));
}

void write_u64(ByteBuffer& out, uint64_t v) {
  for (int i = 0; i < 8; ++i) out.push_back((uint8_t)(v >> (8 * i)));
}

void write_bytes(ByteBuffer& out, ByteSpan b) { out.insert(out.end(), b.begin(), b.end()); }

class Cursor {
 public:
  explicit Cursor(ByteSpan data) : data_(data) {}
  bool ok() const { return ok_; }
  size_t left() const { return ok_ ? data_.size() - pos_ : 0; }
  size_t position() const { return pos_; }

  uint8_t u8() {
    if (left() < 1) { ok_ = false; return 0; }
    return data_[pos_++];
  }
  uint16_t u16() {
    uint16_t v = 0;
    for (int i = 0; i < 2; ++i) v |= (uint16_t)((uint16_t)u8() << (8 * i));
    return v;
  }
  uint32_t u32() {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= (uint32_t)u8() << (8 * i);
    return v;
  }
  uint64_t u64() {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= (uint64_t)u8() << (8 * i);
    return v;
  }
  uint64_t varint() {
    uint8_t tag = u8();
    if (tag < 0xfd) return tag;
    if (tag == 0xfd) return u16();
    if (tag == 0xfe) return u32();
    return u64();
  }
  ByteBuffer take(size_t n) {
    if (left() < n) { ok_ = false; return {}; }
    ByteBuffer out(data_.begin() + (ptrdiff_t)pos_, data_.begin() + (ptrdiff_t)(pos_ + n));
    pos_ += n;
    return out;
  }
  uint8_t peek() const { return (pos_ < data_.size()) ? data_[pos_] : 0; }
  void rewind(size_t p) { pos_ = p; }

 private:
  ByteSpan data_;
  size_t pos_ = 0;
  bool ok_ = true;
};

void serialize_outputs(ByteBuffer& out, const std::vector<TxOut>& outputs) {
  write_varint(out, outputs.size());
  for (const auto& o : outputs) {
    write_u64(out, o.value);
    write_varint(out, o.script_pubkey.size());
    write_bytes(out, ByteSpan(o.script_pubkey.data(), o.script_pubkey.size()));
  }
}

std::array<uint8_t, 32> sha256d_of(const ByteBuffer& b) {
  return sha256d(ByteSpan(b.data(), b.size()));
}

}  // namespace

const NetworkParams& network_params(Network n) {
  switch (n) {
    case Network::Testnet: return TESTNET;
    case Network::Signet: return SIGNET;
    case Network::Regtest: return REGTEST;
    case Network::Mainnet:
    default: return MAINNET;
  }
}

Result<Network> network_by_name(const std::string& name) {
  if (name == "mainnet" || name == "main" || name == "bitcoin") return Network::Mainnet;
  if (name == "testnet" || name == "test") return Network::Testnet;
  if (name == "signet") return Network::Signet;
  if (name == "regtest") return Network::Regtest;
  return Result<Network>(Status::Invalid, "unknown network: " + name);
}

bool Transaction::has_witness() const {
  for (const auto& in : inputs) {
    if (!in.witness.empty()) return true;
  }
  return false;
}

void write_varint(ByteBuffer& out, uint64_t value) {
  if (value < 0xfd) {
    out.push_back((uint8_t)value);
  } else if (value <= 0xffff) {
    out.push_back(0xfd);
    write_u16(out, (uint16_t)value);
  } else if (value <= 0xffffffffull) {
    out.push_back(0xfe);
    write_u32(out, (uint32_t)value);
  } else {
    out.push_back(0xff);
    write_u64(out, value);
  }
}

void write_push(ByteBuffer& out, ByteSpan data) {
  size_t n = data.size();
  if (n < 0x4c) {
    out.push_back((uint8_t)n);
  } else if (n <= 0xff) {
    out.push_back(0x4c);
    out.push_back((uint8_t)n);
  } else if (n <= 0xffff) {
    out.push_back(0x4d);
    write_u16(out, (uint16_t)n);
  } else {
    out.push_back(0x4e);
    write_u32(out, (uint32_t)n);
  }
  write_bytes(out, data);
}

ByteBuffer serialize_tx(const Transaction& tx, bool with_witness) {
  ByteBuffer out;
  write_u32(out, (uint32_t)tx.version);

  bool segwit = with_witness && tx.has_witness();
  if (segwit) {
    out.push_back(0x00);
    out.push_back(0x01);
  }

  write_varint(out, tx.inputs.size());
  for (const auto& in : tx.inputs) {
    write_bytes(out, ByteSpan(in.prevout.txid.data(), 32));
    write_u32(out, in.prevout.index);
    write_varint(out, in.script_sig.size());
    write_bytes(out, ByteSpan(in.script_sig.data(), in.script_sig.size()));
    write_u32(out, in.sequence);
  }

  serialize_outputs(out, tx.outputs);

  if (segwit) {
    for (const auto& in : tx.inputs) {
      write_varint(out, in.witness.size());
      for (const auto& item : in.witness) {
        write_varint(out, item.size());
        write_bytes(out, ByteSpan(item.data(), item.size()));
      }
    }
  }

  write_u32(out, tx.lock_time);
  return out;
}

Result<Transaction> deserialize_tx(ByteSpan raw) {
  Cursor c(raw);
  Transaction tx;
  tx.version = (int32_t)c.u32();

  bool segwit = false;
  size_t after_version = c.position();
  uint64_t count = c.varint();
  if (count == 0) {
    uint8_t flag = c.u8();
    if (flag != 0x01) return Result<Transaction>(Status::Invalid, "bad segwit marker");
    segwit = true;
    count = c.varint();
  } else {
    c.rewind(after_version);
    count = c.varint();
  }

  if (!c.ok() || count == 0) return Result<Transaction>(Status::Invalid, "transaction has no inputs");
  for (uint64_t i = 0; i < count; ++i) {
    TxIn in;
    auto txid = c.take(32);
    if (!c.ok()) return Result<Transaction>(Status::Invalid, "truncated input outpoint");
    std::memcpy(in.prevout.txid.data(), txid.data(), 32);
    in.prevout.index = c.u32();
    uint64_t script_len = c.varint();
    in.script_sig = c.take(script_len);
    in.sequence = c.u32();
    if (!c.ok()) return Result<Transaction>(Status::Invalid, "truncated input");
    tx.inputs.push_back(std::move(in));
  }

  uint64_t out_count = c.varint();
  for (uint64_t i = 0; i < out_count; ++i) {
    TxOut o;
    o.value = c.u64();
    uint64_t script_len = c.varint();
    o.script_pubkey = c.take(script_len);
    if (!c.ok()) return Result<Transaction>(Status::Invalid, "truncated output");
    tx.outputs.push_back(std::move(o));
  }

  if (segwit) {
    for (auto& in : tx.inputs) {
      uint64_t items = c.varint();
      for (uint64_t j = 0; j < items; ++j) {
        uint64_t len = c.varint();
        in.witness.push_back(c.take(len));
        if (!c.ok()) return Result<Transaction>(Status::Invalid, "truncated witness");
      }
    }
  }

  tx.lock_time = c.u32();
  if (!c.ok()) return Result<Transaction>(Status::Invalid, "truncated transaction");
  if (c.left() != 0) return Result<Transaction>(Status::Invalid, "trailing bytes after transaction");
  return tx;
}

std::array<uint8_t, 32> txid_of(const Transaction& tx) {
  return sha256d_of(serialize_tx(tx, false));
}

std::array<uint8_t, 32> wtxid_of(const Transaction& tx) {
  return sha256d_of(serialize_tx(tx, true));
}

std::string hash_to_display(const std::array<uint8_t, 32>& h) {
  std::array<uint8_t, 32> reversed{};
  for (int i = 0; i < 32; ++i) reversed[(size_t)i] = h[31 - (size_t)i];
  return bytes_to_hex(ByteSpan(reversed.data(), 32));
}

Result<std::array<uint8_t, 32>> hash_from_display(const std::string& hex) {
  if (hex.size() != 64) return Result<std::array<uint8_t, 32>>(Status::Invalid, "hash must be 64 hex characters");
  auto bytes = hex_to_bytes(hex);
  if (bytes.size() != 32) return Result<std::array<uint8_t, 32>>(Status::Invalid, "hash is not hex");
  std::array<uint8_t, 32> out{};
  for (int i = 0; i < 32; ++i) out[(size_t)i] = bytes[31 - (size_t)i];
  return out;
}

ByteBuffer script_p2pkh(const uint8_t h160[20]) {
  ByteBuffer s;
  s.push_back(0x76);   // OP_DUP
  s.push_back(0xa9);   // OP_HASH160
  write_push(s, ByteSpan(h160, 20));
  s.push_back(0x88);   // OP_EQUALVERIFY
  s.push_back(0xac);   // OP_CHECKSIG
  return s;
}

ByteBuffer script_p2sh(const uint8_t h160[20]) {
  ByteBuffer s;
  s.push_back(0xa9);
  write_push(s, ByteSpan(h160, 20));
  s.push_back(0x87);   // OP_EQUAL
  return s;
}

ByteBuffer script_p2wpkh(const uint8_t h160[20]) {
  ByteBuffer s;
  s.push_back(0x00);
  write_push(s, ByteSpan(h160, 20));
  return s;
}

ByteBuffer script_p2wsh(const uint8_t sha[32]) {
  ByteBuffer s;
  s.push_back(0x00);
  write_push(s, ByteSpan(sha, 32));
  return s;
}

ByteBuffer script_op_return(ByteSpan data) {
  ByteBuffer s;
  s.push_back(0x6a);
  write_push(s, data);
  return s;
}

ByteBuffer script_hashlock(const uint8_t target[32], ByteSpan redeem_pubkey) {
  ByteBuffer s;
  s.push_back(0xa8);   // OP_SHA256
  write_push(s, ByteSpan(target, 32));
  s.push_back(0x88);   // OP_EQUALVERIFY
  write_push(s, redeem_pubkey);
  s.push_back(0xac);   // OP_CHECKSIG
  return s;
}

ByteBuffer script_htlc(const uint8_t target[32], ByteSpan claim_pubkey, ByteSpan refund_pubkey,
                       uint32_t lock_time) {
  ByteBuffer s;
  s.push_back(0x63);   // OP_IF
  s.push_back(0xa8);   // OP_SHA256
  write_push(s, ByteSpan(target, 32));
  s.push_back(0x88);   // OP_EQUALVERIFY
  write_push(s, claim_pubkey);
  s.push_back(0x67);   // OP_ELSE
  ByteBuffer lt;
  for (uint32_t v = lock_time; v != 0; v >>= 8) lt.push_back((uint8_t)(v & 0xff));
  if (lt.empty()) lt.push_back(0);
  if (lt.back() & 0x80) lt.push_back(0);
  write_push(s, ByteSpan(lt.data(), lt.size()));
  s.push_back(0xb1);   // OP_CHECKLOCKTIMEVERIFY
  s.push_back(0x75);   // OP_DROP
  write_push(s, refund_pubkey);
  s.push_back(0x68);   // OP_ENDIF
  s.push_back(0xac);   // OP_CHECKSIG
  return s;
}

Result<ByteBuffer> script_for_address(const std::string& address, Network net) {
  const auto& p = network_params(net);
  uint8_t version = 0;
  auto decoded = base58check_decode(address, version);
  if (decoded.ok()) {
    if (decoded->size() != 20) return Result<ByteBuffer>(Status::Invalid, "address payload is not 20 bytes");
    if (version == p.p2pkh_version) return script_p2pkh(decoded->data());
    if (version == p.p2sh_version) return script_p2sh(decoded->data());
    return Result<ByteBuffer>(Status::Invalid, "address version does not belong to this network");
  }

  uint8_t witness_version = 0;
  auto program = segwit_decode(address, p.bech32_hrp, witness_version);
  if (!program.ok()) return Result<ByteBuffer>(Status::Invalid, "address is neither base58check nor bech32");

  ByteBuffer s;
  s.push_back(witness_version == 0 ? 0x00 : (uint8_t)(0x50 + witness_version));
  write_push(s, ByteSpan(program->data(), program->size()));
  return s;
}

Result<std::string> address_for_script(ByteSpan script, Network net) {
  const auto& p = network_params(net);
  if (script.size() == 25 && script[0] == 0x76 && script[1] == 0xa9 && script[2] == 0x14 &&
      script[23] == 0x88 && script[24] == 0xac) {
    return base58check_encode(p.p2pkh_version, script.subspan(3, 20));
  }
  if (script.size() == 23 && script[0] == 0xa9 && script[1] == 0x14 && script[22] == 0x87) {
    return base58check_encode(p.p2sh_version, script.subspan(2, 20));
  }
  if (script.size() >= 4 && script.size() <= 42) {
    uint8_t op = script[0];
    uint8_t version = 0xff;
    if (op == 0x00) version = 0;
    else if (op >= 0x51 && op <= 0x60) version = (uint8_t)(op - 0x50);
    if (version != 0xff && script[1] == script.size() - 2) {
      return segwit_encode(p.bech32_hrp, version, script.subspan(2, script.size() - 2));
    }
  }
  return Result<std::string>(Status::Unsupported, "script has no address form");
}

Result<std::string> address_p2wpkh(ByteSpan pubkey, Network net) {
  if (pubkey.size() != 33) {
    return Result<std::string>(Status::Invalid, "p2wpkh requires a compressed public key");
  }
  auto h = hash160(pubkey);
  return segwit_encode(network_params(net).bech32_hrp, 0, ByteSpan(h.data(), 20));
}

Result<std::string> address_p2pkh(ByteSpan pubkey, Network net) {
  auto h = hash160(pubkey);
  return base58check_encode(network_params(net).p2pkh_version, ByteSpan(h.data(), 20));
}

std::array<uint8_t, 32> sighash_legacy(const Transaction& tx, size_t index, ByteSpan script_code, uint8_t type) {
  uint8_t base = (uint8_t)(type & 0x1f);
  bool anyone = (type & SIGHASH_ANYONECANPAY) != 0;

  if (base == SIGHASH_SINGLE && index >= tx.outputs.size()) {
    std::array<uint8_t, 32> one{};
    one[0] = 1;
    return one;
  }

  ByteBuffer out;
  write_u32(out, (uint32_t)tx.version);

  if (anyone) {
    write_varint(out, 1);
    write_bytes(out, ByteSpan(tx.inputs[index].prevout.txid.data(), 32));
    write_u32(out, tx.inputs[index].prevout.index);
    write_varint(out, script_code.size());
    write_bytes(out, script_code);
    write_u32(out, tx.inputs[index].sequence);
  } else {
    write_varint(out, tx.inputs.size());
    for (size_t i = 0; i < tx.inputs.size(); ++i) {
      write_bytes(out, ByteSpan(tx.inputs[i].prevout.txid.data(), 32));
      write_u32(out, tx.inputs[i].prevout.index);
      if (i == index) {
        write_varint(out, script_code.size());
        write_bytes(out, script_code);
      } else {
        write_varint(out, 0);
      }
      uint32_t seq = tx.inputs[i].sequence;
      if (i != index && (base == SIGHASH_NONE || base == SIGHASH_SINGLE)) seq = 0;
      write_u32(out, seq);
    }
  }

  if (base == SIGHASH_NONE) {
    write_varint(out, 0);
  } else if (base == SIGHASH_SINGLE) {
    write_varint(out, index + 1);
    for (size_t i = 0; i < index; ++i) {
      write_u64(out, 0xffffffffffffffffull);
      write_varint(out, 0);
    }
    write_u64(out, tx.outputs[index].value);
    write_varint(out, tx.outputs[index].script_pubkey.size());
    write_bytes(out, ByteSpan(tx.outputs[index].script_pubkey.data(), tx.outputs[index].script_pubkey.size()));
  } else {
    serialize_outputs(out, tx.outputs);
  }

  write_u32(out, tx.lock_time);
  write_u32(out, (uint32_t)type);
  return sha256d_of(out);
}

std::array<uint8_t, 32> sighash_witness_v0(const Transaction& tx, size_t index, ByteSpan script_code,
                                           uint64_t amount, uint8_t type) {
  uint8_t base = (uint8_t)(type & 0x1f);
  bool anyone = (type & SIGHASH_ANYONECANPAY) != 0;

  std::array<uint8_t, 32> hash_prevouts{};
  std::array<uint8_t, 32> hash_sequence{};
  std::array<uint8_t, 32> hash_outputs{};

  if (!anyone) {
    ByteBuffer b;
    for (const auto& in : tx.inputs) {
      write_bytes(b, ByteSpan(in.prevout.txid.data(), 32));
      write_u32(b, in.prevout.index);
    }
    hash_prevouts = sha256d_of(b);
  }

  if (!anyone && base != SIGHASH_SINGLE && base != SIGHASH_NONE) {
    ByteBuffer b;
    for (const auto& in : tx.inputs) write_u32(b, in.sequence);
    hash_sequence = sha256d_of(b);
  }

  if (base != SIGHASH_SINGLE && base != SIGHASH_NONE) {
    ByteBuffer b;
    for (const auto& o : tx.outputs) {
      write_u64(b, o.value);
      write_varint(b, o.script_pubkey.size());
      write_bytes(b, ByteSpan(o.script_pubkey.data(), o.script_pubkey.size()));
    }
    hash_outputs = sha256d_of(b);
  } else if (base == SIGHASH_SINGLE && index < tx.outputs.size()) {
    ByteBuffer b;
    write_u64(b, tx.outputs[index].value);
    write_varint(b, tx.outputs[index].script_pubkey.size());
    write_bytes(b, ByteSpan(tx.outputs[index].script_pubkey.data(), tx.outputs[index].script_pubkey.size()));
    hash_outputs = sha256d_of(b);
  }

  ByteBuffer out;
  write_u32(out, (uint32_t)tx.version);
  write_bytes(out, ByteSpan(hash_prevouts.data(), 32));
  write_bytes(out, ByteSpan(hash_sequence.data(), 32));
  write_bytes(out, ByteSpan(tx.inputs[index].prevout.txid.data(), 32));
  write_u32(out, tx.inputs[index].prevout.index);
  write_varint(out, script_code.size());
  write_bytes(out, script_code);
  write_u64(out, amount);
  write_u32(out, tx.inputs[index].sequence);
  write_bytes(out, ByteSpan(hash_outputs.data(), 32));
  write_u32(out, tx.lock_time);
  write_u32(out, (uint32_t)type);
  return sha256d_of(out);
}

size_t vsize_of(const Transaction& tx) {
  size_t base = serialize_tx(tx, false).size();
  size_t total = serialize_tx(tx, true).size();
  size_t weight = base * 3 + total;
  return (weight + 3) / 4;
}

}  // namespace aios::userland::chain
