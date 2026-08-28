// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_ASMPRINTER_H
#define RCC_CODEGEN_ASMPRINTER_H

#include "rcc/IR/DataLayout.h"
#include "rcc/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <utility>

namespace rcc {

class CompilationUnit;
class GlobalInitializer;
class MachineFunction;
class MachineJumpTableEntry;
class MachineMCContext;
struct GenericConstantData;
class TargetConstant;
class TargetDescription;
class TargetLoweringObjectFile;
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

class AsmPrinter {
public:
  AsmPrinter(const TargetDescription &Target, const TargetMCInstLowering &Lowering,
             const TargetLoweringObjectFile &ObjectFile, DataLayout Layout,
             MachineMCContext &Context, mc::MCStreamer &Streamer,
             AsmPrinterOptions Options = {}) noexcept
      : Target(&Target), Lowering(&Lowering), ObjectFile(&ObjectFile), Layout(std::move(Layout)),
        Context(&Context), Streamer(&Streamer), Options(Options) {}
  virtual ~AsmPrinter() = default;

  [[nodiscard]] Expected<void> emitCompilationUnit(const CompilationUnit &Unit);
  [[nodiscard]] Expected<void> emitFunction(const MachineFunction &Function);

protected:
  [[nodiscard]] virtual Expected<void> emitStartOfAsmFile();
  [[nodiscard]] virtual Expected<void> emitEndOfAsmFile();
  [[nodiscard]] virtual Expected<void> emitTargetConstant(const TargetConstant &Constant);
  [[nodiscard]] virtual Expected<void> emitTargetJumpTable(const MachineFunction &Function,
                                                           const MachineJumpTableEntry &Entry);

  [[nodiscard]] MachineMCContext &machineContext() const noexcept {
    return *Context;
  }

  [[nodiscard]] mc::MCStreamer &streamer() const noexcept {
    return *Streamer;
  }

private:
  [[nodiscard]] Expected<void> verifyFunction(const MachineFunction &Function) const;
  [[nodiscard]] Expected<void> emitGlobalObjects(const CompilationUnit &Unit);
  [[nodiscard]] Expected<void> emitFunctionImpl(const MachineFunction &Function);
  [[nodiscard]] Expected<void> emitFunctionBody(const MachineFunction &Function);
  [[nodiscard]] Expected<void> emitConstantPool(const MachineFunction &Function);
  [[nodiscard]] Expected<void> emitJumpTables(const MachineFunction &Function);
  [[nodiscard]] Expected<void> emitGenericConstant(const GenericConstantData &Constant,
                                                   std::uint64_t AllocationSize,
                                                   std::size_t Depth = 0);
  [[nodiscard]] Expected<void> emitGlobalInitializer(const GlobalInitializer &Initializer,
                                                     std::size_t Depth = 0);

  const TargetDescription *Target;
  const TargetMCInstLowering *Lowering;
  const TargetLoweringObjectFile *ObjectFile;
  DataLayout Layout;
  MachineMCContext *Context;
  mc::MCStreamer *Streamer;
  AsmPrinterOptions Options;
  bool EmissionStarted = false;
  bool EmissionFinished = false;
};

} // namespace rcc

#endif // RCC_CODEGEN_ASMPRINTER_H
