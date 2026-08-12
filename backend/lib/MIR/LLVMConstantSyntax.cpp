// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/MIR/LLVMConstantSyntax.h"

#include "rcc/ADT/APFloat.h"
#include "rcc/ADT/APInt.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rcc::mir {
namespace {

struct ParsedName final {
  std::string Value;
  bool Numeric = false;
};

[[nodiscard]] bool isSpace(char Character) noexcept {
  return Character == ' ' || Character == '\t' || Character == '\r' || Character == '\n';
}

[[nodiscard]] bool isIdentifierStart(char Character) noexcept {
  return (Character >= 'A' && Character <= 'Z') || (Character >= 'a' && Character <= 'z') ||
         Character == '-' || Character == '$' || Character == '.' || Character == '_';
}

[[nodiscard]] bool isIdentifierBody(char Character) noexcept {
  return isIdentifierStart(Character) || (Character >= '0' && Character <= '9');
}

[[nodiscard]] bool isHexDigit(char Character) noexcept {
  return (Character >= '0' && Character <= '9') || (Character >= 'a' && Character <= 'f') ||
         (Character >= 'A' && Character <= 'F');
}

[[nodiscard]] unsigned hexDigitValue(char Character) noexcept {
  if (Character >= '0' && Character <= '9')
    return static_cast<unsigned>(Character - '0');
  if (Character >= 'a' && Character <= 'f')
    return static_cast<unsigned>(Character - 'a') + 10U;
  return static_cast<unsigned>(Character - 'A') + 10U;
}

[[nodiscard]] std::optional<FloatSemantics> floatingSemantics(std::string_view Name) noexcept {
  if (Name == "half")
    return FloatSemantics::IEEEHalf;
  if (Name == "bfloat")
    return FloatSemantics::BFloat;
  if (Name == "float")
    return FloatSemantics::IEEESingle;
  if (Name == "double")
    return FloatSemantics::IEEEDouble;
  if (Name == "fp128")
    return FloatSemantics::IEEEQuad;
  if (Name == "ppc_fp128")
    return FloatSemantics::PPCDoubleDouble;
  if (Name == "x86_fp80")
    return FloatSemantics::X87DoubleExtended;
  return std::nullopt;
}

[[nodiscard]] Expected<std::uint64_t> checkedMultiply(std::uint64_t Left, std::uint64_t Right,
                                                      std::string_view Description) {
  if (Left != 0 && Right > std::numeric_limits<std::uint64_t>::max() / Left)
    return makeUnexpected(ErrorCode::Unsupported, std::string(Description) + " is too large");
  return Left * Right;
}

[[nodiscard]] Expected<std::uint64_t> checkedAdd(std::uint64_t Left, std::uint64_t Right,
                                                 std::string_view Description) {
  if (Right > std::numeric_limits<std::uint64_t>::max() - Left)
    return makeUnexpected(ErrorCode::Unsupported, std::string(Description) + " is too large");
  return Left + Right;
}

[[nodiscard]] Expected<std::uint64_t> alignTo(std::uint64_t Value, std::uint64_t AlignmentValue) {
  if (AlignmentValue == 0 || !std::has_single_bit(AlignmentValue))
    return makeUnexpected(ErrorCode::InvalidArgument, "LLVM type alignment is invalid");
  const std::uint64_t Mask = AlignmentValue - 1U;
  if (Value > std::numeric_limits<std::uint64_t>::max() - Mask)
    return makeUnexpected(ErrorCode::Unsupported, "LLVM type allocation size is too large");
  return (Value + Mask) & ~Mask;
}

[[nodiscard]] const GenericConstantType &resolvedType(const GenericConstantType &Type) noexcept {
  const GenericConstantType *Resolved = &Type;
  std::size_t Depth = 0;
  while (Resolved->Kind == GenericConstantTypeKind::Named && Resolved->Elements.size() == 1 &&
         Depth++ < 1024) {
    Resolved = &Resolved->Elements.front();
  }
  return *Resolved;
}

class ConstantParser final {
public:
  ConstantParser(std::string_view SourceText, LLVMConstantSyntaxOptions ParserOptions,
                 std::vector<std::string> ResolvingTypes = {})
      : Text(SourceText), Options(ParserOptions), ResolvingNamedTypes(std::move(ResolvingTypes)) {}

  [[nodiscard]] Expected<GenericConstantData> parse();
  [[nodiscard]] Expected<GenericConstantType> parseTypeOnly();

private:
  template <typename T> [[nodiscard]] Expected<T> fail(std::string_view Message) const {
    return makeUnexpected(ErrorCode::ParseError, "LLVM constant at byte " +
                                                     std::to_string(Position) + ": " +
                                                     std::string(Message));
  }

  void skipSpace() noexcept {
    while (Position < Text.size() && isSpace(Text[Position]))
      ++Position;
  }

  [[nodiscard]] bool atEnd() {
    skipSpace();
    return Position == Text.size();
  }

  [[nodiscard]] bool consume(char Character) {
    skipSpace();
    if (Position >= Text.size() || Text[Position] != Character)
      return false;
    ++Position;
    return true;
  }

  [[nodiscard]] bool consumeWord(std::string_view Word) {
    skipSpace();
    if (!Text.substr(Position).starts_with(Word))
      return false;
    const std::size_t End = Position + Word.size();
    if (End < Text.size() && isIdentifierBody(Text[End]))
      return false;
    Position = End;
    return true;
  }

  [[nodiscard]] std::optional<std::string_view> peekWord() {
    skipSpace();
    if (Position >= Text.size() || !isIdentifierStart(Text[Position]))
      return std::nullopt;
    std::size_t End = Position + 1;
    while (End < Text.size() && isIdentifierBody(Text[End]))
      ++End;
    return Text.substr(Position, End - Position);
  }

  [[nodiscard]] Expected<void> expect(char Character, std::string_view Description) {
    if (!consume(Character))
      return fail<void>("expected " + std::string(Description));
    return {};
  }

  [[nodiscard]] Expected<void> expectWord(std::string_view Word) {
    if (!consumeWord(Word))
      return fail<void>("expected '" + std::string(Word) + "'");
    return {};
  }

  [[nodiscard]] Expected<std::uint64_t> parseUnsigned();
  [[nodiscard]] Expected<ParsedName> parseName(char Prefix);
  [[nodiscard]] Expected<std::vector<std::byte>> parseByteString();
  [[nodiscard]] Expected<GenericConstantType> parseType(std::size_t Depth = 0);
  [[nodiscard]] Expected<void> resolveNamedType(GenericConstantType &Type, std::size_t Depth);
  [[nodiscard]] Expected<GenericConstantType> parsePrimaryType(std::size_t Depth);
  [[nodiscard]] Expected<GenericConstantData> parseTypedValue(std::size_t Depth);
  [[nodiscard]] Expected<GenericConstantData> parseValue(GenericConstantType Type,
                                                         std::size_t Depth);
  [[nodiscard]] Expected<GenericConstantData>
  parseAggregate(GenericConstantType Type, char Open, char Close, bool Packed, std::size_t Depth);
  [[nodiscard]] Expected<GenericConstantData> parseFloatingValue(GenericConstantType Type,
                                                                 std::size_t Depth);
  [[nodiscard]] Expected<GenericConstantData>
  parseExpression(GenericConstantType Type, std::string_view Opcode, std::size_t Depth);

  [[nodiscard]] Expected<void> accountNode(std::size_t Depth) {
    if (Depth > Options.MaximumDepth || ++Nodes > Options.MaximumNodes)
      return fail<void>("constant exceeds configured structural limits");
    return {};
  }

  [[nodiscard]] Expected<std::string_view> parseValueToken();

  std::string_view Text;
  LLVMConstantSyntaxOptions Options;
  std::size_t Position = 0;
  std::size_t Nodes = 0;
  std::vector<std::string> ResolvingNamedTypes;
};

Expected<std::uint64_t> ConstantParser::parseUnsigned() {
  skipSpace();
  const std::size_t Begin = Position;
  while (Position < Text.size() && Text[Position] >= '0' && Text[Position] <= '9')
    ++Position;
  if (Begin == Position)
    return fail<std::uint64_t>("expected an unsigned integer");
  std::uint64_t Value = 0;
  const auto Parsed = std::from_chars(Text.data() + Begin, Text.data() + Position, Value, 10);
  if (Parsed.ec != std::errc{})
    return fail<std::uint64_t>("unsigned integer is out of range");
  return Value;
}

Expected<ParsedName> ConstantParser::parseName(char Prefix) {
  skipSpace();
  if (Position >= Text.size() || Text[Position] != Prefix)
    return fail<ParsedName>(std::string("expected '") + Prefix + "' identifier");
  ++Position;
  if (Position >= Text.size())
    return fail<ParsedName>("identifier is empty");

  ParsedName Result;
  if (Text[Position] == '"') {
    ++Position;
    while (Position < Text.size() && Text[Position] != '"') {
      if (Text[Position] != '\\') {
        if (static_cast<unsigned char>(Text[Position]) < 0x20U)
          return fail<ParsedName>("identifier contains a control character");
        Result.Value.push_back(Text[Position++]);
        continue;
      }
      ++Position;
      if (Position + 1 >= Text.size() || !isHexDigit(Text[Position]) ||
          !isHexDigit(Text[Position + 1])) {
        return fail<ParsedName>("identifier escape requires two hexadecimal digits");
      }
      Result.Value.push_back(static_cast<char>((hexDigitValue(Text[Position]) << 4U) |
                                               hexDigitValue(Text[Position + 1])));
      Position += 2;
    }
    if (Position >= Text.size())
      return fail<ParsedName>("quoted identifier is unterminated");
    ++Position;
    return Result;
  }

  if (Text[Position] >= '0' && Text[Position] <= '9') {
    Result.Numeric = true;
    const std::size_t Begin = Position;
    while (Position < Text.size() && Text[Position] >= '0' && Text[Position] <= '9')
      ++Position;
    Result.Value = std::string(Text.substr(Begin, Position - Begin));
    return Result;
  }
  if (!isIdentifierStart(Text[Position]))
    return fail<ParsedName>("identifier has an invalid first character");
  const std::size_t Begin = Position++;
  while (Position < Text.size() && isIdentifierBody(Text[Position]))
    ++Position;
  Result.Value = std::string(Text.substr(Begin, Position - Begin));
  return Result;
}

Expected<std::vector<std::byte>> ConstantParser::parseByteString() {
  skipSpace();
  if (Position >= Text.size() || Text[Position] != '"')
    return fail<std::vector<std::byte>>("expected an LLVM byte string");
  ++Position;
  std::vector<std::byte> Bytes;
  while (Position < Text.size() && Text[Position] != '"') {
    const char Character = Text[Position++];
    if (Character != '\\') {
      if (static_cast<unsigned char>(Character) < 0x20U)
        return fail<std::vector<std::byte>>("byte string contains a control character");
      Bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(Character)));
      continue;
    }
    if (Position + 1 >= Text.size() || !isHexDigit(Text[Position]) ||
        !isHexDigit(Text[Position + 1])) {
      return fail<std::vector<std::byte>>("byte-string escape requires two hexadecimal digits");
    }
    Bytes.push_back(static_cast<std::byte>((hexDigitValue(Text[Position]) << 4U) |
                                           hexDigitValue(Text[Position + 1])));
    Position += 2;
  }
  if (Position >= Text.size())
    return fail<std::vector<std::byte>>("LLVM byte string is unterminated");
  ++Position;
  return Bytes;
}

Expected<GenericConstantType> ConstantParser::parsePrimaryType(std::size_t Depth) {
  if (auto Limit = accountNode(Depth); !Limit)
    return std::unexpected(Limit.error());
  skipSpace();

  if (consume('[')) {
    auto Count = parseUnsigned();
    if (!Count)
      return std::unexpected(Count.error());
    if (auto Separator = expectWord("x"); !Separator)
      return std::unexpected(Separator.error());
    auto Element = parseType(Depth + 1);
    if (!Element)
      return std::unexpected(Element.error());
    if (auto End = expect(']', "']' after array type"); !End)
      return std::unexpected(End.error());
    return GenericConstantType::array(*Count, std::move(*Element));
  }

  if (consume('<')) {
    if (consume('{')) {
      std::vector<GenericConstantType> Elements;
      if (!consume('}')) {
        while (true) {
          auto Element = parseType(Depth + 1);
          if (!Element)
            return std::unexpected(Element.error());
          Elements.push_back(std::move(*Element));
          if (consume('}'))
            break;
          if (auto Comma = expect(',', "',' between packed struct members"); !Comma)
            return std::unexpected(Comma.error());
        }
      }
      if (auto End = expect('>', "'>' after packed struct type"); !End)
        return std::unexpected(End.error());
      return GenericConstantType::structure(std::move(Elements), true);
    }

    const bool Scalable = consumeWord("vscale");
    if (Scalable) {
      if (auto Separator = expectWord("x"); !Separator)
        return std::unexpected(Separator.error());
    }
    auto Count = parseUnsigned();
    if (!Count)
      return std::unexpected(Count.error());
    if (auto Separator = expectWord("x"); !Separator)
      return std::unexpected(Separator.error());
    auto Element = parseType(Depth + 1);
    if (!Element)
      return std::unexpected(Element.error());
    if (auto End = expect('>', "'>' after vector type"); !End)
      return std::unexpected(End.error());
    return GenericConstantType::vector(*Count, std::move(*Element), Scalable);
  }

  if (consume('{')) {
    std::vector<GenericConstantType> Elements;
    if (!consume('}')) {
      while (true) {
        auto Element = parseType(Depth + 1);
        if (!Element)
          return std::unexpected(Element.error());
        Elements.push_back(std::move(*Element));
        if (consume('}'))
          break;
        if (auto Comma = expect(',', "',' between struct members"); !Comma)
          return std::unexpected(Comma.error());
      }
    }
    return GenericConstantType::structure(std::move(Elements), false);
  }

  skipSpace();
  if (Position < Text.size() && Text[Position] == '%') {
    auto Name = parseName('%');
    if (!Name)
      return std::unexpected(Name.error());
    return GenericConstantType::named(std::move(Name->Value), Name->Numeric);
  }

  const auto Word = peekWord();
  if (!Word)
    return fail<GenericConstantType>("expected an LLVM IR type");
  Position += Word->size();
  if (*Word == "void")
    return GenericConstantType::primitive(GenericConstantTypeKind::Void);
  if (*Word == "label")
    return GenericConstantType::primitive(GenericConstantTypeKind::Label);
  if (*Word == "token")
    return GenericConstantType::primitive(GenericConstantTypeKind::Token);
  if (*Word == "metadata")
    return GenericConstantType::primitive(GenericConstantTypeKind::Metadata);
  if (*Word == "byte")
    return GenericConstantType::byte();
  if (Word->starts_with('i')) {
    std::uint64_t Width = 0;
    const auto Parsed = std::from_chars(Word->data() + 1, Word->data() + Word->size(), Width, 10);
    if (Word->size() == 1 || Parsed.ec != std::errc{} ||
        Parsed.ptr != Word->data() + Word->size() ||
        Width > std::numeric_limits<std::uint32_t>::max()) {
      return fail<GenericConstantType>("integer type has an invalid bit width");
    }
    return GenericConstantType::integer(static_cast<std::uint32_t>(Width));
  }
  if (const auto Semantics = floatingSemantics(*Word))
    return GenericConstantType::floating(*Semantics);
  if (*Word == "ptr") {
    std::uint32_t AddressSpace = 0;
    if (consumeWord("addrspace")) {
      if (auto Open = expect('(', "'(' after addrspace"); !Open)
        return std::unexpected(Open.error());
      auto ParsedAddressSpace = parseUnsigned();
      if (!ParsedAddressSpace || *ParsedAddressSpace > std::numeric_limits<std::uint32_t>::max())
        return fail<GenericConstantType>("pointer address space is out of range");
      AddressSpace = static_cast<std::uint32_t>(*ParsedAddressSpace);
      if (auto Close = expect(')', "')' after address space"); !Close)
        return std::unexpected(Close.error());
    }
    return GenericConstantType::pointer(AddressSpace);
  }
  return fail<GenericConstantType>("unsupported LLVM IR type '" + std::string(*Word) + "'");
}

Expected<GenericConstantType> ConstantParser::parseType(std::size_t Depth) {
  auto Type = parsePrimaryType(Depth);
  if (!Type)
    return std::unexpected(Type.error());

  while (true) {
    skipSpace();
    if (consume('(')) {
      std::vector<GenericConstantType> Parameters;
      bool VarArg = false;
      if (!consume(')')) {
        while (true) {
          skipSpace();
          if (Text.substr(Position).starts_with("...")) {
            Position += 3;
            VarArg = true;
            if (auto End = expect(')', "')' after variadic function type"); !End)
              return std::unexpected(End.error());
            break;
          }
          auto Parameter = parseType(Depth + 1);
          if (!Parameter)
            return std::unexpected(Parameter.error());
          Parameters.push_back(std::move(*Parameter));
          if (consume(')'))
            break;
          if (auto Comma = expect(',', "',' between function parameters"); !Comma)
            return std::unexpected(Comma.error());
        }
      }
      Type = GenericConstantType::function(std::move(*Type), std::move(Parameters), VarArg);
      if (!Type)
        return std::unexpected(Type.error());
      continue;
    }

    std::uint32_t AddressSpace = 0;
    const std::size_t BeforeAddressSpace = Position;
    if (consumeWord("addrspace")) {
      if (auto Open = expect('(', "'(' after addrspace"); !Open)
        return std::unexpected(Open.error());
      auto ParsedAddressSpace = parseUnsigned();
      if (!ParsedAddressSpace || *ParsedAddressSpace > std::numeric_limits<std::uint32_t>::max())
        return fail<GenericConstantType>("pointer address space is out of range");
      AddressSpace = static_cast<std::uint32_t>(*ParsedAddressSpace);
      if (auto Close = expect(')', "')' after address space"); !Close)
        return std::unexpected(Close.error());
    }
    if (consume('*')) {
      Type = GenericConstantType::pointer(AddressSpace);
      continue;
    }
    Position = BeforeAddressSpace;
    break;
  }
  if (Type->Kind == GenericConstantTypeKind::Named) {
    if (auto Resolved = resolveNamedType(*Type, Depth + 1); !Resolved)
      return std::unexpected(Resolved.error());
  }
  return Type;
}

Expected<void> ConstantParser::resolveNamedType(GenericConstantType &Type, std::size_t Depth) {
  if (Type.Kind != GenericConstantTypeKind::Named || !Type.Elements.empty())
    return {};
  const auto Definition =
      std::ranges::find_if(Options.NamedTypes, [&Type](const LLVMNamedTypeDefinition &Candidate) {
        return Candidate.Name == Type.Name && Candidate.NumericName == Type.NumericName;
      });
  if (Definition == Options.NamedTypes.end())
    return {};
  if (Depth > Options.MaximumDepth ||
      std::ranges::find(ResolvingNamedTypes, Type.Name) != ResolvingNamedTypes.end()) {
    return fail<void>("named LLVM type definition is recursively unsized");
  }
  std::vector<std::string> Stack = ResolvingNamedTypes;
  Stack.push_back(Type.Name);
  ConstantParser DefinitionParser(Definition->Definition, Options, std::move(Stack));
  auto Resolved = DefinitionParser.parseTypeOnly();
  if (!Resolved)
    return std::unexpected(Resolved.error());
  Type.Elements.push_back(std::move(*Resolved));
  return {};
}

Expected<std::string_view> ConstantParser::parseValueToken() {
  skipSpace();
  const std::size_t Begin = Position;
  unsigned ParenthesisDepth = 0;
  while (Position < Text.size()) {
    const char Character = Text[Position];
    if (Character == '(') {
      ++ParenthesisDepth;
      ++Position;
      continue;
    }
    if (Character == ')') {
      if (ParenthesisDepth == 0)
        break;
      --ParenthesisDepth;
      ++Position;
      continue;
    }
    if (ParenthesisDepth == 0 && (isSpace(Character) || Character == ',' || Character == ']' ||
                                  Character == '}' || Character == '>')) {
      break;
    }
    ++Position;
    if (Position - Begin > Options.MaximumTokenLength)
      return fail<std::string_view>("value token exceeds the configured length limit");
  }
  if (Begin == Position)
    return fail<std::string_view>("expected an LLVM constant value");
  return Text.substr(Begin, Position - Begin);
}

