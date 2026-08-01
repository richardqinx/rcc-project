// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineFunctionRecords.h"

#include "Target/TestTarget.h"
#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineDebugInfo.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineFunctionCloner.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineVerifier.h"
#include "rcc/CodeGen/TargetDescription.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t NopOpcode = 1;
constexpr std::uint32_t CallOpcode = 2;

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

[[nodiscard]] rcc::PhysicalRegister phys(std::uint32_t Number) {
  auto Reg = rcc::PhysicalRegister::create(Number);
  return Reg ? *Reg : rcc::PhysicalRegister{};
}

[[nodiscard]] rcc::Expected<rcc::TargetDescription> createTarget() {
  rcc::InstructionDescriptor Nop;
  Nop.InstructionOpcode = rcc::Opcode::fromValue(NopOpcode);
  Nop.Name = "TEST_NOP";
  Nop.Flags = {rcc::InstructionFlag::ProducesCode};

  rcc::InstructionDescriptor Call;
  Call.InstructionOpcode = rcc::Opcode::fromValue(CallOpcode);
  Call.Name = "TEST_CALL";
  Call.Flags = {rcc::InstructionFlag::Call, rcc::InstructionFlag::HasSideEffects,
                rcc::InstructionFlag::ProducesCode};

  std::vector<rcc::InstructionDescriptor> Descriptors;
  Descriptors.push_back(std::move(Nop));
  Descriptors.push_back(std::move(Call));
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
  Context.expect(Block.has_value(), "machine basic block creation must succeed");
  if (!Block)
    return nullptr;
  Context.expect(Function.appendBlock(**Block).has_value(), "machine block linking must succeed");
  return *Block;
}

[[nodiscard]] rcc::MachineInstr *appendInstruction(TestContext &Context,
                                                   rcc::MachineFunction &Function,
                                                   rcc::MachineBasicBlock &Block,
                                                   std::uint32_t OpcodeValue) {
  auto Instruction = Function.createInstruction(rcc::Opcode::fromValue(OpcodeValue));
  Context.expect(Instruction.has_value(), "machine instruction creation must succeed");
  if (!Instruction)
    return nullptr;
  Context.expect(Block.appendInstruction(**Instruction).has_value(),
                 "machine instruction linking must succeed");
  return *Instruction;
}

void testValueValidation(TestContext &Context) {
  rcc::MachineFunctionRecords Records;
  Context.expect(!Records.hasCalleeSavedRegisters() && Records.calleeSavedRegisters().empty(),
                 "an absent callee-saved list must remain distinguishable");
  Context.expect(Records.setCalleeSavedRegisters({}).has_value() &&
                     Records.hasCalleeSavedRegisters() && Records.calleeSavedRegisters().empty(),
                 "an explicit empty callee-saved list must remain present");
  Records.clearCalleeSavedRegisters();

  const rcc::PhysicalRegister G0 = phys(rcc::test::TestRegister::G0);
  Context.expect(!Records.setCalleeSavedRegisters({G0, G0}),
                 "duplicate callee-saved registers must be rejected");
  Context.expect(!Records.addCallSite({}),
                 "a call-site record must require a valid instruction StableID");
  Context.expect(!Records.addCalledGlobal({
                     .CallInstruction = rcc::MachineInstrStableID::fromValue(1),
                     .Callee = {},
                     .Flags = 0,
                 }),
                 "a called-global record must require a nonempty callee");
  Context.expect(!Records.addEntryValue({}),
                 "an entry-value record must require a physical register and debug metadata");

  const rcc::MachinePrefetchTarget Target{
      .Block = {.BaseID = 7, .CloneID = 3},
      .CallsiteIndex = 4,
  };
  Context.expect(Records.addPrefetchTarget(Target).has_value() &&
                     !Records.addPrefetchTarget(Target),
                 "duplicate structured prefetch targets must be rejected");
}

