// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/GlobalISel/Utils.h"

#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/TargetDescription.h"

#include <cstdint>
#include <numeric>
#include <utility>

namespace rcc {
namespace {

[[nodiscard]] bool hasProtectedAttachment(const MachineInstr &Instruction) noexcept {
  return Instruction.flags().has(MachineInstrFlag::FrameSetup) ||
         Instruction.flags().has(MachineInstrFlag::FrameDestroy) ||
         Instruction.debugInstrNumber() != 0 || Instruction.preInstrSymbol().has_value() ||
         Instruction.postInstrSymbol().has_value() || Instruction.heapAllocMarker().has_value() ||
         Instruction.pcSections().has_value() || Instruction.mmra().has_value() ||
         Instruction.cfiType().has_value() || Instruction.deactivationSymbol().has_value();
}

[[nodiscard]] bool wouldBeTriviallyDead(const MachineInstr &Instruction,
                                        const InstructionDescriptor &Descriptor) noexcept {
  switch (static_cast<GenericOpcode>(Instruction.opcode().value())) {
  case GenericOpcode::LOCAL_ESCAPE:
  case GenericOpcode::FAKE_USE:
  case GenericOpcode::LIFETIME_START:
  case GenericOpcode::LIFETIME_END:
    return false;
  default:
    break;
  }

  const InstructionFlags Flags = Descriptor.Flags;
  if (Flags.has(InstructionFlag::Phi))
    return true;
  if (Flags.has(InstructionFlag::Terminator) || Flags.has(InstructionFlag::Branch) ||
      Flags.has(InstructionFlag::Call) || Flags.has(InstructionFlag::Return) ||
      Flags.has(InstructionFlag::Barrier) || Flags.has(InstructionFlag::MayLoad) ||
      Flags.has(InstructionFlag::MayStore) || Flags.has(InstructionFlag::HasSideEffects) ||
      Flags.has(InstructionFlag::UnmodeledSideEffects) ||
      Flags.has(InstructionFlag::MayRaiseFPException) || hasProtectedAttachment(Instruction))
    return false;
  return true;
}

[[nodiscard]] Expected<void> saveUsesAndErase(MachineInstr &Instruction, MachineRegisterInfo &MRI,
                                              std::vector<MachineInstr *> &DeadChain) {
  for (const MachineOperand &Operand : Instruction.operands()) {
    if (!Operand.isRegister() || !Operand.isUse())
      continue;
    const std::optional<Register> Reg = Operand.registerValue();
    if (!Reg || !Reg->isVirtual())
      continue;
    MachineInstr *Definition = MRI.getVRegDef(*Reg);
    if (Definition != nullptr && Definition != &Instruction)
      DeadChain.push_back(Definition);
  }
  MachineBasicBlock *Block = Instruction.parent();
  if (Block == nullptr)
    return {};
  return Block->removeInstruction(Instruction);
}

[[nodiscard]] Expected<LowLevelType> makeVectorType(std::uint64_t ElementCount, bool IsScalable,
                                                    LowLevelType ElementType) {
  if (!IsScalable && ElementCount == 1)
    return ElementType;
  return IsScalable ? LowLevelType::makeScalableVector(ElementCount, ElementType)
                    : LowLevelType::makeFixedVector(ElementCount, ElementType);
}

[[nodiscard]] Expected<void> extractVectorParts(Register Reg, std::size_t ElementCount,
                                                std::vector<Register> &Parts,
                                                MachineIRBuilder &MIRBuilder,
                                                MachineRegisterInfo &MRI) {
  auto RegisterType = MRI.getType(Reg);
  if (!RegisterType)
    return std::unexpected(RegisterType.error());
  if (!RegisterType->isVector() || RegisterType->isScalableVector() || ElementCount == 0) {
    return makeUnexpected(
        ErrorCode::InvalidArgument,
        "vector part extraction requires a fixed vector and a nonzero element count");
  }

  const LowLevelType ElementType = *RegisterType->scalarElementType();
  auto NarrowType = makeVectorType(ElementCount, false, ElementType);
  if (!NarrowType)
    return std::unexpected(NarrowType.error());
  const std::size_t RegisterElementCount = RegisterType->minimumElementCount();
  const std::size_t LeftoverElementCount = RegisterElementCount % ElementCount;
  const std::size_t NarrowPartCount = RegisterElementCount / ElementCount;

  if (LeftoverElementCount == 0)
    return extractParts(Reg, *NarrowType, static_cast<int>(NarrowPartCount), Parts, MIRBuilder,
                        MRI);

  std::vector<Register> Elements;
  auto Extracted = extractParts(Reg, ElementType, static_cast<int>(RegisterElementCount), Elements,
                                MIRBuilder, MRI);
  if (!Extracted)
    return std::unexpected(Extracted.error());

  std::size_t Offset = 0;
  for (std::size_t Index = 0; Index != NarrowPartCount; ++Index, Offset += ElementCount) {
    std::vector<Register> Piece(Elements.begin() + static_cast<std::ptrdiff_t>(Offset),
                                Elements.begin() +
                                    static_cast<std::ptrdiff_t>(Offset + ElementCount));
    auto Part = MRI.createVirtualRegister(*NarrowType);
    if (!Part)
      return std::unexpected(Part.error());
    auto Built = MIRBuilder.buildMergeLikeInstr(*Part, Piece);
    if (!Built)
      return std::unexpected(Built.error());
    Parts.push_back(*Part);
  }

  if (LeftoverElementCount == 1) {
    Parts.push_back(Elements[Offset]);
    return {};
  }

  auto LeftoverType = LowLevelType::makeFixedVector(LeftoverElementCount, ElementType);
  if (!LeftoverType)
    return std::unexpected(LeftoverType.error());
  std::vector<Register> LeftoverElements(Elements.begin() + static_cast<std::ptrdiff_t>(Offset),
                                         Elements.end());
  auto Leftover = MRI.createVirtualRegister(*LeftoverType);
  if (!Leftover)
    return std::unexpected(Leftover.error());
  auto Built = MIRBuilder.buildMergeLikeInstr(*Leftover, LeftoverElements);
  if (!Built)
    return std::unexpected(Built.error());
  Parts.push_back(*Leftover);
  return {};
}

} // namespace

bool isTriviallyDead(const MachineInstr &Instruction, const MachineRegisterInfo &MRI,
                     const TargetDescription &Target) noexcept {
  for (const MachineOperand &Operand : Instruction.operands()) {
    if (!Operand.isRegister() || !Operand.isDef())
      continue;
    const std::optional<Register> Reg = Operand.registerValue();
    if (!Reg || Reg->isPhysical() || !MRI.useEmpty(*Reg, false))
      return false;
  }

  const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction.opcode());
  return Descriptor != nullptr && wouldBeTriviallyDead(Instruction, *Descriptor);
}

