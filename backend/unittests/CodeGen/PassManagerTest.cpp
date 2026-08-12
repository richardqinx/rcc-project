// SPDX-License-Identifier: GPL-3.0-or-later

#include "Target/TestTarget.h"
#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineFunctionAnalysisManager.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
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
                                                 rcc::MachineFunctionAnalysisManager &Analyses) {
    auto Blocks = Analyses.getResult<BlockCountAnalysis>(Function, Target);
    if (!Blocks)
      return std::unexpected(Blocks.error());
    return Result{(*Blocks)->Count};
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

  [[nodiscard]] static rcc::Expected<Result> run(rcc::MachineFunction &,
                                                 const rcc::TargetDescription &,
                                                 rcc::MachineFunctionAnalysisManager &) {
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

  [[nodiscard]] static rcc::Expected<Result> run(rcc::MachineFunction &,
                                                 const rcc::TargetDescription &,
                                                 rcc::MachineFunctionAnalysisManager &) {
    return Result{};
  }
};

class CycleA final {
public:
  struct Result final {};

  [[nodiscard]] static constexpr std::string_view name() noexcept {
    return "CycleA";
  }

  [[nodiscard]] static rcc::Expected<Result> run(rcc::MachineFunction &,
                                                 const rcc::TargetDescription &,
                                                 rcc::MachineFunctionAnalysisManager &) {
    return Result{};
  }
};

class CycleB final {
public:
  struct Result final {};

  [[nodiscard]] static constexpr std::string_view name() noexcept {
    return "CycleB";
  }

  [[nodiscard]] static rcc::Expected<Result> run(rcc::MachineFunction &,
                                                 const rcc::TargetDescription &,
                                                 rcc::MachineFunctionAnalysisManager &) {
    return Result{};
  }
};

void testAnalysisManager(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Function =
      rcc::MachineFunction::create("analysis", rcc::MachineFunctionStableID::fromValue(1));
  Context.expect(Function.has_value(), "analysis function creation must succeed");
  if (!Function)
    return;

  rcc::MachineFunctionAnalysisManager Analyses;
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
                         .Dependencies =
                             {
                                 rcc::MachineFunctionAnalysisManager::id<BlockCountAnalysis>(),
                             },
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

  rcc::MachineFunctionAnalysisManager Cyclic;
  Context.expect(
      Cyclic.registerAnalysis<CycleA>({
                                          .Name = "CycleA",
                                          .Dependencies =
                                              {
                                                  rcc::MachineFunctionAnalysisManager::id<CycleB>(),
                                              },
                                      })
              .has_value() &&
          Cyclic
              .registerAnalysis<CycleB>({
                  .Name = "CycleB",
                  .Dependencies =
                      {
                          rcc::MachineFunctionAnalysisManager::id<CycleA>(),
                      },
              })
              .has_value(),
      "cyclic analysis registrations must be accepted until requested");
  Context.expect(!Cyclic.getResult<CycleA>(**Function, Target),
                 "analysis dependency cycles must produce a structured error");

  rcc::MachineFunctionAnalysisManager CustomInvalidation;
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
  Context.expect(CustomInvalidation.hasCached(
                     **Function, rcc::MachineFunctionAnalysisManager::id<StickyAnalysis>()) &&
                     !CustomInvalidation.hasCached(
                         **Function, rcc::MachineFunctionAnalysisManager::id<ExpiringAnalysis>()),
                 "analysis invalidate() must return false to preserve and true to invalidate");
}

} // namespace

int main() {
  TestContext Context;
  auto Target = createTarget();
  Context.expect(Target.has_value(), "pass manager tests require a valid target description");
  if (!Target)
    return Context.result();

  testAnalysisManager(Context, *Target);
  return Context.result();
}
