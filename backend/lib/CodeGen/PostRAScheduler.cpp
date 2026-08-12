// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/PostRAScheduler.h"

#include "MachineSchedulerInternal.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineFunctionCloner.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineLiveness.h"
#include "rcc/CodeGen/MachineLivenessRepair.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/RegisterMask.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/CodeGen/TargetRegisterInfo.h"
#include "rcc/CodeGen/TargetSchedulingModel.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace rcc {
namespace {

[[nodiscard]] const MachinePassContract &postRASchedulerContract() {
  static const MachinePassContract Contract = [] {
    MachinePassContract Value{
        .Name = "postmisched",
        .Requires = {MachineFunctionProperty::Selected, MachineFunctionProperty::NoPHIs,
                     MachineFunctionProperty::TiedOpsRewritten, MachineFunctionProperty::NoVRegs,
                     MachineFunctionProperty::TracksLiveness},
        .Sets = {},
        .Clears = {},
        .RequiredAnalyses = {MachineAnalysisManager::id<LiveVariablesAnalysis>()},
        .VerificationStage = PipelineStage::RegisterAllocated,
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

struct PhysicalUnitAccess final {
  RegisterUnitID Unit;
  LaneMask Lanes;
};

struct OperandFlagUpdate final {
  MachineOperand *Operand = nullptr;
  RegisterOperandFlags Flags;
};

[[nodiscard]] LaneMask normalizeUnitLanes(LaneMask UnitViewLanes) noexcept {
  std::uint64_t View = UnitViewLanes.rawValue();
  std::uint64_t Normalized = 0;
  std::uint32_t UnitLane = 0;
  while (View != 0) {
    Normalized |= std::uint64_t{1} << UnitLane;
    View &= View - 1U;
    ++UnitLane;
  }
  return LaneMask::fromRaw(Normalized);
}

[[nodiscard]] Expected<std::vector<PhysicalUnitAccess>>
physicalOperandUnits(const MachineOperand &Operand, const TargetDescription &Target) {
  const Register *Reg = Operand.getIf<Register>();
  std::optional<PhysicalRegister> Physical =
      Reg == nullptr ? std::nullopt : Reg->physicalRegister();
  if (!Physical) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "post-RA liveness repair encountered a virtual register");
  }
  if (Operand.subReg().value() != 0) {
    Physical = Target.registers().subRegister(*Physical, Operand.subReg());
    if (!Physical) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "post-RA liveness repair encountered an invalid subregister");
    }
  }
  const PhysicalRegisterDesc *Description = Target.registers().physicalRegister(*Physical);
  if (Description == nullptr || Description->Units.empty()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "post-RA liveness repair encountered a unitless register");
  }
  std::vector<PhysicalUnitAccess> Result;
  Result.reserve(Description->Units.size());
  for (const RegisterUnitLane &Unit : Description->Units) {
    if (Unit.Lanes.empty()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "post-RA liveness repair encountered empty register-unit lanes");
    }
    Result.push_back({Unit.Unit, normalizeUnitLanes(Unit.Lanes)});
  }
  return Result;
}

void addPhysicalRegister(RegisterUnitLaneSet &Live, const PhysicalRegisterDesc &Register) {
  for (const RegisterUnitLane &Unit : Register.Units)
    Live.add(Unit.Unit, normalizeUnitLanes(Unit.Lanes));
}

[[nodiscard]] bool intersects(const RegisterUnitLaneSet &Live,
                              const std::vector<PhysicalUnitAccess> &Units) noexcept {
  return std::ranges::any_of(Units, [&Live](const PhysicalUnitAccess &Unit) {
    return Live.intersects(Unit.Unit, Unit.Lanes);
  });
}

void addUnits(RegisterUnitLaneSet &Live, const std::vector<PhysicalUnitAccess> &Units) {
  for (const PhysicalUnitAccess &Unit : Units)
    Live.add(Unit.Unit, Unit.Lanes);
}

void removeUnits(RegisterUnitLaneSet &Live, const std::vector<PhysicalUnitAccess> &Units) {
  for (const PhysicalUnitAccess &Unit : Units)
    Live.remove(Unit.Unit, Unit.Lanes);
}

