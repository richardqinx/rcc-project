// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MIRPrinter.h"

#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/InlineAsm.h"
#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/MIRYamlMapping.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineCFIInfo.h"
#include "rcc/CodeGen/MachineCompilationStatus.h"
#include "rcc/CodeGen/MachineConstantPool.h"
#include "rcc/CodeGen/MachineFrameInfo.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineFunctionAttributes.h"
#include "rcc/CodeGen/MachineFunctionProperties.h"
#include "rcc/CodeGen/MachineFunctionRecords.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineJumpTableInfo.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/MachineMetadata.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/MachineVerifier.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/CodeGen/TargetMIRTextInfo.h"
#include "rcc/CodeGen/TargetRegisterInfo.h"
#include "rcc/IR/DataLayout.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace rcc::mir {
namespace {

enum class YAMLQuoting : std::uint8_t {
  None,
  Single,
  Double,
};

[[nodiscard]] bool isASCIIAlphanumeric(unsigned char Character) noexcept {
  return (Character >= '0' && Character <= '9') || (Character >= 'A' && Character <= 'Z') ||
         (Character >= 'a' && Character <= 'z');
}

[[nodiscard]] bool isYAMLNumeric(std::string_view Value) noexcept {
  if (Value.empty() || Value == "+" || Value == "-")
    return false;
  if (Value == ".nan" || Value == ".NaN" || Value == ".NAN")
    return true;

  std::string_view Tail = Value;
  if (Tail.front() == '+' || Tail.front() == '-')
    Tail.remove_prefix(1);
  if (Tail == ".inf" || Tail == ".Inf" || Tail == ".INF")
    return true;
  if (Value.starts_with("0o")) {
    return Value.size() > 2U &&
           Value.substr(2).find_first_not_of("01234567") == std::string_view::npos;
  }
  if (Value.starts_with("0x")) {
    return Value.size() > 2U &&
           Value.substr(2).find_first_not_of("0123456789abcdefABCDEF") == std::string_view::npos;
  }

  if (Tail.empty() || Tail.front() == 'e' || Tail.front() == 'E')
    return false;
  std::size_t Position = 0;
  if (Tail.front() == '.') {
    ++Position;
    if (Position == Tail.size() || Tail[Position] < '0' || Tail[Position] > '9')
      return false;
    while (Position < Tail.size() && Tail[Position] >= '0' && Tail[Position] <= '9')
      ++Position;
  } else {
    while (Position < Tail.size() && Tail[Position] >= '0' && Tail[Position] <= '9')
      ++Position;
    if (Position == 0)
      return false;
  }

  if (Position < Tail.size() && Tail[Position] == '.') {
    ++Position;
    while (Position < Tail.size() && Tail[Position] >= '0' && Tail[Position] <= '9')
      ++Position;
  }
  if (Position == Tail.size())
    return true;
  if (Tail[Position] != 'e' && Tail[Position] != 'E')
    return false;
  ++Position;
  if (Position < Tail.size() && (Tail[Position] == '+' || Tail[Position] == '-'))
    ++Position;
  const std::size_t ExponentBegin = Position;
  while (Position < Tail.size() && Tail[Position] >= '0' && Tail[Position] <= '9')
    ++Position;
  return Position == Tail.size() && Position != ExponentBegin;
}

[[nodiscard]] YAMLQuoting yamlQuoting(std::string_view Value) noexcept {
  if (Value.empty())
    return YAMLQuoting::Single;

  YAMLQuoting Result = YAMLQuoting::None;
  const auto IsSpace = [](unsigned char Character) {
    return Character == ' ' || Character == '\t' || Character == '\r' || Character == '\n' ||
           Character == '\f' || Character == '\v';
  };
  if (IsSpace(static_cast<unsigned char>(Value.front())) ||
      IsSpace(static_cast<unsigned char>(Value.back())) || Value == "null" || Value == "Null" ||
      Value == "NULL" || Value == "~" || Value == "true" || Value == "True" || Value == "TRUE" ||
      Value == "false" || Value == "False" || Value == "FALSE" || isYAMLNumeric(Value)) {
    Result = YAMLQuoting::Single;
  }

  switch (Value.front()) {
  case '-':
  case '?':
  case ':':
  case '\\':
  case ',':
  case '[':
  case ']':
  case '{':
  case '}':
  case '#':
  case '&':
  case '*':
  case '!':
  case '|':
  case '>':
  case '\'':
  case '"':
  case '%':
  case '@':
  case '`':
    Result = YAMLQuoting::Single;
    break;
  default:
    break;
  }

  for (const char RawCharacter : Value) {
    const auto Character = static_cast<unsigned char>(RawCharacter);
    if (isASCIIAlphanumeric(Character))
      continue;
    switch (Character) {
    case '_':
    case '-':
    case '^':
    case '.':
    case ',':
    case ' ':
    case '\t':
      continue;
    case '\n':
    case '\r':
    case 0x7f:
      return YAMLQuoting::Double;
    default:
      if (Character <= 0x1fU || (Character & 0x80U) != 0)
        return YAMLQuoting::Double;
      Result = YAMLQuoting::Single;
      break;
    }
  }
  return Result;
}

[[nodiscard]] std::string yamlScalar(std::string_view Value) {
  const YAMLQuoting Quoting = yamlQuoting(Value);
  if (Quoting == YAMLQuoting::None)
    return std::string(Value);

  std::string Result;
  Result.reserve(Value.size() + 2U);
  const char Quote = Quoting == YAMLQuoting::Single ? '\'' : '"';
  Result.push_back(Quote);
  for (const char Character : Value) {
    if (Quoting == YAMLQuoting::Single) {
      Result.push_back(Character);
      if (Character == '\'')
        Result.push_back('\'');
      continue;
    }
    switch (Character) {
    case '\\':
      Result.append("\\\\");
      break;
    case '"':
      Result.append("\\\"");
      break;
    case '\0':
      Result.append("\\0");
      break;
    case '\a':
      Result.append("\\a");
      break;
    case '\b':
      Result.append("\\b");
      break;
    case '\t':
      Result.append("\\t");
      break;
    case '\n':
      Result.append("\\n");
      break;
    case '\v':
      Result.append("\\v");
      break;
    case '\f':
      Result.append("\\f");
      break;
    case '\r':
      Result.append("\\r");
      break;
    case '\x1b':
      Result.append("\\e");
      break;
    default: {
      const auto Byte = static_cast<unsigned char>(Character);
      if (Byte < 0x20U || Byte == 0x7fU) {
        static constexpr std::string_view Hex = "0123456789ABCDEF";
        Result.append("\\x");
        Result.push_back(Hex[Byte >> 4U]);
        Result.push_back(Hex[Byte & 0xfU]);
      } else {
        Result.push_back(Character);
      }
      break;
    }
    }
  }
  Result.push_back(Quote);
  return Result;
}

void appendBlockScalar(std::string &Output, std::string_view Text, std::size_t Indent) {
  std::size_t Position = 0;
  do {
    Output.append(Indent, ' ');
    const std::size_t End = Text.find('\n', Position);
    if (End == std::string_view::npos) {
      Output.append(Text.substr(Position));
      Output.push_back('\n');
      return;
    }
    Output.append(Text.substr(Position, End - Position));
    Output.push_back('\n');
    Position = End + 1U;
  } while (Position < Text.size());
}

[[nodiscard]] std::optional<std::uint32_t> parseDecimal32(std::string_view Text) noexcept {
  std::uint32_t Value = 0;
  if (Text.empty())
    return std::nullopt;
  const auto [End, Error] = std::from_chars(Text.data(), Text.data() + Text.size(), Value, 10);
  if (Error != std::errc{} || End != Text.data() + Text.size())
    return std::nullopt;
  return Value;
}

[[nodiscard]] bool isDefaultFrameState(const MachineFrameState &State) {
  return State == MachineFrameState{};
}

class MIRDocumentPrinter final {
public:
  MIRDocumentPrinter(const MIRDocument &Document, const TargetDescription &Target,
                     MIRDocumentPrinterOptions Options)
      : Document(Document), Target(Target), Options(std::move(Options)) {}

  [[nodiscard]] Expected<std::string> run();

private:
  struct InstructionLocation final {
    std::uint32_t Block = 0;
    std::uint32_t Offset = 0;
  };

  [[nodiscard]] Expected<void> printFunction(const MachineFunction &Function,
                                             const MIRFunctionDocumentInfo *DocumentInfo);
  [[nodiscard]] Expected<MIParserContext> createBodyContext(const MachineFunction &Function) const;
  [[nodiscard]] Expected<void> printRegisters(const MachineFunction &Function,
                                              const MIParserContext &Context);
  [[nodiscard]] Expected<void> printFrameInfo(const MachineFunction &Function,
                                              const MIParserContext &Context);
  [[nodiscard]] Expected<void> printStackObjects(const MachineFunction &Function,
                                                 const MIParserContext &Context, bool Fixed);
  [[nodiscard]] Expected<void> printConstants(const MachineFunction &Function,
                                              const MIParserContext &Context);
  [[nodiscard]] Expected<void> printJumpTable(const MachineFunction &Function,
                                              const MIParserContext &Context);
  [[nodiscard]] Expected<void> printMetadata(const MachineFunction &Function,
                                             const MIParserContext &Context);
  [[nodiscard]] Expected<void> printTargetState(const MachineFunction &Function);
  [[nodiscard]] Expected<void> printFunctionRecords(const MachineFunction &Function,
                                                    const MIParserContext &Context);
  [[nodiscard]] Expected<void> verifyOmittedEmbeddedIRLayout(const MachineFunction &Function) const;

  [[nodiscard]] Expected<std::string> physicalRegister(PhysicalRegister Register) const;
  [[nodiscard]] Expected<std::string> frameReference(FrameIndex Index,
                                                     const MIParserContext &Context) const;
  [[nodiscard]] Expected<std::string> metadataReference(MachineMetadataReference Reference,
                                                        const MIParserContext &Context) const;
  [[nodiscard]] Expected<std::string> metadataValue(const MachineMetadataValue &Value,
                                                    const MIParserContext &Context) const;
  [[nodiscard]] Expected<std::string> debugLocation(const MachineDebugLocation &Location,
                                                    const MIParserContext &Context) const;
  [[nodiscard]] Expected<std::string> diLocation(const MachineDILocation &Location,
                                                 std::size_t FrameIndex,
                                                 const MIParserContext &Context) const;
  [[nodiscard]] Expected<InstructionLocation> instructionLocation(const MachineFunction &Function,
                                                                  MachineInstrStableID ID) const;
  [[nodiscard]] Expected<std::string> blockReference(const MachineFunction &Function,
                                                     MachineBlockStableID ID) const;
  [[nodiscard]] Expected<std::string> targetNode(const MIRTargetNode &Node) const;
  [[nodiscard]] const MIRFunctionDocumentInfo *
  documentInfo(const MachineFunction &Function) const noexcept;

  void printBoolean(std::string_view Key, bool Value, bool Always = false);
  void printUnsigned(std::string_view Key, std::uint64_t Value, bool Always = false);
  void printSigned(std::string_view Key, std::int64_t Value, bool Always = false);

  const MIRDocument &Document;
  const TargetDescription &Target;
  MIRDocumentPrinterOptions Options;
  std::string Output;
};

void MIRDocumentPrinter::printBoolean(std::string_view Key, bool Value, bool Always) {
  if (!Always && Options.Mode == MIPrintMode::Readable && !Value)
    return;
  Output.append(Key);
  Output.append(Value ? ": true\n" : ": false\n");
}

void MIRDocumentPrinter::printUnsigned(std::string_view Key, std::uint64_t Value, bool Always) {
  if (!Always && Options.Mode == MIPrintMode::Readable && Value == 0)
    return;
  Output.append(Key);
  Output.append(": ");
  Output.append(std::to_string(Value));
  Output.push_back('\n');
}

void MIRDocumentPrinter::printSigned(std::string_view Key, std::int64_t Value, bool Always) {
  if (!Always && Options.Mode == MIPrintMode::Readable && Value == 0)
    return;
  Output.append(Key);
  Output.append(": ");
  Output.append(std::to_string(Value));
  Output.push_back('\n');
}

Expected<std::string> MIRDocumentPrinter::physicalRegister(PhysicalRegister Register) const {
  const auto Name = Target.registers().physicalRegisterMIRName(Register);
  if (!Name) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "physical register has no canonical MIR name");
  }
  return "$" + std::string(*Name);
}

const MIRFunctionDocumentInfo *
MIRDocumentPrinter::documentInfo(const MachineFunction &Function) const noexcept {
  const auto Infos = Document.functionDocumentInfo();
  const auto Found = std::ranges::find_if(Infos, [&Function](const MIRFunctionDocumentInfo &Info) {
    return Info.Function == Function.stableID();
  });
  return Found == Infos.end() ? nullptr : &*Found;
}

Expected<void>
MIRDocumentPrinter::verifyOmittedEmbeddedIRLayout(const MachineFunction &Function) const {
  if (Options.EmitEmbeddedIR)
    return {};

  const EmbeddedIRReferenceIndex *Embedded = Document.embeddedIR();
  if (Embedded == nullptr)
    return {};

  bool HasLayoutDependentContent = false;
  for (const VirtualRegisterInfo &RegisterInfo : Function.registerInfo().virtualRegisters()) {
    if (!RegisterInfo.IsTombstone && RegisterInfo.Type &&
        RegisterInfo.Type->isPointerOrPointerVector()) {
      HasLayoutDependentContent = true;
      break;
    }
  }
  if (!HasLayoutDependentContent) {
    for (const MachineConstantPoolEntry &Entry : Function.constantPool().entries()) {
      const MachineConstantPoolEntryDesc *Description = Entry.description();
      if (Description != nullptr &&
          std::holds_alternative<GenericConstantData>(Description->Value)) {
        HasLayoutDependentContent = true;
        break;
      }
    }
  }
  if (!HasLayoutDependentContent)
    return {};

  if (Options.DataLayout.empty()) {
    return makeUnexpected(
        ErrorCode::InvalidArgument,
        "omitting embedded LLVM IR with layout-dependent MIR requires an explicit printer "
        "DataLayout");
  }

  auto Layout = DataLayout::parse(Options.DataLayout);
  if (!Layout)
    return std::unexpected(Layout.error());
  for (const VirtualRegisterInfo &RegisterInfo : Function.registerInfo().virtualRegisters()) {
    if (RegisterInfo.IsTombstone || !RegisterInfo.Type ||
        !RegisterInfo.Type->isPointerOrPointerVector()) {
      continue;
    }
    const auto AddressSpace = RegisterInfo.Type->addressSpace();
    if (!AddressSpace) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "pointer virtual register has no address space");
    }
    auto PointerSize = Layout->getPointerSizeInBits(*AddressSpace);
    if (!PointerSize) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "printer DataLayout cannot determine pointer width: " +
                                std::string(PointerSize.error().message()));
    }
    if (*PointerSize != RegisterInfo.Type->scalarBitWidth()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "printer DataLayout does not match pointer virtual-register width");
    }
  }

  for (const MachineConstantPoolEntry &Entry : Function.constantPool().entries()) {
    const MachineConstantPoolEntryDesc *Description = Entry.description();
    if (Description == nullptr)
      continue;
    const auto *Generic = std::get_if<GenericConstantData>(&Description->Value);
    if (Generic == nullptr)
      continue;
    auto StorageSize = Layout->getTypeAllocSize(Generic->Type);
    if (!StorageSize) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "printer DataLayout cannot lay out generic constant: " +
                                std::string(StorageSize.error().message()));
    }
    if (*StorageSize != Description->Size) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "printer DataLayout does not match generic constant storage size");
    }
  }
  return {};
}

