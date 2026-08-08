// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachinePass.h"

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

[[nodiscard]] Error prefixedError(const Error &Source, std::string Prefix) {
  Prefix.append(Source.message());
  return Error(Source.code(), std::move(Prefix));
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

Expected<void>
MachineAnalysisManager::registerAnalysisImpl(MachineAnalysisRegistration Registration,
                                             ErasedRunner Runner, ErasedInvalidator Invalidator) {
  if (Registration.ID == nullptr || Registration.Name.empty() || !Runner || !Invalidator) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "machine analysis registration is incomplete");
  }
  if (hasDuplicateAnalysisIDs(Registration.Dependencies)) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "machine analysis dependencies must be non-null and unique");
  }
  if (std::ranges::find(Registration.Dependencies, Registration.ID) !=
      Registration.Dependencies.end()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "machine analysis cannot directly depend on itself");
  }
  if (Registry.contains(Registration.ID))
    return makeUnexpected(ErrorCode::InvalidArgument, "machine analysis ID is already registered");
  if (std::ranges::any_of(Registry, [&Registration](const auto &Entry) {
        return Entry.second.Info.Name == Registration.Name;
      })) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "machine analysis name is already registered");
  }
  const AnalysisID RegistrationID = Registration.ID;
  Registry.emplace(RegistrationID, RegisteredAnalysis{std::move(Registration), std::move(Runner),
                                                      std::move(Invalidator)});
  return {};
}

bool MachineAnalysisManager::isRegistered(AnalysisID ID) const noexcept {
  return ID != nullptr && Registry.contains(ID);
}

bool MachineAnalysisManager::hasCached(const MachineFunction &Function,
                                       AnalysisID ID) const noexcept {
  const auto FunctionPosition = Cache.find(const_cast<MachineFunction *>(&Function));
  if (FunctionPosition == Cache.end())
    return false;
  const auto AnalysisPosition = FunctionPosition->second.find(ID);
  return AnalysisPosition != FunctionPosition->second.end() && AnalysisPosition->second.Value;
}

std::size_t
MachineAnalysisManager::cachedResultCount(const MachineFunction &Function) const noexcept {
  const auto Position = Cache.find(const_cast<MachineFunction *>(&Function));
  return Position == Cache.end() ? 0 : Position->second.size();
}

Expected<void>
MachineAnalysisManager::materializeDependencies(const RegisteredAnalysis &Registration,
                                                MachineFunction &Function,
                                                const TargetDescription &Target) {
  for (const AnalysisID Dependency : Registration.Info.Dependencies) {
    if (!Registry.contains(Dependency)) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "machine analysis depends on an unregistered analysis");
    }
    auto Result = getResultImpl(Dependency, Function, Target);
    if (!Result)
      return std::unexpected(Result.error());
  }
  return {};
}

