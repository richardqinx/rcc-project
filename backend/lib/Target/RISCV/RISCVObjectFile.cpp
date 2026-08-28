// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/Target/RISCV/RISCVObjectFile.h"

#include "RISCVAsmPrinter.h"
#include "RISCVTargetObjectFile.h"
#include "rcc/CodeGen/AsmPrinter.h"
#include "rcc/CodeGen/CompilationUnit.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineMCContext.h"
#include "rcc/IR/DataLayout.h"
#include "rcc/MC/ELFObjectWriter.h"
#include "rcc/MC/MCContext.h"
#include "rcc/MC/MCObjectStreamer.h"
#include "rcc/Target/RISCV/RISCVAsmBackend.h"
#include "rcc/Target/RISCV/RISCVMCCodeEmitter.h"
#include "rcc/Target/RISCV/RISCVMCTargetLowering.h"
#include "rcc/Target/RISCV/RISCVTarget.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace rcc::riscv {
namespace {

constexpr std::uint16_t EMRISCV = 243;
constexpr std::uint32_t SHTRISCVAttributes = 0x70000003;

[[nodiscard]] AsmPrinterOptions printerOptions(ObjectEmissionOptions Options) noexcept {
  return AsmPrinterOptions{
      .EmitFunctionDirectives = Options.EmitFunctionDirectives,
      .EmitCFI = Options.EmitCFI,
      .EmitBlockLabels = Options.EmitBlockLabels,
      .EmitInstructionSymbols = true,
      .VerifyInput = Options.VerifyInput,
  };
}

[[nodiscard]] std::vector<std::uint8_t> attributesSection() {
  constexpr char Architecture[] = "rv64i2p1_m2p0_a2p1_f2p2_d2p2_c2p0_zicsr2p0_zifencei2p0_"
                                  "zmmul1p0_zaamo1p0_zalrsc1p0";
  const std::uint32_t FilePayloadSize = 1U + sizeof(Architecture) + 2U;
  const std::uint32_t VendorSize = 4U + 6U + 1U + 4U + FilePayloadSize;
  std::vector<std::uint8_t> Bytes;
  Bytes.reserve(1U + VendorSize);
  const auto AppendU32 = [&Bytes](std::uint32_t Value) {
    for (std::uint8_t Shift = 0; Shift < 32; Shift += 8)
      Bytes.push_back(static_cast<std::uint8_t>(Value >> Shift));
  };
  Bytes.push_back('A');
  AppendU32(VendorSize);
  Bytes.insert(Bytes.end(), {'r', 'i', 's', 'c', 'v', 0});
  Bytes.push_back(1);
  AppendU32(5U + FilePayloadSize);
  Bytes.push_back(5);
  Bytes.insert(Bytes.end(), Architecture, Architecture + sizeof(Architecture));
  Bytes.push_back(4);
  Bytes.push_back(16);
  return Bytes;
}

[[nodiscard]] mc::ELFObjectTargetInfo objectTargetInfo() {
  mc::ELFObjectTargetInfo Info;
  Info.Machine = EMRISCV;
  Info.ExtraSections.push_back(mc::ELFExtraSection{
      .Name = ".riscv.attributes",
      .Type = SHTRISCVAttributes,
      .Flags = 0,
      .Alignment = 1,
      .EntrySize = 0,
      .Contents = attributesSection(),
  });
  return Info;
}

struct ObjectEmissionContext final {
  explicit ObjectEmissionContext(const TargetDescription &Target)
      : MachineContext(Context), Lowering(Target), Emitter(Target), Writer(objectTargetInfo()),
        Streamer(Emitter, Backend, Writer) {
    mc::MCSectionDesc EHFrameDescription{
        .Name = ".eh_frame",
        .Kind = mc::MCSectionKind::EHFrame,
        .Flags = {mc::MCSectionFlag::Alloc},
        .Alignment = 8,
        .EntrySize = 0,
        .Group = {},
        .UniqueID = std::nullopt,
    };
    auto EHFrame = Context.getOrCreateSection(std::move(EHFrameDescription));
    if (!EHFrame) {
      Initialization = std::unexpected(EHFrame.error());
      return;
    }
    Initialization = Streamer.configureDwarfFrame(std::move(*EHFrame),
                                                  mc::MCDwarfFrameInfo{.CodeAlignment = 1,
                                                                       .DataAlignment = -8,
                                                                       .ReturnAddressRegister = 1,
                                                                       .InitialCFARegister = 2,
                                                                       .InitialCFAOffset = 0});
  }

  mc::MCContext Context;
  MachineMCContext MachineContext;
  RISCVMCTargetLowering Lowering;
  RISCVELFTargetObjectFile ObjectFile;
  RISCVMCCodeEmitter Emitter;
  RISCVAsmBackend Backend;
  mc::ELF64LEObjectWriter Writer;
  mc::ELFObjectStreamer Streamer;
  Expected<void> Initialization;
};

} // namespace

Expected<std::vector<std::uint8_t>> emitObject(const CompilationUnit &Unit,
                                               const TargetDescription &Target,
                                               ObjectEmissionOptions Options) {
  ObjectEmissionContext Context(Target);
  if (!Context.Initialization)
    return std::unexpected(Context.Initialization.error());
  auto Layout = DataLayout::parse(TargetDataLayout);
  if (!Layout)
    return std::unexpected(Layout.error());
  RISCVAsmPrinter Printer(Target, Context.Lowering, Context.ObjectFile, std::move(*Layout),
                          Context.MachineContext, Context.Streamer, printerOptions(Options));
  auto Emitted = Printer.emitCompilationUnit(Unit);
  if (!Emitted)
    return std::unexpected(Emitted.error());
  return Context.Streamer.takeOutput();
}

Expected<std::vector<std::uint8_t>> emitObject(const MachineFunction &Function,
                                               const TargetDescription &Target,
                                               ObjectEmissionOptions Options) {
  ObjectEmissionContext Context(Target);
  if (!Context.Initialization)
    return std::unexpected(Context.Initialization.error());
  auto Layout = DataLayout::parse(TargetDataLayout);
  if (!Layout)
    return std::unexpected(Layout.error());
  RISCVAsmPrinter Printer(Target, Context.Lowering, Context.ObjectFile, std::move(*Layout),
                          Context.MachineContext, Context.Streamer, printerOptions(Options));
  auto Emitted = Printer.emitFunction(Function);
  if (!Emitted)
    return std::unexpected(Emitted.error());
  return Context.Streamer.takeOutput();
}

} // namespace rcc::riscv
