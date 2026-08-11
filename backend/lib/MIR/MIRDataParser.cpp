// SPDX-License-Identifier: GPL-3.0-or-later

#include "MIRDocumentParser.h"
#include "MIRDocumentReferenceValidation.h"
#include "MIRMetadataTextParser.h"
#include "MIRParserUtilities.h"
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
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rcc::mir {

using parser_detail::parseBool;
using parser_detail::parsePrefixedID;
using parser_detail::parseSigned;
using parser_detail::parseUnsigned;
using parser_detail::parseUnsigned32;
using parser_detail::trim;

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
