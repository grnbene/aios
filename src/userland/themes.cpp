#include "aios/userland/themes.hpp"

#include <cstring>

namespace aios::userland {

namespace {

// Panic Dark restates the compositor's original `namespace syn` palette
// exactly, byte for byte, so switching to it (or starting on it) changes
// nothing about how an existing file reads.
constexpr uint32_t PD_text    = 0xFFC9D1D9;
constexpr uint32_t PD_keyword = 0xFFFF7B72;
constexpr uint32_t PD_type    = 0xFFFFA657;
constexpr uint32_t PD_str     = 0xFFA5D6FF;
constexpr uint32_t PD_comment = 0xFF8B949E;
constexpr uint32_t PD_number  = 0xFF79C0FF;
constexpr uint32_t PD_preproc = 0xFFD2A8FF;
constexpr uint32_t PD_var     = 0xFFFFA198;
constexpr uint32_t PD_func    = 0xFFD2A8FF;
constexpr uint32_t PD_deco    = 0xFFD2A8FF;
constexpr uint32_t PD_tag     = 0xFF7EE787;
constexpr uint32_t PD_attr    = 0xFF79C0FF;
constexpr uint32_t PD_heading = 0xFF79C0FF;
constexpr uint32_t PD_emph    = 0xFFF0F6FC;
constexpr uint32_t PD_link    = 0xFF58A6FF;
constexpr uint32_t PD_code    = 0xFFA5D6FF;
constexpr uint32_t PD_rule    = 0xFF8B949E;

// Panic Light: the light-ground sibling of the default, in the same family.
constexpr uint32_t PL_ground  = 0xFFF5F2F0;
constexpr uint32_t PL_text    = 0xFF3D3D3D;
constexpr uint32_t PL_keyword = 0xFFA20070;
constexpr uint32_t PL_type    = 0xFF6B4EFF;
constexpr uint32_t PL_str     = 0xFF1B8C38;
constexpr uint32_t PL_comment = 0xFF95928F;
constexpr uint32_t PL_number  = 0xFF4E5BB0;
constexpr uint32_t PL_preproc = 0xFFA20070;
constexpr uint32_t PL_var     = 0xFF6B4EFF;
constexpr uint32_t PL_func    = 0xFF6B4EFF;
constexpr uint32_t PL_deco    = 0xFFA20070;
constexpr uint32_t PL_tag     = 0xFF0E8A71;
constexpr uint32_t PL_attr    = 0xFF4E5BB0;
constexpr uint32_t PL_heading = 0xFF4E5BB0;
constexpr uint32_t PL_emph    = 0xFF232323;
constexpr uint32_t PL_link    = 0xFF2C76B5;
constexpr uint32_t PL_code    = 0xFF1B8C38;
constexpr uint32_t PL_rule    = 0xFF95928F;

// Nord: the cold arctic palette.
constexpr uint32_t NO_ground  = 0xFF2E3440;
constexpr uint32_t NO_text    = 0xFFD8DEE9;
constexpr uint32_t NO_keyword = 0xFF81A1C1;
constexpr uint32_t NO_type    = 0xFF88C0D0;
constexpr uint32_t NO_str     = 0xFFA3BE8C;
constexpr uint32_t NO_comment = 0xFF616E88;
constexpr uint32_t NO_number  = 0xFFB48EAD;
constexpr uint32_t NO_preproc = 0xFFD08770;
constexpr uint32_t NO_var     = 0xFF8FBCBB;
constexpr uint32_t NO_func    = 0xFF88C0D0;
constexpr uint32_t NO_deco    = 0xFFD08770;
constexpr uint32_t NO_tag     = 0xFF88C0D0;
constexpr uint32_t NO_attr    = 0xFF81A1C1;
constexpr uint32_t NO_heading = 0xFF8FBCBB;
constexpr uint32_t NO_emph    = 0xFFECEFF4;
constexpr uint32_t NO_link    = 0xFF5E81AC;
constexpr uint32_t NO_code    = 0xFFA3BE8C;
constexpr uint32_t NO_rule    = 0xFF616E88;

// Solarized (dark variant).
constexpr uint32_t SO_ground  = 0xFF002B36;
constexpr uint32_t SO_text    = 0xFF839496;
constexpr uint32_t SO_keyword = 0xFF859900;
constexpr uint32_t SO_type    = 0xFFB58900;
constexpr uint32_t SO_str     = 0xFF2AA198;
constexpr uint32_t SO_comment = 0xFF586E75;
constexpr uint32_t SO_number  = 0xFFD33682;
constexpr uint32_t SO_preproc = 0xFFCB4B16;
constexpr uint32_t SO_var     = 0xFF6C71C4;
constexpr uint32_t SO_func    = 0xFF268BD2;
constexpr uint32_t SO_deco    = 0xFFCB4B16;
constexpr uint32_t SO_tag     = 0xFF93A1A1;
constexpr uint32_t SO_attr    = 0xFF268BD2;
constexpr uint32_t SO_heading = 0xFFB58900;
constexpr uint32_t SO_emph    = 0xFFEEE8D5;
constexpr uint32_t SO_link    = 0xFF268BD2;
constexpr uint32_t SO_code    = 0xFF2AA198;
constexpr uint32_t SO_rule    = 0xFF586E75;

// Boba by Panic: warm dark boba/tapioca ground, milk-tea text, warm pastel syntax.
constexpr uint32_t BO_ground  = 0xFF252220;
constexpr uint32_t BO_text    = 0xFFF0EAE1;
constexpr uint32_t BO_keyword = 0xFFF46B5D;
constexpr uint32_t BO_type    = 0xFFF5B759;
constexpr uint32_t BO_str     = 0xFF87C095;
constexpr uint32_t BO_comment = 0xFF877C75;
constexpr uint32_t BO_number  = 0xFFF39C6B;
constexpr uint32_t BO_preproc = 0xFFE58A97;
constexpr uint32_t BO_var     = 0xFFE5A97D;
constexpr uint32_t BO_func    = 0xFF78B4DB;
constexpr uint32_t BO_deco    = 0xFFE58A97;
constexpr uint32_t BO_tag     = 0xFF87C095;
constexpr uint32_t BO_attr    = 0xFF78B4DB;
constexpr uint32_t BO_heading = 0xFFF5B759;
constexpr uint32_t BO_emph    = 0xFFF9F6F0;
constexpr uint32_t BO_link    = 0xFF78B4DB;
constexpr uint32_t BO_code    = 0xFF87C095;
constexpr uint32_t BO_rule    = 0xFF877C75;

// Monokai.
constexpr uint32_t MK_ground  = 0xFF272822;
constexpr uint32_t MK_text    = 0xFFF8F8F2;
constexpr uint32_t MK_keyword = 0xFFF92672;
constexpr uint32_t MK_type    = 0xFFA6E22E;
constexpr uint32_t MK_str     = 0xFFE6DB74;
constexpr uint32_t MK_comment = 0xFF75715E;
constexpr uint32_t MK_number  = 0xFFAE81FF;
constexpr uint32_t MK_preproc = 0xFF66D9EF;
constexpr uint32_t MK_var     = 0xFFF8F8F2;
constexpr uint32_t MK_func    = 0xFF66D9EF;
constexpr uint32_t MK_deco    = 0xFFA6E22E;
constexpr uint32_t MK_tag     = 0xFFF92672;
constexpr uint32_t MK_attr    = 0xFFA6E22E;
constexpr uint32_t MK_heading = 0xFFFD971F;
constexpr uint32_t MK_emph    = 0xFFFD971F;
constexpr uint32_t MK_link    = 0xFF66D9EF;
constexpr uint32_t MK_code    = 0xFFA6E22E;
constexpr uint32_t MK_rule    = 0xFF75715E;

const EditorTheme k_themes[] = {
  { "boba",        BO_ground,  { BO_text, BO_keyword, BO_type, BO_str, BO_comment, BO_number, BO_preproc, BO_var, BO_func, BO_deco, BO_tag, BO_attr, BO_heading, BO_emph, BO_link, BO_code, BO_rule } },
  { "panic-boba",  BO_ground,  { BO_text, BO_keyword, BO_type, BO_str, BO_comment, BO_number, BO_preproc, BO_var, BO_func, BO_deco, BO_tag, BO_attr, BO_heading, BO_emph, BO_link, BO_code, BO_rule } },
  { "panic-dark",  0xFF0D1117, { PD_text, PD_keyword, PD_type, PD_str, PD_comment, PD_number, PD_preproc, PD_var, PD_func, PD_deco, PD_tag, PD_attr, PD_heading, PD_emph, PD_link, PD_code, PD_rule } },
  { "panic-light", PL_ground,  { PL_text, PL_keyword, PL_type, PL_str, PL_comment, PL_number, PL_preproc, PL_var, PL_func, PL_deco, PL_tag, PL_attr, PL_heading, PL_emph, PL_link, PL_code, PL_rule } },
  { "nord",        NO_ground,  { NO_text, NO_keyword, NO_type, NO_str, NO_comment, NO_number, NO_preproc, NO_var, NO_func, NO_deco, NO_tag, NO_attr, NO_heading, NO_emph, NO_link, NO_code, NO_rule } },
  { "solarized",   SO_ground,  { SO_text, SO_keyword, SO_type, SO_str, SO_comment, SO_number, SO_preproc, SO_var, SO_func, SO_deco, SO_tag, SO_attr, SO_heading, SO_emph, SO_link, SO_code, SO_rule } },
  { "monokai",     MK_ground,  { MK_text, MK_keyword, MK_type, MK_str, MK_comment, MK_number, MK_preproc, MK_var, MK_func, MK_deco, MK_tag, MK_attr, MK_heading, MK_emph, MK_link, MK_code, MK_rule } },
};

// The active theme index. Kept as a single file-local so both the read (the
// render path) and the write (set_active_theme) agree without a lock. The
// editor is single-threaded over its own main loop, and the only writers run
// there.
int g_active_theme_index = 0;  // k_themes[0] = boba = default

}  // namespace

const EditorTheme* the_themes(size_t* out_count) {
  *out_count = sizeof(k_themes) / sizeof(k_themes[0]);
  return k_themes;
}

const EditorTheme& active_theme() {
  return k_themes[(size_t)g_active_theme_index];
}

bool set_active_theme(const std::string& name) {
  size_t count = 0;
  const EditorTheme* themes = the_themes(&count);
  for (size_t i = 0; i < count; ++i) {
    if (themes[i].name == name) {
      g_active_theme_index = (int)i;
      return true;
    }
  }
  return false;
}

}  // namespace aios::userland
