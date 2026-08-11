// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/MIR/MIParser.h"

#include "Target/TestTarget.h"
#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/MIR/MIPrinter.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t CopyOpcode = 1;
constexpr std::uint32_t AddImmediateOpcode = 2;
constexpr std::uint32_t BranchOpcode = 3;
constexpr std::uint32_t NopOpcode = 4;
constexpr std::uint32_t ConstantOpcode = 5;
constexpr std::uint32_t FloatingConstantOpcode = 6;
constexpr std::uint32_t MetadataOpcode = 7;
constexpr std::uint32_t LoadOpcode = 8;
constexpr std::uint32_t InlineAsmOpcode = 9;

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

[[nodiscard]] rcc::OperandDescriptor
registerOperand(rcc::RegisterOperandRole Role,
                rcc::RegisterClassID Class = rcc::test::TestRegisterClass::GPR64) {
  rcc::OperandDescriptor Descriptor;
  Descriptor.Kinds = {rcc::MachineOperand::Kind::Register};
  Descriptor.RegisterRole = Role;
  Descriptor.RegClass = Class;
  return Descriptor;
}

[[nodiscard]] rcc::InstructionDescriptor descriptor(std::uint32_t OpcodeValue, std::string Name) {
  rcc::InstructionDescriptor Descriptor;
  Descriptor.InstructionOpcode = rcc::Opcode::fromValue(OpcodeValue);
  Descriptor.Name = std::move(Name);
  Descriptor.Flags = {rcc::InstructionFlag::ProducesCode};
  return Descriptor;
}

[[nodiscard]] rcc::OperandDescriptor genericRegisterOperand(rcc::RegisterOperandRole Role) {
  rcc::OperandDescriptor Descriptor;
  Descriptor.Kinds = {rcc::MachineOperand::Kind::Register};
  Descriptor.RegisterRole = Role;
  return Descriptor;
}

[[nodiscard]] rcc::OperandDescriptor operand(rcc::MachineOperand::Kind Kind) {
  rcc::OperandDescriptor Descriptor;
  Descriptor.Kinds = {Kind};
  return Descriptor;
}

[[nodiscard]] rcc::Expected<rcc::TargetDescription> createTarget() {
  auto Copy = descriptor(CopyOpcode, "TEST_COPY");
  Copy.Operands = {
      registerOperand(rcc::RegisterOperandRole::Def),
      registerOperand(rcc::RegisterOperandRole::Use),
  };
  Copy.NumExplicitDefs = 1;

  auto AddImmediate = descriptor(AddImmediateOpcode, "TEST_ADDI");
  AddImmediate.Operands = {
      registerOperand(rcc::RegisterOperandRole::Def),
      registerOperand(rcc::RegisterOperandRole::Use),
  };
  rcc::OperandDescriptor Immediate;
  Immediate.Kinds = {rcc::MachineOperand::Kind::Immediate};
  Immediate.AllowedTargetFlags = 0xFU;
  AddImmediate.Operands.push_back(Immediate);
  AddImmediate.NumExplicitDefs = 1;

  auto Branch = descriptor(BranchOpcode, "TEST_BRANCH");
  rcc::OperandDescriptor Block;
  Block.Kinds = {rcc::MachineOperand::Kind::BasicBlock};
  Branch.Operands.push_back(Block);
  Branch.Flags.set(rcc::InstructionFlag::Branch);
  Branch.Flags.set(rcc::InstructionFlag::Terminator);
  Branch.Flags.set(rcc::InstructionFlag::Barrier);

  auto Nop = descriptor(NopOpcode, "TEST_NOP");

  auto Constant = descriptor(ConstantOpcode, "TEST_CONSTANT");
  Constant.Operands = {
      genericRegisterOperand(rcc::RegisterOperandRole::Def),
      operand(rcc::MachineOperand::Kind::IntegerImmediate),
  };
  Constant.NumExplicitDefs = 1;
  Constant.Flags.set(rcc::InstructionFlag::Generic);
  Constant.Flags.set(rcc::InstructionFlag::AllowedPreLegalizer);

  auto FloatingConstant = descriptor(FloatingConstantOpcode, "TEST_FCONSTANT");
  FloatingConstant.Operands = {
      genericRegisterOperand(rcc::RegisterOperandRole::Def),
      operand(rcc::MachineOperand::Kind::FloatingImmediate),
  };
  FloatingConstant.NumExplicitDefs = 1;
  FloatingConstant.Flags.set(rcc::InstructionFlag::Generic);
  FloatingConstant.Flags.set(rcc::InstructionFlag::AllowedPreLegalizer);

  auto Metadata = descriptor(MetadataOpcode, "TEST_METADATA");
  Metadata.Operands = {
      operand(rcc::MachineOperand::Kind::Metadata),
      operand(rcc::MachineOperand::Kind::Metadata),
      operand(rcc::MachineOperand::Kind::CFIIndex),
  };

  auto Load = descriptor(LoadOpcode, "TEST_LOAD");
  Load.Operands = {
      registerOperand(rcc::RegisterOperandRole::Def),
      registerOperand(rcc::RegisterOperandRole::Use),
  };
  Load.NumExplicitDefs = 1;
  Load.Flags.set(rcc::InstructionFlag::MayLoad);
  Load.RequiresMemoryOperands = true;

  auto InlineAsm = descriptor(InlineAsmOpcode, "INLINEASM");
  InlineAsm.Operands = {
      operand(rcc::MachineOperand::Kind::ExternalSymbol),
      operand(rcc::MachineOperand::Kind::Immediate),
  };

  std::vector<rcc::InstructionDescriptor> Descriptors;
  Descriptors.push_back(std::move(Copy));
  Descriptors.push_back(std::move(AddImmediate));
  Descriptors.push_back(std::move(Branch));
  Descriptors.push_back(std::move(Nop));
  Descriptors.push_back(std::move(Constant));
  Descriptors.push_back(std::move(FloatingConstant));
  Descriptors.push_back(std::move(Metadata));
  Descriptors.push_back(std::move(Load));
  Descriptors.push_back(std::move(InlineAsm));
  auto Instructions = rcc::InstructionSet::create(std::move(Descriptors));
  if (!Instructions)
    return std::unexpected(Instructions.error());
  return rcc::test::createTestTargetDescription(std::move(*Instructions));
}

[[nodiscard]] std::unique_ptr<rcc::MachineFunction> createFunction(TestContext &Context,
                                                                   std::uint64_t ID) {
  auto Function =
      rcc::MachineFunction::create("parser", rcc::MachineFunctionStableID::fromValue(ID));
  Context.expect(Function.has_value(), "machine function creation must succeed");
  if (!Function)
    return nullptr;
  return std::move(*Function);
}

void testTwoPassBodyParsing(TestContext &Context) {
  auto Target = createTarget();
  auto Function = createFunction(Context, 1);
  Context.expect(Target.has_value(), "parser target description must be valid");
  if (!Target)
    std::cerr << "error: " << Target.error().message() << '\n';
  if (!Target || !Function)
    return;
  auto MetadataNode =
      Function->metadata().addNode({.Slot = 100, .IsDistinct = false, .Elements = {}});
  Context.expect(MetadataNode.has_value(), "metadata fixture must be valid");
  if (!MetadataNode)
    return;

  const std::string Body = R"(
bb.0."entry block" (align 16, bb_id 7.1, call-frame-size 32):
  liveins: $g0
  successors: %bb.1."exit block"(2147483648)
  %0:gpr64 = TEST_COPY $g0
  %1:gpr64 = frame-setup TEST_ADDI %0, target-flags(test-direct-one, test-bit-four) 4
  %2:_(s128) = TEST_CONSTANT i128 340282366920938463463374607431768211455
  %3:_(f64) = TEST_FCONSTANT double f0x400921FB54442D18
  TEST_METADATA !100, !DIExpression(DW_OP_LLVM_arg, 0), def_cfa $g0, 16, pre-instr-symbol <mcsymbol before>, post-instr-symbol <mcsymbol after>, heap-alloc-marker !0, pcsections !0, mmra !0, cfi-type 17, deactivation-symbol @guard, debug-instr-number 9, debug-location !DILocation(line: 42, column: 7, scope: !0, inlinedAt: !DILocation(line: 8, scope: !0, inlinedAt: !0), isImplicitCode: true, atomGroup: 3, atomRank: 2)
  %4:gpr64 = TEST_LOAD %0 :: (load 8 from unknown-address, align 8, addrspace 0, !tbaa !0)
  TEST_BRANCH %bb.1."exit block"

bb.1."exit block" (landing-pad, bbsections Cold):
  $g1 = TEST_COPY %1
  TEST_NOP {
    TEST_NOP
  }
)";

  rcc::mir::MIParserContext ParserContext;
  ParserContext.Metadata.push_back(
      {.TextID = 0, .Reference = rcc::MachineMetadataReference::embeddedIR(0)});
  ParserContext.DefaultPointerSizeInBits = 64;
  rcc::mir::MIParser Parser(*Function, *Target, "body.mir", Body, ParserContext);
  const auto Parsed = Parser.parse();
  const bool ParseSucceeded =
      Parsed.has_value() && !Parser.failed() && Parser.diagnostics().empty();
  Context.expect(ParseSucceeded,
                 "valid MIR body must pass both parsing passes and final verification");
  if (!ParseSucceeded) {
    for (const auto &Diagnostic : Parser.diagnostics())
      std::cerr << Diagnostic.Category << ": " << Diagnostic.Message << '\n';
    if (!Parsed)
      std::cerr << "error: " << Parsed.error().message() << '\n';
  }
  if (!Parsed)
    return;

  Context.expect(Function->blockCount() == 2 && Function->allInstructions().size() == 10,
                 "parser must publish every declared block and instruction");
  rcc::MachineBasicBlock *Entry = Function->firstBlock();
  rcc::MachineBasicBlock *Exit = Entry == nullptr ? nullptr : Entry->next();
  Context.expect(Entry != nullptr && Exit != nullptr && !Exit->next(),
                 "parsed block layout must retain source order");
  if (Entry == nullptr || Exit == nullptr)
    return;

  Context.expect(Entry->attributes().name() == "entry block" &&
                     Entry->attributes().alignment().bytes() == 16 &&
                     Entry->attributes().profileID() == rcc::MachineBasicBlockProfileID{7, 1} &&
                     Entry->attributes().callFrameSize() == 32,
                 "first-pass block attributes must be retained");
  Context.expect(Exit->attributes().has(rcc::MachineBasicBlockAttribute::LandingPad) &&
                     Exit->attributes().sectionID() == rcc::MachineBasicBlockSectionID::cold(),
                 "named block flags and section IDs must be retained");
  Context.expect(Entry->liveIns().size() == 1 && Entry->successors().size() == 1 &&
                     Entry->successors().front().Block == Exit &&
                     Entry->successors().front().Probability.rawValue() == 2147483648U,
                 "live-ins and explicitly weighted successors must be published");

  const auto Registers = Function->registerInfo().virtualRegisters();
  Context.expect(Registers.size() == 5 &&
                     Registers[0].RegClass == rcc::test::TestRegisterClass::GPR64 &&
                     Registers[1].RegClass == rcc::test::TestRegisterClass::GPR64 &&
                     Registers[2].Type == *rcc::LLT::makeAnyScalar(128) &&
                     Registers[3].Type == *rcc::LLT::makeFloat(rcc::FloatSemantics::IEEEDouble) &&
                     Registers[4].RegClass == rcc::test::TestRegisterClass::GPR64,
                 "textual virtual registers must map deterministically to classed MachineIR vregs");

  const rcc::MachineInstr *Add = Entry->firstInstruction()->next();
  Context.expect(Add->flags().has(rcc::MachineInstrFlag::FrameSetup) &&
                     Add->operandAt(2)->targetFlags() == 0x5U,
                 "instruction and target operand flags must be preserved");

  const rcc::MachineInstr *Constant = Add->next();
  const auto *Integer = Constant->operandAt(1)->getIf<rcc::IntegerImmediateOperand>();
  const auto *Floating = Constant->next()->operandAt(1)->getIf<rcc::FloatingImmediateOperand>();
  Context.expect(Integer != nullptr && Integer->Value.bitWidth() == 128 &&
                     Integer->Value.words().size() == 2 &&
                     Integer->Value.words()[0] == std::numeric_limits<std::uint64_t>::max() &&
                     Integer->Value.words()[1] == std::numeric_limits<std::uint64_t>::max() &&
                     Floating != nullptr &&
                     Floating->Bits.semantics() == rcc::FloatSemantics::IEEEDouble &&
                     Floating->Bits.words()[0] == 0x400921FB54442D18ULL,
                 "typed integer and exact floating-point operands must retain their bit patterns");

  const rcc::MachineInstr *Metadata = Constant->next()->next();
  const auto *Reference = Metadata->operandAt(0)->getIf<rcc::MetadataOperand>();
  const auto *Expression = Metadata->operandAt(1)->getIf<rcc::MetadataOperand>();
  const auto *CFI = Metadata->operandAt(2)->getIf<rcc::CFIIndexOperand>();
  Context.expect(
      Reference != nullptr && Reference->Value.reference() != nullptr &&
          Reference->Value.reference()->scope() == rcc::MachineMetadataScope::MachineFunction &&
          Reference->Value.reference()->slot() == 100 && Expression != nullptr &&
          Expression->Value.expression() != nullptr &&
          Expression->Value.expression()->elements().size() == 2 &&
          Expression->Value.expression()->elements()[0] == 0x1005U && CFI != nullptr &&
          Function->cfiInfo().instruction(CFI->Index) != nullptr &&
          Function->cfiInfo().instruction(CFI->Index)->operation() ==
              rcc::MachineCFIOperation::DefCFA &&
          Metadata->preInstrSymbol() == "before" && Metadata->postInstrSymbol() == "after" &&
          Metadata->heapAllocMarker().has_value() && Metadata->pcSections().has_value() &&
          Metadata->mmra().has_value() && Metadata->cfiType() == 17 &&
          Metadata->deactivationSymbol() == "guard" && Metadata->debugInstrNumber() == 9 &&
          Metadata->debugLocation().has_value() &&
          Metadata->debugLocation()->inlineLocation() != nullptr &&
          Metadata->debugLocation()->inlineLocation()->inlineChain().size() == 2 &&
          Metadata->debugLocation()->inlineLocation()->inlineChain()[0].Line == 42 &&
          Metadata->debugLocation()->inlineLocation()->inlineChain()[0].Column == 7 &&
          Metadata->debugLocation()->inlineLocation()->inlineChain()[0].IsImplicitCode &&
          Metadata->debugLocation()->inlineLocation()->inlineChain()[0].AtomGroup == 3 &&
          Metadata->debugLocation()->inlineLocation()->inlineChain()[0].AtomRank == 2 &&
          Metadata->debugLocation()->inlineLocation()->terminalInlinedAt() ==
              rcc::MachineMetadataReference::embeddedIR(0),
      "metadata, debug expressions, and CFI operands must retain structured semantics");

  const rcc::MachineInstr *Load = Metadata->next();
  Context.expect(Load->memOperands().size() == 1 && Load->memOperands().front()->isLoad() &&
                     Load->memOperands().front()->description().Size ==
                         *rcc::MemorySize::fixed(8) &&
                     Load->memOperands().front()->description().Align.bytes() == 8 &&
                     Load->memOperands().front()->description().TBAA.has_value(),
                 "machine memory operands must retain access, size, alignment, and metadata");

  const rcc::MachineInstr *BundleHeader = Exit->firstInstruction()->next();
  const rcc::MachineInstr *BundleMember = BundleHeader->next();
  Context.expect(BundleHeader->flags().has(rcc::MachineInstrFlag::BundledWithSucc) &&
                     BundleMember->flags().has(rcc::MachineInstrFlag::BundledWithPred),
                 "instruction bundle braces must produce reciprocal layout flags");

  auto Printed = rcc::mir::printMachineFunctionBody(*Function, *Target, ParserContext);
  Context.expect(Printed.has_value(), "complete MIR body printing must accept the parsed function");
  if (!Printed) {
    std::cerr << "error: " << Printed.error().message() << '\n';
    return;
  }
  Context.expect(Printed->find("TEST_ADDI %0,") != std::string::npos,
                 "virtual-register uses must not repeat a definition constraint");

  auto RoundTripFunction = createFunction(Context, 2);
  if (!RoundTripFunction)
    return;
  auto RoundTripMetadata =
      RoundTripFunction->metadata().addNode({.Slot = 100, .IsDistinct = false, .Elements = {}});
  Context.expect(RoundTripMetadata.has_value(), "round-trip metadata fixture must be valid");
  if (!RoundTripMetadata)
    return;
  rcc::mir::MIParser RoundTripParser(*RoundTripFunction, *Target, "round-trip.mir", *Printed,
                                     ParserContext);
  const auto RoundTrip = RoundTripParser.parse();
  Context.expect(RoundTrip.has_value() && !RoundTripParser.failed(),
                 "the complete body printer output must parse successfully");
  if (!RoundTrip) {
    std::cerr << *Printed << '\n';
    for (const auto &Diagnostic : RoundTripParser.diagnostics())
      std::cerr << Diagnostic.Category << ": " << Diagnostic.Message << '\n';
    return;
  }
  auto PrintedAgain =
      rcc::mir::printMachineFunctionBody(*RoundTripFunction, *Target, ParserContext);
  Context.expect(PrintedAgain.has_value() && *PrintedAgain == *Printed,
                 "complete MIR body parse-print-parse must be deterministic");
}

