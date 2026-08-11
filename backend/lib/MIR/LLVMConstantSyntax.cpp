// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/MIR/LLVMConstantSyntax.h"

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

class BigUnsigned final {
public:
  BigUnsigned() = default;

  explicit BigUnsigned(std::uint64_t Value) {
    if (Value == 0)
      return;
    Limbs.push_back(static_cast<std::uint32_t>(Value));
    if ((Value >> 32U) != 0)
      Limbs.push_back(static_cast<std::uint32_t>(Value >> 32U));
  }

  [[nodiscard]] bool isZero() const noexcept {
    return Limbs.empty();
  }

  [[nodiscard]] bool isOdd() const noexcept {
    return !Limbs.empty() && (Limbs.front() & 1U) != 0;
  }

  [[nodiscard]] std::size_t bitWidth() const noexcept {
    if (Limbs.empty())
      return 0;
    std::uint32_t MostSignificant = Limbs.back();
    std::size_t Bits = 0;
    while (MostSignificant != 0) {
      ++Bits;
      MostSignificant >>= 1U;
    }
    return (Limbs.size() - 1U) * 32U + Bits;
  }

  [[nodiscard]] std::uint64_t word(std::size_t Index) const noexcept {
    const std::size_t First = Index * 2U;
    if (First >= Limbs.size())
      return 0;
    std::uint64_t Result = Limbs[First];
    if (First + 1U < Limbs.size())
      Result |= static_cast<std::uint64_t>(Limbs[First + 1U]) << 32U;
    return Result;
  }

  [[nodiscard]] int compare(const BigUnsigned &Other) const noexcept {
    if (Limbs.size() != Other.Limbs.size())
      return Limbs.size() < Other.Limbs.size() ? -1 : 1;
    for (std::size_t Index = Limbs.size(); Index != 0; --Index) {
      const std::uint32_t Left = Limbs[Index - 1U];
      const std::uint32_t Right = Other.Limbs[Index - 1U];
      if (Left != Right)
        return Left < Right ? -1 : 1;
    }
    return 0;
  }

  void multiplySmall(std::uint32_t Multiplier) {
    if (Multiplier == 0 || isZero()) {
      Limbs.clear();
      return;
    }
    std::uint64_t Carry = 0;
    for (std::uint32_t &Limb : Limbs) {
      const std::uint64_t Product = static_cast<std::uint64_t>(Limb) * Multiplier + Carry;
      Limb = static_cast<std::uint32_t>(Product);
      Carry = Product >> 32U;
    }
    if (Carry != 0)
      Limbs.push_back(static_cast<std::uint32_t>(Carry));
  }

  [[nodiscard]] std::uint32_t divideSmall(std::uint32_t Divisor) {
    std::uint64_t Remainder = 0;
    for (std::size_t Index = Limbs.size(); Index != 0; --Index) {
      const std::uint64_t Dividend = (Remainder << 32U) | Limbs[Index - 1U];
      Limbs[Index - 1U] = static_cast<std::uint32_t>(Dividend / Divisor);
      Remainder = Dividend % Divisor;
    }
    normalize();
    return static_cast<std::uint32_t>(Remainder);
  }

  void addSmall(std::uint32_t Value) {
    std::uint64_t Carry = Value;
    std::size_t Index = 0;
    while (Carry != 0 && Index < Limbs.size()) {
      const std::uint64_t Sum = static_cast<std::uint64_t>(Limbs[Index]) + Carry;
      Limbs[Index] = static_cast<std::uint32_t>(Sum);
      Carry = Sum >> 32U;
      ++Index;
    }
    if (Carry != 0)
      Limbs.push_back(static_cast<std::uint32_t>(Carry));
  }

  void setBit(std::size_t Bit) {
    const std::size_t Limb = Bit / 32U;
    if (Limb >= Limbs.size())
      Limbs.resize(Limb + 1U, 0);
    Limbs[Limb] |= std::uint32_t{1} << (Bit % 32U);
  }

  void clearBit(std::size_t Bit) noexcept {
    const std::size_t Limb = Bit / 32U;
    if (Limb >= Limbs.size())
      return;
    Limbs[Limb] &= ~(std::uint32_t{1} << (Bit % 32U));
    normalize();
  }

  void shiftRightOne() noexcept {
    std::uint32_t Carry = 0;
    for (std::size_t Index = Limbs.size(); Index != 0; --Index) {
      const std::uint32_t Current = Limbs[Index - 1U];
      Limbs[Index - 1U] = (Current >> 1U) | (Carry << 31U);
      Carry = Current & 1U;
    }
    normalize();
  }

  [[nodiscard]] std::size_t countrZero() const noexcept {
    std::size_t Result = 0;
    for (const std::uint32_t Limb : Limbs) {
      if (Limb == 0) {
        Result += 32U;
        continue;
      }
      return Result + static_cast<std::size_t>(std::countr_zero(Limb));
    }
    return Result;
  }

  void shiftRight(std::size_t Amount) noexcept {
    if (Amount == 0 || isZero())
      return;
    const std::size_t LimbOffset = Amount / 32U;
    if (LimbOffset >= Limbs.size()) {
      Limbs.clear();
      return;
    }
    if (LimbOffset != 0)
      Limbs.erase(Limbs.begin(), Limbs.begin() + static_cast<std::ptrdiff_t>(LimbOffset));
    const unsigned BitOffset = static_cast<unsigned>(Amount % 32U);
    if (BitOffset != 0) {
      std::uint32_t Carry = 0;
      for (std::size_t Index = Limbs.size(); Index != 0; --Index) {
        const std::uint32_t Current = Limbs[Index - 1U];
        Limbs[Index - 1U] = (Current >> BitOffset) | Carry;
        Carry = Current << (32U - BitOffset);
      }
    }
    normalize();
  }

  [[nodiscard]] std::string toDecimal() const {
    if (isZero())
      return "0";
    BigUnsigned Remaining = *this;
    std::string Digits;
    while (!Remaining.isZero())
      Digits.push_back(static_cast<char>('0' + Remaining.divideSmall(10)));
    std::ranges::reverse(Digits);
    return Digits;
  }

  [[nodiscard]] BigUnsigned shiftedLeft(std::size_t Amount) const {
    if (isZero() || Amount == 0)
      return *this;
    const std::size_t LimbOffset = Amount / 32U;
    const unsigned BitOffset = static_cast<unsigned>(Amount % 32U);
    BigUnsigned Result;
    Result.Limbs.assign(Limbs.size() + LimbOffset + (BitOffset == 0 ? 0U : 1U), 0);
    for (std::size_t Index = 0; Index < Limbs.size(); ++Index) {
      const std::uint64_t Shifted = static_cast<std::uint64_t>(Limbs[Index]) << BitOffset;
      Result.Limbs[Index + LimbOffset] |= static_cast<std::uint32_t>(Shifted);
      if (BitOffset != 0)
        Result.Limbs[Index + LimbOffset + 1U] |= static_cast<std::uint32_t>(Shifted >> 32U);
    }
    Result.normalize();
    return Result;
  }

  void subtract(const BigUnsigned &Other) noexcept {
    std::uint64_t Borrow = 0;
    for (std::size_t Index = 0; Index < Limbs.size(); ++Index) {
      const std::uint64_t Subtrahend =
          (Index < Other.Limbs.size() ? static_cast<std::uint64_t>(Other.Limbs[Index]) : 0) +
          Borrow;
      const std::uint64_t Current = Limbs[Index];
      Limbs[Index] = static_cast<std::uint32_t>(Current - Subtrahend);
      Borrow = Current < Subtrahend ? 1U : 0U;
    }
    normalize();
  }

private:
  friend int compareToShifted(const BigUnsigned &Left, const BigUnsigned &Right,
                              std::size_t RightShift) noexcept;

  void normalize() noexcept {
    while (!Limbs.empty() && Limbs.back() == 0)
      Limbs.pop_back();
  }

  std::vector<std::uint32_t> Limbs;
};

[[nodiscard]] int compareToShifted(const BigUnsigned &Left, const BigUnsigned &Right,
                                   std::size_t RightShift) noexcept {
  if (Right.isZero())
    return Left.isZero() ? 0 : 1;
  if (Left.isZero())
    return -1;

  const std::size_t LeftBits = Left.bitWidth();
  const std::size_t RightBits = Right.bitWidth();
  if (RightShift > std::numeric_limits<std::size_t>::max() - RightBits)
    return -1;
  const std::size_t ShiftedBits = RightBits + RightShift;
  if (LeftBits != ShiftedBits)
    return LeftBits < ShiftedBits ? -1 : 1;

  const std::size_t LimbOffset = RightShift / 32U;
  const unsigned BitOffset = static_cast<unsigned>(RightShift % 32U);
  const std::size_t WordCount = (ShiftedBits + 31U) / 32U;
  for (std::size_t Index = WordCount; Index != 0; --Index) {
    const std::size_t Destination = Index - 1U;
    std::uint32_t ShiftedWord = 0;
    if (Destination >= LimbOffset) {
      const std::size_t Source = Destination - LimbOffset;
      if (Source < Right.Limbs.size())
        ShiftedWord = static_cast<std::uint32_t>(Right.Limbs[Source] << BitOffset);
      if (BitOffset != 0 && Source != 0 && Source - 1U < Right.Limbs.size())
        ShiftedWord |= Right.Limbs[Source - 1U] >> (32U - BitOffset);
    }
    const std::uint32_t LeftWord = Destination < Left.Limbs.size() ? Left.Limbs[Destination] : 0;
    if (LeftWord != ShiftedWord)
      return LeftWord < ShiftedWord ? -1 : 1;
  }
  return 0;
}

