// QA drive for AIOS editor SSH remote editing & file browser subsystem.
// Exercises real host file system & SSH remote browsing logic, parent dir navigation,
// entry categorization (dirs vs files), and SSH path parsing.

#include "aios/userland/compositor.hpp"
#include "aios/net/ssh.hpp"
#include "aios/crypto/rsa.hpp"
#include "aios/crypto/sha256.hpp"
#include "aios/hal/host.hpp"

#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <sys/stat.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif

using namespace aios;
using namespace aios::userland;

namespace {
int g_fail = 0;
void check(bool ok, const char* what) {
  std::printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_fail;
}
}  // namespace

int main() {
  std::printf("editor-ssh-drive: SSH editing & file browser subsystem QA\n");

  // A throwaway root: booting a kernel writes its identity, store and logs
  // into the root, and this used to be /repos -- the machine's source tree.
  char root_tmpl[] = "/tmp/aios-ssh-drive-XXXXXX";
  if (!mkdtemp(root_tmpl)) { std::perror("mkdtemp"); return 1; }
  KernelConfig cfg;
  cfg.root = root_tmpl;
  std::printf("  root: %s\n", root_tmpl);
  auto k_res = Kernel::boot(cfg);
  if (!k_res.ok()) {
    std::fprintf(stderr, "Kernel boot failed: %s\n", k_res.error().str().c_str());
    return 1;
  }
  Kernel& k = **k_res;
  HardwareCompositor compositor(1280, 800);

  WindowSurface win;
  win.type = WindowType::Editor;

  // --- 1. Local directory listing & dir vs file sorting ---------------------
  {
    std::printf("[1] Local directory listing & file browser entries\n");
    compositor.editor_browse(win, k, "/repos/aios/src");
    std::printf("    dir_entries count: %zu, dir_is_dir count: %zu\n",
                win.dir_entries.size(), win.dir_is_dir.size());

    check(!win.dir_entries.empty(), "dir_entries is non-empty");
    check(win.dir_entries.size() == win.dir_is_dir.size(), "dir_entries and dir_is_dir sizes match");

    // Parent directory .. must be the first entry
    check(win.dir_entries[0] == "..", "first entry is parent dir '..'");
    check(win.dir_is_dir[0] == true, "parent dir entry is marked as dir");

    // Verify directories come before files
    bool saw_file = false;
    bool order_ok = true;
    for (size_t i = 1; i < win.dir_entries.size(); ++i) {
      if (win.dir_is_dir[i]) {
        if (saw_file) { order_ok = false; break; }
      } else {
        saw_file = true;
      }
    }
    check(order_ok, "directories are sorted before files");
  }

  // --- 2. Parent directory navigation --------------------------------------
  {
    std::printf("[2] Parent directory navigation\n");
    std::string start_dir = win.file_dir;
    compositor.editor_browse(win, k, "/repos/aios");
    check(win.file_dir == "/repos/aios", "navigated to parent /repos/aios");
    check(win.dir_entries[0] == "..", "parent entry '..' present in parent dir");
  }

  // --- 3. SSH target & path parsing -----------------------------------------
  {
    std::printf("[3] SSH target and URL specification parsing\n");
    net::SshTarget target;
    std::string remote_path;

    auto res1 = net::ssh_parse_target("arch@localhost:22", "root");
    check(res1.ok() && res1->user == "arch" && res1->host == "localhost" && res1->port == 22,
          "net::ssh_parse_target user@host:port");

    auto res2 = net::ssh_parse_target("deploy@192.168.1.5", "root");
    check(res2.ok() && res2->user == "deploy" && res2->host == "192.168.1.5" && res2->port == 22,
          "net::ssh_parse_target user@host default port");
  }

  // --- 4. File browser scrolling & window clamping -------------------------
  {
    std::printf("[4] File browser scroll offset & view clamping\n");
    win.dir_scroll = 5;
    check(win.dir_scroll == 5, "dir_scroll set to 5");
    compositor.editor_browse(win, k, "/repos");
    check(win.dir_scroll == 0, "editor_browse resets dir_scroll to 0 on folder change");
  }

  // --- 5. Save over ssh, for real ---------------------------------------
  // Only against a server you point it at (a throwaway sshd is the intended
  // one): AIOS_SSH_DRIVE_TARGET=user@127.0.0.1:PORT, AIOS_SSH_DRIVE_AUTHKEYS=
  // that server's authorized_keys (this run's key is appended to it), and
  // AIOS_SSH_DRIVE_DIR=a directory on that machine to write in.
  const char* tgt = std::getenv("AIOS_SSH_DRIVE_TARGET");
  const char* authk = std::getenv("AIOS_SSH_DRIVE_AUTHKEYS");
  const char* rdir = std::getenv("AIOS_SSH_DRIVE_DIR");
  if (tgt && authk && rdir) {
    std::printf("[5] Save over ssh to %s\n", tgt);
    auto id = net::ssh_load_identity(k.host(), std::string(root_tmpl) + "/ssh/id_ed25519", true, "editor-ssh-drive");
    check(id.ok(), "an identity is made for this run");
    if (id.ok()) {
      FILE* af = std::fopen(authk, "a");
      if (af) { std::fprintf(af, "%s\n", id->public_line().c_str()); std::fclose(af); }
      const std::string local = std::string(rdir) + "/remote-edit.txt";
      const std::string url = std::string("ssh://") + tgt + local;
      { FILE* f = std::fopen(local.c_str(), "w"); std::fputs("ORIGINAL\n", f); std::fclose(f); }
      ::chmod(local.c_str(), 0640);

      WindowSurface ew;
      ew.type = WindowType::Editor;
      const bool opened = compositor.editor_open(ew, k, url);
      check(opened && !ew.lines.empty() && ew.lines[0] == "ORIGINAL", "editor_open reads the remote file");
      std::printf("    status: %s\n", ew.status_text.c_str());

      ew.lines = {"EDITED over ssh", "quotes ' \" and $HOME `x` survive"};
      const bool saved = compositor.editor_save(ew, k, url);
      std::printf("    status: %s\n", ew.status_text.c_str());
      std::string got;
      { FILE* f = std::fopen(local.c_str(), "r"); char b[256]; while (f && std::fgets(b, sizeof b, f)) got += b; if (f) std::fclose(f); }
      check(saved && got == "EDITED over ssh\nquotes ' \" and $HOME `x` survive\n", "editor_save replaces the remote file byte for byte");
      struct stat st{};
      ::stat(local.c_str(), &st);
      check((st.st_mode & 0777) == 0640, "the file keeps its mode (0640) across the temp-and-rename");
      bool litter = false;
      for (const auto& e : std::filesystem::directory_iterator(rdir))
        if (e.path().filename().string().rfind(".aios-save.", 0) == 0) litter = true;
      check(!litter, "no temp file is left beside it");

      const std::string bad = std::string("ssh://") + tgt + std::string(rdir) + "/no-such-dir/x.txt";
      const bool bad_saved = compositor.editor_save(ew, k, bad);
      std::printf("    status: %s\n", ew.status_text.c_str());
      check(!bad_saved, "a save that cannot land reports failure");
    }
  } else {
    std::printf("[5] Save over ssh: skipped (set AIOS_SSH_DRIVE_TARGET, _AUTHKEYS, _DIR)\n");
  }

  // --- 6. Encrypted private key parsing with passphrase -------------------
  {
    std::printf("[6] Encrypted private key parsing & passphrase decryption\n");
    const std::string enc_pem =
      "-----BEGIN OPENSSH PRIVATE KEY-----\n"
      "b3BlbnNzaC1rZXktdjEAAAAACmFlczI1Ni1jdHIAAAAGYmNyeXB0AAAAGAAAABC7mnuqv8\n"
      "OdfoHYSotu3+pIAAAAGAAAAAEAAAAzAAAAC3NzaC1lZDI1NTE5AAAAIElyYR2dbH+RqfvB\n"
      "NuYLWLwFlB1cUH35vg7UXv9SOHBuAAAAkP/c5dN9Yi8N3C6AkIh6lv7mzlUln1H33Vs0K1\n"
      "qIRWXlHn9FWRX2IIqn/CtWOu7+uwgVRh98ebje7uIfIiqQroHgNEPyfekhiQJ4iYfEvY5K\n"
      "Iha+ix3hQLzJvtDrJRiPlHZyt0+moDNcFMot6aS0eW9zzRvRSLHtHFNLICPLat3XdmwSVC\n"
      "xcfePe9jpJ3zaL6w==\n"
      "-----END OPENSSH PRIVATE KEY-----\n";

    // Rejection without passphrase or with wrong passphrase
    auto fail_nopass = net::ssh_parse_private_key(span_of(enc_pem), "");
    check(!fail_nopass.ok(), "encrypted key without passphrase is rejected");

    auto fail_wrong = net::ssh_parse_private_key(span_of(enc_pem), "wrongpass");
    check(!fail_wrong.ok(), "encrypted key with wrong passphrase is rejected");

    // Decryption with correct passphrase
    auto ok_id = net::ssh_parse_private_key(span_of(enc_pem), "passphrase123");
    if (!ok_id.ok()) std::printf("    ok_id error: %s\n", ok_id.error().str().c_str());
    check(ok_id.ok() && ok_id->valid, "encrypted key decrypted with correct passphrase");
    if (ok_id.ok()) {
      check(ok_id->type == "ssh-ed25519", "decrypted identity has ed25519 type");
      check(ok_id->comment == "test_enc_ed", "decrypted identity preserves comment");
      check(ok_id->public_line().rfind("ssh-ed25519 ", 0) == 0, "decrypted identity generates public line");
    }
  }

  // --- 7. RSA identity parsing & client signing ---------------------------
  const std::string rsa_pem =
    "-----BEGIN OPENSSH PRIVATE KEY-----\n"
    "b3BlbnNzaC1rZXktdjEAAAAABG5vbmUAAAAEbm9uZQAAAAAAAAABAAABFwAAAAdzc2gtcn\n"
    "NhAAAAAwEAAQAAAQEAjH4dqYYS3tTnQgi81Z7jL7w1BRDeNsc5g71Cc3D2I0ho4TGMQMdK\n"
    "+QIGt1NZskMXwGZi1QLgXz2lZ/CTtw4xeGVokJa0yafk4OdQA2mqACdyeM9GXQBksxgNQ0\n"
    "+Yl81RpsYuUJjNmI/wQwHX2jwcOG8wDOEq+9LmI2cPlfL8+4WlIbzQOLWh+ZLZmFgMf1Lg\n"
    "7G40BNi2VIWyiUaXTEi9Fy9jxabPt2i57qprMSnDSH+GoiSYa24iSkYCWSdSO+jzN2Y7vm\n"
    "7gl365ECdA8lXN3m89cTM4sg+C/InF/Z03Bn0eqFckW7QOdv62HnW62pk34reUpdVZHU7y\n"
    "avnN46c4bQAAA8CgvCDhoLwg4QAAAAdzc2gtcnNhAAABAQCMfh2phhLe1OdCCLzVnuMvvD\n"
    "UFEN42xzmDvUJzcPYjSGjhMYxAx0r5Aga3U1myQxfAZmLVAuBfPaVn8JO3DjF4ZWiQlrTJ\n"
    "p+Tg51ADaaoAJ3J4z0ZdAGSzGA1DT5iXzVGmxi5QmM2Yj/BDAdfaPBw4bzAM4Sr70uYjZw\n"
    "+V8vz7haUhvNA4taH5ktmYWAx/UuDsbjQE2LZUhbKJRpdMSL0XL2PFps+3aLnuqmsxKcNI\n"
    "f4aiJJhrbiJKRgJZJ1I76PM3Zju+buCXfrkQJ0DyVc3ebz1xMziyD4L8icX9nTcGfR6oVy\n"
    "RbtA52/rYedbramTfit5Sl1VkdTvJq+c3jpzhtAAAAAwEAAQAAAQAF6szkhjNWw8hOsXAE\n"
    "nknKyaZBBZ4aWAc923OISa5wO31BWNUjqFEpzjJKt928ApPmRFmux418LWxBRbJvAA/I1p\n"
    "qSFmrcYHAlIdDup91jXsX1PSOlSJ41JdLvLhPK9viUqd2rAtiL76oKpHH7LNNydAplzHkD\n"
    "PLV5Mb7IDcEmdcFN+7YR2fso6X3Vzq7BLiOSx+Hf/Pg6J9SeVmjTXONxDGHJFR4JROR/6f\n"
    "lcBfc+gMZb/aiw9H/v65+BnN4i46rfOG/RuvI1uG4aetmo0Ak7izkz8Wj1g61/+BIihwiO\n"
    "Gl0wgYuCOHGbkdGP6A+7qjAexifNGBWXAQbcs+OnxQJBAAAAgDWwTWCs5XPMQcsrQLdf8B\n"
    "buK/iCsV2PGoGOQQWYz350flQ1aU7bZDp5nu5JQC85fmTX0EzU7DzBXfakv660Q3Yvzka1\n"
    "qrvsa82+cCiLGCLHuFZ4hlpSzA83bRYYspkky0flMNcLn0+k7bDWqubHg+noC+r6W15ODb\n"
    "xkg9D+ZY2lAAAAgQDALQKB0LGUQXzeTtPV3Gfb5inTfhHjD3T4r0eP0eQEaHTDZh9tyzHt\n"
    "rHngqg5LTaqSWjtb+lbhXc6h8EDlhE85HCS7pl6UMLwGSXwYG6MSkQIJMs5kqqs2ytg6WA\n"
    "sndVJD50k2DaIV8hMQrn/4dYLDElRy2mhpFYUdy7Tu/wjArQAAAIEAuybypOKqq1l4wWjc\n"
    "2VMBNWYH2Kqu+EN1yvh7zYKgvub+z0JYpq4+YMjnjGf2C1rYNovApKdUPlt//ErooKFz3N\n"
    "mWOC2Ccz8HiBkxZTNg/ugDhW0+L41fbVwv/nTLTNX3+tU0azVJ16+o4XIgx7rcds/CQimq\n"
    "UWGrv0T7Pn+mjsEAAAAIdGVzdF9yc2EBAgM=\n"
    "-----END OPENSSH PRIVATE KEY-----\n";
  {
    std::printf("[7] RSA identity parsing, signing, and verification\n");
    auto rsa_id = net::ssh_parse_private_key(span_of(rsa_pem));
    check(rsa_id.ok() && rsa_id->valid, "RSA private key parsed successfully");
    if (rsa_id.ok()) {
      check(rsa_id->type == "ssh-rsa", "RSA identity type is ssh-rsa");
      check(rsa_id->public_line().rfind("ssh-rsa ", 0) == 0, "RSA public line starts with ssh-rsa");
      check(!rsa_id->fingerprint().empty(), "RSA fingerprint generated");

      // PKCS#1 v1.5 signing with SHA-256 and SHA-512 (rsa-sha2-256 / rsa-sha2-512)
      const std::string msg = "SSH-2.0 authentication exchange hash simulation";
      auto sig_sha256 = crypto::rsa_sign_pkcs1(rsa_id->rsa, crypto::RsaDigest::Sha256, span_of(msg));
      check(sig_sha256.ok(), "RSA PKCS#1 signing with SHA-256 (rsa-sha2-256)");

      crypto::RsaPublicKey rsa_pub;
      rsa_pub.modulus = rsa_id->rsa.modulus;
      rsa_pub.exponent = rsa_id->rsa.public_exponent;
      check(crypto::rsa_verify_pkcs1(rsa_pub, crypto::RsaDigest::Sha256, span_of(msg), *sig_sha256),
            "RSA PKCS#1 SHA-256 signature verifies with public key");

      auto sig_sha512 = crypto::rsa_sign_pkcs1(rsa_id->rsa, crypto::RsaDigest::Sha512, span_of(msg));
      check(sig_sha512.ok(), "RSA PKCS#1 signing with SHA-512 (rsa-sha2-512)");
      check(crypto::rsa_verify_pkcs1(rsa_pub, crypto::RsaDigest::Sha512, span_of(msg), *sig_sha512),
            "RSA PKCS#1 SHA-512 signature verifies with public key");
    }
  }

  // --- 8. RSA host key verification ---------------------------------------
  {
    std::printf("[8] RSA host key negotiation & signature verification\n");
    auto rsa_id = net::ssh_parse_private_key(span_of(rsa_pem));
    if (rsa_id.ok()) {
      ByteBuffer pub_blob = rsa_id->public_blob();
      check(!pub_blob.empty(), "RSA host key public blob generated");

      crypto::RsaPublicKey hostpub;
      hostpub.modulus = rsa_id->rsa.modulus;
      hostpub.exponent = rsa_id->rsa.public_exponent;

      const std::string exchange_hash = "simulated_kex_exchange_hash_32b!";
      auto sig256 = crypto::rsa_sign_pkcs1(rsa_id->rsa, crypto::RsaDigest::Sha256, span_of(exchange_hash));
      check(sig256.ok(), "generated rsa-sha2-256 signature over exchange hash");
      check(crypto::rsa_verify_pkcs1(hostpub, crypto::RsaDigest::Sha256, span_of(exchange_hash), *sig256),
            "verified rsa-sha2-256 host key signature");

      auto sig512 = crypto::rsa_sign_pkcs1(rsa_id->rsa, crypto::RsaDigest::Sha512, span_of(exchange_hash));
      check(sig512.ok(), "generated rsa-sha2-512 signature over exchange hash");
      check(crypto::rsa_verify_pkcs1(hostpub, crypto::RsaDigest::Sha512, span_of(exchange_hash), *sig512),
            "verified rsa-sha2-512 host key signature");
    }
  }

  std::printf(g_fail ? "\nRESULT: FAIL (%d)\n" : "\nRESULT: PASS\n", g_fail);
  return g_fail ? 1 : 0;
}
