// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/Target/RISCV/RISCVMCTargetLowering.h"

#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/GlobalObject.h"
#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineConstantPool.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineJumpTableInfo.h"
#include "rcc/CodeGen/MachineMCContext.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/Register.h"
#include "rcc/CodeGen/TargetConstant.h"
#include "rcc/IR/Constants.h"
#include "rcc/IR/DataLayout.h"
#include "rcc/MC/MCContext.h"
#include "rcc/MC/MCExpr.h"
#include "rcc/MC/MCInst.h"
#include "rcc/MC/MCSection.h"
#include "rcc/MC/MCStreamer.h"
#include "rcc/MC/MCSymbol.h"
#include "rcc/Target/RISCV/RISCVTarget.h"
#include "rcc/Target/RISCV/RISCVTargetRelocationKinds.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rcc::riscv {
namespace {

constexpr std::uint32_t CallTargetFlag = 1;
constexpr std::uint32_t PCRelativeHighTargetFlag = 2;
constexpr std::uint32_t PCRelativeLowTargetFlag = 4;
constexpr std::uint32_t CopyF32TargetFlag = 8;
constexpr std::uint32_t CopyF64TargetFlag = 16;
constexpr std::uint32_t AbsoluteHighTargetFlag = 32;
constexpr std::uint32_t AbsoluteLowTargetFlag = 64;
constexpr std::uint32_t GOTPCRelativeHighTargetFlag = 128;
constexpr std::uint32_t KnownTargetFlags = CallTargetFlag | PCRelativeHighTargetFlag |
                                           PCRelativeLowTargetFlag | CopyF32TargetFlag |
                                           CopyF64TargetFlag | AbsoluteHighTargetFlag |
                                           AbsoluteLowTargetFlag | GOTPCRelativeHighTargetFlag;

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

[[nodiscard]] Expected<mc::MCExprRef> applyTargetFlags(mc::MCExprRef Expression,
                                                       std::uint32_t Flags) {
  if ((Flags & ~KnownTargetFlags) != 0 || (Flags & (CopyF32TargetFlag | CopyF64TargetFlag)) != 0) {
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "RISC-V operand has target flags that cannot survive MC lowering");
  }
  if (Flags == 0)
    return Expression;

  RISCVMCExprKind Kind;
  switch (Flags) {
  case CallTargetFlag:
    Kind = RISCVMCExprKind::CallPLT;
    break;
  case PCRelativeHighTargetFlag:
    Kind = RISCVMCExprKind::PCRelativeHigh;
    break;
  case PCRelativeLowTargetFlag:
    Kind = RISCVMCExprKind::PCRelativeLow;
    break;
  case AbsoluteHighTargetFlag:
    Kind = RISCVMCExprKind::AbsoluteHigh;
    break;
  case AbsoluteLowTargetFlag:
    Kind = RISCVMCExprKind::AbsoluteLow;
    break;
  case GOTPCRelativeHighTargetFlag:
    Kind = RISCVMCExprKind::GOTPCRelativeHigh;
    break;
  default:
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "RISC-V operand combines incompatible direct target flags");
  }
  auto TargetExpression =
      mc::MCExpr::target(static_cast<std::uint32_t>(Kind), std::move(Expression));
  if (!TargetExpression)
    return std::unexpected(TargetExpression.error());
  return *TargetExpression;
}

[[nodiscard]] Expected<mc::MCOperand> symbolOperand(mc::MCSymbolRef Symbol, std::int64_t Offset,
                                                    std::uint32_t TargetFlags) {
  auto Expression = mc::MCExpr::symbol(std::move(Symbol));
  if (!Expression)
    return std::unexpected(Expression.error());
  auto WithOffset = addOffset(std::move(*Expression), Offset);
  if (!WithOffset)
    return std::unexpected(WithOffset.error());
  auto TargetExpression = applyTargetFlags(std::move(*WithOffset), TargetFlags);
  if (!TargetExpression)
    return std::unexpected(TargetExpression.error());
  return mc::MCOperand::expression(std::move(*TargetExpression));
}

[[nodiscard]] Expected<std::int64_t> lowerIntegerImmediate(const APInt &Value) {
  if (Value.bitWidth() > 64 || Value.words().size() != 1) {
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "RISC-V instruction has an integer immediate wider than 64 bits");
  }
  const std::uint64_t Raw = Value.words().front();
  if (Value.bitWidth() == 64)
    return std::bit_cast<std::int64_t>(Raw);
  if (!Value.isNegative())
    return static_cast<std::int64_t>(Raw);
  const std::uint64_t Mask = (std::uint64_t{1} << Value.bitWidth()) - 1U;
  return std::bit_cast<std::int64_t>(Raw | ~Mask);
}

