// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/PHIElimination.h"

#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineCFGUpdater.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineFunctionCloner.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineLiveness.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/CodeGen/TargetInstrInfo.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rcc {
namespace {

struct EdgeKey final {
  const MachineBasicBlock *From = nullptr;
  const MachineBasicBlock *To = nullptr;

  friend bool operator==(const EdgeKey &, const EdgeKey &) noexcept = default;
};

struct EdgeKeyHash final {
  [[nodiscard]] std::size_t operator()(const EdgeKey &Key) const noexcept {
    const std::size_t FromHash = std::hash<const MachineBasicBlock *>{}(Key.From);
    const std::size_t ToHash = std::hash<const MachineBasicBlock *>{}(Key.To);
    return FromHash ^ (ToHash << 1U);
  }
};

struct RegisterValue final {
  Register Reg;
  SubRegIndex SubReg;
  RegisterOperandFlags Flags;
  std::uint32_t TargetFlags = 0;
};

struct PhiIncoming final {
  RegisterValue Value;
  MachineBasicBlock *Predecessor = nullptr;
  bool IsUndef = false;
};

struct PhiPlan final {
  MachineInstr *Instruction = nullptr;
  RegisterValue Destination;
  Register FreshIncoming;
  std::vector<PhiIncoming> IncomingValues;
};

[[nodiscard]] Error prefixedError(const Error &Source, std::string Prefix) {
  Prefix.append(Source.message());
  return Error(Source.code(), std::move(Prefix));
}

[[nodiscard]] Expected<void> requireProperties(const MachineFunction &Function,
                                               const MachinePassContract &Contract) {
  const MachineFunctionProperties Missing = Contract.Requires.missingFrom(Function.properties());
  if (Missing.empty())
    return {};
  return makeUnexpected(ErrorCode::VerificationError,
                        Contract.Name + " requires properties: " + Missing.toString());
}

[[nodiscard]] bool isPhi(const MachineInstr &Instruction, const TargetDescription &Target) {
  const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction.opcode());
  return Descriptor != nullptr && Descriptor->Flags.has(InstructionFlag::Phi);
}

[[nodiscard]] bool isMeta(const MachineInstr &Instruction, const TargetDescription &Target) {
  const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction.opcode());
  return Descriptor != nullptr && Descriptor->Flags.has(InstructionFlag::Meta);
}

[[nodiscard]] MachineInstr *firstTerminator(MachineBasicBlock &Block,
                                            const TargetDescription &Target) {
  for (MachineInstr *Instruction = Block.firstInstruction(); Instruction != nullptr;
       Instruction = Instruction->next()) {
    const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction->opcode());
    if (Descriptor != nullptr && Descriptor->Flags.has(InstructionFlag::Terminator))
      return Instruction;
  }
  return nullptr;
}

[[nodiscard]] MachineInstr *firstCodeAfterPhiRegion(MachineBasicBlock &Block,
                                                    const TargetDescription &Target) {
  for (MachineInstr *Instruction = Block.firstInstruction(); Instruction != nullptr;
       Instruction = Instruction->next()) {
    if (!isPhi(*Instruction, Target) && !isMeta(*Instruction, Target))
      return Instruction;
  }
  return nullptr;
}

[[nodiscard]] RegisterOperandFlags generatedDefFlags(RegisterOperandFlags Original) noexcept {
  RegisterOperandFlags Result;
  if (Original.has(RegisterOperandFlag::Undef))
    Result.set(RegisterOperandFlag::Undef);
  return Result;
}

[[nodiscard]] Expected<void> copyDebugLocation(MachineInstrBuilder &Builder,
                                               const MachineInstr *Source) {
  if (Source == nullptr || !Source->debugLocation())
    return {};
  return Builder.setDebugLocation(*Source->debugLocation());
}

