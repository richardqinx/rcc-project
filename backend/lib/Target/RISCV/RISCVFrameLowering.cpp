// SPDX-License-Identifier: GPL-3.0-or-later

#include "GISel/RISCVCallLowering.h"
#include "RISCV.h"
#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineCFIInfo.h"
#include "rcc/CodeGen/MachineFrameInfo.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/RegisterMask.h"
#include "rcc/CodeGen/RegisterScavenger.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/Target/RISCV/RISCVInstructions.h"
#include "rcc/Target/RISCV/RISCVRegisters.h"
#include "rcc/Target/RISCV/RISCVTarget.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rcc::riscv {
namespace {

constexpr std::uint64_t StackAlignmentBytes = 16;
constexpr std::int64_t MaxSigned11 = 1023;
constexpr std::int64_t MinSigned12 = -2048;
constexpr std::int64_t MaxSigned12 = 2047;

[[nodiscard]] constexpr bool fitsSigned12(std::int64_t Value) noexcept {
  return Value >= MinSigned12 && Value <= MaxSigned12;
}

[[nodiscard]] constexpr bool hasStackObjectFlag(const StackObjectDesc &Object,
                                                CallStackObjectFlag Flag) noexcept {
  return (Object.TargetFlags & static_cast<std::uint64_t>(Flag)) != 0;
}

[[nodiscard]] constexpr bool isOutgoingCallObject(const StackObjectDesc &Object) noexcept {
  return Object.Kind == StackObjectKind::Target &&
         (hasStackObjectFlag(Object, CallStackObjectFlag::OutgoingArgument) ||
          hasStackObjectFlag(Object, CallStackObjectFlag::ByValCopy));
}

[[nodiscard]] Expected<std::int64_t> checkedAdd(std::int64_t Left, std::int64_t Right) {
  std::int64_t Result = 0;
  if (__builtin_add_overflow(Left, Right, &Result))
    return makeUnexpected(ErrorCode::FrameLayoutFailed, "RISC-V frame offset overflows int64");
  return Result;
}

[[nodiscard]] Expected<std::uint64_t> alignTo(std::uint64_t Value, std::uint64_t Align) {
  if (Align == 0 || !std::has_single_bit(Align))
    return makeUnexpected(ErrorCode::InvalidArgument, "RISC-V frame alignment is invalid");
  const std::uint64_t Mask = Align - 1U;
  if (Value > std::numeric_limits<std::uint64_t>::max() - Mask)
    return makeUnexpected(ErrorCode::FrameLayoutFailed, "RISC-V frame size overflows uint64");
  return (Value + Mask) & ~Mask;
}

[[nodiscard]] Expected<std::int64_t> immediateValue(const MachineOperand &Operand) {
  if (const auto *Immediate = Operand.getIf<std::int64_t>())
    return *Immediate;
  const auto *Integer = Operand.getIf<IntegerImmediateOperand>();
  if (Integer == nullptr || Integer->Value.bitWidth() == 0) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V frame operand requires an integer immediate");
  }
  if (Integer->Value.bitWidth() > 64) {
    return makeUnexpected(ErrorCode::Unsupported, "RISC-V frame immediate must fit in 64 bits");
  }
  std::uint64_t Raw = Integer->Value.words().empty() ? 0 : Integer->Value.words().front();
  const std::uint32_t Width = Integer->Value.bitWidth();
  if (Width < 64 && (Raw & (std::uint64_t{1} << (Width - 1U))) != 0)
    Raw |= ~std::uint64_t{0} << Width;
  return std::bit_cast<std::int64_t>(Raw);
}

struct CallFrameRequirement final {
  std::int64_t Amount = 0;
  std::int64_t PreparedBytes = 0;
  std::int64_t AlignedAmount = 0;
};

[[nodiscard]] Expected<CallFrameRequirement> callFrameRequirement(const MachineInstr &Instruction) {
  if (Instruction.operandCount() < 2) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V call-frame pseudo requires amount and prepared-byte operands");
  }
  for (std::size_t Index = 2; Index < Instruction.operandCount(); ++Index) {
    const MachineOperand *Operand = Instruction.operandAt(Index);
    if (Operand == nullptr || !Operand->isRegister() || !Operand->isImplicit()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V call-frame pseudo has an unexpected explicit operand");
    }
  }
  auto Amount = immediateValue(*Instruction.operandAt(0));
  auto PreparedBytes = immediateValue(*Instruction.operandAt(1));
  if (!Amount || !PreparedBytes || *Amount < 0 || *PreparedBytes < 0) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V call-frame amount or prepared-byte count is invalid");
  }
  auto Aligned = alignTo(static_cast<std::uint64_t>(*Amount), StackAlignmentBytes);
  if (!Aligned || *Aligned > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return makeUnexpected(ErrorCode::FrameLayoutFailed, "RISC-V call-frame amount is too large");
  }
  return CallFrameRequirement{*Amount, *PreparedBytes, static_cast<std::int64_t>(*Aligned)};
}

[[nodiscard]] Expected<std::int64_t> callFrameAmount(const MachineInstr &Instruction) {
  auto Requirement = callFrameRequirement(Instruction);
  if (!Requirement)
    return std::unexpected(Requirement.error());
  return Requirement->AlignedAmount;
}

[[nodiscard]] bool hasVariableObject(const MachineFunction &Function) noexcept {
  return std::ranges::any_of(Function.frameInfo().objects(), [](const StackObject &Object) {
    return !Object.isTombstone() && !Object.description()->IsDead && Object.isVariableSized();
  });
}

[[nodiscard]] std::uint64_t requiredStackAlignment(const MachineFunction &Function) noexcept {
  std::uint64_t Result = StackAlignmentBytes;
  Result =
      std::max(Result, Function.frameInfo().state().MaxAlignment.value_or(Alignment{}).bytes());
  for (const StackObject &Object : Function.frameInfo().objects()) {
    if (!Object.isTombstone() && !Object.description()->IsDead)
      Result = std::max(Result, Object.description()->Align.bytes());
  }
  return Result;
}

[[nodiscard]] bool requiresStackRealignment(const MachineFunction &Function) noexcept {
  return requiredStackAlignment(Function) > StackAlignmentBytes;
}

[[nodiscard]] bool requiresFramePointer(const MachineFunction &Function) noexcept {
  const MachineFrameState &State = Function.frameInfo().state();
  if (State.IsFrameAddressTaken || State.HasOpaqueSPAdjustment || hasVariableObject(Function) ||
      requiresStackRealignment(Function))
    return true;
  if (Function.frameInfo().maxCallFrameSize().value_or(0) >
      static_cast<std::uint64_t>(MaxSigned12)) {
    return true;
  }
  switch (State.FramePointerPolicy) {
  case MachineFramePointerPolicy::All:
    return true;
  case MachineFramePointerPolicy::NonLeaf:
  case MachineFramePointerPolicy::NonLeafNoReserve:
    return State.HasCalls;
  case MachineFramePointerPolicy::None:
  case MachineFramePointerPolicy::Reserved:
  case MachineFramePointerPolicy::Count:
    return false;
  }
  return false;
}

[[nodiscard]] bool requiresBasePointer(const MachineFunction &Function) noexcept {
  const MachineFrameInfo &Frame = Function.frameInfo();
  const bool HasReservedCallFrame =
      !hasVariableObject(Function) && !Frame.state().HasOpaqueSPAdjustment;
  return requiresStackRealignment(Function) &&
         (hasVariableObject(Function) ||
          (!HasReservedCallFrame && Frame.maxCallFrameSize().value_or(0) != 0));
}

[[nodiscard]] Expected<void> validateFrameRegisterUses(const MachineFunction &Function,
                                                       const TargetDescription &Target) {
  std::vector<PhysicalRegister> FrameRegisters;
  if (requiresFramePointer(Function)) {
    auto FP = physicalRegister(PhysicalRegisterID::X8);
    if (!FP)
      return std::unexpected(FP.error());
    FrameRegisters.push_back(*FP);
  }
  if (requiresBasePointer(Function)) {
    auto BP = physicalRegister(PhysicalRegisterID::X9);
    if (!BP)
      return std::unexpected(BP.error());
    FrameRegisters.push_back(*BP);
  }
  for (const MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
       Block = Block->next()) {
    for (const MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      for (const MachineOperand &Operand : Instruction->operands()) {
        const Register *Reg = Operand.getIf<Register>();
        if (Reg == nullptr || !Reg->isPhysical())
          continue;
        const auto Physical = Reg->physicalRegister();
        if (Physical && std::ranges::any_of(FrameRegisters, [&](PhysicalRegister FrameReg) {
              return Target.registers().regsOverlap(*Physical, FrameReg);
            })) {
          return makeUnexpected(
              ErrorCode::VerificationError,
              "RISC-V post-allocation code uses a register reserved for frame lowering");
        }
      }
    }
  }
  return {};
}

[[nodiscard]] bool hasReservedCallFrame(const MachineFunction &Function) noexcept {
  const MachineFrameState &State = Function.frameInfo().state();
  return !hasVariableObject(Function) && !State.HasOpaqueSPAdjustment;
}

[[nodiscard]] Expected<void> computeMaxCallFrameSize(MachineFunction &Function) {
  std::uint64_t Maximum = Function.frameInfo().maxCallFrameSize().value_or(0);
  if (Maximum == std::numeric_limits<std::uint32_t>::max())
    Maximum = 0;
  bool SawPseudo = false;
  for (const MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
       Block = Block->next()) {
    std::optional<CallFrameRequirement> Active;
    for (const MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      const bool IsDown =
          Instruction->opcode() == instructionOpcode(InstructionOpcode::ADJCALLSTACKDOWN);
      const bool IsUp =
          Instruction->opcode() == instructionOpcode(InstructionOpcode::ADJCALLSTACKUP);
      if (!IsDown && !IsUp)
        continue;
      SawPseudo = true;
      auto Requirement = callFrameRequirement(*Instruction);
      if (!Requirement)
        return std::unexpected(Requirement.error());
      if (IsDown) {
        if (Active) {
          return makeUnexpected(ErrorCode::VerificationError,
                                "RISC-V call-frame sequences must not be nested");
        }
        Active = *Requirement;
        Maximum = std::max(Maximum, static_cast<std::uint64_t>(Requirement->Amount));
        continue;
      }
      if (!Active) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "RISC-V call-frame destroy has no matching setup");
      }
      if (Active->Amount != Requirement->Amount ||
          Active->PreparedBytes != Requirement->PreparedBytes) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "RISC-V call-frame setup and destroy requirements differ");
      }
      Active.reset();
    }
    if (Active) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V call-frame setup has no matching destroy");
    }
  }
  if (!SawPseudo && !Function.frameInfo().maxCallFrameSize())
    return {};
  return Function.frameInfo().setMaxCallFrameSize(Maximum);
}

constexpr std::array LP64SaveOrder{
    PhysicalRegisterID::X1,  PhysicalRegisterID::X8,  PhysicalRegisterID::X9,
    PhysicalRegisterID::X18, PhysicalRegisterID::X19, PhysicalRegisterID::X20,
    PhysicalRegisterID::X21, PhysicalRegisterID::X22, PhysicalRegisterID::X23,
    PhysicalRegisterID::X24, PhysicalRegisterID::X25, PhysicalRegisterID::X26,
    PhysicalRegisterID::X27,
};

constexpr std::array LP64DSaveOrder{
    PhysicalRegisterID::X1,    PhysicalRegisterID::X8,    PhysicalRegisterID::X9,
    PhysicalRegisterID::X18,   PhysicalRegisterID::X19,   PhysicalRegisterID::X20,
    PhysicalRegisterID::X21,   PhysicalRegisterID::X22,   PhysicalRegisterID::X23,
    PhysicalRegisterID::X24,   PhysicalRegisterID::X25,   PhysicalRegisterID::X26,
    PhysicalRegisterID::X27,   PhysicalRegisterID::F8_D,  PhysicalRegisterID::F9_D,
    PhysicalRegisterID::F18_D, PhysicalRegisterID::F19_D, PhysicalRegisterID::F20_D,
    PhysicalRegisterID::F21_D, PhysicalRegisterID::F22_D, PhysicalRegisterID::F23_D,
    PhysicalRegisterID::F24_D, PhysicalRegisterID::F25_D, PhysicalRegisterID::F26_D,
    PhysicalRegisterID::F27_D,
};

constexpr std::array SaveRestoreOrder{
    PhysicalRegisterID::X1,  PhysicalRegisterID::X8,  PhysicalRegisterID::X9,
    PhysicalRegisterID::X18, PhysicalRegisterID::X19, PhysicalRegisterID::X20,
    PhysicalRegisterID::X21, PhysicalRegisterID::X22, PhysicalRegisterID::X23,
    PhysicalRegisterID::X24, PhysicalRegisterID::X25, PhysicalRegisterID::X26,
    PhysicalRegisterID::X27,
};

[[nodiscard]] std::span<const PhysicalRegisterID> saveOrder(RISCVABI ABI) noexcept {
  return ABI == RISCVABI::LP64D ? std::span<const PhysicalRegisterID>(LP64DSaveOrder)
                                : std::span<const PhysicalRegisterID>(LP64SaveOrder);
}