[[nodiscard]] bool parseHexWords(std::string_view Digits, std::uint32_t BitWidth,
                                 std::vector<std::uint64_t> &Words) {
  if (Digits.empty() || Digits.size() > (static_cast<std::size_t>(BitWidth) + 3U) / 4U)
    return false;
  Words.assign((BitWidth + 63U) / 64U, 0);
  std::size_t Nibble = 0;
  for (auto Iterator = Digits.rbegin(); Iterator != Digits.rend(); ++Iterator, ++Nibble) {
    if (!isHexDigit(*Iterator))
      return false;
    Words[Nibble / 16U] |= static_cast<std::uint64_t>(hexDigitValue(*Iterator))
                           << ((Nibble % 16U) * 4U);
  }
  return (BitWidth % 64U) == 0 || (Words.back() >> (BitWidth % 64U)) == 0;
}

[[nodiscard]] bool parseLegacyWideFloatHexWords(std::string_view Digits,
                                                std::vector<std::uint64_t> &Words) {
  if (Digits.empty() || Digits.size() > 32U || !std::ranges::all_of(Digits, isHexDigit))
    return false;
  Words.assign(2, 0);
  const auto ParseWord = [](std::string_view Text, std::uint64_t &Word) {
    const auto Parsed = std::from_chars(Text.data(), Text.data() + Text.size(), Word, 16);
    return Parsed.ec == std::errc{} && Parsed.ptr == Text.data() + Text.size();
  };
  const std::size_t FirstWordLength = std::min<std::size_t>(Digits.size(), 16U);
  if (!ParseWord(Digits.substr(0, FirstWordLength), Words[0]))
    return false;
  if (Digits.size() == FirstWordLength)
    return true;
  return ParseWord(Digits.substr(FirstWordLength), Words[1]);
}

enum class SpecialFloatLiteralKind : std::uint8_t {
  Infinity,
  QuietNaN,
  SignalingNaN,
};

struct SpecialFloatLiteral final {
  SpecialFloatLiteralKind Kind = SpecialFloatLiteralKind::Infinity;
  bool Negative = false;
  std::array<std::uint64_t, 2> Payload{};
};

[[nodiscard]] Expected<std::optional<SpecialFloatLiteral>>
parseSpecialFloatLiteral(std::string_view Token) {
  if (!Token.starts_with('+') && !Token.starts_with('-'))
    return std::optional<SpecialFloatLiteral>{};

  SpecialFloatLiteral Result;
  Result.Negative = Token.front() == '-';
  Token.remove_prefix(1);
  if (Token == "inf")
    return Result;
  if (Token == "qnan") {
    Result.Kind = SpecialFloatLiteralKind::QuietNaN;
    return Result;
  }

  if (!Token.starts_with("nan") && !Token.starts_with("snan"))
    return std::optional<SpecialFloatLiteral>{};

  const bool IsSignaling = Token.starts_with("snan");
  const std::size_t NameLength = IsSignaling ? 4U : 3U;
  const std::string_view Suffix = Token.substr(NameLength);
  Result.Kind =
      IsSignaling ? SpecialFloatLiteralKind::SignalingNaN : SpecialFloatLiteralKind::QuietNaN;
  if (Suffix.size() < 5 || Suffix.front() != '(' || Suffix.back() != ')' ||
      !Suffix.substr(1).starts_with("0x")) {
    return makeUnexpected(ErrorCode::ParseError, "invalid floating-point NaN payload");
  }

  const std::string_view Digits = Suffix.substr(3, Suffix.size() - 4U);
  if (Digits.empty() || !std::ranges::all_of(Digits, isHexDigit)) {
    return makeUnexpected(ErrorCode::ParseError, "invalid floating-point NaN payload");
  }
  std::size_t Nibble = 0;
  for (auto Iterator = Digits.rbegin(); Iterator != Digits.rend(); ++Iterator, ++Nibble) {
    if (Nibble >= 32U)
      break;
    Result.Payload[Nibble / 16U] |= static_cast<std::uint64_t>(hexDigitValue(*Iterator))
                                    << ((Nibble % 16U) * 4U);
  }
  return Result;
}

void setFloatBit(std::vector<std::uint64_t> &Words, std::uint32_t Bit) {
  Words[Bit / 64U] |= std::uint64_t{1} << (Bit % 64U);
}

void clearFloatBit(std::vector<std::uint64_t> &Words, std::uint32_t Bit) {
  Words[Bit / 64U] &= ~(std::uint64_t{1} << (Bit % 64U));
}

[[nodiscard]] bool hasFloatBit(std::span<const std::uint64_t> Words, std::uint32_t Bit) noexcept {
  return (Words[Bit / 64U] & (std::uint64_t{1} << (Bit % 64U))) != 0;
}

void copyNaNPayload(std::vector<std::uint64_t> &Words, const std::array<std::uint64_t, 2> &Payload,
                    std::uint32_t PayloadBitWidth) {
  for (std::uint32_t Bit = 0; Bit < PayloadBitWidth; ++Bit) {
    if ((Payload[Bit / 64U] & (std::uint64_t{1} << (Bit % 64U))) != 0)
      setFloatBit(Words, Bit);
  }
}

void makeBinarySpecialFloat(std::vector<std::uint64_t> &Words, std::uint32_t BitWidth,
                            std::uint32_t ExponentBitWidth, const SpecialFloatLiteral &Literal) {
  const std::uint32_t FractionBitWidth = BitWidth - ExponentBitWidth - 1U;
  if (Literal.Negative)
    setFloatBit(Words, BitWidth - 1U);
  for (std::uint32_t Bit = FractionBitWidth; Bit < FractionBitWidth + ExponentBitWidth; ++Bit) {
    setFloatBit(Words, Bit);
  }
  if (Literal.Kind == SpecialFloatLiteralKind::Infinity)
    return;

  copyNaNPayload(Words, Literal.Payload, FractionBitWidth);
  const std::uint32_t QuietNaNBit = FractionBitWidth - 1U;
  if (Literal.Kind == SpecialFloatLiteralKind::QuietNaN) {
    setFloatBit(Words, QuietNaNBit);
    return;
  }
  clearFloatBit(Words, QuietNaNBit);
  bool HasPayload = false;
  for (std::uint32_t Bit = 0; Bit < FractionBitWidth; ++Bit)
    HasPayload = HasPayload || hasFloatBit(Words, Bit);
  if (!HasPayload)
    setFloatBit(Words, QuietNaNBit - 1U);
}

[[nodiscard]] Expected<std::vector<std::uint64_t>>
makeSpecialFloatWords(FloatSemantics Semantics, const SpecialFloatLiteral &Literal) {
  const auto BitWidth = getFloatSemanticsBitWidth(Semantics);
  if (!BitWidth)
    return makeUnexpected(ErrorCode::InvalidArgument, "floating-point semantics is unknown");
  std::vector<std::uint64_t> Words((*BitWidth + 63U) / 64U, 0);
  switch (Semantics) {
  case FloatSemantics::IEEEHalf:
    makeBinarySpecialFloat(Words, *BitWidth, 5, Literal);
    return Words;
  case FloatSemantics::BFloat:
  case FloatSemantics::IEEESingle:
    makeBinarySpecialFloat(Words, *BitWidth, 8, Literal);
    return Words;
  case FloatSemantics::IEEEDouble:
    makeBinarySpecialFloat(Words, *BitWidth, 11, Literal);
    return Words;
  case FloatSemantics::IEEEQuad:
    makeBinarySpecialFloat(Words, *BitWidth, 15, Literal);
    return Words;
  case FloatSemantics::X87DoubleExtended: {
    if (Literal.Negative)
      setFloatBit(Words, 79);
    for (std::uint32_t Bit = 64; Bit < 79; ++Bit)
      setFloatBit(Words, Bit);
    if (Literal.Kind == SpecialFloatLiteralKind::Infinity) {
      setFloatBit(Words, 63);
      return Words;
    }
    copyNaNPayload(Words, Literal.Payload, 63);
    setFloatBit(Words, 63);
    if (Literal.Kind == SpecialFloatLiteralKind::QuietNaN) {
      setFloatBit(Words, 62);
      return Words;
    }
    clearFloatBit(Words, 62);
    bool HasPayload = false;
    for (std::uint32_t Bit = 0; Bit < 63; ++Bit)
      HasPayload = HasPayload || hasFloatBit(Words, Bit);
    if (!HasPayload)
      setFloatBit(Words, 61);
    return Words;
  }
  case FloatSemantics::PPCDoubleDouble:
  case FloatSemantics::PPCDoubleDoubleLegacy: {
    std::vector<std::uint64_t> HighPart(1, 0);
    makeBinarySpecialFloat(HighPart, 64, 11, Literal);
    Words[0] = HighPart.front();
    return Words;
  }
  case FloatSemantics::Float8E5M2:
  case FloatSemantics::Float8E5M2FNUZ:
  case FloatSemantics::Float8E4M3:
  case FloatSemantics::Float8E4M3FN:
  case FloatSemantics::Float8E4M3FNUZ:
  case FloatSemantics::Float8E4M3B11FNUZ:
  case FloatSemantics::Float8E3M4:
  case FloatSemantics::FloatTF32:
  case FloatSemantics::Float8E8M0FNU:
  case FloatSemantics::Float6E3M2FN:
  case FloatSemantics::Float6E2M3FN:
  case FloatSemantics::Float4E2M1FN:
  case FloatSemantics::Count:
    break;
  }
  return makeUnexpected(ErrorCode::Unsupported,
                        "floating-point special literal uses unsupported semantics");
}