Expected<void *> MachineAnalysisManager::getResultImpl(AnalysisID ID, MachineFunction &Function,
                                                       const TargetDescription &Target) {
  if (ID == nullptr)
    return makeUnexpected(ErrorCode::InvalidArgument, "machine analysis ID cannot be null");

  auto &FunctionCache = Cache[&Function];
  if (const auto Cached = FunctionCache.find(ID);
      Cached != FunctionCache.end() && Cached->second.Value) {
    const auto Registration = Registry.find(ID);
    const std::string_view Name = Registration == Registry.end()
                                      ? std::string_view{"unknown-machine-analysis"}
                                      : std::string_view{Registration->second.Info.Name};
    if (Instrumentation != nullptr)
      Instrumentation->afterAnalysis(Name, Function, true, 0);
    return Cached->second.Value.get();
  }

  const auto RegistrationPosition = Registry.find(ID);
  if (RegistrationPosition == Registry.end())
    return makeUnexpected(ErrorCode::InvalidArgument, "machine analysis ID is not registered");
  const RegisteredAnalysis &Registration = RegistrationPosition->second;

  auto &Stack = ActiveComputations[&Function];
  if (std::ranges::find(Stack, ID) != Stack.end()) {
    Error Failure(ErrorCode::InvalidArgument,
                  "machine analysis dependency cycle reaches '" + Registration.Info.Name + "'");
    if (Instrumentation != nullptr)
      Instrumentation->analysisFailure(Registration.Info.Name, Function, Failure, 0);
    return std::unexpected(std::move(Failure));
  }

  if (Instrumentation != nullptr)
    Instrumentation->beforeAnalysis(Registration.Info.Name, Function);
  const Clock::time_point Start = Clock::now();
  Stack.push_back(ID);
  auto Dependencies = materializeDependencies(Registration, Function, Target);
  if (!Dependencies) {
    Stack.pop_back();
    if (Stack.empty())
      ActiveComputations.erase(&Function);
    Error Failure = prefixedError(Dependencies.error(),
                                  "machine analysis '" + Registration.Info.Name + "' failed: ");
    if (Instrumentation != nullptr) {
      Instrumentation->analysisFailure(Registration.Info.Name, Function, Failure,
                                       elapsedNanoseconds(Start));
    }
    return std::unexpected(std::move(Failure));
  }

  auto Computed = Registration.Runner(Function, Target, *this);
  Stack.pop_back();
  if (Stack.empty())
    ActiveComputations.erase(&Function);
  const std::uint64_t Duration = elapsedNanoseconds(Start);
  if (!Computed) {
    Error Failure = prefixedError(Computed.error(),
                                  "machine analysis '" + Registration.Info.Name + "' failed: ");
    if (Instrumentation != nullptr)
      Instrumentation->analysisFailure(Registration.Info.Name, Function, Failure, Duration);
    return std::unexpected(std::move(Failure));
  }
  if (!*Computed) {
    Error Failure(ErrorCode::InternalError,
                  "machine analysis '" + Registration.Info.Name + "' returned a null result");
    if (Instrumentation != nullptr)
      Instrumentation->analysisFailure(Registration.Info.Name, Function, Failure, Duration);
    return std::unexpected(std::move(Failure));
  }

  void *Result = Computed->get();
  FunctionCache.emplace(ID, CacheEntry{std::move(*Computed), ID});
  if (Instrumentation != nullptr)
    Instrumentation->afterAnalysis(Registration.Info.Name, Function, false, Duration);
  return Result;
}

void MachineAnalysisManager::invalidate(MachineFunction &Function,
                                        const PreservedAnalyses &Preserved,
                                        MachinePassChanges Changes) noexcept {
  const auto FunctionPosition = Cache.find(&Function);
  if (FunctionPosition == Cache.end() || Preserved.preservesAll() || Changes.empty())
    return;

  std::set<AnalysisID> Invalid;
  for (auto &[ID, Entry] : FunctionPosition->second) {
    if (Preserved.preserves(ID))
      continue;
    const auto Registration = Registry.find(ID);
    if (Registration == Registry.end() ||
        Registration->second.Info.InvalidatedBy.intersects(Changes)) {
      const bool ShouldInvalidate =
          Registration == Registry.end() || !Entry.Value || !Registration->second.Invalidator ||
          Registration->second.Invalidator(Entry.Value.get(), Function, Preserved);
      if (ShouldInvalidate)
        Invalid.insert(ID);
    }
  }

  bool Added = true;
  while (Added) {
    Added = false;
    for (const auto &[ID, Entry] : FunctionPosition->second) {
      (void)Entry;
      if (Invalid.contains(ID) || Preserved.preserves(ID))
        continue;
      const auto Registration = Registry.find(ID);
      if (Registration != Registry.end() &&
          std::ranges::any_of(
              Registration->second.Info.Dependencies,
              [&Invalid](AnalysisID Dependency) { return Invalid.contains(Dependency); })) {
        Added = Invalid.insert(ID).second || Added;
      }
    }
  }

  for (const AnalysisID ID : Invalid)
    FunctionPosition->second.erase(ID);
  if (FunctionPosition->second.empty())
    Cache.erase(FunctionPosition);
}

