// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineOperand.h"

#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace rcc {

RegisterOperandFlags::RegisterOperandFlags(
    std::initializer_list<RegisterOperandFlag> Flags) noexcept {
  for (const RegisterOperandFlag Flag : Flags)
    set(Flag);
}

Expected<void> PredicateOperand::verify() const {
  if (const auto *Integer = integerPredicate();
      Integer != nullptr &&
      static_cast<std::uint8_t>(*Integer) < static_cast<std::uint8_t>(IntegerPredicate::Count)) {
    return {};
  }
  if (const auto *Floating = floatingPredicate();
      Floating != nullptr &&
      static_cast<std::uint8_t>(*Floating) < static_cast<std::uint8_t>(FloatingPredicate::Count)) {
    return {};
  }
  return makeUnexpected(ErrorCode::VerificationError, "predicate operand contains an invalid kind");
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

MachineOperand MachineOperand::integerImmediate(APInt Value, std::uint32_t TargetFlags) {
  return MachineOperand(Kind::IntegerImmediate, IntegerImmediateOperand{std::move(Value)},
                        TargetFlags);
}

MachineOperand MachineOperand::floatingImmediate(APFloat Bits, std::uint32_t TargetFlags) {
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

Expected<MachineOperand>
MachineOperand::liveOutRegisterMask(std::shared_ptr<const RegisterMask> Mask,
                                    std::uint32_t TargetFlags) {
  if (!Mask) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "live-out register-mask operand cannot be null");
  }
  return MachineOperand(Kind::LiveOutRegisterMask, LiveOutRegisterMaskOperand{std::move(Mask)},
                        TargetFlags);
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

MachineOperand MachineOperand::cfiIndex(CFIInstructionIndex Index, std::uint32_t TargetFlags) {
  return MachineOperand(Kind::CFIIndex, CFIIndexOperand{Index}, TargetFlags);
}

Expected<MachineOperand> MachineOperand::intrinsic(IntrinsicID ID, std::uint32_t TargetFlags) {
  if (ID.value() == 0) {
    return makeUnexpected(ErrorCode::InvalidArgument, "intrinsic operand ID must be nonzero");
  }
  return MachineOperand(Kind::Intrinsic, IntrinsicOperand{ID}, TargetFlags);
}

Expected<MachineOperand> MachineOperand::integerPredicate(IntegerPredicate Predicate,
                                                          std::uint32_t TargetFlags) {
  PredicateOperand Value(Predicate);
  auto Verification = Value.verify();
  if (!Verification)
    return makeUnexpected(ErrorCode::InvalidArgument, "integer predicate operand is invalid");
  return MachineOperand(Kind::Predicate, Value, TargetFlags);
}

Expected<MachineOperand> MachineOperand::floatingPredicate(FloatingPredicate Predicate,
                                                           std::uint32_t TargetFlags) {
  PredicateOperand Value(Predicate);
  auto Verification = Value.verify();
  if (!Verification)
    return makeUnexpected(ErrorCode::InvalidArgument, "floating predicate operand is invalid");
  return MachineOperand(Kind::Predicate, Value, TargetFlags);
}

Expected<MachineOperand> MachineOperand::shuffleMask(std::vector<std::int32_t> Elements,
                                                     std::uint32_t TargetFlags) {
  if (Elements.size() < 2) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "shuffle-mask operand requires at least two elements");
  }
  return MachineOperand(Kind::ShuffleMask, ShuffleMaskOperand{std::move(Elements)}, TargetFlags);
}

Expected<MachineOperand> MachineOperand::debugInstrRef(std::uint64_t InstructionNumber,
                                                       std::uint64_t OperandNumber,
                                                       std::uint32_t TargetFlags) {
  if (InstructionNumber > std::numeric_limits<std::uint32_t>::max()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "debug instruction reference instruction number must fit in uint32");
  }
  if (OperandNumber > std::numeric_limits<std::uint32_t>::max()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "debug instruction reference operand number must fit in uint32");
  }
  return MachineOperand(Kind::DebugInstrRef,
                        DebugInstrRefOperand{static_cast<std::uint32_t>(InstructionNumber),
                                             static_cast<std::uint32_t>(OperandNumber)},
                        TargetFlags);
}

MachineOperand MachineOperand::laneMask(LaneMask Mask, std::uint32_t TargetFlags) {
  return MachineOperand(Kind::LaneMask, LaneMaskOperand{Mask}, TargetFlags);
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

Expected<void> MachineOperand::changeToImmediate(std::int64_t Value) {
  if (!isRegister() || isImplicit() || isDef() || tiedOperandIndex()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "only an untied explicit register use can become an immediate");
  }
  unlinkFromRegInfo();
  OperandKind = Kind::Immediate;
  Storage = Value;
  RegFlags = {};
  RegSubReg = {};
  return {};
}

Expected<void> MachineOperand::setImmediate(std::int64_t Value) {
  if (OperandKind != Kind::Immediate)
    return makeUnexpected(ErrorCode::InvalidArgument, "setImmediate requires an immediate operand");
  Storage = Value;
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

Expected<void> MachineOperand::setBasicBlock(MachineBasicBlock &Block) {
  if (OperandKind != Kind::BasicBlock)
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "setBasicBlock requires a basic-block operand");
  if (Parent != nullptr && &Block.function() != &Parent->function())
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "basic-block operand references another machine function");
  std::get<BasicBlockOperand>(Storage).Block = &Block;
  return {};
}

Expected<void> MachineOperand::setFrameIndex(FrameIndex Index) {
  if (OperandKind != Kind::FrameIndex)
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "setFrameIndex requires a frame-index operand");
  if (Parent != nullptr && Parent->function().frameInfo().object(Index) == nullptr) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "frame-index operand references an unknown stack object");
  }
  std::get<FrameIndexOperand>(Storage).Index = Index;
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
