// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineLiveness.h"

#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/RegisterMask.h"
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
#include <unordered_set>
#include <utility>
#include <vector>

namespace rcc {
namespace {

constexpr std::uint64_t SlotSpacing = 16;
constexpr std::uint64_t SlotAlignmentMask = 3;

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

struct PhysicalUnitAccess final {
  RegisterUnitID Unit;
  LaneMask Lanes;
};

[[nodiscard]] LaneMask normalizeUnitLanes(LaneMask UnitViewLanes, LaneMask RegisterLanes) noexcept {
  std::uint64_t View = UnitViewLanes.rawValue();
  const std::uint64_t Selected = View & RegisterLanes.rawValue();
  std::uint64_t Normalized = 0;
  std::uint32_t UnitLane = 0;
  while (View != 0) {
    const std::uint64_t RegisterLane = std::uint64_t{1} << std::countr_zero(View);
    if ((Selected & RegisterLane) != 0)
      Normalized |= std::uint64_t{1} << UnitLane;
    View &= View - 1U;
    ++UnitLane;
  }
  return LaneMask::fromRaw(Normalized);
}

[[nodiscard]] LaneMask denormalizeUnitLanes(LaneMask UnitLanes, LaneMask UnitViewLanes) noexcept {
  std::uint64_t View = UnitViewLanes.rawValue();
  const std::uint64_t Selected = UnitLanes.rawValue();
  std::uint64_t RegisterLanes = 0;
  std::uint32_t UnitLane = 0;
  while (View != 0) {
    const std::uint64_t RegisterLane = std::uint64_t{1} << std::countr_zero(View);
    if ((Selected & (std::uint64_t{1} << UnitLane)) != 0)
      RegisterLanes |= RegisterLane;
    View &= View - 1U;
    ++UnitLane;
  }
  return LaneMask::fromRaw(RegisterLanes);
}

[[nodiscard]] LaneMask physicalRegisterLanes(const PhysicalRegisterDesc &Description) noexcept {
  LaneMask Result;
  for (const RegisterUnitLane &Unit : Description.Units)
    Result = Result | Unit.Lanes;
  return Result;
}

[[nodiscard]] RegisterUnitLaneSet physicalRegisterCoverage(const PhysicalRegisterDesc &Description,
                                                           LaneMask Requested) {
  RegisterUnitLaneSet Result;
  for (const RegisterUnitLane &Unit : Description.Units)
    Result.add(Unit.Unit, normalizeUnitLanes(Unit.Lanes, Requested));
  return Result;
}

[[nodiscard]] bool isSubset(const RegisterUnitLaneSet &Subset,
                            const RegisterUnitLaneSet &Superset) noexcept {
  return std::ranges::all_of(Subset.entries(), [&Superset](const auto &Entry) {
    return Superset.contains(Entry.first, Entry.second);
  });
}

[[nodiscard]] std::size_t laneCount(const RegisterUnitLaneSet &Set) noexcept {
  std::size_t Result = 0;
  for (const auto &[Unit, Lanes] : Set.entries()) {
    (void)Unit;
    Result += static_cast<std::size_t>(std::popcount(Lanes.rawValue()));
  }
  return Result;
}

[[nodiscard]] Expected<LaneMask> virtualRegisterLanes(Register Reg, const MachineFunction &Function,
                                                      const TargetDescription &Target) {
  if (!Reg.isVirtual()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "virtual lane query requires a virtual register");
  }
  auto Info = Function.registerInfo().virtualRegisterInfo(Reg);
  if (!Info)
    return std::unexpected(Info.error());
  const std::optional<RegisterClassID> ClassID = Info->get().RegClass;
  if (!ClassID)
    return LaneMask::all();
  const RegisterClassDesc *Class = Target.registers().registerClass(*ClassID);
  if (Class == nullptr || Class->Lanes.empty()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "liveness encountered an unknown or empty register class");
  }
  return Class->Lanes;
}

[[nodiscard]] Expected<LaneMask> virtualOperandLanes(const MachineOperand &Operand,
                                                     const MachineFunction &Function,
                                                     const TargetDescription &Target) {
  const Register *Reg = Operand.getIf<Register>();
  if (Reg == nullptr || !Reg->isVirtual()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "virtual lane query requires a virtual-register operand");
  }
  if (Operand.subReg().value() != 0) {
    const SubRegIndexDesc *SubReg = Target.registers().subRegIndex(Operand.subReg());
    if (SubReg == nullptr || SubReg->Lanes.empty()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "liveness encountered an unknown or empty subregister lane mask");
    }
    return SubReg->Lanes;
  }

  return virtualRegisterLanes(*Reg, Function, Target);
}

[[nodiscard]] Expected<std::vector<PhysicalUnitAccess>>
physicalOperandUnits(const MachineOperand &Operand, const TargetDescription &Target) {
  const Register *Reg = Operand.getIf<Register>();
  const std::optional<PhysicalRegister> Original =
      Reg == nullptr ? std::nullopt : Reg->physicalRegister();
  if (!Original) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "physical unit query requires a physical-register operand");
  }

  PhysicalRegister Effective = *Original;
  if (Operand.subReg().value() != 0) {
    const std::optional<PhysicalRegister> SubRegister =
        Target.registers().subRegister(*Original, Operand.subReg());
    if (!SubRegister) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "physical register has no requested subregister projection");
    }
    Effective = *SubRegister;
  }
  const PhysicalRegisterDesc *Description = Target.registers().physicalRegister(Effective);
  if (Description == nullptr || Description->Units.empty()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "liveness encountered an unknown or unitless physical register");
  }

  std::vector<PhysicalUnitAccess> Result;
  Result.reserve(Description->Units.size());
  for (const RegisterUnitLane &Unit : Description->Units) {
    if (Unit.Lanes.empty()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "physical register unit has an empty lane mask");
    }
    Result.push_back({Unit.Unit, normalizeUnitLanes(Unit.Lanes, LaneMask::all())});
  }
  return Result;
}

void addPhysicalRegister(RegisterUnitLaneSet &Set, const PhysicalRegisterDesc &Description,
                         LaneMask Requested = LaneMask::all()) {
  Set.unite(physicalRegisterCoverage(Description, Requested));
}

[[nodiscard]] Expected<void> addRegisterMaskDefs(RegisterUnitLaneSet &Set, const RegisterMask &Mask,
                                                 const TargetDescription &Target) {
  for (const PhysicalRegisterDesc &Description : Target.registers().physicalRegisters()) {
    if (Mask.clobbers(Description.Reg))
      addPhysicalRegister(Set, Description);
  }
  return {};
}

[[nodiscard]] Expected<void> addRegisterMaskUses(RegisterUnitLaneSet &Set, const RegisterMask &Mask,
                                                 const TargetDescription &Target) {
  for (const PhysicalRegisterDesc &Description : Target.registers().physicalRegisters()) {
    if (Mask.preserves(Description.Reg))
      addPhysicalRegister(Set, Description);
  }
  return {};
}

[[nodiscard]] Expected<void> addOperandUse(RegisterLaneSet &VirtualUses,
                                           RegisterUnitLaneSet &PhysicalUses,
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
  auto Units = physicalOperandUnits(Operand, Target);
  if (!Units)
    return std::unexpected(Units.error());
  for (const PhysicalUnitAccess &Unit : *Units)
    PhysicalUses.add(Unit.Unit, Unit.Lanes);
  return {};
}

