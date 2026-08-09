// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/MC/ELFObjectWriter.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rcc::mc {
namespace {

constexpr std::uint32_t SHTNull = 0;
constexpr std::uint32_t SHTProgBits = 1;
constexpr std::uint32_t SHTSymTab = 2;
constexpr std::uint32_t SHTStrTab = 3;
constexpr std::uint32_t SHTRela = 4;
constexpr std::uint32_t SHTNoBits = 8;
constexpr std::uint32_t SHTRel = 9;
constexpr std::uint32_t SHTGroup = 17;
constexpr std::uint32_t SHTSymTabShndx = 18;
constexpr std::uint16_t SHNUndefined = 0;
constexpr std::uint32_t SHNLoReserve = 0xff00U;
constexpr std::uint16_t SHNCommon = 0xfff2U;
constexpr std::uint32_t GRPComdat = 1;

constexpr std::uint64_t SHFWrite = 0x1;
constexpr std::uint64_t SHFAlloc = 0x2;
constexpr std::uint64_t SHFExecInstr = 0x4;
constexpr std::uint64_t SHFMerge = 0x10;
constexpr std::uint64_t SHFStrings = 0x20;
constexpr std::uint64_t SHFInfoLink = 0x40;
constexpr std::uint64_t SHFGroup = 0x200;
constexpr std::uint64_t SHFTLS = 0x400;
constexpr std::uint64_t SHFGNURetain = 0x200000;

class StringTable final {
public:
  StringTable() {
    Bytes.push_back(0);
  }

  [[nodiscard]] Expected<std::uint32_t> add(std::string_view Value) {
    const auto Existing = Offsets.find(std::string(Value));
    if (Existing != Offsets.end())
      return Existing->second;
    constexpr std::size_t Maximum = std::numeric_limits<std::uint32_t>::max();
    if (Value.size() > Maximum - 1U || Bytes.size() > Maximum - Value.size() - 1U) {
      return makeUnexpected(ErrorCode::ObjectWritingFailed, "ELF string table exceeds 4 GiB");
    }
    const auto Offset = static_cast<std::uint32_t>(Bytes.size());
    Bytes.insert(Bytes.end(), Value.begin(), Value.end());
    Bytes.push_back(0);
    Offsets.emplace(std::string(Value), Offset);
    return Offset;
  }

  [[nodiscard]] const std::vector<std::uint8_t> &bytes() const noexcept {
    return Bytes;
  }

private:
  std::vector<std::uint8_t> Bytes;
  std::unordered_map<std::string, std::uint32_t> Offsets;
};

struct OutputSymbol final {
  std::uint32_t Name = 0;
  std::uint8_t Info = 0;
  std::uint8_t Other = 0;
  std::uint16_t Section = SHNUndefined;
  std::uint64_t Value = 0;
  std::uint64_t Size = 0;
  const MCSymbol *Identity = nullptr;
};

struct OutputSection final {
  std::string Name;
  std::uint32_t NameOffset = 0;
  std::uint32_t Type = SHTProgBits;
  std::uint64_t Flags = 0;
  std::uint64_t Alignment = 1;
  std::uint64_t EntrySize = 0;
  std::uint32_t Link = 0;
  std::uint32_t Info = 0;
  std::vector<std::uint8_t> Contents;
  std::uint64_t AddressSize = 0;
  std::uint64_t FileOffset = 0;
  const MCSection *Input = nullptr;
  std::optional<std::size_t> RelocationTarget;
  MCSymbolRef GroupSignature;
};

[[nodiscard]] bool isPowerOfTwo(std::uint64_t Value) noexcept {
  return Value != 0 && (Value & (Value - 1U)) == 0;
}

[[nodiscard]] bool isWriterManagedSectionType(std::uint32_t Type) noexcept {
  switch (Type) {
  case SHTNull:
  case SHTSymTab:
  case SHTStrTab:
  case SHTRela:
  case SHTNoBits:
  case SHTRel:
  case SHTGroup:
  case SHTSymTabShndx:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] Expected<std::uint64_t> checkedAdd(std::uint64_t Left, std::uint64_t Right,
                                                 std::string_view Message) {
  std::uint64_t Result = 0;
  if (__builtin_add_overflow(Left, Right, &Result))
    return makeUnexpected(ErrorCode::ObjectWritingFailed, std::string(Message));
  return Result;
}

[[nodiscard]] Expected<std::uint64_t> alignTo(std::uint64_t Value, std::uint64_t Alignment) {
  if (!isPowerOfTwo(Alignment))
    return makeUnexpected(ErrorCode::ObjectWritingFailed, "ELF alignment is not a power of two");
  auto Adjusted = checkedAdd(Value, Alignment - 1U, "ELF file offset overflows");
  if (!Adjusted)
    return std::unexpected(Adjusted.error());
  return *Adjusted & ~(Alignment - 1U);
}

void appendU16(std::vector<std::uint8_t> &Output, std::uint16_t Value) {
  Output.push_back(static_cast<std::uint8_t>(Value));
  Output.push_back(static_cast<std::uint8_t>(Value >> 8U));
}

void appendU32(std::vector<std::uint8_t> &Output, std::uint32_t Value) {
  for (std::uint32_t Shift = 0; Shift < 32; Shift += 8)
    Output.push_back(static_cast<std::uint8_t>(Value >> Shift));
}

void appendU64(std::vector<std::uint8_t> &Output, std::uint64_t Value) {
  for (std::uint32_t Shift = 0; Shift < 64; Shift += 8)
    Output.push_back(static_cast<std::uint8_t>(Value >> Shift));
}

void writeU64(std::vector<std::uint8_t> &Output, std::size_t Offset, std::uint64_t Value) {
  for (std::uint32_t Shift = 0; Shift < 64; Shift += 8)
    Output[Offset + Shift / 8U] = static_cast<std::uint8_t>(Value >> Shift);
}

[[nodiscard]] std::uint64_t sectionFlags(MCSectionFlags Flags) noexcept {
  std::uint64_t Result = 0;
  if (Flags.has(MCSectionFlag::Writable))
    Result |= SHFWrite;
  if (Flags.has(MCSectionFlag::Alloc))
    Result |= SHFAlloc;
  if (Flags.has(MCSectionFlag::Executable))
    Result |= SHFExecInstr;
  if (Flags.has(MCSectionFlag::Merge))
    Result |= SHFMerge;
  if (Flags.has(MCSectionFlag::Strings))
    Result |= SHFStrings;
  if (Flags.has(MCSectionFlag::Group))
    Result |= SHFGroup;
  if (Flags.has(MCSectionFlag::TLS))
    Result |= SHFTLS;
  if (Flags.has(MCSectionFlag::Retain))
    Result |= SHFGNURetain;
  return Result;
}

[[nodiscard]] std::uint8_t symbolBinding(MCObjectSymbolBinding Binding) noexcept {
  switch (Binding) {
  case MCObjectSymbolBinding::Local:
    return 0;
  case MCObjectSymbolBinding::Global:
    return 1;
  case MCObjectSymbolBinding::Weak:
    return 2;
  }
  return 0;
}

[[nodiscard]] std::uint8_t symbolType(MCObjectSymbolType Type) noexcept {
  switch (Type) {
  case MCObjectSymbolType::None:
    return 0;
  case MCObjectSymbolType::Object:
    return 1;
  case MCObjectSymbolType::Function:
    return 2;
  case MCObjectSymbolType::Section:
    return 3;
  case MCObjectSymbolType::TLSObject:
    return 6;
  }
  return 0;
}

[[nodiscard]] std::uint8_t symbolVisibility(MCObjectSymbolVisibility Visibility) noexcept {
  switch (Visibility) {
  case MCObjectSymbolVisibility::Default:
    return 0;
  case MCObjectSymbolVisibility::Hidden:
    return 2;
  case MCObjectSymbolVisibility::Protected:
    return 3;
  }
  return 0;
}

[[nodiscard]] Expected<void> appendAtOffset(std::vector<std::uint8_t> &Output, std::uint64_t Offset,
                                            std::span<const std::uint8_t> Contents) {
  if (Offset > std::numeric_limits<std::size_t>::max() ||
      Contents.size() >
          std::numeric_limits<std::size_t>::max() - static_cast<std::size_t>(Offset)) {
    return makeUnexpected(ErrorCode::ObjectWritingFailed, "ELF output exceeds the host limit");
  }
  const std::size_t Position = static_cast<std::size_t>(Offset);
  if (Output.size() > Position) {
    return makeUnexpected(ErrorCode::ObjectWritingFailed,
                          "ELF output sections overlap in file layout");
  }
  if (Output.size() < Position)
    Output.resize(Position);
  Output.insert(Output.end(), Contents.begin(), Contents.end());
  return {};
}

} // namespace

Expected<std::vector<std::uint8_t>>
ELF64LEObjectWriter::writeObject(const MCAssembledObject &Object) const {
  if (Target.Machine == 0)
    return makeUnexpected(ErrorCode::ObjectWritingFailed, "ELF target machine is not configured");

  constexpr std::size_t ReservedOutputSections = 4;
  constexpr std::size_t MaximumInputAndExtraSections = SHNLoReserve - ReservedOutputSections - 1U;
  if (Object.Sections.size() > MaximumInputAndExtraSections ||
      Target.ExtraSections.size() > MaximumInputAndExtraSections - Object.Sections.size()) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "ELF extended section numbering is not implemented");
  }

  std::vector<OutputSection> Sections;
  Sections.push_back(OutputSection{.Name = {},
                                   .NameOffset = 0,
                                   .Type = SHTNull,
                                   .Flags = 0,
                                   .Alignment = 0,
                                   .EntrySize = 0,
                                   .Link = 0,
                                   .Info = 0,
                                   .Contents = {},
                                   .AddressSize = 0,
                                   .FileOffset = 0,
                                   .Input = nullptr,
                                   .RelocationTarget = std::nullopt,
                                   .GroupSignature = {}});
  const auto RequireSectionCapacity = [&Sections](std::size_t Additional) -> Expected<void> {
    if (Sections.size() >= SHNLoReserve || Additional >= SHNLoReserve - Sections.size()) {
      return makeUnexpected(ErrorCode::Unsupported,
                            "ELF extended section numbering is not implemented");
    }
    return {};
  };
  auto InputCapacity = RequireSectionCapacity(Object.Sections.size());
  if (!InputCapacity)
    return std::unexpected(InputCapacity.error());
  std::vector<std::uint32_t> InputSectionIndices(Object.Sections.size());
  std::unordered_map<const MCSection *, bool> ExistingSections;
  for (std::size_t Index = 0; Index < Object.Sections.size(); ++Index) {
    const MCObjectSection &Input = Object.Sections[Index];
    if (!Input.Section)
      return makeUnexpected(ErrorCode::ObjectWritingFailed, "ELF input section is null");
    if (!ExistingSections.emplace(Input.Section.get(), true).second) {
      return makeUnexpected(ErrorCode::ObjectWritingFailed,
                            "ELF input contains a duplicate section identity");
    }
    const MCSectionDesc &Description = Input.Section->description();
    auto DescriptionVerification = Description.verify();
    if (!DescriptionVerification)
      return std::unexpected(DescriptionVerification.error());
    const bool NoBits =
        Description.Kind == MCSectionKind::BSS || Description.Kind == MCSectionKind::TLSBSS;
    if (!isPowerOfTwo(Input.Alignment) || Input.Alignment < Description.Alignment) {
      return makeUnexpected(ErrorCode::ObjectWritingFailed,
                            "ELF input section has an invalid effective alignment");
    }
    if ((!NoBits && Input.AddressSize != Input.Contents.size()) ||
        (NoBits && !Input.Contents.empty())) {
      return makeUnexpected(ErrorCode::ObjectWritingFailed,
                            "ELF input section contents disagree with its address size");
    }
    if (NoBits && !Input.Relocations.empty()) {
      return makeUnexpected(ErrorCode::ObjectWritingFailed,
                            "ELF NOBITS section cannot contain relocations");
    }
    const auto OutputIndex = static_cast<std::uint32_t>(Sections.size());
    InputSectionIndices[Index] = OutputIndex;
    Sections.push_back(
        OutputSection{.Name = Description.Name,
                      .NameOffset = 0,
                      .Type = NoBits ? SHTNoBits : SHTProgBits,
                      .Flags = sectionFlags(Description.Flags),
                      .Alignment = Input.Alignment,
                      .EntrySize = Description.EntrySize,
                      .Link = 0,
                      .Info = 0,
                      .Contents = NoBits ? std::vector<std::uint8_t>{} : Input.Contents,
                      .AddressSize = Input.AddressSize,
                      .FileOffset = 0,
                      .Input = Input.Section.get(),
                      .RelocationTarget = std::nullopt,
                      .GroupSignature = Description.Group});
  }

  for (const ELFExtraSection &Extra : Target.ExtraSections) {
    if (Extra.Name.empty() || isWriterManagedSectionType(Extra.Type) ||
        !isPowerOfTwo(Extra.Alignment) || (Extra.Flags & (SHFInfoLink | SHFGroup)) != 0 ||
        (Extra.EntrySize != 0 && Extra.Contents.size() % Extra.EntrySize != 0)) {
      return makeUnexpected(ErrorCode::ObjectWritingFailed,
                            "ELF extra section has an invalid description");
    }
    auto ExtraCapacity = RequireSectionCapacity(1);
    if (!ExtraCapacity)
      return std::unexpected(ExtraCapacity.error());
    Sections.push_back(OutputSection{.Name = Extra.Name,
                                     .NameOffset = 0,
                                     .Type = Extra.Type,
                                     .Flags = Extra.Flags,
                                     .Alignment = Extra.Alignment,
                                     .EntrySize = Extra.EntrySize,
                                     .Link = 0,
                                     .Info = 0,
                                     .Contents = Extra.Contents,
                                     .AddressSize = Extra.Contents.size(),
                                     .FileOffset = 0,
                                     .Input = nullptr,
                                     .RelocationTarget = std::nullopt,
                                     .GroupSignature = {}});
  }

  StringTable SymbolNames;
  std::vector<OutputSymbol> LocalSymbols;
  std::vector<OutputSymbol> GlobalSymbols;
  std::unordered_map<const MCSymbol *, bool> ExistingSymbols;
  for (const MCObjectSymbol &Input : Object.Symbols) {
    if (!Input.Symbol)
      return makeUnexpected(ErrorCode::ObjectWritingFailed, "ELF input symbol is null");
    if (!ExistingSymbols.emplace(Input.Symbol.get(), true).second) {
      return makeUnexpected(ErrorCode::ObjectWritingFailed,
                            "ELF input contains a duplicate symbol identity");
    }
    auto Name = SymbolNames.add(Input.Symbol->name());
    if (!Name)
      return std::unexpected(Name.error());
    std::uint16_t Section = SHNUndefined;
    std::uint64_t Value = Input.Value;
    if (Input.CommonAlignment != 0) {
      if (!isPowerOfTwo(Input.CommonAlignment) || Input.Size == 0 || Input.SectionIndex ||
          Input.Binding == MCObjectSymbolBinding::Local || Input.Value != Input.CommonAlignment) {
        return makeUnexpected(ErrorCode::ObjectWritingFailed,
                              "ELF common symbol has an invalid definition");
      }
      Section = SHNCommon;
      Value = Input.CommonAlignment;
    } else if (Input.SectionIndex) {
      if (*Input.SectionIndex >= InputSectionIndices.size()) {
        return makeUnexpected(ErrorCode::ObjectWritingFailed,
                              "ELF symbol references an invalid section");
      }
      const std::uint32_t Mapped = InputSectionIndices[*Input.SectionIndex];
      if (Mapped >= SHNLoReserve) {
        return makeUnexpected(ErrorCode::Unsupported,
                              "ELF extended section indices are not implemented");
      }
      Section = static_cast<std::uint16_t>(Mapped);
      const std::uint64_t SectionSize = Object.Sections[*Input.SectionIndex].AddressSize;
      if (Input.Value > SectionSize || Input.Size > SectionSize - Input.Value) {
        return makeUnexpected(ErrorCode::ObjectWritingFailed,
                              "ELF symbol range exceeds its section");
      }
    } else if (Input.Value != 0) {
      return makeUnexpected(ErrorCode::ObjectWritingFailed,
                            "ELF undefined symbol has a nonzero value");
    }
    OutputSymbol Symbol{.Name = *Name,
                        .Info = static_cast<std::uint8_t>((symbolBinding(Input.Binding) << 4U) |
                                                          symbolType(Input.Type)),
                        .Other = symbolVisibility(Input.Visibility),
                        .Section = Section,
                        .Value = Value,
                        .Size = Input.Size,
                        .Identity = Input.Symbol.get()};
    if (Input.Binding == MCObjectSymbolBinding::Local)
      LocalSymbols.push_back(Symbol);
    else
      GlobalSymbols.push_back(Symbol);
  }

  for (const OutputSection &Section : Sections) {
    if (!Section.GroupSignature || ExistingSymbols.contains(Section.GroupSignature.get()))
      continue;
    auto Name = SymbolNames.add(Section.GroupSignature->name());
    if (!Name)
      return std::unexpected(Name.error());
    GlobalSymbols.push_back(OutputSymbol{.Name = *Name,
                                         .Info = static_cast<std::uint8_t>(1U << 4U),
                                         .Other = 0,
                                         .Section = SHNUndefined,
                                         .Value = 0,
                                         .Size = 0,
                                         .Identity = Section.GroupSignature.get()});
    ExistingSymbols.emplace(Section.GroupSignature.get(), true);
  }

  std::vector<OutputSymbol> Symbols;
  Symbols.push_back(OutputSymbol{});
  for (std::size_t Index = 1; Index < Sections.size(); ++Index) {
    Symbols.push_back(OutputSymbol{.Name = 0,
                                   .Info = static_cast<std::uint8_t>(3),
                                   .Other = 0,
                                   .Section = static_cast<std::uint16_t>(Index),
                                   .Value = 0,
                                   .Size = 0,
                                   .Identity = nullptr});
  }
  constexpr std::size_t MaximumSymbolCount = std::numeric_limits<std::uint32_t>::max();
  if (LocalSymbols.size() > MaximumSymbolCount - Symbols.size()) {
    return makeUnexpected(ErrorCode::Unsupported, "ELF symbol index exceeds 32 bits");
  }
  Symbols.insert(Symbols.end(), LocalSymbols.begin(), LocalSymbols.end());
  const std::uint32_t FirstGlobal = static_cast<std::uint32_t>(Symbols.size());
  if (GlobalSymbols.size() > MaximumSymbolCount - Symbols.size()) {
    return makeUnexpected(ErrorCode::Unsupported, "ELF symbol index exceeds 32 bits");
  }
  Symbols.insert(Symbols.end(), GlobalSymbols.begin(), GlobalSymbols.end());
  std::unordered_map<const MCSymbol *, std::uint32_t> SymbolIndices;
  for (std::size_t Index = 0; Index < Symbols.size(); ++Index) {
    if (Symbols[Index].Identity)
      SymbolIndices.emplace(Symbols[Index].Identity, static_cast<std::uint32_t>(Index));
  }

  for (std::size_t InputIndex = 0; InputIndex < Object.Sections.size(); ++InputIndex) {
    const MCObjectSection &Input = Object.Sections[InputIndex];
    if (Input.Relocations.empty())
      continue;
    if (Input.Relocations.size() > std::numeric_limits<std::size_t>::max() / 24U) {
      return makeUnexpected(ErrorCode::ObjectWritingFailed,
                            "ELF relocation section exceeds the host limit");
    }
    std::vector<std::uint8_t> Contents;
    Contents.reserve(Input.Relocations.size() * 24U);
    for (const MCObjectRelocation &Relocation : Input.Relocations) {
      const auto Symbol = SymbolIndices.find(Relocation.Symbol.get());
      if (!Relocation.Symbol || Symbol == SymbolIndices.end()) {
        return makeUnexpected(ErrorCode::ObjectWritingFailed,
                              "ELF relocation references an unknown symbol");
      }
      if (Relocation.Type == 0 || Relocation.Offset >= Input.AddressSize) {
        return makeUnexpected(ErrorCode::ObjectWritingFailed,
                              "ELF relocation has an invalid type or section offset");
      }
      appendU64(Contents, Relocation.Offset);
      appendU64(Contents, (static_cast<std::uint64_t>(Symbol->second) << 32U) | Relocation.Type);
      appendU64(Contents, static_cast<std::uint64_t>(Relocation.Addend));
    }
    const std::uint32_t TargetSection = InputSectionIndices[InputIndex];
    const MCSymbolRef GroupSignature = Sections[TargetSection].GroupSignature;
    auto RelocationCapacity = RequireSectionCapacity(1);
    if (!RelocationCapacity)
      return std::unexpected(RelocationCapacity.error());
    Sections.push_back(OutputSection{.Name = ".rela" + std::string(Input.Section->name()),
                                     .NameOffset = 0,
                                     .Type = SHTRela,
                                     .Flags = SHFInfoLink | (GroupSignature ? SHFGroup : 0),
                                     .Alignment = 8,
                                     .EntrySize = 24,
                                     .Link = 0,
                                     .Info = TargetSection,
                                     .Contents = std::move(Contents),
                                     .AddressSize = 0,
                                     .FileOffset = 0,
                                     .Input = nullptr,
                                     .RelocationTarget = InputIndex,
                                     .GroupSignature = GroupSignature});
  }

  struct PendingGroup final {
    MCSymbolRef Signature;
    std::vector<std::uint32_t> Members;
  };
  std::vector<PendingGroup> Groups;
  for (std::size_t SectionIndex = 1; SectionIndex < Sections.size(); ++SectionIndex) {
    if (!Sections[SectionIndex].GroupSignature)
      continue;
    const auto Existing = std::ranges::find_if(Groups, [&](const PendingGroup &Group) {
      return Group.Signature == Sections[SectionIndex].GroupSignature;
    });
    if (Existing == Groups.end()) {
      Groups.push_back(PendingGroup{
          .Signature = Sections[SectionIndex].GroupSignature,
          .Members = {static_cast<std::uint32_t>(SectionIndex)},
      });
    } else {
      Existing->Members.push_back(static_cast<std::uint32_t>(SectionIndex));
    }
  }
  for (const PendingGroup &Group : Groups) {
    const auto Signature = SymbolIndices.find(Group.Signature.get());
    if (Signature == SymbolIndices.end() || (Symbols[Signature->second].Info >> 4U) == 0) {
      return makeUnexpected(ErrorCode::ObjectWritingFailed,
                            "ELF COMDAT group requires a nonlocal signature symbol");
    }
    std::vector<std::uint8_t> Contents;
    appendU32(Contents, GRPComdat);
    for (const std::uint32_t Member : Group.Members)
      appendU32(Contents, Member);
    auto GroupCapacity = RequireSectionCapacity(1);
    if (!GroupCapacity)
      return std::unexpected(GroupCapacity.error());
    Sections.push_back(OutputSection{.Name = ".group",
                                     .NameOffset = 0,
                                     .Type = SHTGroup,
                                     .Flags = 0,
                                     .Alignment = 4,
                                     .EntrySize = 4,
                                     .Link = 0,
                                     .Info = Signature->second,
                                     .Contents = std::move(Contents),
                                     .AddressSize = 0,
                                     .FileOffset = 0,
                                     .Input = nullptr,
                                     .RelocationTarget = std::nullopt,
                                     .GroupSignature = {}});
  }

  auto MetadataCapacity = RequireSectionCapacity(3);
  if (!MetadataCapacity)
    return std::unexpected(MetadataCapacity.error());
  const std::uint32_t SymTabIndex = static_cast<std::uint32_t>(Sections.size());
  const std::uint32_t StrTabIndex = SymTabIndex + 1U;
  for (OutputSection &Section : Sections) {
    if (Section.Type == SHTGroup || Section.Type == SHTRela)
      Section.Link = SymTabIndex;
  }

  std::vector<std::uint8_t> SymbolContents;
  if (Symbols.size() > std::numeric_limits<std::size_t>::max() / 24U) {
    return makeUnexpected(ErrorCode::ObjectWritingFailed,
                          "ELF symbol table exceeds the host limit");
  }
  SymbolContents.reserve(Symbols.size() * 24U);
  for (const OutputSymbol &Symbol : Symbols) {
    appendU32(SymbolContents, Symbol.Name);
    SymbolContents.push_back(Symbol.Info);
    SymbolContents.push_back(Symbol.Other);
    appendU16(SymbolContents, Symbol.Section);
    appendU64(SymbolContents, Symbol.Value);
    appendU64(SymbolContents, Symbol.Size);
  }
  Sections.push_back(OutputSection{.Name = ".symtab",
                                   .NameOffset = 0,
                                   .Type = SHTSymTab,
                                   .Flags = 0,
                                   .Alignment = 8,
                                   .EntrySize = 24,
                                   .Link = StrTabIndex,
                                   .Info = FirstGlobal,
                                   .Contents = std::move(SymbolContents),
                                   .AddressSize = 0,
                                   .FileOffset = 0,
                                   .Input = nullptr,
                                   .RelocationTarget = std::nullopt,
                                   .GroupSignature = {}});
  Sections.push_back(OutputSection{.Name = ".strtab",
                                   .NameOffset = 0,
                                   .Type = SHTStrTab,
                                   .Flags = 0,
                                   .Alignment = 1,
                                   .EntrySize = 0,
                                   .Link = 0,
                                   .Info = 0,
                                   .Contents = SymbolNames.bytes(),
                                   .AddressSize = 0,
                                   .FileOffset = 0,
                                   .Input = nullptr,
                                   .RelocationTarget = std::nullopt,
                                   .GroupSignature = {}});

  StringTable SectionNames;
  for (OutputSection &Section : Sections) {
    auto Name = SectionNames.add(Section.Name);
    if (!Name)
      return std::unexpected(Name.error());
    Section.NameOffset = *Name;
  }
  const std::uint32_t ShStrTabIndex = static_cast<std::uint32_t>(Sections.size());
  auto ShStrName = SectionNames.add(".shstrtab");
  if (!ShStrName)
    return std::unexpected(ShStrName.error());
  Sections.push_back(OutputSection{.Name = ".shstrtab",
                                   .NameOffset = *ShStrName,
                                   .Type = SHTStrTab,
                                   .Flags = 0,
                                   .Alignment = 1,
                                   .EntrySize = 0,
                                   .Link = 0,
                                   .Info = 0,
                                   .Contents = SectionNames.bytes(),
                                   .AddressSize = 0,
                                   .FileOffset = 0,
                                   .Input = nullptr,
                                   .RelocationTarget = std::nullopt,
                                   .GroupSignature = {}});

  if (Sections.size() >= SHNLoReserve) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "ELF extended section numbering is not implemented");
  }

  std::uint64_t FileOffset = 64;
  for (std::size_t Index = 1; Index < Sections.size(); ++Index) {
    OutputSection &Section = Sections[Index];
    auto Aligned = alignTo(FileOffset, Section.Alignment);
    if (!Aligned)
      return std::unexpected(Aligned.error());
    Section.FileOffset = *Aligned;
    if (Section.Type == SHTNoBits)
      continue;
    auto End = checkedAdd(*Aligned, Section.Contents.size(), "ELF section file range overflows");
    if (!End)
      return std::unexpected(End.error());
    FileOffset = *End;
  }
  auto SectionHeaderOffset = alignTo(FileOffset, 8);
  if (!SectionHeaderOffset)
    return std::unexpected(SectionHeaderOffset.error());
  if (Sections.size() > std::numeric_limits<std::uint64_t>::max() / 64U) {
    return makeUnexpected(ErrorCode::ObjectWritingFailed,
                          "ELF section header table exceeds the host limit");
  }
  auto End = checkedAdd(*SectionHeaderOffset, static_cast<std::uint64_t>(Sections.size()) * 64U,
                        "ELF section header table overflows");
  if (!End || *End > std::numeric_limits<std::size_t>::max()) {
    return makeUnexpected(ErrorCode::ObjectWritingFailed, "ELF output exceeds the host limit");
  }

  std::vector<std::uint8_t> Output(64, 0);
  Output[0] = 0x7f;
  Output[1] = 'E';
  Output[2] = 'L';
  Output[3] = 'F';
  Output[4] = 2;
  Output[5] = 1;
  Output[6] = 1;
  Output[7] = Target.OSABI;
  Output[8] = Target.ABIVersion;
  Output[16] = 1;
  Output[18] = static_cast<std::uint8_t>(Target.Machine);
  Output[19] = static_cast<std::uint8_t>(Target.Machine >> 8U);
  Output[20] = 1;
  writeU64(Output, 40, *SectionHeaderOffset);
  Output[48] = static_cast<std::uint8_t>(Target.Flags | Object.Flags);
  Output[49] = static_cast<std::uint8_t>((Target.Flags | Object.Flags) >> 8U);
  Output[50] = static_cast<std::uint8_t>((Target.Flags | Object.Flags) >> 16U);
  Output[51] = static_cast<std::uint8_t>((Target.Flags | Object.Flags) >> 24U);
  Output[52] = 64;
  Output[58] = 64;
  Output[60] = static_cast<std::uint8_t>(Sections.size());
  Output[61] = static_cast<std::uint8_t>(Sections.size() >> 8U);
  Output[62] = static_cast<std::uint8_t>(ShStrTabIndex);
  Output[63] = static_cast<std::uint8_t>(ShStrTabIndex >> 8U);

  for (std::size_t Index = 1; Index < Sections.size(); ++Index) {
    if (Sections[Index].Type == SHTNoBits)
      continue;
    auto Appended = appendAtOffset(Output, Sections[Index].FileOffset, Sections[Index].Contents);
    if (!Appended)
      return std::unexpected(Appended.error());
  }
  if (Output.size() < *SectionHeaderOffset)
    Output.resize(static_cast<std::size_t>(*SectionHeaderOffset));
  for (const OutputSection &Section : Sections) {
    appendU32(Output, Section.NameOffset);
    appendU32(Output, Section.Type);
    appendU64(Output, Section.Flags);
    appendU64(Output, 0);
    appendU64(Output, Section.FileOffset);
    appendU64(Output, Section.Type == SHTNoBits ? Section.AddressSize : Section.Contents.size());
    appendU32(Output, Section.Link);
    appendU32(Output, Section.Info);
    appendU64(Output, Section.Alignment);
    appendU64(Output, Section.EntrySize);
  }
  return Output;
}

} // namespace rcc::mc
