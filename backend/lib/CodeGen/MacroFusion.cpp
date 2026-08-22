// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MacroFusion.h"

#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/ScheduleDAG.h"
#include "rcc/CodeGen/TargetDescription.h"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <optional>
#include <ranges>
#include <span>
#include <vector>

namespace rcc {
namespace {

[[nodiscard]] const MachineInstr *instructionFor(MacroFusionInstruction Side,
                                                 const MachineInstr *First,
                                                 const MachineInstr &Second) noexcept {
  return Side == MacroFusionInstruction::First ? First : &Second;
}

[[nodiscard]] std::optional<Register> registerAt(const MachineInstr &Instruction,
                                                 std::uint32_t OperandIndex) noexcept {
  const MachineOperand *Operand = Instruction.operandAt(OperandIndex);
  return Operand == nullptr ? std::nullopt : Operand->registerValue();
}

[[nodiscard]] const std::int64_t *immediateAt(const MachineInstr &Instruction,
                                              std::uint32_t OperandIndex) noexcept {
  const MachineOperand *Operand = Instruction.operandAt(OperandIndex);
  return Operand == nullptr ? nullptr : Operand->getIf<std::int64_t>();
}

[[nodiscard]] bool containsOpcode(std::span<const Opcode> Opcodes, Opcode Value) noexcept {
  return std::ranges::contains(Opcodes, Value);
}

[[nodiscard]] bool matchesLink(const MacroFusionRule &Rule, const MachineInstr &First,
                               const MachineInstr &Second) noexcept {
  const std::optional<Register> FirstDefinition = registerAt(First, Rule.FirstDefinitionOperand);
  const std::optional<Register> SecondUse = registerAt(Second, Rule.SecondUseOperand);
  const std::optional<Register> AlternateSecondUse =
      Rule.CommutableSecondUseOperand == MacroFusionRule::NoOperand
          ? std::nullopt
          : registerAt(Second, Rule.CommutableSecondUseOperand);
  if (!FirstDefinition ||
      (*FirstDefinition != SecondUse && *FirstDefinition != AlternateSecondUse)) {
    return false;
  }

  if (Rule.RequireSingleNonDebugUse && FirstDefinition->isVirtual() &&
      !Second.function().registerInfo().hasOneUse(*FirstDefinition, false)) {
    return false;
  }

  if (!Rule.RequireSecondDestinationTie)
    return true;
  const std::optional<Register> SecondDefinition = registerAt(Second, Rule.SecondDefinitionOperand);
  if (!SecondDefinition || SecondDefinition->isVirtual())
    return SecondDefinition.has_value();
  return *SecondDefinition == SecondUse || *SecondDefinition == AlternateSecondUse;
}

[[nodiscard]] bool isFusionCandidateDependency(ScheduleDependencyKind Kind) noexcept {
  return Kind != ScheduleDependencyKind::Anti && Kind != ScheduleDependencyKind::Output &&
         Kind != ScheduleDependencyKind::Cluster;
}

[[nodiscard]] std::string_view opcodeName(const TargetDescription &Target, Opcode Value) noexcept {
  const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Value);
  return Descriptor == nullptr ? std::string_view{"<unknown>"} : Descriptor->Name;
}

} // namespace

bool matchesMacroFusion(const MacroFusionRule &Rule, const MachineInstr *First,
                        const MachineInstr &Second) noexcept {
  if (!containsOpcode(Rule.SecondOpcodes, Second.opcode()))
    return false;
  if (First == nullptr)
    return true;
  if (!containsOpcode(Rule.FirstOpcodes, First->opcode()))
    return false;
  if (!Rule.OpcodePairs.empty() &&
      std::ranges::none_of(Rule.OpcodePairs, [First, &Second](MacroFusionOpcodePair Pair) {
        return Pair.First == First->opcode() && Pair.Second == Second.opcode();
      })) {
    return false;
  }

  for (const MacroFusionImmediatePredicate &Predicate : Rule.ImmediatePredicates) {
    const MachineInstr *Instruction = instructionFor(Predicate.Instruction, First, Second);
    const std::int64_t *Value =
        Instruction == nullptr ? nullptr : immediateAt(*Instruction, Predicate.OperandIndex);
    if (Value == nullptr || *Value < Predicate.Minimum || *Value > Predicate.Maximum)
      return false;
  }
  for (const MacroFusionRegisterInequality &Predicate : Rule.RegisterInequalities) {
    const MachineInstr *Instruction = instructionFor(Predicate.Instruction, First, Second);
    if (Instruction == nullptr)
      return false;
    const std::optional<Register> Left = registerAt(*Instruction, Predicate.LeftOperand);
    const std::optional<Register> Right = registerAt(*Instruction, Predicate.RightOperand);
    if (!Left || !Right || *Left == *Right)
      return false;
  }
  for (const MacroFusionImmediateRelation &Predicate : Rule.ImmediateRelations) {
    const std::int64_t *FirstValue = immediateAt(*First, Predicate.FirstOperand);
    const std::int64_t *SecondValue = immediateAt(Second, Predicate.SecondOperand);
    if (FirstValue == nullptr || SecondValue == nullptr || *FirstValue > *SecondValue)
      return false;
  }
  return matchesLink(Rule, *First, Second);
}

Expected<void> applyMacroFusion(ScheduleDAG &DAG, std::span<const MacroFusionRule> Rules,
                                const TargetDescription &Target, bool EmitDebugOutput) {
  if (Rules.empty())
    return {};
  for (ScheduleUnit &Anchor : DAG.units()) {
    if (Anchor.Instructions.size() != 1 || Anchor.ClusterPredecessor || Anchor.ClusterSuccessor)
      continue;
    const MachineInstr &Second = *Anchor.Instructions.front();
    if (std::ranges::none_of(Rules, [&Second](const MacroFusionRule &Rule) {
          return matchesMacroFusion(Rule, nullptr, Second);
        })) {
      continue;
    }
    const std::vector<ScheduleDependency> Predecessors = Anchor.Predecessors;
    for (const ScheduleDependency &Dependency : Predecessors) {
      if (!isFusionCandidateDependency(Dependency.Kind))
        continue;
      const ScheduleUnit *FirstUnit = DAG.unit(Dependency.Source);
      if (FirstUnit == nullptr || FirstUnit->Instructions.size() != 1 ||
          FirstUnit->ClusterPredecessor || FirstUnit->ClusterSuccessor) {
        continue;
      }
      const MachineInstr &First = *FirstUnit->Instructions.front();
      if (std::ranges::none_of(Rules, [&First, &Second](const MacroFusionRule &Rule) {
            return matchesMacroFusion(Rule, &First, Second);
          })) {
        continue;
      }
      auto Fused = DAG.fuseInstructionPair(FirstUnit->ID, Anchor.ID);
      if (!Fused)
        return std::unexpected(Fused.error());
      if (!*Fused)
        continue;
      if (EmitDebugOutput) {
        std::cerr << "Macro fuse: SU(" << FirstUnit->ID << ") - SU(" << Anchor.ID << ") /  "
                  << opcodeName(Target, First.opcode()) << " - "
                  << opcodeName(Target, Second.opcode()) << '\n';
      }
      break;
    }
  }
  return {};
}

} // namespace rcc
