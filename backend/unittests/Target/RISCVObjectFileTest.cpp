// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/Target/RISCV/RISCVObjectFile.h"

#include "rcc/CodeGen/CompilationUnit.h"
#include "rcc/CodeGen/GlobalObject.h"
#include "rcc/MC/ELFObjectWriter.h"
#include "rcc/MC/MCAssembler.h"
#include "rcc/MC/MCContext.h"
#include "rcc/MC/MCDwarfFrameEmitter.h"
#include "rcc/MC/MCExpr.h"
#include "rcc/MC/MCObjectStreamer.h"
#include "rcc/Target/RISCV/RISCVAsmBackend.h"
#include "rcc/Target/RISCV/RISCVMCCodeEmitter.h"
#include "rcc/Target/RISCV/RISCVTarget.h"
#include "rcc/Target/RISCV/RISCVTargetConstant.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t SHTRela = 4;
constexpr std::uint32_t SHTNoBits = 8;
constexpr std::uint32_t SHTGroup = 17;
constexpr std::uint16_t SHNUndefined = 0;
constexpr std::uint16_t SHNCommon = 0xfff2U;
constexpr std::uint64_t SHFWrite = 0x1;
constexpr std::uint64_t SHFAlloc = 0x2;
constexpr std::uint64_t SHFGroup = 0x200;
constexpr std::uint64_t SHFTLS = 0x400;

class TestContext final {
public:
  void expect(bool Condition, std::string_view Message) {
    if (Condition)
      return;
    std::cerr << Message << '\n';
    ++Failures;
  }

  [[nodiscard]] int result() const noexcept {
    return Failures == 0 ? 0 : 1;
  }

private:
  int Failures = 0;
};

[[nodiscard]] std::optional<std::uint16_t> readU16(std::span<const std::uint8_t> Bytes,
                                                   std::uint64_t Offset) {
  if (Offset > Bytes.size() || 2U > Bytes.size() - static_cast<std::size_t>(Offset))
    return std::nullopt;
  const std::size_t Position = static_cast<std::size_t>(Offset);
  return static_cast<std::uint16_t>(Bytes[Position]) |
         static_cast<std::uint16_t>(Bytes[Position + 1U]) << 8U;
}

[[nodiscard]] std::optional<std::uint32_t> readU32(std::span<const std::uint8_t> Bytes,
                                                   std::uint64_t Offset) {
  if (Offset > Bytes.size() || 4U > Bytes.size() - static_cast<std::size_t>(Offset))
    return std::nullopt;
  std::uint32_t Result = 0;
  for (std::uint32_t Index = 0; Index < 4; ++Index)
    Result |= static_cast<std::uint32_t>(Bytes[static_cast<std::size_t>(Offset) + Index])
              << (Index * 8U);
  return Result;
}

[[nodiscard]] std::optional<std::uint64_t> readU64(std::span<const std::uint8_t> Bytes,
                                                   std::uint64_t Offset) {
  if (Offset > Bytes.size() || 8U > Bytes.size() - static_cast<std::size_t>(Offset))
    return std::nullopt;
  std::uint64_t Result = 0;
  for (std::uint32_t Index = 0; Index < 8; ++Index)
    Result |= static_cast<std::uint64_t>(Bytes[static_cast<std::size_t>(Offset) + Index])
              << (Index * 8U);
  return Result;
}

struct ELFSection final {
  std::string Name;
  std::uint32_t Type = 0;
  std::uint64_t Flags = 0;
  std::uint64_t Offset = 0;
  std::uint64_t Size = 0;
  std::uint32_t Link = 0;
  std::uint32_t Info = 0;
  std::uint64_t Alignment = 0;
  std::uint64_t EntrySize = 0;
};

struct ELFSymbol final {
  std::string Name;
  std::uint8_t Binding = 0;
  std::uint8_t Type = 0;
  std::uint8_t Visibility = 0;
  std::uint16_t Section = 0;
  std::uint64_t Value = 0;
  std::uint64_t Size = 0;
};

struct ELFRelocation final {
  std::uint64_t Offset = 0;
  std::uint32_t Symbol = 0;
  std::uint32_t Type = 0;
  std::int64_t Addend = 0;
};

