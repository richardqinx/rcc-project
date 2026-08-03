// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/MIR/MILexer.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rcc::mir {
namespace {

template <std::size_t Size>
[[nodiscard]] consteval std::array<std::string_view, Size>
sortSpellings(std::array<std::string_view, Size> Spellings) {
  for (std::size_t Left = 0; Left < Spellings.size(); ++Left) {
    for (std::size_t Right = Left + 1; Right < Spellings.size(); ++Right) {
      if (Spellings[Right] < Spellings[Left])
        std::swap(Spellings[Left], Spellings[Right]);
    }
  }
  return Spellings;
}

template <std::size_t Size>
[[nodiscard]] consteval bool
hasUniqueSpellings(const std::array<std::string_view, Size> &Spellings) {
  for (std::size_t Index = 1; Index < Spellings.size(); ++Index) {
    if (Spellings[Index - 1] == Spellings[Index])
      return false;
  }
  return true;
}

constexpr auto Keywords = sortSpellings(std::array{
    std::string_view{"addrspace"},
    std::string_view{"adjust_cfa_offset"},
    std::string_view{"afn"},
    std::string_view{"align"},
    std::string_view{"arcp"},
    std::string_view{"basealign"},
    std::string_view{"bb_id"},
    std::string_view{"bbsections"},
    std::string_view{"bfloat"},
    std::string_view{"blockaddress"},
    std::string_view{"call-entry"},
    std::string_view{"call-frame-size"},
    std::string_view{"cfi-type"},
    std::string_view{"constant-pool"},
    std::string_view{"contract"},
    std::string_view{"custom"},
    std::string_view{"dead"},
    std::string_view{"deactivation-symbol"},
    std::string_view{"debug-instr-number"},
    std::string_view{"debug-location"},
    std::string_view{"debug-use"},
    std::string_view{"dbg-instr-ref"},
    std::string_view{"def"},
    std::string_view{"def_cfa"},
    std::string_view{"def_cfa_offset"},
    std::string_view{"def_cfa_register"},
    std::string_view{"dereferenceable"},
    std::string_view{"disjoint"},
    std::string_view{"distinct"},
    std::string_view{"double"},
    std::string_view{"early-clobber"},
    std::string_view{"ehfunclet-entry"},
    std::string_view{"ehscope-entry"},
    std::string_view{"escape"},
    std::string_view{"exact"},
    std::string_view{"float"},
    std::string_view{"floatpred"},
    std::string_view{"fp128"},
    std::string_view{"frame-destroy"},
    std::string_view{"frame-setup"},
    std::string_view{"got"},
    std::string_view{"half"},
    std::string_view{"heap-alloc-marker"},
    std::string_view{"implicit"},
    std::string_view{"implicit-def"},
    std::string_view{"inbounds"},
    std::string_view{"inlineasm-br-indirect-target"},
    std::string_view{"internal"},
    std::string_view{"intpred"},
    std::string_view{"intrinsic"},
    std::string_view{"invariant"},
    std::string_view{"ir-block-address-taken"},
    std::string_view{"jump-table"},
    std::string_view{"killed"},
    std::string_view{"landing-pad"},
    std::string_view{"lanemask"},
    std::string_view{"liveins"},
    std::string_view{"liveout"},
    std::string_view{"llvm_def_aspace_cfa"},
    std::string_view{"llvm_register_pair"},
    std::string_view{"llvm_vector_offset"},
    std::string_view{"llvm_vector_register_mask"},
    std::string_view{"llvm_vector_registers"},
    std::string_view{"machine-block-address-taken"},
    std::string_view{"mmra"},
    std::string_view{"negate_ra_sign_state"},
    std::string_view{"negate_ra_sign_state_with_pc"},
    std::string_view{"ninf"},
    std::string_view{"nnan"},
    std::string_view{"noconvergent"},
    std::string_view{"nofpexcept"},
    std::string_view{"non-temporal"},
    std::string_view{"nneg"},
    std::string_view{"nsw"},
    std::string_view{"nsz"},
    std::string_view{"nusw"},
    std::string_view{"nuw"},
    std::string_view{"offset"},
    std::string_view{"pcsections"},
    std::string_view{"post-instr-symbol"},
    std::string_view{"ppc_fp128"},
    std::string_view{"pre-instr-symbol"},
    std::string_view{"reassoc"},
    std::string_view{"rel_offset"},
    std::string_view{"remember_state"},
    std::string_view{"renamable"},
    std::string_view{"register"},
    std::string_view{"restore"},
    std::string_view{"restore_state"},
    std::string_view{"same_value"},
    std::string_view{"samesign"},
    std::string_view{"shufflemask"},
    std::string_view{"stack"},
    std::string_view{"successors"},
    std::string_view{"target-flags"},
    std::string_view{"target-index"},
    std::string_view{"tied-def"},
    std::string_view{"undef"},
    std::string_view{"undefined"},
    std::string_view{"unknown-address"},
    std::string_view{"unknown-size"},
    std::string_view{"unpredictable"},
    std::string_view{"volatile"},
    std::string_view{"window_save"},
    std::string_view{"x86_fp80"},
});

constexpr auto MetadataKeywords = sortSpellings(std::array{
    std::string_view{"!DIExpression"},
    std::string_view{"!DILocation"},
    std::string_view{"!alias.scope"},
    std::string_view{"!noalias"},
    std::string_view{"!noalias.addrspace"},
    std::string_view{"!range"},
    std::string_view{"!tbaa"},
});

static_assert(hasUniqueSpellings(Keywords));
static_assert(hasUniqueSpellings(MetadataKeywords));

[[nodiscard]] constexpr bool isASCIIAlpha(char Character) noexcept {
  return (Character >= 'a' && Character <= 'z') || (Character >= 'A' && Character <= 'Z');
}

[[nodiscard]] constexpr bool isASCIIDigit(char Character) noexcept {
  return Character >= '0' && Character <= '9';
}

[[nodiscard]] constexpr bool isASCIIHexDigit(char Character) noexcept {
  return isASCIIDigit(Character) || (Character >= 'a' && Character <= 'f') ||
         (Character >= 'A' && Character <= 'F');
}

[[nodiscard]] constexpr unsigned hexDigitValue(char Character) noexcept {
  if (isASCIIDigit(Character))
    return static_cast<unsigned>(Character - '0');
  if (Character >= 'a' && Character <= 'f')
    return static_cast<unsigned>(Character - 'a' + 10);
  return static_cast<unsigned>(Character - 'A' + 10);
}

[[nodiscard]] constexpr bool isIdentifierCharacter(char Character) noexcept {
  return isASCIIAlpha(Character) || isASCIIDigit(Character) || Character == '_' ||
         Character == '-' || Character == '.' || Character == '$';
}

[[nodiscard]] constexpr bool isRegisterCharacter(char Character) noexcept {
  return isIdentifierCharacter(Character) && Character != '.';
}

[[nodiscard]] constexpr bool isHorizontalWhitespace(char Character) noexcept {
  return Character == ' ' || Character == '\t';
}

[[nodiscard]] constexpr bool isNewlineCharacter(char Character) noexcept {
  return Character == '\n' || Character == '\r';
}

[[nodiscard]] constexpr bool isHexFloatingPointPrefix(char Character) noexcept {
  return Character == 'H' || Character == 'K' || Character == 'L' || Character == 'M' ||
         Character == 'R';
}

[[nodiscard]] std::string unexpectedCharacterMessage(unsigned char Character) {
  if (Character >= 0x20U && Character <= 0x7EU) {
    return "unexpected character '" + std::string(1, static_cast<char>(Character)) + "'";
  }
  std::ostringstream Message;
  Message << "unexpected byte 0x" << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
          << static_cast<unsigned>(Character);
  return Message.str();
}

} // namespace

