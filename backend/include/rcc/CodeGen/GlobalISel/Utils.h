// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_GLOBALISEL_UTILS_H
#define RCC_CODEGEN_GLOBALISEL_UTILS_H

#include "rcc/ADT/APInt.h"
#include "rcc/CodeGen/LowLevelType.h"
#include "rcc/CodeGen/Register.h"
#include "rcc/Support/Error.h"

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace rcc {

class MachineIRBuilder;
class MachineInstr;
class MachineRegisterInfo;
class TargetDescription;

struct ValueAndVReg final {
  APInt Value;
  Register VReg;
};

[[nodiscard]] std::optional<ValueAndVReg>
getIConstantVRegValWithLookThrough(Register VReg, const MachineRegisterInfo &MRI,
                                   bool LookThroughInstrs = true);

/// Check whether an instruction only defines dead virtual registers and has no
/// observable side effects.
[[nodiscard]] bool isTriviallyDead(const MachineInstr &Instruction, const MachineRegisterInfo &MRI,
                                   const TargetDescription &Target) noexcept;

[[nodiscard]] Expected<void> eraseInstrs(std::span<MachineInstr *const> Instructions,
                                         MachineRegisterInfo &MRI, const TargetDescription &Target);

[[nodiscard]] Expected<void> eraseInstr(MachineInstr &Instruction, MachineRegisterInfo &MRI,
                                        const TargetDescription &Target);

[[nodiscard]] Expected<void> extractParts(Register Reg, LowLevelType Type, int PartCount,
                                          std::vector<Register> &Parts,
                                          MachineIRBuilder &MIRBuilder, MachineRegisterInfo &MRI);

[[nodiscard]] Expected<bool> extractParts(Register Reg, LowLevelType RegisterType,
                                          LowLevelType MainType, LowLevelType &LeftoverType,
                                          std::vector<Register> &Parts,
                                          std::vector<Register> &LeftoverParts,
                                          MachineIRBuilder &MIRBuilder, MachineRegisterInfo &MRI);

[[nodiscard]] Expected<LowLevelType> getLCMType(LowLevelType OriginalType, LowLevelType TargetType);
[[nodiscard]] Expected<LowLevelType> getGCDType(LowLevelType OriginalType, LowLevelType TargetType);

} // namespace rcc

#endif // RCC_CODEGEN_GLOBALISEL_UTILS_H
