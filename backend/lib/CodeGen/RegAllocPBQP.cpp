// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/RegAllocPBQP.h"

#include "RegAllocBase.h"
#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/LiveIntervals.h"
#include "rcc/CodeGen/LiveRegMatrix.h"
#include "rcc/CodeGen/LiveVariables.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/PBQP/Solver.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace rcc {
namespace {

struct PBQPAllocationNode final {
  Register VirtualReg;
  pbqp::NodeID Node;
  std::vector<std::optional<PhysicalRegister>> Options;
};

[[nodiscard]] std::map<std::pair<std::uint32_t, std::uint32_t>, MachineInstrStableID>
copyRelatedRegisters(const MachineFunction &Function) {
  std::map<std::pair<std::uint32_t, std::uint32_t>, MachineInstrStableID> Result;
  for (const MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
       Block = Block->next()) {
    for (const MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      if (Instruction->opcode() != opcode(GenericOpcode::COPY) || Instruction->operandCount() < 2)
        continue;
      const Register *Destination = Instruction->operands()[0].getIf<Register>();
      const Register *Source = Instruction->operands()[1].getIf<Register>();
      if (Destination == nullptr || Source == nullptr || !Destination->isVirtual() ||
          !Source->isVirtual() || *Destination == *Source)
        continue;
      std::uint32_t Left = Destination->opaqueValue();
      std::uint32_t Right = Source->opaqueValue();
      if (Right < Left)
        std::swap(Left, Right);
      Result.try_emplace({Left, Right}, Instruction->stableID());
    }
  }
  return Result;
}

[[nodiscard]] std::string_view reductionName(pbqp::ReductionKind Kind) noexcept {
  switch (Kind) {
  case pbqp::ReductionKind::DegreeZero:
    return "degree-zero";
  case pbqp::ReductionKind::DegreeOne:
    return "degree-one";
  case pbqp::ReductionKind::DegreeTwo:
    return "degree-two";
  case pbqp::ReductionKind::Heuristic:
    return "heuristic";
  }
  return "unknown";
}

[[nodiscard]] MachineInstrStableID actionInstructionForRegister(const MachineFunction &Function,
                                                                Register Reg) noexcept {
  const MachineOperand *Definition = Function.registerInfo().getUniqueVRegDef(Reg);
  const MachineInstr *Instruction = Definition == nullptr ? nullptr : Definition->parent();
  return Instruction == nullptr ? MachineInstrStableID{} : Instruction->stableID();
}

[[nodiscard]] pbqp::Cost boundedCost(std::uint64_t Value) noexcept {
  const std::uint64_t Limit = static_cast<std::uint64_t>(pbqp::InfiniteCost - 1);
  return static_cast<pbqp::Cost>(std::min(Value, Limit));
}

class PBQPAllocatorEngine final : public RegAllocBase {
public:
  PBQPAllocatorEngine(MachineFunction &Function, const TargetDescription &Target,
                      const TargetRegisterAllocationInfo &TargetInfo,
                      MachineFunctionAnalysisManager &Analyses, RegisterAllocationOptions Options,
                      MachineActionReplay *Actions)
      : RegAllocBase(Function, Target, TargetInfo, Analyses, Options, Actions) {}

private:
  [[nodiscard]] Expected<AllocationRoundResult>
  allocateRound(const LiveIntervalsAnalysis::Result &Intervals, VirtRegMap &Mapping) override {
    LiveRegMatrix FixedMatrix(Function, Target, Intervals);
    pbqp::Graph Problem;
    std::vector<PBQPAllocationNode> Nodes;
    for (const LiveInterval *Interval : orderedAllocationIntervals(Intervals, Function)) {
      const Register VirtualReg = Interval->Reg;
      const bool TerminatorDefined =
          regalloc_detail::hasTerminatorDefinition(Function, Target, VirtualReg);
      if (TerminatorDefined) {
        auto Marked = Mapping.setSpillable(VirtualReg, false);
        if (!Marked)
          return std::unexpected(Marked.error());
      }
      auto ClassID = regalloc_detail::registerClassFor(Function, VirtualReg);
      if (!ClassID)
        return std::unexpected(ClassID.error());
      auto Candidates = regalloc_detail::allocationOrder(Function, Target, TargetInfo, VirtualReg);
      if (!Candidates)
        return std::unexpected(Candidates.error());
      std::erase_if(*Candidates, [&](PhysicalRegister Candidate) {
        return FixedMatrix.checkInterference(VirtualReg, Candidate).Kind !=
               RegisterInterferenceKind::Free;
      });

      std::vector<std::optional<PhysicalRegister>> NodeOptions;
      NodeOptions.reserve(Candidates->size() + 1U);
      NodeOptions.push_back(std::nullopt);
      for (const PhysicalRegister Candidate : *Candidates)
        NodeOptions.push_back(Candidate);
      pbqp::CostVector Costs(NodeOptions.size(), 0);
      if (!Mapping.isSpillable(VirtualReg)) {
        Costs[0] = pbqp::InfiniteCost;
      } else {
        const std::uint64_t TargetSpillCost = TargetInfo.spillCost(Function, *Interval, *ClassID);
        const std::uint64_t Weight = std::max<std::uint64_t>(1, Interval->SpillWeight);
        const std::uint64_t Scaled =
            TargetSpillCost > std::numeric_limits<std::uint64_t>::max() / Weight
                ? std::numeric_limits<std::uint64_t>::max()
                : TargetSpillCost * Weight;
        const std::uint64_t Adjusted =
            Scaled == std::numeric_limits<std::uint64_t>::max() ? Scaled : Scaled + 1U;
        Costs[0] = boundedCost(Adjusted);
      }
      auto Info = Function.registerInfo().virtualRegisterInfo(VirtualReg);
      if (!Info)
        return std::unexpected(Info.error());
      for (std::size_t Index = 1; Index < NodeOptions.size(); ++Index) {
        const auto CandidateOption = NodeOptions[Index];
        if (!CandidateOption) {
          return makeUnexpected(ErrorCode::InternalError,
                                "PBQP allocation node has an unexpected spill option");
        }
        const PhysicalRegister Candidate = CandidateOption.value();
        std::uint64_t Cost =
            TargetInfo.physicalRegisterCost(Function, VirtualReg, Candidate, *ClassID, true);
        const auto Hint = std::ranges::find(Info->get().AllocationHints, Candidate);
        const std::uint64_t HintCost =
            Hint == Info->get().AllocationHints.end()
                ? 2U
                : static_cast<std::uint64_t>(Hint - Info->get().AllocationHints.begin());
        Cost = Cost > std::numeric_limits<std::uint64_t>::max() - HintCost
                   ? std::numeric_limits<std::uint64_t>::max()
                   : Cost + HintCost;
        Costs[Index] = boundedCost(Cost);
      }
      auto NodeID = Problem.addNode(std::move(Costs), VirtualReg.opaqueValue());
      if (!NodeID)
        return std::unexpected(NodeID.error());
      Nodes.push_back(PBQPAllocationNode{VirtualReg, *NodeID, std::move(NodeOptions)});
    }

    const auto CopyPairs = copyRelatedRegisters(Function);
    for (std::size_t LeftIndex = 0; LeftIndex < Nodes.size(); ++LeftIndex) {
      const PBQPAllocationNode &Left = Nodes[LeftIndex];
      const LiveInterval *LeftInterval = Intervals.interval(Left.VirtualReg);
      for (std::size_t RightIndex = LeftIndex + 1U; RightIndex < Nodes.size(); ++RightIndex) {
        const PBQPAllocationNode &Right = Nodes[RightIndex];
        const LiveInterval *RightInterval = Intervals.interval(Right.VirtualReg);
        if (LeftInterval == nullptr || RightInterval == nullptr)
          continue;
        std::uint32_t LeftKey = Left.VirtualReg.opaqueValue();
        std::uint32_t RightKey = Right.VirtualReg.opaqueValue();
        if (RightKey < LeftKey)
          std::swap(LeftKey, RightKey);
        const auto CopyPosition = CopyPairs.find({LeftKey, RightKey});
        bool CopyRelated = Options.EnableCopyCoalescing && CopyPosition != CopyPairs.end();
        if (CopyRelated && Actions != nullptr) {
          auto Accepted = Actions->request("coalesce-copy", Function, CopyPosition->second);
          if (!Accepted)
            return std::unexpected(Accepted.error());
          CopyRelated = *Accepted;
        }
        pbqp::CostMatrix Costs(Left.Options.size(), Right.Options.size(), 0);
        bool HasCost = false;
        for (std::size_t LeftOption = 1; LeftOption < Left.Options.size(); ++LeftOption) {
          for (std::size_t RightOption = 1; RightOption < Right.Options.size(); ++RightOption) {
            const auto LeftRegister = Left.Options[LeftOption];
            const auto RightRegister = Right.Options[RightOption];
            if (!LeftRegister || !RightRegister) {
              return makeUnexpected(ErrorCode::InternalError,
                                    "PBQP edge has an unexpected spill option");
            }
            const PhysicalRegister LeftReg = LeftRegister.value();
            const PhysicalRegister RightReg = RightRegister.value();
            if (regalloc_detail::intervalsOverlapOnPhysicalRegisters(
                    *LeftInterval, LeftReg, *RightInterval, RightReg, Target.registers())) {
              Costs.at(LeftOption, RightOption) = pbqp::InfiniteCost;
              HasCost = true;
            } else if (CopyRelated && LeftReg == RightReg) {
              Costs.at(LeftOption, RightOption) = -1;
              HasCost = true;
            }
          }
        }
        if (!HasCost)
          continue;
        const std::uint64_t StableKey =
            (static_cast<std::uint64_t>(Left.VirtualReg.opaqueValue()) << 32U) |
            Right.VirtualReg.opaqueValue();
        auto Edge = Problem.addEdge(Left.Node, Right.Node, std::move(Costs), StableKey);
        if (!Edge)
          return std::unexpected(Edge.error());
      }
    }

    pbqp::DeterministicSolver Solver;
    auto Solution = Solver.solve(Problem, [&](const pbqp::SolverAction &Action) -> Expected<bool> {
      if (Actions == nullptr)
        return true;
      if (Action.Node.value() >= Nodes.size()) {
        return makeUnexpected(ErrorCode::InternalError,
                              "PBQP action names an unknown allocation node");
      }
      const Register VirtualReg = Nodes[Action.Node.value()].VirtualReg;
      std::string Rule =
          Action.Kind == pbqp::SolverActionKind::Reduction ? "pbqp-reduce." : "pbqp-choice.";
      Rule.append(reductionName(Action.Reduction));
      if (Action.Kind == pbqp::SolverActionKind::Choice) {
        Rule.append(".option-");
        Rule.append(std::to_string(Action.Value));
      }
      return Actions->request(Rule, Function, actionInstructionForRegister(Function, VirtualReg));
    });
    if (!Solution)
      return std::unexpected(Solution.error());

    LiveRegMatrix AssignedMatrix(Function, Target, Intervals);
    AllocationRoundResult Result;
    for (const PBQPAllocationNode &Node : Nodes) {
      const auto Selection = Solution->selection(Node.Node);
      if (!Selection)
        return makeUnexpected(ErrorCode::InternalError,
                              "PBQP solver returned an invalid selection");
      const std::size_t SelectedOption = Selection.value();
      if (SelectedOption >= Node.Options.size()) {
        return makeUnexpected(ErrorCode::InternalError,
                              "PBQP solver selected an out-of-range option");
      }
      const auto AssignedOption = Node.Options[SelectedOption];
      if (!AssignedOption) {
        Result.Spilled.push_back(Node.VirtualReg);
        continue;
      }
      const PhysicalRegister Assigned = AssignedOption.value();
      auto MatrixAssignment = AssignedMatrix.assign(Node.VirtualReg, Assigned);
      if (!MatrixAssignment)
        return makeUnexpected(ErrorCode::InternalError,
                              "PBQP solution violates its encoded interference constraints");
      auto MapAssignment = Mapping.assign(Node.VirtualReg, Assigned);
      if (!MapAssignment)
        return std::unexpected(MapAssignment.error());
    }
    return Result;
  }
};

[[nodiscard]] const MachinePassContract &pbqpContract() {
  static const MachinePassContract Contract = [] {
    MachinePassContract Value{
        .Name = "PBQPAllocator",
        .Requires = {MachineFunctionProperty::NoPHIs},
        .Sets = {MachineFunctionProperty::NoVRegs},
        .Clears = {MachineFunctionProperty::IsSSA},
        .RequiredAnalyses = {MachineFunctionAnalysisManager::id<LiveIntervalsAnalysis>()},
        .VerificationStage = PipelineStage::RegisterAllocated,
    };
    Value.ThreadSafety = PassThreadSafety::ThreadSafe;
    return Value;
  }();
  return Contract;
}

} // namespace