[[nodiscard]] Expected<mc::MCOperand> lowerOperand(const MachineOperand &Operand,
                                                   const MachineFunction &Function,
                                                   MachineMCContext &Context) {
  if (const auto *RegisterValue = Operand.getIf<Register>()) {
    const auto Physical = RegisterValue->physicalRegister();
    if (!Physical || Operand.subReg().value() != 0) {
      return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                            "RISC-V MC lowering requires a resolved physical register");
    }
    if (Operand.targetFlags() != 0) {
      return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                            "RISC-V register operand retains target flags at MC lowering");
    }
    return mc::MCOperand::registerOperand(mc::MCRegister::fromValue(Physical->number()));
  }
  if (const auto *Immediate = Operand.getIf<std::int64_t>()) {
    if (Operand.targetFlags() != 0) {
      return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                            "RISC-V immediate operand has symbol-only target flags");
    }
    return mc::MCOperand::immediate(*Immediate);
  }
  if (const auto *Immediate = Operand.getIf<IntegerImmediateOperand>()) {
    if (Operand.targetFlags() != 0) {
      return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                            "RISC-V integer immediate has symbol-only target flags");
    }
    auto Lowered = lowerIntegerImmediate(Immediate->Value);
    if (!Lowered)
      return std::unexpected(Lowered.error());
    return mc::MCOperand::immediate(*Lowered);
  }
  if (const auto *Immediate = Operand.getIf<FloatingImmediateOperand>()) {
    if (Operand.targetFlags() != 0 || Immediate->Bits.words().size() != 1) {
      return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                            "RISC-V floating immediate cannot be represented in MCInst");
    }
    if (Immediate->Bits.bitWidth() == 32)
      return mc::MCOperand::float32Bits(static_cast<std::uint32_t>(Immediate->Bits.words()[0]));
    if (Immediate->Bits.bitWidth() == 64)
      return mc::MCOperand::float64Bits(Immediate->Bits.words()[0]);
    return makeUnexpected(ErrorCode::Unsupported,
                          "RISC-V MC lowering supports only 32-bit and 64-bit FP immediates");
  }
  if (const auto *Block = Operand.getIf<BasicBlockOperand>()) {
    if (Block->Block == nullptr)
      return makeUnexpected(ErrorCode::AssemblyLoweringFailed, "RISC-V block operand is null");
    const mc::MCSymbolRef Symbol = Context.blockSymbol(*Block->Block);
    if (!Symbol)
      return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                            "RISC-V block operand has no MC symbol");
    return symbolOperand(Symbol, 0, Operand.targetFlags());
  }
  if (const auto *Pool = Operand.getIf<ConstantPoolOperand>()) {
    const mc::MCSymbolRef Symbol = Context.constantPoolSymbol(Function, Pool->Index);
    if (!Symbol)
      return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                            "RISC-V constant-pool operand has no MC symbol");
    return symbolOperand(Symbol, Pool->Offset, Operand.targetFlags());
  }
  if (const auto *Table = Operand.getIf<JumpTableOperand>()) {
    const mc::MCSymbolRef Symbol = Context.jumpTableSymbol(Function, Table->Index);
    if (!Symbol)
      return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                            "RISC-V jump-table operand has no MC symbol");
    return symbolOperand(Symbol, 0, Operand.targetFlags());
  }
  if (const auto *Global = Operand.getIf<GlobalSymbolOperand>()) {
    auto Symbol = Context.symbol(Global->Name, mc::MCSymbolKind::Object);
    if (!Symbol)
      return std::unexpected(Symbol.error());
    return symbolOperand(std::move(*Symbol), Global->Offset, Operand.targetFlags());
  }
  if (const auto *External = Operand.getIf<ExternalSymbolOperand>()) {
    auto Symbol = Context.symbol(External->Name);
    if (!Symbol)
      return std::unexpected(Symbol.error());
    return symbolOperand(std::move(*Symbol), External->Offset, Operand.targetFlags());
  }
  if (const auto *Address = Operand.getIf<BlockAddressOperand>()) {
    auto Symbol = Context.blockAddressSymbol(Address->FunctionName, Address->BlockName);
    if (!Symbol)
      return std::unexpected(Symbol.error());
    return symbolOperand(std::move(*Symbol), Address->Offset, Operand.targetFlags());
  }
  if (const auto *SymbolValue = Operand.getIf<MCSymbolOperand>()) {
    auto Symbol = Context.symbol(SymbolValue->Name);
    if (!Symbol)
      return std::unexpected(Symbol.error());
    return symbolOperand(std::move(*Symbol), SymbolValue->Offset, Operand.targetFlags());
  }
  if (Operand.kind() == MachineOperand::Kind::FrameIndex) {
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "RISC-V MC lowering encountered an unresolved frame index");
  }
  return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                        "RISC-V MC lowering encountered a non-encoding operand");
}

#define GET_MC_PSEUDO_LOWERING
#include "RISCVGenMCPseudoLowering.inc"
#undef GET_MC_PSEUDO_LOWERING

