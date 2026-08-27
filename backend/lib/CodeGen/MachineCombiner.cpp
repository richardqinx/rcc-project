// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineCombiner.h"

#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/TargetDescription.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace rcc {
namespace {

[[nodiscard]] const MachinePassContract &combinerContract() {
  static const MachinePassContract Contract = [] {
    MachinePassContract Value{
        .Name = "machine-combiner",
        .Requires = {MachineFunctionProperty::IsSSA},
        .Sets = {},
        .Clears = {},
        .RequiredAnalyses = {},
        .VerificationStage = PipelineStage::Selected,
    };
    Value.ThreadSafety = PassThreadSafety::ThreadSafe;
    Value.DeclaredChanges = {MachinePassChange::Instructions, MachinePassChange::Operands,
                             MachinePassChange::Registers};
    return Value;
  }();
  return Contract;
}

[[nodiscard]] bool hasProtectedAttachment(const MachineInstr &Instruction) noexcept {
  return Instruction.flags().has(MachineInstrFlag::FrameSetup) ||
         Instruction.flags().has(MachineInstrFlag::FrameDestroy) ||
         Instruction.debugInstrNumber() != 0 || Instruction.preInstrSymbol().has_value() ||
         Instruction.postInstrSymbol().has_value() || Instruction.heapAllocMarker().has_value() ||
         Instruction.pcSections().has_value() || Instruction.mmra().has_value() ||
         Instruction.cfiType().has_value() || Instruction.deactivationSymbol().has_value();
}

[[nodiscard]] MachineInstrFlags intersectFlags(MachineInstrFlags Left,
                                               MachineInstrFlags Right) noexcept {
  MachineInstrFlags Result;
  for (std::uint32_t Index = 0; Index < static_cast<std::uint32_t>(MachineInstrFlag::Count);
       ++Index) {
    const auto Flag = static_cast<MachineInstrFlag>(Index);
    if (Left.has(Flag) && Right.has(Flag))
      Result.set(Flag);
  }
  return Result;
}

[[nodiscard]] bool hasEqualRoundingMode(const MachineInstr &Root,
                                        const MachineInstr &Producer) noexcept {
  const MachineOperand *RootMode = Root.operandAt(3);
  const MachineOperand *ProducerMode = Producer.operandAt(3);
  const auto *RootImmediate = RootMode == nullptr ? nullptr : RootMode->getIf<std::int64_t>();
  const auto *ProducerImmediate =
      ProducerMode == nullptr ? nullptr : ProducerMode->getIf<std::int64_t>();
  return RootImmediate != nullptr && ProducerImmediate != nullptr &&
         *RootImmediate == *ProducerImmediate;
}

[[nodiscard]] MachineInstr *findProducer(MachineInstr &Root,
                                         const MachineCombinerPattern &Pattern) noexcept {
  const MachineOperand *ProducerUse = Root.operandAt(Pattern.ProducerOperand);
  const Register *ProducerRegister =
      ProducerUse == nullptr ? nullptr : ProducerUse->getIf<Register>();
  if (ProducerRegister == nullptr || !ProducerUse->isUse() || !ProducerRegister->isVirtual()) {
    return nullptr;
  }
  MachineInstr *Producer = Root.function().registerInfo().getVRegDef(*ProducerRegister);
  if (Producer == nullptr || Producer->opcode() != Pattern.ProducerOpcode ||
      Producer->parent() != Root.parent() || Producer->operandCount() < 4 ||
      hasProtectedAttachment(*Producer)) {
    return nullptr;
  }
  const MachineOperand *Definition = Producer->operandAt(0);
  const Register *DefinedRegister = Definition == nullptr ? nullptr : Definition->getIf<Register>();
  if (DefinedRegister == nullptr || !Definition->isDef() || Definition->isUse() ||
      *DefinedRegister != *ProducerRegister) {
    return nullptr;
  }
  if (!Root.flags().has(MachineInstrFlag::FastMathAllowContract) ||
      !Producer->flags().has(MachineInstrFlag::FastMathAllowContract) ||
      !hasEqualRoundingMode(Root, *Producer)) {
    return nullptr;
  }
  return Producer;
}

[[nodiscard]] Expected<void> clearKillFlags(MachineRegisterInfo &Registers, Register Reg) {
  for (MachineOperand &Operand : Registers.regOperands(Reg)) {
    RegisterOperandFlags Flags = Operand.registerFlags();
    if (!Flags.has(RegisterOperandFlag::Kill))
      continue;
    Flags.clear(RegisterOperandFlag::Kill);
    auto Changed = Operand.setRegisterFlags(Flags);
    if (!Changed)
      return std::unexpected(Changed.error());
  }
  return {};
}

[[nodiscard]] Expected<bool> combineFusedMultiplyAdd(MachineInstr &Root, MachineInstr &Producer,
                                                     const MachineCombinerPattern &Pattern,
                                                     MachineFunction &Function,
                                                     const TargetDescription &Target) {
  const MachineOperand *RootDefinition = Root.operandAt(0);
  const MachineOperand *MultiplyLeft = Producer.operandAt(1);
  const MachineOperand *MultiplyRight = Producer.operandAt(2);
  const MachineOperand *Addend = Root.operandAt(Pattern.AddendOperand);
  const Register *ProducerResult = Producer.operandAt(0)->getIf<Register>();
  const Register *MultiplyLeftRegister =
      MultiplyLeft == nullptr ? nullptr : MultiplyLeft->getIf<Register>();
  const Register *MultiplyRightRegister =
      MultiplyRight == nullptr ? nullptr : MultiplyRight->getIf<Register>();
  if (RootDefinition == nullptr || MultiplyLeftRegister == nullptr ||
      MultiplyRightRegister == nullptr || Addend == nullptr || ProducerResult == nullptr) {
    return false;
  }

  MachineRegisterInfo &Registers = Function.registerInfo();
  const bool EraseProducer = Registers.hasOneUse(*ProducerResult, false);
  auto Cleared = clearKillFlags(Registers, *MultiplyLeftRegister);
  if (!Cleared)
    return std::unexpected(Cleared.error());
  Cleared = clearKillFlags(Registers, *MultiplyRightRegister);
  if (!Cleared)
    return std::unexpected(Cleared.error());

  MachineIRBuilder Builder(Function, Target);
  auto Point = Builder.setInsertPointBefore(Root);
  if (!Point)
    return std::unexpected(Point.error());
  auto Pending = Builder.buildInstr(Pattern.ReplacementOpcode);
  if (!Pending)
    return std::unexpected(Pending.error());

  auto Added = Pending->addOperand(RootDefinition->cloneDetached());
  if (!Added)
    return std::unexpected(Added.error());
  Added = Pending->addOperand(MultiplyLeft->cloneDetached());
  if (!Added)
    return std::unexpected(Added.error());
  Added = Pending->addOperand(MultiplyRight->cloneDetached());
  if (!Added)
    return std::unexpected(Added.error());
  Added = Pending->addOperand(Addend->cloneDetached());
  if (!Added)
    return std::unexpected(Added.error());
  Added = Pending->addOperand(Root.operandAt(3)->cloneDetached());
  if (!Added)
    return std::unexpected(Added.error());
  for (std::size_t Index = 4; Index < Root.operandCount(); ++Index) {
    Added = Pending->addImplicitOperand(Root.operandAt(Index)->cloneDetached());
    if (!Added)
      return std::unexpected(Added.error());
  }
  Added = Pending->setFlags(intersectFlags(Root.flags(), Producer.flags()));
  if (!Added)
    return std::unexpected(Added.error());
  if (Root.debugLocation()) {
    Added = Pending->setDebugLocation(*Root.debugLocation());
    if (!Added)
      return std::unexpected(Added.error());
  }
  auto Fused = Pending->build();
  if (!Fused)
    return std::unexpected(Fused.error());

  MachineBasicBlock *Block = Root.parent();
  if (Block == nullptr)
    return makeUnexpected(ErrorCode::InternalError, "machine combiner root is detached");
  auto Removed = Block->removeInstruction(Root);
  if (!Removed)
    return std::unexpected(Removed.error());
  if (EraseProducer) {
    MachineBasicBlock *ProducerBlock = Producer.parent();
    if (ProducerBlock == nullptr) {
      return makeUnexpected(ErrorCode::InternalError, "machine combiner producer is detached");
    }
    Removed = ProducerBlock->removeInstruction(Producer);
    if (!Removed)
      return std::unexpected(Removed.error());
  }
  return true;
}

} // namespace

