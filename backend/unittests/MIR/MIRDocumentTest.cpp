// SPDX-License-Identifier: GPL-3.0-or-later

#include "Target/TestTarget.h"
#include "rcc/CodeGen/CompilationUnit.h"
#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/MIRParser/MIRParser.h"
#include "rcc/CodeGen/MIRPrinter.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineCompilationStatus.h"
#include "rcc/CodeGen/MachineConstantPool.h"
#include "rcc/CodeGen/MachineFrameInfo.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineJumpTableInfo.h"
#include "rcc/CodeGen/MachineMetadata.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/TargetConstant.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/CodeGen/TargetFunctionState.h"
#include "rcc/CodeGen/TargetMIRTextInfo.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t CopyOpcode = 1;
constexpr std::uint32_t BranchOpcode = 2;
constexpr std::uint32_t LoadOpcode = 3;
constexpr std::uint32_t GlobalOpcode = 4;

class DocumentTargetState final {
public:
  DocumentTargetState(std::uint32_t FrameBase, std::string Mode)
      : FrameBase(FrameBase), Mode(std::move(Mode)) {}

  [[nodiscard]] static constexpr rcc::TargetFunctionStateTypeID stableTypeID() noexcept {
    return rcc::TargetFunctionStateTypeID::fromValue(0x5243434d49525354ULL);
  }
  [[nodiscard]] static constexpr std::uint32_t schemaVersion() noexcept {
    return 1;
  }
  [[nodiscard]] rcc::Expected<void> verify() const {
    if (FrameBase == 0 || Mode.empty())
      return rcc::makeUnexpected(rcc::ErrorCode::VerificationError,
                                 "document target state is incomplete");
    return {};
  }
  [[nodiscard]] rcc::Expected<DocumentTargetState>
  clone(const rcc::MachineFunctionCloneContext &) const {
    return DocumentTargetState(FrameBase, Mode);
  }
  [[nodiscard]] std::string print() const {
    return Mode + ":" + std::to_string(FrameBase);
  }
  [[nodiscard]] rcc::Expected<std::vector<std::byte>> encode() const {
    std::vector<std::byte> Bytes(4);
    for (std::size_t Index = 0; Index < Bytes.size(); ++Index)
      Bytes[Index] = static_cast<std::byte>((FrameBase >> (Index * 8U)) & 0xffU);
    return Bytes;
  }
  [[nodiscard]] static rcc::Expected<DocumentTargetState> decode(std::uint32_t Version,
                                                                 std::span<const std::byte> Bytes) {
    if (Version != schemaVersion() || Bytes.size() != 4)
      return rcc::makeUnexpected(rcc::ErrorCode::ParseError,
                                 "invalid document target state encoding");
    std::uint32_t Value = 0;
    for (std::size_t Index = 0; Index < Bytes.size(); ++Index)
      Value |= std::to_integer<std::uint32_t>(Bytes[Index]) << (Index * 8U);
    return DocumentTargetState(Value, "test");
  }

  [[nodiscard]] std::uint32_t frameBase() const noexcept {
    return FrameBase;
  }
  [[nodiscard]] std::string_view mode() const noexcept {
    return Mode;
  }

  friend bool operator==(const DocumentTargetState &Left,
                         const DocumentTargetState &Right) = default;

private:
  std::uint32_t FrameBase = 0;
  std::string Mode;
};

class DocumentTargetConstant final {
public:
  explicit DocumentTargetConstant(std::uint64_t Value) : Value(Value) {}

  [[nodiscard]] static constexpr rcc::TargetConstantTypeID stableTypeID() noexcept {
    return rcc::TargetConstantTypeID::fromValue(0x5243434d49524354ULL);
  }
  [[nodiscard]] static constexpr std::uint32_t schemaVersion() noexcept {
    return 1;
  }
  [[nodiscard]] rcc::Expected<void> verify() const {
    return {};
  }
  [[nodiscard]] rcc::Expected<DocumentTargetConstant> clone() const {
    return DocumentTargetConstant(Value);
  }
  [[nodiscard]] std::uint64_t stableHash() const noexcept {
    return Value;
  }
  [[nodiscard]] std::string print() const {
    return "test-constant(" + std::to_string(Value) + ")";
  }
  [[nodiscard]] rcc::Expected<std::vector<std::byte>> encode() const {
    std::vector<std::byte> Bytes(8);
    for (std::size_t Index = 0; Index < Bytes.size(); ++Index)
      Bytes[Index] = static_cast<std::byte>((Value >> (Index * 8U)) & 0xffU);
    return Bytes;
  }
  [[nodiscard]] rcc::Expected<rcc::TargetConstantEmission> emit() const {
    auto Bytes = encode();
    if (!Bytes)
      return std::unexpected(Bytes.error());
    return rcc::TargetConstantEmission{.Bytes = std::move(*Bytes), .Fixups = {}};
  }
  [[nodiscard]] rcc::Alignment requiredAlignment() const {
    return *rcc::Alignment::create(8);
  }
  [[nodiscard]] std::optional<rcc::TargetRelocationKindID> relocationKind() const {
    return std::nullopt;
  }
  [[nodiscard]] static rcc::Expected<DocumentTargetConstant> parse(std::string_view Text) {
    constexpr std::string_view Prefix = "test-constant(";
    if (!Text.starts_with(Prefix) || !Text.ends_with(')'))
      return rcc::makeUnexpected(rcc::ErrorCode::ParseError, "invalid document target constant");
    Text.remove_prefix(Prefix.size());
    Text.remove_suffix(1);
    std::uint64_t Parsed = 0;
    const auto [End, Error] = std::from_chars(Text.data(), Text.data() + Text.size(), Parsed, 10);
    if (Error != std::errc{} || End != Text.data() + Text.size())
      return rcc::makeUnexpected(rcc::ErrorCode::ParseError,
                                 "invalid document target constant value");
    return DocumentTargetConstant(Parsed);
  }
  [[nodiscard]] static rcc::Expected<DocumentTargetConstant>
  decode(std::uint32_t Version, std::span<const std::byte> Bytes) {
    if (Version != schemaVersion() || Bytes.size() != 8)
      return rcc::makeUnexpected(rcc::ErrorCode::ParseError,
                                 "invalid document target constant encoding");
    std::uint64_t Parsed = 0;
    for (std::size_t Index = 0; Index < Bytes.size(); ++Index)
      Parsed |= std::to_integer<std::uint64_t>(Bytes[Index]) << (Index * 8U);
    return DocumentTargetConstant(Parsed);
  }

