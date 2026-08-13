// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachinePassManager.h"

#include "rcc/CodeGen/CompilationUnit.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineFunctionCloner.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineVerifier.h"
#include "rcc/CodeGen/TargetDescription.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rcc {

thread_local MachinePassInstrumentation::BufferedEvents *MachinePassInstrumentation::ActiveBuffer =
    nullptr;

namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] std::uint64_t elapsedNanoseconds(Clock::time_point Start) noexcept {
  const auto Elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - Start);
  if (Elapsed.count() <= 0)
    return 0;
  return static_cast<std::uint64_t>(Elapsed.count());
}

[[nodiscard]] bool hasDuplicateAnalysisIDs(std::span<const AnalysisID> IDs) {
  std::unordered_set<AnalysisID> Unique;
  for (const AnalysisID ID : IDs) {
    if (ID == nullptr || !Unique.insert(ID).second)
      return true;
  }
  return false;
}

[[nodiscard]] Expected<void> validateParameters(std::span<const PassParameter> Parameters) {
  std::unordered_set<std::string> Names;
  for (const PassParameter &Parameter : Parameters) {
    if (Parameter.Name.empty())
      return makeUnexpected(ErrorCode::InvalidArgument, "pass parameter name cannot be empty");
    if (!Names.insert(Parameter.Name).second)
      return makeUnexpected(ErrorCode::InvalidArgument, "pass parameter names must be unique");
    if (Parameter.Required && Parameter.DefaultValue) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "a required pass parameter cannot have a default value");
    }
  }
  return {};
}

void combineFingerprint(std::uint64_t &State, std::uint64_t Value) noexcept {
  State ^= Value + 0x9e3779b97f4a7c15ULL + (State << 6U) + (State >> 2U);
}

[[nodiscard]] std::uint64_t machineFunctionFingerprint(const MachineFunction &Function) noexcept {
  std::uint64_t Result = 0xcbf29ce484222325ULL;
  combineFingerprint(Result, Function.stableID().value());
  combineFingerprint(Result, Function.properties().rawValue());
  combineFingerprint(Result, Function.blockCount());
  combineFingerprint(Result, Function.allInstructions().size());
  combineFingerprint(Result, Function.allMemOperands().size());

  for (const MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
       Block = Block->next()) {
    combineFingerprint(Result, Block->stableID().value());
    combineFingerprint(Result, Block->instructionCount());
    combineFingerprint(Result, Block->successors().size());
    for (const MachineSuccessor &Successor : Block->successors()) {
      combineFingerprint(Result,
                         Successor.Block == nullptr ? 0 : Successor.Block->stableID().value());
      combineFingerprint(Result, Successor.Probability.rawValue());
    }
    for (const MachineBasicBlockLiveIn &LiveIn : Block->liveIns()) {
      combineFingerprint(Result, LiveIn.PhysReg.number());
      combineFingerprint(Result, LiveIn.Lanes.rawValue());
    }

    for (const MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      combineFingerprint(Result, Instruction->stableID().value());
      combineFingerprint(Result, Instruction->opcode().value());
      combineFingerprint(Result, Instruction->flags().rawValue());
      combineFingerprint(Result, Instruction->operandCount());
      combineFingerprint(Result, Instruction->memOperands().size());
      for (const MachineOperand &Operand : Instruction->operands()) {
        combineFingerprint(Result, static_cast<std::uint64_t>(Operand.kind()));
        combineFingerprint(Result, Operand.targetFlags());
        combineFingerprint(Result, Operand.registerFlags().rawValue());
        combineFingerprint(Result, Operand.subReg().value());
        if (const Register *Reg = Operand.getIf<Register>())
          combineFingerprint(Result, Reg->opaqueValue());
        else if (const std::int64_t *Immediate = Operand.getIf<std::int64_t>())
          combineFingerprint(Result, static_cast<std::uint64_t>(*Immediate));
      }
    }
  }
  return Result;
}

[[nodiscard]] Expected<void> verifyCompilationUnit(const CompilationUnit &Unit,
                                                   const TargetDescription &Target) {
  auto UnitVerification = Unit.verify();
  if (!UnitVerification)
    return std::unexpected(UnitVerification.error());
  for (std::size_t Index = 0; Index < Unit.functionCount(); ++Index) {
    const MachineFunction *Function = Unit.functionAt(Index);
    if (Function == nullptr)
      return makeUnexpected(ErrorCode::InternalError,
                            "compilation unit function order contains a null entry");
    auto FunctionVerification = MachineVerifier::verify(*Function, Target);
    if (!FunctionVerification)
      return std::unexpected(FunctionVerification.error());
  }
  return {};
}

} // namespace

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