class ELFView final {
public:
  [[nodiscard]] static std::optional<ELFView> create(std::span<const std::uint8_t> Bytes) {
    if (Bytes.size() < 64 || Bytes[0] != 0x7f || Bytes[1] != 'E' || Bytes[2] != 'L' ||
        Bytes[3] != 'F' || Bytes[4] != 2 || Bytes[5] != 1) {
      return std::nullopt;
    }
    const auto HeaderOffset = readU64(Bytes, 40);
    const auto EntrySize = readU16(Bytes, 58);
    const auto Count = readU16(Bytes, 60);
    const auto NamesIndex = readU16(Bytes, 62);
    if (!HeaderOffset || !EntrySize || !Count || !NamesIndex || *EntrySize != 64 || *Count == 0 ||
        *NamesIndex >= *Count) {
      return std::nullopt;
    }
    if (*HeaderOffset > Bytes.size() ||
        static_cast<std::uint64_t>(*Count) * *EntrySize > Bytes.size() - *HeaderOffset) {
      return std::nullopt;
    }

    const std::uint64_t NamesHeader = *HeaderOffset + static_cast<std::uint64_t>(*NamesIndex) * 64U;
    const auto NamesOffset = readU64(Bytes, NamesHeader + 24U);
    const auto NamesSize = readU64(Bytes, NamesHeader + 32U);
    if (!NamesOffset || !NamesSize || *NamesOffset > Bytes.size() ||
        *NamesSize > Bytes.size() - *NamesOffset) {
      return std::nullopt;
    }
    const std::span<const std::uint8_t> Names =
        Bytes.subspan(static_cast<std::size_t>(*NamesOffset), static_cast<std::size_t>(*NamesSize));

    ELFView Result(Bytes);
    Result.Sections.reserve(*Count);
    for (std::uint16_t Index = 0; Index < *Count; ++Index) {
      const std::uint64_t Position = *HeaderOffset + static_cast<std::uint64_t>(Index) * 64U;
      const auto NameOffset = readU32(Bytes, Position);
      const auto Type = readU32(Bytes, Position + 4U);
      const auto Flags = readU64(Bytes, Position + 8U);
      const auto Offset = readU64(Bytes, Position + 24U);
      const auto Size = readU64(Bytes, Position + 32U);
      const auto Link = readU32(Bytes, Position + 40U);
      const auto Info = readU32(Bytes, Position + 44U);
      const auto Alignment = readU64(Bytes, Position + 48U);
      const auto SectionEntrySize = readU64(Bytes, Position + 56U);
      if (!NameOffset || !Type || !Flags || !Offset || !Size || !Link || !Info || !Alignment ||
          !SectionEntrySize || *NameOffset >= Names.size()) {
        return std::nullopt;
      }
      std::size_t End = *NameOffset;
      while (End < Names.size() && Names[End] != 0)
        ++End;
      if (End == Names.size())
        return std::nullopt;
      Result.Sections.push_back(ELFSection{
          .Name = std::string(reinterpret_cast<const char *>(Names.data() + *NameOffset),
                              End - *NameOffset),
          .Type = *Type,
          .Flags = *Flags,
          .Offset = *Offset,
          .Size = *Size,
          .Link = *Link,
          .Info = *Info,
          .Alignment = *Alignment,
          .EntrySize = *SectionEntrySize,
      });
    }
    return Result;
  }

  [[nodiscard]] const ELFSection *section(std::string_view Name) const noexcept {
    const auto Position =
        std::ranges::find(Sections, Name, [](const ELFSection &Value) { return Value.Name; });
    return Position == Sections.end() ? nullptr : &*Position;
  }

  [[nodiscard]] std::optional<std::size_t> sectionIndex(std::string_view Name) const noexcept {
    for (std::size_t Index = 0; Index < Sections.size(); ++Index) {
      if (Sections[Index].Name == Name)
        return Index;
    }
    return std::nullopt;
  }

  [[nodiscard]] std::span<const std::uint8_t> contents(const ELFSection &Section) const noexcept {
    if (Section.Type == SHTNoBits || Section.Offset > Bytes.size() ||
        Section.Size > Bytes.size() - Section.Offset) {
      return {};
    }
    return Bytes.subspan(static_cast<std::size_t>(Section.Offset),
                         static_cast<std::size_t>(Section.Size));
  }

  [[nodiscard]] std::optional<std::vector<ELFSymbol>> symbols() const {
    const ELFSection *Table = section(".symtab");
    if (Table == nullptr || Table->EntrySize != 24 || Table->Link >= Sections.size())
      return std::nullopt;
    const std::span<const std::uint8_t> TableBytes = contents(*Table);
    const std::span<const std::uint8_t> Names = contents(Sections[Table->Link]);
    if (TableBytes.size() % 24U != 0 || Names.empty())
      return std::nullopt;
    std::vector<ELFSymbol> Result;
    Result.reserve(TableBytes.size() / 24U);
    for (std::size_t Offset = 0; Offset < TableBytes.size(); Offset += 24U) {
      const auto NameOffset = readU32(TableBytes, Offset);
      const auto Section = readU16(TableBytes, Offset + 6U);
      const auto Value = readU64(TableBytes, Offset + 8U);
      const auto Size = readU64(TableBytes, Offset + 16U);
      if (!NameOffset || !Section || !Value || !Size || *NameOffset >= Names.size())
        return std::nullopt;
      std::size_t End = *NameOffset;
      while (End < Names.size() && Names[End] != 0)
        ++End;
      if (End == Names.size())
        return std::nullopt;
      Result.push_back(ELFSymbol{
          .Name = std::string(reinterpret_cast<const char *>(Names.data() + *NameOffset),
                              End - *NameOffset),
          .Binding = static_cast<std::uint8_t>(TableBytes[Offset + 4U] >> 4U),
          .Type = static_cast<std::uint8_t>(TableBytes[Offset + 4U] & 0xfU),
          .Visibility = static_cast<std::uint8_t>(TableBytes[Offset + 5U] & 0x3U),
          .Section = *Section,
          .Value = *Value,
          .Size = *Size,
      });
    }
    return Result;
  }