  [[nodiscard]] std::uint64_t value() const noexcept {
    return Value;
  }

  friend bool operator==(const DocumentTargetConstant &Left,
                         const DocumentTargetConstant &Right) = default;

private:
  std::uint64_t Value = 0;
};

[[nodiscard]] rcc::Expected<rcc::TargetFunctionState>
parseTargetState(const rcc::MIRTargetNode &Node) {
  const rcc::MIRTargetNode *FrameBase = Node.mappingValue("frame-base");
  const rcc::MIRTargetNode *Mode = Node.mappingValue("mode");
  if (FrameBase == nullptr || Mode == nullptr ||
      FrameBase->Kind != rcc::MIRTargetNodeKind::Scalar ||
      Mode->Kind != rcc::MIRTargetNodeKind::Scalar) {
    return rcc::makeUnexpected(rcc::ErrorCode::ParseError,
                               "document target state requires frame-base and mode");
  }
  std::uint32_t Value = 0;
  const auto [End, Error] = std::from_chars(
      FrameBase->Scalar.data(), FrameBase->Scalar.data() + FrameBase->Scalar.size(), Value, 10);
  if (Error != std::errc{} || End != FrameBase->Scalar.data() + FrameBase->Scalar.size())
    return rcc::makeUnexpected(rcc::ErrorCode::ParseError, "document target frame-base is invalid");
  return rcc::TargetFunctionState::create(DocumentTargetState(Value, Mode->Scalar));
}

[[nodiscard]] rcc::Expected<rcc::MIRTargetNode>
printTargetState(const rcc::TargetFunctionState &State) {
  const auto *Value = State.getIf<DocumentTargetState>();
  if (Value == nullptr)
    return rcc::makeUnexpected(rcc::ErrorCode::InvalidArgument, "wrong document target state type");
  std::vector<rcc::MIRTargetMappingEntry> Entries;
  Entries.push_back({"frame-base", rcc::MIRTargetNode::scalar(std::to_string(Value->frameBase()))});
  Entries.push_back({"mode", rcc::MIRTargetNode::scalar(std::string(Value->mode()))});
  return rcc::MIRTargetNode::mapping(std::move(Entries));
}

[[nodiscard]] rcc::Expected<rcc::TargetConstant> parseTargetConstant(std::string_view Text) {
  return rcc::TargetConstant::parse<DocumentTargetConstant>(Text);
}

[[nodiscard]] rcc::Expected<std::string> printTargetConstant(const rcc::TargetConstant &Constant) {
  if (Constant.getIf<DocumentTargetConstant>() == nullptr)
    return rcc::makeUnexpected(rcc::ErrorCode::InvalidArgument,
                               "wrong document target constant type");
  return Constant.print();
}

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

[[nodiscard]] rcc::OperandDescriptor registerOperand(rcc::RegisterOperandRole Role) {
  rcc::OperandDescriptor Operand;
  Operand.Kinds = {rcc::MachineOperand::Kind::Register};
  Operand.RegisterRole = Role;
  Operand.RegClass = rcc::test::TestRegisterClass::GPR64;
  return Operand;
}

[[nodiscard]] rcc::Expected<rcc::TargetDescription> createTarget() {
  rcc::InstructionDescriptor Copy;
  Copy.InstructionOpcode = rcc::Opcode::fromValue(CopyOpcode);
  Copy.Name = "TEST_COPY";
  Copy.Operands = {
      registerOperand(rcc::RegisterOperandRole::Def),
      registerOperand(rcc::RegisterOperandRole::Use),
  };
  Copy.NumExplicitDefs = 1;
  Copy.Flags.set(rcc::InstructionFlag::ProducesCode);

  rcc::InstructionDescriptor Branch;
  Branch.InstructionOpcode = rcc::Opcode::fromValue(BranchOpcode);
  Branch.Name = "TEST_BRANCH";
  rcc::OperandDescriptor Block;
  Block.Kinds = {rcc::MachineOperand::Kind::BasicBlock};
  Branch.Operands.push_back(Block);
  Branch.Flags = {
      rcc::InstructionFlag::ProducesCode,
      rcc::InstructionFlag::Branch,
      rcc::InstructionFlag::Terminator,
      rcc::InstructionFlag::Barrier,
  };

  rcc::InstructionDescriptor Load;
  Load.InstructionOpcode = rcc::Opcode::fromValue(LoadOpcode);
  Load.Name = "TEST_LOAD";
  Load.Operands = {
      registerOperand(rcc::RegisterOperandRole::Def),
      registerOperand(rcc::RegisterOperandRole::Use),
  };
  Load.NumExplicitDefs = 1;
  Load.Flags = {
      rcc::InstructionFlag::ProducesCode,
      rcc::InstructionFlag::MayLoad,
  };
  Load.RequiresMemoryOperands = true;

  rcc::InstructionDescriptor Global;
  Global.InstructionOpcode = rcc::Opcode::fromValue(GlobalOpcode);
  Global.Name = "TEST_GLOBAL";
  rcc::OperandDescriptor GlobalOperand;
  GlobalOperand.Kinds = {
      rcc::MachineOperand::Kind::GlobalSymbol,
      rcc::MachineOperand::Kind::ExternalSymbol,
  };
  Global.Operands.push_back(GlobalOperand);
  Global.Flags.set(rcc::InstructionFlag::ProducesCode);

  std::vector<rcc::InstructionDescriptor> Descriptors;
  Descriptors.push_back(std::move(Copy));
  Descriptors.push_back(std::move(Branch));
  Descriptors.push_back(std::move(Load));
  Descriptors.push_back(std::move(Global));
  auto Instructions = rcc::InstructionSet::create(std::move(Descriptors));
  if (!Instructions)
    return std::unexpected(Instructions.error());
  auto Registers = rcc::test::createTestTargetRegisterInfo();
  if (!Registers)
    return std::unexpected(Registers.error());
  rcc::TargetMIRTextInfoDesc MIRText = rcc::test::createTestTargetMIRTextInfoDescription();
  MIRText.FunctionStateType = DocumentTargetState::stableTypeID();
  MIRText.ParseFunctionState = parseTargetState;
  MIRText.PrintFunctionState = printTargetState;
  MIRText.ParseTargetConstant = parseTargetConstant;
  MIRText.PrintTargetConstant = printTargetConstant;
  return rcc::TargetDescription::create(std::move(*Instructions), std::move(*Registers),
                                        std::move(MIRText));
}

constexpr std::string_view ValidDocument = R"(--- |
  target datalayout = "e-p:64:64"
  target triple = "test64-unknown-linux-gnu"

  %pair = type { i8, i64 }
  @ordinary = external global i8

  define void @documented() {
  entry:
    %slot = alloca i64, align 8
    ret void
  }

  !0 = !DILocalVariable(name: "slot", scope: !1)
  !1 = distinct !DISubprogram(name: "documented")
  !2 = !DILocation(line: 4, column: 2, scope: !1)
---
name: documented
alignment: 16
exposesReturnsTwice: true
tracksRegLiveness: true
noPhis: true
isSSA: true
noVRegs: false
registers:
  - { id: 7, class: gpr64, preferred-register: '$g1', flags: [ test-vreg-one ] }
  - { id: 8, class: gpr64, preferred-register: '' }
liveins:
  - { reg: '$g0', virtual-reg: '%7' }
  - { reg: '$g1', virtual-reg: '' }
calleeSavedRegisters: [ '$g1' ]
frameInfo:
  isFrameAddressTaken: true
  stackSize: 32
  maxAlignment: 16
  adjustsStack: true
  hasCalls: true
  framePointerPolicy: all
  stackProtector: '%stack.4'
  maxCallFrameSize: 24
  localFrameSize: 8
  savePoint:
    - { point: '%bb.0', registers: [ '$g1' ] }
fixedStack:
  - { id: 3, type: spill-slot, offset: -16, size: 8, alignment: 8 }
stack:
  - { id: 4, name: slot, size: 8, alignment: 8, callee-saved-register: '', local-offset: 0,
      debug-info-variable: '!0',
      debug-info-expression: '!DIExpression(DW_OP_LLVM_arg, 0, 18446744073709551615)',
      debug-info-location: '!DILocation(line: 4, column: 2, scope: !1, isImplicitCode: true)' }
  - { id: 5, name: '', size: 4, alignment: 4 }
entry_values:
  - { entry-value-register: '$g0', debug-info-variable: '!0',
      debug-info-expression: '!DIExpression(DW_OP_stack_value)', debug-info-location: '!2' }
constants:
  - id: 5
    value: 'i64 18446744073709551615'
    alignment: 8
  - id: 9
    value: 'test-constant(1234)'
    alignment: 8
    isTargetSpecific: true
  - id: 11
    value: '%pair { i8 1, i64 2 }'
machineFunctionInfo: { frame-base: 64, mode: test }
jumpTable:
  kind: block-address
  entries:
    - { id: 6, blocks: [ '%bb.0' ] }
machineMetadataNodes:
  - '!10 = distinct !{!0, !"machine", null}'
body: |
  bb.0.entry (%ir-block.entry):
    liveins: $g0, $g1
    %7:gpr64 = TEST_LOAD $g0 :: (load syncscope("agent") acquire 8 from %ir.slot, align 8)
    TEST_GLOBAL @ordinary
    TEST_BRANCH %bb.0.entry
)";

constexpr std::string_view HeterogeneousPointerDocument = R"(--- |
  target datalayout = "e-p:64:64-p1:32:32"
  target triple = "test64-unknown-linux-gnu"

  define void @pointer_layout() {
    ret void
  }
---
name: pointer_layout
constants:
  - id: 0
    value: 'ptr addrspace(1) null'
body: |
  bb.0:
    %0:gpr64(p1) = TEST_COPY $g0
)";

void printDiagnostics(const rcc::mir::MIRDocumentParseResult &Parsed) {
  for (const rcc::TextDiagnostic &Diagnostic : Parsed.diagnostics())
    std::cerr << Diagnostic.Category << ": " << Diagnostic.Message << '\n';
}

void testDocumentTransaction(TestContext &Context) {
  auto Target = createTarget();
  Context.expect(Target.has_value(), "MIR document target must be valid");
  if (!Target)
    return;

  auto Parsed = rcc::mir::parseMIRDocument(*Target, "document.mir", std::string(ValidDocument));
  Context.expect(Parsed.succeeded(), "complete MIR document must parse successfully");
  if (!Parsed.succeeded()) {
    printDiagnostics(Parsed);
    return;
  }

  const rcc::mir::MIRDocument *Document = Parsed.document();
  Context.expect(
      Document != nullptr && Document->embeddedIR() != nullptr &&
          Document->unit().functionCount() == 1 && Document->functionDocumentInfo().size() == 1 &&
          Document->syncScopes().size() == 1 && Document->syncScopes().front().Name == "agent",
      "successful parsing must publish the complete document atomically");
  if (Document == nullptr)
    return;
  const rcc::MachineFunction *Function = Document->unit().function("documented");
  Context.expect(Function != nullptr, "parsed function must be queryable by name");
  if (Function == nullptr)
    return;

  Context.expect(Function->registerInfo().virtualRegisters().size() == 2 &&
                     Function->registerInfo().liveIns().size() == 2 &&
                     Function->records().hasCalleeSavedRegisters(),
                 "register tables must publish into the common MachineIR model");
  Context.expect(Function->frameInfo().liveObjectCount() == 3 &&
                     Function->frameInfo().state().StackSize == 32 &&
                     Function->frameInfo().state().SavePoints.size() == 1,
                 "frame records and stack objects must publish together");
  const rcc::StackObjectDesc *StackDescription =
      Function->frameInfo().object(rcc::FrameIndex::fromValue(1))->description();
  Context.expect(StackDescription != nullptr && StackDescription->DebugInfo.has_value() &&
                     StackDescription->DebugInfo->Expression.has_value() &&
                     StackDescription->DebugInfo->Expression->expression() != nullptr &&
                     StackDescription->DebugInfo->Expression->expression()->elements().size() ==
                         3 &&
                     StackDescription->DebugInfo->Location.has_value() &&
                     StackDescription->DebugInfo->Location->inlineLocation() != nullptr &&
                     Function->records().entryValues().size() == 1,
                 "stack and entry-value debug metadata must retain structured values");
  const auto *TargetState = Function->getTargetInfo<DocumentTargetState>();
  const rcc::MachineConstantPoolEntryDesc *TargetConstantDescription =
      Function->constantPool().entry(rcc::ConstantPoolIndex::fromValue(1))->description();
  const auto *TargetConstantValue =
      TargetConstantDescription == nullptr
          ? nullptr
          : std::get_if<rcc::TargetConstant>(&TargetConstantDescription->Value);
  Context.expect(Function->constantPool().liveEntryCount() == 3 &&
                     Function->jumpTableInfo().liveTableCount() == 1 &&
                     Function->metadata().nodes().size() == 1 && TargetState != nullptr &&
                     TargetState->frameBase() == 64 && TargetConstantValue != nullptr &&
                     TargetConstantValue->getIf<DocumentTargetConstant>() != nullptr &&
                     TargetConstantValue->getIf<DocumentTargetConstant>()->value() == 1234,
                 "constant, target-state, jump-table, and metadata tables must publish together");
  Context.expect(Function->blockCount() == 1 && Function->allInstructions().size() == 3,
                 "the body must use the YAML predeclared text slots");

  auto Printed = rcc::mir::printMIRDocument(*Document, *Target);
  Context.expect(Printed.has_value(), "complete MIR document printing must succeed");
  if (!Printed) {
    std::cerr << "error: " << Printed.error().message() << '\n';
    return;
  }
  Context.expect(Printed->find("name: documented\nalignment: 16\n") != std::string::npos &&
                     Printed->find("functionContext: ''") != std::string::npos &&
                     Printed->find("...\n---\nname: documented") != std::string::npos &&
                     Printed->ends_with("...\n"),
                 "complete MIR must use LLVM-compatible scalars, defaults, and document markers");
  auto RoundTrip = rcc::mir::parseMIRDocument(*Target, "round-trip.mir", std::string(*Printed));
  Context.expect(RoundTrip.succeeded(), "printed complete MIR must parse successfully");
  if (!RoundTrip.succeeded()) {
    std::cerr << *Printed;
    printDiagnostics(RoundTrip);
    return;
  }
  auto PrintedAgain = rcc::mir::printMIRDocument(*RoundTrip.document(), *Target);
  Context.expect(PrintedAgain.has_value() && *PrintedAgain == *Printed,
                 "complete MIR document printing must be deterministic");

  auto Readable = rcc::mir::printMIRDocument(
      *Document, *Target,
      rcc::mir::MIRDocumentPrinterOptions{
          .Mode = rcc::mir::MIPrintMode::Readable, .EmitEmbeddedIR = true, .DataLayout = {}});
  Context.expect(Readable.has_value(), "readable MIR document printing must succeed");
  if (!Readable)
    return;
  auto ReadableRoundTrip =
      rcc::mir::parseMIRDocument(*Target, "readable.mir", std::string(*Readable));
  Context.expect(ReadableRoundTrip.succeeded(), "readable MIR output must parse successfully");
  if (!ReadableRoundTrip.succeeded())
    printDiagnostics(ReadableRoundTrip);

  auto WithoutEmbeddedIR = rcc::mir::printMIRDocument(
      *Document, *Target,
      rcc::mir::MIRDocumentPrinterOptions{
          .Mode = rcc::mir::MIPrintMode::Complete, .EmitEmbeddedIR = false, .DataLayout = {}});
  Context.expect(!WithoutEmbeddedIR && WithoutEmbeddedIR.error().message().find(
                                           "requires embedded LLVM IR") != std::string_view::npos,
                 "printer must reject output that omits required embedded LLVM IR");
}

void testEmptyYAMLDocuments(TestContext &Context) {
  auto Target = createTarget();
  if (!Target)
    return;

  constexpr std::string_view Boundary = "\n---\nname: documented";
  std::string WithEmptyDocuments(ValidDocument);
  const std::size_t BoundaryPosition = WithEmptyDocuments.find(Boundary);
  Context.expect(BoundaryPosition != std::string::npos,
                 "MIR empty-document fixture must find its document boundary");
  if (BoundaryPosition == std::string::npos)
    return;
  WithEmptyDocuments.replace(BoundaryPosition, Boundary.size(), "\n---\n---\nname: documented");
  WithEmptyDocuments.insert(0, "---\n");
  WithEmptyDocuments.append("\n...\n---\n");
  auto Parsed =
      rcc::mir::parseMIRDocument(*Target, "empty-documents.mir", std::move(WithEmptyDocuments));
  Context.expect(Parsed.succeeded() && Parsed.document() != nullptr &&
                     Parsed.document()->unit().functionCount() == 1,
                 "MIR must ignore leading, intermediate, and trailing empty YAML documents");
  if (!Parsed.succeeded())
    printDiagnostics(Parsed);

  std::string ExplicitNull(ValidDocument);
  ExplicitNull.replace(BoundaryPosition, Boundary.size(), "\n---\nnull\n---\nname: documented");
  auto Rejected = rcc::mir::parseMIRDocument(*Target, "explicit-null.mir", std::move(ExplicitNull));
  Context.expect(!Rejected.succeeded(),
                 "MIR must not treat an explicit null document as an empty separator");
}

