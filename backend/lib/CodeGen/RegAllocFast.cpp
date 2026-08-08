// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/RegAllocFast.h"

#include "RegAllocInternal.h"
#include "rcc/CodeGen/LiveRangeEdit.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineLiveness.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/Spiller.h"

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
#include <ranges>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rcc {
namespace {

class FastAllocatorEngine final {
public:
  FastAllocatorEngine(MachineFunction &Function, const TargetDescription &Target,
                      const TargetRegisterAllocationInfo &TargetInfo,
                      const LiveVariablesAnalysis::Result &Liveness)
      : Function(Function), Target(Target), TargetInfo(TargetInfo), Liveness(Liveness),
        Mapping(Function), Edit(Function, Target, Mapping) {}

  [[nodiscard]] Expected<regalloc_detail::AllocationRunResult> run() {
    const std::uint64_t InitialFingerprint = regalloc_detail::allocationFingerprint(Function);
    const std::size_t InitialFrameObjects = Function.frameInfo().liveObjectCount();
    auto Initialized = regalloc_detail::initializeMapping(Function, Mapping);
    if (!Initialized)
      return std::unexpected(Initialized.error());

    bool IsEntry = true;
    for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
         Block = Block->next()) {
      auto Allocated = allocateBlock(*Block, IsEntry);
      if (!Allocated)
        return std::unexpected(Allocated.error());
      IsEntry = false;
    }

    Function.registerInfo().clearLiveInVirtualRegisters();
    for (std::size_t Index = 0; Index < Function.registerInfo().virtualRegisters().size();
         ++Index) {
      auto Reg = regalloc_detail::virtualRegisterAt(Index);
      if (!Reg)
        return std::unexpected(Reg.error());
      if (!Function.registerInfo().isKnownVirtualRegister(*Reg))
        continue;
      auto Erased = Function.registerInfo().eraseVirtualRegister(*Reg);
      if (!Erased)
        return std::unexpected(Erased.error());
    }
    auto Rebuilt = rebuildMachineLiveness(Function, Target);
    if (!Rebuilt)
      return std::unexpected(Rebuilt.error());

    regalloc_detail::AllocationRunResult Result;
    const std::uint64_t FinalFingerprint = regalloc_detail::allocationFingerprint(Function);
    Result.Changed = InitialFingerprint != FinalFingerprint;
    Result.ChangedInstructions = Result.Changed;
    Result.ChangedFrame = InitialFrameObjects != Function.frameInfo().liveObjectCount();
    return Result;
  }

private:
  struct Location final {
    PhysicalRegister PhysicalReg;
    bool Dirty = false;
  };

  struct InstructionRegister final {
    Register VirtualReg;
    std::vector<std::size_t> Operands;
    bool HasUse = false;
    bool HasDef = false;
    std::size_t FirstOperand = 0;
  };

  struct BlockLiveInState final {
    PhysicalRegister Reg;
    std::size_t RemainingUses = 0;
  };

  [[nodiscard]] bool liveOut(const MachineBasicBlock &Block, Register Reg) const noexcept {
    return Liveness.isLiveOut(Block, Reg);
  }

  [[nodiscard]] bool neededLater(const MachineBasicBlock &Block, Register Reg) const noexcept {
    const auto Position = RemainingUses.find(Reg);
    return (Position != RemainingUses.end() && Position->second != 0) || liveOut(Block, Reg);
  }

  [[nodiscard]] std::optional<Register> ownerOf(PhysicalRegister Reg) const noexcept {
    for (const auto &[VirtualReg, Value] : Locations) {
      if (Target.registers().regsOverlap(Value.PhysicalReg, Reg))
        return VirtualReg;
    }
    return std::nullopt;
  }

  [[nodiscard]] bool physicalLiveOut(const MachineBasicBlock &Block,
                                     PhysicalRegister Reg) const noexcept {
    const PhysicalRegisterDesc *Description = Target.registers().physicalRegister(Reg);
    if (Description == nullptr)
      return true;
    return std::ranges::any_of(Description->Units, [&](const RegisterUnitLane &Unit) {
      return Liveness.isUnitLiveOut(Block, Unit.Unit, Unit.Lanes);
    });
  }

