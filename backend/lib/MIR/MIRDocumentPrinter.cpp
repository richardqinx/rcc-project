// SPDX-License-Identifier: GPL-3.0-or-later

#include "MIRDocumentReferenceValidation.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineCompilationStatus.h"
#include "rcc/CodeGen/MachineConstantPool.h"
#include "rcc/CodeGen/MachineFrameInfo.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineFunctionAttributes.h"
#include "rcc/CodeGen/MachineFunctionProperties.h"
#include "rcc/CodeGen/MachineFunctionRecords.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineJumpTableInfo.h"
#include "rcc/CodeGen/MachineMetadata.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/CodeGen/TargetMIRTextInfo.h"
#include "rcc/CodeGen/TargetRegisterInfo.h"
#include "rcc/MIR/LLVMConstantSyntax.h"
#include "rcc/MIR/MIDebugSyntax.h"
#include "rcc/MIR/MIRDocument.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
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

  const LLVMConstantSyntaxOptions LayoutOptions{
      .DataLayout = Options.DataLayout,
      .NamedTypes = {},
  };
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
    auto PointerSize = llvmPointerSizeInBits(*AddressSpace, LayoutOptions);
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
    auto StorageSize = llvmConstantStorageSize(Generic->Type, LayoutOptions);
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
      Value = printLLVMConstant(*Generic);
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
