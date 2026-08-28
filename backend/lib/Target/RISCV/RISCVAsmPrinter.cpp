// SPDX-License-Identifier: GPL-3.0-or-later

#include "RISCVAsmPrinter.h"

#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineJumpTableInfo.h"
#include "rcc/CodeGen/MachineMCContext.h"
#include "rcc/CodeGen/TargetConstant.h"
#include "rcc/MC/MCContext.h"
#include "rcc/MC/MCExpr.h"
#include "rcc/MC/MCStreamer.h"
#include "rcc/MC/MCSymbol.h"
#include "rcc/Target/RISCV/RISCVTargetRelocationKinds.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace rcc::riscv {
namespace {

[[nodiscard]] Expected<std::uint8_t> targetRelocationSize(TargetRelocationKindID Kind) {
  if (Kind == targetRelocationKind(RISCVTargetRelocationKind::Absolute32))
    return 4;
  if (Kind == targetRelocationKind(RISCVTargetRelocationKind::Absolute64))
    return 8;
  return makeUnexpected(ErrorCode::Unsupported,
                        "RV64GC target constant has an unknown relocation kind");
}

[[nodiscard]] Expected<mc::MCExprRef> addOffset(mc::MCExprRef Expression, std::int64_t Offset) {
  if (Offset == 0)
    return Expression;
  auto Result = mc::MCExpr::binary(mc::MCBinaryOperator::Add, std::move(Expression),
                                   mc::MCExpr::constant(Offset));
  if (!Result)
    return std::unexpected(Result.error());
  return *Result;
}

[[nodiscard]] Expected<void> emitBytes(std::span<const std::byte> Bytes, mc::MCStreamer &Streamer) {
  if (Bytes.empty())
    return {};
  std::vector<std::uint8_t> Storage;
  Storage.reserve(Bytes.size());
  for (const std::byte Byte : Bytes)
    Storage.push_back(std::to_integer<std::uint8_t>(Byte));
  return Streamer.emitBytes(Storage);
}

} // namespace

Expected<void> RISCVAsmPrinter::emitStartOfAsmFile() {
  auto Attribute = streamer().emitDirective("attribute", "arch, \"rv64gc\"");
  if (!Attribute)
    return std::unexpected(Attribute.error());
  return streamer().emitDirective("attribute", "stack_align, 16");
}

Expected<void> RISCVAsmPrinter::emitEndOfAsmFile() {
  mc::MCSectionDesc Note{
      .Name = ".note.GNU-stack",
      .Kind = mc::MCSectionKind::Custom,
      .Flags = {},
      .Alignment = 1,
      .EntrySize = 0,
      .Group = {},
      .UniqueID = std::nullopt,
  };
  auto Section = machineContext().context().getOrCreateSection(std::move(Note));
  if (!Section)
    return std::unexpected(Section.error());
  return streamer().switchSection(std::move(*Section));
}

