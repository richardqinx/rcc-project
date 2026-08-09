// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineScheduler.h"

#include "MachineSchedulerInternal.h"
#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineFunctionCloner.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineLiveness.h"
#include "rcc/CodeGen/RegisterPressure.h"
#include "rcc/CodeGen/ScheduleDAG.h"
#include "rcc/CodeGen/ScheduleHazardRecognizer.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/CodeGen/TargetSchedulingModel.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace rcc {
namespace {

[[nodiscard]] const MachinePassContract &schedulerContract() {
  static const MachinePassContract Contract = [] {
    MachinePassContract Value{
        .Name = "machine-scheduler",
        .Requires = {MachineFunctionProperty::Selected, MachineFunctionProperty::NoPHIs,
                     MachineFunctionProperty::TiedOpsRewritten},
        .Sets = {},
        .Clears = {},
        .RequiredAnalyses = {MachineAnalysisManager::id<LiveVariablesAnalysis>()},
        .VerificationStage = PipelineStage::TiedOpsRewritten,
    };
    Value.RequiredAnalyses.push_back(MachineAnalysisManager::id<MachineBlockFrequencyAnalysis>());
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

[[nodiscard]] std::uint64_t saturatingAdd(std::uint64_t Left, std::uint64_t Right) noexcept {
  return std::numeric_limits<std::uint64_t>::max() - Left < Right
             ? std::numeric_limits<std::uint64_t>::max()
             : Left + Right;
}

[[nodiscard]] Expected<bool> isSchedulingBoundary(const MachineInstr &Instruction,
                                                  const TargetDescription &Target,
                                                  const TargetSchedulingModel &Model) {
  const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction.opcode());
  if (Descriptor == nullptr) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "machine scheduler encountered an unknown instruction");
  }
  const InstructionFlags Flags = Descriptor->Flags;
  if (Model.hooks().isSchedulingBoundary(Instruction, Target))
    return true;
  if (Flags.has(InstructionFlag::Terminator) || Flags.has(InstructionFlag::Branch) ||
      Flags.has(InstructionFlag::Call) || Flags.has(InstructionFlag::Return) ||
      Flags.has(InstructionFlag::Barrier) || Flags.has(InstructionFlag::HasSideEffects) ||
      Flags.has(InstructionFlag::UnmodeledSideEffects) || Flags.has(InstructionFlag::Convergent) ||
      Flags.has(InstructionFlag::Meta) || Flags.has(InstructionFlag::Phi)) {
    return true;
  }
  if (Model.schedulingClass(Descriptor->SchedulingClass) == nullptr) {
    if (Flags.has(InstructionFlag::Pseudo) || !Flags.has(InstructionFlag::ProducesCode))
      return true;
    return makeUnexpected(ErrorCode::VerificationError,
                          "executable instruction has no scheduling class");
  }
  return false;
}

struct ScheduleCandidate final {
  std::size_t Unit = 0;
  bool Bottom = false;
  std::uint64_t Cycle = 0;
  RegisterPressureCost Pressure;
  std::uint64_t CriticalPath = 0;
  std::uint32_t Latency = 0;
  std::int64_t TargetPreference = 0;
  bool OptimizeLatency = false;
  std::uint64_t StableID = 0;
};

[[nodiscard]] bool isBetterCandidate(const ScheduleCandidate &Left,
                                     const ScheduleCandidate &Right) noexcept {
  if (Left.Cycle != Right.Cycle)
    return Left.Cycle < Right.Cycle;
  if (Left.TargetPreference != Right.TargetPreference)
    return Left.TargetPreference > Right.TargetPreference;
  if (Left.OptimizeLatency && Right.OptimizeLatency) {
    if (Left.CriticalPath != Right.CriticalPath)
      return Left.CriticalPath > Right.CriticalPath;
    if (Left.Latency != Right.Latency)
      return Left.Latency > Right.Latency;
  }
  if (Left.Pressure.PeakIncrease != Right.Pressure.PeakIncrease)
    return Left.Pressure.PeakIncrease < Right.Pressure.PeakIncrease;
  if (Left.Pressure.PeakExcess != Right.Pressure.PeakExcess)
    return Left.Pressure.PeakExcess < Right.Pressure.PeakExcess;
  if (Left.Pressure.CurrentExcess != Right.Pressure.CurrentExcess)
    return Left.Pressure.CurrentExcess < Right.Pressure.CurrentExcess;
  if (Left.CriticalPath != Right.CriticalPath)
    return Left.CriticalPath > Right.CriticalPath;
  if (Left.Latency != Right.Latency)
    return Left.Latency > Right.Latency;
  if (Left.StableID != Right.StableID)
    return Left.StableID < Right.StableID;
  return !Left.Bottom && Right.Bottom;
}

