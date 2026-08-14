// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_GLOBALISEL_GISELVALUETRACKING_H
#define RCC_CODEGEN_GLOBALISEL_GISELVALUETRACKING_H

#include "rcc/ADT/APInt.h"
#include "rcc/CodeGen/GlobalISel/GISelChangeObserver.h"
#include "rcc/CodeGen/Register.h"
#include "rcc/Support/Error.h"

#include <cstdint>

namespace rcc {

class MachineFunction;
class MachineRegisterInfo;
class TargetDescription;

class GISelValueTracking final : public GISelChangeObserver {
public:
  GISelValueTracking(MachineFunction &Function, const TargetDescription &Target,
                     unsigned MaxDepth = 6) noexcept;

  [[nodiscard]] const MachineFunction &getMachineFunction() const noexcept {
    return Function;
  }

  [[nodiscard]] Expected<APInt> getKnownZeroes(Register Reg) const;
  [[nodiscard]] Expected<std::uint32_t> computeNumSignBits(Register Reg, unsigned Depth = 0) const;

  void erasingInstr(MachineInstr &) override {}
  void createdInstr(MachineInstr &) override {}
  void changingInstr(MachineInstr &) override {}
  void changedInstr(MachineInstr &) override {}

private:
  struct KnownBits final {
    APInt Zero;
    APInt One;
  };

  [[nodiscard]] Expected<KnownBits> makeUnknown(std::uint32_t BitWidth) const;
  [[nodiscard]] Expected<KnownBits> getKnownBits(Register Reg, unsigned Depth = 0) const;

  MachineFunction &Function;
  MachineRegisterInfo &MRI;
  const TargetDescription &Target;
  unsigned MaxDepth;
};

} // namespace rcc

#endif // RCC_CODEGEN_GLOBALISEL_GISELVALUETRACKING_H