[[nodiscard]] const AssemblyFormatInfo *assemblyFormat(AssemblyFormatID ID) noexcept {
  const auto Formats = assemblyFormats();
  const auto Position = std::ranges::find(Formats, ID, &AssemblyFormatInfo::ID);
  return Position == Formats.end() ? nullptr : &*Position;
}

[[nodiscard]] std::optional<std::size_t> formatOperandCount(std::string_view Name) noexcept {
  if (Name == "none")
    return 0;
  if (Name == "rs1" || Name == "target" || Name == "imm-sp")
    return 1;
  if (Name == "rd-imm" || Name == "rd-target" || Name == "rd-frs1" || Name == "frd-rs1" ||
      Name == "rs1-target" || Name == "rd-rs2" || Name == "rd-paren-rs1" || Name == "rd-imm-sp" ||
      Name == "rd-offset-sp" || Name == "rs2-offset-sp" || Name == "frd-offset-sp" ||
      Name == "frs2-offset-sp") {
    return 2;
  }
  if (Name == "rd-rs1-imm" || Name == "rd-rs1-rs2" || Name == "rd-offset-rs1" ||
      Name == "rs2-offset-rs1" || Name == "rs1-rs2-target" || Name == "imm-imm" ||
      Name == "rd-csr-rs1" || Name == "rd-csr-imm" || Name == "rd-rs2-paren-rs1" ||
      Name == "frd-offset-rs1" || Name == "frs2-offset-rs1" || Name == "frd-frs1-rm" ||
      Name == "frd-frs1-frs2" || Name == "rd-frs1-rm" || Name == "rd-frs1-frs2" ||
      Name == "frd-rs1-rm") {
    return 3;
  }
  if (Name == "frd-frs1-frs2-rm")
    return 4;
  if (Name == "frd-frs1-frs2-frs3-rm")
    return 5;
  return std::nullopt;
}

[[nodiscard]] std::vector<std::uint8_t> bytesFromWords(std::span<const std::uint64_t> Words,
                                                       std::uint64_t Size) {
  std::vector<std::uint8_t> Bytes(static_cast<std::size_t>(Size), 0);
  for (std::uint64_t ByteIndex = 0; ByteIndex < Size; ++ByteIndex) {
    const std::size_t WordIndex = static_cast<std::size_t>(ByteIndex / 8U);
    if (WordIndex >= Words.size())
      break;
    const unsigned Shift = static_cast<unsigned>((ByteIndex % 8U) * 8U);
    Bytes[static_cast<std::size_t>(ByteIndex)] =
        static_cast<std::uint8_t>((Words[WordIndex] >> Shift) & 0xffU);
  }
  return Bytes;
}