void testAddressSpacePointerWidthResolver(TestContext &Context) {
  auto Target = createTarget();
  auto Function = createFunction(Context, 3);
  Context.expect(Target.has_value(), "pointer-width parser target must be valid");
  if (!Target || !Function)
    return;

  rcc::mir::MIParserContext ParserContext;
  ParserContext.PointerSizeInBits = [](std::uint32_t AddressSpace) -> rcc::Expected<std::uint32_t> {
    if (AddressSpace == 1)
      return 32;
    return rcc::makeUnexpected(rcc::ErrorCode::InvalidArgument,
                               "test data layout has no requested address space");
  };
  rcc::mir::MIParserOptions ParserOptions;
  ParserOptions.VerifyAfterParse = false;
  rcc::mir::MIParser Parser(*Function, *Target, "pointer-width.mir",
                            "bb.0:\n  %0:_(p1) = TEST_COPY $g0\n", ParserContext, ParserOptions);
  auto Parsed = Parser.parse();
  const auto Registers = Function->registerInfo().virtualRegisters();
  auto ExpectedType = rcc::LLT::makePointer(1, 32);
  Context.expect(Parsed.has_value() && ExpectedType.has_value() && Registers.size() == 1 &&
                     Registers.front().Type == *ExpectedType,
                 "MI parser must use the address-space-specific pointer-width resolver");
  if (!Parsed) {
    for (const rcc::TextDiagnostic &Diagnostic : Parser.diagnostics())
      std::cerr << Diagnostic.Category << ": " << Diagnostic.Message << '\n';
    std::cerr << "error: " << Parsed.error().message() << '\n';
  }
}

