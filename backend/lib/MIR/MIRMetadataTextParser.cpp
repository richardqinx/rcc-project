// SPDX-License-Identifier: GPL-3.0-or-later

#include "MIRMetadataTextParser.h"

#include "MIRParserUtilities.h"
#include "rcc/MIR/MIDebugSyntax.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

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