  [[nodiscard]] bool blockedByBlockLiveIn(PhysicalRegister Reg) const noexcept {
    return std::ranges::any_of(BlockLiveIns, [&](const BlockLiveInState &LiveIn) {
      return Target.registers().regsOverlap(LiveIn.Reg, Reg);
    });
  }

  void initializeBlockLiveIns(const MachineBasicBlock &Block) {
    BlockLiveIns.clear();
    for (const MachineBasicBlockLiveIn &LiveIn : Block.liveIns()) {
      if (LiveIn.Lanes.empty() ||
          std::ranges::any_of(BlockLiveIns, [&](const BlockLiveInState &Existing) {
            return Existing.Reg == LiveIn.PhysReg;
          }))
        continue;
      BlockLiveIns.push_back({LiveIn.PhysReg, 0});
    }

    for (const MachineInstr *Instruction = Block.firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      for (const MachineOperand &Operand : Instruction->operands()) {
        const Register *Reg = Operand.getIf<Register>();
        if (Reg == nullptr || !Reg->isPhysical() || !Operand.isUse())
          continue;
        const auto PhysicalValue = Reg->physicalRegister();
        if (!PhysicalValue)
          continue;
        const PhysicalRegister Physical = PhysicalValue.value();
        for (BlockLiveInState &LiveIn : BlockLiveIns) {
          if (Target.registers().regsOverlap(LiveIn.Reg, Physical))
            ++LiveIn.RemainingUses;
        }
      }
    }
  }

  void updateBlockLiveIns(const MachineBasicBlock &Block, const MachineInstr &Instruction) {
    std::vector<PhysicalRegister> Defined;
    std::vector<const RegisterMask *> Masks;
    for (const MachineOperand &Operand : Instruction.operands()) {
      if (const Register *Reg = Operand.getIf<Register>(); Reg != nullptr && Reg->isPhysical()) {
        const auto PhysicalValue = Reg->physicalRegister();
        if (Operand.isDef() && PhysicalValue)
          Defined.push_back(PhysicalValue.value());
      }
      if (const auto *Mask = Operand.getIf<RegisterMaskOperand>(); Mask != nullptr && Mask->Mask)
        Masks.push_back(Mask->Mask.get());
    }

    for (auto Position = BlockLiveIns.begin(); Position != BlockLiveIns.end();) {
      BlockLiveInState &LiveIn = *Position;
      for (const MachineOperand &Operand : Instruction.operands()) {
        const Register *Reg = Operand.getIf<Register>();
        if (Reg == nullptr || !Reg->isPhysical() || !Operand.isUse())
          continue;
        const auto PhysicalValue = Reg->physicalRegister();
        if (!PhysicalValue || !Target.registers().regsOverlap(LiveIn.Reg, PhysicalValue.value()))
          continue;
        if (LiveIn.RemainingUses != 0)
          --LiveIn.RemainingUses;
      }

      const bool DefinedHere = std::ranges::any_of(Defined, [&](PhysicalRegister Reg) {
        return Target.registers().regsOverlap(LiveIn.Reg, Reg);
      });
      const bool MaskClobbered = std::ranges::any_of(
          Masks, [&](const RegisterMask *Mask) { return Mask->clobbers(LiveIn.Reg); });
      const bool KeepForOutput = physicalLiveOut(Block, LiveIn.Reg);
      if (!KeepForOutput && (DefinedHere || MaskClobbered || LiveIn.RemainingUses == 0))
        Position = BlockLiveIns.erase(Position);
      else
        ++Position;
    }
  }

  [[nodiscard]] bool overlapsAny(PhysicalRegister Candidate,
                                 std::span<const PhysicalRegister> Values) const noexcept {
    return std::ranges::any_of(Values, [&](PhysicalRegister Value) {
      return Target.registers().regsOverlap(Candidate, Value);
    });
  }

  [[nodiscard]] Expected<FrameIndex> spillSlot(Register Reg) {
    auto ClassID = regalloc_detail::registerClassFor(Function, Reg);
    if (!ClassID)
      return std::unexpected(ClassID.error());
    auto Validated = TargetInfo.validateSpillReload(*ClassID, Target);
    if (!Validated) {
      return makeUnexpected(
          ErrorCode::InternalError,
          "target declares an allocatable class without FastAllocator spill hooks");
    }
    return Edit.getOrCreateSpillSlot(Reg);
  }