struct BinaryFloatFormat final {
  std::uint32_t Precision = 0;
  std::int32_t MinExponent = 0;
  std::int32_t MaxExponent = 0;
  std::uint32_t ExponentBitWidth = 0;
  std::uint32_t StorageBitWidth = 0;
  bool HasExplicitIntegerBit = false;
};

[[nodiscard]] std::optional<BinaryFloatFormat>
binaryFloatFormat(FloatSemantics Semantics) noexcept {
  switch (Semantics) {
  case FloatSemantics::IEEEHalf:
    return BinaryFloatFormat{.Precision = 11,
                             .MinExponent = -14,
                             .MaxExponent = 15,
                             .ExponentBitWidth = 5,
                             .StorageBitWidth = 16};
  case FloatSemantics::BFloat:
    return BinaryFloatFormat{.Precision = 8,
                             .MinExponent = -126,
                             .MaxExponent = 127,
                             .ExponentBitWidth = 8,
                             .StorageBitWidth = 16};
  case FloatSemantics::IEEESingle:
    return BinaryFloatFormat{.Precision = 24,
                             .MinExponent = -126,
                             .MaxExponent = 127,
                             .ExponentBitWidth = 8,
                             .StorageBitWidth = 32};
  case FloatSemantics::IEEEDouble:
    return BinaryFloatFormat{.Precision = 53,
                             .MinExponent = -1022,
                             .MaxExponent = 1023,
                             .ExponentBitWidth = 11,
                             .StorageBitWidth = 64};
  case FloatSemantics::IEEEQuad:
    return BinaryFloatFormat{.Precision = 113,
                             .MinExponent = -16382,
                             .MaxExponent = 16383,
                             .ExponentBitWidth = 15,
                             .StorageBitWidth = 128};
  case FloatSemantics::X87DoubleExtended:
    return BinaryFloatFormat{.Precision = 64,
                             .MinExponent = -16382,
                             .MaxExponent = 16383,
                             .ExponentBitWidth = 15,
                             .StorageBitWidth = 80,
                             .HasExplicitIntegerBit = true};
  case FloatSemantics::PPCDoubleDouble:
  case FloatSemantics::PPCDoubleDoubleLegacy:
    // LLVM parses a decimal PPC double-double through its documented legacy
    // 106-bit format and then materializes the canonical pair of doubles.
    return BinaryFloatFormat{.Precision = 106,
                             .MinExponent = -969,
                             .MaxExponent = 1023,
                             .ExponentBitWidth = 0,
                             .StorageBitWidth = 128};
  case FloatSemantics::Float8E5M2:
  case FloatSemantics::Float8E5M2FNUZ:
  case FloatSemantics::Float8E4M3:
  case FloatSemantics::Float8E4M3FN:
  case FloatSemantics::Float8E4M3FNUZ:
  case FloatSemantics::Float8E4M3B11FNUZ:
  case FloatSemantics::Float8E3M4:
  case FloatSemantics::FloatTF32:
  case FloatSemantics::Float8E8M0FNU:
  case FloatSemantics::Float6E3M2FN:
  case FloatSemantics::Float6E2M3FN:
  case FloatSemantics::Float4E2M1FN:
  case FloatSemantics::Count:
    return std::nullopt;
  }
  return std::nullopt;
}

struct RoundedBinaryFloat final {
  bool IsZero = false;
  bool IsSubnormal = false;
  std::int64_t Exponent = 0;
  std::int64_t Scale = 0;
  BigUnsigned Significand;
};

[[nodiscard]] RoundedBinaryFloat makeZeroRoundedBinaryFloat() {
  RoundedBinaryFloat Result;
  Result.IsZero = true;
  return Result;
}

[[nodiscard]] bool checkedAddExponent(std::int64_t Left, std::int64_t Right,
                                      std::int64_t &Result) noexcept {
  if (Right > 0 && Left > std::numeric_limits<std::int64_t>::max() - Right)
    return false;
  if (Right < 0 && Left < std::numeric_limits<std::int64_t>::min() - Right)
    return false;
  Result = Left + Right;
  return true;
}

[[nodiscard]] bool checkedSubtractExponent(std::int64_t Left, std::int64_t Right,
                                           std::int64_t &Result) noexcept {
  if (Right == std::numeric_limits<std::int64_t>::min()) {
    if (Left >= 0)
      return false;
    Result = Left - Right;
    return true;
  }
  return checkedAddExponent(Left, -Right, Result);
}

[[nodiscard]] Expected<BigUnsigned> divideAndRoundNearest(BigUnsigned Numerator,
                                                          const BigUnsigned &Denominator) {
  if (Denominator.isZero())
    return makeUnexpected(ErrorCode::VerificationError,
                          "floating-point conversion has a zero denominator");

  BigUnsigned Quotient;
  const std::size_t NumeratorBits = Numerator.bitWidth();
  const std::size_t DenominatorBits = Denominator.bitWidth();
  if (NumeratorBits >= DenominatorBits) {
    for (std::size_t Shift = NumeratorBits - DenominatorBits + 1U; Shift != 0; --Shift) {
      const std::size_t ActualShift = Shift - 1U;
      if (compareToShifted(Numerator, Denominator, ActualShift) < 0)
        continue;
      Numerator.subtract(Denominator.shiftedLeft(ActualShift));
      Quotient.setBit(ActualShift);
    }
  }

  const int HalfComparison = Numerator.shiftedLeft(1).compare(Denominator);
  if (HalfComparison > 0 || (HalfComparison == 0 && Quotient.isOdd()))
    Quotient.addSmall(1);
  return Quotient;
}

[[nodiscard]] Expected<BigUnsigned>
roundScaledRational(BigUnsigned Numerator, BigUnsigned Denominator, std::int64_t Shift) {
  const auto AbsoluteShift = [Shift]() -> std::uint64_t {
    if (Shift >= 0)
      return static_cast<std::uint64_t>(Shift);
    return static_cast<std::uint64_t>(-(Shift + 1)) + 1U;
  }();
  if (AbsoluteShift > std::numeric_limits<std::size_t>::max()) {
    return makeUnexpected(ErrorCode::ParseError,
                          "floating-point literal has an unrepresentable binary exponent");
  }
  if (Shift >= 0)
    Numerator = Numerator.shiftedLeft(static_cast<std::size_t>(AbsoluteShift));
  else
    Denominator = Denominator.shiftedLeft(static_cast<std::size_t>(AbsoluteShift));
  return divideAndRoundNearest(std::move(Numerator), Denominator);
}