Expected<bool> MachineActionReplay::request(std::string_view Rule, const MachineFunction &Function,
                                            MachineInstrStableID Instruction, std::uint64_t Cost) {
  if (!IsReplaying) {
    const auto Position = RecordingInstructionIDs.find(Instruction);
    const MachineInstrStableID CanonicalInstruction =
        Position == RecordingInstructionIDs.end() ? Instruction : Position->second;
    auto Accepted = Request(Rule, Function, CanonicalInstruction, Cost);
    if (!Accepted)
      return std::unexpected(Accepted.error());
    Decisions.push_back({
        .Rule = std::string(Rule),
        .Function = Function.stableID(),
        .Instruction = CanonicalInstruction,
        .Cost = Cost,
        .Accepted = *Accepted,
    });
    return *Accepted;
  }

  if (NextDecision >= Decisions.size()) {
    return makeUnexpected(ErrorCode::InternalError,
                          "machine action replay requested an unrecorded decision");
  }
  const Decision &Recorded = Decisions[NextDecision++];
  if (Recorded.Rule != Rule || Recorded.Function != Function.stableID() ||
      Recorded.Instruction != Instruction || Recorded.Cost != Cost) {
    std::string Message = "machine action replay diverged from transaction preflight: recorded ";
    Message.append(Recorded.Rule);
    Message.append(":f");
    Message.append(std::to_string(Recorded.Function.value()));
    Message.append(":i");
    Message.append(std::to_string(Recorded.Instruction.value()));
    Message.append(" cost=");
    Message.append(std::to_string(Recorded.Cost));
    Message.append(", requested ");
    Message.append(Rule);
    Message.append(":f");
    Message.append(std::to_string(Function.stableID().value()));
    Message.append(":i");
    Message.append(std::to_string(Instruction.value()));
    Message.append(" cost=");
    Message.append(std::to_string(Cost));
    return makeUnexpected(ErrorCode::InternalError, std::move(Message));
  }
  return Recorded.Accepted;
}

void MachineActionReplay::beginReplay() noexcept {
  IsReplaying = true;
  NextDecision = 0;
}

Expected<void> MachineActionReplay::finishReplay() const {
  if (!IsReplaying || NextDecision != Decisions.size()) {
    return makeUnexpected(ErrorCode::InternalError,
                          "machine action replay did not consume every recorded decision");
  }
  return {};
}

void MachinePassInstrumentation::addBeforeCallback(BeforeCallback Callback) {
  if (!Callback)
    return;
  std::scoped_lock Lock(Mutex);
  Before.push_back(std::move(Callback));
}

void MachinePassInstrumentation::addAfterCallback(AfterCallback Callback) {
  if (!Callback)
    return;
  std::scoped_lock Lock(Mutex);
  After.push_back(std::move(Callback));
}

void MachinePassInstrumentation::addFailureCallback(FailureCallback Callback) {
  if (!Callback)
    return;
  std::scoped_lock Lock(Mutex);
  Failure.push_back(std::move(Callback));
}

void MachinePassInstrumentation::addBeforeAnalysisCallback(BeforeAnalysisCallback Callback) {
  if (!Callback)
    return;
  std::scoped_lock Lock(Mutex);
  BeforeAnalysis.push_back(std::move(Callback));
}

void MachinePassInstrumentation::addAfterAnalysisCallback(AfterAnalysisCallback Callback) {
  if (!Callback)
    return;
  std::scoped_lock Lock(Mutex);
  AfterAnalysis.push_back(std::move(Callback));
}

void MachinePassInstrumentation::addAnalysisFailureCallback(AnalysisFailureCallback Callback) {
  if (!Callback)
    return;
  std::scoped_lock Lock(Mutex);
  AnalysisFailure.push_back(std::move(Callback));
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
  if (!Callback)
    return;
  std::scoped_lock Lock(Mutex);
  BeforeUnit.push_back(std::move(Callback));
}

void MachinePassInstrumentation::addAfterUnitCallback(AfterUnitCallback Callback) {
  if (!Callback)
    return;
  std::scoped_lock Lock(Mutex);
  AfterUnit.push_back(std::move(Callback));
}

void MachinePassInstrumentation::addUnitFailureCallback(UnitFailureCallback Callback) {
  if (!Callback)
    return;
  std::scoped_lock Lock(Mutex);
  UnitFailure.push_back(std::move(Callback));
}