[[nodiscard]] Expected<void> addOperandDef(RegisterLaneSet &VirtualDefs,
                                           RegisterUnitLaneSet &PhysicalDefs,
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
  RegisterUnitLaneSet Physical;
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
      RegisterUnitLaneSet InstructionPhysicalUses;
      RegisterUnitLaneSet InstructionPhysicalDefs;
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

      RegisterUnitLaneSet NewPhysicalUses = InstructionPhysicalUses;
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
      RegisterUnitLaneSet PhysicalOut;
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
      RegisterUnitLaneSet PhysicalIn = PhysicalOut;
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

[[nodiscard]] bool maskIntersects(LaneMask Access, std::optional<LaneMask> Filter) noexcept {
  return !Filter || Access.intersects(*Filter);
}

struct InstructionRangeEvent final {
  bool HasUse = false;
  bool HasNormalDef = false;
  bool HasEarlyDef = false;
  bool HasRegMaskClobber = false;
};

[[nodiscard]] Expected<InstructionRangeEvent>
virtualRangeEvent(const MachineInstr &Instruction, Register Reg, std::optional<LaneMask> Filter,
                  const MachineFunction &Function, const TargetDescription &Target) {
  InstructionRangeEvent Event;
  for (const MachineOperand &Operand : Instruction.operands()) {
    const Register *OperandReg = Operand.getIf<Register>();
    if (OperandReg == nullptr || *OperandReg != Reg)
      continue;
    auto Lanes = virtualOperandLanes(Operand, Function, Target);
    if (!Lanes)
      return std::unexpected(Lanes.error());
    if (!maskIntersects(*Lanes, Filter))
      continue;
    Event.HasUse |= Operand.isUse() && !Operand.isDebugUse() &&
                    !Operand.registerFlags().has(RegisterOperandFlag::Undef);
    if (Operand.isDef()) {
      if (Operand.registerFlags().has(RegisterOperandFlag::EarlyClobber))
        Event.HasEarlyDef = true;
      else
        Event.HasNormalDef = true;
    }
  }
  return Event;
}

[[nodiscard]] Expected<InstructionRangeEvent> unitRangeEvent(const MachineInstr &Instruction,
                                                             RegisterUnitID Unit,
                                                             std::optional<LaneMask> Filter,
                                                             const TargetDescription &Target) {
  InstructionRangeEvent Event;
  for (const MachineOperand &Operand : Instruction.operands()) {
    if (Operand.isRegister()) {
      const Register *Reg = Operand.getIf<Register>();
      if (Reg == nullptr || !Reg->isPhysical())
        continue;
      auto Units = physicalOperandUnits(Operand, Target);
      if (!Units)
        return std::unexpected(Units.error());
      const auto Position = std::ranges::find_if(
          *Units, [Unit](const PhysicalUnitAccess &Access) { return Access.Unit == Unit; });
      if (Position == Units->end() || !maskIntersects(Position->Lanes, Filter))
        continue;
      Event.HasUse |= Operand.isUse() && !Operand.isDebugUse() &&
                      !Operand.registerFlags().has(RegisterOperandFlag::Undef);
      if (Operand.isDef()) {
        if (Operand.registerFlags().has(RegisterOperandFlag::EarlyClobber))
          Event.HasEarlyDef = true;
        else
          Event.HasNormalDef = true;
      }
      continue;
    }
    if (const auto *MaskOperand = Operand.getIf<RegisterMaskOperand>();
        MaskOperand != nullptr && MaskOperand->Mask) {
      for (const PhysicalRegisterDesc &Description : Target.registers().physicalRegisters()) {
        if (!MaskOperand->Mask->clobbers(Description.Reg))
          continue;
        if (std::ranges::any_of(Description.Units, [Unit, Filter](const RegisterUnitLane &Access) {
              return Access.Unit == Unit && maskIntersects(Access.Lanes, Filter);
            })) {
          Event.HasRegMaskClobber = true;
          break;
        }
      }
      continue;
    }
    if (const auto *MaskOperand = Operand.getIf<LiveOutRegisterMaskOperand>();
        MaskOperand != nullptr && MaskOperand->Mask) {
      for (const PhysicalRegisterDesc &Description : Target.registers().physicalRegisters()) {
        if (!MaskOperand->Mask->preserves(Description.Reg))
          continue;
        if (std::ranges::any_of(Description.Units, [Unit, Filter](const RegisterUnitLane &Access) {
              return Access.Unit == Unit && maskIntersects(Access.Lanes, Filter);
            })) {
          Event.HasUse = true;
          break;
        }
      }
    }
  }
  return Event;
}

template <typename EventFn, typename LiveOutFn>
[[nodiscard]] Expected<LiveRange>
buildLiveRange(MachineFunction &Function, const SlotIndexesAnalysis::Result &Indexes,
               const EventFn &EventForInstruction, const LiveOutFn &IsLiveOut) {
  LiveRange Range;
  for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr; Block = Block->next()) {
    std::optional<SlotIndex> End;
    bool SawLaterDefinition = false;
    if (IsLiveOut(*Block))
      End = Indexes.blockEnd(*Block);

    for (MachineInstr *Instruction = Block->lastInstruction(); Instruction != nullptr;
         Instruction = Instruction->previous()) {
      auto Event = EventForInstruction(*Instruction);
      if (!Event)
        return std::unexpected(Event.error());
      if (!Event->HasUse && !Event->HasNormalDef && !Event->HasEarlyDef &&
          !Event->HasRegMaskClobber) {
        continue;
      }

      const SlotIndex InstructionSlot = Indexes.instructionIndex(*Instruction);
      if (!InstructionSlot.isValid()) {
        return makeUnexpected(ErrorCode::InternalError,
                              "live-range construction has no instruction SlotIndex");
      }
      auto AddDefinition = [&](SlotIndex DefSlot, bool IsRegMask) -> Expected<void> {
        const bool HasLiveValue = End && DefSlot < *End;
        if (IsRegMask && !HasLiveValue) {
          End.reset();
          SawLaterDefinition = true;
          return {};
        }
        const LiveValueState State = IsRegMask            ? LiveValueState::Redefined
                                     : HasLiveValue       ? LiveValueState::Normal
                                     : SawLaterDefinition ? LiveValueState::Redefined
                                                          : LiveValueState::Unused;
        auto Value = Range.createValue(DefSlot, State, Instruction->stableID());
        if (!Value)
          return std::unexpected(Value.error());
        const SlotIndex SegmentEnd = HasLiveValue ? *End : InstructionSlot.deadSlot();
        if (DefSlot < SegmentEnd) {
          auto Added = Range.addSegment({DefSlot, SegmentEnd, *Value});
          if (!Added)
            return std::unexpected(Added.error());
        }
        End.reset();
        SawLaterDefinition = true;
        return {};
      };

      if (Event->HasNormalDef) {
        auto Added = AddDefinition(InstructionSlot.registerSlot(), false);
        if (!Added)
          return std::unexpected(Added.error());
      }
      if (Event->HasUse) {
        const SlotIndex UseEnd =
            Event->HasNormalDef ? InstructionSlot.registerSlot() : InstructionSlot.deadSlot();
        if (!End || *End < UseEnd)
          End = UseEnd;
      }
      if (Event->HasEarlyDef) {
        auto Added = AddDefinition(InstructionSlot.earlyClobberSlot(), false);
        if (!Added)
          return std::unexpected(Added.error());
      } else if (Event->HasRegMaskClobber) {
        auto Added = AddDefinition(InstructionSlot.earlyClobberSlot(), true);
        if (!Added)
          return std::unexpected(Added.error());
      }
    }

    if (End) {
      const SlotIndex Start = Indexes.blockStart(*Block);
      if (!Start.isValid() || !(*End > Start)) {
        return makeUnexpected(ErrorCode::InternalError,
                              "live-in range has invalid block boundary indexes");
      }
      auto Value = Range.createValue(Start, LiveValueState::Phi);
      if (!Value)
        return std::unexpected(Value.error());
      auto Added = Range.addSegment({Start, *End, *Value});
      if (!Added)
        return std::unexpected(Added.error());
    }
  }
  auto Verification = Range.verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  return Range;
}