Expected<GenericConstantData> ConstantParser::parseFloatingValue(GenericConstantType Type,
                                                                 std::size_t Depth) {
  const GenericConstantType &EffectiveType = resolvedType(Type);
  if (EffectiveType.Kind != GenericConstantTypeKind::Floating)
    return fail<GenericConstantData>("floating literal requires a floating-point type");
  auto Token = parseValueToken();
  if (!Token)
    return std::unexpected(Token.error());
  auto Bits = APFloat::parse(EffectiveType.Semantics, *Token);
  if (!Bits)
    return fail<GenericConstantData>(Bits.error().message());
  GenericConstantData Value;
  Value.Type = std::move(Type);
  Value.Kind = GenericConstantKind::Floating;
  Value.FloatingValue = *Bits;
  if (auto Limit = accountNode(Depth); !Limit)
    return std::unexpected(Limit.error());
  return Value;
}

Expected<GenericConstantData> ConstantParser::parseAggregate(GenericConstantType Type, char Open,
                                                             char Close, bool Packed,
                                                             std::size_t Depth) {
  if (!consume(Open))
    return fail<GenericConstantData>(std::string("expected '") + Open + "' for aggregate");
  if (Packed && !consume('{'))
    return fail<GenericConstantData>("expected '{' in packed struct constant");

  GenericConstantData Value;
  Value.Type = std::move(Type);
  Value.Kind = GenericConstantKind::Aggregate;
  const char InnerClose = Packed ? '}' : Close;
  if (!consume(InnerClose)) {
    while (true) {
      auto Element = parseTypedValue(Depth + 1);
      if (!Element)
        return std::unexpected(Element.error());
      Value.Elements.push_back(std::move(*Element));
      if (consume(InnerClose))
        break;
      if (auto Comma = expect(',', "',' between aggregate elements"); !Comma)
        return std::unexpected(Comma.error());
    }
  }
  if (Packed) {
    if (auto End = expect('>', "'>' after packed struct constant"); !End)
      return std::unexpected(End.error());
  }
  auto Verification = Value.verify();
  if (!Verification)
    return fail<GenericConstantData>(Verification.error().message());
  return Value;
}

Expected<GenericConstantData> ConstantParser::parseTypedValue(std::size_t Depth) {
  auto Type = parseType(Depth + 1);
  if (!Type)
    return std::unexpected(Type.error());
  return parseValue(std::move(*Type), Depth + 1);
}

Expected<GenericConstantData> ConstantParser::parseExpression(GenericConstantType Type,
                                                              std::string_view Opcode,
                                                              std::size_t Depth) {
  GenericConstantData Value;
  Value.Type = std::move(Type);
  Value.Kind = GenericConstantKind::Expression;

  const auto SetOpcode = [&](GenericConstantExpressionOpcode ParsedOpcode) {
    Value.ExpressionOpcode = ParsedOpcode;
  };
  if (Opcode == "trunc")
    SetOpcode(GenericConstantExpressionOpcode::Trunc);
  else if (Opcode == "bitcast")
    SetOpcode(GenericConstantExpressionOpcode::BitCast);
  else if (Opcode == "addrspacecast")
    SetOpcode(GenericConstantExpressionOpcode::AddressSpaceCast);
  else if (Opcode == "inttoptr")
    SetOpcode(GenericConstantExpressionOpcode::IntToPtr);
  else if (Opcode == "ptrtoaddr")
    SetOpcode(GenericConstantExpressionOpcode::PtrToAddr);
  else if (Opcode == "ptrtoint")
    SetOpcode(GenericConstantExpressionOpcode::PtrToInt);
  else if (Opcode == "add")
    SetOpcode(GenericConstantExpressionOpcode::Add);
  else if (Opcode == "sub")
    SetOpcode(GenericConstantExpressionOpcode::Sub);
  else if (Opcode == "xor")
    SetOpcode(GenericConstantExpressionOpcode::Xor);
  else if (Opcode == "getelementptr")
    SetOpcode(GenericConstantExpressionOpcode::GetElementPtr);
  else if (Opcode == "shufflevector")
    SetOpcode(GenericConstantExpressionOpcode::ShuffleVector);
  else if (Opcode == "insertelement")
    SetOpcode(GenericConstantExpressionOpcode::InsertElement);
  else if (Opcode == "extractelement")
    SetOpcode(GenericConstantExpressionOpcode::ExtractElement);
  else if (Opcode == "splat")
    SetOpcode(GenericConstantExpressionOpcode::Splat);
  else
    return fail<GenericConstantData>("unsupported constant expression '" + std::string(Opcode) +
                                     "'");

  const bool IsCast = Value.ExpressionOpcode == GenericConstantExpressionOpcode::Trunc ||
                      Value.ExpressionOpcode == GenericConstantExpressionOpcode::BitCast ||
                      Value.ExpressionOpcode == GenericConstantExpressionOpcode::AddressSpaceCast ||
                      Value.ExpressionOpcode == GenericConstantExpressionOpcode::IntToPtr ||
                      Value.ExpressionOpcode == GenericConstantExpressionOpcode::PtrToAddr ||
                      Value.ExpressionOpcode == GenericConstantExpressionOpcode::PtrToInt;
  const bool IsBinary = Value.ExpressionOpcode == GenericConstantExpressionOpcode::Add ||
                        Value.ExpressionOpcode == GenericConstantExpressionOpcode::Sub ||
                        Value.ExpressionOpcode == GenericConstantExpressionOpcode::Xor;

  if (IsBinary) {
    while (true) {
      if (consumeWord("nuw")) {
        Value.ExpressionFlags.set(GenericConstantExpressionFlag::NoUnsignedWrap);
        continue;
      }
      if (consumeWord("nsw")) {
        Value.ExpressionFlags.set(GenericConstantExpressionFlag::NoSignedWrap);
        continue;
      }
      break;
    }
  } else if (Value.ExpressionOpcode == GenericConstantExpressionOpcode::GetElementPtr) {
    while (true) {
      if (consumeWord("inbounds")) {
        Value.ExpressionFlags.set(GenericConstantExpressionFlag::InBounds);
        continue;
      }
      if (consumeWord("nusw")) {
        Value.ExpressionFlags.set(GenericConstantExpressionFlag::NoUnsignedSignedWrap);
        continue;
      }
      if (consumeWord("nuw")) {
        Value.ExpressionFlags.set(GenericConstantExpressionFlag::GEPNoUnsignedWrap);
        continue;
      }
      break;
    }
    if (consumeWord("inrange")) {
      if (auto Open = expect('(', "'(' after inrange"); !Open)
        return std::unexpected(Open.error());
      auto StartToken = parseValueToken();
      if (!StartToken)
        return std::unexpected(StartToken.error());
      auto Start = APInt::parseDecimal(64, *StartToken);
      if (!Start)
        return fail<GenericConstantData>("invalid inrange start");
      if (auto Comma = expect(',', "',' in inrange interval"); !Comma)
        return std::unexpected(Comma.error());
      auto EndToken = parseValueToken();
      if (!EndToken)
        return std::unexpected(EndToken.error());
      auto End = APInt::parseDecimal(64, *EndToken);
      if (!End)
        return fail<GenericConstantData>("invalid inrange end");
      if (auto Close = expect(')', "')' after inrange interval"); !Close)
        return std::unexpected(Close.error());
      Value.InRangeStart = std::move(*Start);
      Value.InRangeEnd = std::move(*End);
    }
  }

  if (auto Open = expect('(', "'(' after constant-expression opcode"); !Open)
    return std::unexpected(Open.error());
  if (Value.ExpressionOpcode == GenericConstantExpressionOpcode::GetElementPtr) {
    auto SourceType = parseType(Depth + 1);
    if (!SourceType)
      return std::unexpected(SourceType.error());
    Value.SourceElementType = std::move(*SourceType);
    if (auto Comma = expect(',', "',' after getelementptr source type"); !Comma)
      return std::unexpected(Comma.error());
  }

  auto First = parseTypedValue(Depth + 1);
  if (!First)
    return std::unexpected(First.error());
  Value.Elements.push_back(std::move(*First));
  if (IsCast) {
    if (auto To = expectWord("to"); !To)
      return std::unexpected(To.error());
    auto Destination = parseType(Depth + 1);
    if (!Destination)
      return std::unexpected(Destination.error());
    if (*Destination != Value.Type)
      return fail<GenericConstantData>("constant-expression destination type does not match value");
  } else {
    while (consume(',')) {
      auto Operand = parseTypedValue(Depth + 1);
      if (!Operand)
        return std::unexpected(Operand.error());
      Value.Elements.push_back(std::move(*Operand));
    }
  }
  if (auto Close = expect(')', "')' after constant expression"); !Close)
    return std::unexpected(Close.error());

  const std::size_t RequiredOperands = [&]() -> std::size_t {
    switch (Value.ExpressionOpcode) {
    case GenericConstantExpressionOpcode::Trunc:
    case GenericConstantExpressionOpcode::BitCast:
    case GenericConstantExpressionOpcode::AddressSpaceCast:
    case GenericConstantExpressionOpcode::IntToPtr:
    case GenericConstantExpressionOpcode::PtrToAddr:
    case GenericConstantExpressionOpcode::PtrToInt:
    case GenericConstantExpressionOpcode::Splat:
      return 1;
    case GenericConstantExpressionOpcode::Add:
    case GenericConstantExpressionOpcode::Sub:
    case GenericConstantExpressionOpcode::Xor:
    case GenericConstantExpressionOpcode::ExtractElement:
      return 2;
    case GenericConstantExpressionOpcode::ShuffleVector:
    case GenericConstantExpressionOpcode::InsertElement:
      return 3;
    case GenericConstantExpressionOpcode::GetElementPtr:
      return 1;
    case GenericConstantExpressionOpcode::Invalid:
      return 0;
    }
    return 0;
  }();
  if (Value.ExpressionOpcode == GenericConstantExpressionOpcode::GetElementPtr) {
    if (Value.Elements.empty() ||
        resolvedType(Value.Elements.front().Type).Kind != GenericConstantTypeKind::Pointer ||
        resolvedType(Value.Type).Kind != GenericConstantTypeKind::Pointer) {
      return fail<GenericConstantData>("getelementptr constant has no base pointer");
    }
  } else if (Value.Elements.size() != RequiredOperands) {
    return fail<GenericConstantData>("constant expression has the wrong operand count");
  }
  auto Verification = Value.verify();
  if (!Verification)
    return fail<GenericConstantData>(Verification.error().message());
  return Value;
}

