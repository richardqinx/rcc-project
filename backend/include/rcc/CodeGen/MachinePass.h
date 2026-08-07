// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINEPASS_H
#define RCC_CODEGEN_MACHINEPASS_H

#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/MachineFunctionProperties.h"
#include "rcc/Support/Error.h"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rcc {

class CompilationUnit;
class MachineFunction;
class MachineVerifier;
class TargetDescription;

using AnalysisID = const void *;

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

struct MachineAnalysisRegistration final {
  AnalysisID ID = nullptr;
  std::string Name;
  std::vector<AnalysisID> Dependencies;
  MachinePassChanges InvalidatedBy = MachinePassChanges::all();
  PassThreadSafety ThreadSafety = PassThreadSafety::NotThreadSafe;
};

struct CompilationUnitAnalysisRegistration final {
  AnalysisID ID = nullptr;
  std::string Name;
  std::vector<AnalysisID> Dependencies;
  MachinePassChanges InvalidatedBy = MachinePassChanges::all();
};

class MachinePassInstrumentation;

class MachineAnalysisManager final {
public:
  MachineAnalysisManager() = default;
  MachineAnalysisManager(const MachineAnalysisManager &) = delete;
  MachineAnalysisManager &operator=(const MachineAnalysisManager &) = delete;
  MachineAnalysisManager(MachineAnalysisManager &&) = delete;
  MachineAnalysisManager &operator=(MachineAnalysisManager &&) = delete;
  ~MachineAnalysisManager() = default;

  template <typename AnalysisT> [[nodiscard]] static AnalysisID id() noexcept {
    return PreservedAnalyses::analysisID<AnalysisT>();
  }

  template <typename AnalysisT>
  [[nodiscard]] Expected<void> registerAnalysis(MachineAnalysisRegistration Registration = {}) {
    using ResultT = typename AnalysisT::Result;
    if (Registration.ID == nullptr)
      Registration.ID = id<AnalysisT>();
    if (Registration.Name.empty()) {
      if constexpr (requires {
                      { AnalysisT::name() } -> std::convertible_to<std::string_view>;
                    }) {
        Registration.Name = std::string(AnalysisT::name());
      } else {
        Registration.Name = "unnamed-machine-analysis";
      }
    }

    ErasedRunner Runner = [](MachineFunction &Function, const TargetDescription &Target,
                             MachineAnalysisManager &Manager) -> Expected<std::shared_ptr<void>> {
      auto Computed = AnalysisT::run(Function, Target, Manager);
      if (!Computed)
        return std::unexpected(Computed.error());
      return std::shared_ptr<void>(new ResultT(std::move(*Computed)),
                                   [](void *Value) { delete static_cast<ResultT *>(Value); });
    };
    ErasedInvalidator Invalidator = [](void *Value, const MachineFunction &Function,
                                       const PreservedAnalyses &Preserved) -> bool {
      if constexpr (requires(ResultT &Result, const MachineFunction &InputFunction,
                             const PreservedAnalyses &Analyses) {
                      { Result.invalidate(InputFunction, Analyses) } -> std::convertible_to<bool>;
                    }) {
        return static_cast<ResultT *>(Value)->invalidate(Function, Preserved);
      }
      return true;
    };
    return registerAnalysisImpl(std::move(Registration), std::move(Runner), std::move(Invalidator));
  }

  template <typename AnalysisT>
  [[nodiscard]] Expected<typename AnalysisT::Result *> getResult(MachineFunction &Function,
                                                                 const TargetDescription &Target) {
    const AnalysisID Key = id<AnalysisT>();
    if (!isRegistered(Key)) {
      auto Registered = registerAnalysis<AnalysisT>();
      if (!Registered)
        return std::unexpected(Registered.error());
    }
    auto Result = getResultImpl(Key, Function, Target);
    if (!Result)
      return std::unexpected(Result.error());
    return static_cast<typename AnalysisT::Result *>(*Result);
  }