  [[nodiscard]] std::optional<std::vector<ELFRelocation>> relocations(std::string_view Name) const {
    const ELFSection *Table = section(Name);
    if (Table == nullptr || Table->Type != SHTRela || Table->EntrySize != 24)
      return std::nullopt;
    const std::span<const std::uint8_t> TableBytes = contents(*Table);
    if (TableBytes.size() % 24U != 0)
      return std::nullopt;
    std::vector<ELFRelocation> Result;
    Result.reserve(TableBytes.size() / 24U);
    for (std::size_t Offset = 0; Offset < TableBytes.size(); Offset += 24U) {
      const auto RelocationOffset = readU64(TableBytes, Offset);
      const auto Info = readU64(TableBytes, Offset + 8U);
      const auto Addend = readU64(TableBytes, Offset + 16U);
      if (!RelocationOffset || !Info || !Addend)
        return std::nullopt;
      Result.push_back(ELFRelocation{
          .Offset = *RelocationOffset,
          .Symbol = static_cast<std::uint32_t>(*Info >> 32U),
          .Type = static_cast<std::uint32_t>(*Info),
          .Addend = std::bit_cast<std::int64_t>(*Addend),
      });
    }
    return Result;
  }

  [[nodiscard]] const std::vector<ELFSection> &sections() const noexcept {
    return Sections;
  }

private:
  explicit ELFView(std::span<const std::uint8_t> Bytes) : Bytes(Bytes) {}

  std::span<const std::uint8_t> Bytes;
  std::vector<ELFSection> Sections;
};

[[nodiscard]] const ELFSymbol *findSymbol(const std::vector<ELFSymbol> &Symbols,
                                          std::string_view Name) {
  const auto Position =
      std::ranges::find(Symbols, Name, [](const ELFSymbol &Symbol) { return Symbol.Name; });
  return Position == Symbols.end() ? nullptr : &*Position;
}

[[nodiscard]] rcc::Expected<rcc::Alignment> alignment(std::uint64_t Value) {
  return rcc::Alignment::create(Value);
}

[[nodiscard]] rcc::Expected<rcc::GlobalInitializer> integer(std::uint32_t Bits,
                                                            std::uint64_t Value) {
  auto Immediate = rcc::APInt::fromUnsigned(Bits, Value);
  if (!Immediate)
    return std::unexpected(Immediate.error());
  return rcc::GlobalInitializer::integer(std::move(*Immediate));
}

[[nodiscard]] bool addObject(rcc::CompilationUnit &Unit, std::string Name, std::uint64_t Size,
                             std::uint64_t Alignment, rcc::GlobalLinkage Linkage,
                             std::optional<rcc::GlobalInitializer> Initializer,
                             const auto &Configure) {
  auto Align = alignment(Alignment);
  if (!Align)
    return false;
  rcc::GlobalObjectDesc Object;
  Object.Symbol = std::move(Name);
  Object.ObjectType = rcc::GenericConstantType::byte();
  Object.Linkage = Linkage;
  Object.Align = *Align;
  Object.Size = Size;
  Object.Initializer = std::move(Initializer);
  Configure(Object);
  return Unit.addGlobalObject(std::move(Object)).has_value();
}

[[nodiscard]] rcc::Expected<rcc::mc::MCExprRef> symbolDifference(rcc::mc::MCContext &Context,
                                                                 std::string_view Left,
                                                                 std::string_view Right,
                                                                 std::int64_t Addend) {
  auto LeftSymbol = Context.getOrCreateSymbol(std::string(Left), rcc::mc::MCSymbolKind::Generic);
  auto RightSymbol = Context.getOrCreateSymbol(std::string(Right), rcc::mc::MCSymbolKind::Generic);
  if (!LeftSymbol || !RightSymbol)
    return std::unexpected(!LeftSymbol ? LeftSymbol.error() : RightSymbol.error());
  auto LeftExpression = rcc::mc::MCExpr::symbol(*LeftSymbol);
  auto RightExpression = rcc::mc::MCExpr::symbol(*RightSymbol);
  if (!LeftExpression || !RightExpression)
    return std::unexpected(!LeftExpression ? LeftExpression.error() : RightExpression.error());
  auto Difference = rcc::mc::MCExpr::binary(
      rcc::mc::MCBinaryOperator::Subtract, std::move(*LeftExpression), std::move(*RightExpression));
  if (!Difference)
    return std::unexpected(Difference.error());
  return rcc::mc::MCExpr::binary(rcc::mc::MCBinaryOperator::Add, std::move(*Difference),
                                 rcc::mc::MCExpr::constant(Addend));
}

[[nodiscard]] rcc::Expected<rcc::mc::MCExprRef>
symbolExpression(rcc::mc::MCContext &Context, std::string_view Name,
                 rcc::mc::MCSymbolKind Kind = rcc::mc::MCSymbolKind::Generic) {
  auto Symbol = Context.getOrCreateSymbol(std::string(Name), Kind);
  if (!Symbol)
    return std::unexpected(Symbol.error());
  return rcc::mc::MCExpr::symbol(std::move(*Symbol));
}

[[nodiscard]] rcc::mc::MCInst instruction(rcc::riscv::InstructionOpcode Opcode) {
  return rcc::mc::MCInst(rcc::mc::MCOpcode::fromValue(static_cast<std::uint32_t>(Opcode)));
}

void testMCRelaxation(TestContext &Context, const rcc::TargetDescription &Target) {
  rcc::riscv::RISCVMCCodeEmitter Emitter(Target);
  rcc::riscv::RISCVAsmBackend Backend;
  rcc::mc::MCContext MCContext;
  auto Text = MCContext.getOrCreateSection(rcc::mc::MCSectionDesc{
      .Name = ".text.relax",
      .Kind = rcc::mc::MCSectionKind::Text,
      .Flags = {rcc::mc::MCSectionFlag::Alloc, rcc::mc::MCSectionFlag::Executable},
      .Alignment = 2,
      .EntrySize = 0,
      .Group = {},
      .UniqueID = std::nullopt,
  });
  auto JumpTarget = MCContext.createTemporarySymbol(".Ljump_target");
  auto BranchTarget = MCContext.createTemporarySymbol(".Lbranch_target");
  if (!Text || !JumpTarget || !BranchTarget) {
    Context.expect(false, "RISC-V MC relaxation fixture construction must succeed");
    return;
  }
  auto JumpExpression = rcc::mc::MCExpr::symbol(*JumpTarget);
  auto BranchExpression = rcc::mc::MCExpr::symbol(*BranchTarget);
  if (!JumpExpression || !BranchExpression) {
    Context.expect(false, "RISC-V MC relaxation expressions must be constructible");
    return;
  }

  rcc::mc::MCInst Jump = instruction(rcc::riscv::InstructionOpcode::C_J);
  Jump.addOperand(rcc::mc::MCOperand::expression(*JumpExpression));
  rcc::mc::MCInst Branch = instruction(rcc::riscv::InstructionOpcode::C_BEQZ);
  Branch.addOperand(rcc::mc::MCOperand::registerOperand(rcc::mc::MCRegister::fromValue(
      static_cast<std::uint32_t>(rcc::riscv::PhysicalRegisterID::X8))));
  Branch.addOperand(rcc::mc::MCOperand::expression(*BranchExpression));

  rcc::mc::MCAssembler Assembler(Emitter, Backend);
  if (!Assembler.switchSection(*Text) || !Assembler.emitInstruction(Jump) ||
      !Assembler.emitZeroFill(3000) || !Assembler.emitLabel(*JumpTarget) ||
      !Assembler.emitInstruction(Branch) || !Assembler.emitZeroFill(600) ||
      !Assembler.emitLabel(*BranchTarget)) {
    Context.expect(false, "RISC-V MC relaxation events must succeed");
    return;
  }
  auto Object = Assembler.assemble();
  if (!Object)
    std::cerr << "RISC-V MC relaxation failure: " << Object.error().message() << '\n';
  Context.expect(Object.has_value(), "RISC-V MC relaxation must reach a fixed point");
  if (!Object || Object->Sections.size() != 1)
    return;
  const auto JumpWord = readU32(Object->Sections[0].Contents, 0);
  const auto BranchWord = readU32(Object->Sections[0].Contents, 3004);
  Context.expect(JumpWord && (*JumpWord & 0xfffU) == 0x6fU,
                 "out-of-range C.J must monotonically relax to JAL x0");
  Context.expect(BranchWord && (*BranchWord & 0x01fff07fU) == 0x00040063U,
                 "out-of-range C.BEQZ must monotonically relax to BEQ x8, x0");
  Context.expect(Object->Sections[0].AddressSize == 3608 && Object->Sections[0].Relocations.empty(),
                 "resolved MC relaxation must update layout without linker relocations");

  auto ExternalExpression = symbolExpression(MCContext, "external_jump");
  if (!ExternalExpression) {
    Context.expect(false, "external MC relaxation expression must be constructible");
    return;
  }
  rcc::mc::MCInst ExternalJump = instruction(rcc::riscv::InstructionOpcode::C_J);
  ExternalJump.addOperand(rcc::mc::MCOperand::expression(*ExternalExpression));
  rcc::mc::MCAssembler Relocatable(Emitter, Backend);
  auto Switched = Relocatable.switchSection(*Text);
  auto Emitted = Relocatable.emitInstruction(ExternalJump);
  auto ExternalObject = Relocatable.assemble();
  Context.expect(
      Switched && Emitted && ExternalObject &&
          ExternalObject->Sections[0].Relocations.size() == 1 &&
          ExternalObject->Sections[0].Relocations[0].Type == 45,
      "unresolved C.J must retain one mandatory relocation without implicit linker relax");
}

