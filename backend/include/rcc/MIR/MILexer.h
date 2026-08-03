// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_MIR_MILEXER_H
#define RCC_MIR_MILEXER_H

#include "rcc/Support/TextDiagnostic.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace rcc::mir {

enum class MITokenKind : std::uint8_t {
  EndOfFile,
  Error,
  Newline,

  Comma,
  Equal,
  Underscore,
  Colon,
  DoubleColon,
  Dot,
  Exclaim,
  LeftParen,
  RightParen,
  LeftBrace,
  RightBrace,
  Plus,
  Minus,
  Less,
  Greater,

  Keyword,
  MetadataKeyword,
  Identifier,
  NamedRegister,
  NamedVirtualRegister,
  MachineBasicBlockLabel,
  MachineBasicBlock,
  StackObject,
  FixedStackObject,
  NamedGlobalValue,
  GlobalValue,
  ExternalSymbol,
  MCSymbol,
  IntegerLiteral,
  FloatingPointLiteral,
  HexLiteral,
  VirtualRegister,
  ConstantPoolItem,
  JumpTableIndex,
  NamedIRBlock,
  IRBlock,
  NamedIRValue,
  IRValue,
  QuotedIRValue,
  SubRegisterIndex,
  StringConstant,
};

[[nodiscard]] std::string_view getMITokenKindName(MITokenKind Kind) noexcept;
[[nodiscard]] bool isMIKeyword(std::string_view Spelling) noexcept;
[[nodiscard]] bool isMIMetadataKeyword(std::string_view Spelling) noexcept;

class MIToken final {
public:
  [[nodiscard]] MITokenKind kind() const noexcept {
    return Kind;
  }

  [[nodiscard]] const TextRange &range() const noexcept {
    return Range;
  }

  [[nodiscard]] std::string_view spelling() const noexcept {
    return Spelling;
  }

  [[nodiscard]] bool hasStringValue() const noexcept {
    return !std::holds_alternative<std::monostate>(StringValue);
  }

  [[nodiscard]] std::string_view stringValue() const noexcept;

  [[nodiscard]] bool hasIntegerValue() const noexcept {
    return !IntegerValue.empty();
  }

  [[nodiscard]] std::string_view integerValue() const noexcept {
    return IntegerValue;
  }

  [[nodiscard]] bool is(MITokenKind Expected) const noexcept {
    return Kind == Expected;
  }

  [[nodiscard]] bool isKeyword(std::string_view Expected) const noexcept {
    return Kind == MITokenKind::Keyword && stringValue() == Expected;
  }

  [[nodiscard]] bool isMetadataKeyword(std::string_view Expected) const noexcept {
    return Kind == MITokenKind::MetadataKeyword && stringValue() == Expected;
  }

  [[nodiscard]] bool isNewlineOrEnd() const noexcept {
    return Kind == MITokenKind::Newline || Kind == MITokenKind::EndOfFile;
  }

  [[nodiscard]] bool isErrorOrEnd() const noexcept {
    return Kind == MITokenKind::Error || Kind == MITokenKind::EndOfFile;
  }

  [[nodiscard]] bool isRegister() const noexcept;
  [[nodiscard]] bool isRegisterFlag() const noexcept;
  [[nodiscard]] bool isMemoryOperandFlag() const noexcept;

private:
  using StringValueStorage = std::variant<std::monostate, std::string_view, std::string>;

  MIToken(MITokenKind Kind, TextRange Range, std::string_view Spelling,
          StringValueStorage StringValue = {}, std::string_view IntegerValue = {})
      : Kind(Kind), Range(Range), Spelling(Spelling), StringValue(std::move(StringValue)),
        IntegerValue(IntegerValue) {}

  MITokenKind Kind = MITokenKind::Error;
  TextRange Range;
  std::string_view Spelling;
  StringValueStorage StringValue;
  std::string_view IntegerValue;

  friend class MILexer;
};

struct MILexerOptions final {
  std::size_t MaximumTokenLength = std::size_t{1} << 24U;
  std::size_t MaximumTokens = std::size_t{1} << 24U;
};

class MILexer final {
public:
  MILexer(std::string SourceName, std::string SourceText, MILexerOptions Options = {});
  MILexer(const MILexer &) = delete;
  MILexer &operator=(const MILexer &) = delete;
  MILexer(MILexer &&) noexcept;
  MILexer &operator=(MILexer &&) noexcept;
  ~MILexer();

  [[nodiscard]] MIToken next();

  [[nodiscard]] const TextSource &source() const noexcept;
  [[nodiscard]] std::span<const TextDiagnostic> diagnostics() const noexcept;
  [[nodiscard]] bool failed() const noexcept;
  [[nodiscard]] std::size_t tokenCount() const noexcept;

private:
  [[nodiscard]] static MIToken makeToken(MITokenKind Kind, TextRange Range,
                                         std::string_view Spelling,
                                         std::string_view IntegerValue = {});
  [[nodiscard]] static MIToken makeStringToken(MITokenKind Kind, TextRange Range,
                                               std::string_view Spelling,
                                               std::string_view StringValue,
                                               std::string_view IntegerValue = {});
  [[nodiscard]] static MIToken makeOwnedStringToken(MITokenKind Kind, TextRange Range,
                                                    std::string_view Spelling,
                                                    std::string StringValue,
                                                    std::string_view IntegerValue = {});

  class Impl;
  std::unique_ptr<Impl> Implementation;
};

} // namespace rcc::mir

#endif // RCC_MIR_MILEXER_H
