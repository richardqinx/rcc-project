// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachinePassManager.h"

#include "Target/TestTarget.h"
#include "rcc/CodeGen/CompilationUnit.h"
#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/MachineFunction.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
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
                                                 rcc::MachineFunctionAnalysisManager &) {
    return Result{Function.blockCount()};
  }
};

class NonThreadSafeAnalysis final {
public:
  struct Result final {};

  [[nodiscard]] static constexpr std::string_view name() noexcept {
    return "NonThreadSafe";
  }

  [[nodiscard]] static rcc::Expected<Result> run(rcc::MachineFunction &,
                                                 const rcc::TargetDescription &,
                                                 rcc::MachineFunctionAnalysisManager &) {
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
        .RequiredAnalyses = {rcc::MachineFunctionAnalysisManager::id<BlockCountAnalysis>()},
        .VerificationStage = std::nullopt,
    };
    return Contract;
  }

  [[nodiscard]] rcc::Expected<rcc::MachinePassResult>
  run(rcc::MachineFunction &Function, const rcc::TargetDescription &Target,
      rcc::MachineFunctionAnalysisManager &Analyses) override {
    auto Blocks = Analyses.getResult<BlockCountAnalysis>(Function, Target);
    if (!Blocks)
      return std::unexpected(Blocks.error());
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

  [[nodiscard]] rcc::Expected<rcc::MachinePassResult>
  run(rcc::MachineFunction &Function, const rcc::TargetDescription &,
      rcc::MachineFunctionAnalysisManager &) override {
    auto Block = Function.createBasicBlock();
    if (!Block)
      return std::unexpected(Block.error());
    auto Appended = Function.appendBlock(**Block);
    if (!Appended)
      return std::unexpected(Appended.error());
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
        .RequiredAnalyses = {rcc::MachineFunctionAnalysisManager::id<BlockCountAnalysis>()},
        .VerificationStage = std::nullopt,
    };
    return Contract;
  }

  [[nodiscard]] rcc::Expected<rcc::MachinePassResult>
  run(rcc::MachineFunction &Function, const rcc::TargetDescription &,
      rcc::MachineFunctionAnalysisManager &) override {
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

  [[nodiscard]] rcc::Expected<rcc::MachinePassResult>
  run(rcc::MachineFunction &, const rcc::TargetDescription &,
      rcc::MachineFunctionAnalysisManager &) override {
    Count->fetch_add(1, std::memory_order_relaxed);
    return rcc::MachinePassResult{};
  }

private:
  std::atomic<std::uint32_t> *Count;
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
                  rcc::MachineFunctionAnalysisManager::id<NonThreadSafeAnalysis>(),
              },
          .VerificationStage = std::nullopt,
      };
      Value.ThreadSafety = rcc::PassThreadSafety::ThreadSafe;
      return Value;
    }();
    return Contract;
  }

  [[nodiscard]] rcc::Expected<rcc::MachinePassResult>
  run(rcc::MachineFunction &, const rcc::TargetDescription &,
      rcc::MachineFunctionAnalysisManager &) override {
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
      rcc::CompilationUnitAnalysisManager &Analyses,
      rcc::MachineFunctionAnalysisManager &) override {
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
      rcc::CompilationUnitAnalysisManager &, rcc::MachineFunctionAnalysisManager &) override {
    auto Created = Unit.createMachineFunction("added-before-failure");
    if (!Created)
      return std::unexpected(Created.error());
    return rcc::makeUnexpected(rcc::ErrorCode::VerificationError,
                               "intentional compilation-unit pass failure");
  }
};