  [[nodiscard]] Expected<void> emitSpillBefore(Register Reg, PhysicalRegister PhysicalReg,
                                               MachineInstr *Before, MachineBasicBlock &Block) {
    auto ClassID = regalloc_detail::registerClassFor(Function, Reg);
    if (!ClassID)
      return std::unexpected(ClassID.error());
    auto Slot = spillSlot(Reg);
    if (!Slot)
      return std::unexpected(Slot.error());
    MachineIRBuilder Builder(Function, Target);
    Expected<void> Positioned =
        Before != nullptr ? Builder.setInsertPointBefore(*Before) : Builder.setInsertPoint(Block);
    if (!Positioned)
      return std::unexpected(Positioned.error());
    auto Spill = TargetInfo.emitSpill(Builder, Register::fromPhysical(PhysicalReg), *ClassID, *Slot,
                                      LaneMask::all());
    if (!Spill)
      return std::unexpected(Spill.error());
    VirtRegMapEntry *Entry = Mapping.entry(Reg);
    if (Entry != nullptr)
      ++Entry->Spill.SpillCount;
    return {};
  }

  [[nodiscard]] Expected<void> evict(Register Reg, MachineInstr &Before, MachineBasicBlock &Block,
                                     bool Preserve) {
    const auto Position = Locations.find(Reg);
    if (Position == Locations.end())
      return {};
    if (Position->second.Dirty && Preserve) {
      auto Stored = emitSpillBefore(Reg, Position->second.PhysicalReg, &Before, Block);
      if (!Stored)
        return std::unexpected(Stored.error());
    }
    Locations.erase(Position);
    return {};
  }

  [[nodiscard]] Expected<PhysicalRegister>
  ensureLocation(Register Reg, bool NeedsValue, MachineInstr &Instruction, MachineBasicBlock &Block,
                 std::span<const PhysicalRegister> Fixed, std::span<const PhysicalRegister> Locked,
                 const std::unordered_set<Register> &CurrentRegisters) {
    const auto Existing = Locations.find(Reg);
    if (Existing != Locations.end() && !overlapsAny(Existing->second.PhysicalReg, Fixed) &&
        !overlapsAny(Existing->second.PhysicalReg, Locked))
      return Existing->second.PhysicalReg;
    if (Existing != Locations.end()) {
      auto Evicted = evict(Reg, Instruction, Block, NeedsValue || neededLater(Block, Reg));
      if (!Evicted)
        return std::unexpected(Evicted.error());
    }

    auto Candidates = regalloc_detail::allocationOrder(Function, Target, Reg);
    if (!Candidates)
      return std::unexpected(Candidates.error());
    for (const PhysicalRegister Candidate : *Candidates) {
      if (overlapsAny(Candidate, Fixed) || overlapsAny(Candidate, Locked))
        continue;
      if (blockedByBlockLiveIn(Candidate))
        continue;
      if (const auto Owner = ownerOf(Candidate)) {
        if (CurrentRegisters.contains(*Owner))
          continue;
        auto Evicted = evict(*Owner, Instruction, Block, neededLater(Block, *Owner));
        if (!Evicted)
          return std::unexpected(Evicted.error());
      }
      if (ownerOf(Candidate))
        continue;

      if (NeedsValue) {
        auto ClassID = regalloc_detail::registerClassFor(Function, Reg);
        if (!ClassID)
          return std::unexpected(ClassID.error());
        auto Slot = spillSlot(Reg);
        if (!Slot)
          return std::unexpected(Slot.error());
        MachineIRBuilder Builder(Function, Target);
        auto Positioned = Builder.setInsertPointBefore(Instruction);
        if (!Positioned)
          return std::unexpected(Positioned.error());
        auto Reload = TargetInfo.emitReload(Builder, Register::fromPhysical(Candidate), *ClassID,
                                            *Slot, LaneMask::all());
        if (!Reload)
          return std::unexpected(Reload.error());
        VirtRegMapEntry *Entry = Mapping.entry(Reg);
        if (Entry != nullptr)
          ++Entry->Spill.ReloadCount;
      }
      Locations.emplace(Reg, Location{Candidate, false});
      return Candidate;
    }
    return makeUnexpected(ErrorCode::RegisterAllocationFailed,
                          "FastAllocator cannot satisfy simultaneous register constraints");
  }

