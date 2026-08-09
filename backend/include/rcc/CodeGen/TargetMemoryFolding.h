// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_TARGETMEMORYFOLDING_H
#define RCC_CODEGEN_TARGETMEMORYFOLDING_H

#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/Support/Error.h"

#include <cstddef>
#include <optional>

namespace rcc {

class MachineFunction;
class MachineInstr;
class TargetDescription;

class TargetMemoryFoldingInfo {
public:
  virtual ~TargetMemoryFoldingInfo() = default;

  [[nodiscard]] virtual Expected<bool>
  canFoldMemoryOperand(const MachineInstr &Consumer, std::size_t OperandIndex,
                       const MachineInstr &MemoryInstruction, const MachineFunction &Function,
                       const TargetDescription &Target) const = 0;

  [[nodiscard]] virtual Expected<MachineInstr *>
  foldMemoryOperand(MachineInstr &Consumer, std::size_t OperandIndex,
                    MachineInstr &MemoryInstruction, MachineFunction &Function,
                    const TargetDescription &Target) const = 0;

  [[nodiscard]] virtual Expected<bool>
  unfoldMemoryOperand(MachineInstr &Instruction, MachineFunction &Function,
                      const TargetDescription &Target) const = 0;

  [[nodiscard]] virtual Expected<std::optional<MemorySize>>
  getFoldedMemorySize(const MachineInstr &Instruction, const MachineFunction &Function,
                      const TargetDescription &Target) const = 0;
};

} // namespace rcc

#endif // RCC_CODEGEN_TARGETMEMORYFOLDING_H
