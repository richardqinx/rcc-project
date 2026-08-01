// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINECOMPILATIONSTATUS_H
#define RCC_CODEGEN_MACHINECOMPILATIONSTATUS_H

#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/MachineIRTypes.h"
#include "rcc/CodeGen/StableID.h"
#include "rcc/Support/Error.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace rcc {

class MachineFunctionCloneContext;

enum class MachineCompilationState : std::uint8_t {
  Success,
  Unsupported,
  MIRParseFailed,
  LegalizationFailed,
  SelectionFailed,
  RegisterAllocationFailed,
  CallLoweringFailed,
  FrameLayoutFailed,
  FinalizationFailed,
  AssemblyLoweringFailed,
  InternalError,
  Count,
};

[[nodiscard]] bool isKnownMachineCompilationState(MachineCompilationState State) noexcept;
[[nodiscard]] std::string_view
getMachineCompilationStateName(MachineCompilationState State) noexcept;

enum class MachineCompilationDiagnosticSeverity : std::uint8_t {
  Error,
  Fatal,
  Count,
};

struct MachineCompilationDiagnostic final {
  MachineCompilationDiagnosticSeverity Severity = MachineCompilationDiagnosticSeverity::Error;
  ErrorCode Code = ErrorCode::InternalError;
  std::string Message;
  std::optional<SourceLocation> Location;

  friend bool operator==(const MachineCompilationDiagnostic &Left,
                         const MachineCompilationDiagnostic &Right) = default;
};

struct MachineCompilationFailure final {
  MachineCompilationDiagnostic Diagnostic;
  std::optional<std::string> FailurePass;
  std::optional<MachineInstrStableID> FailureInstruction;
  std::optional<PipelineStage> FailureStage;
  std::optional<std::string> FailureRule;
  std::optional<std::string> Target;
  std::optional<std::string> Subtarget;

  friend bool operator==(const MachineCompilationFailure &Left,
                         const MachineCompilationFailure &Right) = default;
};

class MachineCompilationStatus final {
public:
  MachineCompilationStatus() = default;
  MachineCompilationStatus(const MachineCompilationStatus &) = delete;
  MachineCompilationStatus &operator=(const MachineCompilationStatus &) = delete;
  MachineCompilationStatus(MachineCompilationStatus &&) noexcept = default;
  MachineCompilationStatus &operator=(MachineCompilationStatus &&) noexcept = default;
  ~MachineCompilationStatus() = default;

  [[nodiscard]] MachineCompilationState state() const noexcept {
    return State;
  }

  [[nodiscard]] bool succeeded() const noexcept {
    return State == MachineCompilationState::Success;
  }

  [[nodiscard]] const MachineCompilationFailure *failure() const noexcept {
    if (!Failure)
      return nullptr;
    return &*Failure;
  }

  [[nodiscard]] Expected<void> fail(MachineCompilationState NewState,
                                    MachineCompilationFailure NewFailure);
  [[nodiscard]] Expected<void> verify() const;
  [[nodiscard]] Expected<MachineCompilationStatus>
  clone(const MachineFunctionCloneContext &CloneContext) const;

  friend bool operator==(const MachineCompilationStatus &Left,
                         const MachineCompilationStatus &Right) = default;

private:
  MachineCompilationState State = MachineCompilationState::Success;
  std::optional<MachineCompilationFailure> Failure;
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINECOMPILATIONSTATUS_H
