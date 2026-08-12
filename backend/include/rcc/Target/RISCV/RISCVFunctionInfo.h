// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_TARGET_RISCV_RISCVFUNCTIONINFO_H
#define RCC_TARGET_RISCV_RISCVFUNCTIONINFO_H

#include "rcc/CodeGen/TargetFunctionState.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace rcc {

struct TargetMIRTextInfoDesc;

namespace riscv {

class RISCVFunctionInfo final {
public:
  RISCVFunctionInfo(std::int32_t VarArgsFrameIndex = 0, std::int32_t VarArgsSaveSize = 0) noexcept
      : VarArgsFrameIndex(VarArgsFrameIndex), VarArgsSaveSize(VarArgsSaveSize) {}

  [[nodiscard]] static constexpr TargetFunctionStateTypeID stableTypeID() noexcept {
    return TargetFunctionStateTypeID::fromValue(0x5243435256460001ULL);
  }

  [[nodiscard]] static constexpr std::uint32_t schemaVersion() noexcept {
    return 1;
  }

  [[nodiscard]] Expected<void> verify() const;
  [[nodiscard]] Expected<RISCVFunctionInfo> clone(const MachineFunctionCloneContext &Context) const;
  [[nodiscard]] std::string print() const;
  [[nodiscard]] Expected<std::vector<std::byte>> encode() const;
  [[nodiscard]] static Expected<RISCVFunctionInfo> decode(std::uint32_t Version,
                                                          std::span<const std::byte> Payload);

  [[nodiscard]] std::int32_t varArgsFrameIndex() const noexcept {
    return VarArgsFrameIndex;
  }

  [[nodiscard]] std::int32_t varArgsSaveSize() const noexcept {
    return VarArgsSaveSize;
  }

  friend bool operator==(const RISCVFunctionInfo &Left, const RISCVFunctionInfo &Right) = default;

private:
  std::int32_t VarArgsFrameIndex = 0;
  std::int32_t VarArgsSaveSize = 0;
};

void configureFunctionInfoMIRSchema(TargetMIRTextInfoDesc &Description);

} // namespace riscv
} // namespace rcc

#endif // RCC_TARGET_RISCV_RISCVFUNCTIONINFO_H
