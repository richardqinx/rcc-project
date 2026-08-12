// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/ADT/APInt.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rcc {
namespace {

[[nodiscard]] constexpr std::size_t wordCount(std::uint32_t BitWidth) noexcept {
  return (static_cast<std::size_t>(BitWidth) + 63U) / 64U;
}

[[nodiscard]] constexpr std::uint64_t highWordMask(std::uint32_t BitWidth) noexcept {
  const std::uint32_t UsedBits = BitWidth % 64U;
  if (UsedBits == 0)
    return std::numeric_limits<std::uint64_t>::max();
  return (std::uint64_t{1} << UsedBits) - 1U;
}

[[nodiscard]] std::size_t significantWordCount(std::span<const std::uint64_t> Words) noexcept {
  std::size_t Count = Words.size();
  while (Count != 0 && Words[Count - 1U] == 0)
    --Count;
  return Count;
}

[[nodiscard]] Expected<void> verifyAPInt(std::uint32_t BitWidth,
                                         std::span<const std::uint64_t> Words, ErrorCode Code) {
  if (BitWidth == 0 || BitWidth > APInt::MaxBitWidth)
    return makeUnexpected(Code, "APInt bit width is outside the supported range");
  if (Words.size() != wordCount(BitWidth))
    return makeUnexpected(Code, "APInt bit pattern has the wrong word count");
  if ((Words.back() & ~highWordMask(BitWidth)) != 0)
    return makeUnexpected(Code, "APInt has nonzero bits above its declared width");
  return {};
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

void negateWords(std::vector<std::uint64_t> &Words, std::uint32_t BitWidth) {
  for (std::uint64_t &Word : Words)
    Word = ~Word;
  std::uint64_t Carry = 1;
  for (std::uint64_t &Word : Words) {
    const std::uint64_t Previous = Word;
    Word += Carry;
    Carry = Carry != 0 && Word < Previous ? 1U : 0U;
  }
  Words.back() &= highWordMask(BitWidth);
}

[[nodiscard]] std::string unsignedDecimal(std::vector<std::uint64_t> Words) {
  if (std::ranges::all_of(Words, [](std::uint64_t Word) { return Word == 0; }))
    return "0";
  std::string Digits;
  while (!std::ranges::all_of(Words, [](std::uint64_t Word) { return Word == 0; })) {
    unsigned __int128 Remainder = 0;
    for (auto Iterator = Words.rbegin(); Iterator != Words.rend(); ++Iterator) {
      const unsigned __int128 Dividend = (Remainder << 64U) | *Iterator;
      *Iterator = static_cast<std::uint64_t>(Dividend / 10U);
      Remainder = Dividend % 10U;
    }
    Digits.push_back(static_cast<char>('0' + static_cast<unsigned>(Remainder)));
  }
  std::ranges::reverse(Digits);
  return Digits;
}

} // namespace

APInt::APInt(std::uint64_t Value) {
  if (Value == 0)
    return;
  Words.push_back(Value);
  BitWidth = static_cast<std::uint32_t>(std::bit_width(Value));
}

Expected<APInt> APInt::create(std::uint32_t BitWidth, std::span<const std::uint64_t> Words) {
  auto Verification = verifyAPInt(BitWidth, Words, ErrorCode::InvalidArgument);
  if (!Verification)
    return std::unexpected(Verification.error());
  return APInt(BitWidth, std::vector<std::uint64_t>(Words.begin(), Words.end()));
}

Expected<APInt> APInt::fromUnsigned(std::uint32_t BitWidth, std::uint64_t Value) {
  if (BitWidth == 0 || BitWidth > MaxBitWidth)
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "APInt bit width is outside the supported range");
  if (BitWidth < 64U && (Value >> BitWidth) != 0)
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "unsigned value does not fit the APInt bit width");

  std::vector<std::uint64_t> Words(wordCount(BitWidth), 0);
  Words.front() = Value;
  return create(BitWidth, Words);
}