[[nodiscard]] Expected<std::vector<std::size_t>>
scheduleDAG(const ScheduleDAG &DAG, const MachineFunction &Function,
            const TargetDescription &Target, const TargetSchedulingModel &Model,
            ScheduleDAGMode Mode, const MachineBlockLiveness &Liveness,
            std::uint64_t BlockFrequency, bool EnableBidirectionalScheduling) {
  auto TopPressure = RegisterPressureTracker::create(DAG, Function, Target, Model, Mode, Liveness,
                                                     RegisterPressureDirection::TopDown);
  if (!TopPressure)
    return std::unexpected(TopPressure.error());
  auto BottomPressure = RegisterPressureTracker::create(
      DAG, Function, Target, Model, Mode, Liveness, RegisterPressureDirection::BottomUp);
  if (!BottomPressure)
    return std::unexpected(BottomPressure.error());
  auto TopHazards = ScheduleHazardRecognizer::create(Model, ScheduleDirection::TopDown);
  if (!TopHazards)
    return std::unexpected(TopHazards.error());
  auto BottomHazards = ScheduleHazardRecognizer::create(Model, ScheduleDirection::BottomUp);
  if (!BottomHazards)
    return std::unexpected(BottomHazards.error());

  const std::size_t UnitCount = DAG.units().size();
  std::vector<std::size_t> TopPredecessors(UnitCount);
  std::vector<std::size_t> BottomSuccessors(UnitCount);
  std::vector<std::uint64_t> TopReadyCycle(UnitCount, 0);
  std::vector<std::uint64_t> BottomReadyCycle(UnitCount, 0);
  std::vector<bool> Scheduled(UnitCount, false);
  for (const ScheduleUnit &Unit : DAG.units()) {
    TopPredecessors[Unit.ID] = Unit.Predecessors.size();
    BottomSuccessors[Unit.ID] = Unit.Successors.size();
  }

  std::vector<std::size_t> TopOrder;
  std::vector<std::size_t> BottomOrder;
  TopOrder.reserve(UnitCount);
  BottomOrder.reserve(UnitCount);
  for (std::size_t ScheduledCount = 0; ScheduledCount < UnitCount; ++ScheduledCount) {
    std::optional<ScheduleCandidate> Best;
    for (const ScheduleUnit &Unit : DAG.units()) {
      if (Scheduled[Unit.ID])
        continue;
      if (TopPredecessors[Unit.ID] == 0) {
        auto Cycle = TopHazards->earliestIssueCycle(Unit, TopReadyCycle[Unit.ID]);
        if (!Cycle)
          return std::unexpected(Cycle.error());
        auto Pressure = TopPressure->evaluate(Unit.ID);
        if (!Pressure)
          return std::unexpected(Pressure.error());
        const ScheduleCandidate Candidate{
            Unit.ID,
            false,
            *Cycle,
            *Pressure,
            Unit.Height,
            Unit.Latency,
            Model.hooks().candidatePreference(Unit, ScheduleDirection::TopDown, DAG.block(),
                                              BlockFrequency),
            BlockFrequency >= MachineBlockFrequencyAnalysis::EntryFrequency,
            Unit.StableID.value()};
        if (!Best || isBetterCandidate(Candidate, *Best))
          Best = Candidate;
      }
      if (EnableBidirectionalScheduling && BottomSuccessors[Unit.ID] == 0) {
        auto Cycle = BottomHazards->earliestIssueCycle(Unit, BottomReadyCycle[Unit.ID]);
        if (!Cycle)
          return std::unexpected(Cycle.error());
        auto Pressure = BottomPressure->evaluate(Unit.ID);
        if (!Pressure)
          return std::unexpected(Pressure.error());
        const ScheduleCandidate Candidate{
            Unit.ID,
            true,
            *Cycle,
            *Pressure,
            Unit.Depth,
            Unit.Latency,
            Model.hooks().candidatePreference(Unit, ScheduleDirection::BottomUp, DAG.block(),
                                              BlockFrequency),
            BlockFrequency >= MachineBlockFrequencyAnalysis::EntryFrequency,
            Unit.StableID.value()};
        if (!Best || isBetterCandidate(Candidate, *Best))
          Best = Candidate;
      }
    }
    if (!Best) {
      return makeUnexpected(ErrorCode::SchedulingFailed,
                            "schedule DAG has no legal top or bottom ready unit");
    }

    const ScheduleUnit &Selected = DAG.units()[Best->Unit];
    Scheduled[Best->Unit] = true;
    if (Best->Bottom) {
      auto Issued = BottomHazards->issue(Selected, Best->Cycle);
      if (!Issued)
        return std::unexpected(Issued.error());
      auto Pressure = BottomPressure->schedule(Selected.ID);
      if (!Pressure)
        return std::unexpected(Pressure.error());
      BottomOrder.push_back(Selected.ID);
      for (const ScheduleDependency &Dependency : Selected.Predecessors) {
        if (BottomSuccessors[Dependency.Source] == 0) {
          return makeUnexpected(ErrorCode::InternalError,
                                "bottom scheduler successor count underflowed");
        }
        --BottomSuccessors[Dependency.Source];
        BottomReadyCycle[Dependency.Source] = std::max(
            BottomReadyCycle[Dependency.Source], saturatingAdd(Best->Cycle, Dependency.Latency));
      }
    } else {
      auto Issued = TopHazards->issue(Selected, Best->Cycle);
      if (!Issued)
        return std::unexpected(Issued.error());
      auto Pressure = TopPressure->schedule(Selected.ID);
      if (!Pressure)
        return std::unexpected(Pressure.error());
      TopOrder.push_back(Selected.ID);
      for (const ScheduleDependency &Dependency : Selected.Successors) {
        if (TopPredecessors[Dependency.Destination] == 0) {
          return makeUnexpected(ErrorCode::InternalError,
                                "top scheduler predecessor count underflowed");
        }
        --TopPredecessors[Dependency.Destination];
        TopReadyCycle[Dependency.Destination] = std::max(
            TopReadyCycle[Dependency.Destination], saturatingAdd(Best->Cycle, Dependency.Latency));
      }
    }
  }

  TopOrder.insert(TopOrder.end(), BottomOrder.rbegin(), BottomOrder.rend());
  if (TopOrder.size() != UnitCount)
    return makeUnexpected(ErrorCode::InternalError, "scheduler lost a schedule unit");
  return TopOrder;
}