std::string_view getMITokenKindName(MITokenKind Kind) noexcept {
  switch (Kind) {
  case MITokenKind::EndOfFile:
    return "end-of-file";
  case MITokenKind::Error:
    return "error";
  case MITokenKind::Newline:
    return "newline";
  case MITokenKind::Comma:
    return "comma";
  case MITokenKind::Equal:
    return "equal";
  case MITokenKind::Underscore:
    return "underscore";
  case MITokenKind::Colon:
    return "colon";
  case MITokenKind::DoubleColon:
    return "double-colon";
  case MITokenKind::Dot:
    return "dot";
  case MITokenKind::Exclaim:
    return "exclaim";
  case MITokenKind::LeftParen:
    return "left-paren";
  case MITokenKind::RightParen:
    return "right-paren";
  case MITokenKind::LeftBrace:
    return "left-brace";
  case MITokenKind::RightBrace:
    return "right-brace";
  case MITokenKind::Plus:
    return "plus";
  case MITokenKind::Minus:
    return "minus";
  case MITokenKind::Less:
    return "less";
  case MITokenKind::Greater:
    return "greater";
  case MITokenKind::Keyword:
    return "keyword";
  case MITokenKind::MetadataKeyword:
    return "metadata-keyword";
  case MITokenKind::Identifier:
    return "identifier";
  case MITokenKind::NamedRegister:
    return "named-register";
  case MITokenKind::NamedVirtualRegister:
    return "named-virtual-register";
  case MITokenKind::MachineBasicBlockLabel:
    return "machine-basic-block-label";
  case MITokenKind::MachineBasicBlock:
    return "machine-basic-block";
  case MITokenKind::StackObject:
    return "stack-object";
  case MITokenKind::FixedStackObject:
    return "fixed-stack-object";
  case MITokenKind::NamedGlobalValue:
    return "named-global-value";
  case MITokenKind::GlobalValue:
    return "global-value";
  case MITokenKind::ExternalSymbol:
    return "external-symbol";
  case MITokenKind::MCSymbol:
    return "mc-symbol";
  case MITokenKind::IntegerLiteral:
    return "integer-literal";
  case MITokenKind::FloatingPointLiteral:
    return "floating-point-literal";
  case MITokenKind::HexLiteral:
    return "hex-literal";
  case MITokenKind::VirtualRegister:
    return "virtual-register";
  case MITokenKind::ConstantPoolItem:
    return "constant-pool-item";
  case MITokenKind::JumpTableIndex:
    return "jump-table-index";
  case MITokenKind::NamedIRBlock:
    return "named-ir-block";
  case MITokenKind::IRBlock:
    return "ir-block";
  case MITokenKind::NamedIRValue:
    return "named-ir-value";
  case MITokenKind::IRValue:
    return "ir-value";
  case MITokenKind::QuotedIRValue:
    return "quoted-ir-value";
  case MITokenKind::SubRegisterIndex:
    return "subregister-index";
  case MITokenKind::StringConstant:
    return "string-constant";
  }
  return "unknown";
}