[[nodiscard]] Expected<std::int64_t> floorLog2OfRational(const BigUnsigned &Numerator,
                                                         const BigUnsigned &Denominator,
                                                         std::int64_t BinaryScale) {
  if (Numerator.isZero() || Denominator.isZero()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "floating-point conversion requires a nonzero rational value");
  }
  const std::size_t NumeratorBits = Numerator.bitWidth();
  const std::size_t DenominatorBits = Denominator.bitWidth();
  if (NumeratorBits > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()) ||
      DenominatorBits > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "floating-point literal significand is too large");
  }
  const std::int64_t BitDelta =
      static_cast<std::int64_t>(NumeratorBits) - static_cast<std::int64_t>(DenominatorBits);
  std::int64_t Candidate = 0;
  if (!checkedAddExponent(BinaryScale, BitDelta, Candidate)) {
    return makeUnexpected(BinaryScale >= 0 ? ErrorCode::ParseError : ErrorCode::Unsupported,
                          BinaryScale >= 0 ? "floating-point constant overflowed type"
                                           : "floating-point constant underflowed type");
  }

  const std::int64_t RelativeShift = -BitDelta;
  int Comparison = 0;
  if (RelativeShift >= 0) {
    if (static_cast<std::uint64_t>(RelativeShift) > std::numeric_limits<std::size_t>::max()) {
      Comparison = -1;
    } else {
      Comparison =
          -compareToShifted(Denominator, Numerator, static_cast<std::size_t>(RelativeShift));
    }
  } else {
    const std::uint64_t AbsoluteShift = static_cast<std::uint64_t>(-(RelativeShift + 1)) + 1U;
    if (AbsoluteShift > std::numeric_limits<std::size_t>::max())
      Comparison = 1;
    else
      Comparison =
          compareToShifted(Numerator, Denominator, static_cast<std::size_t>(AbsoluteShift));
  }
  if (Comparison >= 0)
    return Candidate;
  if (Candidate == std::numeric_limits<std::int64_t>::min()) {
    return makeUnexpected(ErrorCode::ParseError, "floating-point constant underflowed type");
  }
  return Candidate - 1;
}

[[nodiscard]] Expected<RoundedBinaryFloat> roundBinaryRational(const BinaryFloatFormat &Format,
                                                               const BigUnsigned &Numerator,
                                                               const BigUnsigned &Denominator,
                                                               std::int64_t BinaryScale,
                                                               bool PermitZeroResult = false) {
  if (Numerator.isZero())
    return makeZeroRoundedBinaryFloat();

  auto FloorExponent = floorLog2OfRational(Numerator, Denominator, BinaryScale);
  if (!FloorExponent)
    return std::unexpected(FloorExponent.error());
  if (*FloorExponent > Format.MaxExponent) {
    return makeUnexpected(ErrorCode::ParseError, "floating-point constant overflowed type");
  }

  const std::int64_t SmallestUnit =
      static_cast<std::int64_t>(Format.MinExponent) - (Format.Precision - 1U);
  if (*FloorExponent < SmallestUnit - 1) {
    if (PermitZeroResult)
      return makeZeroRoundedBinaryFloat();
    return makeUnexpected(ErrorCode::ParseError, "floating-point constant underflowed type");
  }

  const bool StartsNormal = *FloorExponent >= Format.MinExponent;
  const std::int64_t Unit = StartsNormal ? *FloorExponent - (Format.Precision - 1U) : SmallestUnit;
  std::int64_t Shift = 0;
  if (!checkedSubtractExponent(BinaryScale, Unit, Shift)) {
    return makeUnexpected(BinaryScale >= 0 ? ErrorCode::ParseError : ErrorCode::Unsupported,
                          BinaryScale >= 0 ? "floating-point constant overflowed type"
                                           : "floating-point constant underflowed type");
  }
  auto Significand = roundScaledRational(Numerator, Denominator, Shift);
  if (!Significand)
    return std::unexpected(Significand.error());
  if (Significand->isZero()) {
    if (PermitZeroResult)
      return makeZeroRoundedBinaryFloat();
    return makeUnexpected(ErrorCode::ParseError, "floating-point constant underflowed type");
  }

  RoundedBinaryFloat Result;
  Result.Significand = std::move(*Significand);
  if (StartsNormal) {
    Result.Exponent = *FloorExponent;
    if (Result.Significand.bitWidth() > Format.Precision) {
      Result.Significand.shiftRightOne();
      if (Result.Exponent == Format.MaxExponent) {
        return makeUnexpected(ErrorCode::ParseError, "floating-point constant overflowed type");
      }
      ++Result.Exponent;
    }
    Result.Scale = Result.Exponent - (Format.Precision - 1U);
    return Result;
  }

  if (Result.Significand.bitWidth() >= Format.Precision) {
    Result.IsSubnormal = false;
    Result.Exponent = Format.MinExponent;
    Result.Scale = Result.Exponent - (Format.Precision - 1U);
    return Result;
  }
  Result.IsSubnormal = true;
  Result.Exponent = Format.MinExponent;
  Result.Scale = SmallestUnit;
  return Result;
}

void appendSignificandWords(std::vector<std::uint64_t> &Words, const BigUnsigned &Significand) {
  for (std::size_t Index = 0; Index < Words.size(); ++Index)
    Words[Index] = Significand.word(Index);
}

[[nodiscard]] Expected<std::vector<std::uint64_t>>
packIEEEBinaryFloat(const BinaryFloatFormat &Format, const RoundedBinaryFloat &Value,
                    bool Negative) {
  if (Format.HasExplicitIntegerBit || Format.ExponentBitWidth == 0 || Format.StorageBitWidth == 0) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "floating-point format does not use IEEE binary storage");
  }
  std::vector<std::uint64_t> Words((Format.StorageBitWidth + 63U) / 64U, 0);
  if (Value.IsZero) {
    if (Negative)
      setFloatBit(Words, Format.StorageBitWidth - 1U);
    return Words;
  }

  appendSignificandWords(Words, Value.Significand);
  const std::uint32_t FractionBitWidth = Format.Precision - 1U;
  if (!Value.IsSubnormal)
    clearFloatBit(Words, FractionBitWidth);
  const std::int64_t Bias = 1 - static_cast<std::int64_t>(Format.MinExponent);
  const std::uint64_t Exponent =
      Value.IsSubnormal ? 0 : static_cast<std::uint64_t>(Value.Exponent + Bias);
  for (std::uint32_t Bit = 0; Bit < Format.ExponentBitWidth; ++Bit) {
    if ((Exponent & (std::uint64_t{1} << Bit)) != 0)
      setFloatBit(Words, FractionBitWidth + Bit);
  }
  if (Negative)
    setFloatBit(Words, Format.StorageBitWidth - 1U);
  return Words;
}

[[nodiscard]] Expected<std::vector<std::uint64_t>>
packX87Float(const BinaryFloatFormat &Format, const RoundedBinaryFloat &Value, bool Negative) {
  if (!Format.HasExplicitIntegerBit || Format.StorageBitWidth != 80 || Format.Precision != 64) {
    return makeUnexpected(ErrorCode::VerificationError, "x87 floating-point format is invalid");
  }
  std::vector<std::uint64_t> Words(2, 0);
  if (Value.IsZero) {
    if (Negative)
      setFloatBit(Words, 79);
    return Words;
  }
  Words[0] = Value.Significand.word(0);
  if (!Value.IsSubnormal) {
    const std::int64_t Bias = 1 - static_cast<std::int64_t>(Format.MinExponent);
    Words[1] = static_cast<std::uint64_t>(Value.Exponent + Bias);
  }
  if (Negative)
    setFloatBit(Words, 79);
  return Words;
}

[[nodiscard]] Expected<std::vector<std::uint64_t>>
packPPCDoubleDouble(const RoundedBinaryFloat &Value, bool Negative) {
  const auto DoubleFormat = binaryFloatFormat(FloatSemantics::IEEEDouble);
  if (!DoubleFormat)
    return makeUnexpected(ErrorCode::VerificationError, "IEEE double format is unavailable");
  if (Value.IsZero) {
    RoundedBinaryFloat Zero = makeZeroRoundedBinaryFloat();
    auto High = packIEEEBinaryFloat(*DoubleFormat, Zero, Negative);
    if (!High)
      return std::unexpected(High.error());
    return std::vector<std::uint64_t>{High->front(), 0};
  }

  BigUnsigned One(1);
  auto High = roundBinaryRational(*DoubleFormat, Value.Significand, One, Value.Scale, true);
  if (!High)
    return std::unexpected(High.error());
  auto HighWords = packIEEEBinaryFloat(*DoubleFormat, *High, Negative);
  if (!HighWords)
    return std::unexpected(HighWords.error());

  BigUnsigned SourceMagnitude = Value.Significand;
  BigUnsigned HighMagnitude = High->Significand;
  const std::int64_t CommonScale = std::min(Value.Scale, High->Scale);
  if (!SourceMagnitude.isZero()) {
    const std::int64_t SourceShift = Value.Scale - CommonScale;
    if (SourceShift > 0)
      SourceMagnitude = SourceMagnitude.shiftedLeft(static_cast<std::size_t>(SourceShift));
  }
  if (!HighMagnitude.isZero()) {
    const std::int64_t HighShift = High->Scale - CommonScale;
    if (HighShift > 0)
      HighMagnitude = HighMagnitude.shiftedLeft(static_cast<std::size_t>(HighShift));
  }

  bool LowNegative = false;
  BigUnsigned LowMagnitude;
  if (SourceMagnitude.compare(HighMagnitude) >= 0) {
    LowMagnitude = std::move(SourceMagnitude);
    LowMagnitude.subtract(HighMagnitude);
    LowNegative = Negative;
  } else {
    LowMagnitude = std::move(HighMagnitude);
    LowMagnitude.subtract(SourceMagnitude);
    LowNegative = !Negative;
  }

  auto Low = roundBinaryRational(*DoubleFormat, LowMagnitude, One, CommonScale, true);
  if (!Low)
    return std::unexpected(Low.error());
  auto LowWords = packIEEEBinaryFloat(*DoubleFormat, *Low, LowNegative);
  if (!LowWords)
    return std::unexpected(LowWords.error());
  return std::vector<std::uint64_t>{HighWords->front(), LowWords->front()};
}

