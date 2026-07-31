// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINECFGUPDATER_H
#define RCC_CODEGEN_MACHINECFGUPDATER_H

#include "rcc/CodeGen/MachineIRTypes.h"
#include "rcc/Support/Error.h"

namespace rcc {

class MachineBasicBlock;
class MachineFunction;

class MachineCFGUpdater final {
public:
  explicit MachineCFGUpdater(MachineFunction &Function) noexcept : Function(&Function) {}

  [[nodiscard]] Expected<void>
  addSuccessor(MachineBasicBlock &From, MachineBasicBlock &To,
               BranchProbability Probability = BranchProbability::zero());
  [[nodiscard]] Expected<void> removeSuccessor(MachineBasicBlock &From, MachineBasicBlock &To);
  [[nodiscard]] Expected<void> replaceSuccessor(MachineBasicBlock &From,
                                                MachineBasicBlock &OldTarget,
                                                MachineBasicBlock &NewTarget);
  [[nodiscard]] Expected<void> updateBranchProbability(MachineBasicBlock &From,
                                                       MachineBasicBlock &To,
                                                       BranchProbability Probability);
  [[nodiscard]] Expected<void> transferSuccessors(MachineBasicBlock &From, MachineBasicBlock &To);
  [[nodiscard]] Expected<void> disconnectBlock(MachineBasicBlock &Block);

private:
  [[nodiscard]] Expected<void> verifyOwnership(const MachineBasicBlock &Block) const;

  MachineFunction *Function;
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINECFGUPDATER_H
