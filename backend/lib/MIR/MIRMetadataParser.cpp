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
#include "rcc/MIR/MILexer.h"

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
    const auto LexerFailed = [this, &Lexer, DefinitionNode, &TranslateRange,
                              &Token]() -> bool {
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
    if (!Token.is(MITokenKind::IntegerLiteral) ||
        !parseUnsigned32(Token.integerValue(), Slot)) {
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
