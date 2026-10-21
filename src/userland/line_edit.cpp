#include "aios/userland/line_edit.hpp"

namespace aios::userland {
namespace {

bool is_word_byte(char c) {
  unsigned char u = (unsigned char)c;
  return (u >= '0' && u <= '9') || (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || u == '_' || u >= 0x80;
}

// UTF-8 continuation bytes are not positions a cursor may stop at: stepping one
// byte at a time through a multi-byte character puts the caret inside a glyph
// and the next backspace cuts it in half.
bool is_continuation(char c) { return ((unsigned char)c & 0xC0) == 0x80; }

}  // namespace

void EditLine::insert(const std::string& s, size_t max_len) {
  if (s.empty()) return;
  std::string add = s;
  if (max_len) {
    if (text.size() >= max_len) return;
    size_t room = max_len - text.size();
    if (add.size() > room) {
      add.resize(room);
      // Never leave half a character behind: a paste trimmed mid-sequence
      // puts bytes in the line that no glyph corresponds to.
      while (!add.empty() && is_continuation(add.back())) add.pop_back();
      if (!add.empty() && ((unsigned char)add.back() & 0xC0) == 0xC0) add.pop_back();
    }
  }
  if (add.empty()) return;
  text.insert(cursor, add);
  cursor += add.size();
}

bool EditLine::backspace() {
  if (cursor == 0) return false;
  size_t at = cursor - 1;
  while (at > 0 && is_continuation(text[at])) --at;
  text.erase(at, cursor - at);
  cursor = at;
  return true;
}

bool EditLine::erase_forward() {
  if (cursor >= text.size()) return false;
  size_t at = cursor + 1;
  while (at < text.size() && is_continuation(text[at])) ++at;
  text.erase(cursor, at - cursor);
  return true;
}

bool EditLine::move_left() {
  if (cursor == 0) return false;
  --cursor;
  while (cursor > 0 && is_continuation(text[cursor])) --cursor;
  return true;
}

bool EditLine::move_right() {
  if (cursor >= text.size()) return false;
  ++cursor;
  while (cursor < text.size() && is_continuation(text[cursor])) ++cursor;
  return true;
}

bool EditLine::move_home() {
  if (cursor == 0) return false;
  cursor = 0;
  return true;
}

bool EditLine::move_end() {
  if (cursor >= text.size()) return false;
  cursor = text.size();
  return true;
}

bool EditLine::move_word_left() {
  if (cursor == 0) return false;
  while (cursor > 0 && !is_word_byte(text[cursor - 1])) --cursor;
  while (cursor > 0 && is_word_byte(text[cursor - 1])) --cursor;
  return true;
}

bool EditLine::move_word_right() {
  if (cursor >= text.size()) return false;
  while (cursor < text.size() && !is_word_byte(text[cursor])) ++cursor;
  while (cursor < text.size() && is_word_byte(text[cursor])) ++cursor;
  return true;
}

bool EditLine::erase_word_left() {
  if (cursor == 0) return false;
  size_t end = cursor;
  move_word_left();
  text.erase(cursor, end - cursor);
  return true;
}

bool EditLine::erase_to_end() {
  if (cursor >= text.size()) return false;
  text.erase(cursor);
  return true;
}

bool EditLine::erase_to_home() {
  if (cursor == 0) return false;
  text.erase(0, cursor);
  cursor = 0;
  return true;
}

bool EditLine::handle_key(int key, bool word_wise) {
  using namespace aios::hal;
  switch (key) {
    case KeyLeft:      return word_wise ? move_word_left() : move_left();
    case KeyRight:     return word_wise ? move_word_right() : move_right();
    case KeyHome:      return move_home();
    case KeyEnd:       return move_end();
    case KeyBackspace: return word_wise ? erase_word_left() : backspace();
    case KeyDelete:    return erase_forward();
    default:           return false;
  }
}

}  // namespace aios::userland