void MachinePassInstrumentation::addBeforeUnitAnalysisCallback(
    BeforeUnitAnalysisCallback Callback) {
  if (!Callback)
    return;
  std::scoped_lock Lock(Mutex);
  BeforeUnitAnalysis.push_back(std::move(Callback));
}

void MachinePassInstrumentation::addAfterUnitAnalysisCallback(AfterUnitAnalysisCallback Callback) {
  if (!Callback)
    return;
  std::scoped_lock Lock(Mutex);
  AfterUnitAnalysis.push_back(std::move(Callback));
}

void MachinePassInstrumentation::addUnitAnalysisFailureCallback(
    UnitAnalysisFailureCallback Callback) {
  if (!Callback)
    return;
  std::scoped_lock Lock(Mutex);
  UnitAnalysisFailure.push_back(std::move(Callback));
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

bool MachinePassInstrumentation::bufferFunctionEvent(
    const MachineFunction &Function, std::function<void(const MachineFunction &)> Replay) const {
  if (ActiveBuffer == nullptr)
    return false;

  std::shared_ptr<const MachineFunction> Snapshot;
  auto Cloned = cloneMachineFunction(
      Function, *ActiveBuffer->Target,
      MachineFunctionCloneOptions{.NewStableID = Function.stableID(), .NewName = std::nullopt});
  if (Cloned) {
    Snapshot = std::shared_ptr<const MachineFunction>(std::move(Cloned->ClonedFunction));
  } else {
    Snapshot =
        std::shared_ptr<const MachineFunction>(&Function, [](const MachineFunction *) noexcept {});
  }
  ActiveBuffer->Events.emplace_back(
      [Snapshot = std::move(Snapshot), Replay = std::move(Replay)]() { Replay(*Snapshot); });
  return true;
}

void MachinePassInstrumentation::beginBuffering(BufferedEvents &Buffer,
                                                const TargetDescription &Target) const noexcept {
  Buffer.Target = &Target;
  Buffer.Events.clear();
  ActiveBuffer = &Buffer;
}

void MachinePassInstrumentation::endBuffering() const noexcept {
  ActiveBuffer = nullptr;
}

void MachinePassInstrumentation::replayBufferedEvents(BufferedEvents &Buffer) const {
  for (const std::function<void()> &Event : Buffer.Events)
    Event();
  Buffer.Events.clear();
  Buffer.Target = nullptr;
}

void MachinePassInstrumentation::before(std::string_view Name,
                                        const MachineFunction &Function) const {
  if (bufferFunctionEvent(Function, [this, Name = std::string(Name)](const MachineFunction &Input) {
        before(Name, Input);
      })) {
    return;
  }
  std::vector<BeforeCallback> Callbacks;
  std::vector<IRPrintCallback> Printers;
  {
    std::scoped_lock Lock(Mutex);
    Callbacks = Before;
    Printers = IRPrint;
  }
  for (const BeforeCallback &Callback : Callbacks)
    Callback(Name, Function);
  for (const IRPrintCallback &Printer : Printers)
    Printer(InstrumentationPhase::Before, Name, Function);
}

void MachinePassInstrumentation::after(std::string_view Name, const MachineFunction &Function,
                                       const MachinePassResult &Result,
                                       std::uint64_t Nanoseconds) const {
  if (bufferFunctionEvent(Function, [this, Name = std::string(Name), Result,
                                     Nanoseconds](const MachineFunction &Input) {
        after(Name, Input, Result, Nanoseconds);
      })) {
    return;
  }
  std::vector<AfterCallback> Callbacks;
  std::vector<IRPrintCallback> Printers;
  {
    std::scoped_lock Lock(Mutex);
    InstrumentationStatistic &Record = statistic(Name, false);
    ++Record.Invocations;
    Record.Changes += Result.Changed ? 1U : 0U;
    Record.TotalNanoseconds += Nanoseconds;
    Callbacks = After;
    Printers = IRPrint;
  }
  for (const AfterCallback &Callback : Callbacks)
    Callback(Name, Function, Result);
  for (const IRPrintCallback &Printer : Printers)
    Printer(InstrumentationPhase::After, Name, Function);
}

void MachinePassInstrumentation::failure(std::string_view Name, const MachineFunction &Function,
                                         const Error &ErrorValue, std::uint64_t Nanoseconds) const {
  if (bufferFunctionEvent(Function, [this, Name = std::string(Name), ErrorValue,
                                     Nanoseconds](const MachineFunction &Input) {
        failure(Name, Input, ErrorValue, Nanoseconds);
      })) {
    return;
  }
  std::vector<FailureCallback> Callbacks;
  {
    std::scoped_lock Lock(Mutex);
    InstrumentationStatistic &Record = statistic(Name, false);
    ++Record.Invocations;
    ++Record.Failures;
    Record.TotalNanoseconds += Nanoseconds;
    Callbacks = Failure;
  }
  for (const FailureCallback &Callback : Callbacks)
    Callback(Name, Function, ErrorValue);
}

void MachinePassInstrumentation::beforeAnalysis(std::string_view Name,
                                                const MachineFunction &Function) const {
  if (bufferFunctionEvent(Function, [this, Name = std::string(Name)](const MachineFunction &Input) {
        beforeAnalysis(Name, Input);
      })) {
    return;
  }
  std::vector<BeforeAnalysisCallback> Callbacks;
  {
    std::scoped_lock Lock(Mutex);
    Callbacks = BeforeAnalysis;
  }
  for (const BeforeAnalysisCallback &Callback : Callbacks)
    Callback(Name, Function);
}

void MachinePassInstrumentation::afterAnalysis(std::string_view Name,
                                               const MachineFunction &Function, bool CacheHit,
                                               std::uint64_t Nanoseconds) const {
  if (bufferFunctionEvent(Function, [this, Name = std::string(Name), CacheHit,
                                     Nanoseconds](const MachineFunction &Input) {
        afterAnalysis(Name, Input, CacheHit, Nanoseconds);
      })) {
    return;
  }
  std::vector<AfterAnalysisCallback> Callbacks;
  {
    std::scoped_lock Lock(Mutex);
    InstrumentationStatistic &Record = statistic(Name, true);
    ++Record.Invocations;
    Record.CacheHits += CacheHit ? 1U : 0U;
    Record.TotalNanoseconds += Nanoseconds;
    Callbacks = AfterAnalysis;
  }
  for (const AfterAnalysisCallback &Callback : Callbacks)
    Callback(Name, Function, CacheHit, Nanoseconds);
}

void MachinePassInstrumentation::analysisFailure(std::string_view Name,
                                                 const MachineFunction &Function,
                                                 const Error &ErrorValue,
                                                 std::uint64_t Nanoseconds) const {
  if (bufferFunctionEvent(Function, [this, Name = std::string(Name), ErrorValue,
                                     Nanoseconds](const MachineFunction &Input) {
        analysisFailure(Name, Input, ErrorValue, Nanoseconds);
      })) {
    return;
  }
  std::vector<AnalysisFailureCallback> Callbacks;
  {
    std::scoped_lock Lock(Mutex);
    InstrumentationStatistic &Record = statistic(Name, true);
    ++Record.Invocations;
    ++Record.Failures;
    Record.TotalNanoseconds += Nanoseconds;
    Callbacks = AnalysisFailure;
  }
  for (const AnalysisFailureCallback &Callback : Callbacks)
    Callback(Name, Function, ErrorValue);
}

void MachinePassInstrumentation::change(std::string_view Name, const MachineFunction &Function,
                                        MachinePassChanges Changes, bool FingerprintChanged) const {
  if (bufferFunctionEvent(Function, [this, Name = std::string(Name), Changes,
                                     FingerprintChanged](const MachineFunction &Input) {
        change(Name, Input, Changes, FingerprintChanged);
      })) {
    return;
  }
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
  std::vector<BeforeUnitCallback> Callbacks;
  {
    std::scoped_lock Lock(Mutex);
    Callbacks = BeforeUnit;
  }
  for (const BeforeUnitCallback &Callback : Callbacks)
    Callback(Name, Unit);
}

void MachinePassInstrumentation::afterUnit(std::string_view Name, const CompilationUnit &Unit,
                                           const CompilationUnitPassResult &Result,
                                           std::uint64_t Nanoseconds) const {
  std::vector<AfterUnitCallback> Callbacks;
  {
    std::scoped_lock Lock(Mutex);
    InstrumentationStatistic &Record = statistic("unit:" + std::string(Name), false);
    ++Record.Invocations;
    Record.Changes += Result.Changed ? 1U : 0U;
    Record.TotalNanoseconds += Nanoseconds;
    Callbacks = AfterUnit;
  }
  for (const AfterUnitCallback &Callback : Callbacks)
    Callback(Name, Unit, Result);
}

void MachinePassInstrumentation::unitFailure(std::string_view Name, const CompilationUnit &Unit,
                                             const Error &ErrorValue,
                                             std::uint64_t Nanoseconds) const {
  std::vector<UnitFailureCallback> Callbacks;
  {
    std::scoped_lock Lock(Mutex);
    InstrumentationStatistic &Record = statistic("unit:" + std::string(Name), false);
    ++Record.Invocations;
    ++Record.Failures;
    Record.TotalNanoseconds += Nanoseconds;
    Callbacks = UnitFailure;
  }
  for (const UnitFailureCallback &Callback : Callbacks)
    Callback(Name, Unit, ErrorValue);
}

void MachinePassInstrumentation::beforeAnalysis(std::string_view Name,
                                                const CompilationUnit &Unit) const {
  std::vector<BeforeUnitAnalysisCallback> Callbacks;
  {
    std::scoped_lock Lock(Mutex);
    Callbacks = BeforeUnitAnalysis;
  }
  for (const BeforeUnitAnalysisCallback &Callback : Callbacks)
    Callback(Name, Unit);
}

void MachinePassInstrumentation::afterAnalysis(std::string_view Name, const CompilationUnit &Unit,
                                               bool CacheHit, std::uint64_t Nanoseconds) const {
  std::vector<AfterUnitAnalysisCallback> Callbacks;
  {
    std::scoped_lock Lock(Mutex);
    InstrumentationStatistic &Record = statistic("unit:" + std::string(Name), true);
    ++Record.Invocations;
    Record.CacheHits += CacheHit ? 1U : 0U;
    Record.TotalNanoseconds += Nanoseconds;
    Callbacks = AfterUnitAnalysis;
  }
  for (const AfterUnitAnalysisCallback &Callback : Callbacks)
    Callback(Name, Unit, CacheHit, Nanoseconds);
}

void MachinePassInstrumentation::analysisFailure(std::string_view Name, const CompilationUnit &Unit,
                                                 const Error &ErrorValue,
                                                 std::uint64_t Nanoseconds) const {
  std::vector<UnitAnalysisFailureCallback> Callbacks;
  {
    std::scoped_lock Lock(Mutex);
    InstrumentationStatistic &Record = statistic("unit:" + std::string(Name), true);
    ++Record.Invocations;
    ++Record.Failures;
    Record.TotalNanoseconds += Nanoseconds;
    Callbacks = UnitAnalysisFailure;
  }
  for (const UnitAnalysisFailureCallback &Callback : Callbacks)
    Callback(Name, Unit, ErrorValue);
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

Expected<void>
PassManagerCallbacks<MachineFunction, MachineFunctionPass, MachinePassManagerOptions>::validate(
    const MachineFunctionPass &Pass) {
  const MachinePassContract &Contract = Pass.contract();
  if (Contract.Name.empty())
    return makeUnexpected(ErrorCode::InvalidArgument, "machine pass must have a non-empty name");
  if (Contract.Sets.intersects(Contract.Clears)) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "machine pass cannot set and clear the same property");
  }
  if (hasDuplicateAnalysisIDs(Contract.RequiredAnalyses)) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "machine pass required analyses must be non-null and unique");
  }
  return validateParameters(Contract.Parameters);
}

