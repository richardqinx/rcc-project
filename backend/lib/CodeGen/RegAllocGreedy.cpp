// SPDX-License-Identifier: GPL-3.0-or-later

#include "RegAllocGreedy.h"

#include "RegAllocBase.h"
#include "rcc/CodeGen/LiveIntervals.h"
#include "rcc/CodeGen/LiveRegMatrix.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <unordered_set>
#include <vector>

namespace rcc {
namespace {

struct EvictionCandidate final {
  PhysicalRegister PhysicalReg;
  std::vector<Register> Victims;
  std::uint64_t Cost = 0;
};

[[nodiscard]] std::uint64_t saturatedAdd(std::uint64_t Left, std::uint64_t Right) noexcept {
  if (Right > std::numeric_limits<std::uint64_t>::max() - Left)
    return std::numeric_limits<std::uint64_t>::max();
  return Left + Right;
}

[[nodiscard]] std::uint64_t normalizedSpillCost(const LiveInterval &Interval) noexcept {
  std::uint64_t Length = 0;
  for (const LiveSegment &Segment : Interval.MainRange.segments())
    Length = saturatedAdd(Length, Segment.End.value() - Segment.Start.value());
  Length = std::max<std::uint64_t>(1, Length);
  constexpr std::uint64_t Scale = std::uint64_t{1} << 20U;
  const std::uint64_t Weight = std::max<std::uint64_t>(1, Interval.SpillWeight);
  const std::uint64_t ScaledWeight = Weight > std::numeric_limits<std::uint64_t>::max() / Scale
                                         ? std::numeric_limits<std::uint64_t>::max()
                                         : Weight * Scale;
  return std::max<std::uint64_t>(1, ScaledWeight / Length);
}

[[nodiscard]] bool isBetterEviction(const EvictionCandidate &Left,
                                    const EvictionCandidate &Right) noexcept {
  if (Left.Cost != Right.Cost)
    return Left.Cost < Right.Cost;
  if (Left.Victims.size() != Right.Victims.size())
    return Left.Victims.size() < Right.Victims.size();
  return Left.PhysicalReg.number() < Right.PhysicalReg.number();
}

class GreedyAllocatorEngine final : public RegAllocBase {
public:
  GreedyAllocatorEngine(MachineFunction &Function, const TargetDescription &Target,
                        const TargetRegisterAllocationInfo &TargetInfo,
                        MachineFunctionAnalysisManager &Analyses, RegisterAllocationOptions Options)
      : RegAllocBase(Function, Target, TargetInfo, Analyses, Options) {}

private:
  [[nodiscard]] Expected<std::optional<EvictionCandidate>>
  evictionCandidate(Register VirtualReg, PhysicalRegister Candidate,
                    const LiveIntervalsAnalysis::Result &Intervals, const LiveRegMatrix &Matrix,
                    const VirtRegMap &Mapping) const {
    const LiveInterval *Current = Intervals.interval(VirtualReg);
    if (Current == nullptr)
      return std::optional<EvictionCandidate>{};

    EvictionCandidate Result{.PhysicalReg = Candidate, .Victims = {}, .Cost = 0};
    for (const RegisterInterference &Interference :
         Matrix.collectInterferences(VirtualReg, Candidate)) {
      if (Interference.Kind != RegisterInterferenceKind::VirtualRegister ||
          !Interference.VirtualReg) {
        return std::optional<EvictionCandidate>{};
      }
      const Register Victim = *Interference.VirtualReg;
      if (std::ranges::find(Result.Victims, Victim) != Result.Victims.end())
        continue;
      if (!Mapping.isSpillable(Victim))
        return std::optional<EvictionCandidate>{};
      const LiveInterval *VictimInterval = Intervals.interval(Victim);
      if (VictimInterval == nullptr)
        return std::optional<EvictionCandidate>{};
      Result.Victims.push_back(Victim);
      Result.Cost = std::max(Result.Cost, normalizedSpillCost(*VictimInterval));
    }
    if (Result.Victims.empty())
      return std::optional<EvictionCandidate>{};

    const std::uint64_t CurrentCost = normalizedSpillCost(*Current);
    if (Mapping.isSpillable(VirtualReg) && Result.Cost >= CurrentCost)
      return std::optional<EvictionCandidate>{};
    std::ranges::sort(Result.Victims, [](Register Left, Register Right) {
      return Left.opaqueValue() < Right.opaqueValue();
    });
    return std::optional<EvictionCandidate>{std::move(Result)};
  }