void testMachinePassManager(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Function = rcc::MachineFunction::create("pass", rcc::MachineFunctionStableID::fromValue(2));
  Context.expect(Function.has_value(), "pass function creation must succeed");
  if (!Function)
    return;

  rcc::MachineFunctionAnalysisManager Analyses;
  Context.expect(Analyses
                     .registerAnalysis<BlockCountAnalysis>({
                         .Name = "BlockCount",
                         .Dependencies = {},
                         .InvalidatedBy = rcc::MachinePassChanges::all(),
                         .ThreadSafety = rcc::PassThreadSafety::ThreadSafe,
                     })
                     .has_value(),
                 "pass analysis registration must succeed");

  rcc::MachineFunctionPassManager Pipeline;
  Context.expect(Pipeline.addPass(std::make_unique<RequiredAnalysisPass>()).has_value(),
                 "a valid pass must be accepted");
  auto Run = Pipeline.run(**Function, Target, Analyses);
  Context.expect(Run.has_value(), "a pass with a materialized required analysis must run");

  auto ChangedFunction =
      rcc::MachineFunction::create("unreported", rcc::MachineFunctionStableID::fromValue(3));
  rcc::MachineFunctionAnalysisManager EmptyAnalyses;
  rcc::MachineFunctionPassManager Detecting(
      {.VerifyBefore = true, .VerifyEach = true, .DetectUnreportedChanges = true});
  Context.expect(ChangedFunction &&
                     Detecting.addPass(std::make_unique<UnreportedChangePass>()).has_value() &&
                     !Detecting.run(**ChangedFunction, Target, EmptyAnalyses),
                 "change detection must reject an unreported MachineIR mutation");

  auto FailedFunction =
      rcc::MachineFunction::create("failed", rcc::MachineFunctionStableID::fromValue(4));
  rcc::MachineFunctionAnalysisManager FailureAnalyses;
  Context.expect(FailedFunction && FailureAnalyses
                                       .registerAnalysis<BlockCountAnalysis>({
                                           .Name = "BlockCount",
                                           .Dependencies = {},
                                           .InvalidatedBy = rcc::MachinePassChanges::all(),
                                       })
                                       .has_value(),
                 "mutating failure analysis fixtures must be valid");
  rcc::MachineFunctionPassManager FailingPipeline;
  Context.expect(
      FailedFunction &&
          FailingPipeline.addPass(std::make_unique<MutatingFailurePass>()).has_value() &&
          !FailingPipeline.run(**FailedFunction, Target, FailureAnalyses) &&
          !FailureAnalyses.hasCached(**FailedFunction,
                                     rcc::MachineFunctionAnalysisManager::id<BlockCountAnalysis>()),
      "a pass that mutates and fails must not leave stale function analyses cached");
}

void testCompilationUnitAndParallelism(TestContext &Context, const rcc::TargetDescription &Target) {
  rcc::CompilationUnit Unit;
  for (std::size_t Index = 0; Index < 8; ++Index) {
    auto Function = Unit.createMachineFunction("f" + std::to_string(Index));
    Context.expect(Function.has_value(), "parallel test function creation must succeed");
  }

  std::atomic<std::uint32_t> Runs = 0;
  rcc::MachineFunctionPassManager Parallel;
  Context.expect(Parallel.addPass(std::make_unique<ParallelCountingPass>(Runs)).has_value(),
                 "thread-safe pass registration must succeed");
  rcc::MachineFunctionAnalysisManager Prototype;
  auto ParallelRun = Parallel.runFunctions(Unit, Target, Prototype, std::size_t{4});
  Context.expect(ParallelRun.has_value() && Runs == Unit.functionCount(),
                 "parallel function execution must visit every function exactly once");

  rcc::MachineFunctionAnalysisManager UnsafePrototype;
  Context.expect(UnsafePrototype
                     .registerAnalysis<NonThreadSafeAnalysis>({
                         .Name = "NonThreadSafe",
                         .Dependencies = {},
                         .InvalidatedBy = rcc::MachinePassChanges::all(),
                         .ThreadSafety = rcc::PassThreadSafety::NotThreadSafe,
                     })
                     .has_value(),
                 "non-thread-safe analysis registration must be explicit");
  rcc::MachineFunctionPassManager UnsafeParallel;
  Context.expect(
      UnsafeParallel.addPass(std::make_unique<RequiresNonThreadSafeAnalysisPass>()).has_value() &&
          !UnsafeParallel.runFunctions(Unit, Target, UnsafePrototype, std::size_t{4}),
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
  rcc::MachineFunctionAnalysisManager FunctionAnalyses;
  rcc::CompilationUnitPassManager UnitPipeline;
  Context.expect(UnitPipeline.addPass(std::make_unique<ObserveUnitPass>()).has_value(),
                 "compilation-unit pass registration must succeed");
  Context.expect(UnitPipeline.run(Unit, Target, UnitAnalyses, FunctionAnalyses).has_value(),
                 "compilation-unit pass and analysis must run through their own manager");

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
          !FunctionAnalyses.hasCached(
              *FirstFunction, rcc::MachineFunctionAnalysisManager::id<BlockCountAnalysis>()),
      "a mutating compilation-unit failure must clear unit and function analysis caches");
}

} // namespace

int main() {
  TestContext Context;
  auto Target = createTarget();
  Context.expect(Target.has_value(), "machine pass tests require a valid target description");
  if (!Target)
    return Context.result();

  testMachinePassManager(Context, *Target);
  testCompilationUnitAndParallelism(Context, *Target);
  return Context.result();
}
