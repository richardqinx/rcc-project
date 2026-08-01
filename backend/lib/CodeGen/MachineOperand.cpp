// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineOperand.h"

#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>

namespace rcc {

Expected<FloatingPointBits> FloatingPointBits::create(FloatSemantics Semantics,
                                                      std::span<const std::uint64_t> Words) {
  const auto BitWidth = getFloatSemanticsBitWidth(Semantics);
  if (!BitWidth) {
    return makeUnexpected(ErrorCode::InvalidArgument, "floating immediate has unknown semantics");
  }

  const std::size_t RequiredWords = (*BitWidth + 63U) / 64U;
  if (RequiredWords > 2 || Words.size() != RequiredWords) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "floating immediate bit pattern has the wrong word count");
  }

  const std::uint32_t UsedBitsInLastWord = *BitWidth % 64U;
  if (UsedBitsInLastWord != 0 && (Words.back() >> UsedBitsInLastWord) != 0) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "floating immediate has nonzero bits above its semantics width");
  }

  std::array<std::uint64_t, 2> StoredWords{};
  std::ranges::copy(Words, StoredWords.begin());
  return FloatingPointBits(Semantics, StoredWords, static_cast<std::uint8_t>(RequiredWords));
}

RegisterOperandFlags::RegisterOperandFlags(
    std::initializer_list<RegisterOperandFlag> Flags) noexcept {
  for (const RegisterOperandFlag Flag : Flags)
    set(Flag);
}

Expected<void> MachineOperand::verifyRegisterFlags(RegisterOperandFlags Flags) {
  const bool IsDef = Flags.has(RegisterOperandFlag::Define);
  const bool IsUse = Flags.has(RegisterOperandFlag::Use);
  if (!IsDef && !IsUse) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "register operand must be a definition or use");
  }
  if (Flags.has(RegisterOperandFlag::Kill) && !IsUse) {
    return makeUnexpected(ErrorCode::InvalidArgument, "kill flag requires a register use");
  }
  if (Flags.has(RegisterOperandFlag::Dead) && !IsDef) {
    return makeUnexpected(ErrorCode::InvalidArgument, "dead flag requires a register definition");
  }
  if (Flags.has(RegisterOperandFlag::Undef) && !IsUse) {
    return makeUnexpected(ErrorCode::InvalidArgument, "undef flag requires a register use");
  }
  if (Flags.has(RegisterOperandFlag::EarlyClobber) && !IsDef) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "early-clobber flag requires a register definition");
  }
  if (Flags.has(RegisterOperandFlag::InternalRead) && (!IsDef || !IsUse)) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "internal-read flag requires a register def-use operand");
  }
  if (IsDef && IsUse && !Flags.has(RegisterOperandFlag::InternalRead)) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "register def-use operand requires internal-read");
  }
  if (Flags.has(RegisterOperandFlag::Debug) && (!IsUse || IsDef)) {
    return makeUnexpected(ErrorCode::InvalidArgument, "debug register operand must be a use");
  }
  return {};
}

Expected<MachineOperand> MachineOperand::registerOperand(Register Reg, RegisterOperandFlags Flags,
                                                         SubRegIndex SubReg,
                                                         std::uint32_t TargetFlags) {
  const auto Validation = verifyRegisterFlags(Flags);
  if (!Validation)
    return std::unexpected(Validation.error());

  MachineOperand Operand(Kind::Register, Reg, TargetFlags);
  Operand.RegFlags = Flags;
  Operand.RegSubReg = SubReg;
  return Operand;
}

MachineOperand MachineOperand::immediate(std::int64_t Value, std::uint32_t TargetFlags) {
  return MachineOperand(Kind::Immediate, Value, TargetFlags);
}

MachineOperand MachineOperand::floatingImmediate(FloatingPointBits Bits,
                                                 std::uint32_t TargetFlags) {
  return MachineOperand(Kind::FloatingImmediate, FloatingImmediateOperand{Bits}, TargetFlags);
}

MachineOperand MachineOperand::basicBlock(MachineBasicBlock &Block, std::uint32_t TargetFlags) {
  return MachineOperand(Kind::BasicBlock, BasicBlockOperand{&Block}, TargetFlags);
}

