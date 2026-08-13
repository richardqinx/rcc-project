// SPDX-License-Identifier: GPL-3.0-or-later

#include "RegAllocBasic.h"

#include "RegAllocBase.h"
#include "rcc/CodeGen/LiveIntervals.h"
#include "rcc/CodeGen/LiveRegMatrix.h"
#include "rcc/CodeGen/LiveVariables.h"

#include <algorithm>
#include <memory>
#include <ranges>

namespace rcc {
namespace {

class BasicAllocatorEngine final : public RegAllocBase {
public:
  BasicAllocatorEngine(MachineFunction &Function, const TargetDescription &Target,
                       const TargetRegisterAllocationInfo &TargetInfo,
                       MachineFunctionAnalysisManager &Analyses, RegisterAllocationOptions Options)
      : RegAllocBase(Function, Target, TargetInfo, Analyses, Options) {}

private:
  [[nodiscard]] Expected<AllocationRoundResult>
  allocateRound(const LiveIntervalsAnalysis::Result &Intervals, VirtRegMap &Mapping) override {
    LiveRegMatrix Matrix(Function, Target, Intervals);
    AllocationRoundResult Result;
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
      bool Assigned = false;
      for (const PhysicalRegister Candidate : *Candidates) {
        if (Matrix.checkInterference(VirtualReg, Candidate).Kind != RegisterInterferenceKind::Free)
          continue;
        auto MatrixAssignment = Matrix.assign(VirtualReg, Candidate);
        if (!MatrixAssignment)
          continue;
        auto MapAssignment = Mapping.assign(VirtualReg, Candidate);
        if (!MapAssignment)
          return std::unexpected(MapAssignment.error());
        Assigned = true;
        break;
      }
      if (!Assigned) {
        if (!Mapping.isSpillable(VirtualReg)) {
          return makeUnexpected(ErrorCode::RegisterAllocationFailed,
                                "BasicAllocator cannot assign a non-spillable virtual register");
        }
        Result.Spilled.push_back(VirtualReg);
      }
    }
    return Result;
  }
};

[[nodiscard]] const MachinePassContract &basicContract() {
  static const MachinePassContract Contract = [] {
    MachinePassContract Value{
        .Name = "BasicAllocator",
        .Requires = {MachineFunctionProperty::Selected, MachineFunctionProperty::NoPHIs,
                     MachineFunctionProperty::TiedOpsRewritten,
                     MachineFunctionProperty::TracksLiveness},
        .Sets = {MachineFunctionProperty::NoVRegs},
        .Clears = {},
        .RequiredAnalyses = {MachineFunctionAnalysisManager::id<LiveIntervalsAnalysis>()},
        .VerificationStage = PipelineStage::RegisterAllocated,
    };
    Value.ThreadSafety = PassThreadSafety::ThreadSafe;
    return Value;
  }();
  return Contract;
}

} // namespace

const MachinePassContract &BasicAllocatorPass::contract() const noexcept {
  return basicContract();
}

Expected<MachinePassResult> BasicAllocatorPass::run(MachineFunction &Function,
                                                    const TargetDescription &Target,
                                                    MachineFunctionAnalysisManager &Analyses) {
  if (Info == nullptr)
    return makeUnexpected(ErrorCode::InvalidArgument, "BasicAllocator requires target behavior");
  auto Required = regalloc_detail::requireAllocationProperties(Function, contract());
  if (!Required)
    return std::unexpected(Required.error());
  auto Trial = regalloc_detail::cloneForAllocation(Function, Target);
  if (!Trial)
    return std::unexpected(Trial.error());
  const std::vector<PhysicalRegister> FunctionReserved =
      regalloc_detail::functionReservedRegisters(Function, Target, *Info);
  auto TrialReserved =
      regalloc_detail::reserveFunctionRegisters(*Trial->ClonedFunction, FunctionReserved);
  if (!TrialReserved)
    return std::unexpected(TrialReserved.error());
  MachineFunctionAnalysisManager TrialAnalyses;
  auto Registered = registerStage11MachineAnalyses(TrialAnalyses);
  if (!Registered)
    return std::unexpected(Registered.error());
  BasicAllocatorEngine TrialAllocator(*Trial->ClonedFunction, Target, *Info, TrialAnalyses,
                                      Options);
  auto TrialRun = TrialAllocator.run();
  if (!TrialRun)
    return std::unexpected(TrialRun.error());
  auto TrialVerification =
      regalloc_detail::verifyAllocationTrial(*Trial->ClonedFunction, Target, contract());
  if (!TrialVerification)
    return std::unexpected(TrialVerification.error());
  BasicAllocatorEngine Allocator(Function, Target, *Info, Analyses, Options);
  auto Allocated = Allocator.run();
  if (!Allocated) {
    return std::unexpected(withErrorContext(
        Allocated.error(), "BasicAllocator diverged after a successful transaction preflight: "));
  }
  auto Reserved = regalloc_detail::reserveFunctionRegisters(Function, FunctionReserved);
  if (!Reserved)
    return std::unexpected(Reserved.error());
  Allocated->Changed |= *Reserved;
  return regalloc_detail::passResult(*Allocated);
}

std::unique_ptr<MachineFunctionPass>
createBasicAllocatorPass(const TargetRegisterAllocationInfo &Info,
                         RegisterAllocationOptions Options) {
  return std::make_unique<BasicAllocatorPass>(Info, Options);
}

} // namespace rcc
