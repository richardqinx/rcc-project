// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_TARGETCODEGEN_H
#define RCC_CODEGEN_TARGETCODEGEN_H

#include "rcc/Support/Error.h"

#include <cstdint>

namespace rcc {

class MachineFunction;
class MachineInstr;
class TargetDescription;
struct VirtualRegisterInfo;

enum class LegalizationAction : std::uint8_t {
  Legal,
  Unsupported,
  Custom,
  Lower,
  Libcall,
  WidenScalar,
  NarrowScalar,
  MoreElements,
  FewerElements,
  Bitcast,
  ChangeElementType,
  ChangeAddressSpace,
};

class TargetLegalizerInfo {
public:
  virtual ~TargetLegalizerInfo() = default;

  [[nodiscard]] virtual Expected<LegalizationAction>
  classify(const MachineInstr &Instruction, const MachineFunction &Function,
           const TargetDescription &Target) const = 0;

  [[nodiscard]] virtual Expected<void> lower(MachineInstr &Instruction, MachineFunction &Function,
                                             const TargetDescription &Target,
                                             LegalizationAction Action) const = 0;

  // This hook performs read-only target checks before a lowering mutation is committed.
  [[nodiscard]] virtual Expected<void> validateLowering(const MachineInstr &,
                                                        const MachineFunction &,
                                                        const TargetDescription &,
                                                        LegalizationAction) const {
    return {};
  }
};

class TargetRegisterBankInfo {
public:
  virtual ~TargetRegisterBankInfo() = default;

  [[nodiscard]] virtual Expected<RegisterBankID>
  selectBank(const MachineFunction &Function, Register Reg, const VirtualRegisterInfo &Info,
             const MachineInstr *Context, const TargetDescription &Target) const = 0;

  [[nodiscard]] virtual Expected<void>
  constrainInstruction(MachineInstr &Instruction, MachineFunction &Function,
                       const TargetDescription &Target) const = 0;

  // This hook must not mutate MachineIR. It allows the pass to reject a bad mapping
  // before publishing any bank or class assignment.
  [[nodiscard]] virtual Expected<void> validateInstruction(const MachineInstr &,
                                                           const MachineFunction &,
                                                           const TargetDescription &) const {
    return {};
  }
};

class TargetInstructionSelector {
public:
  virtual ~TargetInstructionSelector() = default;

  [[nodiscard]] virtual Expected<void> select(MachineInstr &Instruction, MachineFunction &Function,
                                              const TargetDescription &Target) const = 0;

  // Selection matching is read-only and must complete before select() mutates MachineIR.
  [[nodiscard]] virtual Expected<void> canSelect(const MachineInstr &, const MachineFunction &,
                                                 const TargetDescription &) const {
    return {};
  }

  [[nodiscard]] virtual Expected<void>
  constrainSelectedInstruction(MachineInstr &, MachineFunction &, const TargetDescription &) const {
    return {};
  }
};

} // namespace rcc

#endif // RCC_CODEGEN_TARGETCODEGEN_H
