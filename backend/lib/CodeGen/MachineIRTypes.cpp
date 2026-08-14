// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineIRTypes.h"

#include <algorithm>
#include <bit>
#include <cstdint>

namespace rcc {

Expected<Alignment> Alignment::create(std::uint64_t Bytes) {
  if (!std::has_single_bit(Bytes)) {
    return makeUnexpected(ErrorCode::InvalidArgument, "alignment must be a nonzero power of two");
  }
  return Alignment(Bytes);
}

Expected<BranchProbability> BranchProbability::fromFraction(std::uint32_t Numerator,
                                                            std::uint32_t Denominator) {
  if (Denominator == 0) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "branch probability denominator must be nonzero");
  }
  if (Numerator > Denominator) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "branch probability numerator cannot exceed its denominator");
  }

  const std::uint64_t ScaledNumerator = static_cast<std::uint64_t>(Numerator) * MaxRawValue;
  const std::uint64_t Rounded =
      (ScaledNumerator + static_cast<std::uint64_t>(Denominator / 2)) / Denominator;
  return BranchProbability(static_cast<std::uint32_t>(Rounded));
}

void BranchProbability::normalizeProbabilities(
    std::span<BranchProbability> Probabilities) noexcept {
  if (Probabilities.empty())
    return;

  std::uint64_t Sum = 0;
  for (const BranchProbability Probability : Probabilities)
    Sum += Probability.rawValue();

  if (Sum == 0) {
    const std::uint64_t Count = Probabilities.size();
    const auto Equal =
        static_cast<std::uint32_t>((static_cast<std::uint64_t>(MaxRawValue) + Count / 2U) / Count);
    std::ranges::fill(Probabilities, BranchProbability(Equal));
    return;
  }

  for (BranchProbability &Probability : Probabilities) {
    const std::uint64_t Scaled = static_cast<std::uint64_t>(Probability.rawValue()) * MaxRawValue;
    Probability = BranchProbability(static_cast<std::uint32_t>((Scaled + Sum / 2U) / Sum));
  }
}

MachineInstrFlags::MachineInstrFlags(std::initializer_list<MachineInstrFlag> Flags) noexcept {
  for (const MachineInstrFlag Flag : Flags)
    set(Flag);
}

} // namespace rcc
