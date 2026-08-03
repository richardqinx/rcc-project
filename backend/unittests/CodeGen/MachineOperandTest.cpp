// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineOperand.h"

#include "Target/TestTarget.h"
#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineCFIInfo.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineFunctionAttributes.h"
#include "rcc/CodeGen/MachineFunctionCloner.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineVerifier.h"
#include "rcc/CodeGen/RegisterMask.h"
#include "rcc/CodeGen/TargetDescription.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t OperandOpcode = 1;

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

[[nodiscard]] rcc::OperandDescriptor operandDescriptor(rcc::MachineOperand::Kind Kind) {
  rcc::OperandDescriptor Descriptor;
  Descriptor.Kinds = {Kind};
  return Descriptor;
}

[[nodiscard]] rcc::Expected<rcc::TargetDescription> createTarget() {
  rcc::InstructionDescriptor Descriptor;
  Descriptor.InstructionOpcode = rcc::Opcode::fromValue(OperandOpcode);
  Descriptor.Name = "TEST_MIR_OPERANDS";
  Descriptor.Flags = {rcc::InstructionFlag::ProducesCode};
  Descriptor.Operands = {
      operandDescriptor(rcc::MachineOperand::Kind::IntegerImmediate),
      operandDescriptor(rcc::MachineOperand::Kind::LiveOutRegisterMask),
      operandDescriptor(rcc::MachineOperand::Kind::CFIIndex),
      operandDescriptor(rcc::MachineOperand::Kind::Intrinsic),
      operandDescriptor(rcc::MachineOperand::Kind::Predicate),
      operandDescriptor(rcc::MachineOperand::Kind::Predicate),
      operandDescriptor(rcc::MachineOperand::Kind::ShuffleMask),
      operandDescriptor(rcc::MachineOperand::Kind::DebugInstrRef),
      operandDescriptor(rcc::MachineOperand::Kind::LaneMask),
  };

  auto Instructions = rcc::InstructionSet::create({std::move(Descriptor)});
  if (!Instructions)
    return std::unexpected(Instructions.error());
  return rcc::test::createTestTargetDescription(std::move(*Instructions));
}

[[nodiscard]] std::unique_ptr<rcc::MachineFunction>
createFunction(TestContext &Context, std::uint64_t ID, std::string Name) {
  auto Function =
      rcc::MachineFunction::create(std::move(Name), rcc::MachineFunctionStableID::fromValue(ID));
  Context.expect(Function.has_value(), "machine function construction must succeed");
  if (!Function)
    return nullptr;
  return std::move(*Function);
}

[[nodiscard]] rcc::MachineBasicBlock *appendBlock(TestContext &Context,
                                                  rcc::MachineFunction &Function) {
  auto Block = Function.createBasicBlock();
  Context.expect(Block.has_value(), "machine basic block construction must succeed");
  if (!Block)
    return nullptr;
  auto AppendResult = Function.appendBlock(**Block);
  Context.expect(AppendResult.has_value(), "machine basic block insertion must succeed");
  if (!AppendResult)
    return nullptr;
  return *Block;
}

[[nodiscard]] rcc::Expected<rcc::MachineInstr *>
buildOperandInstruction(rcc::MachineFunction &Function, const rcc::TargetDescription &Target,
                        rcc::MachineBasicBlock &Block, rcc::CFIInstructionIndex CFIIndex,
                        std::shared_ptr<const rcc::RegisterMask> LiveOutMask) {
  const std::array<std::uint64_t, 2> Words{
      0x0123456789abcdefULL,
      0xfedcba9876543210ULL,
  };
  auto Integer = rcc::IntegerImmediate::create(128, Words);
  if (!Integer)
    return std::unexpected(Integer.error());

  rcc::MachineIRBuilder Builder(Function, Target);
  auto InsertResult = Builder.setInsertPoint(Block);
  if (!InsertResult)
    return std::unexpected(InsertResult.error());
  auto Pending = Builder.buildInstr(rcc::Opcode::fromValue(OperandOpcode));
  if (!Pending)
    return std::unexpected(Pending.error());
  if (auto Result = Pending->addIntegerImmediate(std::move(*Integer)); !Result)
    return std::unexpected(Result.error());
  if (auto Result = Pending->addLiveOutRegisterMask(std::move(LiveOutMask)); !Result)
    return std::unexpected(Result.error());
  if (auto Result = Pending->addCFIInstruction(CFIIndex); !Result)
    return std::unexpected(Result.error());
  if (auto Result = Pending->addIntrinsic(rcc::IntrinsicID::fromValue(7)); !Result)
    return std::unexpected(Result.error());
  if (auto Result = Pending->addIntegerPredicate(rcc::IntegerPredicate::SignedLessThan); !Result)
    return std::unexpected(Result.error());
  if (auto Result = Pending->addFloatingPredicate(rcc::FloatingPredicate::UnorderedGreaterEqual);
      !Result) {
    return std::unexpected(Result.error());
  }
  if (auto Result = Pending->addShuffleMask({1, rcc::ShuffleMaskOperand::Undef, 0}); !Result) {
    return std::unexpected(Result.error());
  }
  if (auto Result = Pending->addDebugInstrRef(9, 3); !Result)
    return std::unexpected(Result.error());
  if (auto Result = Pending->addLaneMask(rcc::LaneMask::fromRaw(0x8000000000000001ULL)); !Result) {
    return std::unexpected(Result.error());
  }
  return Pending->build();
}

void testIntegerImmediate(TestContext &Context) {
  const std::array<std::uint64_t, 2> Words{
      0x0123456789abcdefULL,
      0x0000000000000001ULL,
  };
  auto Integer = rcc::IntegerImmediate::create(65, Words);
  Context.expect(Integer.has_value() && Integer->bitWidth() == 65 && Integer->words().size() == 2 &&
                     Integer->words()[0] == Words[0] && Integer->words()[1] == Words[1] &&
                     Integer->isNegative() && !Integer->isZero(),
                 "arbitrary-width integer must preserve exact little-endian words");

  Context.expect(!rcc::IntegerImmediate::create(0, std::span<const std::uint64_t>{}) &&
                     !rcc::IntegerImmediate::create(rcc::IntegerImmediate::MaxBitWidth + 1,
                                                    std::span<const std::uint64_t>{}),
                 "integer immediate must reject unsupported bit widths");
  const std::array<std::uint64_t, 2> TwoWords{1, 0};
  const std::array<std::uint64_t, 1> HighBits{0x100};
  Context.expect(!rcc::IntegerImmediate::create(64, TwoWords) &&
                     !rcc::IntegerImmediate::create(8, HighBits),
                 "integer immediate must reject word-count and high-bit mismatches");

  auto UnsignedMaximum = rcc::IntegerImmediate::fromUnsigned(8, 255);
  auto UnsignedOverflow = rcc::IntegerImmediate::fromUnsigned(8, 256);
  auto SignedMinimum = rcc::IntegerImmediate::fromSigned(8, -128);
  auto SignedMaximum = rcc::IntegerImmediate::fromSigned(8, 127);
  auto SignedUnderflow = rcc::IntegerImmediate::fromSigned(8, -129);
  auto SignedOverflow = rcc::IntegerImmediate::fromSigned(8, 128);
  Context.expect(UnsignedMaximum && UnsignedMaximum->words()[0] == 255 && !UnsignedOverflow &&
                     SignedMinimum && SignedMinimum->words()[0] == 128 && SignedMaximum &&
                     SignedMaximum->words()[0] == 127 && !SignedUnderflow && !SignedOverflow,
                 "integer immediate scalar factories must enforce signedness and width");

  auto SignExtended = rcc::IntegerImmediate::fromSigned(128, -1);
  auto Zero = rcc::IntegerImmediate::fromUnsigned(128, 0);
  Context.expect(SignExtended && SignExtended->isNegative() && SignExtended->words().size() == 2 &&
                     SignExtended->words()[0] == std::numeric_limits<std::uint64_t>::max() &&
                     SignExtended->words()[1] == std::numeric_limits<std::uint64_t>::max() &&
                     Zero && Zero->isZero() && !Zero->isNegative(),
                 "wide scalar factories must sign-extend and zero-extend exactly");

  if (Integer) {
    auto Operand = rcc::MachineOperand::integerImmediate(std::move(*Integer), 5);
    const auto *Payload = Operand.getIf<rcc::IntegerImmediateOperand>();
    Context.expect(Operand.kind() == rcc::MachineOperand::Kind::IntegerImmediate &&
                       Operand.targetFlags() == 5 && Payload != nullptr &&
                       Payload->Value.bitWidth() == 65,
                   "typed integer operand must retain its distinct kind and target flags");
  }
}

