#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aios::userland {

// High-fidelity vector glyphs and icons for AIOS Desktop and Face userland.
// Crisp, mathematically drawn vector shapes with anti-aliasing -- no emojis or font fallback hacks.
enum class Icon : uint32_t {
  None = 0,
  Terminal,      // Shell prompt >_
  Code,          // Code angle tags </>
  Database,      // 3D stacked database cylinders
  Browser,       // World globe with latitude/longitude lines
  Mail,          // Sealed envelope with flap and seal
  Inspector,     // Equalizer slider knobs / telemetry
  Gear,          // Precision machine cog
  Flame,         // High-velocity stream / HOT pipeline
  Box,           // 3D isometric inventory package / SKU
  Sql,           // Database table grid with query bolt
  Cluster,       // 4-node distributed mesh graph / SYS
  Search,        // Magnifying glass
  Play,          // Play triangle
  Stop,          // Stop square
  Close,         // Window / action cross ×
  Minimize,      // Horizontal line −
  Maximize,      // Window square outline □
  ArrowUp,       // Directional arrow ↑
  ArrowDown,     // Directional arrow ↓
  ArrowLeft,     // Directional arrow ←
  ArrowRight,    // Directional arrow →
  Return,        // Carriage return / enter arrow ↵
  Command,       // Command loop symbol ⌘
  Check,         // Checkmark ✓
  Sparkle,       // 4-point spark
  User,          // Avatar / user silhouette
  Power,         // Standby power ring with vertical notch
  Reply,         // Curved reply arrow ↩
  ReplyAll,      // Double curved reply all arrow
  Forward,       // Curved forward arrow ↪
  Flag,          // Flag / pennant on pole
  Trash,         // Trash can with lid
  Archive,       // Archive storage box
  Refresh,       // Circular sync/refresh arrow ↻
  Copy,          // Dual overlapping documents
  Sun,           // Sun icon with radial rays
  Moon,          // Crescent moon icon
  Paperclip,     // Paperclip attachment
  Plus,          // Add / insert cross +
  Filter,        // Funnel filter
  Download,      // Tray with a downward arrow (export)
  SkipBack,      // |< jump to the first page
  SkipForward,   // >| jump to the last page
  Columns,       // Grid column bars
  Plug           // Connector plug / link
};

// Visual variants for buttons across AIOS
enum class ButtonVariant {
  Standard,      // Slate background with subtle border
  Primary,       // Glowing blue/emerald vibrant accent gradient
  Secondary,     // Dark acrylic glass container
  Ghost,         // Transparent until hovered
  Danger,        // Crimson fill and border
  DockTile,      // Premium continuous-corner squircle dock app tile
  Keycap,        // Tactile mechanical keycap (ESC, ⌘ 1, Ctrl+P)
  Badge,         // Compact status/action badge (HOT, SKU, SQL, SYS)
  Pill           // Rounded pill button
};

// Interactive state of a button
enum class ButtonState {
  Normal,
  Hover,
  Pressed,
  Active,
  Disabled
};

class ButtonSuite {
 public:
  // --- Geometric & Antialiased Primitives ---
  static void blend_pixel(std::vector<uint32_t>& buf, int buf_w, int buf_h, int x, int y, uint32_t color, int alpha = 255);
  static void draw_line_aa(std::vector<uint32_t>& buf, int buf_w, int buf_h, float x0, float y0, float x1, float y1, uint32_t color, float thickness = 1.0f);
  static void draw_circle(std::vector<uint32_t>& buf, int buf_w, int buf_h, float cx, float cy, float radius, uint32_t fill_color, uint32_t outline_color = 0, float outline_width = 1.0f);
  static void draw_rounded_rect(std::vector<uint32_t>& buf, int buf_w, int buf_h, int x, int y, int w, int h, int radius, uint32_t fill);
  static void draw_round_outline(std::vector<uint32_t>& buf, int buf_w, int buf_h, int x, int y, int w, int h, int radius, uint32_t edge, float thickness = 1.0f);
  static void draw_gradient_v(std::vector<uint32_t>& buf, int buf_w, int buf_h, int x, int y, int w, int h, uint32_t c_top, uint32_t c_bottom);
  // A vertical gradient inside a rounded rectangle. draw_rounded_rect is this
  // with one colour, so the corner coverage is written once: a surface that
  // wants a gradient AND round corners had to draw a square gradient and stroke
  // a rounded outline over it, which leaves the fill sticking out past the
  // stroke in all four corners.
  static void draw_rounded_gradient_v(std::vector<uint32_t>& buf, int buf_w, int buf_h, int x, int y, int w, int h, int radius, uint32_t c_top, uint32_t c_bottom);
  // A one-pixel rim light along the TOP of a rounded rectangle, following the
  // corners round and fading out as they turn away from the light. `alpha` is
  // passed separately on purpose: blend_pixel takes its opacity from the
  // argument and ignores the colour's own alpha channel entirely, so a 0x40-
  // prefixed colour handed to a drawing call is silently fully opaque.
  static void draw_rounded_top_rim(std::vector<uint32_t>& buf, int buf_w, int buf_h, int x, int y, int w, int h, int radius, uint32_t color, int alpha);