  [[nodiscard]] Expected<AllocationRoundResult>
  allocateRound(const LiveIntervalsAnalysis::Result &Intervals, VirtRegMap &Mapping) override {
    LiveRegMatrix Matrix(Function, Target, Intervals);
    AllocationRoundResult Result;
    std::unordered_set<Register> Spilled;
    std::vector<const LiveInterval *> Worklist = orderedAllocationIntervals(Intervals, Function);
    std::ranges::stable_sort(Worklist, [&](const LiveInterval *Left, const LiveInterval *Right) {
      const bool LeftSpillable = Mapping.isSpillable(Left->Reg);
      const bool RightSpillable = Mapping.isSpillable(Right->Reg);
      if (LeftSpillable != RightSpillable)
        return !LeftSpillable;
      if (Left->SpillWeight != Right->SpillWeight)
        return Left->SpillWeight > Right->SpillWeight;
      return false;
    });

    for (const LiveInterval *Interval : Worklist) {
      const Register VirtualReg = Interval->Reg;
      if (regalloc_detail::hasTerminatorDefinition(Function, Target, VirtualReg)) {
        auto Marked = Mapping.setSpillable(VirtualReg, false);
        if (!Marked)
          return std::unexpected(Marked.error());
      }
      auto Candidates = regalloc_detail::allocationOrder(Function, Target, TargetInfo, VirtualReg);
      if (!Candidates)
        return std::unexpected(Candidates.error());

      std::optional<PhysicalRegister> Assignment;
      for (const PhysicalRegister Candidate : *Candidates) {
        if (Matrix.checkInterference(VirtualReg, Candidate).Kind ==
            RegisterInterferenceKind::Free) {
          Assignment = Candidate;
          break;
        }
      }

      std::optional<EvictionCandidate> BestEviction;
      if (!Assignment) {
        for (const PhysicalRegister Candidate : *Candidates) {
          auto CandidateEviction =
              evictionCandidate(VirtualReg, Candidate, Intervals, Matrix, Mapping);
          if (!CandidateEviction)
            return std::unexpected(CandidateEviction.error());
          if (!*CandidateEviction)
            continue;
          if (!BestEviction || isBetterEviction(**CandidateEviction, *BestEviction))
            BestEviction = std::move(**CandidateEviction);
        }
      }

      if (BestEviction) {
        for (const Register Victim : BestEviction->Victims) {
          auto Unassigned = Matrix.unassign(Victim);
          if (!Unassigned)
            return std::unexpected(Unassigned.error());
          VirtRegMapEntry *Entry = Mapping.entry(Victim);
          if (Entry == nullptr)
            return makeUnexpected(ErrorCode::InternalError,
                                  "greedy eviction found an unknown virtual register");
          Entry->PhysicalReg.reset();
          Entry->State = VirtRegAllocationState::Unassigned;
          if (Spilled.insert(Victim).second)
            Result.Spilled.push_back(Victim);
        }
        Assignment = BestEviction->PhysicalReg;
      }

      if (!Assignment) {
        if (!Mapping.isSpillable(VirtualReg)) {
          return makeUnexpected(ErrorCode::RegisterAllocationFailed,
                                "greedy allocator cannot assign a non-spillable virtual register");
        }
        if (Spilled.insert(VirtualReg).second)
          Result.Spilled.push_back(VirtualReg);
        continue;
      }

      auto MatrixAssignment = Matrix.assign(VirtualReg, *Assignment);
      if (!MatrixAssignment)
        return std::unexpected(MatrixAssignment.error());
      auto MapAssignment = Mapping.assign(VirtualReg, *Assignment);
      if (!MapAssignment)
        return std::unexpected(MapAssignment.error());
    }
    return Result;
  }
};

[[nodiscard]] const MachinePassContract &greedyContract() {
  static const MachinePassContract Contract = [] {
    MachinePassContract Value{
        .Name = "greedy",
        .Requires = {MachineFunctionProperty::NoPHIs, MachineFunctionProperty::TracksLiveness},
        .Sets = {},
        .Clears = {MachineFunctionProperty::IsSSA},
        .RequiredAnalyses = {MachineFunctionAnalysisManager::id<LiveIntervalsAnalysis>(),
                             MachineFunctionAnalysisManager::id<VirtRegMapAnalysis>()},
        .VerificationStage = std::nullopt,
    };
    Value.ThreadSafety = PassThreadSafety::ThreadSafe;
    return Value;
  }();
  return Contract;
}

} // namespace

const MachinePassContract &GreedyAllocatorPass::contract() const noexcept {
  return greedyContract();
}

Expected<MachinePassResult> GreedyAllocatorPass::run(MachineFunction &Function,
                                                     const TargetDescription &Target,
                                                     MachineFunctionAnalysisManager &Analyses) {
  if (Info == nullptr)
    return makeUnexpected(ErrorCode::InvalidArgument, "greedy allocator requires target behavior");
  const std::vector<PhysicalRegister> FunctionReserved =
      regalloc_detail::functionReservedRegisters(Function, Target, *Info);
  auto Reserved = regalloc_detail::reserveFunctionRegisters(Function, FunctionReserved);
  if (!Reserved)
    return std::unexpected(Reserved.error());
  auto Mapping = Analyses.getResult<VirtRegMapAnalysis>(Function, Target);
  if (!Mapping)
    return std::unexpected(Mapping.error());
  GreedyAllocatorEngine Allocator(Function, Target, *Info, Analyses, Options);
  auto Allocated = Allocator.run(**Mapping);
  if (!Allocated)
    return std::unexpected(Allocated.error());
  Allocated->Changed |= *Reserved;
  MachinePassResult Result = regalloc_detail::passResult(*Allocated);
  Result.Preserved.preserve<VirtRegMapAnalysis>();
  return Result;
}

std::unique_ptr<MachineFunctionPass>
createGreedyAllocatorPass(const TargetRegisterAllocationInfo &Info,
                          RegisterAllocationOptions Options) {
  return std::make_unique<GreedyAllocatorPass>(Info, Options);
}

} // namespace rcc
