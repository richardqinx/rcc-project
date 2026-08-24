// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_VIRTREGMAP_H
#define RCC_CODEGEN_VIRTREGMAP_H

#include "rcc/CodeGen/MachineFrameInfo.h"
#include "rcc/CodeGen/MachinePassManager.h"
#include "rcc/CodeGen/TargetRegisterInfo.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rcc {

class MachineFunction;
class TargetDescription;

enum class VirtRegAllocationState : std::uint8_t {
  Unassigned,
  Assigned,
  Spilled,
  Split,
};

struct VirtRegSpillMetadata final {
  std::optional<FrameIndex> StackSlot;
  LaneMask Lanes = LaneMask::all();
  std::uint64_t SpillCount = 0;
  std::uint64_t ReloadCount = 0;
  std::uint64_t RematerializationCount = 0;
};

struct VirtRegMapEntry final {
  Register VirtualReg;
  VirtRegAllocationState State = VirtRegAllocationState::Unassigned;
  std::optional<PhysicalRegister> PhysicalReg;
  std::optional<Register> SplitParent;
  std::optional<Register> Origin;
  bool Spillable = true;
  VirtRegSpillMetadata Spill;
};

class VirtRegMap final {
public:
  explicit VirtRegMap(MachineFunction &Function) noexcept : Function(&Function) {}

  [[nodiscard]] MachineFunction &function() const noexcept {
    return *Function;
  }
  [[nodiscard]] Expected<void> add(Register VirtualReg, bool Spillable = true,
                                   std::optional<Register> SplitParent = std::nullopt,
                                   std::optional<Register> Origin = std::nullopt);
  [[nodiscard]] Expected<void> assign(Register VirtualReg, PhysicalRegister PhysicalReg);
  [[nodiscard]] Expected<void> assignStackSlot(Register VirtualReg, FrameIndex StackSlot,
                                               LaneMask Lanes = LaneMask::all());
  [[nodiscard]] Expected<void> markSplit(Register VirtualReg, Register Parent, Register Origin);
  [[nodiscard]] Expected<void> setSpillable(Register VirtualReg, bool Spillable);
  [[nodiscard]] VirtRegMapEntry *entry(Register VirtualReg) noexcept;
  [[nodiscard]] const VirtRegMapEntry *entry(Register VirtualReg) const noexcept;
  [[nodiscard]] std::optional<PhysicalRegister>
  physicalRegister(Register VirtualReg) const noexcept;
  [[nodiscard]] std::optional<FrameIndex> stackSlot(Register VirtualReg) const noexcept;
  [[nodiscard]] bool isSpillable(Register VirtualReg) const noexcept;
  [[nodiscard]] std::vector<const VirtRegMapEntry *> orderedEntries() const;
  [[nodiscard]] Expected<void> verify() const;

private:
  MachineFunction *Function;
  std::unordered_map<Register, VirtRegMapEntry> Entries;
};

[[nodiscard]] Expected<bool> rewriteVirtualRegisters(MachineFunction &Function,
                                                     const TargetDescription &Target,
                                                     const VirtRegMap &Mapping);

class VirtRegMapAnalysis final {
public:
  using Result = VirtRegMap;

  [[nodiscard]] static constexpr std::string_view name() noexcept {
    return "VirtRegMap";
  }
  [[nodiscard]] static Expected<Result> run(MachineFunction &Function,
                                            const TargetDescription &Target,
                                            MachineFunctionAnalysisManager &Analyses);
};

class VirtualRegisterRewritePass final : public MachineFunctionPass {
public:
  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineFunctionAnalysisManager &Analyses) override;
};

} // namespace rcc

#endif // RCC_CODEGEN_VIRTREGMAP_H
