// crypto/signed_chain held to what it promises: the RFC 8032 key, signatures that verify only for
// their own bytes and key, and a chain that reports a dropped, inserted, reordered or edited record.
//
//   aios-signed-chain-check
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "aios/crypto/signed_chain.hpp"

using namespace aios::crypto;

static int failures = 0, checks = 0;
static void check(bool ok, const std::string& what) {
  ++checks;
  if (!ok) ++failures;
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
}

int main() {
  // RFC 8032 section 7.1, test 1.
  auto seed = Ed25519Signer::seed_from_hex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
  Ed25519Signer rfc;
  check(seed.ok() && rfc.open_seed(*seed).ok() && rfc.public_hex() == "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
        "RFC 8032 test 1: public key from seed");
  check(rfc.sign_hex("") ==
            "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
        "RFC 8032 test 1: signature of the empty message");
  check(!Ed25519Signer::seed_from_hex("abc").ok() && !Ed25519Signer::seed_from_hex(std::string(64, 'z')).ok(), "a malformed seed is refused");

  const auto dir = std::filesystem::temp_directory_path() / ("aios-signed-chain-" + std::to_string(::getpid()));
  std::filesystem::create_directories(dir);
  Ed25519Signer a, again, other;
  check(a.open((dir / "key").string()).ok() && again.open((dir / "key").string()).ok() && a.public_hex() == again.public_hex(),
        "a key file is created once and loads to the same key");
  check(((std::filesystem::status(dir / "key").permissions() & std::filesystem::perms::group_all) == std::filesystem::perms::none),
        "the key file is private to its owner");
  check(other.open((dir / "other").string()).ok() && other.public_hex() != a.public_hex(), "another key file is another key");

  auto make = [&](uint64_t seq, const std::string& prev, const std::string& what) {
    std::string r = "{\"seq\":" + std::to_string(seq) + ",\"prev\":\"" + prev + "\",\"what\":\"" + what + "\"}";
    return ChainLink{r, a.sign_hex(r)};
  };
  std::vector<ChainLink> chain;
  std::string prev;
  for (int i = 1; i <= 4; ++i) {
    chain.push_back(make(i, prev, "landing " + std::to_string(i)));
    prev = sha256_hex(chain.back().record);
  }
  check(verify_chain(chain, a.public_hex()).ok(), "an intact chain verifies");
  check(!verify_chain(chain, other.public_hex()).ok(), "it does not verify under another key");

  auto dropped = chain; dropped.erase(dropped.begin() + 1);
  check(!verify_chain(dropped, a.public_hex()).ok(), "a dropped record is reported");
  auto swapped = chain; std::swap(swapped[1], swapped[2]);
  check(!verify_chain(swapped, a.public_hex()).ok(), "a reordered record is reported");
  auto edited = chain; edited[2].record.replace(edited[2].record.find("landing 3"), 9, "landing X");
  check(!verify_chain(edited, a.public_hex()).ok(), "an edited record is reported");
  auto inserted = chain; inserted.insert(inserted.begin() + 2, make(3, sha256_hex(chain[1].record), "forged"));
  check(!verify_chain(inserted, a.public_hex()).ok(), "an inserted record is reported");
  auto tail = std::vector<ChainLink>(chain.begin() + 2, chain.end());
  check(verify_chain(tail, a.public_hex(), 3, sha256_hex(chain[1].record)).ok(), "a chain can be checked from a known point");
  check(!ed25519_verify_hex(chain[0].record, "zz", a.public_hex()), "a malformed signature does not verify");

  std::filesystem::remove_all(dir);
  std::printf("%d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
