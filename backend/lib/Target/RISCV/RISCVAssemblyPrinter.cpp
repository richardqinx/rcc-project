// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/Target/RISCV/RISCVAssemblyPrinter.h"

#include "RISCVAsmPrinter.h"
#include "RISCVTargetObjectFile.h"
#include "rcc/CodeGen/AsmPrinter.h"
#include "rcc/CodeGen/CompilationUnit.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineMCContext.h"
#include "rcc/IR/DataLayout.h"
#include "rcc/MC/MCContext.h"
#include "rcc/MC/MCStreamer.h"
#include "rcc/Target/RISCV/RISCVMCTargetLowering.h"
#include "rcc/Target/RISCV/RISCVTarget.h"

#include <string>
#include <utility>

namespace rcc::riscv {
namespace {

[[nodiscard]] AsmPrinterOptions printerOptions(AssemblyEmissionOptions Options) noexcept {
  return AsmPrinterOptions{
      .EmitFunctionDirectives = Options.EmitFunctionDirectives,
      .EmitCFI = Options.EmitCFI,
      .EmitBlockLabels = Options.EmitBlockLabels,
      .EmitInstructionSymbols = true,
      .VerifyInput = Options.VerifyInput,
  };
}

} // namespace

Expected<std::string> emitAssembly(const CompilationUnit &Unit, const TargetDescription &Target,
                                   AssemblyEmissionOptions Options) {
  mc::MCContext MCContext;
  MachineMCContext MachineContext(MCContext);
  RISCVMCTargetLowering Lowering(Target);
  RISCVELFTargetObjectFile ObjectFile;
  auto Layout = DataLayout::parse(TargetDataLayout);
  if (!Layout)
    return std::unexpected(Layout.error());
  mc::AsmTextStreamer Streamer(Lowering.instructionPrinter());
  RISCVAsmPrinter Printer(Target, Lowering, ObjectFile, std::move(*Layout), MachineContext,
                          Streamer, printerOptions(Options));
  auto Emitted = Printer.emitCompilationUnit(Unit);
  if (!Emitted)
    return std::unexpected(Emitted.error());
  return Streamer.takeOutput();
}

Expected<std::string> emitAssembly(const MachineFunction &Function, const TargetDescription &Target,
                                   AssemblyEmissionOptions Options) {
  mc::MCContext MCContext;
  MachineMCContext MachineContext(MCContext);
  RISCVMCTargetLowering Lowering(Target);
  RISCVELFTargetObjectFile ObjectFile;
  auto Layout = DataLayout::parse(TargetDataLayout);
  if (!Layout)
    return std::unexpected(Layout.error());
  mc::AsmTextStreamer Streamer(Lowering.instructionPrinter());
  RISCVAsmPrinter Printer(Target, Lowering, ObjectFile, std::move(*Layout), MachineContext,
                          Streamer, printerOptions(Options));
  auto Emitted = Printer.emitFunction(Function);
  if (!Emitted)
    return std::unexpected(Emitted.error());
  return Streamer.takeOutput();
}

} // namespace rcc::riscv