void testAssemblerLayout(TestContext &Context, const rcc::TargetDescription &Target) {
  rcc::riscv::RISCVMCCodeEmitter Emitter(Target);
  rcc::riscv::RISCVAsmBackend Backend;
  rcc::mc::MCContext MCContext;
  auto Data = MCContext.getOrCreateSection(rcc::mc::MCSectionDesc{
      .Name = ".data",
      .Kind = rcc::mc::MCSectionKind::Data,
      .Flags = {rcc::mc::MCSectionFlag::Alloc, rcc::mc::MCSectionFlag::Writable},
      .Alignment = 1,
      .EntrySize = 0,
      .Group = {},
      .UniqueID = std::nullopt,
  });
  auto BSS = MCContext.getOrCreateSection(rcc::mc::MCSectionDesc{
      .Name = ".bss",
      .Kind = rcc::mc::MCSectionKind::BSS,
      .Flags = {rcc::mc::MCSectionFlag::Alloc, rcc::mc::MCSectionFlag::Writable},
      .Alignment = 1,
      .EntrySize = 0,
      .Group = {},
      .UniqueID = std::nullopt,
  });
  auto Difference = symbolDifference(MCContext, "left", "right", 5);
  if (!Data || !BSS || !Difference) {
    Context.expect(false, "MC assembler layout fixture construction must succeed");
    return;
  }

  rcc::mc::MCAssembler Assembler(Emitter, Backend);
  constexpr std::uint64_t LargeBSS = (std::uint64_t{1} << 32U) + 32U;
  if (!Assembler.switchSection(*Data) || !Assembler.emitAlignment(128, 0, 0) ||
      !Assembler.emitValue(*Difference, 8) || !Assembler.switchSection(*BSS) ||
      !Assembler.emitAlignment(4096, 0, 0) || !Assembler.emitZeroFill(LargeBSS)) {
    Context.expect(false, "MC assembler layout events must succeed");
    return;
  }
  auto Object = Assembler.assemble();
  if (!Object)
    std::cerr << "MC assembler layout failure: " << Object.error().message() << '\n';
  Context.expect(Object.has_value(), "MC assembler layout must complete");
  if (!Object || Object->Sections.size() != 2)
    return;
  Context.expect(Object->Sections[0].Alignment == 128 &&
                     Object->Sections[0].Relocations.size() == 2 &&
                     Object->Sections[0].Relocations[0].Type == 36 &&
                     Object->Sections[0].Relocations[0].Addend == 5 &&
                     Object->Sections[0].Relocations[1].Type == 40 &&
                     Object->Sections[0].Relocations[1].Addend == 0,
                 "MC assembler must preserve effective alignment and paired symbol relocations");
  Context.expect(Object->Sections[1].Alignment == 4096 &&
                     Object->Sections[1].AddressSize == LargeBSS &&
                     Object->Sections[1].Contents.empty(),
                 "MC assembler must keep NOBITS address size without materializing zero bytes");

  rcc::mc::MCAssembler BadAssembler(Emitter, Backend);
  const std::array<std::uint8_t, 1> Nonzero{1};
  auto Switched = BadAssembler.switchSection(*BSS);
  auto Emitted = BadAssembler.emitBytes(Nonzero);
  auto Failure = BadAssembler.assemble();
  Context.expect(Switched && Emitted && !Failure &&
                     Failure.error().code() == rcc::ErrorCode::ObjectWritingFailed,
                 "MC assembler must reject initialized bytes in a NOBITS section");
}

void testNativeObjectBoundaries(TestContext &Context, const rcc::TargetDescription &Target) {
  rcc::riscv::RISCVMCCodeEmitter Emitter(Target);
  rcc::riscv::RISCVAsmBackend Backend;
  rcc::mc::ELF64LEObjectWriter Writer(rcc::mc::ELFObjectTargetInfo{
      .Machine = 243, .OSABI = 0, .ABIVersion = 0, .Flags = 0, .ExtraSections = {}});
  rcc::mc::ELFObjectStreamer Streamer(Emitter, Backend, Writer);
  auto EmptyAssembly = Streamer.emitInlineAssembly({});
  auto RawAssembly = Streamer.emitInlineAssembly("addi a0, a0, 1");
  Context.expect(EmptyAssembly && !RawAssembly &&
                     RawAssembly.error().code() == rcc::ErrorCode::Unsupported,
                 "native object emission must reject unparsed inline assembly without dropping it");

  rcc::mc::MCContext MCContext;
  auto EHFrame = MCContext.getOrCreateSection(rcc::mc::MCSectionDesc{
      .Name = ".eh_frame.boundary",
      .Kind = rcc::mc::MCSectionKind::EHFrame,
      .Flags = {rcc::mc::MCSectionFlag::Alloc},
      .Alignment = 8,
      .EntrySize = 0,
      .Group = {},
      .UniqueID = std::nullopt,
  });
  auto Function = MCContext.getOrCreateSymbol("cfi_boundary", rcc::mc::MCSymbolKind::Function);
  auto Personality =
      MCContext.getOrCreateSymbol("cfi_personality", rcc::mc::MCSymbolKind::Function);
  if (!EHFrame || !Function || !Personality) {
    Context.expect(false, "native CFI boundary fixture must be constructible");
    return;
  }
  const rcc::mc::MCDwarfFrameInfo Info{.CodeAlignment = 1,
                                       .DataAlignment = -8,
                                       .ReturnAddressRegister = 1,
                                       .InitialCFARegister = 2,
                                       .InitialCFAOffset = 0};
  const std::array UnsupportedEvents{
      rcc::mc::MCCFIInstruction{.Operation = rcc::mc::MCCFIOperation::Personality,
                                .Operands = {0},
                                .Bytes = {},
                                .Symbol = *Personality},
      rcc::mc::MCCFIInstruction{.Operation = rcc::mc::MCCFIOperation::LSDA,
                                .Operands = {0},
                                .Bytes = {},
                                .Symbol = *Personality},
      rcc::mc::MCCFIInstruction{.Operation = rcc::mc::MCCFIOperation::SignalFrame,
                                .Operands = {},
                                .Bytes = {},
                                .Symbol = {}},
      rcc::mc::MCCFIInstruction{.Operation = rcc::mc::MCCFIOperation::ReturnColumn,
                                .Operands = {1},
                                .Bytes = {},
                                .Symbol = {}},
      rcc::mc::MCCFIInstruction{.Operation = rcc::mc::MCCFIOperation::NegateReturnAddressState,
                                .Operands = {},
                                .Bytes = {},
                                .Symbol = {}},
      rcc::mc::MCCFIInstruction{.Operation =
                                    rcc::mc::MCCFIOperation::NegateReturnAddressStateWithPC,
                                .Operands = {},
                                .Bytes = {},
                                .Symbol = {}},
  };
  for (const rcc::mc::MCCFIInstruction &Event : UnsupportedEvents) {
    rcc::mc::MCDwarfFrameProcedure Procedure{
        .Function = *Function,
        .AddressRange = 4,
        .Instructions = {{.Instruction = Event, .Offset = 0}},
    };
    auto Frame = rcc::mc::emitDwarfEHFrame(*EHFrame, std::span(&Procedure, 1), Info, Backend);
    Context.expect(!Frame && Frame.error().code() == rcc::ErrorCode::Unsupported,
                   "unsupported RV64GC CFI extensions must fail structurally");
  }
}