Expected<APInt> APInt::fromSigned(std::uint32_t BitWidth, std::int64_t Value) {
  if (BitWidth == 0 || BitWidth > MaxBitWidth)
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "APInt bit width is outside the supported range");
  if (BitWidth < 64U) {
    const std::uint32_t MagnitudeBits = BitWidth - 1U;
    const std::int64_t Minimum = -(std::int64_t{1} << MagnitudeBits);
    const std::int64_t Maximum = (std::int64_t{1} << MagnitudeBits) - 1;
    if (Value < Minimum || Value > Maximum)
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "signed value does not fit the APInt bit width");
  }

  const std::uint64_t Fill =
      Value < 0 ? std::numeric_limits<std::uint64_t>::max() : std::uint64_t{0};
  std::vector<std::uint64_t> Words(wordCount(BitWidth), Fill);
  Words.front() = static_cast<std::uint64_t>(Value);
  Words.back() &= highWordMask(BitWidth);
  return create(BitWidth, Words);
}

Expected<APInt> APInt::parseDecimal(std::uint32_t BitWidth, std::string_view Text) {
  if (BitWidth == 0 || BitWidth > MaxBitWidth)
    return makeUnexpected(ErrorCode::ParseError, "APInt bit width is outside the supported range");
  const bool Negative = Text.starts_with('-');
  if (Negative)
    Text.remove_prefix(1);
  if (Text.empty())
    return makeUnexpected(ErrorCode::ParseError, "APInt has no decimal digits");

  std::vector<std::uint64_t> Words(wordCount(BitWidth), 0);
  for (const char Character : Text) {
    if (Character < '0' || Character > '9')
      return makeUnexpected(ErrorCode::ParseError, "APInt contains a non-decimal digit");
    std::uint64_t Carry = static_cast<unsigned>(Character - '0');
    for (std::uint64_t &Word : Words) {
      const unsigned __int128 Product = static_cast<unsigned __int128>(Word) * 10U + Carry;
      Word = static_cast<std::uint64_t>(Product);
      Carry = static_cast<std::uint64_t>(Product >> 64U);
    }
  }

  Words.back() &= highWordMask(BitWidth);
  if (Negative)
    negateWords(Words, BitWidth);
  return create(BitWidth, Words);
}

Expected<APInt> APInt::parseLLVMHex(std::uint32_t BitWidth, std::string_view Text) {
  if (Text.size() < 4 || (Text[0] != 'u' && Text[0] != 's') || Text[1] != '0' || Text[2] != 'x') {
    return makeUnexpected(ErrorCode::ParseError,
                          "integer hexadecimal spelling must use u0x or s0x");
  }
  const std::string_view Digits = Text.substr(3);
  if (Digits.empty() || !std::ranges::all_of(Digits, isHexDigit)) {
    return makeUnexpected(ErrorCode::ParseError,
                          "integer hexadecimal spelling contains a non-hexadecimal digit");
  }

  const auto *const FirstNonZero =
      std::ranges::find_if(Digits, [](char Character) { return hexDigitValue(Character) != 0; });
  std::size_t ActiveBits = 0;
  if (FirstNonZero != Digits.end()) {
    const unsigned MostSignificantNibble = hexDigitValue(*FirstNonZero);
    const auto MostSignificantBits = static_cast<unsigned>(std::bit_width(MostSignificantNibble));
    ActiveBits =
        static_cast<std::size_t>(Digits.end() - FirstNonZero - 1U) * 4U + MostSignificantBits;
  }

  if (BitWidth == 0 || BitWidth > MaxBitWidth)
    return makeUnexpected(ErrorCode::ParseError, "APInt bit width is outside the supported range");
  std::vector<std::uint64_t> Words(wordCount(BitWidth), 0);
  std::size_t Nibble = 0;
  for (auto Iterator = Digits.rbegin(); Iterator != Digits.rend(); ++Iterator, ++Nibble) {
    const std::size_t BitOffset = Nibble * 4U;
    if (BitOffset >= BitWidth)
      break;
    Words[BitOffset / 64U] |= static_cast<std::uint64_t>(hexDigitValue(*Iterator))
                              << (BitOffset % 64U);
  }

  if (Text[0] == 's' && ActiveBits != 0 && ActiveBits < BitWidth) {
    const std::size_t FirstWord = ActiveBits / 64U;
    const unsigned FirstBit = static_cast<unsigned>(ActiveBits % 64U);
    Words[FirstWord] |= std::numeric_limits<std::uint64_t>::max() << FirstBit;
    for (std::size_t Index = FirstWord + 1U; Index < Words.size(); ++Index)
      Words[Index] = std::numeric_limits<std::uint64_t>::max();
  }
  Words.back() &= highWordMask(BitWidth);
  return create(BitWidth, Words);
}

