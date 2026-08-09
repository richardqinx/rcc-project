// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/MC/MCDwarfFrameEmitter.h"

#include "rcc/MC/MCExpr.h"
#include "rcc/MC/MCFixup.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace rcc::mc {
namespace {

constexpr std::uint8_t DWCFAAdvanceLoc = 0x40;
constexpr std::uint8_t DWCFAOffset = 0x80;
constexpr std::uint8_t DWCFARestore = 0xc0;
constexpr std::uint8_t DWCFANop = 0x00;
constexpr std::uint8_t DWCFAAdvanceLoc1 = 0x02;
constexpr std::uint8_t DWCFAAdvanceLoc2 = 0x03;
constexpr std::uint8_t DWCFAAdvanceLoc4 = 0x04;
constexpr std::uint8_t DWCFAOffsetExtended = 0x05;
constexpr std::uint8_t DWCFARestoreExtended = 0x06;
constexpr std::uint8_t DWCFAUndefined = 0x07;
constexpr std::uint8_t DWCFASameValue = 0x08;
constexpr std::uint8_t DWCFARegister = 0x09;
constexpr std::uint8_t DWCFARememberState = 0x0a;
constexpr std::uint8_t DWCFARestoreState = 0x0b;
constexpr std::uint8_t DWCFADefCFA = 0x0c;
constexpr std::uint8_t DWCFADefCFARegister = 0x0d;
constexpr std::uint8_t DWCFADefCFAOffset = 0x0e;
constexpr std::uint8_t DWCFAValOffset = 0x14;
constexpr std::uint8_t DWCFAGNUWindowSave = 0x2d;
constexpr std::uint8_t DWCFAGNUArgsSize = 0x2e;
constexpr std::uint8_t DWEHPEPCRelativeSData4 = 0x1b;

void appendU16(std::vector<std::uint8_t> &Bytes, std::uint16_t Value) {
  Bytes.push_back(static_cast<std::uint8_t>(Value));
  Bytes.push_back(static_cast<std::uint8_t>(Value >> 8U));
}

void appendU32(std::vector<std::uint8_t> &Bytes, std::uint32_t Value) {
  for (std::uint8_t Shift = 0; Shift < 32; Shift += 8)
    Bytes.push_back(static_cast<std::uint8_t>(Value >> Shift));
}

void writeU32(std::vector<std::uint8_t> &Bytes, std::size_t Offset, std::uint32_t Value) {
  for (std::uint8_t Shift = 0; Shift < 32; Shift += 8)
    Bytes[Offset + Shift / 8U] = static_cast<std::uint8_t>(Value >> Shift);
}

void appendULEB128(std::vector<std::uint8_t> &Bytes, std::uint64_t Value) {
  do {
    std::uint8_t Byte = static_cast<std::uint8_t>(Value & 0x7fU);
    Value >>= 7U;
    if (Value != 0)
      Byte |= 0x80U;
    Bytes.push_back(Byte);
  } while (Value != 0);
}

void appendSLEB128(std::vector<std::uint8_t> &Bytes, std::int64_t Value) {
  bool More = true;
  while (More) {
    std::uint8_t Byte = static_cast<std::uint8_t>(Value & 0x7f);
    const bool HasSign = (Byte & 0x40U) != 0;
    Value >>= 7;
    More = (Value != 0 || HasSign) && (Value != -1 || !HasSign);
    if (More)
      Byte |= 0x80U;
    Bytes.push_back(Byte);
  }
}

[[nodiscard]] Expected<void> appendAdvance(std::vector<std::uint8_t> &Bytes, std::uint64_t Delta,
                                           const MCDwarfFrameInfo &Info) {
  if (Delta == 0)
    return {};
  if (Delta % Info.CodeAlignment != 0) {
    return makeUnexpected(ErrorCode::ObjectWritingFailed,
                          "CFI location is not aligned to the DWARF code factor");
  }
  const std::uint64_t Factored = Delta / Info.CodeAlignment;
  if (Factored <= 0x3fU) {
    Bytes.push_back(static_cast<std::uint8_t>(DWCFAAdvanceLoc | Factored));
  } else if (Factored <= std::numeric_limits<std::uint8_t>::max()) {
    Bytes.push_back(DWCFAAdvanceLoc1);
    Bytes.push_back(static_cast<std::uint8_t>(Factored));
  } else if (Factored <= std::numeric_limits<std::uint16_t>::max()) {
    Bytes.push_back(DWCFAAdvanceLoc2);
    appendU16(Bytes, static_cast<std::uint16_t>(Factored));
  } else if (Factored <= std::numeric_limits<std::uint32_t>::max()) {
    Bytes.push_back(DWCFAAdvanceLoc4);
    appendU32(Bytes, static_cast<std::uint32_t>(Factored));
  } else {
    return makeUnexpected(ErrorCode::Unsupported, "DWARF CFI location delta exceeds 32 bits");
  }
  return {};
}

[[nodiscard]] Expected<std::uint64_t> registerNumber(std::int64_t Value) {
  if (Value < 0 || static_cast<std::uint64_t>(Value) > std::numeric_limits<std::uint32_t>::max()) {
    return makeUnexpected(ErrorCode::ObjectWritingFailed,
                          "DWARF CFI register number is out of range");
  }
  return static_cast<std::uint64_t>(Value);
}

[[nodiscard]] Expected<std::uint64_t> factoredOffset(std::int64_t Offset,
                                                     std::int64_t DataAlignment) {
  if (Offset == std::numeric_limits<std::int64_t>::min() && DataAlignment == -1) {
    return makeUnexpected(ErrorCode::ObjectWritingFailed, "DWARF CFI offset factor overflows");
  }
  if (Offset % DataAlignment != 0) {
    return makeUnexpected(ErrorCode::ObjectWritingFailed,
                          "DWARF CFI offset is not divisible by its data factor");
  }
  const std::int64_t Factored = Offset / DataAlignment;
  if (Factored < 0) {
    return makeUnexpected(ErrorCode::Unsupported, "DWARF CFI rule requires a signed offset opcode");
  }
  return static_cast<std::uint64_t>(Factored);
}

void appendOffsetRule(std::vector<std::uint8_t> &Bytes, std::uint64_t Register,
                      std::uint64_t Offset) {
  if (Register <= 0x3fU) {
    Bytes.push_back(static_cast<std::uint8_t>(DWCFAOffset | Register));
  } else {
    Bytes.push_back(DWCFAOffsetExtended);
    appendULEB128(Bytes, Register);
  }
  appendULEB128(Bytes, Offset);
}

void appendRestoreRule(std::vector<std::uint8_t> &Bytes, std::uint64_t Register) {
  if (Register <= 0x3fU) {
    Bytes.push_back(static_cast<std::uint8_t>(DWCFARestore | Register));
  } else {
    Bytes.push_back(DWCFARestoreExtended);
    appendULEB128(Bytes, Register);
  }
}

struct CFAState final {
  std::uint64_t Register = 0;
  std::int64_t Offset = 0;
};

[[nodiscard]] Expected<void>
appendFrameInstructions(std::vector<std::uint8_t> &Bytes,
                        std::span<const MCDwarfFrameInstruction> Instructions,
                        const MCDwarfFrameInfo &Info) {
  CFAState State{.Register = Info.InitialCFARegister,
                 .Offset = static_cast<std::int64_t>(Info.InitialCFAOffset)};
  std::vector<CFAState> StateStack;
  std::uint64_t PreviousOffset = 0;
  for (const MCDwarfFrameInstruction &Entry : Instructions) {
    auto Verification = Entry.Instruction.verify();
    if (!Verification)
      return std::unexpected(Verification.error());
    if (Entry.Offset < PreviousOffset) {
      return makeUnexpected(ErrorCode::ObjectWritingFailed,
                            "DWARF CFI locations are not monotonic");
    }
    if (Entry.Instruction.Operation == MCCFIOperation::Label)
      continue;
    auto Advance = appendAdvance(Bytes, Entry.Offset - PreviousOffset, Info);
    if (!Advance)
      return std::unexpected(Advance.error());
    PreviousOffset = Entry.Offset;

    const auto &Operands = Entry.Instruction.Operands;
    switch (Entry.Instruction.Operation) {
    case MCCFIOperation::SameValue: {
      auto Register = registerNumber(Operands[0]);
      if (!Register)
        return std::unexpected(Register.error());
      Bytes.push_back(DWCFASameValue);
      appendULEB128(Bytes, *Register);
      break;
    }
    case MCCFIOperation::RememberState:
      Bytes.push_back(DWCFARememberState);
      StateStack.push_back(State);
      break;
    case MCCFIOperation::RestoreState:
      if (StateStack.empty()) {
        return makeUnexpected(ErrorCode::ObjectWritingFailed,
                              "DWARF CFI restore_state has no saved state");
      }
      Bytes.push_back(DWCFARestoreState);
      State = StateStack.back();
      StateStack.pop_back();
      break;
    case MCCFIOperation::Offset:
    case MCCFIOperation::RelativeOffset: {
      auto Register = registerNumber(Operands[0]);
      if (!Register)
        return std::unexpected(Register.error());
      std::int64_t Offset = Operands[1];
      if (Entry.Instruction.Operation == MCCFIOperation::RelativeOffset &&
          __builtin_sub_overflow(Offset, State.Offset, &Offset)) {
        return makeUnexpected(ErrorCode::ObjectWritingFailed,
                              "relative DWARF CFI offset overflows");
      }
      auto Factored = factoredOffset(Offset, Info.DataAlignment);
      if (!Factored)
        return std::unexpected(Factored.error());
      appendOffsetRule(Bytes, *Register, *Factored);
      break;
    }
    case MCCFIOperation::DefCFARegister: {
      auto Register = registerNumber(Operands[0]);
      if (!Register)
        return std::unexpected(Register.error());
      Bytes.push_back(DWCFADefCFARegister);
      appendULEB128(Bytes, *Register);
      State.Register = *Register;
      break;
    }
    case MCCFIOperation::DefCFAOffset:
      if (Operands[0] < 0) {
        return makeUnexpected(ErrorCode::ObjectWritingFailed,
                              "DWARF CFA offset cannot be negative");
      }
      Bytes.push_back(DWCFADefCFAOffset);
      appendULEB128(Bytes, static_cast<std::uint64_t>(Operands[0]));
      State.Offset = Operands[0];
      break;
    case MCCFIOperation::DefCFA: {
      auto Register = registerNumber(Operands[0]);
      if (!Register)
        return std::unexpected(Register.error());
      if (Operands[1] < 0) {
        return makeUnexpected(ErrorCode::ObjectWritingFailed,
                              "DWARF CFA offset cannot be negative");
      }
      Bytes.push_back(DWCFADefCFA);
      appendULEB128(Bytes, *Register);
      appendULEB128(Bytes, static_cast<std::uint64_t>(Operands[1]));
      State = CFAState{.Register = *Register, .Offset = Operands[1]};
      break;
    }
    case MCCFIOperation::AdjustCFAOffset: {
      std::int64_t Adjusted = 0;
      if (__builtin_add_overflow(State.Offset, Operands[0], &Adjusted) || Adjusted < 0) {
        return makeUnexpected(ErrorCode::ObjectWritingFailed,
                              "adjusted DWARF CFA offset is invalid");
      }
      Bytes.push_back(DWCFADefCFAOffset);
      appendULEB128(Bytes, static_cast<std::uint64_t>(Adjusted));
      State.Offset = Adjusted;
      break;
    }
    case MCCFIOperation::Escape:
      Bytes.insert(Bytes.end(), Entry.Instruction.Bytes.begin(), Entry.Instruction.Bytes.end());
      break;
    case MCCFIOperation::Restore: {
      auto Register = registerNumber(Operands[0]);
      if (!Register)
        return std::unexpected(Register.error());
      appendRestoreRule(Bytes, *Register);
      break;
    }
    case MCCFIOperation::Undefined: {
      auto Register = registerNumber(Operands[0]);
      if (!Register)
        return std::unexpected(Register.error());
      Bytes.push_back(DWCFAUndefined);
      appendULEB128(Bytes, *Register);
      break;
    }
    case MCCFIOperation::Register: {
      auto Register = registerNumber(Operands[0]);
      auto Source = registerNumber(Operands[1]);
      if (!Register)
        return std::unexpected(Register.error());
      if (!Source)
        return std::unexpected(Source.error());
      Bytes.push_back(DWCFARegister);
      appendULEB128(Bytes, *Register);
      appendULEB128(Bytes, *Source);
      break;
    }
    case MCCFIOperation::WindowSave:
      Bytes.push_back(DWCFAGNUWindowSave);
      break;
    case MCCFIOperation::GNUArgsSize:
      if (Operands[0] < 0) {
        return makeUnexpected(ErrorCode::ObjectWritingFailed,
                              "DWARF GNU argument size cannot be negative");
      }
      Bytes.push_back(DWCFAGNUArgsSize);
      appendULEB128(Bytes, static_cast<std::uint64_t>(Operands[0]));
      break;
    case MCCFIOperation::ValueOffset: {
      auto Register = registerNumber(Operands[0]);
      if (!Register)
        return std::unexpected(Register.error());
      auto Factored = factoredOffset(Operands[1], Info.DataAlignment);
      if (!Factored)
        return std::unexpected(Factored.error());
      Bytes.push_back(DWCFAValOffset);
      appendULEB128(Bytes, *Register);
      appendULEB128(Bytes, *Factored);
      break;
    }
    case MCCFIOperation::StartProc:
    case MCCFIOperation::EndProc:
    case MCCFIOperation::Sections:
    case MCCFIOperation::Personality:
    case MCCFIOperation::LSDA:
    case MCCFIOperation::SignalFrame:
    case MCCFIOperation::ReturnColumn:
    case MCCFIOperation::NegateReturnAddressState:
    case MCCFIOperation::NegateReturnAddressStateWithPC:
    case MCCFIOperation::Label:
      return makeUnexpected(ErrorCode::Unsupported,
                            "this CFI operation cannot be encoded in an RV64GC FDE");
    }
  }
  if (!StateStack.empty()) {
    return makeUnexpected(ErrorCode::ObjectWritingFailed,
                          "DWARF CFI procedure has unbalanced remember_state operations");
  }
  return {};
}

[[nodiscard]] Expected<void> finishEntry(std::vector<std::uint8_t> &Bytes, std::size_t EntryStart) {
  while (Bytes.size() % 4U != 0)
    Bytes.push_back(DWCFANop);
  const std::size_t Length = Bytes.size() - EntryStart - 4U;
  if (Length > std::numeric_limits<std::uint32_t>::max()) {
    return makeUnexpected(ErrorCode::Unsupported, "DWARF frame entry exceeds 32 bits");
  }
  writeU32(Bytes, EntryStart, static_cast<std::uint32_t>(Length));
  return {};
}

} // namespace

Expected<void> MCDwarfFrameInfo::verify() const {
  if (CodeAlignment == 0 || DataAlignment == 0) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "DWARF frame alignment factors must be nonzero");
  }
  if (InitialCFAOffset > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return makeUnexpected(ErrorCode::InvalidArgument, "initial DWARF CFA offset is too large");
  }
  return {};
}

Expected<MCObjectSection> emitDwarfEHFrame(MCSectionRef Section,
                                           std::span<const MCDwarfFrameProcedure> Procedures,
                                           const MCDwarfFrameInfo &Info,
                                           const MCAsmBackend &Backend) {
  if (!Section || Section->description().Kind != MCSectionKind::EHFrame) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "DWARF EH frame emission requires an EHFrame section");
  }
  auto Verification = Info.verify();
  if (!Verification)
    return std::unexpected(Verification.error());

  MCObjectSection Result{.Section = std::move(Section),
                         .Contents = {},
                         .AddressSize = 0,
                         .Alignment = 1,
                         .Relocations = {}};
  Result.Alignment = Result.Section->description().Alignment;
  const std::size_t CIEStart = Result.Contents.size();
  appendU32(Result.Contents, 0);
  appendU32(Result.Contents, 0);
  Result.Contents.push_back(1);
  Result.Contents.insert(Result.Contents.end(), {'z', 'R', 0});
  appendULEB128(Result.Contents, Info.CodeAlignment);
  appendSLEB128(Result.Contents, Info.DataAlignment);
  appendULEB128(Result.Contents, Info.ReturnAddressRegister);
  appendULEB128(Result.Contents, 1);
  Result.Contents.push_back(DWEHPEPCRelativeSData4);
  Result.Contents.push_back(DWCFADefCFA);
  appendULEB128(Result.Contents, Info.InitialCFARegister);
  appendULEB128(Result.Contents, Info.InitialCFAOffset);
  auto FinishedCIE = finishEntry(Result.Contents, CIEStart);
  if (!FinishedCIE)
    return std::unexpected(FinishedCIE.error());

  for (const MCDwarfFrameProcedure &Procedure : Procedures) {
    if (!Procedure.Function) {
      return makeUnexpected(ErrorCode::ObjectWritingFailed,
                            "DWARF frame procedure has no function symbol");
    }
    if (Procedure.AddressRange > std::numeric_limits<std::uint32_t>::max()) {
      return makeUnexpected(ErrorCode::Unsupported, "DWARF FDE address range exceeds 32 bits");
    }
    const std::size_t FDEStart = Result.Contents.size();
    appendU32(Result.Contents, 0);
    const std::size_t CIEPointerOffset = Result.Contents.size();
    const std::size_t CIEDistance = CIEPointerOffset - CIEStart;
    if (CIEDistance > std::numeric_limits<std::uint32_t>::max()) {
      return makeUnexpected(ErrorCode::Unsupported, "DWARF FDE CIE distance exceeds 32 bits");
    }
    appendU32(Result.Contents, static_cast<std::uint32_t>(CIEDistance));
    const std::size_t InitialLocationOffset = Result.Contents.size();
    appendU32(Result.Contents, 0);
    appendU32(Result.Contents, static_cast<std::uint32_t>(Procedure.AddressRange));
    appendULEB128(Result.Contents, 0);
    auto Instructions = appendFrameInstructions(Result.Contents, Procedure.Instructions, Info);
    if (!Instructions)
      return std::unexpected(Instructions.error());
    auto FinishedFDE = finishEntry(Result.Contents, FDEStart);
    if (!FinishedFDE)
      return std::unexpected(FinishedFDE.error());

    auto Expression = MCExpr::symbol(Procedure.Function);
    if (!Expression)
      return std::unexpected(Expression.error());
    MCFixup Fixup{.Offset = InitialLocationOffset,
                  .Value = *Expression,
                  .Kind = fixup_kind::PCRelative4,
                  .PCRelative = true,
                  .LinkerRelaxable = false};
    MCValue Value{
        .AddSymbol = Procedure.Function, .SubtractSymbol = {}, .Addend = 0, .TargetKind = 0};
    auto Relocation = Backend.relocationType(Fixup, Value);
    if (!Relocation)
      return std::unexpected(Relocation.error());
    Result.Relocations.push_back(MCObjectRelocation{
        .Offset = InitialLocationOffset,
        .Type = *Relocation,
        .Symbol = Procedure.Function,
        .Addend = 0,
    });
  }
  Result.AddressSize = Result.Contents.size();
  return Result;
}

} // namespace rcc::mc
