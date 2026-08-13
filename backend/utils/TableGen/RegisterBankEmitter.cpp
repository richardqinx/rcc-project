// SPDX-License-Identifier: GPL-3.0-or-later

#include "TableGenBackends.h"

#include <ranges>
#include <sstream>
#include <string>

namespace rcc::targetgen {

std::string emitRegisterBanks(const NormalizedTargetModel &Model) {
  std::ostringstream Output;
  for (const NormalizedRegisterBank &Bank : Model.RegisterBanks) {
    Output << "      {RegisterBankID::fromValue(" << Bank.ID << "), ";
    appendQuoted(Output, Bank.Name);
    Output << ", " << Bank.MaximumSizeInBits << ", {";
    for (const std::uint32_t ClassID : Bank.RegisterClasses) {
      const auto Position =
          std::ranges::find(Model.RegisterClasses, ClassID, &NormalizedRegisterClass::ID);
      Output << "RegisterClassID::fromValue(" << Position->ID << "), ";
    }
    Output << "}},\n";
  }
  return Output.str();
}

} // namespace rcc::targetgen
