// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_CSECONFIGBASE_H
#define RCC_CODEGEN_CSECONFIGBASE_H

#include "rcc/CodeGen/MachineIRTypes.h"

namespace rcc {

class CSEConfigBase {
public:
  virtual ~CSEConfigBase() = default;

  [[nodiscard]] virtual bool shouldCSEOpcode(Opcode) const noexcept {
    return false;
  }
};

} // namespace rcc

#endif // RCC_CODEGEN_CSECONFIGBASE_H
