// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_TARGET_RISCV_RISCVSUBTARGET_H
#define RCC_TARGET_RISCV_RISCVSUBTARGET_H

#include "rcc/Support/Error.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace rcc {

class InstructionSelector;
class LegalizerInfo;
class TargetCallLowering;

namespace riscv {

class RISCVRegisterBankInfo;

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

class RISCVSubtarget final {
public:
  explicit RISCVSubtarget(RISCVSubtargetFeatures Features);
  RISCVSubtarget(const RISCVSubtarget &) = delete;
  RISCVSubtarget &operator=(const RISCVSubtarget &) = delete;
  RISCVSubtarget(RISCVSubtarget &&) = delete;
  RISCVSubtarget &operator=(RISCVSubtarget &&) = delete;
  ~RISCVSubtarget();

  [[nodiscard]] const RISCVSubtargetFeatures &features() const noexcept {
    return Features;
  }

  [[nodiscard]] const TargetCallLowering *getCallLowering() const;
  [[nodiscard]] InstructionSelector *getInstructionSelector() const;
  [[nodiscard]] const LegalizerInfo *getLegalizerInfo() const;
  [[nodiscard]] const RISCVRegisterBankInfo *getRegBankInfo() const;

private:
  RISCVSubtargetFeatures Features;
  mutable std::unique_ptr<TargetCallLowering> CallLoweringInfo;
  mutable std::unique_ptr<InstructionSelector> InstSelector;
  mutable std::unique_ptr<LegalizerInfo> Legalizer;
  mutable std::unique_ptr<RISCVRegisterBankInfo> RegBankInfo;
};

} // namespace riscv
} // namespace rcc

#endif // RCC_TARGET_RISCV_RISCVSUBTARGET_H