[[nodiscard]] std::optional<std::size_t> saveRestoreRegisterIndex(PhysicalRegister Reg) noexcept {
  for (std::size_t Index = 0; Index < SaveRestoreOrder.size(); ++Index) {
    const auto Candidate = physicalRegister(SaveRestoreOrder[Index]);
    if (Candidate && *Candidate == Reg)
      return Index;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<std::size_t>
saveRestoreLibCallID(std::span<const PhysicalRegister> SavedRegisters,
                     const RISCVSubtargetFeatures &Features) noexcept {
  if (!Features.has(RISCVSubtargetOption::SaveRestore))
    return std::nullopt;
  std::optional<std::size_t> Result;
  for (const PhysicalRegister Reg : SavedRegisters) {
    const auto Index = saveRestoreRegisterIndex(Reg);
    if (Index)
      Result = std::max(Result.value_or(0), *Index);
  }
  return Result;
}

[[nodiscard]] std::uint64_t saveRestoreFrameSize(std::span<const PhysicalRegister> SavedRegisters,
                                                 const RISCVSubtargetFeatures &Features) noexcept {
  const auto ID = saveRestoreLibCallID(SavedRegisters, Features);
  if (!ID)
    return 0;
  const std::uint64_t Bytes = (static_cast<std::uint64_t>(*ID) + 1U) * 8U;
  return (Bytes + StackAlignmentBytes - 1U) & ~(StackAlignmentBytes - 1U);
}

[[nodiscard]] Expected<std::vector<PhysicalRegister>>
modifiedCalleeSavedRegisters(const MachineFunction &Function, const TargetDescription &Target,
                             RISCVABI ABI) {
  std::unordered_set<PhysicalRegister> Modified;
  for (const StackObject &Object : Function.frameInfo().objects()) {
    if (!Object.isTombstone() && !Object.description()->IsDead &&
        Object.description()->CalleeSavedRegister) {
      Modified.insert(*Object.description()->CalleeSavedRegister);
    }
  }
  for (const MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
       Block = Block->next()) {
    for (const MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      for (const MachineOperand &Operand : Instruction->operands()) {
        if (const auto *Mask = Operand.getIf<RegisterMaskOperand>(); Mask != nullptr) {
          if (!Mask->Mask) {
            return makeUnexpected(ErrorCode::VerificationError,
                                  "RISC-V callee-save analysis found a null register mask");
          }
          for (const PhysicalRegisterID ID : saveOrder(ABI)) {
            auto Reg = physicalRegister(ID);
            if (!Reg)
              return std::unexpected(Reg.error());
            if (Mask->Mask->clobbers(*Reg))
              Modified.insert(*Reg);
          }
        }
        const Register *Reg = Operand.getIf<Register>();
        if (Reg == nullptr || !Reg->isPhysical() || !Operand.isDef())
          continue;
        const auto Physical = Reg->physicalRegister();
        if (Physical)
          Modified.insert(*Physical);
      }
    }
  }

  if (requiresFramePointer(Function)) {
    auto RA = physicalRegister(PhysicalRegisterID::X1);
    auto FP = physicalRegister(PhysicalRegisterID::X8);
    if (!RA || !FP)
      return std::unexpected(!RA ? RA.error() : FP.error());
    Modified.insert(*RA);
    Modified.insert(*FP);
  }
  if (requiresBasePointer(Function)) {
    auto BP = physicalRegister(PhysicalRegisterID::X9);
    if (!BP)
      return std::unexpected(BP.error());
    Modified.insert(*BP);
  }

  std::vector<PhysicalRegister> Result;
  for (const PhysicalRegisterID ID : saveOrder(ABI)) {
    auto Reg = physicalRegister(ID);
    if (!Reg)
      return std::unexpected(Reg.error());
    if (Modified.contains(*Reg))
      Result.push_back(*Reg);
  }
  for (const PhysicalRegister Reg : Result) {
    if (Target.registers().physicalRegister(Reg) == nullptr) {
      return makeUnexpected(ErrorCode::InternalError,
                            "RISC-V callee-save register is absent from target facts");
    }
  }
  return Result;
}

[[nodiscard]] Expected<const MachineMemOperand *>
createStackMemory(MachineFunction &Function, FrameIndex Index, MemoryAccessFlag Access) {
  auto Size = MemorySize::fixed(8);
  auto Align = Alignment::create(8);
  auto Type = LowLevelType::makeCategoryNeutralScalar(64);
  if (!Size || !Align || !Type)
    return makeUnexpected(ErrorCode::InternalError, "RISC-V spill facts are invalid");
  MachineMemOperandDesc Description;
  Description.Accesses = {Access};
  Description.Size = *Size;
  Description.MemoryType = *Type;
  Description.Align = *Align;
  Description.Base = MemoryBase::frameIndex(Index);
  return Function.createMemOperand(std::move(Description));
}

[[nodiscard]] Expected<const MachineMemOperand *>
createResolvedStackMemory(MachineFunction &Function, MemoryAccessFlag Access, std::int64_t Offset) {
  auto Size = MemorySize::fixed(8);
  auto Align = Alignment::create(8);
  auto Type = LowLevelType::makeCategoryNeutralScalar(64);
  if (!Size || !Align || !Type)
    return makeUnexpected(ErrorCode::InternalError, "RISC-V spill facts are invalid");
  MachineMemOperandDesc Description;
  Description.Accesses = {Access};
  Description.Size = *Size;
  Description.MemoryType = *Type;
  Description.Align = *Align;
  Description.Base = MemoryBase::stack();
  Description.Offset = Offset;
  return Function.createMemOperand(std::move(Description));
}

[[nodiscard]] Expected<void> replaceImmediate(MachineInstr &Instruction, std::size_t Index,
                                              std::int64_t Value) {
  const MachineOperand *Old = Instruction.operandAt(Index);
  if (Old == nullptr)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V frame instruction lacks its offset operand");
  const std::uint32_t TargetFlags = Old->targetFlags();
  auto Erased = Instruction.eraseOperand(Index);
  if (!Erased)
    return std::unexpected(Erased.error());
  return Instruction.insertOperand(Index, MachineOperand::immediate(Value, TargetFlags));
}

[[nodiscard]] Expected<void> replaceWithRegister(MachineInstr &Instruction, std::size_t Index,
                                                 PhysicalRegister Reg) {
  const MachineOperand *Old = Instruction.operandAt(Index);
  if (Old == nullptr)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V frame instruction lacks its frame operand");
  const std::uint32_t TargetFlags = Old->targetFlags();
  auto Replacement = MachineOperand::registerOperand(Register::fromPhysical(Reg),
                                                     {RegisterOperandFlag::Use}, {}, TargetFlags);
  if (!Replacement)
    return std::unexpected(Replacement.error());
  auto Erased = Instruction.eraseOperand(Index);
  if (!Erased)
    return std::unexpected(Erased.error());
  return Instruction.insertOperand(Index, std::move(*Replacement));
}

[[nodiscard]] Expected<void> setFrameFlag(MachineInstrBuilder &Builder,
                                          std::optional<MachineInstrFlag> Flag) {
  if (!Flag)
    return {};
  return Builder.setFlags({*Flag});
}

[[nodiscard]] Expected<DwarfRegisterNumber> dwarfRegister(const TargetDescription &Target,
                                                          PhysicalRegister Reg) {
  const auto Number = Target.mirText().dwarfRegister(Reg);
  if (!Number) {
    return makeUnexpected(ErrorCode::InternalError, "RISC-V frame register has no DWARF mapping");
  }
  return DwarfRegisterNumber::fromValue(*Number);
}

[[nodiscard]] Expected<void> emitCFI(MachineIRBuilder &Builder, MachineCFIInstruction Instruction,
                                     MachineInstrFlag Flag) {
  auto Index = Builder.function().cfiInfo().addInstruction(std::move(Instruction));
  if (!Index)
    return std::unexpected(Index.error());
  auto Pending = Builder.buildInstr(rcc::opcode(GenericOpcode::CFI_INSTRUCTION));
  if (!Pending)
    return std::unexpected(Pending.error());
  if (auto Result = Pending->addCFIInstruction(*Index); !Result)
    return std::unexpected(Result.error());
  if (auto Result = setFrameFlag(*Pending, Flag); !Result)
    return std::unexpected(Result.error());
  auto Built = Pending->build();
  if (!Built)
    return std::unexpected(Built.error());
  return {};
}

[[nodiscard]] Expected<void> emitSavedRegisterCFI(MachineIRBuilder &Builder,
                                                  const TargetDescription &Target,
                                                  PhysicalRegister Reg, std::int64_t Offset,
                                                  MachineInstrFlag Flag) {
  auto DwarfReg = dwarfRegister(Target, Reg);
  if (!DwarfReg)
    return std::unexpected(DwarfReg.error());
  return emitCFI(Builder, MachineCFIInstruction::offset(*DwarfReg, Offset), Flag);
}

[[nodiscard]] Expected<void> emitRestoreRegisterCFI(MachineIRBuilder &Builder,
                                                    const TargetDescription &Target,
                                                    PhysicalRegister Reg, MachineInstrFlag Flag) {
  auto DwarfReg = dwarfRegister(Target, Reg);
  if (!DwarfReg)
    return std::unexpected(DwarfReg.error());
  return emitCFI(Builder, MachineCFIInstruction::restore(*DwarfReg), Flag);
}

[[nodiscard]] Expected<void> emitDefCFI(MachineIRBuilder &Builder, const TargetDescription &Target,
                                        PhysicalRegister Reg, std::int64_t Offset,
                                        MachineInstrFlag Flag) {
  auto DwarfReg = dwarfRegister(Target, Reg);
  if (!DwarfReg)
    return std::unexpected(DwarfReg.error());
  return emitCFI(Builder, MachineCFIInstruction::defCFA(*DwarfReg, Offset), Flag);
}

[[nodiscard]] Expected<void> emitDefCFAOffset(MachineIRBuilder &Builder, std::int64_t Offset,
                                              MachineInstrFlag Flag) {
  return emitCFI(Builder, MachineCFIInstruction::defCFAOffset(Offset), Flag);
}

[[nodiscard]] Expected<void> emitConstant(MachineIRBuilder &Builder, PhysicalRegister Destination,
                                          PhysicalRegister Zero, std::int64_t Value,
                                          std::optional<MachineInstrFlag> Flag) {
  if (Value > MaxSigned12 && Value <= std::numeric_limits<std::int32_t>::max()) {
    const std::int64_t Upper = (Value + 0x800) >> 12;
    const std::int64_t Lower = Value - (Upper << 12);
    auto High = Builder.buildInstr(instructionOpcode(InstructionOpcode::LUI));
    if (!High)
      return std::unexpected(High.error());
    if (auto Result = High->addDef(Register::fromPhysical(Destination)); !Result)
      return std::unexpected(Result.error());
    if (auto Result = High->addImmediate(Upper); !Result)
      return std::unexpected(Result.error());
    if (auto Result = setFrameFlag(*High, Flag); !Result)
      return std::unexpected(Result.error());
    if (auto Result = High->build(); !Result)
      return std::unexpected(Result.error());
    if (Lower == 0)
      return {};
    auto Low = Builder.buildInstr(instructionOpcode(InstructionOpcode::ADDI));
    if (!Low)
      return std::unexpected(Low.error());
    if (auto Result = Low->addDef(Register::fromPhysical(Destination)); !Result)
      return std::unexpected(Result.error());
    RegisterOperandFlags Killed;
    Killed.set(RegisterOperandFlag::Kill);
    if (auto Result = Low->addUse(Register::fromPhysical(Destination), Killed); !Result)
      return std::unexpected(Result.error());
    if (auto Result = Low->addImmediate(Lower); !Result)
      return std::unexpected(Result.error());
    if (auto Result = setFrameFlag(*Low, Flag); !Result)
      return std::unexpected(Result.error());
    auto Built = Low->build();
    if (!Built)
      return std::unexpected(Built.error());
    return {};
  }

  std::uint64_t Remaining = static_cast<std::uint64_t>(Value);
  std::vector<std::int64_t> Digits;
  while (!fitsSigned12(std::bit_cast<std::int64_t>(Remaining))) {
    std::int64_t Digit = static_cast<std::int64_t>(Remaining & 0xFFFU);
    if (Digit >= 0x800)
      Digit -= 0x1000;
    Digits.push_back(Digit);
    const std::uint64_t Adjusted = Remaining - static_cast<std::uint64_t>(Digit);
    Remaining = Adjusted >> 12U;
    if ((Adjusted & (std::uint64_t{1} << 63U)) != 0)
      Remaining |= ~std::uint64_t{0} << 52U;
  }

  auto Base = Builder.buildInstr(instructionOpcode(InstructionOpcode::ADDI));
  if (!Base)
    return std::unexpected(Base.error());
  if (auto Result = Base->addDef(Register::fromPhysical(Destination)); !Result)
    return std::unexpected(Result.error());
  if (auto Result = Base->addUse(Register::fromPhysical(Zero)); !Result)
    return std::unexpected(Result.error());
  if (auto Result = Base->addImmediate(std::bit_cast<std::int64_t>(Remaining)); !Result)
    return std::unexpected(Result.error());
  if (auto Result = setFrameFlag(*Base, Flag); !Result)
    return std::unexpected(Result.error());
  if (auto Result = Base->build(); !Result)
    return std::unexpected(Result.error());

  for (std::size_t Reverse = Digits.size(); Reverse != 0; --Reverse) {
    auto Shift = Builder.buildInstr(instructionOpcode(InstructionOpcode::SLLI));
    if (!Shift)
      return std::unexpected(Shift.error());
    if (auto Result = Shift->addDef(Register::fromPhysical(Destination)); !Result)
      return std::unexpected(Result.error());
    RegisterOperandFlags Killed;
    Killed.set(RegisterOperandFlag::Kill);
    if (auto Result = Shift->addUse(Register::fromPhysical(Destination), Killed); !Result)
      return std::unexpected(Result.error());
    if (auto Result = Shift->addImmediate(12); !Result)
      return std::unexpected(Result.error());
    if (auto Result = setFrameFlag(*Shift, Flag); !Result)
      return std::unexpected(Result.error());
    if (auto Result = Shift->build(); !Result)
      return std::unexpected(Result.error());

    auto Add = Builder.buildInstr(instructionOpcode(InstructionOpcode::ADDI));
    if (!Add)
      return std::unexpected(Add.error());
    if (auto Result = Add->addDef(Register::fromPhysical(Destination)); !Result)
      return std::unexpected(Result.error());
    if (auto Result = Add->addUse(Register::fromPhysical(Destination), Killed); !Result)
      return std::unexpected(Result.error());
    if (auto Result = Add->addImmediate(Digits[Reverse - 1U]); !Result)
      return std::unexpected(Result.error());
    if (auto Result = setFrameFlag(*Add, Flag); !Result)
      return std::unexpected(Result.error());
    if (auto Result = Add->build(); !Result)
      return std::unexpected(Result.error());
  }
  return {};
}

[[nodiscard]] Expected<void> emitRegisterAdd(MachineIRBuilder &Builder,
                                             PhysicalRegister Destination, PhysicalRegister Left,
                                             PhysicalRegister Right,
                                             std::optional<MachineInstrFlag> Flag,
                                             bool KillRight = false) {
  auto Add = Builder.buildInstr(instructionOpcode(InstructionOpcode::ADD));
  if (!Add)
    return std::unexpected(Add.error());
  if (auto Result = Add->addDef(Register::fromPhysical(Destination)); !Result)
    return std::unexpected(Result.error());
  if (auto Result = Add->addUse(Register::fromPhysical(Left)); !Result)
    return std::unexpected(Result.error());
  RegisterOperandFlags RightFlags;
  if (KillRight)
    RightFlags.set(RegisterOperandFlag::Kill);
  if (auto Result = Add->addUse(Register::fromPhysical(Right), RightFlags); !Result)
    return std::unexpected(Result.error());
  if (auto Result = setFrameFlag(*Add, Flag); !Result)
    return std::unexpected(Result.error());
  auto Built = Add->build();
  if (!Built)
    return std::unexpected(Built.error());
  return {};
}

[[nodiscard]] Expected<void> emitRegisterSub(MachineIRBuilder &Builder,
                                             PhysicalRegister Destination, PhysicalRegister Left,
                                             PhysicalRegister Right,
                                             std::optional<MachineInstrFlag> Flag,
                                             bool KillRight = false) {
  auto Sub = Builder.buildInstr(instructionOpcode(InstructionOpcode::SUB));
  if (!Sub)
    return std::unexpected(Sub.error());
  if (auto Result = Sub->addDef(Register::fromPhysical(Destination)); !Result)
    return std::unexpected(Result.error());
  if (auto Result = Sub->addUse(Register::fromPhysical(Left)); !Result)
    return std::unexpected(Result.error());
  RegisterOperandFlags RightFlags;
  if (KillRight)
    RightFlags.set(RegisterOperandFlag::Kill);
  if (auto Result = Sub->addUse(Register::fromPhysical(Right), RightFlags); !Result)
    return std::unexpected(Result.error());
  if (auto Result = setFrameFlag(*Sub, Flag); !Result)
    return std::unexpected(Result.error());
  auto Built = Sub->build();
  if (!Built)
    return std::unexpected(Built.error());
  return {};
}

[[nodiscard]] Expected<void> emitShiftImmediate(MachineIRBuilder &Builder, Opcode OpcodeValue,
                                                PhysicalRegister Destination,
                                                PhysicalRegister Source, std::int64_t Amount,
                                                std::optional<MachineInstrFlag> Flag) {
  auto Shift = Builder.buildInstr(OpcodeValue);
  if (!Shift)
    return std::unexpected(Shift.error());
  if (auto Result = Shift->addDef(Register::fromPhysical(Destination)); !Result)
    return std::unexpected(Result.error());
  if (auto Result = Shift->addUse(Register::fromPhysical(Source)); !Result)
    return std::unexpected(Result.error());
  if (auto Result = Shift->addImmediate(Amount); !Result)
    return std::unexpected(Result.error());
  if (auto Result = setFrameFlag(*Shift, Flag); !Result)
    return std::unexpected(Result.error());
  auto Built = Shift->build();
  if (!Built)
    return std::unexpected(Built.error());
  return {};
}

[[nodiscard]] Expected<void> emitScratchAccess(MachineIRBuilder &Builder, PhysicalRegister Scratch,
                                               FrameIndexReference Reference, bool Restore) {
  auto Memory = createResolvedStackMemory(
      Builder.function(), Restore ? MemoryAccessFlag::Load : MemoryAccessFlag::Store,
      Reference.Offset);
  if (!Memory)
    return std::unexpected(Memory.error());
  auto Pending = Builder.buildInstr(
      instructionOpcode(Restore ? InstructionOpcode::LD : InstructionOpcode::SD));
  if (!Pending)
    return std::unexpected(Pending.error());
  auto Value = Restore ? Pending->addDef(Register::fromPhysical(Scratch))
                       : Pending->addUse(Register::fromPhysical(Scratch));
  if (!Value)
    return std::unexpected(Value.error());
  if (auto Result = Pending->addUse(Register::fromPhysical(Reference.BaseRegister)); !Result)
    return std::unexpected(Result.error());
  if (auto Result = Pending->addImmediate(Reference.Offset); !Result)
    return std::unexpected(Result.error());
  if (auto Result = Pending->addMemOperand(**Memory); !Result)
    return std::unexpected(Result.error());
  auto Built = Pending->build();
  if (!Built)
    return std::unexpected(Built.error());
  return {};
}

[[nodiscard]] Expected<void> emitAddImmediate(MachineIRBuilder &Builder,
                                              PhysicalRegister Destination, PhysicalRegister Source,
                                              std::int64_t Amount,
                                              std::optional<MachineInstrFlag> Flag,
                                              bool KillSource = false) {
  auto Add = Builder.buildInstr(instructionOpcode(InstructionOpcode::ADDI));
  if (!Add)
    return std::unexpected(Add.error());
  if (auto Result = Add->addDef(Register::fromPhysical(Destination)); !Result)
    return std::unexpected(Result.error());
  RegisterOperandFlags SourceFlags;
  if (KillSource)
    SourceFlags.set(RegisterOperandFlag::Kill);
  if (auto Result = Add->addUse(Register::fromPhysical(Source), SourceFlags); !Result)
    return std::unexpected(Result.error());
  if (auto Result = Add->addImmediate(Amount); !Result)
    return std::unexpected(Result.error());
  if (auto Result = setFrameFlag(*Add, Flag); !Result)
    return std::unexpected(Result.error());
  auto Built = Add->build();
  if (!Built)
    return std::unexpected(Built.error());
  return {};
}

[[nodiscard]] constexpr bool
canEmitAddImmediateAdjustment(std::int64_t Amount, std::uint64_t RequiredAlignment) noexcept {
  if (fitsSigned12(Amount))
    return true;
  if (RequiredAlignment == 0 || RequiredAlignment >= 2048U)
    return false;
  const std::int64_t MaximumPositiveStep = 2048 - static_cast<std::int64_t>(RequiredAlignment);
  return Amount > -4096 && Amount <= 2 * MaximumPositiveStep;
}

[[nodiscard]] Expected<void>
emitAddImmediateAdjustment(MachineIRBuilder &Builder, PhysicalRegister Destination,
                           PhysicalRegister Source, std::int64_t Amount,
                           std::uint64_t RequiredAlignment, std::optional<MachineInstrFlag> Flag) {
  if (fitsSigned12(Amount))
    return emitAddImmediate(Builder, Destination, Source, Amount, Flag);
  if (!canEmitAddImmediateAdjustment(Amount, RequiredAlignment)) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "RISC-V stack adjustment cannot be encoded with ADDI instructions");
  }
  const std::int64_t FirstAdjustment =
      Amount < 0 ? -2048 : 2048 - static_cast<std::int64_t>(RequiredAlignment);
  auto First = emitAddImmediate(Builder, Destination, Source, FirstAdjustment, Flag);
  if (!First)
    return std::unexpected(First.error());
  return emitAddImmediate(Builder, Destination, Destination, Amount - FirstAdjustment, Flag, true);
}

