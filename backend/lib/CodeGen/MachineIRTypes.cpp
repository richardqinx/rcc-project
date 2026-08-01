// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineIRTypes.h"

#include <bit>

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

MachineInstrFlags::MachineInstrFlags(std::initializer_list<MachineInstrFlag> Flags) noexcept {
  for (const MachineInstrFlag Flag : Flags)
    set(Flag);
}

} // namespace rcc
