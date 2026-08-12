// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/LiveVariables.h"

#include "rcc/CodeGen/LiveInterval.h"
#include "rcc/CodeGen/LiveIntervals.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/RegisterMask.h"
#include "rcc/CodeGen/SlotIndexes.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/CodeGen/TargetRegisterInfo.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rcc {

namespace {
[[nodiscard]] LaneMask subtractLanes(LaneMask Left, LaneMask Right) noexcept {
  return LaneMask::fromRaw(Left.rawValue() & ~Right.rawValue());
}

template <typename KeyT>
void addLanes(std::unordered_map<KeyT, LaneMask> &Values, KeyT Key, LaneMask Lanes) {
  if (Lanes.empty())
    return;
  const auto Position = Values.find(Key);
  if (Position == Values.end()) {
    Values.emplace(Key, Lanes);
    return;
  }
  Position->second = Position->second | Lanes;
}

template <typename KeyT>
void removeLanes(std::unordered_map<KeyT, LaneMask> &Values, KeyT Key, LaneMask Lanes) {
  const auto Position = Values.find(Key);
  if (Position == Values.end() || Lanes.empty())
    return;
  Position->second = subtractLanes(Position->second, Lanes);
  if (Position->second.empty())
    Values.erase(Position);
}

template <typename KeyT>
[[nodiscard]] LaneMask lanesFor(const std::unordered_map<KeyT, LaneMask> &Values,
                                KeyT Key) noexcept {
  const auto Position = Values.find(Key);
  return Position == Values.end() ? LaneMask{} : Position->second;
}

template <typename KeyT>
[[nodiscard]] bool containsLanes(const std::unordered_map<KeyT, LaneMask> &Values, KeyT Key,
                                 LaneMask Lanes) noexcept {
  const LaneMask Present = lanesFor(Values, Key);
  return !Present.empty() && Present.contains(Lanes);
}

template <typename KeyT>
[[nodiscard]] bool intersectsLanes(const std::unordered_map<KeyT, LaneMask> &Values, KeyT Key,
                                   LaneMask Lanes) noexcept {
  return lanesFor(Values, Key).intersects(Lanes);
}

template <typename KeyT>
void uniteLanes(std::unordered_map<KeyT, LaneMask> &Values,
                const std::unordered_map<KeyT, LaneMask> &Other) {
  for (const auto &[Key, Lanes] : Other)
    addLanes(Values, Key, Lanes);
}

template <typename KeyT>
void subtractLaneSets(std::unordered_map<KeyT, LaneMask> &Values,
                      const std::unordered_map<KeyT, LaneMask> &Other) {
  for (const auto &[Key, Lanes] : Other)
    removeLanes(Values, Key, Lanes);
}

[[nodiscard]] Expected<void> addOperandUse(RegisterLaneSet &VirtualUses, LiveRegUnits &PhysicalUses,
                                           const MachineOperand &Operand,
                                           const MachineFunction &Function,
                                           const TargetDescription &Target) {
  const Register *Reg = Operand.getIf<Register>();
  if (Reg == nullptr || Reg->isNoRegister() || Operand.isDebugUse() ||
      Operand.registerFlags().has(RegisterOperandFlag::Undef)) {
    return {};
  }
  if (Reg->isVirtual()) {
    auto Lanes = virtualOperandLanes(Operand, Function, Target);
    if (!Lanes)
      return std::unexpected(Lanes.error());
    VirtualUses.add(*Reg, *Lanes);
    return {};
  }
  if (isReservedPhysicalRegister(*Reg, Function, Target))
    return {};
  auto Units = physicalOperandUnits(Operand, Target);
  if (!Units)
    return std::unexpected(Units.error());
  for (const PhysicalUnitAccess &Unit : *Units)
    PhysicalUses.add(Unit.Unit, Unit.Lanes);
  return {};
}

[[nodiscard]] Expected<void> addOperandDef(RegisterLaneSet &VirtualDefs, LiveRegUnits &PhysicalDefs,
                                           const MachineOperand &Operand,
                                           const MachineFunction &Function,
                                           const TargetDescription &Target) {
  const Register *Reg = Operand.getIf<Register>();
  if (Reg == nullptr || Reg->isNoRegister())
    return {};
  if (Reg->isVirtual()) {
    auto Lanes = virtualOperandLanes(Operand, Function, Target);
    if (!Lanes)
      return std::unexpected(Lanes.error());
    VirtualDefs.add(*Reg, *Lanes);
    return {};
  }
  auto Units = physicalOperandUnits(Operand, Target);
  if (!Units)
    return std::unexpected(Units.error());
  for (const PhysicalUnitAccess &Unit : *Units)
    PhysicalDefs.add(Unit.Unit, Unit.Lanes);
  return {};
}

struct LivenessEdgeKey final {
  const MachineBasicBlock *Predecessor = nullptr;
  const MachineBasicBlock *Successor = nullptr;

