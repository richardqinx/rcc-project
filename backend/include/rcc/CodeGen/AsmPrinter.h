// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_ASMPRINTER_H
#define RCC_CODEGEN_ASMPRINTER_H

#include "rcc/Support/Error.h"

namespace rcc {

class CompilationUnit;
class MachineFunction;
class MachineMCContext;
class TargetDescription;
class TargetMCInstLowering;

namespace mc {
class MCStreamer;
} // namespace mc

struct AsmPrinterOptions final {
  bool EmitFunctionDirectives = true;
  bool EmitCFI = true;
  bool EmitBlockLabels = true;
  bool EmitInstructionSymbols = true;
  bool VerifyInput = true;
};

class AsmPrinter final {
public:
  AsmPrinter(const TargetDescription &Target, const TargetMCInstLowering &Lowering,
             MachineMCContext &Context, mc::MCStreamer &Streamer,
             AsmPrinterOptions Options = {}) noexcept
      : Target(&Target), Lowering(&Lowering), Context(&Context), Streamer(&Streamer),
        Options(Options) {}

  [[nodiscard]] Expected<void> emitCompilationUnit(const CompilationUnit &Unit);
  [[nodiscard]] Expected<void> emitFunction(const MachineFunction &Function);

private:
  [[nodiscard]] Expected<void> verifyFunction(const MachineFunction &Function) const;
  [[nodiscard]] Expected<void> emitGlobalObjects(const CompilationUnit &Unit);
  [[nodiscard]] Expected<void> emitFunctionBody(const MachineFunction &Function);
  [[nodiscard]] Expected<void> emitConstantPool(const MachineFunction &Function);
  [[nodiscard]] Expected<void> emitJumpTables(const MachineFunction &Function);

  const TargetDescription *Target;
  const TargetMCInstLowering *Lowering;
  MachineMCContext *Context;
  mc::MCStreamer *Streamer;
  AsmPrinterOptions Options;
  bool UnitEmissionStarted = false;
  bool UnitEmissionFinished = false;
};

} // namespace rcc

#endif // RCC_CODEGEN_ASMPRINTER_H