void testHeterogeneousPointerLayout(TestContext &Context) {
  auto Target = createTarget();
  Context.expect(Target.has_value(), "heterogeneous-layout target must be valid");
  if (!Target)
    return;

  rcc::mir::MIRDocumentParserOptions ParserOptions;
  ParserOptions.MachineInstructions.VerifyAfterParse = false;
  ParserOptions.VerifyMachineFunctions = false;
  auto Parsed =
      rcc::mir::parseMIRDocument(*Target, "heterogeneous-layout.mir",
                                 std::string(HeterogeneousPointerDocument), ParserOptions);
  Context.expect(Parsed.succeeded(), "heterogeneous pointer-layout MIR must parse successfully");
  if (!Parsed.succeeded()) {
    printDiagnostics(Parsed);
    return;
  }

  const rcc::MachineFunction *Function = Parsed.document()->unit().function("pointer_layout");
  auto PointerType = rcc::LowLevelType::makePointer(1, 32);
  const rcc::MachineConstantPoolEntry *Constant =
      Function == nullptr ? nullptr
                          : Function->constantPool().entry(rcc::ConstantPoolIndex::fromValue(0));
  const rcc::MachineConstantPoolEntryDesc *ConstantDescription =
      Constant == nullptr ? nullptr : Constant->description();
  Context.expect(Function != nullptr && PointerType.has_value() &&
                     Function->registerInfo().virtualRegisters().size() == 1 &&
                     Function->registerInfo().virtualRegisters().front().Type == *PointerType &&
                     ConstantDescription != nullptr && ConstantDescription->Size == 4,
                 "embedded data layout must determine p1 register and generic-constant widths");

  rcc::mir::MIRDocumentParserOptions OverrideOptions;
  OverrideOptions.DataLayout = "e-p:64:64-p1:64:64";
  OverrideOptions.MachineInstructions.VerifyAfterParse = false;
  OverrideOptions.VerifyMachineFunctions = false;
  auto Overridden = rcc::mir::parseMIRDocument(
      *Target, "overridden-layout.mir", std::string(HeterogeneousPointerDocument), OverrideOptions);
  const rcc::MachineFunction *OverriddenFunction =
      Overridden.succeeded() ? Overridden.document()->unit().function("pointer_layout") : nullptr;
  const rcc::MachineConstantPoolEntry *OverriddenConstant =
      OverriddenFunction == nullptr
          ? nullptr
          : OverriddenFunction->constantPool().entry(rcc::ConstantPoolIndex::fromValue(0));
  const rcc::MachineConstantPoolEntryDesc *OverriddenConstantDescription =
      OverriddenConstant == nullptr ? nullptr : OverriddenConstant->description();
  const auto OverriddenPointerType = rcc::LowLevelType::makePointer(1, 64);
  Context.expect(Overridden.succeeded() && OverriddenFunction != nullptr &&
                     OverriddenPointerType.has_value() &&
                     OverriddenFunction->registerInfo().virtualRegisters().size() == 1 &&
                     OverriddenFunction->registerInfo().virtualRegisters().front().Type ==
                         *OverriddenPointerType &&
                     OverriddenConstantDescription != nullptr &&
                     OverriddenConstantDescription->Size == 8,
                 "explicit target DataLayout must override embedded LLVM IR layout");
  if (!Overridden.succeeded())
    printDiagnostics(Overridden);

  if (Overridden.succeeded()) {
    rcc::mir::MIRDocumentPrinterOptions OverridePrinterOptions;
    OverridePrinterOptions.EmitEmbeddedIR = false;
    OverridePrinterOptions.DataLayout = OverrideOptions.DataLayout;
    auto OverriddenOutput =
        rcc::mir::printMIRDocument(*Overridden.document(), *Target, OverridePrinterOptions);
    Context.expect(OverriddenOutput.has_value(),
                   "matching target DataLayout must allow omitting overridden embedded IR");
  }

  rcc::mir::MIRDocumentPrinterOptions PrinterOptions;
  PrinterOptions.EmitEmbeddedIR = false;
  auto WithoutLayout = rcc::mir::printMIRDocument(*Parsed.document(), *Target, PrinterOptions);
  Context.expect(!WithoutLayout && WithoutLayout.error().message().find("printer DataLayout") !=
                                       std::string_view::npos,
                 "omitting embedded IR must reject layout-dependent MIR without DataLayout");

  PrinterOptions.DataLayout = "e-p:64:64-p1:32:32";
  auto Printed = rcc::mir::printMIRDocument(*Parsed.document(), *Target, PrinterOptions);
  Context.expect(Printed.has_value() && Printed->find("p1:32") == std::string_view::npos &&
                     Printed->find("p1)") != std::string_view::npos,
                 "matching explicit DataLayout must emit canonical LLVM p1 MIR syntax");
  if (!Printed) {
    std::cerr << "error: " << Printed.error().message() << '\n';
    return;
  }

  rcc::mir::MIRDocumentParserOptions ReparseOptions;
  ReparseOptions.DataLayout = PrinterOptions.DataLayout;
  ReparseOptions.MachineInstructions.VerifyAfterParse = false;
  ReparseOptions.VerifyMachineFunctions = false;
  auto RoundTrip = rcc::mir::parseMIRDocument(*Target, "heterogeneous-layout-round-trip.mir",
                                              std::string(*Printed), ReparseOptions);
  const rcc::MachineFunction *RoundTripFunction =
      RoundTrip.succeeded() ? RoundTrip.document()->unit().function("pointer_layout") : nullptr;
  Context.expect(RoundTrip.succeeded() && RoundTripFunction != nullptr &&
                     RoundTripFunction->registerInfo().virtualRegisters().size() == 1 &&
                     RoundTripFunction->registerInfo().virtualRegisters().front().Type ==
                         *PointerType,
                 "explicit DataLayout must preserve p1 type when embedded IR is omitted");
  if (!RoundTrip.succeeded())
    printDiagnostics(RoundTrip);

  PrinterOptions.DataLayout = "e-p:64:64-p1:64:64";
  auto Mismatched = rcc::mir::printMIRDocument(*Parsed.document(), *Target, PrinterOptions);
  Context.expect(!Mismatched && Mismatched.error().message().find(
                                    "pointer virtual-register width") != std::string_view::npos,
                 "mismatched output DataLayout must be rejected when embedded IR is omitted");
}

void expectReferenceFailure(TestContext &Context, const rcc::TargetDescription &Target,
                            std::string_view From, std::string_view To,
                            std::string_view ExpectedDiagnostic) {
  std::string Invalid(ValidDocument);
  const std::size_t Position = Invalid.find(From);
  Context.expect(Position != std::string::npos,
                 "reference-validation test mutation must match the fixture");
  if (Position == std::string::npos)
    return;
  Invalid.replace(Position, From.size(), To);
  auto Parsed = rcc::mir::parseMIRDocument(Target, "reference.mir", std::move(Invalid));
  const bool HasExpectedDiagnostic =
      !Parsed.succeeded() && Parsed.document() == nullptr && Parsed.diagnostics().size() == 1 &&
      Parsed.diagnostics().front().Message.find(ExpectedDiagnostic) != std::string::npos;
  Context.expect(HasExpectedDiagnostic,
                 "invalid embedded IR references must fail with their semantic category");
  if (!HasExpectedDiagnostic)
    printDiagnostics(Parsed);
}

void testEmbeddedReferenceValidation(TestContext &Context) {
  auto Target = createTarget();
  Context.expect(Target.has_value(), "reference-validation target must be valid");
  if (!Target)
    return;

  expectReferenceFailure(Context, *Target, "from %ir.slot", "from %ir.missing",
                         "undefined embedded LLVM IR value '%missing'");
  expectReferenceFailure(Context, *Target, "TEST_GLOBAL @ordinary", "TEST_GLOBAL @missing",
                         "undefined embedded LLVM IR global 'missing'");
  expectReferenceFailure(Context, *Target, "value: 'i64 18446744073709551615'",
                         "value: 'ptr @missing'",
                         "generic constant references an undefined embedded LLVM IR global");
  expectReferenceFailure(Context, *Target, "value: 'i64 18446744073709551615'",
                         "value: 'ptr blockaddress(@documented, %missing)'",
                         "undefined embedded LLVM IR block 'missing'");
  expectReferenceFailure(Context, *Target, "debug-info-location: '!2'", "debug-info-location: '!0'",
                         "debug location requires DILocation metadata");
}

