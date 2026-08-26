// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineFinalization.h"

#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/CodeGen/TargetInstrInfo.h"

#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>

namespace rcc {
namespace {

[[nodiscard]] const MachinePassContract &finalizePseudosContract() {
  static const MachinePassContract Contract = [] {
    MachinePassContract Value{
        .Name = "finalize-emission-pseudos",
        .Requires = {MachineFunctionProperty::NoPHIs, MachineFunctionProperty::NoVRegs,
                     MachineFunctionProperty::FrameFinalized,
                     MachineFunctionProperty::NoFrameIndices},
        .Sets = {MachineFunctionProperty::NoExecutablePseudos},
        .Clears = {},
        .RequiredAnalyses = {},
        .VerificationStage = PipelineStage::Emission,
    };
    Value.ThreadSafety = PassThreadSafety::ThreadSafe;
    Value.DeclaredChanges = {};
    return Value;
  }();
  return Contract;
}

[[nodiscard]] const MachinePassContract &encodabilityContract() {
  static const MachinePassContract Contract = [] {
    MachinePassContract Value{
        .Name = "final-encodability-verifier",
        .Requires = {MachineFunctionProperty::NoPHIs, MachineFunctionProperty::NoVRegs,
                     MachineFunctionProperty::FrameFinalized,
                     MachineFunctionProperty::NoFrameIndices,
                     MachineFunctionProperty::NoExecutablePseudos},
        .Sets = {},
        .Clears = {},
        .RequiredAnalyses = {},
        .VerificationStage = PipelineStage::Emission,
    };
    Value.ThreadSafety = PassThreadSafety::ThreadSafe;
    Value.DeclaredChanges = {};
    return Value;
  }();
  return Contract;
}

[[nodiscard]] Expected<std::uint64_t> alignTo(std::uint64_t Value, std::uint64_t Alignment) {
  if (Alignment == 0 || (Alignment & (Alignment - 1U)) != 0)
    return makeUnexpected(ErrorCode::FinalEncodabilityFailed,
                          "final block alignment is not a power of two");
  const std::uint64_t Mask = Alignment - 1U;
  if (Value > std::numeric_limits<std::uint64_t>::max() - Mask)
    return makeUnexpected(ErrorCode::FinalEncodabilityFailed,
                          "final machine layout offset overflowed");
  return (Value + Mask) & ~Mask;
}

[[nodiscard]] Expected<std::int64_t> displacement(std::uint64_t Source, std::uint64_t Target) {
  if (Target >= Source) {
    const std::uint64_t Difference = Target - Source;
    if (Difference > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
      return makeUnexpected(ErrorCode::FinalEncodabilityFailed,
                            "final branch displacement overflowed");
    return static_cast<std::int64_t>(Difference);
  }
  const std::uint64_t Difference = Source - Target;
  if (Difference > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    return makeUnexpected(ErrorCode::FinalEncodabilityFailed,
                          "final branch displacement overflowed");
  return -static_cast<std::int64_t>(Difference);
}

} // namespace

const MachinePassContract &FinalizeEmissionPseudosPass::contract() const noexcept {
  return finalizePseudosContract();
}

Expected<MachinePassResult> FinalizeEmissionPseudosPass::run(MachineFunction &Function,
                                                             const TargetDescription &Target,
                                                             MachineFunctionAnalysisManager &) {
  for (MachineInstr *Instruction : Function.allInstructions()) {
    if (Instruction == nullptr || Instruction->parent() == nullptr)
      continue;
    const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction->opcode());
    if (Descriptor == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "emission-pseudo finalization encountered an unknown opcode");
    if (!Descriptor->Flags.has(InstructionFlag::Pseudo))
      continue;
    const bool NonEmitting = Descriptor->Flags.has(InstructionFlag::Meta) &&
                             !Descriptor->Flags.has(InstructionFlag::ProducesCode);
    const bool Terminal = Descriptor->Flags.has(InstructionFlag::TerminalEmission) &&
                          Descriptor->Owner == PseudoOwner::AsmPrinter;
    if (!NonEmitting && !Terminal) {
      return makeUnexpected(ErrorCode::PseudoExpansionFailed,
                            "non-terminal executable pseudo survived all expansion owners");
    }
  }
  MachinePassResult Result;
  Result.Preserved = PreservedAnalyses::all();
  return Result;
}

const MachinePassContract &FinalEncodabilityVerifierPass::contract() const noexcept {
  return encodabilityContract();
}

Expected<MachinePassResult> FinalEncodabilityVerifierPass::run(MachineFunction &Function,
                                                               const TargetDescription &Target,
                                                               MachineFunctionAnalysisManager &) {
  std::unordered_map<const MachineBasicBlock *, std::uint64_t> BlockOffsets;
  std::unordered_map<const MachineInstr *, std::uint64_t> InstructionOffsets;
  std::uint64_t Offset = 0;
  for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr; Block = Block->next()) {
    auto Aligned = alignTo(Offset, Block->attributes().alignment().bytes());
    if (!Aligned)
      return std::unexpected(Aligned.error());
    Offset = *Aligned;
    BlockOffsets.emplace(Block, Offset);
    for (MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction->opcode());
      if (Descriptor == nullptr)
        return makeUnexpected(ErrorCode::FinalEncodabilityFailed,
                              "final instruction uses an unknown opcode");
      auto InstructionValidation =
          Target.validateInstruction(*Instruction, Function.registerInfo());
      if (!InstructionValidation) {
        return makeUnexpected(ErrorCode::FinalEncodabilityFailed,
                              "final instruction is not encodable: " +
                                  std::string(InstructionValidation.error().message()));
      }
      const bool BundledWithPred = Instruction->flags().has(MachineInstrFlag::BundledWithPred);
      const bool BundledWithSucc = Instruction->flags().has(MachineInstrFlag::BundledWithSucc);
      if ((BundledWithPred &&
           (Instruction->previous() == nullptr ||
            !Instruction->previous()->flags().has(MachineInstrFlag::BundledWithSucc))) ||
          (BundledWithSucc &&
           (Instruction->next() == nullptr ||
            !Instruction->next()->flags().has(MachineInstrFlag::BundledWithPred)))) {
        return makeUnexpected(ErrorCode::FinalEncodabilityFailed,
                              "final instruction has an inconsistent bundle boundary");
      }
      for (const MachineOperand &Operand : Instruction->operands()) {
        const Register *Reg = Operand.getIf<Register>();
        if (Reg != nullptr && !Reg->isNoRegister() && !Reg->isPhysical())
          return makeUnexpected(ErrorCode::FinalEncodabilityFailed,
                                "final instruction retains a virtual register");
        if (Operand.kind() == MachineOperand::Kind::FrameIndex)
          return makeUnexpected(ErrorCode::FinalEncodabilityFailed,
                                "final instruction retains a frame index");
      }
      auto Size = BranchInfo->instructionSize(*Instruction, Function, Target);
      if (!Size)
        return std::unexpected(Size.error());
      const bool ProducesCode = Descriptor->Flags.has(InstructionFlag::ProducesCode);
      if ((ProducesCode && *Size == 0) ||
          (Descriptor->Flags.has(InstructionFlag::Meta) && *Size != 0)) {
        return makeUnexpected(ErrorCode::FinalEncodabilityFailed,
                              "final instruction violates its exact size contract");
      }
      InstructionOffsets.emplace(Instruction, Offset);
      if (Offset > std::numeric_limits<std::uint64_t>::max() - *Size)
        return makeUnexpected(ErrorCode::FinalEncodabilityFailed,
                              "final machine function size overflowed");
      Offset += *Size;
    }
  }

  for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr; Block = Block->next()) {
    for (MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      auto TargetBlock = BranchInfo->branchTarget(*Instruction, Function, Target);
      if (!TargetBlock)
        return std::unexpected(TargetBlock.error());
      if (*TargetBlock == nullptr)
        continue;
      const auto Source = InstructionOffsets.find(Instruction);
      const auto Destination = BlockOffsets.find(*TargetBlock);
      if (Source == InstructionOffsets.end() || Destination == BlockOffsets.end())
        return makeUnexpected(ErrorCode::FinalEncodabilityFailed,
                              "final branch target has no layout offset");
      auto Distance = displacement(Source->second, Destination->second);
      if (!Distance)
        return std::unexpected(Distance.error());
      if (!BranchInfo->isBranchOffsetInRange(*Instruction, *Distance))
        return makeUnexpected(ErrorCode::FinalEncodabilityFailed,
                              "final instruction contains an out-of-range branch");
    }
  }
  MachinePassResult Result;
  Result.Preserved = PreservedAnalyses::all();
  return Result;
}

} // namespace rcc