void MachineAnalysisManager::clear(MachineFunction &Function) noexcept {
  Cache.erase(&Function);
  ActiveComputations.erase(&Function);
}

void MachineAnalysisManager::clearAll() noexcept {
  Cache.clear();
  ActiveComputations.clear();
}

Expected<std::unique_ptr<MachineAnalysisManager>> MachineAnalysisManager::forkEmpty() const {
  if (!ActiveComputations.empty()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "cannot fork a machine analysis manager during analysis computation");
  }
  auto Result = std::make_unique<MachineAnalysisManager>();
  Result->Registry = Registry;
  return Result;
}

Expected<void> MachineAnalysisManager::validateParallelRequirements(
    std::span<const AnalysisID> RequiredAnalyses) const {
  std::vector<AnalysisID> Worklist(RequiredAnalyses.begin(), RequiredAnalyses.end());
  std::unordered_set<AnalysisID> Visited;
  for (std::size_t Index = 0; Index < Worklist.size(); ++Index) {
    const AnalysisID ID = Worklist[Index];
    if (ID == nullptr || !Visited.insert(ID).second)
      continue;
    const auto Position = Registry.find(ID);
    if (Position == Registry.end()) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "parallel machine pass requires an unregistered analysis");
    }
    if (Position->second.Info.ThreadSafety != PassThreadSafety::ThreadSafe) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "parallel machine pipeline requires analysis '" +
                                Position->second.Info.Name + "' to be marked thread-safe");
    }
    Worklist.insert(Worklist.end(), Position->second.Info.Dependencies.begin(),
                    Position->second.Info.Dependencies.end());
  }
  return {};
}

Expected<bool> MachineAnalysisManager::requestAction(std::string_view Action,
                                                     const MachineFunction &Function,
                                                     std::uint64_t Cost) {
  if (Action.empty() || Cost == 0)
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "machine action requires a name and nonzero cost");
  if (Instrumentation == nullptr)
    return true;
  return Instrumentation->requestAction(Action, Function, Cost);
}

Expected<void> CompilationUnitAnalysisManager::registerAnalysisImpl(
    CompilationUnitAnalysisRegistration Registration, ErasedRunner Runner,
    ErasedInvalidator Invalidator) {
  if (Registration.ID == nullptr || Registration.Name.empty() || !Runner || !Invalidator) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "compilation-unit analysis registration is incomplete");
  }
  if (hasDuplicateAnalysisIDs(Registration.Dependencies)) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "compilation-unit analysis dependencies must be non-null and unique");
  }
  if (std::ranges::find(Registration.Dependencies, Registration.ID) !=
      Registration.Dependencies.end()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "compilation-unit analysis cannot directly depend on itself");
  }
  if (Registry.contains(Registration.ID)) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "compilation-unit analysis ID is already registered");
  }
  if (std::ranges::any_of(Registry, [&Registration](const auto &Entry) {
        return Entry.second.Info.Name == Registration.Name;
      })) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "compilation-unit analysis name is already registered");
  }
  const AnalysisID RegistrationID = Registration.ID;
  Registry.emplace(RegistrationID, RegisteredAnalysis{std::move(Registration), std::move(Runner),
                                                      std::move(Invalidator)});
  return {};
}

bool CompilationUnitAnalysisManager::isRegistered(AnalysisID ID) const noexcept {
  return ID != nullptr && Registry.contains(ID);
}

bool CompilationUnitAnalysisManager::hasCached(const CompilationUnit &Unit,
                                               AnalysisID ID) const noexcept {
  const auto UnitPosition = Cache.find(const_cast<CompilationUnit *>(&Unit));
  if (UnitPosition == Cache.end())
    return false;
  const auto AnalysisPosition = UnitPosition->second.find(ID);
  return AnalysisPosition != UnitPosition->second.end() && AnalysisPosition->second.Value;
}

