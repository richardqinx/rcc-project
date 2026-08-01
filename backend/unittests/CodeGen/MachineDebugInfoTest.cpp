// SPDX-License-Identifier: GPL-3.0-or-later

#include "Target/TestTarget.h"
#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineFunctionAttributes.h"
#include "rcc/CodeGen/MachineFunctionCloner.h"
#include "rcc/CodeGen/MachineFunctionRecords.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineVerifier.h"
#include "rcc/CodeGen/TargetDescription.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t ValueOpcode = 1;
constexpr std::uint32_t MetaOpcode = 2;

class TestContext final {
public:
  void expect(bool Condition, std::string_view Message) {
    if (Condition)
      return;
    std::cerr << Message << '\n';
    ++FailureCount;
  }

  [[nodiscard]] int result() const noexcept {
    return FailureCount == 0 ? 0 : 1;
  }

private:
  int FailureCount = 0;
};

[[nodiscard]] rcc::Expected<rcc::TargetDescription> createTarget() {
  rcc::InstructionDescriptor Value;
  Value.InstructionOpcode = rcc::Opcode::fromValue(ValueOpcode);
  Value.Name = "TEST_VALUE";
  Value.Flags = {rcc::InstructionFlag::ProducesCode};

  rcc::InstructionDescriptor Meta;
  Meta.InstructionOpcode = rcc::Opcode::fromValue(MetaOpcode);
  Meta.Name = "TEST_META";
  Meta.Flags = {rcc::InstructionFlag::Meta};

  std::vector<rcc::InstructionDescriptor> Descriptors;
  Descriptors.push_back(std::move(Value));
  Descriptors.push_back(std::move(Meta));
  auto Instructions = rcc::InstructionSet::create(std::move(Descriptors));
  if (!Instructions)
    return std::unexpected(Instructions.error());
  return rcc::test::createTestTargetDescription(std::move(*Instructions));
}

[[nodiscard]] std::unique_ptr<rcc::MachineFunction>
createFunction(TestContext &Context, std::uint64_t StableID, std::string Name) {
  auto Function = rcc::MachineFunction::create(std::move(Name),
                                               rcc::MachineFunctionStableID::fromValue(StableID));
  Context.expect(Function.has_value(), "machine function creation must succeed");
  if (!Function)
    return nullptr;
  return std::move(*Function);
}

[[nodiscard]] rcc::MachineBasicBlock *appendBlock(TestContext &Context,
                                                  rcc::MachineFunction &Function) {
  auto Block = Function.createBasicBlock();
  Context.expect(Block.has_value() && Function.appendBlock(**Block).has_value(),
                 "machine basic block creation and linking must succeed");
  return Block ? *Block : nullptr;
}

[[nodiscard]] rcc::MachineInstr *appendInstruction(TestContext &Context,
                                                   rcc::MachineFunction &Function,
                                                   rcc::MachineBasicBlock &Block,
                                                   std::uint32_t OpcodeValue) {
  auto Instruction = Function.createInstruction(rcc::Opcode::fromValue(OpcodeValue));
  Context.expect(Instruction.has_value() && Block.appendInstruction(**Instruction).has_value(),
                 "machine instruction creation and linking must succeed");
  return Instruction ? *Instruction : nullptr;
}

void testInstructionNumbering(TestContext &Context) {
  auto Target = createTarget();
  auto Function = createFunction(Context, 1, "debug-numbering");
  if (!Target || Function == nullptr)
    return;
  rcc::MachineBasicBlock *Block = appendBlock(Context, *Function);
  if (Block == nullptr)
    return;
  rcc::MachineInstr *First = appendInstruction(Context, *Function, *Block, ValueOpcode);
  rcc::MachineInstr *Second = appendInstruction(Context, *Function, *Block, ValueOpcode);
  rcc::MachineInstr *Third = appendInstruction(Context, *Function, *Block, ValueOpcode);
  if (First == nullptr || Second == nullptr || Third == nullptr)
    return;

  Context.expect(!First->setDebugInstrNumber(0) &&
                     !First->setDebugInstrNumber(
                         static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1),
                 "debug instruction numbers must fit in nonzero uint32");
  Context.expect(First->setDebugInstrNumber(7).has_value(),
                 "an explicit debug instruction number must be accepted");
  auto SecondNumber = Second->assignDebugInstrNumber();
  auto ExistingSecondNumber = Second->assignDebugInstrNumber();
  Context.expect(SecondNumber.has_value() && *SecondNumber == 8 &&
                     ExistingSecondNumber.has_value() && *ExistingSecondNumber == *SecondNumber,
                 "generated debug numbers must continue after the largest observed number");
  Second->clearDebugInstrNumber();
  auto ReassignedSecondNumber = Second->assignDebugInstrNumber();
  Context.expect(ReassignedSecondNumber.has_value() && *ReassignedSecondNumber == 9,
                 "cleared debug numbers must not rewind the monotonic allocator");
  Context.expect(Third->setDebugInstrNumber(7).has_value() &&
                     !rcc::MachineVerifier::verify(*Function, *Target),
                 "the unified verifier must reject duplicate live debug numbers");
  Context.expect(!Third->setDebugInstrNumber(10),
                 "an existing debug identity must be cleared before replacement");
  Third->clearDebugInstrNumber();
  Context.expect(Third->setDebugInstrNumber(10).has_value() &&
                     rcc::MachineVerifier::verify(*Function, *Target).has_value(),
                 "unique debug numbers must pass unified verification");
}