Expected<void>
PassManagerCallbacks<MachineFunction, MachineFunctionPass, MachinePassManagerOptions>::run(
    const PassList &Passes, const MachinePassManagerOptions &Options, MachineFunction &Function,
    const TargetDescription &Target, MachineFunctionAnalysisManager &Analyses,
    MachinePassInstrumentation *Instrumentation) {
  Analyses.setInstrumentation(Instrumentation);
  for (const std::unique_ptr<MachineFunctionPass> &Pass : Passes) {
    const MachinePassContract &Contract = Pass->contract();
    const std::string_view Name = Contract.Name;

    if (const MachineFunctionProperties Missing =
            Contract.Requires.missingFrom(Function.properties());
        !Missing.empty()) {
      Error Failure(ErrorCode::VerificationError,
                    "pass '" + std::string(Name) + "' requires properties: " + Missing.toString());
      if (Instrumentation != nullptr)
        Instrumentation->failure(Name, Function, Failure);
      Analyses.setInstrumentation(nullptr);
      return std::unexpected(std::move(Failure));
    }

    if (Options.VerifyBefore) {
      auto Verification = MachineVerifier::verify(Function, Target);
      if (!Verification) {
        Error Failure = withErrorContext(
            Verification.error(), "pass '" + std::string(Name) + "' received invalid MachineIR: ");
        if (Instrumentation != nullptr)
          Instrumentation->failure(Name, Function, Failure);
        Analyses.setInstrumentation(nullptr);
        return std::unexpected(std::move(Failure));
      }
    }

    for (const AnalysisID Required : Contract.RequiredAnalyses) {
      auto Result = Analyses.getResultByID(Required, Function, Target);
      if (!Result) {
        Error Failure =
            withErrorContext(Result.error(), "pass '" + std::string(Name) +
                                                 "' could not materialize a required analysis: ");
        if (Instrumentation != nullptr)
          Instrumentation->failure(Name, Function, Failure);
        Analyses.setInstrumentation(nullptr);
        return std::unexpected(std::move(Failure));
      }
    }

    const std::uint64_t FingerprintBefore =
        Options.DetectUnreportedChanges ? machineFunctionFingerprint(Function) : 0;
    if (Instrumentation != nullptr)
      Instrumentation->before(Name, Function);
    const Clock::time_point Start = Clock::now();
    Analyses.setActivePass(std::string(Name));
    auto PassResult = Pass->run(Function, Target, Analyses);
    Analyses.setActivePass({});
    const std::uint64_t Duration = elapsedNanoseconds(Start);
    if (!PassResult) {
      Error Failure =
          withErrorContext(PassResult.error(), "pass '" + std::string(Name) + "' failed: ");
      if (Instrumentation != nullptr)
        Instrumentation->failure(Name, Function, Failure, Duration);
      Analyses.clear(Function);
      Analyses.setInstrumentation(nullptr);
      return std::unexpected(std::move(Failure));
    }

    if (PassResult->Changed != !PassResult->Changes.empty()) {
      Error Failure(ErrorCode::VerificationError,
                    "pass '" + std::string(Name) +
                        "' reported inconsistent changed state and modification categories");
      if (Instrumentation != nullptr)
        Instrumentation->failure(Name, Function, Failure, Duration);
      Analyses.clear(Function);
      Analyses.setInstrumentation(nullptr);
      return std::unexpected(std::move(Failure));
    }
    if (!Contract.DeclaredChanges.contains(PassResult->Changes)) {
      Error Failure(ErrorCode::VerificationError,
                    "pass '" + std::string(Name) +
                        "' reported a modification category absent from its registration");
      if (Instrumentation != nullptr)
        Instrumentation->failure(Name, Function, Failure, Duration);
      Analyses.clear(Function);
      Analyses.setInstrumentation(nullptr);
      return std::unexpected(std::move(Failure));
    }

    bool FingerprintChanged = PassResult->Changed;
    if (Options.DetectUnreportedChanges) {
      FingerprintChanged = FingerprintBefore != machineFunctionFingerprint(Function);
      if (FingerprintChanged && !PassResult->Changed) {
        Error Failure(ErrorCode::VerificationError,
                      "pass '" + std::string(Name) +
                          "' changed MachineIR without reporting a modification");
        if (Instrumentation != nullptr)
          Instrumentation->failure(Name, Function, Failure, Duration);
        Analyses.clear(Function);
        Analyses.setInstrumentation(nullptr);
        return std::unexpected(std::move(Failure));
      }
    }
    if (Instrumentation != nullptr)
      Instrumentation->change(Name, Function, PassResult->Changes, FingerprintChanged);

    auto Transition =
        Function.properties().applyTransition(Contract.Requires, Contract.Sets, Contract.Clears);
    if (!Transition) {
      Error Failure =
          withErrorContext(Transition.error(), "pass '" + std::string(Name) +
                                                   "' produced an invalid property transition: ");
      if (Instrumentation != nullptr)
        Instrumentation->failure(Name, Function, Failure, Duration);
      Analyses.clear(Function);
      Analyses.setInstrumentation(nullptr);
      return std::unexpected(std::move(Failure));
    }
    Analyses.invalidate(Function, PassResult->Preserved, PassResult->Changes);

    if (Options.VerifyEach) {
      MachineVerifierOptions VerifyOptions;
      VerifyOptions.Stage = Contract.VerificationStage;
      auto Verification = MachineVerifier::verify(Function, Target, VerifyOptions);
      if (!Verification) {
        Error Failure = withErrorContext(Verification.error(), "pass '" + std::string(Name) +
                                                                   "' left invalid MachineIR: ");
        if (Instrumentation != nullptr)
          Instrumentation->failure(Name, Function, Failure, Duration);
        Analyses.clear(Function);
        Analyses.setInstrumentation(nullptr);
        return std::unexpected(std::move(Failure));
      }
    }
    if (Instrumentation != nullptr)
      Instrumentation->after(Name, Function, *PassResult, Duration);
  }
  Analyses.setInstrumentation(nullptr);
  return {};
}

Expected<void>
PassManagerCallbacks<MachineFunction, MachineFunctionPass, MachinePassManagerOptions>::runFunctions(
    const PassList &Passes, const MachinePassManagerOptions &Options, CompilationUnit &Unit,
    const TargetDescription &Target, const MachineFunctionAnalysisManager &AnalysisPrototype,
    std::size_t ThreadCount, MachinePassInstrumentation *Instrumentation) {
  if (ThreadCount == 0)
    return makeUnexpected(ErrorCode::InvalidArgument, "machine pass thread count must be nonzero");
  auto UnitVerification = verifyCompilationUnit(Unit, Target);
  if (!UnitVerification)
    return std::unexpected(UnitVerification.error());
  const std::size_t FunctionCount = Unit.functionCount();
  if (FunctionCount == 0)
    return {};
  const std::size_t WorkerCount = std::min(ThreadCount, FunctionCount);
  if (WorkerCount > 1) {
    for (const std::unique_ptr<MachineFunctionPass> &Pass : Passes) {
      if (Pass->contract().ThreadSafety != PassThreadSafety::ThreadSafe) {
        return makeUnexpected(ErrorCode::InvalidArgument,
                              "parallel machine pipeline contains a pass not marked thread-safe");
      }
      auto AnalysisSafety =
          AnalysisPrototype.validateParallelRequirements(Pass->contract().RequiredAnalyses);
      if (!AnalysisSafety)
        return std::unexpected(AnalysisSafety.error());
    }
  }
  std::vector<std::unique_ptr<MachineFunctionAnalysisManager>> Managers;
  Managers.reserve(FunctionCount);
  for (std::size_t Index = 0; Index < FunctionCount; ++Index) {
    auto Manager = AnalysisPrototype.forkEmpty();
    if (!Manager)
      return std::unexpected(Manager.error());
    Managers.push_back(std::move(*Manager));
  }

  std::vector<std::optional<Error>> Failures(FunctionCount);
  std::vector<MachinePassInstrumentation::BufferedEvents> EventBuffers(FunctionCount);
  std::atomic<std::size_t> NextFunction = 0;
  const auto Worker = [&]() {
    while (true) {
      const std::size_t Index = NextFunction.fetch_add(1, std::memory_order_relaxed);
      if (Index >= FunctionCount)
        return;
      MachineFunction *Function = Unit.functionAt(Index);
      if (Function == nullptr) {
        Failures[Index] = Error(ErrorCode::InternalError,
                                "compilation unit contains a null function during parallel run");
        continue;
      }
      if (Instrumentation != nullptr)
        Instrumentation->beginBuffering(EventBuffers[Index], Target);
      auto Result = PassManagerCallbacks::run(Passes, Options, *Function, Target, *Managers[Index],
                                              Instrumentation);
      if (Instrumentation != nullptr)
        Instrumentation->endBuffering();
      if (!Result)
        Failures[Index] = Result.error();
    }
  };

  std::vector<std::thread> Workers;
  Workers.reserve(WorkerCount);
  for (std::size_t Index = 0; Index < WorkerCount; ++Index)
    Workers.emplace_back(Worker);
  for (std::thread &WorkerThread : Workers)
    WorkerThread.join();

  for (std::size_t Index = 0; Index < FunctionCount; ++Index) {
    if (Instrumentation != nullptr)
      Instrumentation->replayBufferedEvents(EventBuffers[Index]);
    const auto &Failure = Failures[Index];
    if (Failure) {
      return std::unexpected(withErrorContext(
          *Failure, "function '" + std::string(Unit.functionAt(Index)->name()) + "' failed: "));
    }
  }
  return {};
}

