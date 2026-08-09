// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINEPSEUDOEXPANSION_H
#define RCC_CODEGEN_MACHINEPSEUDOEXPANSION_H

#include "rcc/CodeGen/MachinePass.h"
#include "rcc/CodeGen/TargetPseudoExpansion.h"

namespace rcc {

class TargetBranchInfo;
class TargetPseudoExpansionInfo;

class PreparePostRAPseudosPass final : public MachineFunctionPass {
public:
  explicit PreparePostRAPseudosPass(const TargetPseudoExpansionInfo &Info) noexcept : Info(&Info) {}

  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineAnalysisManager &Analyses) override;

private:
  const TargetPseudoExpansionInfo *Info;
};

class PrePEITargetPseudoExpansionPass final : public MachineFunctionPass {
public:
  explicit PrePEITargetPseudoExpansionPass(const TargetPseudoExpansionInfo &Info) noexcept
      : Info(&Info) {}

  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineAnalysisManager &Analyses) override;

private:
  const TargetPseudoExpansionInfo *Info;
};

class ExpandPostRAPseudosPass final : public MachineFunctionPass {
public:
  explicit ExpandPostRAPseudosPass(const TargetPseudoExpansionInfo &Info) noexcept : Info(&Info) {}

  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineAnalysisManager &Analyses) override;

private:
  const TargetPseudoExpansionInfo *Info;
};

class TargetLatePseudoExpansionPass final : public MachineFunctionPass {
public:
  TargetLatePseudoExpansionPass(const TargetPseudoExpansionInfo &Info,
                                const TargetBranchInfo &BranchInfo) noexcept
      : Info(&Info), BranchInfo(&BranchInfo) {}

  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineAnalysisManager &Analyses) override;

private:
  const TargetPseudoExpansionInfo *Info;
  const TargetBranchInfo *BranchInfo;
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINEPSEUDOEXPANSION_H
