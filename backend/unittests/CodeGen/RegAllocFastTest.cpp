// SPDX-License-Identifier: GPL-3.0-or-later

#include "CodeGen/RegisterAllocationTestSupport.h"
#include "rcc/CodeGen/MachineCFGUpdater.h"

#include <iostream>

namespace {

using namespace rcc;
using namespace rcc::test;
using namespace rcc::test::ra;

void testBlockBoundary(TestContext &Context, const TargetDescription &Target) {
  auto Fixture = createFunction("fast-boundary", 40);
  if (!Fixture)
    return;
  auto Exit = Fixture->Function->createBasicBlock();
  if (!Exit || !Fixture->Function->appendBlock(**Exit) ||
      !Fixture->Function->cfgUpdater().addSuccessor(*Fixture->Block, **Exit,
                                                    BranchProbability::one()))
    return;
  auto Value = createVReg(*Fixture->Function);
  if (!Value || !buildDef(*Fixture->Function, Target, *Fixture->Block, *Value, 9) ||
      !buildUse(*Fixture->Function, Target, **Exit, *Value) ||
      !buildUse(*Fixture->Function, Target, **Exit,
                Register::fromPhysical(physical(TestRegister::G0))) ||
      !prepareForAllocation(*Fixture->Function, Target))
    return;
  TestRegisterAllocationInfo Info;
  auto Allocated = runAllocator(*Fixture->Function, Target, Info, AllocatorKind::Fast);
  Context.expect(Allocated && countOpcode(*Fixture->Function, testOpcode(SpillStoreOpcode)) == 1 &&
                     countOpcode(*Fixture->Function, testOpcode(SpillLoadOpcode)) == 1 &&
                     Fixture->Function->properties().has(MachineFunctionProperty::NoVRegs),
                 "FastAllocator must preserve values across a block boundary");
}

void testFunctionLiveInAndDebug(TestContext &Context, const TargetDescription &Target) {
  auto Fixture = createFunction("fast-live-in", 41);
  if (!Fixture)
    return;
  auto Value = createVReg(*Fixture->Function);
  const PhysicalRegister G0 = physical(TestRegister::G0);
  if (!Value || !Fixture->Function->registerInfo().addLiveIn(G0, *Value) ||
      !buildUse(*Fixture->Function, Target, *Fixture->Block, *Value, {}, true) ||
      !buildUse(*Fixture->Function, Target, *Fixture->Block, *Value) ||
      !prepareForAllocation(*Fixture->Function, Target))
    return;
  TestRegisterAllocationInfo Info;
  auto Allocated = runAllocator(*Fixture->Function, Target, Info, AllocatorKind::Fast);
  const MachineOperand *DebugOperand = nullptr;
  for (const MachineInstr *Instruction = Fixture->Block->firstInstruction(); Instruction != nullptr;
       Instruction = Instruction->next()) {
    for (const MachineOperand &Operand : Instruction->operands()) {
      if (Operand.isDebugUse()) {
        DebugOperand = &Operand;
        break;
      }
    }
    if (DebugOperand != nullptr)
      break;
  }
  Context.expect(
      Allocated && DebugOperand != nullptr && DebugOperand->registerValue() &&
          (DebugOperand->registerValue()->isNoRegister() ||
           DebugOperand->registerValue() == Register::fromPhysical(G0)) &&
          std::ranges::none_of(Fixture->Function->registerInfo().virtualRegisters(),
                               [](const VirtualRegisterInfo &Info) { return !Info.IsTombstone; }),
      "FastAllocator must preserve function live-ins and debug locations");
}

} // namespace

int main() {
  TestContext Context;
  auto Target = createTarget();
  Context.expect(Target.has_value(), "FastAllocator test target must be constructible");
  if (!Target)
    return Context.result();
  testBlockBoundary(Context, *Target);
  testFunctionLiveInAndDebug(Context, *Target);
  return Context.result();
}