Expected<MIParserContext>
MIRDocumentPrinter::createBodyContext(const MachineFunction &Function) const {
  MIParserContext Context;
  Context.SyncScopes.assign(Document.syncScopes().begin(), Document.syncScopes().end());
  const auto VirtualRegisters = Function.registerInfo().virtualRegisters();
  for (std::size_t Index = 0; Index < VirtualRegisters.size(); ++Index) {
    if (VirtualRegisters[Index].IsTombstone)
      continue;
    auto Reg = Register::makeVirtual(Index);
    if (!Reg)
      return std::unexpected(Reg.error());
    if (Index > std::numeric_limits<std::uint32_t>::max()) {
      return makeUnexpected(ErrorCode::Unsupported,
                            "MIR cannot represent more than 2^32 virtual registers");
    }
    Context.VirtualRegisters.push_back({.TextID = static_cast<std::uint32_t>(Index), .Reg = *Reg});
  }

  std::uint32_t StackID = 0;
  std::uint32_t FixedID = 0;
  for (const StackObject &Object : Function.frameInfo().objects()) {
    const StackObjectDesc *Description = Object.description();
    if (Description == nullptr)
      continue;
    MIFrameObjectSlot Slot{
        .TextID = Object.isFixed() ? FixedID++ : StackID++,
        .Index = Object.index(),
        .Name = Description->AllocaName,
    };
    (Object.isFixed() ? Context.FixedStackObjects : Context.StackObjects)
        .push_back(std::move(Slot));
  }

  std::uint32_t ConstantID = 0;
  for (const MachineConstantPoolEntry &Entry : Function.constantPool().entries()) {
    if (Entry.description() != nullptr)
      Context.ConstantPoolItems.push_back({.TextID = ConstantID++, .Index = Entry.index()});
  }
  std::uint32_t JumpTableID = 0;
  for (const MachineJumpTableEntry &Entry : Function.jumpTableInfo().tables()) {
    if (Entry.description() != nullptr)
      Context.JumpTables.push_back({.TextID = JumpTableID++, .Index = Entry.index()});
  }

  std::unordered_set<std::uint32_t> EmbeddedMetadataSlots;
  if (const EmbeddedIRReferenceIndex *Embedded = Document.embeddedIR()) {
    for (const EmbeddedIRMetadataDefinition &Definition : Embedded->metadataDefinitions()) {
      const auto Slot = parseDecimal32(Definition.Name);
      if (Slot) {
        if (!EmbeddedMetadataSlots.insert(*Slot).second) {
          return makeUnexpected(ErrorCode::VerificationError,
                                "embedded LLVM IR metadata slots are not unique");
        }
        Context.Metadata.push_back(
            {.TextID = *Slot, .Reference = MachineMetadataReference::embeddedIR(*Slot)});
      }
    }
  }

  std::unordered_set<std::uint32_t> UsedMetadataSlots = EmbeddedMetadataSlots;
  for (const MachineMetadataNode &Node : Function.metadata().nodes()) {
    if (!EmbeddedMetadataSlots.contains(Node.Slot))
      UsedMetadataSlots.insert(Node.Slot);
  }
  std::uint64_t NextMetadataSlot = 0;
  for (const MachineMetadataNode &Node : Function.metadata().nodes()) {
    std::uint32_t TextID = Node.Slot;
    if (EmbeddedMetadataSlots.contains(TextID)) {
      while (NextMetadataSlot <= std::numeric_limits<std::uint32_t>::max() &&
             UsedMetadataSlots.contains(static_cast<std::uint32_t>(NextMetadataSlot))) {
        ++NextMetadataSlot;
      }
      if (NextMetadataSlot > std::numeric_limits<std::uint32_t>::max()) {
        return makeUnexpected(ErrorCode::Unsupported, "MIR metadata text slot space is exhausted");
      }
      TextID = static_cast<std::uint32_t>(NextMetadataSlot);
      UsedMetadataSlots.insert(TextID);
      ++NextMetadataSlot;
    }
    Context.Metadata.push_back(
        {.TextID = TextID, .Reference = MachineMetadataReference::machineFunction(Node.Slot)});
  }
  return Context;
}

Expected<std::string> MIRDocumentPrinter::frameReference(FrameIndex Index,
                                                         const MIParserContext &Context) const {
  const auto Stack = std::ranges::find_if(
      Context.StackObjects, [Index](const MIFrameObjectSlot &Slot) { return Slot.Index == Index; });
  if (Stack != Context.StackObjects.end())
    return "%stack." + std::to_string(Stack->TextID);
  const auto Fixed =
      std::ranges::find_if(Context.FixedStackObjects,
                           [Index](const MIFrameObjectSlot &Slot) { return Slot.Index == Index; });
  if (Fixed != Context.FixedStackObjects.end())
    return "%fixed-stack." + std::to_string(Fixed->TextID);
  return makeUnexpected(ErrorCode::VerificationError,
                        "frame state references an unknown stack object");
}

Expected<std::string> MIRDocumentPrinter::metadataReference(MachineMetadataReference Reference,
                                                            const MIParserContext &Context) const {
  if (!Reference.isValid())
    return makeUnexpected(ErrorCode::VerificationError, "invalid metadata reference");
  const auto Slot =
      std::ranges::find_if(Context.Metadata, [Reference](const MIMetadataSlot &Candidate) {
        return Candidate.Reference == Reference;
      });
  if (Slot == Context.Metadata.end()) {
    return makeUnexpected(ErrorCode::VerificationError, "metadata reference has no MIR text slot");
  }
  return "!" + std::to_string(Slot->TextID);
}

Expected<std::string> MIRDocumentPrinter::metadataValue(const MachineMetadataValue &Value,
                                                        const MIParserContext &Context) const {
  if (const MachineMetadataReference *Reference = Value.reference())
    return metadataReference(*Reference, Context);
  const MachineDIExpression *Expression = Value.expression();
  if (Expression == nullptr)
    return makeUnexpected(ErrorCode::VerificationError, "metadata value is not printable");
  return formatDIExpression(Expression->elements());
}

Expected<std::string> MIRDocumentPrinter::diLocation(const MachineDILocation &Location,
                                                     std::size_t FrameIndex,
                                                     const MIParserContext &Context) const {
  if (FrameIndex >= Location.inlineChain().size()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "inline DILocation has an invalid frame chain");
  }
  const MachineDILocationFrame &Frame = Location.inlineChain()[FrameIndex];
  auto Scope = metadataReference(Frame.Scope, Context);
  if (!Scope)
    return std::unexpected(Scope.error());
  std::string Text = "!DILocation(line: " + std::to_string(Frame.Line);
  if (Options.Mode == MIPrintMode::Complete || Frame.Column != 0)
    Text.append(", column: " + std::to_string(Frame.Column));
  Text.append(", scope: " + *Scope);
  if (FrameIndex + 1U < Location.inlineChain().size()) {
    auto Nested = diLocation(Location, FrameIndex + 1U, Context);
    if (!Nested)
      return std::unexpected(Nested.error());
    Text.append(", inlinedAt: " + *Nested);
  } else if (Location.terminalInlinedAt()) {
    auto Terminal = metadataReference(*Location.terminalInlinedAt(), Context);
    if (!Terminal)
      return std::unexpected(Terminal.error());
    Text.append(", inlinedAt: " + *Terminal);
  }
  if (Options.Mode == MIPrintMode::Complete || Frame.IsImplicitCode)
    Text.append(Frame.IsImplicitCode ? ", isImplicitCode: true" : ", isImplicitCode: false");
  if (Options.Mode == MIPrintMode::Complete || Frame.AtomGroup != 0)
    Text.append(", atomGroup: " + std::to_string(Frame.AtomGroup));
  if (Options.Mode == MIPrintMode::Complete || Frame.AtomRank != 0)
    Text.append(", atomRank: " + std::to_string(Frame.AtomRank));
  Text.push_back(')');
  return Text;
}

Expected<std::string> MIRDocumentPrinter::debugLocation(const MachineDebugLocation &Location,
                                                        const MIParserContext &Context) const {
  if (const MachineMetadataReference *Reference = Location.reference())
    return metadataReference(*Reference, Context);
  if (const MachineDILocation *Inline = Location.inlineLocation())
    return diLocation(*Inline, 0, Context);
  return makeUnexpected(ErrorCode::VerificationError, "debug location is not printable");
}

Expected<MIRDocumentPrinter::InstructionLocation>
MIRDocumentPrinter::instructionLocation(const MachineFunction &Function,
                                        MachineInstrStableID ID) const {
  std::uint64_t BlockNumber = 0;
  for (const MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
       Block = Block->next(), ++BlockNumber) {
    if (BlockNumber > std::numeric_limits<std::uint32_t>::max())
      break;
    std::uint64_t Offset = 0;
    for (const MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next(), ++Offset) {
      if (Instruction->stableID() != ID)
        continue;
      if (Offset > std::numeric_limits<std::uint32_t>::max()) {
        return makeUnexpected(ErrorCode::Unsupported, "MIR instruction offset exceeds 32 bits");
      }
      return InstructionLocation{static_cast<std::uint32_t>(BlockNumber),
                                 static_cast<std::uint32_t>(Offset)};
    }
  }
  return makeUnexpected(ErrorCode::VerificationError,
                        "function record references an unlinked instruction");
}

Expected<std::string> MIRDocumentPrinter::blockReference(const MachineFunction &Function,
                                                         MachineBlockStableID ID) const {
  std::uint64_t Number = 0;
  for (const MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
       Block = Block->next(), ++Number) {
    if (Block->stableID() == ID) {
      if (Number > std::numeric_limits<std::uint32_t>::max())
        break;
      return "%bb." + std::to_string(Number);
    }
  }
  return makeUnexpected(ErrorCode::VerificationError,
                        "frame record references an unlinked basic block");
}

Expected<std::string> MIRDocumentPrinter::targetNode(const MIRTargetNode &Node) const {
  switch (Node.Kind) {
  case MIRTargetNodeKind::Null:
    return std::string("null");
  case MIRTargetNodeKind::Scalar:
    return yamlScalar(Node.Scalar);
  case MIRTargetNodeKind::Sequence: {
    std::string Text = "[ ";
    for (std::size_t Index = 0; Index < Node.Sequence.size(); ++Index) {
      if (Index != 0)
        Text.append(", ");
      auto Element = targetNode(Node.Sequence[Index]);
      if (!Element)
        return std::unexpected(Element.error());
      Text.append(*Element);
    }
    Text.append(" ]");
    return Text;
  }
  case MIRTargetNodeKind::Mapping: {
    std::string Text = "{ ";
    for (std::size_t Index = 0; Index < Node.Mapping.size(); ++Index) {
      if (Index != 0)
        Text.append(", ");
      Text.append(yamlScalar(Node.Mapping[Index].Key));
      Text.append(": ");
      auto Value = targetNode(Node.Mapping[Index].Value);
      if (!Value)
        return std::unexpected(Value.error());
      Text.append(*Value);
    }
    Text.append(" }");
    return Text;
  }
  case MIRTargetNodeKind::Count:
    break;
  }
  return makeUnexpected(ErrorCode::VerificationError, "target MIR node has an invalid kind");
}

Expected<void> MIRDocumentPrinter::printRegisters(const MachineFunction &Function,
                                                  const MIParserContext &Context) {
  if (Context.VirtualRegisters.empty()) {
    if (Options.Mode == MIPrintMode::Complete)
      Output.append("registers: []\n");
  } else {
    Output.append("registers:\n");
    for (const MIVirtualRegisterSlot &Slot : Context.VirtualRegisters) {
      auto InfoResult = Function.registerInfo().virtualRegisterInfo(Slot.Reg);
      if (!InfoResult)
        return std::unexpected(InfoResult.error());
      const VirtualRegisterInfo &Info = InfoResult->get();
      std::string Class = "_";
      if (Info.RegClass) {
        const auto Name = Target.registers().registerClassMIRName(*Info.RegClass);
        if (!Name)
          return makeUnexpected(ErrorCode::VerificationError,
                                "virtual register class has no canonical MIR name");
        Class = *Name;
      } else if (Info.Bank) {
        const auto Name = Target.registers().registerBankMIRName(*Info.Bank);
        if (!Name)
          return makeUnexpected(ErrorCode::VerificationError,
                                "virtual register bank has no canonical MIR name");
        Class = *Name;
      }
      if (Info.AllocationHints.size() > 1) {
        return makeUnexpected(
            ErrorCode::Unsupported,
            "fixed LLVM MIR can serialize only one simple preferred-register hint");
      }

      Output.append("  - { id: ");
      Output.append(std::to_string(Slot.TextID));
      Output.append(", class: ");
      Output.append(yamlScalar(Class));
      if (!Info.AllocationHints.empty()) {
        auto Preferred = physicalRegister(Info.AllocationHints.front());
        if (!Preferred)
          return std::unexpected(Preferred.error());
        Output.append(", preferred-register: ");
        Output.append(yamlScalar(*Preferred));
      }
      if (Info.TargetFlags != 0) {
        auto Names = Target.mirText().virtualRegisterFlagNames(Info.TargetFlags);
        if (!Names)
          return std::unexpected(Names.error());
        Output.append(", flags: [ ");
        for (std::size_t Index = 0; Index < Names->size(); ++Index) {
          if (Index != 0)
            Output.append(", ");
          Output.append(yamlScalar((*Names)[Index]));
        }
        Output.append(" ]");
      }
      Output.append(" }\n");
    }
  }

  if (Function.registerInfo().liveIns().empty()) {
    if (Options.Mode == MIPrintMode::Complete)
      Output.append("liveins: []\n");
  } else {
    Output.append("liveins:\n");
    for (const PhysicalRegisterLiveIn &LiveIn : Function.registerInfo().liveIns()) {
      auto Name = physicalRegister(LiveIn.PhysReg);
      if (!Name)
        return std::unexpected(Name.error());
      Output.append("  - { reg: ");
      Output.append(yamlScalar(*Name));
      if (LiveIn.InitialVirtualRegister) {
        const auto Slot = std::ranges::find_if(
            Context.VirtualRegisters, [&LiveIn](const MIVirtualRegisterSlot &Candidate) {
              return Candidate.Reg == *LiveIn.InitialVirtualRegister;
            });
        if (Slot == Context.VirtualRegisters.end()) {
          return makeUnexpected(ErrorCode::VerificationError,
                                "live-in references an unknown virtual register");
        }
        Output.append(", virtual-reg: ");
        Output.append(yamlScalar("%" + std::to_string(Slot->TextID)));
      }
      Output.append(" }\n");
    }
  }

  if (Function.records().hasCalleeSavedRegisters()) {
    Output.append("calleeSavedRegisters: [ ");
    const auto Registers = Function.records().calleeSavedRegisters();
    for (std::size_t Index = 0; Index < Registers.size(); ++Index) {
      if (Index != 0)
        Output.append(", ");
      auto Name = physicalRegister(Registers[Index]);
      if (!Name)
        return std::unexpected(Name.error());
      Output.append(yamlScalar(*Name));
    }
    Output.append(" ]\n");
  }
  return {};
}