  [[nodiscard]] Expected<void> installEntryLiveIns(MachineBasicBlock &Block) {
    if (&Block != Function.firstBlock())
      return {};
    for (const PhysicalRegisterLiveIn &LiveIn : Function.registerInfo().liveIns()) {
      if (!LiveIn.InitialVirtualRegister)
        continue;
      const Register Reg = *LiveIn.InitialVirtualRegister;
      if (!Function.registerInfo().isKnownVirtualRegister(Reg))
        continue;
      auto Candidates = regalloc_detail::allocationOrder(Function, Target, Reg);
      if (!Candidates)
        return std::unexpected(Candidates.error());
      if (std::ranges::find(*Candidates, LiveIn.PhysReg) == Candidates->end()) {
        return makeUnexpected(ErrorCode::RegisterAllocationFailed,
                              "function live-in is outside its allocation order");
      }
      if (ownerOf(LiveIn.PhysReg)) {
        return makeUnexpected(ErrorCode::RegisterAllocationFailed,
                              "overlapping function live-ins require the same physical register");
      }
      Locations.emplace(Reg, Location{LiveIn.PhysReg, true});
    }
    return {};
  }

  [[nodiscard]] Expected<void> allocateInstruction(MachineInstr &Instruction,
                                                   MachineBasicBlock &Block) {
    std::vector<PhysicalRegister> Fixed;
    std::vector<const RegisterMask *> Masks;
    std::map<std::uint32_t, InstructionRegister> Registers;
    std::vector<std::size_t> DebugOperands;
    for (std::size_t Index = 0; Index < Instruction.operandCount(); ++Index) {
      MachineOperand *Operand = Instruction.operandAt(Index);
      if (const auto *Mask = Operand->getIf<RegisterMaskOperand>(); Mask != nullptr && Mask->Mask)
        Masks.push_back(Mask->Mask.get());
      const Register *Reg = Operand->getIf<Register>();
      if (Reg == nullptr || Reg->isNoRegister())
        continue;
      if (Reg->isPhysical()) {
        const auto PhysicalValue = Reg->physicalRegister();
        if (!PhysicalValue) {
          return makeUnexpected(ErrorCode::InternalError,
                                "physical register operand lacks a physical register value");
        }
        Fixed.push_back(PhysicalValue.value());
        continue;
      }
      if (Operand->isDebugUse()) {
        DebugOperands.push_back(Index);
        continue;
      }
      auto [Position, Inserted] = Registers.try_emplace(
          Reg->opaqueValue(), InstructionRegister{*Reg, {}, false, false, Index});
      if (!Inserted)
        Position->second.FirstOperand = std::min(Position->second.FirstOperand, Index);
      Position->second.Operands.push_back(Index);
      Position->second.HasUse |= Operand->isUse();
      Position->second.HasDef |= Operand->isDef();
    }
    std::ranges::sort(Fixed, {}, &PhysicalRegister::number);
    Fixed.erase(std::unique(Fixed.begin(), Fixed.end()), Fixed.end());

    std::unordered_set<Register> CurrentRegisters;
    std::unordered_set<Register> DefinedRegisters;
    for (const auto &[Key, Value] : Registers) {
      (void)Key;
      CurrentRegisters.insert(Value.VirtualReg);
      if (Value.HasDef)
        DefinedRegisters.insert(Value.VirtualReg);
    }
    for (const auto &[Reg, Value] :
         std::vector<std::pair<Register, Location>>(Locations.begin(), Locations.end())) {
      bool Clobbered = false;
      for (const RegisterMask *Mask : Masks)
        Clobbered |= Mask->clobbers(Value.PhysicalReg);
      if (Clobbered) {
        auto Evicted = evict(Reg, Instruction, Block, neededLater(Block, Reg));
        if (!Evicted)
          return std::unexpected(Evicted.error());
      }
    }

    std::vector<InstructionRegister *> Ordered;
    Ordered.reserve(Registers.size());
    for (auto &[Key, Value] : Registers) {
      (void)Key;
      Ordered.push_back(&Value);
    }
    std::ranges::sort(Ordered,
                      [](const InstructionRegister *Left, const InstructionRegister *Right) {
                        if (Left->HasUse != Right->HasUse)
                          return Left->HasUse > Right->HasUse;
                        return Left->FirstOperand < Right->FirstOperand;
                      });

    std::vector<PhysicalRegister> Locked;
    for (InstructionRegister *Value : Ordered) {
      auto Physical = ensureLocation(Value->VirtualReg, Value->HasUse, Instruction, Block, Fixed,
                                     Locked, CurrentRegisters);
      if (!Physical)
        return std::unexpected(Physical.error());
      Locked.push_back(*Physical);
      for (const std::size_t Index : Value->Operands) {
        MachineOperand *Operand = Instruction.operandAt(Index);
        PhysicalRegister Effective = *Physical;
        if (Operand->subReg().value() != 0) {
          const auto Sub = Target.registers().subRegister(*Physical, Operand->subReg());
          if (!Sub)
            return makeUnexpected(ErrorCode::RegisterAllocationFailed,
                                  "FastAllocator assignment lacks a required subregister");
          Effective = *Sub;
        }
        auto Set = Operand->setRegister(Register::fromPhysical(Effective));
        if (!Set)
          return std::unexpected(Set.error());
        if (Operand->subReg().value() != 0) {
          auto Cleared = Operand->setSubReg(SubRegIndex{});
          if (!Cleared)
            return std::unexpected(Cleared.error());
        }
      }
      if (Value->HasDef)
        Locations[Value->VirtualReg].Dirty = true;
    }

    for (const std::size_t Index : DebugOperands) {
      MachineOperand *Operand = Instruction.operandAt(Index);
      const Register *Reg = Operand->getIf<Register>();
      const auto Position = Reg == nullptr ? Locations.end() : Locations.find(*Reg);
      Register Replacement;
      if (Position != Locations.end())
        Replacement = Register::fromPhysical(Position->second.PhysicalReg);
      auto Set = Operand->setRegister(Replacement);
      if (!Set)
        return std::unexpected(Set.error());
      if (Operand->subReg().value() != 0) {
        auto Cleared = Operand->setSubReg(SubRegIndex{});
        if (!Cleared)
          return std::unexpected(Cleared.error());
      }
      if (Replacement.isNoRegister()) {
        RegisterOperandFlags Flags = Operand->registerFlags();
        Flags.set(RegisterOperandFlag::Undef);
        auto Updated = Operand->setRegisterFlags(Flags);
        if (!Updated)
          return std::unexpected(Updated.error());
      }
    }

    for (InstructionRegister *Value : Ordered) {
      if (Value->HasUse) {
        auto Position = RemainingUses.find(Value->VirtualReg);
        if (Position != RemainingUses.end() && Position->second != 0)
          --Position->second;
      }
      if (!neededLater(Block, Value->VirtualReg))
        Locations.erase(Value->VirtualReg);
    }
    for (const RegisterMask *Mask : Masks) {
      for (auto Position = Locations.begin(); Position != Locations.end();) {
        if (Mask->clobbers(Position->second.PhysicalReg) &&
            !DefinedRegisters.contains(Position->first))
          Position = Locations.erase(Position);
        else
          ++Position;
      }
    }
    return {};
  }

