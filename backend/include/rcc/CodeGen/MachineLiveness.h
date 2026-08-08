// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINELIVENESS_H
#define RCC_CODEGEN_MACHINELIVENESS_H

#include "rcc/CodeGen/MachineIRTypes.h"
#include "rcc/CodeGen/MachinePass.h"
#include "rcc/CodeGen/Register.h"
#include "rcc/CodeGen/StableID.h"
#include "rcc/Support/Error.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rcc {

class MachineBasicBlock;
class MachineFunction;
class MachineInstr;
class RegisterMask;
class TargetDescription;

enum class SlotIndexKind : std::uint8_t {
  Block,
  EarlyClobber,
  Register,
  Dead,
};

class SlotIndex final {
public:
  constexpr SlotIndex() noexcept = default;

  [[nodiscard]] static constexpr SlotIndex fromValue(std::uint64_t Value) noexcept {
    return SlotIndex(Value);
  }

  [[nodiscard]] constexpr bool isValid() const noexcept {
    return Value != 0;
  }

  [[nodiscard]] constexpr std::uint64_t value() const noexcept {
    return Value;
  }

  [[nodiscard]] constexpr SlotIndexKind kind() const noexcept {
    return static_cast<SlotIndexKind>(Value & KindMask);
  }

  [[nodiscard]] constexpr SlotIndex baseIndex() const noexcept {
    return SlotIndex(Value & ~KindMask);
  }

  [[nodiscard]] constexpr SlotIndex withKind(SlotIndexKind Kind) const noexcept {
    return isValid() ? SlotIndex((Value & ~KindMask) | static_cast<std::uint64_t>(Kind))
                     : SlotIndex{};
  }

  [[nodiscard]] constexpr SlotIndex earlyClobberSlot() const noexcept {
    return withKind(SlotIndexKind::EarlyClobber);
  }

  [[nodiscard]] constexpr SlotIndex registerSlot() const noexcept {
    return withKind(SlotIndexKind::Register);
  }

  [[nodiscard]] constexpr SlotIndex deadSlot() const noexcept {
    return withKind(SlotIndexKind::Dead);
  }

  friend constexpr bool operator==(SlotIndex, SlotIndex) noexcept = default;
  friend constexpr auto operator<=>(SlotIndex, SlotIndex) noexcept = default;

private:
  static constexpr std::uint64_t KindMask = 3;

  explicit constexpr SlotIndex(std::uint64_t Value) noexcept : Value(Value) {}

  std::uint64_t Value = 0;
};

class SlotIndexesAnalysis final {
public:
  class Result final {
  public:
    [[nodiscard]] SlotIndex instructionIndex(const MachineInstr &Instruction) const noexcept;
    [[nodiscard]] SlotIndex slot(const MachineInstr &Instruction) const noexcept {
      return instructionIndex(Instruction);
    }
    [[nodiscard]] SlotIndex blockStart(const MachineBasicBlock &Block) const noexcept;
    [[nodiscard]] SlotIndex blockEnd(const MachineBasicBlock &Block) const noexcept;
    [[nodiscard]] const MachineInstr *instructionFromIndex(SlotIndex Index) const noexcept;
    [[nodiscard]] const MachineBasicBlock *blockFromIndex(SlotIndex Index) const noexcept;

    [[nodiscard]] Expected<void> insertInstruction(const MachineInstr &Instruction);
    [[nodiscard]] Expected<void> eraseInstruction(const MachineInstr &Instruction);
    [[nodiscard]] Expected<void> renumber(MachineFunction &Function);

    [[nodiscard]] bool invalidate(const MachineFunction &,
                                  const PreservedAnalyses &Preserved) const noexcept;

  private:
    std::unordered_map<MachineInstrStableID, SlotIndex> InstructionSlots;
    std::unordered_map<std::uint64_t, const MachineInstr *> SlotToInstruction;
    std::unordered_map<const MachineBasicBlock *, SlotIndex> BlockStarts;
    std::unordered_map<const MachineBasicBlock *, SlotIndex> BlockEnds;
    std::vector<const MachineBasicBlock *> BlocksByStart;