void testOperandFactories(TestContext &Context) {
  auto Mask = rcc::test::createTestCallPreservedMask();
  auto LiveOut = rcc::MachineOperand::liveOutRegisterMask(Mask, 3);
  Context.expect(LiveOut && LiveOut->kind() == rcc::MachineOperand::Kind::LiveOutRegisterMask &&
                     LiveOut->targetFlags() == 3 &&
                     LiveOut->getIf<rcc::LiveOutRegisterMaskOperand>()->Mask == Mask &&
                     !rcc::MachineOperand::liveOutRegisterMask(nullptr),
                 "live-out mask must be distinct from a call-preserved register mask");

  auto CFI = rcc::MachineOperand::cfiIndex(rcc::CFIInstructionIndex::fromValue(0), 4);
  Context.expect(CFI.kind() == rcc::MachineOperand::Kind::CFIIndex &&
                     CFI.getIf<rcc::CFIIndexOperand>()->Index.value() == 0 &&
                     CFI.targetFlags() == 4,
                 "CFI operand must preserve index zero and target flags");

  auto Intrinsic = rcc::MachineOperand::intrinsic(rcc::IntrinsicID::fromValue(17), 6);
  Context.expect(Intrinsic && Intrinsic->kind() == rcc::MachineOperand::Kind::Intrinsic &&
                     Intrinsic->getIf<rcc::IntrinsicOperand>()->ID.value() == 17 &&
                     Intrinsic->targetFlags() == 6 &&
                     !rcc::MachineOperand::intrinsic(rcc::IntrinsicID{}),
                 "intrinsic operand must use a nonzero strong ID");

  auto IntegerPredicate =
      rcc::MachineOperand::integerPredicate(rcc::IntegerPredicate::UnsignedGreaterEqual);
  auto FloatingPredicate =
      rcc::MachineOperand::floatingPredicate(rcc::FloatingPredicate::OrderedNotEqual);
  Context.expect(
      IntegerPredicate && FloatingPredicate &&
          IntegerPredicate->getIf<rcc::PredicateOperand>()->integerPredicate() != nullptr &&
          FloatingPredicate->getIf<rcc::PredicateOperand>()->floatingPredicate() != nullptr &&
          !rcc::MachineOperand::integerPredicate(rcc::IntegerPredicate::Count) &&
          !rcc::MachineOperand::floatingPredicate(rcc::FloatingPredicate::Count),
      "integer and floating predicates must share one tagged operand without invalid sentinels");

  auto Shuffle = rcc::MachineOperand::shuffleMask({1, rcc::ShuffleMaskOperand::Undef, -2, 0}, 7);
  Context.expect(Shuffle && Shuffle->kind() == rcc::MachineOperand::Kind::ShuffleMask &&
                     Shuffle->getIf<rcc::ShuffleMaskOperand>()->Elements ==
                         std::vector<std::int32_t>({1, -1, -2, 0}) &&
                     Shuffle->targetFlags() == 7 && !rcc::MachineOperand::shuffleMask({0}),
                 "shuffle mask must preserve int32 elements and the canonical undef sentinel");

  auto Reference = rcc::MachineOperand::debugInstrRef(9, 1'000'000, 8);
  auto ZeroReference = rcc::MachineOperand::debugInstrRef(0, 0);
  Context.expect(
      Reference && Reference->kind() == rcc::MachineOperand::Kind::DebugInstrRef &&
          Reference->getIf<rcc::DebugInstrRefOperand>()->InstructionNumber == 9 &&
          Reference->getIf<rcc::DebugInstrRefOperand>()->OperandNumber == 1'000'000 &&
          Reference->targetFlags() == 8 && ZeroReference &&
          ZeroReference->getIf<rcc::DebugInstrRefOperand>()->InstructionNumber == 0 &&
          !rcc::MachineOperand::debugInstrRef(
              static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1, 0) &&
          !rcc::MachineOperand::debugInstrRef(
              1, static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1),
      "debug instruction reference must preserve zero and enforce its two uint32 domains");

  auto LaneMask = rcc::MachineOperand::laneMask(rcc::LaneMask::fromRaw(0xf00000000000000fULL), 9);
  Context.expect(LaneMask.kind() == rcc::MachineOperand::Kind::LaneMask &&
                     LaneMask.getIf<rcc::LaneMaskOperand>()->Mask.rawValue() ==
                         0xf00000000000000fULL &&
                     LaneMask.targetFlags() == 9,
                 "lane-mask operand must preserve all 64 bits");

  auto Clone = LaneMask.cloneDetached();
  Context.expect(Clone.parent() == nullptr && Clone.kind() == LaneMask.kind() &&
                     Clone.targetFlags() == LaneMask.targetFlags() &&
                     Clone.getIf<rcc::LaneMaskOperand>()->Mask ==
                         LaneMask.getIf<rcc::LaneMaskOperand>()->Mask,
                 "detached operand cloning must preserve the new payload kinds");
}