[[nodiscard]] bool buildGlobalFixture(rcc::CompilationUnit &Unit) {
  constexpr auto NoConfiguration = [](rcc::GlobalObjectDesc &) {};
  if (!addObject(Unit, "external_target", 0, 1, rcc::GlobalLinkage::External, std::nullopt,
                 NoConfiguration) ||
      !addObject(Unit, "weak_target", 0, 1, rcc::GlobalLinkage::ExternalWeak, std::nullopt,
                 NoConfiguration)) {
    return false;
  }

  constexpr std::uint64_t LargeBSS = (std::uint64_t{1} << 32U) + 32U;
  auto LargeZero = rcc::GlobalInitializer::zeroFill(LargeBSS);
  auto TLSValue = integer(64, 0x1122334455667788ULL);
  auto TLSZero = rcc::GlobalInitializer::zeroFill(32);
  auto Relocation = rcc::GlobalInitializer::symbolAddress(
      {.Symbol = "external_target", .Addend = 7, .Size = 8, .TargetFlags = 0});
  auto COMDATRelocation = rcc::GlobalInitializer::symbolAddress(
      {.Symbol = "external_target", .Addend = 3, .Size = 8, .TargetFlags = 0});
  auto HiddenValue = integer(32, 0x12345678U);
  auto ProtectedValue = integer(64, 0x8877665544332211ULL);
  auto ReadOnlyValue = integer(32, 0xaabbccddU);
  auto TargetConstant = rcc::TargetConstant::create(rcc::riscv::RISCVSymbolConstant(
      rcc::riscv::RISCVSymbolConstantKind::ExternalSymbol, "external_target", 13));
  auto TargetInitializer =
      TargetConstant
          ? rcc::GlobalInitializer::target(std::move(*TargetConstant))
          : rcc::Expected<rcc::GlobalInitializer>(std::unexpected(TargetConstant.error()));
  if (!LargeZero || !TLSValue || !TLSZero || !Relocation || !COMDATRelocation || !HiddenValue ||
      !ProtectedValue || !ReadOnlyValue || !TargetInitializer) {
    return false;
  }

  std::vector<rcc::GlobalInitializer> Differences;
  for (const auto [Size, Addend] :
       std::array<std::pair<std::uint8_t, std::int64_t>, 4>{{{1, 11}, {2, 9}, {4, 7}, {8, 5}}}) {
    auto Difference = rcc::GlobalInitializer::symbolDifference({.Minuend = "external_target",
                                                                .Subtrahend = "weak_target",
                                                                .Addend = Addend,
                                                                .Size = Size});
    if (!Difference)
      return false;
    Differences.push_back(std::move(*Difference));
  }
  auto DifferenceAggregate = rcc::GlobalInitializer::aggregate(std::move(Differences));
  if (!DifferenceAggregate)
    return false;

  return addObject(Unit, "large_bss", LargeBSS, 4096, rcc::GlobalLinkage::Internal,
                   std::move(*LargeZero), NoConfiguration) &&
         addObject(Unit, "tls_data", 8, 8, rcc::GlobalLinkage::External, std::move(*TLSValue),
                   [](rcc::GlobalObjectDesc &Object) {
                     Object.TLSModel = rcc::GlobalTLSModel::LocalExec;
                   }) &&
         addObject(Unit, "tls_bss", 32, 32, rcc::GlobalLinkage::External, std::move(*TLSZero),
                   [](rcc::GlobalObjectDesc &Object) {
                     Object.TLSModel = rcc::GlobalTLSModel::GeneralDynamic;
                   }) &&
         addObject(Unit, "common_value", 128, 64, rcc::GlobalLinkage::Common, std::nullopt,
                   NoConfiguration) &&
         addObject(Unit, "relro_pointer", 8, 8, rcc::GlobalLinkage::External,
                   std::move(*Relocation),
                   [](rcc::GlobalObjectDesc &Object) { Object.IsConstant = true; }) &&
         addObject(Unit, "difference_value", 15, 1, rcc::GlobalLinkage::External,
                   std::move(*DifferenceAggregate), NoConfiguration) &&
         addObject(Unit, "comdat_value", 8, 8, rcc::GlobalLinkage::LinkOnceODR,
                   std::move(*COMDATRelocation),
                   [](rcc::GlobalObjectDesc &Object) { Object.COMDAT = Object.Symbol; }) &&
         addObject(Unit, "hidden_value", 4, 16, rcc::GlobalLinkage::External,
                   std::move(*HiddenValue),
                   [](rcc::GlobalObjectDesc &Object) {
                     Object.Visibility = rcc::GlobalVisibility::Hidden;
                     Object.Section = ".custom.data";
                   }) &&
         addObject(Unit, "protected_value", 8, 128, rcc::GlobalLinkage::External,
                   std::move(*ProtectedValue),
                   [](rcc::GlobalObjectDesc &Object) {
                     Object.Visibility = rcc::GlobalVisibility::Protected;
                   }) &&
         addObject(Unit, "readonly_value", 4, 4, rcc::GlobalLinkage::External,
                   std::move(*ReadOnlyValue),
                   [](rcc::GlobalObjectDesc &Object) { Object.IsConstant = true; }) &&
         addObject(Unit, "target_constant_value", 8, 8, rcc::GlobalLinkage::Internal,
                   std::move(*TargetInitializer), [](rcc::GlobalObjectDesc &Object) {
                     Object.IsConstant = true;
                     Object.Section = ".data.rel.ro.target-constant";
                   });
}

