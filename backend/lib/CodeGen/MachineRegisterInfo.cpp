// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineRegisterInfo.h"

#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineOperand.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_set>

namespace rcc {

RegOperandIterator::RegOperandIterator(MachineOperand *Current, bool RequireUse, bool RequireDef,
                                       bool SkipDebug) noexcept
    : Current(Current), RequireUse(RequireUse), RequireDef(RequireDef), SkipDebug(SkipDebug) {
  skipFiltered();
}

RegOperandIterator::reference RegOperandIterator::operator*() const noexcept {
  return *Current;
}

RegOperandIterator::pointer RegOperandIterator::operator->() const noexcept {
  return Current;
}

RegOperandIterator &RegOperandIterator::operator++() noexcept {
  Current = Current->NextRegOperand;
  skipFiltered();
  return *this;
}

RegOperandIterator RegOperandIterator::operator++(int) noexcept {
  RegOperandIterator Previous = *this;
  ++*this;
  return Previous;
}

void RegOperandIterator::skipFiltered() noexcept {
  while (Current != nullptr) {
    if (RequireUse && !Current->isUse()) {
      Current = Current->NextRegOperand;
      continue;
    }
    if (RequireDef && !Current->isDef()) {
      Current = Current->NextRegOperand;
      continue;
    }
    if (SkipDebug && Current->isDebugUse()) {
      Current = Current->NextRegOperand;
      continue;
    }
    break;
  }
}

MachineRegisterInfo::~MachineRegisterInfo() = default;

Expected<Register> MachineRegisterInfo::createVirtualRegister() {
  auto Reg = Register::makeVirtual(VirtualRegisters.size());
  if (!Reg)
    return std::unexpected(Reg.error());
  VirtualRegisters.emplace_back();
  return *Reg;
}

Expected<Register> MachineRegisterInfo::createVirtualRegister(LLT Type) {
  if (!Type.isValid()) {
    return makeUnexpected(ErrorCode::InvalidArgument, "virtual register type must be a valid LLT");
  }

  auto Reg = createVirtualRegister();
  if (!Reg)
    return std::unexpected(Reg.error());
  VirtualRegisters.back().Type = Type;
  return *Reg;
}

Expected<void> MachineRegisterInfo::eraseVirtualRegister(Register Reg) {
  auto Info = virtualRegisterInfo(Reg);
  if (!Info)
    return std::unexpected(Info.error());
  if (headFor(Reg) != nullptr) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "cannot erase a virtual register with operands");
  }

  Info->get().IsTombstone = true;
  Info->get().Type.reset();
  Info->get().Bank.reset();
  Info->get().RegClass.reset();
  Info->get().AllocationHints.clear();
  Info->get().TargetFlags = 0;
  return {};
}

bool MachineRegisterInfo::isKnownVirtualRegister(Register Reg) const noexcept {
  const auto Index = Reg.virtualIndex();
  if (!Index || *Index >= VirtualRegisters.size())
    return false;
  return !VirtualRegisters[*Index].IsTombstone;
}

Expected<std::reference_wrapper<VirtualRegisterInfo>>
MachineRegisterInfo::virtualRegisterInfo(Register Reg) {
  const auto Index = Reg.virtualIndex();
  if (!Index || *Index >= VirtualRegisters.size() || VirtualRegisters[*Index].IsTombstone) {
    return makeUnexpected(ErrorCode::InvalidArgument, "register is not a live virtual register");
  }
  return std::ref(VirtualRegisters[*Index]);
}

Expected<std::reference_wrapper<const VirtualRegisterInfo>>
MachineRegisterInfo::virtualRegisterInfo(Register Reg) const {
  const auto Index = Reg.virtualIndex();
  if (!Index || *Index >= VirtualRegisters.size() || VirtualRegisters[*Index].IsTombstone) {
    return makeUnexpected(ErrorCode::InvalidArgument, "register is not a live virtual register");
  }
  return std::cref(VirtualRegisters[*Index]);
}

Expected<void> MachineRegisterInfo::setType(Register Reg, LLT Type) {
  if (!Type.isValid()) {
    return makeUnexpected(ErrorCode::InvalidArgument, "virtual register type must be a valid LLT");
  }
  auto Info = virtualRegisterInfo(Reg);
  if (!Info)
    return std::unexpected(Info.error());
  Info->get().Type = Type;
  return {};
}

Expected<void> MachineRegisterInfo::clearType(Register Reg) {
  auto Info = virtualRegisterInfo(Reg);
  if (!Info)
    return std::unexpected(Info.error());
  Info->get().Type.reset();
  return {};
}

