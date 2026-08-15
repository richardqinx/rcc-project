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
struct LibcallLoweringInfo;

class LegalizerHelper final {
public:
  MachineIRBuilder &MIRBuilder;
  GISelChangeObserver &Observer;

  LegalizerHelper(MachineFunction &Function, const LegalizerInfo &Info,
                  GISelChangeObserver &Observer, MachineIRBuilder &MIRBuilder,
                  const LibcallLoweringInfo *Libcalls = nullptr);

  [[nodiscard]] Expected<void> legalize(MachineInstr &Instruction,
                                        const LegalizationDecision &Decision);
  [[nodiscard]] Expected<void> lower(MachineInstr &Instruction,
                                     const LegalizationDecision &Decision);
  [[nodiscard]] Expected<void> lowerAbsToAddXor(MachineInstr &Instruction);
  [[nodiscard]] Expected<void> lowerAbsDiffToSelect(MachineInstr &Instruction);
  [[nodiscard]] Expected<void> lowerAbsDiffToMinMax(MachineInstr &Instruction);
  [[nodiscard]] Expected<void> lowerMinMax(MachineInstr &Instruction);
  [[nodiscard]] Expected<void> lowerThreewayCompare(MachineInstr &Instruction);
  [[nodiscard]] Expected<void> lowerDivRem(MachineInstr &Instruction);
  [[nodiscard]] Expected<void> lowerHighMultiply(MachineInstr &Instruction);
  [[nodiscard]] Expected<void> lowerBitCount(MachineInstr &Instruction);
  [[nodiscard]] Expected<void> lowerBswap(MachineInstr &Instruction);
  [[nodiscard]] Expected<void> lowerFunnelShift(MachineInstr &Instruction);
  [[nodiscard]] Expected<void> lowerRotate(MachineInstr &Instruction);
  [[nodiscard]] Expected<void> lowerDynStackAlloc(MachineInstr &Instruction);
  [[nodiscard]] Expected<void> lowerStackSave(MachineInstr &Instruction);
  [[nodiscard]] Expected<void> lowerStackRestore(MachineInstr &Instruction);
  [[nodiscard]] Expected<void> lowerVAArg(MachineInstr &Instruction);
  [[nodiscard]] Expected<void> libcall(MachineInstr &Instruction);
  [[nodiscard]] Expected<void> widenScalar(MachineInstr &Instruction, std::size_t TypeIndex,
                                           LowLevelType WideType);
  [[nodiscard]] Expected<void> widenScalarSrc(MachineInstr &Instruction, LowLevelType WideType,
                                              std::size_t OperandIndex,
                                              GenericOpcode ExtensionOpcode);
  [[nodiscard]] Expected<void> narrowScalarSrc(MachineInstr &Instruction, LowLevelType NarrowType,
                                               std::size_t OperandIndex);
  [[nodiscard]] Expected<void> narrowScalarDst(MachineInstr &Instruction, LowLevelType NarrowType,
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
  [[nodiscard]] Expected<void> multiplyRegisters(std::vector<Register> &Destinations,
                                                 const std::vector<Register> &FirstSources,
                                                 const std::vector<Register> &SecondSources,
                                                 LowLevelType NarrowType);
  [[nodiscard]] Expected<void> narrowScalarAddSub(MachineInstr &Instruction, std::size_t TypeIndex,
                                                  LowLevelType NarrowType);
  [[nodiscard]] Expected<void> narrowScalarMul(MachineInstr &Instruction, std::size_t TypeIndex,
                                               LowLevelType NarrowType);
  [[nodiscard]] Expected<void> narrowScalarBasic(MachineInstr &Instruction, std::size_t TypeIndex,
                                                 LowLevelType NarrowType);
  [[nodiscard]] Expected<void> narrowScalarExt(MachineInstr &Instruction, std::size_t TypeIndex,
                                               LowLevelType NarrowType);
  [[nodiscard]] Expected<void> narrowScalarSelect(MachineInstr &Instruction, std::size_t TypeIndex,
                                                  LowLevelType NarrowType);
  [[nodiscard]] Expected<void> narrowScalarShift(MachineInstr &Instruction, std::size_t TypeIndex,
                                                 LowLevelType NarrowType);
  [[nodiscard]] Expected<void> narrowScalarShiftByConstant(MachineInstr &Instruction,
                                                           const APInt &Amount,
                                                           LowLevelType HalfType,
                                                           LowLevelType AmountType);
  [[nodiscard]] Expected<void> narrowScalarShiftMultiway(MachineInstr &Instruction,
                                                         LowLevelType TargetType);
  [[nodiscard]] Expected<void> narrowScalarShiftByConstantMultiway(MachineInstr &Instruction,
                                                                   const APInt &Amount,
                                                                   LowLevelType TargetType,
                                                                   LowLevelType AmountType);
  [[nodiscard]] Expected<void> fewerElements(MachineInstr &Instruction, std::size_t TypeIndex,
                                             LowLevelType NarrowType);

  struct ShiftParameters final {
    Register WordShift;
    Register BitShift;
    Register InverseBitShift;
    Register Zero;
    Register SignBit;
  };

  [[nodiscard]] Expected<Register>
  buildConstantShiftPart(GenericOpcode Generic, std::size_t PartIndex, std::size_t PartCount,
                         const std::vector<Register> &SourceParts,
                         const ShiftParameters &Parameters, LowLevelType TargetType);
  [[nodiscard]] Expected<Register>
  buildVariableShiftPart(GenericOpcode Generic, Register MainOperand, Register ShiftAmount,
                         LowLevelType TargetType, Register CarryOperand = {});
  [[nodiscard]] Expected<void> widenScalarAddSubOverflow(MachineInstr &Instruction,
                                                         std::size_t TypeIndex,
                                                         LowLevelType WideType);
  [[nodiscard]] Expected<void> widenScalarMulo(MachineInstr &Instruction, std::size_t TypeIndex,
                                               LowLevelType WideType);
  [[nodiscard]] Expected<void> lowerSignedAddSubOverflow(MachineInstr &Instruction);
  [[nodiscard]] Expected<void> lowerSaturatingAddSubToMinMax(MachineInstr &Instruction);
  [[nodiscard]] Expected<void> lowerSaturatingAddSubToOverflow(MachineInstr &Instruction);
  [[nodiscard]] Expected<void> lowerSaturatingShiftLeft(MachineInstr &Instruction);
  [[nodiscard]] Expected<void> lowerMulo(MachineInstr &Instruction);

  MachineFunction &Function;
  MachineRegisterInfo &MRI;
  const LegalizerInfo &Info;
  const LibcallLoweringInfo *Libcalls = nullptr;
};

} // namespace rcc

#endif // RCC_CODEGEN_GLOBALISEL_LEGALIZERHELPER_H
