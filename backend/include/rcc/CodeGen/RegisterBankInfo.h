// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_REGISTERBANKINFO_H
#define RCC_CODEGEN_REGISTERBANKINFO_H

#include "rcc/CodeGen/MachineIRTypes.h"
#include "rcc/CodeGen/Register.h"
#include "rcc/Support/Error.h"

#include <optional>
#include <vector>

namespace rcc {

class MachineFunction;
class MachineInstr;
class TargetDescription;
struct VirtualRegisterInfo;

struct RegisterBankSelection final {
  RegisterBankID Bank;
  bool IsConstrained = false;
};

using RegisterBankInstructionMapping = std::vector<std::optional<RegisterBankID>>;

class RegisterBankInfo {
public:
  virtual ~RegisterBankInfo() = default;

  [[nodiscard]] virtual Expected<RegisterBankSelection>
  selectBank(const MachineFunction &Function, Register Reg, const VirtualRegisterInfo &Info,
             const MachineInstr *Context, const TargetDescription &Target) const = 0;

  [[nodiscard]] virtual Expected<RegisterBankInstructionMapping>
  getInstructionMapping(const MachineInstr &, const MachineFunction &,
                        const TargetDescription &) const {
    return RegisterBankInstructionMapping{};
  }

  // This hook rejects invalid mappings before register-bank state is published.
  [[nodiscard]] virtual Expected<void> validateInstruction(const MachineInstr &,
                                                           const MachineFunction &,
                                                           const TargetDescription &) const {
    return {};
  }
};

} // namespace rcc

#endif // RCC_CODEGEN_REGISTERBANKINFO_H
