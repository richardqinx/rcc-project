// SPDX-License-Identifier: GPL-3.0-or-later

#include "CodeGen/RegisterAllocationTestSupport.h"

#include <iostream>

namespace {

using namespace rcc;
using namespace rcc::test;
using namespace rcc::test::ra;

[[nodiscard]] Expected<FunctionFixture> createPressureFunction(const TargetDescription &Target,
                                                               std::uint64_t ID, bool WithCopy) {
  auto Fixture = createFunction("pbqp-pressure", ID);
  if (!Fixture)
    return std::unexpected(Fixture.error());
  std::vector<Register> Values;
  for (std::int64_t Value = 0; Value < 3; ++Value) {
    auto Reg = createVReg(*Fixture->Function);
    if (!Reg || !buildDef(*Fixture->Function, Target, *Fixture->Block, *Reg, Value))
      return makeUnexpected(ErrorCode::InternalError, "failed to construct PBQP pressure value");
    Values.push_back(*Reg);
  }
  if (WithCopy && !buildCopy(*Fixture->Function, Target, *Fixture->Block, Values[1], Values[0]))
    return makeUnexpected(ErrorCode::InternalError, "failed to construct PBQP copy");
  for (const Register Reg : Values)
    if (!buildUse(*Fixture->Function, Target, *Fixture->Block, Reg))
      return makeUnexpected(ErrorCode::InternalError, "failed to construct PBQP use");
  auto Prepared = prepareForAllocation(*Fixture->Function, Target);
  if (!Prepared)
    return std::unexpected(Prepared.error());
  return Fixture;
}

[[nodiscard]] std::vector<std::uint64_t> signature(const MachineFunction &Function) {
  std::vector<std::uint64_t> Result;
  Result.push_back(Function.frameInfo().liveObjectCount());
  for (const MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
       Block = Block->next()) {
    for (const MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      Result.push_back(Instruction->opcode().value());
      for (const MachineOperand &Operand : Instruction->operands()) {
        Result.push_back(static_cast<std::uint64_t>(Operand.kind()));
        if (const Register *Reg = Operand.getIf<Register>())
          Result.push_back(Reg->opaqueValue());
      }
    }
  }
  return Result;
}

void testHintsAndDeterminism(TestContext &Context, const TargetDescription &Target) {
  auto HintFixture = createFunction("pbqp-hint", 50);
  if (!HintFixture)
    return;
  auto Value = createVReg(*HintFixture->Function);
  if (!Value || !buildDef(*HintFixture->Function, Target, *HintFixture->Block, *Value, 1) ||
      !buildUse(*HintFixture->Function, Target, *HintFixture->Block, *Value) ||
      !HintFixture->Function->registerInfo().addAllocationHint(*Value,
                                                               physical(TestRegister::G1)) ||
      !HintFixture->Function->registerInfo().addAllocationHint(*Value,
                                                               physical(TestRegister::G0)) ||
      !prepareForAllocation(*HintFixture->Function, Target))
    return;
  TestRegisterAllocationInfo Info;
  auto Allocated = runAllocator(*HintFixture->Function, Target, Info, AllocatorKind::PBQP);
  const MachineInstr *Use = HintFixture->Block->lastInstruction();
  Context.expect(Allocated && Use != nullptr &&
                     Use->operandAt(0)->registerValue() ==
                         Register::fromPhysical(physical(TestRegister::G1)),
                 "PBQP must preserve allocation-hint preference order");

  auto First = createPressureFunction(Target, 51, false);
  auto Second = createPressureFunction(Target, 52, false);
  if (!First || !Second)
    return;
  auto FirstResult = runAllocator(*First->Function, Target, Info, AllocatorKind::PBQP,
                                  {.EnableRematerialization = false});
  auto SecondResult = runAllocator(*Second->Function, Target, Info, AllocatorKind::PBQP,
                                   {.EnableRematerialization = false});
  Context.expect(FirstResult && SecondResult &&
                     signature(*First->Function) == signature(*Second->Function),
                 "PBQP allocation and spill/rebuild order must be deterministic");
}

void testCopyCoalescingSwitch(TestContext &Context, const TargetDescription &Target) {
  TestRegisterAllocationInfo Info;
  auto Coalesced = createPressureFunction(Target, 53, true);
  auto Uncoalesced = createPressureFunction(Target, 54, true);
  if (!Coalesced || !Uncoalesced)
    return;
  auto WithCoalescing =
      runAllocator(*Coalesced->Function, Target, Info, AllocatorKind::PBQP,
                   {.EnableRematerialization = false, .EnableCopyCoalescing = true});
  auto WithoutCoalescing =
      runAllocator(*Uncoalesced->Function, Target, Info, AllocatorKind::PBQP,
                   {.EnableRematerialization = false, .EnableCopyCoalescing = false});
  Context.expect(WithCoalescing && WithoutCoalescing &&
                     Coalesced->Function->properties().has(MachineFunctionProperty::NoVRegs) &&
                     Uncoalesced->Function->properties().has(MachineFunctionProperty::NoVRegs),
                 "PBQP copy-coalescing modes must both produce valid physical mappings");
}

} // namespace

int main() {
  TestContext Context;
  auto Target = createTarget();
  Context.expect(Target.has_value(), "PBQP allocator test target must be constructible");
  if (!Target)
    return Context.result();
  testHintsAndDeterminism(Context, *Target);
  testCopyCoalescingSwitch(Context, *Target);
  return Context.result();
}