bool APInt::isZero() const noexcept {
  return std::ranges::all_of(Words, [](std::uint64_t Word) { return Word == 0; });
}

bool APInt::isNegative() const noexcept {
  if (BitWidth == 0 || Words.empty())
    return false;
  const std::uint32_t SignBit = BitWidth - 1U;
  return (Words[SignBit / 64U] & (std::uint64_t{1} << (SignBit % 64U))) != 0;
}

std::size_t APInt::activeBitWidth() const noexcept {
  const std::size_t Count = significantWordCount(Words);
  if (Count == 0)
    return 0;
  return (Count - 1U) * 64U + static_cast<std::size_t>(std::bit_width(Words[Count - 1U]));
}

std::size_t APInt::countrZero() const noexcept {
  std::size_t Result = 0;
  for (const std::uint64_t Word : Words) {
    if (Word == 0) {
      Result += 64U;
      continue;
    }
    return Result + static_cast<std::size_t>(std::countr_zero(Word));
  }
  return Result;
}

std::uint64_t APInt::word(std::size_t Index) const noexcept {
  return Index < Words.size() ? Words[Index] : 0;
}

int APInt::compare(const APInt &Other) const noexcept {
  const std::size_t LeftCount = significantWordCount(Words);
  const std::size_t RightCount = significantWordCount(Other.Words);
  if (LeftCount != RightCount)
    return LeftCount < RightCount ? -1 : 1;
  for (std::size_t Index = LeftCount; Index != 0; --Index) {
    if (Words[Index - 1U] != Other.Words[Index - 1U])
      return Words[Index - 1U] < Other.Words[Index - 1U] ? -1 : 1;
  }
  return 0;
}

int APInt::compareToShifted(const APInt &Other, std::size_t OtherShift) const noexcept {
  if (Other.isZero())
    return isZero() ? 0 : 1;
  if (isZero())
    return -1;
  const std::size_t LeftBits = activeBitWidth();
  const std::size_t RightBits = Other.activeBitWidth();
  if (OtherShift > std::numeric_limits<std::size_t>::max() - RightBits)
    return -1;
  const std::size_t ShiftedBits = RightBits + OtherShift;
  if (LeftBits != ShiftedBits)
    return LeftBits < ShiftedBits ? -1 : 1;
  return compare(Other.shiftedLeft(OtherShift));
}

void APInt::multiplySmall(std::uint32_t Multiplier) {
  if (Multiplier == 0 || isZero()) {
    Words.clear();
    BitWidth = 0;
    return;
  }
  unsigned __int128 Carry = 0;
  for (std::uint64_t &Word : Words) {
    const unsigned __int128 Product = static_cast<unsigned __int128>(Word) * Multiplier + Carry;
    Word = static_cast<std::uint64_t>(Product);
    Carry = Product >> 64U;
  }
  if (Carry != 0)
    Words.push_back(static_cast<std::uint64_t>(Carry));
  normalizeMagnitude();
}