[[nodiscard]] Expected<Register> cloneRegisterConstraints(MachineFunction &Function,
                                                          Register Source) {
  if (!Source.isVirtual()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "SSA deconstruction requires a virtual PHI destination");
  }
  auto SourceInfo = Function.registerInfo().virtualRegisterInfo(Source);
  if (!SourceInfo)
    return std::unexpected(SourceInfo.error());
  const VirtualRegisterInfo Info = SourceInfo->get();

  auto Fresh = Function.registerInfo().createVirtualRegister();
  if (!Fresh)
    return std::unexpected(Fresh.error());
  if (Info.Type) {
    auto SetType = Function.registerInfo().setType(*Fresh, *Info.Type);
    if (!SetType)
      return std::unexpected(SetType.error());
  }
  if (Info.Bank) {
    auto SetBank = Function.registerInfo().setRegisterBank(*Fresh, *Info.Bank);
    if (!SetBank)
      return std::unexpected(SetBank.error());
  }
  if (Info.RegClass) {
    auto SetClass = Function.registerInfo().setRegisterClass(*Fresh, *Info.RegClass);
    if (!SetClass)
      return std::unexpected(SetClass.error());
  }
  for (const PhysicalRegister Hint : Info.AllocationHints) {
    auto AddedHint = Function.registerInfo().addAllocationHint(*Fresh, Hint);
    if (!AddedHint)
      return std::unexpected(AddedHint.error());
  }
  if (Info.TargetFlags != 0) {
    auto SetFlags = Function.registerInfo().setTargetRegisterFlags(*Fresh, Info.TargetFlags);
    if (!SetFlags)
      return std::unexpected(SetFlags.error());
  }
  return *Fresh;
}

[[nodiscard]] Expected<MachineInstr *>
buildCopy(MachineFunction &Function, const TargetDescription &Target,
          const TargetInstrInfo &TargetInfo, MachineBasicBlock &Block, MachineInstr *Before,
          RegisterValue Destination, RegisterValue Source, const MachineInstr *DebugSource) {
  auto Legality = TargetInfo.validateCopy(Destination.Reg, Destination.SubReg, Source.Reg,
                                          Source.SubReg, Function, Target);
  if (!Legality)
    return std::unexpected(prefixedError(Legality.error(), "illegal COPY: "));

  MachineIRBuilder Builder(Function, Target);
  auto Point =
      Before == nullptr ? Builder.setInsertPoint(Block) : Builder.setInsertPointBefore(*Before);
  if (!Point)
    return std::unexpected(Point.error());
  auto Pending = Builder.buildInstr(opcode(GenericOpcode::COPY));
  if (!Pending)
    return std::unexpected(Pending.error());
  auto Def = Pending->addDef(Destination.Reg, generatedDefFlags(Destination.Flags),
                             Destination.SubReg, Destination.TargetFlags);
  if (!Def)
    return std::unexpected(Def.error());
  auto Use = Pending->addUse(Source.Reg, {}, Source.SubReg, Source.TargetFlags);
  if (!Use)
    return std::unexpected(Use.error());
  auto Debug = copyDebugLocation(*Pending, DebugSource);
  if (!Debug)
    return std::unexpected(Debug.error());
  return Pending->build();
}

[[nodiscard]] Expected<MachineInstr *>
buildImplicitDef(MachineFunction &Function, const TargetDescription &Target,
                 MachineBasicBlock &Block, MachineInstr *Before, RegisterValue Destination,
                 const MachineInstr *DebugSource) {
  MachineIRBuilder Builder(Function, Target);
  auto Point =
      Before == nullptr ? Builder.setInsertPoint(Block) : Builder.setInsertPointBefore(*Before);
  if (!Point)
    return std::unexpected(Point.error());
  auto Pending = Builder.buildInstr(opcode(GenericOpcode::IMPLICIT_DEF));
  if (!Pending)
    return std::unexpected(Pending.error());
  auto Def = Pending->addDef(Destination.Reg, generatedDefFlags(Destination.Flags),
                             Destination.SubReg, Destination.TargetFlags);
  if (!Def)
    return std::unexpected(Def.error());
  auto Debug = copyDebugLocation(*Pending, DebugSource);
  if (!Debug)
    return std::unexpected(Debug.error());
  return Pending->build();
}