void testUnserializableLowLevelType(TestContext &Context) {
  auto Target = createTarget();
  auto Function = createFunction(Context, 4);
  Context.expect(Target.has_value(), "unserializable-type target must be valid");
  if (!Target || !Function)
    return;

  rcc::mir::MIParserOptions ParserOptions;
  ParserOptions.VerifyAfterParse = false;
  rcc::mir::MIParser Parser(*Function, *Target, "unserializable-type.mir",
                            "bb.0:\n  %0:gpr64(f64) = TEST_COPY $g0\n", {}, ParserOptions);
  auto Parsed = Parser.parse();
  auto Register = rcc::Register::makeVirtual(0);
  auto UnsupportedType = rcc::LLT::makeFloat(rcc::FloatSemantics::Float8E5M2);
  Context.expect(Parsed.has_value() && Register.has_value() && UnsupportedType.has_value(),
                 "unserializable-type test setup must parse a typed virtual register");
  if (!Parsed || !Register || !UnsupportedType)
    return;
  auto SetType = Function->registerInfo().setType(*Register, *UnsupportedType);
  Context.expect(SetType.has_value(), "test setup must assign an extended floating low-level type");
  if (!SetType)
    return;

  auto Printed = rcc::mir::printMachineFunctionBody(*Function, *Target);
  Context.expect(!Printed && Printed.error().message().find("cannot spell this floating") !=
                                 std::string_view::npos,
                 "printer must reject extended low-level types absent from fixed LLVM MIR grammar");
}

