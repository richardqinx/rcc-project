// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MIRYamlMapping.h"

#include "MIRParser/MILexer.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
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
#include "rcc/CodeGen/MachineVerifier.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/CodeGen/TargetMIRTextInfo.h"
#include "rcc/CodeGen/TargetRegisterInfo.h"
#include "rcc/MIR/LLVMConstantSyntax.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace rcc::mir {

namespace parser_detail {

[[nodiscard]] inline std::string_view trim(std::string_view Text) noexcept {
  while (!Text.empty() && (Text.front() == ' ' || Text.front() == '\t' || Text.front() == '\r' ||
                           Text.front() == '\n')) {
    Text.remove_prefix(1);
  }
  while (!Text.empty() && (Text.back() == ' ' || Text.back() == '\t' || Text.back() == '\r' ||
                           Text.back() == '\n')) {
    Text.remove_suffix(1);
  }
  return Text;
}

[[nodiscard]] inline bool parseUnsigned(std::string_view Text, std::uint64_t &Value) noexcept {
  Text = trim(Text);
  int Base = 10;
  if (Text.starts_with("0x") || Text.starts_with("0X")) {
    Text.remove_prefix(2);
    Base = 16;
  }
  if (Text.empty())
    return false;
  const auto [End, Error] = std::from_chars(Text.data(), Text.data() + Text.size(), Value, Base);
  return Error == std::errc{} && End == Text.data() + Text.size();
}

[[nodiscard]] inline bool parseSigned(std::string_view Text, std::int64_t &Value) noexcept {
  Text = trim(Text);
  if (Text.empty())
    return false;
  const auto [End, Error] = std::from_chars(Text.data(), Text.data() + Text.size(), Value, 10);
  return Error == std::errc{} && End == Text.data() + Text.size();
}

[[nodiscard]] inline bool parseUnsigned32(std::string_view Text, std::uint32_t &Value) noexcept {
  std::uint64_t Wide = 0;
  if (!parseUnsigned(Text, Wide) || Wide > std::numeric_limits<std::uint32_t>::max())
    return false;
  Value = static_cast<std::uint32_t>(Wide);
  return true;
}

[[nodiscard]] inline bool parseBool(std::string_view Text, bool &Value) noexcept {
  if (Text == "true") {
    Value = true;
    return true;
  }
  if (Text == "false") {
    Value = false;
    return true;
  }
  return false;
}

[[nodiscard]] inline std::optional<std::uint32_t>
parsePrefixedID(std::string_view Text, std::string_view Prefix) noexcept {
  Text = trim(Text);
  if (!Text.starts_with(Prefix))
    return std::nullopt;
  Text.remove_prefix(Prefix.size());
  const std::size_t Suffix = Text.find_first_of(". \t");
  if (Suffix != std::string_view::npos)
    Text = Text.substr(0, Suffix);
  std::uint32_t ID = 0;
  if (!parseUnsigned32(Text, ID))
    return std::nullopt;
  return ID;
}

} // namespace parser_detail

class MetadataTextParser final {
public:
  using Resolver = std::function<Expected<MachineMetadataReference>(std::uint32_t Slot)>;

  MetadataTextParser(std::string Text, Resolver Resolve)
      : Lexer("MIR metadata scalar", std::move(Text)), Resolve(std::move(Resolve)) {
    do {
      Tokens.push_back(Lexer.next());
    } while (!Tokens.back().isErrorOrEnd());
  }

  [[nodiscard]] Expected<MachineMetadataReference> parseReferenceOnly();
  [[nodiscard]] Expected<MachineMetadataValue> parseValue();
  [[nodiscard]] Expected<MachineDebugLocation> parseLocation();

  [[nodiscard]] const TextRange &diagnosticRange() const noexcept {
    return token().range();
  }

private:
  [[nodiscard]] const MIToken &token() const noexcept {
    return Tokens[Position];
  }
  [[nodiscard]] bool at(MITokenKind Kind) const noexcept {
    return token().is(Kind);
  }
  void advance() noexcept {
    if (Position + 1U < Tokens.size())
      ++Position;
  }
  [[nodiscard]] bool consume(MITokenKind Kind) noexcept {
    if (!at(Kind))
      return false;
    advance();
    return true;
  }
  [[nodiscard]] Expected<void> expect(MITokenKind Kind, std::string_view Description);
  [[nodiscard]] Expected<void> finish();
  [[nodiscard]] Expected<MachineMetadataReference> parseReference();
  [[nodiscard]] Expected<MachineDIExpression> parseExpression();
  [[nodiscard]] Expected<void> parseDILocation(std::vector<MachineDILocationFrame> &Frames,
                                               std::optional<MachineMetadataReference> &Terminal);

  MILexer Lexer;
  Resolver Resolve;
  std::vector<MIToken> Tokens;
  std::size_t Position = 0;
};

using parser_detail::parseBool;
using parser_detail::parsePrefixedID;
using parser_detail::parseSigned;
using parser_detail::parseUnsigned;
using parser_detail::parseUnsigned32;
using parser_detail::trim;

bool MIRDocumentParser::boolean(const yaml::Node &Node, bool &Value) {
  std::string_view Text;
  if (scalar(Node, Text, "boolean") || !parseBool(Text, Value))
    return fail(Node.range(), "expected 'true' or 'false'");
  return false;
}

bool MIRDocumentParser::unsignedValue(const yaml::Node &Node, std::uint64_t &Value,
                                      std::string_view Description) {
  std::string_view Text;
  if (scalar(Node, Text, Description) || !parseUnsigned(Text, Value))
    return fail(Node.range(), "expected " + std::string(Description));
  return false;
}

bool MIRDocumentParser::signedValue(const yaml::Node &Node, std::int64_t &Value,
                                    std::string_view Description) {
  std::string_view Text;
  if (scalar(Node, Text, Description) || !parseSigned(Text, Value))
    return fail(Node.range(), "expected " + std::string(Description));
  return false;
}

bool MIRDocumentParser::alignment(const yaml::Node &Node, Alignment &Value, bool AllowZero) {
  std::uint64_t Bytes = 0;
  if (unsignedValue(Node, Bytes, "an unsigned alignment"))
    return true;
  if (Bytes == 0 && AllowZero) {
    Value = Alignment{};
    return false;
  }
  auto Parsed = Alignment::create(Bytes);
  if (!Parsed)
    return importError(Node.range(), Parsed.error());
  Value = *Parsed;
  return false;
}

bool MIRDocumentParser::physicalRegister(std::string_view Text, const TextRange &Range,
                                         PhysicalRegister &Reg) {
  Text = trim(Text);
  if (!Text.starts_with('$') || Text.size() == 1)
    return fail(Range, "physical register must use the '$name' MIR spelling");
  const PhysicalRegisterDesc *Description = Target.registers().physicalRegister(Text.substr(1));
  if (Description == nullptr)
    return fail(Range, "unknown physical register '" + std::string(Text) + "'");
  Reg = Description->Reg;
  return false;
}

bool MIRDocumentParser::physicalRegister(const yaml::Node &Node, PhysicalRegister &Reg) {
  std::string_view Text;
  return scalar(Node, Text, "physical register") || physicalRegister(Text, Node.range(), Reg);
}

bool MIRDocumentParser::virtualRegister(std::string_view Text, const TextRange &Range,
                                        MachineFunction &Function, MIParserContext &Context,
                                        Register &Reg) {
  const auto ID = parsePrefixedID(Text, "%");
  if (!ID)
    return fail(Range, "virtual register must use the '%N' MIR spelling");
  auto Found =
      std::ranges::find_if(Context.VirtualRegisters,
                           [ID](const MIVirtualRegisterSlot &Slot) { return Slot.TextID == *ID; });
  if (Found == Context.VirtualRegisters.end()) {
    auto Created = Function.registerInfo().createVirtualRegister();
    if (!Created)
      return importError(Range, Created.error());
    Context.VirtualRegisters.push_back({*ID, *Created});
    Reg = *Created;
    return false;
  }
  Reg = Found->Reg;
  return false;
}

bool MIRDocumentParser::parseConstants(const yaml::Node &Root, MachineFunction &Function,
                                       MIParserContext &Context) {
  const yaml::Node *Constants = mappingValue(Root, "constants");
  if (Constants == nullptr)
    return false;
  if (requireKind(*Constants, yaml::NodeKind::Sequence, "constant pool"))
    return true;
  constexpr std::array Keys{
      std::string_view{"id"},
      std::string_view{"value"},
      std::string_view{"alignment"},
      std::string_view{"isTargetSpecific"},
  };
  std::vector<LLVMNamedTypeDefinition> NamedTypes;
  const std::string_view DataLayout = effectiveDataLayout();
  if (ActiveDocument->EmbeddedIR) {
    const auto Definitions = ActiveDocument->EmbeddedIR->typeDefinitions();
    NamedTypes.reserve(Definitions.size());
    for (const EmbeddedIRTypeDefinition &Definition : Definitions) {
      NamedTypes.push_back({.Name = Definition.Name,
                            .Definition = Definition.Definition,
                            .NumericName = Definition.NumericName});
    }
  }
  std::unordered_set<std::uint32_t> IDs;
  for (const yaml::NodeID ConstantID : Constants->sequence()) {
    const yaml::Node *Constant = node(ConstantID);
    if (Constant == nullptr ||
        requireKind(*Constant, yaml::NodeKind::Mapping, "constant pool entry") ||
        validateKeys(*Constant, Keys, "constant pool entry")) {
      return true;
    }
    const yaml::Node *IDNode = mappingValue(*Constant, "id");
    const yaml::Node *ValueNode = mappingValue(*Constant, "value");
    if (IDNode == nullptr || ValueNode == nullptr)
      return fail(Constant->range(), "constant pool entry requires 'id' and 'value'");
    std::uint64_t WideID = 0;
    if (unsignedValue(*IDNode, WideID, "a 32-bit constant pool ID") ||
        WideID > std::numeric_limits<std::uint32_t>::max()) {
      return true;
    }
    const auto TextID = static_cast<std::uint32_t>(WideID);
    if (!IDs.insert(TextID).second)
      return fail(IDNode->range(), "duplicate constant pool ID");

    bool IsTargetSpecific = false;
    if (const yaml::Node *TargetNode = mappingValue(*Constant, "isTargetSpecific")) {
      if (boolean(*TargetNode, IsTargetSpecific))
        return true;
    }
    if (IsTargetSpecific) {
      std::string_view ValueText;
      if (scalar(*ValueNode, ValueText, "target constant"))
        return true;
      auto TargetConstantValue = Target.mirText().parseTargetConstant(ValueText);
      if (!TargetConstantValue)
        return importError(ValueNode->range(), TargetConstantValue.error(), "mir-target-constant");
      auto Emission = TargetConstantValue->emit();
      if (!Emission)
        return importError(ValueNode->range(), Emission.error(), "mir-target-constant");
      auto RequiredAlignment = TargetConstantValue->requiredAlignment();
      if (!RequiredAlignment)
        return importError(ValueNode->range(), RequiredAlignment.error(), "mir-target-constant");

      MachineConstantPoolEntryDesc Description;
      Description.Size = Emission->Bytes.size();
      Description.Align = *RequiredAlignment;
      Description.Value = std::move(*TargetConstantValue);
      if (const yaml::Node *AlignmentNode = mappingValue(*Constant, "alignment")) {
        if (alignment(*AlignmentNode, Description.Align, true))
          return true;
      }
      auto Added = Function.constantPool().addEntry(std::move(Description));
      if (!Added)
        return importError(Constant->range(), Added.error());
      Context.ConstantPoolItems.push_back({.TextID = TextID, .Index = *Added});
      continue;
    }

    std::string_view ValueText;
    if (scalar(*ValueNode, ValueText, "LLVM constant"))
      return true;
    const LLVMConstantSyntaxOptions ConstantOptions{
        .DataLayout = DataLayout,
        .NamedTypes = NamedTypes,
    };
    auto Data = parseLLVMConstant(trim(ValueText), ConstantOptions);
    if (!Data)
      return importError(ValueNode->range(), Data.error(), "mir-llvm-constant");
    auto Size = llvmConstantStorageSize(Data->Type, ConstantOptions);
    if (!Size)
      return importError(ValueNode->range(), Size.error(), "mir-llvm-constant");

    MachineConstantPoolEntryDesc Description;
    Description.Size = *Size;
    Description.Value = std::move(*Data);
    if (const yaml::Node *AlignmentNode = mappingValue(*Constant, "alignment")) {
      if (alignment(*AlignmentNode, Description.Align, true))
        return true;
    } else {
      auto Preferred = llvmConstantPreferredAlignment(
          std::get<GenericConstantData>(Description.Value).Type, ConstantOptions);
      if (!Preferred)
        return importError(ValueNode->range(), Preferred.error(), "mir-llvm-constant");
      Description.Align = *Preferred;
    }
    auto Added = Function.constantPool().addEntry(std::move(Description));
    if (!Added)
      return importError(Constant->range(), Added.error());
    Context.ConstantPoolItems.push_back({.TextID = TextID, .Index = *Added});
  }
  return false;
}