  // Smooth antialiased polygon (triangles, gems, stars, custom shapes)
  static void draw_polygon_aa(std::vector<uint32_t>& buf, int buf_w, int buf_h,
                              const std::vector<std::pair<float, float>>& pts,
                              uint32_t fill_color, uint32_t outline_color = 0, float outline_width = 0.0f);

  // Smooth quadratic Bezier curve
  static void draw_bezier_aa(std::vector<uint32_t>& buf, int buf_w, int buf_h,
                             float x0, float y0, float cx, float cy, float x1, float y1,
                             uint32_t color, float thickness = 1.0f);

  // Geometric N-pointed star
  static void draw_star(std::vector<uint32_t>& buf, int buf_w, int buf_h,
                        float cx, float cy, float r_outer, float r_inner, int points,
                        uint32_t fill_color, uint32_t outline_color = 0, float outline_width = 0.0f);

  // Soft Gaussian alpha-blended drop shadow
  static void draw_drop_shadow(std::vector<uint32_t>& buf, int buf_w, int buf_h,
                               int x, int y, int w, int h, int radius, int blur, int shadow_alpha = 90);

  // --- Vector Icon Rendering Suite ---
  static void draw_icon(std::vector<uint32_t>& buf, int buf_w, int buf_h, int cx, int cy, Icon icon, int size, uint32_t color, uint32_t secondary_color = 0);
  static Icon icon_from_name(const std::string& name);

  // --- Button & Control Components ---
  // High-end Dock App Tile (as shown in modern desktop glass)
  static void draw_dock_tile(std::vector<uint32_t>& buf, int buf_w, int buf_h,
                             int x, int y, int size,
                             Icon icon, uint32_t base_color,
                             bool hot, bool active,
                             int active_pip_count = 1);

  // Tactile Mechanical Keycap Button (e.g. ESC, ⌘ 1, Tab, ↵)
  static void draw_keycap(std::vector<uint32_t>& buf, int buf_w, int buf_h,
                          int x, int y, int w, int h,
                          const std::string& label,
                          bool hot = false,
                          Icon icon = Icon::None);

  // Integrated Badge Button (e.g. HOT, SKU, SQL, SYS)
  static void draw_badge_button(std::vector<uint32_t>& buf, int buf_w, int buf_h,
                                int x, int y, int w, int h,
                                const std::string& text,
                                Icon icon,
                                uint32_t bg_color,
                                uint32_t fg_color,
                                bool hot = false);

  // General Interactive Button
  static void draw_button(std::vector<uint32_t>& buf, int buf_w, int buf_h,
                          int x, int y, int w, int h,
                          const std::string& label,
                          Icon icon = Icon::None,
                          ButtonVariant variant = ButtonVariant::Standard,
                          ButtonState state = ButtonState::Normal);

  // Window Traffic Light Button (Close, Minimize, Maximize)
  enum class TrafficLightType { Close, Minimize, Maximize };
  static void draw_traffic_light(std::vector<uint32_t>& buf, int buf_w, int buf_h,
                                 int cx, int cy, int size,
                                 TrafficLightType type,
                                 bool hot);

  // Window Action Button (Titlebar right buttons: Minimize, Maximize, Close)
  enum class WindowAction { Minimize, Maximize, Close };
  static void draw_window_action(std::vector<uint32_t>& buf, int buf_w, int buf_h,
                                 int x, int y, int w, int h,
                                 WindowAction action,
                                 bool hot);
};

}  // namespace aios::userland