  friend bool operator==(const LivenessEdgeKey &, const LivenessEdgeKey &) = default;
};

struct LivenessEdgeKeyHash final {
  [[nodiscard]] std::size_t operator()(const LivenessEdgeKey &Key) const noexcept {
    const std::size_t Left = std::hash<const MachineBasicBlock *>{}(Key.Predecessor);
    const std::size_t Right = std::hash<const MachineBasicBlock *>{}(Key.Successor);
    return Left ^ (Right + 0x9e3779b9U + (Left << 6U) + (Left >> 2U));
  }
};

struct EdgeLivenessUses final {
  RegisterLaneSet Virtual;
  LiveRegUnits Physical;
};

[[nodiscard]] Expected<LiveVariablesAnalysis::Result>
computeLiveVariables(const MachineFunction &Function, const TargetDescription &Target) {
  LiveVariablesAnalysis::Result Result;
  std::vector<const MachineBasicBlock *> Blocks;
  std::unordered_map<LivenessEdgeKey, EdgeLivenessUses, LivenessEdgeKeyHash> EdgeUses;
  for (const MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
       Block = Block->next()) {
    Blocks.push_back(Block);
    (void)Result.getOrCreateBlock(*Block);
  }

  for (const MachineBasicBlock *Block : Blocks) {
    MachineBlockLiveness &Liveness = Result.getOrCreateBlock(*Block);
    if (Block == Function.firstBlock()) {
      for (const PhysicalRegisterLiveIn &LiveIn : Function.registerInfo().liveIns()) {
        const PhysicalRegisterDesc *Description =
            Target.registers().physicalRegister(LiveIn.PhysReg);
        if (Description == nullptr) {
          return makeUnexpected(ErrorCode::VerificationError,
                                "function live-in references an unknown physical register");
        }
        addPhysicalRegister(Liveness.PhysicalUses, *Description);
        if (LiveIn.InitialVirtualRegister) {
          auto Lanes = virtualRegisterLanes(*LiveIn.InitialVirtualRegister, Function, Target);
          if (!Lanes)
            return std::unexpected(Lanes.error());
          Liveness.VirtualDefs.add(*LiveIn.InitialVirtualRegister, *Lanes);
        }
      }
    }
    for (const MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction->opcode());
      if (Descriptor == nullptr) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "liveness encountered an unknown instruction descriptor");
      }
      const bool IsPhi = Descriptor->Flags.has(InstructionFlag::Phi);
      if (IsPhi) {
        if (Instruction->operandCount() < 3 || (Instruction->operandCount() - 1U) % 2U != 0) {
          return makeUnexpected(ErrorCode::VerificationError,
                                "liveness encountered a malformed PHI instruction");
        }
        std::unordered_set<const MachineBasicBlock *> IncomingBlocks;
        for (std::size_t Index = 1; Index < Instruction->operandCount(); Index += 2U) {
          const MachineOperand *Incoming = Instruction->operandAt(Index);
          const MachineOperand *PredecessorOperand = Instruction->operandAt(Index + 1U);
          const auto *Predecessor = PredecessorOperand == nullptr
                                        ? nullptr
                                        : PredecessorOperand->getIf<BasicBlockOperand>();
          if (Incoming == nullptr || Predecessor == nullptr || Predecessor->Block == nullptr) {
            return makeUnexpected(ErrorCode::VerificationError,
                                  "liveness encountered a malformed PHI incoming pair");
          }
          const MachineBasicBlock *IncomingBlock = Predecessor->Block;
          const bool HasPredecessor = std::ranges::find(Block->predecessors(), IncomingBlock) !=
                                      Block->predecessors().end();
          const bool HasSuccessor = std::ranges::any_of(
              IncomingBlock->successors(),
              [Block](const MachineSuccessor &Successor) { return Successor.Block == Block; });
          if (&IncomingBlock->function() != &Function || !IncomingBlock->isLinked() ||
              !HasPredecessor || !HasSuccessor || !IncomingBlocks.insert(IncomingBlock).second) {
            return makeUnexpected(ErrorCode::VerificationError,
                                  "liveness encountered a PHI incoming without a unique CFG edge");
          }
          EdgeLivenessUses &Uses = EdgeUses[{IncomingBlock, Block}];
          auto Added = addOperandUse(Uses.Virtual, Uses.Physical, *Incoming, Function, Target);
          if (!Added)
            return std::unexpected(Added.error());
        }
      }

      RegisterLaneSet InstructionVirtualUses;
      RegisterLaneSet InstructionVirtualDefs;
      LiveRegUnits InstructionPhysicalUses;
      LiveRegUnits InstructionPhysicalDefs;
      for (std::size_t Index = 0; Index < Instruction->operandCount(); ++Index) {
        const MachineOperand &Operand = *Instruction->operandAt(Index);
        if (Operand.isRegister()) {
          if (Operand.isUse() && (!IsPhi || Index == 0)) {
            auto Use = addOperandUse(InstructionVirtualUses, InstructionPhysicalUses, Operand,
                                     Function, Target);
            if (!Use)
              return std::unexpected(Use.error());
          }
          if (Operand.isDef()) {
            auto Def = addOperandDef(InstructionVirtualDefs, InstructionPhysicalDefs, Operand,
                                     Function, Target);
            if (!Def)
              return std::unexpected(Def.error());
          }
          continue;
        }
        if (const auto *MaskOperand = Operand.getIf<RegisterMaskOperand>();
            MaskOperand != nullptr) {
          if (!MaskOperand->Mask)
            return makeUnexpected(ErrorCode::VerificationError,
                                  "liveness encountered a null register mask");
          auto Added = addRegisterMaskDefs(InstructionPhysicalDefs, *MaskOperand->Mask, Target);
          if (!Added)
            return std::unexpected(Added.error());
          continue;
        }
        if (const auto *MaskOperand = Operand.getIf<LiveOutRegisterMaskOperand>();
            MaskOperand != nullptr) {
          if (!MaskOperand->Mask)
            return makeUnexpected(ErrorCode::VerificationError,
                                  "liveness encountered a null live-out register mask");
          auto Added = addRegisterMaskUses(InstructionPhysicalUses, *MaskOperand->Mask, Target);
          if (!Added)
            return std::unexpected(Added.error());
        }
      }