bool MIRDocumentParser::reserveJumpTables(const yaml::Node &Root, MachineFunction &Function,
                                          MIParserContext &Context,
                                          std::vector<PendingJumpTable> &Pending) {
  const yaml::Node *JumpTable = mappingValue(Root, "jumpTable");
  if (JumpTable == nullptr)
    return false;
  if (requireKind(*JumpTable, yaml::NodeKind::Mapping, "jump table"))
    return true;
  constexpr std::array JumpTableKeys{
      std::string_view{"kind"},
      std::string_view{"entries"},
  };
  if (validateKeys(*JumpTable, JumpTableKeys, "jump table"))
    return true;
  const yaml::Node *KindNode = mappingValue(*JumpTable, "kind");
  if (KindNode == nullptr)
    return fail(JumpTable->range(), "jump table requires 'kind'");
  std::string_view KindName;
  if (scalar(*KindNode, KindName, "jump table kind"))
    return true;

  JumpTableEntryEncoding Encoding;
  std::uint8_t EntrySize = 8;
  if (KindName == "block-address")
    Encoding = JumpTableEntryEncoding::BlockAddress;
  else if (KindName == "gp-rel64-block-address")
    Encoding = JumpTableEntryEncoding::GPRelative64BlockAddress;
  else if (KindName == "gp-rel32-block-address") {
    Encoding = JumpTableEntryEncoding::GPRelative32BlockAddress;
    EntrySize = 4;
  } else if (KindName == "label-difference32") {
    Encoding = JumpTableEntryEncoding::LabelDifference32;
    EntrySize = 4;
  } else if (KindName == "label-difference64")
    Encoding = JumpTableEntryEncoding::LabelDifference64;
  else if (KindName == "inline") {
    Encoding = JumpTableEntryEncoding::Inline;
    EntrySize = 4;
  } else if (KindName == "custom32") {
    Encoding = JumpTableEntryEncoding::Custom32;
    EntrySize = 4;
  } else {
    return fail(KindNode->range(), "unknown enumerated scalar");
  }

  const yaml::Node *Entries = mappingValue(*JumpTable, "entries");
  if (Entries == nullptr)
    return false;
  if (requireKind(*Entries, yaml::NodeKind::Sequence, "jump table entries"))
    return true;
  constexpr std::array EntryKeys{
      std::string_view{"id"},
      std::string_view{"blocks"},
  };
  std::unordered_set<std::uint32_t> IDs;
  for (const yaml::NodeID EntryID : Entries->sequence()) {
    const yaml::Node *Entry = node(EntryID);
    if (Entry == nullptr || requireKind(*Entry, yaml::NodeKind::Mapping, "jump table entry") ||
        validateKeys(*Entry, EntryKeys, "jump table entry")) {
      return true;
    }
    const yaml::Node *IDNode = mappingValue(*Entry, "id");
    if (IDNode == nullptr)
      return fail(Entry->range(), "jump table entry requires 'id'");
    std::uint64_t WideID = 0;
    if (unsignedValue(*IDNode, WideID, "a 32-bit jump table ID") ||
        WideID > std::numeric_limits<std::uint32_t>::max()) {
      return true;
    }
    const auto TextID = static_cast<std::uint32_t>(WideID);
    if (!IDs.insert(TextID).second)
      return fail(IDNode->range(), "duplicate jump table ID");
    auto Reserved = Function.jumpTableInfo().reserveTable();
    if (!Reserved)
      return importError(Entry->range(), Reserved.error());
    PendingJumpTable NewPending{
        .Index = *Reserved,
        .TextID = TextID,
        .Encoding = Encoding,
        .EntrySize = EntrySize,
        .Blocks = {},
    };
    if (const yaml::Node *Blocks = mappingValue(*Entry, "blocks")) {
      if (requireKind(*Blocks, yaml::NodeKind::Sequence, "jump table blocks"))
        return true;
      for (const yaml::NodeID BlockID : Blocks->sequence()) {
        const yaml::Node *BlockNode = node(BlockID);
        std::string_view Text;
        if (BlockNode == nullptr || scalar(*BlockNode, Text, "basic block reference"))
          return true;
        NewPending.Blocks.emplace_back(Text);
      }
    }
    if (NewPending.Blocks.empty())
      return fail(Entry->range(), "jump table entry must contain at least one block");
    Context.JumpTables.push_back({.TextID = TextID, .Index = *Reserved});
    Pending.push_back(std::move(NewPending));
  }
  return false;
}

bool MIRDocumentParser::defineJumpTables(MachineFunction &Function,
                                         std::span<const PendingJumpTable> Pending) {
  for (const PendingJumpTable &Table : Pending) {
    MachineJumpTableDesc Description;
    Description.Encoding = Table.Encoding;
    Description.EntrySize = Table.EntrySize;
    Description.Align = *Alignment::create(Table.EntrySize);
    for (const std::string &Text : Table.Blocks) {
      MachineBasicBlock *Block = nullptr;
      if (parseBlockReference(Text, Result.Source.range(0, 0), Function, Block))
        return true;
      if (Block == nullptr)
        return fail(Result.Source.range(0, 0), "reference to undefined machine basic block");
      Description.Targets.push_back(Block);
    }
    auto DefineResult =
        Function.jumpTableInfo().defineReservedTable(Table.Index, std::move(Description));
    if (!DefineResult)
      return importError(Result.Source.range(0, 0), DefineResult.error());
  }
  return false;
}

} // namespace rcc::mir

namespace rcc::mir {
namespace {

[[nodiscard]] bool parseIndex(std::string_view Text, std::uint64_t &Value) noexcept {
  if (Text.empty())
    return false;
  const auto [End, Error] = std::from_chars(Text.data(), Text.data() + Text.size(), Value, 10);
  return Error == std::errc{} && End == Text.data() + Text.size();
}

[[nodiscard]] const auto &namedOpcodes() {
  static constexpr auto Opcodes = std::to_array<std::pair<std::string_view, std::uint64_t>>({
      {"DW_OP_addr", 0x03},
      {"DW_OP_deref", 0x06},
      {"DW_OP_const1u", 0x08},
      {"DW_OP_const1s", 0x09},
      {"DW_OP_const2u", 0x0A},
      {"DW_OP_const2s", 0x0B},
      {"DW_OP_const4u", 0x0C},
      {"DW_OP_const4s", 0x0D},
      {"DW_OP_const8u", 0x0E},
      {"DW_OP_const8s", 0x0F},
      {"DW_OP_constu", 0x10},
      {"DW_OP_consts", 0x11},
      {"DW_OP_dup", 0x12},
      {"DW_OP_drop", 0x13},
      {"DW_OP_over", 0x14},
      {"DW_OP_pick", 0x15},
      {"DW_OP_swap", 0x16},
      {"DW_OP_rot", 0x17},
      {"DW_OP_xderef", 0x18},
      {"DW_OP_abs", 0x19},
      {"DW_OP_and", 0x1A},
      {"DW_OP_div", 0x1B},
      {"DW_OP_minus", 0x1C},
      {"DW_OP_mod", 0x1D},
      {"DW_OP_mul", 0x1E},
      {"DW_OP_neg", 0x1F},
      {"DW_OP_not", 0x20},
      {"DW_OP_or", 0x21},
      {"DW_OP_plus", 0x22},
      {"DW_OP_plus_uconst", 0x23},
      {"DW_OP_shl", 0x24},
      {"DW_OP_shr", 0x25},
      {"DW_OP_shra", 0x26},
      {"DW_OP_xor", 0x27},
      {"DW_OP_bra", 0x28},
      {"DW_OP_eq", 0x29},
      {"DW_OP_ge", 0x2A},
      {"DW_OP_gt", 0x2B},
      {"DW_OP_le", 0x2C},
      {"DW_OP_lt", 0x2D},
      {"DW_OP_ne", 0x2E},
      {"DW_OP_skip", 0x2F},
      {"DW_OP_regx", 0x90},
      {"DW_OP_fbreg", 0x91},
      {"DW_OP_bregx", 0x92},
      {"DW_OP_piece", 0x93},
      {"DW_OP_deref_size", 0x94},
      {"DW_OP_xderef_size", 0x95},
      {"DW_OP_nop", 0x96},
      {"DW_OP_push_object_address", 0x97},
      {"DW_OP_call2", 0x98},
      {"DW_OP_call4", 0x99},
      {"DW_OP_call_ref", 0x9A},
      {"DW_OP_form_tls_address", 0x9B},
      {"DW_OP_call_frame_cfa", 0x9C},
      {"DW_OP_bit_piece", 0x9D},
      {"DW_OP_implicit_value", 0x9E},
      {"DW_OP_stack_value", 0x9F},
      {"DW_OP_implicit_pointer", 0xA0},
      {"DW_OP_addrx", 0xA1},
      {"DW_OP_constx", 0xA2},
      {"DW_OP_entry_value", 0xA3},
      {"DW_OP_const_type", 0xA4},
      {"DW_OP_regval_type", 0xA5},
      {"DW_OP_deref_type", 0xA6},
      {"DW_OP_xderef_type", 0xA7},
      {"DW_OP_convert", 0xA8},
      {"DW_OP_reinterpret", 0xA9},
      {"DW_OP_LLVM_fragment", 0x1000},
      {"DW_OP_LLVM_convert", 0x1001},
      {"DW_OP_LLVM_tag_offset", 0x1002},
      {"DW_OP_LLVM_entry_value", 0x1003},
      {"DW_OP_LLVM_implicit_pointer", 0x1004},
      {"DW_OP_LLVM_arg", 0x1005},
      {"DW_OP_LLVM_extract_bits_sext", 0x1006},
      {"DW_OP_LLVM_extract_bits_zext", 0x1007},
  });
  return Opcodes;
}

[[nodiscard]] const auto &attributeEncodings() {
  static constexpr auto Encodings = std::to_array<std::pair<std::string_view, std::uint64_t>>({
      {"DW_ATE_address", 0x01},
      {"DW_ATE_boolean", 0x02},
      {"DW_ATE_complex_float", 0x03},
      {"DW_ATE_float", 0x04},
      {"DW_ATE_signed", 0x05},
      {"DW_ATE_signed_char", 0x06},
      {"DW_ATE_unsigned", 0x07},
      {"DW_ATE_unsigned_char", 0x08},
      {"DW_ATE_imaginary_float", 0x09},
      {"DW_ATE_packed_decimal", 0x0A},
      {"DW_ATE_numeric_string", 0x0B},
      {"DW_ATE_edited", 0x0C},
      {"DW_ATE_signed_fixed", 0x0D},
      {"DW_ATE_unsigned_fixed", 0x0E},
      {"DW_ATE_decimal_float", 0x0F},
      {"DW_ATE_UTF", 0x10},
      {"DW_ATE_UCS", 0x11},
      {"DW_ATE_ASCII", 0x12},
  });
  return Encodings;
}

[[nodiscard]] std::optional<std::string> opcodeName(std::uint64_t Opcode) {
  if (Opcode >= 0x30U && Opcode <= 0x4FU)
    return "DW_OP_lit" + std::to_string(Opcode - 0x30U);
  if (Opcode >= 0x50U && Opcode <= 0x6FU)
    return "DW_OP_reg" + std::to_string(Opcode - 0x50U);
  if (Opcode >= 0x70U && Opcode <= 0x8FU)
    return "DW_OP_breg" + std::to_string(Opcode - 0x70U);
  const auto &Opcodes = namedOpcodes();
  const auto Found =
      std::ranges::find_if(Opcodes, [Opcode](const auto &Entry) { return Entry.second == Opcode; });
  if (Found == Opcodes.end())
    return std::nullopt;
  return std::string(Found->first);
}

[[nodiscard]] std::size_t opcodeSize(std::uint64_t Opcode) noexcept {
  if (Opcode >= 0x70U && Opcode <= 0x8FU)
    return 2;
  switch (Opcode) {
  case 0x92:
  case 0x1000:
  case 0x1001:
  case 0x1006:
  case 0x1007:
    return 3;
  case 0x10:
  case 0x11:
  case 0x23:
  case 0x90:
  case 0x94:
  case 0x1002:
  case 0x1003:
  case 0x1005:
    return 2;
  default:
    return 1;
  }
}

[[nodiscard]] std::optional<std::string_view> attributeName(std::uint64_t Encoding) noexcept {
  const auto &Encodings = attributeEncodings();
  const auto Found = std::ranges::find_if(
      Encodings, [Encoding](const auto &Entry) { return Entry.second == Encoding; });
  if (Found == Encodings.end())
    return std::nullopt;
  return Found->first;
}

} // namespace

std::optional<std::uint64_t> parseDIExpressionOpcodeName(std::string_view Name) noexcept {
  const auto NumberedOpcode = [Name](std::string_view Prefix,
                                     std::uint64_t Base) -> std::optional<std::uint64_t> {
    if (!Name.starts_with(Prefix))
      return std::nullopt;
    std::uint64_t Index = 0;
    if (!parseIndex(Name.substr(Prefix.size()), Index) || Index > 31)
      return std::nullopt;
    return Base + Index;
  };
  if (auto Opcode = NumberedOpcode("DW_OP_lit", 0x30U))
    return Opcode;
  if (auto Opcode = NumberedOpcode("DW_OP_reg", 0x50U))
    return Opcode;
  if (auto Opcode = NumberedOpcode("DW_OP_breg", 0x70U))
    return Opcode;

  const auto &NamedOpcodes = namedOpcodes();
  const auto *const Found =
      std::ranges::find_if(NamedOpcodes, [Name](const auto &Entry) { return Entry.first == Name; });
  if (Found == NamedOpcodes.end())
    return std::nullopt;
  return Found->second;
}

std::optional<std::uint64_t>
parseDIExpressionAttributeEncodingName(std::string_view Name) noexcept {
  const auto &Encodings = attributeEncodings();
  const auto Found =
      std::ranges::find_if(Encodings, [Name](const auto &Entry) { return Entry.first == Name; });
  if (Found == Encodings.end())
    return std::nullopt;
  return Found->second;
}

std::string invalidDIExpressionElementMessage(std::string_view Name) {
  if (Name.starts_with("DW_OP_"))
    return "invalid DWARF op '" + std::string(Name) + "'";
  if (Name.starts_with("DW_ATE_"))
    return "invalid DWARF attribute encoding '" + std::string(Name) + "'";
  return "expected unsigned integer";
}

std::string formatDIExpression(std::span<const std::uint64_t> Elements) {
  std::string Result = "!DIExpression(";
  std::size_t Position = 0;
  bool First = true;
  while (Position < Elements.size()) {
    const std::uint64_t Opcode = Elements[Position];
    const auto Name = opcodeName(Opcode);
    const std::size_t Size = opcodeSize(Opcode);
    if (!Name || Position + Size > Elements.size()) {
      Result = "!DIExpression(";
      for (std::size_t Index = 0; Index < Elements.size(); ++Index) {
        if (Index != 0)
          Result.append(", ");
        Result.append(std::to_string(Elements[Index]));
      }
      Result.push_back(')');
      return Result;
    }

    if (!First)
      Result.append(", ");
    First = false;
    Result.append(*Name);
    for (std::size_t Argument = 1; Argument < Size; ++Argument) {
      Result.append(", ");
      if (Opcode == 0x1001U && Argument == 2U) {
        if (const auto Encoding = attributeName(Elements[Position + Argument])) {
          Result.append(*Encoding);
          continue;
        }
      }
      Result.append(std::to_string(Elements[Position + Argument]));
    }
    Position += Size;
  }
  Result.push_back(')');
  return Result;
}

} // namespace rcc::mir

