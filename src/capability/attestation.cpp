#include "aios/capability/attestation.hpp"
#include "aios/crypto/nistp.hpp"
#include "aios/crypto/sha256.hpp"
#include "aios/hal/sync.hpp"
#include "aios/hal/tpm.hpp"

#include <cstring>

namespace aios::capability {
namespace {

// TPM 2.0, as the specification defines it. Nothing here is approximated:
// these are the tags, command codes and structure layouts a TPM answers to.
constexpr uint16_t TPM_ST_NO_SESSIONS = 0x8001;
constexpr uint16_t TPM_ST_SESSIONS    = 0x8002;
constexpr uint16_t TPM_ST_ATTEST_QUOTE = 0x8018;

constexpr uint32_t CC_Startup        = 0x00000144;
constexpr uint32_t CC_GetCapability  = 0x0000017A;
constexpr uint32_t CC_GetRandom      = 0x0000017B;
constexpr uint32_t CC_PCR_Read       = 0x0000017E;
constexpr uint32_t CC_CreatePrimary  = 0x00000131;
constexpr uint32_t CC_Quote          = 0x00000158;
constexpr uint32_t CC_FlushContext   = 0x00000165;

constexpr uint32_t TPM_RH_ENDORSEMENT = 0x4000000B;
constexpr uint32_t TPM_RH_OWNER       = 0x40000001;
constexpr uint32_t TPM_RS_PW          = 0x40000009;

constexpr uint16_t TPM_ALG_SHA256 = 0x000B;
constexpr uint16_t TPM_ALG_ECC    = 0x0023;
constexpr uint16_t TPM_ALG_NULL   = 0x0010;
constexpr uint16_t TPM_ALG_ECDSA  = 0x0018;
constexpr uint16_t TPM_ECC_NIST_P256 = 0x0003;

constexpr uint32_t TPM_CAP_TPM_PROPERTIES = 0x00000006;
constexpr uint32_t PT_MANUFACTURER   = 0x00000105;
constexpr uint32_t PT_FIRMWARE_V1    = 0x0000010B;

constexpr uint32_t TPM_GENERATED_VALUE = 0xFF544347;   // "\xFFTCG"

struct Buf {
  std::vector<uint8_t> b;
  void u8(uint8_t v) { b.push_back(v); }
  void u16(uint16_t v) { b.push_back((uint8_t)(v >> 8)); b.push_back((uint8_t)v); }
  void u32(uint32_t v) { u16((uint16_t)(v >> 16)); u16((uint16_t)v); }
  void raw(const uint8_t* p, size_t n) { b.insert(b.end(), p, p + n); }
  void raw(const std::vector<uint8_t>& v) { raw(v.data(), v.size()); }
  // A TPM2B is a two-byte length and then that many bytes.
  void b2(const std::vector<uint8_t>& v) { u16((uint16_t)v.size()); raw(v); }
};

struct Reader {
  const uint8_t* p;
  size_t n;
  size_t at = 0;
  bool bad = false;
  bool need(size_t k) { if (at + k > n) { bad = true; return false; } return true; }
  uint8_t u8() { if (!need(1)) return 0; return p[at++]; }
  uint16_t u16() { if (!need(2)) return 0; uint16_t v = (uint16_t)((p[at] << 8) | p[at + 1]); at += 2; return v; }
  uint32_t u32() { uint32_t hi = u16(); uint32_t lo = u16(); return (hi << 16) | lo; }
  uint64_t u64() { uint64_t hi = u32(); uint64_t lo = u32(); return (hi << 32) | lo; }
  std::vector<uint8_t> raw(size_t k) {
    if (!need(k)) return {};
    std::vector<uint8_t> v(p + at, p + at + k);
    at += k;
    return v;
  }
  std::vector<uint8_t> b2() { uint16_t k = u16(); return raw(k); }
};

std::vector<uint8_t> frame(uint16_t tag, uint32_t code, const std::vector<uint8_t>& body) {
  Buf out;
  out.u16(tag);
  out.u32((uint32_t)(10 + body.size()));
  out.u32(code);
  out.raw(body);
  return out.b;
}

// A password session with an empty password, which is the authorisation the
// endorsement and owner hierarchies carry on a machine nobody has taken
// ownership of. Sent as the authorization area of a session command.
std::vector<uint8_t> empty_password_session() {
  Buf s;
  s.u32(TPM_RS_PW);
  s.b2({});          // nonce
  s.u8(0);           // sessionAttributes
  s.b2({});          // hmac / password
  Buf area;
  area.u32((uint32_t)s.b.size());
  area.raw(s.b);
  return area.b;
}

struct Response {
  uint32_t code = 0;
  std::vector<uint8_t> body;   // everything after the 10-byte header
};

// The warnings that mean "ask me again": the TPM could not start the command,
// yielded to something else, or is running its self-tests. A client that does
// not retry these reports a failure the TPM never had — TPM2_Quote came back
// TPM_RC_RETRY (0x922) on the first real run of this.
constexpr uint32_t RC_YIELDED = 0x908;
constexpr uint32_t RC_TESTING = 0x90A;
constexpr uint32_t RC_RETRY   = 0x922;
constexpr uint32_t RC_NV_UNAVAILABLE = 0x923;

bool worth_retrying(uint32_t rc) {
  return rc == RC_RETRY || rc == RC_YIELDED || rc == RC_TESTING || rc == RC_NV_UNAVAILABLE;
}

Result<Response> call(uint16_t tag, uint32_t cc, const std::vector<uint8_t>& body, uint32_t timeout_ms = 30000) {
  auto command = frame(tag, cc, body);
  for (int attempt = 0; ; ++attempt) {
    auto answer = hal::tpm_transmit(command, timeout_ms);
    if (!answer.ok()) return answer.error();
    const auto& r = *answer;
    if (r.size() < 10) return Result<Response>(Status::Corrupt, "the TPM returned a short response");
    Response out;
    out.code = ((uint32_t)r[6] << 24) | ((uint32_t)r[7] << 16) | ((uint32_t)r[8] << 8) | r[9];
    out.body.assign(r.begin() + 10, r.end());
    if (!worth_retrying(out.code) || attempt >= 20) return out;
    hal::sleep_ms(50);
  }
}

std::string rc_text(uint32_t rc) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "TPM_RC 0x%08X", rc);
  return buf;
}

