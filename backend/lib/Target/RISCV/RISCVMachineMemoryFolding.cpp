// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/Target/RISCV/RISCVCodeGen.h"

#include <cstddef>
#include <optional>

namespace rcc::riscv {
namespace {

class RISCVMachineMemoryFoldingInfo final : public TargetMemoryFoldingInfo {
public:
  [[nodiscard]] Expected<bool> canFoldMemoryOperand(const MachineInstr &, std::size_t,
                                                    const MachineInstr &, const MachineFunction &,
                                                    const TargetDescription &) const override {
    return false;
  }

  [[nodiscard]] Expected<MachineInstr *>
  foldMemoryOperand(MachineInstr &, std::size_t, MachineInstr &, MachineFunction &,
                    const TargetDescription &) const override {
    return makeUnexpected(ErrorCode::Unsupported,
                          "RV64GC has no register instruction form with a folded memory operand");
  }

  [[nodiscard]] Expected<bool> unfoldMemoryOperand(MachineInstr &, MachineFunction &,
                                                   const TargetDescription &) const override {
    return false;
  }

  [[nodiscard]] Expected<std::optional<MemorySize>>
  getFoldedMemorySize(const MachineInstr &, const MachineFunction &,
                      const TargetDescription &) const override {
    return std::optional<MemorySize>{};
  }
};

} // namespace

const TargetMemoryFoldingInfo &memoryFoldingInfo() noexcept {
  static const RISCVMachineMemoryFoldingInfo Value;
  return Value;
}

} // namespace rcc::riscv