void testSubstitutionsAndClone(TestContext &Context) {
  auto Target = createTarget();
  auto Function = createFunction(Context, 2, "debug-substitutions");
  if (!Target || Function == nullptr)
    return;
  rcc::MachineBasicBlock *Block = appendBlock(Context, *Function);
  if (Block == nullptr)
    return;
  rcc::MachineInstr *Instruction = appendInstruction(Context, *Function, *Block, ValueOpcode);
  if (Instruction == nullptr)
    return;
  Context.expect(Instruction->setDebugInstrNumber(7).has_value(),
                 "tracked instruction numbering must succeed");

  const rcc::MachineDebugValueSubstitution Substitution{
      .Source = {.InstructionNumber = 1, .OperandNumber = 0},
      .Destination = {.InstructionNumber = 7, .OperandNumber = 1},
      .Subregister = rcc::test::TestSubReg::Low32,
  };
  Context.expect(Function->records().addDebugValueSubstitution(Substitution).has_value(),
                 "a structured debug value substitution must be stored");
  Context.expect(!Function->records().addDebugValueSubstitution(Substitution),
                 "a debug value source must have at most one replacement");
  Context.expect(!Function->records().addDebugValueSubstitution({
                     .Source =
                         {
                             .InstructionNumber = 2,
                             .OperandNumber = rcc::MachineDebugMemoryOperandNumber,
                         },
                     .Destination = {.InstructionNumber = 7, .OperandNumber = 0},
                     .Subregister = {},
                 }),
                 "a substitution cannot originate from the reserved memory operand");
  Context.expect(!Function->records().addDebugValueSubstitution({
                     .Source = {.InstructionNumber = 3, .OperandNumber = 0},
                     .Destination = {.InstructionNumber = 3, .OperandNumber = 1},
                     .Subregister = {},
                 }),
                 "a substitution cannot replace a value within one instruction");

  Context.expect(!rcc::MachineVerifier::verify(*Function, *Target),
                 "substitutions must require the DebugInstrRef function policy");
  Function->attributes().set(rcc::MachineFunctionAttribute::DebugInstrRef);
  Context.expect(rcc::MachineVerifier::verify(*Function, *Target).has_value(),
                 "well-formed substitutions must pass unified verification");

  auto Clone =
      rcc::cloneMachineFunction(*Function, *Target,
                                {
                                    .NewStableID = rcc::MachineFunctionStableID::fromValue(3),
                                    .NewName = std::string("debug-substitutions.clone"),
                                });
  Context.expect(Clone.has_value(), "tracked debug state must support explicit deep cloning");
  if (!Clone)
    return;
  const rcc::MachineBasicBlock *ClonedBlock = Clone->ClonedFunction->firstBlock();
  const rcc::MachineInstr *ClonedInstruction =
      ClonedBlock == nullptr ? nullptr : ClonedBlock->firstInstruction();
  Context.expect(ClonedInstruction != nullptr && ClonedInstruction->debugInstrNumber() == 7 &&
                     Clone->ClonedFunction->records().debugValueSubstitutions().size() == 1 &&
                     Clone->ClonedFunction->records().debugValueSubstitutions()[0] == Substitution,
                 "deep clone must preserve debug numbers and substitution identities");
}

void testVerifierFailures(TestContext &Context) {
  auto Target = createTarget();
  if (!Target)
    return;

  {
    auto Function = createFunction(Context, 10, "debug-meta");
    if (Function == nullptr)
      return;
    rcc::MachineBasicBlock *Block = appendBlock(Context, *Function);
    if (Block == nullptr)
      return;
    rcc::MachineInstr *Meta = appendInstruction(Context, *Function, *Block, MetaOpcode);
    if (Meta == nullptr)
      return;
    Context.expect(Meta->setDebugInstrNumber(1).has_value() &&
                       !rcc::MachineVerifier::verify(*Function, *Target),
                   "metadata instructions must not carry debug value tracking numbers");
  }

  {
    auto Function = createFunction(Context, 11, "debug-subreg");
    if (Function == nullptr)
      return;
    rcc::MachineBasicBlock *Block = appendBlock(Context, *Function);
    if (Block == nullptr)
      return;
    rcc::MachineInstr *Instruction = appendInstruction(Context, *Function, *Block, ValueOpcode);
    if (Instruction == nullptr)
      return;
    Function->attributes().set(rcc::MachineFunctionAttribute::DebugInstrRef);
    Context.expect(Function->records()
                           .addDebugValueSubstitution({
                               .Source = {.InstructionNumber = 1, .OperandNumber = 0},
                               .Destination = {.InstructionNumber = 2, .OperandNumber = 0},
                               .Subregister = rcc::SubRegIndex::fromValue(999),
                           })
                           .has_value() &&
                       !rcc::MachineVerifier::verify(*Function, *Target),
                   "debug substitutions must reject target-unknown subregister indices");
  }

  {
    rcc::MachineFunctionRecords Records;
    Context.expect(Records
                           .addDebugValueSubstitution({
                               .Source = {.InstructionNumber = 1, .OperandNumber = 0},
                               .Destination = {.InstructionNumber = 2, .OperandNumber = 0},
                               .Subregister = {},
                           })
                           .has_value() &&
                       Records
                           .addDebugValueSubstitution({
                               .Source = {.InstructionNumber = 2, .OperandNumber = 0},
                               .Destination = {.InstructionNumber = 1, .OperandNumber = 0},
                               .Subregister = {},
                           })
                           .has_value() &&
                       !Records.verify(),
                   "debug substitution chains must be acyclic");
  }
}

} // namespace

int main() {
  TestContext Context;
  testInstructionNumbering(Context);
  testSubstitutionsAndClone(Context);
  testVerifierFailures(Context);
  return Context.result();
}