// TPML_PCR_SELECTION over the SHA-256 bank for the indices asked for.
std::vector<uint8_t> pcr_selection(const std::vector<uint32_t>& indices) {
  uint8_t mask[3] = {0, 0, 0};
  for (uint32_t i : indices) if (i < 24) mask[i / 8] |= (uint8_t)(1u << (i % 8));
  Buf s;
  s.u32(1);                 // one selection
  s.u16(TPM_ALG_SHA256);
  s.u8(3);                  // three bytes of bitmap
  s.raw(mask, 3);
  return s.b;
}

// The template for the attestation key: a restricted ECDSA P-256 signing key,
// primary in its hierarchy, non-duplicable. This is the shape a TPM will
// accept for TPM2_Quote.
std::vector<uint8_t> ak_template() {
  Buf t;
  t.u16(TPM_ALG_ECC);
  t.u16(TPM_ALG_SHA256);                 // nameAlg
  // objectAttributes: fixedTPM | fixedParent | sensitiveDataOrigin |
  // userWithAuth | restricted | sign
  t.u32(0x00000002 | 0x00000010 | 0x00000020 | 0x00000040 | 0x00010000 | 0x00040000);
  t.b2({});                              // authPolicy: none
  // TPMS_ECC_PARMS
  t.u16(TPM_ALG_NULL);                   // symmetric: none (this key only signs)
  t.u16(TPM_ALG_ECDSA);                  // scheme
  t.u16(TPM_ALG_SHA256);                 // scheme hash
  t.u16(TPM_ECC_NIST_P256);              // curveID
  t.u16(TPM_ALG_NULL);                   // kdf
  // TPMS_ECC_POINT: empty for a key the TPM is to create
  t.b2({});
  t.b2({});
  Buf outer;
  outer.b2(t.b);                         // the whole thing is a TPM2B_PUBLIC
  return outer.b;
}

}  // namespace

