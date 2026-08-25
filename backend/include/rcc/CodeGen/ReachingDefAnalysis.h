// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_REACHINGDEFANALYSIS_H
#define RCC_CODEGEN_REACHINGDEFANALYSIS_H

#include "rcc/CodeGen/MachineFunctionAnalysisManager.h"
#include "rcc/Support/Error.h"

#include <cstddef>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rcc {

class MachineFunction;
class MachineInstr;
class MachineOperand;
class TargetDescription;

class ReachingDefAnalysis final {
public:
  using DefinitionSet = std::vector<const MachineInstr *>;

  class Result final {
  public:
    [[nodiscard]] std::span<const MachineInstr *const>
    reachingDefinitions(const MachineOperand &Operand) const noexcept;
    [[nodiscard]] std::size_t instructionNumber(const MachineInstr &Instruction) const noexcept;
    [[nodiscard]] bool invalidate(const MachineFunction &,
                                  const PreservedAnalyses &Preserved) const noexcept;

  private:
    std::unordered_map<const MachineOperand *, DefinitionSet> OperandDefinitions;
    std::unordered_map<const MachineInstr *, std::size_t> InstructionNumbers;

    friend class ReachingDefAnalysis;
  };

  [[nodiscard]] static constexpr std::string_view name() noexcept {
    return "ReachingDefAnalysis";
  }

  [[nodiscard]] static Expected<Result> run(MachineFunction &Function,
                                            const TargetDescription &Target,
                                            MachineFunctionAnalysisManager &Analyses);
};

} // namespace rcc

#endif // RCC_CODEGEN_REACHINGDEFANALYSIS_H