Expected<void> MIRDocumentPrinter::printFrameInfo(const MachineFunction &Function,
                                                  const MIParserContext &Context) {
  const MachineFrameState &State = Function.frameInfo().state();
  if (Options.Mode == MIPrintMode::Readable && isDefaultFrameState(State))
    return {};
  if (State.MaxCallFrameSize &&
      *State.MaxCallFrameSize > std::numeric_limits<std::uint32_t>::max()) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "fixed LLVM MIR maxCallFrameSize is limited to 32 bits");
  }
  Output.append("frameInfo:\n");
  const auto Bool = [this](std::string_view Key, bool Value) {
    if (Options.Mode == MIPrintMode::Readable && !Value)
      return;
    Output.append("  ");
    Output.append(Key);
    Output.append(Value ? ": true\n" : ": false\n");
  };
  const auto Unsigned = [this](std::string_view Key, std::uint64_t Value) {
    if (Options.Mode == MIPrintMode::Readable && Value == 0)
      return;
    Output.append("  ");
    Output.append(Key);
    Output.append(": ");
    Output.append(std::to_string(Value));
    Output.push_back('\n');
  };

  Bool("isFrameAddressTaken", State.IsFrameAddressTaken);
  Bool("isReturnAddressTaken", State.IsReturnAddressTaken);
  Bool("hasStackMap", State.HasStackMap);
  Bool("hasPatchPoint", State.HasPatchPoint);
  Unsigned("stackSize", State.StackSize);
  if (Options.Mode == MIPrintMode::Complete || State.OffsetAdjustment != 0) {
    Output.append("  offsetAdjustment: ");
    Output.append(std::to_string(State.OffsetAdjustment));
    Output.push_back('\n');
  }
  Unsigned("maxAlignment", State.MaxAlignment ? State.MaxAlignment->bytes() : 0);
  Bool("adjustsStack", State.AdjustsStack);
  Bool("hasCalls", State.HasCalls);
  if (Options.Mode == MIPrintMode::Complete ||
      State.FramePointerPolicy != MachineFramePointerPolicy::None) {
    Output.append("  framePointerPolicy: ");
    Output.append(getMachineFramePointerPolicyName(State.FramePointerPolicy));
    Output.push_back('\n');
  }
  if (Options.Mode == MIPrintMode::Complete || State.StackProtector) {
    std::string Reference;
    if (State.StackProtector) {
      auto PrintedReference = frameReference(*State.StackProtector, Context);
      if (!PrintedReference)
        return std::unexpected(PrintedReference.error());
      Reference = std::move(*PrintedReference);
    }
    Output.append("  stackProtector: ");
    Output.append(yamlScalar(Reference));
    Output.push_back('\n');
  }
  if (Options.Mode == MIPrintMode::Complete || State.FunctionContext) {
    std::string Reference;
    if (State.FunctionContext) {
      auto PrintedReference = frameReference(*State.FunctionContext, Context);
      if (!PrintedReference)
        return std::unexpected(PrintedReference.error());
      Reference = std::move(*PrintedReference);
    }
    Output.append("  functionContext: ");
    Output.append(yamlScalar(Reference));
    Output.push_back('\n');
  }
  if (Options.Mode == MIPrintMode::Complete || State.MaxCallFrameSize) {
    Unsigned("maxCallFrameSize",
             State.MaxCallFrameSize.value_or(std::numeric_limits<std::uint32_t>::max()));
  }
  Unsigned("cvBytesOfCalleeSavedRegisters", State.CVBytesOfCalleeSavedRegisters);
  Bool("hasOpaqueSPAdjustment", State.HasOpaqueSPAdjustment);
  Bool("hasVAStart", State.HasVAStart);
  Bool("hasMustTailInVarArgFunc", State.HasMustTailInVarArgFunc);
  Bool("hasTailCall", State.HasTailCall);
  Bool("isCalleeSavedInfoValid", State.IsCalleeSavedInfoValid);
  Unsigned("localFrameSize", State.LocalFrameSize);

  const auto PrintPoints =
      [this, &Function](std::string_view Key,
                        std::span<const MachineFrameSaveRestorePoint> Points) -> Expected<void> {
    if (Points.empty())
      return {};
    Output.append("  ");
    Output.append(Key);
    Output.append(":\n");
    for (const MachineFrameSaveRestorePoint &Point : Points) {
      auto Block = blockReference(Function, Point.Block);
      if (!Block)
        return std::unexpected(Block.error());
      Output.append("    - { point: ");
      Output.append(yamlScalar(*Block));
      if (!Point.Registers.empty()) {
        Output.append(", registers: [ ");
        for (std::size_t Index = 0; Index < Point.Registers.size(); ++Index) {
          if (Index != 0)
            Output.append(", ");
          auto Name = physicalRegister(Point.Registers[Index]);
          if (!Name)
            return std::unexpected(Name.error());
          Output.append(yamlScalar(*Name));
        }
        Output.append(" ]");
      }
      Output.append(" }\n");
    }
    return {};
  };
  if (auto Printed = PrintPoints("savePoint", State.SavePoints); !Printed)
    return Printed;
  return PrintPoints("restorePoint", State.RestorePoints);
}

Expected<void> MIRDocumentPrinter::printStackObjects(const MachineFunction &Function,
                                                     const MIParserContext &Context, bool Fixed) {
  const auto &Slots = Fixed ? Context.FixedStackObjects : Context.StackObjects;
  if (Slots.empty()) {
    if (Options.Mode == MIPrintMode::Complete)
      Output.append(Fixed ? "fixedStack: []\n" : "stack: []\n");
    return {};
  }
  Output.append(Fixed ? "fixedStack:\n" : "stack:\n");
  for (const MIFrameObjectSlot &Slot : Slots) {
    const StackObject *Object = Function.frameInfo().object(Slot.Index);
    const StackObjectDesc *Description = Object == nullptr ? nullptr : Object->description();
    if (Description == nullptr)
      return makeUnexpected(ErrorCode::VerificationError, "MIR stack slot is a tombstone");
    if (Description->IsDead || Description->AddressTaken || !Description->Lifetime.empty() ||
        Description->TargetFlags != 0) {
      return makeUnexpected(ErrorCode::Unsupported,
                            "stack object contains state not represented by fixed LLVM MIR");
    }

    std::string_view Type;
    switch (Description->Kind) {
    case StackObjectKind::Default:
    case StackObjectKind::FixedABI:
      Type = "default";
      break;
    case StackObjectKind::FixedSpillSlot:
    case StackObjectKind::SpillSlot:
      Type = "spill-slot";
      break;
    case StackObjectKind::VariableSized:
      Type = "variable-sized";
      break;
    case StackObjectKind::DynamicAlloca:
    case StackObjectKind::CalleeSaved:
    case StackObjectKind::EmergencySpill:
    case StackObjectKind::Target:
    case StackObjectKind::Count:
      return makeUnexpected(ErrorCode::Unsupported,
                            "stack object kind has no fixed LLVM MIR representation");
    }

    Output.append("  - { id: ");
    Output.append(std::to_string(Slot.TextID));
    if (!Fixed && Description->AllocaName) {
      Output.append(", name: ");
      Output.append(yamlScalar(*Description->AllocaName));
    }
    if (Options.Mode == MIPrintMode::Complete || Type != "default") {
      Output.append(", type: ");
      Output.append(Type);
    }
    if (Options.Mode == MIPrintMode::Complete || Description->Offset.value_or(0) != 0) {
      Output.append(", offset: ");
      Output.append(std::to_string(Description->Offset.value_or(0)));
    }
    if (Description->Kind != StackObjectKind::VariableSized) {
      Output.append(", size: ");
      Output.append(std::to_string(Description->Size.value_or(0)));
    }
    if (Options.Mode == MIPrintMode::Complete || Description->Align.bytes() != 1) {
      Output.append(", alignment: ");
      Output.append(std::to_string(Description->Align.bytes()));
    }
    if (Options.Mode == MIPrintMode::Complete || Description->Stack != StackID::defaultStack()) {
      Output.append(", stack-id: ");
      Output.append(getStackIDName(Description->Stack));
    }
    if (Fixed && Description->Kind != StackObjectKind::FixedSpillSlot) {
      if (Options.Mode == MIPrintMode::Complete || Description->IsImmutable)
        Output.append(Description->IsImmutable ? ", isImmutable: true" : ", isImmutable: false");
      if (Options.Mode == MIPrintMode::Complete || Description->IsAliased)
        Output.append(Description->IsAliased ? ", isAliased: true" : ", isAliased: false");
    } else if (!Fixed && (Description->IsImmutable || Description->IsAliased)) {
      return makeUnexpected(ErrorCode::Unsupported,
                            "ordinary stack object immutable/aliased state is not serialized");
    }
    if (Description->CalleeSavedRegister) {
      auto Name = physicalRegister(*Description->CalleeSavedRegister);
      if (!Name)
        return std::unexpected(Name.error());
      Output.append(", callee-saved-register: ");
      Output.append(yamlScalar(*Name));
    }
    if (Options.Mode == MIPrintMode::Complete || !Description->CalleeSavedRestored) {
      Output.append(Description->CalleeSavedRestored ? ", callee-saved-restored: true"
                                                     : ", callee-saved-restored: false");
    }
    if (!Fixed && Description->LocalOffset) {
      Output.append(", local-offset: ");
      Output.append(std::to_string(*Description->LocalOffset));
    }
    if (Description->DebugInfo) {
      const MachineVariableDebugInfo &Debug = *Description->DebugInfo;
      if (Debug.Variable) {
        auto Value = metadataReference(*Debug.Variable, Context);
        if (!Value)
          return std::unexpected(Value.error());
        Output.append(", debug-info-variable: ");
        Output.append(yamlScalar(*Value));
      }
      if (Debug.Expression) {
        auto Value = metadataValue(*Debug.Expression, Context);
        if (!Value)
          return std::unexpected(Value.error());
        Output.append(", debug-info-expression: ");
        Output.append(yamlScalar(*Value));
      }
      if (Debug.Location) {
        auto Value = debugLocation(*Debug.Location, Context);
        if (!Value)
          return std::unexpected(Value.error());
        Output.append(", debug-info-location: ");
        Output.append(yamlScalar(*Value));
      }
    }
    Output.append(" }\n");
  }
  return {};
}

Expected<void> MIRDocumentPrinter::printConstants(const MachineFunction &Function,
                                                  const MIParserContext &Context) {
  if (Context.ConstantPoolItems.empty()) {
    if (Options.Mode == MIPrintMode::Complete)
      Output.append("constants: []\n");
    return {};
  }
  Output.append("constants:\n");
  for (const MIConstantPoolSlot &Slot : Context.ConstantPoolItems) {
    const MachineConstantPoolEntry *Entry = Function.constantPool().entry(Slot.Index);
    const MachineConstantPoolEntryDesc *Description =
        Entry == nullptr ? nullptr : Entry->description();
    if (Description == nullptr)
      return makeUnexpected(ErrorCode::VerificationError, "constant-pool slot is a tombstone");
    if (Description->Merge != ConstantPoolMergeKind::Never ||
        Description->Relocation != ConstantPoolRelocationMode::None ||
        Description->Placement != ConstantPoolPlacement{}) {
      return makeUnexpected(ErrorCode::Unsupported,
                            "constant-pool placement state is not represented by fixed LLVM MIR");
    }

    bool IsTargetSpecific = false;
    Expected<std::string> Value =
        makeUnexpected(ErrorCode::Unsupported, "constant-pool kind is not MIR serializable");
    if (const auto *Generic = std::get_if<GenericConstantData>(&Description->Value)) {
      Value = Generic->toLLVMString();
    } else if (const auto *TargetConstantValue = std::get_if<TargetConstant>(&Description->Value)) {
      IsTargetSpecific = true;
      Value = Target.mirText().printTargetConstant(*TargetConstantValue);
    }
    if (!Value)
      return std::unexpected(Value.error());

    Output.append("  - id: ");
    Output.append(std::to_string(Slot.TextID));
    Output.append("\n    value: ");
    Output.append(yamlScalar(*Value));
    Output.append("\n    alignment: ");
    Output.append(std::to_string(Description->Align.bytes()));
    Output.push_back('\n');
    if (IsTargetSpecific)
      Output.append("    isTargetSpecific: true\n");
  }
  return {};
}

Expected<void> MIRDocumentPrinter::printJumpTable(const MachineFunction &Function,
                                                  const MIParserContext &Context) {
  if (Context.JumpTables.empty())
    return {};
  const MachineJumpTableEntry *FirstEntry =
      Function.jumpTableInfo().table(Context.JumpTables.front().Index);
  const MachineJumpTableDesc *First = FirstEntry == nullptr ? nullptr : FirstEntry->description();
  if (First == nullptr)
    return makeUnexpected(ErrorCode::VerificationError, "jump-table slot is not defined");

  std::string_view Kind;
  std::uint8_t ExpectedSize = 8;
  switch (First->Encoding) {
  case JumpTableEntryEncoding::BlockAddress:
    Kind = "block-address";
    break;
  case JumpTableEntryEncoding::GPRelative64BlockAddress:
    Kind = "gp-rel64-block-address";
    break;
  case JumpTableEntryEncoding::GPRelative32BlockAddress:
    Kind = "gp-rel32-block-address";
    ExpectedSize = 4;
    break;
  case JumpTableEntryEncoding::LabelDifference32:
    Kind = "label-difference32";
    ExpectedSize = 4;
    break;
  case JumpTableEntryEncoding::LabelDifference64:
    Kind = "label-difference64";
    break;
  case JumpTableEntryEncoding::Inline:
    Kind = "inline";
    ExpectedSize = 4;
    break;
  case JumpTableEntryEncoding::Custom32:
    Kind = "custom32";
    ExpectedSize = 4;
    break;
  case JumpTableEntryEncoding::Target:
  case JumpTableEntryEncoding::Count:
    return makeUnexpected(ErrorCode::Unsupported,
                          "target jump-table encoding has no fixed LLVM MIR spelling");
  }

  Output.append("jumpTable:\n  kind: ");
  Output.append(Kind);
  Output.append("\n  entries:\n");
  for (const MIJumpTableSlot &Slot : Context.JumpTables) {
    const MachineJumpTableEntry *Entry = Function.jumpTableInfo().table(Slot.Index);
    const MachineJumpTableDesc *Description = Entry == nullptr ? nullptr : Entry->description();
    if (Description == nullptr)
      return makeUnexpected(ErrorCode::VerificationError, "jump-table slot is not defined");
    if (Description->Encoding != First->Encoding || Description->EntrySize != ExpectedSize ||
        Description->Align.bytes() != ExpectedSize ||
        Description->Placement != JumpTablePlacement{} || Description->TargetEncoding != 0) {
      return makeUnexpected(ErrorCode::Unsupported,
                            "jump-table entries cannot share one fixed LLVM MIR kind");
    }
    Output.append("    - { id: ");
    Output.append(std::to_string(Slot.TextID));
    Output.append(", blocks: [ ");
    for (std::size_t Index = 0; Index < Description->Targets.size(); ++Index) {
      if (Index != 0)
        Output.append(", ");
      if (Description->Targets[Index] == nullptr)
        return makeUnexpected(ErrorCode::VerificationError,
                              "jump-table entry contains a null block");
      auto Block = blockReference(Function, Description->Targets[Index]->stableID());
      if (!Block)
        return std::unexpected(Block.error());
      Output.append(yamlScalar(*Block));
    }
    Output.append(" ] }\n");
  }
  return {};
}

Expected<void> MIRDocumentPrinter::printMetadata(const MachineFunction &Function,
                                                 const MIParserContext &Context) {
  if (Function.metadata().nodes().empty())
    return {};
  Output.append("machineMetadataNodes:\n");
  for (const MachineMetadataNode &Node : Function.metadata().nodes()) {
    auto NodeReference =
        metadataReference(MachineMetadataReference::machineFunction(Node.Slot), Context);
    if (!NodeReference)
      return std::unexpected(NodeReference.error());
    std::string Text = *NodeReference + " = ";
    if (Node.IsDistinct)
      Text.append("distinct ");
    Text.append("!{");
    for (std::size_t Index = 0; Index < Node.Elements.size(); ++Index) {
      if (Index != 0)
        Text.append(", ");
      const MachineMetadataElement &Element = Node.Elements[Index];
      if (Element.isNull()) {
        Text.append("null");
      } else if (const MachineMetadataReference *Reference = Element.reference()) {
        auto Printed = metadataReference(*Reference, Context);
        if (!Printed)
          return std::unexpected(Printed.error());
        Text.append(*Printed);
      } else if (const std::string *String = Element.string()) {
        Text.append("!\"");
        for (const char Character : *String) {
          if (Character == '\\' || Character == '"')
            Text.push_back('\\');
          Text.push_back(Character);
        }
        Text.push_back('"');
      } else {
        return makeUnexpected(ErrorCode::VerificationError,
                              "machine metadata tuple contains an invalid element");
      }
    }
    Text.push_back('}');
    Output.append("  - ");
    Output.append(yamlScalar(Text));
    Output.push_back('\n');
  }
  return {};
}

