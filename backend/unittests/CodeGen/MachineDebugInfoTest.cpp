// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineDebugInfo.h"

#include "Target/TestTarget.h"
#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFrameInfo.h"
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
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t ValueOpcode = 1024;
constexpr std::uint32_t MetaOpcode = 1025;

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
  rcc::OperandDescriptor OptionalMetadata;
  OptionalMetadata.Kinds = {rcc::MachineOperand::Kind::Metadata};
  OptionalMetadata.Optional = true;
  Value.Operands.push_back(OptionalMetadata);

  rcc::InstructionDescriptor Meta;
  Meta.InstructionOpcode = rcc::Opcode::fromValue(MetaOpcode);
  Meta.Name = "TEST_META";
  Meta.Flags = {rcc::InstructionFlag::Meta};

  std::vector<rcc::InstructionDescriptor> Descriptors;
  Descriptors.push_back(std::move(Value));
  Descriptors.push_back(std::move(Meta));
  std::vector<rcc::ImmediatePredicate> Predicates;
  auto Generic = rcc::appendGenericInstructionInfo(Descriptors, Predicates);
  if (!Generic)
    return std::unexpected(Generic.error());
  auto Instructions = rcc::InstructionSet::create(std::move(Descriptors), std::move(Predicates));
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

void testMetadataValuesAndLocations(TestContext &Context) {
  rcc::MachineMetadataInfo Metadata;
  Context.expect(Metadata.addNode({.Slot = 0, .IsDistinct = false, .Elements = {}}).has_value(),
                 "function-local debug metadata setup must succeed");

  const rcc::MachineDIExpression Expression(
      std::vector<std::uint64_t>{0x1005, 0, 0x22, 0x1000, 0, 64});
  Context.expect(Expression.elements().size() == 6 && Expression.elements()[0] == 0x1005,
                 "inline DIExpression elements must preserve their exact unsigned encoding");
  auto ExpressionValue = rcc::MachineMetadataValue::expression(Expression);
  Context.expect(ExpressionValue.verify(Metadata).has_value() &&
                     ExpressionValue.expression() != nullptr &&
                     *ExpressionValue.expression() == Expression,
                 "inline DIExpression metadata values must remain structured");
  Context.expect(!rcc::MachineMetadataValue{}.verify(Metadata),
                 "an unrepresented metadata value must be rejected");

  Context.expect(!rcc::MachineDILocation::create({}),
                 "an inline DILocation must contain a root frame");
  Context.expect(!rcc::MachineDILocation::create({{}}),
                 "an inline DILocation frame must contain a valid scope");
  Context.expect(!rcc::MachineDILocation::create({
                     {
                         .Line = 1,
                         .Column = 0,
                         .Scope = rcc::MachineMetadataReference::embeddedIR(1),
                         .IsImplicitCode = false,
                         .AtomGroup = 0,
                         .AtomRank = 8,
                     },
                 }),
                 "an inline DILocation atom rank must fit the fixed three-bit field");
  Context.expect(!rcc::MachineDILocation::create({
                     {
                         .Line = 1,
                         .Column = 0,
                         .Scope = rcc::MachineMetadataReference::embeddedIR(1),
                         .IsImplicitCode = false,
                         .AtomGroup = rcc::MachineDILocation::MaxAtomGroup + 1,
                         .AtomRank = 0,
                     },
                 }),
                 "an inline DILocation atom group must fit the fixed 61-bit field");

  auto InlineLocation = rcc::MachineDILocation::create(
      {
          {
              .Line = 7,
              .Column = 65'535,
              .Scope = rcc::MachineMetadataReference::embeddedIR(3),
              .IsImplicitCode = true,
              .AtomGroup = rcc::MachineDILocation::MaxAtomGroup,
              .AtomRank = 7,
          },
          {
              .Line = 9,
              .Column = 4,
              .Scope = rcc::MachineMetadataReference::embeddedIR(0),
              .IsImplicitCode = false,
              .AtomGroup = 0,
              .AtomRank = 0,
          },
      },
      rcc::MachineMetadataReference::embeddedIR(4));
  Context.expect(InlineLocation.has_value() && InlineLocation->verify(Metadata).has_value() &&
                     InlineLocation->inlineChain().size() == 2 &&
                     InlineLocation->terminalInlinedAt() ==
                         rcc::MachineMetadataReference::embeddedIR(4),
                 "inline DILocation chains must preserve nested and terminal references");
  if (!InlineLocation)
    return;

  auto DebugLocation = rcc::MachineDebugLocation::inlineLocation(*InlineLocation);
  Context.expect(DebugLocation.verify(Metadata).has_value() &&
                     DebugLocation.inlineLocation() != nullptr,
                 "a structured inline DILocation must form a debug-location attachment");
  rcc::MachineVariableDebugInfo VariableInfo{
      .Variable = rcc::MachineMetadataReference::embeddedIR(5),
      .Expression = ExpressionValue,
      .Location = DebugLocation,
  };
  Context.expect(VariableInfo.verify(Metadata).has_value() &&
                     !rcc::MachineVariableDebugInfo{}.verify(Metadata),
                 "variable debug records must carry at least one verified metadata component");

  auto Undefined =
      rcc::MachineDebugLocation::reference(rcc::MachineMetadataReference::machineFunction(99));
  Context.expect(!Undefined, "typed debug locations must not accept function-local tuple metadata");
}

