// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_REGISTERSCAVENGER_H
#define RCC_CODEGEN_REGISTERSCAVENGER_H

#include "rcc/CodeGen/Register.h"
#include "rcc/Support/Error.h"

#include <span>

namespace rcc {

class MachineFunction;
class MachineInstr;
class TargetDescription;

struct ScavengedRegister final {
  PhysicalRegister Reg;
  bool RequiresSpill = false;
};

class RegisterScavenger final {
public:
  RegisterScavenger(const MachineFunction &Function, const TargetDescription &Target) noexcept
      : Function(&Function), Target(&Target) {}

  [[nodiscard]] Expected<ScavengedRegister>
  scavenge(const MachineInstr &Instruction, std::span<const PhysicalRegister> Candidates) const;

private:
  [[nodiscard]] Expected<bool> isLiveBefore(const MachineInstr &Instruction,
                                            PhysicalRegister Reg) const;
  [[nodiscard]] bool instructionUses(const MachineInstr &Instruction,
                                     PhysicalRegister Reg) const noexcept;

  const MachineFunction *Function;
  const TargetDescription *Target;
};

} // namespace rcc

#endif // RCC_CODEGEN_REGISTERSCAVENGER_H