void expectParseError(TestContext &Context, const rcc::TargetDescription &Target, std::string Body,
                      std::string_view Message);

void testCompatibleRegisterClassAndBankAnnotations(TestContext &Context) {
  auto Target = createTarget();
  auto Function = createFunction(Context, 5);
  Context.expect(Target.has_value(), "register-constraint target must be valid");
  if (!Target || !Function)
    return;

  rcc::mir::MIParser Parser(*Function, *Target, "compatible-register-constraints.mir",
                            "bb.0:\n  %0:gpr64 = TEST_COPY $g0\n  $g1 = TEST_COPY %0:gpr\n");
  auto Parsed = Parser.parse();
  const auto Registers = Function->registerInfo().virtualRegisters();
  Context.expect(Parsed.has_value() && Registers.size() == 1 &&
                     Registers.front().RegClass == rcc::test::TestRegisterClass::GPR64 &&
                     Registers.front().Bank == rcc::test::TestRegisterBank::GPR,
                 "compatible class and bank annotations must coexist on a virtual register");
  if (!Parsed) {
    for (const rcc::TextDiagnostic &Diagnostic : Parser.diagnostics())
      std::cerr << Diagnostic.Category << ": " << Diagnostic.Message << '\n';
    std::cerr << "error: " << Parsed.error().message() << '\n';
  }

  expectParseError(Context, *Target,
                   "bb.0:\n  %0:gpr64 = TEST_COPY $g0\n  $g1 = TEST_COPY %0:fpr\n",
                   "conflicting virtual register bank");
}