[[nodiscard]] Expected<bool>
scheduleRegion(MachineBasicBlock &Block, std::span<MachineInstr *const> Instructions,
               MachineFunction &Function, const TargetDescription &Target,
               const TargetSchedulingModel &Model, ScheduleDAGMode Mode,
               const MachineBlockLiveness &Liveness, std::uint64_t BlockFrequency,
               MachineSchedulerOptions Options) {
  if (Instructions.size() < 2 || Instructions.size() > Options.MaximumRegionSize)
    return false;
  auto DAG = ScheduleDAG::build(Block, Instructions, Function, Target, Model, Mode);
  if (!DAG)
    return std::unexpected(DAG.error());
  if (DAG->units().size() < 2)
    return false;
  auto UnitOrder = scheduleDAG(*DAG, Function, Target, Model, Mode, Liveness, BlockFrequency,
                               Options.EnableBidirectionalScheduling);
  if (!UnitOrder)
    return std::unexpected(UnitOrder.error());

  std::vector<MachineInstr *> NewOrder;
  NewOrder.reserve(Instructions.size());
  for (const std::size_t UnitID : *UnitOrder) {
    const ScheduleUnit *Unit = DAG->unit(UnitID);
    if (Unit == nullptr)
      return makeUnexpected(ErrorCode::InternalError, "scheduler produced an unknown unit ID");
    NewOrder.insert(NewOrder.end(), Unit->Instructions.begin(), Unit->Instructions.end());
  }
  if (std::ranges::equal(NewOrder, Instructions))
    return false;

  MachineInstr *Anchor = Instructions.back()->next();
  for (MachineInstr *Instruction : Instructions) {
    auto Removed = Block.removeInstruction(*Instruction);
    if (!Removed)
      return std::unexpected(Removed.error());
  }
  for (MachineInstr *Instruction : NewOrder) {
    Expected<void> Inserted = Anchor == nullptr
                                  ? Block.appendInstruction(*Instruction)
                                  : Block.insertInstructionBefore(*Anchor, *Instruction);
    if (!Inserted)
      return std::unexpected(Inserted.error());
  }
  return true;
}

} // namespace