      RegisterLaneSet NewVirtualUses = InstructionVirtualUses;
      NewVirtualUses.subtract(Liveness.VirtualDefs);
      Liveness.VirtualUses.unite(NewVirtualUses);
      Liveness.VirtualDefs.unite(InstructionVirtualDefs);

      LiveRegUnits NewPhysicalUses = InstructionPhysicalUses;
      NewPhysicalUses.subtract(Liveness.PhysicalDefs);
      Liveness.PhysicalUses.unite(NewPhysicalUses);
      Liveness.PhysicalDefs.unite(InstructionPhysicalDefs);
    }
  }

  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (const MachineBasicBlock *Block : Blocks | std::views::reverse) {
      MachineBlockLiveness &Liveness = Result.getOrCreateBlock(*Block);
      RegisterLaneSet VirtualOut;
      LiveRegUnits PhysicalOut;
      for (const MachineSuccessor &Successor : Block->successors()) {
        if (Successor.Block == nullptr || Result.block(*Successor.Block) == nullptr) {
          return makeUnexpected(ErrorCode::VerificationError,
                                "liveness encountered an invalid CFG successor");
        }
        const MachineBlockLiveness &SuccessorLiveness = *Result.block(*Successor.Block);
        VirtualOut.unite(SuccessorLiveness.VirtualLiveIn);
        PhysicalOut.unite(SuccessorLiveness.PhysicalLiveIn);
        const auto Edge = EdgeUses.find({Block, Successor.Block});
        if (Edge != EdgeUses.end()) {
          VirtualOut.unite(Edge->second.Virtual);
          PhysicalOut.unite(Edge->second.Physical);
        }
      }

      RegisterLaneSet VirtualIn = VirtualOut;
      VirtualIn.subtract(Liveness.VirtualDefs);
      VirtualIn.unite(Liveness.VirtualUses);
      LiveRegUnits PhysicalIn = PhysicalOut;
      PhysicalIn.subtract(Liveness.PhysicalDefs);
      PhysicalIn.unite(Liveness.PhysicalUses);

      if (VirtualOut != Liveness.VirtualLiveOut || VirtualIn != Liveness.VirtualLiveIn ||
          PhysicalOut != Liveness.PhysicalLiveOut || PhysicalIn != Liveness.PhysicalLiveIn) {
        Liveness.VirtualLiveOut = std::move(VirtualOut);
        Liveness.VirtualLiveIn = std::move(VirtualIn);
        Liveness.PhysicalLiveOut = std::move(PhysicalOut);
        Liveness.PhysicalLiveIn = std::move(PhysicalIn);
        Changed = true;
      }
    }
  }
  return Result;
}
[[nodiscard]] Expected<std::vector<MachineBasicBlockLiveIn>>
canonicalPhysicalLiveIns(const LiveRegUnits &Units, const TargetDescription &Target) {
  struct Candidate final {
    const PhysicalRegisterDesc *Description = nullptr;
    LaneMask RegisterLanes;
    LiveRegUnits Coverage;
    std::size_t ExtraLanes = 0;
    bool Selected = false;
  };

  std::vector<Candidate> Candidates;
  for (const PhysicalRegisterDesc &Description : Target.registers().physicalRegisters()) {
    const LaneMask FullLanes = physicalRegisterLanes(Description);
    std::uint64_t SelectableLanes = 0;
    LaneMask RelevantLanes;
    for (const RegisterUnitLane &Unit : Description.Units) {
      RelevantLanes = RelevantLanes | denormalizeUnitLanes(Units.lanes(Unit.Unit), Unit.Lanes);
    }
    std::uint64_t PendingLanes = RelevantLanes.rawValue();
    while (PendingLanes != 0) {
      const std::uint64_t Lane = std::uint64_t{1} << std::countr_zero(PendingLanes);
      const LiveRegUnits Coverage = physicalRegisterCoverage(Description, LaneMask::fromRaw(Lane));
      if (isSubset(Coverage, Units))
        SelectableLanes |= Lane;
      PendingLanes &= PendingLanes - 1U;
    }
    if (SelectableLanes == 0)
      continue;
    Candidate Value;
    Value.Description = &Description;
    Value.RegisterLanes = LaneMask::fromRaw(SelectableLanes);
    Value.Coverage = physicalRegisterCoverage(Description, Value.RegisterLanes);
    Value.ExtraLanes = static_cast<std::size_t>(std::popcount(FullLanes.rawValue()) -
                                                std::popcount(SelectableLanes));
    Candidates.push_back(std::move(Value));
  }

  LiveRegUnits Covered;
  std::vector<MachineBasicBlockLiveIn> Result;
  while (Covered != Units) {
    Candidate *Best = nullptr;
    std::size_t BestNewLanes = 0;
    std::size_t BestCoverage = 0;
    for (Candidate &Value : Candidates) {
      if (Value.Selected)
        continue;
      LiveRegUnits NewCoverage = Value.Coverage;
      NewCoverage.subtract(Covered);
      const std::size_t NewLanes = laneCount(NewCoverage);
      if (NewLanes == 0)
        continue;
      const std::size_t Coverage = laneCount(Value.Coverage);
      if (Best == nullptr || NewLanes > BestNewLanes ||
          (NewLanes == BestNewLanes && Value.ExtraLanes < Best->ExtraLanes) ||
          (NewLanes == BestNewLanes && Value.ExtraLanes == Best->ExtraLanes &&
           Coverage > BestCoverage) ||
          (NewLanes == BestNewLanes && Value.ExtraLanes == Best->ExtraLanes &&
           Coverage == BestCoverage &&
           Value.Description->Reg.number() < Best->Description->Reg.number())) {
        Best = &Value;
        BestNewLanes = NewLanes;
        BestCoverage = Coverage;
      }
    }
    if (Best == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "physical live-in units cannot be represented by target registers");
    }
    Best->Selected = true;
    Covered.unite(Best->Coverage);
    const LaneMask FullLanes = physicalRegisterLanes(*Best->Description);
    const LaneMask Lanes = Best->RegisterLanes == FullLanes ? LaneMask::all() : Best->RegisterLanes;
    Result.push_back({Best->Description->Reg, Lanes});
  }
  std::ranges::sort(Result,
                    [](const MachineBasicBlockLiveIn &Left, const MachineBasicBlockLiveIn &Right) {
                      return Left.PhysReg.number() < Right.PhysReg.number();
                    });
  return Result;
}

