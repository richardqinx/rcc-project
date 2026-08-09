// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_TARGETMCINSTLOWERING_H
#define RCC_CODEGEN_TARGETMCINSTLOWERING_H

#include "rcc/MC/MCInst.h"
#include "rcc/MC/MCInstPrinter.h"
#include "rcc/MC/MCSection.h"
#include "rcc/MC/MCStreamer.h"
#include "rcc/Support/Error.h"

namespace rcc {

class MachineConstantPoolEntry;
class GlobalInitializer;
struct GlobalObjectDesc;
class MachineFunction;
class MachineInstr;
class MachineJumpTableEntry;
class MachineMCContext;
struct GenericConstantData;
class TargetConstant;

class TargetMCInstLowering {
public:
  virtual ~TargetMCInstLowering() = default;

  [[nodiscard]] virtual const mc::MCInstPrinter &instructionPrinter() const noexcept = 0;

  [[nodiscard]] virtual Expected<mc::MCInst> lowerInstruction(const MachineInstr &Instruction,
                                                              MachineMCContext &Context) const = 0;

  [[nodiscard]] virtual Expected<void> emitTerminalInstruction(const MachineInstr &Instruction,
                                                               MachineMCContext &Context,
                                                               mc::MCStreamer &Streamer) const;

  [[nodiscard]] virtual Expected<mc::MCSectionDesc>
  textSection(const MachineFunction &Function) const = 0;
  [[nodiscard]] virtual Expected<mc::MCSectionDesc>
  globalSection(const GlobalObjectDesc &Object, MachineMCContext &Context) const = 0;
  [[nodiscard]] virtual Expected<mc::MCSectionDesc>
  constantPoolSection(const MachineFunction &Function,
                      const MachineConstantPoolEntry &Entry) const = 0;
  [[nodiscard]] virtual Expected<mc::MCSectionDesc>
  jumpTableSection(const MachineFunction &Function, const MachineJumpTableEntry &Entry) const = 0;

  [[nodiscard]] virtual Expected<void> emitFileHeader(MachineMCContext &Context,
                                                      mc::MCStreamer &Streamer) const;
  [[nodiscard]] virtual Expected<void> emitFileTrailer(MachineMCContext &Context,
                                                       mc::MCStreamer &Streamer) const;
  [[nodiscard]] virtual Expected<void> emitGenericConstant(const GenericConstantData &Constant,
                                                           std::uint64_t Size,
                                                           MachineMCContext &Context,
                                                           mc::MCStreamer &Streamer) const;
  [[nodiscard]] virtual Expected<void> emitGlobalInitializer(const GlobalInitializer &Initializer,
                                                             MachineMCContext &Context,
                                                             mc::MCStreamer &Streamer) const;
  [[nodiscard]] virtual Expected<void> emitTargetConstant(const TargetConstant &Constant,
                                                          MachineMCContext &Context,
                                                          mc::MCStreamer &Streamer) const;
  [[nodiscard]] virtual Expected<void> emitTargetJumpTable(const MachineFunction &Function,
                                                           const MachineJumpTableEntry &Entry,
                                                           MachineMCContext &Context,
                                                           mc::MCStreamer &Streamer) const;
};

} // namespace rcc

#endif // RCC_CODEGEN_TARGETMCINSTLOWERING_H
