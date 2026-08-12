// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_ADT_APINT_H
#define RCC_ADT_APINT_H

#include "rcc/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rcc {

class APInt final {
public:
  static constexpr std::uint32_t MaxBitWidth = std::uint32_t{1} << 23U;

  APInt() = default;
  explicit APInt(std::uint64_t Value);

  [[nodiscard]] static Expected<APInt> create(std::uint32_t BitWidth,
                                              std::span<const std::uint64_t> Words);
  [[nodiscard]] static Expected<APInt> fromUnsigned(std::uint32_t BitWidth, std::uint64_t Value);
  [[nodiscard]] static Expected<APInt> fromSigned(std::uint32_t BitWidth, std::int64_t Value);
  [[nodiscard]] static Expected<APInt> parseDecimal(std::uint32_t BitWidth, std::string_view Text);
  [[nodiscard]] static Expected<APInt> parseLLVMHex(std::uint32_t BitWidth, std::string_view Text);

  [[nodiscard]] std::uint32_t bitWidth() const noexcept {
    return BitWidth;
  }

  [[nodiscard]] std::span<const std::uint64_t> words() const noexcept {
    return Words;
  }

  [[nodiscard]] bool isZero() const noexcept;
  [[nodiscard]] bool isNegative() const noexcept;
  [[nodiscard]] std::size_t activeBitWidth() const noexcept;
  [[nodiscard]] std::size_t countrZero() const noexcept;
  [[nodiscard]] std::uint64_t word(std::size_t Index) const noexcept;
  [[nodiscard]] int compare(const APInt &Other) const noexcept;
  [[nodiscard]] int compareToShifted(const APInt &Other, std::size_t OtherShift) const noexcept;

  void multiplySmall(std::uint32_t Multiplier);
  void addSmall(std::uint32_t Value);
  void setBit(std::size_t Bit);
  void clearBit(std::size_t Bit) noexcept;
  void shiftRight(std::size_t Amount) noexcept;
  [[nodiscard]] APInt shiftedLeft(std::size_t Amount) const;
  void subtract(const APInt &Other) noexcept;

  [[nodiscard]] std::string toUnsignedDecimal() const;
  [[nodiscard]] std::string toSignedDecimal() const;
  [[nodiscard]] Expected<void> verify() const;

  friend bool operator==(const APInt &Left, const APInt &Right) = default;

private:
  APInt(std::uint32_t BitWidth, std::vector<std::uint64_t> Words)
      : BitWidth(BitWidth), Words(std::move(Words)) {}

  void normalizeMagnitude() noexcept;

  std::uint32_t BitWidth = 0;
  std::vector<std::uint64_t> Words;
};

} // namespace rcc

#endif // RCC_ADT_APINT_H
