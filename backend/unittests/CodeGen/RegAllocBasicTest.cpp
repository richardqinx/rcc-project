// SPDX-License-Identifier: GPL-3.0-or-later

#include "CodeGen/RegisterAllocationTestSupport.h"

#include <iostream>

namespace {

using namespace rcc;
using namespace rcc::test;
using namespace rcc::test::ra;

[[nodiscard]] Expected<MachineInstr *> buildEarly(MachineFunction &Function,
                                                  const TargetDescription &Target,
                                                  MachineBasicBlock &Block, Register Destination,
                                                  Register Source) {
  MachineIRBuilder Builder(Function, Target);
  if (auto Result = Builder.setInsertPoint(Block); !Result)
    return std::unexpected(Result.error());
  auto Pending = Builder.buildInstr(testOpcode(EarlyOpcode));
  if (!Pending)
    return std::unexpected(Pending.error());
  RegisterOperandFlags Flags;
  Flags.set(RegisterOperandFlag::EarlyClobber);
  if (auto Result = Pending->addDef(Destination, Flags); !Result)
    return std::unexpected(Result.error());
  if (auto Result = Pending->addUse(Source); !Result)
    return std::unexpected(Result.error());
  return Pending->build();
}

void testHintsAndPressure(TestContext &Context, const TargetDescription &Target) {
  auto HintFixture = createFunction("basic-hint", 30);
  if (!HintFixture)
    return;
  auto HintValue = createVReg(*HintFixture->Function);
  if (!HintValue || !buildDef(*HintFixture->Function, Target, *HintFixture->Block, *HintValue, 1) ||
      !buildUse(*HintFixture->Function, Target, *HintFixture->Block, *HintValue) ||
      !HintFixture->Function->registerInfo().addAllocationHint(*HintValue,
                                                               physical(TestRegister::G1)) ||
      !prepareForAllocation(*HintFixture->Function, Target))
    return;
  TestRegisterAllocationInfo Info;
  auto Allocated = runAllocator(*HintFixture->Function, Target, Info, AllocatorKind::Basic);
  const MachineInstr *Use = HintFixture->Block->lastInstruction();
  Context.expect(Allocated && Use != nullptr &&
                     Use->operandAt(0)->registerValue() ==
                         Register::fromPhysical(physical(TestRegister::G1)),
                 "BasicAllocator must honor the first allocation hint");

  auto Pressure = createFunction("basic-pressure", 31);
  if (!Pressure)
    return;
  std::vector<Register> Values;
  for (std::int64_t Value = 0; Value < 3; ++Value) {
    auto Reg = createVReg(*Pressure->Function);
    if (!Reg || !buildDef(*Pressure->Function, Target, *Pressure->Block, *Reg, Value))
      return;
    Values.push_back(*Reg);
  }
  for (const Register Reg : Values)
    if (!buildUse(*Pressure->Function, Target, *Pressure->Block, Reg))
      return;
  if (!prepareForAllocation(*Pressure->Function, Target))
    return;
  Allocated = runAllocator(*Pressure->Function, Target, Info, AllocatorKind::Basic,
                           {.EnableRematerialization = false});
  Context.expect(Allocated &&
                     Pressure->Function->properties().has(MachineFunctionProperty::NoVRegs) &&
                     Pressure->Function->frameInfo().liveObjectCount() != 0,
                 "BasicAllocator must close spill/reload pressure");
}

void testEarlyClobberAndRewrite(TestContext &Context, const TargetDescription &Target) {
  auto Fixture = createFunction("basic-constraints", 32);
  if (!Fixture)
    return;
  auto Source = createVReg(*Fixture->Function);
  auto Destination = createVReg(*Fixture->Function);
  if (!Source || !Destination ||
      !buildDef(*Fixture->Function, Target, *Fixture->Block, *Source, 1) ||
      !buildEarly(*Fixture->Function, Target, *Fixture->Block, *Destination, *Source) ||
      !buildTied(*Fixture->Function, Target, *Fixture->Block, *Destination) ||
      !buildUse(*Fixture->Function, Target, *Fixture->Block, *Destination) ||
      !prepareForAllocation(*Fixture->Function, Target))
    return;
  TestRegisterAllocationInfo Info;
  auto Allocated = runAllocator(*Fixture->Function, Target, Info, AllocatorKind::Basic);
  const MachineInstr *Early = Fixture->Block->firstInstruction()->next();
  const MachineInstr *Tied = Early == nullptr ? nullptr : Early->next();
  Context.expect(Allocated && Early != nullptr && Tied != nullptr &&
                     Early->operandAt(0)->registerValue() != Early->operandAt(1)->registerValue() &&
                     Tied->operandAt(0)->registerValue() == Tied->operandAt(1)->registerValue(),
                 "BasicAllocator must preserve early-clobber and tied constraints");
}

} // namespace

int main() {
  TestContext Context;
  auto Target = createTarget();
  Context.expect(Target.has_value(), "BasicAllocator test target must be constructible");
  if (!Target)
    return Context.result();
  testHintsAndPressure(Context, *Target);
  testEarlyClobberAndRewrite(Context, *Target);
  return Context.result();
}