namespace rcc::mir {

using parser_detail::parseSigned;
using parser_detail::parseUnsigned;
using parser_detail::parseUnsigned32;

Expected<void> MetadataTextParser::expect(MITokenKind Kind, std::string_view Description) {
  if (consume(Kind))
    return {};
  return makeUnexpected(ErrorCode::ParseError,
                        "expected " + std::string(Description) + " in MIR metadata scalar");
}

Expected<void> MetadataTextParser::finish() {
  while (consume(MITokenKind::Newline)) {
  }
  if (Lexer.failed()) {
    if (!Lexer.diagnostics().empty()) {
      return makeUnexpected(ErrorCode::ParseError,
                            std::string(Lexer.diagnostics().front().Message));
    }
    return makeUnexpected(ErrorCode::ParseError, "invalid MIR metadata scalar");
  }
  if (!at(MITokenKind::EndOfFile))
    return makeUnexpected(ErrorCode::ParseError, "unexpected trailing MIR metadata tokens");
  return {};
}

Expected<MachineMetadataReference> MetadataTextParser::parseReference() {
  if (auto Exclaim = expect(MITokenKind::Exclaim, "'!' before a metadata slot"); !Exclaim)
    return std::unexpected(Exclaim.error());
  if (!at(MITokenKind::IntegerLiteral))
    return makeUnexpected(ErrorCode::ParseError, "expected a 32-bit metadata slot");
  std::uint32_t Slot = 0;
  if (!parseUnsigned32(token().integerValue(), Slot))
    return makeUnexpected(ErrorCode::ParseError, "metadata slot is outside the 32-bit range");
  advance();
  return Resolve(Slot);
}

Expected<MachineMetadataReference> MetadataTextParser::parseReferenceOnly() {
  auto Reference = parseReference();
  if (!Reference)
    return std::unexpected(Reference.error());
  if (auto Finished = finish(); !Finished)
    return std::unexpected(Finished.error());
  return Reference;
}

Expected<MachineDIExpression> MetadataTextParser::parseExpression() {
  if (!at(MITokenKind::MetadataKeyword) || !token().isMetadataKeyword("!DIExpression")) {
    return makeUnexpected(ErrorCode::ParseError, "expected an inline !DIExpression");
  }
  advance();
  if (auto Left = expect(MITokenKind::LeftParen, "'(' after !DIExpression"); !Left)
    return std::unexpected(Left.error());
  std::vector<std::uint64_t> Elements;
  while (!at(MITokenKind::RightParen)) {
    if (at(MITokenKind::Identifier)) {
      const auto Opcode = parseDIExpressionOpcodeName(token().stringValue());
      const auto Encoding = parseDIExpressionAttributeEncodingName(token().stringValue());
      if (!Opcode && !Encoding)
        return makeUnexpected(ErrorCode::ParseError,
                              invalidDIExpressionElementMessage(token().stringValue()));
      Elements.push_back(Opcode ? *Opcode : *Encoding);
      advance();
    } else if (at(MITokenKind::IntegerLiteral)) {
      std::int64_t Signed = 0;
      std::uint64_t Unsigned = 0;
      if (parseSigned(token().integerValue(), Signed))
        Elements.push_back(static_cast<std::uint64_t>(Signed));
      else if (parseUnsigned(token().integerValue(), Unsigned))
        Elements.push_back(Unsigned);
      else
        return makeUnexpected(ErrorCode::ParseError, "DIExpression integer exceeds 64 bits");
      advance();
    } else {
      return makeUnexpected(ErrorCode::ParseError, "expected a DIExpression opcode or integer");
    }
    if (!consume(MITokenKind::Comma))
      break;
  }
  if (auto Right = expect(MITokenKind::RightParen, "')' after !DIExpression"); !Right)
    return std::unexpected(Right.error());
  return MachineDIExpression(std::move(Elements));
}

Expected<MachineMetadataValue> MetadataTextParser::parseValue() {
  MachineMetadataValue Value;
  if (at(MITokenKind::Exclaim)) {
    auto Reference = parseReference();
    if (!Reference)
      return std::unexpected(Reference.error());
    auto Created = MachineMetadataValue::reference(*Reference);
    if (!Created)
      return std::unexpected(Created.error());
    Value = std::move(*Created);
  } else {
    auto Expression = parseExpression();
    if (!Expression)
      return std::unexpected(Expression.error());
    Value = MachineMetadataValue::expression(std::move(*Expression));
  }
  if (auto Finished = finish(); !Finished)
    return std::unexpected(Finished.error());
  return Value;
}

Expected<void>
MetadataTextParser::parseDILocation(std::vector<MachineDILocationFrame> &Frames,
                                    std::optional<MachineMetadataReference> &Terminal) {
  if (!at(MITokenKind::MetadataKeyword) || !token().isMetadataKeyword("!DILocation")) {
    return makeUnexpected(ErrorCode::ParseError, "expected an inline !DILocation");
  }
  advance();
  if (auto Left = expect(MITokenKind::LeftParen, "'(' after !DILocation"); !Left)
    return Left;

  MachineDILocationFrame Frame;
  bool HasLine = false;
  bool HasScope = false;
  std::vector<MachineDILocationFrame> NestedFrames;
  std::optional<MachineMetadataReference> NestedTerminal;
  std::unordered_set<std::string> Seen;
  while (!at(MITokenKind::RightParen)) {
    if (!at(MITokenKind::Identifier))
      return makeUnexpected(ErrorCode::ParseError, "expected a DILocation field name");
    const std::string Field(token().stringValue());
    if (!Seen.insert(Field).second) {
      return makeUnexpected(ErrorCode::ParseError, "duplicate DILocation field '" + Field + "'");
    }
    advance();
    if (auto Colon = expect(MITokenKind::Colon, "':' after a DILocation field"); !Colon)
      return Colon;

    if (Field == "line") {
      std::uint32_t Value = 0;
      if (!at(MITokenKind::IntegerLiteral) || !parseUnsigned32(token().integerValue(), Value)) {
        return makeUnexpected(ErrorCode::ParseError,
                              "DILocation line must be an unsigned 32-bit integer");
      }
      Frame.Line = Value;
      HasLine = true;
      advance();
    } else if (Field == "column") {
      std::uint64_t Value = 0;
      if (!at(MITokenKind::IntegerLiteral) || !parseUnsigned(token().integerValue(), Value) ||
          Value > std::numeric_limits<std::uint16_t>::max()) {
        return makeUnexpected(ErrorCode::ParseError,
                              "DILocation column must be an unsigned 16-bit integer");
      }
      Frame.Column = static_cast<std::uint16_t>(Value);
      advance();
    } else if (Field == "scope") {
      auto Scope = parseReference();
      if (!Scope)
        return std::unexpected(Scope.error());
      if (Scope->scope() != MachineMetadataScope::EmbeddedIR) {
        return makeUnexpected(ErrorCode::ParseError,
                              "DILocation scope must reference embedded LLVM IR metadata");
      }
      Frame.Scope = *Scope;
      HasScope = true;
    } else if (Field == "inlinedAt") {
      if (at(MITokenKind::MetadataKeyword) && token().isMetadataKeyword("!DILocation")) {
        if (auto Nested = parseDILocation(NestedFrames, NestedTerminal); !Nested)
          return Nested;
      } else {
        auto Reference = parseReference();
        if (!Reference)
          return std::unexpected(Reference.error());
        if (Reference->scope() != MachineMetadataScope::EmbeddedIR) {
          return makeUnexpected(ErrorCode::ParseError,
                                "DILocation inlinedAt must reference embedded LLVM IR metadata");
        }
        NestedTerminal = *Reference;
      }
    } else if (Field == "isImplicitCode") {
      if (!at(MITokenKind::Identifier) ||
          (token().stringValue() != "true" && token().stringValue() != "false")) {
        return makeUnexpected(ErrorCode::ParseError,
                              "DILocation isImplicitCode must be true or false");
      }
      Frame.IsImplicitCode = token().stringValue() == "true";
      advance();
    } else if (Field == "atomGroup") {
      std::uint64_t Value = 0;
      if (!at(MITokenKind::IntegerLiteral) || !parseUnsigned(token().integerValue(), Value) ||
          Value > MachineDILocation::MaxAtomGroup) {
        return makeUnexpected(ErrorCode::ParseError, "DILocation atomGroup exceeds 61 bits");
      }
      Frame.AtomGroup = Value;
      advance();
    } else if (Field == "atomRank") {
      std::uint64_t Value = 0;
      if (!at(MITokenKind::IntegerLiteral) || !parseUnsigned(token().integerValue(), Value) ||
          Value > 7) {
        return makeUnexpected(ErrorCode::ParseError, "DILocation atomRank exceeds three bits");
      }
      Frame.AtomRank = static_cast<std::uint8_t>(Value);
      advance();
    } else {
      return makeUnexpected(ErrorCode::ParseError, "unknown DILocation field '" + Field + "'");
    }
    if (!consume(MITokenKind::Comma))
      break;
  }
  if (auto Right = expect(MITokenKind::RightParen, "')' after !DILocation"); !Right)
    return Right;
  if (!HasLine || !HasScope)
    return makeUnexpected(ErrorCode::ParseError, "DILocation requires line and scope fields");
  Frames.push_back(Frame);
  Frames.insert(Frames.end(), NestedFrames.begin(), NestedFrames.end());
  Terminal = NestedTerminal;
  return {};
}

Expected<MachineDebugLocation> MetadataTextParser::parseLocation() {
  MachineDebugLocation Location;
  if (at(MITokenKind::Exclaim)) {
    auto Reference = parseReference();
    if (!Reference)
      return std::unexpected(Reference.error());
    auto Created = MachineDebugLocation::reference(*Reference);
    if (!Created)
      return std::unexpected(Created.error());
    Location = std::move(*Created);
  } else {
    std::vector<MachineDILocationFrame> Frames;
    std::optional<MachineMetadataReference> Terminal;
    if (auto Parsed = parseDILocation(Frames, Terminal); !Parsed)
      return std::unexpected(Parsed.error());
    auto Created = MachineDILocation::create(std::move(Frames), Terminal);
    if (!Created)
      return std::unexpected(Created.error());
    Location = MachineDebugLocation::inlineLocation(std::move(*Created));
  }
  if (auto Finished = finish(); !Finished)
    return std::unexpected(Finished.error());
  return Location;
}

} // namespace rcc::mir

namespace rcc::mir {

Expected<void> verifyMIRDocumentReferences(const MIRDocument &Document,
                                           const MachineFunction &Function, bool UseEmbeddedIR) {
  const EmbeddedIRReferenceIndex *Embedded = UseEmbeddedIR ? Document.embeddedIR() : nullptr;
  const EmbeddedIRFunction *IRFunction = nullptr;
  if (Embedded != nullptr) {
    if (Function.name().empty()) {
      std::size_t AnonymousOrdinal = 0;
      bool FoundFunction = false;
      for (std::size_t Index = 0; Index < Document.unit().functionCount(); ++Index) {
        const MachineFunction *Candidate = Document.unit().functionAt(Index);
        if (Candidate == &Function) {
          FoundFunction = true;
          break;
        }
        if (Candidate != nullptr && Candidate->name().empty())
          ++AnonymousOrdinal;
      }
      if (FoundFunction)
        IRFunction = Embedded->unnamedFunction(AnonymousOrdinal);
    } else {
      IRFunction = Embedded->function(Function.name());
    }
    if (IRFunction == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "machine function has no matching embedded LLVM IR function");
    }
  }