Expected<void> MIRDocumentPrinter::printTargetState(const MachineFunction &Function) {
  if (!Function.targetState().hasValue())
    return {};
  auto Node = Target.mirText().printFunctionState(Function.targetState());
  if (!Node)
    return std::unexpected(Node.error());
  auto Text = targetNode(*Node);
  if (!Text)
    return std::unexpected(Text.error());
  Output.append("machineFunctionInfo: ");
  Output.append(*Text);
  Output.push_back('\n');
  return {};
}

Expected<void> MIRDocumentPrinter::printFunctionRecords(const MachineFunction &Function,
                                                        const MIParserContext &Context) {
  const MachineFunctionRecords &Records = Function.records();
  if (!Records.entryValues().empty()) {
    Output.append("entry_values:\n");
    for (const MachineEntryValueInfo &Entry : Records.entryValues()) {
      auto RegisterName = physicalRegister(Entry.Reg);
      if (!RegisterName)
        return std::unexpected(RegisterName.error());
      if (!Entry.DebugInfo.Variable || !Entry.DebugInfo.Expression || !Entry.DebugInfo.Location) {
        return makeUnexpected(ErrorCode::Unsupported,
                              "MIR entry value requires complete debug metadata");
      }
      auto Variable = metadataReference(*Entry.DebugInfo.Variable, Context);
      auto Expression = metadataValue(*Entry.DebugInfo.Expression, Context);
      auto Location = debugLocation(*Entry.DebugInfo.Location, Context);
      if (!Variable)
        return std::unexpected(Variable.error());
      if (!Expression)
        return std::unexpected(Expression.error());
      if (!Location)
        return std::unexpected(Location.error());
      Output.append("  - { entry-value-register: ");
      Output.append(yamlScalar(*RegisterName));
      Output.append(", debug-info-variable: ");
      Output.append(yamlScalar(*Variable));
      Output.append(", debug-info-expression: ");
      Output.append(yamlScalar(*Expression));
      Output.append(", debug-info-location: ");
      Output.append(yamlScalar(*Location));
      Output.append(" }\n");
    }
  } else if (Options.Mode == MIPrintMode::Complete) {
    Output.append("entry_values: []\n");
  }

  if (!Records.callSites().empty()) {
    Output.append("callSites:\n");
    for (const MachineCallSiteInfo &CallSite : Records.callSites()) {
      auto Location = instructionLocation(Function, CallSite.CallInstruction);
      if (!Location)
        return std::unexpected(Location.error());
      Output.append("  - { bb: ");
      Output.append(std::to_string(Location->Block));
      Output.append(", offset: ");
      Output.append(std::to_string(Location->Offset));
      if (!CallSite.ForwardedArguments.empty()) {
        Output.append(", fwdArgRegs: [ ");
        for (std::size_t Index = 0; Index < CallSite.ForwardedArguments.size(); ++Index) {
          if (Index != 0)
            Output.append(", ");
          const MachineCallSiteArgument &Argument = CallSite.ForwardedArguments[Index];
          auto RegisterName = physicalRegister(Argument.Reg);
          if (!RegisterName)
            return std::unexpected(RegisterName.error());
          Output.append("{ arg: ");
          Output.append(std::to_string(Argument.ArgumentNumber));
          Output.append(", reg: ");
          Output.append(yamlScalar(*RegisterName));
          Output.append(" }");
        }
        Output.append(" ]");
      }
      if (!CallSite.CalleeTypeIDs.empty()) {
        Output.append(", calleeTypeIds: [ ");
        for (std::size_t Index = 0; Index < CallSite.CalleeTypeIDs.size(); ++Index) {
          if (Index != 0)
            Output.append(", ");
          Output.append(std::to_string(CallSite.CalleeTypeIDs[Index]));
        }
        Output.append(" ]");
      }
      Output.append(" }\n");
    }
  } else if (Options.Mode == MIPrintMode::Complete) {
    Output.append("callSites: []\n");
  }

  if (!Records.debugValueSubstitutions().empty()) {
    Output.append("debugValueSubstitutions:\n");
    for (const MachineDebugValueSubstitution &Substitution : Records.debugValueSubstitutions()) {
      Output.append("  - { srcinst: ");
      Output.append(std::to_string(Substitution.Source.InstructionNumber));
      Output.append(", srcop: ");
      Output.append(std::to_string(Substitution.Source.OperandNumber));
      Output.append(", dstinst: ");
      Output.append(std::to_string(Substitution.Destination.InstructionNumber));
      Output.append(", dstop: ");
      Output.append(std::to_string(Substitution.Destination.OperandNumber));
      Output.append(", subreg: ");
      Output.append(std::to_string(Substitution.Subregister.value()));
      Output.append(" }\n");
    }
  } else if (Options.Mode == MIPrintMode::Complete) {
    Output.append("debugValueSubstitutions: []\n");
  }

  if (!Records.calledGlobals().empty()) {
    Output.append("calledGlobals:\n");
    for (const MachineCalledGlobal &Called : Records.calledGlobals()) {
      auto Location = instructionLocation(Function, Called.CallInstruction);
      if (!Location)
        return std::unexpected(Location.error());
      Output.append("  - { bb: ");
      Output.append(std::to_string(Location->Block));
      Output.append(", offset: ");
      Output.append(std::to_string(Location->Offset));
      Output.append(", callee: ");
      Output.append(yamlScalar(Called.Callee));
      Output.append(", flags: ");
      Output.append(std::to_string(Called.Flags));
      Output.append(" }\n");
    }
  }

  if (!Records.prefetchTargets().empty()) {
    Output.append("prefetch-targets: [ ");
    const auto Targets = Records.prefetchTargets();
    for (std::size_t Index = 0; Index < Targets.size(); ++Index) {
      if (Index != 0)
        Output.append(", ");
      const MachinePrefetchTarget &TargetValue = Targets[Index];
      std::string Text = "bb_id " + std::to_string(TargetValue.Block.BaseID) + ", " +
                         std::to_string(TargetValue.Block.CloneID) + ", " +
                         std::to_string(TargetValue.CallsiteIndex);
      Output.append(yamlScalar(Text));
    }
    Output.append(" ]\n");
  }
  return {};
}

Expected<void> MIRDocumentPrinter::printFunction(const MachineFunction &Function,
                                                 const MIRFunctionDocumentInfo *) {
  if (Function.compilationStatus().state() != MachineCompilationState::Success) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "fixed LLVM MIR cannot serialize this compilation failure state");
  }
  auto Context = createBodyContext(Function);
  if (!Context)
    return std::unexpected(Context.error());

  Output.append("---\nname: ");
  Output.append(yamlScalar(Function.name()));
  Output.push_back('\n');
  const auto Alignment = Function.attributes().alignment();
  if (Options.Mode == MIPrintMode::Complete || Alignment) {
    Output.append("alignment: ");
    Output.append(std::to_string(Alignment ? Alignment->bytes() : 1U));
    Output.push_back('\n');
  }

  const MachineFunctionProperties &Properties = Function.properties();
  const MachineFunctionAttributes &Attributes = Function.attributes();
  printBoolean("exposesReturnsTwice",
               Attributes.has(MachineFunctionAttribute::ExposesReturnsTwice));
  printBoolean("legalized", Properties.has(MachineFunctionProperty::Legalized));
  printBoolean("regBankSelected", Properties.has(MachineFunctionProperty::RegBankSelected));
  printBoolean("selected", Properties.has(MachineFunctionProperty::Selected));
  printBoolean("failedISel", Properties.has(MachineFunctionProperty::FailedISel));
  printBoolean("tracksRegLiveness", Properties.has(MachineFunctionProperty::TracksLiveness));
  printBoolean("hasWinCFI", Attributes.has(MachineFunctionAttribute::HasWinCFI));
  printBoolean("noPhis", Properties.has(MachineFunctionProperty::NoPHIs), true);
  printBoolean("isSSA", Properties.has(MachineFunctionProperty::IsSSA), true);
  printBoolean("noVRegs", Properties.has(MachineFunctionProperty::NoVRegs), true);
  printBoolean("hasFakeUses", Attributes.has(MachineFunctionAttribute::HasFakeUses), true);
  printBoolean("callsEHReturn", Attributes.has(MachineFunctionAttribute::CallsEHReturn));
  printBoolean("callsUnwindInit", Attributes.has(MachineFunctionAttribute::CallsUnwindInit));
  printBoolean("hasEHContTarget", Attributes.has(MachineFunctionAttribute::HasEHContTarget));
  printBoolean("hasEHScopes", Attributes.has(MachineFunctionAttribute::HasEHScopes));
  printBoolean("hasEHFunclets", Attributes.has(MachineFunctionAttribute::HasEHFunclets));
  printBoolean("isOutlined", Attributes.has(MachineFunctionAttribute::IsOutlined));
  printBoolean("debugInstrRef", Attributes.has(MachineFunctionAttribute::DebugInstrRef));
  printBoolean("failsVerification", Properties.has(MachineFunctionProperty::FailsVerification));
  printBoolean("tracksDebugUserValues",
               Attributes.has(MachineFunctionAttribute::TracksDebugUserValues));

  if (auto Printed = printRegisters(Function, *Context); !Printed)
    return Printed;
  if (auto Printed = printFrameInfo(Function, *Context); !Printed)
    return Printed;
  if (auto Printed = printStackObjects(Function, *Context, true); !Printed)
    return Printed;
  if (auto Printed = printStackObjects(Function, *Context, false); !Printed)
    return Printed;
  if (auto Printed = printFunctionRecords(Function, *Context); !Printed)
    return Printed;
  if (auto Printed = printConstants(Function, *Context); !Printed)
    return Printed;
  if (auto Printed = printTargetState(Function); !Printed)
    return Printed;
  if (auto Printed = printJumpTable(Function, *Context); !Printed)
    return Printed;
  if (auto Printed = printMetadata(Function, *Context); !Printed)
    return Printed;

  auto Body =
      printMachineFunctionBody(Function, Target, *Context, MIPrinterOptions{.Mode = Options.Mode});
  if (!Body)
    return std::unexpected(Body.error());
  Output.append("body: |\n");
  appendBlockScalar(Output, *Body, 2);
  Output.append("...\n");
  return {};
}

Expected<std::string> MIRDocumentPrinter::run() {
  auto UnitVerification = Document.unit().verify();
  if (!UnitVerification)
    return std::unexpected(UnitVerification.error());
  if (Document.unit().functionCount() == 0) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "MIR document must contain at least one machine function");
  }

  for (const MIRFunctionDocumentInfo &Info : Document.functionDocumentInfo()) {
    if (Document.unit().function(Info.Function) == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "MIR document info references an unknown machine function");
    }
  }

  for (std::size_t Index = 0; Index < Document.unit().functionCount(); ++Index) {
    const MachineFunction *Function = Document.unit().functionAt(Index);
    if (Function == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "compilation unit contains a null machine function");
    }
    auto ReferenceVerification =
        verifyMIRDocumentReferences(Document, *Function, Options.EmitEmbeddedIR);
    if (!ReferenceVerification)
      return std::unexpected(ReferenceVerification.error());
    auto LayoutVerification = verifyOmittedEmbeddedIRLayout(*Function);
    if (!LayoutVerification)
      return std::unexpected(LayoutVerification.error());
  }

  if (Options.EmitEmbeddedIR && Document.embeddedIR() != nullptr) {
    Output.append("--- |\n");
    appendBlockScalar(Output, Document.embeddedIR()->source().text(), 2);
    Output.append("...\n");
  }
  for (std::size_t Index = 0; Index < Document.unit().functionCount(); ++Index) {
    const MachineFunction *Function = Document.unit().functionAt(Index);
    if (Function == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "compilation unit contains a null machine function");
    if (auto Printed = printFunction(*Function, documentInfo(*Function)); !Printed)
      return std::unexpected(Printed.error());
  }
  return std::move(Output);
}

} // namespace

Expected<std::string> printMIRDocument(const MIRDocument &Document, const TargetDescription &Target,
                                       MIRDocumentPrinterOptions Options) {
  return MIRDocumentPrinter(Document, Target, std::move(Options)).run();
}

} // namespace rcc::mir

namespace rcc::mir {
namespace {

[[nodiscard]] bool isBareNameCharacter(char Character) noexcept {
  return (Character >= 'a' && Character <= 'z') || (Character >= 'A' && Character <= 'Z') ||
         (Character >= '0' && Character <= '9') || Character == '_' || Character == '-' ||
         Character == '.' || Character == '$';
}

[[nodiscard]] std::string quoteName(std::string_view Name) {
  if (!Name.empty() && std::ranges::all_of(Name, isBareNameCharacter))
    return std::string(Name);

  constexpr std::string_view Hex = "0123456789ABCDEF";
  std::string Result;
  Result.reserve(Name.size() + 2U);
  Result.push_back('"');
  for (const char RawCharacter : Name) {
    const auto Character = static_cast<unsigned char>(RawCharacter);
    if (Character >= 0x20U && Character <= 0x7EU && Character != '"' && Character != '\\') {
      Result.push_back(static_cast<char>(Character));
      continue;
    }
    Result.push_back('\\');
    Result.push_back(Hex[Character >> 4U]);
    Result.push_back(Hex[Character & 0xFU]);
  }
  Result.push_back('"');
  return Result;
}

[[nodiscard]] std::string quoteString(std::string_view Value) {
  std::string Result = quoteName(Value);
  if (!Result.empty() && Result.front() == '"')
    return Result;
  Result.insert(Result.begin(), '"');
  Result.push_back('"');
  return Result;
}

[[nodiscard]] std::string hexadecimalWords(std::span<const std::uint64_t> Words,
                                           std::uint32_t BitWidth) {
  constexpr std::string_view Hex = "0123456789ABCDEF";
  const std::size_t DigitCount = (static_cast<std::size_t>(BitWidth) + 3U) / 4U;
  std::string Result(DigitCount, '0');
  for (std::size_t Digit = 0; Digit < DigitCount; ++Digit) {
    const std::size_t BitOffset = Digit * 4U;
    const std::uint64_t Nibble = (Words[BitOffset / 64U] >> (BitOffset % 64U)) & 0xFU;
    Result[DigitCount - Digit - 1U] = Hex[Nibble];
  }
  return Result;
}

[[nodiscard]] std::string_view integerPredicateName(IntegerPredicate Predicate) noexcept {
  switch (Predicate) {
  case IntegerPredicate::Equal:
    return "eq";
  case IntegerPredicate::NotEqual:
    return "ne";
  case IntegerPredicate::SignedGreaterThan:
    return "sgt";
  case IntegerPredicate::SignedGreaterEqual:
    return "sge";
  case IntegerPredicate::SignedLessThan:
    return "slt";
  case IntegerPredicate::SignedLessEqual:
    return "sle";
  case IntegerPredicate::UnsignedGreaterThan:
    return "ugt";
  case IntegerPredicate::UnsignedGreaterEqual:
    return "uge";
  case IntegerPredicate::UnsignedLessThan:
    return "ult";
  case IntegerPredicate::UnsignedLessEqual:
    return "ule";
  case IntegerPredicate::Count:
    return {};
  }
  return {};
}

[[nodiscard]] std::string_view floatingPredicateName(FloatingPredicate Predicate) noexcept {
  switch (Predicate) {
  case FloatingPredicate::AlwaysFalse:
    return "false";
  case FloatingPredicate::OrderedEqual:
    return "oeq";
  case FloatingPredicate::OrderedGreaterThan:
    return "ogt";
  case FloatingPredicate::OrderedGreaterEqual:
    return "oge";
  case FloatingPredicate::OrderedLessThan:
    return "olt";
  case FloatingPredicate::OrderedLessEqual:
    return "ole";
  case FloatingPredicate::OrderedNotEqual:
    return "one";
  case FloatingPredicate::Ordered:
    return "ord";
  case FloatingPredicate::Unordered:
    return "uno";
  case FloatingPredicate::UnorderedEqual:
    return "ueq";
  case FloatingPredicate::UnorderedGreaterThan:
    return "ugt";
  case FloatingPredicate::UnorderedGreaterEqual:
    return "uge";
  case FloatingPredicate::UnorderedLessThan:
    return "ult";
  case FloatingPredicate::UnorderedLessEqual:
    return "ule";
  case FloatingPredicate::UnorderedNotEqual:
    return "une";
  case FloatingPredicate::AlwaysTrue:
    return "true";
  case FloatingPredicate::Count:
    return {};
  }
  return {};
}

[[nodiscard]] std::string_view atomicOrderingName(AtomicOrdering Ordering) noexcept {
  switch (Ordering) {
  case AtomicOrdering::Unordered:
    return "unordered";
  case AtomicOrdering::Monotonic:
    return "monotonic";
  case AtomicOrdering::Acquire:
    return "acquire";
  case AtomicOrdering::Release:
    return "release";
  case AtomicOrdering::AcquireRelease:
    return "acq_rel";
  case AtomicOrdering::SequentiallyConsistent:
    return "seq_cst";
  case AtomicOrdering::NotAtomic:
    return {};
  }
  return {};
}

[[nodiscard]] Expected<std::string> llvmMIRScalarLowLevelType(LLT Type) {
  if (Type.isToken())
    return "s0";
  if (Type.isAnyScalar())
    return "s" + std::to_string(Type.scalarBitWidth());
  if (Type.isInteger())
    return "i" + std::to_string(Type.scalarBitWidth());
  if (Type.isPointer()) {
    const auto AddressSpace = Type.addressSpace();
    if (!AddressSpace) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "pointer low-level type has no address space");
    }
    return "p" + std::to_string(*AddressSpace);
  }
  if (!Type.isFloat()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "invalid scalar low-level type cannot be printed as LLVM MIR");
  }

  const auto Semantics = Type.floatSemantics();
  if (!Semantics) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "floating low-level type has no floating-point semantics");
  }
  switch (*Semantics) {
  case FloatSemantics::IEEEHalf:
    return "f16";
  case FloatSemantics::BFloat:
    return "bf16";
  case FloatSemantics::IEEESingle:
    return "f32";
  case FloatSemantics::IEEEDouble:
    return "f64";
  case FloatSemantics::IEEEQuad:
    return "f128";
  case FloatSemantics::PPCDoubleDouble:
  case FloatSemantics::Float8E5M2:
  case FloatSemantics::Float8E5M2FNUZ:
  case FloatSemantics::Float8E4M3:
  case FloatSemantics::Float8E4M3FN:
  case FloatSemantics::Float8E4M3FNUZ:
  case FloatSemantics::Float8E4M3B11FNUZ:
  case FloatSemantics::Float8E3M4:
  case FloatSemantics::FloatTF32:
  case FloatSemantics::Float8E8M0FNU:
  case FloatSemantics::Float6E3M2FN:
  case FloatSemantics::Float6E2M3FN:
  case FloatSemantics::Float4E2M1FN:
  case FloatSemantics::X87DoubleExtended:
  case FloatSemantics::Count:
    return makeUnexpected(ErrorCode::Unsupported,
                          "fixed LLVM MIR cannot spell this floating low-level type");
  }
  return makeUnexpected(ErrorCode::InternalError, "floating low-level type is unhandled");
}

[[nodiscard]] Expected<std::string> llvmMIRLowLevelType(LLT Type) {
  if (!Type.isValid()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "invalid low-level type cannot be printed as LLVM MIR");
  }
  if (!Type.isVector())
    return llvmMIRScalarLowLevelType(Type);

  const auto ElementType = Type.scalarElementType();
  if (!ElementType) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "vector low-level type has no scalar element type");
  }
  auto Scalar = llvmMIRScalarLowLevelType(*ElementType);
  if (!Scalar)
    return std::unexpected(Scalar.error());

  std::string Result = "<";
  if (Type.isScalableVector())
    Result.append("vscale x ");
  Result.append(std::to_string(Type.minimumElementCount()));
  Result.append(" x ");
  Result.append(*Scalar);
  Result.push_back('>');
  return Result;
}

class BodyPrinter final {
public:
  BodyPrinter(const MachineFunction &Function, const TargetDescription &Target,
              const MIParserContext &Context, MIPrinterOptions Options)
      : Function(Function), Target(Target), Context(Context), Options(Options) {}

  [[nodiscard]] Expected<std::string> run();

private:
  [[nodiscard]] Expected<void> initialize();
  [[nodiscard]] Expected<void> printBlock(const MachineBasicBlock &Block);
  [[nodiscard]] Expected<void> printInstruction(const MachineInstr &Instruction);
  [[nodiscard]] Expected<void> printOperand(const MachineOperand &Operand, bool IsExplicitDef,
                                            bool PrintVirtualType = true);
  [[nodiscard]] Expected<void> printInlineAsmExtraInfo(const MachineOperand &Operand);
  [[nodiscard]] Expected<std::size_t> printInlineAsmOperandFlag(const MachineOperand &Operand);
  [[nodiscard]] Expected<void> printRegisterOperand(const MachineOperand &Operand,
                                                    bool IsExplicitDef, bool PrintVirtualType);
  [[nodiscard]] Expected<void> printTargetFlags(std::uint32_t Flags);
  [[nodiscard]] Expected<void> printMetadataReference(MachineMetadataReference Reference);
  [[nodiscard]] Expected<void> printDILocation(const MachineDILocation &Location,
                                               std::size_t FrameIndex);
  [[nodiscard]] Expected<void> printCFIInstruction(CFIInstructionIndex Index);
  [[nodiscard]] Expected<void> printMemoryOperand(const MachineMemOperand &Operand);
  [[nodiscard]] Expected<void> printMemoryBase(const MemoryBase &Base, std::int64_t Offset);

  [[nodiscard]] Expected<std::uint32_t> blockID(const MachineBasicBlock &Block) const;
  [[nodiscard]] Expected<std::uint32_t> virtualRegisterID(Register Reg) const;
  [[nodiscard]] Expected<std::uint32_t> frameID(FrameIndex Index) const;
  [[nodiscard]] Expected<std::uint32_t> constantPoolID(ConstantPoolIndex Index) const;
  [[nodiscard]] Expected<std::uint32_t> jumpTableID(JumpTableIndex Index) const;
  [[nodiscard]] Expected<std::uint32_t> metadataID(MachineMetadataReference Reference) const;
  [[nodiscard]] Expected<void> appendPhysicalRegister(PhysicalRegister Register);
  void appendBlockReference(const MachineBasicBlock &Block);
  void appendOffset(std::int64_t Offset);

  const MachineFunction &Function;
  const TargetDescription &Target;
  const MIParserContext &Context;
  MIPrinterOptions Options;
  std::string Output;
  std::unordered_map<const MachineBasicBlock *, std::uint32_t> BlockIDs;
};

Expected<void> BodyPrinter::initialize() {
  std::uint64_t NextID = 0;
  for (const MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
       Block = Block->next()) {
    if (NextID > std::numeric_limits<std::uint32_t>::max()) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "machine function has too many blocks to print");
    }
    BlockIDs.emplace(Block, static_cast<std::uint32_t>(NextID));
    ++NextID;
  }
  return {};
}

Expected<std::uint32_t> BodyPrinter::blockID(const MachineBasicBlock &Block) const {
  const auto Found = BlockIDs.find(&Block);
  if (Found == BlockIDs.end())
    return makeUnexpected(ErrorCode::VerificationError,
                          "machine operand references an unlinked basic block");
  return Found->second;
}

Expected<std::uint32_t> BodyPrinter::virtualRegisterID(Register Reg) const {
  if (!Context.VirtualRegisters.empty()) {
    const auto Found =
        std::ranges::find_if(Context.VirtualRegisters,
                             [Reg](const MIVirtualRegisterSlot &Slot) { return Slot.Reg == Reg; });
    if (Found == Context.VirtualRegisters.end()) {
      return makeUnexpected(ErrorCode::VerificationError, "virtual register has no MIR text slot");
    }
    return Found->TextID;
  }
  const auto VirtualIndex = Reg.virtualIndex();
  if (!VirtualIndex)
    return makeUnexpected(ErrorCode::VerificationError, "virtual register has no numeric index");
  return *VirtualIndex;
}

Expected<std::uint32_t> BodyPrinter::frameID(FrameIndex Index) const {
  const auto InStack = std::ranges::find_if(
      Context.StackObjects, [Index](const MIFrameObjectSlot &Slot) { return Slot.Index == Index; });
  if (InStack != Context.StackObjects.end())
    return InStack->TextID;
  const auto InFixed =
      std::ranges::find_if(Context.FixedStackObjects,
                           [Index](const MIFrameObjectSlot &Slot) { return Slot.Index == Index; });
  if (InFixed != Context.FixedStackObjects.end())
    return InFixed->TextID;
  return makeUnexpected(ErrorCode::VerificationError, "frame index has no MIR text slot");
}

Expected<std::uint32_t> BodyPrinter::constantPoolID(ConstantPoolIndex Index) const {
  const auto Found =
      std::ranges::find_if(Context.ConstantPoolItems,
                           [Index](const MIConstantPoolSlot &Slot) { return Slot.Index == Index; });
  if (Found == Context.ConstantPoolItems.end()) {
    return makeUnexpected(ErrorCode::VerificationError, "constant-pool index has no MIR text slot");
  }
  return Found->TextID;
}

Expected<std::uint32_t> BodyPrinter::jumpTableID(JumpTableIndex Index) const {
  const auto Found = std::ranges::find_if(
      Context.JumpTables, [Index](const MIJumpTableSlot &Slot) { return Slot.Index == Index; });
  if (Found == Context.JumpTables.end())
    return makeUnexpected(ErrorCode::VerificationError, "jump-table index has no MIR text slot");
  return Found->TextID;
}

Expected<std::uint32_t> BodyPrinter::metadataID(MachineMetadataReference Reference) const {
  const auto Found =
      std::ranges::find_if(Context.Metadata, [Reference](const MIMetadataSlot &Slot) {
        return Slot.Reference == Reference;
      });
  if (Found != Context.Metadata.end())
    return Found->TextID;
  if (Reference.scope() == MachineMetadataScope::MachineFunction)
    return Reference.slot();
  return makeUnexpected(ErrorCode::VerificationError,
                        "embedded metadata reference has no MIR text slot");
}

Expected<void> BodyPrinter::appendPhysicalRegister(PhysicalRegister Register) {
  const auto Name = Target.registers().physicalRegisterMIRName(Register);
  if (!Name)
    return makeUnexpected(ErrorCode::VerificationError, "physical register has no MIR name");
  Output.push_back('$');
  Output.append(*Name);
  return {};
}

void BodyPrinter::appendBlockReference(const MachineBasicBlock &Block) {
  Output.append("%bb.");
  Output.append(std::to_string(BlockIDs.at(&Block)));
}

void BodyPrinter::appendOffset(std::int64_t Offset) {
  if (Offset == 0)
    return;
  if (Offset > 0) {
    Output.append(" + ");
    Output.append(std::to_string(Offset));
    return;
  }
  Output.append(" - ");
  const std::uint64_t Magnitude = Offset == std::numeric_limits<std::int64_t>::min()
                                      ? std::uint64_t{1} << 63U
                                      : static_cast<std::uint64_t>(-Offset);
  Output.append(std::to_string(Magnitude));
}

Expected<void> BodyPrinter::printTargetFlags(std::uint32_t Flags) {
  if (Flags == 0)
    return {};
  auto Names = Target.mirText().operandFlagNames(Flags);
  if (!Names)
    return std::unexpected(Names.error());
  Output.append("target-flags(");
  for (std::size_t Index = 0; Index < Names->size(); ++Index) {
    if (Index != 0)
      Output.append(", ");
    Output.append((*Names)[Index]);
  }
  Output.append(") ");
  return {};
}

Expected<void> BodyPrinter::printMetadataReference(MachineMetadataReference Reference) {
  auto ID = metadataID(Reference);
  if (!ID)
    return std::unexpected(ID.error());
  Output.push_back('!');
  Output.append(std::to_string(*ID));
  return {};
}

Expected<void> BodyPrinter::printDILocation(const MachineDILocation &Location,
                                            std::size_t FrameIndex) {
  const auto Frames = Location.inlineChain();
  if (FrameIndex >= Frames.size()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "inline DILocation has an invalid frame index");
  }
  const MachineDILocationFrame &Frame = Frames[FrameIndex];
  Output.append("!DILocation(line: ");
  Output.append(std::to_string(Frame.Line));
  if (Options.Mode == MIPrintMode::Complete || Frame.Column != 0) {
    Output.append(", column: ");
    Output.append(std::to_string(Frame.Column));
  }
  Output.append(", scope: ");
  if (auto Printed = printMetadataReference(Frame.Scope); !Printed)
    return Printed;
  if (FrameIndex + 1U < Frames.size()) {
    Output.append(", inlinedAt: ");
    if (auto Printed = printDILocation(Location, FrameIndex + 1U); !Printed)
      return Printed;
  } else if (Location.terminalInlinedAt()) {
    Output.append(", inlinedAt: ");
    if (auto Printed = printMetadataReference(*Location.terminalInlinedAt()); !Printed) {
      return Printed;
    }
  }
  if (Options.Mode == MIPrintMode::Complete || Frame.IsImplicitCode) {
    Output.append(", isImplicitCode: ");
    Output.append(Frame.IsImplicitCode ? "true" : "false");
  }
  if (Options.Mode == MIPrintMode::Complete || Frame.AtomGroup != 0) {
    Output.append(", atomGroup: ");
    Output.append(std::to_string(Frame.AtomGroup));
  }
  if (Options.Mode == MIPrintMode::Complete || Frame.AtomRank != 0) {
    Output.append(", atomRank: ");
    Output.append(std::to_string(Frame.AtomRank));
  }
  Output.push_back(')');
  return {};
}