bool isMIKeyword(std::string_view Spelling) noexcept {
  return std::ranges::binary_search(Keywords, Spelling);
}

bool isMIMetadataKeyword(std::string_view Spelling) noexcept {
  return std::ranges::binary_search(MetadataKeywords, Spelling);
}

std::string_view MIToken::stringValue() const noexcept {
  if (const auto *View = std::get_if<std::string_view>(&StringValue))
    return *View;
  if (const auto *Owned = std::get_if<std::string>(&StringValue))
    return *Owned;
  return {};
}

bool MIToken::isRegister() const noexcept {
  return Kind == MITokenKind::NamedRegister || Kind == MITokenKind::NamedVirtualRegister ||
         Kind == MITokenKind::VirtualRegister || Kind == MITokenKind::Underscore;
}

bool MIToken::isRegisterFlag() const noexcept {
  return isKeyword("implicit") || isKeyword("implicit-def") || isKeyword("def") ||
         isKeyword("dead") || isKeyword("killed") || isKeyword("undef") || isKeyword("internal") ||
         isKeyword("early-clobber") || isKeyword("debug-use") || isKeyword("renamable");
}

bool MIToken::isMemoryOperandFlag() const noexcept {
  return isKeyword("volatile") || isKeyword("non-temporal") || isKeyword("dereferenceable") ||
         isKeyword("invariant") || Kind == MITokenKind::StringConstant;
}

MIToken MILexer::makeToken(MITokenKind Kind, TextRange Range, std::string_view Spelling,
                           std::string_view IntegerValue) {
  return MIToken(Kind, Range, Spelling, {}, IntegerValue);
}

MIToken MILexer::makeStringToken(MITokenKind Kind, TextRange Range, std::string_view Spelling,
                                 std::string_view StringValue, std::string_view IntegerValue) {
  return MIToken(Kind, Range, Spelling, StringValue, IntegerValue);
}

MIToken MILexer::makeOwnedStringToken(MITokenKind Kind, TextRange Range, std::string_view Spelling,
                                      std::string StringValue, std::string_view IntegerValue) {
  return MIToken(Kind, Range, Spelling, std::move(StringValue), IntegerValue);
}

