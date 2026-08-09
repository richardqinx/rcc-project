// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/MC/MCAssembler.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace rcc::mc {
namespace {

[[nodiscard]] bool isPowerOfTwo(std::uint64_t Value) noexcept {
  return Value != 0 && (Value & (Value - 1U)) == 0;
}

[[nodiscard]] Expected<std::uint64_t> checkedAdd(std::uint64_t Left, std::uint64_t Right,
                                                 std::string_view Message) {
  std::uint64_t Result = 0;
  if (__builtin_add_overflow(Left, Right, &Result))
    return makeUnexpected(ErrorCode::ObjectWritingFailed, std::string(Message));
  return Result;
}

[[nodiscard]] Expected<std::int64_t> checkedAdd(std::int64_t Left, std::int64_t Right,
                                                std::string_view Message) {
  std::int64_t Result = 0;
  if (__builtin_add_overflow(Left, Right, &Result))
    return makeUnexpected(ErrorCode::RelocationFailed, std::string(Message));
  return Result;
}

[[nodiscard]] Expected<std::int64_t> checkedSubtract(std::int64_t Left, std::int64_t Right,
                                                     std::string_view Message) {
  std::int64_t Result = 0;
  if (__builtin_sub_overflow(Left, Right, &Result))
    return makeUnexpected(ErrorCode::RelocationFailed, std::string(Message));
  return Result;
}

[[nodiscard]] Expected<std::int64_t> signedOffset(std::uint64_t Value, std::string_view Message) {
  if (Value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    return makeUnexpected(ErrorCode::RelocationFailed, std::string(Message));
  return static_cast<std::int64_t>(Value);
}

[[nodiscard]] Expected<std::uint64_t> alignmentPadding(std::uint64_t Offset,
                                                       std::uint64_t Alignment) {
  if (!isPowerOfTwo(Alignment))
    return makeUnexpected(ErrorCode::InvalidArgument, "MC alignment is not a power of two");
  const std::uint64_t Mask = Alignment - 1U;
  auto Adjusted = checkedAdd(Offset, Mask, "MC section alignment overflows");
  if (!Adjusted)
    return std::unexpected(Adjusted.error());
  return (*Adjusted & ~Mask) - Offset;
}

[[nodiscard]] MCFixupKind dataFixupKind(std::uint8_t Size) noexcept {
  switch (Size) {
  case 1:
    return fixup_kind::Data1;
  case 2:
    return fixup_kind::Data2;
  case 4:
    return fixup_kind::Data4;
  case 8:
    return fixup_kind::Data8;
  default:
    return fixup_kind::None;
  }
}

void appendLittleEndian(std::vector<std::uint8_t> &Bytes, std::uint64_t Value, std::uint8_t Size) {
  for (std::uint8_t Index = 0; Index < Size; ++Index)
    Bytes.push_back(static_cast<std::uint8_t>(Value >> (Index * 8U)));
}

[[nodiscard]] bool isNoBits(const MCSection &Section) noexcept {
  return Section.description().Kind == MCSectionKind::BSS ||
         Section.description().Kind == MCSectionKind::TLSBSS;
}

} // namespace

Expected<std::size_t> MCAssembler::currentSectionIndex() const {
  if (WasAssembled)
    return makeUnexpected(ErrorCode::ObjectWritingFailed, "MC assembler is already finalized");
  if (!CurrentSection)
    return makeUnexpected(ErrorCode::ObjectWritingFailed,
                          "object emission requires an active MC section");
  return *CurrentSection;
}

MCAssembler::SymbolData &MCAssembler::getOrCreateSymbol(MCSymbolRef Symbol) {
  const auto Position = SymbolIndices.find(Symbol.get());
  if (Position != SymbolIndices.end())
    return Symbols[Position->second];
  const std::size_t Index = Symbols.size();
  Symbols.push_back(SymbolData{.Symbol = std::move(Symbol),
                               .Binding = MCObjectSymbolBinding::Local,
                               .BindingWasSet = false,
                               .Type = MCObjectSymbolType::None,
                               .Visibility = MCObjectSymbolVisibility::Default,
                               .SectionIndex = std::nullopt,
                               .FragmentIndex = 0,
                               .Size = {},
                               .CommonSize = 0,
                               .CommonAlignment = 0});
  SymbolIndices.emplace(Symbols.back().Symbol.get(), Index);
  return Symbols.back();
}

std::optional<std::uint64_t> MCAssembler::symbolOffset(const SymbolData &Symbol) const {
  if (!Symbol.SectionIndex || *Symbol.SectionIndex >= Sections.size())
    return std::nullopt;
  const SectionData &Section = Sections[*Symbol.SectionIndex];
  if (Symbol.FragmentIndex > Section.Fragments.size())
    return std::nullopt;
  if (Symbol.FragmentIndex == Section.Fragments.size())
    return Section.AddressSize;
  return Section.Fragments[Symbol.FragmentIndex].Offset;
}

Expected<void> MCAssembler::switchSection(MCSectionRef Section) {
  if (WasAssembled)
    return makeUnexpected(ErrorCode::ObjectWritingFailed, "MC assembler is already finalized");
  if (!Section)
    return makeUnexpected(ErrorCode::InvalidArgument, "cannot switch to a null MC section");
  auto Verification = Section->description().verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  const auto Existing = SectionIndices.find(Section.get());
  if (Existing != SectionIndices.end()) {
    CurrentSection = Existing->second;
    return {};
  }
  const std::size_t Index = Sections.size();
  const std::uint64_t Alignment = Section->description().Alignment;
  Sections.push_back(SectionData{
      .Section = std::move(Section), .Fragments = {}, .AddressSize = 0, .Alignment = Alignment});
  SectionIndices.emplace(Sections.back().Section.get(), Index);
  CurrentSection = Index;
  return {};
}

Expected<void> MCAssembler::emitLabel(MCSymbolRef Symbol) {
  auto SectionIndex = currentSectionIndex();
  if (!SectionIndex)
    return std::unexpected(SectionIndex.error());
  if (!Symbol)
    return makeUnexpected(ErrorCode::InvalidArgument, "cannot emit a null MC symbol label");
  SymbolData &Data = getOrCreateSymbol(std::move(Symbol));
  if (Data.SectionIndex || Data.CommonSize != 0) {
    return makeUnexpected(ErrorCode::ObjectWritingFailed, "MC symbol is defined more than once");
  }
  Data.SectionIndex = *SectionIndex;
  Data.FragmentIndex = Sections[*SectionIndex].Fragments.size();
  return {};
}

Expected<void> MCAssembler::emitInstruction(const MCInst &Instruction) {
  auto SectionIndex = currentSectionIndex();
  if (!SectionIndex)
    return std::unexpected(SectionIndex.error());
  if (!Instruction.opcode().isValid())
    return makeUnexpected(ErrorCode::InvalidArgument, "cannot encode an invalid MC opcode");
  Sections[*SectionIndex].Fragments.push_back(
      Fragment{.Payload = InstructionFragment{.Instruction = Instruction},
               .Offset = 0,
               .Size = 0,
               .EncodedBytes = {},
               .Fixups = {}});
  return {};
}

Expected<void> MCAssembler::emitValue(MCExprRef Value, std::uint8_t Size) {
  auto SectionIndex = currentSectionIndex();
  if (!SectionIndex)
    return std::unexpected(SectionIndex.error());
  const MCFixupKind Kind = dataFixupKind(Size);
  if (!Value || Kind == fixup_kind::None) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "MC value requires an expression and a supported size");
  }
  DataFragment Data;
  if (const auto Absolute = Value->evaluateAbsolute()) {
    appendLittleEndian(Data.Bytes, static_cast<std::uint64_t>(*Absolute), Size);
  } else {
    Data.Bytes.resize(Size);
    Data.Fixups.push_back(MCFixup{.Offset = 0, .Value = std::move(Value), .Kind = Kind});
  }
  Sections[*SectionIndex].Fragments.push_back(Fragment{
      .Payload = std::move(Data), .Offset = 0, .Size = 0, .EncodedBytes = {}, .Fixups = {}});
  return {};
}