HardwareAttestor::HardwareAttestor() { (void)attach(); }
HardwareAttestor::~HardwareAttestor() = default;

Result<void> HardwareAttestor::attach() {
  if (available_) return Ok();
  auto opened = hal::tpm_open();
  if (!opened.ok()) { why_not_ = opened.error().message; return opened.error(); }
  where_ = *opened;

  // A software TPM comes up un-started; a kernel-managed device is already
  // running and answers INITIALIZE, which is not an error here.
  {
    Buf b;
    b.u16(0x0000);   // TPM_SU_CLEAR
    (void)call(TPM_ST_NO_SESSIONS, CC_Startup, b.b, 5000);
  }

  auto ask = [&](uint32_t property, uint32_t& value) -> bool {
    Buf b;
    b.u32(TPM_CAP_TPM_PROPERTIES);
    b.u32(property);
    b.u32(1);
    auto r = call(TPM_ST_NO_SESSIONS, CC_GetCapability, b.b, 5000);
    if (!r.ok() || r->code != 0) return false;
    Reader rd{r->body.data(), r->body.size()};
    rd.u8();                       // moreData
    rd.u32();                      // capability
    uint32_t count = rd.u32();
    if (rd.bad || count == 0) return false;
    rd.u32();                      // property id
    value = rd.u32();
    return !rd.bad;
  };

  uint32_t mfr = 0, fw = 0;
  if (!ask(PT_MANUFACTURER, mfr)) {
    why_not_ = "something answered at " + where_ + " but it is not a TPM 2.0";
    hal::tpm_close();
    return Result<void>(Status::Unsupported, why_not_);
  }
  char name[5] = {0};
  name[0] = (char)((mfr >> 24) & 0xFF);
  name[1] = (char)((mfr >> 16) & 0xFF);
  name[2] = (char)((mfr >> 8) & 0xFF);
  name[3] = (char)(mfr & 0xFF);
  for (int i = 0; i < 4; ++i) if (name[i] < 32 || name[i] > 126) name[i] = '?';
  manufacturer_ = name;
  if (ask(PT_FIRMWARE_V1, fw)) {
    firmware_ = std::to_string(fw >> 16) + "." + std::to_string(fw & 0xFFFF);
  }
  available_ = true;
  return Ok();
}

std::string HardwareAttestor::describe_root_of_trust() const {
  if (!available_) return why_not_.empty() ? "no TPM on this machine" : why_not_;
  return "TPM 2.0 at " + where_ + ", manufacturer " + manufacturer_ +
         (firmware_.empty() ? "" : ", firmware " + firmware_);
}

Result<std::vector<uint8_t>> HardwareAttestor::random(uint16_t bytes) {
  if (!available_) return Result<std::vector<uint8_t>>(Status::Unsupported, describe_root_of_trust());
  if (bytes == 0 || bytes > 48) return Result<std::vector<uint8_t>>(Status::Invalid, "a TPM returns at most 48 bytes at once");
  Buf b;
  b.u16(bytes);
  auto r = call(TPM_ST_NO_SESSIONS, CC_GetRandom, b.b);
  if (!r.ok()) return r.error();
  if (r->code != 0) return Result<std::vector<uint8_t>>(Status::IoError, "TPM2_GetRandom: " + rc_text(r->code));
  Reader rd{r->body.data(), r->body.size()};
  auto out = rd.b2();
  if (rd.bad) return Result<std::vector<uint8_t>>(Status::Corrupt, "malformed TPM2_GetRandom response");
  return out;
}