[[nodiscard]] Expected<mc::MCExprRef> lowerConstantExpression(const GenericConstantData &Constant,
                                                              MachineMCContext &Context,
                                                              std::size_t Depth = 0) {
  if (Depth > 1024) {
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "constant expression exceeds its nesting limit");
  }
  switch (Constant.Kind) {
  case GenericConstantKind::Integer: {
    if (!Constant.IntegerValue || Constant.IntegerValue->bitWidth() > 64 ||
        Constant.IntegerValue->words().size() != 1) {
      return makeUnexpected(ErrorCode::Unsupported,
                            "RV64GC MC expression requires an integer no wider than 64 bits");
    }
    const std::uint64_t Raw = Constant.IntegerValue->words().front();
    const std::int64_t Value = Constant.IntegerValue->bitWidth() == 64
                                   ? std::bit_cast<std::int64_t>(Raw)
                                   : static_cast<std::int64_t>(Raw);
    return mc::MCExpr::constant(Value);
  }
  case GenericConstantKind::NullPointer:
  case GenericConstantKind::ZeroInitializer:
  case GenericConstantKind::Undef:
  case GenericConstantKind::Poison:
    return mc::MCExpr::constant(0);
  case GenericConstantKind::GlobalAddress:
  case GenericConstantKind::DSOLocalEquivalent:
  case GenericConstantKind::NoCFI: {
    auto Symbol = Context.symbol(Constant.Symbol, mc::MCSymbolKind::Object);
    if (!Symbol)
      return std::unexpected(Symbol.error());
    return mc::MCExpr::symbol(std::move(*Symbol));
  }
  case GenericConstantKind::BlockAddress: {
    auto Symbol = Context.blockAddressSymbol(Constant.Symbol, Constant.Block);
    if (!Symbol)
      return std::unexpected(Symbol.error());
    return mc::MCExpr::symbol(std::move(*Symbol));
  }
  case GenericConstantKind::Expression: {
    if (Constant.Elements.empty()) {
      return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                            "generic constant expression has no operands");
    }
    auto Left = lowerConstantExpression(Constant.Elements.front(), Context, Depth + 1);
    if (!Left)
      return std::unexpected(Left.error());
    switch (Constant.ExpressionOpcode) {
    case GenericConstantExpressionOpcode::Trunc:
    case GenericConstantExpressionOpcode::BitCast:
    case GenericConstantExpressionOpcode::AddressSpaceCast:
    case GenericConstantExpressionOpcode::IntToPtr:
    case GenericConstantExpressionOpcode::PtrToAddr:
    case GenericConstantExpressionOpcode::PtrToInt:
      return *Left;
    case GenericConstantExpressionOpcode::Add:
    case GenericConstantExpressionOpcode::Sub:
    case GenericConstantExpressionOpcode::Xor: {
      if (Constant.Elements.size() != 2) {
        return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                              "binary constant expression has the wrong operand count");
      }
      auto Right = lowerConstantExpression(Constant.Elements[1], Context, Depth + 1);
      if (!Right)
        return std::unexpected(Right.error());
      const mc::MCBinaryOperator Operator =
          Constant.ExpressionOpcode == GenericConstantExpressionOpcode::Add
              ? mc::MCBinaryOperator::Add
          : Constant.ExpressionOpcode == GenericConstantExpressionOpcode::Sub
              ? mc::MCBinaryOperator::Subtract
              : mc::MCBinaryOperator::BitwiseXor;
      return mc::MCExpr::binary(Operator, std::move(*Left), std::move(*Right));
    }
    case GenericConstantExpressionOpcode::Invalid:
    case GenericConstantExpressionOpcode::GetElementPtr:
    case GenericConstantExpressionOpcode::ShuffleVector:
    case GenericConstantExpressionOpcode::InsertElement:
    case GenericConstantExpressionOpcode::ExtractElement:
    case GenericConstantExpressionOpcode::Splat:
      return makeUnexpected(ErrorCode::Unsupported,
                            "RV64GC assembly cannot lower this generic constant expression");
    }
  }
  case GenericConstantKind::Invalid:
  case GenericConstantKind::Floating:
  case GenericConstantKind::Aggregate:
  case GenericConstantKind::ByteString:
  case GenericConstantKind::PointerAuth:
    return makeUnexpected(ErrorCode::Unsupported,
                          "generic constant cannot be represented as an MC expression");
  }
  return makeUnexpected(ErrorCode::InternalError, "generic constant kind is invalid");
}

[[nodiscard]] Expected<void> emitPadding(std::uint64_t Size, mc::MCStreamer &Streamer) {
  return Size == 0 ? Expected<void>{} : Streamer.emitZeroFill(Size);
}

