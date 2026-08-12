// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineBranchRelaxation.h"

#include "rcc/CodeGen/LiveVariables.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineFunctionCloner.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/CodeGen/TargetInstrInfo.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace rcc {
namespace {

struct MachineLayout final {
  std::unordered_map<const MachineBasicBlock *, std::uint64_t> BlockOffsets;
  std::unordered_map<const MachineInstr *, std::uint64_t> InstructionOffsets;
};

[[nodiscard]] const MachinePassContract &relaxationContract() {
  static const MachinePassContract Contract = [] {
    MachinePassContract Value{
        .Name = "machine-branch-relaxation",
        .Requires = {MachineFunctionProperty::Selected, MachineFunctionProperty::NoPHIs,
                     MachineFunctionProperty::TiedOpsRewritten, MachineFunctionProperty::NoVRegs,
                     MachineFunctionProperty::TracksLiveness,
                     MachineFunctionProperty::FrameFinalized,
                     MachineFunctionProperty::NoFrameIndices},
        .Sets = {},
        .Clears = {},
        .RequiredAnalyses = {},
        .VerificationStage = PipelineStage::PostRAFinalized,
    };
    Value.ThreadSafety = PassThreadSafety::ThreadSafe;
    Value.DeclaredChanges = {MachinePassChange::Instructions, MachinePassChange::Operands};
    return Value;
  }();
  return Contract;
}

[[nodiscard]] Error prefixedError(const Error &Failure, std::string Prefix) {
  Prefix.append(Failure.message());
  return Error(Failure.code(), std::move(Prefix));
}

[[nodiscard]] Expected<std::uint64_t> alignTo(std::uint64_t Value, std::uint64_t Alignment) {
  if (Alignment == 0 || (Alignment & (Alignment - 1U)) != 0)
    return makeUnexpected(ErrorCode::VerificationError, "block alignment is not a power of two");
  const std::uint64_t Mask = Alignment - 1U;
  if (Value > std::numeric_limits<std::uint64_t>::max() - Mask)
    return makeUnexpected(ErrorCode::BranchRelaxationFailed, "machine layout offset overflowed");
  return (Value + Mask) & ~Mask;
}

[[nodiscard]] Expected<MachineLayout> computeLayout(MachineFunction &Function,
                                                    const TargetDescription &Target,
                                                    const TargetInstrInfo &Info) {
  MachineLayout Result;
  std::uint64_t Offset = 0;
  for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr; Block = Block->next()) {
    auto Aligned = alignTo(Offset, Block->attributes().alignment().bytes());
    if (!Aligned)
      return std::unexpected(Aligned.error());
    Offset = *Aligned;
    Result.BlockOffsets.emplace(Block, Offset);
    for (MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      Result.InstructionOffsets.emplace(Instruction, Offset);
      auto Size = Info.instructionSize(*Instruction, Function, Target);
      if (!Size)
        return std::unexpected(Size.error());
      if (Offset > std::numeric_limits<std::uint64_t>::max() - *Size)
        return makeUnexpected(ErrorCode::BranchRelaxationFailed, "machine layout size overflowed");
      Offset += *Size;
    }
  }
  return Result;
}

