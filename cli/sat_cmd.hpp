#pragma once
// `sat`: the aios SAT solver driven through the substrate and the fleet.
//   sat devices                                   every device that runs sat.solve, here and on peers
//   sat solve FILE [on=SEL] [conflicts=N] [check_on=SEL]
//   sat fleet FILE on=SEL,SEL,... [budget=N] [depth=D]
// SEL is a device selector: "cpu" (this machine), "remote:relay:<peer>" (a
// fleet peer's CPU), or any device-name / id prefix.
#include <string>
#include <vector>

#include "aios/kernel/kernel.hpp"
#include "aios/kernel/surface.hpp"
#include "out.hpp"

namespace aios::cli {

Result<void> run_sat_command(Kernel& k, Surface& me, Out& o, const std::vector<std::string>& a);

}  // namespace aios::cli
