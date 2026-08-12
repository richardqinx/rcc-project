// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_LIVEINTERVALS_H
#define RCC_CODEGEN_LIVEINTERVALS_H

#include "rcc/CodeGen/LiveInterval.h"
#include "rcc/CodeGen/MachinePass.h"
#include "rcc/CodeGen/RegisterMask.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rcc {

class MachineFunction;
class MachineInstr;
class TargetDescription;

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

} // namespace rcc

#endif // RCC_CODEGEN_LIVEINTERVALS_H