[[nodiscard]] bool isImplicitlyUndef(const MachineFunction &Function,
                                     const MachineOperand &Operand) {
  if (Operand.registerFlags().has(RegisterOperandFlag::Undef))
    return true;
  const Register *Reg = Operand.getIf<Register>();
  if (Reg == nullptr || Reg->isNoRegister())
    return true;
  if (!Reg->isVirtual())
    return false;
  const MachineOperand *Definition = Function.registerInfo().getUniqueVRegDef(*Reg);
  if (Definition == nullptr || Definition->parent() == nullptr)
    return false;
  if (Definition->subReg().value() != 0 && Definition->subReg() != Operand.subReg())
    return false;
  const Opcode DefinitionOpcode = Definition->parent()->opcode();
  return DefinitionOpcode == opcode(GenericOpcode::IMPLICIT_DEF) ||
         DefinitionOpcode == opcode(GenericOpcode::G_IMPLICIT_DEF) ||
         Definition->registerFlags().has(RegisterOperandFlag::Undef);
}

[[nodiscard]] Expected<void> validatePhiValue(const MachineFunction &Function,
                                              const TargetDescription &Target,
                                              const MachineOperand &Destination,
                                              const MachineOperand &Source) {
  const Register *DestinationReg = Destination.getIf<Register>();
  const Register *SourceReg = Source.getIf<Register>();
  if (DestinationReg == nullptr || SourceReg == nullptr)
    return makeUnexpected(ErrorCode::VerificationError, "PHI operands must name registers");
  if (SourceReg->isNoRegister())
    return {};

  if (DestinationReg->isVirtual() && SourceReg->isVirtual()) {
    auto DestinationInfo = Function.registerInfo().virtualRegisterInfo(*DestinationReg);
    if (!DestinationInfo)
      return std::unexpected(DestinationInfo.error());
    auto SourceInfo = Function.registerInfo().virtualRegisterInfo(*SourceReg);
    if (!SourceInfo)
      return std::unexpected(SourceInfo.error());
    if (DestinationInfo->get().Type && SourceInfo->get().Type &&
        DestinationInfo->get().Type != SourceInfo->get().Type) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "PHI destination and incoming value types do not match");
    }
  }
  return Target.validateTieCompatibility(Destination, Source, Function.registerInfo());
}

[[nodiscard]] Expected<std::vector<PhiPlan>> collectPhiPlans(MachineBasicBlock &Block,
                                                             MachineFunction &Function,
                                                             const TargetDescription &Target) {
  std::vector<PhiPlan> Plans;
  bool SawCode = false;
  for (MachineInstr *Instruction = Block.firstInstruction(); Instruction != nullptr;
       Instruction = Instruction->next()) {
    const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction->opcode());
    if (Descriptor == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "PHI elimination encountered an unknown opcode");
    }
    if (Instruction->opcode() == opcode(GenericOpcode::G_PHI)) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "PHI elimination requires G_PHI to be selected to PHI");
    }
    if (!Descriptor->Flags.has(InstructionFlag::Phi)) {
      SawCode |= !Descriptor->Flags.has(InstructionFlag::Meta);
      continue;
    }
    if (SawCode) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "PHI instruction appears after the leading PHI/meta region");
    }
    if (Instruction->operandCount() < 3 || (Instruction->operandCount() - 1U) % 2U != 0) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "PHI must contain a destination and incoming value/block pairs");
    }

    MachineOperand *DestinationOperand = Instruction->operandAt(0);
    const Register *Destination =
        DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
    if (Destination == nullptr || !Destination->isVirtual() || !DestinationOperand->isDef() ||
        DestinationOperand->isUse() || DestinationOperand->isImplicit()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "PHI destination must be an explicit virtual-register definition");
    }

    PhiPlan Plan;
    Plan.Instruction = Instruction;
    Plan.Destination =
        RegisterValue{*Destination, DestinationOperand->subReg(),
                      DestinationOperand->registerFlags(), DestinationOperand->targetFlags()};
    std::unordered_set<MachineBasicBlock *> SeenPredecessors;
    for (std::size_t Index = 1; Index < Instruction->operandCount(); Index += 2U) {
      MachineOperand *SourceOperand = Instruction->operandAt(Index);
      MachineOperand *BlockOperand = Instruction->operandAt(Index + 1U);
      const Register *Source =
          SourceOperand == nullptr ? nullptr : SourceOperand->getIf<Register>();
      const BasicBlockOperand *IncomingBlock =
          BlockOperand == nullptr ? nullptr : BlockOperand->getIf<BasicBlockOperand>();
      if (Source == nullptr || SourceOperand == nullptr || !SourceOperand->isUse() ||
          SourceOperand->isDef() || SourceOperand->isImplicit() || IncomingBlock == nullptr ||
          IncomingBlock->Block == nullptr || BlockOperand->isImplicit()) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "PHI incoming operands must be explicit register/block pairs");
      }
      if (std::ranges::find(Block.predecessors(), IncomingBlock->Block) ==
          Block.predecessors().end()) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "PHI incoming block is not a CFG predecessor");
      }
      if (!SeenPredecessors.insert(IncomingBlock->Block).second) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "PHI contains duplicate incoming predecessors");
      }
      auto Compatibility = validatePhiValue(Function, Target, *DestinationOperand, *SourceOperand);
      if (!Compatibility)
        return std::unexpected(Compatibility.error());
      Plan.IncomingValues.push_back(
          PhiIncoming{RegisterValue{*Source, SourceOperand->subReg(),
                                    SourceOperand->registerFlags(), SourceOperand->targetFlags()},
                      IncomingBlock->Block, isImplicitlyUndef(Function, *SourceOperand)});
    }
    if (SeenPredecessors.size() != Block.predecessors().size()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "PHI incoming blocks do not exactly cover CFG predecessors");
    }
    Plans.push_back(std::move(Plan));
  }
  return Plans;
}

