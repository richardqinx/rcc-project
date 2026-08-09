// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/MC/ELFObjectWriter.h"
#include "rcc/MC/MCAsmBackend.h"
#include "rcc/MC/MCAssembler.h"
#include "rcc/MC/MCCodeEmitter.h"
#include "rcc/MC/MCContext.h"
#include "rcc/MC/MCExpr.h"
#include "rcc/MC/MCFixup.h"
#include "rcc/MC/MCInst.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace {

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

class TestCodeEmitter final : public rcc::mc::MCCodeEmitter {
public:
  [[nodiscard]] rcc::Expected<rcc::mc::MCEncodedInstruction>
  encodeInstruction(const rcc::mc::MCInst &Instruction) const override {
    if (FailNextEncoding) {
      FailNextEncoding = false;
      return rcc::makeUnexpected(rcc::ErrorCode::InstructionEncodingFailed,
                                 "intentional test encoding failure");
    }
    if (Instruction.opcode().value() != 1 || !Instruction.operands().empty()) {
      return rcc::makeUnexpected(rcc::ErrorCode::InstructionEncodingFailed,
                                 "unknown test instruction");
    }
    return rcc::mc::MCEncodedInstruction{.Bytes = {0x11, 0x22, 0x33, 0x44}, .Fixups = {}};
  }

  mutable bool FailNextEncoding = false;
};

class TestAsmBackend final : public rcc::mc::MCAsmBackend {
public:
  [[nodiscard]] rcc::Expected<rcc::mc::MCFixupKindInfo>
  fixupKindInfo(rcc::mc::MCFixupKind Kind) const override {
    switch (Kind) {
    case rcc::mc::fixup_kind::Data1:
      return rcc::mc::MCFixupKindInfo{.StorageSize = 1, .PCRelative = false};
    case rcc::mc::fixup_kind::Data2:
      return rcc::mc::MCFixupKindInfo{.StorageSize = 2, .PCRelative = false};
    case rcc::mc::fixup_kind::Data4:
      return rcc::mc::MCFixupKindInfo{.StorageSize = 4, .PCRelative = false};
    case rcc::mc::fixup_kind::Data8:
      return rcc::mc::MCFixupKindInfo{.StorageSize = 8, .PCRelative = false};
    case rcc::mc::fixup_kind::PCRelative4:
      return rcc::mc::MCFixupKindInfo{.StorageSize = 4, .PCRelative = true};
    default:
      return rcc::makeUnexpected(rcc::ErrorCode::InvalidArgument, "unknown test fixup kind");
    }
  }

  [[nodiscard]] rcc::Expected<void> applyFixup(rcc::mc::MCFixupKind Kind,
                                               std::span<std::uint8_t> Data, std::uint64_t Offset,
                                               std::int64_t Value) const override {
    auto Info = fixupKindInfo(Kind);
    if (!Info)
      return std::unexpected(Info.error());
    if (Offset > Data.size() ||
        Info->StorageSize > Data.size() - static_cast<std::size_t>(Offset)) {
      return rcc::makeUnexpected(rcc::ErrorCode::FixupApplicationFailed,
                                 "test fixup exceeds its section");
    }
    const std::uint64_t Bits = static_cast<std::uint64_t>(Value);
    for (std::uint8_t Index = 0; Index < Info->StorageSize; ++Index)
      Data[static_cast<std::size_t>(Offset) + Index] =
          static_cast<std::uint8_t>(Bits >> (Index * 8U));
    return {};
  }

  [[nodiscard]] rcc::Expected<std::uint32_t>
  relocationType(const rcc::mc::MCFixup &, const rcc::mc::MCValue &Value) const override {
    if (!Value.AddSymbol || Value.SubtractSymbol)
      return rcc::makeUnexpected(rcc::ErrorCode::RelocationFailed,
                                 "test relocation requires one symbol");
    return 77;
  }

  [[nodiscard]] std::uint32_t objectFlags() const noexcept override {
    return 0x20;
  }
};

[[nodiscard]] rcc::mc::MCInst testInstruction() {
  return rcc::mc::MCInst(rcc::mc::MCOpcode::fromValue(1));
}

[[nodiscard]] rcc::Expected<rcc::mc::MCExprRef>
symbolExpression(const rcc::mc::MCSymbolRef &Symbol) {
  return rcc::mc::MCExpr::symbol(Symbol);
}