void testInstructionMetadataAndClone(TestContext &Context) {
  auto Target = createTarget();
  auto Function = createFunction(Context, 20, "debug-metadata");
  if (!Target || Function == nullptr)
    return;
  Context.expect(
      Function->metadata().addNode({.Slot = 0, .IsDistinct = false, .Elements = {}}).has_value(),
      "instruction metadata graph setup must succeed");
  rcc::MachineBasicBlock *Block = appendBlock(Context, *Function);
  if (Block == nullptr)
    return;
  rcc::MachineInstr *Instruction = appendInstruction(Context, *Function, *Block, ValueOpcode);
  if (Instruction == nullptr)
    return;

  auto MetadataValue =
      rcc::MachineMetadataValue::reference(rcc::MachineMetadataReference::machineFunction(0));
  Context.expect(MetadataValue.has_value(), "instruction metadata reference must be valid");
  if (!MetadataValue)
    return;
  auto Operand = rcc::MachineOperand::metadata(std::move(*MetadataValue));
  auto InlineLocation = rcc::MachineDILocation::create({
      {
          .Line = 12,
          .Column = 8,
          .Scope = rcc::MachineMetadataReference::embeddedIR(7),
          .IsImplicitCode = false,
          .AtomGroup = 0,
          .AtomRank = 0,
      },
  });
  Context.expect(Operand.has_value() && InlineLocation.has_value(),
                 "instruction metadata values must be constructible");
  if (!Operand || !InlineLocation)
    return;
  const rcc::MachineDebugLocation DebugLocation =
      rcc::MachineDebugLocation::inlineLocation(*InlineLocation);
  Context.expect(Instruction->addOperand(std::move(*Operand)).has_value() &&
                     Instruction->setDebugLocation(DebugLocation).has_value(),
                 "instruction metadata operand and debug location must attach independently");
  Instruction->setSourceLocation({"debug-input.mir", 3, 5, 0});
  const rcc::MachineVariableDebugInfo StackDebugInfo{
      .Variable = rcc::MachineMetadataReference::embeddedIR(8),
      .Expression = rcc::MachineMetadataValue::expression(
          rcc::MachineDIExpression(std::vector<std::uint64_t>{0x1000, 0, 64})),
      .Location = DebugLocation,
  };
  rcc::StackObjectDesc StackDescription;
  StackDescription.Size = 8;
  StackDescription.Align = *rcc::Alignment::create(8);
  StackDescription.DebugInfo = StackDebugInfo;
  auto StackIndex = Function->frameInfo().createObject(std::move(StackDescription));
  Context.expect(StackIndex.has_value(),
                 "structured variable debug information must attach to a stack object");
  if (!StackIndex)
    return;
  Context.expect(rcc::MachineVerifier::verify(*Function, *Target).has_value(),
                 "well-formed instruction metadata must pass unified verification");

  auto Clone =
      rcc::cloneMachineFunction(*Function, *Target,
                                {
                                    .NewStableID = rcc::MachineFunctionStableID::fromValue(21),
                                    .NewName = std::string("debug-metadata.clone"),
                                });
  Context.expect(Clone.has_value(), "instruction debug metadata must support deep cloning");
  if (!Clone)
    return;
  const rcc::MachineInstr *ClonedInstruction =
      Clone->ClonedFunction->firstBlock()->firstInstruction();
  const auto ClonedStackIndex = Clone->FrameIndices.find(*StackIndex);
  const rcc::StackObject *ClonedStack =
      ClonedStackIndex == Clone->FrameIndices.end()
          ? nullptr
          : Clone->ClonedFunction->frameInfo().object(ClonedStackIndex->second);
  Context.expect(
      ClonedInstruction->debugLocation() == DebugLocation &&
          ClonedInstruction->sourceLocation() == rcc::SourceLocation{"debug-input.mir", 3, 5, 0} &&
          ClonedInstruction->operandAt(0)->getIf<rcc::MetadataOperand>()->Value.reference() !=
              nullptr &&
          ClonedStack != nullptr && ClonedStack->description() != nullptr &&
          ClonedStack->description()->DebugInfo == StackDebugInfo,
      "clone must preserve distinct parser source and semantic debug information");

  auto InvalidFunction = createFunction(Context, 22, "invalid-debug-metadata");
  if (InvalidFunction == nullptr)
    return;
  rcc::MachineBasicBlock *InvalidBlock = appendBlock(Context, *InvalidFunction);
  if (InvalidBlock == nullptr)
    return;
  rcc::MachineInstr *InvalidInstruction =
      appendInstruction(Context, *InvalidFunction, *InvalidBlock, ValueOpcode);
  if (InvalidInstruction == nullptr)
    return;
  auto InvalidValue =
      rcc::MachineMetadataValue::reference(rcc::MachineMetadataReference::machineFunction(9));
  Context.expect(InvalidValue.has_value(), "undefined scoped references remain representable");
  if (!InvalidValue)
    return;
  auto InvalidOperand = rcc::MachineOperand::metadata(std::move(*InvalidValue));
  Context.expect(InvalidOperand.has_value() &&
                     InvalidInstruction->addOperand(std::move(*InvalidOperand)).has_value() &&
                     !rcc::MachineVerifier::verify(*InvalidFunction, *Target),
                 "undefined instruction metadata references must fail unified verification");

  auto InvalidFrameFunction = createFunction(Context, 23, "invalid-frame-debug-metadata");
  if (InvalidFrameFunction == nullptr)
    return;
  rcc::StackObjectDesc InvalidStackDescription;
  InvalidStackDescription.Size = 8;
  InvalidStackDescription.Align = *rcc::Alignment::create(8);
  InvalidStackDescription.DebugInfo = rcc::MachineVariableDebugInfo{
      .Variable = rcc::MachineMetadataReference::machineFunction(9),
      .Expression = std::nullopt,
      .Location = std::nullopt,
  };
  Context.expect(
      !InvalidFrameFunction->frameInfo().createObject(std::move(InvalidStackDescription)),
      "typed stack debug information must reject function-local tuple metadata");
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
    auto Function = createFunction(Context, 9, "missing-debug-location");
    if (Function == nullptr)
      return;
    rcc::MachineBasicBlock *Block = appendBlock(Context, *Function);
    if (Block == nullptr)
      return;
    auto Instruction = Function->createInstruction(rcc::opcode(rcc::GenericOpcode::DBG_VALUE));
    if (!Instruction)
      return;
    for (const std::int64_t Value : {1, 2, 3, 4}) {
      if (!(*Instruction)->addOperand(rcc::MachineOperand::immediate(Value)))
        return;
    }
    Context.expect(Block->appendInstruction(**Instruction).has_value(),
                   "four-operand DBG_VALUE must be linkable for verifier coverage");
    auto Verification = rcc::MachineVerifier::verify(*Function, *Target);
    Context.expect(!Verification &&
                       Verification.error().message().find(
                           "Bad machine code: Missing DebugLoc for debug instruction\n"
                           "- instruction: DBG_VALUE 1, 2, 3, 4") != std::string_view::npos,
                   "complete DBG_VALUE must require a DebugLoc with LLVM-compatible diagnostics");
  }

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
  testMetadataValuesAndLocations(Context);
  testInstructionMetadataAndClone(Context);
  testInstructionNumbering(Context);
  testSubstitutionsAndClone(Context);
  testVerifierFailures(Context);
  return Context.result();
}