const MachinePassContract &PBQPAllocatorPass::contract() const noexcept {
  return pbqpContract();
}

Expected<MachinePassResult> PBQPAllocatorPass::run(MachineFunction &Function,
                                                   const TargetDescription &Target,
                                                   MachineFunctionAnalysisManager &Analyses) {
  if (Info == nullptr)
    return makeUnexpected(ErrorCode::InvalidArgument, "PBQPAllocator requires target behavior");
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
  MachineActionReplay Actions(Analyses, Trial->InstructionStableIDs);
  PBQPAllocatorEngine TrialAllocator(*Trial->ClonedFunction, Target, *Info, TrialAnalyses, Options,
                                     &Actions);
  auto TrialRun = TrialAllocator.run();
  if (!TrialRun)
    return std::unexpected(TrialRun.error());
  auto TrialVerification =
      regalloc_detail::verifyAllocationTrial(*Trial->ClonedFunction, Target, contract());
  if (!TrialVerification)
    return std::unexpected(TrialVerification.error());
  Actions.beginReplay();
  PBQPAllocatorEngine Allocator(Function, Target, *Info, Analyses, Options, &Actions);
  auto Allocated = Allocator.run();
  if (!Allocated) {
    return std::unexpected(withErrorContext(
        Allocated.error(), "PBQPAllocator diverged after a successful transaction preflight: "));
  }
  auto Replayed = Actions.finishReplay();
  if (!Replayed)
    return std::unexpected(Replayed.error());
  auto Reserved = regalloc_detail::reserveFunctionRegisters(Function, FunctionReserved);
  if (!Reserved)
    return std::unexpected(Reserved.error());
  Allocated->Changed |= *Reserved;
  return regalloc_detail::passResult(*Allocated);
}

} // namespace rcc