[[nodiscard]] Expected<LiveRegUnits> liveInUnits(std::span<const MachineBasicBlockLiveIn> LiveIns,
                                                 const TargetDescription &Target) {
  LiveRegUnits Result;
  for (const MachineBasicBlockLiveIn &LiveIn : LiveIns) {
    const PhysicalRegisterDesc *Description = Target.registers().physicalRegister(LiveIn.PhysReg);
    if (Description == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "block live-in references an unknown physical register");
    }
    Result.unite(physicalRegisterCoverage(*Description, LiveIn.Lanes));
  }
  return Result;
}

struct OperandFlagPlan final {
  MachineInstrStableID Instruction;
  std::size_t OperandIndex = 0;
  RegisterOperandFlags Original;
  RegisterOperandFlags Rebuilt;
  bool KillIsValid = false;
  bool DeadIsValid = false;
};

struct BlockLivenessPlan final {
  MachineBlockStableID Block;
  std::vector<MachineBasicBlockLiveIn> OriginalLiveIns;
  std::vector<MachineBasicBlockLiveIn> RebuiltLiveIns;
};

struct LivenessRebuildPlan final {
  std::vector<BlockLivenessPlan> Blocks;
  std::vector<OperandFlagPlan> Operands;
};

[[nodiscard]] Expected<bool>
liveSetIntersects(const MachineOperand &Operand, const RegisterLaneSet &Virtual,
                  const LiveRegUnits &Physical, const MachineFunction &Function,
                  const TargetDescription &Target, bool TreatReservedAsLive) {
  const Register *Reg = Operand.getIf<Register>();
  if (Reg == nullptr || Reg->isNoRegister())
    return false;
  if (Reg->isVirtual()) {
    auto Lanes = virtualOperandLanes(Operand, Function, Target);
    if (!Lanes)
      return std::unexpected(Lanes.error());
    return Virtual.intersects(*Reg, *Lanes);
  }
  if (TreatReservedAsLive && isReservedPhysicalRegister(*Reg, Function, Target))
    return true;
  auto Units = physicalOperandUnits(Operand, Target);
  if (!Units)
    return std::unexpected(Units.error());
  return std::ranges::any_of(*Units, [&Physical](const PhysicalUnitAccess &Unit) {
    return Physical.intersects(Unit.Unit, Unit.Lanes);
  });
}

[[nodiscard]] Expected<void> removeOperandFromLiveSet(const MachineOperand &Operand,
                                                      RegisterLaneSet &Virtual,
                                                      LiveRegUnits &Physical,
                                                      const MachineFunction &Function,
                                                      const TargetDescription &Target) {
  const Register *Reg = Operand.getIf<Register>();
  if (Reg == nullptr || Reg->isNoRegister())
    return {};
  if (Reg->isVirtual()) {
    auto Lanes = virtualOperandLanes(Operand, Function, Target);
    if (!Lanes)
      return std::unexpected(Lanes.error());
    Virtual.remove(*Reg, *Lanes);
    return {};
  }
  auto Units = physicalOperandUnits(Operand, Target);
  if (!Units)
    return std::unexpected(Units.error());
  for (const PhysicalUnitAccess &Unit : *Units)
    Physical.remove(Unit.Unit, Unit.Lanes);
  return {};
}

[[nodiscard]] Expected<void> addOperandToLiveSet(const MachineOperand &Operand,
                                                 RegisterLaneSet &Virtual, LiveRegUnits &Physical,
                                                 const MachineFunction &Function,
                                                 const TargetDescription &Target) {
  return addOperandUse(Virtual, Physical, Operand, Function, Target);
}

[[nodiscard]] Expected<LiveRegUnits> requiredLocalPhysicalLiveIns(const MachineBasicBlock &Block,
                                                                  const MachineFunction &Function,
                                                                  const TargetDescription &Target) {
  RegisterLaneSet Virtual;
  LiveRegUnits Physical;
  for (const MachineInstr *Instruction = Block.lastInstruction(); Instruction != nullptr;
       Instruction = Instruction->previous()) {
    const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction->opcode());
    if (Descriptor == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "liveness verification encountered an unknown instruction descriptor");
    }
    const bool IsPhi = Descriptor->Flags.has(InstructionFlag::Phi);
    for (const MachineOperand &Operand : Instruction->operands()) {
      const auto *MaskOperand = Operand.getIf<LiveOutRegisterMaskOperand>();
      if (MaskOperand == nullptr)
        continue;
      if (!MaskOperand->Mask) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "liveness verification encountered a null live-out register mask");
      }
      auto Added = addRegisterMaskUses(Physical, *MaskOperand->Mask, Target);
      if (!Added)
        return std::unexpected(Added.error());
    }

    for (const MachineOperand &Operand : Instruction->operands()) {
      if (Operand.isRegister() && Operand.isDef()) {
        auto Removed = removeOperandFromLiveSet(Operand, Virtual, Physical, Function, Target);
        if (!Removed)
          return std::unexpected(Removed.error());
        continue;
      }
      const auto *MaskOperand = Operand.getIf<RegisterMaskOperand>();
      if (MaskOperand == nullptr)
        continue;
      if (!MaskOperand->Mask) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "liveness verification encountered a null register mask");
      }
      LiveRegUnits Clobbers;
      auto Added = addRegisterMaskDefs(Clobbers, *MaskOperand->Mask, Target);
      if (!Added)
        return std::unexpected(Added.error());
      Physical.subtract(Clobbers);
    }

    if (IsPhi)
      continue;
    for (const MachineOperand &Operand : Instruction->operands()) {
      if (!Operand.isRegister() || !Operand.isUse())
        continue;
      auto Added = addOperandToLiveSet(Operand, Virtual, Physical, Function, Target);
      if (!Added)
        return std::unexpected(Added.error());
    }
  }
  return Physical;
}

