// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINEBLOCKPLACEMENT_H
#define RCC_CODEGEN_MACHINEBLOCKPLACEMENT_H

#include "rcc/CodeGen/MachinePassManager.h"

namespace rcc {

class MachineBlockPlacementPass final : public MachineFunctionPass {
public:
  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineFunctionAnalysisManager &Analyses) override;

private:
  [[nodiscard]] Expected<MachinePassResult>
  runImplementation(MachineFunction &Function, const TargetDescription &Target,
                    MachineFunctionAnalysisManager &Analyses, MachineActionReplay &Actions) const;
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINEBLOCKPLACEMENT_H
