// Drives the native AetherMail client on the AIOS C++ desktop through the
// compositor's input pipeline, dumps the resulting frames as PPM images, and
// checks what each step did to the mail store.
//
//   aios-mail-drive <root> <out-dir>
//
// It sends a message through the real send path, so it REFUSES a root that
// already has a mail account: on a configured machine that send would be real
// mail to a real address. Point it at a fresh, throwaway root.

#include "aios/kernel/kernel.hpp"
#include "aios/kernel/surface.hpp"
#include "aios/hal/keys.hpp"
#include "aios/userland/compositor.hpp"
#include "aios/userland/mail_client.hpp"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>
#include <string>
#include <vector>

using namespace aios;

namespace {

void write_ppm(const userland::HardwareCompositor& c, uint32_t w, uint32_t h, const std::string& path) {
  const auto& fb = c.framebuffer();
  std::ofstream out(path, std::ios::binary);
  out << "P6\n" << w << " " << h << "\n255\n";
  for (size_t i = 0; i < fb.size(); ++i) {
    uint32_t p = fb[i];
    char rgb[3] = {(char)((p >> 16) & 0xFF), (char)((p >> 8) & 0xFF), (char)(p & 0xFF)};
    out.write(rgb, 3);
  }
}

void frame(userland::HardwareCompositor& c, Kernel& k, uint32_t w, uint32_t h, const std::string& path) {
  c.render_desktop(k);
  write_ppm(c, w, h, path);
}

int g_failures = 0;
void check(bool ok, const std::string& what) {
  std::printf("    %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok) ++g_failures;
}

userland::WindowSurface* mail_window(userland::HardwareCompositor& c) {
  for (auto& w : c.windows()) if (w.type == userland::WindowType::Mail) return &w;
  return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: aios-mail-drive <root> <out-dir>\n");
    return 2;
  }
  const std::string root = argv[1];
  const std::string out = argv[2];

  KernelConfig cfg;
  cfg.root = root;
  auto k = Kernel::boot(cfg);
  if (!k.ok()) {
    std::fprintf(stderr, "boot error: %s\n", k.error().str().c_str());
    return 1;
  }
  Kernel& kernel = **k;

  {
    userland::MailStore probe(root + "/mail");
    if (!probe.accounts().empty()) {
      std::fprintf(stderr, "refusing: %s/mail already has %zu mail account(s); this drive sends a message and would "
                           "send it for real. Use a fresh throwaway root.\n", root.c_str(), probe.accounts().size());
      return 2;
    }
  }

  AuthorityProfile profile;
  auto me = kernel.resident("mail-drive", profile);
  if (!me.ok()) {
    std::fprintf(stderr, "resident error: %s\n", me.error().str().c_str());
    return 1;
  }
  Surface console = kernel.as(*me);

  const uint32_t W = 1280, H = 800;
  userland::HardwareCompositor desk(W, H);
  desk.set_fullscreen(false);
  desk.set_console(&console);
  desk.set_dispatcher([](const std::string&, userland::OutputCallback) {});
  desk.init_default_workspace(kernel);

  std::printf("Step 1: Switch to Native AetherMail Pane via F7 / PaneMail action (a fresh root: no account)\n");
  desk.run_ui_action(userland::UiAction::PaneMail);
  frame(desk, kernel, W, H, out + "/1-mail-inbox.ppm");

  std::printf("Step 2: Navigate message list to Leanne Andersson (Finance Introduction) with KeyDown\n");
  desk.input_key(hal::KeyDown);
  frame(desk, kernel, W, H, out + "/2-mail-linus-review.ppm");

  std::printf("Step 3: Toggle Flag on current message with 'f'\n");
  desk.input_key('f');
  frame(desk, kernel, W, H, out + "/3-mail-flagged-toggle.ppm");

  std::printf("Step 4: Open Compose Modal Sheet with 'n' and compose new email\n");
  desk.input_key('n');
  desk.input_text("someone@example.test");
  desk.input_key(hal::KeyTab); // to Cc
  desk.input_key(hal::KeyTab); // to Subject
  desk.input_text("Re: Finance Introduction");
  desk.input_key(hal::KeyTab); // to Body
  desk.input_text("Hi Leanne,\n\nWelcome Lyndsay! We will set up a review call this week to walk through P&L drill-downs and ledger recategorization.\n\nBest,\nSean");
  frame(desk, kernel, W, H, out + "/4-mail-compose-modal.ppm");

  std::printf("Step 5: Send composed email\n");
  // Click Send button inside modal (mw=700, mh=480, mx=(1280-700)/2=290, my=(800-480)/2=160 -> send btn around mx+mw-50, my+16)
  desk.input_mouse_button(290 + 700 - 45, 160 + 16, true, hal::MouseLeft);
  frame(desk, kernel, W, H, out + "/5-mail-sent.ppm");
  if (auto* w = mail_window(desk); w && w->mail_store) {
    auto sent = w->mail_store->get_messages("sent", "subject:\"Re: Finance Introduction\"", 0, "all");
    check(sent.size() == 1, "the composed message is in Sent (" + std::to_string(sent.size()) + " found)");
    if (!sent.empty()) check(!sent[0].to.empty() && sent[0].to[0] == "someone@example.test", "addressed to what was typed");
    const std::string st = w->mail_store->status_line();
    check(st.find("not delivered") != std::string::npos,
          "with no account the status says it was not delivered: \"" + st + "\"");
  } else {
    check(false, "no mail window");
  }

  std::printf("Step 6: Open Account Setup & Settings Modal with the Google preset\n");
  // Find focused window and open accounts modal
  for (auto& w : desk.windows()) {
    if (w.type == userland::WindowType::Mail) {
      desk.mail_open_accounts(w);
      desk.mail_apply_preset(w, 1); // Google preset
      w.mail_acc_name.text = "Drive Test";
      w.mail_acc_email.text = "drive-test@example.test";
      w.mail_acc_user.text = "drive-test@example.test";
      // Leave the password field ALONE. This harness used to type a row of
      // bullet characters here and then save the account, which wrote the
      // string "••••••••••" into the keyring as the real credential -- every
      // later IMAP login died on it, and because the sync swallowed its own
      // errors the header just went on saying "Updated just now". A drive
      // harness pointed at a live root must never author a secret.
      // mail_save_account keeps the stored password when this field is empty.
      w.mail_acc_pass.text.clear();
      // No invented connection result either. Whatever the last real probe
      // said stands; a screenshot claiming "[Login Verified]" for a login that
      // never happened is worse than a blank line.
      break;
    }
  }
  frame(desk, kernel, W, H, out + "/6-mail-account-setup.ppm");

  std::printf("Step 7: Save Account and verify multi-account sidebar\n");
  for (auto& w : desk.windows()) {
    if (w.type == userland::WindowType::Mail) {
      desk.mail_save_account(w);
      break;
    }
  }
  frame(desk, kernel, W, H, out + "/7-mail-account-configured.ppm");
  if (auto* w = mail_window(desk); w && w->mail_store) {
    const auto* acc = w->mail_store->get_account("drive-test@example.test");
    check(acc != nullptr, "the account was saved");
    if (acc) check(acc->password.empty(), "and the drive authored no password for it");
    check(w->mail_store->accounts().size() == 1, "one account configured");
  }

  std::printf("Step 8: Perform Instant Structured Search: 'from:Linode' in search bar\n");
  for (auto& w : desk.windows()) {
    if (w.type == userland::WindowType::Mail) {
      w.mail_active_folder = "inbox";
      w.mail_filter_mode = 0;
      w.mail_search_focused = true;
      w.mail_search_edit.text = "from:Linode";
      w.mail_search_edit.cursor = static_cast<int>(w.mail_search_edit.text.size());
      w.mail_list_scroll = 0;
      auto msgs = w.mail_store->get_messages(w.mail_active_folder, w.mail_search_edit.text, w.mail_filter_mode, w.mail_active_account_id);
      if (!msgs.empty()) {
        w.mail_selected_id = msgs[0].id;
      }
      // Every hit must actually match; a fresh root has none, which is fine.
      bool all_match = true;
      for (const auto& m : msgs) {
        const std::string hay = std::string("from:Linode").rfind("from:", 0) == 0 ? m.from_name + " " + m.from_addr : m.subject;
        std::string lo = hay, needle = std::string("Linode");
        for (auto& ch : lo) ch = (char)std::tolower((unsigned char)ch);
        for (auto& ch : needle) ch = (char)std::tolower((unsigned char)ch);
        if (lo.find(needle) == std::string::npos) all_match = false;
      }
      check(all_match, std::to_string(msgs.size()) + " hit(s) for from:Linode, each one matching");
      break;
    }
  }
  frame(desk, kernel, W, H, out + "/8-mail-search-from.ppm");

  std::printf("Step 9: Perform Multi-Field Search: 'subject:Discrepancy' & Attachments Filter\n");
  for (auto& w : desk.windows()) {
    if (w.type == userland::WindowType::Mail) {
      w.mail_active_folder = "inbox";
      w.mail_filter_mode = 0;
      w.mail_search_focused = true;
      w.mail_search_edit.text = "subject:Discrepancy";
      w.mail_search_edit.cursor = static_cast<int>(w.mail_search_edit.text.size());
      w.mail_list_scroll = 0;
      auto msgs = w.mail_store->get_messages(w.mail_active_folder, w.mail_search_edit.text, w.mail_filter_mode, w.mail_active_account_id);
      if (!msgs.empty()) {
        w.mail_selected_id = msgs[0].id;
      }
      // Every hit must actually match; a fresh root has none, which is fine.
      bool all_match = true;
      for (const auto& m : msgs) {
        const std::string hay = std::string("subject:Discrepancy").rfind("from:", 0) == 0 ? m.from_name + " " + m.from_addr : m.subject;
        std::string lo = hay, needle = std::string("Discrepancy");
        for (auto& ch : lo) ch = (char)std::tolower((unsigned char)ch);
        for (auto& ch : needle) ch = (char)std::tolower((unsigned char)ch);
        if (lo.find(needle) == std::string::npos) all_match = false;
      }
      check(all_match, std::to_string(msgs.size()) + " hit(s) for subject:Discrepancy, each one matching");
      break;
    }
  }
  frame(desk, kernel, W, H, out + "/9-mail-search-filters.ppm");

  if (g_failures) {
    std::printf("%d check(s) FAILED\n", g_failures);
    return 1;
  }
  std::printf("All checks passed (local store only: no server was contacted, no mail left this machine).\n");
  return 0;
}