  void invalidate(MachineFunction &Function, const PreservedAnalyses &Preserved,
                  MachinePassChanges Changes) noexcept;
  void invalidate(MachineFunction &Function, const PreservedAnalyses &Preserved) noexcept {
    invalidate(Function, Preserved, MachinePassChanges::all());
  }
  [[nodiscard]] bool hasCached(const MachineFunction &Function, AnalysisID ID) const noexcept;
  [[nodiscard]] bool isRegistered(AnalysisID ID) const noexcept;
  [[nodiscard]] std::size_t cachedResultCount(const MachineFunction &Function) const noexcept;
  void clear(MachineFunction &Function) noexcept;
  void clearAll() noexcept;

  [[nodiscard]] Expected<std::unique_ptr<MachineAnalysisManager>> forkEmpty() const;
  [[nodiscard]] Expected<bool>
  requestAction(std::string_view Action, const MachineFunction &Function, std::uint64_t Cost = 1);

private:
  using ErasedRunner = std::function<Expected<std::shared_ptr<void>>(
      MachineFunction &, const TargetDescription &, MachineAnalysisManager &)>;
  using ErasedInvalidator =
      std::function<bool(void *, const MachineFunction &, const PreservedAnalyses &)>;

  struct RegisteredAnalysis final {
    MachineAnalysisRegistration Info;
    ErasedRunner Runner;
    ErasedInvalidator Invalidator;
  };

  struct CacheEntry final {
    std::shared_ptr<void> Value;
    AnalysisID ID = nullptr;
  };

  [[nodiscard]] Expected<void> registerAnalysisImpl(MachineAnalysisRegistration Registration,
                                                    ErasedRunner Runner,
                                                    ErasedInvalidator Invalidator);
  [[nodiscard]] Expected<void *> getResultImpl(AnalysisID ID, MachineFunction &Function,
                                               const TargetDescription &Target);
  [[nodiscard]] Expected<void> materializeDependencies(const RegisteredAnalysis &Registration,
                                                       MachineFunction &Function,
                                                       const TargetDescription &Target);
  [[nodiscard]] Expected<void>
  validateParallelRequirements(std::span<const AnalysisID> RequiredAnalyses) const;
  void setInstrumentation(MachinePassInstrumentation *NewInstrumentation) noexcept {
    Instrumentation = NewInstrumentation;
  }

  std::unordered_map<AnalysisID, RegisteredAnalysis> Registry;
  std::unordered_map<MachineFunction *, std::unordered_map<AnalysisID, CacheEntry>> Cache;
  std::unordered_map<MachineFunction *, std::vector<AnalysisID>> ActiveComputations;
  MachinePassInstrumentation *Instrumentation = nullptr;

  friend class MachinePassManager;
  friend class CompilationUnitPassManager;
  friend Expected<void> registerStage8MachineAnalyses(MachineAnalysisManager &Analyses);
};

class CompilationUnitAnalysisManager final {
public:
  CompilationUnitAnalysisManager() = default;
  CompilationUnitAnalysisManager(const CompilationUnitAnalysisManager &) = delete;
  CompilationUnitAnalysisManager &operator=(const CompilationUnitAnalysisManager &) = delete;
  CompilationUnitAnalysisManager(CompilationUnitAnalysisManager &&) = delete;
  CompilationUnitAnalysisManager &operator=(CompilationUnitAnalysisManager &&) = delete;
  ~CompilationUnitAnalysisManager() = default;

  template <typename AnalysisT> [[nodiscard]] static AnalysisID id() noexcept {
    return PreservedAnalyses::analysisID<AnalysisT>();
  }

