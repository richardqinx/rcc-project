// SPDX-License-Identifier: GPL-3.0-or-later

#include "Target/TestTarget.h"
#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachinePassManager.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
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

class InstrumentedPass final : public rcc::MachineFunctionPass {
public:
  [[nodiscard]] const rcc::MachinePassContract &contract() const noexcept override {
    static const rcc::MachinePassContract Contract{
        .Name = "Instrumented",
        .Requires = {},
        .Sets = {},
        .Clears = {},
        .RequiredAnalyses =
            {
                rcc::MachineFunctionAnalysisManager::id<BlockCountAnalysis>(),
            },
        .VerificationStage = std::nullopt,
    };
    return Contract;
  }

  [[nodiscard]] rcc::Expected<rcc::MachinePassResult>
  run(rcc::MachineFunction &Function, const rcc::TargetDescription &Target,
      rcc::MachineFunctionAnalysisManager &Analyses) override {
    auto First = Analyses.getResult<BlockCountAnalysis>(Function, Target);
    auto Second = Analyses.getResult<BlockCountAnalysis>(Function, Target);
    auto FirstAction = Analyses.requestAction("first", Function);
    auto SecondAction = Analyses.requestAction("second", Function);
    if (!First || !Second || !FirstAction || !SecondAction || !*FirstAction || *SecondAction)
      return rcc::makeUnexpected(rcc::ErrorCode::VerificationError,
                                 "instrumentation action policy was not enforced");
    rcc::MachinePassResult Result;
    Result.Preserved = rcc::PreservedAnalyses::all();
    return Result;
  }
};

void testInstrumentation(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Function =
      rcc::MachineFunction::create("instrumentation", rcc::MachineFunctionStableID::fromValue(1));
  Context.expect(Function.has_value(), "instrumentation function creation must succeed");
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
                 "instrumentation analysis registration must succeed");

  rcc::MachinePassInstrumentation Instrumentation;
  Instrumentation.setPerFunctionFuel(1);
  std::uint32_t BeforePass = 0;
  std::uint32_t AfterPass = 0;
  std::uint32_t BeforeAnalysis = 0;
  std::uint32_t AfterAnalysis = 0;
  std::uint32_t CacheHits = 0;
  std::uint32_t Printed = 0;
  std::uint32_t Changes = 0;
  Instrumentation.addBeforeCallback(
      [&BeforePass](std::string_view, const rcc::MachineFunction &) { ++BeforePass; });
  Instrumentation.addAfterCallback([&AfterPass](std::string_view, const rcc::MachineFunction &,
                                                const rcc::MachinePassResult &) { ++AfterPass; });
  Instrumentation.addBeforeAnalysisCallback(
      [&BeforeAnalysis](std::string_view, const rcc::MachineFunction &) { ++BeforeAnalysis; });
  Instrumentation.addAfterAnalysisCallback(
      [&AfterAnalysis, &CacheHits](std::string_view, const rcc::MachineFunction &, bool CacheHit,
                                   std::uint64_t) {
        ++AfterAnalysis;
        CacheHits += CacheHit ? 1U : 0U;
      });
  Instrumentation.addIRPrintCallback([&Printed](rcc::InstrumentationPhase, std::string_view,
                                                const rcc::MachineFunction &) { ++Printed; });
  Instrumentation.addChangeCallback([&Changes](std::string_view, const rcc::MachineFunction &,
                                               rcc::MachinePassChanges ReportedChanges,
                                               bool FingerprintChanged) {
    if (ReportedChanges.empty() && !FingerprintChanged)
      ++Changes;
  });

  rcc::MachineFunctionPassManager Pipeline;
  Context.expect(Pipeline.addPass(std::make_unique<InstrumentedPass>()).has_value() &&
                     Pipeline.run(**Function, Target, Analyses, &Instrumentation).has_value(),
                 "instrumented pass must run");
  Context.expect(BeforePass == 1 && AfterPass == 1 && BeforeAnalysis == 1 && AfterAnalysis == 3 &&
                     CacheHits == 2 && Printed == 2 && Changes == 1,
                 "all pass and analysis instrumentation points must run");

  const auto Events = Instrumentation.actionEvents();
  Context.expect(Events.size() == 2 && Events[0].Accepted && !Events[1].Accepted,
                 "action instrumentation must record accepted and rejected decisions");
  const auto Statistics = Instrumentation.statistics();
  Context.expect(Statistics.size() == 2,
                 "instrumentation must collect one pass and one analysis statistic");
}

} // namespace

int main() {
  TestContext Context;
  auto Target = createTarget();
  Context.expect(Target.has_value(),
                 "pass instrumentation tests require a valid target description");
  if (!Target)
    return Context.result();

  testInstrumentation(Context, *Target);
  return Context.result();
}