void expectNoEmbeddedIRReferenceFailure(TestContext &Context, const rcc::TargetDescription &Target,
                                        std::string_view DocumentText,
                                        std::string_view ExpectedDiagnostic) {
  auto Parsed =
      rcc::mir::parseMIRDocument(Target, "no-embedded-reference.mir", std::string(DocumentText));
  const bool Rejected =
      !Parsed.succeeded() && Parsed.document() == nullptr && Parsed.diagnostics().size() == 1 &&
      Parsed.diagnostics().front().Message.find(ExpectedDiagnostic) != std::string::npos;
  Context.expect(Rejected, "IR-specific MIR references without embedded LLVM IR must be rejected");
  if (!Rejected)
    printDiagnostics(Parsed);
}

void testReferencesWithoutEmbeddedIR(TestContext &Context) {
  auto Target = createTarget();
  Context.expect(Target.has_value(), "no-embedded-reference target must be valid");
  if (!Target)
    return;

  constexpr std::string_view ExternalDocument = R"(---
name: external_symbol
body: |
  bb.0:
    TEST_GLOBAL &runtime_symbol
)";
  auto External =
      rcc::mir::parseMIRDocument(*Target, "external-symbol.mir", std::string(ExternalDocument));
  Context.expect(External.succeeded(),
                 "external symbols must remain usable without embedded LLVM IR");
  if (!External.succeeded()) {
    printDiagnostics(External);
    return;
  }
  auto Printed = rcc::mir::printMIRDocument(*External.document(), *Target);
  Context.expect(Printed.has_value(),
                 "external-symbol MIR without embedded IR must remain printable");
  if (!Printed) {
    std::cerr << "error: " << Printed.error().message() << '\n';
  } else {
    auto RoundTrip =
        rcc::mir::parseMIRDocument(*Target, "external-round-trip.mir", std::move(*Printed));
    Context.expect(RoundTrip.succeeded(),
                   "external-symbol MIR without embedded IR must remain reparseable");
    if (!RoundTrip.succeeded())
      printDiagnostics(RoundTrip);
  }

  expectNoEmbeddedIRReferenceFailure(Context, *Target, R"(---
name: global_reference
body: |
  bb.0:
    TEST_GLOBAL @ordinary
)",
                                     "requires embedded LLVM IR to resolve global");
  expectNoEmbeddedIRReferenceFailure(Context, *Target, R"(---
name: ir_block_reference
body: |
  bb.0 (%ir-block.entry):
)",
                                     "machine basic block requires embedded LLVM IR");
  expectNoEmbeddedIRReferenceFailure(Context, *Target, R"(---
name: alloca_reference
stack:
  - { id: 0, name: slot, size: 8, alignment: 8 }
body: |
  bb.0:
)",
                                     "stack object requires embedded LLVM IR");
  expectNoEmbeddedIRReferenceFailure(Context, *Target, R"(---
name: ir_value_reference
body: |
  bb.0:
    $g1 = TEST_LOAD $g0 :: (load 8 from %ir.slot, align 8)
)",
                                     "requires embedded LLVM IR to resolve IR value");
  expectNoEmbeddedIRReferenceFailure(
      Context, *Target, R"(---
name: constant_reference
constants:
  - { id: 0, value: 'i64 ptrtoint (ptr @ordinary to i64)', alignment: 8 }
body: |
  bb.0:
)",
      "generic constant requires embedded LLVM IR to resolve global");
  expectNoEmbeddedIRReferenceFailure(Context, *Target, R"(---
name: block_address_reference
constants:
  - { id: 0, value: 'i64 ptrtoint (ptr blockaddress(@function, %entry) to i64)', alignment: 8 }
body: |
  bb.0:
)",
                                     "generic constant blockaddress requires embedded LLVM IR");
}

void testPrinterRejectsUnresolvedProgrammaticReferences(TestContext &Context) {
  auto Target = createTarget();
  Context.expect(Target.has_value(), "programmatic-reference target must be valid");
  if (!Target)
    return;

  rcc::CompilationUnit Unit;
  auto Function = Unit.createMachineFunction("programmatic_reference");
  Context.expect(Function.has_value(), "programmatic document function creation must succeed");
  if (!Function)
    return;
  auto Block = (*Function)->createBasicBlock();
  auto Appended = Block ? (*Function)->appendBlock(**Block) : rcc::Expected<void>{};
  Context.expect(Block && Appended.has_value(),
                 "programmatic document block creation must succeed");
  if (!Block || !Appended)
    return;

  rcc::MachineIRBuilder Builder(**Function, *Target);
  auto Insertion = Builder.setInsertPoint(**Block);
  auto Pending = Builder.buildInstr(rcc::Opcode::fromValue(GlobalOpcode));
  auto Added = Pending ? Pending->addGlobalSymbol("ordinary") : rcc::Expected<void>{};
  Context.expect(Insertion && Pending && Added.has_value(),
                 "programmatic global-reference instruction construction must succeed");
  if (!Insertion || !Pending || !Added)
    return;
  auto Built = Pending->build();
  Context.expect(Built.has_value(), "programmatic global-reference instruction must build");
  if (!Built)
    return;

  rcc::mir::MIRDocument Document(std::move(Unit));
  auto Printed = rcc::mir::printMIRDocument(Document, *Target);
  const bool Rejected =
      !Printed && Printed.error().message().find("requires embedded LLVM IR to resolve global") !=
                      std::string_view::npos;
  Context.expect(Rejected,
                 "printer must reject programmatic IR-specific references without embedded IR");
  if (!Rejected && !Printed)
    std::cerr << "error: " << Printed.error().message() << '\n';
}