void testVerifierAndClone(TestContext &Context) {
  auto Target = createTarget();
  auto Function = createFunction(Context, 1, "records");
  if (!Target || Function == nullptr)
    return;
  Context.expect(
      Function->metadata().addNode({.Slot = 0, .IsDistinct = false, .Elements = {}}).has_value(),
      "function record metadata setup must succeed");

  rcc::MachineBasicBlock *Block = appendBlock(Context, *Function);
  if (Block == nullptr)
    return;
  Block->attributes().setProfileID({.BaseID = 19, .CloneID = 2});
  rcc::MachineInstr *Call = appendInstruction(Context, *Function, *Block, CallOpcode);
  if (Call == nullptr)
    return;

  const rcc::PhysicalRegister G0 = phys(rcc::test::TestRegister::G0);
  const rcc::PhysicalRegister G1 = phys(rcc::test::TestRegister::G1);
  Context.expect(Function->records().setCalleeSavedRegisters({G0, G1}).has_value(),
                 "callee-saved registers must be stored");
  Context.expect(Function->records()
                     .addCallSite({
                         .CallInstruction = Call->stableID(),
                         .ForwardedArguments =
                             {
                                 {.Reg = G0, .ArgumentNumber = 0},
                                 {.Reg = G1, .ArgumentNumber = 0},
                             },
                         .CalleeTypeIDs = {0x11, 0x22},
                     })
                     .has_value(),
                 "structured call-site information must be stored");
  Context.expect(Function->records()
                     .addCalledGlobal({
                         .CallInstruction = Call->stableID(),
                         .Callee = "callee",
                         .Flags = 5,
                     })
                     .has_value(),
                 "structured called-global information must be stored");
  Context.expect(Function->records()
                     .addPrefetchTarget({
                         .Block = {.BaseID = 19, .CloneID = 2},
                         .CallsiteIndex = 8,
                     })
                     .has_value(),
                 "structured prefetch target information must be stored");
  auto EntryLocation =
      rcc::MachineDebugLocation::reference(rcc::MachineMetadataReference::embeddedIR(12));
  Context.expect(EntryLocation.has_value(), "entry-value debug location must be representable");
  if (!EntryLocation)
    return;
  const rcc::MachineEntryValueInfo EntryValue{
      .Reg = G0,
      .DebugInfo =
          {
              .Variable = rcc::MachineMetadataReference::embeddedIR(10),
              .Expression = rcc::MachineMetadataValue::expression(rcc::MachineDIExpression{}),
              .Location = *EntryLocation,
          },
  };
  Context.expect(Function->records().addEntryValue(EntryValue).has_value(),
                 "structured entry-value debug information must be stored");
  Context.expect(rcc::MachineVerifier::verify(*Function, *Target).has_value(),
                 "well-formed function records must pass unified verification");

  auto Clone =
      rcc::cloneMachineFunction(*Function, *Target,
                                {
                                    .NewStableID = rcc::MachineFunctionStableID::fromValue(2),
                                    .NewName = std::string("records.clone"),
                                });
  Context.expect(Clone.has_value(), "machine function records must support deep cloning");
  if (!Clone)
    return;

  const auto InstructionID = Clone->InstructionStableIDs.find(Call->stableID());
  const rcc::MachineFunctionRecords &ClonedRecords = Clone->ClonedFunction->records();
  Context.expect(
      InstructionID != Clone->InstructionStableIDs.end() &&
          ClonedRecords.hasCalleeSavedRegisters() &&
          ClonedRecords.calleeSavedRegisters().size() == 2 &&
          ClonedRecords.callSites().size() == 1 &&
          ClonedRecords.callSites()[0].CallInstruction == InstructionID->second &&
          ClonedRecords.callSites()[0].ForwardedArguments.size() == 2 &&
          ClonedRecords.callSites()[0].ForwardedArguments[1].ArgumentNumber == 0 &&
          ClonedRecords.callSites()[0].CalleeTypeIDs == std::vector<std::uint64_t>({0x11, 0x22}) &&
          ClonedRecords.calledGlobals().size() == 1 &&
          ClonedRecords.calledGlobals()[0].CallInstruction == InstructionID->second &&
          ClonedRecords.calledGlobals()[0].Callee == "callee" &&
          ClonedRecords.calledGlobals()[0].Flags == 5 &&
          ClonedRecords.prefetchTargets().size() == 1 &&
          ClonedRecords.prefetchTargets()[0].Block ==
              rcc::MachineBasicBlockProfileID{.BaseID = 19, .CloneID = 2} &&
          ClonedRecords.prefetchTargets()[0].CallsiteIndex == 8 &&
          ClonedRecords.entryValues().size() == 1 && ClonedRecords.entryValues()[0] == EntryValue,
      "deep clone must preserve records and remap instruction StableIDs");
}

