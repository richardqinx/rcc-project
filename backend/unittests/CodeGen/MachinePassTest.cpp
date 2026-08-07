// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachinePass.h"

#include "Target/TestTarget.h"
#include "rcc/CodeGen/CompilationUnit.h"
#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/MachineFunction.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

class TestContext final {
public:
  void expect(bool Condition, std::string_view Message) {
    if (Condition)
      return;
    std::cerr << Message << '\n';
    ++Failures;
  }

  [[nodiscard]] int result() const noexcept {
    return Failures == 0 ? 0 : 1;
  }

private:
  int Failures = 0;
};

[[nodiscard]] rcc::Expected<rcc::TargetDescription> createTarget() {
  std::vector<rcc::InstructionDescriptor> Descriptors;
  std::vector<rcc::ImmediatePredicate> Predicates;
  auto Appended = rcc::appendGenericInstructionInfo(Descriptors, Predicates);
  if (!Appended)
    return std::unexpected(Appended.error());
  auto Instructions = rcc::InstructionSet::create(std::move(Descriptors), std::move(Predicates));
  if (!Instructions)
    return std::unexpected(Instructions.error());
  return rcc::test::createTestTargetDescription(std::move(*Instructions));
}

class BlockCountAnalysis final {
public:
  struct Result final {
    std::size_t Count = 0;
  };

  [[nodiscard]] static constexpr std::string_view name() noexcept {
    return "BlockCount";
  }

  [[nodiscard]] static rcc::Expected<Result> run(rcc::MachineFunction &Function,
                                                 const rcc::TargetDescription &,
                                                 rcc::MachineAnalysisManager &) {
    ++Runs;
    return Result{Function.blockCount()};
  }

  static inline std::atomic<std::uint32_t> Runs = 0;
};

class DependentAnalysis final {
public:
  struct Result final {
    std::size_t Count = 0;
  };

  [[nodiscard]] static constexpr std::string_view name() noexcept {
    return "Dependent";
  }

  [[nodiscard]] static rcc::Expected<Result> run(rcc::MachineFunction &Function,
                                                 const rcc::TargetDescription &Target,
                                                 rcc::MachineAnalysisManager &Analyses) {
    auto Blocks = Analyses.getResult<BlockCountAnalysis>(Function, Target);
    if (!Blocks)
      return std::unexpected(Blocks.error());
    return Result{(*Blocks)->Count};
  }
};

class NonThreadSafeAnalysis final {
public:
  struct Result final {};

  [[nodiscard]] static constexpr std::string_view name() noexcept {
    return "NonThreadSafe";
  }

  [[nodiscard]] static rcc::Expected<Result>
  run(rcc::MachineFunction &, const rcc::TargetDescription &, rcc::MachineAnalysisManager &) {
    return Result{};
  }
};

class FailingAnalysis final {
public:
  struct Result final {};

  [[nodiscard]] static constexpr std::string_view name() noexcept {
    return "Failing";
  }

  [[nodiscard]] static rcc::Expected<Result>
  run(rcc::MachineFunction &, const rcc::TargetDescription &, rcc::MachineAnalysisManager &) {
    return rcc::makeUnexpected(rcc::ErrorCode::VerificationError,
                               "intentional machine analysis failure");
  }
};

class StickyAnalysis final {
public:
  struct Result final {
    [[nodiscard]] bool invalidate(const rcc::MachineFunction &,
                                  const rcc::PreservedAnalyses &) noexcept {
      return false;
    }
  };

  [[nodiscard]] static constexpr std::string_view name() noexcept {
    return "Sticky";
  }
  [[nodiscard]] static rcc::Expected<Result>
  run(rcc::MachineFunction &, const rcc::TargetDescription &, rcc::MachineAnalysisManager &) {
    return Result{};
  }
};

class ExpiringAnalysis final {
public:
  struct Result final {
    [[nodiscard]] bool invalidate(const rcc::MachineFunction &,
                                  const rcc::PreservedAnalyses &) noexcept {
      return true;
    }
  };

  [[nodiscard]] static constexpr std::string_view name() noexcept {
    return "Expiring";
  }
  [[nodiscard]] static rcc::Expected<Result>
  run(rcc::MachineFunction &, const rcc::TargetDescription &, rcc::MachineAnalysisManager &) {
    return Result{};
  }
};

class CycleA final {
public:
  struct Result final {};
  [[nodiscard]] static constexpr std::string_view name() noexcept {
    return "CycleA";
  }
  [[nodiscard]] static rcc::Expected<Result>
  run(rcc::MachineFunction &, const rcc::TargetDescription &, rcc::MachineAnalysisManager &) {
    return Result{};
  }
};

class CycleB final {
public:
  struct Result final {};
  [[nodiscard]] static constexpr std::string_view name() noexcept {
    return "CycleB";
  }
  [[nodiscard]] static rcc::Expected<Result>
  run(rcc::MachineFunction &, const rcc::TargetDescription &, rcc::MachineAnalysisManager &) {
    return Result{};
  }
};

