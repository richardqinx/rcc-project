// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineOptimizationPipeline.h"
#include "rcc/CodeGen/MachinePass.h"
#include "rcc/CodeGen/MachinePipelinePasses.h"
#include "rcc/CodeGen/MachineSSAExit.h"
#include "rcc/CodeGen/RegAllocPBQP.h"
#include "rcc/MIR/MIRDocument.h"
#include "rcc/Support/TextDiagnostic.h"
#include "rcc/Target/RISCV/RISCVAssemblyPrinter.h"
#include "rcc/Target/RISCV/RISCVCodeGen.h"
#include "rcc/Target/RISCV/RISCVTarget.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace {

struct CommandLine final {
  std::string InputPath;
};

[[nodiscard]] bool parseCommandLine(int ArgumentCount, char *Arguments[], CommandLine &Result) {
  for (int Index = 1; Index < ArgumentCount; ++Index) {
    const std::string_view Argument(Arguments[Index]);
    if (Argument != "--input" || Index + 1 >= ArgumentCount) {
      std::cerr << "usage: rcc-backend-asm --input <mir-file|->\n";
      return false;
    }
    Result.InputPath = Arguments[++Index];
  }
  if (Result.InputPath.empty()) {
    std::cerr << "usage: rcc-backend-asm --input <mir-file|->\n";
    return false;
  }
  return true;
}

[[nodiscard]] bool readInput(const CommandLine &Command, std::string &Contents) {
  if (Command.InputPath == "-") {
    Contents.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
    return true;
  }
  std::ifstream Input(Command.InputPath, std::ios::binary);
  if (!Input) {
    std::cerr << "rcc-backend-asm: cannot open input '" << Command.InputPath << "'\n";
    return false;
  }
  Contents.assign(std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>());
  return true;
}

template <typename PassT, typename... ArgumentT>
[[nodiscard]] rcc::Expected<void> addPass(rcc::MachinePassManager &Pipeline,
                                          ArgumentT &&...Arguments) {
  return Pipeline.addPass(std::make_unique<PassT>(std::forward<ArgumentT>(Arguments)...));
}

[[nodiscard]] rcc::Expected<void> runBackendPipeline(rcc::MachineFunction &Function,
                                                     const rcc::TargetDescription &Target) {
  rcc::MachinePassManager Pipeline;
  const rcc::MachineOptimizationPipelineTargetInfo TargetInfo =
      rcc::riscv::optimizationPipelineInfo();

  if (!Function.properties().has(rcc::MachineFunctionProperty::Legalized)) {
    auto Added = addPass<rcc::LegalizerPass>(Pipeline, rcc::riscv::legalizerInfo());
    if (!Added)
      return std::unexpected(Added.error());
  }
  if (!Function.properties().has(rcc::MachineFunctionProperty::RegBankSelected)) {
    auto Added = addPass<rcc::RegisterBankSelectPass>(Pipeline, rcc::riscv::registerBankInfo());
    if (!Added)
      return std::unexpected(Added.error());
  }
  if (!Function.properties().has(rcc::MachineFunctionProperty::Selected)) {
    auto Added = addPass<rcc::InstructionSelectPass>(Pipeline, rcc::riscv::instructionSelector());
    if (!Added)
      return std::unexpected(Added.error());
  }
  if (!Function.properties().has(rcc::MachineFunctionProperty::NoPHIs)) {
    auto Added = rcc::addSelectedSSAOptimizationPipeline(Pipeline, TargetInfo);
    if (!Added)
      return std::unexpected(Added.error());
    Added = addPass<rcc::PHIEliminationPass>(Pipeline, rcc::riscv::ssaInfo());
    if (!Added)
      return std::unexpected(Added.error());
  }
  if (!Function.properties().has(rcc::MachineFunctionProperty::TiedOpsRewritten)) {
    auto Added = addPass<rcc::TwoAddressRewritePass>(Pipeline, rcc::riscv::ssaInfo());
    if (!Added)
      return std::unexpected(Added.error());
  }
  if (!Function.properties().has(rcc::MachineFunctionProperty::NoVRegs)) {
    auto Added = rcc::addPreRegisterAllocationOptimizationPipeline(Pipeline, TargetInfo);
    if (!Added)
      return std::unexpected(Added.error());
    Added = addPass<rcc::PBQPAllocatorPass>(Pipeline, rcc::riscv::registerAllocationInfo());
    if (!Added)
      return std::unexpected(Added.error());
  }
  if (!Function.properties().has(rcc::MachineFunctionProperty::NoExecutablePseudos)) {
    auto Added = rcc::addPostRegisterAllocationPipeline(Pipeline, TargetInfo);
    if (!Added)
      return std::unexpected(Added.error());
  }

  rcc::MachineAnalysisManager Analyses;
  auto Registered = rcc::registerMachineOptimizationAnalyses(Analyses);
  if (!Registered)
    return std::unexpected(Registered.error());
  return Pipeline.run(Function, Target, Analyses);
}

} // namespace

int main(int ArgumentCount, char *Arguments[]) {
  CommandLine Command;
  if (!parseCommandLine(ArgumentCount, Arguments, Command))
    return 1;

  std::string SourceText;
  if (!readInput(Command, SourceText))
    return 1;

  auto Target = rcc::riscv::createTargetDescription();
  if (!Target) {
    std::cerr << "rcc-backend-asm: cannot create RV64GC target: " << Target.error().message()
              << '\n';
    return 1;
  }
  rcc::mir::MIRDocumentParserOptions Options;
  Options.DataLayout = "e-m:e-p:64:64-i64:64-i128:128-n32:64-S128";
  Options.DefaultPointerSizeInBits = 64;
  auto Parsed =
      rcc::mir::parseMIRDocument(*Target, Command.InputPath, std::move(SourceText), Options);
  if (!Parsed.succeeded()) {
    for (const rcc::TextDiagnostic &Diagnostic : Parsed.diagnostics())
      std::cerr << rcc::formatTextDiagnostic(Diagnostic, Parsed.source()) << '\n';
    return 1;
  }

  rcc::CompilationUnit &Unit = Parsed.document()->unit();
  for (std::size_t Index = 0; Index < Unit.functionCount(); ++Index) {
    rcc::MachineFunction *Function = Unit.functionAt(Index);
    if (Function == nullptr) {
      std::cerr << "rcc-backend-asm: parsed document contains a null function\n";
      return 1;
    }
    auto Run = runBackendPipeline(*Function, *Target);
    if (!Run) {
      std::cerr << "rcc-backend-asm: backend pipeline failed for '" << Function->name()
                << "': " << Run.error().message() << '\n';
      return 1;
    }
  }

  auto Assembly = rcc::riscv::emitAssembly(Unit, *Target);
  if (!Assembly) {
    std::cerr << "rcc-backend-asm: assembly emission failed: " << Assembly.error().message()
              << '\n';
    return 1;
  }
  std::cout << *Assembly;
  return 0;
}