[[nodiscard]] Expected<LaneMask> virtualRegisterFullLanes(Register Reg,
                                                          const MachineFunction &Function,
                                                          const TargetDescription &Target) {
  return virtualRegisterLanes(Reg, Function, Target);
}

[[nodiscard]] Expected<std::vector<LaneMask>>
partitionVirtualRegisterLanes(Register Reg, MachineFunction &Function,
                              const TargetDescription &Target) {
  auto Full = virtualRegisterFullLanes(Reg, Function, Target);
  if (!Full)
    return std::unexpected(Full.error());
  std::vector<LaneMask> AccessMasks;
  for (const MachineOperand &Operand : Function.registerInfo().regNoDebugOperands(Reg)) {
    auto Lanes = virtualOperandLanes(Operand, Function, Target);
    if (!Lanes)
      return std::unexpected(Lanes.error());
    if (*Lanes != *Full && std::ranges::find(AccessMasks, *Lanes) == AccessMasks.end())
      AccessMasks.push_back(*Lanes);
  }
  if (AccessMasks.empty())
    return {};

  std::vector<LaneMask> Groups{*Full};
  for (const LaneMask Access : AccessMasks) {
    std::vector<LaneMask> Refined;
    for (const LaneMask Group : Groups) {
      const LaneMask Intersection = Group & Access;
      const LaneMask Difference = subtractLanes(Group, Access);
      if (!Intersection.empty())
        Refined.push_back(Intersection);
      if (!Difference.empty())
        Refined.push_back(Difference);
    }
    Groups = std::move(Refined);
  }
  std::ranges::sort(
      Groups, [](LaneMask Left, LaneMask Right) { return Left.rawValue() < Right.rawValue(); });
  return Groups;
}

[[nodiscard]] Expected<std::vector<MachineBasicBlockLiveIn>>
canonicalPhysicalLiveIns(const RegisterUnitLaneSet &Units, const TargetDescription &Target) {
  struct Candidate final {
    const PhysicalRegisterDesc *Description = nullptr;
    LaneMask RegisterLanes;
    RegisterUnitLaneSet Coverage;
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
      const RegisterUnitLaneSet Coverage =
          physicalRegisterCoverage(Description, LaneMask::fromRaw(Lane));
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

  RegisterUnitLaneSet Covered;
  std::vector<MachineBasicBlockLiveIn> Result;
  while (Covered != Units) {
    Candidate *Best = nullptr;
    std::size_t BestNewLanes = 0;
    std::size_t BestCoverage = 0;
    for (Candidate &Value : Candidates) {
      if (Value.Selected)
        continue;
      RegisterUnitLaneSet NewCoverage = Value.Coverage;
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

[[nodiscard]] Expected<RegisterUnitLaneSet>
liveInUnits(std::span<const MachineBasicBlockLiveIn> LiveIns, const TargetDescription &Target) {
  RegisterUnitLaneSet Result;
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

[[nodiscard]] Expected<bool> liveSetIntersects(const MachineOperand &Operand,
                                               const RegisterLaneSet &Virtual,
                                               const RegisterUnitLaneSet &Physical,
                                               const MachineFunction &Function,
                                               const TargetDescription &Target) {
  const Register *Reg = Operand.getIf<Register>();
  if (Reg == nullptr || Reg->isNoRegister())
    return false;
  if (Reg->isVirtual()) {
    auto Lanes = virtualOperandLanes(Operand, Function, Target);
    if (!Lanes)
      return std::unexpected(Lanes.error());
    return Virtual.intersects(*Reg, *Lanes);
  }
  auto Units = physicalOperandUnits(Operand, Target);
  if (!Units)
    return std::unexpected(Units.error());
  return std::ranges::any_of(*Units, [&Physical](const PhysicalUnitAccess &Unit) {
    return Physical.intersects(Unit.Unit, Unit.Lanes);
  });
}

[[nodiscard]] Expected<void> removeOperandFromLiveSet(const MachineOperand &Operand,
                                                      RegisterLaneSet &Virtual,
                                                      RegisterUnitLaneSet &Physical,
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
                                                 RegisterLaneSet &Virtual,
                                                 RegisterUnitLaneSet &Physical,
                                                 const MachineFunction &Function,
                                                 const TargetDescription &Target) {
  return addOperandUse(Virtual, Physical, Operand, Function, Target);
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
    RegisterUnitLaneSet Physical = BlockVariables->PhysicalLiveOut;
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
        auto IsLive = liveSetIntersects(*Operand, Virtual, Physical, Function, Target);
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
        RegisterUnitLaneSet Clobbers;
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
          auto IsLive = liveSetIntersects(*Operand, Virtual, Physical, Function, Target);
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

SlotIndex
SlotIndexesAnalysis::Result::instructionIndex(const MachineInstr &Instruction) const noexcept {
  const auto Position = InstructionSlots.find(Instruction.stableID());
  return Position == InstructionSlots.end() ? SlotIndex{} : Position->second;
}

SlotIndex SlotIndexesAnalysis::Result::blockStart(const MachineBasicBlock &Block) const noexcept {
  const auto Position = BlockStarts.find(&Block);
  return Position == BlockStarts.end() ? SlotIndex{} : Position->second;
}

SlotIndex SlotIndexesAnalysis::Result::blockEnd(const MachineBasicBlock &Block) const noexcept {
  const auto Position = BlockEnds.find(&Block);
  return Position == BlockEnds.end() ? SlotIndex{} : Position->second;
}

const MachineInstr *
SlotIndexesAnalysis::Result::instructionFromIndex(SlotIndex Index) const noexcept {
  if (!Index.isValid())
    return nullptr;
  const auto Position = SlotToInstruction.find(Index.baseIndex().value());
  return Position == SlotToInstruction.end() ? nullptr : Position->second;
}

const MachineBasicBlock *
SlotIndexesAnalysis::Result::blockFromIndex(SlotIndex Index) const noexcept {
  if (!Index.isValid())
    return nullptr;
  for (const MachineBasicBlock *Block : BlocksByStart) {
    const SlotIndex Start = blockStart(*Block);
    const SlotIndex End = blockEnd(*Block);
    if (!(Index < Start) && Index < End)
      return Block;
  }
  return nullptr;
}

Expected<void> SlotIndexesAnalysis::Result::insertInstruction(const MachineInstr &Instruction) {
  const MachineBasicBlock *Block = Instruction.parent();
  if (Block == nullptr || !BlockStarts.contains(Block) || !BlockEnds.contains(Block)) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "SlotIndex insertion requires a linked indexed instruction");
  }
  if (InstructionSlots.contains(Instruction.stableID())) {
    return makeUnexpected(ErrorCode::InvalidArgument, "instruction already has a SlotIndex");
  }

  const MachineInstr *Previous = Instruction.previous();
  const MachineInstr *Next = Instruction.next();
  const SlotIndex LowerIndex =
      Previous == nullptr ? blockStart(*Block) : instructionIndex(*Previous);
  const SlotIndex UpperIndex = Next == nullptr ? blockEnd(*Block) : instructionIndex(*Next);
  if (!LowerIndex.isValid() || !UpperIndex.isValid()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "adjacent instructions are missing SlotIndexes");
  }
  const std::uint64_t Lower = LowerIndex.baseIndex().value();
  const std::uint64_t Upper = UpperIndex.baseIndex().value();
  if (Upper <= Lower || Upper - Lower <= SlotAlignmentMask + 1U) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "local SlotIndex space is exhausted; renumbering is required");
  }
  std::uint64_t Candidate = ((Lower + Upper) / 2U) & ~SlotAlignmentMask;
  if (Candidate <= Lower)
    Candidate = Lower + SlotAlignmentMask + 1U;
  if (Candidate >= Upper) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "local SlotIndex space is exhausted; renumbering is required");
  }
  const SlotIndex Index = SlotIndex::fromValue(Candidate).registerSlot();
  InstructionSlots.emplace(Instruction.stableID(), Index);
  SlotToInstruction.emplace(Index.baseIndex().value(), &Instruction);
  return {};
}