  const auto ValidateGlobal = [Embedded](std::string_view Name,
                                         std::string_view Description) -> Expected<void> {
    if (Embedded == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            std::string(Description) +
                                " requires embedded LLVM IR to resolve global '" +
                                std::string(Name) + "'");
    }
    if (Embedded->hasGlobalValue(Name))
      return {};
    return makeUnexpected(ErrorCode::VerificationError,
                          std::string(Description) +
                              " references an undefined embedded LLVM IR global '" +
                              std::string(Name) + "'");
  };
  const auto ValidateBlockAddress = [Embedded](std::string_view FunctionName,
                                               std::string_view BlockName,
                                               std::string_view Description) -> Expected<void> {
    if (Embedded == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            std::string(Description) +
                                " requires embedded LLVM IR to resolve a block address");
    }
    const EmbeddedIRFunction *AddressFunction = Embedded->function(FunctionName);
    if (AddressFunction == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            std::string(Description) +
                                " references an undefined embedded LLVM IR function '" +
                                std::string(FunctionName) + "'");
    }
    if (AddressFunction->block(BlockName) == nullptr) {
      return makeUnexpected(
          ErrorCode::VerificationError,
          std::string(Description) + " references an undefined embedded LLVM IR block '" +
              std::string(BlockName) + "' in function '" + std::string(FunctionName) + "'");
    }
    return {};
  };
  const auto ValidateIRValue = [IRFunction](std::string_view Name,
                                            std::string_view Description) -> Expected<void> {
    if (IRFunction == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            std::string(Description) +
                                " requires embedded LLVM IR to resolve IR value '%" +
                                std::string(Name) + "'");
    }
    if (IRFunction->value(Name) != nullptr)
      return {};
    return makeUnexpected(ErrorCode::VerificationError,
                          std::string(Description) +
                              " references an undefined embedded LLVM IR value '%" +
                              std::string(Name) + "'");
  };
  const auto ValidateMetadata = [Embedded](MachineMetadataReference Reference,
                                           std::initializer_list<std::string_view> ExpectedTypes,
                                           std::string_view Description) -> Expected<void> {
    if (!Reference.isValid()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            std::string(Description) + " has an invalid metadata reference");
    }
    if (Reference.scope() != MachineMetadataScope::EmbeddedIR)
      return {};
    if (Embedded == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            std::string(Description) + " requires embedded LLVM IR metadata !" +
                                std::to_string(Reference.slot()));
    }
    const EmbeddedIRMetadataDefinition *Definition =
        Embedded->metadata(std::to_string(Reference.slot()));
    if (Definition == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            std::string(Description) +
                                " references undefined embedded LLVM IR metadata !" +
                                std::to_string(Reference.slot()));
    }
    if (ExpectedTypes.size() == 0 ||
        std::ranges::find(ExpectedTypes, Definition->DynamicType) != ExpectedTypes.end()) {
      return {};
    }
    std::string Expected;
    for (const std::string_view Type : ExpectedTypes) {
      if (!Expected.empty())
        Expected.append(" or ");
      Expected.append(Type);
    }
    return makeUnexpected(ErrorCode::VerificationError,
                          std::string(Description) + " requires " + Expected + " metadata, but !" +
                              std::to_string(Reference.slot()) + " is " +
                              (Definition->DynamicType.empty()
                                   ? std::string("an unclassified metadata node")
                                   : Definition->DynamicType));
  };
  const auto ValidateMetadataValue =
      [&ValidateMetadata](const MachineMetadataValue &Value,
                          std::string_view Description) -> Expected<void> {
    const MachineMetadataReference *Reference = Value.reference();
    return Reference == nullptr ? Expected<void>{} : ValidateMetadata(*Reference, {}, Description);
  };
  const auto ValidateLocation =
      [&ValidateMetadata](const MachineDebugLocation &Location,
                          std::string_view Description) -> Expected<void> {
    if (const MachineMetadataReference *Reference = Location.reference())
      return ValidateMetadata(*Reference, {"DILocation"}, Description);
    const MachineDILocation *Inline = Location.inlineLocation();
    if (Inline == nullptr)
      return {};
    for (const MachineDILocationFrame &Frame : Inline->inlineChain()) {
      auto Verification =
          ValidateMetadata(Frame.Scope, {"DISubprogram", "DILexicalBlock", "DILexicalBlockFile"},
                           "inline DILocation scope");
      if (!Verification)
        return Verification;
    }
    if (Inline->terminalInlinedAt()) {
      return ValidateMetadata(*Inline->terminalInlinedAt(), {"DILocation"},
                              "inline DILocation inlinedAt");
    }
    return {};
  };
  const auto ValidateVariableDebugInfo =
      [&ValidateMetadata, &ValidateMetadataValue,
       &ValidateLocation](const MachineVariableDebugInfo &DebugInfo) -> Expected<void> {
    if (DebugInfo.Variable) {
      auto Verification =
          ValidateMetadata(*DebugInfo.Variable, {"DILocalVariable"}, "debug variable");
      if (!Verification)
        return Verification;
    }
    if (DebugInfo.Expression) {
      if (const MachineMetadataReference *Reference = DebugInfo.Expression->reference();
          Reference != nullptr) {
        auto Verification = ValidateMetadata(*Reference, {"DIExpression"}, "debug expression");
        if (!Verification)
          return Verification;
      }
      auto Verification = ValidateMetadataValue(*DebugInfo.Expression, "debug expression");
      if (!Verification)
        return Verification;
    }
    if (DebugInfo.Location)
      return ValidateLocation(*DebugInfo.Location, "debug location");
    return {};
  };
  const auto ValidateGenericType = [Embedded](const auto &Self,
                                              const GenericConstantType &Type) -> Expected<void> {
    if (Type.Kind == GenericConstantTypeKind::Named) {
      if (Embedded == nullptr) {
        return makeUnexpected(
            ErrorCode::VerificationError,
            "generic constant type requires embedded LLVM IR to resolve named type '%" + Type.Name +
                "'");
      }
      if (Embedded->typeDefinition(Type.Name) == nullptr) {
        return makeUnexpected(
            ErrorCode::VerificationError,
            "generic constant type references an undefined embedded LLVM IR named type '%" +
                Type.Name + "'");
      }
    }
    for (const GenericConstantType &Element : Type.Elements) {
      auto Verification = Self(Self, Element);
      if (!Verification)
        return Verification;
    }
    return {};
  };
  const auto ValidateGenericConstant =
      [&ValidateBlockAddress, &ValidateGenericType,
       &ValidateGlobal](const auto &Self, const GenericConstantData &Constant) -> Expected<void> {
    auto TypeVerification = ValidateGenericType(ValidateGenericType, Constant.Type);
    if (!TypeVerification)
      return TypeVerification;
    if (Constant.SourceElementType) {
      TypeVerification = ValidateGenericType(ValidateGenericType, *Constant.SourceElementType);
      if (!TypeVerification)
        return TypeVerification;
    }
    switch (Constant.Kind) {
    case GenericConstantKind::GlobalAddress:
    case GenericConstantKind::DSOLocalEquivalent:
    case GenericConstantKind::NoCFI:
      if (auto Verification = ValidateGlobal(Constant.Symbol, "generic constant"); !Verification)
        return Verification;
      break;
    case GenericConstantKind::BlockAddress:
      if (auto Verification = ValidateBlockAddress(Constant.Symbol, Constant.Block,
                                                   "generic constant blockaddress");
          !Verification) {
        return Verification;
      }
      break;
    default:
      break;
    }
    for (const GenericConstantData &Element : Constant.Elements) {
      auto Verification = Self(Self, Element);
      if (!Verification)
        return Verification;
    }
    return {};
  };

  for (const MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
       Block = Block->next()) {
    if (const auto Name = Block->attributes().irBlockName(); Name) {
      if (IRFunction == nullptr) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "machine basic block requires embedded LLVM IR to resolve block '" +
                                  std::string(*Name) + "'");
      }
      if (IRFunction->block(*Name) == nullptr) {
        return makeUnexpected(
            ErrorCode::VerificationError,
            "machine basic block references an undefined embedded LLVM IR block '" +
                std::string(*Name) + "'");
      }
    }
  }
  for (const StackObject &Object : Function.frameInfo().objects()) {
    const StackObjectDesc *Description = Object.description();
    if (Description == nullptr || !Description->AllocaName)
      continue;
    if (IRFunction == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "stack object requires embedded LLVM IR to resolve alloca '" +
                                *Description->AllocaName + "'");
    }
    if (IRFunction->alloca(*Description->AllocaName) == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "stack object references an undefined embedded LLVM IR alloca '" +
                                *Description->AllocaName + "'");
    }
  }

  for (const MachineConstantPoolEntry &Entry : Function.constantPool().entries()) {
    const MachineConstantPoolEntryDesc *Description = Entry.description();
    if (Description == nullptr)
      continue;
    if (const auto *Generic = std::get_if<GenericConstantData>(&Description->Value)) {
      auto Verification = ValidateGenericConstant(ValidateGenericConstant, *Generic);
      if (!Verification)
        return Verification;
    } else if (const auto *Address = std::get_if<BlockAddressConstant>(&Description->Value)) {
      auto Verification = ValidateBlockAddress(Address->FunctionName, Address->BlockName,
                                               "constant-pool block address");
      if (!Verification)
        return Verification;
    }
  }

  for (const MachineMetadataNode &Node : Function.metadata().nodes()) {
    for (const MachineMetadataElement &Element : Node.Elements) {
      const MachineMetadataReference *Reference = Element.reference();
      if (Reference == nullptr)
        continue;
      auto Verification = ValidateMetadata(*Reference, {}, "machine metadata node");
      if (!Verification)
        return Verification;
    }
  }

  for (const StackObject &Object : Function.frameInfo().objects()) {
    const StackObjectDesc *Description = Object.description();
    if (Description == nullptr || !Description->DebugInfo)
      continue;
    auto Verification = ValidateVariableDebugInfo(*Description->DebugInfo);
    if (!Verification)
      return Verification;
  }
  for (const MachineEntryValueInfo &Entry : Function.records().entryValues()) {
    auto Verification = ValidateVariableDebugInfo(Entry.DebugInfo);
    if (!Verification)
      return Verification;
  }
  for (const MachineCalledGlobal &Called : Function.records().calledGlobals()) {
    auto Verification = ValidateGlobal(Called.Callee, "called-global record");
    if (!Verification)
      return Verification;
  }

  for (const MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
       Block = Block->next()) {
    for (const MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      if (Instruction->debugLocation()) {
        auto Verification =
            ValidateLocation(*Instruction->debugLocation(), "instruction debug location");
        if (!Verification)
          return Verification;
      }
      const std::array InstructionMetadata{
          Instruction->heapAllocMarker(),
          Instruction->pcSections(),
          Instruction->mmra(),
      };
      for (const auto &Reference : InstructionMetadata) {
        if (!Reference)
          continue;
        auto Verification = ValidateMetadata(*Reference, {}, "instruction metadata attachment");
        if (!Verification)
          return Verification;
      }

      for (const MachineOperand &Operand : Instruction->operands()) {
        if (const auto *Global = Operand.getIf<GlobalSymbolOperand>(); Global != nullptr) {
          auto Verification = ValidateGlobal(Global->Name, "global-symbol operand");
          if (!Verification)
            return Verification;
        }
        if (const auto *Address = Operand.getIf<BlockAddressOperand>(); Address != nullptr) {
          auto Verification = ValidateBlockAddress(Address->FunctionName, Address->BlockName,
                                                   "block-address operand");
          if (!Verification)
            return Verification;
        }
        if (const auto *Metadata = Operand.getIf<MetadataOperand>(); Metadata != nullptr) {
          auto Verification = ValidateMetadataValue(Metadata->Value, "metadata operand");
          if (!Verification)
            return Verification;
        }
      }

      for (const MachineMemOperand *Memory : Instruction->memOperands()) {
        const MachineMemOperandDesc &Description = Memory->description();
        const std::optional<std::string_view> BaseName = Description.Base.symbolName();
        switch (Description.Base.kind()) {
        case MemoryBase::Kind::GlobalSymbol:
        case MemoryBase::Kind::CallEntryGlobal:
          if (BaseName) {
            auto Verification = ValidateGlobal(*BaseName, "machine memory base");
            if (!Verification)
              return Verification;
          }
          break;
        case MemoryBase::Kind::IRValue:
          if (BaseName) {
            auto Verification = ValidateIRValue(*BaseName, "machine memory base");
            if (!Verification)
              return Verification;
          }
          break;
        default:
          break;
        }
        const std::array MetadataReferences{
            Description.TBAA,          Description.AliasScope,
            Description.NoAlias,       Description.NoAliasAddressSpace,
            Description.RangeMetadata,
        };
        for (const auto &Reference : MetadataReferences) {
          if (!Reference)
            continue;
          auto Verification = ValidateMetadata(*Reference, {}, "machine memory operand metadata");
          if (!Verification)
            return Verification;
        }
        if (Description.SyncScope.value() == 0)
          continue;
        const auto Scope =
            std::ranges::find_if(Document.syncScopes(), [&Description](const auto &Slot) {
              return Slot.ID == Description.SyncScope;
            });
        if (Scope == Document.syncScopes().end()) {
          return makeUnexpected(
              ErrorCode::VerificationError,
              "machine memory operand synchronization scope has no document name");
        }
      }
    }
  }
  return {};
}

} // namespace rcc::mir

namespace rcc::mir {

using parser_detail::parseBool;
using parser_detail::parsePrefixedID;
using parser_detail::parseSigned;
using parser_detail::parseUnsigned;
using parser_detail::parseUnsigned32;
using parser_detail::trim;

bool MIRDocumentParser::parseStackObjects(const yaml::Node &Root, MachineFunction &Function,
                                          MIParserContext &Context) {
  auto ParseList = [this, &Function, &Context](const yaml::Node &List, bool Fixed) -> bool {
    if (requireKind(List, yaml::NodeKind::Sequence,
                    Fixed ? "fixed stack objects" : "stack objects")) {
      return true;
    }
    constexpr std::array FixedKeys{
        std::string_view{"id"},
        std::string_view{"type"},
        std::string_view{"offset"},
        std::string_view{"size"},
        std::string_view{"alignment"},
        std::string_view{"stack-id"},
        std::string_view{"isImmutable"},
        std::string_view{"isAliased"},
        std::string_view{"callee-saved-register"},
        std::string_view{"callee-saved-restored"},
        std::string_view{"debug-info-variable"},
        std::string_view{"debug-info-expression"},
        std::string_view{"debug-info-location"},
    };
    constexpr std::array StackKeys{
        std::string_view{"id"},
        std::string_view{"name"},
        std::string_view{"type"},
        std::string_view{"offset"},
        std::string_view{"size"},
        std::string_view{"alignment"},
        std::string_view{"stack-id"},
        std::string_view{"callee-saved-register"},
        std::string_view{"callee-saved-restored"},
        std::string_view{"local-offset"},
        std::string_view{"debug-info-variable"},
        std::string_view{"debug-info-expression"},
        std::string_view{"debug-info-location"},
    };
    std::unordered_set<std::uint32_t> IDs;
    for (const yaml::NodeID ElementID : List.sequence()) {
      const yaml::Node *Element = node(ElementID);
      if (Element == nullptr || requireKind(*Element, yaml::NodeKind::Mapping, "stack object") ||
          (Fixed ? validateKeys(*Element, FixedKeys, "fixed stack object")
                 : validateKeys(*Element, StackKeys, "stack object"))) {
        return true;
      }
      const yaml::Node *IDNode = mappingValue(*Element, "id");
      if (IDNode == nullptr)
        return fail(Element->range(), "stack object requires 'id'");
      std::uint64_t WideID = 0;
      if (unsignedValue(*IDNode, WideID, "a 32-bit stack object ID") ||
          WideID > std::numeric_limits<std::uint32_t>::max()) {
        return true;
      }
      const auto TextID = static_cast<std::uint32_t>(WideID);
      if (!IDs.insert(TextID).second)
        return fail(IDNode->range(), "duplicate stack object ID");

      StackObjectDesc Description;
      Description.Kind = Fixed ? StackObjectKind::FixedABI : StackObjectKind::Default;
      Description.Offset = 0;
      if (const yaml::Node *NameNode = mappingValue(*Element, "name")) {
        std::string_view Name;
        if (scalar(*NameNode, Name, "stack object name"))
          return true;
        if (!Name.empty())
          Description.AllocaName = std::string(Name);
      }
      if (const yaml::Node *TypeNode = mappingValue(*Element, "type")) {
        std::string_view Type;
        if (scalar(*TypeNode, Type, "stack object type"))
          return true;
        if (Type == "default")
          Description.Kind = Fixed ? StackObjectKind::FixedABI : StackObjectKind::Default;
        else if (Type == "spill-slot")
          Description.Kind = Fixed ? StackObjectKind::FixedSpillSlot : StackObjectKind::SpillSlot;
        else if (!Fixed && Type == "variable-sized")
          Description.Kind = StackObjectKind::VariableSized;
        else
          return fail(TypeNode->range(), "unknown stack object type '" + std::string(Type) + "'");
      }
      if (const yaml::Node *OffsetNode = mappingValue(*Element, "offset")) {
        std::int64_t Offset = 0;
        if (signedValue(*OffsetNode, Offset, "a signed stack object offset"))
          return true;
        Description.Offset = Offset;
      } else if (Fixed) {
        Description.Offset = 0;
      }
      if (Description.Kind != StackObjectKind::VariableSized) {
        const yaml::Node *SizeNode = mappingValue(*Element, "size");
        if (SizeNode == nullptr) {
          if (!Fixed)
            return fail(Element->range(), "fixed-size stack object requires 'size'");
          Description.Size = 0;
        } else {
          std::uint64_t Size = 0;
          if (unsignedValue(*SizeNode, Size, "an unsigned stack object size"))
            return true;
          Description.Size = Size;
        }
      }
      if (const yaml::Node *AlignmentNode = mappingValue(*Element, "alignment")) {
        if (alignment(*AlignmentNode, Description.Align, true))
          return true;
      }
      if (const yaml::Node *StackIDNode = mappingValue(*Element, "stack-id")) {
        std::string_view Name;
        if (scalar(*StackIDNode, Name, "stack ID"))
          return true;
        const auto Stack = parseStackIDName(Name);
        if (!Stack)
          return fail(StackIDNode->range(), "unknown stack ID '" + std::string(Name) + "'");
        Description.Stack = *Stack;
      }
      if (const yaml::Node *ImmutableNode = mappingValue(*Element, "isImmutable")) {
        if (boolean(*ImmutableNode, Description.IsImmutable))
          return true;
      }
      if (const yaml::Node *AliasedNode = mappingValue(*Element, "isAliased")) {
        if (boolean(*AliasedNode, Description.IsAliased))
          return true;
      }
      if (const yaml::Node *SavedNode = mappingValue(*Element, "callee-saved-register")) {
        std::string_view SavedName;
        if (scalar(*SavedNode, SavedName, "physical register"))
          return true;
        if (!trim(SavedName).empty()) {
          PhysicalRegister Reg;
          if (physicalRegister(SavedName, SavedNode->range(), Reg))
            return true;
          Description.CalleeSavedRegister = Reg;
        }
      }
      if (const yaml::Node *RestoredNode = mappingValue(*Element, "callee-saved-restored")) {
        if (boolean(*RestoredNode, Description.CalleeSavedRestored))
          return true;
      }
      if (const yaml::Node *LocalNode = mappingValue(*Element, "local-offset")) {
        std::int64_t Offset = 0;
        if (signedValue(*LocalNode, Offset, "a signed local frame offset"))
          return true;
        Description.LocalOffset = Offset;
      }
      MachineVariableDebugInfo DebugInfo;
      if (parseVariableDebugInfo(*Element, Function, DebugInfo))
        return true;
      if (DebugInfo.Variable || DebugInfo.Expression || DebugInfo.Location)
        Description.DebugInfo = std::move(DebugInfo);

      auto Created = Function.frameInfo().createObject(std::move(Description));
      if (!Created)
        return importError(Element->range(), Created.error());
      MIFrameObjectSlot Slot{
          .TextID = TextID,
          .Index = *Created,
          .Name = std::nullopt,
      };
      if (const StackObjectDesc *CreatedDescription =
              Function.frameInfo().object(*Created)->description();
          CreatedDescription != nullptr && CreatedDescription->AllocaName) {
        Slot.Name = *CreatedDescription->AllocaName;
      }
      (Fixed ? Context.FixedStackObjects : Context.StackObjects).push_back(std::move(Slot));
    }
    return false;
  };

  if (const yaml::Node *Fixed = mappingValue(Root, "fixedStack")) {
    if (ParseList(*Fixed, true))
      return true;
  }
  if (const yaml::Node *Stack = mappingValue(Root, "stack")) {
    if (ParseList(*Stack, false))
      return true;
  }
  return false;
}

bool MIRDocumentParser::parseFrameReference(const yaml::Node &Node, const MIParserContext &Context,
                                            FrameIndex &Index) {
  std::string_view Text;
  if (scalar(Node, Text, "frame index"))
    return true;
  bool Fixed = false;
  std::optional<std::uint32_t> ID = parsePrefixedID(Text, "%stack.");
  if (!ID) {
    ID = parsePrefixedID(Text, "%fixed-stack.");
    Fixed = true;
  }
  if (!ID)
    return fail(Node.range(), "frame index must use '%stack.N' or '%fixed-stack.N'");
  const auto &Slots = Fixed ? Context.FixedStackObjects : Context.StackObjects;
  const auto Found = std::ranges::find_if(
      Slots, [ID](const MIFrameObjectSlot &Slot) { return Slot.TextID == *ID; });
  if (Found == Slots.end())
    return fail(Node.range(), "reference to undefined frame object");
  Index = Found->Index;
  return false;
}

MachineBasicBlock *MIRDocumentParser::blockAt(MachineFunction &Function,
                                              std::uint32_t Number) const noexcept {
  for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr; Block = Block->next()) {
    if (Block->denseNumber() == Number)
      return Block;
  }
  return nullptr;
}