Result<std::vector<PcrValue>> HardwareAttestor::read_pcrs(const std::vector<uint32_t>& indices) {
  if (!available_) return Result<std::vector<PcrValue>>(Status::Unsupported, describe_root_of_trust());
  if (indices.empty()) return std::vector<PcrValue>{};

  std::vector<PcrValue> out;
  // A TPM answers TPM2_PCR_Read with as many values as it feels like returning
  // and tells you which ones in the selection it echoes back, so this loops
  // until every index asked for has been read.
  std::vector<uint32_t> pending = indices;
  for (int round = 0; round < 8 && !pending.empty(); ++round) {
    auto r = call(TPM_ST_NO_SESSIONS, CC_PCR_Read, pcr_selection(pending));
    if (!r.ok()) return r.error();
    if (r->code != 0) return Result<std::vector<PcrValue>>(Status::IoError, "TPM2_PCR_Read: " + rc_text(r->code));

    Reader rd{r->body.data(), r->body.size()};
    rd.u32();                       // pcrUpdateCounter
    uint32_t sel_count = rd.u32();
    std::vector<uint32_t> answered;
    for (uint32_t i = 0; i < sel_count && !rd.bad; ++i) {
      uint16_t alg = rd.u16();
      uint8_t size_of_select = rd.u8();
      auto mask = rd.raw(size_of_select);
      if (alg != TPM_ALG_SHA256) continue;
      for (size_t byte = 0; byte < mask.size(); ++byte) {
        for (int bit = 0; bit < 8; ++bit) {
          if (mask[byte] & (1u << bit)) answered.push_back((uint32_t)(byte * 8 + bit));
        }
      }
    }
    uint32_t digest_count = rd.u32();
    for (uint32_t i = 0; i < digest_count && !rd.bad; ++i) {
      auto d = rd.b2();
      if (i >= answered.size() || d.size() != 32) continue;
      PcrValue v;
      v.index = answered[i];
      std::memcpy(v.digest.data(), d.data(), 32);
      out.push_back(v);
    }
    if (rd.bad) return Result<std::vector<PcrValue>>(Status::Corrupt, "malformed TPM2_PCR_Read response");
    if (answered.empty()) break;

    std::vector<uint32_t> still;
    for (uint32_t want : pending) {
      bool got = false;
      for (const auto& v : out) if (v.index == want) { got = true; break; }
      if (!got) still.push_back(want);
    }
    if (still.size() == pending.size()) break;   // no progress; stop rather than spin
    pending = still;
  }
  return out;
}