void APInt::addSmall(std::uint32_t Value) {
  unsigned __int128 Carry = Value;
  std::size_t Index = 0;
  while (Carry != 0 && Index < Words.size()) {
    const unsigned __int128 Sum = static_cast<unsigned __int128>(Words[Index]) + Carry;
    Words[Index] = static_cast<std::uint64_t>(Sum);
    Carry = Sum >> 64U;
    ++Index;
  }
  if (Carry != 0)
    Words.push_back(static_cast<std::uint64_t>(Carry));
  normalizeMagnitude();
}

void APInt::setBit(std::size_t Bit) {
  const std::size_t Index = Bit / 64U;
  if (Index >= Words.size())
    Words.resize(Index + 1U, 0);
  Words[Index] |= std::uint64_t{1} << (Bit % 64U);
  normalizeMagnitude();
}

void APInt::clearBit(std::size_t Bit) noexcept {
  const std::size_t Index = Bit / 64U;
  if (Index >= Words.size())
    return;
  Words[Index] &= ~(std::uint64_t{1} << (Bit % 64U));
  normalizeMagnitude();
}

void APInt::shiftRight(std::size_t Amount) noexcept {
  if (Amount == 0 || isZero())
    return;
  const std::size_t WordOffset = Amount / 64U;
  if (WordOffset >= Words.size()) {
    Words.clear();
    BitWidth = 0;
    return;
  }
  const unsigned BitOffset = static_cast<unsigned>(Amount % 64U);
  const std::size_t NewSize = Words.size() - WordOffset;
  for (std::size_t Index = 0; Index < NewSize; ++Index) {
    std::uint64_t Value = Words[Index + WordOffset] >> BitOffset;
    if (BitOffset != 0 && Index + WordOffset + 1U < Words.size())
      Value |= Words[Index + WordOffset + 1U] << (64U - BitOffset);
    Words[Index] = Value;
  }
  Words.resize(NewSize);
  normalizeMagnitude();
}

APInt APInt::shiftedLeft(std::size_t Amount) const {
  if (isZero() || Amount == 0)
    return *this;
  const std::size_t WordOffset = Amount / 64U;
  const unsigned BitOffset = static_cast<unsigned>(Amount % 64U);
  APInt Result;
  Result.Words.assign(Words.size() + WordOffset + (BitOffset == 0 ? 0U : 1U), 0);
  for (std::size_t Index = 0; Index < Words.size(); ++Index) {
    const unsigned __int128 Shifted = static_cast<unsigned __int128>(Words[Index]) << BitOffset;
    Result.Words[Index + WordOffset] |= static_cast<std::uint64_t>(Shifted);
    if (BitOffset != 0)
      Result.Words[Index + WordOffset + 1U] |= static_cast<std::uint64_t>(Shifted >> 64U);
  }
  Result.normalizeMagnitude();
  return Result;
}

void APInt::subtract(const APInt &Other) noexcept {
  std::uint64_t Borrow = 0;
  for (std::size_t Index = 0; Index < Words.size(); ++Index) {
    const unsigned __int128 Subtrahend =
        (Index < Other.Words.size() ? static_cast<unsigned __int128>(Other.Words[Index]) : 0) +
        Borrow;
    const unsigned __int128 Current = Words[Index];
    Words[Index] = static_cast<std::uint64_t>(Current - Subtrahend);
    Borrow = Current < Subtrahend ? 1U : 0U;
  }
  normalizeMagnitude();
}

std::string APInt::toUnsignedDecimal() const {
  return unsignedDecimal(Words);
}

std::string APInt::toSignedDecimal() const {
  if (!isNegative())
    return toUnsignedDecimal();

  std::vector<std::uint64_t> Magnitude = Words;
  negateWords(Magnitude, BitWidth);
  return "-" + unsignedDecimal(std::move(Magnitude));
}

Expected<void> APInt::verify() const {
  return verifyAPInt(BitWidth, Words, ErrorCode::VerificationError);
}

void APInt::normalizeMagnitude() noexcept {
  while (!Words.empty() && Words.back() == 0)
    Words.pop_back();
  const std::size_t ActiveBits = activeBitWidth();
  BitWidth = static_cast<std::uint32_t>(ActiveBits);
}

} // namespace rcc