[[nodiscard]] Expected<void>
emitGenericConstantImpl(const GenericConstantData &Constant, const DataLayout &Layout,
                        std::uint64_t AllocationSize, MachineMCContext &Context,
                        mc::MCStreamer &Streamer, std::size_t Depth = 0) {
  if (Depth > 1024) {
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "generic constant exceeds its emission nesting limit");
  }
  switch (Constant.Kind) {
  case GenericConstantKind::Integer: {
    if (!Constant.IntegerValue)
      return makeUnexpected(ErrorCode::InternalError, "integer constant has no value");
    const auto Bytes = bytesFromWords(Constant.IntegerValue->words(), AllocationSize);
    return Streamer.emitBytes(Bytes);
  }
  case GenericConstantKind::Floating: {
    if (!Constant.FloatingValue)
      return makeUnexpected(ErrorCode::InternalError, "floating constant has no value");
    const auto Bytes = bytesFromWords(Constant.FloatingValue->words(), AllocationSize);
    return Streamer.emitBytes(Bytes);
  }
  case GenericConstantKind::ByteString: {
    if (Constant.ByteValue.size() != AllocationSize) {
      return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                            "byte-string constant size does not match its RV64GC layout");
    }
    std::vector<std::uint8_t> Bytes;
    Bytes.reserve(Constant.ByteValue.size());
    for (const std::byte Byte : Constant.ByteValue)
      Bytes.push_back(std::to_integer<std::uint8_t>(Byte));
    return Streamer.emitBytes(Bytes);
  }
  case GenericConstantKind::NullPointer:
  case GenericConstantKind::ZeroInitializer:
  case GenericConstantKind::Undef:
  case GenericConstantKind::Poison:
    return Streamer.emitZeroFill(AllocationSize);
  case GenericConstantKind::Aggregate: {
    const GenericConstantType &Type = Constant.Type.resolved();
    std::uint64_t Offset = 0;
    for (std::size_t Index = 0; Index < Constant.Elements.size(); ++Index) {
      auto ElementSize = Layout.getTypeAllocSize(Constant.Elements[Index].Type);
      auto ElementAlignment = Layout.getABITypeAlign(Constant.Elements[Index].Type);
      if (!ElementSize)
        return std::unexpected(ElementSize.error());
      if (!ElementAlignment)
        return std::unexpected(ElementAlignment.error());
      const std::uint64_t Alignment =
          Type.Kind == GenericConstantTypeKind::Struct && Type.Packed ? 1 : *ElementAlignment;
      const std::uint64_t AlignmentMask = Alignment - 1U;
      if (Offset > std::numeric_limits<std::uint64_t>::max() - AlignmentMask) {
        return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                              "aggregate constant exceeds its RV64GC layout");
      }
      const std::uint64_t ElementOffset = (Offset + AlignmentMask) & ~AlignmentMask;
      if (ElementOffset > AllocationSize || *ElementSize > AllocationSize - ElementOffset) {
        return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                              "aggregate constant exceeds its RV64GC layout");
      }
      auto Padding = emitPadding(ElementOffset - Offset, Streamer);
      if (!Padding)
        return std::unexpected(Padding.error());
      auto Element = emitGenericConstantImpl(Constant.Elements[Index], Layout, *ElementSize,
                                             Context, Streamer, Depth + 1);
      if (!Element)
        return std::unexpected(Element.error());
      Offset = ElementOffset + *ElementSize;
    }
    if (Offset > AllocationSize) {
      return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                            "aggregate constant exceeds its RV64GC layout");
    }
    return emitPadding(AllocationSize - Offset, Streamer);
  }
  case GenericConstantKind::GlobalAddress:
  case GenericConstantKind::BlockAddress:
  case GenericConstantKind::DSOLocalEquivalent:
  case GenericConstantKind::NoCFI:
  case GenericConstantKind::Expression: {
    if (AllocationSize != 1 && AllocationSize != 2 && AllocationSize != 4 && AllocationSize != 8) {
      return makeUnexpected(ErrorCode::Unsupported,
                            "symbolic RV64GC constant has an unsupported storage size");
    }
    auto Expression = lowerConstantExpression(Constant, Context);
    if (!Expression)
      return std::unexpected(Expression.error());
    return Streamer.emitValue(std::move(*Expression), static_cast<std::uint8_t>(AllocationSize));
  }
  case GenericConstantKind::PointerAuth:
    return makeUnexpected(ErrorCode::Unsupported,
                          "RV64GC assembly does not support pointer-auth constants");
  case GenericConstantKind::Invalid:
    return makeUnexpected(ErrorCode::InternalError, "generic constant kind is invalid");
  }
  return makeUnexpected(ErrorCode::InternalError, "generic constant kind is invalid");
}

[[nodiscard]] Expected<void> emitRepeatedByte(std::uint8_t Byte, std::uint64_t Size,
                                              mc::MCStreamer &Streamer) {
  if (Size == 0)
    return {};
  if (Byte == 0)
    return Streamer.emitZeroFill(Size);
  constexpr std::uint64_t ChunkSize = 4096;
  const std::size_t InitialSize = static_cast<std::size_t>(std::min(Size, ChunkSize));
  const std::vector<std::uint8_t> Chunk(InitialSize, Byte);
  while (Size != 0) {
    const std::size_t EmittedSize = static_cast<std::size_t>(std::min(Size, ChunkSize));
    auto Emitted = Streamer.emitBytes(std::span<const std::uint8_t>(Chunk).first(EmittedSize));
    if (!Emitted)
      return std::unexpected(Emitted.error());
    Size -= static_cast<std::uint64_t>(EmittedSize);
  }
  return {};
}

[[nodiscard]] Expected<void> emitTargetBytes(std::span<const std::byte> Bytes,
                                             mc::MCStreamer &Streamer) {
  if (Bytes.empty())
    return {};
  std::vector<std::uint8_t> Storage;
  Storage.reserve(Bytes.size());
  for (const std::byte Byte : Bytes)
    Storage.push_back(std::to_integer<std::uint8_t>(Byte));
  return Streamer.emitBytes(Storage);
}

[[nodiscard]] Expected<void> emitTargetConstantImpl(const TargetConstant &Constant,
                                                    MachineMCContext &Context,
                                                    mc::MCStreamer &Streamer) {
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
    return emitTargetBytes(Emission->Bytes, Streamer);

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
    auto RawBytes = emitTargetBytes(std::span<const std::byte>(Emission->Bytes)
                                        .subspan(static_cast<std::size_t>(Cursor),
                                                 static_cast<std::size_t>(Fixup->Offset - Cursor)),
                                    Streamer);
    if (!RawBytes)
      return std::unexpected(RawBytes.error());

    auto Symbol = Context.symbol(Fixup->Symbol);
    if (!Symbol)
      return std::unexpected(Symbol.error());
    auto Expression = mc::MCExpr::symbol(std::move(*Symbol));
    if (!Expression)
      return std::unexpected(Expression.error());
    auto WithAddend = addOffset(std::move(*Expression), Fixup->Addend);
    if (!WithAddend)
      return std::unexpected(WithAddend.error());
    auto Value = Streamer.emitValue(std::move(*WithAddend), *Size);
    if (!Value)
      return std::unexpected(Value.error());
    Cursor = Fixup->Offset + *Size;
  }
  return emitTargetBytes(
      std::span<const std::byte>(Emission->Bytes).subspan(static_cast<std::size_t>(Cursor)),
      Streamer);
}

