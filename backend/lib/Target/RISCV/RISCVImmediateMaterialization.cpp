// SPDX-License-Identifier: GPL-3.0-or-later

#include "RISCVImmediateMaterialization.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace rcc::riscv {
namespace {

[[nodiscard]] constexpr bool fitsSigned(std::int64_t Value, std::uint32_t Bits) noexcept {
  const std::int64_t Minimum = -(std::int64_t{1} << (Bits - 1U));
  const std::int64_t Maximum = (std::int64_t{1} << (Bits - 1U)) - 1;
  return Value >= Minimum && Value <= Maximum;
}

[[nodiscard]] constexpr std::int64_t signExtend(std::uint64_t Value, std::uint32_t Bits) noexcept {
  const std::uint64_t Mask = (std::uint64_t{1} << Bits) - 1U;
  const std::uint64_t Sign = std::uint64_t{1} << (Bits - 1U);
  Value &= Mask;
  if ((Value & Sign) != 0)
    Value |= ~Mask;
  return std::bit_cast<std::int64_t>(Value);
}

[[nodiscard]] std::size_t baseBuildCost(std::int64_t Value) noexcept {
  const std::uint64_t Bits = static_cast<std::uint64_t>(Value);
  if (fitsSigned(Value, 32)) {
    const std::uint64_t High20 = ((Bits + 0x800U) >> 12U) & 0xFFFFFU;
    const std::int64_t Low12 = signExtend(Bits, 12);
    return static_cast<std::size_t>(High20 != 0) +
           static_cast<std::size_t>(Low12 != 0 || High20 == 0);
  }

  const std::int64_t Low12 = signExtend(Bits, 12);
  const std::uint64_t WithoutLow12 = Bits - static_cast<std::uint64_t>(Low12);
  std::int64_t Remaining = std::bit_cast<std::int64_t>(WithoutLow12);
  std::uint32_t Shift = 0;
  if (!fitsSigned(Remaining, 32)) {
    Shift = static_cast<std::uint32_t>(std::countr_zero(WithoutLow12));
    Remaining >>= Shift;

    if (Shift > 12 && !fitsSigned(Remaining, 12)) {
      const std::int64_t WithLUILowBits =
          std::bit_cast<std::int64_t>(static_cast<std::uint64_t>(Remaining) << 12U);
      if (fitsSigned(WithLUILowBits, 32)) {
        Shift -= 12;
        Remaining = WithLUILowBits;
      }
    }
  }

  return baseBuildCost(Remaining) + static_cast<std::size_t>(Shift != 0) +
         static_cast<std::size_t>(Low12 != 0);
}

[[nodiscard]] std::optional<std::size_t> leadingZeroBuildCost(std::uint64_t Value) noexcept {
  if (Value == 0 || (Value & (std::uint64_t{1} << 63U)) != 0)
    return std::nullopt;

  const std::uint32_t LeadingZeros = static_cast<std::uint32_t>(std::countl_zero(Value));
  const std::uint64_t Shifted = Value << LeadingZeros;
  const std::uint64_t LowMask = (std::uint64_t{1} << LeadingZeros) - 1U;
  std::optional<std::size_t> Best;
  for (const std::uint64_t Candidate : {Shifted | LowMask, Shifted}) {
    const std::size_t Cost = baseBuildCost(std::bit_cast<std::int64_t>(Candidate));
    if (Cost >= 8)
      continue;
    const std::size_t WithShift = Cost + 1U;
    Best = Best ? std::min(*Best, WithShift) : WithShift;
  }
  return Best;
}

[[nodiscard]] std::optional<std::size_t> twoRegisterBuildCost(std::int64_t Value) noexcept {
  const std::uint64_t Bits = static_cast<std::uint64_t>(Value);
  const std::int64_t Low = signExtend(Bits, 32);
  if (Low == 0)
    return std::nullopt;

  const std::uint64_t HighContribution = Bits - static_cast<std::uint64_t>(Low);
  if (HighContribution == 0)
    return std::nullopt;
  const std::uint32_t LowZeros =
      static_cast<std::uint32_t>(std::countr_zero(static_cast<std::uint64_t>(Low)));
  const std::uint32_t HighZeros = static_cast<std::uint32_t>(std::countr_zero(HighContribution));
  if (LowZeros >= 32 || HighZeros < 32 || HighZeros < LowZeros)
    return std::nullopt;
  const std::uint32_t Shift = HighZeros - LowZeros;
  if (Shift >= 64 || HighContribution != (static_cast<std::uint64_t>(Low) << Shift))
    return std::nullopt;
  return estimateRV64IntegerBuildCost(Low) + 2U;
}

} // namespace

std::size_t estimateRV64IntegerBuildCost(std::int64_t Value) noexcept {
  const std::uint64_t Bits = static_cast<std::uint64_t>(Value);
  std::size_t Best = baseBuildCost(Value);

  if ((Bits & 0xFFFU) != 0 && (Bits & 1U) == 0 && Best >= 2) {
    const std::uint32_t TrailingZeros = static_cast<std::uint32_t>(std::countr_zero(Bits));
    const std::int64_t Shifted = Value >> TrailingZeros;
    const std::size_t Candidate = baseBuildCost(Shifted) + 1U;
    if (Candidate < Best || fitsSigned(Shifted, 6))
      Best = Candidate;
  }
  if (Best <= 2)
    return Best;

  if ((Bits & 0xFFFU) != 0 && (Bits & 0x1800U) == 0x1000U) {
    const std::int64_t LastAdd = -static_cast<std::int64_t>(0x800U - (Bits & 0xFFFU));
    const std::uint64_t AdjustedBits = Bits - static_cast<std::uint64_t>(LastAdd);
    Best = std::min(Best, baseBuildCost(std::bit_cast<std::int64_t>(AdjustedBits)) + 1U);
  }

  if (Value > 0 && Best > 2) {
    if (const auto Candidate = leadingZeroBuildCost(Bits); Candidate && *Candidate < Best)
      Best = *Candidate;
  } else if (Value < 0 && Best > 3) {
    if (const auto Candidate = leadingZeroBuildCost(~Bits); Candidate && *Candidate + 1U < Best) {
      Best = *Candidate + 1U;
    }
  }
  return Best;
}

bool shouldUseRV64IntegerConstantPool(std::int64_t Value, std::size_t MaximumBuildCost) noexcept {
  if (fitsSigned(Value, 32) || estimateRV64IntegerBuildCost(Value) <= MaximumBuildCost)
    return false;
  const auto TwoRegisterCost = twoRegisterBuildCost(Value);
  return !TwoRegisterCost || *TwoRegisterCost > MaximumBuildCost;
}

} // namespace rcc::riscv
