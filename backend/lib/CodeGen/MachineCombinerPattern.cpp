// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineCombinerPattern.h"

#include <cstddef>
#include <ranges>
#include <string>
#include <unordered_set>
#include <utility>

namespace rcc {

Expected<TargetMachineCombinerInfo>
TargetMachineCombinerInfo::create(std::vector<MachineCombinerPattern> Patterns) {
  TargetMachineCombinerInfo Result(std::move(Patterns));
  std::unordered_set<std::string> Names;
  for (const MachineCombinerPattern &Pattern : Result.Patterns) {
    if (Pattern.Name.empty() || !Pattern.RootOpcode.isValid() || Pattern.Stages == 0 ||
        !Names.insert(Pattern.Name).second) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "machine combiner pattern has an invalid identity or stage mask");
    }
    for (const MachineCombinerPredicate &Predicate : Pattern.Predicates) {
      switch (Predicate.Kind) {
      case MachineCombinerPredicateKind::ImmediateEquals:
      case MachineCombinerPredicateKind::PhysicalRegisterEquals:
      case MachineCombinerPredicateKind::RegisterIsVirtual:
      case MachineCombinerPredicateKind::RegisterIsPhysical:
        break;
      case MachineCombinerPredicateKind::RegisterEquals:
        if (Predicate.Operand == Predicate.OtherOperand) {
          return makeUnexpected(ErrorCode::InvalidArgument,
                                "register equality pattern compares an operand with itself");
        }
        break;
      }
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
