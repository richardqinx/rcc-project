// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/AsmParser/LLParser.h"

#include "rcc/ADT/APFloat.h"
#include "rcc/ADT/APInt.h"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rcc {
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
class ConstantParser final {
public:
  ConstantParser(std::string_view SourceText, LLParserOptions ParserOptions,
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
  LLParserOptions Options;
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
      std::ranges::find_if(Options.NamedTypes, [&Type](const LLNamedTypeDefinition &Candidate) {
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
  const GenericConstantType &EffectiveType = Type.resolved();
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
        Value.Elements.front().Type.resolved().Kind != GenericConstantTypeKind::Pointer ||
        Value.Type.resolved().Kind != GenericConstantTypeKind::Pointer) {
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

  const GenericConstantType &EffectiveType = Type.resolved();

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

} // namespace

Expected<GenericConstantData> parseLLVMConstant(std::string_view Text, LLParserOptions Options) {
  if (Options.MaximumDepth == 0 || Options.MaximumNodes == 0 || Options.MaximumTokenLength == 0) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "LLVM constant parser limits must be nonzero");
  }
  return ConstantParser(Text, Options).parse();
}

Expected<GenericConstantType> parseLLVMType(std::string_view Text, LLParserOptions Options) {
  if (Options.MaximumDepth == 0 || Options.MaximumNodes == 0 || Options.MaximumTokenLength == 0) {
    return makeUnexpected(ErrorCode::InvalidArgument, "LLVM type parser limits must be nonzero");
  }
  return ConstantParser(Text, Options).parseTypeOnly();
}

} // namespace rcc