Expected<void> MCAssembler::emitBytes(std::span<const std::uint8_t> Bytes) {
  auto SectionIndex = currentSectionIndex();
  if (!SectionIndex)
    return std::unexpected(SectionIndex.error());
  if (Bytes.empty())
    return {};
  Sections[*SectionIndex].Fragments.push_back(
      Fragment{.Payload = DataFragment{.Bytes = {Bytes.begin(), Bytes.end()}, .Fixups = {}},
               .Offset = 0,
               .Size = 0,
               .EncodedBytes = {},
               .Fixups = {}});
  return {};
}

Expected<void> MCAssembler::emitZeroFill(std::uint64_t Size) {
  auto SectionIndex = currentSectionIndex();
  if (!SectionIndex)
    return std::unexpected(SectionIndex.error());
  if (Size == 0)
    return {};
  Sections[*SectionIndex].Fragments.push_back(Fragment{.Payload = ZeroFillFragment{.Size = Size},
                                                       .Offset = 0,
                                                       .Size = 0,
                                                       .EncodedBytes = {},
                                                       .Fixups = {}});
  return {};
}

Expected<void> MCAssembler::emitAlignment(std::uint64_t Alignment, std::uint8_t Fill,
                                          std::uint64_t MaximumBytes) {
  auto SectionIndex = currentSectionIndex();
  if (!SectionIndex)
    return std::unexpected(SectionIndex.error());
  if (!isPowerOfTwo(Alignment))
    return makeUnexpected(ErrorCode::InvalidArgument, "MC alignment is not a power of two");
  Sections[*SectionIndex].Fragments.push_back(Fragment{
      .Payload =
          AlignmentFragment{.Alignment = Alignment, .Fill = Fill, .MaximumBytes = MaximumBytes},
      .Offset = 0,
      .Size = 0,
      .EncodedBytes = {},
      .Fixups = {}});
  return {};
}

