// SPDX-License-Identifier: GPL-3.0-or-later

#include "RISCVTargetObjectFile.h"

#include "rcc/CodeGen/GlobalObject.h"
#include "rcc/CodeGen/MachineConstantPool.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineJumpTableInfo.h"
#include "rcc/CodeGen/MachineMCContext.h"
#include "rcc/CodeGen/TargetConstant.h"
#include "rcc/MC/MCContext.h"
#include "rcc/MC/MCSymbol.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string>

namespace rcc::riscv {
namespace {

[[nodiscard]] bool initializerIsZero(const GlobalInitializer &Initializer, std::size_t Depth = 0) {
  if (Depth > 1024)
    return false;
  switch (Initializer.kind()) {
  case GlobalInitializer::Kind::Integer: {
    const auto *Value = Initializer.getIf<APInt>();
    return Value != nullptr &&
           std::ranges::all_of(Value->words(), [](std::uint64_t Word) { return Word == 0; });
  }
  case GlobalInitializer::Kind::Floating: {
    const auto *Value = Initializer.getIf<APFloat>();
    return Value != nullptr &&
           std::ranges::all_of(Value->words(), [](std::uint64_t Word) { return Word == 0; });
  }
  case GlobalInitializer::Kind::Bytes: {
    const auto *Value = Initializer.getIf<std::vector<std::byte>>();
    return Value != nullptr &&
           std::ranges::all_of(*Value, [](std::byte Byte) { return Byte == std::byte{0}; });
  }
  case GlobalInitializer::Kind::ZeroFill:
  case GlobalInitializer::Kind::UndefinedStorage:
    return true;
  case GlobalInitializer::Kind::Repeated: {
    const auto *Value = Initializer.getIf<GlobalRepeatedInitializer>();
    return Value != nullptr && Value->Value && initializerIsZero(*Value->Value, Depth + 1);
  }
  case GlobalInitializer::Kind::Aggregate: {
    const auto *Value = Initializer.getIf<GlobalAggregateInitializer>();
    return Value != nullptr && std::ranges::all_of(Value->Elements, [Depth](const auto &Element) {
             return Element && initializerIsZero(*Element, Depth + 1);
           });
  }
  case GlobalInitializer::Kind::AlignmentPadding: {
    const auto *Value = Initializer.getIf<GlobalAlignmentPadding>();
    return Value != nullptr && Value->Fill == 0;
  }
  case GlobalInitializer::Kind::Target: {
    const auto *Value = Initializer.getIf<TargetConstant>();
    if (Value == nullptr)
      return false;
    auto Emission = Value->emit();
    return Emission && Emission->Fixups.empty() &&
           std::ranges::all_of(Emission->Bytes,
                               [](std::byte Byte) { return Byte == std::byte{0}; });
  }
  case GlobalInitializer::Kind::Invalid:
  case GlobalInitializer::Kind::SymbolAddress:
  case GlobalInitializer::Kind::SymbolDifference:
    return false;
  }
  return false;
}

[[nodiscard]] bool initializerHasRelocation(const GlobalInitializer &Initializer,
                                            std::size_t Depth = 0) {
  if (Depth > 1024)
    return true;
  switch (Initializer.kind()) {
  case GlobalInitializer::Kind::SymbolAddress:
  case GlobalInitializer::Kind::SymbolDifference:
    return true;
  case GlobalInitializer::Kind::Repeated: {
    const auto *Value = Initializer.getIf<GlobalRepeatedInitializer>();
    return Value == nullptr || !Value->Value || initializerHasRelocation(*Value->Value, Depth + 1);
  }
  case GlobalInitializer::Kind::Aggregate: {
    const auto *Value = Initializer.getIf<GlobalAggregateInitializer>();
    return Value == nullptr || std::ranges::any_of(Value->Elements, [Depth](const auto &Element) {
             return !Element || initializerHasRelocation(*Element, Depth + 1);
           });
  }
  case GlobalInitializer::Kind::Target: {
    const auto *Value = Initializer.getIf<TargetConstant>();
    if (Value == nullptr)
      return true;
    auto Emission = Value->emit();
    return !Emission || !Emission->Fixups.empty();
  }
  case GlobalInitializer::Kind::Invalid:
    return true;
  case GlobalInitializer::Kind::Integer:
  case GlobalInitializer::Kind::Floating:
  case GlobalInitializer::Kind::Bytes:
  case GlobalInitializer::Kind::ZeroFill:
  case GlobalInitializer::Kind::AlignmentPadding:
  case GlobalInitializer::Kind::UndefinedStorage:
    return false;
  }
  return true;
}

[[nodiscard]] mc::MCSectionDesc textSectionDescription() {
  return mc::MCSectionDesc{
      .Name = ".text",
      .Kind = mc::MCSectionKind::Text,
      .Flags = {mc::MCSectionFlag::Alloc, mc::MCSectionFlag::Executable},
      .Alignment = 4,
      .EntrySize = 0,
      .Group = {},
      .UniqueID = std::nullopt,
  };
}

[[nodiscard]] mc::MCSectionDesc readOnlySectionDescription() {
  return mc::MCSectionDesc{
      .Name = ".rodata",
      .Kind = mc::MCSectionKind::ReadOnly,
      .Flags = {mc::MCSectionFlag::Alloc},
      .Alignment = 1,
      .EntrySize = 0,
      .Group = {},
      .UniqueID = std::nullopt,
  };
}

} // namespace

Expected<mc::MCSectionDesc> RISCVELFTargetObjectFile::textSection(const MachineFunction &) const {
  return textSectionDescription();
}

Expected<mc::MCSectionDesc>
RISCVELFTargetObjectFile::globalSection(const GlobalObjectDesc &Object,
                                        MachineMCContext &Context) const {
  if (Object.AddressSpace != 0) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "RV64GC assembly does not support nonzero global address spaces");
  }
  if (Object.TargetFlags != 0 || !Object.TargetAttributes.empty()) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "RV64GC assembly does not recognize this global target state");
  }
  const bool IsTLS = Object.TLSModel != GlobalTLSModel::None;
  const bool IsZero = Object.Initializer && initializerIsZero(*Object.Initializer);
  const bool HasRelocation = Object.Initializer && initializerHasRelocation(*Object.Initializer);

  mc::MCSectionDesc Result;
  Result.Alignment = Object.Align.bytes();
  if (IsTLS) {
    Result.Name = IsZero ? ".tbss" : ".tdata";
    Result.Kind = IsZero ? mc::MCSectionKind::TLSBSS : mc::MCSectionKind::TLSData;
    Result.Flags = {mc::MCSectionFlag::Alloc, mc::MCSectionFlag::Writable, mc::MCSectionFlag::TLS};
  } else if (Object.IsConstant && HasRelocation) {
    Result.Name = ".data.rel.ro";
    Result.Kind = mc::MCSectionKind::Data;
    Result.Flags = {mc::MCSectionFlag::Alloc, mc::MCSectionFlag::Writable};
  } else if (Object.IsConstant) {
    Result = readOnlySectionDescription();
    Result.Alignment = Object.Align.bytes();
  } else if (IsZero) {
    Result.Name = ".bss";
    Result.Kind = mc::MCSectionKind::BSS;
    Result.Flags = {mc::MCSectionFlag::Alloc, mc::MCSectionFlag::Writable};
  } else {
    Result.Name = ".data";
    Result.Kind = mc::MCSectionKind::Data;
    Result.Flags = {mc::MCSectionFlag::Alloc, mc::MCSectionFlag::Writable};
  }

  if (Object.Section)
    Result.Name = *Object.Section;
  if (Object.COMDAT) {
    auto Group = Context.symbol(*Object.COMDAT);
    if (!Group)
      return std::unexpected(Group.error());
    Result.Flags.set(mc::MCSectionFlag::Group);
    Result.Group = std::move(*Group);
    if (!Object.Section)
      Result.Name += "." + Object.Symbol;
  }
  return Result;
}

