// SPDX-License-Identifier: GPL-3.0-or-later

#include "TableGenBackends.h"

#include <sstream>
#include <string>

namespace rcc::targetgen {

std::string emitSubtarget(const NormalizedTargetModel &Model) {
  std::ostringstream Output;
  Output << "constexpr std::array<SchedulingClassInfo, " << Model.SchedulingClasses.size()
         << "> GeneratedSchedulingClasses = {{\n";
  for (const NormalizedSchedulingClass &Class : Model.SchedulingClasses) {
    Output << "    {SchedulingClassID::fromValue(" << Class.ID << "), ";
    appendQuoted(Output, Class.Name);
    Output << ", " << Class.MicroOps << ", " << Class.Latency << "},\n";
  }
  Output << "}};\n\n";
  return Output.str();
}

} // namespace rcc::targetgen