Expected<void> MachineRegisterInfo::setRegisterBank(Register Reg, RegisterBankID Bank) {
  auto Info = virtualRegisterInfo(Reg);
  if (!Info)
    return std::unexpected(Info.error());
  Info->get().Bank = Bank;
  return {};
}

Expected<void> MachineRegisterInfo::clearRegisterBank(Register Reg) {
  auto Info = virtualRegisterInfo(Reg);
  if (!Info)
    return std::unexpected(Info.error());
  Info->get().Bank.reset();
  return {};
}

Expected<void> MachineRegisterInfo::setRegisterClass(Register Reg, RegisterClassID RegClass) {
  auto Info = virtualRegisterInfo(Reg);
  if (!Info)
    return std::unexpected(Info.error());
  Info->get().RegClass = RegClass;
  return {};
}

Expected<void> MachineRegisterInfo::clearRegisterClass(Register Reg) {
  auto Info = virtualRegisterInfo(Reg);
  if (!Info)
    return std::unexpected(Info.error());
  Info->get().RegClass.reset();
  return {};
}

Expected<void> MachineRegisterInfo::addAllocationHint(Register Reg, PhysicalRegister Hint) {
  if (!Hint.isValid()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "allocation hint must name a physical register");
  }
  auto Info = virtualRegisterInfo(Reg);
  if (!Info)
    return std::unexpected(Info.error());

  auto &Hints = Info->get().AllocationHints;
  if (std::ranges::find(Hints, Hint) == Hints.end())
    Hints.push_back(Hint);
  return {};
}

Expected<void> MachineRegisterInfo::setTargetRegisterFlags(Register Reg, std::uint8_t Flags) {
  auto Info = virtualRegisterInfo(Reg);
  if (!Info)
    return std::unexpected(Info.error());
  Info->get().TargetFlags = Flags;
  return {};
}

Expected<void> MachineRegisterInfo::clearTargetRegisterFlags(Register Reg) {
  return setTargetRegisterFlags(Reg, 0);
}

void MachineRegisterInfo::clearAllVirtualRegisterTypes() noexcept {
  for (VirtualRegisterInfo &Info : VirtualRegisters) {
    if (!Info.IsTombstone)
      Info.Type.reset();
  }
}

Expected<void> MachineRegisterInfo::addLiveIn(PhysicalRegister PhysReg,
                                              std::optional<Register> InitialVirtualRegister) {
  if (!PhysReg.isValid()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "function live-in must name a physical register");
  }
  if (InitialVirtualRegister && !isKnownVirtualRegister(*InitialVirtualRegister)) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "function live-in initial value must be a live virtual register");
  }
  if (std::ranges::any_of(LiveIns, [PhysReg](const PhysicalRegisterLiveIn &LiveIn) {
        return LiveIn.PhysReg == PhysReg;
      })) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "function physical live-in is already present");
  }

  LiveIns.push_back(PhysicalRegisterLiveIn{PhysReg, InitialVirtualRegister});
  return {};
}

Expected<void> MachineRegisterInfo::reservePhysicalRegister(PhysicalRegister PhysReg) {
  if (!PhysReg.isValid()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "reserved register must name a physical register");
  }
  if (isReserved(PhysReg)) {
    return makeUnexpected(ErrorCode::InvalidArgument, "physical register is already reserved");
  }
  ReservedRegisters.push_back(PhysReg);
  return {};
}

bool MachineRegisterInfo::isReserved(PhysicalRegister PhysReg) const noexcept {
  return std::ranges::find(ReservedRegisters, PhysReg) != ReservedRegisters.end();
}

RegOperandRange MachineRegisterInfo::regOperands(Register Reg) noexcept {
  return RegOperandRange(headFor(Reg), false, false, false);
}

RegOperandRange MachineRegisterInfo::regNoDebugOperands(Register Reg) noexcept {
  return RegOperandRange(headFor(Reg), false, false, true);
}

RegOperandRange MachineRegisterInfo::useOperands(Register Reg, bool IncludeDebug) noexcept {
  return RegOperandRange(headFor(Reg), true, false, !IncludeDebug);
}

RegOperandRange MachineRegisterInfo::defOperands(Register Reg) noexcept {
  return RegOperandRange(headFor(Reg), false, true, false);
}

bool MachineRegisterInfo::useEmpty(Register Reg, bool IncludeDebug) const noexcept {
  for (MachineOperand *Operand = headFor(Reg); Operand != nullptr;
       Operand = Operand->NextRegOperand) {
    if (Operand->isUse() && (IncludeDebug || !Operand->isDebugUse()))
      return false;
  }
  return true;
}