class RequiredAnalysisPass final : public rcc::MachineFunctionPass {
public:
  [[nodiscard]] const rcc::MachinePassContract &contract() const noexcept override {
    static const rcc::MachinePassContract Contract{
        .Name = "RequiredAnalysis",
        .Requires = {},
        .Sets = {},
        .Clears = {},
        .RequiredAnalyses = {rcc::MachineAnalysisManager::id<DependentAnalysis>()},
        .VerificationStage = std::nullopt,
    };
    return Contract;
  }

  [[nodiscard]] rcc::Expected<rcc::MachinePassResult>
  run(rcc::MachineFunction &Function, const rcc::TargetDescription &Target,
      rcc::MachineAnalysisManager &Analyses) override {
    auto Dependent = Analyses.getResult<DependentAnalysis>(Function, Target);
    if (!Dependent)
      return std::unexpected(Dependent.error());
    auto FirstAction = Analyses.requestAction("first", Function);
    auto SecondAction = Analyses.requestAction("second", Function);
    if (!FirstAction || !SecondAction || !*FirstAction || *SecondAction) {
      return rcc::makeUnexpected(rcc::ErrorCode::VerificationError,
                                 "per-function action fuel was not enforced");
    }
    rcc::MachinePassResult Result;
    Result.Preserved = rcc::PreservedAnalyses::all();
    return Result;
  }
};

class UnreportedChangePass final : public rcc::MachineFunctionPass {
public:
  [[nodiscard]] const rcc::MachinePassContract &contract() const noexcept override {
    static const rcc::MachinePassContract Contract{
        .Name = "UnreportedChange",
        .Requires = {},
        .Sets = {},
        .Clears = {},
        .RequiredAnalyses = {},
        .VerificationStage = std::nullopt,
    };
    return Contract;
  }

  [[nodiscard]] rcc::Expected<rcc::MachinePassResult> run(rcc::MachineFunction &Function,
                                                          const rcc::TargetDescription &,
                                                          rcc::MachineAnalysisManager &) override {
    auto Block = Function.createBasicBlock();
    if (!Block)
      return std::unexpected(Block.error());
    auto Appended = Function.appendBlock(**Block);
    if (!Appended)
      return std::unexpected(Appended.error());
    return rcc::MachinePassResult{};
  }
};

class RequiresFailingAnalysisPass final : public rcc::MachineFunctionPass {
public:
  [[nodiscard]] const rcc::MachinePassContract &contract() const noexcept override {
    static const rcc::MachinePassContract Contract{
        .Name = "RequiresFailingAnalysis",
        .Requires = {},
        .Sets = {},
        .Clears = {},
        .RequiredAnalyses = {rcc::MachineAnalysisManager::id<FailingAnalysis>()},
        .VerificationStage = std::nullopt,
    };
    return Contract;
  }

  [[nodiscard]] rcc::Expected<rcc::MachinePassResult> run(rcc::MachineFunction &,
                                                          const rcc::TargetDescription &,
                                                          rcc::MachineAnalysisManager &) override {
    return rcc::MachinePassResult{};
  }
};

class MutatingFailurePass final : public rcc::MachineFunctionPass {
public:
  [[nodiscard]] const rcc::MachinePassContract &contract() const noexcept override {
    static const rcc::MachinePassContract Contract{
        .Name = "MutatingFailure",
        .Requires = {},
        .Sets = {},
        .Clears = {},
        .RequiredAnalyses = {rcc::MachineAnalysisManager::id<BlockCountAnalysis>()},
        .VerificationStage = std::nullopt,
    };
    return Contract;
  }

  [[nodiscard]] rcc::Expected<rcc::MachinePassResult> run(rcc::MachineFunction &Function,
                                                          const rcc::TargetDescription &,
                                                          rcc::MachineAnalysisManager &) override {
    auto Block = Function.createBasicBlock();
    if (!Block)
      return std::unexpected(Block.error());
    auto Appended = Function.appendBlock(**Block);
    if (!Appended)
      return std::unexpected(Appended.error());
    return rcc::makeUnexpected(rcc::ErrorCode::VerificationError,
                               "intentional mutating pass failure");
  }
};

class ParallelCountingPass final : public rcc::MachineFunctionPass {
public:
  explicit ParallelCountingPass(std::atomic<std::uint32_t> &Count) : Count(&Count) {}

  [[nodiscard]] const rcc::MachinePassContract &contract() const noexcept override {
    static const rcc::MachinePassContract Contract = [] {
      rcc::MachinePassContract Value{
          .Name = "ParallelCounting",
          .Requires = {},
          .Sets = {},
          .Clears = {},
          .RequiredAnalyses = {},
          .VerificationStage = std::nullopt,
      };
      Value.ThreadSafety = rcc::PassThreadSafety::ThreadSafe;
      return Value;
    }();
    return Contract;
  }