Expected<void> eraseInstrs(std::span<MachineInstr *const> Instructions, MachineRegisterInfo &MRI,
                           const TargetDescription &Target) {
  std::vector<MachineInstr *> DeadChain;
  for (MachineInstr *Instruction : Instructions) {
    if (Instruction == nullptr || Instruction->parent() == nullptr)
      continue;
    auto Removed = saveUsesAndErase(*Instruction, MRI, DeadChain);
    if (!Removed)
      return std::unexpected(Removed.error());
  }

  while (!DeadChain.empty()) {
    MachineInstr *Instruction = DeadChain.back();
    DeadChain.pop_back();
    if (Instruction == nullptr || Instruction->parent() == nullptr ||
        !isTriviallyDead(*Instruction, MRI, Target))
      continue;
    auto Removed = saveUsesAndErase(*Instruction, MRI, DeadChain);
    if (!Removed)
      return std::unexpected(Removed.error());
  }
  return {};
}

Expected<void> eraseInstr(MachineInstr &Instruction, MachineRegisterInfo &MRI,
                          const TargetDescription &Target) {
  MachineInstr *Instructions[] = {&Instruction};
  return eraseInstrs(Instructions, MRI, Target);
}

Expected<void> extractParts(Register Reg, LowLevelType Type, int PartCount,
                            std::vector<Register> &Parts, MachineIRBuilder &MIRBuilder,
                            MachineRegisterInfo &MRI) {
  std::vector<Register> NewParts;
  if (PartCount < 0)
    return makeUnexpected(ErrorCode::InvalidArgument, "part count cannot be negative");
  NewParts.reserve(static_cast<std::size_t>(PartCount));
  for (int Index = 0; Index != PartCount; ++Index) {
    auto Part = MRI.createVirtualRegister(Type);
    if (!Part)
      return std::unexpected(Part.error());
    NewParts.push_back(*Part);
  }
  auto Built = MIRBuilder.buildUnmergeValues(NewParts, Reg);
  if (!Built)
    return std::unexpected(Built.error());
  Parts.insert(Parts.end(), NewParts.begin(), NewParts.end());
  return {};
}

