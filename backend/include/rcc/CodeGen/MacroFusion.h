// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACROFUSION_H
#define RCC_CODEGEN_MACROFUSION_H

#include "rcc/CodeGen/MachineIRTypes.h"
#include "rcc/Support/Error.h"

#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

namespace rcc {

class MachineInstr;
class ScheduleDAG;
class TargetDescription;

enum class MacroFusionInstruction : std::uint8_t {
  First,
  Second,
};

struct MacroFusionOpcodePair final {
  Opcode First;
  Opcode Second;
};

struct MacroFusionImmediatePredicate final {
  MacroFusionInstruction Instruction = MacroFusionInstruction::First;
  std::uint32_t OperandIndex = 0;
  std::int64_t Minimum = 0;
  std::int64_t Maximum = 0;
};

struct MacroFusionRegisterInequality final {
  MacroFusionInstruction Instruction = MacroFusionInstruction::First;
  std::uint32_t LeftOperand = 0;
  std::uint32_t RightOperand = 0;
};

struct MacroFusionImmediateRelation final {
  std::uint32_t FirstOperand = 0;
  std::uint32_t SecondOperand = 0;
};

struct MacroFusionRule final {
  static constexpr std::uint32_t NoOperand = std::numeric_limits<std::uint32_t>::max();

  std::string_view Name;
  std::string_view Feature;
  std::span<const Opcode> FirstOpcodes;
  std::span<const Opcode> SecondOpcodes;
  std::span<const MacroFusionOpcodePair> OpcodePairs;
  std::span<const MacroFusionImmediatePredicate> ImmediatePredicates;
  std::span<const MacroFusionRegisterInequality> RegisterInequalities;
  std::span<const MacroFusionImmediateRelation> ImmediateRelations;
  std::uint32_t FirstDefinitionOperand = 0;
  std::uint32_t SecondUseOperand = 1;
  std::uint32_t SecondDefinitionOperand = 0;
  std::uint32_t CommutableSecondUseOperand = NoOperand;
  bool RequireSingleNonDebugUse = false;
  bool RequireSecondDestinationTie = false;
};

[[nodiscard]] bool matchesMacroFusion(const MacroFusionRule &Rule, const MachineInstr *First,
                                      const MachineInstr &Second) noexcept;

[[nodiscard]] Expected<void> applyMacroFusion(ScheduleDAG &DAG,
                                              std::span<const MacroFusionRule> Rules,
                                              const TargetDescription &Target,
                                              bool EmitDebugOutput);

} // namespace rcc

#endif // RCC_CODEGEN_MACROFUSION_H