void testTypedIntegerTruncation(TestContext &Context) {
  auto Target = createTarget();
  auto Function = createFunction(Context, 12);
  Context.expect(Target.has_value(), "typed-integer target description must be valid");
  if (!Target || !Function)
    return;

  const std::string Body = R"(bb.0:
  %0:_(s64) = TEST_CONSTANT i64 -1
  %1:_(s16) = TEST_CONSTANT i16 -64769
  %2:_(s4) = TEST_CONSTANT i4 -8
  %3:_(s64) = TEST_CONSTANT i64 18446744073709551617
)";
  rcc::mir::MIParser Parser(*Function, *Target, "typed-integer-truncation.mir", Body);
  const auto Parsed = Parser.parse();
  const auto Instructions = Function->allInstructions();
  const auto *MinusOne = Instructions.size() > 0
                             ? Instructions[0]->operandAt(1)->getIf<rcc::IntegerImmediateOperand>()
                             : nullptr;
  const auto *NarrowNegative =
      Instructions.size() > 1
          ? Instructions[1]->operandAt(1)->getIf<rcc::IntegerImmediateOperand>()
          : nullptr;
  const auto *NarrowMinimum =
      Instructions.size() > 2
          ? Instructions[2]->operandAt(1)->getIf<rcc::IntegerImmediateOperand>()
          : nullptr;
  const auto *WidePositive =
      Instructions.size() > 3
          ? Instructions[3]->operandAt(1)->getIf<rcc::IntegerImmediateOperand>()
          : nullptr;
  Context.expect(Parsed.has_value() && MinusOne != nullptr &&
                     MinusOne->Value.words()[0] == std::numeric_limits<std::uint64_t>::max() &&
                     NarrowNegative != nullptr && NarrowNegative->Value.bitWidth() == 16 &&
                     NarrowNegative->Value.words()[0] == 767 && NarrowMinimum != nullptr &&
                     NarrowMinimum->Value.bitWidth() == 4 && NarrowMinimum->Value.words()[0] == 8 &&
                     WidePositive != nullptr && WidePositive->Value.words()[0] == 1,
                 "typed integer literals must use LLVM width truncation semantics");
  if (!Parsed) {
    for (const auto &Diagnostic : Parser.diagnostics())
      std::cerr << Diagnostic.Category << ": " << Diagnostic.Message << '\n';
    std::cerr << "error: " << Parsed.error().message() << '\n';
  }
}