class MILexer::Impl final {
public:
  Impl(std::string SourceName, std::string SourceText, MILexerOptions Options)
      : Source(std::move(SourceName), std::move(SourceText)), Options(Options) {}

  [[nodiscard]] MIToken next() {
    if (Failed)
      return token(MITokenKind::EndOfFile, Offset, Offset);

    if (auto TriviaError = skipTrivia())
      return std::move(*TriviaError);

    if (atEnd())
      return token(MITokenKind::EndOfFile, Offset, Offset);

    const std::size_t Begin = Offset;
    MIToken Result = lexToken();
    if (Result.kind() == MITokenKind::Error)
      return Result;

    if (Options.MaximumTokenLength != 0 &&
        Result.range().End.Offset - Result.range().Begin.Offset > Options.MaximumTokenLength) {
      return fail(Begin, Offset, "machine instruction token exceeds the configured length limit");
    }
    if (Options.MaximumTokens != 0 && TokenCount >= Options.MaximumTokens) {
      return fail(Begin, Offset, "machine instruction token count exceeds the configured limit");
    }
    ++TokenCount;
    return Result;
  }

  [[nodiscard]] const TextSource &source() const noexcept {
    return Source;
  }

  [[nodiscard]] std::span<const TextDiagnostic> diagnostics() const noexcept {
    return Diagnostics;
  }

  [[nodiscard]] bool failed() const noexcept {
    return Failed;
  }

  [[nodiscard]] std::size_t tokenCount() const noexcept {
    return TokenCount;
  }

private:
  struct QuotedValue final {
    std::size_t End = 0;
    std::string Value;
  };

  [[nodiscard]] std::string_view text() const noexcept {
    return Source.text();
  }

  [[nodiscard]] bool atEnd(std::size_t Lookahead = 0) const noexcept {
    return Offset >= text().size() || Lookahead >= text().size() - Offset;
  }

  [[nodiscard]] char peek(std::size_t Lookahead = 0) const noexcept {
    if (Offset >= text().size() || Lookahead >= text().size() - Offset)
      return '\0';
    return text()[Offset + Lookahead];
  }

  [[nodiscard]] bool startsWith(std::string_view Prefix) const noexcept {
    return text().substr(Offset).starts_with(Prefix);
  }

  [[nodiscard]] std::string_view slice(std::size_t Begin, std::size_t End) const noexcept {
    assert(Begin <= End && End <= text().size());
    return text().substr(Begin, End - Begin);
  }

  [[nodiscard]] MIToken token(MITokenKind Kind, std::size_t Begin, std::size_t End,
                              std::string_view IntegerValue = {}) const {
    return MILexer::makeToken(Kind, Source.range(Begin, End), slice(Begin, End), IntegerValue);
  }

  [[nodiscard]] MIToken stringToken(MITokenKind Kind, std::size_t Begin, std::size_t End,
                                    std::string_view Value,
                                    std::string_view IntegerValue = {}) const {
    return MILexer::makeStringToken(Kind, Source.range(Begin, End), slice(Begin, End), Value,
                                    IntegerValue);
  }

  [[nodiscard]] MIToken ownedStringToken(MITokenKind Kind, std::size_t Begin, std::size_t End,
                                         std::string Value,
                                         std::string_view IntegerValue = {}) const {
    return MILexer::makeOwnedStringToken(Kind, Source.range(Begin, End), slice(Begin, End),
                                         std::move(Value), IntegerValue);
  }

  [[nodiscard]] MIToken fail(std::size_t Begin, std::size_t End, std::string Message) {
    End = std::clamp(End, Begin, text().size());
    Diagnostics.push_back(TextDiagnostic{
        .Severity = DiagnosticSeverity::Error,
        .Category = "mir-lexer",
        .Message = std::move(Message),
        .SourceName = std::string(Source.name()),
        .Range = Source.range(Begin, End),
    });
    Failed = true;
    return token(MITokenKind::Error, Begin, End);
  }

  [[nodiscard]] std::optional<MIToken> skipTrivia() {
    while (true) {
      while (!atEnd() && isHorizontalWhitespace(peek()))
        ++Offset;

      if (peek() == ';') {
        while (!atEnd() && !isNewlineCharacter(peek()))
          ++Offset;
        continue;
      }

      if (!startsWith("/*"))
        return std::nullopt;

      const std::size_t Begin = Offset;
      Offset += 2;
      while (!atEnd() && !startsWith("*/"))
        ++Offset;
      if (atEnd()) {
        return fail(Begin, Offset,
                    "end of machine instruction input reached before the closing '*/'");
      }
      Offset += 2;
    }
  }

