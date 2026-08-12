// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/LivePhysRegs.h"

#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/RegisterMask.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/CodeGen/TargetRegisterInfo.h"

#include <algorithm>
#include <cstddef>
#include <ranges>
#include <vector>

namespace rcc {

namespace {
struct OperandFlagUpdate final {
  MachineOperand *Operand = nullptr;
  RegisterOperandFlags Flags;
};

[[nodiscard]] bool intersects(const LiveRegUnits &Live,
                              const std::vector<PhysicalUnitAccess> &Units) noexcept {
  return std::ranges::any_of(Units, [&Live](const PhysicalUnitAccess &Unit) {
    return Live.intersects(Unit.Unit, Unit.Lanes);
  });
}

void addUnits(LiveRegUnits &Live, const std::vector<PhysicalUnitAccess> &Units) {
  for (const PhysicalUnitAccess &Unit : Units)
    Live.add(Unit.Unit, Unit.Lanes);
}

void removeUnits(LiveRegUnits &Live, const std::vector<PhysicalUnitAccess> &Units) {
  for (const PhysicalUnitAccess &Unit : Units)
    Live.remove(Unit.Unit, Unit.Lanes);
}

[[nodiscard]] OperandFlagUpdate &getFlagUpdate(std::vector<OperandFlagUpdate> &Updates,
                                               MachineOperand &Operand) {
  const auto Position = std::ranges::find_if(
      Updates, [&Operand](const OperandFlagUpdate &Update) { return Update.Operand == &Operand; });
  if (Position != Updates.end())
    return *Position;
  Updates.push_back({&Operand, Operand.registerFlags()});
  return Updates.back();
}

[[nodiscard]] Expected<std::vector<OperandFlagUpdate>>
buildBlockLivenessRepairPlan(MachineBasicBlock &Block, const TargetDescription &Target,
                             const MachineBlockLiveness &Boundary) {
  LiveRegUnits Live = Boundary.PhysicalLiveOut;
  std::vector<OperandFlagUpdate> Updates;
  for (MachineInstr *Instruction = Block.lastInstruction(); Instruction != nullptr;
       Instruction = Instruction->previous()) {
    for (const MachineOperand &Operand : Instruction->operands()) {
      const auto *Mask = Operand.getIf<LiveOutRegisterMaskOperand>();
      if (Mask == nullptr)
        continue;
      if (!Mask->Mask) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "post-RA liveness repair encountered a null live-out mask");
      }
      for (const PhysicalRegisterDesc &Register : Target.registers().physicalRegisters()) {
        if (Mask->Mask->preserves(Register.Reg))
          addPhysicalRegister(Live, Register);
      }
    }

    for (std::size_t Index = 0; Index < Instruction->operandCount(); ++Index) {
      MachineOperand *Operand = Instruction->operandAt(Index);
      if (Operand == nullptr || !Operand->isRegister())
        continue;
      OperandFlagUpdate &Update = getFlagUpdate(Updates, *Operand);
      Update.Flags.clear(RegisterOperandFlag::Dead);
      if (!Operand->isDef())
        continue;
      const Register *Reg = Operand->getIf<Register>();
      if (Reg == nullptr || Reg->isNoRegister())
        continue;
      auto Units = physicalOperandUnits(*Operand, Target);
      if (!Units)
        return std::unexpected(Units.error());
      if (!intersects(Live, *Units))
        Update.Flags.set(RegisterOperandFlag::Dead);
    }

    for (const MachineOperand &Operand : Instruction->operands()) {
      if (Operand.isRegister() && Operand.isDef()) {
        const Register *Reg = Operand.getIf<Register>();
        if (Reg == nullptr || Reg->isNoRegister())
          continue;
        auto Units = physicalOperandUnits(Operand, Target);
        if (!Units)
          return std::unexpected(Units.error());
        removeUnits(Live, *Units);
        continue;
      }
      const auto *Mask = Operand.getIf<RegisterMaskOperand>();
      if (Mask == nullptr)
        continue;
      if (!Mask->Mask) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "post-RA liveness repair encountered a null register mask");
      }
      LiveRegUnits Clobbers;
      for (const PhysicalRegisterDesc &Register : Target.registers().physicalRegisters()) {
        if (Mask->Mask->clobbers(Register.Reg))
          addPhysicalRegister(Clobbers, Register);
      }
      Live.subtract(Clobbers);
    }

    for (std::size_t Index = 0; Index < Instruction->operandCount(); ++Index) {
      MachineOperand *Operand = Instruction->operandAt(Index);
      if (Operand == nullptr || !Operand->isRegister())
        continue;
      OperandFlagUpdate &Update = getFlagUpdate(Updates, *Operand);
      Update.Flags.clear(RegisterOperandFlag::Kill);
      if (!Operand->isUse() || Operand->isDebugUse() ||
          Operand->registerFlags().has(RegisterOperandFlag::Undef)) {
        continue;
      }
      const Register *Reg = Operand->getIf<Register>();
      if (Reg == nullptr || Reg->isNoRegister())
        continue;
      if (isReservedPhysicalRegister(*Reg, Block.function(), Target))
        continue;
      auto Units = physicalOperandUnits(*Operand, Target);
      if (!Units)
        return std::unexpected(Units.error());
      if (!intersects(Live, *Units))
        Update.Flags.set(RegisterOperandFlag::Kill);
      addUnits(Live, *Units);
    }
  }
  if (Live != Boundary.PhysicalLiveIn) {
    return makeUnexpected(ErrorCode::InternalError,
                          "post-RA local liveness repair changed the block boundary");
  }
  return Updates;
}
} // namespace

Expected<bool> repairPostRAMachineBlockLiveness(std::span<MachineBasicBlock *const> Blocks,
                                                const TargetDescription &Target,
                                                const LiveVariablesAnalysis::Result &Liveness) {
  std::vector<OperandFlagUpdate> Updates;
  for (MachineBasicBlock *Block : Blocks) {
    const MachineBlockLiveness *Boundary = Liveness.block(*Block);
    if (Boundary == nullptr) {
      return makeUnexpected(ErrorCode::InternalError,
                            "post-RA scheduler lost a block liveness boundary");
    }
    auto BlockUpdates = buildBlockLivenessRepairPlan(*Block, Target, *Boundary);
    if (!BlockUpdates)
      return std::unexpected(BlockUpdates.error());
    Updates.insert(Updates.end(), BlockUpdates->begin(), BlockUpdates->end());
  }
  bool Changed = false;
  for (const OperandFlagUpdate &Update : Updates) {
    Changed |= Update.Operand->registerFlags() != Update.Flags;
    auto Applied = Update.Operand->setRegisterFlags(Update.Flags);
    if (!Applied)
      return std::unexpected(Applied.error());
  }
  return Changed;
}

} // namespace rcc