[[nodiscard]] Expected<void> copyLiveIns(const MachineBasicBlock &Source,
                                         MachineBasicBlock &Destination) {
  for (const MachineBasicBlockLiveIn &LiveIn : Source.liveIns()) {
    auto Added = Destination.addLiveIn(LiveIn.PhysReg, LiveIn.Lanes);
    if (!Added)
      return std::unexpected(Added.error());
  }
  return {};
}

[[nodiscard]] Expected<void> ensurePhysicalLiveIn(MachineBasicBlock &Block, PhysicalRegister Reg,
                                                  const TargetDescription &Target) {
  const PhysicalRegisterDesc *Description = Target.registers().physicalRegister(Reg);
  if (Description == nullptr) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "PHI incoming value references an unknown physical register");
  }
  LaneMask Required;
  for (const RegisterUnitLane &Unit : Description->Units)
    Required = Required | Unit.Lanes;
  if (Required.empty()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "PHI incoming physical register has no addressable lanes");
  }

  for (const MachineBasicBlockLiveIn &LiveIn : Block.liveIns()) {
    if (LiveIn.PhysReg != Reg)
      continue;
    if (LiveIn.Lanes.contains(Required))
      return {};
    const LaneMask Combined = LiveIn.Lanes | Required;
    auto Removed = Block.removeLiveIn(Reg);
    if (!Removed)
      return std::unexpected(Removed.error());
    return Block.addLiveIn(Reg, Combined);
  }
  return Block.addLiveIn(Reg, Required);
}