Expected<bool> extractParts(Register Reg, LowLevelType RegisterType, LowLevelType MainType,
                            LowLevelType &LeftoverType, std::vector<Register> &Parts,
                            std::vector<Register> &LeftoverParts, MachineIRBuilder &MIRBuilder,
                            MachineRegisterInfo &MRI) {
  if (LeftoverType.isValid()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "part extraction leftover type must be an output");
  }

  const std::uint64_t RegisterSize = RegisterType.minimumBitWidth();
  const std::uint64_t MainSize = MainType.minimumBitWidth();
  if (MainSize == 0 || MainSize > RegisterSize) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "part extraction requires a smaller nonzero main type");
  }
  const std::size_t PartCount = RegisterSize / MainSize;
  const std::uint64_t LeftoverSize = RegisterSize - PartCount * MainSize;

  if (LeftoverSize == 0) {
    auto Extracted =
        extractParts(Reg, MainType, static_cast<int>(PartCount), Parts, MIRBuilder, MRI);
    if (!Extracted)
      return std::unexpected(Extracted.error());
    return true;
  }

  if (RegisterType.isVector() && MainType.isVector() && !RegisterType.isScalableVector() &&
      !MainType.isScalableVector()) {
    const std::size_t RegisterElementCount = RegisterType.minimumElementCount();
    const std::size_t MainElementCount = MainType.minimumElementCount();
    const std::size_t LeftoverElementCount = RegisterElementCount % MainElementCount;
    if (LeftoverElementCount > 1 && MainElementCount % LeftoverElementCount == 0 &&
        RegisterElementCount % LeftoverElementCount == 0 &&
        RegisterType.scalarBitWidth() == MainType.scalarBitWidth()) {
      auto Candidate =
          LowLevelType::makeFixedVector(LeftoverElementCount, *RegisterType.scalarElementType());
      if (!Candidate)
        return std::unexpected(Candidate.error());
      LeftoverType = *Candidate;

      std::vector<Register> Unmerged;
      auto Extracted = extractParts(Reg, LeftoverType,
                                    static_cast<int>(RegisterElementCount / LeftoverElementCount),
                                    Unmerged, MIRBuilder, MRI);
      if (!Extracted)
        return std::unexpected(Extracted.error());

      const std::size_t LeftoversPerMain = MainElementCount / LeftoverElementCount;
      const std::size_t LeftoverValueCount =
          (RegisterElementCount % MainElementCount) / LeftoverElementCount;
      std::vector<Register> MergeValues;
      for (std::size_t Index = 0; Index != Unmerged.size() - LeftoverValueCount; ++Index) {
        MergeValues.push_back(Unmerged[Index]);
        if (MergeValues.size() != LeftoversPerMain)
          continue;
        auto Part = MRI.createVirtualRegister(MainType);
        if (!Part)
          return std::unexpected(Part.error());
        auto Built = MIRBuilder.buildMergeLikeInstr(*Part, MergeValues);
        if (!Built)
          return std::unexpected(Built.error());
        Parts.push_back(*Part);
        MergeValues.clear();
      }
      LeftoverParts.insert(LeftoverParts.end(),
                           Unmerged.end() - static_cast<std::ptrdiff_t>(LeftoverValueCount),
                           Unmerged.end());
      return true;
    }
  }

  if (MainType.isVector()) {
    std::vector<Register> RegisterPieces;
    auto Extracted =
        extractVectorParts(Reg, MainType.minimumElementCount(), RegisterPieces, MIRBuilder, MRI);
    if (!Extracted)
      return std::unexpected(Extracted.error());
    Parts.insert(Parts.end(), RegisterPieces.begin(), std::prev(RegisterPieces.end()));
    LeftoverParts.push_back(RegisterPieces.back());
    auto Type = MRI.getType(LeftoverParts.front());
    if (!Type)
      return std::unexpected(Type.error());
    LeftoverType = *Type;
    return true;
  }

  auto ScalarLeftover = LowLevelType::makeCategoryNeutralScalar(LeftoverSize);
  if (!ScalarLeftover)
    return std::unexpected(ScalarLeftover.error());
  LeftoverType = *ScalarLeftover;
  for (std::size_t Index = 0; Index != PartCount; ++Index) {
    auto Part = MRI.createVirtualRegister(MainType);
    if (!Part)
      return std::unexpected(Part.error());
    auto Built = MIRBuilder.buildExtract(*Part, Reg, MainSize * Index);
    if (!Built)
      return std::unexpected(Built.error());
    Parts.push_back(*Part);
  }
  for (std::uint64_t Offset = MainSize * PartCount; Offset < RegisterSize; Offset += LeftoverSize) {
    auto Part = MRI.createVirtualRegister(LeftoverType);
    if (!Part)
      return std::unexpected(Part.error());
    auto Built = MIRBuilder.buildExtract(*Part, Reg, Offset);
    if (!Built)
      return std::unexpected(Built.error());
    LeftoverParts.push_back(*Part);
  }
  return true;
}