[[nodiscard]] Expected<LivenessRebuildPlan>
buildLivenessRebuildPlan(const MachineFunction &Function, const TargetDescription &Target) {
  auto Variables = computeLiveVariables(Function, Target);
  if (!Variables)
    return std::unexpected(Variables.error());

  LivenessRebuildPlan Plan;
  for (const MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
       Block = Block->next()) {
    const MachineBlockLiveness *BlockVariables = Variables->block(*Block);
    if (BlockVariables == nullptr)
      return makeUnexpected(ErrorCode::InternalError, "liveness result omitted a machine block");
    auto NewLiveIns = canonicalPhysicalLiveIns(BlockVariables->PhysicalLiveIn, Target);
    if (!NewLiveIns)
      return std::unexpected(NewLiveIns.error());
    Plan.Blocks.push_back(
        {Block->stableID(),
         std::vector<MachineBasicBlockLiveIn>(Block->liveIns().begin(), Block->liveIns().end()),
         std::move(*NewLiveIns)});

    RegisterLaneSet Virtual = BlockVariables->VirtualLiveOut;
    LiveRegUnits Physical = BlockVariables->PhysicalLiveOut;
    for (const MachineInstr *Instruction = Block->lastInstruction(); Instruction != nullptr;
         Instruction = Instruction->previous()) {
      const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction->opcode());
      if (Descriptor == nullptr) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "liveness rebuild encountered an unknown instruction descriptor");
      }
      const bool IsPhi = Descriptor->Flags.has(InstructionFlag::Phi);
      for (const MachineOperand &Operand : Instruction->operands()) {
        const auto *MaskOperand = Operand.getIf<LiveOutRegisterMaskOperand>();
        if (MaskOperand == nullptr)
          continue;
        if (!MaskOperand->Mask)
          return makeUnexpected(ErrorCode::VerificationError,
                                "liveness rebuild encountered a null live-out register mask");
        auto Added = addRegisterMaskUses(Physical, *MaskOperand->Mask, Target);
        if (!Added)
          return std::unexpected(Added.error());
      }

      for (std::size_t Index = 0; Index < Instruction->operandCount(); ++Index) {
        const MachineOperand *Operand = Instruction->operandAt(Index);
        if (Operand == nullptr || !Operand->isRegister() || !Operand->isDef())
          continue;
        auto IsLive = liveSetIntersects(*Operand, Virtual, Physical, Function, Target, false);
        if (!IsLive)
          return std::unexpected(IsLive.error());
        RegisterOperandFlags Flags = Operand->registerFlags();
        Flags.clear(RegisterOperandFlag::Dead);
        if (!*IsLive)
          Flags.set(RegisterOperandFlag::Dead);
        Plan.Operands.push_back(
            {Instruction->stableID(), Index, Operand->registerFlags(), Flags, false, !*IsLive});
      }

      for (const MachineOperand &Operand : Instruction->operands()) {
        if (Operand.isRegister() && Operand.isDef()) {
          auto Removed = removeOperandFromLiveSet(Operand, Virtual, Physical, Function, Target);
          if (!Removed)
            return std::unexpected(Removed.error());
          continue;
        }
        const auto *MaskOperand = Operand.getIf<RegisterMaskOperand>();
        if (MaskOperand == nullptr)
          continue;
        if (!MaskOperand->Mask)
          return makeUnexpected(ErrorCode::VerificationError,
                                "liveness rebuild encountered a null register mask");
        LiveRegUnits Clobbers;
        auto Added = addRegisterMaskDefs(Clobbers, *MaskOperand->Mask, Target);
        if (!Added)
          return std::unexpected(Added.error());
        Physical.subtract(Clobbers);
      }

      for (std::size_t Index = 0; Index < Instruction->operandCount(); ++Index) {
        const MachineOperand *Operand = Instruction->operandAt(Index);
        if (Operand == nullptr || !Operand->isRegister() || !Operand->isUse())
          continue;
        if (IsPhi) {
          RegisterOperandFlags Flags = Operand->registerFlags();
          Flags.clear(RegisterOperandFlag::Kill);
          const auto Existing = std::ranges::find_if(
              Plan.Operands, [Instruction, Index](const OperandFlagPlan &Entry) {
                return Entry.Instruction == Instruction->stableID() && Entry.OperandIndex == Index;
              });
          if (Existing == Plan.Operands.end()) {
            Plan.Operands.push_back(
                {Instruction->stableID(), Index, Operand->registerFlags(), Flags, false, false});
          } else {
            Existing->Rebuilt.clear(RegisterOperandFlag::Kill);
            Existing->KillIsValid = false;
          }
          continue;
        }
        const bool IsSemanticUse = !Operand->isDebugUse() &&
                                   !Operand->registerFlags().has(RegisterOperandFlag::Undef) &&
                                   !Operand->registerValue()->isNoRegister();
        bool IsKill = false;
        if (IsSemanticUse) {
          auto IsLive = liveSetIntersects(*Operand, Virtual, Physical, Function, Target, true);
          if (!IsLive)
            return std::unexpected(IsLive.error());
          IsKill = !*IsLive;
        }
        RegisterOperandFlags Flags = Operand->registerFlags();
        Flags.clear(RegisterOperandFlag::Kill);
        if (IsKill)
          Flags.set(RegisterOperandFlag::Kill);

        const auto Existing =
            std::ranges::find_if(Plan.Operands, [Instruction, Index](const OperandFlagPlan &Entry) {
              return Entry.Instruction == Instruction->stableID() && Entry.OperandIndex == Index;
            });
        if (Existing == Plan.Operands.end()) {
          Plan.Operands.push_back(
              {Instruction->stableID(), Index, Operand->registerFlags(), Flags, IsKill, false});
        } else {
          Existing->Rebuilt.clear(RegisterOperandFlag::Kill);
          if (IsKill)
            Existing->Rebuilt.set(RegisterOperandFlag::Kill);
          Existing->KillIsValid = IsKill;
        }
        if (IsSemanticUse) {
          auto Added = addOperandToLiveSet(*Operand, Virtual, Physical, Function, Target);
          if (!Added)
            return std::unexpected(Added.error());
        }
      }
    }

    if (Block == Function.firstBlock()) {
      for (const PhysicalRegisterLiveIn &LiveIn : Function.registerInfo().liveIns()) {
        const PhysicalRegisterDesc *Description =
            Target.registers().physicalRegister(LiveIn.PhysReg);
        if (Description == nullptr) {
          return makeUnexpected(ErrorCode::VerificationError,
                                "liveness rebuild encountered an unknown function live-in");
        }
        addPhysicalRegister(Physical, *Description);
        if (LiveIn.InitialVirtualRegister) {
          auto Lanes = virtualRegisterLanes(*LiveIn.InitialVirtualRegister, Function, Target);
          if (!Lanes)
            return std::unexpected(Lanes.error());
          Virtual.remove(*LiveIn.InitialVirtualRegister, *Lanes);
        }
      }
    }

    if (Virtual != BlockVariables->VirtualLiveIn || Physical != BlockVariables->PhysicalLiveIn) {
      return makeUnexpected(ErrorCode::InternalError,
                            "reverse liveness reconstruction disagrees with block dataflow");
    }
  }
  return Plan;
}

