// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_TARGETFRAMELOWERING_H
#define RCC_CODEGEN_TARGETFRAMELOWERING_H

#include "rcc/CodeGen/MachineIRTypes.h"
#include "rcc/CodeGen/Register.h"
#include "rcc/Support/Error.h"

#include <cstddef>
#include <cstdint>

namespace rcc {

class MachineBasicBlock;
class MachineFunction;
class MachineInstr;
class RegisterScavenger;
class TargetDescription;

struct FrameIndexReference final {
  PhysicalRegister BaseRegister;
  std::int64_t Offset = 0;
};

struct FrameLoweringOptions final {
  bool EmitDebugOutput = false;
};

class TargetFrameLowering {
public:
  virtual ~TargetFrameLowering() = default;

  [[nodiscard]] virtual PhysicalRegister
  initialCFARegister(const MachineFunction &Function,
                     const TargetDescription &Target) const noexcept = 0;
  [[nodiscard]] virtual std::int64_t initialCFAOffset(const MachineFunction &) const noexcept {
    return 0;
  }

  [[nodiscard]] virtual Expected<void> prepareFrame(MachineFunction &Function,
                                                    const TargetDescription &Target,
                                                    FrameLoweringOptions Options = {}) const = 0;
  [[nodiscard]] virtual Expected<void>
  insertCalleeSavedSpillsAndRestores(MachineFunction &Function,
                                     const TargetDescription &Target) const = 0;
  [[nodiscard]] virtual Expected<void>
  processFunctionBeforeFrameFinalized(MachineFunction &, const TargetDescription &) const {
    return {};
  }
  [[nodiscard]] virtual Expected<void> finalizeFrame(MachineFunction &Function,
                                                     const TargetDescription &Target) const = 0;

  [[nodiscard]] virtual bool isCalleeSavedRegister(PhysicalRegister, const MachineFunction &,
                                                   const TargetDescription &) const noexcept {
    return false;
  }

  [[nodiscard]] virtual bool isCallFramePseudo(const MachineInstr &Instruction) const noexcept = 0;
  [[nodiscard]] virtual Expected<void>
  eliminateCallFramePseudo(MachineInstr &Instruction, MachineFunction &Function,
                           const TargetDescription &Target, RegisterScavenger &Scavenger) const = 0;

  [[nodiscard]] virtual Expected<void> emitPrologue(MachineFunction &Function,
                                                    const TargetDescription &Target,
                                                    RegisterScavenger &Scavenger) const = 0;
  [[nodiscard]] virtual bool
  isEpilogueTerminator(const MachineInstr &Instruction,
                       const TargetDescription &Target) const noexcept = 0;
  [[nodiscard]] virtual Expected<void>
  emitEpilogue(MachineBasicBlock &Block, MachineInstr &Terminator, MachineFunction &Function,
               const TargetDescription &Target, RegisterScavenger &Scavenger) const = 0;
  [[nodiscard]] virtual Expected<void>
  processFunctionBeforeFrameIndexElimination(MachineFunction &, const TargetDescription &) const {
    return {};
  }

  [[nodiscard]] virtual Expected<FrameIndexReference>
  getFrameIndexReference(const MachineFunction &Function, const MachineInstr &Instruction,
                         FrameIndex Index, std::int64_t OperandOffset) const = 0;
  [[nodiscard]] virtual Expected<void> eliminateFrameIndex(MachineInstr &Instruction,
                                                           std::size_t OperandIndex,
                                                           MachineFunction &Function,
                                                           const TargetDescription &Target,
                                                           RegisterScavenger &Scavenger) const = 0;
};

} // namespace rcc

#endif // RCC_CODEGEN_TARGETFRAMELOWERING_H
