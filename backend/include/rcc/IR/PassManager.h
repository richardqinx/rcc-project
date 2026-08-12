// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_IR_PASSMANAGER_H
#define RCC_IR_PASSMANAGER_H

#include "rcc/Support/Error.h"

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rcc {

class TargetDescription;

using AnalysisID = const void *;

class PreservedAnalyses final {
public:
  [[nodiscard]] static PreservedAnalyses all() noexcept {
    PreservedAnalyses Result;
    Result.PreserveAll = true;
    return Result;
  }

  [[nodiscard]] static PreservedAnalyses none() noexcept {
    return {};
  }

  void preserve(AnalysisID ID) {
    if (ID != nullptr)
      Preserved.insert(ID);
  }

  template <typename AnalysisT> void preserve() {
    preserve(analysisID<AnalysisT>());
  }

  [[nodiscard]] bool preserves(AnalysisID ID) const noexcept {
    return PreserveAll || (ID != nullptr && Preserved.contains(ID));
  }

  [[nodiscard]] bool preservesAll() const noexcept {
    return PreserveAll;
  }

  template <typename AnalysisT> [[nodiscard]] static AnalysisID analysisID() noexcept {
    static const std::uint8_t Token = 0;
    return &Token;
  }

private:
  bool PreserveAll = false;
  std::unordered_set<AnalysisID> Preserved;
};

enum class PassThreadSafety : std::uint8_t {
  NotThreadSafe,
  ThreadSafe,
};

struct PassParameter final {
  std::string Name;
  std::string Description;
  std::optional<std::string> DefaultValue;
  bool Required = false;
};

template <typename IRUnitT, typename InstrumentationT> struct AnalysisManagerInstrumentation {
  static void beforeAnalysis(InstrumentationT *, std::string_view, const IRUnitT &) {}
  static void afterAnalysis(InstrumentationT *, std::string_view, const IRUnitT &, bool,
                            std::uint64_t) {}
  static void analysisFailure(InstrumentationT *, std::string_view, const IRUnitT &, const Error &,
                              std::uint64_t) {}

  template <typename InstructionT>
  [[nodiscard]] static Expected<bool> requestAction(InstrumentationT *, std::string_view,
                                                    std::string_view, const IRUnitT &, InstructionT,
                                                    std::uint64_t) {
    return true;
  }
};

template <typename ChangeT> struct AnalysisRegistration final {
  AnalysisID ID = nullptr;
  std::string Name;
  std::vector<AnalysisID> Dependencies;
  ChangeT InvalidatedBy = ChangeT::all();
  PassThreadSafety ThreadSafety = PassThreadSafety::NotThreadSafe;
};

template <typename IRUnitT, typename PassT, typename OptionsT> struct PassManagerCallbacks;

template <typename IRUnitT, typename PassT, typename OptionsT> class PassManager {
public:
  using PassList = std::vector<std::unique_ptr<PassT>>;
  using Callbacks = PassManagerCallbacks<IRUnitT, PassT, OptionsT>;

  explicit PassManager(OptionsT Options = {}) : Options(Options) {}
  PassManager(const PassManager &) = delete;
  PassManager &operator=(const PassManager &) = delete;
  PassManager(PassManager &&) = default;
  PassManager &operator=(PassManager &&) = default;
  ~PassManager() = default;

  [[nodiscard]] Expected<void> addPass(std::unique_ptr<PassT> Pass) {
    if (!Pass)
      return makeUnexpected(ErrorCode::InvalidArgument, "pass manager cannot add a null pass");
    auto Validation = Callbacks::validate(*Pass);
    if (!Validation)
      return std::unexpected(Validation.error());
    Passes.push_back(std::move(Pass));
    return {};
  }

  template <typename... Args>
  [[nodiscard]] Expected<void> run(IRUnitT &Unit, Args &&...Arguments) const {
    return Callbacks::run(Passes, Options, Unit, std::forward<Args>(Arguments)...);
  }

  template <typename... Args>
  [[nodiscard]] Expected<void> runFunctions(Args &&...Arguments) const
    requires requires(const PassList &InputPasses, const OptionsT &InputOptions,
                      Args &&...InputArguments) {
      Callbacks::runFunctions(InputPasses, InputOptions, std::forward<Args>(InputArguments)...);
    }
  {
    return Callbacks::runFunctions(Passes, Options, std::forward<Args>(Arguments)...);
  }

  [[nodiscard]] std::size_t size() const noexcept {
    return Passes.size();
  }

  [[nodiscard]] PassList takePasses() && noexcept {
    return std::move(Passes);
  }

private:
  PassList Passes;
  OptionsT Options;
};

template <typename IRUnitT, typename RegistrationT, typename ChangeT, typename InstrumentationT>
class AnalysisManager {
public:
  using Self = AnalysisManager<IRUnitT, RegistrationT, ChangeT, InstrumentationT>;

  AnalysisManager() = default;
  AnalysisManager(const AnalysisManager &) = delete;
  AnalysisManager &operator=(const AnalysisManager &) = delete;
  AnalysisManager(AnalysisManager &&) = delete;
  AnalysisManager &operator=(AnalysisManager &&) = delete;
  ~AnalysisManager() = default;

  template <typename AnalysisT> [[nodiscard]] static AnalysisID id() noexcept {
    return PreservedAnalyses::analysisID<AnalysisT>();
  }

  template <typename AnalysisT>
  [[nodiscard]] Expected<void> registerAnalysis(RegistrationT Registration = {}) {
    using ResultT = typename AnalysisT::Result;
    if (Registration.ID == nullptr)
      Registration.ID = id<AnalysisT>();
    if (Registration.Name.empty()) {
      if constexpr (requires {
                      { AnalysisT::name() } -> std::convertible_to<std::string_view>;
                    })
        Registration.Name = std::string(AnalysisT::name());
      else
        Registration.Name = "unnamed-analysis";
    }

    ErasedRunner Runner = [](IRUnitT &Unit, const TargetDescription &Target,
                             Self &Manager) -> Expected<std::shared_ptr<void>> {
      auto Computed = AnalysisT::run(Unit, Target, Manager);
      if (!Computed)
        return std::unexpected(Computed.error());
      return std::shared_ptr<void>(new ResultT(std::move(*Computed)),
                                   [](void *Value) { delete static_cast<ResultT *>(Value); });
    };
    ErasedInvalidator Invalidator = [](void *Value, const IRUnitT &Unit,
                                       const PreservedAnalyses &Preserved) -> bool {
      if constexpr (requires(ResultT &Result, const IRUnitT &InputUnit,
                             const PreservedAnalyses &Analyses) {
                      { Result.invalidate(InputUnit, Analyses) } -> std::convertible_to<bool>;
                    })
        return static_cast<ResultT *>(Value)->invalidate(Unit, Preserved);
      return true;
    };
    return registerAnalysisImpl(std::move(Registration), std::move(Runner), std::move(Invalidator));
  }

  template <typename AnalysisT>
  [[nodiscard]] Expected<typename AnalysisT::Result *> getResult(IRUnitT &Unit,
                                                                 const TargetDescription &Target) {
    const AnalysisID Key = id<AnalysisT>();
    if (!isRegistered(Key)) {
      auto Registered = registerAnalysis<AnalysisT>();
      if (!Registered)
        return std::unexpected(Registered.error());
    }
    auto Result = getResultImpl(Key, Unit, Target);
    if (!Result)
      return std::unexpected(Result.error());
    return static_cast<typename AnalysisT::Result *>(*Result);
  }

  [[nodiscard]] Expected<void *> getResultByID(AnalysisID ID, IRUnitT &Unit,
                                               const TargetDescription &Target) {
    return getResultImpl(ID, Unit, Target);
  }

  void invalidate(IRUnitT &Unit, const PreservedAnalyses &Preserved, ChangeT Changes) noexcept {
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
                [&Invalid](AnalysisID Dependency) { return Invalid.contains(Dependency); }))
          Added = Invalid.insert(ID).second || Added;
      }
    }

    for (const AnalysisID ID : Invalid)
      UnitPosition->second.erase(ID);
    if (UnitPosition->second.empty())
      Cache.erase(UnitPosition);
  }

  void invalidate(IRUnitT &Unit, const PreservedAnalyses &Preserved) noexcept {
    invalidate(Unit, Preserved, ChangeT::all());
  }

  [[nodiscard]] bool hasCached(const IRUnitT &Unit, AnalysisID ID) const noexcept {
    const auto UnitPosition = Cache.find(const_cast<IRUnitT *>(&Unit));
    if (UnitPosition == Cache.end())
      return false;
    const auto AnalysisPosition = UnitPosition->second.find(ID);
    return AnalysisPosition != UnitPosition->second.end() && AnalysisPosition->second.Value;
  }

  [[nodiscard]] std::size_t cachedResultCount(const IRUnitT &Unit) const noexcept {
    const auto Position = Cache.find(const_cast<IRUnitT *>(&Unit));
    return Position == Cache.end() ? 0 : Position->second.size();
  }

  [[nodiscard]] bool isRegistered(AnalysisID ID) const noexcept {
    return ID != nullptr && Registry.contains(ID);
  }

  void clear(IRUnitT &Unit) noexcept {
    Cache.erase(&Unit);
    ActiveComputations.erase(&Unit);
  }

  void clearAll() noexcept {
    Cache.clear();
    ActiveComputations.clear();
  }

  [[nodiscard]] Expected<std::unique_ptr<Self>> forkEmpty() const {
    if (!ActiveComputations.empty())
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "cannot fork an analysis manager during analysis computation");
    auto Result = std::make_unique<Self>();
    Result->Registry = Registry;
    return Result;
  }

  [[nodiscard]] Expected<void> replaceRegistrations(Self &&Source) {
    if (!Cache.empty() || !ActiveComputations.empty() || !Source.Cache.empty() ||
        !Source.ActiveComputations.empty())
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "analysis registrations can only be replaced on empty managers");
    Registry = std::move(Source.Registry);
    return {};
  }

  [[nodiscard]] Expected<void>
  validateParallelRequirements(std::span<const AnalysisID> RequiredAnalyses) const {
    std::vector<AnalysisID> Worklist(RequiredAnalyses.begin(), RequiredAnalyses.end());
    std::unordered_set<AnalysisID> Visited;
    for (std::size_t Index = 0; Index < Worklist.size(); ++Index) {
      const AnalysisID ID = Worklist[Index];
      if (ID == nullptr || !Visited.insert(ID).second)
        continue;
      const auto Position = Registry.find(ID);
      if (Position == Registry.end())
        return makeUnexpected(ErrorCode::InvalidArgument,
                              "parallel pass requires an unregistered analysis");
      if (Position->second.Info.ThreadSafety != PassThreadSafety::ThreadSafe)
        return makeUnexpected(ErrorCode::InvalidArgument, "parallel pipeline requires analysis '" +
                                                              Position->second.Info.Name +
                                                              "' to be marked thread-safe");
      Worklist.insert(Worklist.end(), Position->second.Info.Dependencies.begin(),
                      Position->second.Info.Dependencies.end());
    }
    return {};
  }

  void setInstrumentation(InstrumentationT *NewInstrumentation) noexcept {
    Instrumentation = NewInstrumentation;
  }

  void setActivePass(std::string Name) {
    ActivePass = std::move(Name);
  }

  template <typename InstructionT>
  [[nodiscard]] Expected<bool> requestAction(std::string_view Rule, const IRUnitT &Unit,
                                             InstructionT Instruction, std::uint64_t Cost = 1) {
    if (Rule.empty() || Cost == 0)
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "pass action requires a name and nonzero cost");
    if (Instrumentation == nullptr)
      return true;
    return AnalysisManagerInstrumentation<IRUnitT, InstrumentationT>::requestAction(
        Instrumentation, ActivePass.empty() ? "unscoped" : ActivePass, Rule, Unit, Instruction,
        Cost);
  }

  [[nodiscard]] Expected<bool> requestAction(std::string_view Rule, const IRUnitT &Unit,
                                             std::uint64_t Cost = 1) {
    if (Rule.empty() || Cost == 0)
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "pass action requires a name and nonzero cost");
    if (Instrumentation == nullptr)
      return true;
    return AnalysisManagerInstrumentation<IRUnitT, InstrumentationT>::requestAction(
        Instrumentation, ActivePass.empty() ? "unscoped" : ActivePass, Rule, Unit, nullptr, Cost);
  }

  [[nodiscard]] const std::string &activePass() const noexcept {
    return ActivePass;
  }