void testCFIInfo(TestContext &Context) {
  const auto R1 = rcc::DwarfRegisterNumber::fromValue(1);
  const auto R2 = rcc::DwarfRegisterNumber::fromValue(2);
  const auto R3 = rcc::DwarfRegisterNumber::fromValue(3);
  rcc::MachineCFIInfo Info;

  auto Add = [&Context, &Info](rcc::MachineCFIInstruction Instruction) {
    auto Index = Info.addInstruction(std::move(Instruction));
    Context.expect(Index.has_value(), "valid CFI instruction insertion must succeed");
  };
  auto AddExpected = [&Context, &Info](rcc::Expected<rcc::MachineCFIInstruction> Instruction) {
    Context.expect(Instruction.has_value(), "valid structured CFI factory must succeed");
    if (!Instruction)
      return;
    auto Index = Info.addInstruction(std::move(*Instruction));
    Context.expect(Index.has_value(), "valid structured CFI insertion must succeed");
  };

  Add(rcc::MachineCFIInstruction::sameValue(R1));
  Add(rcc::MachineCFIInstruction::rememberState());
  Add(rcc::MachineCFIInstruction::restoreState());
  Add(rcc::MachineCFIInstruction::offset(R1, -8));
  Add(rcc::MachineCFIInstruction::llvmDefAddressSpaceCFA(R1, 16, 5));
  Add(rcc::MachineCFIInstruction::defCFARegister(R2));
  Add(rcc::MachineCFIInstruction::defCFAOffset(32));
  Add(rcc::MachineCFIInstruction::defCFA(R2, 24));
  Add(rcc::MachineCFIInstruction::relativeOffset(R3, -16));
  Add(rcc::MachineCFIInstruction::adjustCFAOffset(8));
  AddExpected(rcc::MachineCFIInstruction::escape({0x0c, 0x07, 0x08}));
  Add(rcc::MachineCFIInstruction::restore(R1));
  Add(rcc::MachineCFIInstruction::undefined(R2));
  Add(rcc::MachineCFIInstruction::registerRule(R1, R3));
  Add(rcc::MachineCFIInstruction::windowSave());
  Add(rcc::MachineCFIInstruction::negateReturnAddressState());
  Add(rcc::MachineCFIInstruction::negateReturnAddressStateWithPC());
  Add(rcc::MachineCFIInstruction::gnuArgsSize(48));
  AddExpected(rcc::MachineCFIInstruction::label("cfi.label"));
  Add(rcc::MachineCFIInstruction::valueOffset(R3, 64));
  Add(rcc::MachineCFIInstruction::llvmRegisterPair({
      .Register = R1,
      .FirstRegister = R2,
      .FirstSizeInBits = 32,
      .SecondRegister = R3,
      .SecondSizeInBits = 32,
  }));
  AddExpected(rcc::MachineCFIInstruction::llvmVectorRegisters({
      .Register = R1,
      .VectorRegisters = {{R2, 0, 64}, {R3, 1, 64}},
  }));
  Add(rcc::MachineCFIInstruction::llvmVectorOffset({
      .Register = R1,
      .RegisterSizeInBits = 64,
      .MaskRegister = R2,
      .MaskRegisterSizeInBits = 8,
      .Offset = -32,
  }));
  Add(rcc::MachineCFIInstruction::llvmVectorRegisterMask({
      .Register = R1,
      .SpillRegister = R2,
      .SpillRegisterLaneSizeInBits = 64,
      .MaskRegister = R3,
      .MaskRegisterSizeInBits = 8,
  }));

  Context.expect(Info.instructions().size() == 24 && Info.verify().has_value() &&
                     Info.instruction(rcc::CFIInstructionIndex::fromValue(0))->operation() ==
                         rcc::MachineCFIOperation::SameValue &&
                     Info.instruction(rcc::CFIInstructionIndex::fromValue(23))->operation() ==
                         rcc::MachineCFIOperation::LLVMVectorRegisterMask &&
                     Info.instruction(rcc::CFIInstructionIndex::fromValue(24)) == nullptr,
                 "CFI table must preserve deterministic zero-based definition order");

  Context.expect(!rcc::MachineCFIInstruction::escape({}) &&
                     !rcc::MachineCFIInstruction::label("") &&
                     !rcc::MachineCFIInstruction::llvmVectorRegisters({
                         .Register = R1,
                         .VectorRegisters = {},
                     }),
                 "CFI factories must reject structurally incomplete payloads");

  auto ZeroWidthVectorRegisters = rcc::MachineCFIInstruction::llvmVectorRegisters({
      .Register = R1,
      .VectorRegisters = {{R2, 0, 0}},
  });
  Context.expect(
      rcc::MachineCFIInstruction::llvmRegisterPair({
                                                       .Register = R1,
                                                       .FirstRegister = R2,
                                                       .FirstSizeInBits = 0,
                                                       .SecondRegister = R3,
                                                       .SecondSizeInBits = 0,
                                                   })
              .verify() &&
          ZeroWidthVectorRegisters && ZeroWidthVectorRegisters->verify() &&
          rcc::MachineCFIInstruction::llvmVectorOffset({
                                                           .Register = R1,
                                                           .RegisterSizeInBits = 0,
                                                           .MaskRegister = R2,
                                                           .MaskRegisterSizeInBits = 0,
                                                           .Offset = 0,
                                                       })
              .verify() &&
          rcc::MachineCFIInstruction::llvmVectorRegisterMask({
                                                                 .Register = R1,
                                                                 .SpillRegister = R2,
                                                                 .SpillRegisterLaneSizeInBits = 0,
                                                                 .MaskRegister = R3,
                                                                 .MaskRegisterSizeInBits = 0,
                                                             })
              .verify(),
      "CFI unsigned size fields must preserve every value accepted by the fixed MIR grammar");

  auto Clone = Info.clone();
  Context.expect(Clone && *Clone == Info, "CFI information clone must preserve every rule");
  if (Clone) {
    auto Extra = Clone->addInstruction(rcc::MachineCFIInstruction::rememberState());
    Context.expect(Extra && Clone->instructions().size() == 25 && Info.instructions().size() == 24,
                   "CFI information clone must own independent storage");
  }
}

void testBuilderVerifierAndClone(TestContext &Context) {
  auto Target = createTarget();
  Context.expect(Target.has_value(), "operand test target construction must succeed");
  auto Function = createFunction(Context, 1, "operand-integration");
  if (!Target || !Function)
    return;
  Function->attributes().set(rcc::MachineFunctionAttribute::DebugInstrRef);
  auto CFIIndex = Function->cfiInfo().addInstruction(rcc::MachineCFIInstruction::defCFAOffset(16));
  Context.expect(CFIIndex.has_value(), "function CFI instruction insertion must succeed");
  rcc::MachineBasicBlock *Block = appendBlock(Context, *Function);
  if (!CFIIndex || Block == nullptr)
    return;

  auto Instruction = buildOperandInstruction(*Function, *Target, *Block, *CFIIndex,
                                             rcc::test::createTestCallPreservedMask());
  Context.expect(Instruction.has_value() &&
                     rcc::MachineVerifier::verify(*Function, *Target).has_value(),
                 "builder and verifier must accept every newly modeled operand kind");
  if (!Instruction)
    return;

  const auto Operands = (*Instruction)->operands();
  Context.expect(
      Operands.size() == 9 &&
          Operands[0].getIf<rcc::IntegerImmediateOperand>()->Value.bitWidth() == 128 &&
          Operands[1].kind() == rcc::MachineOperand::Kind::LiveOutRegisterMask &&
          Operands[2].getIf<rcc::CFIIndexOperand>()->Index == *CFIIndex &&
          Operands[3].getIf<rcc::IntrinsicOperand>()->ID.value() == 7 &&
          *Operands[4].getIf<rcc::PredicateOperand>()->integerPredicate() ==
              rcc::IntegerPredicate::SignedLessThan &&
          *Operands[5].getIf<rcc::PredicateOperand>()->floatingPredicate() ==
              rcc::FloatingPredicate::UnorderedGreaterEqual &&
          Operands[6].getIf<rcc::ShuffleMaskOperand>()->Elements[1] ==
              rcc::ShuffleMaskOperand::Undef &&
          Operands[7].getIf<rcc::DebugInstrRefOperand>()->InstructionNumber == 9 &&
          Operands[8].getIf<rcc::LaneMaskOperand>()->Mask.rawValue() == 0x8000000000000001ULL,
      "built instruction must expose all new payloads without string or immediate overloading");

  auto Clone =
      rcc::cloneMachineFunction(*Function, *Target,
                                {
                                    .NewStableID = rcc::MachineFunctionStableID::fromValue(2),
                                    .NewName = std::string("operand-integration.clone"),
                                });
  Context.expect(Clone.has_value(), "operand-rich function must support explicit deep cloning");
  if (Clone) {
    const rcc::MachineInstr *ClonedInstruction =
        Clone->ClonedFunction->firstBlock()->firstInstruction();
    Context.expect(
        Clone->ClonedFunction->cfiInfo() == Function->cfiInfo() && ClonedInstruction != nullptr &&
            ClonedInstruction->operands().size() == 9 &&
            ClonedInstruction->operands()[0].getIf<rcc::IntegerImmediateOperand>()->Value ==
                Operands[0].getIf<rcc::IntegerImmediateOperand>()->Value &&
            ClonedInstruction->operands()[6].getIf<rcc::ShuffleMaskOperand>()->Elements ==
                Operands[6].getIf<rcc::ShuffleMaskOperand>()->Elements,
        "deep clone must preserve CFI indices and owned variable-size operand data");
  }

  auto MissingCFIFunction = createFunction(Context, 3, "missing-cfi");
  if (!MissingCFIFunction)
    return;
  rcc::MachineBasicBlock *MissingCFIBlock = appendBlock(Context, *MissingCFIFunction);
  if (MissingCFIBlock == nullptr)
    return;
  auto MissingCFI = buildOperandInstruction(*MissingCFIFunction, *Target, *MissingCFIBlock,
                                            rcc::CFIInstructionIndex::fromValue(0),
                                            rcc::test::createTestCallPreservedMask());
  Context.expect(!MissingCFI, "builder must reject an operand that references an unknown CFI rule");

  auto InvalidMaskFunction = createFunction(Context, 4, "invalid-liveout");
  if (!InvalidMaskFunction)
    return;
  auto ValidCFI =
      InvalidMaskFunction->cfiInfo().addInstruction(rcc::MachineCFIInstruction::rememberState());
  rcc::MachineBasicBlock *InvalidMaskBlock = appendBlock(Context, *InvalidMaskFunction);
  if (!ValidCFI || InvalidMaskBlock == nullptr)
    return;
  auto InvalidMask = std::make_shared<const rcc::RegisterMask>(
      std::vector<std::uint64_t>{std::uint64_t{1} << 63U});
  auto InvalidLiveOut = buildOperandInstruction(*InvalidMaskFunction, *Target, *InvalidMaskBlock,
                                                *ValidCFI, std::move(InvalidMask));
  Context.expect(!InvalidLiveOut,
                 "target validation must reject a live-out mask with unknown physical registers");
}

} // namespace

int main() {
  TestContext Context;
  testIntegerImmediate(Context);
  testOperandFactories(Context);
  testCFIInfo(Context);
  testBuilderVerifierAndClone(Context);
  return Context.result();
}