  [[nodiscard]] std::optional<QuotedValue> scanQuotedString(std::size_t QuoteOffset) {
    assert(QuoteOffset < text().size() && text()[QuoteOffset] == '"');
    std::size_t Cursor = QuoteOffset + 1;
    while (Cursor < text().size() && text()[Cursor] != '"') {
      if (isNewlineCharacter(text()[Cursor])) {
        Offset = Cursor;
        static_cast<void>(fail(QuoteOffset, Cursor,
                               "end of machine instruction reached before the closing '\"'"));
        return std::nullopt;
      }
      ++Cursor;
    }
    if (Cursor == text().size()) {
      Offset = Cursor;
      static_cast<void>(
          fail(QuoteOffset, Cursor, "end of machine instruction reached before the closing '\"'"));
      return std::nullopt;
    }

    std::string Value;
    Value.reserve(Cursor - QuoteOffset - 1);
    for (std::size_t Index = QuoteOffset + 1; Index < Cursor; ++Index) {
      const char Character = text()[Index];
      if (Character == '\\' && Index + 1 < Cursor && text()[Index + 1] == '\\') {
        Value.push_back('\\');
        ++Index;
        continue;
      }
      if (Character == '\\' && Index + 2 < Cursor && isASCIIHexDigit(text()[Index + 1]) &&
          isASCIIHexDigit(text()[Index + 2])) {
        const unsigned Decoded =
            hexDigitValue(text()[Index + 1]) * 16U + hexDigitValue(text()[Index + 2]);
        Value.push_back(static_cast<char>(Decoded));
        Index += 2;
        continue;
      }
      Value.push_back(Character);
    }
    return QuotedValue{.End = Cursor + 1, .Value = std::move(Value)};
  }

  [[nodiscard]] MIToken lexName(MITokenKind Kind, std::size_t PrefixLength) {
    const std::size_t Begin = Offset;
    Offset += PrefixLength;
    if (peek() == '"') {
      auto Quoted = scanQuotedString(Offset);
      if (!Quoted)
        return token(MITokenKind::Error, Begin, Offset);
      Offset = Quoted->End;
      return ownedStringToken(Kind, Begin, Offset, std::move(Quoted->Value));
    }

    const std::size_t ValueBegin = Offset;
    while (!atEnd() && isIdentifierCharacter(peek()))
      ++Offset;
    return stringToken(Kind, Begin, Offset, slice(ValueBegin, Offset));
  }

  [[nodiscard]] std::optional<MIToken> lexMachineBasicBlock() {
    const bool IsReference = startsWith("%bb.");
    if (!IsReference && !startsWith("bb."))
      return std::nullopt;

    const std::size_t Begin = Offset;
    const std::size_t PrefixLength = IsReference ? 4 : 3;
    Offset += PrefixLength;
    if (!isASCIIDigit(peek())) {
      return fail(Offset, std::min(Offset + 1, text().size()),
                  "expected a decimal block number after '" +
                      std::string(IsReference ? "%bb.'" : "bb.'"));
    }

    const std::size_t NumberBegin = Offset;
    while (!atEnd() && isASCIIDigit(peek()))
      ++Offset;
    const std::string_view Number = slice(NumberBegin, Offset);

    std::size_t NameBegin = Offset;
    if (peek() == '.') {
      ++Offset;
      NameBegin = Offset;
      while (!atEnd() && isIdentifierCharacter(peek()))
        ++Offset;
    }

    return stringToken(IsReference ? MITokenKind::MachineBasicBlock
                                   : MITokenKind::MachineBasicBlockLabel,
                       Begin, Offset, slice(NameBegin, Offset), Number);
  }

