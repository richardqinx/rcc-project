// SPDX-License-Identifier: GPL-3.0-or-later

#include "RISCV.h"
#include "rcc/CodeGen/GlobalISel/GIMatchTableExecutor.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/TargetDescription.h"

#include <string>

namespace rcc::riscv {
namespace {

#define GET_GLOBALISEL_COMBINER_TABLE
#include "RISCVGenGlobalISelCombiner.inc"
#undef GET_GLOBALISEL_COMBINER_TABLE

class RISCVGISelCombinerExecutor final : public GIMatchTableExecutor {
protected:
  [[nodiscard]] Expected<bool>
  matchesTargetPredicate(std::string_view Name, const MachineInstr &Instruction,
                         const MachineFunction &Function,
                         const TargetDescription &Target) const override {
    if (Name != "sext_cheaper_than_zext")
      return GIMatchTableExecutor::matchesTargetPredicate(Name, Instruction, Function, Target);
    const MachineOperand *Destination = Instruction.operandAt(0);
    const MachineOperand *Source = Instruction.operandAt(1);
    const Register *DestinationReg =
        Destination == nullptr ? nullptr : Destination->getIf<Register>();
    const Register *SourceReg = Source == nullptr ? nullptr : Source->getIf<Register>();
    if (DestinationReg == nullptr || SourceReg == nullptr || !DestinationReg->isVirtual() ||
        !SourceReg->isVirtual())
      return false;
    auto DestinationType = Function.registerInfo().getType(*DestinationReg);
    auto SourceType = Function.registerInfo().getType(*SourceReg);
    if (!DestinationType || !SourceType)
      return false;
    return DestinationType->isCategoryNeutralScalar() && SourceType->isCategoryNeutralScalar() &&
           DestinationType->scalarBitWidth() == 64 && SourceType->scalarBitWidth() == 32;
  }
};

} // namespace

const GIMatchTable &giselCombinerTable() noexcept {
  static constexpr GIMatchTable Table = generatedGlobalISelCombinerMatchTable();
  return Table;
}

const GIMatchTableExecutor &giselCombinerExecutor() noexcept {
  static const RISCVGISelCombinerExecutor Executor;
  return Executor;
}

} // namespace rcc::riscv
