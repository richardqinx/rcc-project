// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MIRPRINTER_H
#define RCC_CODEGEN_MIRPRINTER_H

#include "rcc/CodeGen/MIRParser/MIRParser.h"
#include "rcc/Support/Error.h"

#include <string>
#include <vector>

namespace rcc {

class MachineFunction;
class MachineBasicBlock;
class MachineInstr;
class MachineOperand;
class TargetDescription;

namespace mir {

enum class MIPrintMode : unsigned char {
  Complete,
  Readable,
};

struct MIPrinterOptions final {
  MIPrintMode Mode = MIPrintMode::Complete;
};

[[nodiscard]] Expected<void> guessSuccessors(const MachineBasicBlock &Block,
                                             const TargetDescription &Target,
                                             std::vector<MachineBasicBlock *> &Result,
                                             bool &IsFallthrough);

[[nodiscard]] Expected<std::string> printMachineFunctionBody(const MachineFunction &Function,
                                                             const TargetDescription &Target,
                                                             const MIParserContext &Context = {},
                                                             MIPrinterOptions Options = {});

[[nodiscard]] Expected<MIParserContext>
createMachineFunctionPrintContext(const MachineFunction &Function);
[[nodiscard]] Expected<std::string> printMachineInstruction(const MachineInstr &Instruction,
                                                            const TargetDescription &Target,
                                                            const MIParserContext &Context,
                                                            MIPrinterOptions Options = {});
[[nodiscard]] Expected<std::string> printMachineOperand(const MachineOperand &Operand,
                                                        const TargetDescription &Target,
                                                        const MIParserContext &Context,
                                                        MIPrinterOptions Options = {});

struct MIRDocumentPrinterOptions final {
  MIPrintMode Mode = MIPrintMode::Complete;
  bool EmitEmbeddedIR = true;
  std::string DataLayout;
};

[[nodiscard]] Expected<std::string> printMIRDocument(const MIRDocument &Document,
                                                     const TargetDescription &Target,
                                                     MIRDocumentPrinterOptions Options = {});

} // namespace mir
} // namespace rcc

#endif // RCC_CODEGEN_MIRPRINTER_H
