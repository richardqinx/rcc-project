// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/ADT/APFloat.h"

#include "rcc/ADT/APInt.h"

#include <algorithm>
#include <array>
#include <charconv>
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

namespace rcc {
namespace {

struct FloatSemanticsInfo final {
  std::uint32_t BitWidth;
  std::string_view Spelling;
};

constexpr std::array FloatSemanticsTable{
    FloatSemanticsInfo{16, "f16"},       FloatSemanticsInfo{16, "bf16"},
    FloatSemanticsInfo{32, "f32"},       FloatSemanticsInfo{64, "f64"},
    FloatSemanticsInfo{128, "f128"},     FloatSemanticsInfo{128, "ppcf128"},
    FloatSemanticsInfo{8, "f8e5m2"},     FloatSemanticsInfo{8, "f8e5m2fnuz"},
    FloatSemanticsInfo{8, "f8e4m3"},     FloatSemanticsInfo{8, "f8e4m3fn"},
    FloatSemanticsInfo{8, "f8e4m3fnuz"}, FloatSemanticsInfo{8, "f8e4m3b11fnuz"},
    FloatSemanticsInfo{8, "f8e3m4"},     FloatSemanticsInfo{19, "tf32"},
    FloatSemanticsInfo{8, "f8e8m0fnu"},  FloatSemanticsInfo{6, "f6e3m2fn"},
    FloatSemanticsInfo{6, "f6e2m3fn"},   FloatSemanticsInfo{4, "f4e2m1fn"},
    FloatSemanticsInfo{80, "x86fp80"},
};

static_assert(FloatSemanticsTable.size() == static_cast<std::size_t>(FloatSemantics::Count));

[[nodiscard]] const FloatSemanticsInfo *getFloatSemanticsInfo(FloatSemantics Semantics) noexcept {
  const auto Index = static_cast<std::size_t>(Semantics);
  if (Index >= FloatSemanticsTable.size())
    return nullptr;
  return &FloatSemanticsTable[Index];
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

[[nodiscard]] bool parseWideFloatHexWords(std::string_view Digits,
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
  case FloatSemantics::PPCDoubleDouble: {
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
    // Decimal PPC double-double literals use 106 bits of precision and are
    // materialized as a canonical pair of doubles.
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
  APInt Significand;
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

[[nodiscard]] Expected<APInt> divideAndRoundNearest(APInt Numerator, const APInt &Denominator) {
  if (Denominator.isZero())
    return makeUnexpected(ErrorCode::VerificationError,
                          "floating-point conversion has a zero denominator");

  APInt Quotient;
  const std::size_t NumeratorBits = Numerator.activeBitWidth();
  const std::size_t DenominatorBits = Denominator.activeBitWidth();
  if (NumeratorBits >= DenominatorBits) {
    for (std::size_t Shift = NumeratorBits - DenominatorBits + 1U; Shift != 0; --Shift) {
      const std::size_t ActualShift = Shift - 1U;
      if (Numerator.compareToShifted(Denominator, ActualShift) < 0)
        continue;
      Numerator.subtract(Denominator.shiftedLeft(ActualShift));
      Quotient.setBit(ActualShift);
    }
  }

  const int HalfComparison = Numerator.shiftedLeft(1).compare(Denominator);
  if (HalfComparison > 0 || (HalfComparison == 0 && (Quotient.word(0) & 1U) != 0))
    Quotient.addSmall(1);
  return Quotient;
}

[[nodiscard]] Expected<APInt> roundScaledRational(APInt Numerator, APInt Denominator,
                                                  std::int64_t Shift) {
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

[[nodiscard]] Expected<std::int64_t>
floorLog2OfRational(const APInt &Numerator, const APInt &Denominator, std::int64_t BinaryScale) {
  if (Numerator.isZero() || Denominator.isZero()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "floating-point conversion requires a nonzero rational value");
  }
  const std::size_t NumeratorBits = Numerator.activeBitWidth();
  const std::size_t DenominatorBits = Denominator.activeBitWidth();
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
          -Denominator.compareToShifted(Numerator, static_cast<std::size_t>(RelativeShift));
    }
  } else {
    const std::uint64_t AbsoluteShift = static_cast<std::uint64_t>(-(RelativeShift + 1)) + 1U;
    if (AbsoluteShift > std::numeric_limits<std::size_t>::max())
      Comparison = 1;
    else
      Comparison = Numerator.compareToShifted(Denominator, static_cast<std::size_t>(AbsoluteShift));
  }
  if (Comparison >= 0)
    return Candidate;
  if (Candidate == std::numeric_limits<std::int64_t>::min()) {
    return makeUnexpected(ErrorCode::ParseError, "floating-point constant underflowed type");
  }
  return Candidate - 1;
}

[[nodiscard]] Expected<RoundedBinaryFloat> roundBinaryRational(const BinaryFloatFormat &Format,
                                                               const APInt &Numerator,
                                                               const APInt &Denominator,
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
    if (Result.Significand.activeBitWidth() > Format.Precision) {
      Result.Significand.shiftRight(1);
      if (Result.Exponent == Format.MaxExponent) {
        return makeUnexpected(ErrorCode::ParseError, "floating-point constant overflowed type");
      }
      ++Result.Exponent;
    }
    Result.Scale = Result.Exponent - (Format.Precision - 1U);
    return Result;
  }

  if (Result.Significand.activeBitWidth() >= Format.Precision) {
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

void appendSignificandWords(std::vector<std::uint64_t> &Words, const APInt &Significand) {
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

  APInt One(1);
  auto High = roundBinaryRational(*DoubleFormat, Value.Significand, One, Value.Scale, true);
  if (!High)
    return std::unexpected(High.error());
  auto HighWords = packIEEEBinaryFloat(*DoubleFormat, *High, Negative);
  if (!HighWords)
    return std::unexpected(HighWords.error());

  APInt SourceMagnitude = Value.Significand;
  APInt HighMagnitude = High->Significand;
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
  APInt LowMagnitude;
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
convertRationalToFloatWords(FloatSemantics Semantics, bool Negative, const APInt &Numerator,
                            const APInt &Denominator, std::int64_t BinaryScale) {
  const auto Format = binaryFloatFormat(Semantics);
  if (!Format) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "floating-point literal uses unsupported semantics");
  }
  auto Rounded = roundBinaryRational(*Format, Numerator, Denominator, BinaryScale);
  if (!Rounded)
    return std::unexpected(Rounded.error());
  if (Semantics == FloatSemantics::PPCDoubleDouble) {
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

[[nodiscard]] APInt parseDecimalSignificand(std::string_view Digits) {
  APInt Result;
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

[[nodiscard]] APInt parseHexSignificand(std::string_view Digits) {
  APInt Result;
  for (const char Character : Digits) {
    Result.multiplySmall(16U);
    Result.addSmall(hexDigitValue(Character));
  }
  return Result;
}

void multiplyByPowerOfFive(APInt &Value, std::uint64_t Exponent) {
  for (std::uint64_t Index = 0; Index < Exponent; ++Index)
    Value.multiplySmall(5U);
}

[[nodiscard]] APInt powerOfFive(std::uint64_t Exponent) {
  APInt Result(1);
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
  APInt Numerator;
  APInt Denominator{1};
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

[[nodiscard]] int compareScaledBinaryIntegers(const APInt &Left, std::int64_t LeftScale,
                                              const APInt &Right, std::int64_t RightScale) {
  if (Left.isZero())
    return Right.isZero() ? 0 : -1;
  if (Right.isZero())
    return 1;
  if (LeftScale == RightScale)
    return Left.compare(Right);
  if (LeftScale > RightScale)
    return -Right.compareToShifted(Left, static_cast<std::size_t>(LeftScale - RightScale));
  return Left.compareToShifted(Right, static_cast<std::size_t>(RightScale - LeftScale));
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
  const APInt Source(Significand);
  const APInt One(1);
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
  APInt Significand;
  std::int32_t BinaryExponent = 0;
  APInt NaNPayload;
  std::uint32_t QuietNaNBit = 0;
};

[[nodiscard]] std::string hexadecimalWords(std::span<const std::uint64_t> Words,
                                           std::uint32_t BitWidth) {
  constexpr std::string_view Digits = "0123456789ABCDEF";
  const std::size_t DigitCount = (static_cast<std::size_t>(BitWidth) + 3U) / 4U;
  std::string Result(DigitCount, '0');
  for (std::size_t Index = 0; Index < DigitCount; ++Index) {
    const std::size_t Bit = Index * 4U;
    const std::uint64_t Nibble = (Words[Bit / 64U] >> (Bit % 64U)) & 0xFU;
    Result[DigitCount - Index - 1U] = Digits[static_cast<std::size_t>(Nibble)];
  }
  return Result;
}

[[nodiscard]] std::uint64_t extractFloatField(std::span<const std::uint64_t> Words,
                                              std::uint32_t Begin, std::uint32_t Width) noexcept {
  std::uint64_t Result = 0;
  for (std::uint32_t Bit = 0; Bit < Width; ++Bit) {
    if (hasFloatBit(Words, Begin + Bit))
      Result |= std::uint64_t{1} << Bit;
  }
  return Result;
}

[[nodiscard]] APInt extractFloatBits(std::span<const std::uint64_t> Words, std::uint32_t Begin,
                                     std::uint32_t Width) {
  APInt Result;
  for (std::uint32_t Bit = 0; Bit < Width; ++Bit) {
    if (hasFloatBit(Words, Begin + Bit))
      Result.setBit(Bit);
  }
  return Result;
}

[[nodiscard]] std::optional<PrintedBinaryFloat> decodePrintedFloat(const APFloat &Bits) {
  std::optional<BinaryFloatFormat> Format = binaryFloatFormat(Bits.semantics());
  std::span<const std::uint64_t> Words = Bits.words();
  if (!Format)
    return std::nullopt;

  if (Bits.semantics() == FloatSemantics::PPCDoubleDouble) {
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

[[nodiscard]] std::string printNaNPayload(APInt Payload, std::uint32_t QuietNaNBit) {
  Payload.clearBit(QuietNaNBit);
  const auto Width =
      static_cast<std::uint32_t>(std::max<std::size_t>(Payload.activeBitWidth(), 1U));
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

  std::string Digits = Value.Significand.toUnsignedDecimal();
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

[[nodiscard]] std::string exactFloatingText(const APFloat &Bits) {
  return "f0x" + hexadecimalWords(Bits.words(), Bits.bitWidth());
}

[[nodiscard]] std::string canonicalFloatingText(const APFloat &Bits) {
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
    APInt Payload = Decoded->NaNPayload;
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

  auto Parsed = APFloat::parse(Bits.semantics(), Candidate);
  if (!Parsed || *Parsed != Bits)
    return exactFloatingText(Bits);
  return Candidate;
}

} // namespace

std::optional<std::uint32_t> getFloatSemanticsBitWidth(FloatSemantics Semantics) noexcept {
  const auto *Info = getFloatSemanticsInfo(Semantics);
  if (Info == nullptr)
    return std::nullopt;
  return Info->BitWidth;
}

std::string_view getFloatSemanticsSpelling(FloatSemantics Semantics) noexcept {
  const auto *Info = getFloatSemanticsInfo(Semantics);
  return Info == nullptr ? std::string_view{"invalid-float"} : Info->Spelling;
}

Expected<APFloat> APFloat::create(FloatSemantics Semantics, std::span<const std::uint64_t> Words) {
  const auto BitWidth = getFloatSemanticsBitWidth(Semantics);
  if (!BitWidth)
    return makeUnexpected(ErrorCode::InvalidArgument, "floating value has unknown semantics");

  const std::size_t RequiredWords = (*BitWidth + 63U) / 64U;
  if (RequiredWords > 2 || Words.size() != RequiredWords) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "floating bit pattern has the wrong word count");
  }
  const std::uint32_t UsedBitsInLastWord = *BitWidth % 64U;
  if (UsedBitsInLastWord != 0 && (Words.back() >> UsedBitsInLastWord) != 0) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "floating value has nonzero bits above its semantics width");
  }

  std::array<std::uint64_t, 2> StoredWords{};
  std::ranges::copy(Words, StoredWords.begin());
  return APFloat(Semantics, StoredWords, static_cast<std::uint8_t>(RequiredWords));
}

Expected<APFloat> APFloat::parse(FloatSemantics Semantics, std::string_view Text) {
  const auto Format = binaryFloatFormat(Semantics);
  if (!Format)
    return makeUnexpected(ErrorCode::Unsupported, "floating-point semantics are unsupported");

  std::vector<std::uint64_t> Words;
  auto SpecialLiteral = parseSpecialFloatLiteral(Text);
  if (!SpecialLiteral)
    return std::unexpected(SpecialLiteral.error());
  if (*SpecialLiteral) {
    auto SpecialWords = makeSpecialFloatWords(Semantics, **SpecialLiteral);
    if (!SpecialWords)
      return std::unexpected(SpecialWords.error());
    Words = std::move(*SpecialWords);
  } else if (Text.starts_with("f0x")) {
    const std::uint32_t BitWidth = getFloatSemanticsBitWidth(Semantics).value_or(0);
    const std::string_view Digits = Text.substr(3);
    if (Digits.size() != (static_cast<std::size_t>(BitWidth) + 3U) / 4U ||
        !parseHexWords(Digits, BitWidth, Words)) {
      return makeUnexpected(ErrorCode::ParseError,
                            "exact floating-point bit pattern does not fit its type");
    }
  } else if (Text.starts_with("0x") && Text.find('.') != std::string_view::npos) {
    auto Literal = parseHexFloatLiteral(Text);
    if (!Literal)
      return std::unexpected(Literal.error());
    auto Converted = convertRationalToFloatWords(Semantics, Literal->Negative, Literal->Numerator,
                                                 Literal->Denominator, Literal->BinaryScale);
    if (!Converted)
      return std::unexpected(Converted.error());
    Words = std::move(*Converted);
  } else if (Text.starts_with("0x")) {
    const std::uint32_t BitWidth = getFloatSemanticsBitWidth(Semantics).value_or(0);
    std::string_view Digits = Text.substr(2);
    char Prefix = '\0';
    if (!Digits.empty() && !isHexDigit(Digits.front())) {
      Prefix = Digits.front();
      Digits.remove_prefix(1);
    }
    char ExpectedPrefix = '\0';
    switch (Semantics) {
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
      ExpectedPrefix = 'M';
      break;
    default:
      break;
    }
    if (ExpectedPrefix != '\0') {
      const bool HasWideWords =
          Semantics == FloatSemantics::IEEEQuad || Semantics == FloatSemantics::PPCDoubleDouble;
      const bool Parsed = HasWideWords ? parseWideFloatHexWords(Digits, Words)
                                       : parseHexWords(Digits, BitWidth, Words);
      if (Prefix != ExpectedPrefix || !Parsed) {
        return makeUnexpected(ErrorCode::ParseError,
                              "floating-point hexadecimal prefix does not match its type");
      }
    } else if (Prefix != '\0') {
      return makeUnexpected(ErrorCode::ParseError, "unexpected floating-point hexadecimal prefix");
    } else {
      auto DoubleBits = parseRawDoubleBits(Digits);
      if (!DoubleBits)
        return std::unexpected(DoubleBits.error());
      auto Converted = convertRawDoubleLiteral(Semantics, *DoubleBits);
      if (!Converted)
        return std::unexpected(Converted.error());
      Words = std::move(*Converted);
    }
  } else {
    auto Literal = parseDecimalFloatLiteral(Text, *Format);
    if (!Literal)
      return std::unexpected(Literal.error());
    auto Converted = convertRationalToFloatWords(Semantics, Literal->Negative, Literal->Numerator,
                                                 Literal->Denominator, Literal->BinaryScale);
    if (!Converted)
      return std::unexpected(Converted.error());
    Words = std::move(*Converted);
  }
  return create(Semantics, Words);
}

std::string APFloat::toString() const {
  return canonicalFloatingText(*this);
}

} // namespace rcc