Expected<void>
CompilationUnitAnalysisManager::materializeDependencies(const RegisteredAnalysis &Registration,
                                                        CompilationUnit &Unit,
                                                        const TargetDescription &Target) {
  for (const AnalysisID Dependency : Registration.Info.Dependencies) {
    if (!Registry.contains(Dependency)) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "compilation-unit analysis depends on an unregistered analysis");
    }
    auto Result = getResultImpl(Dependency, Unit, Target);
    if (!Result)
      return std::unexpected(Result.error());
  }
  return {};
}

Expected<void *> CompilationUnitAnalysisManager::getResultImpl(AnalysisID ID, CompilationUnit &Unit,
                                                               const TargetDescription &Target) {
  if (ID == nullptr)
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "compilation-unit analysis ID cannot be null");
  auto &UnitCache = Cache[&Unit];
  if (const auto Cached = UnitCache.find(ID); Cached != UnitCache.end() && Cached->second.Value) {
    const auto Registration = Registry.find(ID);
    const std::string_view Name = Registration == Registry.end()
                                      ? std::string_view{"unknown-compilation-unit-analysis"}
                                      : std::string_view{Registration->second.Info.Name};
    if (Instrumentation != nullptr)
      Instrumentation->afterUnitAnalysis(Name, Unit, true, 0);
    return Cached->second.Value.get();
  }

  const auto RegistrationPosition = Registry.find(ID);
  if (RegistrationPosition == Registry.end()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "compilation-unit analysis ID is not registered");
  }
  const RegisteredAnalysis &Registration = RegistrationPosition->second;
  auto &Stack = ActiveComputations[&Unit];
  if (std::ranges::find(Stack, ID) != Stack.end()) {
    Error Failure(ErrorCode::InvalidArgument,
                  "compilation-unit analysis dependency cycle reaches '" + Registration.Info.Name +
                      "'");
    if (Instrumentation != nullptr)
      Instrumentation->unitAnalysisFailure(Registration.Info.Name, Unit, Failure, 0);
    return std::unexpected(std::move(Failure));
  }

  if (Instrumentation != nullptr)
    Instrumentation->beforeUnitAnalysis(Registration.Info.Name, Unit);
  const Clock::time_point Start = Clock::now();
  Stack.push_back(ID);
  auto Dependencies = materializeDependencies(Registration, Unit, Target);
  if (!Dependencies) {
    Stack.pop_back();
    if (Stack.empty())
      ActiveComputations.erase(&Unit);
    Error Failure = prefixedError(Dependencies.error(), "compilation-unit analysis '" +
                                                            Registration.Info.Name + "' failed: ");
    if (Instrumentation != nullptr) {
      Instrumentation->unitAnalysisFailure(Registration.Info.Name, Unit, Failure,
                                           elapsedNanoseconds(Start));
    }
    return std::unexpected(std::move(Failure));
  }

  auto Computed = Registration.Runner(Unit, Target, *this);
  Stack.pop_back();
  if (Stack.empty())
    ActiveComputations.erase(&Unit);
  const std::uint64_t Duration = elapsedNanoseconds(Start);
  if (!Computed) {
    Error Failure = prefixedError(Computed.error(), "compilation-unit analysis '" +
                                                        Registration.Info.Name + "' failed: ");
    if (Instrumentation != nullptr)
      Instrumentation->unitAnalysisFailure(Registration.Info.Name, Unit, Failure, Duration);
    return std::unexpected(std::move(Failure));
  }
  if (!*Computed) {
    Error Failure(ErrorCode::InternalError, "compilation-unit analysis '" + Registration.Info.Name +
                                                "' returned a null result");
    if (Instrumentation != nullptr)
      Instrumentation->unitAnalysisFailure(Registration.Info.Name, Unit, Failure, Duration);
    return std::unexpected(std::move(Failure));
  }

  void *Result = Computed->get();
  UnitCache.emplace(ID, CacheEntry{std::move(*Computed), ID});
  if (Instrumentation != nullptr)
    Instrumentation->afterUnitAnalysis(Registration.Info.Name, Unit, false, Duration);
  return Result;
}