const MachinePassContract &MachineCombinerPass::contract() const noexcept {
  return combinerContract();
}

Expected<MachinePassResult> MachineCombinerPass::run(MachineFunction &Function,
                                                     const TargetDescription &Target,
                                                     MachineFunctionAnalysisManager &Analyses) {
  return runImplementation(Function, Target, Analyses);
}

Expected<MachinePassResult>
MachineCombinerPass::runImplementation(MachineFunction &Function, const TargetDescription &Target,
                                       MachineFunctionAnalysisManager &Analyses) const {
  if (Options.MaximumIterations == 0)
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "machine combiner iteration budget must be nonzero");
  MachinePassResult Result;
  std::size_t Rewrites = 0;
  for (std::size_t Iteration = 0; Iteration < Options.MaximumIterations; ++Iteration) {
    bool IterationChanged = false;
    for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
         Block = Block->next()) {
      for (MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;) {
        MachineInstr *Next = Instruction->next();
        for (const MachineCombinerPattern &Pattern : Info->patterns(Instruction->opcode())) {
          if (Rewrites == Options.MaximumRewrites)
            break;
          if (hasProtectedAttachment(*Instruction) || Instruction->operandCount() < 4)
            continue;
          MachineInstr *Producer = findProducer(*Instruction, Pattern);
          if (Producer == nullptr)
            continue;
          std::string Rule = "combine.";
          Rule.append(Pattern.Name);
          auto Accepted = Analyses.requestAction(Rule, Function, Instruction->stableID());
          if (!Accepted)
            return std::unexpected(Accepted.error());
          if (!*Accepted)
            continue;
          auto Executed =
              combineFusedMultiplyAdd(*Instruction, *Producer, Pattern, Function, Target);
          if (!Executed)
            return std::unexpected(Executed.error());
          if (!*Executed)
            continue;
          ++Rewrites;
          IterationChanged = true;
          Result.Changed = true;
          break;
        }
        Instruction = Next;
      }
    }
    if (!IterationChanged || Rewrites == Options.MaximumRewrites)
      break;
  }

  if (!Result.Changed) {
    Result.Preserved = PreservedAnalyses::all();
    return Result;
  }
  Result.Changes = {MachinePassChange::Instructions, MachinePassChange::Operands,
                    MachinePassChange::Registers};
  Result.Preserved = PreservedAnalyses::none();
  return Result;
}

} // namespace rcc