MachineOperand MachineOperand::frameIndex(FrameIndex Index, std::int64_t Offset,
                                          std::uint32_t TargetFlags) {
  return MachineOperand(Kind::FrameIndex, FrameIndexOperand{Index, Offset}, TargetFlags);
}

MachineOperand MachineOperand::constantPool(ConstantPoolIndex Index, std::int64_t Offset,
                                            std::uint32_t TargetFlags) {
  return MachineOperand(Kind::ConstantPoolIndex, ConstantPoolOperand{Index, Offset}, TargetFlags);
}

MachineOperand MachineOperand::jumpTable(JumpTableIndex Index, std::uint32_t TargetFlags) {
  return MachineOperand(Kind::JumpTableIndex, JumpTableOperand{Index}, TargetFlags);
}

Expected<MachineOperand> MachineOperand::globalSymbol(std::string Name, std::int64_t Offset,
                                                      std::uint32_t TargetFlags) {
  if (Name.empty()) {
    return makeUnexpected(ErrorCode::InvalidArgument, "global symbol operand name cannot be empty");
  }
  return MachineOperand(Kind::GlobalSymbol, GlobalSymbolOperand{std::move(Name), Offset},
                        TargetFlags);
}

Expected<MachineOperand> MachineOperand::externalSymbol(std::string Name, std::int64_t Offset,
                                                        std::uint32_t TargetFlags) {
  if (Name.empty()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "external symbol operand name cannot be empty");
  }
  return MachineOperand(Kind::ExternalSymbol, ExternalSymbolOperand{std::move(Name), Offset},
                        TargetFlags);
}

Expected<MachineOperand> MachineOperand::blockAddress(std::string FunctionName,
                                                      std::string BlockName, std::int64_t Offset,
                                                      std::uint32_t TargetFlags) {
  if (FunctionName.empty() || BlockName.empty()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "block address operand requires function and block names");
  }
  return MachineOperand(Kind::BlockAddress,
                        BlockAddressOperand{std::move(FunctionName), std::move(BlockName), Offset},
                        TargetFlags);
}

Expected<MachineOperand> MachineOperand::registerMask(std::shared_ptr<const RegisterMask> Mask,
                                                      std::uint32_t TargetFlags) {
  if (!Mask) {
    return makeUnexpected(ErrorCode::InvalidArgument, "register mask operand cannot be null");
  }
  return MachineOperand(Kind::RegisterMask, RegisterMaskOperand{std::move(Mask)}, TargetFlags);
}

Expected<MachineOperand> MachineOperand::metadata(MachineMetadataValue Value,
                                                  std::uint32_t TargetFlags) {
  if (!Value.isValid()) {
    return makeUnexpected(ErrorCode::InvalidArgument, "metadata operand value must be valid");
  }
  return MachineOperand(Kind::Metadata, MetadataOperand{std::move(Value)}, TargetFlags);
}

MachineOperand MachineOperand::targetIndex(TargetIndex Index, std::int64_t Offset,
                                           std::uint32_t TargetFlags) {
  return MachineOperand(Kind::TargetIndex, TargetIndexOperand{Index, Offset}, TargetFlags);
}

Expected<MachineOperand> MachineOperand::mcSymbol(std::string Name, std::int64_t Offset,
                                                  std::uint32_t TargetFlags) {
  if (Name.empty()) {
    return makeUnexpected(ErrorCode::InvalidArgument, "MC symbol operand name cannot be empty");
  }
  return MachineOperand(Kind::MCSymbol, MCSymbolOperand{std::move(Name), Offset}, TargetFlags);
}

MachineOperand::MachineOperand(MachineOperand &&Other) noexcept
    : OperandKind(Other.OperandKind), Storage(std::move(Other.Storage)), RegFlags(Other.RegFlags),
      RegSubReg(Other.RegSubReg), TiedOperandIndex(Other.TiedOperandIndex),
      TargetFlags(Other.TargetFlags), Parent(Other.Parent) {
  takeRegLinkFrom(Other);
  Other.Parent = nullptr;
  Other.TiedOperandIndex.reset();
}

MachineOperand &MachineOperand::operator=(MachineOperand &&Other) noexcept {
  if (this == &Other)
    return *this;

  unlinkFromRegInfo();
  OperandKind = Other.OperandKind;
  Storage = std::move(Other.Storage);
  RegFlags = Other.RegFlags;
  RegSubReg = Other.RegSubReg;
  TiedOperandIndex = Other.TiedOperandIndex;
  TargetFlags = Other.TargetFlags;
  Parent = Other.Parent;
  takeRegLinkFrom(Other);
  Other.Parent = nullptr;
  Other.TiedOperandIndex.reset();
  return *this;
}

