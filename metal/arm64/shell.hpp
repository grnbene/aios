#pragma once
/* Shared shell command dispatch (metal/arm64/main.cpp).
 *
 * One command line in, whatever it prints goes to the console (and, if
 * kconsole_capture_start() is active, also into that buffer). Shared
 * between the local UART/USB-keyboard REPL and the UDP remote console in
 * net.cpp, so a command behaves identically from either.
 */
void dispatch_shell_command(const char* cmd);
