// SPDX-License-Identifier: GPL-3.0-or-later

#include "CodeGen/RegisterAllocationTestSupport.h"
#include "rcc/CodeGen/LiveRangeEdit.h"

#include <iostream>

namespace {

using namespace rcc;
using namespace rcc::test;
using namespace rcc::test::ra;

void testSplitMetadata(TestContext &Context, const TargetDescription &Target) {
  auto Fixture = createFunction("virt-map", 10);
  if (!Fixture)
    return;
  auto Parent = createVReg(*Fixture->Function);
  if (!Parent)
    return;
  VirtRegMap Mapping(*Fixture->Function);
  LiveRangeEdit Edit(*Fixture->Function, Target, Mapping);
  Context.expect(Mapping.add(*Parent).has_value(), "VirtRegMap must accept a known vreg");
  auto Slot = Edit.getOrCreateSpillSlot(*Parent);
  auto Child = Edit.createFrom(*Parent, false);
  Context.expect(
      Slot && Child && Mapping.verify().has_value() && Mapping.stackSlot(*Child) == Slot &&
          Mapping.entry(*Child)->SplitParent == *Parent && Mapping.entry(*Child)->Origin == *Parent,
      "LiveRangeEdit must preserve split origin and spill metadata");
  if (!Child)
    return;
  const PhysicalRegister G0 = physical(TestRegister::G0);
  Context.expect(Mapping.assign(*Child, G0).has_value() && Mapping.physicalRegister(*Child) == G0 &&
                     Mapping.entry(*Child)->State == VirtRegAllocationState::Assigned,
                 "VirtRegMap must transition a split child to an assigned state");
}

void testRewriteAndAtomicValidation(TestContext &Context, const TargetDescription &Target) {
  auto Fixture = createFunction("virt-rewrite", 11);
  if (!Fixture)
    return;
  auto Value = createVReg(*Fixture->Function);
  if (!Value ||
      !buildDef(*Fixture->Function, Target, *Fixture->Block, *Value, 1, TestSubReg::Low32) ||
      !buildUse(*Fixture->Function, Target, *Fixture->Block, *Value, TestSubReg::Low32) ||
      !prepareForAllocation(*Fixture->Function, Target))
    return;
  VirtRegMap Mapping(*Fixture->Function);
  const PhysicalRegister G0 = physical(TestRegister::G0);
  Context.expect(Mapping.add(*Value).has_value() && Mapping.assign(*Value, G0).has_value(),
                 "VirtRegMap must accept a physical assignment before rewrite");
  auto Rewritten = rewriteVirtualRegisters(*Fixture->Function, Target, Mapping);
  const MachineOperand *Def = Fixture->Block->firstInstruction()->operandAt(0);
  Context.expect(
      Rewritten && *Rewritten && Def != nullptr && Def->registerValue() &&
          Def->registerValue() == Register::fromPhysical(physical(TestRegister::G0Low32)) &&
          Def->subReg().value() == 0 &&
          std::ranges::none_of(Fixture->Function->registerInfo().virtualRegisters(),
                               [](const VirtualRegisterInfo &Info) { return !Info.IsTombstone; }),
      "VirtualRegisterRewrite must project and clear subregister operands");

  auto InvalidFixture = createFunction("virt-rewrite-invalid", 12);
  if (!InvalidFixture)
    return;
  auto Invalid = createVReg(*InvalidFixture->Function);
  if (!Invalid ||
      !buildDef(*InvalidFixture->Function, Target, *InvalidFixture->Block, *Invalid, 2) ||
      !buildUse(*InvalidFixture->Function, Target, *InvalidFixture->Block, *Invalid) ||
      !prepareForAllocation(*InvalidFixture->Function, Target))
    return;
  VirtRegMap InvalidMapping(*InvalidFixture->Function);
  const PhysicalRegister F0 = physical(TestRegister::F0);
  if (!InvalidMapping.add(*Invalid) || !InvalidMapping.assign(*Invalid, F0))
    return;
  auto Failed = rewriteVirtualRegisters(*InvalidFixture->Function, Target, InvalidMapping);
  Context.expect(!Failed && Failed.error().code() == ErrorCode::RegisterAllocationFailed,
                 "VirtualRegisterRewrite must reject class-incompatible mappings atomically");
}

} // namespace

int main() {
  TestContext Context;
  auto Target = createTarget();
  Context.expect(Target.has_value(), "VirtRegMap test target must be constructible");
  if (!Target)
    return Context.result();
  testSplitMetadata(Context, *Target);
  testRewriteAndAtomicValidation(Context, *Target);
  return Context.result();
}