[[nodiscard]] Expected<MachineBasicBlock *>
placePhiCopiesOnEdge(MachineFunction &Function, const TargetDescription &Target,
                     const TargetInstrInfo &TargetInfo, MachineBasicBlock &From,
                     MachineBasicBlock &To,
                     std::unordered_map<EdgeKey, MachineBasicBlock *, EdgeKeyHash> &Cache,
                     MachinePassResult &PassResult) {
  if (From.successors().size() <= 1)
    return &From;

  const EdgeKey Key{&From, &To};
  if (const auto Existing = Cache.find(Key); Existing != Cache.end())
    return Existing->second;
  if (std::ranges::none_of(From.successors(), [&To](const MachineSuccessor &Successor) {
        return Successor.Block == &To;
      })) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "PHI incoming predecessor has no edge to its successor");
  }

  auto SourceFallthrough = TargetInfo.isFallthroughEdge(From, To, Function, Target);
  if (!SourceFallthrough)
    return std::unexpected(SourceFallthrough.error());

  MachineBasicBlock *LayoutPredecessor = To.previous();
  if (LayoutPredecessor != nullptr && LayoutPredecessor != &From) {
    auto OtherFallthrough = TargetInfo.isFallthroughEdge(*LayoutPredecessor, To, Function, Target);
    if (!OtherFallthrough)
      return std::unexpected(OtherFallthrough.error());
    if (*OtherFallthrough) {
      auto Branch = TargetInfo.insertBranch(*LayoutPredecessor, To, Function, Target);
      if (!Branch)
        return std::unexpected(Branch.error());
      PassResult.Changed = true;
      PassResult.Changes.add(MachinePassChange::Instructions);
    }
  }

  auto EdgeBlockResult = Function.createBasicBlock();
  if (!EdgeBlockResult)
    return std::unexpected(EdgeBlockResult.error());
  MachineBasicBlock *EdgeBlock = *EdgeBlockResult;
  auto Inserted = Function.insertBlockBefore(To, *EdgeBlock);
  if (!Inserted)
    return std::unexpected(Inserted.error());

  auto Rewritten = TargetInfo.rewriteBranchTarget(From, To, *EdgeBlock, Function, Target);
  if (!Rewritten)
    return std::unexpected(Rewritten.error());
  if (!*Rewritten && !*SourceFallthrough) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "PHI elimination cannot split an indirect or unmodeled CFG edge");
  }

  auto Split = Function.cfgUpdater().splitEdge(From, To, *EdgeBlock);
  if (!Split)
    return std::unexpected(Split.error());
  auto LiveIns = copyLiveIns(To, *EdgeBlock);
  if (!LiveIns)
    return std::unexpected(LiveIns.error());
  auto Branch = TargetInfo.insertBranch(*EdgeBlock, To, Function, Target);
  if (!Branch)
    return std::unexpected(Branch.error());

  Cache.emplace(Key, EdgeBlock);
  PassResult.Changed = true;
  PassResult.Changes.add(MachinePassChange::CFG);
  PassResult.Changes.add(MachinePassChange::BlockLayout);
  PassResult.Changes.add(MachinePassChange::Instructions);
  if (*Rewritten)
    PassResult.Changes.add(MachinePassChange::Operands);
  return EdgeBlock;
}

[[nodiscard]] Expected<void> repairTrackedLiveness(MachineFunction &Function,
                                                   const TargetDescription &Target,
                                                   MachinePassResult &Result) {
  if (!Result.Changed || !Function.properties().has(MachineFunctionProperty::TracksLiveness)) {
    return {};
  }
  auto Rebuilt = rebuildMachineLiveness(Function, Target);
  if (!Rebuilt)
    return std::unexpected(Rebuilt.error());
  if (*Rebuilt)
    Result.Changes.add(MachinePassChange::Operands);
  return {};
}

} // namespace

const MachinePassContract &PHIEliminationPass::contract() const noexcept {
  static const MachinePassContract Contract{
      .Name = "phi-node-elimination",
      .Requires = {MachineFunctionProperty::IsSSA, MachineFunctionProperty::Selected,
                   MachineFunctionProperty::RegBankSelected},
      .Sets = {MachineFunctionProperty::NoPHIs},
      .Clears = {MachineFunctionProperty::IsSSA},
      .RequiredAnalyses = {},
      .VerificationStage = PipelineStage::SSAEliminated,
  };
  return Contract;
}

Expected<MachinePassResult> PHIEliminationPass::run(MachineFunction &Function,
                                                    const TargetDescription &Target,
                                                    MachineAnalysisManager &Analyses) {
  auto Required = requireProperties(Function, contract());
  if (!Required)
    return std::unexpected(Required.error());

  auto Trial = cloneMachineFunction(
      Function, Target,
      MachineFunctionCloneOptions{.NewStableID = Function.stableID(), .NewName = std::nullopt});
  if (!Trial) {
    return std::unexpected(
        prefixedError(Trial.error(), "PHI-elimination transaction setup failed: "));
  }
  MachineActionReplay Actions(Analyses, Trial->InstructionStableIDs);
  auto TrialResult = runImplementation(*Trial->ClonedFunction, Target, Actions);
  if (!TrialResult)
    return std::unexpected(TrialResult.error());

  Actions.beginReplay();
  auto Result = runImplementation(Function, Target, Actions);
  if (!Result) {
    return std::unexpected(prefixedError(
        Result.error(), "PHI elimination diverged after a successful transaction preflight: "));
  }
  auto Replayed = Actions.finishReplay();
  if (!Replayed)
    return std::unexpected(Replayed.error());
  return Result;
}

