// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_MIR_MIPRINTER_H
#define RCC_MIR_MIPRINTER_H

#include "rcc/MIR/MIParser.h"
#include "rcc/Support/Error.h"

#include <string>

namespace rcc {

class MachineFunction;
class TargetDescription;

namespace mir {

enum class MIPrintMode : unsigned char {
  Complete,
  Readable,
};

struct MIPrinterOptions final {
  MIPrintMode Mode = MIPrintMode::Complete;
};

[[nodiscard]] Expected<std::string> printMachineFunctionBody(const MachineFunction &Function,
                                                             const TargetDescription &Target,
                                                             const MIParserContext &Context = {},
                                                             MIPrinterOptions Options = {});

} // namespace mir
} // namespace rcc

#endif // RCC_MIR_MIPRINTER_H