    friend class SlotIndexesAnalysis;
  };

  [[nodiscard]] static constexpr std::string_view name() noexcept {
    return "SlotIndexes";
  }

  [[nodiscard]] static Expected<Result>
  run(MachineFunction &Function, const TargetDescription &Target, MachineAnalysisManager &Analyses);
};

class RegisterLaneSet final {
public:
  [[nodiscard]] LaneMask lanes(Register Reg) const noexcept;
  [[nodiscard]] bool contains(Register Reg, LaneMask Lanes = LaneMask::all()) const noexcept;
  [[nodiscard]] bool intersects(Register Reg, LaneMask Lanes = LaneMask::all()) const noexcept;
  [[nodiscard]] bool empty() const noexcept {
    return Values.empty();
  }

  void add(Register Reg, LaneMask Lanes);
  void remove(Register Reg, LaneMask Lanes);
  void unite(const RegisterLaneSet &Other);
  void subtract(const RegisterLaneSet &Other);

  [[nodiscard]] const std::unordered_map<Register, LaneMask> &entries() const noexcept {
    return Values;
  }

  friend bool operator==(const RegisterLaneSet &, const RegisterLaneSet &) = default;

private:
  std::unordered_map<Register, LaneMask> Values;
};

class RegisterUnitLaneSet final {
public:
  [[nodiscard]] LaneMask lanes(RegisterUnitID Unit) const noexcept;
  [[nodiscard]] bool contains(RegisterUnitID Unit, LaneMask Lanes = LaneMask::all()) const noexcept;
  [[nodiscard]] bool intersects(RegisterUnitID Unit,
                                LaneMask Lanes = LaneMask::all()) const noexcept;
  [[nodiscard]] bool empty() const noexcept {
    return Values.empty();
  }

  void add(RegisterUnitID Unit, LaneMask Lanes);
  void remove(RegisterUnitID Unit, LaneMask Lanes);
  void unite(const RegisterUnitLaneSet &Other);
  void subtract(const RegisterUnitLaneSet &Other);

  [[nodiscard]] const std::unordered_map<RegisterUnitID, LaneMask> &entries() const noexcept {
    return Values;
  }

  friend bool operator==(const RegisterUnitLaneSet &, const RegisterUnitLaneSet &) = default;

private:
  std::unordered_map<RegisterUnitID, LaneMask> Values;
};

struct MachineBlockLiveness final {
  RegisterLaneSet VirtualUses;
  RegisterLaneSet VirtualDefs;
  RegisterLaneSet VirtualLiveIn;
  RegisterLaneSet VirtualLiveOut;
  RegisterUnitLaneSet PhysicalUses;
  RegisterUnitLaneSet PhysicalDefs;
  RegisterUnitLaneSet PhysicalLiveIn;
  RegisterUnitLaneSet PhysicalLiveOut;
};

class LiveVariablesAnalysis final {
public:
  class Result final {
  public:
    [[nodiscard]] MachineBlockLiveness &getOrCreateBlock(const MachineBasicBlock &Block);
    [[nodiscard]] const MachineBlockLiveness *block(const MachineBasicBlock &Block) const noexcept;
    [[nodiscard]] bool isLiveIn(const MachineBasicBlock &Block, Register Reg,
                                LaneMask Lanes = LaneMask::all()) const noexcept;
    [[nodiscard]] bool isLiveOut(const MachineBasicBlock &Block, Register Reg,
                                 LaneMask Lanes = LaneMask::all()) const noexcept;
    [[nodiscard]] bool isUnitLiveIn(const MachineBasicBlock &Block, RegisterUnitID Unit,
                                    LaneMask Lanes = LaneMask::all()) const noexcept;
    [[nodiscard]] bool isUnitLiveOut(const MachineBasicBlock &Block, RegisterUnitID Unit,
                                     LaneMask Lanes = LaneMask::all()) const noexcept;

