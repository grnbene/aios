#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include "aios/core/result.hpp"

namespace aios::userland {

struct CalendarEvent {
  std::string id;
  std::string title;
  std::string location;
  std::string calendar_id = "work"; // "work", "personal", "releases", "ops"
  int year = 2026;
  int month = 9;
  int day = 13;
  int start_hour = 10;
  int start_min = 0;
  int duration_mins = 60;
  bool all_day = false;
  std::string description;
  std::vector<std::string> attendees;
  std::string recurrence = "NONE";
  uint32_t color = 0xFF0A84FF;
  std::string ics_path;

  std::string time_summary() const {
    if (all_day) return "All Day";
    char buf[64];
    int end_hour = start_hour + (start_min + duration_mins) / 60;
    int end_min = (start_min + duration_mins) % 60;
    const char* start_am = start_hour >= 12 ? "PM" : "AM";
    const char* end_am = end_hour >= 12 ? "PM" : "AM";
    int s_disp = start_hour % 12 == 0 ? 12 : start_hour % 12;
    int e_disp = end_hour % 12 == 0 ? 12 : end_hour % 12;
    std::snprintf(buf, sizeof(buf), "%d:%02d %s - %d:%02d %s", s_disp, start_min, start_am, e_disp, end_min, end_am);
    return std::string(buf);
  }

  std::string date_summary() const {
    const char* months[] = {"", "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const char* m = (month >= 1 && month <= 12) ? months[month] : "Sep";
    return std::string(m) + " " + std::to_string(day) + ", " + std::to_string(year);
  }
};

struct CalendarCategory {
  std::string id;
  std::string name;
  uint32_t color = 0xFF0A84FF;
  bool visible = true;
};

class CalendarStore {
 public:
  explicit CalendarStore(const std::string& base_dir);
  ~CalendarStore() = default;

  void reload();
  void save_all();

  std::vector<CalendarEvent> all_events() const;
  std::vector<CalendarEvent> events_for_day(int year, int month, int day) const;
  std::vector<CalendarEvent> events_for_month(int year, int month) const;
  const CalendarEvent* get_event(const std::string& id) const;

  // Assigns ev.id (and its ics path) in place, so the caller can name what it made.
  bool add_event(CalendarEvent& ev);
  bool update_event(const CalendarEvent& ev);
  bool delete_event(const std::string& id);

  std::vector<CalendarCategory> categories() const { return categories_; }
  // Add a category (a calendar the events belong to) if there is not one of that id yet.
  void ensure_category(const CalendarCategory& cat);
  void toggle_category(const std::string& cat_id);
  bool is_category_visible(const std::string& cat_id) const;

  std::vector<CalendarEvent> search(const std::string& query) const;

  bool import_ics_data(const std::string& ics_text);
  std::string export_ics(const std::string& id) const;
  std::string export_all_ics() const;

  size_t total_events() const { return events_.size(); }
  std::string status_line() const;

 private:
  std::string base_dir_;
  std::string db_path_;
  std::string events_dir_;
  std::vector<CalendarEvent> events_;
  std::vector<CalendarCategory> categories_;

  void load_index();
  void save_index();
  void save_ics_file(const CalendarEvent& ev);
  void delete_ics_file(const std::string& id);
};

}  // namespace aios::userland
