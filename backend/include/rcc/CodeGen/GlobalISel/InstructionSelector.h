// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_GLOBALISEL_INSTRUCTIONSELECTOR_H
#define RCC_CODEGEN_GLOBALISEL_INSTRUCTIONSELECTOR_H

#include "rcc/CodeGen/GlobalISel/GIMatchTableExecutor.h"
#include "rcc/Support/Error.h"

namespace rcc {

class MachineFunction;
class MachineInstr;
class TargetDescription;

class InstructionSelector : public GIMatchTableExecutor {
public:
  virtual ~InstructionSelector() = default;

  [[nodiscard]] virtual Expected<void> select(MachineInstr &Instruction, MachineFunction &Function,
                                              const TargetDescription &Target) const = 0;

  [[nodiscard]] virtual Expected<void>
  constrainSelectedInstruction(MachineInstr &, MachineFunction &, const TargetDescription &) const {
    return {};
  }

  [[nodiscard]] Expected<void> propagateSelectionFlags(MachineInstr &Output,
                                                       MachineInstrFlags InputFlags,
                                                       bool InputMayRaiseFPException,
                                                       const TargetDescription &Target) const;

  [[nodiscard]] Expected<void> propagateSelectionFlags(MachineInstr &Output,
                                                       const MachineInstr &Input,
                                                       const TargetDescription &Target) const;

protected:
  [[nodiscard]] Expected<void>
  finalizeGeneratedInstruction(MachineInstr &Output, MachineFunction &Function,
                               const TargetDescription &Target, MachineInstrFlags InputFlags,
                               bool InputMayRaiseFPException) const override;
};

} // namespace rcc

#endif // RCC_CODEGEN_GLOBALISEL_INSTRUCTIONSELECTOR_H
