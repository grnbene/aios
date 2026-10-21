#include <cstring>
#include "aios/crypto/ripemd160.hpp"
#include "aios/crypto/secp256k1.hpp"
#include "aios/crypto/sha256.hpp"
#include "aios/userland/chain/btc_tx.hpp"

namespace aios::userland::chain {

using namespace aios::crypto;

namespace {

using Stack = std::vector<ByteBuffer>;

bool truth_of(const ByteBuffer& v) {
  for (size_t i = 0; i < v.size(); ++i) {
    if (v[i] != 0) return !(i == v.size() - 1 && v[i] == 0x80);
  }
  return false;
}

ByteBuffer num_to_bytes(int64_t value) {
  ByteBuffer out;
  if (value == 0) return out;
  bool negative = value < 0;
  uint64_t magnitude = negative ? (uint64_t)(-value) : (uint64_t)value;
  while (magnitude) {
    out.push_back((uint8_t)(magnitude & 0xff));
    magnitude >>= 8;
  }
  if (out.back() & 0x80) {
    out.push_back(negative ? 0x80 : 0x00);
  } else if (negative) {
    out.back() |= 0x80;
  }
  return out;
}

bool bytes_to_num(const ByteBuffer& v, int64_t& out, size_t max_size = 4) {
  if (v.size() > max_size) return false;
  out = 0;
  if (v.empty()) return true;
  for (size_t i = 0; i < v.size(); ++i) out |= (int64_t)v[i] << (8 * i);
  if (v.back() & 0x80) {
    out &= ~((int64_t)0x80 << (8 * (v.size() - 1)));
    out = -out;
  }
  return true;
}

struct Reader {
  ByteSpan script;
  size_t pos = 0;
  bool fault = false;

