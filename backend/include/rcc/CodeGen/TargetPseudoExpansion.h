// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_TARGETPSEUDOEXPANSION_H
#define RCC_CODEGEN_TARGETPSEUDOEXPANSION_H

#include "rcc/Support/Error.h"

#include <cstdint>

namespace rcc {

class MachineFunction;
class MachineInstr;
class TargetDescription;

enum class MachinePseudoExpansionPhase : std::uint8_t {
  PreRA,
  PrePEI,
  PostRA,
  TargetLate,
};

class TargetPseudoExpansionInfo {
public:
  virtual ~TargetPseudoExpansionInfo() = default;

  [[nodiscard]] virtual Expected<bool> preparePostRAPseudo(MachineInstr &, MachineFunction &,
                                                           const TargetDescription &) const {
    return false;
  }

  [[nodiscard]] virtual Expected<bool> expandPseudo(MachineInstr &, MachinePseudoExpansionPhase,
                                                    MachineFunction &,
                                                    const TargetDescription &) const {
    return false;
  }
};

} // namespace rcc

#endif // RCC_CODEGEN_TARGETPSEUDOEXPANSION_H