[[nodiscard]] Expected<void> emitAndImmediate(MachineIRBuilder &Builder,
                                              PhysicalRegister Destination, PhysicalRegister Source,
                                              std::int64_t Amount,
                                              std::optional<MachineInstrFlag> Flag) {
  auto And = Builder.buildInstr(instructionOpcode(InstructionOpcode::ANDI));
  if (!And)
    return std::unexpected(And.error());
  if (auto Result = And->addDef(Register::fromPhysical(Destination)); !Result)
    return std::unexpected(Result.error());
  if (auto Result = And->addUse(Register::fromPhysical(Source)); !Result)
    return std::unexpected(Result.error());
  if (auto Result = And->addImmediate(Amount); !Result)
    return std::unexpected(Result.error());
  if (auto Result = setFrameFlag(*And, Flag); !Result)
    return std::unexpected(Result.error());
  auto Built = And->build();
  if (!Built)
    return std::unexpected(Built.error());
  return {};
}

[[nodiscard]] std::array<PhysicalRegisterID, 15> scratchOrder() noexcept {
  return {
      PhysicalRegisterID::X10, PhysicalRegisterID::X11, PhysicalRegisterID::X12,
      PhysicalRegisterID::X13, PhysicalRegisterID::X14, PhysicalRegisterID::X15,
      PhysicalRegisterID::X16, PhysicalRegisterID::X17, PhysicalRegisterID::X5,
      PhysicalRegisterID::X6,  PhysicalRegisterID::X7,  PhysicalRegisterID::X28,
      PhysicalRegisterID::X29, PhysicalRegisterID::X30, PhysicalRegisterID::X31,
  };
}

[[nodiscard]] Expected<std::vector<PhysicalRegister>> scratchRegisters() {
  std::vector<PhysicalRegister> Result;
  for (const PhysicalRegisterID ID : scratchOrder()) {
    auto Reg = physicalRegister(ID);
    if (!Reg)
      return std::unexpected(Reg.error());
    Result.push_back(*Reg);
  }
  return Result;
}

[[nodiscard]] const StackObject *emergencyObject(const MachineFunction &Function) noexcept {
  for (const StackObject &Object : Function.frameInfo().objects()) {
    if (!Object.isTombstone() && !Object.description()->IsDead &&
        Object.description()->Kind == StackObjectKind::EmergencySpill)
      return &Object;
  }
  return nullptr;
}

[[nodiscard]] Expected<FrameIndex> createEmergencyObject(MachineFunction &Function) {
  auto Align = Alignment::create(8);
  if (!Align)
    return std::unexpected(Align.error());
  StackObjectDesc Description;
  Description.Kind = StackObjectKind::EmergencySpill;
  Description.Size = 8;
  Description.Align = *Align;
  return Function.frameInfo().createObject(std::move(Description));
}

[[nodiscard]] Expected<std::uint64_t> layoutFrame(MachineFunction &Function, bool ReserveCallFrame,
                                                  std::uint64_t ReservedFrameSize,
                                                  bool EmitDebugOutput = false) {
  MachineFrameInfo &Frame = Function.frameInfo();
  const std::uint64_t CallFrame = ReserveCallFrame ? Frame.maxCallFrameSize().value_or(0) : 0;
  std::uint64_t Cursor = ReservedFrameSize;

  std::vector<const StackObject *> Specials;
  std::vector<const StackObject *> Locals;
  std::vector<const StackObject *> Emergencies;
  std::uint64_t MaximumAlignment = requiredStackAlignment(Function);
  for (const StackObject &Object : Frame.objects()) {
    if (Object.isTombstone() || Object.description()->IsDead)
      continue;
    const StackObjectDesc &Description = *Object.description();
    if (Description.Stack != StackID::defaultStack()) {
      return makeUnexpected(ErrorCode::Unsupported,
                            "RV64GC does not support non-default stack IDs");
    }
    MaximumAlignment = std::max(MaximumAlignment, Description.Align.bytes());
    if (Object.isVariableSized())
      continue;
    if (Description.LocalOffset && isOutgoingCallObject(Description)) {
      if (*Description.LocalOffset < 0 ||
          static_cast<std::uint64_t>(*Description.LocalOffset) % Description.Align.bytes() != 0) {
        return makeUnexpected(ErrorCode::FrameLayoutFailed,
                              "RISC-V outgoing stack object has an invalid offset");
      }
      const std::uint64_t Offset = static_cast<std::uint64_t>(*Description.LocalOffset);
      if (Description.Size.value_or(0) > std::numeric_limits<std::uint64_t>::max() - Offset) {
        return makeUnexpected(ErrorCode::FrameLayoutFailed,
                              "RISC-V outgoing stack object range overflows");
      }
      const std::uint64_t End = Offset + Description.Size.value_or(0);
      if (!Frame.maxCallFrameSize() || End > *Frame.maxCallFrameSize()) {
        return makeUnexpected(ErrorCode::FrameLayoutFailed,
                              "RISC-V outgoing stack object exceeds the call frame");
      }
      continue;
    }
    if (Object.isFixed())
      continue;
    if (Description.Kind == StackObjectKind::CalleeSaved) {
      Specials.push_back(&Object);
    } else if (Description.Kind == StackObjectKind::EmergencySpill) {
      Emergencies.push_back(&Object);
    } else {
      Locals.push_back(&Object);
    }
  }
  const auto Order = [](const StackObject *Left, const StackObject *Right) {
    const StackObjectDesc &L = *Left->description();
    const StackObjectDesc &R = *Right->description();
    if (L.Align.bytes() != R.Align.bytes())
      return L.Align.bytes() > R.Align.bytes();
    if (L.Size != R.Size)
      return L.Size.value_or(0) > R.Size.value_or(0);
    return Left->index().value() < Right->index().value();
  };
  std::ranges::sort(Locals, Order);
  std::ranges::sort(Specials, Order);
  std::ranges::sort(Emergencies, Order);

  const auto Place = [&](std::span<const StackObject *const> Objects) -> Expected<void> {
    for (const StackObject *Object : Objects) {
      const StackObjectDesc &Description = *Object->description();
      if (Description.Size.value_or(0) > std::numeric_limits<std::uint64_t>::max() - Cursor) {
        return makeUnexpected(ErrorCode::FrameLayoutFailed, "RISC-V frame size overflows uint64");
      }
      Cursor += Description.Size.value_or(0);
      auto Aligned = alignTo(Cursor, Description.Align.bytes());
      if (!Aligned)
        return std::unexpected(Aligned.error());
      Cursor = *Aligned;
      if (Cursor > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return makeUnexpected(ErrorCode::FrameLayoutFailed,
                              "RISC-V frame offset exceeds signed 64-bit range");
      }
      auto Set = Frame.setObjectOffset(Object->index(), -static_cast<std::int64_t>(Cursor));
      if (!Set)
        return std::unexpected(Set.error());
    }
    return {};
  };

  auto PlacedSpecials = Place(Specials);
  if (!PlacedSpecials)
    return std::unexpected(PlacedSpecials.error());
  const bool PlaceEmergenciesBeforeLocals =
      requiresFramePointer(Function) && !requiresStackRealignment(Function);
  if (PlaceEmergenciesBeforeLocals) {
    auto PlacedEmergencies = Place(Emergencies);
    if (!PlacedEmergencies)
      return std::unexpected(PlacedEmergencies.error());
  }
  auto PlacedLocals = Place(Locals);
  if (!PlacedLocals)
    return std::unexpected(PlacedLocals.error());
  if (!PlaceEmergenciesBeforeLocals) {
    auto PlacedEmergencies = Place(Emergencies);
    if (!PlacedEmergencies)
      return std::unexpected(PlacedEmergencies.error());
  }

  for (const StackObject &Object : Frame.objects()) {
    if (Object.isTombstone() || Object.description()->IsDead || Object.isVariableSized())
      continue;
    const StackObjectDesc &Description = *Object.description();
    if (Description.LocalOffset && isOutgoingCallObject(Description)) {
      auto Set = Frame.setObjectOffset(Object.index(), *Description.LocalOffset);
      if (!Set)
        return std::unexpected(Set.error());
    }
  }
  if (CallFrame > std::numeric_limits<std::uint64_t>::max() - Cursor)
    return makeUnexpected(ErrorCode::FrameLayoutFailed, "RISC-V call frame size overflows uint64");
  Cursor += CallFrame;
  const std::uint64_t SizeBeforeFinalAlignment = Cursor;
  auto FinalSize = alignTo(Cursor, MaximumAlignment);
  if (!FinalSize)
    return std::unexpected(FinalSize.error());
  if (*FinalSize != SizeBeforeFinalAlignment) {
    const std::uint64_t Delta = *FinalSize - SizeBeforeFinalAlignment;
    if (Delta > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return makeUnexpected(ErrorCode::FrameLayoutFailed,
                            "RISC-V emergency spill adjustment exceeds int64");
    }
    if (EmitDebugOutput && !Emergencies.empty())
      std::cerr << "Adjusting emergency spill slots!\n";
    for (const StackObject *Object : Emergencies) {
      if (!Object->description()->Offset)
        return makeUnexpected(ErrorCode::InternalError,
                              "RISC-V emergency spill slot has no preliminary offset");
      const std::int64_t PreliminaryOffset = *Object->description()->Offset;
      auto Adjusted = checkedAdd(PreliminaryOffset, -static_cast<std::int64_t>(Delta));
      if (!Adjusted)
        return std::unexpected(Adjusted.error());
      if (EmitDebugOutput) {
        std::cerr << "Adjusting offset of emergency spill slot #" << Object->index().value()
                  << " from " << PreliminaryOffset << " to " << *Adjusted << '\n';
      }
      auto Set = Frame.setObjectOffset(Object->index(), *Adjusted);
      if (!Set)
        return std::unexpected(Set.error());
    }
  }
  if (*FinalSize > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return makeUnexpected(ErrorCode::FrameLayoutFailed,
                          "RISC-V frame size exceeds signed 64-bit range");
  }
  return *FinalSize;
}

