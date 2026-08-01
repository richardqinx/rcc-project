// SPDX-License-Identifier: GPL-3.0-or-later

#include "Target/TestTarget.h"
#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineBasicBlockAttributes.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineFunctionAttributes.h"
#include "rcc/CodeGen/MachineFunctionCloner.h"
#include "rcc/CodeGen/MachineIRTypes.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineVerifier.h"
#include "rcc/CodeGen/TargetDescription.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t NopOpcode = 1;

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
  rcc::InstructionDescriptor Nop;
  Nop.InstructionOpcode = rcc::Opcode::fromValue(NopOpcode);
  Nop.Name = "TEST_NOP";
  Nop.Flags = {rcc::InstructionFlag::ProducesCode};

  std::vector<rcc::InstructionDescriptor> Descriptors;
  Descriptors.push_back(std::move(Nop));
  auto Instructions = rcc::InstructionSet::create(std::move(Descriptors));
  if (!Instructions)
    return std::unexpected(Instructions.error());
  return rcc::test::createTestTargetDescription(std::move(*Instructions));
}

[[nodiscard]] std::unique_ptr<rcc::MachineFunction> createFunction(TestContext &Context,
                                                                   std::uint64_t StableID) {
  auto Function =
      rcc::MachineFunction::create("attributes", rcc::MachineFunctionStableID::fromValue(StableID));
  Context.expect(Function.has_value(), "machine function creation must succeed");
  if (!Function)
    return nullptr;
  return std::move(*Function);
}

void testAttributeValueTypes(TestContext &Context) {
  rcc::MachineFunctionAttributes FunctionAttributes;
  FunctionAttributes.set(rcc::MachineFunctionAttribute::ExposesReturnsTwice);
  FunctionAttributes.set(rcc::MachineFunctionAttribute::TracksDebugUserValues);
  Context.expect(FunctionAttributes.has(rcc::MachineFunctionAttribute::ExposesReturnsTwice) &&
                     FunctionAttributes.has(rcc::MachineFunctionAttribute::TracksDebugUserValues) &&
                     rcc::getMachineFunctionAttributeName(
                         rcc::MachineFunctionAttribute::TracksDebugUserValues) ==
                         "TracksDebugUserValues",
                 "function attribute flags and names must be stable");

  auto DecodedFunctionAttributes =
      rcc::MachineFunctionAttributes::fromRawFlags(FunctionAttributes.rawFlags());
  Context.expect(DecodedFunctionAttributes.has_value() &&
                     DecodedFunctionAttributes->rawFlags() == FunctionAttributes.rawFlags(),
                 "function attribute raw flags must round trip");
  Context.expect(!rcc::MachineFunctionAttributes::fromRawFlags(std::uint16_t{1} << 15U),
                 "unknown function attribute bits must be rejected");

  rcc::MachineBasicBlockAttributes BlockAttributes;
  Context.expect(!BlockAttributes.setName("") && !BlockAttributes.setIRBlockName(""),
                 "empty block and IR block names must be rejected");
  Context.expect(BlockAttributes.setName("entry").has_value() &&
                     BlockAttributes.setIRBlockName("entry").has_value(),
                 "decoded block and IR block names must be accepted");
  BlockAttributes.set(rcc::MachineBasicBlockAttribute::IRBlockAddressTaken);
  BlockAttributes.setCallFrameSize(0);
  Context.expect(BlockAttributes.verify().has_value() && BlockAttributes.callFrameSize() == 0,
                 "zero call-frame size must be the semantic default");
  BlockAttributes.setSectionID(rcc::MachineBasicBlockSectionID::exception());
  BlockAttributes.setProfileID({.BaseID = 7, .CloneID = 2});
  Context.expect(
      BlockAttributes.sectionID().kind() == rcc::MachineBasicBlockSectionKind::Exception &&
          BlockAttributes.sectionID().number() == 0 &&
          BlockAttributes.profileID() == rcc::MachineBasicBlockProfileID{.BaseID = 7, .CloneID = 2},
      "special section and fixed profile identities must remain structured");
  Context.expect(!rcc::MachineBasicBlockAttributes::fromRawFlags(std::uint8_t{1} << 7U),
                 "unknown block attribute bits must be rejected");

  rcc::MachineInstrFlags Flags{
      rcc::MachineInstrFlag::NoConvergent, rcc::MachineInstrFlag::NonNeg,
      rcc::MachineInstrFlag::Disjoint,     rcc::MachineInstrFlag::NoUSWrap,
      rcc::MachineInstrFlag::SameSign,     rcc::MachineInstrFlag::InBounds,
  };
  Context.expect(
      Flags.has(rcc::MachineInstrFlag::NoConvergent) && Flags.has(rcc::MachineInstrFlag::NonNeg) &&
          Flags.has(rcc::MachineInstrFlag::Disjoint) &&
          Flags.has(rcc::MachineInstrFlag::NoUSWrap) &&
          Flags.has(rcc::MachineInstrFlag::SameSign) && Flags.has(rcc::MachineInstrFlag::InBounds),
      "all fixed MIR instruction flags must have independent storage");
}