  [[nodiscard]] rcc::Expected<rcc::MachinePassResult> run(rcc::MachineFunction &,
                                                          const rcc::TargetDescription &,
                                                          rcc::MachineAnalysisManager &) override {
    Count->fetch_add(1, std::memory_order_relaxed);
    return rcc::MachinePassResult{};
  }

private:
  std::atomic<std::uint32_t> *Count;
};

class InstrumentedParallelPass final : public rcc::MachineFunctionPass {
public:
  InstrumentedParallelPass(std::atomic<std::uint32_t> &Runs, std::atomic<std::uint32_t> &Active,
                           std::atomic<std::uint32_t> &MaximumActive)
      : Runs(&Runs), Active(&Active), MaximumActive(&MaximumActive) {}

  [[nodiscard]] const rcc::MachinePassContract &contract() const noexcept override {
    static const rcc::MachinePassContract Contract = [] {
      rcc::MachinePassContract Value{
          .Name = "InstrumentedParallel",
          .Requires = {},
          .Sets = {},
          .Clears = {},
          .RequiredAnalyses = {},
          .VerificationStage = std::nullopt,
      };
      Value.ThreadSafety = rcc::PassThreadSafety::ThreadSafe;
      Value.DeclaredChanges = {rcc::MachinePassChange::BlockLayout};
      return Value;
    }();
    return Contract;
  }

  [[nodiscard]] rcc::Expected<rcc::MachinePassResult> run(rcc::MachineFunction &Function,
                                                          const rcc::TargetDescription &,
                                                          rcc::MachineAnalysisManager &) override {
    const std::uint32_t Current = Active->fetch_add(1, std::memory_order_acq_rel) + 1U;
    std::uint32_t Maximum = MaximumActive->load(std::memory_order_relaxed);
    while (Maximum < Current &&
           !MaximumActive->compare_exchange_weak(Maximum, Current, std::memory_order_relaxed)) {
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    auto Block = Function.createBasicBlock();
    if (!Block) {
      Active->fetch_sub(1, std::memory_order_acq_rel);
      return std::unexpected(Block.error());
    }
    auto Appended = Function.appendBlock(**Block);
    Active->fetch_sub(1, std::memory_order_acq_rel);
    if (!Appended)
      return std::unexpected(Appended.error());
    Runs->fetch_add(1, std::memory_order_relaxed);
    rcc::MachinePassResult Result;
    Result.Changed = true;
    Result.Changes.add(rcc::MachinePassChange::BlockLayout);
    return Result;
  }

private:
  std::atomic<std::uint32_t> *Runs;
  std::atomic<std::uint32_t> *Active;
  std::atomic<std::uint32_t> *MaximumActive;
};

class RequiresNonThreadSafeAnalysisPass final : public rcc::MachineFunctionPass {
public:
  [[nodiscard]] const rcc::MachinePassContract &contract() const noexcept override {
    static const rcc::MachinePassContract Contract = [] {
      rcc::MachinePassContract Value{
          .Name = "RequiresNonThreadSafe",
          .Requires = {},
          .Sets = {},
          .Clears = {},
          .RequiredAnalyses =
              {
                  rcc::MachineAnalysisManager::id<NonThreadSafeAnalysis>(),
              },
          .VerificationStage = std::nullopt,
      };
      Value.ThreadSafety = rcc::PassThreadSafety::ThreadSafe;
      return Value;
    }();
    return Contract;
  }

  [[nodiscard]] rcc::Expected<rcc::MachinePassResult> run(rcc::MachineFunction &,
                                                          const rcc::TargetDescription &,
                                                          rcc::MachineAnalysisManager &) override {
    return rcc::MachinePassResult{};
  }
};

class UnitFunctionCountAnalysis final {
public:
  struct Result final {
    std::size_t Count = 0;
  };
  [[nodiscard]] static constexpr std::string_view name() noexcept {
    return "UnitFunctionCount";
  }
  [[nodiscard]] static rcc::Expected<Result> run(rcc::CompilationUnit &Unit,
                                                 const rcc::TargetDescription &,
                                                 rcc::CompilationUnitAnalysisManager &) {
    return Result{Unit.functionCount()};
  }
};

class FailingUnitAnalysis final {
public:
  struct Result final {};

  [[nodiscard]] static constexpr std::string_view name() noexcept {
    return "FailingUnit";
  }

  [[nodiscard]] static rcc::Expected<Result> run(rcc::CompilationUnit &,
                                                 const rcc::TargetDescription &,
                                                 rcc::CompilationUnitAnalysisManager &) {
    return rcc::makeUnexpected(rcc::ErrorCode::VerificationError,
                               "intentional compilation-unit analysis failure");
  }
};

class RequiresFailingUnitAnalysisPass final : public rcc::CompilationUnitPass {
public:
  [[nodiscard]] const rcc::CompilationUnitPassContract &contract() const noexcept override {
    static const rcc::CompilationUnitPassContract Contract{
        .Name = "RequiresFailingUnitAnalysis",
        .RequiredAnalyses =
            {
                rcc::CompilationUnitAnalysisManager::id<FailingUnitAnalysis>(),
            },
        .Parameters = {},
        .DeclaredChanges = {},
    };
    return Contract;
  }