Expected<mc::MCSectionDesc>
RISCVELFTargetObjectFile::constantPoolSection(const MachineFunction &Function,
                                              const MachineConstantPoolEntry &Entry) const {
  const MachineConstantPoolEntryDesc *Description = Entry.description();
  if (Description == nullptr)
    return makeUnexpected(ErrorCode::InvalidArgument, "constant-pool entry is not live");
  if (Description->Placement.Section == ConstantPoolSectionKind::TextAdjacent)
    return textSectionDescription();

  mc::MCSectionDesc Result = readOnlySectionDescription();
  if (Description->Placement.Section == ConstantPoolSectionKind::Data) {
    Result.Name = ".data";
    Result.Kind = mc::MCSectionKind::Data;
    Result.Flags.set(mc::MCSectionFlag::Writable);
  } else if (Description->Placement.Section == ConstantPoolSectionKind::Target) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "RV64GC constant pool has an unknown target section");
  } else if (Description->Placement.Section == ConstantPoolSectionKind::MergeableReadOnly) {
    Result.Name = ".rodata.cst" + std::to_string(Description->Size);
    Result.Flags.set(mc::MCSectionFlag::Merge);
    Result.EntrySize = Description->Size;
  }
  if (Description->Placement.UniqueSection) {
    Result.Name += "." + std::to_string(Function.stableID().value()) + "." +
                   std::to_string(Entry.index().value());
    Result.UniqueID = static_cast<std::uint64_t>(Entry.index().value()) + 1U;
  }
  Result.Alignment = Description->Align.bytes();
  return Result;
}

Expected<mc::MCSectionDesc>
RISCVELFTargetObjectFile::jumpTableSection(const MachineFunction &Function,
                                           const MachineJumpTableEntry &Entry) const {
  const MachineJumpTableDesc *Description = Entry.description();
  if (Description == nullptr)
    return makeUnexpected(ErrorCode::InvalidArgument, "jump-table entry is not live");
  mc::MCSectionDesc Result = Description->Placement.InTextSection ? textSectionDescription()
                                                                  : readOnlySectionDescription();
  if (Description->Placement.UniqueSection) {
    Result.Name += "." + std::to_string(Function.stableID().value()) + ".jt." +
                   std::to_string(Entry.index().value());
    Result.UniqueID = static_cast<std::uint64_t>(Entry.index().value()) + 1U;
  }
  Result.Alignment = Description->Align.bytes();
  return Result;
}

} // namespace rcc::riscv