void CompilationUnitAnalysisManager::invalidate(CompilationUnit &Unit,
                                                const PreservedAnalyses &Preserved,
                                                MachinePassChanges Changes) noexcept {
  const auto UnitPosition = Cache.find(&Unit);
  if (UnitPosition == Cache.end() || Preserved.preservesAll() || Changes.empty())
    return;

  std::set<AnalysisID> Invalid;
  for (auto &[ID, Entry] : UnitPosition->second) {
    if (Preserved.preserves(ID))
      continue;
    const auto Registration = Registry.find(ID);
    if (Registration == Registry.end() ||
        Registration->second.Info.InvalidatedBy.intersects(Changes)) {
      const bool ShouldInvalidate =
          Registration == Registry.end() || !Entry.Value || !Registration->second.Invalidator ||
          Registration->second.Invalidator(Entry.Value.get(), Unit, Preserved);
      if (ShouldInvalidate)
        Invalid.insert(ID);
    }
  }

  bool Added = true;
  while (Added) {
    Added = false;
    for (const auto &[ID, Entry] : UnitPosition->second) {
      (void)Entry;
      if (Invalid.contains(ID) || Preserved.preserves(ID))
        continue;
      const auto Registration = Registry.find(ID);
      if (Registration != Registry.end() &&
          std::ranges::any_of(
              Registration->second.Info.Dependencies,
              [&Invalid](AnalysisID Dependency) { return Invalid.contains(Dependency); })) {
        Added = Invalid.insert(ID).second || Added;
      }
    }
  }
  for (const AnalysisID InvalidID : Invalid)
    UnitPosition->second.erase(InvalidID);
  if (UnitPosition->second.empty())
    Cache.erase(UnitPosition);
}

void CompilationUnitAnalysisManager::clear(CompilationUnit &Unit) noexcept {
  Cache.erase(&Unit);
  ActiveComputations.erase(&Unit);
}

