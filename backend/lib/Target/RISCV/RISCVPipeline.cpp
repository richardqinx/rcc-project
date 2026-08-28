// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/Target/RISCV/RISCVPipeline.h"

#include "GISel/RISCVCallLowering.h"
#include "GISel/RISCVRegisterBankInfo.h"
#include "RISCV.h"
#include "RISCVCallingConv.h"
#include "RISCVInstrInfo.h"
#include "rcc/CodeGen/CFIInstrInserter.h"
#include "rcc/CodeGen/FinalizeISel.h"
#include "rcc/CodeGen/GlobalISel/Combiner.h"
#include "rcc/CodeGen/GlobalISel/InstructionSelect.h"
#include "rcc/CodeGen/GlobalISel/Legalizer.h"
#include "rcc/CodeGen/GlobalISel/RegBankSelect.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineOptimizationPipeline.h"
#include "rcc/CodeGen/MachinePseudoExpansion.h"
#include "rcc/CodeGen/PHIElimination.h"
#include "rcc/CodeGen/RegAllocCommon.h"
#include "rcc/CodeGen/RegAllocPBQP.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/CodeGen/TwoAddressInstructionPass.h"
#include "rcc/CodeGen/VirtRegMap.h"

#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rcc::riscv {
namespace {

[[nodiscard]] LibcallLoweringInfo libcallLoweringInfo(const RISCVSubtarget &Subtarget) {
  const CallingConvention Convention =
      Subtarget.targetABI() == RISCVABI::LP64D ? CallingConvention::LP64D : CallingConvention::LP64;
  return {Subtarget.getCallLowering(), callingConventionID(Convention)};
}

template <typename PassT, typename... ArgumentT>
[[nodiscard]] Expected<void> addPass(MachineFunctionPassManager &Pipeline,
                                     ArgumentT &&...Arguments) {
  return Pipeline.addPass(std::make_unique<PassT>(std::forward<ArgumentT>(Arguments)...));
}

[[nodiscard]] Expected<void> addInitialPipeline(MachineFunctionPassManager &Pipeline,
                                                const RISCVSubtarget &Subtarget,
                                                TargetPipelineOptions Options) {
  auto Added = addPass<GlobalISelCombinerPass>(
      Pipeline, giselCombinerTable(), giselCombinerExecutor(), "riscv-prelegalizer-combiner");
  if (!Added)
    return std::unexpected(Added.error());
  Added = addPass<Legalizer>(Pipeline, Subtarget.getLegalizerInfo(), Options.Optimization,
                             libcallLoweringInfo(Subtarget), Options.GlobalISelAbort);
  if (!Added)
    return std::unexpected(Added.error());
  Added = addPass<RegBankSelect>(Pipeline, Subtarget.getRegBankInfo());
  if (!Added)
    return std::unexpected(Added.error());
  Added = addPass<InstructionSelect>(Pipeline, Subtarget.getInstructionSelector());
  if (!Added)
    return std::unexpected(Added.error());
  return addPass<FinalizeISelPass>(Pipeline, finalizeISelInfo());
}

[[nodiscard]] Expected<void> addSSAExitPipeline(MachineFunctionPassManager &Pipeline,
                                                const RISCVSubtarget &Subtarget,
                                                TargetPipelineOptions Options) {
  auto Added = addSelectedSSAOptimizationPipeline(
      Pipeline, optimizationPipelineInfo(Subtarget, Options.DebugMachineScheduler,
                                         Options.DebugPrologueEpilogue));
  if (!Added)
    return std::unexpected(Added.error());
  Added = Pipeline.addPass(createOptWInstrsPass(Subtarget));
  if (!Added)
    return std::unexpected(Added.error());
  Added = addPass<PHIEliminationPass>(Pipeline, instrInfo());
  if (!Added)
    return std::unexpected(Added.error());
  return addPass<TwoAddressInstructionPass>(Pipeline, instrInfo());
}

[[nodiscard]] Expected<void> addRegisterAllocationPipeline(MachineFunctionPassManager &Pipeline,
                                                           const RISCVSubtarget &Subtarget,
                                                           TargetPipelineOptions Options) {
  auto Added = addPass<PreRATargetPseudoExpansionPass>(Pipeline, pseudoExpansionInfo());
  if (!Added)
    return std::unexpected(Added.error());
  Added = Pipeline.addPass(createMergeBaseOffsetPass());
  if (!Added)
    return std::unexpected(Added.error());
  Added = addPreRegisterAllocationOptimizationPipeline(
      Pipeline, optimizationPipelineInfo(Subtarget, Options.DebugMachineScheduler,
                                         Options.DebugPrologueEpilogue));
  if (!Added)
    return std::unexpected(Added.error());
  Added = addPass<PBQPAllocatorPass>(Pipeline, registerAllocationInfo());
  if (!Added)
    return std::unexpected(Added.error());
  return addPass<VirtualRegisterRewritePass>(Pipeline);
}

[[nodiscard]] Expected<void> addCompleteRegistry(MachineFunctionPassManager &Pipeline,
                                                 const RISCVSubtarget &Subtarget,
                                                 TargetPipelineOptions Options) {
  auto Added = addInitialPipeline(Pipeline, Subtarget, Options);
  if (!Added)
    return std::unexpected(Added.error());
  Added = addSSAExitPipeline(Pipeline, Subtarget, Options);
  if (!Added)
    return std::unexpected(Added.error());
  Added = Pipeline.addPass(createGreedyAllocatorPass(registerAllocationInfo()));
  if (!Added)
    return std::unexpected(Added.error());
  Added = addRegisterAllocationPipeline(Pipeline, Subtarget, Options);
  if (!Added)
    return std::unexpected(Added.error());
  Added = addPostRegisterAllocationPipeline(
      Pipeline,
      optimizationPipelineInfo(Subtarget, Options.DebugMachineScheduler,
                               Options.DebugPrologueEpilogue),
      MachineOptimizationPipelineOptions{.UseTargetCopyInstructions =
                                             Options.UseTargetCopyInstructions},
      createPushPopOptimizationPass(Subtarget));
  if (!Added)
    return std::unexpected(Added.error());
  Added = Pipeline.addPass(createMakeCompressiblePass(Subtarget));
  if (!Added)
    return std::unexpected(Added.error());
  Added = Pipeline.addPass(createMoveMergePass(Subtarget));
  if (!Added)
    return std::unexpected(Added.error());
  return addPass<CFIInstrInserterPass>(Pipeline, Subtarget.frameLowering());
}

} // namespace