  template <typename AnalysisT>
  [[nodiscard]] Expected<void>
  registerAnalysis(CompilationUnitAnalysisRegistration Registration = {}) {
    using ResultT = typename AnalysisT::Result;
    if (Registration.ID == nullptr)
      Registration.ID = id<AnalysisT>();
    if (Registration.Name.empty()) {
      if constexpr (requires {
                      { AnalysisT::name() } -> std::convertible_to<std::string_view>;
                    }) {
        Registration.Name = std::string(AnalysisT::name());
      } else {
        Registration.Name = "unnamed-compilation-unit-analysis";
      }
    }

    ErasedRunner Runner =
        [](CompilationUnit &Unit, const TargetDescription &Target,
           CompilationUnitAnalysisManager &Manager) -> Expected<std::shared_ptr<void>> {
      auto Computed = AnalysisT::run(Unit, Target, Manager);
      if (!Computed)
        return std::unexpected(Computed.error());
      return std::shared_ptr<void>(new ResultT(std::move(*Computed)),
                                   [](void *Value) { delete static_cast<ResultT *>(Value); });
    };
    ErasedInvalidator Invalidator = [](void *Value, const CompilationUnit &Unit,
                                       const PreservedAnalyses &Preserved) -> bool {
      if constexpr (requires(ResultT &Result, const CompilationUnit &InputUnit,
                             const PreservedAnalyses &Analyses) {
                      { Result.invalidate(InputUnit, Analyses) } -> std::convertible_to<bool>;
                    }) {
        return static_cast<ResultT *>(Value)->invalidate(Unit, Preserved);
      }
      return true;
    };
    return registerAnalysisImpl(std::move(Registration), std::move(Runner), std::move(Invalidator));
  }

  template <typename AnalysisT>
  [[nodiscard]] Expected<typename AnalysisT::Result *> getResult(CompilationUnit &Unit,
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

  void invalidate(CompilationUnit &Unit, const PreservedAnalyses &Preserved,
                  MachinePassChanges Changes) noexcept;
  [[nodiscard]] bool hasCached(const CompilationUnit &Unit, AnalysisID ID) const noexcept;
  [[nodiscard]] bool isRegistered(AnalysisID ID) const noexcept;
  void clear(CompilationUnit &Unit) noexcept;
  void clearAll() noexcept;

private:
  using ErasedRunner = std::function<Expected<std::shared_ptr<void>>(
      CompilationUnit &, const TargetDescription &, CompilationUnitAnalysisManager &)>;
  using ErasedInvalidator =
      std::function<bool(void *, const CompilationUnit &, const PreservedAnalyses &)>;

  struct RegisteredAnalysis final {
    CompilationUnitAnalysisRegistration Info;
    ErasedRunner Runner;
    ErasedInvalidator Invalidator;
  };

  struct CacheEntry final {
    std::shared_ptr<void> Value;
    AnalysisID ID = nullptr;
  };

  [[nodiscard]] Expected<void>
  registerAnalysisImpl(CompilationUnitAnalysisRegistration Registration, ErasedRunner Runner,
                       ErasedInvalidator Invalidator);
  [[nodiscard]] Expected<void *> getResultImpl(AnalysisID ID, CompilationUnit &Unit,
                                               const TargetDescription &Target);
  [[nodiscard]] Expected<void> materializeDependencies(const RegisteredAnalysis &Registration,
                                                       CompilationUnit &Unit,
                                                       const TargetDescription &Target);
  void setInstrumentation(MachinePassInstrumentation *NewInstrumentation) noexcept {
    Instrumentation = NewInstrumentation;
  }

  std::unordered_map<AnalysisID, RegisteredAnalysis> Registry;
  std::unordered_map<CompilationUnit *, std::unordered_map<AnalysisID, CacheEntry>> Cache;
  std::unordered_map<CompilationUnit *, std::vector<AnalysisID>> ActiveComputations;
  MachinePassInstrumentation *Instrumentation = nullptr;

  friend class CompilationUnitPassManager;
};

class MachineFunctionPass {
public:
  MachineFunctionPass() = default;
  MachineFunctionPass(const MachineFunctionPass &) = delete;
  MachineFunctionPass &operator=(const MachineFunctionPass &) = delete;
  virtual ~MachineFunctionPass() = default;