Expected<void> BodyPrinter::printRegisterOperand(const MachineOperand &Operand, bool IsExplicitDef,
                                                 bool PrintVirtualType) {
  const RegisterOperandFlags Flags = Operand.registerFlags();
  if (Flags.has(RegisterOperandFlag::Implicit) && Flags.has(RegisterOperandFlag::Define)) {
    Output.append("implicit-def ");
  } else {
    if (!IsExplicitDef && Flags.has(RegisterOperandFlag::Define))
      Output.append("def ");
    if (Flags.has(RegisterOperandFlag::Implicit))
      Output.append("implicit ");
  }
  if (Flags.has(RegisterOperandFlag::Dead))
    Output.append("dead ");
  if (Flags.has(RegisterOperandFlag::Kill))
    Output.append("killed ");
  if (Flags.has(RegisterOperandFlag::Undef))
    Output.append("undef ");
  if (Flags.has(RegisterOperandFlag::InternalRead))
    Output.append("internal ");
  if (Flags.has(RegisterOperandFlag::EarlyClobber))
    Output.append("early-clobber ");
  if (Flags.has(RegisterOperandFlag::Debug))
    Output.append("debug-use ");
  if (Flags.has(RegisterOperandFlag::Renamable))
    Output.append("renamable ");

  const auto RegisterValue = Operand.registerValue();
  if (!RegisterValue) {
    return makeUnexpected(ErrorCode::VerificationError, "register operand has no register value");
  }
  const Register Reg = *RegisterValue;
  if (Reg.isNoRegister()) {
    Output.append("$noreg");
  } else if (Reg.isPhysical()) {
    const auto Physical = Reg.physicalRegister();
    if (!Physical) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "physical register has no physical-register value");
    }
    auto Printed = appendPhysicalRegister(*Physical);
    if (!Printed)
      return Printed;
  } else {
    auto Info = Function.registerInfo().virtualRegisterInfo(Reg);
    if (!Info)
      return std::unexpected(Info.error());
    const VirtualRegisterInfo &RegisterInfo = Info->get();
    Output.push_back('%');
    if (RegisterInfo.Name) {
      Output.append(*RegisterInfo.Name);
    } else {
      auto VirtualIndex = virtualRegisterID(Reg);
      if (!VirtualIndex)
        return std::unexpected(VirtualIndex.error());
      Output.append(std::to_string(*VirtualIndex));
    }
    if (Operand.subReg().value() != 0) {
      const SubRegIndexDesc *SubReg = Target.registers().subRegIndex(Operand.subReg());
      if (SubReg == nullptr)
        return makeUnexpected(ErrorCode::VerificationError,
                              "virtual register has an unknown subregister");
      Output.push_back('.');
      Output.append(SubReg->Name);
    }

    const bool PrintRegisterConstraint =
        IsExplicitDef || Function.registerInfo().getUniqueVRegDef(Reg) == nullptr;
    if (PrintRegisterConstraint && RegisterInfo.RegClass) {
      const auto Name = Target.registers().registerClassMIRName(*RegisterInfo.RegClass);
      if (!Name)
        return makeUnexpected(ErrorCode::VerificationError,
                              "virtual register class has no MIR name");
      Output.push_back(':');
      Output.append(*Name);
    } else if (PrintRegisterConstraint && RegisterInfo.Bank) {
      const auto Name = Target.registers().registerBankMIRName(*RegisterInfo.Bank);
      if (!Name)
        return makeUnexpected(ErrorCode::VerificationError,
                              "virtual register bank has no MIR name");
      Output.push_back(':');
      Output.append(*Name);
    } else if (PrintRegisterConstraint && RegisterInfo.Type) {
      Output.append(":_");
    }
    if (PrintVirtualType && RegisterInfo.Type) {
      auto Type = llvmMIRLowLevelType(*RegisterInfo.Type);
      if (!Type)
        return std::unexpected(Type.error());
      Output.push_back('(');
      Output.append(*Type);
      Output.push_back(')');
    }
  }

  if (Operand.tiedOperandIndex() && Operand.isUse()) {
    Output.append("(tied-def ");
    Output.append(std::to_string(*Operand.tiedOperandIndex()));
    Output.push_back(')');
  }
  return {};
}

Expected<void> BodyPrinter::printCFIInstruction(CFIInstructionIndex Index) {
  const MachineCFIInstruction *Instruction = Function.cfiInfo().instruction(Index);
  if (Instruction == nullptr)
    return makeUnexpected(ErrorCode::VerificationError,
                          "CFI operand references an unknown instruction");
  const auto PrintRegister = [this](DwarfRegisterNumber Dwarf) -> Expected<void> {
    const auto Register = Target.mirText().physicalRegister(Dwarf.value());
    if (!Register) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "CFI instruction has no canonical physical register");
    }
    return appendPhysicalRegister(*Register);
  };
  const auto PrintRegisterOffset =
      [this, &PrintRegister](std::string_view Name,
                             const MachineCFIRegisterOffsetRule &Rule) -> Expected<void> {
    Output.append(Name);
    Output.push_back(' ');
    if (auto Printed = PrintRegister(Rule.Register); !Printed)
      return Printed;
    Output.append(", ");
    Output.append(std::to_string(Rule.Offset));
    return {};
  };

  switch (Instruction->operation()) {
  case MachineCFIOperation::SameValue: {
    Output.append("same_value ");
    return PrintRegister(Instruction->getIf<MachineCFIRegisterRule>()->Register);
  }
  case MachineCFIOperation::RememberState:
    Output.append("remember_state");
    return {};
  case MachineCFIOperation::RestoreState:
    Output.append("restore_state");
    return {};
  case MachineCFIOperation::Offset:
    return PrintRegisterOffset("offset", *Instruction->getIf<MachineCFIRegisterOffsetRule>());
  case MachineCFIOperation::LLVMDefAddressSpaceCFA: {
    const auto &Rule = *Instruction->getIf<MachineCFIAddressSpaceCFARule>();
    Output.append("llvm_def_aspace_cfa ");
    if (auto Printed = PrintRegister(Rule.Register); !Printed)
      return Printed;
    Output.append(", ");
    Output.append(std::to_string(Rule.Offset));
    Output.append(", ");
    Output.append(std::to_string(Rule.AddressSpace));
    return {};
  }
  case MachineCFIOperation::DefCFARegister:
    Output.append("def_cfa_register ");
    return PrintRegister(Instruction->getIf<MachineCFIRegisterRule>()->Register);
  case MachineCFIOperation::DefCFAOffset:
    Output.append("def_cfa_offset ");
    Output.append(std::to_string(Instruction->getIf<MachineCFIOffsetRule>()->Offset));
    return {};
  case MachineCFIOperation::DefCFA:
    return PrintRegisterOffset("def_cfa", *Instruction->getIf<MachineCFIRegisterOffsetRule>());
  case MachineCFIOperation::RelativeOffset:
    return PrintRegisterOffset("rel_offset", *Instruction->getIf<MachineCFIRegisterOffsetRule>());
  case MachineCFIOperation::AdjustCFAOffset:
    Output.append("adjust_cfa_offset ");
    Output.append(std::to_string(Instruction->getIf<MachineCFIOffsetRule>()->Offset));
    return {};
  case MachineCFIOperation::Escape: {
    Output.append("escape ");
    constexpr std::string_view Hex = "0123456789ABCDEF";
    const auto &Bytes = Instruction->getIf<MachineCFIEscapeRule>()->Bytes;
    for (std::size_t ByteIndex = 0; ByteIndex < Bytes.size(); ++ByteIndex) {
      if (ByteIndex != 0)
        Output.append(", ");
      Output.append("0x");
      Output.push_back(Hex[Bytes[ByteIndex] >> 4U]);
      Output.push_back(Hex[Bytes[ByteIndex] & 0xFU]);
    }
    return {};
  }
  case MachineCFIOperation::Restore:
    Output.append("restore ");
    return PrintRegister(Instruction->getIf<MachineCFIRegisterRule>()->Register);
  case MachineCFIOperation::Undefined:
    Output.append("undefined ");
    return PrintRegister(Instruction->getIf<MachineCFIRegisterRule>()->Register);
  case MachineCFIOperation::Register: {
    const auto &Rule = *Instruction->getIf<MachineCFIRegisterRegisterRule>();
    Output.append("register ");
    if (auto Printed = PrintRegister(Rule.Register); !Printed)
      return Printed;
    Output.append(", ");
    return PrintRegister(Rule.SourceRegister);
  }
  case MachineCFIOperation::WindowSave:
    Output.append("window_save");
    return {};
  case MachineCFIOperation::NegateReturnAddressState:
    Output.append("negate_ra_sign_state");
    return {};
  case MachineCFIOperation::NegateReturnAddressStateWithPC:
    Output.append("negate_ra_sign_state_with_pc");
    return {};
  case MachineCFIOperation::GNUArgsSize:
  case MachineCFIOperation::Label:
  case MachineCFIOperation::ValueOffset:
    return makeUnexpected(
        ErrorCode::Unsupported,
        "CFI operation has no textual representation in the fixed LLVM MIR baseline");
  case MachineCFIOperation::LLVMRegisterPair: {
    const auto &Rule = *Instruction->getIf<MachineCFIRegisterPairRule>();
    Output.append("llvm_register_pair ");
    if (auto Printed = PrintRegister(Rule.Register); !Printed)
      return Printed;
    Output.append(", ");
    if (auto Printed = PrintRegister(Rule.FirstRegister); !Printed)
      return Printed;
    Output.append(", ");
    Output.append(std::to_string(Rule.FirstSizeInBits));
    Output.append(", ");
    if (auto Printed = PrintRegister(Rule.SecondRegister); !Printed)
      return Printed;
    Output.append(", ");
    Output.append(std::to_string(Rule.SecondSizeInBits));
    return {};
  }
  case MachineCFIOperation::LLVMVectorRegisters: {
    const auto &Rule = *Instruction->getIf<MachineCFIVectorRegistersRule>();
    Output.append("llvm_vector_registers ");
    if (auto Printed = PrintRegister(Rule.Register); !Printed)
      return Printed;
    for (const MachineCFIVectorRegisterLane &Lane : Rule.VectorRegisters) {
      Output.append(", ");
      if (auto Printed = PrintRegister(Lane.Register); !Printed)
        return Printed;
      Output.append(", ");
      Output.append(std::to_string(Lane.Lane));
      Output.append(", ");
      Output.append(std::to_string(Lane.SizeInBits));
    }
    return {};
  }
  case MachineCFIOperation::LLVMVectorOffset: {
    const auto &Rule = *Instruction->getIf<MachineCFIVectorOffsetRule>();
    Output.append("llvm_vector_offset ");
    if (auto Printed = PrintRegister(Rule.Register); !Printed)
      return Printed;
    Output.append(", ");
    Output.append(std::to_string(Rule.RegisterSizeInBits));
    Output.append(", ");
    if (auto Printed = PrintRegister(Rule.MaskRegister); !Printed)
      return Printed;
    Output.append(", ");
    Output.append(std::to_string(Rule.MaskRegisterSizeInBits));
    Output.append(", ");
    Output.append(std::to_string(Rule.Offset));
    return {};
  }
  case MachineCFIOperation::LLVMVectorRegisterMask: {
    const auto &Rule = *Instruction->getIf<MachineCFIVectorRegisterMaskRule>();
    Output.append("llvm_vector_register_mask ");
    if (auto Printed = PrintRegister(Rule.Register); !Printed)
      return Printed;
    Output.append(", ");
    if (auto Printed = PrintRegister(Rule.SpillRegister); !Printed)
      return Printed;
    Output.append(", ");
    Output.append(std::to_string(Rule.SpillRegisterLaneSizeInBits));
    Output.append(", ");
    if (auto Printed = PrintRegister(Rule.MaskRegister); !Printed)
      return Printed;
    Output.append(", ");
    Output.append(std::to_string(Rule.MaskRegisterSizeInBits));
    return {};
  }
  case MachineCFIOperation::Count:
    return makeUnexpected(ErrorCode::VerificationError, "CFI instruction has an invalid operation");
  }
  return makeUnexpected(ErrorCode::InternalError, "CFI printer did not handle an operation");
}

Expected<void> BodyPrinter::printOperand(const MachineOperand &Operand, bool IsExplicitDef,
                                         bool PrintVirtualType) {
  if (Operand.isRegister())
    return printRegisterOperand(Operand, IsExplicitDef, PrintVirtualType);
  if (auto Flags = printTargetFlags(Operand.targetFlags()); !Flags)
    return Flags;

  switch (Operand.kind()) {
  case MachineOperand::Kind::Register:
    return makeUnexpected(ErrorCode::InternalError,
                          "register operand reached the non-register printer");
  case MachineOperand::Kind::Immediate:
    Output.append(std::to_string(*Operand.getIf<std::int64_t>()));
    return {};
  case MachineOperand::Kind::IntegerImmediate: {
    const APInt &Value = Operand.getIf<IntegerImmediateOperand>()->Value;
    Output.push_back('i');
    Output.append(std::to_string(Value.bitWidth()));
    Output.push_back(' ');
    if (Value.bitWidth() == 1U)
      Output.append(Value.isZero() ? "false" : "true");
    else
      Output.append(Value.toSignedDecimal());
    return {};
  }
  case MachineOperand::Kind::FloatingImmediate: {
    const APFloat &Bits = Operand.getIf<FloatingImmediateOperand>()->Bits;
    auto Constant = GenericConstantData::floating(Bits);
    if (!Constant)
      return std::unexpected(Constant.error());
    auto Text = Constant->toLLVMString();
    if (!Text)
      return std::unexpected(Text.error());
    Output.append(*Text);
    return {};
  }
  case MachineOperand::Kind::BasicBlock: {
    const MachineBasicBlock *Block = Operand.getIf<BasicBlockOperand>()->Block;
    if (Block == nullptr || !BlockIDs.contains(Block)) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "basic-block operand references an unlinked block");
    }
    appendBlockReference(*Block);
    return {};
  }
  case MachineOperand::Kind::FrameIndex: {
    const auto &Frame = *Operand.getIf<FrameIndexOperand>();
    auto ID = frameID(Frame.Index);
    if (!ID)
      return std::unexpected(ID.error());
    const auto Stack = std::ranges::find_if(
        Context.StackObjects,
        [Index = Frame.Index](const MIFrameObjectSlot &Slot) { return Slot.Index == Index; });
    if (Stack != Context.StackObjects.end()) {
      Output.append("%stack.");
      Output.append(std::to_string(*ID));
      if (Stack->Name) {
        Output.push_back('.');
        Output.append(quoteName(*Stack->Name));
      }
    } else {
      Output.append("%fixed-stack.");
      Output.append(std::to_string(*ID));
    }
    appendOffset(Frame.Offset);
    return {};
  }
  case MachineOperand::Kind::ConstantPoolIndex: {
    const auto &Constant = *Operand.getIf<ConstantPoolOperand>();
    auto ID = constantPoolID(Constant.Index);
    if (!ID)
      return std::unexpected(ID.error());
    Output.append("%const.");
    Output.append(std::to_string(*ID));
    appendOffset(Constant.Offset);
    return {};
  }
  case MachineOperand::Kind::JumpTableIndex: {
    auto ID = jumpTableID(Operand.getIf<JumpTableOperand>()->Index);
    if (!ID)
      return std::unexpected(ID.error());
    Output.append("%jump-table.");
    Output.append(std::to_string(*ID));
    return {};
  }
  case MachineOperand::Kind::GlobalSymbol: {
    const auto &Global = *Operand.getIf<GlobalSymbolOperand>();
    Output.push_back('@');
    Output.append(quoteName(Global.Name));
    appendOffset(Global.Offset);
    return {};
  }
  case MachineOperand::Kind::ExternalSymbol: {
    const auto &External = *Operand.getIf<ExternalSymbolOperand>();
    Output.push_back('&');
    Output.append(quoteName(External.Name));
    appendOffset(External.Offset);
    return {};
  }
  case MachineOperand::Kind::BlockAddress: {
    const auto &Address = *Operand.getIf<BlockAddressOperand>();
    Output.append("blockaddress(@");
    Output.append(quoteName(Address.FunctionName));
    Output.append(", %ir-block.");
    Output.append(quoteName(Address.BlockName));
    Output.push_back(')');
    appendOffset(Address.Offset);
    return {};
  }
  case MachineOperand::Kind::RegisterMask: {
    const RegisterMask &Mask = *Operand.getIf<RegisterMaskOperand>()->Mask;
    if (const auto Name = Target.mirText().registerMaskName(Mask)) {
      Output.append(*Name);
      return {};
    }
    Output.append("CustomRegMask(");
    bool First = true;
    for (const PhysicalRegisterDesc &Register : Target.registers().physicalRegisters()) {
      if (!Mask.preserves(Register.Reg))
        continue;
      if (!First)
        Output.append(", ");
      First = false;
      if (auto Printed = appendPhysicalRegister(Register.Reg); !Printed)
        return Printed;
    }
    Output.push_back(')');
    return {};
  }
  case MachineOperand::Kind::LiveOutRegisterMask: {
    const RegisterMask &Mask = *Operand.getIf<LiveOutRegisterMaskOperand>()->Mask;
    Output.append("liveout(");
    bool First = true;
    for (const PhysicalRegisterDesc &Register : Target.registers().physicalRegisters()) {
      if (!Mask.preserves(Register.Reg))
        continue;
      if (!First)
        Output.append(", ");
      First = false;
      if (auto Printed = appendPhysicalRegister(Register.Reg); !Printed)
        return Printed;
    }
    Output.push_back(')');
    return {};
  }
  case MachineOperand::Kind::Metadata: {
    const MachineMetadataValue &Value = Operand.getIf<MetadataOperand>()->Value;
    if (const auto *Reference = Value.reference())
      return printMetadataReference(*Reference);
    const MachineDIExpression *Expression = Value.expression();
    if (Expression == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "metadata operand has no printable value");
    }
    Output.append(formatDIExpression(Expression->elements()));
    return {};
  }
  case MachineOperand::Kind::TargetIndex: {
    const auto &TargetIndexValue = *Operand.getIf<TargetIndexOperand>();
    const auto Name = Target.mirText().targetIndexName(TargetIndexValue.Index);
    if (!Name)
      return makeUnexpected(ErrorCode::VerificationError, "target index has no MIR name");
    Output.append("target-index(");
    Output.append(*Name);
    Output.push_back(')');
    appendOffset(TargetIndexValue.Offset);
    return {};
  }
  case MachineOperand::Kind::MCSymbol: {
    const auto &Symbol = *Operand.getIf<MCSymbolOperand>();
    Output.append("<mcsymbol ");
    Output.append(quoteName(Symbol.Name));
    Output.push_back('>');
    appendOffset(Symbol.Offset);
    return {};
  }
  case MachineOperand::Kind::CFIIndex:
    return printCFIInstruction(Operand.getIf<CFIIndexOperand>()->Index);
  case MachineOperand::Kind::Intrinsic: {
    const auto Name = Target.mirText().intrinsicName(Operand.getIf<IntrinsicOperand>()->ID);
    if (!Name)
      return makeUnexpected(ErrorCode::VerificationError, "intrinsic has no MIR name");
    Output.append("intrinsic(@");
    Output.append(*Name);
    Output.push_back(')');
    return {};
  }
  case MachineOperand::Kind::Predicate: {
    const PredicateOperand &Predicate = *Operand.getIf<PredicateOperand>();
    if (const auto *Integer = Predicate.integerPredicate()) {
      Output.append("intpred(");
      Output.append(integerPredicateName(*Integer));
    } else {
      Output.append("floatpred(");
      Output.append(floatingPredicateName(*Predicate.floatingPredicate()));
    }
    Output.push_back(')');
    return {};
  }
  case MachineOperand::Kind::ShuffleMask: {
    Output.append("shufflemask(");
    const auto &Elements = Operand.getIf<ShuffleMaskOperand>()->Elements;
    for (std::size_t Index = 0; Index < Elements.size(); ++Index) {
      if (Index != 0)
        Output.append(", ");
      if (Elements[Index] == ShuffleMaskOperand::Undef)
        Output.append("undef");
      else
        Output.append(std::to_string(Elements[Index]));
    }
    Output.push_back(')');
    return {};
  }
  case MachineOperand::Kind::DebugInstrRef: {
    const auto &Reference = *Operand.getIf<DebugInstrRefOperand>();
    Output.append("dbg-instr-ref(");
    Output.append(std::to_string(Reference.InstructionNumber));
    Output.append(", ");
    Output.append(std::to_string(Reference.OperandNumber));
    Output.push_back(')');
    return {};
  }
  case MachineOperand::Kind::LaneMask:
    Output.append("lanemask(0x");
    Output.append(
        hexadecimalWords(std::array{Operand.getIf<LaneMaskOperand>()->Mask.rawValue()}, 64));
    Output.push_back(')');
    return {};
  case MachineOperand::Kind::Count:
    return makeUnexpected(ErrorCode::VerificationError, "machine operand has an invalid kind");
  }
  return makeUnexpected(ErrorCode::InternalError, "machine operand printer did not handle a kind");
}

Expected<void> BodyPrinter::printInlineAsmExtraInfo(const MachineOperand &Operand) {
  const auto *Value = Operand.getIf<std::int64_t>();
  if (Value == nullptr || Operand.targetFlags() != 0) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "inline assembly extra info must be an unflagged immediate");
  }
  const InlineAsmExtraInfo ExtraInfo(static_cast<std::uint32_t>(*Value));
  const std::vector<std::string_view> Names = ExtraInfo.names();
  for (std::size_t Index = 0; Index < Names.size(); ++Index) {
    if (Index != 0)
      Output.push_back(' ');
    Output.append(Names[Index]);
  }
  return {};
}

Expected<std::size_t> BodyPrinter::printInlineAsmOperandFlag(const MachineOperand &Operand) {
  const auto *Value = Operand.getIf<std::int64_t>();
  if (Value == nullptr || *Value < 0 ||
      static_cast<std::uint64_t>(*Value) > std::numeric_limits<std::uint32_t>::max() ||
      Operand.targetFlags() != 0) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "inline assembly operand flag must be an unflagged 32-bit immediate");
  }

  const InlineAsmOperandFlag Flag(static_cast<std::uint32_t>(*Value));
  const std::string_view KindName = InlineAsmOperandFlag::kindName(Flag.kind());
  if (KindName.empty() || Flag.operandCount() == 0) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "inline assembly operand flag has an invalid kind or operand count");
  }
  Output.append(KindName);

  if (const auto MatchingOperand = Flag.matchingOperand()) {
    Output.append(" tiedto:$");
    Output.append(std::to_string(*MatchingOperand));
    return Flag.operandCount();
  }
  if (Flag.kind() == InlineAsmOperandKind::Memory ||
      Flag.kind() == InlineAsmOperandKind::Function) {
    const auto Constraint = InlineAsmOperandFlag::memoryConstraintName(Flag.memoryConstraint());
    if (Constraint) {
      Output.push_back(':');
      Output.append(*Constraint);
    }
    return Flag.operandCount();
  }
  if (Flag.isRegisterKind()) {
    if (const auto RegisterClass = Flag.registerClass()) {
      const auto Name =
          Target.registers().registerClassMIRName(RegisterClassID::fromValue(*RegisterClass));
      if (!Name) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "inline assembly operand flag references an unknown register class");
      }
      Output.push_back(':');
      Output.append(*Name);
    }
  }
  return Flag.operandCount();
}

Expected<void> BodyPrinter::printMemoryBase(const MemoryBase &Base, std::int64_t Offset) {
  switch (Base.kind()) {
  case MemoryBase::Kind::Unknown:
    Output.append("unknown-address");
    break;
  case MemoryBase::Kind::FrameIndex: {
    const auto Frame = Base.frameIndex();
    if (!Frame) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "frame-index memory base has no frame index");
    }
    const FrameIndex Index = *Frame;
    auto ID = frameID(Index);
    if (!ID)
      return std::unexpected(ID.error());
    const auto Stack =
        std::ranges::find_if(Context.StackObjects, [Index](const MIFrameObjectSlot &Slot) {
          return Slot.Index == Index;
        });
    if (Stack != Context.StackObjects.end()) {
      Output.append("%stack.");
      Output.append(std::to_string(*ID));
      if (Stack->Name) {
        Output.push_back('.');
        Output.append(quoteName(*Stack->Name));
      }
    } else {
      Output.append("%fixed-stack.");
      Output.append(std::to_string(*ID));
    }
    break;
  }
  case MemoryBase::Kind::ConstantPool: {
    const auto Constant = Base.constantPoolIndex();
    if (!Constant) {
      return makeUnexpected(ErrorCode::VerificationError, "constant-pool memory base has no index");
    }
    auto ID = constantPoolID(*Constant);
    if (!ID)
      return std::unexpected(ID.error());
    Output.append("%const.");
    Output.append(std::to_string(*ID));
    break;
  }
  case MemoryBase::Kind::JumpTable: {
    const auto JumpTable = Base.jumpTableIndex();
    if (!JumpTable) {
      return makeUnexpected(ErrorCode::VerificationError, "jump-table memory base has no index");
    }
    auto ID = jumpTableID(*JumpTable);
    if (!ID)
      return std::unexpected(ID.error());
    Output.append("%jump-table.");
    Output.append(std::to_string(*ID));
    break;
  }
  case MemoryBase::Kind::GlobalSymbol: {
    const auto &Name = Base.symbolName();
    if (!Name) {
      return makeUnexpected(ErrorCode::VerificationError, "global memory base has no symbol name");
    }
    Output.push_back('@');
    Output.append(quoteName(*Name));
    break;
  }
  case MemoryBase::Kind::ExternalSymbol: {
    const auto &Name = Base.symbolName();
    if (!Name) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "external memory base has no symbol name");
    }
    Output.push_back('&');
    Output.append(quoteName(*Name));
    break;
  }
  case MemoryBase::Kind::IRValue: {
    const auto &Name = Base.symbolName();
    if (!Name) {
      return makeUnexpected(ErrorCode::VerificationError, "IR-value memory base has no value name");
    }
    Output.append("%ir.");
    Output.append(quoteName(*Name));
    break;
  }
  case MemoryBase::Kind::IRConstant: {
    const GenericConstantData *Constant = Base.constantValue();
    if (Constant == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "IR-constant memory base has no constant value");
    }
    auto Printed = Constant->toLLVMString();
    if (!Printed)
      return std::unexpected(Printed.error());
    Output.push_back('`');
    Output.append(*Printed);
    Output.push_back('`');
    break;
  }
  case MemoryBase::Kind::Stack:
    Output.append("stack");
    break;
  case MemoryBase::Kind::GOT:
    Output.append("got");
    break;
  case MemoryBase::Kind::ConstantPoolPseudo:
    Output.append("constant-pool");
    break;
  case MemoryBase::Kind::JumpTablePseudo:
    Output.append("jump-table");
    break;
  case MemoryBase::Kind::CallEntryGlobal: {
    const auto &Name = Base.symbolName();
    if (!Name) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "global call-entry memory base has no symbol name");
    }
    Output.append("call-entry @");
    Output.append(quoteName(*Name));
    break;
  }
  case MemoryBase::Kind::CallEntryExternal: {
    const auto &Name = Base.symbolName();
    if (!Name) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "external call-entry memory base has no symbol name");
    }
    Output.append("call-entry &");
    Output.append(quoteName(*Name));
    break;
  }
  case MemoryBase::Kind::Custom: {
    const auto &Name = Base.symbolName();
    if (!Name) {
      return makeUnexpected(ErrorCode::VerificationError, "custom memory base has no name");
    }
    Output.append("custom ");
    Output.append(quoteName(*Name));
    break;
  }
  }
  appendOffset(Offset);
  return {};
}

Expected<void> BodyPrinter::printMemoryOperand(const MachineMemOperand &Operand) {
  const MachineMemOperandDesc &Description = Operand.description();
  Output.push_back('(');
  if (Description.Accesses.has(MemoryAccessFlag::Volatile))
    Output.append("volatile ");
  if (Description.Accesses.has(MemoryAccessFlag::NonTemporal))
    Output.append("non-temporal ");
  if (Description.Accesses.has(MemoryAccessFlag::Dereferenceable))
    Output.append("dereferenceable ");
  if (Description.Accesses.has(MemoryAccessFlag::Invariant))
    Output.append("invariant ");
  if (Description.TargetFlags != 0) {
    auto Names = Target.mirText().machineMemOperandFlagNames(Description.TargetFlags);
    if (!Names)
      return std::unexpected(Names.error());
    for (std::string_view Name : *Names) {
      Output.append(quoteName(Name));
      Output.push_back(' ');
    }
  }

  if (Operand.isLoad())
    Output.append("load");
  else
    Output.append("store");
  if (Operand.isLoad() && Operand.isStore())
    Output.append(" store");

  if (Description.SyncScope.value() != 0) {
    const auto Scope =
        std::ranges::find_if(Context.SyncScopes, [&Description](const MISyncScopeSlot &Slot) {
          return Slot.ID == Description.SyncScope;
        });
    if (Scope == Context.SyncScopes.end()) {
      return makeUnexpected(ErrorCode::VerificationError, "synchronization scope has no MIR name");
    }
    Output.append(" syncscope(");
    Output.append(quoteString(Scope->Name));
    Output.push_back(')');
  }
  if (Description.SuccessOrdering != AtomicOrdering::NotAtomic) {
    Output.push_back(' ');
    Output.append(atomicOrderingName(Description.SuccessOrdering));
  }
  if (Description.FailureOrdering != AtomicOrdering::NotAtomic) {
    Output.push_back(' ');
    Output.append(atomicOrderingName(Description.FailureOrdering));
  }

  Output.push_back(' ');
  if (Description.MemoryType) {
    auto Type = llvmMIRLowLevelType(*Description.MemoryType);
    if (!Type)
      return std::unexpected(Type.error());
    Output.push_back('(');
    Output.append(*Type);
    Output.push_back(')');
  } else {
    switch (Description.Size.kind()) {
    case MemorySize::Kind::Unknown:
      Output.append("unknown-size");
      break;
    case MemorySize::Kind::Fixed:
      Output.append(std::to_string(Description.Size.minimumBytes()));
      break;
    case MemorySize::Kind::Scalable:
      Output.append("(<vscale x ");
      Output.append(std::to_string(Description.Size.minimumBytes()));
      Output.append(" x s8>)");
      break;
    }
  }

  const std::string_view Preposition =
      Operand.isLoad() && Operand.isStore() ? " on " : (Operand.isLoad() ? " from " : " into ");
  if (Description.Base.kind() != MemoryBase::Kind::Unknown || Description.Offset != 0) {
    Output.append(Preposition);
    if (auto Printed = printMemoryBase(Description.Base, Description.Offset); !Printed) {
      return Printed;
    }
  }

  if (!Description.Size.isKnown() ||
      (Description.Size.minimumBytes() != 0 &&
       Description.Align.bytes() != Description.Size.minimumBytes())) {
    Output.append(", align ");
    Output.append(std::to_string(Description.Align.bytes()));
  }
  if (Description.BaseAlign && *Description.BaseAlign != Description.Align) {
    Output.append(", basealign ");
    Output.append(std::to_string(Description.BaseAlign->bytes()));
  }
  if (Description.AddressSpace != 0) {
    Output.append(", addrspace ");
    Output.append(std::to_string(Description.AddressSpace));
  }

  const auto PrintMetadata =
      [this](std::string_view Name,
             const std::optional<MachineMetadataReference> &Reference) -> Expected<void> {
    if (!Reference)
      return {};
    Output.append(", ");
    Output.append(Name);
    Output.push_back(' ');
    return printMetadataReference(*Reference);
  };
  if (auto Printed = PrintMetadata("!tbaa", Description.TBAA); !Printed)
    return Printed;
  if (auto Printed = PrintMetadata("!alias.scope", Description.AliasScope); !Printed) {
    return Printed;
  }
  if (auto Printed = PrintMetadata("!noalias", Description.NoAlias); !Printed)
    return Printed;
  if (auto Printed = PrintMetadata("!noalias.addrspace", Description.NoAliasAddressSpace);
      !Printed) {
    return Printed;
  }
  if (auto Printed = PrintMetadata("!range", Description.RangeMetadata); !Printed) {
    return Printed;
  }
  Output.push_back(')');
  return {};
}