Expected<void> addCanonicalPipeline(MachineFunctionPassManager &Pipeline,
                                    const MachineFunction &Function,
                                    const RISCVSubtarget &Subtarget,
                                    TargetPipelineOptions Options) {
  if (!Function.properties().has(MachineFunctionProperty::Legalized)) {
    auto Added = addPass<GlobalISelCombinerPass>(
        Pipeline, giselCombinerTable(), giselCombinerExecutor(), "riscv-prelegalizer-combiner");
    if (!Added)
      return std::unexpected(Added.error());
    Added = addPass<Legalizer>(Pipeline, Subtarget.getLegalizerInfo(), Options.Optimization,
                               libcallLoweringInfo(Subtarget), Options.GlobalISelAbort);
    if (!Added)
      return std::unexpected(Added.error());
  }
  if (!Function.properties().has(MachineFunctionProperty::RegBankSelected)) {
    auto Added = addPass<RegBankSelect>(Pipeline, Subtarget.getRegBankInfo());
    if (!Added)
      return std::unexpected(Added.error());
  }
  if (!Function.properties().has(MachineFunctionProperty::Selected)) {
    auto Added = addPass<InstructionSelect>(Pipeline, Subtarget.getInstructionSelector());
    if (!Added)
      return std::unexpected(Added.error());
  }
  if (!Function.properties().has(MachineFunctionProperty::TiedOpsRewritten)) {
    auto Added = addPass<FinalizeISelPass>(Pipeline, finalizeISelInfo());
    if (!Added)
      return std::unexpected(Added.error());
  }
  if (!Function.properties().has(MachineFunctionProperty::TiedOpsRewritten)) {
    auto Added = addSelectedSSAOptimizationPipeline(
        Pipeline, optimizationPipelineInfo(Subtarget, Options.DebugMachineScheduler,
                                           Options.DebugPrologueEpilogue));
    if (!Added)
      return std::unexpected(Added.error());
    Added = Pipeline.addPass(createOptWInstrsPass(Subtarget));
    if (!Added)
      return std::unexpected(Added.error());
    if (!Function.properties().has(MachineFunctionProperty::NoPHIs)) {
      Added = addPass<PHIEliminationPass>(Pipeline, instrInfo());
      if (!Added)
        return std::unexpected(Added.error());
    }
  }
  if (!Function.properties().has(MachineFunctionProperty::TiedOpsRewritten)) {
    auto Added = addPass<TwoAddressInstructionPass>(Pipeline, instrInfo());
    if (!Added)
      return std::unexpected(Added.error());
  }
  if (!Function.properties().has(MachineFunctionProperty::NoVRegs)) {
    auto Added = addRegisterAllocationPipeline(Pipeline, Subtarget, Options);
    if (!Added)
      return std::unexpected(Added.error());
  }
  if (!Function.properties().has(MachineFunctionProperty::NoExecutablePseudos)) {
    auto Added = addPostRegisterAllocationPipeline(
        Pipeline,
        optimizationPipelineInfo(Subtarget, Options.DebugMachineScheduler,
                                 Options.DebugPrologueEpilogue),
        MachineOptimizationPipelineOptions{.UseTargetCopyInstructions =
                                               Options.UseTargetCopyInstructions},
        createPushPopOptimizationPass(Subtarget));
    if (!Added)
      return std::unexpected(Added.error());
  }
  auto Added = Pipeline.addPass(createMakeCompressiblePass(Subtarget));
  if (!Added)
    return std::unexpected(Added.error());
  if (Subtarget.features().has(RISCVExtension::Zcmp))
    return Pipeline.addPass(createMoveMergePass(Subtarget));
  return {};
}

