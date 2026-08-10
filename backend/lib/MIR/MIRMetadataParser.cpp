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
        return importError(ExpressionNode->range(), Value.error(), "mir-debug-metadata");
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

  struct Definition final {
    const yaml::Node *Node = nullptr;
    std::uint32_t Slot = 0;
    bool IsDistinct = false;
    std::string_view Elements;
  };
  std::vector<Definition> Definitions;
  std::unordered_set<std::uint32_t> Slots;

  for (const yaml::NodeID DefinitionID : Metadata->sequence()) {
    const yaml::Node *DefinitionNode = node(DefinitionID);
    std::string_view Text;
    if (DefinitionNode == nullptr || scalar(*DefinitionNode, Text, "machine metadata definition")) {
      return true;
    }
    Text = trim(Text);
    if (!Text.starts_with('!'))
      return fail(DefinitionNode->range(), "machine metadata definition must start with '!N'");
    const std::size_t Equals = Text.find('=');
    if (Equals == std::string_view::npos)
      return fail(DefinitionNode->range(), "machine metadata definition requires '='");
    std::uint32_t Slot = 0;
    if (!parseUnsigned32(trim(Text.substr(1, Equals - 1)), Slot))
      return fail(DefinitionNode->range(), "machine metadata definition has an invalid slot");
    if (!Slots.insert(Slot).second)
      return fail(DefinitionNode->range(), "duplicate machine metadata slot");
    std::string_view Right = trim(Text.substr(Equals + 1));
    bool IsDistinct = false;
    if (Right.starts_with("distinct")) {
      Right = trim(Right.substr(std::string_view("distinct").size()));
      IsDistinct = true;
    }
    if (!Right.starts_with("!{") || !Right.ends_with('}'))
      return fail(DefinitionNode->range(), "machine metadata node must be a metadata tuple");
    Definitions.push_back({.Node = DefinitionNode,
                           .Slot = Slot,
                           .IsDistinct = IsDistinct,
                           .Elements = Right.substr(2, Right.size() - 3)});
    Context.Metadata.push_back(
        {.TextID = Slot, .Reference = MachineMetadataReference::machineFunction(Slot)});
  }

  auto ResolveReference = [this,
                           &Slots](std::uint32_t Slot,
                                   const TextRange &Range) -> Expected<MachineMetadataElement> {
    MachineMetadataReference Reference;
    if (ActiveDocument->EmbeddedIR != std::nullopt &&
        ActiveDocument->EmbeddedIR->metadata(std::to_string(Slot)) != nullptr) {
      Reference = MachineMetadataReference::embeddedIR(Slot);
    } else if (Slots.contains(Slot)) {
      Reference = MachineMetadataReference::machineFunction(Slot);
    } else {
      return makeUnexpected(ErrorCode::ParseError, "metadata tuple references undefined slot '!" +
                                                       std::to_string(Slot) + "' at line " +
                                                       std::to_string(Range.Begin.Line));
    }
    return MachineMetadataElement::reference(Reference);
  };

  for (const Definition &Definition : Definitions) {
    MachineMetadataNode NewNode{
        .Slot = Definition.Slot,
        .IsDistinct = Definition.IsDistinct,
        .Elements = {},
    };
    std::string_view Remaining = trim(Definition.Elements);
    while (!Remaining.empty()) {
      std::size_t End = 0;
      bool InString = false;
      bool Escaped = false;
      for (; End < Remaining.size(); ++End) {
        const char Character = Remaining[End];
        if (InString && Escaped) {
          Escaped = false;
          continue;
        }
        if (InString && Character == '\\') {
          Escaped = true;
          continue;
        }
        if (Character == '"') {
          InString = !InString;
          continue;
        }
        if (!InString && Character == ',')
          break;
      }
      if (InString)
        return fail(Definition.Node->range(), "unterminated machine metadata string");
      std::string_view Element = trim(Remaining.substr(0, End));
      if (Element.empty())
        return fail(Definition.Node->range(), "empty machine metadata tuple element");

      if (Element == "null") {
        NewNode.Elements.push_back(MachineMetadataElement::null());
      } else if (Element.starts_with('!') && Element.size() > 1 && Element[1] >= '0' &&
                 Element[1] <= '9') {
        std::uint32_t ReferenceSlot = 0;
        if (!parseUnsigned32(Element.substr(1), ReferenceSlot))
          return fail(Definition.Node->range(), "invalid metadata tuple reference");
        auto Reference = ResolveReference(ReferenceSlot, Definition.Node->range());
        if (!Reference)
          return importError(Definition.Node->range(), Reference.error());
        NewNode.Elements.push_back(std::move(*Reference));
      } else if (Element.starts_with("!\"") && Element.ends_with('"')) {
        std::string Value;
        std::string_view Encoded = Element.substr(2, Element.size() - 3);
        for (std::size_t Index = 0; Index < Encoded.size(); ++Index) {
          if (Encoded[Index] == '\\' && Index + 1 < Encoded.size()) {
            const char EscapedCharacter = Encoded[++Index];
            if (EscapedCharacter != '\\' && EscapedCharacter != '"')
              return fail(Definition.Node->range(), "unsupported machine metadata string escape");
            Value.push_back(EscapedCharacter);
          } else {
            Value.push_back(Encoded[Index]);
          }
        }
        NewNode.Elements.push_back(MachineMetadataElement::string(std::move(Value)));
      } else {
        return fail(Definition.Node->range(), "unsupported machine metadata tuple element");
      }

      if (End == Remaining.size())
        Remaining = {};
      else
        Remaining = trim(Remaining.substr(End + 1));
    }
    auto AddResult = Function.metadata().addNode(std::move(NewNode));
    if (!AddResult)
      return importError(Definition.Node->range(), AddResult.error());
  }
  return false;
}

} // namespace rcc::mir