[[nodiscard]] std::uint64_t firstSPAdjustAmount(const MachineFunction &Function,
                                                const RISCVSubtargetFeatures &Features) noexcept {
  if (saveRestoreFrameSize(Function.records().calleeSavedRegisters(), Features) != 0)
    return 0;
  const std::uint64_t StackSize = Function.frameInfo().state().StackSize;
  if (StackSize <= static_cast<std::uint64_t>(MaxSigned12) ||
      Function.records().calleeSavedRegisters().empty()) {
    return 0;
  }
  if (Features.has(RISCVExtension::C)) {
    const auto CanCompress = [StackSize](std::uint64_t CompressLength) {
      return StackSize <= 2047U + CompressLength ||
             (StackSize > 4096U - StackAlignmentBytes && StackSize <= 4094U + CompressLength) ||
             StackSize > 6144U - StackAlignmentBytes;
    };
    constexpr std::uint64_t AddImmediate16SPCompressLength = 496;
    if (CanCompress(AddImmediate16SPCompressLength))
      return AddImmediate16SPCompressLength;
    constexpr std::uint64_t RV64StackCompressLength = 512;
    if (CanCompress(RV64StackCompressLength))
      return RV64StackCompressLength;
  }
  return 2048U - StackAlignmentBytes;
}

[[nodiscard]] Expected<std::int64_t> objectReferenceOffset(const MachineFunction &Function,
                                                           const MachineInstr &Instruction,
                                                           const StackObjectDesc &Object,
                                                           std::int64_t OperandOffset,
                                                           const RISCVSubtargetFeatures &Features) {
  if (!Object.Offset)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V FrameIndex references an object without a final offset");
  const bool HasFP = requiresFramePointer(Function);
  std::int64_t BaseOffset = *Object.Offset;
  if (Object.LocalOffset && isOutgoingCallObject(Object))
    BaseOffset = *Object.LocalOffset;
  else if (Object.Kind == StackObjectKind::CalleeSaved) {
    const std::uint64_t FirstAdjustment = firstSPAdjustAmount(Function, Features);
    const std::uint64_t Adjustment =
        FirstAdjustment == 0 ? Function.frameInfo().state().StackSize : FirstAdjustment;
    if (Adjustment > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return makeUnexpected(ErrorCode::FrameLayoutFailed,
                            "RISC-V callee-save adjustment exceeds int64");
    }
    auto Added = checkedAdd(BaseOffset, static_cast<std::int64_t>(Adjustment));
    if (!Added)
      return std::unexpected(Added.error());
    BaseOffset = *Added;
  } else if (Object.Kind == StackObjectKind::FixedABI ||
             Object.Kind == StackObjectKind::FixedSpillSlot) {
    if (!HasFP) {
      if (Function.frameInfo().state().StackSize >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return makeUnexpected(ErrorCode::FrameLayoutFailed,
                              "RISC-V stack size exceeds signed 64-bit range");
      }
      auto Added =
          checkedAdd(BaseOffset, static_cast<std::int64_t>(Function.frameInfo().state().StackSize));
      if (!Added)
        return std::unexpected(Added.error());
      BaseOffset = *Added;
    }
  } else if (!HasFP || requiresBasePointer(Function) || requiresStackRealignment(Function) ||
             Instruction.flags().has(MachineInstrFlag::FrameSetup)) {
    if (Function.frameInfo().state().StackSize >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return makeUnexpected(ErrorCode::FrameLayoutFailed,
                            "RISC-V stack size exceeds signed 64-bit range");
    }
    auto Added =
        checkedAdd(BaseOffset, static_cast<std::int64_t>(Function.frameInfo().state().StackSize));
    if (!Added)
      return std::unexpected(Added.error());
    BaseOffset = *Added;
  }
  return checkedAdd(BaseOffset, OperandOffset);
}

[[nodiscard]] Expected<FrameIndexReference> emergencyFrameReference(const MachineFunction &Function,
                                                                    const StackObjectDesc &Object) {
  if (!Object.Offset) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V emergency spill object has no final offset");
  }
  PhysicalRegisterID BaseID = PhysicalRegisterID::X2;
  std::int64_t Offset = *Object.Offset;
  if (requiresBasePointer(Function)) {
    BaseID = PhysicalRegisterID::X9;
  }
  if (Function.frameInfo().state().StackSize >
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return makeUnexpected(ErrorCode::FrameLayoutFailed,
                          "RISC-V stack size exceeds signed 64-bit range");
  }
  auto Adjusted =
      checkedAdd(Offset, static_cast<std::int64_t>(Function.frameInfo().state().StackSize));
  if (!Adjusted)
    return std::unexpected(Adjusted.error());
  Offset = *Adjusted;
  auto Base = physicalRegister(BaseID);
  if (!Base)
    return std::unexpected(Base.error());
  return FrameIndexReference{*Base, Offset};
}

[[nodiscard]] Expected<MachineInstr *> emitCalleeSave(MachineIRBuilder &Builder,
                                                      PhysicalRegister Reg, FrameIndex Index,
                                                      bool Restore, bool KillValue = false) {
  const bool IsFPR = Reg.number() >= static_cast<std::uint32_t>(PhysicalRegisterID::F0_D);
  const InstructionOpcode OpcodeValue =
      Restore ? (IsFPR ? InstructionOpcode::FLD : InstructionOpcode::LD)
              : (IsFPR ? InstructionOpcode::FSD : InstructionOpcode::SD);
  auto Memory = createStackMemory(Builder.function(), Index,
                                  Restore ? MemoryAccessFlag::Load : MemoryAccessFlag::Store);
  if (!Memory)
    return std::unexpected(Memory.error());
  auto Pending = Builder.buildInstr(instructionOpcode(OpcodeValue));
  if (!Pending)
    return std::unexpected(Pending.error());
  const Register Value = Register::fromPhysical(Reg);
  RegisterOperandFlags ValueFlags;
  if (!Restore && KillValue)
    ValueFlags.set(RegisterOperandFlag::Kill);
  auto RegisterResult = Restore ? Pending->addDef(Value) : Pending->addUse(Value, ValueFlags);
  if (!RegisterResult)
    return std::unexpected(RegisterResult.error());
  if (auto Result = Pending->addFrameIndex(Index); !Result)
    return std::unexpected(Result.error());
  if (auto Result = Pending->addImmediate(0); !Result)
    return std::unexpected(Result.error());
  if (auto Result = Pending->addMemOperand(**Memory); !Result)
    return std::unexpected(Result.error());
  if (auto Result = setFrameFlag(*Pending, Restore ? MachineInstrFlag::FrameDestroy
                                                   : MachineInstrFlag::FrameSetup);
      !Result) {
    return std::unexpected(Result.error());
  }
  auto Built = Pending->build();
  if (!Built)
    return std::unexpected(Built.error());
  return *Built;
}

[[nodiscard]] std::optional<FrameIndex> calleeSaveSlot(const MachineFunction &Function,
                                                       PhysicalRegister Reg) noexcept {
  for (const StackObject &Object : Function.frameInfo().objects()) {
    if (Object.isTombstone() || Object.description()->IsDead)
      continue;
    if (Object.description()->CalleeSavedRegister == Reg)
      return Object.index();
  }
  return std::nullopt;
}

[[nodiscard]] bool isABIStackSaveRegister(PhysicalRegister Reg, RISCVABI ABI) {
  return std::ranges::any_of(saveOrder(ABI), [Reg](PhysicalRegisterID ID) {
    auto Candidate = physicalRegister(ID);
    return Candidate && *Candidate == Reg;
  });
}

[[nodiscard]] Expected<void>
validateCalleeSaveObjects(const MachineFunction &Function,
                          std::span<const PhysicalRegister> SavedRegisters, RISCVABI ABI,
                          const RISCVSubtargetFeatures &Features) {
  std::unordered_set<PhysicalRegister> Slots;
  for (const StackObject &Object : Function.frameInfo().objects()) {
    if (Object.isTombstone() || Object.description()->IsDead ||
        !Object.description()->CalleeSavedRegister) {
      continue;
    }
    const StackObjectDesc &Description = *Object.description();
    const PhysicalRegister Reg = *Description.CalleeSavedRegister;
    if (!isABIStackSaveRegister(Reg, ABI)) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V callee-save object names a caller-saved register");
    }
    const auto SaveRestoreIndex = saveRestoreRegisterIndex(Reg);
    const bool ManagedByLibCall =
        Features.has(RISCVSubtargetOption::SaveRestore) && SaveRestoreIndex.has_value();
    const StackObjectKind ExpectedKind =
        ManagedByLibCall ? StackObjectKind::FixedSpillSlot : StackObjectKind::CalleeSaved;
    const std::optional<std::int64_t> ExpectedOffset =
        ManagedByLibCall
            ? std::optional<std::int64_t>{-static_cast<std::int64_t>(*SaveRestoreIndex + 1U) * 8}
            : std::nullopt;
    if (Description.Kind != ExpectedKind || Description.Size != std::optional<std::uint64_t>{8} ||
        Description.Align.bytes() < 8 || Description.Stack != StackID::defaultStack() ||
        (ManagedByLibCall && Description.Offset != ExpectedOffset)) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V callee-save object has an invalid kind, size, or alignment");
    }
    if (!Slots.insert(Reg).second) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V callee-save register has duplicate stack objects");
    }
    if (std::ranges::find(SavedRegisters, Reg) == SavedRegisters.end()) {
      return makeUnexpected(ErrorCode::InternalError,
                            "RISC-V callee-save object was omitted from save analysis");
    }
  }
  return {};
}

[[nodiscard]] bool calleeSaveIsRestored(const MachineFunction &Function,
                                        PhysicalRegister Reg) noexcept {
  const auto Slot = calleeSaveSlot(Function, Reg);
  const StackObject *Object = Slot ? Function.frameInfo().object(*Slot) : nullptr;
  return Object != nullptr && Object->description()->CalleeSavedRestored;
}

struct CalleeSaveOperation final {
  MachineInstr *Instruction = nullptr;
  PhysicalRegister Reg;
};

[[nodiscard]] std::vector<CalleeSaveOperation>
calleeSaveOperations(MachineBasicBlock &Block, const MachineFunction &Function, bool Restore) {
  std::vector<CalleeSaveOperation> Result;
  for (MachineInstr *Instruction = Block.firstInstruction(); Instruction != nullptr;
       Instruction = Instruction->next()) {
    if (!Instruction->flags().has(Restore ? MachineInstrFlag::FrameDestroy
                                          : MachineInstrFlag::FrameSetup)) {
      continue;
    }
    for (const MachineOperand &Operand : Instruction->operands()) {
      const auto *Frame = Operand.getIf<FrameIndexOperand>();
      if (Frame == nullptr)
        continue;
      const StackObject *Object = Function.frameInfo().object(Frame->Index);
      if (Object != nullptr && Object->description()->CalleeSavedRegister) {
        Result.push_back(
            CalleeSaveOperation{Instruction, *Object->description()->CalleeSavedRegister});
        break;
      }
    }
  }
  return Result;
}

[[nodiscard]] MachineBasicBlock *blockWithID(MachineFunction &Function,
                                             MachineBlockStableID ID) noexcept {
  for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr; Block = Block->next()) {
    if (Block->stableID() == ID)
      return Block;
  }
  return nullptr;
}

[[nodiscard]] MachineBasicBlock *frameSaveBlock(MachineFunction &Function) noexcept {
  const auto &SavePoints = Function.frameInfo().state().SavePoints;
  return SavePoints.empty() ? Function.firstBlock()
                            : blockWithID(Function, SavePoints.front().Block);
}

[[nodiscard]] bool isFrameRestoreBlock(const MachineFunction &Function,
                                       const MachineBasicBlock &Block) noexcept {
  const MachineFrameState &State = Function.frameInfo().state();
  if (State.RestorePoints.empty())
    return State.SavePoints.empty();
  return std::ranges::any_of(State.RestorePoints,
                             [&Block](const MachineFrameSaveRestorePoint &Point) {
                               return Point.Block == Block.stableID();
                             });
}

class RISCVFrameLowering final : public TargetFrameLowering {
public:
  explicit RISCVFrameLowering(const RISCVSubtarget &Subtarget) noexcept : Subtarget(Subtarget) {}

  [[nodiscard]] PhysicalRegister
  initialCFARegister(const MachineFunction &,
                     const TargetDescription &Target) const noexcept override {
    return Target.stackPointerRegisterToSaveRestore();
  }

