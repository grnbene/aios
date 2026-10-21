#pragma once
// The familiar command layer: a Unix-shaped shell an intelligence (or a human)
// already knows how to drive, mapped onto AOS primitives. NOT part of the
// kernel — it lives entirely in the command surface, above the Surface API.
// The "filesystem" is the persistent root (the AOS volume on bare metal); ps
// is entities; free is the memory fabric; uname is the topology.
#include <string>
#include <vector>
#include "cli.hpp"

namespace aios::cli {
// Handles a familiar command; returns true if it was one, false to let the
// native AOS commands try. Throws the CLI's Fail on error, like the others.
bool shell_command(Session& s, Out& o, Surface& me, const std::vector<std::string>& a);
void shell_help(Out& o);
}  // namespace aios::cli
