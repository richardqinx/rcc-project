// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/Target/RISCV/RISCVPipeline.h"

#include "rcc/CodeGen/MachineCombiner.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineOptimizationPipeline.h"
#include "rcc/CodeGen/MachinePipelinePasses.h"
#include "rcc/CodeGen/MachineSSAExit.h"
#include "rcc/CodeGen/RegAllocPBQP.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/Target/RISCV/RISCVCodeGen.h"

#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rcc::riscv {
namespace {

template <typename PassT, typename... ArgumentT>
[[nodiscard]] Expected<void> addPass(MachinePassManager &Pipeline, ArgumentT &&...Arguments) {
  return Pipeline.addPass(std::make_unique<PassT>(std::forward<ArgumentT>(Arguments)...));
}

[[nodiscard]] Expected<void> addInitialPipeline(MachinePassManager &Pipeline,
                                                RISCVSubtargetFeatures Features) {
  auto Added = addPass<MachineCombinerPass>(Pipeline, machineCombinerInfo(),
                                            MachineCombinerStage::PreLegalizer);
  if (!Added)
    return std::unexpected(Added.error());
  Added = addPass<LegalizerPass>(Pipeline, createLegalizerInfo(Features));
  if (!Added)
    return std::unexpected(Added.error());
  Added = addPass<RegisterBankSelectPass>(Pipeline, registerBankInfo());
  if (!Added)
    return std::unexpected(Added.error());
  return addPass<InstructionSelectPass>(Pipeline, instructionSelector());
}

[[nodiscard]] Expected<void> addSSAExitPipeline(MachinePassManager &Pipeline) {
  auto Added = addSelectedSSAOptimizationPipeline(Pipeline, optimizationPipelineInfo());
  if (!Added)
    return std::unexpected(Added.error());
  Added = addPass<PHIEliminationPass>(Pipeline, ssaInfo());
  if (!Added)
    return std::unexpected(Added.error());
  return addPass<TwoAddressRewritePass>(Pipeline, ssaInfo());
}

[[nodiscard]] Expected<void> addRegisterAllocationPipeline(MachinePassManager &Pipeline) {
  auto Added = addPreRegisterAllocationOptimizationPipeline(Pipeline, optimizationPipelineInfo());
  if (!Added)
    return std::unexpected(Added.error());
  return addPass<PBQPAllocatorPass>(Pipeline, registerAllocationInfo());
}

[[nodiscard]] Expected<void> addCompleteRegistry(MachinePassManager &Pipeline,
                                                 RISCVSubtargetFeatures Features) {
  auto Added = addInitialPipeline(Pipeline, Features);
  if (!Added)
    return std::unexpected(Added.error());
  Added = addSSAExitPipeline(Pipeline);
  if (!Added)
    return std::unexpected(Added.error());
  Added = addRegisterAllocationPipeline(Pipeline);
  if (!Added)
    return std::unexpected(Added.error());
  return addPostRegisterAllocationPipeline(Pipeline, optimizationPipelineInfo());
}

} // namespace

Expected<void> addCanonicalPipeline(MachinePassManager &Pipeline, const MachineFunction &Function) {
  return addCanonicalPipeline(Pipeline, Function, RISCVSubtargetFeatures::rv64gc());
}

Expected<void> addCanonicalPipeline(MachinePassManager &Pipeline, const MachineFunction &Function,
                                    RISCVSubtargetFeatures Features) {
  if (!Function.properties().has(MachineFunctionProperty::Legalized)) {
    auto Added = addPass<MachineCombinerPass>(Pipeline, machineCombinerInfo(),
                                              MachineCombinerStage::PreLegalizer);
    if (!Added)
      return std::unexpected(Added.error());
    Added = addPass<LegalizerPass>(Pipeline, createLegalizerInfo(Features));
    if (!Added)
      return std::unexpected(Added.error());
  }
  if (!Function.properties().has(MachineFunctionProperty::RegBankSelected)) {
    auto Added = addPass<RegisterBankSelectPass>(Pipeline, registerBankInfo());
    if (!Added)
      return std::unexpected(Added.error());
  }
  if (!Function.properties().has(MachineFunctionProperty::Selected)) {
    auto Added = addPass<InstructionSelectPass>(Pipeline, instructionSelector());
    if (!Added)
      return std::unexpected(Added.error());
  }
  if (!Function.properties().has(MachineFunctionProperty::TiedOpsRewritten)) {
    auto Added = addSelectedSSAOptimizationPipeline(Pipeline, optimizationPipelineInfo());
    if (!Added)
      return std::unexpected(Added.error());
    if (!Function.properties().has(MachineFunctionProperty::NoPHIs)) {
      Added = addPass<PHIEliminationPass>(Pipeline, ssaInfo());
      if (!Added)
        return std::unexpected(Added.error());
    }
  }
  if (!Function.properties().has(MachineFunctionProperty::TiedOpsRewritten)) {
    auto Added = addPass<TwoAddressRewritePass>(Pipeline, ssaInfo());
    if (!Added)
      return std::unexpected(Added.error());
  }
  if (!Function.properties().has(MachineFunctionProperty::NoVRegs)) {
    auto Added = addRegisterAllocationPipeline(Pipeline);
    if (!Added)
      return std::unexpected(Added.error());
  }
  if (!Function.properties().has(MachineFunctionProperty::NoExecutablePseudos)) {
    auto Added = addPostRegisterAllocationPipeline(Pipeline, optimizationPipelineInfo());
    if (!Added)
      return std::unexpected(Added.error());
  }
  return {};
}

Expected<std::unique_ptr<MachineFunctionPass>> createCanonicalPipelinePass(std::string_view Name) {
  return createCanonicalPipelinePass(Name, RISCVSubtargetFeatures::rv64gc());
}

Expected<std::unique_ptr<MachineFunctionPass>>
createCanonicalPipelinePass(std::string_view Name, RISCVSubtargetFeatures Features) {
  if (Name.empty())
    return makeUnexpected(ErrorCode::InvalidArgument, "pipeline pass name cannot be empty");
  MachinePassManager Registry;
  auto Added = addCompleteRegistry(Registry, Features);
  if (!Added)
    return std::unexpected(Added.error());
  MachinePassManager::PassList Passes = std::move(Registry).takePasses();
  auto Position = std::ranges::find_if(Passes, [Name](const auto &Pass) {
    return Pass != nullptr && Pass->contract().Name == Name;
  });
  if (Position == Passes.end()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "unknown RV64GC machine pass '" + std::string(Name) + "'");
  }
  return std::move(*Position);
}

Expected<std::vector<std::string>> canonicalPipelinePassNames(const MachineFunction &Function) {
  MachinePassManager Pipeline;
  auto Added = addCanonicalPipeline(Pipeline, Function);
  if (!Added)
    return std::unexpected(Added.error());
  MachinePassManager::PassList Passes = std::move(Pipeline).takePasses();
  std::vector<std::string> Result;
  Result.reserve(Passes.size());
  for (const std::unique_ptr<MachineFunctionPass> &Pass : Passes) {
    if (Pass == nullptr)
      return makeUnexpected(ErrorCode::InternalError, "canonical pipeline contains a null pass");
    Result.push_back(Pass->contract().Name);
  }
  return Result;
}

Expected<void> runCanonicalPipeline(MachineFunction &Function, const TargetDescription &Target,
                                    MachinePassManagerOptions Options,
                                    MachinePassInstrumentation *Instrumentation) {
  MachinePassManager Pipeline(Options);
  auto Added = addCanonicalPipeline(Pipeline, Function);
  if (!Added)
    return std::unexpected(Added.error());
  MachineAnalysisManager Analyses;
  auto Registered = registerMachineOptimizationAnalyses(Analyses);
  if (!Registered)
    return std::unexpected(Registered.error());
  return Pipeline.run(Function, Target, Analyses, Instrumentation);
}

} // namespace rcc::riscv
