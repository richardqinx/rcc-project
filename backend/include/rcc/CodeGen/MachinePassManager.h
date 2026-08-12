// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINEPASSMANAGER_H
#define RCC_CODEGEN_MACHINEPASSMANAGER_H

#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/MachineFunctionAnalysisManager.h"
#include "rcc/CodeGen/MachineFunctionProperties.h"
#include "rcc/IR/PassInstrumentation.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rcc {

class CompilationUnit;
class MachineFunction;
class TargetDescription;

struct MachinePassRegistrationMetadata {
  std::vector<PassParameter> Parameters;
  PassThreadSafety ThreadSafety = PassThreadSafety::NotThreadSafe;
  MachinePassChanges DeclaredChanges = MachinePassChanges::all();
};

struct MachinePassContract final : MachinePassRegistrationMetadata {
  std::string Name;
  MachineFunctionProperties Requires;
  MachineFunctionProperties Sets;
  MachineFunctionProperties Clears;
  std::vector<AnalysisID> RequiredAnalyses;
  std::optional<PipelineStage> VerificationStage;
};

struct MachinePassResult final {
  bool Changed = false;
  MachinePassChanges Changes;
  PreservedAnalyses Preserved = PreservedAnalyses::none();
};

struct CompilationUnitPassContract final {
  std::string Name;
  std::vector<AnalysisID> RequiredAnalyses;
  std::vector<PassParameter> Parameters;
  MachinePassChanges DeclaredChanges = MachinePassChanges::all();
};

struct CompilationUnitPassResult final {
  bool Changed = false;
  MachinePassChanges Changes;
  PreservedAnalyses PreservedUnitAnalyses = PreservedAnalyses::none();
  PreservedAnalyses PreservedFunctionAnalyses = PreservedAnalyses::none();
};

class MachineFunctionPass {
public:
  MachineFunctionPass() = default;
  MachineFunctionPass(const MachineFunctionPass &) = delete;
  MachineFunctionPass &operator=(const MachineFunctionPass &) = delete;
  virtual ~MachineFunctionPass() = default;

  [[nodiscard]] virtual const MachinePassContract &contract() const noexcept = 0;
  [[nodiscard]] virtual Expected<MachinePassResult>
  run(MachineFunction &Function, const TargetDescription &Target,
      MachineFunctionAnalysisManager &Analyses) = 0;
};

class CompilationUnitPass {
public:
  CompilationUnitPass() = default;
  CompilationUnitPass(const CompilationUnitPass &) = delete;
  CompilationUnitPass &operator=(const CompilationUnitPass &) = delete;
  virtual ~CompilationUnitPass() = default;

  [[nodiscard]] virtual const CompilationUnitPassContract &contract() const noexcept = 0;
  [[nodiscard]] virtual Expected<CompilationUnitPassResult>
  run(CompilationUnit &Unit, const TargetDescription &Target,
      CompilationUnitAnalysisManager &UnitAnalyses,
      MachineFunctionAnalysisManager &FunctionAnalyses) = 0;
};

struct MachinePassManagerOptions final {
  bool VerifyBefore = true;
  bool VerifyEach = true;
  bool DetectUnreportedChanges = false;
};

template <>
struct PassManagerCallbacks<MachineFunction, MachineFunctionPass, MachinePassManagerOptions> final {
  using PassList = std::vector<std::unique_ptr<MachineFunctionPass>>;

  [[nodiscard]] static Expected<void> validate(const MachineFunctionPass &Pass);
  [[nodiscard]] static Expected<void>
  run(const PassList &Passes, const MachinePassManagerOptions &Options, MachineFunction &Function,
      const TargetDescription &Target, MachineFunctionAnalysisManager &Analyses,
      MachinePassInstrumentation *Instrumentation = nullptr);
  [[nodiscard]] static Expected<void>
  runFunctions(const PassList &Passes, const MachinePassManagerOptions &Options,
               CompilationUnit &Unit, const TargetDescription &Target,
               const MachineFunctionAnalysisManager &AnalysisPrototype, std::size_t ThreadCount,
               MachinePassInstrumentation *Instrumentation = nullptr);
};

using MachineFunctionPassManager =
    PassManager<MachineFunction, MachineFunctionPass, MachinePassManagerOptions>;

struct CompilationUnitPassManagerOptions final {
  bool VerifyBefore = true;
  bool VerifyEach = true;
};

template <>
struct PassManagerCallbacks<CompilationUnit, CompilationUnitPass, CompilationUnitPassManagerOptions>
    final {
  using PassList = std::vector<std::unique_ptr<CompilationUnitPass>>;

  [[nodiscard]] static Expected<void> validate(const CompilationUnitPass &Pass);
  [[nodiscard]] static Expected<void> run(const PassList &Passes,
                                          const CompilationUnitPassManagerOptions &Options,
                                          CompilationUnit &Unit, const TargetDescription &Target,
                                          CompilationUnitAnalysisManager &UnitAnalyses,
                                          MachineFunctionAnalysisManager &FunctionAnalyses,
                                          MachinePassInstrumentation *Instrumentation = nullptr);
};

using CompilationUnitPassManager =
    PassManager<CompilationUnit, CompilationUnitPass, CompilationUnitPassManagerOptions>;

} // namespace rcc

#endif // RCC_CODEGEN_MACHINEPASSMANAGER_H