void testVerifierAndClone(TestContext &Context) {
  auto Target = createTarget();
  auto Function = createFunction(Context, 1);
  if (!Target || Function == nullptr)
    return;

  auto FunctionAlignment = rcc::Alignment::create(32);
  auto BlockAlignment = rcc::Alignment::create(16);
  Context.expect(FunctionAlignment.has_value() && BlockAlignment.has_value(),
                 "power-of-two alignments must be accepted");
  if (!FunctionAlignment || !BlockAlignment)
    return;

  Function->attributes().set(rcc::MachineFunctionAttribute::ExposesReturnsTwice);
  Function->attributes().set(rcc::MachineFunctionAttribute::HasEHScopes);
  Function->attributes().set(rcc::MachineFunctionAttribute::DebugInstrRef);
  Function->attributes().setAlignment(*FunctionAlignment);

  auto BlockResult = Function->createBasicBlock();
  Context.expect(BlockResult.has_value(), "machine basic block creation must succeed");
  if (!BlockResult)
    return;
  rcc::MachineBasicBlock *Block = *BlockResult;
  Context.expect(Function->appendBlock(*Block).has_value(),
                 "machine basic block linking must succeed");
  Context.expect(Block->attributes().setName("entry").has_value(),
                 "machine basic block name must be stored");
  Block->attributes().set(rcc::MachineBasicBlockAttribute::IRBlockAddressTaken);
  Context.expect(!rcc::MachineVerifier::verify(*Function, *Target),
                 "IR-block-address-taken must require an IR block association");

  Context.expect(Block->attributes().setIRBlockName("entry").has_value(),
                 "IR block association must be stored");
  Block->attributes().set(rcc::MachineBasicBlockAttribute::LandingPad);
  Block->attributes().setAlignment(*BlockAlignment);
  Block->attributes().setSectionID(rcc::MachineBasicBlockSectionID::numbered(3));
  Block->attributes().setProfileID({.BaseID = 11, .CloneID = 4});
  Block->attributes().setCallFrameSize(64);

  auto InstructionResult = Function->createInstruction(rcc::Opcode::fromValue(NopOpcode));
  Context.expect(InstructionResult.has_value(), "machine instruction creation must succeed");
  if (!InstructionResult)
    return;
  rcc::MachineInstr *Instruction = *InstructionResult;
  Instruction->setFlags({
      rcc::MachineInstrFlag::NoConvergent,
      rcc::MachineInstrFlag::NonNeg,
      rcc::MachineInstrFlag::Disjoint,
      rcc::MachineInstrFlag::NoUSWrap,
      rcc::MachineInstrFlag::SameSign,
      rcc::MachineInstrFlag::InBounds,
  });
  Context.expect(Block->appendInstruction(*Instruction).has_value(),
                 "machine instruction linking must succeed");
  Context.expect(rcc::MachineVerifier::verify(*Function, *Target).has_value(),
                 "function and block MIR attributes must pass the unified verifier");

  auto Clone =
      rcc::cloneMachineFunction(*Function, *Target,
                                rcc::MachineFunctionCloneOptions{
                                    .NewStableID = rcc::MachineFunctionStableID::fromValue(2),
                                    .NewName = std::string("attributes.clone"),
                                });
  Context.expect(Clone.has_value(), "attributed machine function clone must succeed");
  if (!Clone)
    return;

  const rcc::MachineFunction &ClonedFunction = *Clone->ClonedFunction;
  const rcc::MachineBasicBlock *ClonedBlock = ClonedFunction.firstBlock();
  const rcc::MachineInstr *ClonedInstruction =
      ClonedBlock == nullptr ? nullptr : ClonedBlock->firstInstruction();
  Context.expect(ClonedFunction.attributes() == Function->attributes(),
                 "function MIR attributes must survive deep cloning");
  Context.expect(ClonedBlock != nullptr && ClonedBlock->attributes() == Block->attributes(),
                 "basic block MIR attributes must survive deep cloning");
  Context.expect(ClonedInstruction != nullptr && ClonedInstruction->flags() == Instruction->flags(),
                 "complete MIR instruction flags must survive deep cloning");
}

} // namespace

int main() {
  TestContext Context;
  testAttributeValueTypes(Context);
  testVerifierAndClone(Context);
  return Context.result();
}