    [[nodiscard]] bool invalidate(const MachineFunction &,
                                  const PreservedAnalyses &Preserved) const noexcept;

  private:
    std::unordered_map<const MachineBasicBlock *, MachineBlockLiveness> Blocks;

    friend class LiveVariablesAnalysis;
  };

  [[nodiscard]] static constexpr std::string_view name() noexcept {
    return "LiveVariables";
  }

  [[nodiscard]] static Expected<Result>
  run(MachineFunction &Function, const TargetDescription &Target, MachineAnalysisManager &Analyses);
};

class LiveValueNumberID final {
public:
  constexpr LiveValueNumberID() noexcept = default;

  [[nodiscard]] static constexpr LiveValueNumberID fromValue(std::uint32_t Value) noexcept {
    return LiveValueNumberID(Value);
  }

  [[nodiscard]] constexpr bool isValid() const noexcept {
    return Value != 0;
  }

  [[nodiscard]] constexpr std::uint32_t value() const noexcept {
    return Value;
  }

  friend constexpr bool operator==(LiveValueNumberID, LiveValueNumberID) noexcept = default;
  friend constexpr auto operator<=>(LiveValueNumberID, LiveValueNumberID) noexcept = default;

private:
  explicit constexpr LiveValueNumberID(std::uint32_t Value) noexcept : Value(Value) {}

  std::uint32_t Value = 0;
};

enum class LiveValueState : std::uint8_t {
  Normal,
  Phi,
  Unused,
  Redefined,
};

struct LiveValueNumber final {
  LiveValueNumberID ID;
  SlotIndex Definition;
  LiveValueState State = LiveValueState::Normal;
  std::optional<MachineInstrStableID> Origin;

  friend bool operator==(const LiveValueNumber &, const LiveValueNumber &) = default;
};

struct LiveSegment final {
  SlotIndex Start;
  SlotIndex End;
  LiveValueNumberID Value;

  [[nodiscard]] constexpr bool empty() const noexcept {
    return !(Start < End);
  }

  [[nodiscard]] constexpr bool contains(SlotIndex Position) const noexcept {
    return !(Position < Start) && Position < End;
  }

  [[nodiscard]] constexpr bool overlaps(const LiveSegment &Other) const noexcept {
    return Start < Other.End && Other.Start < End;
  }

  friend constexpr bool operator==(const LiveSegment &, const LiveSegment &) noexcept = default;
};

class LiveRange final {
public:
  [[nodiscard]] std::span<const LiveSegment> segments() const noexcept {
    return Segments;
  }
  [[nodiscard]] std::span<const LiveValueNumber> values() const noexcept {
    return Values;
  }
  [[nodiscard]] bool empty() const noexcept {
    return Segments.empty();
  }

  [[nodiscard]] const LiveSegment *segmentAt(SlotIndex Position) const noexcept;
  [[nodiscard]] const LiveValueNumber *value(LiveValueNumberID ID) const noexcept;
  [[nodiscard]] const LiveValueNumber *valueAt(SlotIndex Position) const noexcept;
  [[nodiscard]] bool liveAt(SlotIndex Position) const noexcept;
  [[nodiscard]] bool overlaps(const LiveRange &Other) const noexcept;

  [[nodiscard]] Expected<LiveValueNumberID>
  createValue(SlotIndex Definition, LiveValueState State = LiveValueState::Normal,
              std::optional<MachineInstrStableID> Origin = std::nullopt);
  [[nodiscard]] Expected<void> addSegment(LiveSegment Segment);
  [[nodiscard]] Expected<void> extend(LiveValueNumberID Value, SlotIndex Start, SlotIndex End);
  [[nodiscard]] Expected<void> remove(SlotIndex Start, SlotIndex End);
  [[nodiscard]] Expected<void> shrinkToUses(std::span<const SlotIndex> Uses);
  [[nodiscard]] Expected<void> verify() const;

private:
  void mergeAdjacent();