Expected<void> MCAssembler::emitSymbolAttribute(MCSymbolRef Symbol, MCSymbolAttribute Attribute) {
  if (WasAssembled)
    return makeUnexpected(ErrorCode::ObjectWritingFailed, "MC assembler is already finalized");
  if (!Symbol)
    return makeUnexpected(ErrorCode::InvalidArgument, "MC symbol attribute has no symbol");
  SymbolData &Data = getOrCreateSymbol(std::move(Symbol));
  switch (Attribute) {
  case MCSymbolAttribute::Global:
    Data.Binding = MCObjectSymbolBinding::Global;
    Data.BindingWasSet = true;
    break;
  case MCSymbolAttribute::Weak:
    Data.Binding = MCObjectSymbolBinding::Weak;
    Data.BindingWasSet = true;
    break;
  case MCSymbolAttribute::Local:
    Data.Binding = MCObjectSymbolBinding::Local;
    Data.BindingWasSet = true;
    break;
  case MCSymbolAttribute::Hidden:
    Data.Visibility = MCObjectSymbolVisibility::Hidden;
    break;
  case MCSymbolAttribute::Protected:
    Data.Visibility = MCObjectSymbolVisibility::Protected;
    break;
  case MCSymbolAttribute::Function:
    Data.Type = MCObjectSymbolType::Function;
    break;
  case MCSymbolAttribute::Object:
    Data.Type = MCObjectSymbolType::Object;
    break;
  case MCSymbolAttribute::TLSObject:
    Data.Type = MCObjectSymbolType::TLSObject;
    break;
  }
  return {};
}

Expected<void> MCAssembler::emitSymbolSize(MCSymbolRef Symbol, MCExprRef Size) {
  if (WasAssembled)
    return makeUnexpected(ErrorCode::ObjectWritingFailed, "MC assembler is already finalized");
  if (!Symbol || !Size)
    return makeUnexpected(ErrorCode::InvalidArgument, "MC symbol size is incomplete");
  SymbolData &Data = getOrCreateSymbol(std::move(Symbol));
  if (Data.Size)
    return makeUnexpected(ErrorCode::ObjectWritingFailed, "MC symbol size is defined twice");
  Data.Size = std::move(Size);
  return {};
}

Expected<void> MCAssembler::emitCommonSymbol(MCSymbolRef Symbol, std::uint64_t Size,
                                             std::uint64_t Alignment) {
  if (WasAssembled)
    return makeUnexpected(ErrorCode::ObjectWritingFailed, "MC assembler is already finalized");
  if (!Symbol || Size == 0 || !isPowerOfTwo(Alignment)) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "common symbol requires a size and power-of-two alignment");
  }
  SymbolData &Data = getOrCreateSymbol(std::move(Symbol));
  if (Data.SectionIndex || Data.CommonSize != 0)
    return makeUnexpected(ErrorCode::ObjectWritingFailed, "common symbol is defined twice");
  Data.CommonSize = Size;
  Data.CommonAlignment = Alignment;
  if (Data.Binding == MCObjectSymbolBinding::Local)
    Data.Binding = MCObjectSymbolBinding::Global;
  if (Data.Type == MCObjectSymbolType::None)
    Data.Type = MCObjectSymbolType::Object;
  return {};
}

Expected<void> MCAssembler::configureDwarfFrame(MCSectionRef Section, MCDwarfFrameInfo Info) {
  if (WasAssembled || !CFIProcedures.empty() || DwarfFrameInfo) {
    return makeUnexpected(ErrorCode::ObjectWritingFailed,
                          "DWARF frame emission is already configured or active");
  }
  if (!Section || Section->description().Kind != MCSectionKind::EHFrame) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "DWARF frame emission requires an EHFrame section");
  }
  auto SectionVerification = Section->description().verify();
  if (!SectionVerification)
    return std::unexpected(SectionVerification.error());
  auto InfoVerification = Info.verify();
  if (!InfoVerification)
    return std::unexpected(InfoVerification.error());
  EHFrameSection = std::move(Section);
  DwarfFrameInfo = Info;
  return {};
}