Expected<void> SlotIndexesAnalysis::Result::eraseInstruction(const MachineInstr &Instruction) {
  const auto Position = InstructionSlots.find(Instruction.stableID());
  if (Position == InstructionSlots.end())
    return makeUnexpected(ErrorCode::InvalidArgument, "instruction has no SlotIndex");
  SlotToInstruction.erase(Position->second.baseIndex().value());
  InstructionSlots.erase(Position);
  return {};
}

Expected<void> SlotIndexesAnalysis::Result::renumber(MachineFunction &Function) {
  Result NewResult;
  std::uint64_t Position = SlotSpacing;
  auto Advance = [&Position]() -> Expected<void> {
    if (Position > std::numeric_limits<std::uint64_t>::max() - SlotSpacing) {
      return makeUnexpected(ErrorCode::Unsupported, "machine SlotIndex space is exhausted");
    }
    Position += SlotSpacing;
    return {};
  };

  for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr; Block = Block->next()) {
    const SlotIndex Start = SlotIndex::fromValue(Position).withKind(SlotIndexKind::Block);
    NewResult.BlockStarts.emplace(Block, Start);
    NewResult.BlocksByStart.push_back(Block);
    auto Advanced = Advance();
    if (!Advanced)
      return std::unexpected(Advanced.error());

    for (MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      const SlotIndex Index = SlotIndex::fromValue(Position).registerSlot();
      if (!NewResult.InstructionSlots.emplace(Instruction->stableID(), Index).second ||
          !NewResult.SlotToInstruction.emplace(Index.baseIndex().value(), Instruction).second) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "duplicate instruction identity during SlotIndex construction");
      }
      Advanced = Advance();
      if (!Advanced)
        return std::unexpected(Advanced.error());
    }
    NewResult.BlockEnds.emplace(Block,
                                SlotIndex::fromValue(Position).withKind(SlotIndexKind::Block));
    Advanced = Advance();
    if (!Advanced)
      return std::unexpected(Advanced.error());
  }
  *this = std::move(NewResult);
  return {};
}

bool SlotIndexesAnalysis::Result::invalidate(const MachineFunction &,
                                             const PreservedAnalyses &Preserved) const noexcept {
  return !Preserved.preserves(MachineAnalysisManager::id<SlotIndexesAnalysis>());
}