struct ResolvedBlockLivenessPlan final {
  MachineBasicBlock *Block = nullptr;
  const BlockLivenessPlan *Plan = nullptr;
};

struct ResolvedOperandFlagPlan final {
  MachineOperand *Operand = nullptr;
  const OperandFlagPlan *Plan = nullptr;
};

struct ResolvedLivenessRebuildPlan final {
  std::vector<ResolvedBlockLivenessPlan> Blocks;
  std::vector<ResolvedOperandFlagPlan> Operands;
};

[[nodiscard]] Expected<ResolvedLivenessRebuildPlan>
resolveLivenessPlan(MachineFunction &Function, const LivenessRebuildPlan &Plan) {
  ResolvedLivenessRebuildPlan Result;
  Result.Blocks.reserve(Plan.Blocks.size());
  for (const BlockLivenessPlan &BlockPlan : Plan.Blocks) {
    const auto Position =
        std::ranges::find_if(Function.allBlocks(), [&BlockPlan](const MachineBasicBlock *Block) {
          return Block != nullptr && Block->stableID() == BlockPlan.Block;
        });
    if (Position == Function.allBlocks().end() || !(*Position)->isLinked()) {
      return makeUnexpected(ErrorCode::InternalError,
                            "liveness rebuild plan references an unavailable machine block");
    }
    Result.Blocks.push_back({*Position, &BlockPlan});
  }

  Result.Operands.reserve(Plan.Operands.size());
  for (const OperandFlagPlan &OperandPlan : Plan.Operands) {
    const auto Position = std::ranges::find_if(
        Function.allInstructions(), [&OperandPlan](const MachineInstr *Instruction) {
          return Instruction != nullptr && Instruction->stableID() == OperandPlan.Instruction;
        });
    if (Position == Function.allInstructions().end() || !(*Position)->isLinked()) {
      return makeUnexpected(ErrorCode::InternalError,
                            "liveness rebuild plan references an unavailable instruction");
    }
    MachineOperand *Operand = (*Position)->operandAt(OperandPlan.OperandIndex);
    if (Operand == nullptr) {
      return makeUnexpected(ErrorCode::InternalError,
                            "liveness rebuild plan references an unavailable operand");
    }
    Result.Operands.push_back({Operand, &OperandPlan});
  }
  return Result;
}

[[nodiscard]] Expected<void> restoreLivenessPlan(const ResolvedLivenessRebuildPlan &Plan) {
  for (const ResolvedBlockLivenessPlan &Resolved : Plan.Blocks) {
    Resolved.Block->clearLiveIns();
    for (const MachineBasicBlockLiveIn &LiveIn : Resolved.Plan->OriginalLiveIns) {
      auto Restored = Resolved.Block->addLiveIn(LiveIn.PhysReg, LiveIn.Lanes);
      if (!Restored)
        return std::unexpected(Restored.error());
    }
  }
  for (const ResolvedOperandFlagPlan &Resolved : Plan.Operands) {
    auto Restored = Resolved.Operand->setRegisterFlags(Resolved.Plan->Original);
    if (!Restored)
      return std::unexpected(Restored.error());
  }
  return {};
}