[[nodiscard]] Expected<void> emitGlobalInitializerImpl(const GlobalInitializer &Initializer,
                                                       MachineMCContext &Context,
                                                       mc::MCStreamer &Streamer,
                                                       std::size_t Depth = 0) {
  if (Depth > 1024) {
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "global initializer exceeds its emission nesting limit");
  }
  switch (Initializer.kind()) {
  case GlobalInitializer::Kind::Integer: {
    const auto *Value = Initializer.getIf<APInt>();
    if (Value == nullptr)
      return makeUnexpected(ErrorCode::InternalError, "integer initializer has no value");
    const std::uint64_t Size = (static_cast<std::uint64_t>(Value->bitWidth()) + 7U) / 8U;
    return Streamer.emitBytes(bytesFromWords(Value->words(), Size));
  }
  case GlobalInitializer::Kind::Floating: {
    const auto *Value = Initializer.getIf<APFloat>();
    if (Value == nullptr)
      return makeUnexpected(ErrorCode::InternalError, "floating initializer has no value");
    const std::uint64_t Size = (static_cast<std::uint64_t>(Value->bitWidth()) + 7U) / 8U;
    return Streamer.emitBytes(bytesFromWords(Value->words(), Size));
  }
  case GlobalInitializer::Kind::Bytes: {
    const auto *Value = Initializer.getIf<std::vector<std::byte>>();
    if (Value == nullptr)
      return makeUnexpected(ErrorCode::InternalError, "byte initializer has no value");
    std::vector<std::uint8_t> Bytes;
    Bytes.reserve(Value->size());
    for (const std::byte Byte : *Value)
      Bytes.push_back(std::to_integer<std::uint8_t>(Byte));
    return Streamer.emitBytes(Bytes);
  }
  case GlobalInitializer::Kind::ZeroFill: {
    const auto *Value = Initializer.getIf<GlobalZeroFill>();
    return Value == nullptr
               ? makeUnexpected(ErrorCode::InternalError, "zero-fill initializer has no value")
               : Streamer.emitZeroFill(Value->Size);
  }
  case GlobalInitializer::Kind::Repeated: {
    const auto *Value = Initializer.getIf<GlobalRepeatedInitializer>();
    if (Value == nullptr || !Value->Value)
      return makeUnexpected(ErrorCode::InternalError, "repeated initializer has no value");
    for (std::uint64_t Index = 0; Index < Value->Count; ++Index) {
      auto Emitted = emitGlobalInitializerImpl(*Value->Value, Context, Streamer, Depth + 1);
      if (!Emitted)
        return std::unexpected(Emitted.error());
    }
    return {};
  }
  case GlobalInitializer::Kind::Aggregate: {
    const auto *Value = Initializer.getIf<GlobalAggregateInitializer>();
    if (Value == nullptr)
      return makeUnexpected(ErrorCode::InternalError, "aggregate initializer has no value");
    for (const GlobalInitializerRef &Element : Value->Elements) {
      if (!Element)
        return makeUnexpected(ErrorCode::InternalError, "aggregate initializer is null");
      auto Emitted = emitGlobalInitializerImpl(*Element, Context, Streamer, Depth + 1);
      if (!Emitted)
        return std::unexpected(Emitted.error());
    }
    return {};
  }
  case GlobalInitializer::Kind::SymbolAddress: {
    const auto *Value = Initializer.getIf<GlobalSymbolAddress>();
    if (Value == nullptr)
      return makeUnexpected(ErrorCode::InternalError, "symbol initializer has no value");
    auto Symbol = Context.symbol(Value->Symbol, mc::MCSymbolKind::Object);
    if (!Symbol)
      return std::unexpected(Symbol.error());
    auto Expression = mc::MCExpr::symbol(std::move(*Symbol));
    if (!Expression)
      return std::unexpected(Expression.error());
    auto WithAddend = addOffset(std::move(*Expression), Value->Addend);
    if (!WithAddend)
      return std::unexpected(WithAddend.error());
    auto Lowered = applyTargetFlags(std::move(*WithAddend), Value->TargetFlags);
    if (!Lowered)
      return std::unexpected(Lowered.error());
    return Streamer.emitValue(std::move(*Lowered), Value->Size);
  }
  case GlobalInitializer::Kind::SymbolDifference: {
    const auto *Value = Initializer.getIf<GlobalSymbolDifference>();
    if (Value == nullptr)
      return makeUnexpected(ErrorCode::InternalError, "symbol difference has no value");
    auto MinuendSymbol = Context.symbol(Value->Minuend);
    if (!MinuendSymbol)
      return std::unexpected(MinuendSymbol.error());
    auto SubtrahendSymbol = Context.symbol(Value->Subtrahend);
    if (!SubtrahendSymbol)
      return std::unexpected(SubtrahendSymbol.error());
    auto Minuend = mc::MCExpr::symbol(std::move(*MinuendSymbol));
    if (!Minuend)
      return std::unexpected(Minuend.error());
    auto Subtrahend = mc::MCExpr::symbol(std::move(*SubtrahendSymbol));
    if (!Subtrahend)
      return std::unexpected(Subtrahend.error());
    auto Difference = mc::MCExpr::binary(mc::MCBinaryOperator::Subtract, std::move(*Minuend),
                                         std::move(*Subtrahend));
    if (!Difference)
      return std::unexpected(Difference.error());
    auto WithAddend = addOffset(std::move(*Difference), Value->Addend);
    if (!WithAddend)
      return std::unexpected(WithAddend.error());
    return Streamer.emitValue(std::move(*WithAddend), Value->Size);
  }
  case GlobalInitializer::Kind::Target: {
    const auto *Value = Initializer.getIf<TargetConstant>();
    if (Value == nullptr)
      return makeUnexpected(ErrorCode::InternalError, "target initializer has no value");
    return emitTargetConstantImpl(*Value, Context, Streamer);
  }
  case GlobalInitializer::Kind::AlignmentPadding: {
    const auto *Value = Initializer.getIf<GlobalAlignmentPadding>();
    return Value == nullptr
               ? makeUnexpected(ErrorCode::InternalError, "padding initializer has no value")
               : emitRepeatedByte(Value->Fill, Value->Size, Streamer);
  }
  case GlobalInitializer::Kind::UndefinedStorage: {
    const auto *Value = Initializer.getIf<GlobalUndefinedStorage>();
    return Value == nullptr
               ? makeUnexpected(ErrorCode::InternalError, "undefined initializer has no value")
               : Streamer.emitZeroFill(Value->Size);
  }
  case GlobalInitializer::Kind::Invalid:
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed, "global initializer is invalid");
  }
  return makeUnexpected(ErrorCode::InternalError, "global initializer kind is invalid");
}

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

