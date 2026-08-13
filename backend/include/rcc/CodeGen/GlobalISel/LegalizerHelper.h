// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_GLOBALISEL_LEGALIZERHELPER_H
#define RCC_CODEGEN_GLOBALISEL_LEGALIZERHELPER_H

#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/GlobalISel/LegalizerInfo.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/Support/Error.h"

namespace rcc {

class MachineFunction;
class MachineInstr;
class GISelChangeObserver;

class LegalizerHelper final {
public:
  MachineIRBuilder &MIRBuilder;
  GISelChangeObserver &Observer;

  LegalizerHelper(MachineFunction &Function, const LegalizerInfo &Info,
                  GISelChangeObserver &Observer, MachineIRBuilder &MIRBuilder);

  [[nodiscard]] Expected<void> legalize(MachineInstr &Instruction,
                                        const LegalizationDecision &Decision);
  [[nodiscard]] Expected<void> lower(MachineInstr &Instruction,
                                     const LegalizationDecision &Decision);
  [[nodiscard]] Expected<void> lowerVAArg(MachineInstr &Instruction);
  [[nodiscard]] Expected<void> widenScalar(MachineInstr &Instruction, std::size_t TypeIndex,
                                           LowLevelType WideType);
  [[nodiscard]] Expected<void> widenScalarSrc(MachineInstr &Instruction, LowLevelType WideType,
                                              std::size_t OperandIndex,
                                              GenericOpcode ExtensionOpcode);
  [[nodiscard]] Expected<void> widenScalarDst(MachineInstr &Instruction, LowLevelType WideType,
                                              std::size_t OperandIndex = 0,
                                              GenericOpcode TruncOpcode = GenericOpcode::G_TRUNC);

private:
  MachineFunction &Function;
  MachineRegisterInfo &MRI;
  const LegalizerInfo &Info;
};

} // namespace rcc

#endif // RCC_CODEGEN_GLOBALISEL_LEGALIZERHELPER_H