[[nodiscard]] Expected<bool> applyLivenessPlan(MachineFunction &Function,
                                               const LivenessRebuildPlan &Plan) {
  auto Resolved = resolveLivenessPlan(Function, Plan);
  if (!Resolved)
    return std::unexpected(Resolved.error());

  bool Changed = false;
  for (const ResolvedBlockLivenessPlan &BlockPlan : Resolved->Blocks) {
    Changed |= BlockPlan.Plan->OriginalLiveIns != BlockPlan.Plan->RebuiltLiveIns;
    BlockPlan.Block->clearLiveIns();
    for (const MachineBasicBlockLiveIn &LiveIn : BlockPlan.Plan->RebuiltLiveIns) {
      auto Added = BlockPlan.Block->addLiveIn(LiveIn.PhysReg, LiveIn.Lanes);
      if (!Added) {
        auto Restored = restoreLivenessPlan(*Resolved);
        if (!Restored)
          return makeUnexpected(ErrorCode::InternalError,
                                "liveness rebuild failed and rollback also failed");
        return std::unexpected(Added.error());
      }
    }
  }
  for (const ResolvedOperandFlagPlan &OperandPlan : Resolved->Operands) {
    Changed |= OperandPlan.Plan->Original != OperandPlan.Plan->Rebuilt;
    auto Updated = OperandPlan.Operand->setRegisterFlags(OperandPlan.Plan->Rebuilt);
    if (!Updated) {
      auto Restored = restoreLivenessPlan(*Resolved);
      if (!Restored)
        return makeUnexpected(ErrorCode::InternalError,
                              "liveness rebuild failed and rollback also failed");
      return std::unexpected(Updated.error());
    }
  }
  return Changed;
}
} // namespace

LaneMask RegisterLaneSet::lanes(Register Reg) const noexcept {
  return lanesFor(Values, Reg);
}

bool RegisterLaneSet::contains(Register Reg, LaneMask Lanes) const noexcept {
  return containsLanes(Values, Reg, Lanes);
}

bool RegisterLaneSet::intersects(Register Reg, LaneMask Lanes) const noexcept {
  return intersectsLanes(Values, Reg, Lanes);
}

void RegisterLaneSet::add(Register Reg, LaneMask Lanes) {
  if (!Reg.isNoRegister())
    addLanes(Values, Reg, Lanes);
}

void RegisterLaneSet::remove(Register Reg, LaneMask Lanes) {
  removeLanes(Values, Reg, Lanes);
}

void RegisterLaneSet::unite(const RegisterLaneSet &Other) {
  uniteLanes(Values, Other.Values);
}

void RegisterLaneSet::subtract(const RegisterLaneSet &Other) {
  subtractLaneSets(Values, Other.Values);
}
const MachineBlockLiveness *
LiveVariablesAnalysis::Result::block(const MachineBasicBlock &Block) const noexcept {
  const auto Position = Blocks.find(&Block);
  return Position == Blocks.end() ? nullptr : &Position->second;
}

MachineBlockLiveness &
LiveVariablesAnalysis::Result::getOrCreateBlock(const MachineBasicBlock &Block) {
  return Blocks[&Block];
}

bool LiveVariablesAnalysis::Result::isLiveIn(const MachineBasicBlock &Block, Register Reg,
                                             LaneMask Lanes) const noexcept {
  const MachineBlockLiveness *Value = block(Block);
  return Value != nullptr && Value->VirtualLiveIn.intersects(Reg, Lanes);
}

bool LiveVariablesAnalysis::Result::isLiveOut(const MachineBasicBlock &Block, Register Reg,
                                              LaneMask Lanes) const noexcept {
  const MachineBlockLiveness *Value = block(Block);
  return Value != nullptr && Value->VirtualLiveOut.intersects(Reg, Lanes);
}

bool LiveVariablesAnalysis::Result::isUnitLiveIn(const MachineBasicBlock &Block,
                                                 RegisterUnitID Unit,
                                                 LaneMask Lanes) const noexcept {
  const MachineBlockLiveness *Value = block(Block);
  return Value != nullptr && Value->PhysicalLiveIn.intersects(Unit, Lanes);
}

bool LiveVariablesAnalysis::Result::isUnitLiveOut(const MachineBasicBlock &Block,
                                                  RegisterUnitID Unit,
                                                  LaneMask Lanes) const noexcept {
  const MachineBlockLiveness *Value = block(Block);
  return Value != nullptr && Value->PhysicalLiveOut.intersects(Unit, Lanes);
}

bool LiveVariablesAnalysis::Result::invalidate(const MachineFunction &,
                                               const PreservedAnalyses &Preserved) const noexcept {
  return !Preserved.preserves(MachineAnalysisManager::id<LiveVariablesAnalysis>());
}

Expected<LiveVariablesAnalysis::Result> LiveVariablesAnalysis::run(MachineFunction &Function,
                                                                   const TargetDescription &Target,
                                                                   MachineAnalysisManager &) {
  return computeLiveVariables(Function, Target);
}
const MachinePassContract &RebuildMachineLivenessPass::contract() const noexcept {
  static const MachinePassContract Contract = [] {
    MachinePassContract Value{
        .Name = "RebuildMachineLiveness",
        .Requires = {MachineFunctionProperty::Selected, MachineFunctionProperty::NoPHIs,
                     MachineFunctionProperty::TiedOpsRewritten},
        .Sets = {MachineFunctionProperty::TracksLiveness},
        .Clears = {},
        .RequiredAnalyses = {},
        .VerificationStage = std::nullopt,
    };
    Value.DeclaredChanges = {MachinePassChange::Operands};
    return Value;
  }();
  return Contract;
}

