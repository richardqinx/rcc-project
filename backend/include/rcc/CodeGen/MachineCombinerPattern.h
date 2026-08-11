// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINECOMBINERPATTERN_H
#define RCC_CODEGEN_MACHINECOMBINERPATTERN_H

#include "rcc/CodeGen/MachineIRTypes.h"
#include "rcc/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace rcc {

enum class MachineCombinerStage : std::uint8_t {
  PreLegalizer,
  SelectedSSA,
  PreRegisterAllocation,
  PostRegisterAllocation,
  PostFrame,
};

using MachineCombinerStageMask = std::uint8_t;

[[nodiscard]] constexpr MachineCombinerStageMask
machineCombinerStageMask(MachineCombinerStage Stage) noexcept {
  return static_cast<MachineCombinerStageMask>(1U << static_cast<std::uint8_t>(Stage));
}

enum class MachineCombinerPredicateKind : std::uint8_t {
  ImmediateEquals,
  RegisterEquals,
  PhysicalRegisterEquals,
  RegisterIsVirtual,
  RegisterIsPhysical,
};

struct MachineCombinerPredicate final {
  MachineCombinerPredicateKind Kind = MachineCombinerPredicateKind::ImmediateEquals;
  std::size_t Operand = 0;
  std::size_t OtherOperand = 0;
  std::int64_t Value = 0;
};

enum class MachineCombinerActionKind : std::uint8_t {
  ReplaceDefWithOperandAndErase,
};

struct MachineCombinerAction final {
  MachineCombinerActionKind Kind = MachineCombinerActionKind::ReplaceDefWithOperandAndErase;
  std::size_t DefinitionOperand = 0;
  std::size_t SourceOperand = 0;
};

struct MachineCombinerPattern final {
  std::string Name;
  Opcode RootOpcode;
  MachineCombinerStageMask Stages = 0;
  std::vector<MachineCombinerPredicate> Predicates;
  MachineCombinerAction Action;
};

class TargetMachineCombinerInfo final {
public:
  [[nodiscard]] static Expected<TargetMachineCombinerInfo>
  create(std::vector<MachineCombinerPattern> Patterns);

  [[nodiscard]] std::span<const MachineCombinerPattern> patterns(Opcode RootOpcode) const noexcept;

private:
  explicit TargetMachineCombinerInfo(std::vector<MachineCombinerPattern> Patterns)
      : Patterns(std::move(Patterns)) {}

  std::vector<MachineCombinerPattern> Patterns;
  std::unordered_map<Opcode, std::vector<MachineCombinerPattern>> MatchTable;
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINECOMBINERPATTERN_H