Expected<mc::MCInst> RISCVMCTargetLowering::lowerInstruction(const MachineInstr &Instruction,
                                                             MachineMCContext &Context) const {
  const InstructionDescriptor *Descriptor = Target->instructionDescriptor(Instruction.opcode());
  const AssemblyInstructionInfo *Assembly = assemblyInstruction(Instruction.opcode());
  if (Descriptor == nullptr || Assembly == nullptr || Assembly->Mnemonic.empty()) {
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "RISC-V instruction has no MC lowering description");
  }
  const AssemblyFormatInfo *Format = assemblyFormat(Assembly->Format);
  if (Format == nullptr)
    return makeUnexpected(ErrorCode::InternalError, "RISC-V assembly format is missing");
  const auto RequiredOperands = formatOperandCount(Format->Name);
  if (!RequiredOperands) {
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "RISC-V instruction uses an unsupported assembly format");
  }

  mc::MCInst Result(mc::MCOpcode::fromValue(Instruction.opcode().value()));
  Result.setFlags(Instruction.flags().rawValue());
  Result.setSourceIdentity(Instruction.stableID().value());
  Result.reserveOperands(*RequiredOperands);
  for (const MachineOperand &Operand : Instruction.operands()) {
    if (Result.operands().size() == *RequiredOperands)
      break;
    if (Operand.isRegister() && Operand.isImplicit())
      continue;
    if (Operand.kind() == MachineOperand::Kind::RegisterMask ||
        Operand.kind() == MachineOperand::Kind::LiveOutRegisterMask) {
      continue;
    }
    auto Lowered = lowerOperand(Operand, Instruction.function(), Context);
    if (!Lowered)
      return std::unexpected(Lowered.error());
    Result.addOperand(std::move(*Lowered));
  }
  if (Result.operands().size() != *RequiredOperands) {
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "RISC-V instruction has too few encoding operands");
  }
  return Result;
}