bool MachineRegisterInfo::hasOneUse(Register Reg, bool IncludeDebug) const noexcept {
  bool FoundUse = false;
  for (MachineOperand *Operand = headFor(Reg); Operand != nullptr;
       Operand = Operand->NextRegOperand) {
    if (!Operand->isUse() || (!IncludeDebug && Operand->isDebugUse()))
      continue;
    if (FoundUse)
      return false;
    FoundUse = true;
  }
  return FoundUse;
}

const MachineOperand *MachineRegisterInfo::getUniqueVRegDef(Register Reg) const noexcept {
  if (!isKnownVirtualRegister(Reg))
    return nullptr;

  const MachineOperand *UniqueDef = nullptr;
  for (MachineOperand *Operand = headFor(Reg); Operand != nullptr;
       Operand = Operand->NextRegOperand) {
    if (!Operand->isDef())
      continue;
    if (UniqueDef != nullptr)
      return nullptr;
    UniqueDef = Operand;
  }
  return UniqueDef;
}

Expected<std::size_t> MachineRegisterInfo::replaceRegWith(Register OldReg, Register NewReg) {
  if (OldReg.isNoRegister() || NewReg.isNoRegister()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "register replacement requires two nonzero registers");
  }
  if ((OldReg.isVirtual() && !isKnownVirtualRegister(OldReg)) ||
      (NewReg.isVirtual() && !isKnownVirtualRegister(NewReg))) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "register replacement references an unknown virtual register");
  }

  std::size_t ReplacementCount = 0;
  MachineOperand *Operand = headFor(OldReg);
  while (Operand != nullptr) {
    MachineOperand *Next = Operand->NextRegOperand;
    const auto Result = Operand->setRegister(NewReg);
    if (!Result)
      return std::unexpected(Result.error());
    ++ReplacementCount;
    Operand = Next;
  }
  return ReplacementCount;
}

Expected<void> MachineRegisterInfo::verifyDefUseChains() const {
  std::unordered_set<const MachineOperand *> SeenOperands;

  for (const auto &[Reg, HeadStorage] : DefUseHeads) {
    if (!HeadStorage) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "register def-use table contains null head storage");
    }
    if (HeadStorage->Head == nullptr)
      continue;
    if (Reg.isNoRegister() || (Reg.isVirtual() && !isKnownVirtualRegister(Reg))) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "register def-use chain is keyed by an invalid register");
    }
    MachineOperand *const *ExpectedPreviousLink = &HeadStorage->Head;
    for (MachineOperand *Operand = HeadStorage->Head; Operand != nullptr;
         Operand = Operand->NextRegOperand) {
      if (!SeenOperands.insert(Operand).second) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "register def-use chains contain a cycle or duplicate operand");
      }
      if (!Operand->isRegister() || Operand->registerValue() != Reg) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "register def-use chain contains an operand for another register");
      }
      if (Operand->LinkedRegInfo != this || Operand->PreviousRegLink != ExpectedPreviousLink) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "register def-use chain back-link is corrupt");
      }
      if (Operand->Parent == nullptr || !Operand->Parent->isLinked() ||
          &Operand->Parent->function().registerInfo() != this) {
        return makeUnexpected(
            ErrorCode::VerificationError,
            "register def-use chain contains an operand outside its function layout");
      }
      ExpectedPreviousLink = &Operand->NextRegOperand;
    }
  }

  return {};
}

bool MachineRegisterInfo::isLinkedOperand(const MachineOperand &Operand) const noexcept {
  return Operand.LinkedRegInfo == this && Operand.PreviousRegLink != nullptr;
}

MachineOperand *MachineRegisterInfo::headFor(Register Reg) const noexcept {
  const auto Position = DefUseHeads.find(Reg);
  if (Position == DefUseHeads.end())
    return nullptr;
  return Position->second->Head;
}

void MachineRegisterInfo::linkOperand(MachineOperand &Operand) {
  const auto Reg = Operand.registerValue();
  if (!Reg || Reg->isNoRegister())
    return;

  auto &HeadStorage = DefUseHeads[*Reg];
  if (!HeadStorage)
    HeadStorage = std::make_unique<RegOperandListHead>();

  Operand.unlinkFromRegInfo();
  Operand.LinkedRegInfo = this;
  Operand.PreviousRegLink = &HeadStorage->Head;
  Operand.NextRegOperand = HeadStorage->Head;
  if (HeadStorage->Head != nullptr)
    HeadStorage->Head->PreviousRegLink = &Operand.NextRegOperand;
  HeadStorage->Head = &Operand;
}

} // namespace rcc
