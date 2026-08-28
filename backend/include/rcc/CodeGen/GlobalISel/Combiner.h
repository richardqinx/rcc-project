// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_GLOBALISEL_COMBINER_H
#define RCC_CODEGEN_GLOBALISEL_COMBINER_H

#include "rcc/CodeGen/GlobalISel/GIMatchTableExecutor.h"
#include "rcc/CodeGen/MachinePassManager.h"

#include <cstddef>
#include <string>
#include <utility>

namespace rcc {

class GlobalISelCombinerPass final : public MachineFunctionPass {
public:
  GlobalISelCombinerPass(const GIMatchTable &Table, const GIMatchTableExecutor &Executor,
                         std::string PassName, std::size_t MaximumRewrites = 4096,
                         std::size_t MaximumIterations = 8) noexcept
      : Table(&Table), Executor(&Executor), PassName(std::move(PassName)),
        MaximumRewrites(MaximumRewrites), MaximumIterations(MaximumIterations) {
    Contract.Name = this->PassName;
    Contract.Requires = {MachineFunctionProperty::IsSSA};
    Contract.VerificationStage = PipelineStage::MachineIRInput;
    Contract.ThreadSafety = PassThreadSafety::ThreadSafe;
    Contract.DeclaredChanges = {MachinePassChange::Instructions, MachinePassChange::Operands,
                                MachinePassChange::Registers};
  }

  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineFunctionAnalysisManager &Analyses) override;

private:
  [[nodiscard]] Expected<MachinePassResult>
  runImplementation(MachineFunction &Function, const TargetDescription &Target,
                    MachineFunctionAnalysisManager &Analyses) const;

  const GIMatchTable *Table;
  const GIMatchTableExecutor *Executor;
  std::string PassName;
  std::size_t MaximumRewrites;
  std::size_t MaximumIterations;
  MachinePassContract Contract;
};

} // namespace rcc

#endif // RCC_CODEGEN_GLOBALISEL_COMBINER_H