Expected<GenericConstantData> ConstantParser::parseValue(GenericConstantType Type,
                                                         std::size_t Depth) {
  if (auto Limit = accountNode(Depth); !Limit)
    return std::unexpected(Limit.error());
  skipSpace();

  const auto Marker = [&](GenericConstantKind Kind) -> Expected<GenericConstantData> {
    GenericConstantData Value;
    Value.Type = std::move(Type);
    Value.Kind = Kind;
    auto Verification = Value.verify();
    if (!Verification)
      return fail<GenericConstantData>(Verification.error().message());
    return Value;
  };
  if (consumeWord("zeroinitializer"))
    return Marker(GenericConstantKind::ZeroInitializer);
  if (consumeWord("undef"))
    return Marker(GenericConstantKind::Undef);
  if (consumeWord("poison"))
    return Marker(GenericConstantKind::Poison);
  if (consumeWord("null"))
    return Marker(GenericConstantKind::NullPointer);

  const GenericConstantType &EffectiveType = resolvedType(Type);

  if (consumeWord("c")) {
    auto Bytes = parseByteString();
    if (!Bytes)
      return std::unexpected(Bytes.error());
    return GenericConstantData::byteString(std::move(Type), std::move(*Bytes));
  }

  if (EffectiveType.Kind == GenericConstantTypeKind::Array)
    return parseAggregate(std::move(Type), '[', ']', false, Depth + 1);
  if (EffectiveType.Kind == GenericConstantTypeKind::Struct)
    return parseAggregate(std::move(Type), EffectiveType.Packed ? '<' : '{', '}',
                          EffectiveType.Packed, Depth + 1);
  if (EffectiveType.Kind == GenericConstantTypeKind::Vector) {
    if (consumeWord("splat"))
      return parseExpression(std::move(Type), "splat", Depth + 1);
    skipSpace();
    if (Position < Text.size() && Text[Position] == '<')
      return parseAggregate(std::move(Type), '<', '>', false, Depth + 1);
  }

  skipSpace();
  if (Position < Text.size() && Text[Position] == '@') {
    auto Name = parseName('@');
    if (!Name)
      return std::unexpected(Name.error());
    GenericConstantData Value;
    Value.Type = std::move(Type);
    Value.Kind = GenericConstantKind::GlobalAddress;
    Value.Symbol = std::move(Name->Value);
    Value.NumericSymbol = Name->Numeric;
    auto Verification = Value.verify();
    if (!Verification)
      return fail<GenericConstantData>(Verification.error().message());
    return Value;
  }

  if (consumeWord("blockaddress")) {
    if (auto Open = expect('(', "'(' after blockaddress"); !Open)
      return std::unexpected(Open.error());
    auto Function = parseName('@');
    if (!Function)
      return std::unexpected(Function.error());
    if (auto Comma = expect(',', "',' in blockaddress"); !Comma)
      return std::unexpected(Comma.error());
    auto Block = parseName('%');
    if (!Block)
      return std::unexpected(Block.error());
    if (auto Close = expect(')', "')' after blockaddress"); !Close)
      return std::unexpected(Close.error());
    GenericConstantData Value;
    Value.Type = std::move(Type);
    Value.Kind = GenericConstantKind::BlockAddress;
    Value.Symbol = std::move(Function->Value);
    Value.NumericSymbol = Function->Numeric;
    Value.Block = std::move(Block->Value);
    Value.NumericBlock = Block->Numeric;
    auto Verification = Value.verify();
    if (!Verification)
      return fail<GenericConstantData>(Verification.error().message());
    return Value;
  }

  for (const auto &[Keyword, Kind] :
       {std::pair{std::string_view{"dso_local_equivalent"},
                  GenericConstantKind::DSOLocalEquivalent},
        std::pair{std::string_view{"no_cfi"}, GenericConstantKind::NoCFI}}) {
    if (!consumeWord(Keyword))
      continue;
    auto Name = parseName('@');
    if (!Name)
      return std::unexpected(Name.error());
    GenericConstantData Value;
    Value.Type = std::move(Type);
    Value.Kind = Kind;
    Value.Symbol = std::move(Name->Value);
    Value.NumericSymbol = Name->Numeric;
    auto Verification = Value.verify();
    if (!Verification)
      return fail<GenericConstantData>(Verification.error().message());
    return Value;
  }

  if (consumeWord("ptrauth")) {
    if (auto Open = expect('(', "'(' after ptrauth"); !Open)
      return std::unexpected(Open.error());
    GenericConstantData Value;
    Value.Type = std::move(Type);
    Value.Kind = GenericConstantKind::PointerAuth;
    while (true) {
      auto Operand = parseTypedValue(Depth + 1);
      if (!Operand)
        return std::unexpected(Operand.error());
      Value.Elements.push_back(std::move(*Operand));
      if (consume(')'))
        break;
      if (auto Comma = expect(',', "',' in ptrauth constant"); !Comma)
        return std::unexpected(Comma.error());
    }
    auto Verification = Value.verify();
    if (!Verification)
      return fail<GenericConstantData>(Verification.error().message());
    return Value;
  }

  const auto Word = peekWord();
  if (Word) {
    constexpr std::array ExpressionNames{
        std::string_view{"trunc"},
        std::string_view{"bitcast"},
        std::string_view{"addrspacecast"},
        std::string_view{"inttoptr"},
        std::string_view{"ptrtoaddr"},
        std::string_view{"ptrtoint"},
        std::string_view{"add"},
        std::string_view{"sub"},
        std::string_view{"xor"},
        std::string_view{"getelementptr"},
        std::string_view{"shufflevector"},
        std::string_view{"insertelement"},
        std::string_view{"extractelement"},
    };
    if (std::ranges::find(ExpressionNames, *Word) != ExpressionNames.end()) {
      Position += Word->size();
      return parseExpression(std::move(Type), *Word, Depth + 1);
    }
  }

  if (EffectiveType.Kind == GenericConstantTypeKind::Floating)
    return parseFloatingValue(std::move(Type), Depth + 1);
  if (EffectiveType.Kind == GenericConstantTypeKind::Integer ||
      EffectiveType.Kind == GenericConstantTypeKind::Byte) {
    auto Token = parseValueToken();
    if (!Token)
      return std::unexpected(Token.error());
    std::string_view Spelling = *Token;
    if (Spelling == "true")
      Spelling = "1";
    else if (Spelling == "false")
      Spelling = "0";
    const std::uint32_t BitWidth =
        EffectiveType.Kind == GenericConstantTypeKind::Byte ? 8U : EffectiveType.BitWidth;
    const bool IsExplicitHex = Spelling.starts_with("u0x") || Spelling.starts_with("s0x");
    auto Integer = IsExplicitHex ? APInt::parseLLVMHex(BitWidth, Spelling)
                                 : APInt::parseDecimal(BitWidth, Spelling);
    if (!Integer)
      return fail<GenericConstantData>(Integer.error().message());
    GenericConstantData Value;
    Value.Type = std::move(Type);
    Value.Kind = GenericConstantKind::Integer;
    Value.IntegerValue = std::move(*Integer);
    return Value;
  }
  return fail<GenericConstantData>("constant value is not valid for its LLVM IR type");
}

Expected<GenericConstantData> ConstantParser::parse() {
  if (Text.empty())
    return fail<GenericConstantData>("constant text is empty");
  auto Value = parseTypedValue(0);
  if (!Value)
    return Value;
  if (!atEnd())
    return fail<GenericConstantData>("unexpected trailing text");
  auto Verification = Value->verify();
  if (!Verification)
    return fail<GenericConstantData>(Verification.error().message());
  return Value;
}

Expected<GenericConstantType> ConstantParser::parseTypeOnly() {
  if (Text.empty())
    return fail<GenericConstantType>("type text is empty");
  auto Type = parseType(0);
  if (!Type)
    return Type;
  if (!atEnd())
    return fail<GenericConstantType>("unexpected trailing text after LLVM type");
  auto Verification = Type->verify();
  if (!Verification)
    return fail<GenericConstantType>(Verification.error().message());
  return Type;
}

[[nodiscard]] bool canUseBareName(std::string_view Name) noexcept {
  if (Name.empty() || !isIdentifierStart(Name.front()))
    return false;
  return std::ranges::all_of(Name.substr(1), isIdentifierBody);
}

[[nodiscard]] std::string printName(char Prefix, std::string_view Name, bool Numeric) {
  std::string Result(1, Prefix);
  if (Numeric || canUseBareName(Name)) {
    Result.append(Name);
    return Result;
  }
  static constexpr std::string_view HexDigits = "0123456789ABCDEF";
  Result.push_back('"');
  for (const char RawCharacter : Name) {
    const auto Character = static_cast<unsigned char>(RawCharacter);
    if (Character >= 0x20U && Character <= 0x7eU && Character != '"' && Character != '\\') {
      Result.push_back(static_cast<char>(Character));
      continue;
    }
    Result.push_back('\\');
    Result.push_back(HexDigits[Character >> 4U]);
    Result.push_back(HexDigits[Character & 0xfU]);
  }
  Result.push_back('"');
  return Result;
}

[[nodiscard]] Expected<std::string> printType(const GenericConstantType &Type);

[[nodiscard]] Expected<std::string> printTypeList(std::span<const GenericConstantType> Types,
                                                  std::string_view Open, std::string_view Close) {
  std::string Result(Open);
  for (std::size_t Index = 0; Index < Types.size(); ++Index) {
    if (Index != 0)
      Result.append(", ");
    auto Element = printType(Types[Index]);
    if (!Element)
      return std::unexpected(Element.error());
    Result.append(*Element);
  }
  Result.append(Close);
  return Result;
}