  [[nodiscard]] bool
  isCalleeSavedRegister(PhysicalRegister Reg, const MachineFunction &,
                        const TargetDescription &Target) const noexcept override {
    return std::ranges::any_of(saveOrder(Subtarget.targetABI()), [&](PhysicalRegisterID ID) {
      const auto Candidate = physicalRegister(ID);
      return Candidate && Target.registers().regsOverlap(Reg, *Candidate);
    });
  }

  [[nodiscard]] Expected<void> prepareFrame(MachineFunction &Function,
                                            const TargetDescription &Target,
                                            FrameLoweringOptions Options) const override {
    MachineFrameInfo &Frame = Function.frameInfo();
    if (Frame.isFrozen())
      return makeUnexpected(ErrorCode::InvalidArgument, "RISC-V frame layout is already frozen");

    MachineFrameState State = Frame.state();
    const std::vector<MachineFrameSaveRestorePoint> RequestedSavePoints = State.SavePoints;
    const std::vector<MachineFrameSaveRestorePoint> RequestedRestorePoints = State.RestorePoints;
    const bool HasRequestedPlacement =
        !RequestedSavePoints.empty() || !RequestedRestorePoints.empty();
    for (const MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
         Block = Block->next()) {
      for (const MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
           Instruction = Instruction->next()) {
        const InstructionDescriptor *Descriptor =
            Target.instructionDescriptor(Instruction->opcode());
        if (Descriptor != nullptr && Descriptor->Flags.has(InstructionFlag::Call))
          State.HasCalls = true;
      }
    }
    auto UpdatedState = Frame.setState(State);
    if (!UpdatedState)
      return std::unexpected(UpdatedState.error());

    auto CallFrame = computeMaxCallFrameSize(Function);
    if (!CallFrame)
      return std::unexpected(CallFrame.error());
    if (requiresFramePointer(Function)) {
      auto FP = physicalRegister(PhysicalRegisterID::X8);
      if (!FP)
        return std::unexpected(FP.error());
      if (!Function.registerInfo().isReserved(*FP)) {
        auto Reserved = Function.registerInfo().reservePhysicalRegister(*FP);
        if (!Reserved)
          return std::unexpected(Reserved.error());
      }
    }
    if (requiresBasePointer(Function)) {
      auto BP = physicalRegister(PhysicalRegisterID::X9);
      if (!BP)
        return std::unexpected(BP.error());
      if (!Function.registerInfo().isReserved(*BP)) {
        auto Reserved = Function.registerInfo().reservePhysicalRegister(*BP);
        if (!Reserved)
          return std::unexpected(Reserved.error());
      }
    }
    auto FrameRegisters = validateFrameRegisterUses(Function, Target);
    if (!FrameRegisters)
      return std::unexpected(FrameRegisters.error());

    auto SavedRegisters = modifiedCalleeSavedRegisters(Function, Target, Subtarget.targetABI());
    if (!SavedRegisters)
      return std::unexpected(SavedRegisters.error());
    auto Records = Function.records().setCalleeSavedRegisters(*SavedRegisters);
    if (!Records)
      return std::unexpected(Records.error());
    auto ExistingSlots = validateCalleeSaveObjects(Function, *SavedRegisters, Subtarget.targetABI(),
                                                   Subtarget.features());
    if (!ExistingSlots)
      return std::unexpected(ExistingSlots.error());
    auto Align8 = Alignment::create(8);
    auto Align16 = Alignment::create(16);
    if (!Align8 || !Align16)
      return std::unexpected(!Align8 ? Align8.error() : Align16.error());
    for (const PhysicalRegister Reg : *SavedRegisters) {
      if (calleeSaveSlot(Function, Reg))
        continue;
      StackObjectDesc Description;
      const auto SaveRestoreIndex = saveRestoreRegisterIndex(Reg);
      const bool ManagedByLibCall =
          Subtarget.features().has(RISCVSubtargetOption::SaveRestore) && SaveRestoreIndex;
      Description.Kind =
          ManagedByLibCall ? StackObjectKind::FixedSpillSlot : StackObjectKind::CalleeSaved;
      Description.Size = 8;
      Description.Align =
          ManagedByLibCall && (*SaveRestoreIndex + 1U) % 2U == 0 ? *Align16 : *Align8;
      if (ManagedByLibCall)
        Description.Offset = -static_cast<std::int64_t>(*SaveRestoreIndex + 1U) * 8;
      Description.CalleeSavedRegister = Reg;
      auto Created = Frame.createObject(std::move(Description));
      if (!Created)
        return std::unexpected(Created.error());
    }
    const std::uint64_t LibCallFrameSize =
        saveRestoreFrameSize(*SavedRegisters, Subtarget.features());
    if (LibCallFrameSize != 0) {
      const bool HasCoveringObject =
          std::ranges::any_of(Frame.objects(), [&](const StackObject &O) {
            const StackObjectDesc *Description = O.description();
            return Description != nullptr && !Description->IsDead &&
                   Description->Kind == StackObjectKind::FixedSpillSlot &&
                   !Description->CalleeSavedRegister &&
                   Description->Size == std::optional<std::uint64_t>{LibCallFrameSize} &&
                   Description->Offset ==
                       std::optional<std::int64_t>{-static_cast<std::int64_t>(LibCallFrameSize)};
          });
      if (!HasCoveringObject) {
        StackObjectDesc Description;
        Description.Kind = StackObjectKind::FixedSpillSlot;
        Description.Size = LibCallFrameSize;
        Description.Align = *Align16;
        Description.Offset = -static_cast<std::int64_t>(LibCallFrameSize);
        auto Created = Frame.createObject(std::move(Description));
        if (!Created)
          return std::unexpected(Created.error());
      }
    }
    auto CompleteSlots = validateCalleeSaveObjects(Function, *SavedRegisters, Subtarget.targetABI(),
                                                   Subtarget.features());
    if (!CompleteSlots)
      return std::unexpected(CompleteSlots.error());

    const bool ReserveCallFrame = hasReservedCallFrame(Function);
    auto StackSize =
        layoutFrame(Function, ReserveCallFrame, LibCallFrameSize, Options.EmitDebugOutput);
    if (!StackSize)
      return std::unexpected(StackSize.error());

    State = Frame.state();
    State.StackSize = *StackSize;
    auto RequiredAlignment = Alignment::create(requiredStackAlignment(Function));
    if (!RequiredAlignment)
      return std::unexpected(RequiredAlignment.error());
    State.MaxAlignment = *RequiredAlignment;
    auto StateForReferences = Frame.setState(State);
    if (!StateForReferences)
      return std::unexpected(StateForReferences.error());

    bool NeedsEmergency = *StackSize > static_cast<std::uint64_t>(MaxSigned11);
    for (const MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
         Block = Block->next()) {
      for (const MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
           Instruction = Instruction->next()) {
        for (std::size_t Index = 0; Index < Instruction->operandCount(); ++Index) {
          const auto *FrameOperand = Instruction->operandAt(Index)->getIf<FrameIndexOperand>();
          if (FrameOperand == nullptr)
            continue;
          std::int64_t Extra = FrameOperand->Offset;
          if (Index + 1U >= Instruction->operandCount()) {
            return makeUnexpected(ErrorCode::VerificationError,
                                  "RISC-V frame operand lacks an adjacent immediate");
          }
          auto Immediate = immediateValue(*Instruction->operandAt(Index + 1U));
          if (!Immediate)
            return std::unexpected(Immediate.error());
          auto Added = checkedAdd(Extra, *Immediate);
          if (!Added)
            return std::unexpected(Added.error());
          Extra = *Added;
          const StackObject *Object = Frame.object(FrameOperand->Index);
          if (Object == nullptr || Object->description()->IsDead)
            return makeUnexpected(ErrorCode::VerificationError,
                                  "RISC-V frame instruction references an invalid object");
          auto Offset = objectReferenceOffset(Function, *Instruction, *Object->description(), Extra,
                                              Subtarget.features());
          if (!Offset)
            return std::unexpected(Offset.error());
          NeedsEmergency |= !fitsSigned12(*Offset);
        }
      }
    }
    if (NeedsEmergency && emergencyObject(Function) == nullptr) {
      auto Emergency = createEmergencyObject(Function);
      if (!Emergency)
        return std::unexpected(Emergency.error());
      StackSize =
          layoutFrame(Function, ReserveCallFrame, LibCallFrameSize, Options.EmitDebugOutput);
      if (!StackSize)
        return std::unexpected(StackSize.error());
    }

    State = Frame.state();
    State.StackSize = *StackSize;
    if (*StackSize < LibCallFrameSize) {
      return makeUnexpected(ErrorCode::InternalError,
                            "RISC-V save/restore frame exceeds the complete frame");
    }
    State.AdjustsStack = State.AdjustsStack || *StackSize != LibCallFrameSize;
    State.IsCalleeSavedInfoValid = true;
    State.SavePoints.clear();
    State.RestorePoints.clear();
    std::vector<PhysicalRegister> RestoredRegisters;
    std::ranges::copy_if(
        *SavedRegisters, std::back_inserter(RestoredRegisters),
        [&Function](PhysicalRegister Reg) { return calleeSaveIsRestored(Function, Reg); });
    if (HasRequestedPlacement) {
      if (RequestedSavePoints.size() != 1 || RequestedRestorePoints.empty()) {
        return makeUnexpected(
            ErrorCode::VerificationError,
            "RISC-V shrink-wrapped frame requires one save point and at least one restore point");
      }
      State.SavePoints.push_back({RequestedSavePoints.front().Block, *SavedRegisters});
      for (const MachineFrameSaveRestorePoint &Point : RequestedRestorePoints)
        State.RestorePoints.push_back({Point.Block, RestoredRegisters});
    } else {
      if (!SavedRegisters->empty() && Function.firstBlock() != nullptr) {
        State.SavePoints.push_back({Function.firstBlock()->stableID(), *SavedRegisters});
      }
      for (const MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
           Block = Block->next()) {
        bool HasEpilogue = false;
        for (const MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
             Instruction = Instruction->next()) {
          HasEpilogue |= isEpilogueTerminator(*Instruction, Target);
        }
        if (HasEpilogue && !RestoredRegisters.empty()) {
          State.RestorePoints.push_back({Block->stableID(), RestoredRegisters});
        }
      }
    }
    auto SetState = Frame.setState(std::move(State));
    if (!SetState)
      return std::unexpected(SetState.error());
    return {};
  }