  [[nodiscard]] rcc::Expected<rcc::CompilationUnitPassResult>
  run(rcc::CompilationUnit &, const rcc::TargetDescription &, rcc::CompilationUnitAnalysisManager &,
      rcc::MachineAnalysisManager &) override {
    return rcc::CompilationUnitPassResult{};
  }
};

class ObserveUnitPass final : public rcc::CompilationUnitPass {
public:
  [[nodiscard]] const rcc::CompilationUnitPassContract &contract() const noexcept override {
    static const rcc::CompilationUnitPassContract Contract{
        .Name = "ObserveUnit",
        .RequiredAnalyses = {rcc::CompilationUnitAnalysisManager::id<UnitFunctionCountAnalysis>()},
        .Parameters = {},
        .DeclaredChanges = {},
    };
    return Contract;
  }

  [[nodiscard]] rcc::Expected<rcc::CompilationUnitPassResult>
  run(rcc::CompilationUnit &Unit, const rcc::TargetDescription &Target,
      rcc::CompilationUnitAnalysisManager &Analyses, rcc::MachineAnalysisManager &) override {
    auto Count = Analyses.getResult<UnitFunctionCountAnalysis>(Unit, Target);
    if (!Count)
      return std::unexpected(Count.error());
    if ((*Count)->Count != Unit.functionCount()) {
      return rcc::makeUnexpected(rcc::ErrorCode::VerificationError,
                                 "unit analysis returned a stale function count");
    }
    return rcc::CompilationUnitPassResult{};
  }
};

class MutatingUnitFailurePass final : public rcc::CompilationUnitPass {
public:
  [[nodiscard]] const rcc::CompilationUnitPassContract &contract() const noexcept override {
    static const rcc::CompilationUnitPassContract Contract{
        .Name = "MutatingUnitFailure",
        .RequiredAnalyses = {rcc::CompilationUnitAnalysisManager::id<UnitFunctionCountAnalysis>()},
        .Parameters = {},
        .DeclaredChanges = {rcc::MachinePassChange::BlockLayout},
    };
    return Contract;
  }