void CompilationUnitAnalysisManager::clearAll() noexcept {
  Cache.clear();
  ActiveComputations.clear();
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

Expected<bool> MachinePassInstrumentation::requestAction(std::string_view Action,
                                                         const MachineFunction &Function,
                                                         std::uint64_t Cost) {
  std::scoped_lock ActionLock(ActionMutex);
  ActionFilter CurrentFilter;
  {
    std::scoped_lock Lock(Mutex);
    CurrentFilter = Filter;
  }
  if (CurrentFilter && !CurrentFilter(Action, Function, Cost))
    return false;

  std::scoped_lock Lock(Mutex);
  if (!PerFunctionFuel)
    return true;
  std::uint64_t &Consumed = ConsumedFuel[Function.stableID().value()];
  if (Consumed > *PerFunctionFuel || Cost > *PerFunctionFuel - Consumed)
    return false;
  Consumed += Cost;
  return true;
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

void MachinePassInstrumentation::beforeUnitAnalysis(std::string_view Name,
                                                    const CompilationUnit &Unit) const {
  std::vector<BeforeUnitAnalysisCallback> Callbacks;
  {
    std::scoped_lock Lock(Mutex);
    Callbacks = BeforeUnitAnalysis;
  }
  for (const BeforeUnitAnalysisCallback &Callback : Callbacks)
    Callback(Name, Unit);
}

void MachinePassInstrumentation::afterUnitAnalysis(std::string_view Name,
                                                   const CompilationUnit &Unit, bool CacheHit,
                                                   std::uint64_t Nanoseconds) const {
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

void MachinePassInstrumentation::unitAnalysisFailure(std::string_view Name,
                                                     const CompilationUnit &Unit,
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

void MachinePassInstrumentation::clearStatistics() {
  std::scoped_lock Lock(Mutex);
  Statistics.clear();
  ConsumedFuel.clear();
}

Expected<void> MachinePassManager::validateContract(const MachinePassContract &Contract) const {
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

Expected<void> MachinePassManager::addPass(std::unique_ptr<MachineFunctionPass> Pass) {
  if (!Pass)
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "machine pass manager cannot add a null pass");
  auto Validation = validateContract(Pass->contract());
  if (!Validation)
    return std::unexpected(Validation.error());
  Passes.push_back(std::move(Pass));
  return {};
}

Expected<void> MachinePassManager::run(MachineFunction &Function, const TargetDescription &Target,
                                       MachineAnalysisManager &Analyses,
                                       MachinePassInstrumentation *Instrumentation) const {
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
        Error Failure = prefixedError(Verification.error(), "pass '" + std::string(Name) +
                                                                "' received invalid MachineIR: ");
        if (Instrumentation != nullptr)
          Instrumentation->failure(Name, Function, Failure);
        Analyses.setInstrumentation(nullptr);
        return std::unexpected(std::move(Failure));
      }
    }

    for (const AnalysisID Required : Contract.RequiredAnalyses) {
      auto Result = Analyses.getResultImpl(Required, Function, Target);
      if (!Result) {
        Error Failure =
            prefixedError(Result.error(), "pass '" + std::string(Name) +
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
    auto PassResult = Pass->run(Function, Target, Analyses);
    const std::uint64_t Duration = elapsedNanoseconds(Start);
    if (!PassResult) {
      Error Failure =
          prefixedError(PassResult.error(), "pass '" + std::string(Name) + "' failed: ");
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
          prefixedError(Transition.error(), "pass '" + std::string(Name) +
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
        Error Failure = prefixedError(Verification.error(),
                                      "pass '" + std::string(Name) + "' left invalid MachineIR: ");
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

Expected<void> MachinePassManager::runFunctions(CompilationUnit &Unit,
                                                const TargetDescription &Target,
                                                const MachineAnalysisManager &AnalysisPrototype,
                                                std::size_t ThreadCount,
                                                MachinePassInstrumentation *Instrumentation) const {
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
  std::vector<std::unique_ptr<MachineAnalysisManager>> Managers;
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
      auto Result = run(*Function, Target, *Managers[Index], Instrumentation);
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
      return std::unexpected(prefixedError(
          *Failure, "function '" + std::string(Unit.functionAt(Index)->name()) + "' failed: "));
    }
  }
  return {};
}

Expected<void> CompilationUnitPassManager::addPass(std::unique_ptr<CompilationUnitPass> Pass) {
  if (!Pass) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "compilation-unit pass manager cannot add a null pass");
  }
  const CompilationUnitPassContract &Contract = Pass->contract();
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
  Passes.push_back(std::move(Pass));
  return {};
}

Expected<void> CompilationUnitPassManager::run(CompilationUnit &Unit,
                                               const TargetDescription &Target,
                                               CompilationUnitAnalysisManager &UnitAnalyses,
                                               MachineAnalysisManager &FunctionAnalyses,
                                               MachinePassInstrumentation *Instrumentation) const {
  UnitAnalyses.setInstrumentation(Instrumentation);
  FunctionAnalyses.setInstrumentation(Instrumentation);
  for (const std::unique_ptr<CompilationUnitPass> &Pass : Passes) {
    const CompilationUnitPassContract &Contract = Pass->contract();
    const std::string_view Name = Contract.Name;

    if (Options.VerifyBefore) {
      auto Verification = verifyCompilationUnit(Unit, Target);
      if (!Verification) {
        Error Failure =
            prefixedError(Verification.error(), "compilation-unit pass '" + std::string(Name) +
                                                    "' received invalid MachineIR: ");
        if (Instrumentation != nullptr)
          Instrumentation->unitFailure(Name, Unit, Failure);
        UnitAnalyses.setInstrumentation(nullptr);
        FunctionAnalyses.setInstrumentation(nullptr);
        return std::unexpected(std::move(Failure));
      }
    }

    for (const AnalysisID Required : Contract.RequiredAnalyses) {
      auto Result = UnitAnalyses.getResultImpl(Required, Unit, Target);
      if (!Result) {
        Error Failure =
            prefixedError(Result.error(), "compilation-unit pass '" + std::string(Name) +
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
      Error Failure = prefixedError(Result.error(),
                                    "compilation-unit pass '" + std::string(Name) + "' failed: ");
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
            prefixedError(Verification.error(), "compilation-unit pass '" + std::string(Name) +
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
