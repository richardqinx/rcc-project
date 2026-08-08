// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/Spiller.h"

#include "CodeGen/RegisterAllocationTestSupport.h"

#include <iostream>
#include <optional>

namespace {

using namespace rcc;
using namespace rcc::test;
using namespace rcc::test::ra;

void testRematerialization(TestContext &Context, const TargetDescription &Target) {
  auto Fixture = createFunction("spiller-remat", 20);
  if (!Fixture)
    return;
  auto Value = createVReg(*Fixture->Function);
  if (!Value || !buildDef(*Fixture->Function, Target, *Fixture->Block, *Value, 42, {}, true) ||
      !buildUse(*Fixture->Function, Target, *Fixture->Block, *Value) ||
      !buildUse(*Fixture->Function, Target, *Fixture->Block, *Value))
    return;
  VirtRegMap Mapping(*Fixture->Function);
  TestRegisterAllocationInfo Info;
  if (!Mapping.add(*Value))
    return;
  Spiller ValueSpiller(*Fixture->Function, Target, Info, Mapping, true);
  auto Children = ValueSpiller.spill(*Value);
  Context.expect(Children && Children->size() == 2 &&
                     Fixture->Function->frameInfo().liveObjectCount() == 0 &&
                     countOpcode(*Fixture->Function, testOpcode(RematerializableDefOpcode)) == 2 &&
                     countOpcode(*Fixture->Function, testOpcode(SpillLoadOpcode)) == 0,
                 "Spiller must rematerialize each use without allocating a stack slot");
}

void testDirectSpillAndDebugUse(TestContext &Context, const TargetDescription &Target) {
  auto Fixture = createFunction("spiller-debug", 21);
  if (!Fixture)
    return;
  auto Value = createVReg(*Fixture->Function);
  if (!Value || !buildDef(*Fixture->Function, Target, *Fixture->Block, *Value, 7) ||
      !buildUse(*Fixture->Function, Target, *Fixture->Block, *Value, {}, true) ||
      !buildUse(*Fixture->Function, Target, *Fixture->Block, *Value))
    return;
  VirtRegMap Mapping(*Fixture->Function);
  TestRegisterAllocationInfo Info;
  if (!Mapping.add(*Value))
    return;
  Spiller ValueSpiller(*Fixture->Function, Target, Info, Mapping, false);
  auto Children = ValueSpiller.spill(*Value);
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
  const std::optional<Register> DebugRegister =
      DebugOperand == nullptr ? std::nullopt : DebugOperand->registerValue();
  const bool DebugRegisterCleared = DebugRegister && DebugRegister.value().isNoRegister();
  Context.expect(Children && !Children->empty() &&
                     countOpcode(*Fixture->Function, testOpcode(SpillLoadOpcode)) != 0 &&
                     countOpcode(*Fixture->Function, testOpcode(SpillStoreOpcode)) != 0 &&
                     DebugOperand != nullptr && DebugRegisterCleared &&
                     DebugOperand->registerFlags().has(RegisterOperandFlag::Undef),
                 "Spiller must preserve direct spill code and clear debug-only uses");

  auto Unspillable = createVReg(*Fixture->Function);
  if (!Unspillable || !Mapping.add(*Unspillable, false))
    return;
  auto Rejected = ValueSpiller.spill(*Unspillable);
  Context.expect(!Rejected && Rejected.error().code() == ErrorCode::RegisterAllocationFailed,
                 "Spiller must reject an explicitly non-spillable value");
}

} // namespace

int main() {
  TestContext Context;
  auto Target = createTarget();
  Context.expect(Target.has_value(), "Spiller test target must be constructible");
  if (!Target)
    return Context.result();
  testRematerialization(Context, *Target);
  testDirectSpillAndDebugUse(Context, *Target);
  return Context.result();
}