  bool done() const { return pos >= script.size(); }
  uint8_t byte() {
    if (pos >= script.size()) { fault = true; return 0; }
    return script[pos++];
  }
  bool take(size_t n, ByteBuffer& out) {
    if (pos + n > script.size()) { fault = true; return false; }
    out.assign(script.begin() + (ptrdiff_t)pos, script.begin() + (ptrdiff_t)(pos + n));
    pos += n;
    return true;
  }
};

bool check_signature(const ByteBuffer& sig_with_type, const ByteBuffer& pubkey, ByteSpan script_code,
                     const ScriptContext& ctx) {
  if (sig_with_type.empty() || ctx.tx == nullptr) return false;
  uint8_t type = sig_with_type.back();
  ByteSpan der(sig_with_type.data(), sig_with_type.size() - 1);
  auto sig = ecdsa_der_decode(der);
  if (!sig.ok()) return false;

  auto digest = ctx.witness
                    ? sighash_witness_v0(*ctx.tx, ctx.input_index, script_code, ctx.amount, type)
                    : sighash_legacy(*ctx.tx, ctx.input_index, script_code, type);
  return ecdsa_verify(ByteSpan(pubkey.data(), pubkey.size()), digest.data(), *sig);
}

Result<bool> run(Stack& stack, ByteSpan script, const ScriptContext& ctx) {
  Reader r{script, 0, false};
  std::vector<bool> conditions;
  Stack alt;

  auto executing = [&]() {
    for (bool c : conditions) {
      if (!c) return false;
    }
    return true;
  };

  auto pop = [&](ByteBuffer& out) {
    if (stack.empty()) return false;
    out = stack.back();
    stack.pop_back();
    return true;
  };

  while (!r.done()) {
    uint8_t op = r.byte();
    if (r.fault) return Result<bool>(Status::Invalid, "script ended mid-opcode");

    if (op <= 0x4e) {
      ByteBuffer data;
      size_t len = 0;
      if (op < 0x4c) {
        len = op;
      } else if (op == 0x4c) {
        len = r.byte();
      } else if (op == 0x4d) {
        len = (size_t)r.byte();
        len |= (size_t)r.byte() << 8;
      } else {
        len = 0;
        for (int i = 0; i < 4; ++i) len |= (size_t)r.byte() << (8 * i);
      }
      if (r.fault || !r.take(len, data)) return Result<bool>(Status::Invalid, "push runs past script end");
      if (executing()) stack.push_back(data);
      continue;
    }

    if (op == 0x63 || op == 0x64) {   // OP_IF / OP_NOTIF
      bool value = false;
      if (executing()) {
        ByteBuffer top;
        if (!pop(top)) return Result<bool>(Status::Invalid, "OP_IF with empty stack");
        value = truth_of(top);
        if (op == 0x64) value = !value;
      }
      conditions.push_back(value);
      continue;
    }
    if (op == 0x67) {   // OP_ELSE
      if (conditions.empty()) return Result<bool>(Status::Invalid, "OP_ELSE without OP_IF");
      conditions.back() = !conditions.back();
      continue;
    }
    if (op == 0x68) {   // OP_ENDIF
      if (conditions.empty()) return Result<bool>(Status::Invalid, "OP_ENDIF without OP_IF");
      conditions.pop_back();
      continue;
    }
    if (!executing()) continue;

    switch (op) {
      case 0x4f: stack.push_back(num_to_bytes(-1)); break;
      case 0x61: break;   // OP_NOP
      case 0x69: {        // OP_VERIFY
        ByteBuffer top;
        if (!pop(top)) return Result<bool>(Status::Invalid, "OP_VERIFY with empty stack");
        if (!truth_of(top)) return false;
        break;
      }
      case 0x6a: return false;   // OP_RETURN
      case 0x6b: {               // OP_TOALTSTACK
        ByteBuffer top;
        if (!pop(top)) return Result<bool>(Status::Invalid, "OP_TOALTSTACK with empty stack");
        alt.push_back(top);
        break;
      }
      case 0x6c: {   // OP_FROMALTSTACK
        if (alt.empty()) return Result<bool>(Status::Invalid, "OP_FROMALTSTACK with empty alt stack");
        stack.push_back(alt.back());
        alt.pop_back();
        break;
      }
      case 0x6d: {   // OP_2DROP
        if (stack.size() < 2) return Result<bool>(Status::Invalid, "OP_2DROP needs two items");
        stack.pop_back();
        stack.pop_back();
        break;
      }
      case 0x6e: {   // OP_2DUP
        if (stack.size() < 2) return Result<bool>(Status::Invalid, "OP_2DUP needs two items");
        stack.push_back(stack[stack.size() - 2]);
        stack.push_back(stack[stack.size() - 2]);
        break;
      }
      case 0x75: {   // OP_DROP
        if (stack.empty()) return Result<bool>(Status::Invalid, "OP_DROP with empty stack");
        stack.pop_back();
        break;
      }
      case 0x76: {   // OP_DUP
        if (stack.empty()) return Result<bool>(Status::Invalid, "OP_DUP with empty stack");
        stack.push_back(stack.back());
        break;
      }
      case 0x77: {   // OP_NIP
        if (stack.size() < 2) return Result<bool>(Status::Invalid, "OP_NIP needs two items");
        stack.erase(stack.end() - 2);
        break;
      }
      case 0x78: {   // OP_OVER
        if (stack.size() < 2) return Result<bool>(Status::Invalid, "OP_OVER needs two items");
        stack.push_back(stack[stack.size() - 2]);
        break;
      }
      case 0x7c: {   // OP_SWAP
        if (stack.size() < 2) return Result<bool>(Status::Invalid, "OP_SWAP needs two items");
        std::swap(stack[stack.size() - 1], stack[stack.size() - 2]);
        break;
      }
      case 0x82: {   // OP_SIZE
        if (stack.empty()) return Result<bool>(Status::Invalid, "OP_SIZE with empty stack");
        stack.push_back(num_to_bytes((int64_t)stack.back().size()));
        break;
      }
      case 0x87:     // OP_EQUAL
      case 0x88: {   // OP_EQUALVERIFY
        ByteBuffer a, b;
        if (!pop(a) || !pop(b)) return Result<bool>(Status::Invalid, "OP_EQUAL needs two items");
        bool same = (a == b);
        if (op == 0x88) {
          if (!same) return false;
        } else {
          stack.push_back(same ? num_to_bytes(1) : ByteBuffer{});
        }
        break;
      }
      case 0x9c:     // OP_NUMEQUAL
      case 0x9d: {   // OP_NUMEQUALVERIFY
        ByteBuffer a, b;
        if (!pop(a) || !pop(b)) return Result<bool>(Status::Invalid, "OP_NUMEQUAL needs two items");
        int64_t x = 0, y = 0;
        if (!bytes_to_num(a, x) || !bytes_to_num(b, y)) {
          return Result<bool>(Status::Invalid, "OP_NUMEQUAL operand is not a number");
        }
        bool same = (x == y);
        if (op == 0x9d) {
          if (!same) return false;
        } else {
          stack.push_back(same ? num_to_bytes(1) : ByteBuffer{});
        }
        break;
      }
      case 0xa6: {   // OP_RIPEMD160
        ByteBuffer top;
        if (!pop(top)) return Result<bool>(Status::Invalid, "OP_RIPEMD160 with empty stack");
        auto h = ripemd160(ByteSpan(top.data(), top.size()));
        stack.push_back(ByteBuffer(h.begin(), h.end()));
        break;
      }
      case 0xa8: {   // OP_SHA256
        ByteBuffer top;
        if (!pop(top)) return Result<bool>(Status::Invalid, "OP_SHA256 with empty stack");
        auto h = sha256(ByteSpan(top.data(), top.size()));
        stack.push_back(ByteBuffer(h.begin(), h.end()));
        break;
      }
      case 0xa9: {   // OP_HASH160
        ByteBuffer top;
        if (!pop(top)) return Result<bool>(Status::Invalid, "OP_HASH160 with empty stack");
        auto h = hash160(ByteSpan(top.data(), top.size()));
        stack.push_back(ByteBuffer(h.begin(), h.end()));
        break;
      }
      case 0xaa: {   // OP_HASH256
        ByteBuffer top;
        if (!pop(top)) return Result<bool>(Status::Invalid, "OP_HASH256 with empty stack");
        auto h = sha256d(ByteSpan(top.data(), top.size()));
        stack.push_back(ByteBuffer(h.begin(), h.end()));
        break;
      }
      case 0xac:     // OP_CHECKSIG
      case 0xad: {   // OP_CHECKSIGVERIFY
        ByteBuffer pubkey, sig;
        if (!pop(pubkey) || !pop(sig)) return Result<bool>(Status::Invalid, "OP_CHECKSIG needs two items");
        bool valid = check_signature(sig, pubkey, script, ctx);
        if (op == 0xad) {
          if (!valid) return false;
        } else {
          stack.push_back(valid ? num_to_bytes(1) : ByteBuffer{});
        }
        break;
      }
      case 0xb1: {   // OP_CHECKLOCKTIMEVERIFY
        if (stack.empty()) return Result<bool>(Status::Invalid, "OP_CLTV with empty stack");
        int64_t required = 0;
        if (!bytes_to_num(stack.back(), required, 5)) {
          return Result<bool>(Status::Invalid, "OP_CLTV operand is not a number");
        }
        if (required < 0) return false;
        if (ctx.tx == nullptr) return false;
        bool same_domain = ((uint64_t)required < 500000000ull) == (ctx.tx->lock_time < 500000000u);
        if (!same_domain) return false;
        if ((uint64_t)required > ctx.tx->lock_time) return false;
        if (ctx.input_index < ctx.tx->inputs.size() &&
            ctx.tx->inputs[ctx.input_index].sequence == 0xffffffffu) {
          return false;
        }
        break;
      }
      case 0xb2: break;   // OP_CHECKSEQUENCEVERIFY: relative locks are not modelled here
      default: {
        if (op >= 0x51 && op <= 0x60) {
          stack.push_back(num_to_bytes(op - 0x50));
          break;
        }
        return Result<bool>(Status::Unsupported, "opcode not implemented by this interpreter");
      }
    }
  }

  if (!conditions.empty()) return Result<bool>(Status::Invalid, "unbalanced conditional");
  if (stack.empty()) return false;
  return truth_of(stack.back());
}

}  // namespace

Result<bool> evaluate_script_stack(const std::vector<ByteBuffer>& initial_stack, ByteSpan script,
                                   const ScriptContext& ctx) {
  Stack stack = initial_stack;
  return run(stack, script, ctx);
}

Result<bool> evaluate_script(ByteSpan script_sig, ByteSpan script_pubkey, const ScriptContext& ctx) {
  Stack stack;
  // The unlocking script only has to leave its items on the stack; whether its
  // own top item is falsey says nothing, so only a hard fault stops us here.
  auto pushed = run(stack, script_sig, ctx);
  if (!pushed.ok()) return pushed;
  return run(stack, script_pubkey, ctx);
}

}  // namespace aios::userland::chain