  std::vector<LiveSegment> Segments;
  std::vector<LiveValueNumber> Values;
};

struct LiveSubRange final {
  LaneMask Lanes;
  LiveRange Range;
};

struct LiveInterval final {
  Register Reg;
  LiveRange MainRange;
  std::vector<LiveSubRange> SubRanges;
  std::uint64_t SpillWeight = 0;
  bool Spillable = true;
  std::vector<PhysicalRegister> AllocationHints;
  std::optional<Register> SplitParent;
  std::optional<Register> Origin;

  [[nodiscard]] bool overlaps(const LiveInterval &Other) const noexcept {
    return MainRange.overlaps(Other.MainRange);
  }
};

struct RegMaskPosition final {
  SlotIndex Position;
  MachineInstrStableID Instruction;
  std::shared_ptr<const RegisterMask> Mask;
};

class LiveIntervalsAnalysis final {
public:
  class Result final {
  public:
    [[nodiscard]] const SlotIndexesAnalysis::Result &slotIndexes() const noexcept {
      return Indexes;
    }
    [[nodiscard]] SlotIndex slot(const MachineInstr &Instruction) const noexcept {
      return Indexes.instructionIndex(Instruction);
    }
    [[nodiscard]] const LiveInterval *interval(Register Reg) const noexcept;
    [[nodiscard]] LiveInterval *interval(Register Reg) noexcept;
    [[nodiscard]] const LiveRange *regUnitRange(RegisterUnitID Unit) const noexcept;
    [[nodiscard]] std::span<const RegMaskPosition> regMaskPositions() const noexcept {
      return RegMasks;
    }
    [[nodiscard]] bool interferes(Register Left, Register Right) const noexcept;
    [[nodiscard]] bool regMaskClobbers(const LiveInterval &Interval,
                                       PhysicalRegister Candidate) const noexcept;
    [[nodiscard]] Expected<void> shrinkToUses(Register Reg, std::span<const SlotIndex> Uses);

    [[nodiscard]] bool invalidate(const MachineFunction &,
                                  const PreservedAnalyses &Preserved) const noexcept;

  private:
    SlotIndexesAnalysis::Result Indexes;
    std::unordered_map<Register, LiveInterval> Intervals;
    std::unordered_map<RegisterUnitID, LiveRange> RegisterUnitRanges;
    std::vector<RegMaskPosition> RegMasks;

    friend class LiveIntervalsAnalysis;
  };

  [[nodiscard]] static constexpr std::string_view name() noexcept {
    return "LiveIntervals";
  }

  [[nodiscard]] static Expected<Result>
  run(MachineFunction &Function, const TargetDescription &Target, MachineAnalysisManager &Analyses);
};

class RebuildMachineLivenessPass final : public MachineFunctionPass {
public:
  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineAnalysisManager &Analyses) override;
};

[[nodiscard]] Expected<void> verifyTrackedMachineLiveness(const MachineFunction &Function,
                                                          const TargetDescription &Target);
[[nodiscard]] Expected<bool> rebuildMachineLiveness(MachineFunction &Function,
                                                    const TargetDescription &Target);
[[nodiscard]] Expected<void> registerStage11MachineAnalyses(MachineAnalysisManager &Analyses);

} // namespace rcc

template <> struct std::hash<rcc::SlotIndex> {
  [[nodiscard]] std::size_t operator()(rcc::SlotIndex Index) const noexcept {
    return std::hash<std::uint64_t>{}(Index.value());
  }
};

template <> struct std::hash<rcc::LiveValueNumberID> {
  [[nodiscard]] std::size_t operator()(rcc::LiveValueNumberID ID) const noexcept {
    return std::hash<std::uint32_t>{}(ID.value());
  }
};

#endif // RCC_CODEGEN_MACHINELIVENESS_H
