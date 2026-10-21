#pragma once
// A line someone is typing, with a cursor in it.
//
// Every prompt in this machine used to be a std::string that text was appended
// to and pop_back()'d off. That is not a line editor: it cannot go back to fix
// the word you forgot at the front, which is the single most common thing a
// person does at a prompt. The cursor lives here, with the line, so the
// terminal prompt, the editor's command line and the start page's prompt all
// behave the same way rather than each growing their own half of it.
#include <cstddef>
#include <string>
#include "aios/hal/keys.hpp"

namespace aios::userland {

struct EditLine {
  std::string text;
  size_t cursor = 0;      // byte offset in `text`; always <= text.size()

  void clear() { text.clear(); cursor = 0; }
  bool empty() const { return text.empty(); }
  size_t size() const { return text.size(); }

  // Puts the cursor at the end, which is where a line that has just been
  // recalled from history or handed in whole wants it.
  void set(const std::string& s) { text = s; cursor = text.size(); }

  void insert(const std::string& s, size_t max_len = 0);
  bool backspace();       // erase before the cursor
  bool erase_forward();   // erase under the cursor (Delete)

  bool move_left();
  bool move_right();
  bool move_home();
  bool move_end();
  // Whole words, for Alt/Ctrl with an arrow: a prompt without these makes
  // fixing a long line a matter of holding an arrow key down.
  bool move_word_left();
  bool move_word_right();
  bool erase_word_left();
  bool erase_to_end();
  bool erase_to_home();

  // Handles the keys that only move or erase, and says whether it did. Text
  // insertion is not here: the caller knows its own length limit and whether
  // the key was even meant for this line.
  bool handle_key(int key, bool word_wise = false);
};

}  // namespace aios::userland