Expected<MachinePassResult> PHIEliminationPass::runImplementation(MachineFunction &Function,
                                                                  const TargetDescription &Target,
                                                                  MachineActionReplay &Actions) {
  MachinePassResult Result;
  std::unordered_map<EdgeKey, MachineBasicBlock *, EdgeKeyHash> SplitEdges;
  const std::vector<MachineBasicBlock *> Blocks(Function.allBlocks().begin(),
                                                Function.allBlocks().end());

  for (MachineBasicBlock *Block : Blocks) {
    auto Plans = collectPhiPlans(*Block, Function, Target);
    if (!Plans)
      return std::unexpected(Plans.error());
    if (Plans->empty())
      continue;

    for (PhiPlan &Plan : *Plans) {
      auto Accepted = Actions.request("eliminate-phi", Function, Plan.Instruction->stableID());
      if (!Accepted)
        return std::unexpected(Accepted.error());
      if (!*Accepted) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "required PHI elimination action was rejected");
      }
      auto Fresh = cloneRegisterConstraints(Function, Plan.Destination.Reg);
      if (!Fresh)
        return std::unexpected(Fresh.error());
      Plan.FreshIncoming = *Fresh;
    }

    for (PhiPlan &Plan : *Plans) {
      for (const PhiIncoming &Incoming : Plan.IncomingValues) {
        auto EdgeBlock = placePhiCopiesOnEdge(Function, Target, Info, *Incoming.Predecessor, *Block,
                                              SplitEdges, Result);
        if (!EdgeBlock)
          return std::unexpected(EdgeBlock.error());
        if (*EdgeBlock != Incoming.Predecessor) {
          const std::optional<PhysicalRegister> Physical = Incoming.Value.Reg.physicalRegister();
          if (Physical) {
            auto LiveIn = ensurePhysicalLiveIn(**EdgeBlock, *Physical, Target);
            if (!LiveIn)
              return std::unexpected(LiveIn.error());
          }
        }
        MachineInstr *Terminator = firstTerminator(**EdgeBlock, Target);
        RegisterValue Fresh{Plan.FreshIncoming, Plan.Destination.SubReg, Plan.Destination.Flags,
                            Plan.Destination.TargetFlags};
        Expected<MachineInstr *> Transfer =
            Incoming.IsUndef ? buildImplicitDef(Function, Target, **EdgeBlock, Terminator, Fresh,
                                                Plan.Instruction)
                             : buildCopy(Function, Target, Info, **EdgeBlock, Terminator, Fresh,
                                         Incoming.Value, Plan.Instruction);
        if (!Transfer)
          return std::unexpected(Transfer.error());
        Result.Changed = true;
        Result.Changes.add(MachinePassChange::Instructions);
      }
    }

    MachineInstr *InsertionPoint = firstCodeAfterPhiRegion(*Block, Target);
    for (PhiPlan &Plan : *Plans) {
      RegisterValue Fresh{
          Plan.FreshIncoming, Plan.Destination.SubReg, {}, Plan.Destination.TargetFlags};
      auto Copy = buildCopy(Function, Target, Info, *Block, InsertionPoint, Plan.Destination, Fresh,
                            Plan.Instruction);
      if (!Copy)
        return std::unexpected(Copy.error());
      Result.Changed = true;
      Result.Changes.add(MachinePassChange::Instructions);
    }

    for (PhiPlan &Plan : *Plans) {
      auto Removed = Block->removeInstruction(*Plan.Instruction);
      if (!Removed)
        return std::unexpected(Removed.error());
    }
    Result.Changes.add(MachinePassChange::Registers);
  }

  if (Result.Changed && Function.properties().has(MachineFunctionProperty::TracksLiveness)) {
    auto Repaired = repairTrackedLiveness(Function, Target, Result);
    if (!Repaired)
      return std::unexpected(
          prefixedError(Repaired.error(), "PHI-elimination liveness repair failed: "));
  }
  Result.Preserved = Result.Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  return Result;
}

} // namespace rcc
