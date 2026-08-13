// SPDX-License-Identifier: GPL-3.0-or-later

#include "TableGenBackends.h"

#include <sstream>
#include <string>

namespace rcc::targetgen {

std::string emitAssemblyWriter(const NormalizedTargetModel &Model) {
  std::ostringstream Output;
  Output << "constexpr std::array<AssemblyFormatInfo, " << Model.AssemblyFormats.size()
         << "> GeneratedAssemblyFormats = {{\n";
  for (const NormalizedAssemblyFormat &Format : Model.AssemblyFormats) {
    Output << "    {AssemblyFormatID::fromValue(" << Format.ID << "), ";
    appendQuoted(Output, Format.Name);
    Output << "},\n";
  }
  Output << "}};\n\n";
  return Output.str();
}

} // namespace rcc::targetgen