Expected<std::string> printType(const GenericConstantType &Type) {
  auto Verification = Type.verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  switch (Type.Kind) {
  case GenericConstantTypeKind::Invalid:
    break;
  case GenericConstantTypeKind::Void:
    return std::string("void");
  case GenericConstantTypeKind::Label:
    return std::string("label");
  case GenericConstantTypeKind::Token:
    return std::string("token");
  case GenericConstantTypeKind::Metadata:
    return std::string("metadata");
  case GenericConstantTypeKind::Integer:
    return "i" + std::to_string(Type.BitWidth);
  case GenericConstantTypeKind::Byte:
    return std::string("byte");
  case GenericConstantTypeKind::Floating:
    switch (Type.Semantics) {
    case FloatSemantics::IEEEHalf:
      return std::string("half");
    case FloatSemantics::BFloat:
      return std::string("bfloat");
    case FloatSemantics::IEEESingle:
      return std::string("float");
    case FloatSemantics::IEEEDouble:
      return std::string("double");
    case FloatSemantics::IEEEQuad:
      return std::string("fp128");
    case FloatSemantics::PPCDoubleDouble:
      return std::string("ppc_fp128");
    case FloatSemantics::X87DoubleExtended:
      return std::string("x86_fp80");
    default:
      return makeUnexpected(ErrorCode::Unsupported,
                            "floating semantics has no LLVM IR type spelling");
    }
  case GenericConstantTypeKind::Pointer:
    if (Type.AddressSpace == 0)
      return std::string("ptr");
    return "ptr addrspace(" + std::to_string(Type.AddressSpace) + ")";
  case GenericConstantTypeKind::Array: {
    auto Element = printType(Type.Elements.front());
    if (!Element)
      return std::unexpected(Element.error());
    return "[" + std::to_string(Type.ElementCount) + " x " + *Element + "]";
  }
  case GenericConstantTypeKind::Vector: {
    auto Element = printType(Type.Elements.front());
    if (!Element)
      return std::unexpected(Element.error());
    std::string Result = "<";
    if (Type.Scalable)
      Result.append("vscale x ");
    Result.append(std::to_string(Type.ElementCount));
    Result.append(" x ");
    Result.append(*Element);
    Result.push_back('>');
    return Result;
  }
  case GenericConstantTypeKind::Struct:
    return printTypeList(Type.Elements, Type.Packed ? "<{ " : "{ ", Type.Packed ? " }>" : " }");
  case GenericConstantTypeKind::Function: {
    auto Return = printType(Type.Elements.front());
    if (!Return)
      return std::unexpected(Return.error());
    std::string Result = *Return + " (";
    for (std::size_t Index = 1; Index < Type.Elements.size(); ++Index) {
      if (Index != 1)
        Result.append(", ");
      auto Parameter = printType(Type.Elements[Index]);
      if (!Parameter)
        return std::unexpected(Parameter.error());
      Result.append(*Parameter);
    }
    if (Type.VarArg) {
      if (Type.Elements.size() != 1)
        Result.append(", ");
      Result.append("...");
    }
    Result.push_back(')');
    return Result;
  }
  case GenericConstantTypeKind::Named:
    return printName('%', Type.Name, Type.NumericName);
  }
  return makeUnexpected(ErrorCode::VerificationError, "generic constant type is invalid");
}

[[nodiscard]] std::string_view
expressionOpcodeName(GenericConstantExpressionOpcode Opcode) noexcept {
  switch (Opcode) {
  case GenericConstantExpressionOpcode::Trunc:
    return "trunc";
  case GenericConstantExpressionOpcode::BitCast:
    return "bitcast";
  case GenericConstantExpressionOpcode::AddressSpaceCast:
    return "addrspacecast";
  case GenericConstantExpressionOpcode::IntToPtr:
    return "inttoptr";
  case GenericConstantExpressionOpcode::PtrToAddr:
    return "ptrtoaddr";
  case GenericConstantExpressionOpcode::PtrToInt:
    return "ptrtoint";
  case GenericConstantExpressionOpcode::Add:
    return "add";
  case GenericConstantExpressionOpcode::Sub:
    return "sub";
  case GenericConstantExpressionOpcode::Xor:
    return "xor";
  case GenericConstantExpressionOpcode::GetElementPtr:
    return "getelementptr";
  case GenericConstantExpressionOpcode::ShuffleVector:
    return "shufflevector";
  case GenericConstantExpressionOpcode::InsertElement:
    return "insertelement";
  case GenericConstantExpressionOpcode::ExtractElement:
    return "extractelement";
  case GenericConstantExpressionOpcode::Splat:
    return "splat";
  case GenericConstantExpressionOpcode::Invalid:
    return {};
  }
  return {};
}

[[nodiscard]] Expected<std::string> printValue(const GenericConstantData &Value, bool IncludeType);

[[nodiscard]] Expected<void> appendTypedOperands(std::string &Output,
                                                 std::span<const GenericConstantData> Operands) {
  for (std::size_t Index = 0; Index < Operands.size(); ++Index) {
    if (Index != 0)
      Output.append(", ");
    auto Operand = printValue(Operands[Index], true);
    if (!Operand)
      return std::unexpected(Operand.error());
    Output.append(*Operand);
  }
  return {};
}

Expected<std::string> printValue(const GenericConstantData &Value, bool IncludeType) {
  auto Verification = Value.verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  std::string Result;
  if (IncludeType) {
    auto Type = printType(Value.Type);
    if (!Type)
      return std::unexpected(Type.error());
    Result.append(*Type);
    Result.push_back(' ');
  }

  switch (Value.Kind) {
  case GenericConstantKind::Invalid:
    break;
  case GenericConstantKind::Integer:
    if (!Value.IntegerValue)
      return makeUnexpected(ErrorCode::VerificationError,
                            "integer constant is missing its integer payload");
    if (resolvedType(Value.Type).Kind == GenericConstantTypeKind::Integer &&
        resolvedType(Value.Type).BitWidth == 1)
      Result.append(Value.IntegerValue.value().isZero() ? "false" : "true");
    else
      Result.append(Value.IntegerValue.value().toUnsignedDecimal());
    return Result;
  case GenericConstantKind::Floating:
    if (!Value.FloatingValue)
      return makeUnexpected(ErrorCode::VerificationError,
                            "floating constant is missing its floating payload");
    Result.append(Value.FloatingValue->toString());
    return Result;
  case GenericConstantKind::NullPointer:
    Result.append("null");
    return Result;
  case GenericConstantKind::ZeroInitializer:
    Result.append("zeroinitializer");
    return Result;
  case GenericConstantKind::Undef:
    Result.append("undef");
    return Result;
  case GenericConstantKind::Poison:
    Result.append("poison");
    return Result;
  case GenericConstantKind::Aggregate: {
    const GenericConstantType &EffectiveType = resolvedType(Value.Type);
    const bool Packed =
        EffectiveType.Kind == GenericConstantTypeKind::Struct && EffectiveType.Packed;
    Result.append(Packed                                                  ? "<{ "
                  : EffectiveType.Kind == GenericConstantTypeKind::Array  ? "[ "
                  : EffectiveType.Kind == GenericConstantTypeKind::Struct ? "{ "
                                                                          : "< ");
    auto Appended = appendTypedOperands(Result, Value.Elements);
    if (!Appended)
      return std::unexpected(Appended.error());
    Result.append(Packed                                                  ? " }>"
                  : EffectiveType.Kind == GenericConstantTypeKind::Array  ? " ]"
                  : EffectiveType.Kind == GenericConstantTypeKind::Struct ? " }"
                                                                          : " >");
    return Result;
  }
  case GenericConstantKind::ByteString: {
    static constexpr std::string_view Digits = "0123456789ABCDEF";
    Result.append("c\"");
    for (const std::byte Byte : Value.ByteValue) {
      const unsigned Character = std::to_integer<unsigned>(Byte);
      if (Character >= 0x20U && Character <= 0x7eU && Character != '"' && Character != '\\') {
        Result.push_back(static_cast<char>(Character));
      } else {
        Result.push_back('\\');
        Result.push_back(Digits[Character >> 4U]);
        Result.push_back(Digits[Character & 0xfU]);
      }
    }
    Result.push_back('"');
    return Result;
  }
  case GenericConstantKind::GlobalAddress:
    Result.append(printName('@', Value.Symbol, Value.NumericSymbol));
    return Result;
  case GenericConstantKind::BlockAddress:
    Result.append("blockaddress(");
    Result.append(printName('@', Value.Symbol, Value.NumericSymbol));
    Result.append(", ");
    Result.append(printName('%', Value.Block, Value.NumericBlock));
    Result.push_back(')');
    return Result;
  case GenericConstantKind::DSOLocalEquivalent:
    Result.append("dso_local_equivalent ");
    Result.append(printName('@', Value.Symbol, Value.NumericSymbol));
    return Result;
  case GenericConstantKind::NoCFI:
    Result.append("no_cfi ");
    Result.append(printName('@', Value.Symbol, Value.NumericSymbol));
    return Result;
  case GenericConstantKind::PointerAuth:
    Result.append("ptrauth (");
    if (auto Appended = appendTypedOperands(Result, Value.Elements); !Appended)
      return std::unexpected(Appended.error());
    Result.push_back(')');
    return Result;
  case GenericConstantKind::Expression: {
    const std::string_view Opcode = expressionOpcodeName(Value.ExpressionOpcode);
    if (Opcode.empty())
      return makeUnexpected(ErrorCode::VerificationError,
                            "constant expression has an invalid opcode");
    Result.append(Opcode);
    if (Value.ExpressionFlags.has(GenericConstantExpressionFlag::InBounds))
      Result.append(" inbounds");
    if (Value.ExpressionFlags.has(GenericConstantExpressionFlag::NoUnsignedSignedWrap))
      Result.append(" nusw");
    if (Value.ExpressionFlags.has(GenericConstantExpressionFlag::GEPNoUnsignedWrap) ||
        Value.ExpressionFlags.has(GenericConstantExpressionFlag::NoUnsignedWrap))
      Result.append(" nuw");
    if (Value.ExpressionFlags.has(GenericConstantExpressionFlag::NoSignedWrap))
      Result.append(" nsw");
    if (Value.InRangeStart) {
      if (!Value.InRangeEnd) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "constant expression is missing its inrange end");
      }
      const auto &InRangeStart = Value.InRangeStart.value();
      const auto &InRangeEnd = Value.InRangeEnd.value();
      Result.append(" inrange(");
      Result.append(std::to_string(std::bit_cast<std::int64_t>(InRangeStart.words()[0])));
      Result.append(", ");
      Result.append(std::to_string(std::bit_cast<std::int64_t>(InRangeEnd.words()[0])));
      Result.push_back(')');
    }
    Result.append(" (");
    if (Value.ExpressionOpcode == GenericConstantExpressionOpcode::GetElementPtr) {
      if (!Value.SourceElementType) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "getelementptr constant is missing its source element type");
      }
      auto SourceType = printType(Value.SourceElementType.value());
      if (!SourceType)
        return std::unexpected(SourceType.error());
      Result.append(*SourceType);
      Result.append(", ");
    }
    auto First = printValue(Value.Elements.front(), true);
    if (!First)
      return std::unexpected(First.error());
    Result.append(*First);
    const bool IsCast =
        Value.ExpressionOpcode == GenericConstantExpressionOpcode::Trunc ||
        Value.ExpressionOpcode == GenericConstantExpressionOpcode::BitCast ||
        Value.ExpressionOpcode == GenericConstantExpressionOpcode::AddressSpaceCast ||
        Value.ExpressionOpcode == GenericConstantExpressionOpcode::IntToPtr ||
        Value.ExpressionOpcode == GenericConstantExpressionOpcode::PtrToAddr ||
        Value.ExpressionOpcode == GenericConstantExpressionOpcode::PtrToInt;
    if (IsCast) {
      auto Destination = printType(Value.Type);
      if (!Destination)
        return std::unexpected(Destination.error());
      Result.append(" to ");
      Result.append(*Destination);
    } else {
      for (std::size_t Index = 1; Index < Value.Elements.size(); ++Index) {
        Result.append(", ");
        auto Operand = printValue(Value.Elements[Index], true);
        if (!Operand)
          return std::unexpected(Operand.error());
        Result.append(*Operand);
      }
    }
    Result.push_back(')');
    return Result;
  }
  }
  return makeUnexpected(ErrorCode::VerificationError, "generic constant value is invalid");
}