  [[nodiscard]] Expected<void>
  insertCalleeSavedSpillsAndRestores(MachineFunction &Function,
                                     const TargetDescription &Target) const override {
    if (Function.frameInfo().isFrozen()) {
      return makeUnexpected(ErrorCode::InternalError,
                            "RISC-V callee saves must be inserted before frame freeze");
    }
    const std::span<const PhysicalRegister> Saved = Function.records().calleeSavedRegisters();
    if (Saved.empty())
      return {};
    MachineBasicBlock *SaveBlock = frameSaveBlock(Function);
    if (SaveBlock == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V callee-save frame has no valid save block");
    }
    auto FP = physicalRegister(PhysicalRegisterID::X8);
    auto BP = physicalRegister(PhysicalRegisterID::X9);
    if (!FP || !BP)
      return makeUnexpected(ErrorCode::InternalError, "RISC-V frame registers are unavailable");
    const bool Realign = requiresStackRealignment(Function);

    std::unordered_set<PhysicalRegister> KilledSaves;
    if (Function.properties().has(MachineFunctionProperty::TracksLiveness)) {
      for (const PhysicalRegister Reg : Saved) {
        const bool IsLiveOnEntry =
            std::ranges::any_of(SaveBlock->liveIns(), [&](const MachineBasicBlockLiveIn &LiveIn) {
              return Target.registers().regsOverlap(Reg, LiveIn.PhysReg);
            });
        if (!IsLiveOnEntry)
          KilledSaves.insert(Reg);
      }
    }

    if (Function.properties().has(MachineFunctionProperty::TracksLiveness)) {
      for (const PhysicalRegister Reg : Saved) {
        if (Function.registerInfo().isReserved(Reg))
          continue;
        auto LiveIn = SaveBlock->addLiveIn(Reg);
        if (!LiveIn)
          return std::unexpected(LiveIn.error());
      }
    }

    MachineIRBuilder SaveBuilder(Function, Target);
    auto SavePoint = SaveBlock->firstInstruction() != nullptr
                         ? SaveBuilder.setInsertPointBefore(*SaveBlock->firstInstruction())
                         : SaveBuilder.setInsertPoint(*SaveBlock);
    if (!SavePoint)
      return std::unexpected(SavePoint.error());
    const auto LibCallID = saveRestoreLibCallID(Saved, Subtarget.features());
    if (LibCallID) {
      auto Link = physicalRegister(PhysicalRegisterID::X5);
      auto SP = physicalRegister(PhysicalRegisterID::X2);
      if (!Link || !SP)
        return std::unexpected(!Link ? Link.error() : SP.error());
      auto Pending = SaveBuilder.buildInstr(instructionOpcode(InstructionOpcode::PseudoCALLReg));
      if (!Pending)
        return std::unexpected(Pending.error());
      if (auto Result = Pending->addDef(Register::fromPhysical(*Link)); !Result)
        return std::unexpected(Result.error());
      if (auto Result =
              Pending->addExternalSymbol("__riscv_save_" + std::to_string(*LibCallID), 0,
                                         static_cast<std::uint32_t>(CallTargetFlag::DirectCall));
          !Result) {
        return std::unexpected(Result.error());
      }
      if (auto Result = Pending->addImplicitUse(Register::fromPhysical(*SP)); !Result)
        return std::unexpected(Result.error());
      if (auto Result = Pending->addImplicitDef(Register::fromPhysical(*SP)); !Result)
        return std::unexpected(Result.error());
      for (const PhysicalRegister Reg : Saved) {
        if (auto Result = Pending->addImplicitUse(Register::fromPhysical(Reg)); !Result)
          return std::unexpected(Result.error());
      }
      if (auto Result = setFrameFlag(*Pending, MachineInstrFlag::FrameSetup); !Result)
        return std::unexpected(Result.error());
      auto Built = Pending->build();
      if (!Built)
        return std::unexpected(Built.error());
    }
    for (const PhysicalRegister Reg : Saved) {
      if (LibCallID && saveRestoreRegisterIndex(Reg))
        continue;
      const auto Slot = calleeSaveSlot(Function, Reg);
      if (!Slot) {
        return makeUnexpected(ErrorCode::InternalError,
                              "RISC-V callee-save register has no stack slot");
      }
      auto Stored = emitCalleeSave(SaveBuilder, Reg, *Slot, false, KilledSaves.contains(Reg));
      if (!Stored)
        return std::unexpected(Stored.error());
    }

    for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
         Block = Block->next()) {
      if (!isFrameRestoreBlock(Function, *Block))
        continue;
      std::vector<MachineInstr *> Terminators;
      for (MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
           Instruction = Instruction->next()) {
        if (isEpilogueTerminator(*Instruction, Target))
          Terminators.push_back(Instruction);
      }
      for (MachineInstr *Terminator : Terminators) {
        MachineIRBuilder RestoreBuilder(Function, Target);
        auto Point = RestoreBuilder.setInsertPointBefore(*Terminator);
        if (!Point)
          return std::unexpected(Point.error());
        const auto EmitRestore = [&](PhysicalRegister Reg) -> Expected<void> {
          if (!calleeSaveIsRestored(Function, Reg))
            return {};
          if (LibCallID && saveRestoreRegisterIndex(Reg))
            return {};
          const auto Slot = calleeSaveSlot(Function, Reg);
          if (!Slot) {
            return makeUnexpected(ErrorCode::InternalError,
                                  "RISC-V callee-save register has no restore slot");
          }
          auto Loaded = emitCalleeSave(RestoreBuilder, Reg, *Slot, true);
          if (!Loaded)
            return std::unexpected(Loaded.error());
          return {};
        };
        for (const PhysicalRegister Reg : Saved) {
          if ((requiresFramePointer(Function) && Reg == *FP) || (Realign && Reg == *BP))
            continue;
          auto Loaded = EmitRestore(Reg);
          if (!Loaded)
            return std::unexpected(Loaded.error());
        }
        if (requiresFramePointer(Function)) {
          auto Loaded = EmitRestore(*FP);
          if (!Loaded)
            return std::unexpected(Loaded.error());
        }
        if (Realign) {
          auto Loaded = EmitRestore(*BP);
          if (!Loaded)
            return std::unexpected(Loaded.error());
        }
      }
    }
    return {};
  }

  [[nodiscard]] Expected<void> finalizeFrame(MachineFunction &Function,
                                             const TargetDescription &) const override {
    MachineFrameInfo &Frame = Function.frameInfo();
    if (Frame.isFrozen())
      return makeUnexpected(ErrorCode::InternalError,
                            "RISC-V frame was frozen before final layout");
    std::vector<std::optional<std::int64_t>> PreliminaryOffsets;
    PreliminaryOffsets.reserve(Frame.objects().size());
    for (const StackObject &Object : Frame.objects()) {
      PreliminaryOffsets.push_back(Object.isTombstone() ? std::nullopt
                                                        : Object.description()->Offset);
    }
    const std::uint64_t PreliminarySize = Frame.state().StackSize;
    const std::uint64_t LibCallFrameSize =
        saveRestoreFrameSize(Function.records().calleeSavedRegisters(), Subtarget.features());
    auto StackSize = layoutFrame(Function, hasReservedCallFrame(Function), LibCallFrameSize);
    if (!StackSize)
      return std::unexpected(StackSize.error());
    if (*StackSize != PreliminarySize || Frame.objects().size() != PreliminaryOffsets.size()) {
      return makeUnexpected(ErrorCode::InternalError,
                            "RISC-V frame resources changed after preliminary layout");
    }
    for (const StackObject &Object : Frame.objects()) {
      if (!Object.isTombstone() &&
          Object.description()->Offset != PreliminaryOffsets[Object.index().value()]) {
        return makeUnexpected(ErrorCode::InternalError,
                              "RISC-V frame offsets changed after callee-save insertion");
      }
    }
    auto StackAlignment = Alignment::create(requiredStackAlignment(Function));
    if (!StackAlignment)
      return std::unexpected(StackAlignment.error());
    return Frame.freeze(*StackSize, *StackAlignment);
  }

  [[nodiscard]] bool isCallFramePseudo(const MachineInstr &Instruction) const noexcept override {
    return Instruction.opcode() == instructionOpcode(InstructionOpcode::ADJCALLSTACKDOWN) ||
           Instruction.opcode() == instructionOpcode(InstructionOpcode::ADJCALLSTACKUP);
  }

  [[nodiscard]] Expected<void>
  eliminateCallFramePseudo(MachineInstr &Instruction, MachineFunction &Function,
                           const TargetDescription &Target,
                           RegisterScavenger &Scavenger) const override {
    MachineBasicBlock *Block = Instruction.parent();
    if (Block == nullptr)
      return makeUnexpected(ErrorCode::VerificationError, "RISC-V call-frame pseudo is detached");
    if (!hasReservedCallFrame(Function)) {
      auto Amount = callFrameAmount(Instruction);
      if (!Amount)
        return std::unexpected(Amount.error());
      std::int64_t Adjustment = *Amount;
      if (Instruction.opcode() == instructionOpcode(InstructionOpcode::ADJCALLSTACKDOWN))
        Adjustment = -Adjustment;
      auto Adjusted =
          emitStackAdjustment(Instruction, Function, Target, Scavenger, Adjustment, std::nullopt);
      if (!Adjusted)
        return std::unexpected(Adjusted.error());
    }
    return Block->removeInstruction(Instruction);
  }

  [[nodiscard]] Expected<void> emitPrologue(MachineFunction &Function,
                                            const TargetDescription &Target,
                                            RegisterScavenger &Scavenger) const override {
    MachineBasicBlock *SaveBlock = frameSaveBlock(Function);
    if (SaveBlock == nullptr)
      return Function.frameInfo().state().StackSize == 0
                 ? Expected<void>{}
                 : makeUnexpected(ErrorCode::VerificationError,
                                  "RISC-V nonempty frame has no save block");
    const std::vector<CalleeSaveOperation> Saves =
        calleeSaveOperations(*SaveBlock, Function, false);
    const std::span<const PhysicalRegister> SavedRegisters =
        Function.records().calleeSavedRegisters();
    const auto LibCallID = saveRestoreLibCallID(SavedRegisters, Subtarget.features());
    const std::size_t ManualSaveCount =
        static_cast<std::size_t>(std::ranges::count_if(SavedRegisters, [&](PhysicalRegister Reg) {
          return !LibCallID || !saveRestoreRegisterIndex(Reg);
        }));
    if (Saves.size() != ManualSaveCount) {
      return makeUnexpected(ErrorCode::InternalError,
                            "RISC-V prologue has an incomplete callee-save sequence");
    }
    MachineInstr *SpillLibCall = nullptr;
    if (LibCallID) {
      for (MachineInstr *Instruction = SaveBlock->firstInstruction(); Instruction != nullptr;
           Instruction = Instruction->next()) {
        if (Instruction->opcode() == instructionOpcode(InstructionOpcode::PseudoCALLReg) &&
            Instruction->flags().has(MachineInstrFlag::FrameSetup)) {
          if (SpillLibCall != nullptr) {
            return makeUnexpected(ErrorCode::InternalError,
                                  "RISC-V prologue has duplicate save libcalls");
          }
          SpillLibCall = Instruction;
        }
      }
      if (SpillLibCall == nullptr) {
        return makeUnexpected(ErrorCode::InternalError,
                              "RISC-V prologue is missing its save libcall");
      }
    }
    MachineInstr *Anchor = SaveBlock->firstInstruction();
    MachineIRBuilder Builder(Function, Target);
    auto Point = SpillLibCall != nullptr
                     ? Builder.setInsertPointAfter(*SpillLibCall)
                     : (Anchor != nullptr ? Builder.setInsertPointBefore(*Anchor)
                                          : Builder.setInsertPoint(*SaveBlock));
    if (!Point)
      return std::unexpected(Point.error());

    const std::uint64_t StackSize = Function.frameInfo().state().StackSize;
    const std::uint64_t StackAlignment = requiredStackAlignment(Function);
    const bool Realign = requiresStackRealignment(Function);
    auto SP = physicalRegister(PhysicalRegisterID::X2);
    auto FP = physicalRegister(PhysicalRegisterID::X8);
    auto BP = physicalRegister(PhysicalRegisterID::X9);
    auto Zero = physicalRegister(PhysicalRegisterID::X0);
    auto DefaultTemporary = physicalRegister(PhysicalRegisterID::X6);
    if (!SP || !FP || !BP || !Zero || !DefaultTemporary) {
      return makeUnexpected(ErrorCode::InternalError, "RISC-V frame registers are unavailable");
    }
    PhysicalRegister Temporary = *DefaultTemporary;
    std::optional<ScavengedRegister> Scratch;
    const std::uint64_t LibCallFrameSize =
        saveRestoreFrameSize(SavedRegisters, Subtarget.features());
    if (StackSize < LibCallFrameSize) {
      return makeUnexpected(ErrorCode::InternalError,
                            "RISC-V save/restore frame exceeds the complete frame");
    }
    const std::uint64_t ManagedStackSize = StackSize - LibCallFrameSize;
    const std::uint64_t FirstAdjustment = firstSPAdjustAmount(Function, Subtarget.features());
    const std::uint64_t InitialAllocation =
        FirstAdjustment == 0 ? ManagedStackSize : FirstAdjustment;
    const std::uint64_t SecondAdjustment = ManagedStackSize - InitialAllocation;
    const std::uint64_t FramePointerAdjustment = LibCallFrameSize + InitialAllocation;
    const std::int64_t InitialAmount = -static_cast<std::int64_t>(InitialAllocation);
    const std::int64_t SecondAmount = -static_cast<std::int64_t>(SecondAdjustment);
    const std::int64_t AlignmentMask = std::bit_cast<std::int64_t>(~(StackAlignment - 1U));
    const bool NeedsTemporary =
        !canEmitAddImmediateAdjustment(InitialAmount, StackAlignmentBytes) ||
        !canEmitAddImmediateAdjustment(SecondAmount, StackAlignmentBytes) ||
        (!Realign && requiresFramePointer(Function) &&
         !canEmitAddImmediateAdjustment(static_cast<std::int64_t>(FramePointerAdjustment),
                                        StackAlignmentBytes)) ||
        (Realign && !fitsSigned12(AlignmentMask));
    if (NeedsTemporary) {
      auto Candidates = scratchRegisters();
      if (!Candidates)
        return std::unexpected(Candidates.error());
      if (Anchor != nullptr) {
        auto Scavenged = Scavenger.scavenge(*Anchor, *Candidates);
        if (!Scavenged)
          return std::unexpected(Scavenged.error());
        Temporary = Scavenged->Reg;
        Scratch = *Scavenged;
      } else {
        Temporary = Candidates->front();
      }
    }

    if (LibCallFrameSize != 0) {
      auto CFA = emitDefCFAOffset(Builder, static_cast<std::int64_t>(LibCallFrameSize),
                                  MachineInstrFlag::FrameSetup);
      if (!CFA)
        return std::unexpected(CFA.error());
      for (const PhysicalRegister Reg : SavedRegisters) {
        if (!saveRestoreRegisterIndex(Reg))
          continue;
        const auto Slot = calleeSaveSlot(Function, Reg);
        const StackObject *Object = Slot ? Function.frameInfo().object(*Slot) : nullptr;
        if (Object == nullptr || !Object->description()->Offset) {
          return makeUnexpected(ErrorCode::InternalError,
                                "RISC-V save libcall stack slot is incomplete");
        }
        auto Saved = emitSavedRegisterCFI(Builder, Target, Reg, *Object->description()->Offset,
                                          MachineInstrFlag::FrameSetup);
        if (!Saved)
          return std::unexpected(Saved.error());
      }
    }

    const std::uint64_t AllocationSize = InitialAllocation;
    if (AllocationSize > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return makeUnexpected(ErrorCode::FrameLayoutFailed, "RISC-V stack adjustment exceeds int64");
    }
    if (AllocationSize != 0) {
      if (canEmitAddImmediateAdjustment(InitialAmount, StackAlignmentBytes)) {
        auto Adjust = emitAddImmediateAdjustment(Builder, *SP, *SP, InitialAmount,
                                                 StackAlignmentBytes, MachineInstrFlag::FrameSetup);
        if (!Adjust)
          return std::unexpected(Adjust.error());
      } else {
        if (Scratch && Scratch->RequiresSpill) {
          return makeUnexpected(ErrorCode::RegisterAllocationFailed,
                                "RISC-V initial stack adjustment has no dead scratch register");
        }
        auto Constant =
            emitConstant(Builder, Temporary, *Zero, static_cast<std::int64_t>(AllocationSize),
                         MachineInstrFlag::FrameSetup);
        if (!Constant)
          return std::unexpected(Constant.error());
        auto Adjust =
            emitRegisterSub(Builder, *SP, *SP, Temporary, MachineInstrFlag::FrameSetup, true);
        if (!Adjust)
          return std::unexpected(Adjust.error());
      }
      if (!Realign) {
        auto CFA =
            emitDefCFAOffset(Builder, static_cast<std::int64_t>(LibCallFrameSize + AllocationSize),
                             MachineInstrFlag::FrameSetup);
        if (!CFA)
          return std::unexpected(CFA.error());
      }
    }
    if (Realign) {
      if (Saves.empty() && SpillLibCall == nullptr) {
        return makeUnexpected(ErrorCode::InternalError,
                              "RISC-V realigned frame has no callee-save sequence");
      }
      MachineIRBuilder AfterManualSaves(Function, Target);
      MachineIRBuilder *AfterSaves = &Builder;
      if (!Saves.empty()) {
        auto PointAfterSaves = AfterManualSaves.setInsertPointAfter(*Saves.back().Instruction);
        if (!PointAfterSaves)
          return std::unexpected(PointAfterSaves.error());
        AfterSaves = &AfterManualSaves;
      }
      auto Set =
          emitAddImmediate(*AfterSaves, *FP, *SP, static_cast<std::int64_t>(FramePointerAdjustment),
                           MachineInstrFlag::FrameSetup);
      if (!Set)
        return std::unexpected(Set.error());
      auto CFA = emitDefCFI(*AfterSaves, Target, *FP, 0, MachineInstrFlag::FrameSetup);
      if (!CFA)
        return std::unexpected(CFA.error());
      for (const CalleeSaveOperation &Save : Saves) {
        const auto Slot = calleeSaveSlot(Function, Save.Reg);
        const StackObject *Object = Slot ? Function.frameInfo().object(*Slot) : nullptr;
        if (Object == nullptr || !Object->description()->Offset) {
          return makeUnexpected(ErrorCode::InternalError,
                                "RISC-V realigned CFI slot is incomplete");
        }
        auto CFI =
            emitSavedRegisterCFI(*AfterSaves, Target, Save.Reg, *Object->description()->Offset,
                                 MachineInstrFlag::FrameSetup);
        if (!CFI)
          return std::unexpected(CFI.error());
      }

      std::optional<FrameIndexReference> Emergency;
      if (Scratch && Scratch->RequiresSpill) {
        const StackObject *Object = emergencyObject(Function);
        if (Object == nullptr) {
          return makeUnexpected(ErrorCode::RegisterAllocationFailed,
                                "RISC-V prologue requires an emergency spill slot");
        }
        auto Reference = emergencyFrameReference(Function, *Object->description());
        if (!Reference || !fitsSigned12(Reference->Offset)) {
          return makeUnexpected(ErrorCode::RegisterAllocationFailed,
                                "RISC-V prologue emergency spill slot is unavailable");
        }
        Emergency = *Reference;
        auto Saved = emitScratchAccess(*AfterSaves, Temporary, *Emergency, false);
        if (!Saved)
          return std::unexpected(Saved.error());
      }

      if (SecondAdjustment != 0) {
        if (SecondAdjustment >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
          return makeUnexpected(ErrorCode::FrameLayoutFailed,
                                "RISC-V second stack adjustment exceeds int64");
        }
        if (canEmitAddImmediateAdjustment(SecondAmount, StackAlignmentBytes)) {
          auto Adjust =
              emitAddImmediateAdjustment(*AfterSaves, *SP, *SP, SecondAmount, StackAlignmentBytes,
                                         MachineInstrFlag::FrameSetup);
          if (!Adjust)
            return std::unexpected(Adjust.error());
        } else {
          auto Constant = emitConstant(*AfterSaves, Temporary, *Zero,
                                       static_cast<std::int64_t>(SecondAdjustment),
                                       MachineInstrFlag::FrameSetup);
          if (!Constant)
            return std::unexpected(Constant.error());
          auto Adjust =
              emitRegisterSub(*AfterSaves, *SP, *SP, Temporary, MachineInstrFlag::FrameSetup, true);
          if (!Adjust)
            return std::unexpected(Adjust.error());
        }
      }

      if (fitsSigned12(AlignmentMask)) {
        auto Aligned =
            emitAndImmediate(*AfterSaves, *SP, *SP, AlignmentMask, MachineInstrFlag::FrameSetup);
        if (!Aligned)
          return std::unexpected(Aligned.error());
      } else {
        const std::int64_t ShiftAmount =
            static_cast<std::int64_t>(std::countr_zero(StackAlignment));
        auto Shifted =
            emitShiftImmediate(*AfterSaves, instructionOpcode(InstructionOpcode::SRLI), Temporary,
                               *SP, ShiftAmount, MachineInstrFlag::FrameSetup);
        if (!Shifted)
          return std::unexpected(Shifted.error());
        Shifted = emitShiftImmediate(*AfterSaves, instructionOpcode(InstructionOpcode::SLLI), *SP,
                                     Temporary, ShiftAmount, MachineInstrFlag::FrameSetup);
        if (!Shifted)
          return std::unexpected(Shifted.error());
      }
      if (requiresBasePointer(Function)) {
        auto Base = emitAddImmediate(*AfterSaves, *BP, *SP, 0, MachineInstrFlag::FrameSetup);
        if (!Base)
          return std::unexpected(Base.error());
      }
      if (Emergency) {
        auto Restored = emitScratchAccess(*AfterSaves, Temporary, *Emergency, true);
        if (!Restored)
          return std::unexpected(Restored.error());
      }
      return {};
    }

    if (!Saves.empty()) {
      MachineIRBuilder AfterSaves(Function, Target);
      auto PointAfterSaves = AfterSaves.setInsertPointAfter(*Saves.back().Instruction);
      if (!PointAfterSaves)
        return std::unexpected(PointAfterSaves.error());
      for (const CalleeSaveOperation &Save : Saves) {
        const auto Slot = calleeSaveSlot(Function, Save.Reg);
        const StackObject *Object = Slot ? Function.frameInfo().object(*Slot) : nullptr;
        if (Object == nullptr || !Object->description()->Offset) {
          return makeUnexpected(ErrorCode::InternalError,
                                "RISC-V callee-save CFI slot is incomplete");
        }
        auto CFI =
            emitSavedRegisterCFI(AfterSaves, Target, Save.Reg, *Object->description()->Offset,
                                 MachineInstrFlag::FrameSetup);
        if (!CFI)
          return std::unexpected(CFI.error());
      }
      if (requiresFramePointer(Function)) {
        const std::int64_t FramePointerAmount = static_cast<std::int64_t>(FramePointerAdjustment);
        Expected<void> Set =
            canEmitAddImmediateAdjustment(FramePointerAmount, StackAlignmentBytes)
                ? emitAddImmediateAdjustment(AfterSaves, *FP, *SP, FramePointerAmount,
                                             StackAlignmentBytes, MachineInstrFlag::FrameSetup)
                : emitConstant(AfterSaves, Temporary, *Zero, FramePointerAmount,
                               MachineInstrFlag::FrameSetup);
        if (!Set)
          return std::unexpected(Set.error());
        if (!canEmitAddImmediateAdjustment(FramePointerAmount, StackAlignmentBytes)) {
          Set =
              emitRegisterAdd(AfterSaves, *FP, *SP, Temporary, MachineInstrFlag::FrameSetup, true);
          if (!Set)
            return std::unexpected(Set.error());
        }
        auto CFA = emitDefCFI(AfterSaves, Target, *FP, 0, MachineInstrFlag::FrameSetup);
        if (!CFA)
          return std::unexpected(CFA.error());
      }
      if (SecondAdjustment != 0) {
        if (canEmitAddImmediateAdjustment(SecondAmount, StackAlignmentBytes)) {
          auto Adjust =
              emitAddImmediateAdjustment(AfterSaves, *SP, *SP, SecondAmount, StackAlignmentBytes,
                                         MachineInstrFlag::FrameSetup);
          if (!Adjust)
            return std::unexpected(Adjust.error());
        } else {
          auto Constant = emitConstant(AfterSaves, Temporary, *Zero,
                                       static_cast<std::int64_t>(SecondAdjustment),
                                       MachineInstrFlag::FrameSetup);
          if (!Constant)
            return std::unexpected(Constant.error());
          auto Adjust =
              emitRegisterSub(AfterSaves, *SP, *SP, Temporary, MachineInstrFlag::FrameSetup, true);
          if (!Adjust)
            return std::unexpected(Adjust.error());
        }
        if (!requiresFramePointer(Function)) {
          auto CFA = emitDefCFAOffset(AfterSaves, static_cast<std::int64_t>(StackSize),
                                      MachineInstrFlag::FrameSetup);
          if (!CFA)
            return std::unexpected(CFA.error());
        }
      }
    }
    if (Saves.empty() && requiresFramePointer(Function)) {
      const std::int64_t FramePointerAmount = static_cast<std::int64_t>(FramePointerAdjustment);
      Expected<void> Set =
          canEmitAddImmediateAdjustment(FramePointerAmount, StackAlignmentBytes)
              ? emitAddImmediateAdjustment(Builder, *FP, *SP, FramePointerAmount,
                                           StackAlignmentBytes, MachineInstrFlag::FrameSetup)
              : emitConstant(Builder, Temporary, *Zero, FramePointerAmount,
                             MachineInstrFlag::FrameSetup);
      if (!Set)
        return std::unexpected(Set.error());
      if (!canEmitAddImmediateAdjustment(FramePointerAmount, StackAlignmentBytes)) {
        Set = emitRegisterAdd(Builder, *FP, *SP, Temporary, MachineInstrFlag::FrameSetup, true);
        if (!Set)
          return std::unexpected(Set.error());
      }
      auto CFA = emitDefCFI(Builder, Target, *FP, 0, MachineInstrFlag::FrameSetup);
      if (!CFA)
        return std::unexpected(CFA.error());
    }
    return {};
  }

  [[nodiscard]] bool isEpilogueTerminator(const MachineInstr &Instruction,
                                          const TargetDescription &Target) const noexcept override {
    if (Instruction.opcode() == instructionOpcode(InstructionOpcode::PseudoTAIL))
      return true;
    const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction.opcode());
    return Descriptor != nullptr && Descriptor->Flags.has(InstructionFlag::Return);
  }

  [[nodiscard]] Expected<void> emitEpilogue(MachineBasicBlock &Block, MachineInstr &Terminator,
                                            MachineFunction &Function,
                                            const TargetDescription &Target,
                                            RegisterScavenger &Scavenger) const override {
    if (!isFrameRestoreBlock(Function, Block))
      return {};
    auto FP = physicalRegister(PhysicalRegisterID::X8);
    auto SP = physicalRegister(PhysicalRegisterID::X2);
    auto BP = physicalRegister(PhysicalRegisterID::X9);
    if (!FP || !SP || !BP) {
      return makeUnexpected(ErrorCode::InternalError, "RISC-V frame registers are unavailable");
    }
    const bool HasFP = requiresFramePointer(Function);
    const bool Realign = requiresStackRealignment(Function);
    const std::span<const PhysicalRegister> SavedRegisters =
        Function.records().calleeSavedRegisters();
    const auto LibCallID = saveRestoreLibCallID(SavedRegisters, Subtarget.features());
    const std::uint64_t LibCallFrameSize =
        saveRestoreFrameSize(SavedRegisters, Subtarget.features());
    const std::uint64_t TotalStackSize = Function.frameInfo().state().StackSize;
    if (TotalStackSize < LibCallFrameSize) {
      return makeUnexpected(ErrorCode::InternalError,
                            "RISC-V save/restore frame exceeds the complete frame");
    }
    const std::uint64_t StackSize = TotalStackSize - LibCallFrameSize;

    const std::vector<CalleeSaveOperation> BlockRestores =
        calleeSaveOperations(Block, Function, true);
    std::vector<CalleeSaveOperation> Restores;
    for (MachineInstr *Cursor = Terminator.previous(); Cursor != nullptr;
         Cursor = Cursor->previous()) {
      const auto Position =
          std::ranges::find(BlockRestores, Cursor, &CalleeSaveOperation::Instruction);
      if (Position == BlockRestores.end())
        break;
      Restores.push_back(*Position);
    }
    std::ranges::reverse(Restores);
    const std::size_t ExpectedRestores =
        static_cast<std::size_t>(std::ranges::count_if(SavedRegisters, [&](PhysicalRegister Reg) {
          return calleeSaveIsRestored(Function, Reg) &&
                 (!LibCallID || !saveRestoreRegisterIndex(Reg));
        }));
    if (Restores.size() != ExpectedRestores) {
      return makeUnexpected(ErrorCode::InternalError,
                            "RISC-V epilogue has an incomplete callee-restore sequence");
    }

    if (!Restores.empty()) {
      MachineIRBuilder AfterRestores(Function, Target);
      auto Point = AfterRestores.setInsertPointAfter(*Restores.back().Instruction);
      if (!Point)
        return std::unexpected(Point.error());
      for (const CalleeSaveOperation &Restore : Restores) {
        auto CFI = emitRestoreRegisterCFI(AfterRestores, Target, Restore.Reg,
                                          MachineInstrFlag::FrameDestroy);
        if (!CFI)
          return std::unexpected(CFI.error());
      }
    }

    if (Realign) {
      const std::uint64_t FirstAdjustment = firstSPAdjustAmount(Function, Subtarget.features());
      if (FirstAdjustment > static_cast<std::uint64_t>(MaxSigned12)) {
        return makeUnexpected(ErrorCode::FrameLayoutFailed,
                              "RISC-V first epilogue adjustment exceeds signed 12 bits");
      }
      MachineIRBuilder Teardown(Function, Target);
      MachineInstr *FirstRestore = Restores.empty() ? &Terminator : Restores.front().Instruction;
      auto Point = Teardown.setInsertPointBefore(*FirstRestore);
      if (!Point)
        return std::unexpected(Point.error());
      auto RestoreSP =
          emitAddImmediate(Teardown, *SP, *FP, -static_cast<std::int64_t>(FirstAdjustment),
                           MachineInstrFlag::FrameDestroy);
      if (!RestoreSP)
        return std::unexpected(RestoreSP.error());
      auto CFA = emitDefCFI(Teardown, Target, *SP, 0, MachineInstrFlag::FrameDestroy);
      if (!CFA)
        return std::unexpected(CFA.error());
      if (FirstAdjustment != 0) {
        MachineIRBuilder Deallocate(Function, Target);
        auto BeforeTerminator = Deallocate.setInsertPointBefore(Terminator);
        if (!BeforeTerminator)
          return std::unexpected(BeforeTerminator.error());
        auto Adjust =
            emitAddImmediate(Deallocate, *SP, *SP, static_cast<std::int64_t>(FirstAdjustment),
                             MachineInstrFlag::FrameDestroy);
        if (!Adjust)
          return std::unexpected(Adjust.error());
      }
    } else if (HasFP || firstSPAdjustAmount(Function, Subtarget.features()) != 0) {
      const std::uint64_t FirstAdjustment = firstSPAdjustAmount(Function, Subtarget.features());
      const std::uint64_t RestoreAreaSize = FirstAdjustment == 0 ? StackSize : FirstAdjustment;
      const std::uint64_t SecondAdjustment = StackSize - RestoreAreaSize;
      MachineInstr *FirstRestore = Restores.empty() ? &Terminator : Restores.front().Instruction;
      if (SecondAdjustment != 0) {
        if (SecondAdjustment >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
          return makeUnexpected(ErrorCode::FrameLayoutFailed,
                                "RISC-V second epilogue adjustment exceeds int64");
        }
        auto Adjusted = emitStackAdjustment(*FirstRestore, Function, Target, Scavenger,
                                            static_cast<std::int64_t>(SecondAdjustment),
                                            MachineInstrFlag::FrameDestroy);
        if (!Adjusted)
          return std::unexpected(Adjusted.error());
      }
      MachineIRBuilder Teardown(Function, Target);
      auto Point = Teardown.setInsertPointBefore(*FirstRestore);
      if (!Point)
        return std::unexpected(Point.error());
      if (SecondAdjustment == 0) {
        if (!HasFP) {
          return makeUnexpected(ErrorCode::InternalError,
                                "RISC-V split epilogue has no second adjustment");
        }
        auto RestoreSP = emitAddImmediateAdjustment(
            Teardown, *SP, *FP, -static_cast<std::int64_t>(RestoreAreaSize), StackAlignmentBytes,
            MachineInstrFlag::FrameDestroy);
        if (!RestoreSP)
          return std::unexpected(RestoreSP.error());
      }
      auto CFA = HasFP
                     ? emitDefCFI(Teardown, Target, *SP, static_cast<std::int64_t>(RestoreAreaSize),
                                  MachineInstrFlag::FrameDestroy)
                     : emitDefCFAOffset(Teardown, static_cast<std::int64_t>(RestoreAreaSize),
                                        MachineInstrFlag::FrameDestroy);
      if (!CFA)
        return std::unexpected(CFA.error());
      if (RestoreAreaSize != 0) {
        MachineIRBuilder Deallocate(Function, Target);
        auto BeforeTerminator = Deallocate.setInsertPointBefore(Terminator);
        if (!BeforeTerminator)
          return std::unexpected(BeforeTerminator.error());
        auto Adjust =
            emitAddImmediate(Deallocate, *SP, *SP, static_cast<std::int64_t>(RestoreAreaSize),
                             MachineInstrFlag::FrameDestroy);
        if (!Adjust)
          return std::unexpected(Adjust.error());
        auto FinalCFA = emitDefCFAOffset(Deallocate, 0, MachineInstrFlag::FrameDestroy);
        if (!FinalCFA)
          return std::unexpected(FinalCFA.error());
      }
    } else if (StackSize != 0) {
      if (StackSize > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return makeUnexpected(ErrorCode::FrameLayoutFailed,
                              "RISC-V stack adjustment exceeds int64");
      MachineIRBuilder Teardown(Function, Target);
      auto Point = Teardown.setInsertPointBefore(Terminator);
      if (!Point)
        return std::unexpected(Point.error());
      const std::int64_t Amount = static_cast<std::int64_t>(StackSize);
      if (canEmitAddImmediateAdjustment(Amount, StackAlignmentBytes)) {
        auto Adjust = emitAddImmediateAdjustment(Teardown, *SP, *SP, Amount, StackAlignmentBytes,
                                                 MachineInstrFlag::FrameDestroy);
        if (!Adjust)
          return std::unexpected(Adjust.error());
      } else {
        auto Candidates = scratchRegisters();
        auto Zero = physicalRegister(PhysicalRegisterID::X0);
        if (!Candidates || !Zero)
          return std::unexpected(!Candidates ? Candidates.error() : Zero.error());
        auto Scratch = Scavenger.scavenge(Terminator, *Candidates);
        if (!Scratch)
          return std::unexpected(Scratch.error());
        if (Scratch->RequiresSpill) {
          return makeUnexpected(ErrorCode::RegisterAllocationFailed,
                                "RISC-V epilogue has no dead scratch register");
        }
        auto Constant =
            emitConstant(Teardown, Scratch->Reg, *Zero, Amount, MachineInstrFlag::FrameDestroy);
        if (!Constant)
          return std::unexpected(Constant.error());
        auto Adjust =
            emitRegisterAdd(Teardown, *SP, *SP, Scratch->Reg, MachineInstrFlag::FrameDestroy, true);
        if (!Adjust)
          return std::unexpected(Adjust.error());
      }
      auto CFA = emitDefCFAOffset(Teardown, 0, MachineInstrFlag::FrameDestroy);
      if (!CFA)
        return std::unexpected(CFA.error());
    }
    if (LibCallID) {
      MachineIRBuilder RestoreBuilder(Function, Target);
      auto Point = RestoreBuilder.setInsertPointBefore(Terminator);
      if (!Point)
        return std::unexpected(Point.error());
      auto Pending = RestoreBuilder.buildInstr(instructionOpcode(InstructionOpcode::PseudoTAIL));
      if (!Pending)
        return std::unexpected(Pending.error());
      if (auto Result =
              Pending->addExternalSymbol("__riscv_restore_" + std::to_string(*LibCallID), 0,
                                         static_cast<std::uint32_t>(CallTargetFlag::DirectCall));
          !Result) {
        return std::unexpected(Result.error());
      }
      if (auto Result = Pending->addImplicitUse(Register::fromPhysical(*SP)); !Result)
        return std::unexpected(Result.error());
      if (auto Result = Pending->addImplicitDef(Register::fromPhysical(*SP)); !Result)
        return std::unexpected(Result.error());
      for (const PhysicalRegister Reg : SavedRegisters) {
        if (!calleeSaveIsRestored(Function, Reg))
          continue;
        if (auto Result = Pending->addImplicitDef(Register::fromPhysical(Reg)); !Result)
          return std::unexpected(Result.error());
      }
      if (auto Result = setFrameFlag(*Pending, MachineInstrFlag::FrameDestroy); !Result)
        return std::unexpected(Result.error());
      auto Built = Pending->build();
      if (!Built)
        return std::unexpected(Built.error());
      auto Removed = Block.removeInstruction(Terminator);
      if (!Removed)
        return std::unexpected(Removed.error());
    }
    return {};
  }

  [[nodiscard]] Expected<FrameIndexReference>
  getFrameIndexReference(const MachineFunction &Function, const MachineInstr &Instruction,
                         FrameIndex Index, std::int64_t OperandOffset) const override {
    const StackObject *Object = Function.frameInfo().object(Index);
    if (Object == nullptr || Object->description()->IsDead)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V FrameIndex references an invalid stack object");
    if (Object->isVariableSized())
      return makeUnexpected(ErrorCode::Unsupported,
                            "RISC-V variable-sized object cannot be addressed by FrameIndex");
    auto Offset = objectReferenceOffset(Function, Instruction, *Object->description(),
                                        OperandOffset, Subtarget.features());
    if (!Offset)
      return std::unexpected(Offset.error());
    const StackObjectDesc &Description = *Object->description();
    PhysicalRegisterID BaseID = PhysicalRegisterID::X2;
    if (Description.Kind == StackObjectKind::CalleeSaved) {
      BaseID = PhysicalRegisterID::X2;
    } else if (requiresStackRealignment(Function) && !Object->isFixed()) {
      BaseID = requiresBasePointer(Function) ? PhysicalRegisterID::X9 : PhysicalRegisterID::X2;
    } else if (!isOutgoingCallObject(Description) &&
               !Instruction.flags().has(MachineInstrFlag::FrameSetup)) {
      const bool FixedObject = Description.Kind == StackObjectKind::FixedABI ||
                               Description.Kind == StackObjectKind::FixedSpillSlot;
      const bool HasFP = requiresFramePointer(Function);
      const bool HasBP = requiresBasePointer(Function);
      if (HasFP && (FixedObject || !HasBP)) {
        BaseID = PhysicalRegisterID::X8;
      } else if (HasBP) {
        BaseID = PhysicalRegisterID::X9;
      }
    }
    auto Base = physicalRegister(BaseID);
    if (!Base)
      return std::unexpected(Base.error());
    return FrameIndexReference{*Base, *Offset};
  }

  [[nodiscard]] Expected<void> eliminateFrameIndex(MachineInstr &Instruction,
                                                   std::size_t OperandIndex,
                                                   MachineFunction &Function,
                                                   const TargetDescription &Target,
                                                   RegisterScavenger &Scavenger) const override {
    const auto *Frame = Instruction.operandAt(OperandIndex)->getIf<FrameIndexOperand>();
    if (Frame == nullptr)
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "RISC-V frame elimination requires a FrameIndex operand");
    if (OperandIndex + 1U >= Instruction.operandCount())
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V frame operand lacks an adjacent immediate");
    auto Immediate = immediateValue(*Instruction.operandAt(OperandIndex + 1U));
    if (!Immediate)
      return std::unexpected(Immediate.error());
    auto Extra = checkedAdd(Frame->Offset, *Immediate);
    if (!Extra)
      return std::unexpected(Extra.error());
    auto Reference = getFrameIndexReference(Function, Instruction, Frame->Index, *Extra);
    if (!Reference)
      return std::unexpected(Reference.error());
    if (fitsSigned12(Reference->Offset)) {
      auto Base = replaceWithRegister(Instruction, OperandIndex, Reference->BaseRegister);
      if (!Base)
        return std::unexpected(Base.error());
      return replaceImmediate(Instruction, OperandIndex + 1U, Reference->Offset);
    }

    auto Candidates = scratchRegisters();
    if (!Candidates)
      return std::unexpected(Candidates.error());
    auto Scratch = Scavenger.scavenge(Instruction, *Candidates);
    if (!Scratch)
      return std::unexpected(Scratch.error());
    std::optional<FrameIndexReference> Emergency;
    if (Scratch->RequiresSpill) {
      const StackObject *Object = emergencyObject(Function);
      if (Object == nullptr)
        return makeUnexpected(ErrorCode::RegisterAllocationFailed,
                              "RISC-V scavenging requires an emergency spill slot");
      auto Ref = emergencyFrameReference(Function, *Object->description());
      if (!Ref)
        return std::unexpected(Ref.error());
      if (!fitsSigned12(Ref->Offset)) {
        return makeUnexpected(ErrorCode::RegisterAllocationFailed,
                              "RISC-V emergency spill slot is out of range");
      }
      Emergency = *Ref;
      auto Saved =
          emitResolvedScratchAccess(Instruction, Function, Target, Scratch->Reg, *Emergency, false);
      if (!Saved)
        return std::unexpected(Saved.error());
    }

    MachineIRBuilder Builder(Function, Target);
    auto Point = Builder.setInsertPointBefore(Instruction);
    if (!Point)
      return std::unexpected(Point.error());
    auto Zero = physicalRegister(PhysicalRegisterID::X0);
    if (!Zero)
      return std::unexpected(Zero.error());
    std::int64_t InstructionOffset = 0;
    std::int64_t AddressOffset = Reference->Offset;
    if (Instruction.opcode() != instructionOpcode(InstructionOpcode::ADDI)) {
      InstructionOffset = Reference->Offset & 0xFFF;
      if (InstructionOffset >= 0x800)
        InstructionOffset -= 0x1000;
      AddressOffset -= InstructionOffset;
    }
    auto Constant = emitConstant(Builder, Scratch->Reg, *Zero, AddressOffset, std::nullopt);
    if (!Constant)
      return std::unexpected(Constant.error());
    auto Address = emitRegisterAdd(Builder, Scratch->Reg, Reference->BaseRegister, Scratch->Reg,
                                   std::nullopt, true);
    if (!Address)
      return std::unexpected(Address.error());
    auto Base = replaceWithRegister(Instruction, OperandIndex, Scratch->Reg);
    if (!Base)
      return std::unexpected(Base.error());
    auto Offset = replaceImmediate(Instruction, OperandIndex + 1U, InstructionOffset);
    if (!Offset)
      return std::unexpected(Offset.error());
    if (Emergency) {
      auto Restored =
          emitResolvedScratchAccess(Instruction, Function, Target, Scratch->Reg, *Emergency, true);
      if (!Restored)
        return std::unexpected(Restored.error());
    }
    return {};
  }

