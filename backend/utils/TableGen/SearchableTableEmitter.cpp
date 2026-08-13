// SPDX-License-Identifier: GPL-3.0-or-later

#include "TableGenBackends.h"

#include <algorithm>
#include <ranges>
#include <sstream>
#include <string>

namespace rcc::targetgen {

std::string emitSearchableTables(const NormalizedTargetModel &Model) {
  std::ostringstream Output;
  const auto AssemblyInstructionCount =
      std::ranges::count_if(Model.Instructions, [](const NormalizedInstruction &Instruction) {
        return !Instruction.AssemblyMnemonic.empty();
      });
  Output << "constexpr std::array<AssemblyInstructionInfo, " << AssemblyInstructionCount
         << "> GeneratedAssemblyInstructions = {{\n";
  for (const NormalizedInstruction &Instruction : Model.Instructions) {
    if (Instruction.AssemblyMnemonic.empty())
      continue;
    Output << "    {Opcode::fromValue(" << Instruction.ID << "), ";
    appendQuoted(Output, Instruction.AssemblyMnemonic);
    Output << ", AssemblyFormatID::fromValue(" << Instruction.AssemblyFormat << "), "
           << Instruction.BinaryEncoding << "},\n";
  }
  Output << "}};\n\n";
  return Output.str();
}

} // namespace rcc::targetgen
