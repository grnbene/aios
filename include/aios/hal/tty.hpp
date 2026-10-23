#pragma once
// Asking the person at the controlling terminal, as ssh does for a passphrase
// or an unknown host key: the question and the answer go through the terminal
// itself (/dev/tty; on Windows CONIN$/CONOUT$), never stdin/stdout, which may
// be carrying a protocol. A substrate with no terminal to ask answers
// Status::Unsupported, and the caller treats that as "nobody to ask".
#include <string>

#include "aios/core/result.hpp"

namespace aios::hal {

// Writes `question`, reads one line (without its newline). `echo` false hides what is typed and restores the
// terminal afterwards, even when interrupted. Status::NotFound when this process has no controlling terminal.
Result<std::string> tty_ask(const std::string& question, bool echo);

}  // namespace aios::hal