Expected<void> BodyPrinter::printInstruction(const MachineInstr &Instruction) {
  const InstructionDescriptor *Descriptor = Target.instructions().descriptor(Instruction.opcode());
  if (Descriptor == nullptr) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "machine instruction has no target descriptor");
  }
  std::size_t ExplicitDefs = 0;
  while (ExplicitDefs < Instruction.operandCount()) {
    const MachineOperand *Operand = Instruction.operandAt(ExplicitDefs);
    if (Operand == nullptr || !Operand->isRegister() || Operand->isImplicit() ||
        !Operand->isDef() || Operand->isUse())
      break;
    ++ExplicitDefs;
  }

  Output.append("  ");
  for (std::size_t Index = 0; Index < ExplicitDefs; ++Index) {
    if (Index != 0)
      Output.append(", ");
    if (auto Printed = printOperand(*Instruction.operandAt(Index), true); !Printed) {
      return Printed;
    }
  }
  if (ExplicitDefs != 0)
    Output.append(" = ");

  const MachineInstrFlags Flags = Instruction.flags();
  constexpr std::array FlagSpellings{
      std::pair{MachineInstrFlag::FrameSetup, std::string_view{"frame-setup"}},
      std::pair{MachineInstrFlag::FrameDestroy, std::string_view{"frame-destroy"}},
      std::pair{MachineInstrFlag::FastMathNoNaNs, std::string_view{"nnan"}},
      std::pair{MachineInstrFlag::FastMathNoInfs, std::string_view{"ninf"}},
      std::pair{MachineInstrFlag::FastMathNoSignedZeros, std::string_view{"nsz"}},
      std::pair{MachineInstrFlag::FastMathAllowReciprocal, std::string_view{"arcp"}},
      std::pair{MachineInstrFlag::FastMathAllowContract, std::string_view{"contract"}},
      std::pair{MachineInstrFlag::FastMathApproxFunc, std::string_view{"afn"}},
      std::pair{MachineInstrFlag::FastMathAllowReassoc, std::string_view{"reassoc"}},
      std::pair{MachineInstrFlag::NoUnsignedWrap, std::string_view{"nuw"}},
      std::pair{MachineInstrFlag::NoSignedWrap, std::string_view{"nsw"}},
      std::pair{MachineInstrFlag::NoUSWrap, std::string_view{"nusw"}},
      std::pair{MachineInstrFlag::Exact, std::string_view{"exact"}},
      std::pair{MachineInstrFlag::NoFPExcept, std::string_view{"nofpexcept"}},
      std::pair{MachineInstrFlag::Unpredictable, std::string_view{"unpredictable"}},
      std::pair{MachineInstrFlag::NoConvergent, std::string_view{"noconvergent"}},
      std::pair{MachineInstrFlag::NonNeg, std::string_view{"nneg"}},
      std::pair{MachineInstrFlag::Disjoint, std::string_view{"disjoint"}},
      std::pair{MachineInstrFlag::SameSign, std::string_view{"samesign"}},
      std::pair{MachineInstrFlag::InBounds, std::string_view{"inbounds"}},
  };
  for (const auto &[Flag, Spelling] : FlagSpellings) {
    if (Flags.has(Flag)) {
      Output.append(Spelling);
      Output.push_back(' ');
    }
  }
  Output.append(Descriptor->Name);

  bool HasPrintedTail = false;
  const bool IsInlineAsm = Descriptor->Name == "INLINEASM" || Descriptor->Name == "INLINEASM_BR";
  std::size_t InlineAsmPayloadRemaining = 0;
  for (std::size_t Index = ExplicitDefs; Index < Instruction.operandCount(); ++Index) {
    Output.append(HasPrintedTail ? ", " : " ");
    HasPrintedTail = true;
    const MachineOperand &Operand = *Instruction.operandAt(Index);
    if (IsInlineAsm && Index == InlineAsmExtraInfoOperandIndex) {
      auto Printed = printInlineAsmExtraInfo(Operand);
      if (!Printed)
        return Printed;
      continue;
    }
    if (IsInlineAsm && Index >= InlineAsmFirstConstraintOperandIndex &&
        InlineAsmPayloadRemaining == 0) {
      auto OperandCount = printInlineAsmOperandFlag(Operand);
      if (!OperandCount)
        return std::unexpected(OperandCount.error());
      InlineAsmPayloadRemaining = *OperandCount;
      continue;
    }
    bool PrintVirtualType = true;
    if (isGenericOpcode(Instruction.opcode()) && !Descriptor->Flags.has(InstructionFlag::Phi) &&
        Index < Descriptor->Operands.size()) {
      const std::optional<OperandTypeConstraint> &UseConstraint =
          Descriptor->Operands[Index].TypeConstraint;
      if (UseConstraint) {
        for (std::size_t DefIndex = 0; DefIndex < ExplicitDefs; ++DefIndex) {
          if (DefIndex >= Descriptor->Operands.size())
            break;
          const std::optional<OperandTypeConstraint> &DefConstraint =
              Descriptor->Operands[DefIndex].TypeConstraint;
          if (DefConstraint && DefConstraint->EqualityGroup == UseConstraint->EqualityGroup) {
            PrintVirtualType = false;
            break;
          }
        }
      } else if (const Register *UseRegister = Operand.getIf<Register>();
                 UseRegister != nullptr && UseRegister->isVirtual()) {
        const auto UseInfo = Function.registerInfo().virtualRegisterInfo(*UseRegister);
        for (std::size_t DefIndex = 0; DefIndex < ExplicitDefs && UseInfo && UseInfo->get().Type;
             ++DefIndex) {
          if (DefIndex >= Descriptor->Operands.size() ||
              Descriptor->Operands[DefIndex].TypeConstraint)
            continue;
          const MachineOperand *DefOperand = Instruction.operandAt(DefIndex);
          const Register *DefRegister =
              DefOperand == nullptr ? nullptr : DefOperand->getIf<Register>();
          if (DefRegister == nullptr || !DefRegister->isVirtual())
            continue;
          const auto DefInfo = Function.registerInfo().virtualRegisterInfo(*DefRegister);
          if (DefInfo && DefInfo->get().Type == UseInfo->get().Type) {
            PrintVirtualType = false;
            break;
          }
        }
      }
    }
    auto Printed = printOperand(Operand, false, PrintVirtualType);
    if (!Printed)
      return Printed;
    if (IsInlineAsm && InlineAsmPayloadRemaining != 0)
      --InlineAsmPayloadRemaining;
  }
  if (InlineAsmPayloadRemaining != 0)
    return makeUnexpected(ErrorCode::VerificationError,
                          "inline assembly operand flag has missing payload operands");

  const auto BeginAttachment = [this, &HasPrintedTail](std::string_view Name) {
    Output.append(HasPrintedTail ? ", " : " ");
    HasPrintedTail = true;
    Output.append(Name);
    Output.push_back(' ');
  };
  if (Instruction.preInstrSymbol()) {
    BeginAttachment("pre-instr-symbol");
    Output.append("<mcsymbol ");
    Output.append(quoteName(*Instruction.preInstrSymbol()));
    Output.push_back('>');
  }
  if (Instruction.postInstrSymbol()) {
    BeginAttachment("post-instr-symbol");
    Output.append("<mcsymbol ");
    Output.append(quoteName(*Instruction.postInstrSymbol()));
    Output.push_back('>');
  }
  if (Instruction.heapAllocMarker()) {
    BeginAttachment("heap-alloc-marker");
    if (auto Printed = printMetadataReference(*Instruction.heapAllocMarker()); !Printed) {
      return Printed;
    }
  }
  if (Instruction.pcSections()) {
    BeginAttachment("pcsections");
    if (auto Printed = printMetadataReference(*Instruction.pcSections()); !Printed) {
      return Printed;
    }
  }
  if (Instruction.mmra()) {
    BeginAttachment("mmra");
    if (auto Printed = printMetadataReference(*Instruction.mmra()); !Printed)
      return Printed;
  }
  if (Instruction.cfiType()) {
    BeginAttachment("cfi-type");
    Output.append(std::to_string(*Instruction.cfiType()));
  }
  if (Instruction.deactivationSymbol()) {
    BeginAttachment("deactivation-symbol");
    Output.push_back('@');
    Output.append(quoteName(*Instruction.deactivationSymbol()));
  }
  if (Instruction.debugInstrNumber() != 0) {
    BeginAttachment("debug-instr-number");
    Output.append(std::to_string(Instruction.debugInstrNumber()));
  }
  if (Instruction.debugLocation()) {
    BeginAttachment("debug-location");
    if (const MachineMetadataReference *Reference = Instruction.debugLocation()->reference()) {
      if (auto Printed = printMetadataReference(*Reference); !Printed)
        return Printed;
    } else if (const MachineDILocation *Location = Instruction.debugLocation()->inlineLocation()) {
      if (auto Printed = printDILocation(*Location, 0); !Printed)
        return Printed;
    } else {
      return makeUnexpected(ErrorCode::VerificationError,
                            "machine debug location has no printable value");
    }
  }

  if (!Instruction.memOperands().empty()) {
    Output.append(" :: ");
    for (std::size_t Index = 0; Index < Instruction.memOperands().size(); ++Index) {
      if (Index != 0)
        Output.append(", ");
      if (auto Printed = printMemoryOperand(*Instruction.memOperands()[Index]); !Printed) {
        return Printed;
      }
    }
  }
  return {};
}

Expected<void> BodyPrinter::printBlock(const MachineBasicBlock &Block) {
  auto ID = blockID(Block);
  if (!ID)
    return std::unexpected(ID.error());
  Output.append("bb.");
  Output.append(std::to_string(*ID));
  const auto &Name = Block.attributes().name();
  if (Name) {
    Output.push_back('.');
    Output.append(quoteName(*Name));
  }

  std::vector<std::string> Attributes;
  const auto AddFlag = [&Block, &Attributes](MachineBasicBlockAttribute Attribute,
                                             std::string_view Spelling) {
    if (Block.attributes().has(Attribute))
      Attributes.emplace_back(Spelling);
  };
  AddFlag(MachineBasicBlockAttribute::MachineBlockAddressTaken, "machine-block-address-taken");
  if (Block.attributes().has(MachineBasicBlockAttribute::IRBlockAddressTaken)) {
    std::string Attribute = "ir-block-address-taken";
    const auto &IRBlockName = Block.attributes().irBlockName();
    if (IRBlockName) {
      Attribute.append(" %ir-block.");
      Attribute.append(quoteName(*IRBlockName));
    }
    Attributes.push_back(std::move(Attribute));
  } else {
    const auto &IRBlockName = Block.attributes().irBlockName();
    if (IRBlockName)
      Attributes.push_back("%ir-block." + quoteName(*IRBlockName));
  }
  AddFlag(MachineBasicBlockAttribute::LandingPad, "landing-pad");
  AddFlag(MachineBasicBlockAttribute::InlineAsmBrIndirectTarget, "inlineasm-br-indirect-target");
  AddFlag(MachineBasicBlockAttribute::EHFuncletEntry, "ehfunclet-entry");
  AddFlag(MachineBasicBlockAttribute::EHScopeEntry, "ehscope-entry");
  if (Block.attributes().alignment().bytes() != 1) {
    Attributes.push_back("align " + std::to_string(Block.attributes().alignment().bytes()));
  }
  const MachineBasicBlockSectionID Section = Block.attributes().sectionID();
  if (!Section.isDefault()) {
    std::string Value;
    if (Section.kind() == MachineBasicBlockSectionKind::Exception)
      Value = "Exception";
    else if (Section.kind() == MachineBasicBlockSectionKind::Cold)
      Value = "Cold";
    else
      Value = std::to_string(Section.number());
    Attributes.push_back("bbsections " + Value);
  }
  const auto &ProfileID = Block.attributes().profileID();
  if (ProfileID) {
    const MachineBasicBlockProfileID Profile = *ProfileID;
    Attributes.push_back("bb_id " + std::to_string(Profile.BaseID) + "." +
                         std::to_string(Profile.CloneID));
  }
  if (Block.attributes().callFrameSize() != 0) {
    Attributes.push_back("call-frame-size " + std::to_string(Block.attributes().callFrameSize()));
  }
  if (!Attributes.empty()) {
    Output.append(" (");
    for (std::size_t Index = 0; Index < Attributes.size(); ++Index) {
      if (Index != 0)
        Output.append(", ");
      Output.append(Attributes[Index]);
    }
    Output.push_back(')');
  }
  Output.append(":\n");

  bool HasLineAttributes = false;
  if (!Block.successors().empty()) {
    Output.append("  successors: ");
    for (std::size_t Index = 0; Index < Block.successors().size(); ++Index) {
      if (Index != 0)
        Output.append(", ");
      const MachineSuccessor &Successor = Block.successors()[Index];
      if (Successor.Block == nullptr || !BlockIDs.contains(Successor.Block)) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "successor references an unlinked block");
      }
      appendBlockReference(*Successor.Block);
      Output.append("(0x");
      Output.append(hexadecimalWords(
          std::array{static_cast<std::uint64_t>(Successor.Probability.rawValue())}, 32));
      Output.push_back(')');
    }
    Output.push_back('\n');
    HasLineAttributes = true;
  }
  if (!Block.liveIns().empty()) {
    Output.append("  liveins: ");
    for (std::size_t Index = 0; Index < Block.liveIns().size(); ++Index) {
      if (Index != 0)
        Output.append(", ");
      const MachineBasicBlockLiveIn &LiveIn = Block.liveIns()[Index];
      if (auto Printed = appendPhysicalRegister(LiveIn.PhysReg); !Printed)
        return Printed;
      if (LiveIn.Lanes != LaneMask::all()) {
        Output.append(": 0x");
        Output.append(hexadecimalWords(std::array{LiveIn.Lanes.rawValue()}, 64));
      }
    }
    Output.push_back('\n');
    HasLineAttributes = true;
  }

  if (HasLineAttributes && Block.firstInstruction() != nullptr)
    Output.push_back('\n');

  bool InBundle = false;
  for (const MachineInstr *Instruction = Block.firstInstruction(); Instruction != nullptr;
       Instruction = Instruction->next()) {
    if (auto Printed = printInstruction(*Instruction); !Printed)
      return Printed;
    const bool HasPredecessor = Instruction->flags().has(MachineInstrFlag::BundledWithPred);
    const bool HasSuccessor = Instruction->flags().has(MachineInstrFlag::BundledWithSucc);
    if (!InBundle && HasSuccessor && !HasPredecessor) {
      Output.append(" {");
      InBundle = true;
    }
    Output.push_back('\n');
    if (InBundle && !HasSuccessor) {
      Output.append("  }\n");
      InBundle = false;
    }
  }
  if (InBundle) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "unterminated instruction bundle cannot be printed");
  }
  return {};
}

Expected<std::string> BodyPrinter::run() {
  auto Verification = MachineVerifier::verify(
      Function, Target,
      MachineVerifierOptions{
          .Mode = MachineVerifierMode::Generic, .Stage = std::nullopt, .VerifyProperties = false});
  if (!Verification)
    return std::unexpected(Verification.error());
  if (auto Initialized = initialize(); !Initialized)
    return std::unexpected(Initialized.error());

  bool First = true;
  for (const MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
       Block = Block->next()) {
    if (!First)
      Output.push_back('\n');
    First = false;
    if (auto Printed = printBlock(*Block); !Printed)
      return std::unexpected(Printed.error());
  }
  return std::move(Output);
}

} // namespace

Expected<std::string> printMachineFunctionBody(const MachineFunction &Function,
                                               const TargetDescription &Target,
                                               const MIParserContext &Context,
                                               MIPrinterOptions Options) {
  return BodyPrinter(Function, Target, Context, Options).run();
}

} // namespace rcc::mir
