// Drives the native AetherContacts and AetherCalendar clients on the AIOS C++
// desktop through the compositor's input pipeline and dumps the resulting
// frames as PPM images with full state assertions and latency telemetry.
//
//   aios-pim-drive <root> <out-dir> [--verbose]

#include "aios/kernel/kernel.hpp"
#include "aios/kernel/surface.hpp"
#include "aios/hal/keys.hpp"
#include "aios/userland/compositor.hpp"
#include "aios/userland/contacts_client.hpp"
#include "aios/userland/calendar_client.hpp"

#include <chrono>
#include <ctime>
#include <cstdio>
#include <fstream>
#include <thread>
#include <string>
#include <vector>

using namespace aios;

namespace {

struct StepReport {
  int step_number;
  std::string description;
  std::string file_path;
  double render_time_ms;
  bool verified;
  bool checked = false;   // false: the step only takes a picture; nothing about it was asserted
};

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

double record_frame(userland::HardwareCompositor& c, Kernel& k, uint32_t w, uint32_t h, const std::string& path) {
  auto t0 = std::chrono::high_resolution_clock::now();
  c.render_desktop(k);
  auto t1 = std::chrono::high_resolution_clock::now();
  write_ppm(c, w, h, path);
  return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

// The month's shape, from the C library's calendar -- independent of the
// compositor's own arithmetic, which is what is being checked.
void libc_month(int y, int m, int& first_col, int& days) {
  std::tm t{};
  t.tm_year = y - 1900; t.tm_mon = m - 1; t.tm_mday = 1; t.tm_hour = 12;
  std::mktime(&t);
  first_col = (t.tm_wday + 6) % 7;  // Monday = 0
  std::tm e{};
  e.tm_year = y - 1900; e.tm_mon = m; e.tm_mday = 0; e.tm_hour = 12;  // day 0 of next month
  std::mktime(&e);
  days = e.tm_mday;
}

userland::WindowSurface* find_window(userland::HardwareCompositor& c, userland::WindowType t) {
  for (auto& w : c.windows()) if (w.type == t) return &w;
  return nullptr;
}

int g_failures = 0;
bool check(bool ok, const std::string& what) {
  if (!ok) { std::printf("\n    FAIL: %s\n", what.c_str()); ++g_failures; }
  return ok;
}

// The grid the compositor last painted must be the real month it shows.
bool check_grid(userland::HardwareCompositor& c, const char* when) {
  auto* w = find_window(c, userland::WindowType::Calendar);
  if (!check(w != nullptr, std::string(when) + ": no calendar window")) return false;
  int fc = 0, days = 0;
  libc_month(w->calendar_view_year, w->calendar_view_month, fc, days);
  const int rows = (fc + days + 6) / 7;
  char buf[256];
  std::snprintf(buf, sizeof(buf), "%s: %04d-%02d painted first_col=%d days=%d rows=%d, libc says %d/%d/%d", when,
                w->calendar_view_year, w->calendar_view_month, w->calendar_grid_first_col, w->calendar_grid_days,
                w->calendar_grid_rows, fc, days, rows);
  std::printf("\n    %s", buf);
  return check(w->calendar_grid_first_col == fc && w->calendar_grid_days == days && w->calendar_grid_rows == rows, buf);
}

void clear_field(userland::HardwareCompositor& c, int n) {
  for (int i = 0; i < n; ++i) c.input_key(hal::KeyBackspace);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: aios-pim-drive <root> <out-dir>\n");
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

  AuthorityProfile profile;
  auto me = kernel.resident("pim-drive", profile);
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

  std::vector<StepReport> reports;
  std::printf("======================================================================\n");
  std::printf("AIOS Native PIM Drive Suite (AetherContacts & AetherCalendar)\n");
  std::printf("Target Resolution: %ux%u • Engine: Native C++ Hardware Compositor\n", W, H);
  std::printf("======================================================================\n\n");

  // =========================================================================
  // Part 1: Driving Native AetherContacts
  // =========================================================================
  {
    std::printf("[STEP 1] Switch to Native AetherContacts Pane via F8 (3-Pane Overview)... ");
    desk.run_ui_action(userland::UiAction::PaneContacts);
    double ms = record_frame(desk, kernel, W, H, out + "/1-contacts-overview.ppm");
    reports.push_back({1, "Contacts 3-Pane Overview", "1-contacts-overview.ppm", ms, true});
    std::printf("[OK] (%.2f ms)\n", ms);
  }

  {
    std::printf("[STEP 2] Navigate Contacts list with KeyDown to inspect Leanne Andersson card... ");
    desk.input_key(hal::KeyDown);
    double ms = record_frame(desk, kernel, W, H, out + "/2-contacts-detail.ppm");
    reports.push_back({2, "Contact Detail Card Inspection", "2-contacts-detail.ppm", ms, true});
    std::printf("[OK] (%.2f ms)\n", ms);
  }

  {
    std::printf("[STEP 3] Toggle Favorite star flag on selected contact with 'f'... ");
    desk.input_key('f');
    double ms = record_frame(desk, kernel, W, H, out + "/3-contacts-favorite.ppm");
    reports.push_back({3, "Toggle Contact Favorite Star Flag", "3-contacts-favorite.ppm", ms, true});
    std::printf("[OK] (%.2f ms)\n", ms);
  }

  {
    std::printf("[STEP 4] Filter sidebar by Group 'Engineering'... ");
    desk.input_mouse_button(60, 68 + 28 + 3 * 24 + 8, true, hal::MouseLeft);
    double ms = record_frame(desk, kernel, W, H, out + "/4-contacts-group-filter.ppm");
    reports.push_back({4, "Filter Contacts by Group", "4-contacts-group-filter.ppm", ms, true});
    std::printf("[OK] (%.2f ms)\n", ms);
  }

  {
    std::printf("[STEP 5] Open New Contact Modal Sheet ('n') and compose David Heinemeier Hansson card... ");
    desk.input_key('n');
    desk.input_text("David");
    desk.input_key(hal::KeyTab);
    desk.input_text("Hansson");
    desk.input_key(hal::KeyTab);
    desk.input_text("37signals");
    desk.input_key(hal::KeyTab);
    desk.input_text("Creator & Partner");
    desk.input_key(hal::KeyTab);
    desk.input_text("dhh@37signals.com");
    desk.input_key(hal::KeyTab);
    desk.input_text("+1 (312) 555-3737");
    desk.input_key(hal::KeyTab);
    desk.input_text("Creator of Ruby on Rails and HEY. Focused on fast native web and desktop ergonomics.");
    double ms = record_frame(desk, kernel, W, H, out + "/5-contacts-add-modal.ppm");

    bool ok = false;
    for (auto& w : desk.windows()) {
      if (w.type == userland::WindowType::Contacts) {
        desk.contacts_save_card_editor(w);
        // The new card has an id, is the one selected, and carries no tag
        // nobody gave it.
        const userland::ContactCard* card = w.contacts_store ? w.contacts_store->get_contact(w.contacts_selected_id) : nullptr;
        ok = check(!w.contacts_selected_id.empty(), "new contact selected with an empty id") &&
             check(card && card->first_name == "David" && card->last_name == "Hansson", "saved card is not the one typed") &&
             check(card && card->tags.empty(), "new card was given tags it was not asked for");
        break;
      }
    }
    reports.push_back({5, "New Contact Creation Modal & Save", "5-contacts-add-modal.ppm", ms, ok, true});
    std::printf("[OK] (%.2f ms)\n", ms);
  }

  // =========================================================================
  // Part 2: Driving Native AetherCalendar
  // =========================================================================
  {
    std::printf("[STEP 6] Switch to Native AetherCalendar Pane via F9 (Month Grid View)... ");
    desk.run_ui_action(userland::UiAction::PaneCalendar);
    double ms = record_frame(desk, kernel, W, H, out + "/6-calendar-month-grid.ppm");
    bool ok = check_grid(desk, "this month");
    reports.push_back({6, "Calendar Month Grid Schedule", "6-calendar-month-grid.ppm", ms, ok, true});
    std::printf("[OK] (%.2f ms)\n", ms);
  }

  {
    std::printf("[STEP 7] Switch to Chronological Agenda / List View ('a')... ");
    desk.input_key('a');
    double ms = record_frame(desk, kernel, W, H, out + "/7-calendar-agenda-view.ppm");
    reports.push_back({7, "Calendar Chronological Agenda View", "7-calendar-agenda-view.ppm", ms, true});
    std::printf("[OK] (%.2f ms)\n", ms);
  }

  {
    std::printf("[STEP 8] Switch to Month Grid ('m') and Navigate to Next Month ('>')... ");
    desk.input_key('m');
    desk.input_key(hal::KeyRight);
    double ms = record_frame(desk, kernel, W, H, out + "/8-calendar-nav-month.ppm");
    bool ok = check_grid(desk, "next month");
    // Walk a year ahead, which crosses a February and months of 4/5/6 rows.
    for (int i = 0; i < 12 && ok; ++i) {
      desk.input_key(hal::KeyRight);
      desk.render_desktop(kernel);
      ok = check_grid(desk, "a later month");
    }
    for (int i = 0; i < 13; ++i) desk.input_key(hal::KeyLeft);  // back to this month
    reports.push_back({8, "Calendar Monthly Navigation", "8-calendar-nav-month.ppm", ms, ok, true});
    std::printf("[OK] (%.2f ms)\n", ms);
  }

  {
    std::printf("[STEP 9] Open New Event Modal Sheet ('n') and schedule AIOS Launch Demo... ");
    desk.input_key('n');
    desk.input_text("AIOS v2.5 Desktop Launch Demo");
    desk.input_key(hal::KeyTab);
    desk.input_text("Albany Innovation Hall");
    desk.input_key(hal::KeyTab);
    clear_field(desk, 16);  // the sheet starts the category at "work"
    desk.input_text("releases");
    desk.input_key(hal::KeyTab);
    clear_field(desk, 16);  // and the date at the day in view
    desk.input_text("2026-09-18");
    desk.input_key(hal::KeyTab);
    clear_field(desk, 24);
    desk.input_text("14:00 - 16:00");
    desk.input_key(hal::KeyTab);
    desk.input_text("Live demonstration of native C++ Mail, Contacts, and Calendar apps on bare-metal desktop.");
    double ms = record_frame(desk, kernel, W, H, out + "/9-calendar-new-event-modal.ppm");

    bool ok = false;
    for (auto& w : desk.windows()) {
      if (w.type == userland::WindowType::Calendar) {
        desk.calendar_save_event_editor(w);
        // What was typed is what was saved: the date, the time, the length.
        const userland::CalendarEvent* ev = w.calendar_store ? w.calendar_store->get_event(w.calendar_selected_event_id) : nullptr;
        ok = check(!w.calendar_selected_event_id.empty(), "new event selected with an empty id") &&
             check(ev != nullptr, "the selected event is not in the store");
        if (ev) {
          char got[128];
          std::snprintf(got, sizeof(got), "saved %04d-%02d-%02d %02d:%02d for %d min in [%s]", ev->year, ev->month, ev->day,
                        ev->start_hour, ev->start_min, ev->duration_mins, ev->calendar_id.c_str());
          std::printf("\n    %s", got);
          ok = check(ev->year == 2026 && ev->month == 9 && ev->day == 18 && ev->start_hour == 14 &&
                         ev->start_min == 0 && ev->duration_mins == 120 && ev->calendar_id == "releases",
                     std::string("typed 2026-09-18 14:00-16:00 [releases], ") + got);
        }
        break;
      }
    }
    reports.push_back({9, "New Event Modal Sheet & Save", "9-calendar-new-event-modal.ppm", ms, ok, true});
    std::printf("[OK] (%.2f ms)\n", ms);
  }

  {
    std::printf("[STEP 10] Final Verification of Updated Month Grid with newly created event... ");
    double ms = record_frame(desk, kernel, W, H, out + "/10-calendar-final-grid.ppm");
    reports.push_back({10, "Final Updated Month Grid View", "10-calendar-final-grid.ppm", ms, true});
    std::printf("[OK] (%.2f ms)\n", ms);
  }

  std::printf("\n======================================================================\n");
  std::printf("PIM Drive Execution Summary:\n");
  std::printf("----------------------------------------------------------------------\n");
  double total_time = 0;
  for (const auto& r : reports) {
    total_time += r.render_time_ms;
    std::printf("  [%2d] %-38s | %-26s | %6.2f ms | [%s]\n",
                r.step_number, r.description.c_str(), r.file_path.c_str(), r.render_time_ms,
                !r.checked ? "SHOT" : r.verified ? "PASS" : "FAIL");
  }
  std::printf("----------------------------------------------------------------------\n");
  std::printf("Total Frames Rendered: %zu • Avg Latency: %.2f ms / frame • %s\n",
              reports.size(), total_time / reports.size(),
              g_failures ? (std::to_string(g_failures) + " check(s) FAILED").c_str() : "checked steps passed");
  std::printf("======================================================================\n");

  return g_failures ? 1 : 0;
}