void testGlobalObjectEmission(TestContext &Context, const rcc::TargetDescription &Target) {
  rcc::CompilationUnit Unit;
  Context.expect(buildGlobalFixture(Unit), "global object fixture construction must succeed");
  auto First = rcc::riscv::emitObject(Unit, Target);
  auto Second = rcc::riscv::emitObject(Unit, Target);
  if (!First)
    std::cerr << "first global object emission failure: " << First.error().message() << '\n';
  if (!Second)
    std::cerr << "second global object emission failure: " << Second.error().message() << '\n';
  Context.expect(First && Second && *First == *Second,
                 "global object emission must be successful and deterministic");
  if (!First)
    return;
  Context.expect(First->size() < std::size_t{64} * 1024U,
                 "large NOBITS globals must not inflate the ELF file");
  auto View = ELFView::create(*First);
  Context.expect(View.has_value(), "emitted global object must be a valid ELF64LE container");
  if (!View)
    return;

  const ELFSection *BSS = View->section(".bss");
  const ELFSection *Data = View->section(".data");
  const ELFSection *TLSData = View->section(".tdata");
  const ELFSection *TLSBSS = View->section(".tbss");
  const ELFSection *RelRO = View->section(".data.rel.ro");
  const ELFSection *COMDAT = View->section(".data.comdat_value");
  const ELFSection *COMDATRelocations = View->section(".rela.data.comdat_value");
  const ELFSection *Group = View->section(".group");
  constexpr std::uint64_t LargeBSS = (std::uint64_t{1} << 32U) + 32U;
  Context.expect(BSS != nullptr && BSS->Type == SHTNoBits && BSS->Size == LargeBSS &&
                     BSS->Alignment == 4096 &&
                     (BSS->Flags & (SHFWrite | SHFAlloc)) == (SHFWrite | SHFAlloc),
                 "ELF BSS section must preserve its address size, flags, and alignment");
  Context.expect(Data != nullptr && Data->Alignment == 128,
                 "ELF data section alignment must include later fragment requirements");
  Context.expect(TLSData != nullptr && TLSData->Type != SHTNoBits &&
                     (TLSData->Flags & SHFTLS) != 0 && TLSBSS != nullptr &&
                     TLSBSS->Type == SHTNoBits && (TLSBSS->Flags & SHFTLS) != 0,
                 "ELF TLS data and BSS sections must carry their TLS semantics");
  Context.expect(RelRO != nullptr && COMDAT != nullptr && COMDATRelocations != nullptr &&
                     Group != nullptr && Group->Type == SHTGroup &&
                     (COMDAT->Flags & SHFGroup) != 0 && (COMDATRelocations->Flags & SHFGroup) != 0,
                 "ELF COMDAT data and relocation sections must share a group");
  Context.expect(View->section(".custom.data") != nullptr && View->section(".rodata") != nullptr &&
                     View->section(".riscv.attributes") != nullptr,
                 "ELF custom, read-only, and RISC-V attribute sections must be present");
  const ELFSection *TargetConstantSection = View->section(".data.rel.ro.target-constant");
  Context.expect(TargetConstantSection != nullptr && TargetConstantSection->Size == 8 &&
                     std::ranges::all_of(View->contents(*TargetConstantSection),
                                         [](std::uint8_t Byte) { return Byte == 0; }),
                 "RV64GC target constants must reserve their relocation storage");

  auto Symbols = View->symbols();
  Context.expect(Symbols.has_value(), "ELF symbol table must be parseable");
  if (!Symbols)
    return;
  const ELFSymbol *External = findSymbol(*Symbols, "external_target");
  const ELFSymbol *Weak = findSymbol(*Symbols, "weak_target");
  const ELFSymbol *Large = findSymbol(*Symbols, "large_bss");
  const ELFSymbol *TLS = findSymbol(*Symbols, "tls_data");
  const ELFSymbol *Common = findSymbol(*Symbols, "common_value");
  const ELFSymbol *Hidden = findSymbol(*Symbols, "hidden_value");
  const ELFSymbol *Protected = findSymbol(*Symbols, "protected_value");
  const ELFSymbol *COMDATSymbol = findSymbol(*Symbols, "comdat_value");
  Context.expect(External != nullptr && External->Binding == 1 &&
                     External->Section == SHNUndefined && External->Type == 1 && Weak != nullptr &&
                     Weak->Binding == 2 && Weak->Section == SHNUndefined,
                 "undefined object declarations must retain global and weak bindings");
  Context.expect(Large != nullptr && Large->Binding == 0 && Large->Size == LargeBSS &&
                     TLS != nullptr && TLS->Binding == 1 && TLS->Type == 6,
                 "local BSS and global TLS symbols must have correct ELF types and sizes");
  Context.expect(Common != nullptr && Common->Binding == 1 && Common->Section == SHNCommon &&
                     Common->Value == 64 && Common->Size == 128,
                 "ELF common symbol alignment and size must be preserved");
  Context.expect(Hidden != nullptr && Hidden->Visibility == 2 && Protected != nullptr &&
                     Protected->Visibility == 3 && COMDATSymbol != nullptr &&
                     COMDATSymbol->Binding == 2,
                 "ELF visibility and COMDAT weak binding must be preserved");

  auto DifferenceRelocations = View->relocations(".rela.data");
  Context.expect(DifferenceRelocations && DifferenceRelocations->size() == 8,
                 "symbol differences must emit eight paired ELF relocations");
  if (DifferenceRelocations) {
    const std::array<std::uint32_t, 8> ExpectedTypes{33, 37, 34, 38, 35, 39, 36, 40};
    for (std::size_t Index = 0; Index < ExpectedTypes.size(); ++Index) {
      Context.expect((*DifferenceRelocations)[Index].Type == ExpectedTypes[Index],
                     "symbol difference relocation type order is incorrect");
    }
    const std::array<std::int64_t, 8> ExpectedAddends{11, 0, 9, 0, 7, 0, 5, 0};
    for (std::size_t Index = 0; Index < ExpectedAddends.size(); ++Index) {
      Context.expect((*DifferenceRelocations)[Index].Addend == ExpectedAddends[Index],
                     "symbol difference relocation addend is incorrect");
    }
  }
  auto Relocations = View->relocations(".rela.data.rel.ro");
  Context.expect(Relocations && Relocations->size() == 1 && (*Relocations)[0].Type == 2 &&
                     (*Relocations)[0].Addend == 7,
                 "read-only pointer data must emit an R_RISCV_64 relocation");
  auto COMDATFixups = View->relocations(".rela.data.comdat_value");
  Context.expect(COMDATFixups && COMDATFixups->size() == 1 && (*COMDATFixups)[0].Type == 2 &&
                     (*COMDATFixups)[0].Addend == 3,
                 "COMDAT pointer data must retain its relocation");
  auto TargetConstantFixups = View->relocations(".rela.data.rel.ro.target-constant");
  Context.expect(TargetConstantFixups && TargetConstantFixups->size() == 1 &&
                     (*TargetConstantFixups)[0].Type == 2 &&
                     (*TargetConstantFixups)[0].Addend == 13,
                 "RV64GC target constants must emit their declared R_RISCV_64 relocation");

  if (Group != nullptr && COMDATSymbol != nullptr) {
    const std::span<const std::uint8_t> GroupBytes = View->contents(*Group);
    const auto GroupFlags = readU32(GroupBytes, 0);
    const auto COMDATIndex = View->sectionIndex(".data.comdat_value");
    const auto RelocationIndex = View->sectionIndex(".rela.data.comdat_value");
    bool ContainsCOMDAT = false;
    bool ContainsRelocations = false;
    for (std::size_t Offset = 4; Offset + 4 <= GroupBytes.size(); Offset += 4) {
      const auto Member = readU32(GroupBytes, Offset);
      ContainsCOMDAT |= Member && COMDATIndex && *Member == *COMDATIndex;
      ContainsRelocations |= Member && RelocationIndex && *Member == *RelocationIndex;
    }
    Context.expect(GroupFlags == 1 && Group->Info < Symbols->size() &&
                       (*Symbols)[Group->Info].Name == "comdat_value" && ContainsCOMDAT &&
                       ContainsRelocations,
                   "ELF COMDAT group must name its signature and both member sections");
  }
}

} // namespace

int main() {
  TestContext Context;
  auto Target = rcc::riscv::createTargetDescription();
  Context.expect(Target.has_value(), "RISC-V object test target creation must succeed");
  if (!Target)
    return Context.result();
  testAssemblerLayout(Context, *Target);
  testMCRelaxation(Context, *Target);
  testNativeObjectBoundaries(Context, *Target);
  testGlobalObjectEmission(Context, *Target);
  return Context.result();
}
