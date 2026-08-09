// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_TARGETREGISTERALLOCATIONINFO_H
#define RCC_CODEGEN_TARGETREGISTERALLOCATIONINFO_H

#include "rcc/CodeGen/MachineFrameInfo.h"
#include "rcc/CodeGen/MachineLiveness.h"
#include "rcc/CodeGen/TargetRegisterInfo.h"

#include <cstdint>

namespace rcc {

class MachineIRBuilder;
class MachineInstr;
class TargetDescription;

class TargetRegisterAllocationInfo {
public:
  virtual ~TargetRegisterAllocationInfo() = default;

  [[nodiscard]] virtual Expected<void>
  validateSpillReload(RegisterClassID Class, const TargetDescription &Target) const = 0;
  [[nodiscard]] virtual Expected<MachineInstr *>
  emitReload(MachineIRBuilder &Builder, Register Destination, RegisterClassID Class,
             FrameIndex StackSlot, LaneMask Lanes) const = 0;
  [[nodiscard]] virtual Expected<MachineInstr *> emitSpill(MachineIRBuilder &Builder,
                                                           Register Source, RegisterClassID Class,
                                                           FrameIndex StackSlot,
                                                           LaneMask Lanes) const = 0;

  [[nodiscard]] virtual Expected<bool> isRematerializable(const MachineInstr &, Register,
                                                          const MachineFunction &,
                                                          const TargetDescription &) const {
    return false;
  }
  [[nodiscard]] virtual Expected<MachineInstr *> rematerialize(MachineIRBuilder &,
                                                               const MachineInstr &, Register,
                                                               Register,
                                                               const TargetDescription &) const {
    return makeUnexpected(ErrorCode::InternalError,
                          "target accepted rematerialization without implementing it");
  }

  [[nodiscard]] virtual std::uint64_t physicalRegisterCost(const MachineFunction &, Register,
                                                           PhysicalRegister, RegisterClassID,
                                                           bool) const noexcept {
    return 0;
  }
  [[nodiscard]] virtual std::uint64_t spillCost(const MachineFunction &, const LiveInterval &,
                                                RegisterClassID) const noexcept {
    return 1;
  }

  [[nodiscard]] virtual bool isReservedForFunction(const MachineFunction &, PhysicalRegister,
                                                   const TargetDescription &) const noexcept {
    return false;
  }
};

} // namespace rcc

#endif // RCC_CODEGEN_TARGETREGISTERALLOCATIONINFO_H
