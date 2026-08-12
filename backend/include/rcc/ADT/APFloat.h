// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_ADT_APFLOAT_H
#define RCC_ADT_APFLOAT_H

#include "rcc/Support/Error.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace rcc {

enum class FloatSemantics : std::uint8_t {
  IEEEHalf,
  BFloat,
  IEEESingle,
  IEEEDouble,
  IEEEQuad,
  PPCDoubleDouble,
  Float8E5M2,
  Float8E5M2FNUZ,
  Float8E4M3,
  Float8E4M3FN,
  Float8E4M3FNUZ,
  Float8E4M3B11FNUZ,
  Float8E3M4,
  FloatTF32,
  Float8E8M0FNU,
  Float6E3M2FN,
  Float6E2M3FN,
  Float4E2M1FN,
  X87DoubleExtended,
  Count,
};

[[nodiscard]] std::optional<std::uint32_t>
getFloatSemanticsBitWidth(FloatSemantics Semantics) noexcept;
[[nodiscard]] std::string_view getFloatSemanticsSpelling(FloatSemantics Semantics) noexcept;

class APFloat final {
public:
  [[nodiscard]] static Expected<APFloat> create(FloatSemantics Semantics,
                                                std::span<const std::uint64_t> Words);
  [[nodiscard]] static Expected<APFloat> parse(FloatSemantics Semantics, std::string_view Text);

  [[nodiscard]] FloatSemantics semantics() const noexcept {
    return Semantics;
  }

  [[nodiscard]] std::uint32_t bitWidth() const noexcept {
    return getFloatSemanticsBitWidth(Semantics).value_or(0);
  }

  [[nodiscard]] std::span<const std::uint64_t> words() const noexcept {
    return std::span(Words).first(WordCount);
  }

  [[nodiscard]] std::string toString() const;

  friend bool operator==(const APFloat &Left, const APFloat &Right) = default;

private:
  APFloat(FloatSemantics Semantics, std::array<std::uint64_t, 2> Words,
          std::uint8_t WordCount) noexcept
      : Semantics(Semantics), Words(Words), WordCount(WordCount) {}

  FloatSemantics Semantics;
  std::array<std::uint64_t, 2> Words;
  std::uint8_t WordCount;
};

} // namespace rcc

#endif // RCC_ADT_APFLOAT_H
