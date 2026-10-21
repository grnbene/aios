#pragma once
// Key codes shared by every input backend and everything that consumes input.
//
// Printable ASCII passes through as itself, and the handful of control keys
// ASCII already names (backspace, tab, enter, escape) keep their ASCII values,
// so a backend that only has characters needs no translation table at all.
// Everything ASCII has no room for is numbered from 0x1000 up, where no
// printable character can ever collide with it.
#include <cstdint>

namespace aios::hal {

enum Key : int {
  KeyNone = -1,
  KeyBackspace = 8,
  KeyTab = 9,
  KeyEnter = 13,
  KeyEscape = 27,
  KeyDelete = 127,

  KeyUp = 0x1000,
  KeyDown,
  KeyLeft,
  KeyRight,
  KeyHome,
  KeyEnd,
  KeyPageUp,
  KeyPageDown,

  KeyF1 = 0x1010,
  KeyF2,
  KeyF3,
  KeyF4,
  KeyF5,
  KeyF6,
  KeyF7,
  KeyF8,
  KeyF9,
  KeyF10,
  KeyF11,
  KeyF12,
};

// Ctrl+<letter> arrives as the ASCII control code that letter names, which is
// what every input backend here sends. Named because a desktop that spells
// copy as `3` is a desktop nobody can read.
enum CtrlKey : int {
  KeyCtrlA = 1, KeyCtrlB = 2, KeyCtrlC = 3, KeyCtrlD = 4, KeyCtrlE = 5,
  KeyCtrlF = 6, KeyCtrlG = 7, KeyCtrlK = 11, KeyCtrlL = 12, KeyCtrlN = 14,
  KeyCtrlO = 15, KeyCtrlP = 16, KeyCtrlQ = 17, KeyCtrlR = 18, KeyCtrlS = 19,
  KeyCtrlT = 20, KeyCtrlU = 21, KeyCtrlV = 22, KeyCtrlW = 23, KeyCtrlX = 24,
  KeyCtrlY = 25,
  KeyCtrlZ = 26,
};

// A key that stands for a character someone typed, as opposed to a command.
inline bool key_is_text(int k) { return k >= 32 && k < 127; }
inline bool key_is_special(int k) { return k >= KeyUp; }

// Mouse button bits, in the order the PS/2 packet reports them.
enum MouseButton : int { MouseLeft = 1, MouseRight = 2, MouseMiddle = 4 };

}  // namespace aios::hal