  [[nodiscard]] virtual const MachinePassContract &contract() const noexcept = 0;
  [[nodiscard]] virtual Expected<MachinePassResult> run(MachineFunction &Function,
                                                        const TargetDescription &Target,
                                                        MachineAnalysisManager &Analyses) = 0;
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
      CompilationUnitAnalysisManager &UnitAnalyses, MachineAnalysisManager &FunctionAnalyses) = 0;
};

enum class InstrumentationPhase : std::uint8_t {
  Before,
  After,
};

struct InstrumentationStatistic final {
  std::string Name;
  bool IsAnalysis = false;
  std::uint64_t Invocations = 0;
  std::uint64_t CacheHits = 0;
  std::uint64_t Changes = 0;
  std::uint64_t Failures = 0;
  std::uint64_t TotalNanoseconds = 0;
};

class MachinePassInstrumentation final {
public:
  using BeforeCallback = std::function<void(std::string_view, const MachineFunction &)>;
  using AfterCallback =
      std::function<void(std::string_view, const MachineFunction &, const MachinePassResult &)>;
  using FailureCallback =
      std::function<void(std::string_view, const MachineFunction &, const Error &)>;
  using BeforeAnalysisCallback = std::function<void(std::string_view, const MachineFunction &)>;
  using AfterAnalysisCallback =
      std::function<void(std::string_view, const MachineFunction &, bool, std::uint64_t)>;
  using AnalysisFailureCallback =
      std::function<void(std::string_view, const MachineFunction &, const Error &)>;
  using IRPrintCallback =
      std::function<void(InstrumentationPhase, std::string_view, const MachineFunction &)>;
  using ChangeCallback =
      std::function<void(std::string_view, const MachineFunction &, MachinePassChanges, bool)>;
  using ActionFilter =
      std::function<bool(std::string_view, const MachineFunction &, std::uint64_t)>;

  using BeforeUnitCallback = std::function<void(std::string_view, const CompilationUnit &)>;
  using AfterUnitCallback = std::function<void(std::string_view, const CompilationUnit &,
                                               const CompilationUnitPassResult &)>;
  using UnitFailureCallback =
      std::function<void(std::string_view, const CompilationUnit &, const Error &)>;
  using BeforeUnitAnalysisCallback = std::function<void(std::string_view, const CompilationUnit &)>;
  using AfterUnitAnalysisCallback =
      std::function<void(std::string_view, const CompilationUnit &, bool, std::uint64_t)>;
  using UnitAnalysisFailureCallback =
      std::function<void(std::string_view, const CompilationUnit &, const Error &)>;

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
  void clearStatistics();

private:
  struct BufferedEvents final {
    const TargetDescription *Target = nullptr;
    std::vector<std::function<void()>> Events;
  };

  [[nodiscard]] bool bufferFunctionEvent(const MachineFunction &Function,
                                         std::function<void(const MachineFunction &)> Replay) const;
  void beginBuffering(BufferedEvents &Buffer, const TargetDescription &Target) const noexcept;
  void endBuffering() const noexcept;
  void replayBufferedEvents(BufferedEvents &Buffer) const;

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
  [[nodiscard]] Expected<bool> requestAction(std::string_view Action,
                                             const MachineFunction &Function, std::uint64_t Cost);

  void beforeUnit(std::string_view Name, const CompilationUnit &Unit) const;
  void afterUnit(std::string_view Name, const CompilationUnit &Unit,
                 const CompilationUnitPassResult &Result, std::uint64_t Nanoseconds) const;
  void unitFailure(std::string_view Name, const CompilationUnit &Unit, const Error &ErrorValue,
                   std::uint64_t Nanoseconds = 0) const;
  void beforeUnitAnalysis(std::string_view Name, const CompilationUnit &Unit) const;
  void afterUnitAnalysis(std::string_view Name, const CompilationUnit &Unit, bool CacheHit,
                         std::uint64_t Nanoseconds) const;
  void unitAnalysisFailure(std::string_view Name, const CompilationUnit &Unit,
                           const Error &ErrorValue, std::uint64_t Nanoseconds) const;