Expected<std::unique_ptr<MachineFunctionPass>>
createCanonicalPipelinePass(std::string_view Name, const RISCVSubtarget &Subtarget,
                            TargetPipelineOptions Options) {
  if (Name.empty())
    return makeUnexpected(ErrorCode::InvalidArgument, "pipeline pass name cannot be empty");
  MachineFunctionPassManager Registry;
  auto Added = addCompleteRegistry(Registry, Subtarget, Options);
  if (!Added)
    return std::unexpected(Added.error());
  MachineFunctionPassManager::PassList Passes = std::move(Registry).takePasses();
  auto Position = std::ranges::find_if(Passes, [Name](const auto &Pass) {
    return Pass != nullptr && Pass->contract().Name == Name;
  });
  if (Position == Passes.end()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "unknown RV64GC machine pass '" + std::string(Name) + "'");
  }
  return std::move(*Position);
}

Expected<std::vector<std::string>> canonicalPipelinePassNames(const MachineFunction &Function,
                                                              const RISCVSubtarget &Subtarget) {
  MachineFunctionPassManager Pipeline;
  auto Added = addCanonicalPipeline(Pipeline, Function, Subtarget);
  if (!Added)
    return std::unexpected(Added.error());
  MachineFunctionPassManager::PassList Passes = std::move(Pipeline).takePasses();
  std::vector<std::string> Result;
  Result.reserve(Passes.size());
  for (const std::unique_ptr<MachineFunctionPass> &Pass : Passes) {
    if (Pass == nullptr)
      return makeUnexpected(ErrorCode::InternalError, "canonical pipeline contains a null pass");
    Result.push_back(Pass->contract().Name);
  }
  return Result;
}

} // namespace rcc::riscv