[[nodiscard]] Expected<std::vector<std::uint64_t>>
convertRationalToFloatWords(FloatSemantics Semantics, bool Negative, const BigUnsigned &Numerator,
                            const BigUnsigned &Denominator, std::int64_t BinaryScale) {
  const auto Format = binaryFloatFormat(Semantics);
  if (!Format) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "floating-point literal uses unsupported semantics");
  }
  auto Rounded = roundBinaryRational(*Format, Numerator, Denominator, BinaryScale);
  if (!Rounded)
    return std::unexpected(Rounded.error());
  if (Semantics == FloatSemantics::PPCDoubleDouble ||
      Semantics == FloatSemantics::PPCDoubleDoubleLegacy) {
    return packPPCDoubleDouble(*Rounded, Negative);
  }
  if (Format->HasExplicitIntegerBit)
    return packX87Float(*Format, *Rounded, Negative);
  return packIEEEBinaryFloat(*Format, *Rounded, Negative);
}

[[nodiscard]] bool isDecimalDigit(char Character) noexcept {
  return Character >= '0' && Character <= '9';
}

[[nodiscard]] Expected<std::int32_t> parseFloatingExponent(std::string_view Text,
                                                           std::int32_t Limit) {
  if (Text.empty())
    return makeUnexpected(ErrorCode::ParseError, "floating-point exponent has no digits");
  bool Negative = false;
  if (Text.front() == '+' || Text.front() == '-') {
    Negative = Text.front() == '-';
    Text.remove_prefix(1);
  }
  if (Text.empty())
    return makeUnexpected(ErrorCode::ParseError, "floating-point exponent has no digits");

  std::int32_t Value = 0;
  for (const char Character : Text) {
    if (!isDecimalDigit(Character)) {
      return makeUnexpected(ErrorCode::ParseError,
                            "floating-point exponent contains a non-decimal digit");
    }
    const std::int32_t Digit = Character - '0';
    if (Value >= Limit || Value > (Limit - Digit) / 10) {
      Value = Limit;
      continue;
    }
    Value = Value * 10 + Digit;
  }
  return Negative ? -Value : Value;
}

[[nodiscard]] BigUnsigned parseDecimalSignificand(std::string_view Digits) {
  BigUnsigned Result;
  std::size_t Position = 0;
  std::size_t FirstChunkLength = Digits.size() % 9U;
  if (FirstChunkLength == 0)
    FirstChunkLength = std::min<std::size_t>(Digits.size(), 9U);
  while (Position < Digits.size()) {
    const std::size_t Length = Position == 0 ? FirstChunkLength : 9U;
    std::uint32_t Chunk = 0;
    for (std::size_t Index = 0; Index < Length; ++Index)
      Chunk = Chunk * 10U + static_cast<std::uint32_t>(Digits[Position + Index] - '0');
    Result.multiplySmall(Position == 0 ? 1U : 1000000000U);
    Result.addSmall(Chunk);
    Position += Length;
  }
  return Result;
}

[[nodiscard]] BigUnsigned parseHexSignificand(std::string_view Digits) {
  BigUnsigned Result;
  for (const char Character : Digits) {
    Result.multiplySmall(16U);
    Result.addSmall(hexDigitValue(Character));
  }
  return Result;
}

void multiplyByPowerOfFive(BigUnsigned &Value, std::uint64_t Exponent) {
  for (std::uint64_t Index = 0; Index < Exponent; ++Index)
    Value.multiplySmall(5U);
}

[[nodiscard]] BigUnsigned powerOfFive(std::uint64_t Exponent) {
  BigUnsigned Result(1);
  multiplyByPowerOfFive(Result, Exponent);
  return Result;
}

[[nodiscard]] bool decimalDefinitelyOverflows(const BinaryFloatFormat &Format,
                                              std::int64_t NormalizedExponent) noexcept {
  const __int128 Left = (static_cast<__int128>(NormalizedExponent) - 1) * 42039;
  const __int128 Right = static_cast<__int128>(12655) * Format.MaxExponent;
  return Left >= Right;
}

[[nodiscard]] bool decimalDefinitelyUnderflows(const BinaryFloatFormat &Format,
                                               std::int64_t NormalizedExponent) noexcept {
  const __int128 Left = (static_cast<__int128>(NormalizedExponent) + 1) * 28738;
  const __int128 Right = static_cast<__int128>(8651) *
                         (static_cast<std::int64_t>(Format.MinExponent) - Format.Precision);
  return Left <= Right;
}

struct ParsedFiniteFloatLiteral final {
  bool Negative = false;
  BigUnsigned Numerator;
  BigUnsigned Denominator{1};
  std::int64_t BinaryScale = 0;
};

[[nodiscard]] Expected<ParsedFiniteFloatLiteral>
parseDecimalFloatLiteral(std::string_view Token, const BinaryFloatFormat &Format) {
  ParsedFiniteFloatLiteral Result;
  if (Token.starts_with('+') || Token.starts_with('-')) {
    Result.Negative = Token.front() == '-';
    Token.remove_prefix(1);
  }
  if (Token.empty())
    return makeUnexpected(ErrorCode::ParseError, "floating-point literal has no digits");

  std::size_t Position = 0;
  const std::size_t IntegerBegin = Position;
  while (Position < Token.size() && isDecimalDigit(Token[Position]))
    ++Position;
  if (IntegerBegin == Position || Position == Token.size() || Token[Position] != '.') {
    return makeUnexpected(ErrorCode::ParseError,
                          "decimal floating-point literal requires a decimal point");
  }

  std::string Digits(Token.substr(IntegerBegin, Position - IntegerBegin));
  ++Position;
  const std::size_t FractionBegin = Position;
  while (Position < Token.size() && isDecimalDigit(Token[Position]))
    ++Position;
  Digits.append(Token.substr(FractionBegin, Position - FractionBegin));
  const std::size_t FractionDigits = Position - FractionBegin;

  std::int32_t Exponent = 0;
  if (Position < Token.size() && (Token[Position] == 'e' || Token[Position] == 'E')) {
    auto ParsedExponent = parseFloatingExponent(Token.substr(Position + 1U), 24000);
    if (!ParsedExponent)
      return std::unexpected(ParsedExponent.error());
    Exponent = *ParsedExponent;
    Position = Token.size();
  }
  if (Position != Token.size()) {
    return makeUnexpected(ErrorCode::ParseError,
                          "decimal floating-point literal has an invalid suffix");
  }

  const std::size_t FirstSignificant = Digits.find_first_not_of('0');
  if (FirstSignificant == std::string::npos)
    return Result;
  const std::size_t LastSignificant = Digits.find_last_not_of('0');
  const std::string_view Significant(Digits.data() + FirstSignificant,
                                     LastSignificant - FirstSignificant + 1U);
  const std::size_t TrailingZeroCount = Digits.size() - LastSignificant - 1U;
  std::int64_t DecimalScale = static_cast<std::int64_t>(Exponent) -
                              static_cast<std::int64_t>(FractionDigits) +
                              static_cast<std::int64_t>(TrailingZeroCount);
  const std::int64_t NormalizedExponent =
      DecimalScale + static_cast<std::int64_t>(Significant.size() - 1U);
  if (decimalDefinitelyOverflows(Format, NormalizedExponent)) {
    return makeUnexpected(ErrorCode::ParseError, "floating-point constant overflowed type");
  }
  if (decimalDefinitelyUnderflows(Format, NormalizedExponent)) {
    return makeUnexpected(ErrorCode::ParseError, "floating-point constant underflowed type");
  }

  Result.Numerator = parseDecimalSignificand(Significant);
  Result.BinaryScale = DecimalScale;
  if (DecimalScale >= 0) {
    multiplyByPowerOfFive(Result.Numerator, static_cast<std::uint64_t>(DecimalScale));
  } else {
    Result.Denominator = powerOfFive(static_cast<std::uint64_t>(-(DecimalScale + 1)) + 1U);
  }
  return Result;
}

