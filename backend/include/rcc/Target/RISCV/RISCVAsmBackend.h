// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_TARGET_RISCV_RISCVASMBACKEND_H
#define RCC_TARGET_RISCV_RISCVASMBACKEND_H

#include "rcc/MC/MCAsmBackend.h"

namespace rcc::riscv {

class RISCVAsmBackend final : public mc::MCAsmBackend {
public:
  [[nodiscard]] Expected<mc::MCFixupKindInfo> fixupKindInfo(mc::MCFixupKind Kind) const override;
  [[nodiscard]] Expected<void> applyFixup(mc::MCFixupKind Kind, std::span<std::uint8_t> Data,
                                          std::uint64_t Offset, std::int64_t Value) const override;
  [[nodiscard]] Expected<std::uint32_t> relocationType(const mc::MCFixup &Fixup,
                                                       const mc::MCValue &Value) const override;
  [[nodiscard]] Expected<std::vector<mc::MCFixupRelocation>>
  relocationsForFixup(const mc::MCFixup &Fixup, const mc::MCValue &Value) const override;

  [[nodiscard]] bool mayNeedRelaxation(const mc::MCInst &Instruction) const noexcept override;
  [[nodiscard]] Expected<bool> fixupNeedsRelaxation(const mc::MCFixup &Fixup,
                                                    std::int64_t Value) const override;
  [[nodiscard]] Expected<mc::MCInst> relaxInstruction(const mc::MCInst &Instruction) const override;

  [[nodiscard]] std::uint32_t objectFlags() const noexcept override;
};

} // namespace rcc::riscv

#endif // RCC_TARGET_RISCV_RISCVASMBACKEND_H