  [[nodiscard]] std::optional<MIToken> lexIndex(std::string_view Prefix, MITokenKind Kind,
                                                bool HasOptionalName) {
    if (!startsWith(Prefix) || !isASCIIDigit(peek(Prefix.size())))
      return std::nullopt;

    const std::size_t Begin = Offset;
    Offset += Prefix.size();
    const std::size_t NumberBegin = Offset;
    while (!atEnd() && isASCIIDigit(peek()))
      ++Offset;
    const std::string_view Number = slice(NumberBegin, Offset);

    if (!HasOptionalName)
      return token(Kind, Begin, Offset, Number);

    std::size_t NameBegin = Offset;
    if (peek() == '.') {
      ++Offset;
      NameBegin = Offset;
      while (!atEnd() && isIdentifierCharacter(peek()))
        ++Offset;
    }
    return stringToken(Kind, Begin, Offset, slice(NameBegin, Offset), Number);
  }

  [[nodiscard]] std::optional<MIToken>
  lexIRReference(std::string_view Prefix, MITokenKind NumericKind, MITokenKind NamedKind) {
    if (!startsWith(Prefix))
      return std::nullopt;
    if (isASCIIDigit(peek(Prefix.size())))
      return lexIndex(Prefix, NumericKind, false);
    return lexName(NamedKind, Prefix.size());
  }

  [[nodiscard]] std::optional<MIToken> lexRegister() {
    if (peek() != '%' && peek() != '$')
      return std::nullopt;

    const std::size_t Begin = Offset;
    const char Prefix = peek();
    ++Offset;
    if (Prefix == '%' && isASCIIDigit(peek())) {
      const std::size_t NumberBegin = Offset;
      while (!atEnd() && isASCIIDigit(peek()))
        ++Offset;
      return token(MITokenKind::VirtualRegister, Begin, Offset, slice(NumberBegin, Offset));
    }

    if (Prefix == '%' && !isRegisterCharacter(peek())) {
      Offset = Begin;
      return std::nullopt;
    }

    const std::size_t NameBegin = Offset;
    while (!atEnd() && isRegisterCharacter(peek()))
      ++Offset;
    return stringToken(Prefix == '%' ? MITokenKind::NamedVirtualRegister
                                     : MITokenKind::NamedRegister,
                       Begin, Offset, slice(NameBegin, Offset));
  }

  [[nodiscard]] std::optional<MIToken> lexGlobalValue() {
    if (peek() != '@')
      return std::nullopt;
    if (!isASCIIDigit(peek(1)))
      return lexName(MITokenKind::NamedGlobalValue, 1);

    const std::size_t Begin = Offset++;
    const std::size_t NumberBegin = Offset;
    while (!atEnd() && isASCIIDigit(peek()))
      ++Offset;
    return token(MITokenKind::GlobalValue, Begin, Offset, slice(NumberBegin, Offset));
  }

  [[nodiscard]] std::optional<MIToken> lexMCSymbol() {
    constexpr std::string_view Prefix = "<mcsymbol ";
    if (!startsWith(Prefix))
      return std::nullopt;

    const std::size_t Begin = Offset;
    Offset += Prefix.size();
    if (peek() != '"') {
      const std::size_t NameBegin = Offset;
      while (!atEnd() && isIdentifierCharacter(peek()))
        ++Offset;
      const std::string_view Name = slice(NameBegin, Offset);
      if (peek() != '>') {
        return fail(Begin, Offset, "expected the '<mcsymbol ...' operand to be closed by '>'");
      }
      ++Offset;
      return stringToken(MITokenKind::MCSymbol, Begin, Offset, Name);
    }

    auto Quoted = scanQuotedString(Offset);
    if (!Quoted)
      return token(MITokenKind::Error, Begin, Offset);
    Offset = Quoted->End;
    if (peek() != '>') {
      return fail(Begin, Offset, "expected the '<mcsymbol ...' operand to be closed by '>'");
    }
    ++Offset;
    return ownedStringToken(MITokenKind::MCSymbol, Begin, Offset, std::move(Quoted->Value));
  }

  [[nodiscard]] std::optional<MIToken> lexFloatHexBits() {
    if (!startsWith("f0x") && !startsWith("f0X"))
      return std::nullopt;
    std::size_t End = Offset + 3;
    while (End < text().size() && isASCIIHexDigit(text()[End]))
      ++End;
    if (End == Offset + 3)
      return std::nullopt;
    const std::size_t Begin = Offset;
    Offset = End;
    return token(MITokenKind::FloatingPointLiteral, Begin, Offset);
  }

