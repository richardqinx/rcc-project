// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineCombinerPattern.h"

#include <string>
#include <unordered_set>
#include <utility>

namespace rcc {

Expected<TargetMachineCombinerInfo>
TargetMachineCombinerInfo::create(std::vector<MachineCombinerPattern> Patterns) {
  TargetMachineCombinerInfo Result(std::move(Patterns));
  std::unordered_set<std::string> Names;
  for (const MachineCombinerPattern &Pattern : Result.Patterns) {
    if (Pattern.Name.empty() || !Pattern.RootOpcode.isValid() ||
        !Pattern.ProducerOpcode.isValid() || !Pattern.ReplacementOpcode.isValid() ||
        Pattern.ProducerOperand == 0 || Pattern.AddendOperand == 0 ||
        Pattern.ProducerOperand == Pattern.AddendOperand || !Names.insert(Pattern.Name).second) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "machine combiner pattern has an invalid target pattern");
    }
    Result.MatchTable[Pattern.RootOpcode].push_back(Pattern);
  }
  return Result;
}

std::span<const MachineCombinerPattern>
TargetMachineCombinerInfo::patterns(Opcode RootOpcode) const noexcept {
  const auto Position = MatchTable.find(RootOpcode);
  return Position == MatchTable.end() ? std::span<const MachineCombinerPattern>{}
                                      : std::span<const MachineCombinerPattern>(Position->second);
}

} // namespace rcc