  [[nodiscard]] rcc::Expected<rcc::CompilationUnitPassResult>
  run(rcc::CompilationUnit &Unit, const rcc::TargetDescription &,
      rcc::CompilationUnitAnalysisManager &, rcc::MachineAnalysisManager &) override {
    auto Created = Unit.createMachineFunction("added-before-failure");
    if (!Created)
      return std::unexpected(Created.error());
    return rcc::makeUnexpected(rcc::ErrorCode::VerificationError,
                               "intentional compilation-unit pass failure");
  }
};

void testAnalysisInfrastructure(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Function =
      rcc::MachineFunction::create("analysis", rcc::MachineFunctionStableID::fromValue(1));
  Context.expect(Function.has_value(), "analysis function creation must succeed");
  if (!Function)
    return;

  rcc::MachineAnalysisManager Analyses;
  Context.expect(Analyses
                     .registerAnalysis<BlockCountAnalysis>({
                         .Name = "BlockCount",
                         .Dependencies = {},
                         .InvalidatedBy = {rcc::MachinePassChange::CFG},
                     })
                     .has_value(),
                 "base analysis registration must succeed");
  Context.expect(Analyses
                     .registerAnalysis<DependentAnalysis>({
                         .Name = "Dependent",
                         .Dependencies = {rcc::MachineAnalysisManager::id<BlockCountAnalysis>()},
                         .InvalidatedBy = {rcc::MachinePassChange::Operands},
                     })
                     .has_value(),
                 "dependent analysis registration must succeed");

  BlockCountAnalysis::Runs = 0;
  auto First = Analyses.getResult<DependentAnalysis>(**Function, Target);
  auto Second = Analyses.getResult<DependentAnalysis>(**Function, Target);
  Context.expect(First && Second && *First == *Second && BlockCountAnalysis::Runs == 1,
                 "analysis dependencies must materialize once and then use the cache");

  Analyses.invalidate(**Function, rcc::PreservedAnalyses::none(),
                      {rcc::MachinePassChange::Registers});
  Context.expect(Analyses.cachedResultCount(**Function) == 2,
                 "unrelated changes must preserve cached analyses precisely");
  Analyses.invalidate(**Function, rcc::PreservedAnalyses::none(), {rcc::MachinePassChange::CFG});
  Context.expect(Analyses.cachedResultCount(**Function) == 0,
                 "invalidating a dependency must invalidate its dependent analysis");

  rcc::MachineAnalysisManager Cyclic;
  Context.expect(Cyclic
                         .registerAnalysis<CycleA>({
                             .Name = "CycleA",
                             .Dependencies = {rcc::MachineAnalysisManager::id<CycleB>()},
                         })
                         .has_value() &&
                     Cyclic
                         .registerAnalysis<CycleB>({
                             .Name = "CycleB",
                             .Dependencies = {rcc::MachineAnalysisManager::id<CycleA>()},
                         })
                         .has_value(),
                 "cyclic analysis registrations must be accepted until requested");
  Context.expect(!Cyclic.getResult<CycleA>(**Function, Target),
                 "analysis dependency cycles must produce a structured error");

  rcc::MachineAnalysisManager CustomInvalidation;
  Context.expect(CustomInvalidation
                         .registerAnalysis<StickyAnalysis>({
                             .Name = "Sticky",
                             .Dependencies = {},
                             .InvalidatedBy = {rcc::MachinePassChange::CFG},
                         })
                         .has_value() &&
                     CustomInvalidation
                         .registerAnalysis<ExpiringAnalysis>({
                             .Name = "Expiring",
                             .Dependencies = {},
                             .InvalidatedBy = {rcc::MachinePassChange::CFG},
                         })
                         .has_value() &&
                     CustomInvalidation.getResult<StickyAnalysis>(**Function, Target) &&
                     CustomInvalidation.getResult<ExpiringAnalysis>(**Function, Target),
                 "custom invalidation analyses must materialize");
  CustomInvalidation.invalidate(**Function, rcc::PreservedAnalyses::none(),
                                {rcc::MachinePassChange::CFG});
  Context.expect(
      CustomInvalidation.hasCached(**Function, rcc::MachineAnalysisManager::id<StickyAnalysis>()) &&
          !CustomInvalidation.hasCached(**Function,
                                        rcc::MachineAnalysisManager::id<ExpiringAnalysis>()),
      "analysis invalidate() must return false to preserve and true to invalidate");
}

void testPassAndInstrumentation(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Function = rcc::MachineFunction::create("pass", rcc::MachineFunctionStableID::fromValue(2));
  Context.expect(Function.has_value(), "pass function creation must succeed");
  if (!Function)
    return;

  rcc::MachineAnalysisManager Analyses;
  Context.expect(
      Analyses
              .registerAnalysis<BlockCountAnalysis>({
                  .Name = "BlockCount",
                  .Dependencies = {},
                  .InvalidatedBy = rcc::MachinePassChanges::all(),
                  .ThreadSafety = rcc::PassThreadSafety::ThreadSafe,
              })
              .has_value() &&
          Analyses
              .registerAnalysis<DependentAnalysis>({
                  .Name = "Dependent",
                  .Dependencies = {rcc::MachineAnalysisManager::id<BlockCountAnalysis>()},
              })
              .has_value(),
      "pass analysis registrations must succeed");

  rcc::MachinePassInstrumentation Instrumentation;
  Instrumentation.setPerFunctionFuel(1);
  std::uint32_t BeforePass = 0;
  std::uint32_t AfterPass = 0;
  std::uint32_t BeforeAnalysis = 0;
  std::uint32_t AfterAnalysis = 0;
  std::uint32_t AnalysisCacheHits = 0;
  std::uint32_t Printed = 0;
  std::uint32_t ChangeEvents = 0;
  std::uint32_t FailureEvents = 0;
  Instrumentation.addBeforeCallback(
      [&BeforePass](std::string_view, const rcc::MachineFunction &) { ++BeforePass; });
  Instrumentation.addAfterCallback([&AfterPass](std::string_view, const rcc::MachineFunction &,
                                                const rcc::MachinePassResult &) { ++AfterPass; });
  Instrumentation.addBeforeAnalysisCallback(
      [&BeforeAnalysis](std::string_view, const rcc::MachineFunction &) { ++BeforeAnalysis; });
  Instrumentation.addAfterAnalysisCallback(
      [&AfterAnalysis, &AnalysisCacheHits](std::string_view, const rcc::MachineFunction &,
                                           bool CacheHit, std::uint64_t) {
        ++AfterAnalysis;
        AnalysisCacheHits += CacheHit ? 1U : 0U;
      });
  Instrumentation.addIRPrintCallback([&Printed](rcc::InstrumentationPhase, std::string_view,
                                                const rcc::MachineFunction &) { ++Printed; });
  Instrumentation.addChangeCallback([&ChangeEvents](std::string_view, const rcc::MachineFunction &,
                                                    rcc::MachinePassChanges Changes,
                                                    bool FingerprintChanged) {
    if (Changes.empty() && !FingerprintChanged)
      ++ChangeEvents;
  });
  Instrumentation.addFailureCallback([&FailureEvents](std::string_view,
                                                      const rcc::MachineFunction &,
                                                      const rcc::Error &) { ++FailureEvents; });

  rcc::MachinePassManager Pipeline;
  Context.expect(Pipeline.addPass(std::make_unique<RequiredAnalysisPass>()).has_value(),
                 "a valid pass must be accepted");
  auto Run = Pipeline.run(**Function, Target, Analyses, &Instrumentation);
  Context.expect(Run.has_value(), "a pass with a materialized required analysis must run");
  Context.expect(BeforePass == 1 && AfterPass == 1 && BeforeAnalysis == 2 && AfterAnalysis == 4 &&
                     AnalysisCacheHits == 2 && Printed == 2 && ChangeEvents == 1,
                 "pass, analysis, cache-hit, IR-print, and change callbacks must all run");
  const auto Statistics = Instrumentation.statistics();
  Context.expect(Statistics.size() == 3,
                 "instrumentation must collect one pass and two analysis statistics");

  auto ChangedFunction =
      rcc::MachineFunction::create("unreported", rcc::MachineFunctionStableID::fromValue(3));
  rcc::MachineAnalysisManager EmptyAnalyses;
  rcc::MachinePassManager Detecting(
      {.VerifyBefore = true, .VerifyEach = true, .DetectUnreportedChanges = true});
  Context.expect(ChangedFunction &&
                     Detecting.addPass(std::make_unique<UnreportedChangePass>()).has_value() &&
                     !Detecting.run(**ChangedFunction, Target, EmptyAnalyses, &Instrumentation) &&
                     FailureEvents == 1,
                 "change detection must reject an unreported MachineIR mutation");

  auto FailedFunction =
      rcc::MachineFunction::create("failed", rcc::MachineFunctionStableID::fromValue(4));
  rcc::MachineAnalysisManager FailureAnalyses;
  Context.expect(FailedFunction && FailureAnalyses
                                       .registerAnalysis<BlockCountAnalysis>({
                                           .Name = "BlockCount",
                                           .Dependencies = {},
                                           .InvalidatedBy = rcc::MachinePassChanges::all(),
                                       })
                                       .has_value(),
                 "mutating failure analysis fixtures must be valid");
  rcc::MachinePassManager FailingPipeline;
  Context.expect(FailedFunction &&
                     FailingPipeline.addPass(std::make_unique<MutatingFailurePass>()).has_value() &&
                     !FailingPipeline.run(**FailedFunction, Target, FailureAnalyses) &&
                     !FailureAnalyses.hasCached(
                         **FailedFunction, rcc::MachineAnalysisManager::id<BlockCountAnalysis>()),
                 "a pass that mutates and fails must not leave stale function analyses cached");

  rcc::MachineAnalysisManager FailingAnalyses;
  Context.expect(FailingAnalyses
                     .registerAnalysis<FailingAnalysis>({
                         .Name = "Failing",
                         .Dependencies = {},
                     })
                     .has_value(),
                 "failing analysis registration must succeed");
  rcc::MachinePassInstrumentation FailureInstrumentation;
  std::uint32_t BeforeFailingAnalysis = 0;
  std::uint32_t FailingAnalysisEvents = 0;
  std::uint32_t RequiredAnalysisPassFailures = 0;
  FailureInstrumentation.addBeforeAnalysisCallback(
      [&BeforeFailingAnalysis](std::string_view, const rcc::MachineFunction &) {
        ++BeforeFailingAnalysis;
      });
  FailureInstrumentation.addAnalysisFailureCallback(
      [&FailingAnalysisEvents](std::string_view, const rcc::MachineFunction &, const rcc::Error &) {
        ++FailingAnalysisEvents;
      });
  FailureInstrumentation.addFailureCallback(
      [&RequiredAnalysisPassFailures](std::string_view, const rcc::MachineFunction &,
                                      const rcc::Error &) { ++RequiredAnalysisPassFailures; });
  rcc::MachinePassManager RequiredAnalysisFailurePipeline;
  Context.expect(
      RequiredAnalysisFailurePipeline.addPass(std::make_unique<RequiresFailingAnalysisPass>())
              .has_value() &&
          !RequiredAnalysisFailurePipeline.run(**Function, Target, FailingAnalyses,
                                               &FailureInstrumentation) &&
          BeforeFailingAnalysis == 1 && FailingAnalysisEvents == 1 &&
          RequiredAnalysisPassFailures == 1,
      "analysis and pass failure instrumentation must report required-analysis failures");
}

void testCompilationUnitAndParallelism(TestContext &Context, const rcc::TargetDescription &Target) {
  rcc::CompilationUnit Unit;
  for (std::size_t Index = 0; Index < 8; ++Index) {
    auto Function = Unit.createMachineFunction("f" + std::to_string(Index));
    Context.expect(Function.has_value(), "parallel test function creation must succeed");
  }

  std::atomic<std::uint32_t> Runs = 0;
  rcc::MachinePassManager Parallel;
  Context.expect(Parallel.addPass(std::make_unique<ParallelCountingPass>(Runs)).has_value(),
                 "thread-safe pass registration must succeed");
  rcc::MachineAnalysisManager Prototype;
  auto ParallelRun = Parallel.runFunctions(Unit, Target, Prototype, 4);
  Context.expect(ParallelRun.has_value() && Runs == Unit.functionCount(),
                 "parallel function execution must visit every function exactly once");

  std::atomic<std::uint32_t> DuplicateRuns = 0;
  rcc::MachinePassManager Repeated;
  Context.expect(
      Repeated.addPass(std::make_unique<ParallelCountingPass>(DuplicateRuns)).has_value() &&
          Repeated.addPass(std::make_unique<ParallelCountingPass>(DuplicateRuns)).has_value() &&
          Repeated.runFunctions(Unit, Target, Prototype, 4).has_value() &&
          DuplicateRuns == Unit.functionCount() * 2,
      "a pipeline must allow the same named pass to run more than once");

  rcc::MachinePassInstrumentation Instrumentation;
  std::vector<std::string> ObservedFunctions;
  std::vector<std::size_t> BeforeBlockCounts;
  std::vector<std::size_t> AfterBlockCounts;
  Instrumentation.addBeforeCallback([&ObservedFunctions, &BeforeBlockCounts](
                                        std::string_view, const rcc::MachineFunction &Function) {
    ObservedFunctions.emplace_back(Function.name());
    BeforeBlockCounts.push_back(Function.blockCount());
  });
  Instrumentation.addAfterCallback([&AfterBlockCounts](std::string_view,
                                                       const rcc::MachineFunction &Function,
                                                       const rcc::MachinePassResult &) {
    AfterBlockCounts.push_back(Function.blockCount());
  });
  std::atomic<std::uint32_t> InstrumentedRuns = 0;
  std::atomic<std::uint32_t> ActiveRuns = 0;
  std::atomic<std::uint32_t> MaximumActiveRuns = 0;
  rcc::MachinePassManager Instrumented;
  Context.expect(
      Instrumented
              .addPass(std::make_unique<InstrumentedParallelPass>(InstrumentedRuns, ActiveRuns,
                                                                  MaximumActiveRuns))
              .has_value() &&
          Instrumented.runFunctions(Unit, Target, Prototype, 4, &Instrumentation).has_value() &&
          InstrumentedRuns == Unit.functionCount() && MaximumActiveRuns > 1,
      "instrumented function pipelines must execute concurrently with a parallel request");
  bool StableCallbackOrder = ObservedFunctions.size() == Unit.functionCount();
  for (std::size_t Index = 0; StableCallbackOrder && Index < ObservedFunctions.size(); ++Index)
    StableCallbackOrder = ObservedFunctions[Index] == "f" + std::to_string(Index);
  Context.expect(StableCallbackOrder,
                 "instrumentation callbacks must commit in stable compilation-unit order");
  Context.expect(
      BeforeBlockCounts.size() == Unit.functionCount() &&
          AfterBlockCounts.size() == Unit.functionCount() &&
          std::ranges::all_of(BeforeBlockCounts, [](std::size_t Count) { return Count == 0; }) &&
          std::ranges::all_of(AfterBlockCounts, [](std::size_t Count) { return Count == 1; }),
      "buffered instrumentation must retain distinct before/after IR snapshots");

  rcc::MachineAnalysisManager UnsafePrototype;
  Context.expect(UnsafePrototype
                     .registerAnalysis<NonThreadSafeAnalysis>({
                         .Name = "NonThreadSafe",
                         .Dependencies = {},
                         .InvalidatedBy = rcc::MachinePassChanges::all(),
                         .ThreadSafety = rcc::PassThreadSafety::NotThreadSafe,
                     })
                     .has_value(),
                 "non-thread-safe analysis registration must be explicit");
  rcc::MachinePassManager UnsafeParallel;
  Context.expect(
      UnsafeParallel.addPass(std::make_unique<RequiresNonThreadSafeAnalysisPass>()).has_value() &&
          !UnsafeParallel.runFunctions(Unit, Target, UnsafePrototype, 4),
      "parallel pipelines must reject a required analysis not marked thread-safe");

  rcc::CompilationUnitAnalysisManager UnitAnalyses;
  Context.expect(UnitAnalyses
                     .registerAnalysis<UnitFunctionCountAnalysis>({
                         .Name = "UnitFunctionCount",
                         .Dependencies = {},
                         .InvalidatedBy = rcc::MachinePassChanges::all(),
                     })
                     .has_value(),
                 "compilation-unit analysis registration must succeed");
  rcc::MachineAnalysisManager FunctionAnalyses;
  rcc::CompilationUnitPassManager UnitPipeline;
  Context.expect(UnitPipeline.addPass(std::make_unique<ObserveUnitPass>()).has_value(),
                 "compilation-unit pass registration must succeed");
  rcc::MachinePassInstrumentation UnitInstrumentation;
  std::uint32_t BeforeUnit = 0;
  std::uint32_t AfterUnit = 0;
  std::uint32_t BeforeUnitAnalysis = 0;
  std::uint32_t AfterUnitAnalysis = 0;
  std::uint32_t UnitAnalysisCacheHits = 0;
  UnitInstrumentation.addBeforeUnitCallback(
      [&BeforeUnit](std::string_view, const rcc::CompilationUnit &) { ++BeforeUnit; });
  UnitInstrumentation.addAfterUnitCallback(
      [&AfterUnit](std::string_view, const rcc::CompilationUnit &,
                   const rcc::CompilationUnitPassResult &) { ++AfterUnit; });
  UnitInstrumentation.addBeforeUnitAnalysisCallback(
      [&BeforeUnitAnalysis](std::string_view, const rcc::CompilationUnit &) {
        ++BeforeUnitAnalysis;
      });
  UnitInstrumentation.addAfterUnitAnalysisCallback(
      [&AfterUnitAnalysis, &UnitAnalysisCacheHits](std::string_view, const rcc::CompilationUnit &,
                                                   bool CacheHit, std::uint64_t) {
        ++AfterUnitAnalysis;
        UnitAnalysisCacheHits += CacheHit ? 1U : 0U;
      });
  Context.expect(
      UnitPipeline.run(Unit, Target, UnitAnalyses, FunctionAnalyses, &UnitInstrumentation)
          .has_value(),
      "compilation-unit pass and analysis must run through their own manager");
  Context.expect(BeforeUnit == 1 && AfterUnit == 1 && BeforeUnitAnalysis == 1 &&
                     AfterUnitAnalysis == 2 && UnitAnalysisCacheHits == 1,
                 "compilation-unit instrumentation must report pass and analysis cache events");

  rcc::CompilationUnitAnalysisManager FailingUnitAnalyses;
  Context.expect(FailingUnitAnalyses
                     .registerAnalysis<FailingUnitAnalysis>({
                         .Name = "FailingUnit",
                         .Dependencies = {},
                     })
                     .has_value(),
                 "failing compilation-unit analysis registration must succeed");
  rcc::CompilationUnitPassManager FailingUnitAnalysisPipeline;
  Context.expect(
      FailingUnitAnalysisPipeline.addPass(std::make_unique<RequiresFailingUnitAnalysisPass>())
          .has_value(),
      "a pass requiring a failing unit analysis must register");
  rcc::MachinePassInstrumentation FailingUnitInstrumentation;
  std::uint32_t BeforeFailingUnitAnalysis = 0;
  std::uint32_t FailingUnitAnalysisEvents = 0;
  std::uint32_t RequiredUnitAnalysisPassFailures = 0;
  FailingUnitInstrumentation.addBeforeUnitAnalysisCallback(
      [&BeforeFailingUnitAnalysis](std::string_view, const rcc::CompilationUnit &) {
        ++BeforeFailingUnitAnalysis;
      });
  FailingUnitInstrumentation.addUnitAnalysisFailureCallback(
      [&FailingUnitAnalysisEvents](std::string_view, const rcc::CompilationUnit &,
                                   const rcc::Error &) { ++FailingUnitAnalysisEvents; });
  FailingUnitInstrumentation.addUnitFailureCallback(
      [&RequiredUnitAnalysisPassFailures](std::string_view, const rcc::CompilationUnit &,
                                          const rcc::Error &) {
        ++RequiredUnitAnalysisPassFailures;
      });
  Context.expect(
      !FailingUnitAnalysisPipeline.run(Unit, Target, FailingUnitAnalyses, FunctionAnalyses,
                                       &FailingUnitInstrumentation) &&
          BeforeFailingUnitAnalysis == 1 && FailingUnitAnalysisEvents == 1 &&
          RequiredUnitAnalysisPassFailures == 1,
      "unit analysis and pass failure instrumentation must report required-analysis failures");

  rcc::MachineFunction *FirstFunction = Unit.functionAt(0);
  Context.expect(
      FirstFunction != nullptr &&
          FunctionAnalyses
              .registerAnalysis<BlockCountAnalysis>({
                  .Name = "BlockCount",
                  .Dependencies = {},
                  .InvalidatedBy = rcc::MachinePassChanges::all(),
              })
              .has_value() &&
          FunctionAnalyses.getResult<BlockCountAnalysis>(*FirstFunction, Target).has_value(),
      "compilation-unit failure cache fixtures must materialize");
  rcc::CompilationUnitPassManager FailingUnitPipeline;
  Context.expect(
      FirstFunction != nullptr &&
          FailingUnitPipeline.addPass(std::make_unique<MutatingUnitFailurePass>()).has_value() &&
          !FailingUnitPipeline.run(Unit, Target, UnitAnalyses, FunctionAnalyses) &&
          !UnitAnalyses.hasCached(
              Unit, rcc::CompilationUnitAnalysisManager::id<UnitFunctionCountAnalysis>()) &&
          !FunctionAnalyses.hasCached(*FirstFunction,
                                      rcc::MachineAnalysisManager::id<BlockCountAnalysis>()),
      "a mutating compilation-unit failure must clear unit and function analysis caches");
}

} // namespace

int main() {
  TestContext Context;
  auto Target = createTarget();
  Context.expect(Target.has_value(), "machine pass tests require a valid target description");
  if (!Target)
    return Context.result();

  testAnalysisInfrastructure(Context, *Target);
  testPassAndInstrumentation(Context, *Target);
  testCompilationUnitAndParallelism(Context, *Target);
  return Context.result();
}