[[nodiscard]] rcc::Expected<rcc::mc::MCExprRef> addConstant(const rcc::mc::MCSymbolRef &Symbol,
                                                            std::int64_t Addend) {
  auto Base = symbolExpression(Symbol);
  if (!Base)
    return std::unexpected(Base.error());
  return rcc::mc::MCExpr::binary(rcc::mc::MCBinaryOperator::Add, *Base,
                                 rcc::mc::MCExpr::constant(Addend));
}

[[nodiscard]] rcc::Expected<rcc::mc::MCExprRef> subtractSymbols(const rcc::mc::MCSymbolRef &Left,
                                                                const rcc::mc::MCSymbolRef &Right) {
  auto LeftExpression = symbolExpression(Left);
  auto RightExpression = symbolExpression(Right);
  if (!LeftExpression)
    return std::unexpected(LeftExpression.error());
  if (!RightExpression)
    return std::unexpected(RightExpression.error());
  return rcc::mc::MCExpr::binary(rcc::mc::MCBinaryOperator::Subtract, *LeftExpression,
                                 *RightExpression);
}

[[nodiscard]] rcc::Expected<rcc::mc::MCSectionRef>
createSection(rcc::mc::MCContext &Context, std::string_view Name, rcc::mc::MCSectionKind Kind,
              rcc::mc::MCSectionFlags Flags, std::uint64_t Alignment) {
  return Context.getOrCreateSection(rcc::mc::MCSectionDesc{
      .Name = std::string(Name),
      .Kind = Kind,
      .Flags = Flags,
      .Alignment = Alignment,
      .EntrySize = 0,
      .Group = {},
      .UniqueID = std::nullopt,
  });
}

[[nodiscard]] const rcc::mc::MCObjectSymbol *findSymbol(const rcc::mc::MCAssembledObject &Object,
                                                        std::string_view Name) {
  const auto Position = std::ranges::find_if(Object.Symbols, [&](const auto &Symbol) {
    return Symbol.Symbol && Symbol.Symbol->name() == Name;
  });
  return Position == Object.Symbols.end() ? nullptr : &*Position;
}

[[nodiscard]] rcc::Expected<rcc::mc::MCAssembledObject>
assembleFixture(rcc::mc::MCContext &Context, const TestCodeEmitter &Emitter,
                const TestAsmBackend &Backend) {
  auto Text = createSection(Context, ".text", rcc::mc::MCSectionKind::Text,
                            {rcc::mc::MCSectionFlag::Alloc, rcc::mc::MCSectionFlag::Executable}, 4);
  auto Data = createSection(Context, ".data", rcc::mc::MCSectionKind::Data,
                            {rcc::mc::MCSectionFlag::Alloc, rcc::mc::MCSectionFlag::Writable}, 1);
  auto BSS = createSection(Context, ".bss", rcc::mc::MCSectionKind::BSS,
                           {rcc::mc::MCSectionFlag::Alloc, rcc::mc::MCSectionFlag::Writable}, 1);
  auto Function = Context.getOrCreateSymbol("function", rcc::mc::MCSymbolKind::Function);
  auto End = Context.createTemporarySymbol(".Lend");
  auto External = Context.getOrCreateSymbol("external", rcc::mc::MCSymbolKind::Generic);
  if (!Text || !Data || !BSS || !Function || !End || !External)
    return rcc::makeUnexpected(rcc::ErrorCode::InternalError,
                               "failed to create the MC object fixture");
  auto FunctionSize = subtractSymbols(*End, *Function);
  auto ExternalValue = addConstant(*External, 7);
  if (!FunctionSize || !ExternalValue)
    return rcc::makeUnexpected(rcc::ErrorCode::InternalError,
                               "failed to create the MC object expressions");

  rcc::mc::MCAssembler Assembler(Emitter, Backend);
  constexpr std::uint64_t LargeBSS = (std::uint64_t{1} << 32U) + 17U;
  if (!Assembler.switchSection(*Text) ||
      !Assembler.emitSymbolAttribute(*Function, rcc::mc::MCSymbolAttribute::Global) ||
      !Assembler.emitSymbolAttribute(*Function, rcc::mc::MCSymbolAttribute::Function) ||
      !Assembler.emitLabel(*Function) || !Assembler.emitInstruction(testInstruction()) ||
      !Assembler.emitAlignment(8, 0xaa, 0) || !Assembler.emitLabel(*End) ||
      !Assembler.emitSymbolSize(*Function, *FunctionSize) || !Assembler.switchSection(*Data) ||
      !Assembler.emitAlignment(16, 0, 0) || !Assembler.emitValue(*ExternalValue, 8) ||
      !Assembler.switchSection(*BSS) || !Assembler.emitAlignment(4096, 0, 0) ||
      !Assembler.emitZeroFill(LargeBSS)) {
    return rcc::makeUnexpected(rcc::ErrorCode::InternalError,
                               "failed to emit the MC object fixture");
  }
  return Assembler.assemble();
}