private:
  using ErasedRunner =
      std::function<Expected<std::shared_ptr<void>>(IRUnitT &, const TargetDescription &, Self &)>;
  using ErasedInvalidator = std::function<bool(void *, const IRUnitT &, const PreservedAnalyses &)>;

  struct RegisteredAnalysis final {
    RegistrationT Info;
    ErasedRunner Runner;
    ErasedInvalidator Invalidator;
  };

  struct CacheEntry final {
    std::shared_ptr<void> Value;
    AnalysisID ID = nullptr;
  };

  [[nodiscard]] static bool hasDuplicateAnalysisIDs(std::span<const AnalysisID> IDs) {
    std::unordered_set<AnalysisID> Unique;
    for (const AnalysisID ID : IDs) {
      if (ID == nullptr || !Unique.insert(ID).second)
        return true;
    }
    return false;
  }

  [[nodiscard]] Expected<void> registerAnalysisImpl(RegistrationT Registration, ErasedRunner Runner,
                                                    ErasedInvalidator Invalidator) {
    if (Registration.ID == nullptr || Registration.Name.empty() || !Runner || !Invalidator)
      return makeUnexpected(ErrorCode::InvalidArgument, "analysis registration is incomplete");
    if (hasDuplicateAnalysisIDs(Registration.Dependencies))
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "analysis dependencies must be non-null and unique");
    if (std::ranges::find(Registration.Dependencies, Registration.ID) !=
        Registration.Dependencies.end())
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "analysis cannot directly depend on itself");
    if (Registry.contains(Registration.ID))
      return makeUnexpected(ErrorCode::InvalidArgument, "analysis ID is already registered");
    if (std::ranges::any_of(Registry, [&Registration](const auto &Entry) {
          return Entry.second.Info.Name == Registration.Name;
        }))
      return makeUnexpected(ErrorCode::InvalidArgument, "analysis name is already registered");
    Registry.emplace(Registration.ID, RegisteredAnalysis{std::move(Registration), std::move(Runner),
                                                         std::move(Invalidator)});
    return {};
  }

  [[nodiscard]] Expected<void *> getResultImpl(AnalysisID ID, IRUnitT &Unit,
                                               const TargetDescription &Target) {
    if (ID == nullptr)
      return makeUnexpected(ErrorCode::InvalidArgument, "analysis ID cannot be null");
    auto &UnitCache = Cache[&Unit];
    if (const auto Cached = UnitCache.find(ID); Cached != UnitCache.end() && Cached->second.Value) {
      const auto Registration = Registry.find(ID);
      const std::string_view Name = Registration == Registry.end()
                                        ? std::string_view{"unknown-analysis"}
                                        : std::string_view{Registration->second.Info.Name};
      if (Instrumentation != nullptr)
        AnalysisManagerInstrumentation<IRUnitT, InstrumentationT>::afterAnalysis(
            Instrumentation, Name, Unit, true, 0);
      return Cached->second.Value.get();
    }

    const auto RegistrationPosition = Registry.find(ID);
    if (RegistrationPosition == Registry.end())
      return makeUnexpected(ErrorCode::InvalidArgument, "analysis ID is not registered");
    const RegisteredAnalysis &Registration = RegistrationPosition->second;
    auto &Stack = ActiveComputations[&Unit];
    if (std::ranges::find(Stack, ID) != Stack.end()) {
      Error Failure(ErrorCode::InvalidArgument,
                    "analysis dependency cycle reaches '" + Registration.Info.Name + "'");
      if (Instrumentation != nullptr)
        AnalysisManagerInstrumentation<IRUnitT, InstrumentationT>::analysisFailure(
            Instrumentation, Registration.Info.Name, Unit, Failure, 0);
      return std::unexpected(std::move(Failure));
    }

    if (Instrumentation != nullptr)
      AnalysisManagerInstrumentation<IRUnitT, InstrumentationT>::beforeAnalysis(
          Instrumentation, Registration.Info.Name, Unit);
    const auto Start = std::chrono::steady_clock::now();
    Stack.push_back(ID);
    for (const AnalysisID Dependency : Registration.Info.Dependencies) {
      if (!Registry.contains(Dependency)) {
        Stack.pop_back();
        if (Stack.empty())
          ActiveComputations.erase(&Unit);
        return makeUnexpected(ErrorCode::InvalidArgument,
                              "analysis depends on an unregistered analysis");
      }
      auto DependencyResult = getResultImpl(Dependency, Unit, Target);
      if (!DependencyResult) {
        Stack.pop_back();
        if (Stack.empty())
          ActiveComputations.erase(&Unit);
        Error Failure = DependencyResult.error();
        std::string Message = "analysis '" + Registration.Info.Name + "' failed: ";
        Message.append(Failure.message());
        Failure = Error(Failure.code(), std::move(Message));
        if (Instrumentation != nullptr)
          AnalysisManagerInstrumentation<IRUnitT, InstrumentationT>::analysisFailure(
              Instrumentation, Registration.Info.Name, Unit, Failure, elapsedNanoseconds(Start));
        return std::unexpected(std::move(Failure));
      }
    }

    auto Computed = Registration.Runner(Unit, Target, *this);
    Stack.pop_back();
    if (Stack.empty())
      ActiveComputations.erase(&Unit);
    const std::uint64_t Duration = elapsedNanoseconds(Start);
    if (!Computed) {
      Error Failure = Computed.error();
      std::string Message = "analysis '" + Registration.Info.Name + "' failed: ";
      Message.append(Failure.message());
      Failure = Error(Failure.code(), std::move(Message));
      if (Instrumentation != nullptr)
        AnalysisManagerInstrumentation<IRUnitT, InstrumentationT>::analysisFailure(
            Instrumentation, Registration.Info.Name, Unit, Failure, Duration);
      return std::unexpected(std::move(Failure));
    }
    if (!*Computed) {
      Error Failure(ErrorCode::InternalError,
                    "analysis '" + Registration.Info.Name + "' returned a null result");
      if (Instrumentation != nullptr)
        AnalysisManagerInstrumentation<IRUnitT, InstrumentationT>::analysisFailure(
            Instrumentation, Registration.Info.Name, Unit, Failure, Duration);
      return std::unexpected(std::move(Failure));
    }

    void *Result = Computed->get();
    UnitCache.emplace(ID, CacheEntry{std::move(*Computed), ID});
    if (Instrumentation != nullptr)
      AnalysisManagerInstrumentation<IRUnitT, InstrumentationT>::afterAnalysis(
          Instrumentation, Registration.Info.Name, Unit, false, Duration);
    return Result;
  }

  [[nodiscard]] static std::uint64_t
  elapsedNanoseconds(std::chrono::steady_clock::time_point Start) noexcept {
    const auto Elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - Start);
    return Elapsed.count() <= 0 ? 0 : static_cast<std::uint64_t>(Elapsed.count());
  }

  std::unordered_map<AnalysisID, RegisteredAnalysis> Registry;
  std::unordered_map<IRUnitT *, std::unordered_map<AnalysisID, CacheEntry>> Cache;
  std::unordered_map<IRUnitT *, std::vector<AnalysisID>> ActiveComputations;
  InstrumentationT *Instrumentation = nullptr;
  std::string ActivePass;
};

} // namespace rcc

#endif // RCC_IR_PASSMANAGER_H