Result<TPMQuote> HardwareAttestor::generate_tpm2_quote(const std::vector<uint32_t>& pcr_indices,
                                                       const std::vector<uint8_t>& nonce) {
  if (!available_) return Result<TPMQuote>(Status::Unsupported, describe_root_of_trust());

  auto pcrs = read_pcrs(pcr_indices);
  if (!pcrs.ok()) return pcrs.error();

  // The attestation key. Created in the endorsement hierarchy, restricted and
  // non-duplicable, so the private half never leaves the chip.
  uint32_t ak_handle = 0;
  std::vector<uint8_t> ak_x, ak_y;
  {
    Buf b;
    b.u32(TPM_RH_ENDORSEMENT);
    auto sess = empty_password_session();
    b.raw(sess);
    // TPM2B_SENSITIVE_CREATE: no auth value, no data — the TPM makes the key.
    Buf sens;
    sens.b2({});
    sens.b2({});
    b.b2(sens.b);
    b.raw(ak_template());
    b.b2({});                    // outsideInfo
    b.u32(0);                    // creationPCR: none

    auto r = call(TPM_ST_SESSIONS, CC_CreatePrimary, b.b, 60000);
    if (!r.ok()) return r.error();
    if (r->code != 0) {
      return Result<TPMQuote>(Status::Denied,
          "TPM2_CreatePrimary in the endorsement hierarchy: " + rc_text(r->code));
    }
    Reader rd{r->body.data(), r->body.size()};
    ak_handle = rd.u32();
    rd.u32();                    // parameterSize (this is a session response)
    auto pub = rd.b2();
    if (rd.bad || pub.empty()) return Result<TPMQuote>(Status::Corrupt, "malformed TPM2_CreatePrimary response");
    // Walk the TPMT_PUBLIC to its TPMS_ECC_POINT.
    Reader pr{pub.data(), pub.size()};
    pr.u16();                    // type
    pr.u16();                    // nameAlg
    pr.u32();                    // objectAttributes
    pr.b2();                     // authPolicy
    pr.u16(); pr.u16(); pr.u16(); pr.u16(); pr.u16();   // symmetric, scheme, hash, curve, kdf
    ak_x = pr.b2();
    ak_y = pr.b2();
    if (pr.bad || ak_x.size() != 32 || ak_y.size() != 32) {
      Buf f; f.u32(ak_handle);
      (void)call(TPM_ST_NO_SESSIONS, CC_FlushContext, f.b);
      return Result<TPMQuote>(Status::Corrupt, "the attestation key is not a P-256 point");
    }
  }

  TPMQuote quote;
  quote.nonce = nonce;
  {
    Buf b;
    b.u32(ak_handle);
    auto sess = empty_password_session();
    b.raw(sess);
    b.b2(nonce);                 // qualifyingData: the caller's freshness
    b.u16(TPM_ALG_NULL);         // inScheme: the key's own scheme
    b.raw(pcr_selection(pcr_indices));

    auto r = call(TPM_ST_SESSIONS, CC_Quote, b.b, 60000);
    Buf f; f.u32(ak_handle);
    (void)call(TPM_ST_NO_SESSIONS, CC_FlushContext, f.b);
    if (!r.ok()) return r.error();
    if (r->code != 0) return Result<TPMQuote>(Status::IoError, "TPM2_Quote: " + rc_text(r->code));

    Reader rd{r->body.data(), r->body.size()};
    rd.u32();                    // parameterSize
    quote.attested = rd.b2();    // TPM2B_ATTEST — signed byte for byte
    quote.algorithm = rd.u16();  // TPMT_SIGNATURE.sigAlg
    if (quote.algorithm != TPM_ALG_ECDSA) {
      return Result<TPMQuote>(Status::Unsupported, "the TPM signed with an algorithm this machine cannot check");
    }
    rd.u16();                    // hash
    auto r_part = rd.b2();
    auto s_part = rd.b2();
    if (rd.bad) return Result<TPMQuote>(Status::Corrupt, "malformed TPM2_Quote response");
    // ECDSA integers come back minimally encoded; pad to the curve width.
    quote.signature.assign(64, 0);
    std::memcpy(quote.signature.data() + (32 - std::min<size_t>(32, r_part.size())), r_part.data(),
                std::min<size_t>(32, r_part.size()));
    std::memcpy(quote.signature.data() + 32 + (32 - std::min<size_t>(32, s_part.size())), s_part.data(),
                std::min<size_t>(32, s_part.size()));
  }

  quote.signer_public.push_back(0x04);
  quote.signer_public.insert(quote.signer_public.end(), ak_x.begin(), ak_x.end());
  quote.signer_public.insert(quote.signer_public.end(), ak_y.begin(), ak_y.end());
  quote.pcrs = *pcrs;

  // Parse the attestation the TPM signed, so the caller can see what it says
  // without re-deriving the structure.
  {
    Reader rd{quote.attested.data(), quote.attested.size()};
    uint32_t magic = rd.u32();
    uint16_t type = rd.u16();
    rd.b2();                     // qualifiedSigner
    auto extra = rd.b2();        // extraData: our nonce, echoed
    quote.clock = rd.u64();
    rd.u32();                    // resetCount
    quote.reset_count = 0;
    rd.u32();                    // restartCount
    rd.u8();                     // safe
    rd.u64();                    // firmwareVersion
    rd.raw(0);
    // TPMS_QUOTE_INFO: the PCR selection, then the digest over them.
    uint32_t sel_count = rd.u32();
    for (uint32_t i = 0; i < sel_count && !rd.bad; ++i) {
      rd.u16();
      uint8_t sz = rd.u8();
      rd.raw(sz);
    }
    auto digest = rd.b2();
    if (magic != TPM_GENERATED_VALUE || type != TPM_ST_ATTEST_QUOTE) {
      return Result<TPMQuote>(Status::Corrupt, "the attestation is not a TPM-generated quote");
    }
    if (digest.size() == 32) std::memcpy(quote.pcr_digest.data(), digest.data(), 32);
    if (!extra.empty()) quote.nonce = extra;
  }

  auto checked = verify_quote(quote, nonce);
  quote.verified = checked.ok() && *checked;
  return quote;
}

Result<bool> HardwareAttestor::verify_quote(const TPMQuote& quote, const std::vector<uint8_t>& expected_nonce) {
  return verify_quote_standalone(quote, expected_nonce);
}

