// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/LiveRangeEdit.h"

#include "RegAllocInternal.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/TargetDescription.h"

namespace rcc {

Expected<Register> LiveRangeEdit::createFrom(Register Parent, bool Spillable,
                                             std::optional<Register> Origin) {
  auto ParentInfo = Function->registerInfo().virtualRegisterInfo(Parent);
  if (!ParentInfo)
    return std::unexpected(ParentInfo.error());
  const VirtualRegisterInfo &Source = ParentInfo->get();
  Expected<Register> Created = Source.Type
                                   ? Function->registerInfo().createVirtualRegister(*Source.Type)
                                   : Function->registerInfo().createVirtualRegister();
  if (!Created)
    return std::unexpected(Created.error());
  const Register Child = *Created;
  auto Fail = [&](const Error &Failure) -> Expected<Register> {
    auto Erased = Function->registerInfo().eraseVirtualRegister(Child);
    if (!Erased)
      return makeUnexpected(ErrorCode::InternalError,
                            "split virtual-register setup and rollback both failed");
    return std::unexpected(Failure);
  };
  if (Source.Bank) {
    auto Set = Function->registerInfo().setRegisterBank(Child, *Source.Bank);
    if (!Set)
      return Fail(Set.error());
  }
  if (Source.RegClass) {
    auto Set = Function->registerInfo().setRegisterClass(Child, *Source.RegClass);
    if (!Set)
      return Fail(Set.error());
  }
  for (const PhysicalRegister Hint : Source.AllocationHints) {
    auto Added = Function->registerInfo().addAllocationHint(Child, Hint);
    if (!Added)
      return Fail(Added.error());
  }
  if (Source.TargetFlags != 0) {
    auto Set = Function->registerInfo().setTargetRegisterFlags(Child, Source.TargetFlags);
    if (!Set)
      return Fail(Set.error());
  }
  const VirtRegMapEntry *ParentEntry = Mapping->entry(Parent);
  const Register Root = Origin.value_or(
      ParentEntry != nullptr && ParentEntry->Origin ? *ParentEntry->Origin : Parent);
  const std::optional<FrameIndex> ParentStackSlot =
      ParentEntry == nullptr ? std::nullopt : ParentEntry->Spill.StackSlot;
  const LaneMask ParentSpillLanes =
      ParentEntry == nullptr ? LaneMask::all() : ParentEntry->Spill.Lanes;
  auto Added = Mapping->add(Child, Spillable, Parent, Root);
  if (!Added)
    return Fail(Added.error());
  auto Split = Mapping->markSplit(Child, Parent, Root);
  if (!Split)
    return Fail(Split.error());
  if (ParentStackSlot) {
    auto SharedSlot = Mapping->assignStackSlot(Child, *ParentStackSlot, ParentSpillLanes);
    if (!SharedSlot)
      return Fail(SharedSlot.error());
  }
  return Child;
}

Expected<FrameIndex> LiveRangeEdit::getOrCreateSpillSlot(Register VirtualReg) {
  if (const auto Existing = Mapping->stackSlot(VirtualReg))
    return *Existing;
  const VirtRegMapEntry *Entry = Mapping->entry(VirtualReg);
  const Register Original = Entry != nullptr && Entry->Origin ? *Entry->Origin : VirtualReg;
  if (Original != VirtualReg) {
    if (const auto Existing = Mapping->stackSlot(Original)) {
      auto Shared = Mapping->assignStackSlot(VirtualReg, *Existing);
      if (!Shared)
        return std::unexpected(Shared.error());
      return *Existing;
    }
  }
  auto ClassID = regalloc_detail::registerClassFor(*Function, Original);
  if (!ClassID)
    return std::unexpected(ClassID.error());
  const RegisterClassDesc *Class = Target->registers().registerClass(*ClassID);
  if (Class == nullptr || Class->SpillSizeInBits == 0 || Class->SpillAlignmentInBits == 0 ||
      Class->SpillSizeInBits % 8U != 0 || Class->SpillAlignmentInBits % 8U != 0) {
    return makeUnexpected(ErrorCode::InternalError,
                          "allocatable register class has no valid spill size and alignment");
  }
  auto Align = Alignment::create(Class->SpillAlignmentInBits / 8U);
  if (!Align)
    return std::unexpected(Align.error());
  StackObjectDesc Description;
  Description.Kind = StackObjectKind::SpillSlot;
  Description.Size = Class->SpillSizeInBits / 8U;
  Description.Align = *Align;
  auto Slot = Function->frameInfo().createObject(std::move(Description));
  if (!Slot)
    return std::unexpected(Slot.error());
  auto AssignedOriginal = Mapping->assignStackSlot(Original, *Slot);
  if (!AssignedOriginal)
    return std::unexpected(AssignedOriginal.error());
  if (Original != VirtualReg) {
    auto AssignedChild = Mapping->assignStackSlot(VirtualReg, *Slot);
    if (!AssignedChild)
      return std::unexpected(AssignedChild.error());
  }
  return *Slot;
}

Expected<void> LiveRangeEdit::eraseVirtualRegister(Register VirtualReg) {
  if (!Function->registerInfo().useEmpty(VirtualReg))
    return makeUnexpected(ErrorCode::InternalError,
                          "cannot erase a split virtual register with remaining uses");
  for (const MachineOperand &Operand : Function->registerInfo().defOperands(VirtualReg)) {
    if (Operand.parent() != nullptr)
      return makeUnexpected(ErrorCode::InternalError,
                            "cannot erase a split virtual register with remaining definitions");
  }
  return Function->registerInfo().eraseVirtualRegister(VirtualReg);
}

} // namespace rcc
