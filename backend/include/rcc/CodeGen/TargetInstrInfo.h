// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_TARGETINSTRINFO_H
#define RCC_CODEGEN_TARGETINSTRINFO_H

#include "rcc/CodeGen/MachineIRTypes.h"
#include "rcc/CodeGen/Register.h"
#include "rcc/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace rcc {

class MachineBasicBlock;
class MachineFunction;
class MachineInstr;
class MachineOperand;
class TargetDescription;

struct MachineCopyOperands final {
  MachineOperand *Destination = nullptr;
  MachineOperand *Source = nullptr;
};

struct MachineBranchAnalysis final {
  MachineInstr *Conditional = nullptr;
  MachineInstr *Unconditional = nullptr;
  MachineBasicBlock *TrueTarget = nullptr;
  MachineBasicBlock *FalseTarget = nullptr;
  bool HasUnanalyzableTerminator = false;
};

class TargetInstrInfo {
public:
  virtual ~TargetInstrInfo();

  [[nodiscard]] virtual Expected<MachineBranchAnalysis>
  analyzeBranch(MachineBasicBlock &Block, MachineFunction &Function,
                const TargetDescription &Target) const = 0;
  [[nodiscard]] virtual Expected<void>
  reverseBranchCondition(MachineInstr &Branch, MachineBasicBlock &NewTarget,
                         MachineFunction &Function, const TargetDescription &Target) const = 0;
  [[nodiscard]] virtual Expected<void> removeBranch(MachineInstr &Branch, MachineFunction &Function,
                                                    const TargetDescription &Target) const = 0;
  [[nodiscard]] virtual Expected<bool>
  rewriteBranchTarget(MachineBasicBlock &Block, MachineBasicBlock &OldTarget,
                      MachineBasicBlock &NewTarget, MachineFunction &Function,
                      const TargetDescription &Target) const = 0;
  [[nodiscard]] virtual Expected<MachineInstr *>
  insertBranch(MachineBasicBlock &Block, MachineBasicBlock &TargetBlock, MachineFunction &Function,
               const TargetDescription &Target) const = 0;

  [[nodiscard]] virtual Expected<bool> isFallthroughEdge(const MachineBasicBlock &From,
                                                         const MachineBasicBlock &To,
                                                         const MachineFunction &Function,
                                                         const TargetDescription &Target) const = 0;

  [[nodiscard]] virtual Expected<void> validateCopy(Register Destination,
                                                    SubRegIndex DestinationSubReg, Register Source,
                                                    SubRegIndex SourceSubReg,
                                                    const MachineFunction &Function,
                                                    const TargetDescription &Target) const = 0;

  [[nodiscard]] virtual std::optional<MachineCopyOperands>
  getCopyOperands(MachineInstr &) const noexcept {
    return std::nullopt;
  }

  [[nodiscard]] virtual Expected<bool> simplifyInstruction(MachineInstr &, MachineFunction &,
                                                           const TargetDescription &) const {
    return false;
  }

  [[nodiscard]] virtual Expected<bool> tryConvertToThreeAddress(MachineInstr &, std::size_t,
                                                                std::size_t, MachineFunction &,
                                                                const TargetDescription &) const {
    return false;
  }

  [[nodiscard]] virtual Expected<std::uint32_t>
  instructionSize(const MachineInstr &Instruction, const MachineFunction &Function,
                  const TargetDescription &Target) const = 0;
  [[nodiscard]] virtual Expected<MachineBasicBlock *>
  branchTarget(MachineInstr &Instruction, MachineFunction &Function,
               const TargetDescription &Target) const = 0;
  [[nodiscard]] virtual Expected<std::optional<bool>>
  evaluateBranchCondition(const MachineInstr &, const MachineFunction &,
                          const TargetDescription &) const {
    return std::optional<bool>{};
  }
  [[nodiscard]] virtual bool isBranchOffsetInRange(const MachineInstr &Instruction,
                                                   std::int64_t Offset) const noexcept = 0;
  [[nodiscard]] virtual Expected<bool> relaxBranch(MachineInstr &Instruction, std::int64_t Offset,
                                                   MachineFunction &Function,
                                                   const TargetDescription &Target) const = 0;
};

} // namespace rcc

#endif // RCC_CODEGEN_TARGETINSTRINFO_H