Expected<MachineSchedulingRunResult> runMachineScheduler(
    MachineFunction &Function, const TargetDescription &Target, const TargetSchedulingModel &Model,
    ScheduleDAGMode Mode, const LiveVariablesAnalysis::Result &Liveness,
    const MachineBlockFrequencyAnalysis::Result &Frequencies, MachineSchedulerOptions Options) {
  if (Options.MaximumRegionSize == 0) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "machine scheduler region budget must be nonzero");
  }
  MachineSchedulingRunResult Result;
  for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr; Block = Block->next()) {
    const MachineBlockLiveness *BlockLiveness = Liveness.block(*Block);
    if (BlockLiveness == nullptr) {
      return makeUnexpected(ErrorCode::InternalError,
                            "machine scheduler has no block liveness boundary");
    }
    bool BlockChanged = false;
    std::vector<MachineInstr *> Region;
    auto FlushRegion = [&]() -> Expected<void> {
      auto Changed = scheduleRegion(*Block, Region, Function, Target, Model, Mode, *BlockLiveness,
                                    Frequencies.frequency(*Block), Options);
      if (!Changed)
        return std::unexpected(Changed.error());
      BlockChanged |= *Changed;
      Region.clear();
      return {};
    };

    for (MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;) {
      if (Instruction->flags().has(MachineInstrFlag::BundledWithPred)) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "machine scheduler encountered a broken bundle header");
      }
      std::vector<MachineInstr *> Group;
      bool Boundary = false;
      for (;;) {
        Group.push_back(Instruction);
        auto IsBoundary = isSchedulingBoundary(*Instruction, Target, Model);
        if (!IsBoundary)
          return std::unexpected(IsBoundary.error());
        Boundary |= *IsBoundary;
        const bool HasSuccessor = Instruction->flags().has(MachineInstrFlag::BundledWithSucc);
        if (!HasSuccessor)
          break;
        MachineInstr *Next = Instruction->next();
        if (Next == nullptr || !Next->flags().has(MachineInstrFlag::BundledWithPred)) {
          return makeUnexpected(ErrorCode::VerificationError,
                                "machine scheduler encountered a truncated bundle");
        }
        Instruction = Next;
      }
      MachineInstr *NextGroup = Instruction->next();
      if (Boundary) {
        auto Flushed = FlushRegion();
        if (!Flushed)
          return std::unexpected(Flushed.error());
      } else {
        Region.insert(Region.end(), Group.begin(), Group.end());
      }
      Instruction = NextGroup;
    }
    auto Flushed = FlushRegion();
    if (!Flushed)
      return std::unexpected(Flushed.error());
    if (BlockChanged) {
      Result.Changed = true;
      Result.ChangedBlocks.push_back(Block);
    }
  }
  return Result;
}

const MachinePassContract &MachineSchedulerPass::contract() const noexcept {
  return schedulerContract();
}

Expected<MachinePassResult> MachineSchedulerPass::run(MachineFunction &Function,
                                                      const TargetDescription &Target,
                                                      MachineAnalysisManager &Analyses) {
  if (!Function.properties().containsAll(contract().Requires)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "pre-RA machine scheduler requires selected, SSA-exited MachineIR");
  }
  auto Trial = cloneMachineFunction(
      Function, Target,
      MachineFunctionCloneOptions{.NewStableID = Function.stableID(), .NewName = std::nullopt});
  if (!Trial) {
    return std::unexpected(
        prefixedError(Trial.error(), "machine scheduler transaction setup failed: "));
  }
  MachineAnalysisManager TrialAnalyses;
  auto TrialLiveness =
      TrialAnalyses.getResult<LiveVariablesAnalysis>(*Trial->ClonedFunction, Target);
  if (!TrialLiveness)
    return std::unexpected(TrialLiveness.error());
  auto TrialFrequencies =
      TrialAnalyses.getResult<MachineBlockFrequencyAnalysis>(*Trial->ClonedFunction, Target);
  if (!TrialFrequencies)
    return std::unexpected(TrialFrequencies.error());
  auto TrialResult = runMachineScheduler(*Trial->ClonedFunction, Target, *Model,
                                         ScheduleDAGMode::PreRegisterAllocation, **TrialLiveness,
                                         **TrialFrequencies, Options);
  if (!TrialResult)
    return std::unexpected(TrialResult.error());

  auto Liveness = Analyses.getResult<LiveVariablesAnalysis>(Function, Target);
  if (!Liveness)
    return std::unexpected(Liveness.error());
  auto Frequencies = Analyses.getResult<MachineBlockFrequencyAnalysis>(Function, Target);
  if (!Frequencies)
    return std::unexpected(Frequencies.error());
  auto Run = runMachineScheduler(Function, Target, *Model, ScheduleDAGMode::PreRegisterAllocation,
                                 **Liveness, **Frequencies, Options);
  if (!Run) {
    return std::unexpected(prefixedError(
        Run.error(), "machine scheduler diverged after a successful transaction preflight: "));
  }

  MachinePassResult Result;
  Result.Changed = Run->Changed;
  if (!Result.Changed) {
    Result.Preserved = PreservedAnalyses::all();
    return Result;
  }
  if (Function.properties().has(MachineFunctionProperty::TracksLiveness)) {
    auto Rebuilt = rebuildMachineLiveness(Function, Target);
    if (!Rebuilt)
      return std::unexpected(Rebuilt.error());
  }
  Result.Changes = {MachinePassChange::Instructions, MachinePassChange::Operands};
  Result.Preserved = PreservedAnalyses::none();
  return Result;
}

} // namespace rcc