void testAssembler(TestContext &Test, const TestCodeEmitter &Emitter,
                   const TestAsmBackend &Backend) {
  rcc::mc::MCContext Context;
  auto Object = assembleFixture(Context, Emitter, Backend);
  if (!Object) {
    std::cerr << "MC object fixture assembly failed: " << Object.error().message() << '\n';
    Test.expect(false, "MC assembler fixture must assemble");
    return;
  }
  Test.expect(Object->Flags == 0x20 && Object->Sections.size() == 3,
              "MC assembler must preserve target flags and section order");
  if (Object->Sections.size() != 3)
    return;
  Test.expect(Object->Sections[0].Alignment == 8 && Object->Sections[0].AddressSize == 8 &&
                  Object->Sections[0].Contents ==
                      std::vector<std::uint8_t>({0x11, 0x22, 0x33, 0x44, 0xaa, 0xaa, 0xaa, 0xaa}),
              "MC assembler must materialize instructions and alignment padding");
  Test.expect(Object->Sections[1].Alignment == 16 && Object->Sections[1].AddressSize == 8 &&
                  Object->Sections[1].Contents == std::vector<std::uint8_t>(8) &&
                  Object->Sections[1].Relocations.size() == 1 &&
                  Object->Sections[1].Relocations[0].Offset == 0 &&
                  Object->Sections[1].Relocations[0].Type == 77 &&
                  Object->Sections[1].Relocations[0].Addend == 7,
              "MC assembler must preserve unresolved symbol relocations");
  constexpr std::uint64_t LargeBSS = (std::uint64_t{1} << 32U) + 17U;
  Test.expect(Object->Sections[2].Alignment == 4096 &&
                  Object->Sections[2].AddressSize == LargeBSS &&
                  Object->Sections[2].Contents.empty(),
              "MC assembler must not materialize NOBITS storage");

  const rcc::mc::MCObjectSymbol *Function = findSymbol(*Object, "function");
  const rcc::mc::MCObjectSymbol *External = findSymbol(*Object, "external");
  Test.expect(Function != nullptr && Function->Binding == rcc::mc::MCObjectSymbolBinding::Global &&
                  Function->Type == rcc::mc::MCObjectSymbolType::Function &&
                  Function->SectionIndex == 0 && Function->Value == 0 && Function->Size == 8,
              "MC assembler must resolve function binding, type, value, and size");
  Test.expect(External != nullptr && External->Binding == rcc::mc::MCObjectSymbolBinding::Global &&
                  !External->SectionIndex && External->Value == 0,
              "unqualified undefined symbols must become global ELF symbols");
}

void testAssemblerAtomicity(TestContext &Test, TestCodeEmitter &Emitter,
                            const TestAsmBackend &Backend) {
  rcc::mc::MCContext Context;
  auto Text = createSection(Context, ".text.atomic", rcc::mc::MCSectionKind::Text,
                            {rcc::mc::MCSectionFlag::Alloc, rcc::mc::MCSectionFlag::Executable}, 4);
  auto Symbol = Context.getOrCreateSymbol("atomic_function", rcc::mc::MCSymbolKind::Function);
  if (!Text || !Symbol) {
    Test.expect(false, "MC assembler atomicity fixture must be constructible");
    return;
  }
  rcc::mc::MCAssembler Assembler(Emitter, Backend);
  if (!Assembler.switchSection(*Text) || !Assembler.emitLabel(*Symbol) ||
      !Assembler.emitInstruction(testInstruction())) {
    Test.expect(false, "MC assembler atomicity events must succeed");
    return;
  }

  Emitter.FailNextEncoding = true;
  auto Failed = Assembler.assemble();
  Test.expect(!Failed && Failed.error().code() == rcc::ErrorCode::InstructionEncodingFailed,
              "MC assembler must propagate an encoding failure");
  Test.expect(Assembler.emitAlignment(8, 0, 0).has_value(),
              "failed assembly must leave the input event stream mutable and unchanged");
  auto Succeeded = Assembler.assemble();
  Test.expect(Succeeded.has_value() && Succeeded->Sections.size() == 1 &&
                  Succeeded->Sections[0].AddressSize == 8,
              "MC assembler must support a clean retry after a failed transaction");
  if (!Succeeded)
    return;
  auto Size = rcc::mc::MCExpr::constant(4);
  Test.expect(!Assembler.switchSection(*Text) && !Assembler.emitLabel(*Symbol) &&
                  !Assembler.emitInstruction(testInstruction()) && !Assembler.emitValue(Size, 4) &&
                  !Assembler.emitZeroFill(1) && !Assembler.emitAlignment(4, 0, 0) &&
                  !Assembler.emitSymbolAttribute(*Symbol, rcc::mc::MCSymbolAttribute::Global) &&
                  !Assembler.emitSymbolSize(*Symbol, Size) &&
                  !Assembler.emitCommonSymbol(*Symbol, 4, 4) && !Assembler.assemble(),
              "successful assembly must make every mutation and repeated finalization terminal");
}

void testSymbolSizeValidation(TestContext &Test, const TestCodeEmitter &Emitter,
                              const TestAsmBackend &Backend) {
  rcc::mc::MCContext Context;
  auto Text = createSection(Context, ".text.size", rcc::mc::MCSectionKind::Text,
                            {rcc::mc::MCSectionFlag::Alloc, rcc::mc::MCSectionFlag::Executable}, 4);
  auto Data = createSection(Context, ".data.size", rcc::mc::MCSectionKind::Data,
                            {rcc::mc::MCSectionFlag::Alloc, rcc::mc::MCSectionFlag::Writable}, 4);
  auto Function = Context.getOrCreateSymbol("sized_function", rcc::mc::MCSymbolKind::Function);
  auto DataEnd = Context.createTemporarySymbol(".Ldata_end");
  if (!Text || !Data || !Function || !DataEnd) {
    Test.expect(false, "symbol-size validation fixture must be constructible");
    return;
  }

  auto CrossSectionSize = subtractSymbols(*DataEnd, *Function);
  auto FunctionExpression = symbolExpression(*Function);
  if (!CrossSectionSize || !FunctionExpression) {
    Test.expect(false, "symbol-size validation expressions must be constructible");
    return;
  }
  auto TargetSize = rcc::mc::MCExpr::target(1, *FunctionExpression);
  if (!TargetSize) {
    Test.expect(false, "target symbol-size expression must be constructible");
    return;
  }

  rcc::mc::MCAssembler CrossSectionAssembler(Emitter, Backend);
  if (!CrossSectionAssembler.switchSection(*Text) || !CrossSectionAssembler.emitLabel(*Function) ||
      !CrossSectionAssembler.emitInstruction(testInstruction()) ||
      !CrossSectionAssembler.switchSection(*Data) || !CrossSectionAssembler.emitLabel(*DataEnd) ||
      !CrossSectionAssembler.emitSymbolSize(*Function, *CrossSectionSize)) {
    Test.expect(false, "cross-section symbol-size events must succeed before finalization");
    return;
  }
  auto CrossSectionResult = CrossSectionAssembler.assemble();
  Test.expect(!CrossSectionResult &&
                  CrossSectionResult.error().code() == rcc::ErrorCode::ObjectWritingFailed,
              "MC assembler must reject a symbol-size difference across sections");

  rcc::mc::MCAssembler TargetExpressionAssembler(Emitter, Backend);
  if (!TargetExpressionAssembler.switchSection(*Text) ||
      !TargetExpressionAssembler.emitLabel(*Function) ||
      !TargetExpressionAssembler.emitInstruction(testInstruction()) ||
      !TargetExpressionAssembler.emitSymbolSize(*Function, *TargetSize)) {
    Test.expect(false, "target symbol-size events must succeed before finalization");
    return;
  }
  auto TargetExpressionResult = TargetExpressionAssembler.assemble();
  Test.expect(!TargetExpressionResult &&
                  TargetExpressionResult.error().code() == rcc::ErrorCode::ObjectWritingFailed,
              "MC assembler must reject a target expression as a symbol size");
}