Expected<MachinePassResult> RebuildMachineLivenessPass::run(MachineFunction &Function,
                                                            const TargetDescription &Target,
                                                            MachineAnalysisManager &) {
  const MachineFunctionProperties Missing = contract().Requires.missingFrom(Function.properties());
  if (!Missing.empty()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RebuildMachineLiveness requires properties: " + Missing.toString());
  }
  auto Applied = rebuildMachineLiveness(Function, Target);
  if (!Applied)
    return std::unexpected(Applied.error());

  MachinePassResult Result;
  Result.Changed = *Applied;
  if (Result.Changed)
    Result.Changes.add(MachinePassChange::Operands);
  Result.Preserved = PreservedAnalyses::all();
  return Result;
}

Expected<bool> rebuildMachineLiveness(MachineFunction &Function, const TargetDescription &Target) {
  auto Plan = buildLivenessRebuildPlan(Function, Target);
  if (!Plan)
    return std::unexpected(Plan.error());
  return applyLivenessPlan(Function, *Plan);
}

Expected<void> verifyTrackedMachineLiveness(const MachineFunction &Function,
                                            const TargetDescription &Target) {
  auto Plan = buildLivenessRebuildPlan(Function, Target);
  if (!Plan)
    return std::unexpected(Plan.error());
  for (const MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
       Block = Block->next()) {
    auto Actual = liveInUnits(Block->liveIns(), Target);
    if (!Actual)
      return std::unexpected(Actual.error());
    auto Expected = requiredLocalPhysicalLiveIns(*Block, Function, Target);
    if (!Expected)
      return std::unexpected(Expected.error());
    if (!isSubset(*Expected, *Actual)) {
      const auto Missing = std::ranges::find_if(Expected->entries(), [&Actual](const auto &Entry) {
        return !Actual->contains(Entry.first, Entry.second);
      });
      if (Missing != Expected->entries().end()) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "TracksLiveness block " + std::to_string(Block->stableID().value()) +
                                  " is missing required physical register unit " +
                                  std::to_string(Missing->first.value()));
      }
      return makeUnexpected(ErrorCode::VerificationError,
                            "TracksLiveness block is missing required physical live-ins");
    }
  }
  for (const OperandFlagPlan &OperandPlan : Plan->Operands) {
    if (OperandPlan.Original.has(RegisterOperandFlag::Kill) && !OperandPlan.KillIsValid) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "TracksLiveness operand has a stale kill flag");
    }
    if (OperandPlan.Original.has(RegisterOperandFlag::Dead) && !OperandPlan.DeadIsValid) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "TracksLiveness operand has a stale dead flag");
    }
  }
  return {};
}

Expected<void> registerStage11MachineAnalyses(MachineAnalysisManager &Analyses) {
  const std::array IDs{
      MachineAnalysisManager::id<SlotIndexesAnalysis>(),
      MachineAnalysisManager::id<LiveVariablesAnalysis>(),
      MachineAnalysisManager::id<LiveIntervalsAnalysis>(),
  };
  const std::size_t Registered = static_cast<std::size_t>(
      std::ranges::count_if(IDs, [&Analyses](AnalysisID ID) { return Analyses.isRegistered(ID); }));
  if (Registered == IDs.size())
    return {};
  if (Registered != 0) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "standard liveness analyses are only partially registered");
  }

  auto Candidate = Analyses.forkEmpty();
  if (!Candidate)
    return std::unexpected(Candidate.error());
  MachineAnalysisManager &Staging = **Candidate;
  auto Slots = Staging.registerAnalysis<SlotIndexesAnalysis>({
      .Name = "SlotIndexes",
      .Dependencies = {},
      .InvalidatedBy = {MachinePassChange::Instructions, MachinePassChange::BlockLayout},
      .ThreadSafety = PassThreadSafety::ThreadSafe,
  });
  if (!Slots)
    return std::unexpected(Slots.error());
  auto Variables = Staging.registerAnalysis<LiveVariablesAnalysis>({
      .Name = "LiveVariables",
      .Dependencies = {},
      .InvalidatedBy = {MachinePassChange::Instructions, MachinePassChange::Operands,
                        MachinePassChange::Registers, MachinePassChange::CFG,
                        MachinePassChange::TargetState},
      .ThreadSafety = PassThreadSafety::ThreadSafe,
  });
  if (!Variables)
    return std::unexpected(Variables.error());
  auto Intervals = Staging.registerAnalysis<LiveIntervalsAnalysis>({
      .Name = "LiveIntervals",
      .Dependencies = {MachineAnalysisManager::id<SlotIndexesAnalysis>(),
                       MachineAnalysisManager::id<LiveVariablesAnalysis>()},
      .InvalidatedBy = {MachinePassChange::Instructions, MachinePassChange::Operands,
                        MachinePassChange::Registers, MachinePassChange::CFG,
                        MachinePassChange::BlockLayout, MachinePassChange::TargetState},
      .ThreadSafety = PassThreadSafety::ThreadSafe,
  });
  if (!Intervals)
    return std::unexpected(Intervals.error());
  Analyses.Registry = std::move(Staging.Registry);
  return {};
}

} // namespace rcc