Expected<SlotIndexesAnalysis::Result> SlotIndexesAnalysis::run(MachineFunction &Function,
                                                               const TargetDescription &,
                                                               MachineAnalysisManager &) {
  Result ResultValue;
  auto Numbered = ResultValue.renumber(Function);
  if (!Numbered)
    return std::unexpected(Numbered.error());
  return ResultValue;
}

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

LaneMask RegisterUnitLaneSet::lanes(RegisterUnitID Unit) const noexcept {
  return lanesFor(Values, Unit);
}

bool RegisterUnitLaneSet::contains(RegisterUnitID Unit, LaneMask Lanes) const noexcept {
  return containsLanes(Values, Unit, Lanes);
}

bool RegisterUnitLaneSet::intersects(RegisterUnitID Unit, LaneMask Lanes) const noexcept {
  return intersectsLanes(Values, Unit, Lanes);
}

void RegisterUnitLaneSet::add(RegisterUnitID Unit, LaneMask Lanes) {
  if (Unit.value() != 0 && !Lanes.empty())
    addLanes(Values, Unit, Lanes);
}

void RegisterUnitLaneSet::remove(RegisterUnitID Unit, LaneMask Lanes) {
  removeLanes(Values, Unit, Lanes);
}

void RegisterUnitLaneSet::unite(const RegisterUnitLaneSet &Other) {
  uniteLanes(Values, Other.Values);
}

void RegisterUnitLaneSet::subtract(const RegisterUnitLaneSet &Other) {
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

const LiveSegment *LiveRange::segmentAt(SlotIndex Position) const noexcept {
  const auto Candidate = std::ranges::upper_bound(
      Segments, Position, {}, [](const LiveSegment &Segment) { return Segment.Start; });
  if (Candidate == Segments.begin())
    return nullptr;
  const LiveSegment &Previous = *std::prev(Candidate);
  return Previous.contains(Position) ? &Previous : nullptr;
}

const LiveValueNumber *LiveRange::value(LiveValueNumberID ID) const noexcept {
  if (!ID.isValid() || ID.value() > Values.size())
    return nullptr;
  const LiveValueNumber &Result = Values[ID.value() - 1U];
  return Result.ID == ID ? &Result : nullptr;
}

const LiveValueNumber *LiveRange::valueAt(SlotIndex Position) const noexcept {
  const LiveSegment *Segment = segmentAt(Position);
  return Segment == nullptr ? nullptr : value(Segment->Value);
}

bool LiveRange::liveAt(SlotIndex Position) const noexcept {
  return segmentAt(Position) != nullptr;
}

bool LiveRange::overlaps(const LiveRange &Other) const noexcept {
  std::size_t Left = 0;
  std::size_t Right = 0;
  while (Left < Segments.size() && Right < Other.Segments.size()) {
    if (Segments[Left].overlaps(Other.Segments[Right]))
      return true;
    if (Segments[Left].End <= Other.Segments[Right].Start)
      ++Left;
    else
      ++Right;
  }
  return false;
}

Expected<LiveValueNumberID> LiveRange::createValue(SlotIndex Definition, LiveValueState State,
                                                   std::optional<MachineInstrStableID> Origin) {
  if (!Definition.isValid())
    return makeUnexpected(ErrorCode::InvalidArgument, "live value requires a valid definition");
  if (Values.size() >= std::numeric_limits<std::uint32_t>::max())
    return makeUnexpected(ErrorCode::Unsupported, "live value-number space is exhausted");
  const LiveValueNumberID ID =
      LiveValueNumberID::fromValue(static_cast<std::uint32_t>(Values.size() + 1U));
  Values.push_back({ID, Definition, State, Origin});
  return ID;
}

Expected<void> LiveRange::addSegment(LiveSegment Segment) {
  if (!Segment.Start.isValid() || !Segment.End.isValid() || Segment.empty() ||
      value(Segment.Value) == nullptr) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "live segment requires ordered indexes and a known value number");
  }
  std::vector<LiveSegment> Candidate = Segments;
  Candidate.push_back(Segment);
  std::ranges::sort(Candidate, [](const LiveSegment &Left, const LiveSegment &Right) {
    if (Left.Start != Right.Start)
      return Left.Start < Right.Start;
    return Left.End < Right.End;
  });
  for (std::size_t Index = 1; Index < Candidate.size(); ++Index) {
    if (Candidate[Index].Start < Candidate[Index - 1U].End &&
        Candidate[Index].Value != Candidate[Index - 1U].Value) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "live segments with different values overlap");
    }
  }
  Segments = std::move(Candidate);
  mergeAdjacent();
  return {};
}

Expected<void> LiveRange::extend(LiveValueNumberID Value, SlotIndex Start, SlotIndex End) {
  return addSegment({Start, End, Value});
}

Expected<void> LiveRange::remove(SlotIndex Start, SlotIndex End) {
  if (!Start.isValid() || !End.isValid() || !(Start < End)) {
    return makeUnexpected(ErrorCode::InvalidArgument, "removed live range must be nonempty");
  }
  std::vector<LiveSegment> Result;
  for (const LiveSegment &Segment : Segments) {
    if (!(Segment.Start < End && Start < Segment.End)) {
      Result.push_back(Segment);
      continue;
    }
    if (Segment.Start < Start)
      Result.push_back({Segment.Start, Start, Segment.Value});
    if (End < Segment.End)
      Result.push_back({End, Segment.End, Segment.Value});
  }
  Segments = std::move(Result);
  mergeAdjacent();
  return {};
}

