// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_TARGET_RISCV_RISCVSUBTARGET_H
#define RCC_TARGET_RISCV_RISCVSUBTARGET_H

#include "rcc/Support/Error.h"

#include <cstdint>
#include <span>
#include <string>

namespace rcc::riscv {

enum class RISCVExtension : std::uint8_t {
  I,
  M,
  A,
  F,
  D,
  C,
  Zicsr,
  Zifencei,
  Zacas,
  Zabha,
  Count,
};

class RISCVSubtargetFeatures final {
public:
  [[nodiscard]] static Expected<RISCVSubtargetFeatures>
  fromFeatureList(std::span<const std::string> Features);
  [[nodiscard]] static RISCVSubtargetFeatures rv64gc() noexcept;

  [[nodiscard]] bool has(RISCVExtension Extension) const noexcept;

  friend bool operator==(const RISCVSubtargetFeatures &Left,
                         const RISCVSubtargetFeatures &Right) = default;

private:
  explicit constexpr RISCVSubtargetFeatures(std::uint16_t Enabled) noexcept : Enabled(Enabled) {}

  std::uint16_t Enabled = 0;
};

} // namespace rcc::riscv

#endif // RCC_TARGET_RISCV_RISCVSUBTARGET_H