struct LayoutAlignment final {
  std::uint64_t ABI = 1;
  std::uint64_t Preferred = 1;
};

struct LayoutAlignmentEntry final {
  char Kind = '\0';
  std::uint32_t BitWidth = 0;
  LayoutAlignment Alignment;
};

struct LayoutPointerEntry final {
  std::uint32_t AddressSpace = 0;
  std::uint32_t SizeInBits = 0;
  std::uint32_t IndexSizeInBits = 0;
  LayoutAlignment Alignment;
  bool HasExternalState = false;
  bool HasUnstableRepresentation = false;
  bool NullIsAllOnes = false;
  std::string AddressSpaceName;
};

constexpr std::uint64_t MaximumDataLayoutSize = (std::uint64_t{1} << 24U) - 1U;
constexpr std::uint64_t MaximumDataLayoutAlignment = (std::uint64_t{1} << 16U) - 1U;

class DataLayoutInfo final {
public:
  [[nodiscard]] static Expected<DataLayoutInfo> parse(LLVMConstantSyntaxOptions Options);

  [[nodiscard]] bool isExplicit() const noexcept {
    return Explicit;
  }

  [[nodiscard]] Expected<LayoutPointerEntry> pointer(std::uint32_t AddressSpace) const;
  [[nodiscard]] LayoutAlignment integer(std::uint32_t BitWidth,
                                        std::uint64_t StoreSize) const noexcept;
  [[nodiscard]] LayoutAlignment floating(std::uint32_t BitWidth,
                                         std::uint64_t StoreSize) const noexcept;
  [[nodiscard]] LayoutAlignment vector(std::uint32_t BitWidth,
                                       std::uint64_t StoreSize) const noexcept;

  [[nodiscard]] LayoutAlignment aggregate() const noexcept {
    return Aggregate;
  }

private:
  [[nodiscard]] static Expected<std::uint64_t> parseUnsignedField(std::string_view Text,
                                                                  std::string_view Description);
  [[nodiscard]] static Expected<std::uint64_t> alignmentBytes(std::string_view Text, bool AllowZero,
                                                              std::string_view Description);
  [[nodiscard]] static std::vector<std::string_view> split(std::string_view Text, char Separator);

  void setAlignment(LayoutAlignmentEntry Entry);
  void setPointer(LayoutPointerEntry Entry);
  [[nodiscard]] LayoutAlignment natural(std::uint64_t StoreSize) const noexcept;
  [[nodiscard]] LayoutAlignment scalarAlignment(char Kind, std::uint32_t BitWidth,
                                                std::uint64_t StoreSize) const noexcept;

  bool Explicit = false;
  std::vector<LayoutAlignmentEntry> Alignments;
  std::vector<LayoutPointerEntry> Pointers;
  LayoutAlignment Aggregate{1, 8};
};

Expected<std::uint64_t> DataLayoutInfo::parseUnsignedField(std::string_view Text,
                                                           std::string_view Description) {
  std::uint64_t Value = 0;
  const auto Parsed = std::from_chars(Text.data(), Text.data() + Text.size(), Value, 10);
  if (Text.empty() || Parsed.ec != std::errc{} || Parsed.ptr != Text.data() + Text.size()) {
    return makeUnexpected(ErrorCode::ParseError,
                          "LLVM data layout has an invalid " + std::string(Description));
  }
  return Value;
}

Expected<std::uint64_t> DataLayoutInfo::alignmentBytes(std::string_view Text, bool AllowZero,
                                                       std::string_view Description) {
  auto Bits = parseUnsignedField(Text, Description);
  if (!Bits)
    return std::unexpected(Bits.error());
  if (*Bits == 0 && AllowZero)
    return 1;
  if (*Bits > MaximumDataLayoutAlignment || *Bits < 8 || (*Bits % 8U) != 0 ||
      !std::has_single_bit(*Bits)) {
    return makeUnexpected(ErrorCode::ParseError,
                          "LLVM data layout " + std::string(Description) +
                              " must be a nonzero power-of-two byte alignment");
  }
  return *Bits / 8U;
}

std::vector<std::string_view> DataLayoutInfo::split(std::string_view Text, char Separator) {
  std::vector<std::string_view> Parts;
  while (true) {
    const std::size_t Position = Text.find(Separator);
    Parts.push_back(Text.substr(0, Position));
    if (Position == std::string_view::npos)
      return Parts;
    Text.remove_prefix(Position + 1U);
  }
}

void DataLayoutInfo::setAlignment(LayoutAlignmentEntry Entry) {
  const auto Existing =
      std::ranges::find_if(Alignments, [&Entry](const LayoutAlignmentEntry &Candidate) {
        return Candidate.Kind == Entry.Kind && Candidate.BitWidth == Entry.BitWidth;
      });
  if (Existing == Alignments.end())
    Alignments.push_back(Entry);
  else
    *Existing = Entry;
}

void DataLayoutInfo::setPointer(LayoutPointerEntry Entry) {
  const auto Existing =
      std::ranges::find_if(Pointers, [&Entry](const LayoutPointerEntry &Candidate) {
        return Candidate.AddressSpace == Entry.AddressSpace;
      });
  if (Existing == Pointers.end())
    Pointers.push_back(Entry);
  else
    *Existing = Entry;
}