Expected<void> RISCVMCTargetLowering::emitTerminalInstruction(const MachineInstr &Instruction,
                                                              MachineMCContext &Context,
                                                              mc::MCStreamer &Streamer) const {
  auto Pseudo = lowerGeneratedMCPseudo(Instruction, Context);
  if (!Pseudo)
    return std::unexpected(Pseudo.error());
  if (*Pseudo)
    return Streamer.emitInstruction(**Pseudo);
  if (Instruction.opcode() != opcode(GenericOpcode::INLINEASM) &&
      Instruction.opcode() != opcode(GenericOpcode::INLINEASM_BR)) {
    return TargetMCInstLowering::emitTerminalInstruction(Instruction, Context, Streamer);
  }
  for (const MachineOperand &Operand : Instruction.operands()) {
    if (Operand.kind() == MachineOperand::Kind::RegisterMask ||
        Operand.kind() == MachineOperand::Kind::LiveOutRegisterMask ||
        (Operand.isRegister() && Operand.isImplicit())) {
      continue;
    }
    const auto *Text = Operand.getIf<MCSymbolOperand>();
    if (Text == nullptr || Text->Offset != 0 || Operand.targetFlags() != 0) {
      return makeUnexpected(ErrorCode::Unsupported,
                            "RISC-V inline assembly requires a literal MC-symbol text operand");
    }
    return Streamer.emitInlineAssembly(Text->Name);
  }
  return {};
}

Expected<mc::MCSectionDesc> RISCVMCTargetLowering::textSection(const MachineFunction &) const {
  return textSectionDescription();
}

Expected<mc::MCSectionDesc> RISCVMCTargetLowering::globalSection(const GlobalObjectDesc &Object,
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
RISCVMCTargetLowering::constantPoolSection(const MachineFunction &Function,
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
RISCVMCTargetLowering::jumpTableSection(const MachineFunction &Function,
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

Expected<void> RISCVMCTargetLowering::emitFileHeader(MachineMCContext &,
                                                     mc::MCStreamer &Streamer) const {
  auto Attribute = Streamer.emitDirective("attribute", "arch, \"rv64gc\"");
  if (!Attribute)
    return std::unexpected(Attribute.error());
  return Streamer.emitDirective("attribute", "stack_align, 16");
}

Expected<void> RISCVMCTargetLowering::emitFileTrailer(MachineMCContext &Context,
                                                      mc::MCStreamer &Streamer) const {
  mc::MCSectionDesc Note{
      .Name = ".note.GNU-stack",
      .Kind = mc::MCSectionKind::Custom,
      .Flags = {},
      .Alignment = 1,
      .EntrySize = 0,
      .Group = {},
      .UniqueID = std::nullopt,
  };
  auto Section = Context.context().getOrCreateSection(std::move(Note));
  if (!Section)
    return std::unexpected(Section.error());
  return Streamer.switchSection(std::move(*Section));
}

Expected<void> RISCVMCTargetLowering::emitGenericConstant(const GenericConstantData &Constant,
                                                          std::uint64_t Size,
                                                          MachineMCContext &Context,
                                                          mc::MCStreamer &Streamer) const {
  auto Verification = Constant.verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  auto Layout = DataLayout::parse(TargetDataLayout);
  if (!Layout)
    return std::unexpected(Layout.error());
  auto AllocationSize = Layout->getTypeAllocSize(Constant.Type);
  if (!AllocationSize)
    return std::unexpected(AllocationSize.error());
  if (*AllocationSize != Size) {
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "generic constant size does not match the RV64GC data layout");
  }
  return emitGenericConstantImpl(Constant, *Layout, *AllocationSize, Context, Streamer);
}

Expected<void> RISCVMCTargetLowering::emitGlobalInitializer(const GlobalInitializer &Initializer,
                                                            MachineMCContext &Context,
                                                            mc::MCStreamer &Streamer) const {
  auto Verification = Initializer.verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  return emitGlobalInitializerImpl(Initializer, Context, Streamer);
}

Expected<void> RISCVMCTargetLowering::emitTargetConstant(const TargetConstant &Constant,
                                                         MachineMCContext &Context,
                                                         mc::MCStreamer &Streamer) const {
  return emitTargetConstantImpl(Constant, Context, Streamer);
}

Expected<void> RISCVMCTargetLowering::emitTargetJumpTable(const MachineFunction &Function,
                                                          const MachineJumpTableEntry &Entry,
                                                          MachineMCContext &Context,
                                                          mc::MCStreamer &Streamer) const {
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
    auto GlobalPointer = Context.symbol("__global_pointer$");
    if (!GlobalPointer)
      return std::unexpected(GlobalPointer.error());
    BaseSymbol = std::move(*GlobalPointer);
  } else if (Description->Encoding == JumpTableEntryEncoding::Custom32) {
    BaseSymbol = Context.jumpTableSymbol(Function, Entry.index());
  }

  for (const MachineBasicBlock *TargetBlock : Description->Targets) {
    if (TargetBlock == nullptr)
      return makeUnexpected(ErrorCode::AssemblyLoweringFailed, "jump table has a null target");
    const mc::MCSymbolRef TargetSymbol = Context.blockSymbol(*TargetBlock);
    if (!TargetSymbol)
      return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                            "jump-table target has no MC symbol");
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
    auto Value = Streamer.emitValue(std::move(*Expression), Description->EntrySize);
    if (!Value)
      return std::unexpected(Value.error());
  }
  return {};
}

} // namespace rcc::riscv