  [[nodiscard]] InstrumentationStatistic &statistic(std::string_view Name, bool IsAnalysis) const;

  mutable std::mutex Mutex;
  mutable std::mutex ActionMutex;
  std::vector<BeforeCallback> Before;
  std::vector<AfterCallback> After;
  std::vector<FailureCallback> Failure;
  std::vector<BeforeAnalysisCallback> BeforeAnalysis;
  std::vector<AfterAnalysisCallback> AfterAnalysis;
  std::vector<AnalysisFailureCallback> AnalysisFailure;
  std::vector<IRPrintCallback> IRPrint;
  std::vector<ChangeCallback> Change;
  ActionFilter Filter;
  std::optional<std::uint64_t> PerFunctionFuel;
  std::unordered_map<std::uint64_t, std::uint64_t> ConsumedFuel;

  std::vector<BeforeUnitCallback> BeforeUnit;
  std::vector<AfterUnitCallback> AfterUnit;
  std::vector<UnitFailureCallback> UnitFailure;
  std::vector<BeforeUnitAnalysisCallback> BeforeUnitAnalysis;
  std::vector<AfterUnitAnalysisCallback> AfterUnitAnalysis;
  std::vector<UnitAnalysisFailureCallback> UnitAnalysisFailure;
  mutable std::unordered_map<std::string, InstrumentationStatistic> Statistics;

  static thread_local BufferedEvents *ActiveBuffer;

  friend class CompilationUnitAnalysisManager;
  friend class CompilationUnitPassManager;
  friend class MachineAnalysisManager;
  friend class MachinePassManager;
};

struct MachinePassManagerOptions final {
  bool VerifyBefore = true;
  bool VerifyEach = true;
  bool DetectUnreportedChanges = false;
};

class MachinePassManager final {
public:
  explicit MachinePassManager(MachinePassManagerOptions Options = {}) : Options(Options) {}

  MachinePassManager(const MachinePassManager &) = delete;
  MachinePassManager &operator=(const MachinePassManager &) = delete;

  [[nodiscard]] Expected<void> addPass(std::unique_ptr<MachineFunctionPass> Pass);
  [[nodiscard]] Expected<void> run(MachineFunction &Function, const TargetDescription &Target,
                                   MachineAnalysisManager &Analyses,
                                   MachinePassInstrumentation *Instrumentation = nullptr) const;
  [[nodiscard]] Expected<void>
  runFunctions(CompilationUnit &Unit, const TargetDescription &Target,
               const MachineAnalysisManager &AnalysisPrototype, std::size_t ThreadCount,
               MachinePassInstrumentation *Instrumentation = nullptr) const;

  [[nodiscard]] std::size_t size() const noexcept {
    return Passes.size();
  }

private:
  [[nodiscard]] Expected<void> validateContract(const MachinePassContract &Contract) const;

  MachinePassManagerOptions Options;
  std::vector<std::unique_ptr<MachineFunctionPass>> Passes;
};

struct CompilationUnitPassManagerOptions final {
  bool VerifyBefore = true;
  bool VerifyEach = true;
};

class CompilationUnitPassManager final {
public:
  explicit CompilationUnitPassManager(CompilationUnitPassManagerOptions Options = {})
      : Options(Options) {}

  CompilationUnitPassManager(const CompilationUnitPassManager &) = delete;
  CompilationUnitPassManager &operator=(const CompilationUnitPassManager &) = delete;

  [[nodiscard]] Expected<void> addPass(std::unique_ptr<CompilationUnitPass> Pass);
  [[nodiscard]] Expected<void> run(CompilationUnit &Unit, const TargetDescription &Target,
                                   CompilationUnitAnalysisManager &UnitAnalyses,
                                   MachineAnalysisManager &FunctionAnalyses,
                                   MachinePassInstrumentation *Instrumentation = nullptr) const;

private:
  CompilationUnitPassManagerOptions Options;
  std::vector<std::unique_ptr<CompilationUnitPass>> Passes;
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINEPASS_H