  [[nodiscard]] Expected<void> flushLiveOuts(MachineBasicBlock &Block) {
    MachineInstr *Terminator = regalloc_detail::firstTerminator(Block, Target);
    for (const auto &[Reg, Value] :
         std::vector<std::pair<Register, Location>>(Locations.begin(), Locations.end())) {
      if (!Value.Dirty || !liveOut(Block, Reg))
        continue;
      if (Terminator != nullptr) {
        for (const MachineOperand &Operand : Terminator->operands()) {
          const Register *OperandReg = Operand.getIf<Register>();
          if (OperandReg != nullptr && *OperandReg == Reg && Operand.isDef()) {
            return makeUnexpected(ErrorCode::RegisterAllocationFailed,
                                  "FastAllocator cannot spill a terminator-defined live-out");
          }
        }
      }
      auto Stored = emitSpillBefore(Reg, Value.PhysicalReg, Terminator, Block);
      if (!Stored)
        return std::unexpected(Stored.error());
    }
    Locations.clear();
    return {};
  }

  [[nodiscard]] Expected<void> allocateBlock(MachineBasicBlock &Block, bool IsEntry) {
    (void)IsEntry;
    Locations.clear();
    RemainingUses.clear();
    initializeBlockLiveIns(Block);
    std::vector<MachineInstr *> Instructions;
    for (MachineInstr *Instruction = Block.firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      Instructions.push_back(Instruction);
      std::unordered_set<Register> UsesInInstruction;
      for (const MachineOperand &Operand : Instruction->operands()) {
        const Register *Reg = Operand.getIf<Register>();
        if (Reg != nullptr && Reg->isVirtual() && Operand.isUse() && !Operand.isDebugUse())
          UsesInInstruction.insert(*Reg);
      }
      for (const Register Reg : UsesInInstruction)
        ++RemainingUses[Reg];
    }
    auto EntryValues = installEntryLiveIns(Block);
    if (!EntryValues)
      return std::unexpected(EntryValues.error());
    for (MachineInstr *Instruction : Instructions) {
      auto Allocated = allocateInstruction(*Instruction, Block);
      if (!Allocated)
        return std::unexpected(Allocated.error());
      updateBlockLiveIns(Block, *Instruction);
    }
    auto Flushed = flushLiveOuts(Block);
    BlockLiveIns.clear();
    return Flushed;
  }