void testMetadataSlotPriorityAndCanonicalization(TestContext &Context) {
  auto Target = createTarget();
  Context.expect(Target.has_value(), "metadata-priority target must be valid");
  if (!Target)
    return;

  std::string CollisionDocument(ValidDocument);
  constexpr std::string_view Original = "  - '!10 = distinct !{!0, !\"machine\", null}'";
  constexpr std::string_view Replacement = "  - '!0 = distinct !{!0, !\"machine\", null}'";
  const std::size_t Position = CollisionDocument.find(Original);
  Context.expect(Position != std::string::npos,
                 "metadata-priority test mutation must match the fixture");
  if (Position == std::string::npos)
    return;
  CollisionDocument.replace(Position, Original.size(), Replacement);

  auto Parsed =
      rcc::mir::parseMIRDocument(*Target, "metadata-priority.mir", std::move(CollisionDocument));
  Context.expect(Parsed.succeeded(),
                 "machine metadata may reuse an embedded LLVM IR metadata slot");
  if (!Parsed.succeeded()) {
    printDiagnostics(Parsed);
    return;
  }
  const rcc::MachineFunction *Function = Parsed.document()->unit().function("documented");
  const auto Nodes = Function == nullptr ? std::span<const rcc::MachineMetadataNode>{}
                                         : Function->metadata().nodes();
  const rcc::MachineMetadataReference *Reference =
      Nodes.size() == 1 && Nodes.front().Elements.size() == 3
          ? Nodes.front().Elements.front().reference()
          : nullptr;
  Context.expect(Function != nullptr && Nodes.size() == 1 && Nodes.front().Slot == 0 &&
                     Reference != nullptr &&
                     *Reference == rcc::MachineMetadataReference::embeddedIR(0),
                 "bare metadata references must prefer embedded LLVM IR slots");

  auto Printed = rcc::mir::printMIRDocument(*Parsed.document(), *Target);
  const bool Canonicalized =
      Printed && Printed->find("'!3 = distinct !{!0, !\"machine\", null}'") != std::string::npos;
  Context.expect(Canonicalized,
                 "printer must assign a noncolliding metadata slot for local metadata");
  if (!Canonicalized)
    return;
  auto RoundTrip =
      rcc::mir::parseMIRDocument(*Target, "metadata-priority-round-trip.mir", std::move(*Printed));
  Context.expect(RoundTrip.succeeded(),
                 "canonicalized metadata slots must survive a document round-trip");
  if (!RoundTrip.succeeded())
    printDiagnostics(RoundTrip);
}

void testFixedWidthDocumentFields(TestContext &Context) {
  auto Target = createTarget();
  Context.expect(Target.has_value(), "fixed-width field target must be valid");
  if (!Target)
    return;

  constexpr std::string_view Original = "maxCallFrameSize: 24";
  std::string BoundaryDocument(ValidDocument);
  const std::size_t BoundaryPosition = BoundaryDocument.find(Original);
  Context.expect(BoundaryPosition != std::string::npos,
                 "fixed-width boundary test mutation must match the fixture");
  if (BoundaryPosition == std::string::npos)
    return;
  BoundaryDocument.replace(BoundaryPosition, Original.size(), "maxCallFrameSize: 4294967295");
  auto Boundary =
      rcc::mir::parseMIRDocument(*Target, "frame-boundary.mir", std::move(BoundaryDocument));
  Context.expect(Boundary.succeeded(),
                 "maximum 32-bit maxCallFrameSize must remain representable in MIR");
  if (Boundary.succeeded()) {
    auto Printed = rcc::mir::printMIRDocument(*Boundary.document(), *Target);
    Context.expect(Printed && Printed->find("maxCallFrameSize: 4294967295") != std::string::npos,
                   "maximum 32-bit maxCallFrameSize must print without narrowing");
  } else {
    printDiagnostics(Boundary);
  }

  std::string OversizedDocument(ValidDocument);
  const std::size_t OversizedPosition = OversizedDocument.find(Original);
  if (OversizedPosition == std::string::npos)
    return;
  OversizedDocument.replace(OversizedPosition, Original.size(), "maxCallFrameSize: 4294967296");
  auto Oversized =
      rcc::mir::parseMIRDocument(*Target, "frame-oversized.mir", std::move(OversizedDocument));
  const bool Rejected =
      !Oversized.succeeded() && Oversized.document() == nullptr &&
      Oversized.diagnostics().size() == 1 &&
      Oversized.diagnostics().front().Message.find("maximum call-frame size") != std::string::npos;
  Context.expect(Rejected,
                 "maxCallFrameSize values beyond the fixed 32-bit MIR range must be rejected");
  if (!Rejected)
    printDiagnostics(Oversized);
}

void testAtomicFailure(TestContext &Context) {
  auto Target = createTarget();
  if (!Target)
    return;
  std::string Invalid(ValidDocument);
  Invalid.append("---\nname: second\nunknownField: true\n");
  auto Parsed = rcc::mir::parseMIRDocument(*Target, "atomic.mir", std::move(Invalid));
  Context.expect(!Parsed.succeeded() && Parsed.document() == nullptr &&
                     Parsed.diagnostics().size() == 1 &&
                     Parsed.diagnostics().front().Message.find("unknown machine function key") !=
                         std::string::npos,
                 "a later function failure must prevent publication of the entire document");
}

void testLLVMPropertySemantics(TestContext &Context) {
  auto Target = createTarget();
  Context.expect(Target.has_value(), "property-semantics target must be valid");
  if (!Target)
    return;

  constexpr std::string_view PropertyDocument = R"(---
name: property_state
failedISel: true
failsVerification: true
body: |
  bb.0:
)";
  auto Parsed =
      rcc::mir::parseMIRDocument(*Target, "properties.mir", std::string(PropertyDocument));
  Context.expect(Parsed.succeeded(), "LLVM MIR pipeline properties must parse");
  if (!Parsed.succeeded()) {
    printDiagnostics(Parsed);
    return;
  }
  const rcc::MachineFunction *Function = Parsed.document()->unit().function("property_state");
  Context.expect(Function != nullptr &&
                     Function->properties().has(rcc::MachineFunctionProperty::FailedISel) &&
                     Function->properties().has(rcc::MachineFunctionProperty::FailsVerification) &&
                     Function->compilationStatus().state() == rcc::MachineCompilationState::Success,
                 "failedISel and failsVerification must be pipeline properties, not run failures");

  auto Printed = rcc::mir::printMIRDocument(*Parsed.document(), *Target);
  Context.expect(Printed.has_value() && Printed->find("failedISel: true") != std::string::npos &&
                     Printed->find("failsVerification: true") != std::string::npos,
                 "LLVM MIR pipeline properties must print deterministically");

  constexpr std::string_view InvalidDocument = R"(---
name: still_checked
failsVerification: true
noVRegs: true
registers:
  - { id: 1, class: gpr64 }
body: |
  bb.0:
)";
  auto Invalid =
      rcc::mir::parseMIRDocument(*Target, "properties-invalid.mir", std::string(InvalidDocument));
  const bool Rejected = !Invalid.succeeded() && Invalid.document() == nullptr &&
                        Invalid.diagnostics().size() == 1 &&
                        Invalid.diagnostics().front().Message.find("NoVRegs") != std::string::npos;
  Context.expect(Rejected, "failsVerification must not bypass RCC's mandatory input validation");
  if (!Rejected)
    printDiagnostics(Invalid);
}

