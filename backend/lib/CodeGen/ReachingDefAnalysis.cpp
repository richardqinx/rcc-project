// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/ReachingDefAnalysis.h"

#include "rcc/CodeGen/LiveRegUnits.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/RegisterMask.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/CodeGen/TargetRegisterInfo.h"

#include <algorithm>
#include <cstddef>
#include <ranges>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rcc {
namespace {

using DefinitionSet = ReachingDefAnalysis::DefinitionSet;

struct ReachingDefState final {
  std::unordered_map<RegisterUnitID, DefinitionSet> Registers;
  std::unordered_map<FrameIndex, DefinitionSet> FrameObjects;

  friend bool operator==(const ReachingDefState &, const ReachingDefState &) = default;
};

void sortAndUnique(DefinitionSet &Definitions,
                   const std::unordered_map<const MachineInstr *, std::size_t> &Numbers) {
  std::ranges::sort(Definitions, [&Numbers](const MachineInstr *Left, const MachineInstr *Right) {
    return Numbers.at(Left) < Numbers.at(Right);
  });
  Definitions.erase(std::ranges::unique(Definitions).begin(), Definitions.end());
}

void mergeDefinitions(DefinitionSet &Destination, const DefinitionSet &Source,
                      const std::unordered_map<const MachineInstr *, std::size_t> &Numbers) {
  Destination.insert(Destination.end(), Source.begin(), Source.end());
  sortAndUnique(Destination, Numbers);
}

void mergeState(ReachingDefState &Destination, const ReachingDefState &Source,
                const std::unordered_map<const MachineInstr *, std::size_t> &Numbers) {
  for (const auto &[Unit, Definitions] : Source.Registers)
    mergeDefinitions(Destination.Registers[Unit], Definitions, Numbers);
  for (const auto &[Index, Definitions] : Source.FrameObjects)
    mergeDefinitions(Destination.FrameObjects[Index], Definitions, Numbers);
}

[[nodiscard]] Expected<void> definePhysicalOperand(ReachingDefState &State,
                                                   const MachineOperand &Operand,
                                                   const MachineInstr &Instruction,
                                                   const TargetDescription &Target) {
  auto Units = physicalOperandUnits(Operand, Target);
  if (!Units)
    return std::unexpected(Units.error());
  for (const PhysicalUnitAccess &Access : *Units)
    State.Registers[Access.Unit] = {&Instruction};
  return {};
}

void defineRegisterMask(ReachingDefState &State, const RegisterMask &Mask,
                        const MachineInstr &Instruction, const TargetDescription &Target) {
  for (const PhysicalRegisterDesc &Register : Target.registers().physicalRegisters()) {
    if (!Mask.clobbers(Register.Reg))
      continue;
    for (const RegisterUnitLane &Unit : Register.Units)
      State.Registers[Unit.Unit] = {&Instruction};
  }
}

[[nodiscard]] bool instructionMayStore(const MachineInstr &Instruction) noexcept {
  return std::ranges::any_of(Instruction.memOperands(), [](const MachineMemOperand *Memory) {
    return Memory != nullptr && Memory->isStore();
  });
}

[[nodiscard]] std::unordered_set<FrameIndex> storedFrameObjects(const MachineInstr &Instruction) {
  std::unordered_set<FrameIndex> Result;
  for (const MachineMemOperand *Memory : Instruction.memOperands()) {
    if (Memory == nullptr || !Memory->isStore())
      continue;
    const std::optional<FrameIndex> Index = Memory->description().Base.frameIndex();
    if (Index)
      Result.insert(*Index);
  }
  return Result;
}

[[nodiscard]] Expected<void> transferInstruction(ReachingDefState &State,
                                                 const MachineInstr &Instruction,
                                                 const TargetDescription &Target) {
  if (Instruction.isDebugInstr())
    return {};

  for (const MachineOperand &Operand : Instruction.operands()) {
    if (Operand.isRegister()) {
      const std::optional<Register> Reg = Operand.registerValue();
      if (!Operand.isDef() || !Reg || Reg->isNoRegister())
        continue;
      if (!Reg->isPhysical()) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "reaching-def analysis requires physical registers");
      }
      auto Defined = definePhysicalOperand(State, Operand, Instruction, Target);
      if (!Defined)
        return std::unexpected(Defined.error());
      continue;
    }
    const auto *MaskOperand = Operand.getIf<RegisterMaskOperand>();
    if (MaskOperand == nullptr)
      continue;
    if (!MaskOperand->Mask) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "reaching-def analysis encountered a null register mask");
    }
    defineRegisterMask(State, *MaskOperand->Mask, Instruction, Target);
  }

  if (!instructionMayStore(Instruction))
    return {};
  const std::unordered_set<FrameIndex> ExplicitStores = storedFrameObjects(Instruction);
  for (const MachineOperand &Operand : Instruction.operands()) {
    const auto *Frame = Operand.getIf<FrameIndexOperand>();
    if (Frame == nullptr || (!ExplicitStores.empty() && !ExplicitStores.contains(Frame->Index)))
      continue;
    State.FrameObjects[Frame->Index] = {&Instruction};
  }
  return {};
}