MachineInstr *MIRDocumentParser::instructionAt(MachineFunction &Function, std::uint32_t BlockNumber,
                                               std::uint32_t Offset) const noexcept {
  MachineBasicBlock *Block = blockAt(Function, BlockNumber);
  if (Block == nullptr)
    return nullptr;
  MachineInstr *Instruction = Block->firstInstruction();
  for (std::uint32_t Index = 0; Instruction != nullptr && Index < Offset; ++Index)
    Instruction = Instruction->next();
  return Instruction;
}

bool MIRDocumentParser::parseBlockReference(std::string_view Text, const TextRange &Range,
                                            MachineFunction &Function, MachineBasicBlock *&Block) {
  const auto Number = parsePrefixedID(Text, "%bb.");
  if (!Number)
    return fail(Range, "basic block reference must use the '%bb.N' MIR spelling");
  Block = blockAt(Function, *Number);
  if (Block == nullptr)
    return fail(Range, "reference to undefined machine basic block");
  return false;
}

bool MIRDocumentParser::parseSaveRestorePoints(const yaml::Node &Node, MachineFunction &Function,
                                               std::vector<MachineFrameSaveRestorePoint> &Points) {
  if (requireKind(Node, yaml::NodeKind::Sequence, "frame save/restore points"))
    return true;
  constexpr std::array Keys{
      std::string_view{"point"},
      std::string_view{"registers"},
  };
  for (const yaml::NodeID PointID : Node.sequence()) {
    const yaml::Node *PointNode = node(PointID);
    if (PointNode == nullptr ||
        requireKind(*PointNode, yaml::NodeKind::Mapping, "frame save/restore point") ||
        validateKeys(*PointNode, Keys, "frame save/restore point")) {
      return true;
    }
    const yaml::Node *BlockNode = mappingValue(*PointNode, "point");
    if (BlockNode == nullptr)
      return fail(PointNode->range(), "frame save/restore point requires 'point'");
    std::string_view BlockText;
    MachineBasicBlock *Block = nullptr;
    if (scalar(*BlockNode, BlockText, "basic block reference") ||
        parseBlockReference(BlockText, BlockNode->range(), Function, Block)) {
      return true;
    }
    if (Block == nullptr)
      return fail(BlockNode->range(), "reference to undefined machine basic block");
    MachineFrameSaveRestorePoint Point{.Block = Block->stableID(), .Registers = {}};
    if (const yaml::Node *RegistersNode = mappingValue(*PointNode, "registers")) {
      if (requireKind(*RegistersNode, yaml::NodeKind::Sequence, "save/restore point registers")) {
        return true;
      }
      for (const yaml::NodeID RegID : RegistersNode->sequence()) {
        const yaml::Node *RegNode = node(RegID);
        PhysicalRegister Reg;
        if (RegNode == nullptr || physicalRegister(*RegNode, Reg))
          return true;
        Point.Registers.push_back(Reg);
      }
    }
    Points.push_back(std::move(Point));
  }
  return false;
}

bool MIRDocumentParser::parseFrameState(const yaml::Node &Root, MachineFunction &Function,
                                        const MIParserContext &Context) {
  const yaml::Node *Frame = mappingValue(Root, "frameInfo");
  if (Frame == nullptr)
    return false;
  if (requireKind(*Frame, yaml::NodeKind::Mapping, "frameInfo"))
    return true;
  constexpr std::array Keys{
      std::string_view{"isFrameAddressTaken"},
      std::string_view{"isReturnAddressTaken"},
      std::string_view{"hasStackMap"},
      std::string_view{"hasPatchPoint"},
      std::string_view{"stackSize"},
      std::string_view{"offsetAdjustment"},
      std::string_view{"maxAlignment"},
      std::string_view{"adjustsStack"},
      std::string_view{"hasCalls"},
      std::string_view{"framePointerPolicy"},
      std::string_view{"stackProtector"},
      std::string_view{"functionContext"},
      std::string_view{"maxCallFrameSize"},
      std::string_view{"cvBytesOfCalleeSavedRegisters"},
      std::string_view{"hasOpaqueSPAdjustment"},
      std::string_view{"hasVAStart"},
      std::string_view{"hasMustTailInVarArgFunc"},
      std::string_view{"hasTailCall"},
      std::string_view{"isCalleeSavedInfoValid"},
      std::string_view{"localFrameSize"},
      std::string_view{"savePoint"},
      std::string_view{"restorePoint"},
  };
  if (validateKeys(*Frame, Keys, "frameInfo"))
    return true;

  MachineFrameState State = Function.frameInfo().state();
  const std::array BoolBindings{
      std::pair{"isFrameAddressTaken", &State.IsFrameAddressTaken},
      std::pair{"isReturnAddressTaken", &State.IsReturnAddressTaken},
      std::pair{"hasStackMap", &State.HasStackMap},
      std::pair{"hasPatchPoint", &State.HasPatchPoint},
      std::pair{"adjustsStack", &State.AdjustsStack},
      std::pair{"hasCalls", &State.HasCalls},
      std::pair{"hasOpaqueSPAdjustment", &State.HasOpaqueSPAdjustment},
      std::pair{"hasVAStart", &State.HasVAStart},
      std::pair{"hasMustTailInVarArgFunc", &State.HasMustTailInVarArgFunc},
      std::pair{"hasTailCall", &State.HasTailCall},
      std::pair{"isCalleeSavedInfoValid", &State.IsCalleeSavedInfoValid},
  };
  for (const auto &[Key, Destination] : BoolBindings) {
    if (const yaml::Node *Value = mappingValue(*Frame, Key)) {
      if (boolean(*Value, *Destination))
        return true;
    }
  }

  if (const yaml::Node *Value = mappingValue(*Frame, "stackSize")) {
    if (unsignedValue(*Value, State.StackSize, "an unsigned stack size"))
      return true;
  }
  if (const yaml::Node *Value = mappingValue(*Frame, "offsetAdjustment")) {
    std::int64_t Wide = 0;
    if (signedValue(*Value, Wide, "a 32-bit signed offset adjustment") ||
        Wide < std::numeric_limits<std::int32_t>::min() ||
        Wide > std::numeric_limits<std::int32_t>::max()) {
      return fail(Value->range(), "offset adjustment is outside the 32-bit signed range");
    }
    State.OffsetAdjustment = static_cast<std::int32_t>(Wide);
  }
  if (const yaml::Node *Value = mappingValue(*Frame, "maxAlignment")) {
    std::uint64_t Bytes = 0;
    if (unsignedValue(*Value, Bytes, "an unsigned maximum alignment"))
      return true;
    if (Bytes == 0) {
      State.MaxAlignment.reset();
    } else {
      auto Parsed = Alignment::create(Bytes);
      if (!Parsed)
        return importError(Value->range(), Parsed.error());
      State.MaxAlignment = *Parsed;
    }
  }
  if (const yaml::Node *Value = mappingValue(*Frame, "framePointerPolicy")) {
    std::string_view Name;
    if (scalar(*Value, Name, "frame-pointer policy"))
      return true;
    const auto Policy = parseMachineFramePointerPolicy(Name);
    if (!Policy)
      return fail(Value->range(), "unknown frame-pointer policy '" + std::string(Name) + "'");
    State.FramePointerPolicy = *Policy;
  }
  for (const auto &[Key, Destination] : {std::pair{"stackProtector", &State.StackProtector},
                                         std::pair{"functionContext", &State.FunctionContext}}) {
    if (const yaml::Node *Value = mappingValue(*Frame, Key)) {
      std::string_view Reference;
      if (scalar(*Value, Reference, "frame index"))
        return true;
      if (Reference.empty()) {
        Destination->reset();
        continue;
      }
      FrameIndex Index;
      if (parseFrameReference(*Value, Context, Index))
        return true;
      *Destination = Index;
    }
  }
  if (const yaml::Node *Value = mappingValue(*Frame, "maxCallFrameSize")) {
    std::uint64_t Size = 0;
    if (unsignedValue(*Value, Size, "a 32-bit unsigned maximum call-frame size") ||
        Size > std::numeric_limits<std::uint32_t>::max()) {
      return fail(Value->range(), "maximum call-frame size is outside the 32-bit unsigned range");
    }
    if (Size == std::numeric_limits<std::uint32_t>::max())
      State.MaxCallFrameSize.reset();
    else
      State.MaxCallFrameSize = Size;
  }
  for (const auto &[Key, Destination] :
       {std::pair{"cvBytesOfCalleeSavedRegisters", &State.CVBytesOfCalleeSavedRegisters},
        std::pair{"localFrameSize", &State.LocalFrameSize}}) {
    if (const yaml::Node *Value = mappingValue(*Frame, Key)) {
      std::uint64_t Wide = 0;
      if (unsignedValue(*Value, Wide, "a 32-bit unsigned frame value") ||
          Wide > std::numeric_limits<std::uint32_t>::max()) {
        return fail(Value->range(), "frame value is outside the 32-bit unsigned range");
      }
      *Destination = static_cast<std::uint32_t>(Wide);
    }
  }
  if (const yaml::Node *Value = mappingValue(*Frame, "savePoint")) {
    if (parseSaveRestorePoints(*Value, Function, State.SavePoints))
      return true;
  }
  if (const yaml::Node *Value = mappingValue(*Frame, "restorePoint")) {
    if (parseSaveRestorePoints(*Value, Function, State.RestorePoints))
      return true;
  }
  auto StateResult = Function.frameInfo().setState(std::move(State));
  if (!StateResult)
    return importError(Frame->range(), StateResult.error());
  return false;
}

} // namespace rcc::mir

namespace rcc::mir {

using parser_detail::parseBool;
using parser_detail::parsePrefixedID;
using parser_detail::parseSigned;
using parser_detail::parseUnsigned;
using parser_detail::parseUnsigned32;
using parser_detail::trim;

bool MIRDocumentParser::parseFunctionScalars(const yaml::Node &Root, MachineFunction &Function,
                                             MIRFunctionDocumentInfo &DocumentInfo) {
  if (const yaml::Node *AlignNode = mappingValue(Root, "alignment")) {
    Alignment Align;
    if (alignment(*AlignNode, Align, true))
      return true;
    if (Align.bytes() != 1 || AlignNode->scalar() != "0")
      Function.attributes().setAlignment(Align);
  }

  constexpr std::array PropertyBindings{
      std::pair{"legalized", MachineFunctionProperty::Legalized},
      std::pair{"regBankSelected", MachineFunctionProperty::RegBankSelected},
      std::pair{"selected", MachineFunctionProperty::Selected},
      std::pair{"failedISel", MachineFunctionProperty::FailedISel},
      std::pair{"tracksRegLiveness", MachineFunctionProperty::TracksLiveness},
      std::pair{"noPhis", MachineFunctionProperty::NoPHIs},
      std::pair{"isSSA", MachineFunctionProperty::IsSSA},
      std::pair{"noVRegs", MachineFunctionProperty::NoVRegs},
      std::pair{"failsVerification", MachineFunctionProperty::FailsVerification},
  };
  for (const auto &[Key, Property] : PropertyBindings) {
    const yaml::Node *ValueNode = mappingValue(Root, Key);
    if (ValueNode == nullptr)
      continue;
    bool Value = false;
    if (boolean(*ValueNode, Value))
      return true;
    if (Value)
      Function.properties().set(Property);
    else
      Function.properties().clear(Property);
    if (Key == std::string_view("noPhis"))
      DocumentInfo.NoPHIs = Value;
    else if (Key == std::string_view("isSSA"))
      DocumentInfo.IsSSA = Value;
    else if (Key == std::string_view("noVRegs"))
      DocumentInfo.NoVRegs = Value;
  }

  constexpr std::array AttributeBindings{
      std::pair{"exposesReturnsTwice", MachineFunctionAttribute::ExposesReturnsTwice},
      std::pair{"hasWinCFI", MachineFunctionAttribute::HasWinCFI},
      std::pair{"hasFakeUses", MachineFunctionAttribute::HasFakeUses},
      std::pair{"callsEHReturn", MachineFunctionAttribute::CallsEHReturn},
      std::pair{"callsUnwindInit", MachineFunctionAttribute::CallsUnwindInit},
      std::pair{"hasEHContTarget", MachineFunctionAttribute::HasEHContTarget},
      std::pair{"hasEHScopes", MachineFunctionAttribute::HasEHScopes},
      std::pair{"hasEHFunclets", MachineFunctionAttribute::HasEHFunclets},
      std::pair{"isOutlined", MachineFunctionAttribute::IsOutlined},
      std::pair{"debugInstrRef", MachineFunctionAttribute::DebugInstrRef},
      std::pair{"tracksDebugUserValues", MachineFunctionAttribute::TracksDebugUserValues},
  };
  for (const auto &[Key, Attribute] : AttributeBindings) {
    const yaml::Node *ValueNode = mappingValue(Root, Key);
    if (ValueNode == nullptr)
      continue;
    bool Value = false;
    if (boolean(*ValueNode, Value))
      return true;
    if (Value)
      Function.attributes().set(Attribute);
    else
      Function.attributes().clear(Attribute);
    if (Key == std::string_view("hasFakeUses"))
      DocumentInfo.HasFakeUses = Value;
  }

  return false;
}

bool MIRDocumentParser::recomputeProperties(const yaml::Node &Root, MachineFunction &Function,
                                            const MIRFunctionDocumentInfo &DocumentInfo) {
  bool HasPHI = false;
  bool HasFakeUse = false;
  bool IsSSA = true;
  bool HasTiedOperands = false;
  bool AllTiedOperandsRewritten = true;
  bool HasFrameIndex = false;
  bool HasExecutablePseudo = false;
  std::unordered_set<Register> Definitions;
  for (const MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
       Block = Block->next()) {
    for (const MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction->opcode());
      if (Descriptor == nullptr)
        return fail(Root.range(), "instruction has no target descriptor");
      HasPHI |= Descriptor->Flags.has(InstructionFlag::Phi);
      HasFakeUse |= Descriptor->Flags.has(InstructionFlag::FakeUse);
      HasExecutablePseudo |= Descriptor->Flags.has(InstructionFlag::Pseudo) &&
                             !Descriptor->Flags.has(InstructionFlag::Meta) &&
                             !Descriptor->Flags.has(InstructionFlag::TerminalEmission);
      const auto Operands = Instruction->operands();
      for (std::size_t Index = 0; Index < Operands.size(); ++Index) {
        const MachineOperand &Operand = Operands[Index];
        HasFrameIndex |= Operand.getIf<FrameIndexOperand>() != nullptr;
        const auto Reg = Operand.registerValue();
        if (Reg && Reg->isVirtual() && Operand.isDef() && !Definitions.insert(*Reg).second) {
          IsSSA = false;
        }
        if (!Operand.tiedOperandIndex() || !Operand.isUse())
          continue;
        HasTiedOperands = true;
        const MachineOperand *Definition = Instruction->operandAt(*Operand.tiedOperandIndex());
        if (Definition == nullptr || Definition->registerValue() != Reg)
          AllTiedOperandsRewritten = false;
      }
      for (const MachineMemOperand *Memory : Instruction->memOperands())
        HasFrameIndex |= Memory->description().Base.frameIndex().has_value();
    }
  }
  const bool NoPHIs = !HasPHI;
  const bool NoVRegs =
      std::ranges::none_of(Function.registerInfo().virtualRegisters(),
                           [](const VirtualRegisterInfo &Info) { return !Info.IsTombstone; });