[[nodiscard]] Expected<ParsedFiniteFloatLiteral> parseHexFloatLiteral(std::string_view Token) {
  ParsedFiniteFloatLiteral Result;
  if (Token.starts_with('+') || Token.starts_with('-')) {
    Result.Negative = Token.front() == '-';
    Token.remove_prefix(1);
  }
  if (!Token.starts_with("0x")) {
    return makeUnexpected(ErrorCode::ParseError,
                          "hexadecimal floating-point literal must use the 0x prefix");
  }
  std::size_t Position = 2;
  const std::size_t IntegerBegin = Position;
  while (Position < Token.size() && isHexDigit(Token[Position]))
    ++Position;
  if (IntegerBegin == Position || Position == Token.size() || Token[Position] != '.') {
    return makeUnexpected(ErrorCode::ParseError,
                          "hexadecimal floating-point literal requires a hexadecimal point");
  }
  std::string Digits(Token.substr(IntegerBegin, Position - IntegerBegin));
  ++Position;
  const std::size_t FractionBegin = Position;
  while (Position < Token.size() && isHexDigit(Token[Position]))
    ++Position;
  Digits.append(Token.substr(FractionBegin, Position - FractionBegin));
  const std::size_t FractionDigits = Position - FractionBegin;
  if (Position == Token.size() || (Token[Position] != 'p' && Token[Position] != 'P')) {
    return makeUnexpected(ErrorCode::ParseError,
                          "hexadecimal floating-point literal requires a binary exponent");
  }
  auto Exponent = parseFloatingExponent(Token.substr(Position + 1U), 32767);
  if (!Exponent)
    return std::unexpected(Exponent.error());

  const std::size_t FirstSignificant = Digits.find_first_not_of('0');
  if (FirstSignificant == std::string::npos)
    return Result;
  const std::string_view Significant(Digits.data() + FirstSignificant,
                                     Digits.size() - FirstSignificant);
  Result.Numerator = parseHexSignificand(Significant);
  Result.BinaryScale =
      static_cast<std::int64_t>(*Exponent) - static_cast<std::int64_t>(FractionDigits) * 4;
  return Result;
}

[[nodiscard]] Expected<std::uint64_t> parseRawDoubleBits(std::string_view Digits) {
  if (Digits.empty())
    return makeUnexpected(ErrorCode::ParseError,
                          "hexadecimal floating-point literal has no digits");
  std::uint64_t Result = 0;
  for (const char Character : Digits) {
    if (!isHexDigit(Character)) {
      return makeUnexpected(ErrorCode::ParseError,
                            "hexadecimal floating-point literal contains a non-hexadecimal digit");
    }
    const std::uint64_t Digit = hexDigitValue(Character);
    if (Result > (std::numeric_limits<std::uint64_t>::max() - Digit) / 16U) {
      return makeUnexpected(ErrorCode::ParseError,
                            "hexadecimal floating-point literal does not fit IEEE double");
    }
    Result = Result * 16U + Digit;
  }
  return Result;
}

[[nodiscard]] int compareScaledBinaryIntegers(const BigUnsigned &Left, std::int64_t LeftScale,
                                              const BigUnsigned &Right, std::int64_t RightScale) {
  if (Left.isZero())
    return Right.isZero() ? 0 : -1;
  if (Right.isZero())
    return 1;
  if (LeftScale == RightScale)
    return Left.compare(Right);
  if (LeftScale > RightScale)
    return -compareToShifted(Right, Left, static_cast<std::size_t>(LeftScale - RightScale));
  return compareToShifted(Left, Right, static_cast<std::size_t>(RightScale - LeftScale));
}

[[nodiscard]] Expected<std::vector<std::uint64_t>>
convertRawDoubleNaN(FloatSemantics Semantics, bool Negative, std::uint64_t Fraction) {
  if (Semantics == FloatSemantics::IEEEDouble) {
    return std::vector<std::uint64_t>{(Negative ? std::uint64_t{1} << 63U : 0) |
                                      0x7ff0000000000000ULL | Fraction};
  }
  const auto Format = binaryFloatFormat(Semantics);
  if (!Format || Format->HasExplicitIntegerBit || Format->StorageBitWidth > 64 ||
      (Semantics != FloatSemantics::IEEEHalf && Semantics != FloatSemantics::BFloat &&
       Semantics != FloatSemantics::IEEESingle)) {
    return makeUnexpected(ErrorCode::ParseError,
                          "unprefixed hexadecimal literal is invalid for type");
  }
  const std::uint32_t TargetFractionBits = Format->Precision - 1U;
  const std::uint32_t DroppedBits = 53U - Format->Precision;
  const std::uint64_t DroppedMask = (std::uint64_t{1} << DroppedBits) - 1U;
  if ((Fraction & DroppedMask) != 0) {
    return makeUnexpected(
        ErrorCode::ParseError,
        "hexadecimal floating-point literal is not exactly representable by its type");
  }

  const bool IsSignaling = (Fraction & (std::uint64_t{1} << 51U)) == 0;
  std::uint64_t TargetFraction = Fraction >> DroppedBits;
  const std::uint64_t QuietBit = std::uint64_t{1} << (TargetFractionBits - 1U);
  if (IsSignaling) {
    TargetFraction &= ~QuietBit;
    if ((TargetFraction & (QuietBit - 1U)) == 0)
      TargetFraction |= QuietBit >> 1U;
  } else {
    TargetFraction |= QuietBit;
  }
  const std::uint64_t Sign = Negative ? std::uint64_t{1} << (Format->StorageBitWidth - 1U) : 0;
  const std::uint64_t Exponent = (std::uint64_t{1} << Format->ExponentBitWidth) - 1U;
  return std::vector<std::uint64_t>{Sign | (Exponent << TargetFractionBits) | TargetFraction};
}

[[nodiscard]] Expected<std::vector<std::uint64_t>> convertRawDoubleLiteral(FloatSemantics Semantics,
                                                                           std::uint64_t Bits) {
  if (Semantics != FloatSemantics::IEEEHalf && Semantics != FloatSemantics::BFloat &&
      Semantics != FloatSemantics::IEEESingle && Semantics != FloatSemantics::IEEEDouble) {
    return makeUnexpected(ErrorCode::ParseError,
                          "unprefixed hexadecimal literal is invalid for type");
  }
  if (Semantics == FloatSemantics::IEEEDouble)
    return std::vector<std::uint64_t>{Bits};

  const bool Negative = (Bits >> 63U) != 0;
  const std::uint64_t Exponent = (Bits >> 52U) & 0x7ffU;
  const std::uint64_t Fraction = Bits & 0x000fffffffffffffULL;
  if (Exponent == 0x7ffU) {
    if (Fraction != 0)
      return convertRawDoubleNaN(Semantics, Negative, Fraction);
    SpecialFloatLiteral Infinity{.Kind = SpecialFloatLiteralKind::Infinity, .Negative = Negative};
    return makeSpecialFloatWords(Semantics, Infinity);
  }

  const auto Format = binaryFloatFormat(Semantics);
  if (!Format)
    return makeUnexpected(ErrorCode::VerificationError, "IEEE target format is unavailable");
  if (Exponent == 0 && Fraction == 0) {
    RoundedBinaryFloat Zero = makeZeroRoundedBinaryFloat();
    return packIEEEBinaryFloat(*Format, Zero, Negative);
  }

  const bool IsSubnormal = Exponent == 0;
  const std::uint64_t Significand = IsSubnormal ? Fraction : (std::uint64_t{1} << 52U) | Fraction;
  const std::int64_t SourceScale =
      (IsSubnormal ? -1022 : static_cast<std::int64_t>(Exponent) - 1023) - 52;
  const BigUnsigned Source(Significand);
  const BigUnsigned One(1);
  auto Rounded = roundBinaryRational(*Format, Source, One, SourceScale);
  if (!Rounded)
    return std::unexpected(Rounded.error());
  if (compareScaledBinaryIntegers(Source, SourceScale, Rounded->Significand, Rounded->Scale) != 0) {
    return makeUnexpected(
        ErrorCode::ParseError,
        "hexadecimal floating-point literal is not exactly representable by its type");
  }
  return packIEEEBinaryFloat(*Format, *Rounded, Negative);
}

[[nodiscard]] Expected<IntegerImmediate> parseExplicitHexInteger(std::uint32_t BitWidth,
                                                                 std::string_view Spelling) {
  if (Spelling.size() < 4 || (Spelling[0] != 'u' && Spelling[0] != 's') || Spelling[1] != '0' ||
      Spelling[2] != 'x') {
    return makeUnexpected(ErrorCode::ParseError,
                          "integer hexadecimal spelling must use u0x or s0x");
  }
  const std::string_view Digits = Spelling.substr(3);
  if (Digits.empty() || !std::ranges::all_of(Digits, isHexDigit)) {
    return makeUnexpected(ErrorCode::ParseError,
                          "integer hexadecimal spelling contains a non-hexadecimal digit");
  }

  const auto *const FirstNonZero =
      std::ranges::find_if(Digits, [](char Character) { return hexDigitValue(Character) != 0; });
  std::size_t ActiveBits = 0;
  if (FirstNonZero != Digits.end()) {
    const unsigned MostSignificantNibble = hexDigitValue(*FirstNonZero);
    unsigned MostSignificantBits = 0;
    for (unsigned Value = MostSignificantNibble; Value != 0; Value >>= 1U)
      ++MostSignificantBits;
    ActiveBits =
        static_cast<std::size_t>(Digits.end() - FirstNonZero - 1U) * 4U + MostSignificantBits;
  }

  std::vector<std::uint64_t> Words((BitWidth + 63U) / 64U, 0);
  std::size_t Nibble = 0;
  for (auto Iterator = Digits.rbegin(); Iterator != Digits.rend(); ++Iterator, ++Nibble) {
    const std::size_t BitOffset = Nibble * 4U;
    if (BitOffset >= BitWidth)
      break;
    Words[BitOffset / 64U] |= static_cast<std::uint64_t>(hexDigitValue(*Iterator))
                              << (BitOffset % 64U);
  }

  // LLVM discards leading zero bits before it assigns signedness to an s0x
  // literal. A nonzero signed spelling is therefore sign-extended from its
  // active width, rather than from the number of written hexadecimal digits.
  if (Spelling[0] == 's' && ActiveBits != 0 && ActiveBits < BitWidth) {
    const std::size_t FirstExtendedBit = ActiveBits;
    const std::size_t FirstWord = FirstExtendedBit / 64U;
    const unsigned FirstBit = static_cast<unsigned>(FirstExtendedBit % 64U);
    Words[FirstWord] |= std::numeric_limits<std::uint64_t>::max() << FirstBit;
    for (std::size_t Index = FirstWord + 1U; Index < Words.size(); ++Index)
      Words[Index] = std::numeric_limits<std::uint64_t>::max();
  }
  if ((BitWidth % 64U) != 0)
    Words.back() &= (std::uint64_t{1} << (BitWidth % 64U)) - 1U;
  return IntegerImmediate::create(BitWidth, Words);
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
  const std::uint32_t BitWidth = getFloatSemanticsBitWidth(EffectiveType.Semantics).value_or(0);
  const auto Format = binaryFloatFormat(EffectiveType.Semantics);
  if (!Format)
    return fail<GenericConstantData>("floating-point type has unsupported semantics");
  std::vector<std::uint64_t> Words;

  auto SpecialLiteral = parseSpecialFloatLiteral(*Token);
  if (!SpecialLiteral)
    return fail<GenericConstantData>(SpecialLiteral.error().message());
  if (*SpecialLiteral) {
    auto SpecialWords = makeSpecialFloatWords(EffectiveType.Semantics, **SpecialLiteral);
    if (!SpecialWords)
      return fail<GenericConstantData>(SpecialWords.error().message());
    Words = std::move(*SpecialWords);
  } else if (Token->starts_with("f0x")) {
    const std::string_view Digits = Token->substr(3);
    if (Digits.size() != (static_cast<std::size_t>(BitWidth) + 3U) / 4U ||
        !parseHexWords(Digits, BitWidth, Words)) {
      return fail<GenericConstantData>("exact floating-point bit pattern does not fit its type");
    }
  } else if (Token->starts_with("0x") && Token->find('.') != std::string_view::npos) {
    auto Literal = parseHexFloatLiteral(*Token);
    if (!Literal)
      return fail<GenericConstantData>(Literal.error().message());
    auto Converted =
        convertRationalToFloatWords(EffectiveType.Semantics, Literal->Negative, Literal->Numerator,
                                    Literal->Denominator, Literal->BinaryScale);
    if (!Converted)
      return fail<GenericConstantData>(Converted.error().message());
    Words = std::move(*Converted);
  } else if (Token->starts_with("0x")) {
    std::string_view Digits = Token->substr(2);
    char Prefix = '\0';
    if (!Digits.empty() && !isHexDigit(Digits.front())) {
      Prefix = Digits.front();
      Digits.remove_prefix(1);
    }
    char ExpectedPrefix = '\0';
    switch (EffectiveType.Semantics) {
    case FloatSemantics::IEEEHalf:
      ExpectedPrefix = 'H';
      break;
    case FloatSemantics::BFloat:
      ExpectedPrefix = 'R';
      break;
    case FloatSemantics::X87DoubleExtended:
      ExpectedPrefix = 'K';
      break;
    case FloatSemantics::IEEEQuad:
      ExpectedPrefix = 'L';
      break;
    case FloatSemantics::PPCDoubleDouble:
    case FloatSemantics::PPCDoubleDoubleLegacy:
      ExpectedPrefix = 'M';
      break;
    default:
      break;
    }
    if (ExpectedPrefix != '\0') {
      const bool IsLegacyWideFormat =
          EffectiveType.Semantics == FloatSemantics::IEEEQuad ||
          EffectiveType.Semantics == FloatSemantics::PPCDoubleDouble ||
          EffectiveType.Semantics == FloatSemantics::PPCDoubleDoubleLegacy;
      const bool Parsed = IsLegacyWideFormat ? parseLegacyWideFloatHexWords(Digits, Words)
                                             : parseHexWords(Digits, BitWidth, Words);
      if (Prefix != ExpectedPrefix || !Parsed) {
        return fail<GenericConstantData>(
            "floating-point hexadecimal prefix does not match its type");
      }
    } else if (Prefix != '\0') {
      return fail<GenericConstantData>("unexpected floating-point hexadecimal prefix");
    } else {
      auto DoubleBits = parseRawDoubleBits(Digits);
      if (!DoubleBits)
        return fail<GenericConstantData>(DoubleBits.error().message());
      auto Converted = convertRawDoubleLiteral(EffectiveType.Semantics, *DoubleBits);
      if (!Converted)
        return fail<GenericConstantData>(Converted.error().message());
      Words = std::move(*Converted);
    }
  } else {
    auto Literal = parseDecimalFloatLiteral(*Token, *Format);
    if (!Literal)
      return fail<GenericConstantData>(Literal.error().message());
    auto Converted =
        convertRationalToFloatWords(EffectiveType.Semantics, Literal->Negative, Literal->Numerator,
                                    Literal->Denominator, Literal->BinaryScale);
    if (!Converted)
      return fail<GenericConstantData>(Converted.error().message());
    Words = std::move(*Converted);
  }

  auto Bits = FloatingPointBits::create(EffectiveType.Semantics, Words);
  if (!Bits)
    return std::unexpected(Bits.error());
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
      auto Start = IntegerImmediate::parseDecimal(64, *StartToken);
      if (!Start)
        return fail<GenericConstantData>("invalid inrange start");
      if (auto Comma = expect(',', "',' in inrange interval"); !Comma)
        return std::unexpected(Comma.error());
      auto EndToken = parseValueToken();
      if (!EndToken)
        return std::unexpected(EndToken.error());
      auto End = IntegerImmediate::parseDecimal(64, *EndToken);
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
    auto Integer = IsExplicitHex ? parseExplicitHexInteger(BitWidth, Spelling)
                                 : IntegerImmediate::parseDecimal(BitWidth, Spelling);
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

[[nodiscard]] std::string hexadecimalWords(std::span<const std::uint64_t> Words,
                                           std::uint32_t BitWidth) {
  static constexpr std::string_view Digits = "0123456789ABCDEF";
  const std::size_t DigitCount = std::max<std::size_t>((BitWidth + 3U) / 4U, 1);
  std::string Result(DigitCount, '0');
  for (std::size_t DigitIndex = 0; DigitIndex < DigitCount; ++DigitIndex) {
    const std::size_t SourceNibble = DigitCount - 1U - DigitIndex;
    const std::size_t WordIndex = SourceNibble / 16U;
    const unsigned Shift = static_cast<unsigned>((SourceNibble % 16U) * 4U);
    if (WordIndex < Words.size())
      Result[DigitIndex] = Digits[(Words[WordIndex] >> Shift) & 0xfU];
  }
  return Result;
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
    case FloatSemantics::PPCDoubleDoubleLegacy:
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

enum class PrintedFloatCategory : std::uint8_t {
  Zero,
  Finite,
  Infinity,
  QuietNaN,
  SignalingNaN,
};

struct PrintedBinaryFloat final {
  PrintedFloatCategory Category = PrintedFloatCategory::Zero;
  bool Negative = false;
  BigUnsigned Significand;
  std::int32_t BinaryExponent = 0;
  BigUnsigned NaNPayload;
  std::uint32_t QuietNaNBit = 0;
};

[[nodiscard]] std::uint64_t extractFloatField(std::span<const std::uint64_t> Words,
                                              std::uint32_t Begin, std::uint32_t Width) noexcept {
  std::uint64_t Result = 0;
  for (std::uint32_t Bit = 0; Bit < Width; ++Bit) {
    if (hasFloatBit(Words, Begin + Bit))
      Result |= std::uint64_t{1} << Bit;
  }
  return Result;
}

[[nodiscard]] BigUnsigned extractFloatBits(std::span<const std::uint64_t> Words,
                                           std::uint32_t Begin, std::uint32_t Width) {
  BigUnsigned Result;
  for (std::uint32_t Bit = 0; Bit < Width; ++Bit) {
    if (hasFloatBit(Words, Begin + Bit))
      Result.setBit(Bit);
  }
  return Result;
}

[[nodiscard]] std::optional<PrintedBinaryFloat> decodePrintedFloat(const FloatingPointBits &Bits) {
  std::optional<BinaryFloatFormat> Format = binaryFloatFormat(Bits.semantics());
  std::span<const std::uint64_t> Words = Bits.words();
  if (!Format)
    return std::nullopt;

  if (Bits.semantics() == FloatSemantics::PPCDoubleDouble ||
      Bits.semantics() == FloatSemantics::PPCDoubleDoubleLegacy) {
    if (Words.size() != 2 || Words[1] != 0)
      return std::nullopt;
    Format = binaryFloatFormat(FloatSemantics::IEEEDouble);
    Words = Words.first(1);
  }
  if (!Format || Format->ExponentBitWidth == 0)
    return std::nullopt;

  PrintedBinaryFloat Result;
  Result.Negative = hasFloatBit(Words, Format->StorageBitWidth - 1U);
  const std::uint32_t StoredSignificandWidth =
      Format->HasExplicitIntegerBit ? Format->Precision : Format->Precision - 1U;
  const std::uint64_t Exponent =
      extractFloatField(Words, StoredSignificandWidth, Format->ExponentBitWidth);
  const std::uint64_t MaximumExponent = (std::uint64_t{1} << Format->ExponentBitWidth) - 1U;

  if (Exponent == MaximumExponent) {
    const std::uint32_t PayloadWidth = Format->Precision - 1U;
    Result.NaNPayload = extractFloatBits(Words, 0, PayloadWidth);
    if (Result.NaNPayload.isZero()) {
      Result.Category = PrintedFloatCategory::Infinity;
      return Result;
    }
    Result.QuietNaNBit = PayloadWidth - 1U;
    Result.Category = hasFloatBit(Words, Result.QuietNaNBit) ? PrintedFloatCategory::QuietNaN
                                                             : PrintedFloatCategory::SignalingNaN;
    return Result;
  }

  Result.Significand = extractFloatBits(Words, 0, StoredSignificandWidth);
  if (Exponent != 0 && !Format->HasExplicitIntegerBit)
    Result.Significand.setBit(Format->Precision - 1U);
  if (Result.Significand.isZero()) {
    Result.Category = PrintedFloatCategory::Zero;
    return Result;
  }

  const std::int64_t Bias = 1 - static_cast<std::int64_t>(Format->MinExponent);
  const std::int64_t UnbiasedExponent =
      Exponent == 0 ? Format->MinExponent : static_cast<std::int64_t>(Exponent) - Bias;
  Result.BinaryExponent = static_cast<std::int32_t>(UnbiasedExponent - (Format->Precision - 1U));
  Result.Category = PrintedFloatCategory::Finite;
  return Result;
}

[[nodiscard]] std::string printNaNPayload(BigUnsigned Payload, std::uint32_t QuietNaNBit) {
  Payload.clearBit(QuietNaNBit);
  const auto Width = static_cast<std::uint32_t>(std::max<std::size_t>(Payload.bitWidth(), 1U));
  std::array<std::uint64_t, 2> Words{Payload.word(0), Payload.word(1)};
  std::string Digits = hexadecimalWords(Words, Width);
  const std::size_t FirstNonzero = Digits.find_first_not_of('0');
  if (FirstNonzero == std::string::npos)
    return "0x0";
  Digits.erase(0, FirstNonzero);
  return "0x" + Digits;
}

[[nodiscard]] std::string formatFiniteFloat(PrintedBinaryFloat Value) {
  std::string Result;
  if (Value.Negative)
    Result.push_back('-');
  if (Value.Category == PrintedFloatCategory::Zero) {
    Result.append("0.000000e+00");
    return Result;
  }

  const std::size_t TrailingZeros = Value.Significand.countrZero();
  Value.Significand.shiftRight(TrailingZeros);
  Value.BinaryExponent += static_cast<std::int32_t>(TrailingZeros);

  std::int32_t DecimalExponent = 0;
  if (Value.BinaryExponent >= 0) {
    Value.Significand =
        Value.Significand.shiftedLeft(static_cast<std::size_t>(Value.BinaryExponent));
  } else {
    DecimalExponent = Value.BinaryExponent;
    for (std::int32_t Power = 0; Power < -Value.BinaryExponent; ++Power)
      Value.Significand.multiplySmall(5);
  }

  std::string Digits = Value.Significand.toDecimal();
  while (Digits.size() > 1U && Digits.back() == '0') {
    Digits.pop_back();
    ++DecimalExponent;
  }

  constexpr std::size_t FormatPrecision = 6;
  if (Digits.size() > FormatPrecision) {
    std::size_t Removed = Digits.size() - FormatPrecision;
    const bool RoundUp = Digits[FormatPrecision] >= '5';
    Digits.resize(FormatPrecision);
    if (RoundUp) {
      std::size_t Position = Digits.size();
      while (Position != 0 && Digits[Position - 1U] == '9') {
        Digits[Position - 1U] = '0';
        --Position;
      }
      if (Position == 0)
        Digits.insert(Digits.begin(), '1');
      else
        ++Digits[Position - 1U];
    }
    while (Digits.size() > 1U && Digits.back() == '0') {
      Digits.pop_back();
      ++Removed;
    }
    DecimalExponent += static_cast<std::int32_t>(Removed);
  }

  DecimalExponent += static_cast<std::int32_t>(Digits.size() - 1U);
  Result.push_back(Digits.front());
  Result.push_back('.');
  Result.append(Digits.substr(1));
  Result.append(7U - Digits.size(), '0');
  Result.push_back('e');
  Result.push_back(DecimalExponent < 0 ? '-' : '+');
  const std::uint32_t Magnitude = static_cast<std::uint32_t>(
      DecimalExponent < 0 ? -static_cast<std::int64_t>(DecimalExponent) : DecimalExponent);
  std::string Exponent = std::to_string(Magnitude);
  if (Exponent.size() < 2U)
    Result.push_back('0');
  Result.append(Exponent);
  return Result;
}

[[nodiscard]] std::string exactFloatingText(const FloatingPointBits &Bits) {
  return "f0x" + hexadecimalWords(Bits.words(), Bits.bitWidth());
}

[[nodiscard]] std::string canonicalFloatingText(const FloatingPointBits &Bits) {
  auto Decoded = decodePrintedFloat(Bits);
  if (!Decoded)
    return exactFloatingText(Bits);

  std::string Candidate;
  switch (Decoded->Category) {
  case PrintedFloatCategory::Zero:
  case PrintedFloatCategory::Finite:
    Candidate = formatFiniteFloat(std::move(*Decoded));
    break;
  case PrintedFloatCategory::Infinity:
    Candidate = Decoded->Negative ? "-inf" : "+inf";
    break;
  case PrintedFloatCategory::QuietNaN: {
    BigUnsigned Payload = Decoded->NaNPayload;
    Payload.clearBit(Decoded->QuietNaNBit);
    if (Payload.isZero()) {
      Candidate = Decoded->Negative ? "-qnan" : "+qnan";
      break;
    }
    Candidate = Decoded->Negative ? "-nan(" : "+nan(";
    Candidate.append(printNaNPayload(Decoded->NaNPayload, Decoded->QuietNaNBit));
    Candidate.push_back(')');
    break;
  }
  case PrintedFloatCategory::SignalingNaN:
    Candidate = Decoded->Negative ? "-snan(" : "+snan(";
    Candidate.append(printNaNPayload(Decoded->NaNPayload, Decoded->QuietNaNBit));
    Candidate.push_back(')');
    break;
  }

  auto Type = GenericConstantType::floating(Bits.semantics());
  if (!Type)
    return exactFloatingText(Bits);
  auto TypeText = printType(*Type);
  if (!TypeText)
    return exactFloatingText(Bits);
  auto Parsed = parseLLVMConstant(*TypeText + " " + Candidate);
  if (!Parsed || !Parsed->FloatingValue || *Parsed->FloatingValue != Bits)
    return exactFloatingText(Bits);
  return Candidate;
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
    Result.append(canonicalFloatingText(*Value.FloatingValue));
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
  std::uint32_t LegacyPointerSizeInBits = 0;
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
  Result.LegacyPointerSizeInBits = Options.DefaultPointerSizeInBits;
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
    if (LegacyPointerSizeInBits == 0) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "pointer size is required to lay out pointer constants");
    }
    const std::uint64_t Bytes = (static_cast<std::uint64_t>(LegacyPointerSizeInBits) + 7U) / 8U;
    return LayoutPointerEntry{
        .AddressSpace = AddressSpace,
        .SizeInBits = LegacyPointerSizeInBits,
        .IndexSizeInBits = LegacyPointerSizeInBits,
        .Alignment = natural(Bytes),
        .HasExternalState = false,
        .HasUnstableRepresentation = false,
        .NullIsAllOnes = false,
        .AddressSpaceName = {},
    };
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
