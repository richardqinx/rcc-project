// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_TARGET_RISCV_RISCVINSTPRINTER_H
#define RCC_TARGET_RISCV_RISCVINSTPRINTER_H

#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/MC/MCInstPrinter.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace rcc::riscv {

enum class RISCVMCExprKind : std::uint32_t {
  CallPLT = 1,
  PCRelativeHigh = 2,
  PCRelativeLow = 3,
  AbsoluteHigh = 4,
  AbsoluteLow = 5,
  GOTPCRelativeHigh = 6,
};

class RISCVInstPrinter final : public mc::MCInstPrinter {
public:
  explicit RISCVInstPrinter(const TargetDescription &Target) noexcept : Target(&Target) {}

  [[nodiscard]] Expected<std::string>
  printInstruction(const mc::MCInst &Instruction) const override;
  [[nodiscard]] Expected<std::string>
  printTargetExpression(std::uint32_t Kind, std::string_view Operand) const override;

private:
  const TargetDescription *Target;
};

} // namespace rcc::riscv

#endif // RCC_TARGET_RISCV_RISCVINSTPRINTER_H