[[nodiscard]] Expected<std::int64_t> displacement(std::uint64_t Source, std::uint64_t Target) {
  if (Target >= Source) {
    const std::uint64_t Difference = Target - Source;
    if (Difference > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
      return makeUnexpected(ErrorCode::BranchRelaxationFailed,
                            "forward branch displacement overflowed");
    return static_cast<std::int64_t>(Difference);
  }
  const std::uint64_t Difference = Source - Target;
  if (Difference > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    return makeUnexpected(ErrorCode::BranchRelaxationFailed,
                          "backward branch displacement overflowed");
  return -static_cast<std::int64_t>(Difference);
}

} // namespace

const MachinePassContract &MachineBranchRelaxationPass::contract() const noexcept {
  return relaxationContract();
}

Expected<MachinePassResult>
MachineBranchRelaxationPass::run(MachineFunction &Function, const TargetDescription &Target,
                                 MachineFunctionAnalysisManager &Analyses) {
  if (!Function.properties().containsAll(contract().Requires)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "branch relaxation requires finalized frame and tracked liveness");
  }
  auto Trial = cloneMachineFunction(
      Function, Target,
      MachineFunctionCloneOptions{.NewStableID = Function.stableID(), .NewName = std::nullopt});
  if (!Trial)
    return std::unexpected(prefixedError(Trial.error(), "branch relaxation setup failed: "));
  MachineActionReplay Actions(Analyses, Trial->InstructionStableIDs);
  auto TrialResult = runImplementation(*Trial->ClonedFunction, Target, Actions);
  if (!TrialResult)
    return std::unexpected(TrialResult.error());
  Actions.beginReplay();
  auto Result = runImplementation(Function, Target, Actions);
  if (!Result) {
    return std::unexpected(prefixedError(
        Result.error(), "branch relaxation diverged after a successful transaction preflight: "));
  }
  auto Replayed = Actions.finishReplay();
  if (!Replayed)
    return std::unexpected(Replayed.error());
  return Result;
}

Expected<MachinePassResult>
MachineBranchRelaxationPass::runImplementation(MachineFunction &Function,
                                               const TargetDescription &Target,
                                               MachineActionReplay &Actions) const {
  if (Options.ConvergenceWatchdog == 0)
    return makeUnexpected(ErrorCode::InvalidArgument, "branch relaxation watchdog must be nonzero");
  MachinePassResult Result;
  for (std::size_t Iteration = 0; Iteration < Options.ConvergenceWatchdog; ++Iteration) {
    auto Layout = computeLayout(Function, Target, *Info);
    if (!Layout)
      return std::unexpected(Layout.error());
    bool Changed = false;
    for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr && !Changed;
         Block = Block->next()) {
      for (MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
           Instruction = Instruction->next()) {
        auto BranchTarget = Info->branchTarget(*Instruction, Function, Target);
        if (!BranchTarget)
          return std::unexpected(BranchTarget.error());
        if (*BranchTarget == nullptr)
          continue;
        const auto SourcePosition = Layout->InstructionOffsets.find(Instruction);
        const auto TargetPosition = Layout->BlockOffsets.find(*BranchTarget);
        if (SourcePosition == Layout->InstructionOffsets.end() ||
            TargetPosition == Layout->BlockOffsets.end()) {
          return makeUnexpected(ErrorCode::InternalError,
                                "branch relaxation lost an instruction or target offset");
        }
        auto Offset = displacement(SourcePosition->second, TargetPosition->second);
        if (!Offset)
          return std::unexpected(Offset.error());
        if (Info->isBranchOffsetInRange(*Instruction, *Offset))
          continue;
        auto Accepted = Actions.request("relax-branch", Function, Instruction->stableID());
        if (!Accepted)
          return std::unexpected(Accepted.error());
        if (!*Accepted) {
          return makeUnexpected(ErrorCode::BranchRelaxationFailed,
                                "required branch relaxation action was rejected");
        }
        auto Relaxed = Info->relaxBranch(*Instruction, *Offset, Function, Target);
        if (!Relaxed)
          return std::unexpected(Relaxed.error());
        if (!*Relaxed) {
          return makeUnexpected(ErrorCode::BranchRelaxationFailed,
                                "target did not relax an out-of-range branch");
        }
        Changed = true;
        Result.Changed = true;
        break;
      }
    }
    if (Changed)
      continue;
    if (Result.Changed) {
      auto Rebuilt = rebuildMachineLiveness(Function, Target);
      if (!Rebuilt)
        return std::unexpected(Rebuilt.error());
      Result.Changes = {MachinePassChange::Instructions, MachinePassChange::Operands};
      Result.Preserved = PreservedAnalyses::none();
    } else {
      Result.Preserved = PreservedAnalyses::all();
    }
    return Result;
  }
  return makeUnexpected(ErrorCode::InternalError,
                        "branch relaxation failed to reach a monotonic fixed point");
}

} // namespace rcc