std::vector<uint8_t> HardwareAttestor::signed_pcr_digest(const TPMQuote& quote) {
  Reader rd{quote.attested.data(), quote.attested.size()};
  rd.u32(); rd.u16(); rd.b2(); rd.b2();
  rd.u64(); rd.u32(); rd.u32(); rd.u8(); rd.u64();
  uint32_t n = rd.u32();
  for (uint32_t i = 0; i < n && !rd.bad; ++i) { rd.u16(); uint8_t sz = rd.u8(); rd.raw(sz); }
  auto d = rd.b2();
  if (rd.bad || d.size() != 32) return {};
  return std::vector<uint8_t>(d.begin(), d.end());
}

Result<bool> HardwareAttestor::verify_quote_standalone(const TPMQuote& quote, const std::vector<uint8_t>& expected_nonce) {
  if (quote.attested.size() < 20) return Result<bool>(Status::Invalid, "there is no attestation to check");
  if (quote.signature.size() != 64) return Result<bool>(Status::Invalid, "an ECDSA P-256 signature is 64 bytes");
  if (quote.signer_public.size() != 65 || quote.signer_public[0] != 0x04) {
    return Result<bool>(Status::Invalid, "the signer's key is not an uncompressed P-256 point");
  }

  Reader rd{quote.attested.data(), quote.attested.size()};
  if (rd.u32() != TPM_GENERATED_VALUE) return Result<bool>(Status::Corrupt, "the attestation was not generated by a TPM");
  if (rd.u16() != TPM_ST_ATTEST_QUOTE) return Result<bool>(Status::Corrupt, "the attestation is not a quote");
  rd.b2();
  auto extra = rd.b2();
  if (!expected_nonce.empty() && extra != expected_nonce) {
    return Result<bool>(Status::Conflict, "the quote does not carry the nonce it was asked for: it is a replay");
  }

  // The PCR digest inside the signed structure must be the digest of the PCRs
  // reported beside it — otherwise the quote is real and the values are not.
  // It is read from the signed bytes, never from a field the sender filled in.
  std::array<uint8_t, 32> signed_digest{};
  {
    rd.u64();                    // clock
    rd.u32();                    // resetCount
    rd.u32();                    // restartCount
    rd.u8();                     // safe
    rd.u64();                    // firmwareVersion
    uint32_t sel_count = rd.u32();
    for (uint32_t i = 0; i < sel_count && !rd.bad; ++i) { rd.u16(); uint8_t sz = rd.u8(); rd.raw(sz); }
    auto d = rd.b2();
    if (rd.bad || d.size() != 32) return Result<bool>(Status::Corrupt, "the attestation carries no PCR digest");
    std::memcpy(signed_digest.data(), d.data(), 32);
  }
  if (!quote.pcrs.empty()) {
    std::vector<uint8_t> concat;
    for (const auto& p : quote.pcrs) concat.insert(concat.end(), p.digest.begin(), p.digest.end());
    auto recomputed = crypto::sha256(ByteSpan(concat.data(), concat.size()));
    if (std::memcmp(recomputed.data(), signed_digest.data(), 32) != 0) {
      return Result<bool>(Status::Conflict,
          "the PCR values do not hash to the digest the TPM signed");
    }
  }

  if (!crypto::nist_point_valid(crypto::NistCurve::P256,
                                ByteSpan(quote.signer_public.data(), quote.signer_public.size()))) {
    return Result<bool>(Status::Invalid, "the signer's key is not on the curve");
  }
  auto digest = crypto::sha256(ByteSpan(quote.attested.data(), quote.attested.size()));
  bool ok = crypto::ecdsa_verify(crypto::NistCurve::P256,
                                 ByteSpan(quote.signer_public.data(), quote.signer_public.size()),
                                 ByteSpan(digest.data(), digest.size()),
                                 ByteSpan(quote.signature.data(), 32),
                                 ByteSpan(quote.signature.data() + 32, 32));
  if (!ok) return Result<bool>(Status::Denied, "the signature over the attestation does not check out");
  return true;
}

}  // namespace aios::capability
