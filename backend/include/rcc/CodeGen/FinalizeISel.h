// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_FINALIZEISEL_H
#define RCC_CODEGEN_FINALIZEISEL_H

#include "rcc/CodeGen/MachinePassManager.h"

namespace rcc {

class MachineBasicBlock;
class MachineInstr;

class TargetFinalizeISelInfo {
public:
  virtual ~TargetFinalizeISelInfo() = default;

  [[nodiscard]] virtual Expected<MachineBasicBlock *>
  emitInstrWithCustomInserter(MachineInstr &Instruction, MachineBasicBlock &Block,
                              MachineFunction &Function, const TargetDescription &Target) const = 0;
};

class FinalizeISelPass final : public MachineFunctionPass {
public:
  explicit FinalizeISelPass(const TargetFinalizeISelInfo &Info) noexcept : Info(&Info) {}

  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineFunctionAnalysisManager &Analyses) override;

private:
  const TargetFinalizeISelInfo *Info;
};

} // namespace rcc

#endif // RCC_CODEGEN_FINALIZEISEL_H