Expected<void>
PassManagerCallbacks<CompilationUnit, CompilationUnitPass,
                     CompilationUnitPassManagerOptions>::validate(const CompilationUnitPass &Pass) {
  const CompilationUnitPassContract &Contract = Pass.contract();
  if (Contract.Name.empty()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "compilation-unit pass must have a non-empty name");
  }
  if (hasDuplicateAnalysisIDs(Contract.RequiredAnalyses)) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "compilation-unit pass required analyses must be non-null and unique");
  }
  auto Parameters = validateParameters(Contract.Parameters);
  if (!Parameters)
    return std::unexpected(Parameters.error());
  return {};
}

Expected<void>
PassManagerCallbacks<CompilationUnit, CompilationUnitPass, CompilationUnitPassManagerOptions>::run(
    const PassList &Passes, const CompilationUnitPassManagerOptions &Options, CompilationUnit &Unit,
    const TargetDescription &Target, CompilationUnitAnalysisManager &UnitAnalyses,
    MachineFunctionAnalysisManager &FunctionAnalyses, MachinePassInstrumentation *Instrumentation) {
  UnitAnalyses.setInstrumentation(Instrumentation);
  FunctionAnalyses.setInstrumentation(Instrumentation);
  for (const std::unique_ptr<CompilationUnitPass> &Pass : Passes) {
    const CompilationUnitPassContract &Contract = Pass->contract();
    const std::string_view Name = Contract.Name;

    if (Options.VerifyBefore) {
      auto Verification = verifyCompilationUnit(Unit, Target);
      if (!Verification) {
        Error Failure =
            withErrorContext(Verification.error(), "compilation-unit pass '" + std::string(Name) +
                                                       "' received invalid MachineIR: ");
        if (Instrumentation != nullptr)
          Instrumentation->unitFailure(Name, Unit, Failure);
        UnitAnalyses.setInstrumentation(nullptr);
        FunctionAnalyses.setInstrumentation(nullptr);
        return std::unexpected(std::move(Failure));
      }
    }

    for (const AnalysisID Required : Contract.RequiredAnalyses) {
      auto Result = UnitAnalyses.getResultByID(Required, Unit, Target);
      if (!Result) {
        Error Failure =
            withErrorContext(Result.error(), "compilation-unit pass '" + std::string(Name) +
                                                 "' could not materialize a required analysis: ");
        if (Instrumentation != nullptr)
          Instrumentation->unitFailure(Name, Unit, Failure);
        UnitAnalyses.setInstrumentation(nullptr);
        FunctionAnalyses.setInstrumentation(nullptr);
        return std::unexpected(std::move(Failure));
      }
    }

    if (Instrumentation != nullptr)
      Instrumentation->beforeUnit(Name, Unit);
    const Clock::time_point Start = Clock::now();
    auto Result = Pass->run(Unit, Target, UnitAnalyses, FunctionAnalyses);
    const std::uint64_t Duration = elapsedNanoseconds(Start);
    if (!Result) {
      Error Failure = withErrorContext(Result.error(), "compilation-unit pass '" +
                                                           std::string(Name) + "' failed: ");
      if (Instrumentation != nullptr)
        Instrumentation->unitFailure(Name, Unit, Failure, Duration);
      UnitAnalyses.clearAll();
      FunctionAnalyses.clearAll();
      UnitAnalyses.setInstrumentation(nullptr);
      FunctionAnalyses.setInstrumentation(nullptr);
      return std::unexpected(std::move(Failure));
    }
    if (Result->Changed != !Result->Changes.empty() ||
        !Contract.DeclaredChanges.contains(Result->Changes)) {
      Error Failure(ErrorCode::VerificationError,
                    "compilation-unit pass '" + std::string(Name) +
                        "' reported inconsistent or undeclared modifications");
      if (Instrumentation != nullptr)
        Instrumentation->unitFailure(Name, Unit, Failure, Duration);
      UnitAnalyses.clearAll();
      FunctionAnalyses.clearAll();
      UnitAnalyses.setInstrumentation(nullptr);
      FunctionAnalyses.setInstrumentation(nullptr);
      return std::unexpected(std::move(Failure));
    }

    UnitAnalyses.invalidate(Unit, Result->PreservedUnitAnalyses, Result->Changes);
    for (std::size_t Index = 0; Index < Unit.functionCount(); ++Index) {
      MachineFunction *Function = Unit.functionAt(Index);
      if (Function != nullptr) {
        FunctionAnalyses.invalidate(*Function, Result->PreservedFunctionAnalyses, Result->Changes);
      }
    }
    if (Options.VerifyEach) {
      auto Verification = verifyCompilationUnit(Unit, Target);
      if (!Verification) {
        Error Failure =
            withErrorContext(Verification.error(), "compilation-unit pass '" + std::string(Name) +
                                                       "' left invalid MachineIR: ");
        if (Instrumentation != nullptr)
          Instrumentation->unitFailure(Name, Unit, Failure, Duration);
        UnitAnalyses.clearAll();
        FunctionAnalyses.clearAll();
        UnitAnalyses.setInstrumentation(nullptr);
        FunctionAnalyses.setInstrumentation(nullptr);
        return std::unexpected(std::move(Failure));
      }
    }
    if (Instrumentation != nullptr)
      Instrumentation->afterUnit(Name, Unit, *Result, Duration);
  }
  UnitAnalyses.setInstrumentation(nullptr);
  FunctionAnalyses.setInstrumentation(nullptr);
  return {};
}

} // namespace rcc