[[nodiscard]] std::uint16_t readU16(std::span<const std::uint8_t> Bytes, std::size_t Offset) {
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(Bytes[Offset]) |
                                    static_cast<std::uint16_t>(Bytes[Offset + 1U]) << 8U);
}

[[nodiscard]] std::uint32_t readU32(std::span<const std::uint8_t> Bytes, std::size_t Offset) {
  std::uint32_t Result = 0;
  for (std::uint32_t Index = 0; Index < 4; ++Index)
    Result |= static_cast<std::uint32_t>(Bytes[Offset + Index]) << (Index * 8U);
  return Result;
}

[[nodiscard]] bool contains(std::span<const std::uint8_t> Bytes, std::string_view Text) {
  if (Text.size() > Bytes.size())
    return false;
  const auto *Data = reinterpret_cast<const char *>(Bytes.data());
  return std::string_view(Data, Bytes.size()).find(Text) != std::string_view::npos;
}

void testELFWriter(TestContext &Test, const TestCodeEmitter &Emitter,
                   const TestAsmBackend &Backend) {
  rcc::mc::MCContext Context;
  auto Object = assembleFixture(Context, Emitter, Backend);
  if (!Object) {
    Test.expect(false, "ELF writer fixture must assemble");
    return;
  }
  rcc::mc::ELFObjectTargetInfo Target{
      .Machine = 243,
      .OSABI = 3,
      .ABIVersion = 1,
      .Flags = 1,
      .ExtraSections = {{.Name = ".note.test",
                         .Type = 7,
                         .Flags = 0,
                         .Alignment = 4,
                         .EntrySize = 0,
                         .Contents = {1, 2, 3, 4}}},
  };
  rcc::mc::ELF64LEObjectWriter Writer(Target);
  auto First = Writer.writeObject(*Object);
  auto Second = Writer.writeObject(*Object);
  Test.expect(First.has_value() && Second.has_value() && *First == *Second,
              "ELF writer output must be deterministic");
  if (!First)
    return;
  Test.expect(First->size() < 4096 && First->size() >= 64 &&
                  std::ranges::equal(std::span<const std::uint8_t>(*First).first(4),
                                     std::array<std::uint8_t, 4>{0x7f, 'E', 'L', 'F'}) &&
                  readU16(*First, 16) == 1 && readU16(*First, 18) == 243 &&
                  readU32(*First, 48) == 0x21 && contains(*First, ".bss") &&
                  contains(*First, ".rela.data") && contains(*First, ".note.test") &&
                  contains(*First, "function") && contains(*First, "external"),
              "ELF writer must encode its header, sparse BSS, sections, symbols, and relocation");

  rcc::mc::ELFObjectTargetInfo MissingMachine = Target;
  MissingMachine.Machine = 0;
  Test.expect(!rcc::mc::ELF64LEObjectWriter(MissingMachine).writeObject(*Object),
              "ELF writer must reject an unconfigured target machine");

  auto DuplicateSection = *Object;
  DuplicateSection.Sections.push_back(DuplicateSection.Sections.front());
  Test.expect(!Writer.writeObject(DuplicateSection),
              "ELF writer must reject duplicate section identities");

  auto BadAlignment = *Object;
  BadAlignment.Sections[0].Alignment = 3;
  Test.expect(!Writer.writeObject(BadAlignment),
              "ELF writer must reject a non-power-of-two effective alignment");

  auto BadContents = *Object;
  ++BadContents.Sections[0].AddressSize;
  Test.expect(!Writer.writeObject(BadContents),
              "ELF writer must reject PROGBITS size/content disagreement");

  auto BadNoBits = *Object;
  BadNoBits.Sections[2].Relocations.push_back(rcc::mc::MCObjectRelocation{
      .Offset = 0, .Type = 1, .Symbol = Object->Symbols.back().Symbol, .Addend = 0});
  Test.expect(!Writer.writeObject(BadNoBits),
              "ELF writer must reject relocations in a NOBITS section");

  auto DuplicateSymbol = *Object;
  DuplicateSymbol.Symbols.push_back(DuplicateSymbol.Symbols.front());
  Test.expect(!Writer.writeObject(DuplicateSymbol),
              "ELF writer must reject duplicate symbol identities");

  auto OutOfRangeSymbol = *Object;
  auto Defined = std::ranges::find_if(
      OutOfRangeSymbol.Symbols, [](const auto &Symbol) { return Symbol.SectionIndex.has_value(); });
  if (Defined != OutOfRangeSymbol.Symbols.end()) {
    const std::size_t SectionIndex =
        Defined->SectionIndex.value_or(OutOfRangeSymbol.Sections.size());
    Defined->Value = OutOfRangeSymbol.Sections[SectionIndex].AddressSize + 1U;
    Test.expect(!Writer.writeObject(OutOfRangeSymbol),
                "ELF writer must reject a symbol outside its defining section");
  }

  auto OutOfRangeSymbolSize = *Object;
  auto Sized = std::ranges::find_if(OutOfRangeSymbolSize.Symbols, [](const auto &Symbol) {
    return Symbol.SectionIndex.has_value() && Symbol.Size != 0;
  });
  if (Sized != OutOfRangeSymbolSize.Symbols.end()) {
    const std::size_t SectionIndex =
        Sized->SectionIndex.value_or(OutOfRangeSymbolSize.Sections.size());
    Sized->Size = OutOfRangeSymbolSize.Sections[SectionIndex].AddressSize - Sized->Value + 1U;
    Test.expect(!Writer.writeObject(OutOfRangeSymbolSize),
                "ELF writer must reject a symbol range outside its defining section");
  }

  auto BadCommon = *Object;
  auto Undefined = std::ranges::find_if(BadCommon.Symbols,
                                        [](const auto &Symbol) { return !Symbol.SectionIndex; });
  if (Undefined != BadCommon.Symbols.end()) {
    Undefined->Binding = rcc::mc::MCObjectSymbolBinding::Local;
    Undefined->Value = 3;
    Undefined->Size = 4;
    Undefined->CommonAlignment = 3;
    Test.expect(!Writer.writeObject(BadCommon),
                "ELF writer must reject an invalid common symbol definition");
  }

  auto BadRelocationOffset = *Object;
  BadRelocationOffset.Sections[1].Relocations[0].Offset =
      BadRelocationOffset.Sections[1].AddressSize;
  Test.expect(!Writer.writeObject(BadRelocationOffset),
              "ELF writer must reject a relocation outside its section");

  rcc::mc::MCContext OtherContext;
  auto Unknown = OtherContext.getOrCreateSymbol("unknown", rcc::mc::MCSymbolKind::Generic);
  if (Unknown) {
    auto UnknownRelocation = *Object;
    UnknownRelocation.Sections[1].Relocations[0].Symbol = *Unknown;
    Test.expect(!Writer.writeObject(UnknownRelocation),
                "ELF writer must reject a relocation to an unknown symbol");
  }

  auto BadExtraTarget = Target;
  BadExtraTarget.ExtraSections[0].Alignment = 3;
  Test.expect(!rcc::mc::ELF64LEObjectWriter(std::move(BadExtraTarget)).writeObject(*Object),
              "ELF writer must reject an invalid target extra section");

  auto ManagedExtraTarget = Target;
  ManagedExtraTarget.ExtraSections[0].Type = 2;
  Test.expect(!rcc::mc::ELF64LEObjectWriter(std::move(ManagedExtraTarget)).writeObject(*Object),
              "ELF writer must reject a writer-managed target extra section type");

  auto ExtendedNumberingTarget = Target;
  ExtendedNumberingTarget.ExtraSections.resize(0xff00U);
  auto ExtendedNumbering =
      rcc::mc::ELF64LEObjectWriter(std::move(ExtendedNumberingTarget)).writeObject(*Object);
  Test.expect(!ExtendedNumbering && ExtendedNumbering.error().code() == rcc::ErrorCode::Unsupported,
              "ELF writer must reject section counts that require extended numbering");
}

} // namespace

int main() {
  TestContext Test;
  TestCodeEmitter Emitter;
  TestAsmBackend Backend;
  testAssembler(Test, Emitter, Backend);
  testAssemblerAtomicity(Test, Emitter, Backend);
  testSymbolSizeValidation(Test, Emitter, Backend);
  testELFWriter(Test, Emitter, Backend);
  return Test.result();
}