  const auto ApplyComputed = [this, &Root, &Function](std::optional<bool> Explicit, bool Computed,
                                                      MachineFunctionProperty Property,
                                                      std::string_view Conflict) -> bool {
    if (Explicit && *Explicit && !Computed) {
      return fail(Root.range(), std::string(Function.name()) + std::string(Conflict));
    }
    if (Explicit.value_or(Computed))
      Function.properties().set(Property);
    else
      Function.properties().clear(Property);
    return false;
  };
  if (ApplyComputed(DocumentInfo.NoPHIs, NoPHIs, MachineFunctionProperty::NoPHIs,
                    " has explicit property NoPhi, but contains at least one PHI") ||
      ApplyComputed(DocumentInfo.IsSSA, IsSSA, MachineFunctionProperty::IsSSA,
                    " has explicit property IsSSA, but is not valid SSA") ||
      ApplyComputed(DocumentInfo.NoVRegs, NoVRegs, MachineFunctionProperty::NoVRegs,
                    " has explicit property NoVRegs, but contains virtual registers")) {
    return true;
  }

  if (DocumentInfo.HasFakeUses && !*DocumentInfo.HasFakeUses && HasFakeUse) {
    return fail(Root.range(),
                std::string(Function.name()) +
                    " has explicit property hasFakeUses=false, but contains fake uses");
  }
  if (DocumentInfo.HasFakeUses.value_or(HasFakeUse))
    Function.attributes().set(MachineFunctionAttribute::HasFakeUses);
  else
    Function.attributes().clear(MachineFunctionAttribute::HasFakeUses);

  const bool Selected = Function.properties().has(MachineFunctionProperty::Selected);
  const bool StructurallyPostSSA =
      Selected && NoPHIs && !Function.properties().has(MachineFunctionProperty::IsSSA);
  if (AllTiedOperandsRewritten && (HasTiedOperands || StructurallyPostSSA))
    Function.properties().set(MachineFunctionProperty::TiedOpsRewritten);
  else
    Function.properties().clear(MachineFunctionProperty::TiedOpsRewritten);

  if (HasFrameIndex)
    Function.properties().clear(MachineFunctionProperty::NoFrameIndices);
  else
    Function.properties().set(MachineFunctionProperty::NoFrameIndices);

  const MachineFrameState &FrameState = Function.frameInfo().state();
  const bool StructurallyFinalizedFrame = Selected && NoVRegs && !HasFrameIndex &&
                                          FrameState.IsCalleeSavedInfoValid &&
                                          FrameState.MaxAlignment.has_value();
  if (StructurallyFinalizedFrame) {
    if (!Function.frameInfo().isFrozen()) {
      auto Frozen = Function.frameInfo().freeze(FrameState.StackSize, *FrameState.MaxAlignment);
      if (!Frozen)
        return importError(Root.range(), Frozen.error(), "mir-frame-finalization");
    }
    Function.properties().set(MachineFunctionProperty::FrameFinalized);
  } else {
    Function.properties().clear(MachineFunctionProperty::FrameFinalized);
  }

  if (StructurallyFinalizedFrame && !HasExecutablePseudo)
    Function.properties().set(MachineFunctionProperty::NoExecutablePseudos);
  else
    Function.properties().clear(MachineFunctionProperty::NoExecutablePseudos);
  return false;
}

bool MIRDocumentParser::parseRegisters(const yaml::Node &Root, MachineFunction &Function,
                                       MIParserContext &Context) {
  const yaml::Node *Registers = mappingValue(Root, "registers");
  if (Registers != nullptr) {
    if (requireKind(*Registers, yaml::NodeKind::Sequence, "registers"))
      return true;
    constexpr std::array RegisterKeys{
        std::string_view{"id"},
        std::string_view{"class"},
        std::string_view{"preferred-register"},
        std::string_view{"flags"},
    };
    std::unordered_set<std::uint32_t> IDs;
    for (const yaml::NodeID ElementID : Registers->sequence()) {
      const yaml::Node *Element = node(ElementID);
      if (Element == nullptr || requireKind(*Element, yaml::NodeKind::Mapping, "register entry") ||
          validateKeys(*Element, RegisterKeys, "register")) {
        return true;
      }
      const yaml::Node *IDNode = mappingValue(*Element, "id");
      const yaml::Node *ClassNode = mappingValue(*Element, "class");
      if (IDNode == nullptr || ClassNode == nullptr)
        return fail(Element->range(), "register entry requires 'id' and 'class'");
      std::uint64_t WideID = 0;
      std::string_view ClassName;
      if (unsignedValue(*IDNode, WideID, "a 32-bit virtual-register ID") ||
          WideID > std::numeric_limits<std::uint32_t>::max() ||
          scalar(*ClassNode, ClassName, "register class or bank")) {
        return true;
      }
      const auto TextID = static_cast<std::uint32_t>(WideID);
      if (!IDs.insert(TextID).second)
        return fail(IDNode->range(), "duplicate virtual register ID");
      auto Created = Function.registerInfo().createVirtualRegister();
      if (!Created)
        return importError(IDNode->range(), Created.error());
      Context.VirtualRegisters.push_back({TextID, *Created});

      if (ClassName != "_") {
        if (const RegisterClassDesc *Class = Target.registers().registerClass(ClassName)) {
          auto SetResult = Function.registerInfo().setRegisterClass(*Created, Class->ID);
          if (!SetResult)
            return importError(ClassNode->range(), SetResult.error());
          SetResult = Function.registerInfo().setRegisterBank(*Created, Class->DefaultBank);
          if (!SetResult)
            return importError(ClassNode->range(), SetResult.error());
        } else if (const RegisterBankDesc *Bank = Target.registers().registerBank(ClassName)) {
          auto SetResult = Function.registerInfo().setRegisterBank(*Created, Bank->ID);
          if (!SetResult)
            return importError(ClassNode->range(), SetResult.error());
        } else {
          return fail(ClassNode->range(),
                      "unknown register class or bank '" + std::string(ClassName) + "'");
        }
      }

      if (const yaml::Node *Preferred = mappingValue(*Element, "preferred-register")) {
        std::string_view PreferredName;
        PhysicalRegister Reg;
        if (scalar(*Preferred, PreferredName, "physical register"))
          return true;
        if (!trim(PreferredName).empty()) {
          if (physicalRegister(PreferredName, Preferred->range(), Reg))
            return true;
          auto AddResult = Function.registerInfo().addAllocationHint(*Created, Reg);
          if (!AddResult)
            return importError(Preferred->range(), AddResult.error());
        }
      }
      if (const yaml::Node *Flags = mappingValue(*Element, "flags")) {
        if (requireKind(*Flags, yaml::NodeKind::Sequence, "virtual-register flags"))
          return true;
        std::uint8_t Mask = 0;
        for (const yaml::NodeID FlagID : Flags->sequence()) {
          const yaml::Node *FlagNode = node(FlagID);
          std::string_view Name;
          if (FlagNode == nullptr || scalar(*FlagNode, Name, "virtual-register flag"))
            return true;
          const auto Flag = Target.mirText().virtualRegisterFlag(Name);
          if (!Flag)
            return fail(FlagNode->range(),
                        "use of undefined register flag '" + std::string(Name) + "'");
          if ((Mask & *Flag) != 0)
            return fail(FlagNode->range(), "duplicate virtual-register flag");
          Mask = static_cast<std::uint8_t>(Mask | *Flag);
        }
        auto SetResult = Function.registerInfo().setTargetRegisterFlags(*Created, Mask);
        if (!SetResult)
          return importError(Flags->range(), SetResult.error());
      }
    }
  }

  const yaml::Node *LiveIns = mappingValue(Root, "liveins");
  if (LiveIns != nullptr) {
    if (requireKind(*LiveIns, yaml::NodeKind::Sequence, "liveins"))
      return true;
    constexpr std::array LiveInKeys{
        std::string_view{"reg"},
        std::string_view{"virtual-reg"},
    };
    for (const yaml::NodeID ElementID : LiveIns->sequence()) {
      const yaml::Node *Element = node(ElementID);
      if (Element == nullptr || requireKind(*Element, yaml::NodeKind::Mapping, "live-in entry") ||
          validateKeys(*Element, LiveInKeys, "live-in")) {
        return true;
      }
      const yaml::Node *RegNode = mappingValue(*Element, "reg");
      if (RegNode == nullptr)
        return fail(Element->range(), "live-in entry requires 'reg'");
      PhysicalRegister Reg;
      if (physicalRegister(*RegNode, Reg))
        return true;
      std::optional<Register> Initial;
      if (const yaml::Node *VirtualNode = mappingValue(*Element, "virtual-reg")) {
        std::string_view Text;
        if (scalar(*VirtualNode, Text, "virtual register"))
          return true;
        if (!trim(Text).empty()) {
          Register Virtual;
          if (virtualRegister(Text, VirtualNode->range(), Function, Context, Virtual))
            return true;
          Initial = Virtual;
        }
      }
      auto AddResult = Function.registerInfo().addLiveIn(Reg, Initial);
      if (!AddResult)
        return importError(Element->range(), AddResult.error());
    }
  }

  if (const yaml::Node *CalleeSaved = mappingValue(Root, "calleeSavedRegisters")) {
    if (requireKind(*CalleeSaved, yaml::NodeKind::Sequence, "callee-saved registers"))
      return true;
    std::vector<PhysicalRegister> RegistersValue;
    for (const yaml::NodeID RegID : CalleeSaved->sequence()) {
      const yaml::Node *RegNode = node(RegID);
      PhysicalRegister Reg;
      if (RegNode == nullptr || physicalRegister(*RegNode, Reg))
        return true;
      RegistersValue.push_back(Reg);
    }
    auto SetResult = Function.records().setCalleeSavedRegisters(std::move(RegistersValue));
    if (!SetResult)
      return importError(CalleeSaved->range(), SetResult.error());
  }
  return false;
}

TextRange MIRDocumentParser::scalarDiagnosticRange(const yaml::Node &Scalar,
                                                   const TextRange &ScalarRange) const noexcept {
  if (Scalar.scalarStyle() != yaml::ScalarStyle::Literal &&
      Scalar.scalarStyle() != yaml::ScalarStyle::Folded) {
    std::size_t ContentOffset = Scalar.range().Begin.Offset;
    if (Scalar.scalarStyle() == yaml::ScalarStyle::SingleQuoted ||
        Scalar.scalarStyle() == yaml::ScalarStyle::DoubleQuoted) {
      ++ContentOffset;
    }
    return Result.Source.range(ContentOffset + ScalarRange.Begin.Offset,
                               ContentOffset + ScalarRange.End.Offset);
  }

  const std::size_t FirstBodyLine = Scalar.range().Begin.Line + 1U;
  const std::size_t LastBodyLine = Scalar.range().End.Line;
  if (FirstBodyLine > LastBodyLine || FirstBodyLine == 0)
    return Scalar.range();

  std::size_t Indent = 0;
  bool FoundContent = false;
  for (std::size_t Line = FirstBodyLine; Line <= LastBodyLine; ++Line) {
    const std::string_view Text = Result.Source.line(Line);
    std::size_t CurrentIndent = 0;
    while (CurrentIndent < Text.size() && Text[CurrentIndent] == ' ')
      ++CurrentIndent;
    if (CurrentIndent == Text.size())
      continue;
    Indent = CurrentIndent;
    FoundContent = true;
    break;
  }
  if (!FoundContent)
    return Scalar.range();

  const TextPosition DefaultPosition = Scalar.range().Begin;
  const auto MapPosition = [this, FirstBodyLine, LastBodyLine, Indent,
                            DefaultPosition](TextPosition Position) {
    const std::size_t SourceLine = FirstBodyLine + Position.Line - 1U;
    if (Position.Line == 0 || SourceLine > LastBodyLine || SourceLine > Result.Source.lineCount()) {
      return DefaultPosition;
    }
    const std::string_view SourceLineText = Result.Source.line(SourceLine);
    const std::size_t BodyColumn = Position.Column == 0 ? 0 : Position.Column - 1U;
    const std::size_t SourceColumn = std::min(Indent + BodyColumn, SourceLineText.size());
    const std::size_t Offset = Result.Source.lineOffsets()[SourceLine - 1U] + SourceColumn;
    return Result.Source.positionAt(Offset);
  };
  return {.Begin = MapPosition(ScalarRange.Begin), .End = MapPosition(ScalarRange.End)};
}

