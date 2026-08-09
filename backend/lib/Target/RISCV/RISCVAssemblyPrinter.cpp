// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/Target/RISCV/RISCVAssemblyPrinter.h"

#include "rcc/CodeGen/AsmPrinter.h"
#include "rcc/CodeGen/CompilationUnit.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineMCContext.h"
#include "rcc/MC/MCContext.h"
#include "rcc/MC/MCStreamer.h"
#include "rcc/Target/RISCV/RISCVMCTargetLowering.h"

#include <string>

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
  mc::AsmTextStreamer Streamer(Lowering.instructionPrinter());
  AsmPrinter Printer(Target, Lowering, MachineContext, Streamer, printerOptions(Options));
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
  mc::AsmTextStreamer Streamer(Lowering.instructionPrinter());
  AsmPrinter Printer(Target, Lowering, MachineContext, Streamer, printerOptions(Options));

  auto Header = Lowering.emitFileHeader(MachineContext, Streamer);
  if (!Header)
    return std::unexpected(Header.error());
  auto Emitted = Printer.emitFunction(Function);
  if (!Emitted)
    return std::unexpected(Emitted.error());
  auto Trailer = Lowering.emitFileTrailer(MachineContext, Streamer);
  if (!Trailer)
    return std::unexpected(Trailer.error());
  auto Finished = Streamer.finish();
  if (!Finished)
    return std::unexpected(Finished.error());
  return Streamer.takeOutput();
}

} // namespace rcc::riscv
