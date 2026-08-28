// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachinePassInstrumentation.h"

#include "rcc/CodeGen/CompilationUnit.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachinePassManager.h"

#include <algorithm>
#include <ranges>
#include <utility>

namespace rcc {

void AnalysisManagerInstrumentation<MachineFunction, MachinePassInstrumentation>::beforeAnalysis(
    MachinePassInstrumentation *Instrumentation, std::string_view Name,
    const MachineFunction &Function) {
  Instrumentation->beforeAnalysis(Name, Function);
}

void AnalysisManagerInstrumentation<MachineFunction, MachinePassInstrumentation>::afterAnalysis(
    MachinePassInstrumentation *Instrumentation, std::string_view Name,
    const MachineFunction &Function, bool CacheHit, std::uint64_t Nanoseconds) {
  Instrumentation->afterAnalysis(Name, Function, CacheHit, Nanoseconds);
}

void AnalysisManagerInstrumentation<MachineFunction, MachinePassInstrumentation>::analysisFailure(
    MachinePassInstrumentation *Instrumentation, std::string_view Name,
    const MachineFunction &Function, const Error &Failure, std::uint64_t Nanoseconds) {
  Instrumentation->analysisFailure(Name, Function, Failure, Nanoseconds);
}

Expected<bool>
AnalysisManagerInstrumentation<MachineFunction, MachinePassInstrumentation>::requestAction(
    MachinePassInstrumentation *Instrumentation, std::string_view Pass, std::string_view Rule,
    const MachineFunction &Function, MachineInstrStableID Instruction, std::uint64_t Cost) {
  return Instrumentation->requestAction(Pass, Rule, Function, Instruction, Cost);
}

Expected<bool>
AnalysisManagerInstrumentation<MachineFunction, MachinePassInstrumentation>::requestAction(
    MachinePassInstrumentation *Instrumentation, std::string_view Pass, std::string_view Rule,
    const MachineFunction &Function, std::nullptr_t, std::uint64_t Cost) {
  return Instrumentation->requestAction(Pass, Rule, Function, nullptr, Cost);
}

void AnalysisManagerInstrumentation<CompilationUnit, MachinePassInstrumentation>::beforeAnalysis(
    MachinePassInstrumentation *Instrumentation, std::string_view Name,
    const CompilationUnit &Unit) {
  Instrumentation->beforeAnalysis(Name, Unit);
}

void AnalysisManagerInstrumentation<CompilationUnit, MachinePassInstrumentation>::afterAnalysis(
    MachinePassInstrumentation *Instrumentation, std::string_view Name, const CompilationUnit &Unit,
    bool CacheHit, std::uint64_t Nanoseconds) {
  Instrumentation->afterAnalysis(Name, Unit, CacheHit, Nanoseconds);
}

void AnalysisManagerInstrumentation<CompilationUnit, MachinePassInstrumentation>::analysisFailure(
    MachinePassInstrumentation *Instrumentation, std::string_view Name, const CompilationUnit &Unit,
    const Error &Failure, std::uint64_t Nanoseconds) {
  Instrumentation->analysisFailure(Name, Unit, Failure, Nanoseconds);
}

std::string MachineActionID::toString() const {
  std::string Result;
  Result.reserve(Pass.size() + Rule.size() + 48U);
  Result.append(Pass);
  Result.push_back(':');
  Result.append(Rule);
  Result.append(":f");
  Result.append(std::to_string(Function.value()));
  Result.append(":i");
  Result.append(std::to_string(Instruction.value()));
  return Result;
}

void MachinePassInstrumentation::addBeforeCallback(BeforeCallback Callback) {
  FunctionCallbacks.addBeforeCallback(std::move(Callback));
}

void MachinePassInstrumentation::addAfterCallback(AfterCallback Callback) {
  FunctionCallbacks.addAfterCallback(std::move(Callback));
}

void MachinePassInstrumentation::addFailureCallback(FailureCallback Callback) {
  FunctionCallbacks.addFailureCallback(std::move(Callback));
}

void MachinePassInstrumentation::addBeforeAnalysisCallback(BeforeAnalysisCallback Callback) {
  FunctionCallbacks.addBeforeAnalysisCallback(std::move(Callback));
}

void MachinePassInstrumentation::addAfterAnalysisCallback(AfterAnalysisCallback Callback) {
  FunctionCallbacks.addAfterAnalysisCallback(std::move(Callback));
}

void MachinePassInstrumentation::addAnalysisFailureCallback(AnalysisFailureCallback Callback) {
  FunctionCallbacks.addAnalysisFailureCallback(std::move(Callback));
}

void MachinePassInstrumentation::addIRPrintCallback(IRPrintCallback Callback) {
  if (!Callback)
    return;
  std::scoped_lock Lock(Mutex);
  IRPrint.push_back(std::move(Callback));
}

void MachinePassInstrumentation::addChangeCallback(ChangeCallback Callback) {
  if (!Callback)
    return;
  std::scoped_lock Lock(Mutex);
  Change.push_back(std::move(Callback));
}

void MachinePassInstrumentation::setActionFilter(ActionFilter NewFilter) {
  std::scoped_lock Lock(Mutex);
  Filter = std::move(NewFilter);
}

void MachinePassInstrumentation::setPerFunctionFuel(std::optional<std::uint64_t> Fuel) {
  std::scoped_lock Lock(Mutex);
  PerFunctionFuel = Fuel;
  ConsumedFuel.clear();
  ActionEvents.clear();
}

void MachinePassInstrumentation::addBeforeUnitCallback(BeforeUnitCallback Callback) {
  UnitCallbacks.addBeforeCallback(std::move(Callback));
}

void MachinePassInstrumentation::addAfterUnitCallback(AfterUnitCallback Callback) {
  UnitCallbacks.addAfterCallback(std::move(Callback));
}

void MachinePassInstrumentation::addUnitFailureCallback(UnitFailureCallback Callback) {
  UnitCallbacks.addFailureCallback(std::move(Callback));
}

void MachinePassInstrumentation::addBeforeUnitAnalysisCallback(
    BeforeUnitAnalysisCallback Callback) {
  UnitCallbacks.addBeforeAnalysisCallback(std::move(Callback));
}

void MachinePassInstrumentation::addAfterUnitAnalysisCallback(AfterUnitAnalysisCallback Callback) {
  UnitCallbacks.addAfterAnalysisCallback(std::move(Callback));
}

void MachinePassInstrumentation::addUnitAnalysisFailureCallback(
    UnitAnalysisFailureCallback Callback) {
  UnitCallbacks.addAnalysisFailureCallback(std::move(Callback));
}

InstrumentationStatistic &MachinePassInstrumentation::statistic(std::string_view Name,
                                                                bool IsAnalysis) const {
  const std::string Key = std::string(IsAnalysis ? "analysis:" : "pass:") + std::string(Name);
  auto [Position, Inserted] = Statistics.try_emplace(Key);
  if (Inserted) {
    Position->second.Name = std::string(Name);
    Position->second.IsAnalysis = IsAnalysis;
  }
  return Position->second;
}

void MachinePassInstrumentation::before(std::string_view Name,
                                        const MachineFunction &Function) const {
  FunctionCallbacks.runBefore(Name, Function);
  std::vector<IRPrintCallback> Printers;
  {
    std::scoped_lock Lock(Mutex);
    Printers = IRPrint;
  }
  for (const IRPrintCallback &Printer : Printers)
    Printer(InstrumentationPhase::Before, Name, Function);
}

void MachinePassInstrumentation::after(std::string_view Name, const MachineFunction &Function,
                                       const MachinePassResult &Result,
                                       std::uint64_t Nanoseconds) const {
  std::vector<IRPrintCallback> Printers;
  {
    std::scoped_lock Lock(Mutex);
    InstrumentationStatistic &Record = statistic(Name, false);
    ++Record.Invocations;
    Record.Changes += Result.Changed ? 1U : 0U;
    Record.TotalNanoseconds += Nanoseconds;
    Printers = IRPrint;
  }
  FunctionCallbacks.runAfter(Name, Function, Result);
  for (const IRPrintCallback &Printer : Printers)
    Printer(InstrumentationPhase::After, Name, Function);
}

void MachinePassInstrumentation::failure(std::string_view Name, const MachineFunction &Function,
                                         const Error &ErrorValue, std::uint64_t Nanoseconds) const {
  {
    std::scoped_lock Lock(Mutex);
    InstrumentationStatistic &Record = statistic(Name, false);
    ++Record.Invocations;
    ++Record.Failures;
    Record.TotalNanoseconds += Nanoseconds;
  }
  FunctionCallbacks.runFailure(Name, Function, ErrorValue);
}

void MachinePassInstrumentation::beforeAnalysis(std::string_view Name,
                                                const MachineFunction &Function) const {
  FunctionCallbacks.runBeforeAnalysis(Name, Function);
}

void MachinePassInstrumentation::afterAnalysis(std::string_view Name,
                                               const MachineFunction &Function, bool CacheHit,
                                               std::uint64_t Nanoseconds) const {
  {
    std::scoped_lock Lock(Mutex);
    InstrumentationStatistic &Record = statistic(Name, true);
    ++Record.Invocations;
    Record.CacheHits += CacheHit ? 1U : 0U;
    Record.TotalNanoseconds += Nanoseconds;
  }
  FunctionCallbacks.runAfterAnalysis(Name, Function, CacheHit, Nanoseconds);
}

void MachinePassInstrumentation::analysisFailure(std::string_view Name,
                                                 const MachineFunction &Function,
                                                 const Error &ErrorValue,
                                                 std::uint64_t Nanoseconds) const {
  {
    std::scoped_lock Lock(Mutex);
    InstrumentationStatistic &Record = statistic(Name, true);
    ++Record.Invocations;
    ++Record.Failures;
    Record.TotalNanoseconds += Nanoseconds;
  }
  FunctionCallbacks.runAnalysisFailure(Name, Function, ErrorValue);
}

void MachinePassInstrumentation::change(std::string_view Name, const MachineFunction &Function,
                                        MachinePassChanges Changes, bool FingerprintChanged) const {
  std::vector<ChangeCallback> Callbacks;
  {
    std::scoped_lock Lock(Mutex);
    Callbacks = Change;
  }
  for (const ChangeCallback &Callback : Callbacks)
    Callback(Name, Function, Changes, FingerprintChanged);
}

Expected<bool> MachinePassInstrumentation::requestAction(std::string_view Pass,
                                                         std::string_view Rule,
                                                         const MachineFunction &Function,
                                                         MachineInstrStableID Instruction,
                                                         std::uint64_t Cost) {
  MachineActionID Action{
      .Pass = std::string(Pass),
      .Rule = std::string(Rule),
      .Function = Function.stableID(),
      .Instruction = Instruction,
  };
  if (Action.Pass.empty() || Action.Rule.empty() || Action.Function != Function.stableID() ||
      Cost == 0) {
    return makeUnexpected(ErrorCode::InvalidArgument, "machine action identity is invalid");
  }
  std::scoped_lock ActionLock(ActionMutex);
  ActionFilter CurrentFilter;
  {
    std::scoped_lock Lock(Mutex);
    CurrentFilter = Filter;
  }
  if (CurrentFilter && !CurrentFilter(Action, Cost)) {
    std::scoped_lock Lock(Mutex);
    ActionEvents.push_back({Action, Cost, false});
    return false;
  }

  std::scoped_lock Lock(Mutex);
  if (!PerFunctionFuel) {
    ActionEvents.push_back({Action, Cost, true});
    return true;
  }
  std::uint64_t &Consumed = ConsumedFuel[Function.stableID().value()];
  if (Consumed > *PerFunctionFuel || Cost > *PerFunctionFuel - Consumed) {
    ActionEvents.push_back({Action, Cost, false});
    return false;
  }
  Consumed += Cost;
  ActionEvents.push_back({Action, Cost, true});
  return true;
}

Expected<bool> MachinePassInstrumentation::requestAction(std::string_view Pass,
                                                         std::string_view Rule,
                                                         const MachineFunction &Function,
                                                         std::nullptr_t, std::uint64_t Cost) {
  return requestAction(Pass, Rule, Function, MachineInstrStableID{}, Cost);
}

void MachinePassInstrumentation::beforeUnit(std::string_view Name,
                                            const CompilationUnit &Unit) const {
  UnitCallbacks.runBefore(Name, Unit);
}

void MachinePassInstrumentation::afterUnit(std::string_view Name, const CompilationUnit &Unit,
                                           const CompilationUnitPassResult &Result,
                                           std::uint64_t Nanoseconds) const {
  {
    std::scoped_lock Lock(Mutex);
    InstrumentationStatistic &Record = statistic("unit:" + std::string(Name), false);
    ++Record.Invocations;
    Record.Changes += Result.Changed ? 1U : 0U;
    Record.TotalNanoseconds += Nanoseconds;
  }
  UnitCallbacks.runAfter(Name, Unit, Result);
}

void MachinePassInstrumentation::unitFailure(std::string_view Name, const CompilationUnit &Unit,
                                             const Error &ErrorValue,
                                             std::uint64_t Nanoseconds) const {
  {
    std::scoped_lock Lock(Mutex);
    InstrumentationStatistic &Record = statistic("unit:" + std::string(Name), false);
    ++Record.Invocations;
    ++Record.Failures;
    Record.TotalNanoseconds += Nanoseconds;
  }
  UnitCallbacks.runFailure(Name, Unit, ErrorValue);
}

void MachinePassInstrumentation::beforeAnalysis(std::string_view Name,
                                                const CompilationUnit &Unit) const {
  UnitCallbacks.runBeforeAnalysis(Name, Unit);
}

void MachinePassInstrumentation::afterAnalysis(std::string_view Name, const CompilationUnit &Unit,
                                               bool CacheHit, std::uint64_t Nanoseconds) const {
  {
    std::scoped_lock Lock(Mutex);
    InstrumentationStatistic &Record = statistic("unit:" + std::string(Name), true);
    ++Record.Invocations;
    Record.CacheHits += CacheHit ? 1U : 0U;
    Record.TotalNanoseconds += Nanoseconds;
  }
  UnitCallbacks.runAfterAnalysis(Name, Unit, CacheHit, Nanoseconds);
}

void MachinePassInstrumentation::analysisFailure(std::string_view Name, const CompilationUnit &Unit,
                                                 const Error &ErrorValue,
                                                 std::uint64_t Nanoseconds) const {
  {
    std::scoped_lock Lock(Mutex);
    InstrumentationStatistic &Record = statistic("unit:" + std::string(Name), true);
    ++Record.Invocations;
    ++Record.Failures;
    Record.TotalNanoseconds += Nanoseconds;
  }
  UnitCallbacks.runAnalysisFailure(Name, Unit, ErrorValue);
}

std::vector<InstrumentationStatistic> MachinePassInstrumentation::statistics() const {
  std::vector<InstrumentationStatistic> Result;
  {
    std::scoped_lock Lock(Mutex);
    Result.reserve(Statistics.size());
    for (const auto &[Key, Record] : Statistics) {
      (void)Key;
      Result.push_back(Record);
    }
  }
  std::ranges::sort(
      Result, [](const InstrumentationStatistic &Left, const InstrumentationStatistic &Right) {
        if (Left.IsAnalysis != Right.IsAnalysis)
          return Left.IsAnalysis < Right.IsAnalysis;
        return Left.Name < Right.Name;
      });
  return Result;
}

std::vector<MachineActionEvent> MachinePassInstrumentation::actionEvents() const {
  std::vector<MachineActionEvent> Result;
  {
    std::scoped_lock Lock(Mutex);
    Result = ActionEvents;
  }
  std::ranges::sort(Result, [](const MachineActionEvent &Left, const MachineActionEvent &Right) {
    if (Left.ID.Function.value() != Right.ID.Function.value())
      return Left.ID.Function.value() < Right.ID.Function.value();
    if (Left.ID.Instruction.value() != Right.ID.Instruction.value())
      return Left.ID.Instruction.value() < Right.ID.Instruction.value();
    if (Left.ID.Pass != Right.ID.Pass)
      return Left.ID.Pass < Right.ID.Pass;
    if (Left.ID.Rule != Right.ID.Rule)
      return Left.ID.Rule < Right.ID.Rule;
    if (Left.Cost != Right.Cost)
      return Left.Cost < Right.Cost;
    return Left.Accepted < Right.Accepted;
  });
  return Result;
}

bool MachinePassInstrumentation::requiresSerialFunctionExecution() const {
  if (!FunctionCallbacks.empty())
    return true;
  std::scoped_lock Lock(Mutex);
  return !IRPrint.empty() || !Change.empty();
}

} // namespace rcc