  MachineFunction &Function;
  const TargetDescription &Target;
  const TargetRegisterAllocationInfo &TargetInfo;
  const LiveVariablesAnalysis::Result &Liveness;
  VirtRegMap Mapping;
  LiveRangeEdit Edit;
  std::unordered_map<Register, Location> Locations;
  std::unordered_map<Register, std::size_t> RemainingUses;
  std::vector<BlockLiveInState> BlockLiveIns;
};

} // namespace

const MachinePassContract &FastAllocatorPass::contract() const noexcept {
  static const MachinePassContract Contract = [] {
    MachinePassContract Value{
        .Name = "FastAllocator",
        .Requires = {MachineFunctionProperty::Selected, MachineFunctionProperty::NoPHIs,
                     MachineFunctionProperty::TiedOpsRewritten,
                     MachineFunctionProperty::TracksLiveness},
        .Sets = {MachineFunctionProperty::NoVRegs},
        .Clears = {},
        .RequiredAnalyses = {MachineAnalysisManager::id<LiveVariablesAnalysis>()},
        .VerificationStage = PipelineStage::RegisterAllocated,
    };
    Value.ThreadSafety = PassThreadSafety::ThreadSafe;
    return Value;
  }();
  return Contract;
}

Expected<MachinePassResult> FastAllocatorPass::run(MachineFunction &Function,
                                                   const TargetDescription &Target,
                                                   MachineAnalysisManager &Analyses) {
  if (Info == nullptr)
    return makeUnexpected(ErrorCode::InvalidArgument, "FastAllocator requires target behavior");
  auto Required = regalloc_detail::requireAllocationProperties(Function, contract());
  if (!Required)
    return std::unexpected(Required.error());
  auto Trial = regalloc_detail::cloneForAllocation(Function, Target);
  if (!Trial)
    return std::unexpected(Trial.error());
  MachineAnalysisManager TrialAnalyses;
  auto Registered = registerStage11MachineAnalyses(TrialAnalyses);
  if (!Registered)
    return std::unexpected(Registered.error());
  auto TrialLiveness =
      TrialAnalyses.getResult<LiveVariablesAnalysis>(*Trial->ClonedFunction, Target);
  if (!TrialLiveness)
    return std::unexpected(TrialLiveness.error());
  FastAllocatorEngine TrialAllocator(*Trial->ClonedFunction, Target, *Info, **TrialLiveness);
  auto TrialRun = TrialAllocator.run();
  if (!TrialRun)
    return std::unexpected(TrialRun.error());
  auto TrialVerification =
      regalloc_detail::verifyAllocationTrial(*Trial->ClonedFunction, Target, contract());
  if (!TrialVerification)
    return std::unexpected(TrialVerification.error());
  auto Liveness = Analyses.getResult<LiveVariablesAnalysis>(Function, Target);
  if (!Liveness)
    return std::unexpected(Liveness.error());
  FastAllocatorEngine Allocator(Function, Target, *Info, **Liveness);
  auto Allocated = Allocator.run();
  if (!Allocated) {
    return std::unexpected(regalloc_detail::withPrefix(
        Allocated.error(), "FastAllocator diverged after a successful transaction preflight: "));
  }
  return regalloc_detail::passResult(*Allocated);
}

} // namespace rcc