[[nodiscard]] Expected<DefinitionSet>
registerDefinitions(const ReachingDefState &State, const MachineOperand &Operand,
                    const TargetDescription &Target,
                    const std::unordered_map<const MachineInstr *, std::size_t> &Numbers) {
  DefinitionSet Result;
  auto Units = physicalOperandUnits(Operand, Target);
  if (!Units)
    return std::unexpected(Units.error());
  for (const PhysicalUnitAccess &Access : *Units) {
    const auto Position = State.Registers.find(Access.Unit);
    if (Position != State.Registers.end())
      mergeDefinitions(Result, Position->second, Numbers);
  }
  return Result;
}

} // namespace

std::span<const MachineInstr *const>
ReachingDefAnalysis::Result::reachingDefinitions(const MachineOperand &Operand) const noexcept {
  const auto Position = OperandDefinitions.find(&Operand);
  if (Position == OperandDefinitions.end())
    return {};
  return Position->second;
}

std::size_t
ReachingDefAnalysis::Result::instructionNumber(const MachineInstr &Instruction) const noexcept {
  const auto Position = InstructionNumbers.find(&Instruction);
  return Position == InstructionNumbers.end() ? 0 : Position->second;
}

bool ReachingDefAnalysis::Result::invalidate(const MachineFunction &,
                                             const PreservedAnalyses &Preserved) const noexcept {
  return !Preserved.preserves(MachineFunctionAnalysisManager::id<ReachingDefAnalysis>());
}

Expected<ReachingDefAnalysis::Result> ReachingDefAnalysis::run(MachineFunction &Function,
                                                               const TargetDescription &Target,
                                                               MachineFunctionAnalysisManager &) {
  Result Analysis;
  std::size_t Number = 0;
  for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr; Block = Block->next()) {
    for (MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      Analysis.InstructionNumbers.emplace(Instruction, Number++);
    }
  }

  std::unordered_map<const MachineBasicBlock *, ReachingDefState> Inputs;
  std::unordered_map<const MachineBasicBlock *, ReachingDefState> Outputs;
  bool Changed = false;
  do {
    Changed = false;
    for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
         Block = Block->next()) {
      ReachingDefState Input;
      for (MachineBasicBlock *Predecessor : Block->predecessors()) {
        const auto Position = Outputs.find(Predecessor);
        if (Position != Outputs.end())
          mergeState(Input, Position->second, Analysis.InstructionNumbers);
      }

      ReachingDefState Output = Input;
      for (MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
           Instruction = Instruction->next()) {
        auto Transferred = transferInstruction(Output, *Instruction, Target);
        if (!Transferred)
          return std::unexpected(Transferred.error());
      }
      if (Inputs[Block] != Input || Outputs[Block] != Output) {
        Inputs[Block] = std::move(Input);
        Outputs[Block] = std::move(Output);
        Changed = true;
      }
    }
  } while (Changed);

  for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr; Block = Block->next()) {
    ReachingDefState State = Inputs[Block];
    for (MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      for (const MachineOperand &Operand : Instruction->operands()) {
        if (Operand.isRegister()) {
          const std::optional<Register> Reg = Operand.registerValue();
          if (Operand.isDef() || !Reg || Reg->isNoRegister())
            continue;
          if (!Reg->isPhysical()) {
            return makeUnexpected(ErrorCode::VerificationError,
                                  "reaching-def analysis requires physical registers");
          }
          auto Definitions =
              registerDefinitions(State, Operand, Target, Analysis.InstructionNumbers);
          if (!Definitions)
            return std::unexpected(Definitions.error());
          Analysis.OperandDefinitions.emplace(&Operand, std::move(*Definitions));
          continue;
        }
        const auto *Frame = Operand.getIf<FrameIndexOperand>();
        if (Frame == nullptr)
          continue;
        const auto Position = State.FrameObjects.find(Frame->Index);
        Analysis.OperandDefinitions.emplace(
            &Operand, Position == State.FrameObjects.end() ? DefinitionSet{} : Position->second);
      }
      auto Transferred = transferInstruction(State, *Instruction, Target);
      if (!Transferred)
        return std::unexpected(Transferred.error());
    }
  }
  return Analysis;
}

} // namespace rcc