private:
  [[nodiscard]] Expected<void> emitStackAdjustment(MachineInstr &Anchor, MachineFunction &Function,
                                                   const TargetDescription &Target,
                                                   RegisterScavenger &Scavenger,
                                                   std::int64_t Amount,
                                                   std::optional<MachineInstrFlag> Flag) const {
    if (Amount == 0)
      return {};
    auto SP = physicalRegister(PhysicalRegisterID::X2);
    if (!SP)
      return std::unexpected(SP.error());
    MachineIRBuilder Builder(Function, Target);
    auto Point = Builder.setInsertPointBefore(Anchor);
    if (!Point)
      return std::unexpected(Point.error());
    if (canEmitAddImmediateAdjustment(Amount, StackAlignmentBytes)) {
      return emitAddImmediateAdjustment(Builder, *SP, *SP, Amount, StackAlignmentBytes, Flag);
    }

    auto Candidates = scratchRegisters();
    if (!Candidates)
      return std::unexpected(Candidates.error());
    auto Scratch = Scavenger.scavenge(Anchor, *Candidates);
    if (!Scratch)
      return std::unexpected(Scratch.error());
    std::optional<FrameIndexReference> Emergency;
    if (Scratch->RequiresSpill) {
      const StackObject *Object = emergencyObject(Function);
      if (Object == nullptr)
        return makeUnexpected(ErrorCode::RegisterAllocationFailed,
                              "RISC-V stack adjustment requires an emergency spill slot");
      auto Ref = emergencyFrameReference(Function, *Object->description());
      if (!Ref || !fitsSigned12(Ref->Offset))
        return makeUnexpected(ErrorCode::RegisterAllocationFailed,
                              "RISC-V emergency spill slot is unavailable");
      Emergency = *Ref;
      auto Saved =
          emitResolvedScratchAccess(Anchor, Function, Target, Scratch->Reg, *Emergency, false);
      if (!Saved)
        return std::unexpected(Saved.error());
    }
    auto Zero = physicalRegister(PhysicalRegisterID::X0);
    if (!Zero)
      return std::unexpected(Zero.error());
    const bool Subtract = Amount < 0;
    const std::int64_t Magnitude = Subtract ? -Amount : Amount;
    auto Constant = emitConstant(Builder, Scratch->Reg, *Zero, Magnitude, Flag);
    if (!Constant)
      return std::unexpected(Constant.error());
    auto Adjust = Subtract ? emitRegisterSub(Builder, *SP, *SP, Scratch->Reg, Flag, true)
                           : emitRegisterAdd(Builder, *SP, *SP, Scratch->Reg, Flag, true);
    if (!Adjust)
      return std::unexpected(Adjust.error());
    if (Emergency) {
      auto Restored =
          emitResolvedScratchAccess(Anchor, Function, Target, Scratch->Reg, *Emergency, true);
      if (!Restored)
        return std::unexpected(Restored.error());
    }
    return {};
  }

  [[nodiscard]] Expected<void>
  emitResolvedScratchAccess(MachineInstr &Anchor, MachineFunction &Function,
                            const TargetDescription &Target, PhysicalRegister Scratch,
                            FrameIndexReference Reference, bool Restore) const {
    MachineIRBuilder Builder(Function, Target);
    auto Point =
        Restore ? Builder.setInsertPointAfter(Anchor) : Builder.setInsertPointBefore(Anchor);
    if (!Point)
      return std::unexpected(Point.error());
    return emitScratchAccess(Builder, Scratch, Reference, Restore);
  }

  const RISCVSubtarget &Subtarget;
};

} // namespace

std::unique_ptr<TargetFrameLowering> createFrameLowering(const RISCVSubtarget &Subtarget) {
  return std::make_unique<RISCVFrameLowering>(Subtarget);
}

} // namespace rcc::riscv