Expected<LowLevelType> getLCMType(LowLevelType OriginalType, LowLevelType TargetType) {
  if (OriginalType.minimumBitWidth() == TargetType.minimumBitWidth())
    return OriginalType;
  if ((OriginalType.isScalableVector() && TargetType.isVector() &&
       !TargetType.isScalableVector()) ||
      (TargetType.isScalableVector() && OriginalType.isVector() &&
       !OriginalType.isScalableVector())) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "LCM type is undefined between fixed and scalable vectors");
  }

  if (OriginalType.isVector() && TargetType.isVector()) {
    const LowLevelType OriginalElement = *OriginalType.scalarElementType();
    const LowLevelType TargetElement = *TargetType.scalarElementType();
    if (OriginalElement.minimumBitWidth() == TargetElement.minimumBitWidth()) {
      const std::uint64_t ElementCount =
          std::lcm(static_cast<std::uint64_t>(OriginalType.minimumElementCount()),
                   static_cast<std::uint64_t>(TargetType.minimumElementCount()));
      return makeVectorType(ElementCount, OriginalType.isScalableVector(), OriginalElement);
    }
    const std::uint64_t Width =
        std::lcm(OriginalType.minimumBitWidth(), TargetType.minimumBitWidth());
    return makeVectorType(Width / OriginalElement.minimumBitWidth(),
                          OriginalType.isScalableVector(), OriginalElement);
  }

  if (OriginalType.isVector() || TargetType.isVector()) {
    const LowLevelType VectorType = OriginalType.isVector() ? OriginalType : TargetType;
    const LowLevelType ScalarType = OriginalType.isVector() ? TargetType : OriginalType;
    const LowLevelType VectorElement = *VectorType.scalarElementType();
    const LowLevelType OriginalElement =
        OriginalType.isVector() ? *OriginalType.scalarElementType() : OriginalType;
    if (VectorElement.minimumBitWidth() == ScalarType.minimumBitWidth())
      return makeVectorType(VectorType.minimumElementCount(), VectorType.isScalableVector(),
                            OriginalElement);
    const std::uint64_t Width =
        std::lcm(VectorType.minimumBitWidth(), ScalarType.minimumBitWidth());
    return makeVectorType(Width / OriginalElement.minimumBitWidth(), VectorType.isScalableVector(),
                          OriginalElement);
  }

  const std::uint64_t Width =
      std::lcm(OriginalType.minimumBitWidth(), TargetType.minimumBitWidth());
  if (Width == OriginalType.minimumBitWidth())
    return OriginalType;
  if (Width == TargetType.minimumBitWidth())
    return TargetType;
  return LowLevelType::makeCategoryNeutralScalar(Width);
}

Expected<LowLevelType> getGCDType(LowLevelType OriginalType, LowLevelType TargetType) {
  if (OriginalType.minimumBitWidth() == TargetType.minimumBitWidth())
    return OriginalType;
  if ((OriginalType.isScalableVector() && TargetType.isVector() &&
       !TargetType.isScalableVector()) ||
      (TargetType.isScalableVector() && OriginalType.isVector() &&
       !OriginalType.isScalableVector())) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "GCD type is undefined between fixed and scalable vectors");
  }

  if (OriginalType.isVector() && TargetType.isVector()) {
    const LowLevelType OriginalElement = *OriginalType.scalarElementType();
    const std::uint64_t Width =
        std::gcd(OriginalType.minimumBitWidth(), TargetType.minimumBitWidth());
    if (Width == OriginalElement.minimumBitWidth())
      return makeVectorType(1, OriginalType.isScalableVector(), OriginalElement);
    if (Width < OriginalElement.minimumBitWidth())
      return LowLevelType::makeCategoryNeutralScalar(Width);
    return makeVectorType(Width / OriginalElement.minimumBitWidth(),
                          OriginalType.isScalableVector(), OriginalElement);
  }

  if (OriginalType.isVector() && OriginalType.scalarBitWidth() == TargetType.minimumBitWidth())
    return *OriginalType.scalarElementType();
  if (TargetType.isVector() && TargetType.scalarBitWidth() == OriginalType.minimumBitWidth())
    return OriginalType;

  const std::uint64_t OriginalScalarWidth = OriginalType.scalarBitWidth();
  const std::uint64_t TargetScalarWidth = TargetType.scalarBitWidth();
  return LowLevelType::makeCategoryNeutralScalar(std::gcd(OriginalScalarWidth, TargetScalarWidth));
}

} // namespace rcc
