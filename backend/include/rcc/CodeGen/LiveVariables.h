// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_LIVEVARIABLES_H
#define RCC_CODEGEN_LIVEVARIABLES_H

#include "rcc/CodeGen/LiveRegUnits.h"
#include "rcc/CodeGen/MachinePassManager.h"
#include "rcc/CodeGen/Register.h"
#include "rcc/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace rcc {

class MachineBasicBlock;
class MachineFunction;
class TargetDescription;

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

struct MachineBlockLiveness final {
  RegisterLaneSet VirtualUses;
  RegisterLaneSet VirtualDefs;
  RegisterLaneSet VirtualLiveIn;
  RegisterLaneSet VirtualLiveOut;
  LiveRegUnits PhysicalUses;
  LiveRegUnits PhysicalDefs;
  LiveRegUnits PhysicalLiveIn;
  LiveRegUnits PhysicalLiveOut;
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

  [[nodiscard]] static Expected<Result> run(MachineFunction &Function,
                                            const TargetDescription &Target,
                                            MachineFunctionAnalysisManager &Analyses);
};

class RebuildMachineLivenessPass final : public MachineFunctionPass {
public:
  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineFunctionAnalysisManager &Analyses) override;
};

[[nodiscard]] Expected<void> verifyTrackedMachineLiveness(const MachineFunction &Function,
                                                          const TargetDescription &Target);
[[nodiscard]] Expected<bool> rebuildMachineLiveness(MachineFunction &Function,
                                                    const TargetDescription &Target);
[[nodiscard]] Expected<void>
registerStage11MachineAnalyses(MachineFunctionAnalysisManager &Analyses);

} // namespace rcc

#endif // RCC_CODEGEN_LIVEVARIABLES_H
