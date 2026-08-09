// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/RegisterScavenger.h"

#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/RegisterMask.h"
#include "rcc/CodeGen/TargetDescription.h"

#include <algorithm>
#include <optional>
#include <ranges>
#include <unordered_map>
#include <vector>

namespace rcc {
namespace {

[[nodiscard]] std::optional<PhysicalRegister>
physicalRegister(const MachineOperand &Operand) noexcept {
  const Register *Reg = Operand.getIf<Register>();
  if (Reg == nullptr || !Reg->isPhysical())
    return std::nullopt;
  return Reg->physicalRegister();
}

[[nodiscard]] bool maskClobbersRegisterUnits(const RegisterMask &Mask, PhysicalRegister Reg,
                                             const TargetDescription &Target) noexcept {
  return std::ranges::any_of(Target.registers().physicalRegisters(),
                             [&](const PhysicalRegisterDesc &Description) {
                               return Target.registers().regsOverlap(Description.Reg, Reg) &&
                                      Mask.clobbers(Description.Reg);
                             });
}

[[nodiscard]] bool maskPreservesRegisterUnits(const RegisterMask &Mask, PhysicalRegister Reg,
                                              const TargetDescription &Target) noexcept {
  return std::ranges::any_of(Target.registers().physicalRegisters(),
                             [&](const PhysicalRegisterDesc &Description) {
                               return Target.registers().regsOverlap(Description.Reg, Reg) &&
                                      Mask.preserves(Description.Reg);
                             });
}

[[nodiscard]] bool isUnavailable(const MachineFunction &Function, const TargetDescription &Target,
                                 PhysicalRegister Reg) noexcept {
  const PhysicalRegisterDesc *Description = Target.registers().physicalRegister(Reg);
  if (Description == nullptr || Description->IsConstant || Description->IsArtificial ||
      Description->IsReserved || Target.registers().isReservedRegister(Reg)) {
    return true;
  }
  return std::ranges::any_of(
      Function.registerInfo().reservedRegisters(),
      [&](PhysicalRegister Reserved) { return Target.registers().regsOverlap(Reg, Reserved); });
}

} // namespace

bool RegisterScavenger::instructionUses(const MachineInstr &Instruction,
                                        PhysicalRegister Reg) const noexcept {
  for (const MachineOperand &Operand : Instruction.operands()) {
    const auto Physical = physicalRegister(Operand);
    if (Physical && Target->registers().regsOverlap(*Physical, Reg))
      return true;
  }
  return false;
}

Expected<bool> RegisterScavenger::isLiveBefore(const MachineInstr &Instruction,
                                               PhysicalRegister Reg) const {
  const MachineBasicBlock *Block = Instruction.parent();
  if (Block == nullptr || &Instruction.function() != Function || &Block->function() != Function) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "register scavenging anchor is detached or belongs to another function");
  }

  std::vector<const MachineBasicBlock *> Blocks;
  std::unordered_map<const MachineBasicBlock *, std::size_t> BlockIndices;
  for (const MachineBasicBlock *Current = Function->firstBlock(); Current != nullptr;
       Current = Current->next()) {
    BlockIndices.emplace(Current, Blocks.size());
    Blocks.push_back(Current);
  }

  if (!BlockIndices.contains(Block)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "register scavenging anchor block is not linked in its function");
  }
  for (const MachineBasicBlock *Current : Blocks) {
    for (const MachineSuccessor &Successor : Current->successors()) {
      if (Successor.Block == nullptr || !BlockIndices.contains(Successor.Block)) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "register scavenging CFG has an unknown successor");
      }
    }
    for (const MachineInstr *CurrentInstruction = Current->firstInstruction();
         CurrentInstruction != nullptr; CurrentInstruction = CurrentInstruction->next()) {
      for (const MachineOperand &Operand : CurrentInstruction->operands()) {
        if (const auto *Mask = Operand.getIf<RegisterMaskOperand>();
            Mask != nullptr && !Mask->Mask) {
          return makeUnexpected(ErrorCode::VerificationError,
                                "register scavenging found a null register mask");
        }
        if (const auto *Mask = Operand.getIf<LiveOutRegisterMaskOperand>();
            Mask != nullptr && !Mask->Mask) {
          return makeUnexpected(ErrorCode::VerificationError,
                                "register scavenging found a null live-out register mask");
        }
      }
    }
  }

  const auto Transfer = [&](const MachineBasicBlock &Current, bool LiveOut) noexcept {
    bool Live = LiveOut;
    for (const MachineInstr *CurrentInstruction = Current.lastInstruction();
         CurrentInstruction != nullptr; CurrentInstruction = CurrentInstruction->previous()) {
      for (const MachineOperand &Operand : CurrentInstruction->operands()) {
        if (const auto *Mask = Operand.getIf<RegisterMaskOperand>();
            Mask != nullptr && maskClobbersRegisterUnits(*Mask->Mask, Reg, *Target)) {
          Live = false;
        }
        const auto Physical = physicalRegister(Operand);
        if (Physical && Operand.isDef() && Target->registers().regsOverlap(*Physical, Reg)) {
          Live = false;
        }
      }
      for (const MachineOperand &Operand : CurrentInstruction->operands()) {
        if (const auto *Mask = Operand.getIf<LiveOutRegisterMaskOperand>();
            Mask != nullptr && maskPreservesRegisterUnits(*Mask->Mask, Reg, *Target)) {
          Live = true;
        }
        const auto Physical = physicalRegister(Operand);
        if (Physical && Operand.isUse() && !Operand.isDebugUse() &&
            !Operand.registerFlags().has(RegisterOperandFlag::Undef) &&
            Target->registers().regsOverlap(*Physical, Reg)) {
          Live = true;
        }
      }
    }
    return Live;
  };

  std::vector<bool> LiveIns(Blocks.size(), false);
  for (std::size_t Index = 0; Index < Blocks.size(); ++Index) {
    for (const MachineBasicBlockLiveIn &LiveIn : Blocks[Index]->liveIns()) {
      if (Target->registers().regsOverlap(LiveIn.PhysReg, Reg))
        LiveIns[Index] = true;
    }
  }
  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (std::size_t Reverse = Blocks.size(); Reverse != 0; --Reverse) {
      const std::size_t Index = Reverse - 1U;
      bool LiveOut = false;
      for (const MachineSuccessor &Successor : Blocks[Index]->successors()) {
        LiveOut |= LiveIns[BlockIndices.at(Successor.Block)];
      }
      const bool NewLiveIn = LiveIns[Index] || Transfer(*Blocks[Index], LiveOut);
      if (NewLiveIn != LiveIns[Index]) {
        LiveIns[Index] = NewLiveIn;
        Changed = true;
      }
    }
  }

  bool Live = false;
  for (const MachineSuccessor &Successor : Block->successors()) {
    Live |= LiveIns[BlockIndices.at(Successor.Block)];
  }

  for (const MachineInstr *Current = Block->lastInstruction(); Current != nullptr;
       Current = Current->previous()) {
    for (const MachineOperand &Operand : Current->operands()) {
      if (const auto *Mask = Operand.getIf<RegisterMaskOperand>();
          Mask != nullptr && maskClobbersRegisterUnits(*Mask->Mask, Reg, *Target)) {
        Live = false;
      }
      const auto Physical = physicalRegister(Operand);
      if (Physical && Operand.isDef() && Target->registers().regsOverlap(*Physical, Reg))
        Live = false;
    }
    for (const MachineOperand &Operand : Current->operands()) {
      if (const auto *Mask = Operand.getIf<LiveOutRegisterMaskOperand>();
          Mask != nullptr && maskPreservesRegisterUnits(*Mask->Mask, Reg, *Target)) {
        Live = true;
      }
      const auto Physical = physicalRegister(Operand);
      if (Physical && Operand.isUse() && !Operand.isDebugUse() &&
          !Operand.registerFlags().has(RegisterOperandFlag::Undef) &&
          Target->registers().regsOverlap(*Physical, Reg))
        Live = true;
    }
    if (Current == &Instruction)
      return Live;
  }
  return true;
}

Expected<ScavengedRegister>
RegisterScavenger::scavenge(const MachineInstr &Instruction,
                            std::span<const PhysicalRegister> Candidates) const {
  std::optional<PhysicalRegister> SpillCandidate;
  for (const PhysicalRegister Candidate : Candidates) {
    if (!Candidate.isValid() || isUnavailable(*Function, *Target, Candidate) ||
        instructionUses(Instruction, Candidate)) {
      continue;
    }
    if (!SpillCandidate)
      SpillCandidate = Candidate;
    auto Live = isLiveBefore(Instruction, Candidate);
    if (!Live)
      return std::unexpected(Live.error());
    if (!*Live)
      return ScavengedRegister{Candidate, false};
  }
  if (SpillCandidate)
    return ScavengedRegister{*SpillCandidate, true};
  return makeUnexpected(ErrorCode::RegisterAllocationFailed,
                        "register scavenging found no usable physical register");
}

} // namespace rcc