bool MIRDocumentParser::parseBody(const yaml::Node &Root, MachineFunction &Function,
                                  MIParserContext &Context) {
  const yaml::Node *Body = mappingValue(Root, "body");
  std::string BodyText;
  TextRange BodyRange = Root.range();
  if (Body != nullptr) {
    std::string_view Text;
    if (scalar(*Body, Text, "machine function body"))
      return true;
    BodyText = Text;
    BodyRange = Body->range();
  }

  if (!effectiveDataLayout().empty()) {
    const LLVMConstantSyntaxOptions PointerOptions = pointerLayoutOptions();
    Context.PointerSizeInBits = [PointerOptions](std::uint32_t AddressSpace) {
      return llvmPointerSizeInBits(AddressSpace, PointerOptions);
    };
  }
  Context.SyncScopes = ActiveDocument->SyncScopes;
  Context.InternSyncScope = [this](std::string_view Name) -> Expected<SyncScopeID> {
    if (Name.empty())
      return makeUnexpected(ErrorCode::ParseError, "synchronization scope name cannot be empty");
    const auto Existing =
        std::ranges::find_if(ActiveDocument->SyncScopes,
                             [Name](const MISyncScopeSlot &Slot) { return Slot.Name == Name; });
    if (Existing != ActiveDocument->SyncScopes.end())
      return Existing->ID;
    if (ActiveDocument->SyncScopes.size() >=
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
      return makeUnexpected(ErrorCode::Unsupported,
                            "MIR document contains too many synchronization scopes");
    }
    const auto ID =
        SyncScopeID::fromValue(static_cast<std::uint32_t>(ActiveDocument->SyncScopes.size() + 1U));
    ActiveDocument->SyncScopes.push_back({.Name = std::string(Name), .ID = ID});
    return ID;
  };
  MIParserOptions BodyOptions = Options.MachineInstructions;
  BodyOptions.VerifyAfterParse = false;
  MIParser Parser(Function, Target,
                  std::string(Result.Source.name()) + ":" + std::string(Function.name()),
                  std::move(BodyText), Context, BodyOptions);
  auto Parsed = Parser.parse();
  if (!Parsed) {
    if (!Parser.diagnostics().empty()) {
      const TextRange Range =
          Body == nullptr ? BodyRange
                          : scalarDiagnosticRange(*Body, Parser.diagnostics().front().Range);
      return fail(Range, Parser.diagnostics().front().Message, "mir-body");
    }
    return importError(BodyRange, Parsed.error(), "mir-body");
  }
  return false;
}

bool MIRDocumentParser::convertTargetNode(const yaml::Node &Source, MIRTargetNode &Destination) {
  switch (Source.kind()) {
  case yaml::NodeKind::Null:
    Destination = MIRTargetNode::null();
    return false;
  case yaml::NodeKind::Scalar:
    Destination = MIRTargetNode::scalar(std::string(Source.scalar()));
    return false;
  case yaml::NodeKind::Sequence: {
    std::vector<MIRTargetNode> Values;
    Values.reserve(Source.sequence().size());
    for (const yaml::NodeID ElementID : Source.sequence()) {
      const yaml::Node *Element = node(ElementID);
      if (Element == nullptr)
        return fail(Source.range(), "target MIR sequence contains an invalid node");
      MIRTargetNode Value;
      if (convertTargetNode(*Element, Value))
        return true;
      Values.push_back(std::move(Value));
    }
    Destination = MIRTargetNode::sequence(std::move(Values));
    return false;
  }
  case yaml::NodeKind::Mapping: {
    std::vector<MIRTargetMappingEntry> Entries;
    Entries.reserve(Source.mapping().size());
    for (const yaml::MappingEntry &SourceEntry : Source.mapping()) {
      const yaml::Node *ValueNode = node(SourceEntry.Value);
      if (ValueNode == nullptr)
        return fail(SourceEntry.KeyRange, "target MIR mapping contains an invalid node");
      MIRTargetNode Value;
      if (convertTargetNode(*ValueNode, Value))
        return true;
      Entries.push_back({SourceEntry.Key, std::move(Value)});
    }
    Destination = MIRTargetNode::mapping(std::move(Entries));
    return false;
  }
  }
  return fail(Source.range(), "target MIR node has an unknown kind");
}

bool MIRDocumentParser::parseTargetState(const yaml::Node &Root, MachineFunction &Function) {
  const yaml::Node *StateNode = mappingValue(Root, "machineFunctionInfo");
  if (StateNode == nullptr)
    return false;
  MIRTargetNode TargetNode;
  if (convertTargetNode(*StateNode, TargetNode))
    return true;
  auto State = Target.mirText().parseFunctionState(TargetNode);
  if (!State)
    return importError(StateNode->range(), State.error(), "mir-target-state");
  auto SetResult = Function.setTargetState(std::move(*State));
  if (!SetResult)
    return importError(StateNode->range(), SetResult.error(), "mir-target-state");
  return false;
}

bool MIRDocumentParser::validateDocumentReferences(const MachineFunction &Function) {
  auto Verification = verifyMIRDocumentReferences(*ActiveDocument, Function, true);
  if (Verification)
    return false;
  return importError(Result.Source.range(0, 0), Verification.error());
}

} // namespace rcc::mir

namespace rcc::mir {

using parser_detail::parseBool;
using parser_detail::parsePrefixedID;
using parser_detail::parseSigned;
using parser_detail::parseUnsigned;
using parser_detail::parseUnsigned32;
using parser_detail::trim;

bool MIRDocumentParser::parseFunctionRecords(const yaml::Node &Root, MachineFunction &Function) {
  auto ParseLocation = [this, &Function](const yaml::Node &Entry,
                                         MachineInstr *&Instruction) -> bool {
    const yaml::Node *BlockNode = mappingValue(Entry, "bb");
    const yaml::Node *OffsetNode = mappingValue(Entry, "offset");
    if (BlockNode == nullptr || OffsetNode == nullptr)
      return fail(Entry.range(), "instruction location requires 'bb' and 'offset'");
    std::uint64_t Block = 0;
    std::uint64_t Offset = 0;
    if (unsignedValue(*BlockNode, Block, "a 32-bit basic block number") ||
        unsignedValue(*OffsetNode, Offset, "a 32-bit instruction offset") ||
        Block > std::numeric_limits<std::uint32_t>::max() ||
        Offset > std::numeric_limits<std::uint32_t>::max()) {
      return true;
    }
    Instruction = instructionAt(Function, static_cast<std::uint32_t>(Block),
                                static_cast<std::uint32_t>(Offset));
    if (Instruction == nullptr)
      return fail(Entry.range(), "instruction location is outside the machine function body");
    return false;
  };

  if (const yaml::Node *CallSites = mappingValue(Root, "callSites")) {
    if (requireKind(*CallSites, yaml::NodeKind::Sequence, "call-site records"))
      return true;
    constexpr std::array CallSiteKeys{
        std::string_view{"bb"},
        std::string_view{"offset"},
        std::string_view{"fwdArgRegs"},
        std::string_view{"calleeTypeIds"},
    };
    constexpr std::array ArgumentKeys{
        std::string_view{"arg"},
        std::string_view{"reg"},
    };
    for (const yaml::NodeID CallSiteID : CallSites->sequence()) {
      const yaml::Node *CallSiteNode = node(CallSiteID);
      if (CallSiteNode == nullptr ||
          requireKind(*CallSiteNode, yaml::NodeKind::Mapping, "call-site record") ||
          validateKeys(*CallSiteNode, CallSiteKeys, "call-site record")) {
        return true;
      }
      MachineInstr *Call = nullptr;
      if (ParseLocation(*CallSiteNode, Call))
        return true;
      if (Call == nullptr)
        return fail(CallSiteNode->range(),
                    "instruction location is outside the machine function body");
      MachineCallSiteInfo Info{
          .CallInstruction = Call->stableID(),
          .ForwardedArguments = {},
          .CalleeTypeIDs = {},
      };
      if (const yaml::Node *Arguments = mappingValue(*CallSiteNode, "fwdArgRegs")) {
        if (requireKind(*Arguments, yaml::NodeKind::Sequence, "forwarded call arguments")) {
          return true;
        }
        for (const yaml::NodeID ArgumentID : Arguments->sequence()) {
          const yaml::Node *Argument = node(ArgumentID);
          if (Argument == nullptr ||
              requireKind(*Argument, yaml::NodeKind::Mapping, "forwarded call argument") ||
              validateKeys(*Argument, ArgumentKeys, "forwarded call argument")) {
            return true;
          }
          const yaml::Node *NumberNode = mappingValue(*Argument, "arg");
          const yaml::Node *RegNode = mappingValue(*Argument, "reg");
          if (NumberNode == nullptr || RegNode == nullptr)
            return fail(Argument->range(), "forwarded call argument requires 'arg' and 'reg'");
          std::uint16_t Number = 0;
          std::uint64_t WideNumber = 0;
          PhysicalRegister Reg;
          if (unsignedValue(*NumberNode, WideNumber, "a 16-bit argument number") ||
              WideNumber > std::numeric_limits<std::uint16_t>::max() ||
              physicalRegister(*RegNode, Reg)) {
            return true;
          }
          Number = static_cast<std::uint16_t>(WideNumber);
          Info.ForwardedArguments.push_back({Reg, Number});
        }
      }
      if (const yaml::Node *TypeIDs = mappingValue(*CallSiteNode, "calleeTypeIds")) {
        if (requireKind(*TypeIDs, yaml::NodeKind::Sequence, "callee type IDs"))
          return true;
        for (const yaml::NodeID TypeID : TypeIDs->sequence()) {
          const yaml::Node *TypeNode = node(TypeID);
          std::uint64_t Value = 0;
          if (TypeNode == nullptr ||
              unsignedValue(*TypeNode, Value, "an unsigned callee type ID")) {
            return true;
          }
          Info.CalleeTypeIDs.push_back(Value);
        }
      }
      auto AddResult = Function.records().addCallSite(std::move(Info));
      if (!AddResult)
        return importError(CallSiteNode->range(), AddResult.error());
    }
  }

  if (const yaml::Node *CalledGlobals = mappingValue(Root, "calledGlobals")) {
    if (requireKind(*CalledGlobals, yaml::NodeKind::Sequence, "called-global records"))
      return true;
    constexpr std::array Keys{
        std::string_view{"bb"},
        std::string_view{"offset"},
        std::string_view{"callee"},
        std::string_view{"flags"},
    };
    for (const yaml::NodeID CalledID : CalledGlobals->sequence()) {
      const yaml::Node *CalledNode = node(CalledID);
      if (CalledNode == nullptr ||
          requireKind(*CalledNode, yaml::NodeKind::Mapping, "called-global record") ||
          validateKeys(*CalledNode, Keys, "called-global record")) {
        return true;
      }
      MachineInstr *Call = nullptr;
      if (ParseLocation(*CalledNode, Call))
        return true;
      if (Call == nullptr)
        return fail(CalledNode->range(),
                    "instruction location is outside the machine function body");
      const yaml::Node *CalleeNode = mappingValue(*CalledNode, "callee");
      const yaml::Node *FlagsNode = mappingValue(*CalledNode, "flags");
      if (CalleeNode == nullptr || FlagsNode == nullptr)
        return fail(CalledNode->range(), "called-global record requires 'callee' and 'flags'");
      std::string_view Callee;
      std::uint64_t WideFlags = 0;
      if (scalar(*CalleeNode, Callee, "callee name") ||
          unsignedValue(*FlagsNode, WideFlags, "32-bit called-global flags") ||
          WideFlags > std::numeric_limits<std::uint32_t>::max()) {
        return true;
      }
      if (ActiveDocument->EmbeddedIR && !ActiveDocument->EmbeddedIR->hasGlobalValue(Callee)) {
        return fail(CalleeNode->range(), "called global references an undefined embedded global");
      }
      auto AddResult =
          Function.records().addCalledGlobal({.CallInstruction = Call->stableID(),
                                              .Callee = std::string(Callee),
                                              .Flags = static_cast<std::uint32_t>(WideFlags)});
      if (!AddResult)
        return importError(CalledNode->range(), AddResult.error());
    }
  }

  if (const yaml::Node *Substitutions = mappingValue(Root, "debugValueSubstitutions")) {
    if (requireKind(*Substitutions, yaml::NodeKind::Sequence, "debug value substitutions")) {
      return true;
    }
    constexpr std::array Keys{
        std::string_view{"srcinst"}, std::string_view{"srcop"},  std::string_view{"dstinst"},
        std::string_view{"dstop"},   std::string_view{"subreg"},
    };
    for (const yaml::NodeID SubstitutionID : Substitutions->sequence()) {
      const yaml::Node *Substitution = node(SubstitutionID);
      if (Substitution == nullptr ||
          requireKind(*Substitution, yaml::NodeKind::Mapping, "debug value substitution") ||
          validateKeys(*Substitution, Keys, "debug value substitution")) {
        return true;
      }
      std::array<std::uint32_t, 5> Values{};
      for (std::size_t Index = 0; Index < Keys.size(); ++Index) {
        const yaml::Node *ValueNode = mappingValue(*Substitution, Keys[Index]);
        if (ValueNode == nullptr)
          return fail(Substitution->range(), "debug value substitution requires every fixed field");
        std::uint64_t Wide = 0;
        if (unsignedValue(*ValueNode, Wide, "a 32-bit debug substitution value") ||
            Wide > std::numeric_limits<std::uint32_t>::max()) {
          return true;
        }
        Values[Index] = static_cast<std::uint32_t>(Wide);
      }
      auto AddResult = Function.records().addDebugValueSubstitution(
          {.Source = {Values[0], Values[1]},
           .Destination = {Values[2], Values[3]},
           .Subregister = SubRegIndex::fromValue(Values[4])});
      if (!AddResult)
        return importError(Substitution->range(), AddResult.error());
    }
  }

  if (const yaml::Node *PrefetchTargets = mappingValue(Root, "prefetch-targets")) {
    if (requireKind(*PrefetchTargets, yaml::NodeKind::Sequence, "prefetch targets"))
      return true;
    for (const yaml::NodeID TargetID : PrefetchTargets->sequence()) {
      const yaml::Node *TargetNode = node(TargetID);
      std::string_view Text;
      if (TargetNode == nullptr || scalar(*TargetNode, Text, "prefetch target"))
        return true;

      std::size_t ContentOffset = TargetNode->range().Begin.Offset;
      if (TargetNode->scalarStyle() == yaml::ScalarStyle::SingleQuoted ||
          TargetNode->scalarStyle() == yaml::ScalarStyle::DoubleQuoted) {
        ++ContentOffset;
      }
      const auto ErrorRange = [this, ContentOffset](std::size_t Begin, std::size_t End = 1U) {
        return Result.Source.range(ContentOffset + Begin, ContentOffset + Begin + End);
      };
      const auto SkipSpace = [&Text](std::size_t &Cursor) {
        while (Cursor < Text.size() && (Text[Cursor] == ' ' || Text[Cursor] == '\t'))
          ++Cursor;
      };
      const auto ParseNumber = [&Text, &SkipSpace](std::size_t &Cursor, std::uint32_t &Value) {
        SkipSpace(Cursor);
        const std::size_t Begin = Cursor;
        while (Cursor < Text.size() && Text[Cursor] >= '0' && Text[Cursor] <= '9')
          ++Cursor;
        return Begin != Cursor && parseUnsigned32(Text.substr(Begin, Cursor - Begin), Value);
      };

      std::size_t Cursor = 0;
      SkipSpace(Cursor);
      if (!Text.substr(Cursor).starts_with("bb_id") ||
          (Cursor + std::string_view("bb_id").size() < Text.size() &&
           Text[Cursor + std::string_view("bb_id").size()] != ' ' &&
           Text[Cursor + std::string_view("bb_id").size()] != '\t')) {
        return fail(ErrorRange(Cursor), "expected 'bb_id'");
      }
      Cursor += std::string_view("bb_id").size();

      std::array<std::uint32_t, 3> Values{};
      if (!ParseNumber(Cursor, Values[0]))
        return fail(ErrorRange(0), "expected unsigned integer");

      SkipSpace(Cursor);
      if (Cursor < Text.size() && (Text[Cursor] == ',' || Text[Cursor] == '.')) {
        ++Cursor;
        if (!ParseNumber(Cursor, Values[1]))
          return fail(ErrorRange(0), "expected unsigned integer");
      } else if (Cursor < Text.size() && Text[Cursor] >= '0' && Text[Cursor] <= '9') {
        if (!ParseNumber(Cursor, Values[1]))
          return fail(ErrorRange(0), "expected unsigned integer");
      }

      SkipSpace(Cursor);
      if (Cursor >= Text.size() || Text[Cursor] != ',')
        return fail(ErrorRange(Cursor), "expected ','");
      ++Cursor;
      if (!ParseNumber(Cursor, Values[2]))
        return fail(ErrorRange(0), "expected unsigned integer");
      SkipSpace(Cursor);
      if (Cursor != Text.size())
        return fail(ErrorRange(0), "unexpected content after prefetch target");

      auto AddResult = Function.records().addPrefetchTarget(
          {.Block = {Values[0], Values[1]}, .CallsiteIndex = Values[2]});
      if (!AddResult)
        return importError(TargetNode->range(), AddResult.error());
    }
  }

  if (const yaml::Node *EntryValues = mappingValue(Root, "entry_values")) {
    if (requireKind(*EntryValues, yaml::NodeKind::Sequence, "entry values"))
      return true;
    constexpr std::array Keys{
        std::string_view{"entry-value-register"},
        std::string_view{"debug-info-variable"},
        std::string_view{"debug-info-expression"},
        std::string_view{"debug-info-location"},
    };
    for (const yaml::NodeID EntryID : EntryValues->sequence()) {
      const yaml::Node *Entry = node(EntryID);
      if (Entry == nullptr || requireKind(*Entry, yaml::NodeKind::Mapping, "entry value") ||
          validateKeys(*Entry, Keys, "entry value")) {
        return true;
      }
      const yaml::Node *RegNode = mappingValue(*Entry, "entry-value-register");
      const yaml::Node *VariableNode = mappingValue(*Entry, "debug-info-variable");
      const yaml::Node *ExpressionNode = mappingValue(*Entry, "debug-info-expression");
      const yaml::Node *LocationNode = mappingValue(*Entry, "debug-info-location");
      if (RegNode == nullptr || VariableNode == nullptr || ExpressionNode == nullptr ||
          LocationNode == nullptr) {
        return fail(Entry->range(), "entry value requires all four fixed fields");
      }
      PhysicalRegister Reg;
      if (physicalRegister(*RegNode, Reg))
        return true;
      MachineVariableDebugInfo DebugInfo;
      if (parseVariableDebugInfo(*Entry, Function, DebugInfo))
        return true;
      if (!DebugInfo.Variable || !DebugInfo.Expression || !DebugInfo.Location) {
        return fail(Entry->range(), "entry value debug metadata cannot be empty",
                    "mir-debug-metadata");
      }
      auto AddResult =
          Function.records().addEntryValue({.Reg = Reg, .DebugInfo = std::move(DebugInfo)});
      if (!AddResult)
        return importError(Entry->range(), AddResult.error());
    }
  }
  return false;
}

} // namespace rcc::mir