  [[nodiscard]] std::optional<MIToken> lexHexadecimalLiteral() {
    if (peek() != '0' || (peek(1) != 'x' && peek(1) != 'X'))
      return std::nullopt;

    std::size_t End = Offset + 2;
    bool IsFloatingPoint = false;
    if (End < text().size() && isHexFloatingPointPrefix(text()[End])) {
      IsFloatingPoint = true;
      ++End;
    }
    const std::size_t DigitsBegin = End;
    while (End < text().size() && isASCIIHexDigit(text()[End]))
      ++End;
    if (End == DigitsBegin)
      return std::nullopt;

    const std::size_t Begin = Offset;
    Offset = End;
    return token(IsFloatingPoint ? MITokenKind::FloatingPointLiteral : MITokenKind::HexLiteral,
                 Begin, Offset);
  }

  [[nodiscard]] std::optional<MIToken> lexNumericalLiteral() {
    if (!isASCIIDigit(peek()) && (peek() != '-' || !isASCIIDigit(peek(1)))) {
      return std::nullopt;
    }

    const std::size_t Begin = Offset;
    if (peek() == '-')
      ++Offset;
    while (!atEnd() && isASCIIDigit(peek()))
      ++Offset;

    if (peek() != '.') {
      return token(MITokenKind::IntegerLiteral, Begin, Offset, slice(Begin, Offset));
    }

    ++Offset;
    while (!atEnd() && isASCIIDigit(peek()))
      ++Offset;
    if ((peek() == 'e' || peek() == 'E') &&
        (isASCIIDigit(peek(1)) || ((peek(1) == '-' || peek(1) == '+') && isASCIIDigit(peek(2))))) {
      ++Offset;
      if (peek() == '-' || peek() == '+')
        ++Offset;
      while (!atEnd() && isASCIIDigit(peek()))
        ++Offset;
    }
    return token(MITokenKind::FloatingPointLiteral, Begin, Offset);
  }

  [[nodiscard]] std::optional<MIToken> lexExclaim() {
    if (peek() != '!')
      return std::nullopt;

    const std::size_t Begin = Offset++;
    if (isASCIIDigit(peek()) || !isIdentifierCharacter(peek()))
      return token(MITokenKind::Exclaim, Begin, Offset);

    while (!atEnd() && isIdentifierCharacter(peek()))
      ++Offset;
    const std::string_view Spelling = slice(Begin, Offset);
    if (!isMIMetadataKeyword(Spelling)) {
      return fail(Begin, Offset, "use of unknown metadata keyword '" + std::string(Spelling) + "'");
    }
    return stringToken(MITokenKind::MetadataKeyword, Begin, Offset, Spelling);
  }

  [[nodiscard]] std::optional<MIToken> lexSymbol() {
    const std::size_t Begin = Offset;
    if (startsWith("::")) {
      Offset += 2;
      return token(MITokenKind::DoubleColon, Begin, Offset);
    }

    MITokenKind Kind = MITokenKind::Error;
    switch (peek()) {
    case ',':
      Kind = MITokenKind::Comma;
      break;
    case '=':
      Kind = MITokenKind::Equal;
      break;
    case ':':
      Kind = MITokenKind::Colon;
      break;
    case '.':
      Kind = MITokenKind::Dot;
      break;
    case '(':
      Kind = MITokenKind::LeftParen;
      break;
    case ')':
      Kind = MITokenKind::RightParen;
      break;
    case '{':
      Kind = MITokenKind::LeftBrace;
      break;
    case '}':
      Kind = MITokenKind::RightBrace;
      break;
    case '+':
      Kind = MITokenKind::Plus;
      break;
    case '-':
      Kind = MITokenKind::Minus;
      break;
    case '<':
      Kind = MITokenKind::Less;
      break;
    case '>':
      Kind = MITokenKind::Greater;
      break;
    default:
      return std::nullopt;
    }
    ++Offset;
    return token(Kind, Begin, Offset);
  }

  [[nodiscard]] std::optional<MIToken> lexNewline() {
    if (!isNewlineCharacter(peek()))
      return std::nullopt;
    const std::size_t Begin = Offset++;
    if (text()[Begin] == '\r' && peek() == '\n')
      ++Offset;
    return token(MITokenKind::Newline, Begin, Offset);
  }