Expected<DataLayoutInfo> DataLayoutInfo::parse(LLVMConstantSyntaxOptions Options) {
  DataLayoutInfo Result;
  if (Options.DataLayout.empty())
    return Result;
  if (Options.DataLayout.size() > Options.MaximumTokenLength) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "LLVM data layout exceeds the configured token length limit");
  }
  Result.Explicit = true;
  Result.setPointer({.AddressSpace = 0,
                     .SizeInBits = 64,
                     .IndexSizeInBits = 64,
                     .Alignment = {.ABI = 8, .Preferred = 8},
                     .HasExternalState = false,
                     .HasUnstableRepresentation = false,
                     .NullIsAllOnes = false,
                     .AddressSpaceName = {}});
  for (const LayoutAlignmentEntry Entry : {
           LayoutAlignmentEntry{'i', 1, {1, 1}},
           LayoutAlignmentEntry{'i', 8, {1, 1}},
           LayoutAlignmentEntry{'i', 16, {2, 2}},
           LayoutAlignmentEntry{'i', 32, {4, 4}},
           LayoutAlignmentEntry{'i', 64, {4, 8}},
           LayoutAlignmentEntry{'f', 16, {2, 2}},
           LayoutAlignmentEntry{'f', 32, {4, 4}},
           LayoutAlignmentEntry{'f', 64, {8, 8}},
           LayoutAlignmentEntry{'f', 128, {16, 16}},
           LayoutAlignmentEntry{'v', 64, {8, 8}},
           LayoutAlignmentEntry{'v', 128, {16, 16}},
       }) {
    Result.setAlignment(Entry);
  }

  for (const std::string_view Component : split(Options.DataLayout, '-')) {
    if (Component.empty())
      return makeUnexpected(ErrorCode::ParseError, "LLVM data layout contains an empty component");
    if (Component.front() == 'p') {
      const std::size_t Colon = Component.find(':');
      if (Colon == std::string_view::npos)
        return makeUnexpected(ErrorCode::ParseError,
                              "LLVM pointer data layout component has no size");
      std::string_view AddressSpaceText = Component.substr(1, Colon - 1U);
      bool HasExternalState = false;
      bool HasUnstableRepresentation = false;
      bool NullIsAllOnes = false;
      bool HasExplicitNullValue = false;
      while (!AddressSpaceText.empty()) {
        const char Flag = AddressSpaceText.front();
        if (Flag == 'e') {
          HasExternalState = true;
        } else if (Flag == 'u') {
          HasUnstableRepresentation = true;
        } else if (Flag == 'z' || Flag == 'o') {
          if (HasExplicitNullValue) {
            return makeUnexpected(ErrorCode::ParseError,
                                  "LLVM pointer data layout has conflicting null-pointer flags");
          }
          HasExplicitNullValue = true;
          NullIsAllOnes = Flag == 'o';
        } else {
          break;
        }
        AddressSpaceText.remove_prefix(1);
      }

      std::uint64_t AddressSpace = 0;
      std::string AddressSpaceName;
      if (!AddressSpaceText.empty()) {
        std::size_t NumberLength = 0;
        while (NumberLength < AddressSpaceText.size() && AddressSpaceText[NumberLength] >= '0' &&
               AddressSpaceText[NumberLength] <= '9') {
          ++NumberLength;
        }
        if (NumberLength != 0) {
          auto Parsed =
              parseUnsignedField(AddressSpaceText.substr(0, NumberLength), "pointer address space");
          if (!Parsed)
            return std::unexpected(Parsed.error());
          AddressSpace = *Parsed;
          AddressSpaceText.remove_prefix(NumberLength);
        }
        if (!AddressSpaceText.empty()) {
          if (AddressSpaceText.size() <= 2 || AddressSpaceText.front() != '(' ||
              AddressSpaceText.back() != ')') {
            return makeUnexpected(ErrorCode::ParseError,
                                  "LLVM pointer address space or name is invalid");
          }
          AddressSpaceName = std::string(AddressSpaceText.substr(1, AddressSpaceText.size() - 2));
          if (AddressSpaceName.size() == 1 &&
              (AddressSpaceName.front() == 'P' || AddressSpaceName.front() == 'G' ||
               AddressSpaceName.front() == 'A')) {
            return makeUnexpected(ErrorCode::ParseError,
                                  "LLVM pointer address space uses a reserved name");
          }
        }
      }
      if (AddressSpace == 0 && (HasExternalState || HasUnstableRepresentation)) {
        return makeUnexpected(ErrorCode::ParseError,
                              "LLVM address space zero cannot be unstable or external");
      }
      if (!AddressSpaceName.empty() &&
          std::ranges::any_of(Result.Pointers,
                              [&AddressSpaceName](const LayoutPointerEntry &Entry) {
                                return Entry.AddressSpaceName == AddressSpaceName;
                              })) {
        return makeUnexpected(ErrorCode::ParseError,
                              "LLVM pointer address-space name is duplicated");
      }
      const auto Fields = split(Component.substr(Colon + 1U), ':');
      if (Fields.size() < 2 || Fields.size() > 4 || AddressSpace > MaximumDataLayoutSize) {
        return makeUnexpected(ErrorCode::ParseError,
                              "LLVM pointer data layout component has invalid fields");
      }
      auto Size = parseUnsignedField(Fields[0], "pointer size");
      auto ABI = alignmentBytes(Fields[1], false, "pointer ABI alignment");
      auto Preferred = Fields.size() >= 3
                           ? alignmentBytes(Fields[2], false, "pointer preferred alignment")
                           : ABI;
      if (!Size || !ABI || !Preferred || *Size == 0 || *Size > MaximumDataLayoutSize ||
          *Preferred < *ABI) {
        return makeUnexpected(ErrorCode::ParseError,
                              "LLVM pointer data layout component is invalid");
      }
      std::uint64_t IndexSize = *Size;
      if (Fields.size() == 4) {
        auto ParsedIndexSize = parseUnsignedField(Fields[3], "pointer index size");
        if (!ParsedIndexSize || *ParsedIndexSize == 0 || *ParsedIndexSize > MaximumDataLayoutSize ||
            *ParsedIndexSize > *Size) {
          return makeUnexpected(ErrorCode::ParseError, "LLVM pointer index size is invalid");
        }
        IndexSize = *ParsedIndexSize;
      }
      Result.setPointer({.AddressSpace = static_cast<std::uint32_t>(AddressSpace),
                         .SizeInBits = static_cast<std::uint32_t>(*Size),
                         .IndexSizeInBits = static_cast<std::uint32_t>(IndexSize),
                         .Alignment = {.ABI = *ABI, .Preferred = *Preferred},
                         .HasExternalState = HasExternalState,
                         .HasUnstableRepresentation = HasUnstableRepresentation,
                         .NullIsAllOnes = NullIsAllOnes,
                         .AddressSpaceName = std::move(AddressSpaceName)});
      continue;
    }

    if (Component == "ve")
      continue;

    if (Component.front() == 'i' || Component.front() == 'f' || Component.front() == 'v') {
      const std::size_t Colon = Component.find(':');
      if (Colon == std::string_view::npos)
        return makeUnexpected(ErrorCode::ParseError,
                              "LLVM scalar data layout component has no alignment");
      auto Width = parseUnsignedField(Component.substr(1, Colon - 1U), "type alignment bit width");
      const auto Fields = split(Component.substr(Colon + 1U), ':');
      if (!Width || *Width == 0 || *Width > MaximumDataLayoutSize || Fields.empty() ||
          Fields.size() > 2) {
        return makeUnexpected(ErrorCode::ParseError,
                              "LLVM scalar data layout component has invalid fields");
      }
      auto ABI = alignmentBytes(Fields[0], false, "type ABI alignment");
      auto Preferred =
          Fields.size() == 2 ? alignmentBytes(Fields[1], false, "type preferred alignment") : ABI;
      if (!ABI || !Preferred || *Preferred < *ABI)
        return makeUnexpected(ErrorCode::ParseError,
                              "LLVM scalar data layout alignment is invalid");
      Result.setAlignment(
          {Component.front(), static_cast<std::uint32_t>(*Width), {*ABI, *Preferred}});
      continue;
    }

    if (Component.starts_with('a')) {
      const auto Fields = split(Component.substr(1), ':');
      if (Fields.size() < 2 || Fields.size() > 3 || (!Fields[0].empty() && Fields[0] != "0")) {
        return makeUnexpected(ErrorCode::ParseError,
                              "LLVM aggregate data layout component has invalid fields");
      }
      auto ABI = alignmentBytes(Fields[1], true, "aggregate ABI alignment");
      auto Preferred = Fields.size() == 3
                           ? alignmentBytes(Fields[2], false, "aggregate preferred alignment")
                           : ABI;
      if (!ABI || !Preferred || *Preferred < *ABI)
        return makeUnexpected(ErrorCode::ParseError,
                              "LLVM aggregate data layout alignment is invalid");
      Result.Aggregate = {.ABI = *ABI, .Preferred = *Preferred};
      continue;
    }

    if (Component == "e" || Component == "E" || Component == "s" || Component == "ve")
      continue;

    if (Component.starts_with("ni:")) {
      const auto AddressSpaces = split(Component.substr(3), ':');
      if (AddressSpaces.empty()) {
        return makeUnexpected(ErrorCode::ParseError,
                              "LLVM non-integral address-space component is empty");
      }
      for (const std::string_view AddressSpace : AddressSpaces) {
        auto Parsed = parseUnsignedField(AddressSpace, "non-integral address space");
        if (!Parsed || *Parsed == 0 || *Parsed > MaximumDataLayoutSize) {
          return makeUnexpected(ErrorCode::ParseError,
                                "LLVM non-integral address space is invalid");
        }
      }
      continue;
    }

    if (Component.starts_with("m:")) {
      const std::string_view Mode = Component.substr(2);
      if (Mode.size() != 1 || (Mode.front() != 'e' && Mode.front() != 'l' && Mode.front() != 'o' &&
                               Mode.front() != 'm' && Mode.front() != 'w' && Mode.front() != 'x' &&
                               Mode.front() != 'a')) {
        return makeUnexpected(ErrorCode::ParseError, "LLVM mangling mode is invalid");
      }
      continue;
    }

    if (Component.starts_with('n')) {
      const auto Widths = split(Component.substr(1), ':');
      if (Widths.empty()) {
        return makeUnexpected(ErrorCode::ParseError,
                              "LLVM native integer-width component is empty");
      }
      for (const std::string_view Width : Widths) {
        auto Parsed = parseUnsignedField(Width, "native integer bit width");
        if (!Parsed || *Parsed == 0 || *Parsed > MaximumDataLayoutSize) {
          return makeUnexpected(ErrorCode::ParseError, "LLVM native integer bit width is invalid");
        }
      }
      continue;
    }

    if (Component.starts_with('S')) {
      auto AlignmentValue = alignmentBytes(Component.substr(1), false, "stack natural alignment");
      if (!AlignmentValue)
        return std::unexpected(AlignmentValue.error());
      continue;
    }

    if (Component.starts_with('F')) {
      const std::string_view Rest = Component.substr(1);
      if (Rest.size() < 2 || (Rest.front() != 'i' && Rest.front() != 'n')) {
        return makeUnexpected(ErrorCode::ParseError,
                              "LLVM function-pointer alignment component is invalid");
      }
      auto AlignmentValue = alignmentBytes(Rest.substr(1), false, "function-pointer ABI alignment");
      if (!AlignmentValue)
        return std::unexpected(AlignmentValue.error());
      continue;
    }

    if (Component.starts_with('P') || Component.starts_with('G') || Component.starts_with('A')) {
      auto AddressSpace = parseUnsignedField(Component.substr(1), "default address space");
      if (!AddressSpace || *AddressSpace > MaximumDataLayoutSize) {
        return makeUnexpected(ErrorCode::ParseError, "LLVM default address space is invalid");
      }
      continue;
    }

    return makeUnexpected(ErrorCode::ParseError, "unsupported LLVM data layout component '" +
                                                     std::string(Component) + "'");
  }
  return Result;
}

LayoutAlignment DataLayoutInfo::natural(std::uint64_t StoreSize) const noexcept {
  if (StoreSize == 0)
    return {1, 1};
  constexpr std::uint64_t MaximumPowerOfTwo = std::uint64_t{1} << 63U;
  const std::uint64_t LimitedSize =
      std::min<std::uint64_t>(StoreSize, Explicit ? MaximumPowerOfTwo : 16U);
  const std::uint64_t Alignment = std::bit_ceil(LimitedSize);
  return {Alignment, Alignment};
}

LayoutAlignment DataLayoutInfo::scalarAlignment(char Kind, std::uint32_t BitWidth,
                                                std::uint64_t StoreSize) const noexcept {
  if (!Explicit)
    return natural(StoreSize);
  const LayoutAlignmentEntry *Selected = nullptr;
  for (const LayoutAlignmentEntry &Entry : Alignments) {
    if (Entry.Kind != Kind)
      continue;
    if (Kind == 'i') {
      const bool BetterMatch =
          Selected == nullptr ||
          (Entry.BitWidth >= BitWidth && Entry.BitWidth < Selected->BitWidth) ||
          (Selected->BitWidth < BitWidth && Entry.BitWidth > Selected->BitWidth);
      if (BetterMatch)
        Selected = &Entry;
    } else if (Entry.BitWidth == BitWidth) {
      Selected = &Entry;
      break;
    }
  }
  return Selected == nullptr ? natural(StoreSize) : Selected->Alignment;
}

LayoutAlignment DataLayoutInfo::integer(std::uint32_t BitWidth,
                                        std::uint64_t StoreSize) const noexcept {
  return scalarAlignment('i', BitWidth, StoreSize);
}

LayoutAlignment DataLayoutInfo::floating(std::uint32_t BitWidth,
                                         std::uint64_t StoreSize) const noexcept {
  return scalarAlignment('f', BitWidth, StoreSize);
}

LayoutAlignment DataLayoutInfo::vector(std::uint32_t BitWidth,
                                       std::uint64_t StoreSize) const noexcept {
  return scalarAlignment('v', BitWidth, StoreSize);
}

Expected<LayoutPointerEntry> DataLayoutInfo::pointer(std::uint32_t AddressSpace) const {
  if (!Explicit) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "pointer size is required to lay out pointer constants");
  }
  auto Entry = std::ranges::find_if(Pointers, [AddressSpace](const LayoutPointerEntry &Candidate) {
    return Candidate.AddressSpace == AddressSpace;
  });
  if (Entry == Pointers.end()) {
    Entry = std::ranges::find_if(
        Pointers, [](const LayoutPointerEntry &Candidate) { return Candidate.AddressSpace == 0; });
  }
  if (Entry == Pointers.end())
    return makeUnexpected(ErrorCode::InvalidArgument, "LLVM data layout has no pointer layout");
  return *Entry;
}

struct TypeLayout final {
  std::uint64_t StoreSize = 0;
  std::uint64_t AllocationSize = 0;
  std::uint64_t ABIAlignment = 1;
  std::uint64_t PreferredAlignment = 1;
};