[[nodiscard]] bool isReservedPhysicalRegister(Register Reg, const MachineFunction &Function,
                                              const TargetDescription &Target) noexcept {
  const std::optional<PhysicalRegister> Physical = Reg.physicalRegister();
  return Physical && (Function.registerInfo().isReserved(*Physical) ||
                      Target.registers().isReservedRegister(*Physical));
}

[[nodiscard]] OperandFlagUpdate &getFlagUpdate(std::vector<OperandFlagUpdate> &Updates,
                                               MachineOperand &Operand) {
  const auto Position = std::ranges::find_if(
      Updates, [&Operand](const OperandFlagUpdate &Update) { return Update.Operand == &Operand; });
  if (Position != Updates.end())
    return *Position;
  Updates.push_back({&Operand, Operand.registerFlags()});
  return Updates.back();
}

[[nodiscard]] Expected<std::vector<OperandFlagUpdate>>
buildBlockLivenessRepairPlan(MachineBasicBlock &Block, const TargetDescription &Target,
                             const MachineBlockLiveness &Boundary) {
  RegisterUnitLaneSet Live = Boundary.PhysicalLiveOut;
  std::vector<OperandFlagUpdate> Updates;
  for (MachineInstr *Instruction = Block.lastInstruction(); Instruction != nullptr;
       Instruction = Instruction->previous()) {
    for (const MachineOperand &Operand : Instruction->operands()) {
      const auto *Mask = Operand.getIf<LiveOutRegisterMaskOperand>();
      if (Mask == nullptr)
        continue;
      if (!Mask->Mask) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "post-RA liveness repair encountered a null live-out mask");
      }
      for (const PhysicalRegisterDesc &Register : Target.registers().physicalRegisters()) {
        if (Mask->Mask->preserves(Register.Reg))
          addPhysicalRegister(Live, Register);
      }
    }

    for (std::size_t Index = 0; Index < Instruction->operandCount(); ++Index) {
      MachineOperand *Operand = Instruction->operandAt(Index);
      if (Operand == nullptr || !Operand->isRegister())
        continue;
      OperandFlagUpdate &Update = getFlagUpdate(Updates, *Operand);
      Update.Flags.clear(RegisterOperandFlag::Dead);
      if (!Operand->isDef())
        continue;
      const Register *Reg = Operand->getIf<Register>();
      if (Reg == nullptr || Reg->isNoRegister())
        continue;
      auto Units = physicalOperandUnits(*Operand, Target);
      if (!Units)
        return std::unexpected(Units.error());
      if (!intersects(Live, *Units))
        Update.Flags.set(RegisterOperandFlag::Dead);
    }

    for (const MachineOperand &Operand : Instruction->operands()) {
      if (Operand.isRegister() && Operand.isDef()) {
        const Register *Reg = Operand.getIf<Register>();
        if (Reg == nullptr || Reg->isNoRegister())
          continue;
        auto Units = physicalOperandUnits(Operand, Target);
        if (!Units)
          return std::unexpected(Units.error());
        removeUnits(Live, *Units);
        continue;
      }
      const auto *Mask = Operand.getIf<RegisterMaskOperand>();
      if (Mask == nullptr)
        continue;
      if (!Mask->Mask) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "post-RA liveness repair encountered a null register mask");
      }
      RegisterUnitLaneSet Clobbers;
      for (const PhysicalRegisterDesc &Register : Target.registers().physicalRegisters()) {
        if (Mask->Mask->clobbers(Register.Reg))
          addPhysicalRegister(Clobbers, Register);
      }
      Live.subtract(Clobbers);
    }

    for (std::size_t Index = 0; Index < Instruction->operandCount(); ++Index) {
      MachineOperand *Operand = Instruction->operandAt(Index);
      if (Operand == nullptr || !Operand->isRegister())
        continue;
      OperandFlagUpdate &Update = getFlagUpdate(Updates, *Operand);
      Update.Flags.clear(RegisterOperandFlag::Kill);
      if (!Operand->isUse() || Operand->isDebugUse() ||
          Operand->registerFlags().has(RegisterOperandFlag::Undef)) {
        continue;
      }
      const Register *Reg = Operand->getIf<Register>();
      if (Reg == nullptr || Reg->isNoRegister())
        continue;
      if (isReservedPhysicalRegister(*Reg, Block.function(), Target))
        continue;
      auto Units = physicalOperandUnits(*Operand, Target);
      if (!Units)
        return std::unexpected(Units.error());
      if (!intersects(Live, *Units))
        Update.Flags.set(RegisterOperandFlag::Kill);
      addUnits(Live, *Units);
    }
  }
  if (Live != Boundary.PhysicalLiveIn) {
    return makeUnexpected(ErrorCode::InternalError,
                          "post-RA local liveness repair changed the block boundary");
  }
  return Updates;
}

} // namespace