void testCrossReferenceFailures(TestContext &Context) {
  auto Target = createTarget();
  if (!Target)
    return;

  {
    auto Function = createFunction(Context, 10, "noncall");
    if (Function == nullptr)
      return;
    rcc::MachineBasicBlock *Block = appendBlock(Context, *Function);
    if (Block == nullptr)
      return;
    rcc::MachineInstr *Nop = appendInstruction(Context, *Function, *Block, NopOpcode);
    if (Nop == nullptr)
      return;
    Context.expect(Function->records()
                           .addCallSite({
                               .CallInstruction = Nop->stableID(),
                               .ForwardedArguments = {},
                               .CalleeTypeIDs = {},
                           })
                           .has_value() &&
                       !rcc::MachineVerifier::verify(*Function, *Target),
                   "call-site records must reference instructions described as calls");
  }

  {
    auto Function = createFunction(Context, 11, "detached");
    if (Function == nullptr)
      return;
    rcc::MachineBasicBlock *Block = appendBlock(Context, *Function);
    if (Block == nullptr)
      return;
    auto Detached = Function->createInstruction(rcc::Opcode::fromValue(CallOpcode));
    Context.expect(Detached.has_value() &&
                       Function->records()
                           .addCalledGlobal({
                               .CallInstruction = (*Detached)->stableID(),
                               .Callee = "callee",
                           })
                           .has_value() &&
                       !rcc::MachineVerifier::verify(*Function, *Target),
                   "function records must not reference detached instructions");
  }

  {
    auto Function = createFunction(Context, 12, "unknown-register");
    if (Function == nullptr)
      return;
    rcc::MachineBasicBlock *Block = appendBlock(Context, *Function);
    if (Block == nullptr)
      return;
    rcc::MachineInstr *Call = appendInstruction(Context, *Function, *Block, CallOpcode);
    if (Call == nullptr)
      return;
    Context.expect(Function->records()
                           .addCallSite({
                               .CallInstruction = Call->stableID(),
                               .ForwardedArguments = {{.Reg = phys(999), .ArgumentNumber = 0}},
                               .CalleeTypeIDs = {},
                           })
                           .has_value() &&
                       !rcc::MachineVerifier::verify(*Function, *Target),
                   "call-site records must reject target-unknown physical registers");
  }

  {
    auto Function = createFunction(Context, 13, "unknown-bbid");
    if (Function == nullptr)
      return;
    rcc::MachineBasicBlock *Block = appendBlock(Context, *Function);
    if (Block == nullptr)
      return;
    Context.expect(Function->records()
                           .addPrefetchTarget({
                               .Block = {.BaseID = 1, .CloneID = 0},
                               .CallsiteIndex = 0,
                           })
                           .has_value() &&
                       !rcc::MachineVerifier::verify(*Function, *Target),
                   "prefetch targets must reference a live block bb_id");
  }

  {
    auto Function = createFunction(Context, 14, "duplicate-bbid");
    if (Function == nullptr)
      return;
    rcc::MachineBasicBlock *First = appendBlock(Context, *Function);
    rcc::MachineBasicBlock *Second = appendBlock(Context, *Function);
    if (First == nullptr || Second == nullptr)
      return;
    First->attributes().setProfileID({.BaseID = 3, .CloneID = 1});
    Second->attributes().setProfileID({.BaseID = 3, .CloneID = 1});
    Context.expect(!rcc::MachineVerifier::verify(*Function, *Target),
                   "linked block bb_id values must be unique");
  }

  {
    auto Function = createFunction(Context, 13, "entry-value-register");
    if (Function == nullptr)
      return;
    Context.expect(Function->records()
                           .addEntryValue({
                               .Reg = phys(999),
                               .DebugInfo =
                                   {
                                       .Variable = rcc::MachineMetadataReference::embeddedIR(1),
                                       .Expression = std::nullopt,
                                       .Location = std::nullopt,
                                   },
                           })
                           .has_value() &&
                       !rcc::MachineVerifier::verify(*Function, *Target),
                   "entry-value records must reject target-unknown physical registers");
  }
}

} // namespace

int main() {
  TestContext Context;
  testValueValidation(Context);
  testVerifierAndClone(Context);
  testCrossReferenceFailures(Context);
  return Context.result();
}
