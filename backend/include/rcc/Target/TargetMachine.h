// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_TARGET_TARGETMACHINE_H
#define RCC_TARGET_TARGETMACHINE_H

#include "rcc/CodeGen/MachineOutliner.h"
#include "rcc/CodeGen/MachinePassManager.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/Support/Error.h"
#include "rcc/Target/TargetOptions.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace rcc {

class CompilationUnit;
class MachineFunction;
class Target;

class TargetMachine {
public:
  TargetMachine(const TargetMachine &) = delete;
  TargetMachine &operator=(const TargetMachine &) = delete;
  TargetMachine(TargetMachine &&) = delete;
  TargetMachine &operator=(TargetMachine &&) = delete;
  virtual ~TargetMachine();

  [[nodiscard]] const Target &target() const noexcept {
    return RegisteredTarget;
  }

  [[nodiscard]] const TargetDescription &targetDescription() const noexcept {
    return Description;
  }

  [[nodiscard]] const TargetMachineOptions &options() const noexcept {
    return Options;
  }

  [[nodiscard]] virtual Expected<void>
  addCodeGenPasses(MachineFunctionPassManager &Pipeline, const MachineFunction &Function,
                   TargetPipelineOptions PipelineOptions) const = 0;
  [[nodiscard]] virtual Expected<std::unique_ptr<MachineFunctionPass>>
  createMachinePass(std::string_view Name, TargetPipelineOptions PipelineOptions) const = 0;
  [[nodiscard]] virtual Expected<std::vector<std::string>>
  codeGenPassNames(const MachineFunction &Function) const = 0;
  [[nodiscard]] virtual Expected<std::unique_ptr<TargetMachineOutlinerInfo>>
  createOutlinerInfo() const = 0;
  [[nodiscard]] virtual Expected<std::string> emitAssembly(const CompilationUnit &Unit) const = 0;
  [[nodiscard]] virtual Expected<std::vector<std::uint8_t>>
  emitObject(const CompilationUnit &Unit) const = 0;

protected:
  TargetMachine(const Target &RegisteredTarget, TargetMachineOptions Options,
                TargetDescription Description);

private:
  const Target &RegisteredTarget;
  TargetMachineOptions Options;
  TargetDescription Description;
};

} // namespace rcc

#endif // RCC_TARGET_TARGETMACHINE_H
