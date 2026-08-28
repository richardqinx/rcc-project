// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINEPASSINSTRUMENTATION_H
#define RCC_CODEGEN_MACHINEPASSINSTRUMENTATION_H

#include "rcc/CodeGen/MachineFunctionAnalysisManager.h"
#include "rcc/IR/PassInstrumentation.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rcc {

class CompilationUnit;
class MachineFunction;
struct MachinePassResult;
struct CompilationUnitPassResult;

struct InstrumentationStatistic final {
  std::string Name;
  bool IsAnalysis = false;
  std::uint64_t Invocations = 0;
  std::uint64_t CacheHits = 0;
  std::uint64_t Changes = 0;
  std::uint64_t Failures = 0;
  std::uint64_t TotalNanoseconds = 0;
};

struct MachineActionID final {
  std::string Pass;
  std::string Rule;
  MachineFunctionStableID Function;
  MachineInstrStableID Instruction;

  [[nodiscard]] std::string toString() const;

  friend bool operator==(const MachineActionID &, const MachineActionID &) = default;
};

struct MachineActionEvent final {
  MachineActionID ID;
  std::uint64_t Cost = 0;
  bool Accepted = false;

  friend bool operator==(const MachineActionEvent &, const MachineActionEvent &) = default;
};

class MachinePassInstrumentation final {
  using FunctionCallbackRegistry = PassInstrumentationCallbacks<MachineFunction, MachinePassResult>;
  using UnitCallbackRegistry =
      PassInstrumentationCallbacks<CompilationUnit, CompilationUnitPassResult>;

public:
  using BeforeCallback = FunctionCallbackRegistry::BeforeCallback;
  using AfterCallback = FunctionCallbackRegistry::AfterCallback;
  using FailureCallback = FunctionCallbackRegistry::FailureCallback;
  using BeforeAnalysisCallback = FunctionCallbackRegistry::BeforeAnalysisCallback;
  using AfterAnalysisCallback = FunctionCallbackRegistry::AfterAnalysisCallback;
  using AnalysisFailureCallback = FunctionCallbackRegistry::AnalysisFailureCallback;
  using IRPrintCallback =
      std::function<void(InstrumentationPhase, std::string_view, const MachineFunction &)>;
  using ChangeCallback =
      std::function<void(std::string_view, const MachineFunction &, MachinePassChanges, bool)>;
  using ActionFilter = std::function<bool(const MachineActionID &, std::uint64_t)>;

  using BeforeUnitCallback = UnitCallbackRegistry::BeforeCallback;
  using AfterUnitCallback = UnitCallbackRegistry::AfterCallback;
  using UnitFailureCallback = UnitCallbackRegistry::FailureCallback;
  using BeforeUnitAnalysisCallback = UnitCallbackRegistry::BeforeAnalysisCallback;
  using AfterUnitAnalysisCallback = UnitCallbackRegistry::AfterAnalysisCallback;
  using UnitAnalysisFailureCallback = UnitCallbackRegistry::AnalysisFailureCallback;

  void addBeforeCallback(BeforeCallback Callback);
  void addAfterCallback(AfterCallback Callback);
  void addFailureCallback(FailureCallback Callback);
  void addBeforeAnalysisCallback(BeforeAnalysisCallback Callback);
  void addAfterAnalysisCallback(AfterAnalysisCallback Callback);
  void addAnalysisFailureCallback(AnalysisFailureCallback Callback);
  void addIRPrintCallback(IRPrintCallback Callback);
  void addChangeCallback(ChangeCallback Callback);
  void setActionFilter(ActionFilter Filter);
  void setPerFunctionFuel(std::optional<std::uint64_t> Fuel);

  void addBeforeUnitCallback(BeforeUnitCallback Callback);
  void addAfterUnitCallback(AfterUnitCallback Callback);
  void addUnitFailureCallback(UnitFailureCallback Callback);
  void addBeforeUnitAnalysisCallback(BeforeUnitAnalysisCallback Callback);
  void addAfterUnitAnalysisCallback(AfterUnitAnalysisCallback Callback);
  void addUnitAnalysisFailureCallback(UnitAnalysisFailureCallback Callback);

  [[nodiscard]] std::vector<InstrumentationStatistic> statistics() const;
  [[nodiscard]] std::vector<MachineActionEvent> actionEvents() const;
  [[nodiscard]] bool requiresSerialFunctionExecution() const;

  [[nodiscard]] Expected<bool> requestAction(std::string_view Pass, std::string_view Rule,
                                             const MachineFunction &Function,
                                             MachineInstrStableID Instruction, std::uint64_t Cost);
  [[nodiscard]] Expected<bool> requestAction(std::string_view Pass, std::string_view Rule,
                                             const MachineFunction &Function, std::nullptr_t,
                                             std::uint64_t Cost);

private:
  void before(std::string_view Name, const MachineFunction &Function) const;
  void after(std::string_view Name, const MachineFunction &Function,
             const MachinePassResult &Result, std::uint64_t Nanoseconds) const;
  void failure(std::string_view Name, const MachineFunction &Function, const Error &ErrorValue,
               std::uint64_t Nanoseconds = 0) const;
  void beforeAnalysis(std::string_view Name, const MachineFunction &Function) const;
  void afterAnalysis(std::string_view Name, const MachineFunction &Function, bool CacheHit,
                     std::uint64_t Nanoseconds) const;
  void analysisFailure(std::string_view Name, const MachineFunction &Function,
                       const Error &ErrorValue, std::uint64_t Nanoseconds) const;
  void change(std::string_view Name, const MachineFunction &Function, MachinePassChanges Changes,
              bool FingerprintChanged) const;

  void beforeUnit(std::string_view Name, const CompilationUnit &Unit) const;
  void afterUnit(std::string_view Name, const CompilationUnit &Unit,
                 const CompilationUnitPassResult &Result, std::uint64_t Nanoseconds) const;
  void unitFailure(std::string_view Name, const CompilationUnit &Unit, const Error &ErrorValue,
                   std::uint64_t Nanoseconds = 0) const;
  void beforeAnalysis(std::string_view Name, const CompilationUnit &Unit) const;
  void afterAnalysis(std::string_view Name, const CompilationUnit &Unit, bool CacheHit,
                     std::uint64_t Nanoseconds) const;
  void analysisFailure(std::string_view Name, const CompilationUnit &Unit, const Error &ErrorValue,
                       std::uint64_t Nanoseconds) const;

  [[nodiscard]] InstrumentationStatistic &statistic(std::string_view Name, bool IsAnalysis) const;

  FunctionCallbackRegistry FunctionCallbacks;
  UnitCallbackRegistry UnitCallbacks;
  mutable std::mutex Mutex;
  mutable std::mutex ActionMutex;
  std::vector<IRPrintCallback> IRPrint;
  std::vector<ChangeCallback> Change;
  ActionFilter Filter;
  std::optional<std::uint64_t> PerFunctionFuel;
  std::unordered_map<std::uint64_t, std::uint64_t> ConsumedFuel;
  std::vector<MachineActionEvent> ActionEvents;
  mutable std::unordered_map<std::string, InstrumentationStatistic> Statistics;

  template <typename, typename> friend struct AnalysisManagerInstrumentation;
  template <typename, typename, typename> friend struct PassManagerCallbacks;
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINEPASSINSTRUMENTATION_H
