// Named time zones for grain's dates: the machine's tz database (the TZif files under /usr/share/zoneinfo, or
// $TZDIR), read here rather than through the C library, which can only hold one zone at a time (TZ=).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace aios::userland::grain::tz {

// A POSIX TZ rule ("EST5EDT,M3.2.0,M11.1.0"): what a TZif file's footer says for times after its last transition.
struct Rule {
  int64_t std_offset = 0;   // seconds east of UTC
  bool has_dst = false;
  int64_t dst_offset = 0;
  // A change's day: kind 'J' (Jn, 1-365, no Feb 29), 'D' (n, 0-365), 'M' (Mm.w.d); and its local time in seconds.
  struct When { char kind = 'M'; int month = 0, week = 0, weekday = 0, day = 0; int64_t time = 7200; };
  When start, end;
};

struct Zone {
  std::vector<int64_t> transitions;   // moments, ascending
  std::vector<uint8_t> type_of;       // the ttinfo each transition starts
  std::vector<int64_t> offsets;       // per ttinfo: seconds east of UTC
  bool has_rule = false;
  Rule rule;                          // after the last transition
  int64_t offset_at(int64_t t) const;
};

// The zone called `name` ("America/New_York"), loaded once and kept. Null with `error` set when there is no such
// zone, the name is not a zone's name, or the machine has no tz database.
std::shared_ptr<const Zone> load(const std::string& name, std::string& error);

// Parsers, apart for checking.
bool parse_tzif(const std::string& data, Zone& out, std::string& error);
bool parse_rule(const std::string& text, Rule& out);

}  // namespace aios::userland::grain::tz