namespace rcc::mir {

using parser_detail::parseBool;
using parser_detail::parsePrefixedID;
using parser_detail::parseSigned;
using parser_detail::parseUnsigned;
using parser_detail::parseUnsigned32;
using parser_detail::trim;

Expected<MachineMetadataReference>
MIRDocumentParser::resolveMetadataReference(const MachineFunction &Function,
                                            std::uint32_t Slot) const {
  if (ActiveDocument != nullptr && ActiveDocument->EmbeddedIR &&
      ActiveDocument->EmbeddedIR->metadata(std::to_string(Slot)) != nullptr) {
    return MachineMetadataReference::embeddedIR(Slot);
  }
  if (Function.metadata().node(Slot) != nullptr)
    return MachineMetadataReference::machineFunction(Slot);
  return makeUnexpected(ErrorCode::ParseError,
                        "metadata reference uses undefined slot '!" + std::to_string(Slot) + "'");
}

bool MIRDocumentParser::parseVariableDebugInfo(const yaml::Node &Entry, MachineFunction &Function,
                                               MachineVariableDebugInfo &DebugInfo) {
  const auto Resolve = [this, &Function](std::uint32_t Slot) {
    return resolveMetadataReference(Function, Slot);
  };
  if (const yaml::Node *VariableNode = mappingValue(Entry, "debug-info-variable")) {
    std::string_view Text;
    if (scalar(*VariableNode, Text, "debug variable"))
      return true;
    if (!Text.empty()) {
      MetadataTextParser Parser(std::string(Text), Resolve);
      auto Reference = Parser.parseReferenceOnly();
      if (!Reference)
        return importError(VariableNode->range(), Reference.error(), "mir-debug-metadata");
      if (Reference->scope() != MachineMetadataScope::EmbeddedIR) {
        return fail(VariableNode->range(),
                    "debug variable must reference embedded LLVM IR metadata",
                    "mir-debug-metadata");
      }
      DebugInfo.Variable = *Reference;
    }
  }
  if (const yaml::Node *ExpressionNode = mappingValue(Entry, "debug-info-expression")) {
    std::string_view Text;
    if (scalar(*ExpressionNode, Text, "debug expression"))
      return true;
    if (!Text.empty()) {
      MetadataTextParser Parser(std::string(Text), Resolve);
      auto Value = Parser.parseValue();
      if (!Value)
        return importError(scalarDiagnosticRange(*ExpressionNode, Parser.diagnosticRange()),
                           Value.error(), "mir-debug-metadata");
      if (Value->reference() != nullptr &&
          Value->reference()->scope() != MachineMetadataScope::EmbeddedIR) {
        return fail(ExpressionNode->range(),
                    "debug expression reference must use embedded LLVM IR metadata",
                    "mir-debug-metadata");
      }
      DebugInfo.Expression = std::move(*Value);
    }
  }
  if (const yaml::Node *LocationNode = mappingValue(Entry, "debug-info-location")) {
    std::string_view Text;
    if (scalar(*LocationNode, Text, "debug location"))
      return true;
    if (!Text.empty()) {
      MetadataTextParser Parser(std::string(Text), Resolve);
      auto Location = Parser.parseLocation();
      if (!Location)
        return importError(LocationNode->range(), Location.error(), "mir-debug-metadata");
      if (Location->reference() != nullptr &&
          Location->reference()->scope() != MachineMetadataScope::EmbeddedIR) {
        return fail(LocationNode->range(),
                    "variable debug location must reference embedded LLVM IR metadata",
                    "mir-debug-metadata");
      }
      DebugInfo.Location = std::move(*Location);
    }
  }
  if (!DebugInfo.Variable && !DebugInfo.Expression && !DebugInfo.Location)
    return false;
  auto Verification = DebugInfo.verifyStructure();
  if (!Verification)
    return importError(Entry.range(), Verification.error(), "mir-debug-metadata");
  return false;
}

bool MIRDocumentParser::parseMetadata(const yaml::Node &Root, MachineFunction &Function,
                                      MIParserContext &Context) {
  if (ActiveDocument->EmbeddedIR) {
    for (const EmbeddedIRMetadataDefinition &Reference :
         ActiveDocument->EmbeddedIR->metadataDefinitions()) {
      std::uint32_t ID = 0;
      if (!parseUnsigned32(Reference.Name, ID))
        continue;
      if (std::ranges::any_of(Context.Metadata,
                              [ID](const MIMetadataSlot &Slot) { return Slot.TextID == ID; })) {
        return fail(Root.range(), "embedded LLVM IR metadata slots are not unique");
      }
      Context.Metadata.push_back(
          {.TextID = ID, .Reference = MachineMetadataReference::embeddedIR(ID)});
    }
  }

  const yaml::Node *Metadata = mappingValue(Root, "machineMetadataNodes");
  if (Metadata == nullptr)
    return false;
  if (requireKind(*Metadata, yaml::NodeKind::Sequence, "machine metadata nodes"))
    return true;

  enum class ParsedElementKind : std::uint8_t {
    Null,
    Reference,
    String,
  };
  struct ParsedElement final {
    ParsedElementKind Kind = ParsedElementKind::Reference;
    std::uint32_t Reference = 0;
    std::string String;
    TextRange Range;
  };
  struct Definition final {
    const yaml::Node *Node = nullptr;
    std::uint32_t Slot = 0;
    bool IsDistinct = false;
    std::vector<ParsedElement> Elements;
  };
  std::vector<Definition> Definitions;
  std::unordered_set<std::uint32_t> Slots;

  const auto TranslateRange = [this](const yaml::Node &Node, const TextRange &Range) {
    std::size_t ContentBegin = Node.range().Begin.Offset;
    if (Node.scalarStyle() == yaml::ScalarStyle::SingleQuoted ||
        Node.scalarStyle() == yaml::ScalarStyle::DoubleQuoted) {
      ++ContentBegin;
    }
    return Result.Source.range(ContentBegin + Range.Begin.Offset, ContentBegin + Range.End.Offset);
  };

  for (const yaml::NodeID DefinitionID : Metadata->sequence()) {
    const yaml::Node *DefinitionNode = node(DefinitionID);
    std::string_view Text;
    if (DefinitionNode == nullptr || scalar(*DefinitionNode, Text, "machine metadata definition"))
      return true;

    MILexer Lexer("machine-metadata", std::string(Text));
    MIToken Token = Lexer.next();
    const auto LexerFailed = [this, &Lexer, DefinitionNode, &TranslateRange, &Token]() -> bool {
      if (!Token.is(MITokenKind::Error))
        return false;
      const auto Diagnostics = Lexer.diagnostics();
      const TextRange Range = Diagnostics.empty()
                                  ? TranslateRange(*DefinitionNode, Token.range())
                                  : TranslateRange(*DefinitionNode, Diagnostics.front().Range);
      return fail(Range, Diagnostics.empty() ? "invalid machine metadata node"
                                             : Diagnostics.front().Message);
    };
    const auto Advance = [&Lexer, &Token, &LexerFailed]() -> bool {
      Token = Lexer.next();
      return LexerFailed();
    };
    const auto FailAtToken = [this, DefinitionNode, &TranslateRange](const MIToken &At,
                                                                     std::string Message) {
      return fail(TranslateRange(*DefinitionNode, At.range()), std::move(Message));
    };

    if (LexerFailed())
      return true;
    if (!Token.is(MITokenKind::Exclaim))
      return FailAtToken(Token, "expected a metadata node");
    if (Advance())
      return true;
    std::uint32_t Slot = 0;
    if (!Token.is(MITokenKind::IntegerLiteral) || !parseUnsigned32(Token.integerValue(), Slot)) {
      return FailAtToken(Token, "expected metadata id after '!'");
    }
    if (Advance())
      return true;
    if (!Token.is(MITokenKind::Equal))
      return FailAtToken(Token, "expected '='");
    if (Advance())
      return true;

    const bool IsDistinct = Token.isKeyword("distinct");
    if (IsDistinct && Advance())
      return true;
    if (!Token.is(MITokenKind::Exclaim))
      return FailAtToken(Token, "expected a metadata node");
    if (Advance())
      return true;
    if (!Token.is(MITokenKind::LeftBrace))
      return FailAtToken(Token, "expected '{' here");
    if (Advance())
      return true;

    std::vector<ParsedElement> Elements;
    if (!Token.is(MITokenKind::RightBrace)) {
      while (true) {
        if (Token.hasStringValue() && Token.stringValue() == "null") {
          Elements.push_back({.Kind = ParsedElementKind::Null,
                              .Reference = 0,
                              .String = {},
                              .Range = TranslateRange(*DefinitionNode, Token.range())});
        } else {
          if (!Token.is(MITokenKind::Exclaim))
            return FailAtToken(Token, "expected '!' here");
          const TextRange ReferenceRange = TranslateRange(*DefinitionNode, Token.range());
          if (Advance())
            return true;

          if (Token.is(MITokenKind::StringConstant)) {
            Elements.push_back({.Kind = ParsedElementKind::String,
                                .Reference = 0,
                                .String = std::string(Token.stringValue()),
                                .Range = TranslateRange(*DefinitionNode, Token.range())});
          } else {
            std::uint32_t Reference = 0;
            if (!Token.is(MITokenKind::IntegerLiteral) ||
                !parseUnsigned32(Token.integerValue(), Reference)) {
              return FailAtToken(Token, "expected metadata id after '!'");
            }
            Elements.push_back({.Kind = ParsedElementKind::Reference,
                                .Reference = Reference,
                                .String = {},
                                .Range = ReferenceRange});
          }
        }
        if (Advance())
          return true;
        if (!Token.is(MITokenKind::Comma))
          break;
        if (Advance())
          return true;
      }
    }
    if (!Token.is(MITokenKind::RightBrace))
      return FailAtToken(Token, "expected end of metadata node");
    if (Advance())
      return true;
    if (!Token.is(MITokenKind::EndOfFile))
      return FailAtToken(Token, "expected end of metadata node");
    if (!Slots.insert(Slot).second)
      return FailAtToken(Token, "Metadata id is already used");

    Definitions.push_back({.Node = DefinitionNode,
                           .Slot = Slot,
                           .IsDistinct = IsDistinct,
                           .Elements = std::move(Elements)});
    Context.Metadata.push_back(
        {.TextID = Slot, .Reference = MachineMetadataReference::machineFunction(Slot)});
  }

  for (const Definition &Definition : Definitions) {
    MachineMetadataNode NewNode{
        .Slot = Definition.Slot,
        .IsDistinct = Definition.IsDistinct,
        .Elements = {},
    };
    for (const ParsedElement &Element : Definition.Elements) {
      if (Element.Kind == ParsedElementKind::Null) {
        NewNode.Elements.push_back(MachineMetadataElement::null());
        continue;
      }
      if (Element.Kind == ParsedElementKind::String) {
        NewNode.Elements.push_back(MachineMetadataElement::string(Element.String));
        continue;
      }

      MachineMetadataReference Reference;
      if (ActiveDocument->EmbeddedIR != std::nullopt &&
          ActiveDocument->EmbeddedIR->metadata(std::to_string(Element.Reference)) != nullptr) {
        Reference = MachineMetadataReference::embeddedIR(Element.Reference);
      } else if (Slots.contains(Element.Reference)) {
        Reference = MachineMetadataReference::machineFunction(Element.Reference);
      } else {
        return fail(Element.Range,
                    "use of undefined metadata '!" + std::to_string(Element.Reference) + "'");
      }
      auto Resolved = MachineMetadataElement::reference(Reference);
      if (!Resolved)
        return importError(Element.Range, Resolved.error());
      NewNode.Elements.push_back(std::move(*Resolved));
    }
    auto AddResult = Function.metadata().addNode(std::move(NewNode));
    if (!AddResult)
      return importError(Definition.Node->range(), AddResult.error());
  }
  return false;
}

} // namespace rcc::mir