Expected<void> RISCVAsmPrinter::emitTargetConstant(const TargetConstant &Constant) {
  auto Emission = Constant.emit();
  if (!Emission)
    return std::unexpected(Emission.error());
  auto DeclaredKind = Constant.relocationKind();
  if (!DeclaredKind)
    return std::unexpected(DeclaredKind.error());
  if (Emission->Fixups.empty() != !DeclaredKind->has_value()) {
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "RV64GC target constant relocation declaration is inconsistent");
  }
  if (Emission->Fixups.empty())
    return emitBytes(Emission->Bytes, streamer());

  std::vector<const TargetConstantFixup *> Fixups;
  Fixups.reserve(Emission->Fixups.size());
  for (const TargetConstantFixup &Fixup : Emission->Fixups) {
    if (Fixup.Kind != **DeclaredKind) {
      return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                            "RV64GC target constant contains mixed relocation kinds");
    }
    Fixups.push_back(&Fixup);
  }
  std::ranges::sort(Fixups, {}, [](const TargetConstantFixup *Fixup) { return Fixup->Offset; });

  std::uint64_t Cursor = 0;
  for (const TargetConstantFixup *Fixup : Fixups) {
    auto Size = targetRelocationSize(Fixup->Kind);
    if (!Size)
      return std::unexpected(Size.error());
    if (Fixup->Offset < Cursor || Fixup->Offset > Emission->Bytes.size() ||
        *Size > Emission->Bytes.size() - static_cast<std::size_t>(Fixup->Offset)) {
      return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                            "RV64GC target constant relocation storage is invalid");
    }
    auto RawBytes = emitBytes(std::span<const std::byte>(Emission->Bytes)
                                  .subspan(static_cast<std::size_t>(Cursor),
                                           static_cast<std::size_t>(Fixup->Offset - Cursor)),
                              streamer());
    if (!RawBytes)
      return std::unexpected(RawBytes.error());

    auto Symbol = machineContext().symbol(Fixup->Symbol);
    if (!Symbol)
      return std::unexpected(Symbol.error());
    auto Expression = mc::MCExpr::symbol(std::move(*Symbol));
    if (!Expression)
      return std::unexpected(Expression.error());
    auto WithAddend = addOffset(std::move(*Expression), Fixup->Addend);
    if (!WithAddend)
      return std::unexpected(WithAddend.error());
    auto Value = streamer().emitValue(std::move(*WithAddend), *Size);
    if (!Value)
      return std::unexpected(Value.error());
    Cursor = Fixup->Offset + *Size;
  }
  return emitBytes(
      std::span<const std::byte>(Emission->Bytes).subspan(static_cast<std::size_t>(Cursor)),
      streamer());
}

Expected<void> RISCVAsmPrinter::emitTargetJumpTable(const MachineFunction &Function,
                                                    const MachineJumpTableEntry &Entry) {
  const MachineJumpTableDesc *Description = Entry.description();
  if (Description == nullptr)
    return makeUnexpected(ErrorCode::InvalidArgument, "jump-table entry is not live");
  if (Description->Encoding == JumpTableEntryEncoding::Target) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "RV64GC jump table has an unknown target encoding");
  }

  mc::MCSymbolRef BaseSymbol;
  if (Description->Encoding == JumpTableEntryEncoding::GPRelative32BlockAddress ||
      Description->Encoding == JumpTableEntryEncoding::GPRelative64BlockAddress) {
    auto GlobalPointer = machineContext().symbol("__global_pointer$");
    if (!GlobalPointer)
      return std::unexpected(GlobalPointer.error());
    BaseSymbol = std::move(*GlobalPointer);
  } else if (Description->Encoding == JumpTableEntryEncoding::Custom32) {
    BaseSymbol = machineContext().jumpTableSymbol(Function, Entry.index());
  }

  for (const MachineBasicBlock *TargetBlock : Description->Targets) {
    if (TargetBlock == nullptr)
      return makeUnexpected(ErrorCode::AssemblyLoweringFailed, "jump table has a null target");
    const mc::MCSymbolRef TargetSymbol = machineContext().blockSymbol(*TargetBlock);
    if (!TargetSymbol) {
      return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                            "jump-table target has no MC symbol");
    }
    auto Expression = mc::MCExpr::symbol(TargetSymbol);
    if (!Expression)
      return std::unexpected(Expression.error());
    if (BaseSymbol) {
      auto Base = mc::MCExpr::symbol(BaseSymbol);
      if (!Base)
        return std::unexpected(Base.error());
      Expression = mc::MCExpr::binary(mc::MCBinaryOperator::Subtract, std::move(*Expression),
                                      std::move(*Base));
      if (!Expression)
        return std::unexpected(Expression.error());
    }
    auto Value = streamer().emitValue(std::move(*Expression), Description->EntrySize);
    if (!Value)
      return std::unexpected(Value.error());
  }
  return {};
}

} // namespace rcc::riscv