void testLiveInCreatesVirtualRegister(TestContext &Context) {
  auto Target = createTarget();
  Context.expect(Target.has_value(), "live-in virtual-register target must be valid");
  if (!Target)
    return;

  constexpr std::string_view DocumentText = R"(---
name: live_in_vreg
liveins:
  - { reg: '$g0', virtual-reg: '%0' }
body: |
  bb.0:
    %0:gpr64 = TEST_COPY $g0
)";
  auto Parsed = rcc::mir::parseMIRDocument(*Target, "live-in-vreg.mir", std::string(DocumentText));
  const rcc::MachineFunction *Function =
      Parsed.succeeded() ? Parsed.document()->unit().function("live_in_vreg") : nullptr;
  const auto Registers = Function != nullptr ? Function->registerInfo().virtualRegisters()
                                             : std::span<const rcc::VirtualRegisterInfo>{};
  const auto LiveIns = Function != nullptr ? Function->registerInfo().liveIns()
                                           : std::span<const rcc::PhysicalRegisterLiveIn>{};
  Context.expect(Parsed.succeeded() && Function != nullptr && Registers.size() == 1 &&
                     Registers.front().RegClass.has_value() && LiveIns.size() == 1 &&
                     LiveIns.front().InitialVirtualRegister.has_value(),
                 "a live-in must create a virtual register before the body constrains it");
  if (!Parsed.succeeded()) {
    printDiagnostics(Parsed);
    return;
  }

  auto Printed = rcc::mir::printMIRDocument(*Parsed.document(), *Target);
  Context.expect(Printed.has_value(),
                 "an implicitly created live-in virtual register must be printable");
  if (!Printed)
    return;
  auto RoundTrip = rcc::mir::parseMIRDocument(*Target, "live-in-vreg-round-trip.mir", *Printed);
  Context.expect(RoundTrip.succeeded(),
                 "an implicitly created live-in virtual register must round-trip");
  if (!RoundTrip.succeeded())
    printDiagnostics(RoundTrip);
}

void testBodyDiagnosticLocation(TestContext &Context) {
  auto Target = createTarget();
  Context.expect(Target.has_value(), "body-diagnostic target must be valid");
  if (!Target)
    return;

  constexpr std::string_view InvalidDocument = R"(---
name: body_diagnostic
body: |
  bb.0:
    UNKNOWN_OPCODE
)";
  const std::size_t ExpectedOffset = InvalidDocument.find("UNKNOWN_OPCODE");
  auto Parsed =
      rcc::mir::parseMIRDocument(*Target, "body-diagnostic.mir", std::string(InvalidDocument));
  const bool PointsAtToken = !Parsed.succeeded() && Parsed.document() == nullptr &&
                             Parsed.diagnostics().size() == 1 &&
                             Parsed.diagnostics().front().Category == "mir-body" &&
                             Parsed.diagnostics().front().Range.Begin.Offset == ExpectedOffset;
  Context.expect(PointsAtToken,
                 "body parser errors must retain the source position of the failing token");
  if (!PointsAtToken)
    printDiagnostics(Parsed);
}

void testAnonymousFunctionsAndIRBlocks(TestContext &Context) {
  auto Target = createTarget();
  Context.expect(Target.has_value(), "anonymous-function target must be valid");
  if (!Target)
    return;

  constexpr std::string_view DocumentText = R"(--- |
  define void @0() {
    ret void
  }

  define void @named() {
    ret void
  }

  define void @1() {
    ret void
  }
...
---
name: ''
body: |
  bb.0 (%ir-block.0):
...
---
name: named
body: |
  bb.0 (%ir-block.0):
...
---
name: ''
body: |
  bb.0 (%ir-block.0):
...
)";
  auto Parsed = rcc::mir::parseMIRDocument(*Target, "anonymous.mir", std::string(DocumentText));
  const rcc::MachineFunction *First =
      Parsed.succeeded() ? Parsed.document()->unit().functionAt(0) : nullptr;
  const rcc::MachineFunction *Named =
      Parsed.succeeded() ? Parsed.document()->unit().functionAt(1) : nullptr;
  const rcc::MachineFunction *Second =
      Parsed.succeeded() ? Parsed.document()->unit().functionAt(2) : nullptr;
  Context.expect(Parsed.succeeded() && First != nullptr && First->name().empty() &&
                     Named != nullptr && Named->name() == "named" && Second != nullptr &&
                     Second->name().empty(),
                 "MIR must match empty names to embedded anonymous functions in document order");
  if (!Parsed.succeeded()) {
    printDiagnostics(Parsed);
    return;
  }

  auto Printed = rcc::mir::printMIRDocument(*Parsed.document(), *Target);
  Context.expect(Printed.has_value(), "anonymous MIR functions must remain printable");
  if (!Printed)
    return;
  const std::size_t FirstAnonymous = Printed->find("name: ''");
  const std::size_t SecondAnonymous =
      FirstAnonymous == std::string::npos
          ? std::string::npos
          : Printed->find("name: ''", FirstAnonymous + std::string_view("name: ''").size());
  Context.expect(FirstAnonymous != std::string::npos && SecondAnonymous != std::string::npos &&
                     Printed->find("%ir-block.0") != std::string::npos,
                 "anonymous function names and unnamed IR block slots must print canonically");

  constexpr std::string_view NoIRText = R"(...
---
name: ''
body: |
  bb.0:
...
)";
  auto NoIR = rcc::mir::parseMIRDocument(*Target, "anonymous-no-ir.mir", std::string(NoIRText));
  Context.expect(NoIR.succeeded() && NoIR.document()->unit().functionAt(0) != nullptr &&
                     NoIR.document()->unit().functionAt(0)->name().empty(),
                 "MIR without embedded IR must accept an anonymous dummy function");
  if (!NoIR.succeeded())
    printDiagnostics(NoIR);
}

} // namespace

int main() {
  TestContext Context;
  testDocumentTransaction(Context);
  testEmptyYAMLDocuments(Context);
  testHeterogeneousPointerLayout(Context);
  testEmbeddedReferenceValidation(Context);
  testReferencesWithoutEmbeddedIR(Context);
  testPrinterRejectsUnresolvedProgrammaticReferences(Context);
  testMetadataSlotPriorityAndCanonicalization(Context);
  testFixedWidthDocumentFields(Context);
  testAtomicFailure(Context);
  testLLVMPropertySemantics(Context);
  testLiveInCreatesVirtualRegister(Context);
  testBodyDiagnosticLocation(Context);
  testAnonymousFunctionsAndIRBlocks(Context);
  return Context.result();
}