  [[nodiscard]] std::optional<MIToken> lexQuotedIRValue() {
    if (peek() != '`')
      return std::nullopt;

    const std::size_t Begin = Offset++;
    const std::size_t ValueBegin = Offset;
    while (!atEnd() && peek() != '`' && !isNewlineCharacter(peek()))
      ++Offset;
    if (atEnd() || isNewlineCharacter(peek())) {
      return fail(Begin, Offset, "end of machine instruction reached before the closing '`'");
    }
    const std::size_t ValueEnd = Offset++;
    return stringToken(MITokenKind::QuotedIRValue, Begin, Offset, slice(ValueBegin, ValueEnd));
  }

  [[nodiscard]] MIToken lexToken() {
    if (auto Result = lexMachineBasicBlock())
      return std::move(*Result);
    if (auto Result = lexFloatHexBits())
      return std::move(*Result);

    if (isASCIIAlpha(peek()) || peek() == '_') {
      const std::size_t Begin = Offset;
      while (!atEnd() && isIdentifierCharacter(peek()))
        ++Offset;
      const std::string_view Value = slice(Begin, Offset);
      if (Value == "_")
        return token(MITokenKind::Underscore, Begin, Offset);
      return stringToken(isMIKeyword(Value) ? MITokenKind::Keyword : MITokenKind::Identifier, Begin,
                         Offset, Value);
    }

    if (auto Result = lexIndex("%jump-table.", MITokenKind::JumpTableIndex, false))
      return std::move(*Result);
    if (auto Result = lexIndex("%stack.", MITokenKind::StackObject, true))
      return std::move(*Result);
    if (auto Result = lexIndex("%fixed-stack.", MITokenKind::FixedStackObject, false))
      return std::move(*Result);
    if (auto Result = lexIndex("%const.", MITokenKind::ConstantPoolItem, false))
      return std::move(*Result);
    if (startsWith("%subreg."))
      return lexName(MITokenKind::SubRegisterIndex, 8);
    if (auto Result = lexIRReference("%ir-block.", MITokenKind::IRBlock, MITokenKind::NamedIRBlock))
      return std::move(*Result);
    if (auto Result = lexIRReference("%ir.", MITokenKind::IRValue, MITokenKind::NamedIRValue))
      return std::move(*Result);
    if (auto Result = lexRegister())
      return std::move(*Result);
    if (auto Result = lexGlobalValue())
      return std::move(*Result);
    if (peek() == '&')
      return lexName(MITokenKind::ExternalSymbol, 1);
    if (auto Result = lexMCSymbol())
      return std::move(*Result);
    if (auto Result = lexHexadecimalLiteral())
      return std::move(*Result);
    if (auto Result = lexNumericalLiteral())
      return std::move(*Result);
    if (auto Result = lexExclaim())
      return std::move(*Result);
    if (auto Result = lexSymbol())
      return std::move(*Result);
    if (auto Result = lexNewline())
      return std::move(*Result);
    if (auto Result = lexQuotedIRValue())
      return std::move(*Result);
    if (peek() == '"')
      return lexName(MITokenKind::StringConstant, 0);

    const std::size_t Begin = Offset++;
    return fail(Begin, Offset,
                unexpectedCharacterMessage(static_cast<unsigned char>(text()[Begin])));
  }

  TextSource Source;
  MILexerOptions Options;
  std::size_t Offset = 0;
  std::size_t TokenCount = 0;
  bool Failed = false;
  std::vector<TextDiagnostic> Diagnostics;
};

MILexer::MILexer(std::string SourceName, std::string SourceText, MILexerOptions Options)
    : Implementation(
          std::make_unique<Impl>(std::move(SourceName), std::move(SourceText), Options)) {}

MILexer::MILexer(MILexer &&) noexcept = default;
MILexer &MILexer::operator=(MILexer &&) noexcept = default;
MILexer::~MILexer() = default;

MIToken MILexer::next() {
  assert(Implementation != nullptr);
  return Implementation->next();
}

const TextSource &MILexer::source() const noexcept {
  assert(Implementation != nullptr);
  return Implementation->source();
}

std::span<const TextDiagnostic> MILexer::diagnostics() const noexcept {
  assert(Implementation != nullptr);
  return Implementation->diagnostics();
}

bool MILexer::failed() const noexcept {
  assert(Implementation != nullptr);
  return Implementation->failed();
}

std::size_t MILexer::tokenCount() const noexcept {
  assert(Implementation != nullptr);
  return Implementation->tokenCount();
}

} // namespace rcc::mir