Expected<void> LiveRange::shrinkToUses(std::span<const SlotIndex> Uses) {
  std::vector<LiveSegment> Result;
  for (const LiveSegment &Segment : Segments) {
    const LiveValueNumber *Value = value(Segment.Value);
    if (Value == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "live segment references an unknown value number");
    std::optional<SlotIndex> LastUse;
    for (const SlotIndex Use : Uses) {
      if (Segment.contains(Use) && (!LastUse || *LastUse < Use))
        LastUse = Use;
    }
    if (!LastUse) {
      if (Value->State == LiveValueState::Unused)
        Result.push_back(Segment);
      continue;
    }
    SlotIndex NewEnd = LastUse->deadSlot();
    if (Segment.End < NewEnd)
      NewEnd = Segment.End;
    if (Segment.Start < NewEnd)
      Result.push_back({Segment.Start, NewEnd, Segment.Value});
  }
  Segments = std::move(Result);
  mergeAdjacent();
  return verify();
}

Expected<void> LiveRange::verify() const {
  std::unordered_set<LiveValueNumberID> IDs;
  for (std::size_t Index = 0; Index < Values.size(); ++Index) {
    const LiveValueNumber &Value = Values[Index];
    if (!Value.ID.isValid() || Value.ID.value() != Index + 1U || !IDs.insert(Value.ID).second ||
        !Value.Definition.isValid()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "live range contains an invalid value number");
    }
  }
  for (std::size_t Index = 0; Index < Segments.size(); ++Index) {
    const LiveSegment &Segment = Segments[Index];
    if (!Segment.Start.isValid() || !Segment.End.isValid() || Segment.empty() ||
        value(Segment.Value) == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError, "live range contains an invalid segment");
    }
    if (Index != 0 && Segments[Index - 1U].End > Segment.Start) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "live range segments are not ordered and disjoint");
    }
  }
  return {};
}

void LiveRange::mergeAdjacent() {
  if (Segments.empty())
    return;
  std::vector<LiveSegment> Result;
  Result.reserve(Segments.size());
  Result.push_back(Segments.front());
  for (std::size_t Index = 1; Index < Segments.size(); ++Index) {
    LiveSegment &Previous = Result.back();
    const LiveSegment &Current = Segments[Index];
    if (Current.Value == Previous.Value && Current.Start <= Previous.End) {
      if (Previous.End < Current.End)
        Previous.End = Current.End;
      continue;
    }
    Result.push_back(Current);
  }
  Segments = std::move(Result);
}

const LiveInterval *LiveIntervalsAnalysis::Result::interval(Register Reg) const noexcept {
  const auto Position = Intervals.find(Reg);
  return Position == Intervals.end() ? nullptr : &Position->second;
}

LiveInterval *LiveIntervalsAnalysis::Result::interval(Register Reg) noexcept {
  const auto Position = Intervals.find(Reg);
  return Position == Intervals.end() ? nullptr : &Position->second;
}

const LiveRange *LiveIntervalsAnalysis::Result::regUnitRange(RegisterUnitID Unit) const noexcept {
  const auto Position = RegisterUnitRanges.find(Unit);
  return Position == RegisterUnitRanges.end() ? nullptr : &Position->second;
}

bool LiveIntervalsAnalysis::Result::interferes(Register Left, Register Right) const noexcept {
  const LiveInterval *LeftInterval = interval(Left);
  const LiveInterval *RightInterval = interval(Right);
  return LeftInterval != nullptr && RightInterval != nullptr &&
         LeftInterval->overlaps(*RightInterval);
}

bool LiveIntervalsAnalysis::Result::regMaskClobbers(const LiveInterval &Interval,
                                                    PhysicalRegister Candidate) const noexcept {
  for (const RegMaskPosition &Position : RegMasks) {
    if (Position.Mask && Position.Mask->clobbers(Candidate) &&
        Interval.MainRange.liveAt(Position.Position)) {
      return true;
    }
  }
  return false;
}

Expected<void> LiveIntervalsAnalysis::Result::shrinkToUses(Register Reg,
                                                           std::span<const SlotIndex> Uses) {
  LiveInterval *Value = interval(Reg);
  if (Value == nullptr)
    return makeUnexpected(ErrorCode::InvalidArgument, "virtual register has no live interval");
  auto Main = Value->MainRange.shrinkToUses(Uses);
  if (!Main)
    return std::unexpected(Main.error());
  for (LiveSubRange &SubRange : Value->SubRanges) {
    auto Shrunk = SubRange.Range.shrinkToUses(Uses);
    if (!Shrunk)
      return std::unexpected(Shrunk.error());
  }
  return {};
}

bool LiveIntervalsAnalysis::Result::invalidate(const MachineFunction &,
                                               const PreservedAnalyses &Preserved) const noexcept {
  return !Preserved.preserves(MachineAnalysisManager::id<LiveIntervalsAnalysis>());
}

Expected<LiveIntervalsAnalysis::Result>
LiveIntervalsAnalysis::run(MachineFunction &Function, const TargetDescription &Target,
                           MachineAnalysisManager &Analyses) {
  auto Indexes = Analyses.getResult<SlotIndexesAnalysis>(Function, Target);
  if (!Indexes)
    return std::unexpected(Indexes.error());
  auto Variables = Analyses.getResult<LiveVariablesAnalysis>(Function, Target);
  if (!Variables)
    return std::unexpected(Variables.error());

  Result ResultValue;
  ResultValue.Indexes = **Indexes;
  for (std::size_t Index = 0; Index < Function.registerInfo().virtualRegisters().size(); ++Index) {
    auto RegValue = Register::makeVirtual(Index);
    if (!RegValue || !Function.registerInfo().isKnownVirtualRegister(*RegValue))
      continue;
    const Register Reg = *RegValue;
    auto MainRange = buildLiveRange(
        Function, ResultValue.Indexes,
        [&Function, &Target, Reg](const MachineInstr &Instruction) {
          return virtualRangeEvent(Instruction, Reg, std::nullopt, Function, Target);
        },
        [&Variables, Reg](const MachineBasicBlock &Block) {
          const MachineBlockLiveness *Value = (*Variables)->block(Block);
          return Value != nullptr && Value->VirtualLiveOut.intersects(Reg);
        });
    if (!MainRange)
      return std::unexpected(MainRange.error());
    if (MainRange->empty())
      continue;

    LiveInterval Interval;
    Interval.Reg = Reg;
    Interval.MainRange = std::move(*MainRange);
    auto Info = Function.registerInfo().virtualRegisterInfo(Reg);
    if (!Info)
      return std::unexpected(Info.error());
    // Preserve the target-provided hint order.  MachineRegisterInfo already de-duplicates
    // hints, and their order is the deterministic preference order consumed by allocators.
    Interval.AllocationHints = Info->get().AllocationHints;

    std::uint64_t Weight = 1;
    for (const MachineOperand &Operand : Function.registerInfo().useOperands(Reg, false)) {
      if (Operand.parent() == nullptr || Operand.registerFlags().has(RegisterOperandFlag::Undef)) {
        continue;
      }
      Weight = Weight > std::numeric_limits<std::uint64_t>::max() - 10U
                   ? std::numeric_limits<std::uint64_t>::max()
                   : Weight + 10U;
    }
    Interval.SpillWeight = Weight;

    auto LaneGroups = partitionVirtualRegisterLanes(Reg, Function, Target);
    if (!LaneGroups)
      return std::unexpected(LaneGroups.error());
    for (const LaneMask Lanes : *LaneGroups) {
      auto SubRange = buildLiveRange(
          Function, ResultValue.Indexes,
          [&Function, &Target, Reg, Lanes](const MachineInstr &Instruction) {
            return virtualRangeEvent(Instruction, Reg, Lanes, Function, Target);
          },
          [&Variables, Reg, Lanes](const MachineBasicBlock &Block) {
            const MachineBlockLiveness *Value = (*Variables)->block(Block);
            return Value != nullptr && Value->VirtualLiveOut.intersects(Reg, Lanes);
          });
      if (!SubRange)
        return std::unexpected(SubRange.error());
      if (!SubRange->empty())
        Interval.SubRanges.push_back({Lanes, std::move(*SubRange)});
    }
    ResultValue.Intervals.emplace(Reg, std::move(Interval));
  }

  for (const RegisterUnitDesc &Unit : Target.registers().registerUnits()) {
    auto Range = buildLiveRange(
        Function, ResultValue.Indexes,
        [&Target, Unit](const MachineInstr &Instruction) {
          return unitRangeEvent(Instruction, Unit.ID, std::nullopt, Target);
        },
        [&Variables, Unit](const MachineBasicBlock &Block) {
          const MachineBlockLiveness *Value = (*Variables)->block(Block);
          return Value != nullptr && Value->PhysicalLiveOut.intersects(Unit.ID);
        });
    if (!Range)
      return std::unexpected(Range.error());
    if (!Range->empty())
      ResultValue.RegisterUnitRanges.emplace(Unit.ID, std::move(*Range));
  }

  for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr; Block = Block->next()) {
    for (MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      for (const MachineOperand &Operand : Instruction->operands()) {
        const auto *MaskOperand = Operand.getIf<RegisterMaskOperand>();
        if (MaskOperand == nullptr)
          continue;
        if (!MaskOperand->Mask)
          return makeUnexpected(ErrorCode::VerificationError,
                                "live intervals encountered a null register mask");
        ResultValue.RegMasks.push_back(
            {ResultValue.Indexes.instructionIndex(*Instruction).earlyClobberSlot(),
             Instruction->stableID(), MaskOperand->Mask});
      }
    }
  }
  return ResultValue;
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
  for (const BlockLivenessPlan &BlockPlan : Plan->Blocks) {
    auto Actual = liveInUnits(BlockPlan.OriginalLiveIns, Target);
    if (!Actual)
      return std::unexpected(Actual.error());
    auto Expected = liveInUnits(BlockPlan.RebuiltLiveIns, Target);
    if (!Expected)
      return std::unexpected(Expected.error());
    if (*Actual != *Expected) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "TracksLiveness block has stale physical live-ins");
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