MachineOperand::~MachineOperand() {
  unlinkFromRegInfo();
}

std::optional<Register> MachineOperand::registerValue() const noexcept {
  if (!isRegister())
    return std::nullopt;
  return std::get<Register>(Storage);
}

bool MachineOperand::isDef() const noexcept {
  return isRegister() && RegFlags.has(RegisterOperandFlag::Define);
}

bool MachineOperand::isUse() const noexcept {
  return isRegister() && RegFlags.has(RegisterOperandFlag::Use);
}

bool MachineOperand::isDebugUse() const noexcept {
  return isUse() && RegFlags.has(RegisterOperandFlag::Debug);
}

bool MachineOperand::isImplicit() const noexcept {
  return isRegister() && RegFlags.has(RegisterOperandFlag::Implicit);
}

Expected<void> MachineOperand::setRegister(Register Reg) {
  if (!isRegister()) {
    return makeUnexpected(ErrorCode::InvalidArgument, "setRegister requires a register operand");
  }

  MachineRegisterInfo *RegInfo = LinkedRegInfo;
  if (RegInfo == nullptr && Parent != nullptr)
    RegInfo = &Parent->function().registerInfo();
  if (RegInfo != nullptr && Reg.isVirtual() && !RegInfo->isKnownVirtualRegister(Reg)) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "register operand references an unknown virtual register");
  }

  const bool WasLinked = LinkedRegInfo != nullptr;
  unlinkFromRegInfo();
  Storage = Reg;
  if (WasLinked && !Reg.isNoRegister())
    linkToRegInfo(*RegInfo);
  return {};
}

Expected<void> MachineOperand::setRegisterFlags(RegisterOperandFlags Flags) {
  if (!isRegister()) {
    return makeUnexpected(ErrorCode::InvalidArgument, "register flags require a register operand");
  }
  const auto Validation = verifyRegisterFlags(Flags);
  if (!Validation)
    return std::unexpected(Validation.error());
  RegFlags = Flags;
  return {};
}

Expected<void> MachineOperand::setSubReg(SubRegIndex SubReg) {
  if (!isRegister()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "subregister index requires a register operand");
  }
  RegSubReg = SubReg;
  return {};
}

MachineOperand MachineOperand::cloneDetached() const {
  MachineOperand Result(OperandKind, Storage, TargetFlags);
  Result.RegFlags = RegFlags;
  Result.RegSubReg = RegSubReg;
  return Result;
}

void MachineOperand::attachTo(MachineInstr &Instruction, MachineRegisterInfo *RegInfo) {
  Parent = &Instruction;
  if (RegInfo != nullptr && isRegister() && !std::get<Register>(Storage).isNoRegister()) {
    linkToRegInfo(*RegInfo);
  }
}

void MachineOperand::detachFromInstruction() noexcept {
  unlinkFromRegInfo();
  Parent = nullptr;
}

void MachineOperand::linkToRegInfo(MachineRegisterInfo &RegInfo) {
  RegInfo.linkOperand(*this);
}

void MachineOperand::unlinkFromRegInfo() noexcept {
  if (PreviousRegLink == nullptr)
    return;

  *PreviousRegLink = NextRegOperand;
  if (NextRegOperand != nullptr)
    NextRegOperand->PreviousRegLink = PreviousRegLink;

  PreviousRegLink = nullptr;
  NextRegOperand = nullptr;
  LinkedRegInfo = nullptr;
}

void MachineOperand::takeRegLinkFrom(MachineOperand &Other) noexcept {
  LinkedRegInfo = Other.LinkedRegInfo;
  PreviousRegLink = Other.PreviousRegLink;
  NextRegOperand = Other.NextRegOperand;

  if (PreviousRegLink != nullptr)
    *PreviousRegLink = this;
  if (NextRegOperand != nullptr)
    NextRegOperand->PreviousRegLink = &NextRegOperand;

  Other.LinkedRegInfo = nullptr;
  Other.PreviousRegLink = nullptr;
  Other.NextRegOperand = nullptr;
}

} // namespace rcc
