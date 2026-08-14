// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_GLOBALISEL_LEGALIZERHELPER_H
#define RCC_CODEGEN_GLOBALISEL_LEGALIZERHELPER_H

#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/GlobalISel/LegalizerInfo.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/Support/Error.h"

#include <vector>

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
  [[nodiscard]] Expected<void> lowerAbsToAddXor(MachineInstr &Instruction);
  [[nodiscard]] Expected<void> lowerVAArg(MachineInstr &Instruction);
  [[nodiscard]] Expected<void> widenScalar(MachineInstr &Instruction, std::size_t TypeIndex,
                                           LowLevelType WideType);
  [[nodiscard]] Expected<void> widenScalarSrc(MachineInstr &Instruction, LowLevelType WideType,
                                              std::size_t OperandIndex,
                                              GenericOpcode ExtensionOpcode);
  [[nodiscard]] Expected<void> widenScalarDst(MachineInstr &Instruction, LowLevelType WideType,
                                              std::size_t OperandIndex = 0,
                                              GenericOpcode TruncOpcode = GenericOpcode::G_TRUNC);

  [[nodiscard]] Expected<void> narrowScalar(MachineInstr &Instruction, std::size_t TypeIndex,
                                            LowLevelType NarrowType);

private:
  [[nodiscard]] Expected<void> widenScalarMergeValues(MachineInstr &Instruction,
                                                      std::size_t TypeIndex, LowLevelType WideType);
  [[nodiscard]] Expected<void>
  widenScalarUnmergeValues(MachineInstr &Instruction, std::size_t TypeIndex, LowLevelType WideType);
  [[nodiscard]] Expected<void> insertParts(Register Destination, LowLevelType ResultType,
                                           LowLevelType PartType,
                                           const std::vector<Register> &PartRegisters,
                                           LowLevelType LeftoverType = {},
                                           const std::vector<Register> &LeftoverRegisters = {});
  [[nodiscard]] Expected<void> extractGCDType(std::vector<Register> &Parts, LowLevelType GCDType,
                                              Register Source);
  [[nodiscard]] Expected<LowLevelType> extractGCDType(std::vector<Register> &Parts,
                                                      LowLevelType DestinationType,
                                                      LowLevelType NarrowType, Register Source);
  [[nodiscard]] Expected<LowLevelType>
  buildLCMMergePieces(LowLevelType DestinationType, LowLevelType NarrowType, LowLevelType GCDType,
                      std::vector<Register> &Registers,
                      GenericOpcode PadStrategy = GenericOpcode::G_ANYEXT);
  [[nodiscard]] Expected<void>
  buildWidenedRemergeToDst(Register Destination, LowLevelType LCMType,
                           const std::vector<Register> &RemergeRegisters);
  [[nodiscard]] Expected<void> narrowScalarAddSub(MachineInstr &Instruction, std::size_t TypeIndex,
                                                  LowLevelType NarrowType);
  [[nodiscard]] Expected<void> narrowScalarExt(MachineInstr &Instruction, std::size_t TypeIndex,
                                               LowLevelType NarrowType);

  MachineFunction &Function;
  MachineRegisterInfo &MRI;
  const LegalizerInfo &Info;
};

} // namespace rcc

#endif // RCC_CODEGEN_GLOBALISEL_LEGALIZERHELPER_H