Expected<bool> repairPostRAMachineBlockLiveness(std::span<MachineBasicBlock *const> Blocks,
                                                const TargetDescription &Target,
                                                const LiveVariablesAnalysis::Result &Liveness) {
  std::vector<OperandFlagUpdate> Updates;
  for (MachineBasicBlock *Block : Blocks) {
    const MachineBlockLiveness *Boundary = Liveness.block(*Block);
    if (Boundary == nullptr) {
      return makeUnexpected(ErrorCode::InternalError,
                            "post-RA scheduler lost a block liveness boundary");
    }
    auto BlockUpdates = buildBlockLivenessRepairPlan(*Block, Target, *Boundary);
    if (!BlockUpdates)
      return std::unexpected(BlockUpdates.error());
    Updates.insert(Updates.end(), BlockUpdates->begin(), BlockUpdates->end());
  }
  bool Changed = false;
  for (const OperandFlagUpdate &Update : Updates) {
    Changed |= Update.Operand->registerFlags() != Update.Flags;
    auto Applied = Update.Operand->setRegisterFlags(Update.Flags);
    if (!Applied)
      return std::unexpected(Applied.error());
  }
  return Changed;
}

const MachinePassContract &PostRASchedulerPass::contract() const noexcept {
  return postRASchedulerContract();
}

Expected<MachinePassResult> PostRASchedulerPass::run(MachineFunction &Function,
                                                     const TargetDescription &Target,
                                                     MachineAnalysisManager &Analyses) {
  if (!Function.properties().containsAll(contract().Requires)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "post-RA scheduler requires allocated tracked-liveness MachineIR");
  }
  auto Trial = cloneMachineFunction(
      Function, Target,
      MachineFunctionCloneOptions{.NewStableID = Function.stableID(), .NewName = std::nullopt});
  if (!Trial) {
    return std::unexpected(
        prefixedError(Trial.error(), "post-RA scheduler transaction setup failed: "));
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
  MachineActionReplay Actions(Analyses, Trial->InstructionStableIDs);
  auto TrialRun = runMachineScheduler(*Trial->ClonedFunction, Target, *Model,
                                      ScheduleDAGMode::PostRegisterAllocation, **TrialLiveness,
                                      **TrialFrequencies, Options, &Actions);
  if (!TrialRun)
    return std::unexpected(TrialRun.error());
  auto TrialRepair =
      repairPostRAMachineBlockLiveness(TrialRun->ChangedBlocks, Target, **TrialLiveness);
  if (!TrialRepair)
    return std::unexpected(TrialRepair.error());

  auto Liveness = Analyses.getResult<LiveVariablesAnalysis>(Function, Target);
  if (!Liveness)
    return std::unexpected(Liveness.error());
  auto Frequencies = Analyses.getResult<MachineBlockFrequencyAnalysis>(Function, Target);
  if (!Frequencies)
    return std::unexpected(Frequencies.error());
  Actions.beginReplay();
  auto Run = runMachineScheduler(Function, Target, *Model, ScheduleDAGMode::PostRegisterAllocation,
                                 **Liveness, **Frequencies, Options, &Actions);
  if (!Run) {
    return std::unexpected(prefixedError(
        Run.error(), "post-RA scheduler diverged after a successful transaction preflight: "));
  }
  auto Replayed = Actions.finishReplay();
  if (!Replayed)
    return std::unexpected(Replayed.error());
  auto Repaired = repairPostRAMachineBlockLiveness(Run->ChangedBlocks, Target, **Liveness);
  if (!Repaired)
    return std::unexpected(Repaired.error());

  MachinePassResult Result;
  Result.Changed = Run->Changed || *Repaired;
  if (!Result.Changed) {
    Result.Preserved = PreservedAnalyses::all();
    return Result;
  }
  Result.Changes = {MachinePassChange::Instructions, MachinePassChange::Operands};
  Result.Preserved.preserve<LiveVariablesAnalysis>();
  return Result;
}

} // namespace rcc