Expected<void> MCAssembler::emitCFI(const MCCFIInstruction &Instruction) {
  if (WasAssembled)
    return makeUnexpected(ErrorCode::ObjectWritingFailed, "MC assembler is already finalized");
  auto Verification = Instruction.verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  auto SectionIndex = currentSectionIndex();
  if (!SectionIndex)
    return std::unexpected(SectionIndex.error());
  const std::size_t FragmentIndex = Sections[*SectionIndex].Fragments.size();

  if (Instruction.Operation == MCCFIOperation::StartProc) {
    if (ActiveCFIProcedure) {
      return makeUnexpected(ErrorCode::ObjectWritingFailed,
                            "nested CFI procedures are not permitted");
    }
    MCSymbolRef Function;
    for (auto Position = Symbols.rbegin(); Position != Symbols.rend(); ++Position) {
      if (Position->SectionIndex == SectionIndex && Position->FragmentIndex == FragmentIndex &&
          Position->Symbol && Position->Symbol->kind() == MCSymbolKind::Function) {
        Function = Position->Symbol;
        break;
      }
    }
    if (!Function) {
      return makeUnexpected(ErrorCode::ObjectWritingFailed,
                            "CFI procedure does not begin at a function symbol");
    }
    CFIProcedures.push_back(CFIProcedure{.Function = std::move(Function),
                                         .SectionIndex = *SectionIndex,
                                         .BeginFragmentIndex = FragmentIndex,
                                         .EndFragmentIndex = 0,
                                         .Events = {},
                                         .IsClosed = false});
    ActiveCFIProcedure = CFIProcedures.size() - 1U;
    return {};
  }

  if (!ActiveCFIProcedure) {
    return makeUnexpected(ErrorCode::ObjectWritingFailed, "CFI event appears outside a procedure");
  }
  CFIProcedure &Procedure = CFIProcedures[*ActiveCFIProcedure];
  if (Procedure.SectionIndex != *SectionIndex) {
    return makeUnexpected(ErrorCode::ObjectWritingFailed,
                          "CFI procedure changes sections before it ends");
  }
  if (Instruction.Operation == MCCFIOperation::EndProc) {
    Procedure.EndFragmentIndex = FragmentIndex;
    Procedure.IsClosed = true;
    ActiveCFIProcedure.reset();
    return {};
  }
  if (Instruction.Operation == MCCFIOperation::Label) {
    return emitLabel(Instruction.Symbol);
  }
  Procedure.Events.push_back(CFIEvent{.Instruction = Instruction, .FragmentIndex = FragmentIndex});
  return {};
}

Expected<void> MCAssembler::encodeFragments() {
  for (SectionData &Section : Sections) {
    for (Fragment &Fragment : Section.Fragments) {
      Fragment.EncodedBytes.clear();
      Fragment.Fixups.clear();
      Fragment.Size = 0;
      if (auto *Instruction = std::get_if<InstructionFragment>(&Fragment.Payload)) {
        auto Encoded = Emitter->encodeInstruction(Instruction->Instruction);
        if (!Encoded)
          return std::unexpected(Encoded.error());
        Fragment.EncodedBytes = std::move(Encoded->Bytes);
        Fragment.Fixups = std::move(Encoded->Fixups);
      } else if (auto *Data = std::get_if<DataFragment>(&Fragment.Payload)) {
        Fragment.EncodedBytes = Data->Bytes;
        Fragment.Fixups = Data->Fixups;
      }
      Fragment.Size = Fragment.EncodedBytes.size();
      for (const MCFixup &Fixup : Fragment.Fixups) {
        auto Verification = Fixup.verify();
        if (!Verification)
          return std::unexpected(Verification.error());
        auto KindInfo = Backend->fixupKindInfo(Fixup.Kind);
        if (!KindInfo)
          return std::unexpected(KindInfo.error());
        if (Fixup.PCRelative != KindInfo->PCRelative) {
          return makeUnexpected(ErrorCode::InstructionEncodingFailed,
                                "MC fixup PC-relative state disagrees with its kind");
        }
        if (Fixup.Offset > Fragment.EncodedBytes.size() ||
            KindInfo->StorageSize >
                Fragment.EncodedBytes.size() - static_cast<std::size_t>(Fixup.Offset)) {
          return makeUnexpected(ErrorCode::InstructionEncodingFailed,
                                "MC fixup exceeds its encoded fragment");
        }
      }
    }
  }
  return {};
}

Expected<bool> MCAssembler::layoutAndRelax() {
  std::size_t RelaxableInstructionCount = 0;
  for (const SectionData &Section : Sections) {
    for (const Fragment &Fragment : Section.Fragments) {
      const auto *Instruction = std::get_if<InstructionFragment>(&Fragment.Payload);
      if (Instruction != nullptr && Backend->mayNeedRelaxation(Instruction->Instruction)) {
        if (RelaxableInstructionCount == std::numeric_limits<std::size_t>::max()) {
          return makeUnexpected(ErrorCode::ObjectWritingFailed,
                                "MC relaxation budget exceeds the host limit");
        }
        ++RelaxableInstructionCount;
      }
    }
  }
  const std::size_t MaximumIterations = RelaxableInstructionCount + 1U;
  for (std::size_t Iteration = 0; Iteration < MaximumIterations; ++Iteration) {
    auto Encoded = encodeFragments();
    if (!Encoded)
      return std::unexpected(Encoded.error());

    for (SectionData &Section : Sections) {
      std::uint64_t Offset = 0;
      Section.Alignment = Section.Section->description().Alignment;
      const bool NoBits = isNoBits(*Section.Section);
      for (Fragment &Fragment : Section.Fragments) {
        Fragment.Offset = Offset;
        std::uint64_t Size = Fragment.Size;
        if (const auto *Alignment = std::get_if<AlignmentFragment>(&Fragment.Payload)) {
          auto Padding = alignmentPadding(Offset, Alignment->Alignment);
          if (!Padding)
            return std::unexpected(Padding.error());
          Size = Alignment->MaximumBytes != 0 && *Padding > Alignment->MaximumBytes ? 0 : *Padding;
          Section.Alignment = std::max(Section.Alignment, Alignment->Alignment);
          if (NoBits)
            Fragment.EncodedBytes.clear();
          else
            Fragment.EncodedBytes.assign(static_cast<std::size_t>(Size), Alignment->Fill);
        } else if (const auto *Zero = std::get_if<ZeroFillFragment>(&Fragment.Payload)) {
          Size = Zero->Size;
          if (!NoBits && Size > std::numeric_limits<std::size_t>::max()) {
            return makeUnexpected(ErrorCode::ObjectWritingFailed,
                                  "zero-fill fragment exceeds the host size limit");
          }
          if (NoBits)
            Fragment.EncodedBytes.clear();
          else
            Fragment.EncodedBytes.assign(static_cast<std::size_t>(Size), 0);
        } else if (NoBits) {
          if (std::holds_alternative<InstructionFragment>(Fragment.Payload) ||
              !Fragment.Fixups.empty() ||
              std::ranges::any_of(Fragment.EncodedBytes,
                                  [](std::uint8_t Byte) { return Byte != 0; })) {
            return makeUnexpected(ErrorCode::ObjectWritingFailed,
                                  "MC NOBITS section contains initialized data");
          }
          Fragment.EncodedBytes.clear();
        }
        Fragment.Size = Size;
        auto End = checkedAdd(Offset, Size, "MC section size overflows");
        if (!End)
          return std::unexpected(End.error());
        Offset = *End;
      }
      Section.AddressSize = Offset;
    }

    bool Changed = false;
    for (SectionData &Section : Sections) {
      for (Fragment &Fragment : Section.Fragments) {
        auto *Instruction = std::get_if<InstructionFragment>(&Fragment.Payload);
        if (Instruction == nullptr || !Backend->mayNeedRelaxation(Instruction->Instruction))
          continue;
        for (const MCFixup &Fixup : Fragment.Fixups) {
          auto Value = evaluateAsRelocatable(*Fixup.Value);
          if (!Value)
            return std::unexpected(Value.error());
          if (!Value->AddSymbol || Value->SubtractSymbol || Value->TargetKind != 0)
            continue;
          const auto SymbolPosition = SymbolIndices.find(Value->AddSymbol.get());
          if (SymbolPosition == SymbolIndices.end())
            continue;
          const SymbolData &Target = Symbols[SymbolPosition->second];
          if (!Target.SectionIndex || *Target.SectionIndex != SectionIndices[Section.Section.get()])
            continue;
          const auto TargetOffset = symbolOffset(Target);
          if (!TargetOffset)
            continue;
          auto PlaceOffset = checkedAdd(Fragment.Offset, Fixup.Offset, "MC fixup place overflows");
          if (!PlaceOffset)
            return std::unexpected(PlaceOffset.error());
          auto Place = signedOffset(*PlaceOffset, "MC fixup place exceeds the signed range");
          auto TargetPosition =
              signedOffset(*TargetOffset, "MC relaxation target exceeds the signed range");
          if (!Place)
            return std::unexpected(Place.error());
          if (!TargetPosition)
            return std::unexpected(TargetPosition.error());
          auto WithAddend =
              checkedAdd(*TargetPosition, Value->Addend, "MC relaxation target addend overflows");
          if (!WithAddend)
            return std::unexpected(WithAddend.error());
          auto Displacement =
              checkedSubtract(*WithAddend, *Place, "MC relaxation displacement overflows");
          if (!Displacement)
            return std::unexpected(Displacement.error());
          auto NeedsRelaxation = Backend->fixupNeedsRelaxation(Fixup, *Displacement);
          if (!NeedsRelaxation)
            return std::unexpected(NeedsRelaxation.error());
          if (!*NeedsRelaxation)
            continue;
          auto Relaxed = Backend->relaxInstruction(Instruction->Instruction);
          if (!Relaxed)
            return std::unexpected(Relaxed.error());
          Instruction->Instruction = std::move(*Relaxed);
          Changed = true;
          break;
        }
      }
    }
    if (!Changed)
      return false;
  }
  return makeUnexpected(ErrorCode::InternalError, "MC relaxation did not reach a fixed point");
}

Expected<MCAssembledObject> MCAssembler::finalizeObject() {
  for (SectionData &Section : Sections) {
    for (Fragment &Fragment : Section.Fragments) {
      for (const MCFixup &Fixup : Fragment.Fixups) {
        auto Value = evaluateAsRelocatable(*Fixup.Value);
        if (!Value)
          return std::unexpected(Value.error());
        if (Value->AddSymbol)
          static_cast<void>(getOrCreateSymbol(Value->AddSymbol));
        if (Value->SubtractSymbol)
          static_cast<void>(getOrCreateSymbol(Value->SubtractSymbol));
      }
    }
  }

  MCAssembledObject Result;
  Result.Flags = Backend->objectFlags();
  Result.Sections.reserve(Sections.size());
  for (const SectionData &Section : Sections) {
    MCObjectSection Output{.Section = Section.Section,
                           .Contents = {},
                           .AddressSize = Section.AddressSize,
                           .Alignment = Section.Alignment,
                           .Relocations = {}};
    if (!isNoBits(*Section.Section) &&
        Section.AddressSize > std::numeric_limits<std::size_t>::max()) {
      return makeUnexpected(ErrorCode::ObjectWritingFailed,
                            "MC section exceeds the host size limit");
    }
    if (!isNoBits(*Section.Section)) {
      Output.Contents.reserve(static_cast<std::size_t>(Section.AddressSize));
      for (const Fragment &Fragment : Section.Fragments)
        Output.Contents.insert(Output.Contents.end(), Fragment.EncodedBytes.begin(),
                               Fragment.EncodedBytes.end());
    }
    Result.Sections.push_back(std::move(Output));
  }

  Result.Symbols.reserve(Symbols.size());
  for (const SymbolData &Data : Symbols) {
    if (!Data.SectionIndex && Data.CommonSize == 0 && Data.Symbol->isTemporary()) {
      return makeUnexpected(ErrorCode::RelocationFailed,
                            "temporary MC symbol is referenced but not defined");
    }
    const MCObjectSymbolBinding Binding =
        !Data.BindingWasSet && !Data.SectionIndex && Data.CommonSize == 0
            ? MCObjectSymbolBinding::Global
            : Data.Binding;
    MCObjectSymbol Output{.Symbol = Data.Symbol,
                          .Binding = Binding,
                          .Type = Data.Type,
                          .Visibility = Data.Visibility,
                          .SectionIndex = Data.SectionIndex,
                          .CommonAlignment = Data.CommonAlignment};
    if (Data.CommonSize != 0) {
      Output.Size = Data.CommonSize;
      Output.Value = Data.CommonAlignment;
    } else if (const auto Offset = symbolOffset(Data)) {
      Output.Value = *Offset;
    }
    if (Data.Size) {
      auto SizeValue = evaluateAsRelocatable(*Data.Size);
      if (!SizeValue)
        return std::unexpected(SizeValue.error());
      if (SizeValue->TargetKind != 0) {
        return makeUnexpected(ErrorCode::ObjectWritingFailed,
                              "symbol size cannot use a target expression");
      }
      if (SizeValue->AddSymbol && SizeValue->SubtractSymbol) {
        const auto AddPosition = SymbolIndices.find(SizeValue->AddSymbol.get());
        const auto SubtractPosition = SymbolIndices.find(SizeValue->SubtractSymbol.get());
        if (AddPosition == SymbolIndices.end() || SubtractPosition == SymbolIndices.end()) {
          return makeUnexpected(ErrorCode::ObjectWritingFailed, "symbol size is unresolved");
        }
        const SymbolData &Add = Symbols[AddPosition->second];
        const SymbolData &Subtract = Symbols[SubtractPosition->second];
        if (!Add.SectionIndex || !Subtract.SectionIndex ||
            Add.SectionIndex != Subtract.SectionIndex) {
          return makeUnexpected(ErrorCode::ObjectWritingFailed,
                                "symbol size difference crosses sections");
        }
      }
      std::int64_t Resolved = SizeValue->Addend;
      const auto Resolve = [&](const MCSymbolRef &Symbol) -> Expected<std::int64_t> {
        const auto Position = SymbolIndices.find(Symbol.get());
        if (Position == SymbolIndices.end())
          return makeUnexpected(ErrorCode::ObjectWritingFailed, "symbol size is unresolved");
        const auto Offset = symbolOffset(Symbols[Position->second]);
        if (!Offset)
          return makeUnexpected(ErrorCode::ObjectWritingFailed, "symbol size is unresolved");
        return signedOffset(*Offset, "symbol size offset exceeds the signed range");
      };
      if (SizeValue->AddSymbol) {
        auto Offset = Resolve(SizeValue->AddSymbol);
        if (!Offset)
          return std::unexpected(Offset.error());
        auto Updated = checkedAdd(Resolved, *Offset, "symbol size addition overflows");
        if (!Updated)
          return std::unexpected(Updated.error());
        Resolved = *Updated;
      }
      if (SizeValue->SubtractSymbol) {
        auto Offset = Resolve(SizeValue->SubtractSymbol);
        if (!Offset)
          return std::unexpected(Offset.error());
        auto Updated = checkedSubtract(Resolved, *Offset, "symbol size subtraction overflows");
        if (!Updated)
          return std::unexpected(Updated.error());
        Resolved = *Updated;
      }
      if (Resolved < 0)
        return makeUnexpected(ErrorCode::ObjectWritingFailed, "symbol size is negative");
      Output.Size = static_cast<std::uint64_t>(Resolved);
    }
    if (Output.SectionIndex) {
      const std::uint64_t SectionSize = Sections[*Output.SectionIndex].AddressSize;
      if (Output.Value > SectionSize || Output.Size > SectionSize - Output.Value) {
        return makeUnexpected(ErrorCode::ObjectWritingFailed,
                              "symbol range exceeds its defining section");
      }
    }
    Result.Symbols.push_back(std::move(Output));
  }

  for (std::size_t SectionIndex = 0; SectionIndex < Sections.size(); ++SectionIndex) {
    const SectionData &Section = Sections[SectionIndex];
    MCObjectSection &Output = Result.Sections[SectionIndex];
    for (const Fragment &Fragment : Section.Fragments) {
      for (const MCFixup &Fixup : Fragment.Fixups) {
        auto Value = evaluateAsRelocatable(*Fixup.Value);
        if (!Value)
          return std::unexpected(Value.error());
        auto PlaceValue =
            checkedAdd(Fragment.Offset, Fixup.Offset, "MC relocation offset overflows");
        if (!PlaceValue)
          return std::unexpected(PlaceValue.error());
        const std::uint64_t Place = *PlaceValue;
        bool Resolved = Value->isAbsolute();
        std::int64_t FixedValue = Value->Addend;
        const auto FindSymbol = [&](const MCSymbolRef &Symbol) -> const SymbolData * {
          const auto Position = SymbolIndices.find(Symbol.get());
          return Position == SymbolIndices.end() ? nullptr : &Symbols[Position->second];
        };
        const SymbolData *Add = Value->AddSymbol ? FindSymbol(Value->AddSymbol) : nullptr;
        const SymbolData *Subtract =
            Value->SubtractSymbol ? FindSymbol(Value->SubtractSymbol) : nullptr;
        if (Value->TargetKind == 0 && Add != nullptr && Subtract != nullptr && Add->SectionIndex &&
            Subtract->SectionIndex && Add->SectionIndex == Subtract->SectionIndex) {
          const auto AddOffset = symbolOffset(*Add);
          const auto SubtractOffset = symbolOffset(*Subtract);
          if (AddOffset && SubtractOffset) {
            auto SignedAdd = signedOffset(*AddOffset, "MC symbol offset exceeds the signed range");
            auto SignedSubtract =
                signedOffset(*SubtractOffset, "MC symbol offset exceeds the signed range");
            if (!SignedAdd)
              return std::unexpected(SignedAdd.error());
            if (!SignedSubtract)
              return std::unexpected(SignedSubtract.error());
            auto Difference =
                checkedSubtract(*SignedAdd, *SignedSubtract, "MC symbol difference overflows");
            if (!Difference)
              return std::unexpected(Difference.error());
            auto Updated = checkedAdd(FixedValue, *Difference, "MC resolved fixup value overflows");
            if (!Updated)
              return std::unexpected(Updated.error());
            FixedValue = *Updated;
            Resolved = true;
          }
        } else if (Value->TargetKind == 0 && Fixup.PCRelative && Add != nullptr &&
                   !Value->SubtractSymbol && Add->SectionIndex &&
                   *Add->SectionIndex == SectionIndex) {
          const auto AddOffset = symbolOffset(*Add);
          if (AddOffset) {
            auto SignedAdd = signedOffset(*AddOffset, "MC symbol offset exceeds the signed range");
            auto SignedPlace = signedOffset(Place, "MC fixup place exceeds the signed range");
            if (!SignedAdd)
              return std::unexpected(SignedAdd.error());
            if (!SignedPlace)
              return std::unexpected(SignedPlace.error());
            auto Difference =
                checkedSubtract(*SignedAdd, *SignedPlace, "MC PC-relative displacement overflows");
            if (!Difference)
              return std::unexpected(Difference.error());
            auto Updated = checkedAdd(FixedValue, *Difference, "MC resolved fixup value overflows");
            if (!Updated)
              return std::unexpected(Updated.error());
            FixedValue = *Updated;
            Resolved = true;
          }
        }

        if (Resolved) {
          auto Applied = Backend->applyFixup(Fixup.Kind, Output.Contents, Place, FixedValue);
          if (!Applied)
            return std::unexpected(Applied.error());
          continue;
        }
        auto Relocations = Backend->relocationsForFixup(Fixup, *Value);
        if (!Relocations)
          return std::unexpected(Relocations.error());
        for (const MCFixupRelocation &Relocation : *Relocations) {
          if (Relocation.Type == 0 || !Relocation.Symbol) {
            return makeUnexpected(ErrorCode::RelocationFailed,
                                  "MC backend produced an invalid relocation");
          }
          Output.Relocations.push_back(MCObjectRelocation{.Offset = Place,
                                                          .Type = Relocation.Type,
                                                          .Symbol = Relocation.Symbol,
                                                          .Addend = Relocation.Addend});
        }
      }
    }
  }
  auto DwarfFrame = appendDwarfFrame(Result);
  if (!DwarfFrame)
    return std::unexpected(DwarfFrame.error());
  return Result;
}

Expected<void> MCAssembler::appendDwarfFrame(MCAssembledObject &Object) const {
  if (CFIProcedures.empty())
    return {};
  if (!DwarfFrameInfo || !EHFrameSection) {
    return makeUnexpected(ErrorCode::ObjectWritingFailed,
                          "CFI was emitted without DWARF frame configuration");
  }
  if (ActiveCFIProcedure) {
    return makeUnexpected(ErrorCode::ObjectWritingFailed, "CFI procedure was not terminated");
  }

  std::vector<MCDwarfFrameProcedure> Procedures;
  Procedures.reserve(CFIProcedures.size());
  for (const CFIProcedure &Procedure : CFIProcedures) {
    if (!Procedure.IsClosed || Procedure.SectionIndex >= Sections.size()) {
      return makeUnexpected(ErrorCode::ObjectWritingFailed, "CFI procedure is incomplete");
    }
    const SectionData &Section = Sections[Procedure.SectionIndex];
    const auto FragmentOffset = [&Section](std::size_t Index) -> std::optional<std::uint64_t> {
      if (Index > Section.Fragments.size())
        return std::nullopt;
      if (Index == Section.Fragments.size())
        return Section.AddressSize;
      return Section.Fragments[Index].Offset;
    };
    const auto Begin = FragmentOffset(Procedure.BeginFragmentIndex);
    const auto End = FragmentOffset(Procedure.EndFragmentIndex);
    if (!Begin || !End || *End < *Begin) {
      return makeUnexpected(ErrorCode::ObjectWritingFailed,
                            "CFI procedure has an invalid address range");
    }
    MCDwarfFrameProcedure Output{
        .Function = Procedure.Function, .AddressRange = *End - *Begin, .Instructions = {}};
    Output.Instructions.reserve(Procedure.Events.size());
    for (const CFIEvent &Event : Procedure.Events) {
      const auto Offset = FragmentOffset(Event.FragmentIndex);
      if (!Offset || *Offset < *Begin || *Offset > *End) {
        return makeUnexpected(ErrorCode::ObjectWritingFailed,
                              "CFI event lies outside its procedure");
      }
      Output.Instructions.push_back(MCDwarfFrameInstruction{
          .Instruction = Event.Instruction,
          .Offset = *Offset - *Begin,
      });
    }
    Procedures.push_back(std::move(Output));
  }
  auto Frame = emitDwarfEHFrame(EHFrameSection, Procedures, *DwarfFrameInfo, *Backend);
  if (!Frame)
    return std::unexpected(Frame.error());
  Object.Sections.push_back(std::move(*Frame));
  return {};
}

Expected<MCAssembledObject> MCAssembler::assemble() {
  if (WasAssembled)
    return makeUnexpected(ErrorCode::ObjectWritingFailed, "MC assembler was already finalized");
  if (ActiveCFIProcedure) {
    return makeUnexpected(ErrorCode::ObjectWritingFailed, "CFI procedure was not terminated");
  }
  MCAssembler Working = *this;
  auto Relaxed = Working.layoutAndRelax();
  if (!Relaxed)
    return std::unexpected(Relaxed.error());
  auto Result = Working.finalizeObject();
  if (!Result)
    return std::unexpected(Result.error());
  WasAssembled = true;
  return Result;
}

} // namespace rcc::mc