[[nodiscard]] Expected<TypeLayout> layoutType(const GenericConstantType &Type,
                                              LLVMConstantSyntaxOptions Options,
                                              const DataLayoutInfo &Layout,
                                              std::vector<std::string> &NamedTypeStack,
                                              std::size_t Depth) {
  if (Depth > Options.MaximumDepth)
    return makeUnexpected(ErrorCode::Unsupported, "LLVM type exceeds the layout depth limit");
  auto Verification = Type.verify();
  if (!Verification)
    return std::unexpected(Verification.error());

  const auto ScalarLayout = [&Layout](std::uint64_t StoreSize,
                                      LayoutAlignment TypeAlignment) -> Expected<TypeLayout> {
    std::uint64_t AllocationSize = StoreSize;
    if (Layout.isExplicit()) {
      auto Aligned = alignTo(StoreSize, TypeAlignment.ABI);
      if (!Aligned)
        return std::unexpected(Aligned.error());
      AllocationSize = *Aligned;
    }
    return TypeLayout{StoreSize, AllocationSize, TypeAlignment.ABI, TypeAlignment.Preferred};
  };

  switch (Type.Kind) {
  case GenericConstantTypeKind::Integer:
  case GenericConstantTypeKind::Byte: {
    const std::uint32_t Bits = Type.Kind == GenericConstantTypeKind::Byte ? 8U : Type.BitWidth;
    const std::uint64_t StoreSize = (static_cast<std::uint64_t>(Bits) + 7U) / 8U;
    return ScalarLayout(StoreSize, Layout.integer(Bits, StoreSize));
  }
  case GenericConstantTypeKind::Floating: {
    const std::uint32_t Bits = getFloatSemanticsBitWidth(Type.Semantics).value_or(0);
    const std::uint64_t StoreSize = (static_cast<std::uint64_t>(Bits) + 7U) / 8U;
    return ScalarLayout(StoreSize, Layout.floating(Bits, StoreSize));
  }
  case GenericConstantTypeKind::Pointer: {
    auto Pointer = Layout.pointer(Type.AddressSpace);
    if (!Pointer)
      return std::unexpected(Pointer.error());
    const std::uint64_t StoreSize = (static_cast<std::uint64_t>(Pointer->SizeInBits) + 7U) / 8U;
    return ScalarLayout(StoreSize, Pointer->Alignment);
  }
  case GenericConstantTypeKind::Array: {
    auto Element = layoutType(Type.Elements.front(), Options, Layout, NamedTypeStack, Depth + 1);
    if (!Element)
      return std::unexpected(Element.error());
    auto Stride = alignTo(Element->AllocationSize, Element->ABIAlignment);
    if (!Stride)
      return std::unexpected(Stride.error());
    auto Size = checkedMultiply(*Stride, Type.ElementCount, "LLVM array allocation size");
    if (!Size)
      return std::unexpected(Size.error());
    return TypeLayout{*Size, *Size, Element->ABIAlignment, Element->PreferredAlignment};
  }
  case GenericConstantTypeKind::Vector: {
    if (Type.Scalable)
      return makeUnexpected(ErrorCode::Unsupported,
                            "scalable vector has no fixed constant-pool size");
    const GenericConstantType &ElementType = resolvedType(Type.Elements.front());
    std::uint64_t ElementBits = 0;
    if (ElementType.Kind == GenericConstantTypeKind::Integer)
      ElementBits = ElementType.BitWidth;
    else if (ElementType.Kind == GenericConstantTypeKind::Byte)
      ElementBits = 8;
    else if (ElementType.Kind == GenericConstantTypeKind::Floating)
      ElementBits = getFloatSemanticsBitWidth(ElementType.Semantics).value_or(0);
    else if (ElementType.Kind == GenericConstantTypeKind::Pointer) {
      auto Pointer = Layout.pointer(ElementType.AddressSpace);
      if (!Pointer)
        return std::unexpected(Pointer.error());
      ElementBits = Pointer->SizeInBits;
    } else {
      return makeUnexpected(ErrorCode::Unsupported,
                            "LLVM vector element type has no fixed bit size");
    }
    auto TotalBits = checkedMultiply(ElementBits, Type.ElementCount, "LLVM vector bit size");
    if (!TotalBits || *TotalBits > std::numeric_limits<std::uint64_t>::max() - 7U)
      return makeUnexpected(ErrorCode::Unsupported, "LLVM vector bit size is too large");
    const std::uint64_t StoreSize = (*TotalBits + 7U) / 8U;
    return ScalarLayout(StoreSize,
                        Layout.vector(static_cast<std::uint32_t>(std::min<std::uint64_t>(
                                          *TotalBits, std::numeric_limits<std::uint32_t>::max())),
                                      StoreSize));
  }
  case GenericConstantTypeKind::Struct: {
    if (Type.Packed) {
      std::uint64_t Size = 0;
      for (const GenericConstantType &MemberType : Type.Elements) {
        auto Member = layoutType(MemberType, Options, Layout, NamedTypeStack, Depth + 1);
        if (!Member)
          return std::unexpected(Member.error());
        auto Next = checkedAdd(Size, Member->AllocationSize, "LLVM packed struct allocation size");
        if (!Next)
          return std::unexpected(Next.error());
        Size = *Next;
      }
      return TypeLayout{Size, Size, 1, std::max<std::uint64_t>(Layout.aggregate().Preferred, 1)};
    }
    std::uint64_t Offset = 0;
    std::uint64_t ABIAlignment = std::max<std::uint64_t>(Layout.aggregate().ABI, 1);
    for (const GenericConstantType &MemberType : Type.Elements) {
      auto Member = layoutType(MemberType, Options, Layout, NamedTypeStack, Depth + 1);
      if (!Member)
        return std::unexpected(Member.error());
      auto AlignedOffset = alignTo(Offset, Member->ABIAlignment);
      if (!AlignedOffset)
        return std::unexpected(AlignedOffset.error());
      auto Next = checkedAdd(*AlignedOffset, Member->AllocationSize, "LLVM struct allocation size");
      if (!Next)
        return std::unexpected(Next.error());
      Offset = *Next;
      ABIAlignment = std::max(ABIAlignment, Member->ABIAlignment);
    }
    auto Size = alignTo(Offset, ABIAlignment);
    if (!Size)
      return std::unexpected(Size.error());
    return TypeLayout{*Size, *Size, ABIAlignment,
                      std::max(Layout.aggregate().Preferred, ABIAlignment)};
  }
  case GenericConstantTypeKind::Named: {
    if (std::ranges::find(NamedTypeStack, Type.Name) != NamedTypeStack.end()) {
      return makeUnexpected(ErrorCode::Unsupported,
                            "named LLVM type definition is recursively unsized");
    }
    NamedTypeStack.push_back(Type.Name);
    Expected<TypeLayout> Result =
        makeUnexpected(ErrorCode::Unsupported, "named LLVM type has no embedded type definition");
    if (Type.Elements.size() == 1) {
      Result = layoutType(Type.Elements.front(), Options, Layout, NamedTypeStack, Depth + 1);
    } else {
      const auto Definition = std::ranges::find_if(
          Options.NamedTypes, [&Type](const LLVMNamedTypeDefinition &Candidate) {
            return Candidate.Name == Type.Name && Candidate.NumericName == Type.NumericName;
          });
      if (Definition != Options.NamedTypes.end()) {
        auto Resolved = parseLLVMType(Definition->Definition, Options);
        if (!Resolved)
          Result = std::unexpected(Resolved.error());
        else
          Result = layoutType(*Resolved, Options, Layout, NamedTypeStack, Depth + 1);
      }
    }
    NamedTypeStack.pop_back();
    return Result;
  }
  case GenericConstantTypeKind::Invalid:
  case GenericConstantTypeKind::Void:
  case GenericConstantTypeKind::Label:
  case GenericConstantTypeKind::Token:
  case GenericConstantTypeKind::Metadata:
  case GenericConstantTypeKind::Function:
    return makeUnexpected(ErrorCode::Unsupported,
                          "LLVM type is not sized for a machine constant pool");
  }
  return makeUnexpected(ErrorCode::InternalError, "LLVM constant type layout is unhandled");
}

[[nodiscard]] Expected<TypeLayout> layoutType(const GenericConstantType &Type,
                                              LLVMConstantSyntaxOptions Options) {
  auto Layout = DataLayoutInfo::parse(Options);
  if (!Layout)
    return std::unexpected(Layout.error());
  std::vector<std::string> NamedTypeStack;
  return layoutType(Type, Options, *Layout, NamedTypeStack, 0);
}

} // namespace

Expected<GenericConstantData> parseLLVMConstant(std::string_view Text,
                                                LLVMConstantSyntaxOptions Options) {
  if (Options.MaximumDepth == 0 || Options.MaximumNodes == 0 || Options.MaximumTokenLength == 0) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "LLVM constant parser limits must be nonzero");
  }
  return ConstantParser(Text, Options).parse();
}

Expected<GenericConstantType> parseLLVMType(std::string_view Text,
                                            LLVMConstantSyntaxOptions Options) {
  if (Options.MaximumDepth == 0 || Options.MaximumNodes == 0 || Options.MaximumTokenLength == 0) {
    return makeUnexpected(ErrorCode::InvalidArgument, "LLVM type parser limits must be nonzero");
  }
  return ConstantParser(Text, Options).parseTypeOnly();
}

Expected<std::string> printLLVMConstant(const GenericConstantData &Constant) {
  return printValue(Constant, true);
}

Expected<std::uint64_t> llvmConstantStorageSize(const GenericConstantType &Type,
                                                LLVMConstantSyntaxOptions Options) {
  auto Layout = layoutType(Type, Options);
  if (!Layout)
    return std::unexpected(Layout.error());
  return Layout->AllocationSize;
}

Expected<Alignment> llvmConstantPreferredAlignment(const GenericConstantType &Type,
                                                   LLVMConstantSyntaxOptions Options) {
  auto Layout = layoutType(Type, Options);
  if (!Layout)
    return std::unexpected(Layout.error());
  return Alignment::create(Layout->PreferredAlignment);
}

Expected<std::uint32_t> llvmPointerSizeInBits(std::uint32_t AddressSpace,
                                              LLVMConstantSyntaxOptions Options) {
  auto Layout = DataLayoutInfo::parse(Options);
  if (!Layout)
    return std::unexpected(Layout.error());
  auto Pointer = Layout->pointer(AddressSpace);
  if (!Pointer)
    return std::unexpected(Pointer.error());
  return Pointer->SizeInBits;
}

} // namespace rcc::mir