void testLLVMInlineAsmAndConstantMemorySyntax(TestContext &Context) {
  auto Target = createTarget();
  auto Function = createFunction(Context, 11);
  Context.expect(Target.has_value(), "LLVM MIR syntax target description must be valid");
  if (!Target || !Function)
    return;

  const std::string Body = R"(bb.0:
  INLINEASM &"", sideeffect mayload inteldialect
  %0:gpr64 = TEST_LOAD $g0 :: (load (s64) from `ptr poison`)
)";
  rcc::mir::MIParserContext ParserContext;
  ParserContext.DefaultPointerSizeInBits = 64;
  rcc::mir::MIParser Parser(*Function, *Target, "llvm-compatible.mir", Body, ParserContext);
  auto Parsed = Parser.parse();
  Context.expect(Parsed.has_value(), "LLVM inline-asm and constant-pointer MMO syntax must parse");
  if (!Parsed) {
    for (const auto &Diagnostic : Parser.diagnostics())
      std::cerr << Diagnostic.Category << ": " << Diagnostic.Message << '\n';
    return;
  }

  const rcc::MachineInstr *InlineAsm = Function->firstBlock()->firstInstruction();
  const auto *ExtraInfo = InlineAsm->operandAt(1)->getIf<std::int64_t>();
  const rcc::MachineInstr *Load = InlineAsm->next();
  const rcc::MachineMemOperandDesc &Memory = Load->memOperands().front()->description();
  const auto MemoryType = rcc::LLT::makeAnyScalar(64);
  Context.expect(ExtraInfo != nullptr && *ExtraInfo == 13,
                 "inline-asm extra-info names must aggregate into the LLVM bit field");
  Context.expect(Memory.Base.kind() == rcc::MemoryBase::Kind::IRConstant &&
                     Memory.Base.constantValue() != nullptr &&
                     Memory.Base.constantValue()->Kind == rcc::GenericConstantKind::Poison &&
                     MemoryType && Memory.MemoryType == *MemoryType,
                 "quoted LLVM pointer constants must remain structured MMO bases");

  auto Printed = rcc::mir::printMachineFunctionBody(*Function, *Target, ParserContext);
  Context.expect(Printed &&
                     Printed->find("INLINEASM &\"\", sideeffect mayload inteldialect") !=
                         std::string::npos &&
                     Printed->find("from `ptr poison`") != std::string::npos,
                 "canonical MIR must print symbolic inline-asm flags and LLVM constant bases");
}

void expectParseError(TestContext &Context, const rcc::TargetDescription &Target, std::string Body,
                      std::string_view Message) {
  auto Function = createFunction(Context, 10);
  if (!Function)
    return;
  rcc::mir::MIParser Parser(*Function, Target, "invalid.mir", std::move(Body));
  const auto Parsed = Parser.parse();
  const bool IsExpectedDiagnostic =
      !Parsed && Parser.failed() && Parser.diagnostics().size() == 1 &&
      Parser.diagnostics().front().Category == "mir-parser" &&
      Parser.diagnostics().front().Message.find(Message) != std::string::npos;
  Context.expect(IsExpectedDiagnostic,
                 "invalid MIR body must produce one stable parser diagnostic");
  if (!IsExpectedDiagnostic) {
    for (const auto &Diagnostic : Parser.diagnostics())
      std::cerr << Diagnostic.Category << ": " << Diagnostic.Message << '\n';
    if (!Parsed)
      std::cerr << "error: " << Parsed.error().message() << '\n';
  }
}

void testDiagnostics(TestContext &Context) {
  auto Target = createTarget();
  Context.expect(Target.has_value(), "diagnostic target description must be valid");
  if (!Target)
    std::cerr << "error: " << Target.error().message() << '\n';
  if (!Target)
    return;

  expectParseError(Context, *Target, "bb.0:\n  TEST_BRANCH %bb.9\n",
                   "undefined machine basic block");
  expectParseError(Context, *Target, "bb.0:\n  TEST_NOP\nbb.0:\n  TEST_NOP\n",
                   "redefinition of machine basic block");
  expectParseError(Context, *Target,
                   "bb.0:\n  %0:gpr64 = TEST_ADDI $g0, "
                   "target-flags(no-such-flag) 1\n",
                   "unknown target operand flag");
  expectParseError(Context, *Target, "  bb.0:\n  TEST_NOP\n",
                   "basic block definition must be at the start of a line");
}

} // namespace

int main() {
  TestContext Context;
  testTwoPassBodyParsing(Context);
  testAddressSpacePointerWidthResolver(Context);
  testUnserializableLowLevelType(Context);
  testCompatibleRegisterClassAndBankAnnotations(Context);
  testTypedIntegerTruncation(Context);
  testLLVMInlineAsmAndConstantMemorySyntax(Context);
  testDiagnostics(Context);
  return Context.result();
}
