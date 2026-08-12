// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINEFUNCTIONANALYSISMANAGER_H
#define RCC_CODEGEN_MACHINEFUNCTIONANALYSISMANAGER_H

#include "rcc/CodeGen/StableID.h"
#include "rcc/IR/PassManager.h"

#include <cstdint>
#include <initializer_list>

namespace rcc {

class CompilationUnit;
class MachineFunction;
class MachinePassInstrumentation;

enum class MachinePassChange : std::uint32_t {
  None = 0,
  Instructions = 1U << 0U,
  Operands = 1U << 1U,
  Registers = 1U << 2U,
  CFG = 1U << 3U,
  BlockLayout = 1U << 4U,
  Frame = 1U << 5U,
  ConstantPool = 1U << 6U,
  JumpTable = 1U << 7U,
  TargetState = 1U << 8U,
};

class MachinePassChanges final {
public:
  constexpr MachinePassChanges() noexcept = default;
  constexpr MachinePassChanges(std::initializer_list<MachinePassChange> Changes) noexcept {
    for (const MachinePassChange Change : Changes)
      add(Change);
  }

  [[nodiscard]] static constexpr MachinePassChanges all() noexcept {
    return fromRaw((1U << 9U) - 1U);
  }

  [[nodiscard]] static constexpr MachinePassChanges fromRaw(std::uint32_t Bits) noexcept {
    MachinePassChanges Result;
    Result.Bits = Bits & rawMask();
    return Result;
  }

  constexpr void add(MachinePassChange Change) noexcept {
    Bits |= static_cast<std::uint32_t>(Change);
  }

  [[nodiscard]] constexpr bool has(MachinePassChange Change) const noexcept {
    return (Bits & static_cast<std::uint32_t>(Change)) != 0;
  }

  [[nodiscard]] constexpr bool contains(MachinePassChanges Other) const noexcept {
    return (Bits & Other.Bits) == Other.Bits;
  }

  [[nodiscard]] constexpr bool intersects(MachinePassChanges Other) const noexcept {
    return (Bits & Other.Bits) != 0;
  }

  [[nodiscard]] constexpr bool empty() const noexcept {
    return Bits == 0;
  }

  [[nodiscard]] constexpr std::uint32_t rawValue() const noexcept {
    return Bits;
  }

  friend constexpr bool operator==(MachinePassChanges Left,
                                   MachinePassChanges Right) noexcept = default;

private:
  [[nodiscard]] static constexpr std::uint32_t rawMask() noexcept {
    return (1U << 9U) - 1U;
  }

  std::uint32_t Bits = 0;
};

template <>
struct AnalysisManagerInstrumentation<MachineFunction, MachinePassInstrumentation> final {
  static void beforeAnalysis(MachinePassInstrumentation *Instrumentation, std::string_view Name,
                             const MachineFunction &Function);
  static void afterAnalysis(MachinePassInstrumentation *Instrumentation, std::string_view Name,
                            const MachineFunction &Function, bool CacheHit,
                            std::uint64_t Nanoseconds);
  static void analysisFailure(MachinePassInstrumentation *Instrumentation, std::string_view Name,
                              const MachineFunction &Function, const Error &Failure,
                              std::uint64_t Nanoseconds);
  [[nodiscard]] static Expected<bool> requestAction(MachinePassInstrumentation *Instrumentation,
                                                    std::string_view Pass, std::string_view Rule,
                                                    const MachineFunction &Function,
                                                    MachineInstrStableID Instruction,
                                                    std::uint64_t Cost);
  [[nodiscard]] static Expected<bool> requestAction(MachinePassInstrumentation *Instrumentation,
                                                    std::string_view Pass, std::string_view Rule,
                                                    const MachineFunction &Function, std::nullptr_t,
                                                    std::uint64_t Cost);
};

template <>
struct AnalysisManagerInstrumentation<CompilationUnit, MachinePassInstrumentation> final {
  static void beforeAnalysis(MachinePassInstrumentation *Instrumentation, std::string_view Name,
                             const CompilationUnit &Unit);
  static void afterAnalysis(MachinePassInstrumentation *Instrumentation, std::string_view Name,
                            const CompilationUnit &Unit, bool CacheHit, std::uint64_t Nanoseconds);
  static void analysisFailure(MachinePassInstrumentation *Instrumentation, std::string_view Name,
                              const CompilationUnit &Unit, const Error &Failure,
                              std::uint64_t Nanoseconds);
};

using MachineAnalysisRegistration = AnalysisRegistration<MachinePassChanges>;
using CompilationUnitAnalysisRegistration = AnalysisRegistration<MachinePassChanges>;

using MachineFunctionAnalysisManager =
    AnalysisManager<MachineFunction, MachineAnalysisRegistration, MachinePassChanges,
                    MachinePassInstrumentation>;
using CompilationUnitAnalysisManager =
    AnalysisManager<CompilationUnit, CompilationUnitAnalysisRegistration, MachinePassChanges,
                    MachinePassInstrumentation>;

[[nodiscard]] Expected<void>
registerStage8MachineAnalyses(MachineFunctionAnalysisManager &Analyses);
[[nodiscard]] Expected<void>
registerStage11MachineAnalyses(MachineFunctionAnalysisManager &Analyses);

} // namespace rcc

#endif // RCC_CODEGEN_MACHINEFUNCTIONANALYSISMANAGER_H
