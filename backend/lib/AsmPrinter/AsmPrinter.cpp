// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/AsmPrinter.h"

#include "rcc/CodeGen/CompilationUnit.h"
#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/GlobalObject.h"
#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineCFIInfo.h"
#include "rcc/CodeGen/MachineConstantPool.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineJumpTableInfo.h"
#include "rcc/CodeGen/MachineMCContext.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineVerifier.h"
#include "rcc/CodeGen/TargetConstant.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/CodeGen/TargetLoweringObjectFile.h"
#include "rcc/CodeGen/TargetMCInstLowering.h"
#include "rcc/IR/Constants.h"
#include "rcc/MC/MCCFI.h"
#include "rcc/MC/MCContext.h"
#include "rcc/MC/MCExpr.h"
#include "rcc/MC/MCSection.h"
#include "rcc/MC/MCStreamer.h"
#include "rcc/MC/MCSymbol.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace rcc {
namespace {

[[nodiscard]] Expected<mc::MCExprRef> symbolExpression(mc::MCSymbolRef Symbol) {
  auto Expression = mc::MCExpr::symbol(std::move(Symbol));
  if (!Expression)
    return std::unexpected(Expression.error());
  return *Expression;
}

[[nodiscard]] Expected<mc::MCExprRef> addConstant(mc::MCExprRef Expression, std::int64_t Addend) {
  if (Addend == 0)
    return Expression;
  auto Result = mc::MCExpr::binary(mc::MCBinaryOperator::Add, std::move(Expression),
                                   mc::MCExpr::constant(Addend));
  if (!Result)
    return std::unexpected(Result.error());
  return *Result;
}

[[nodiscard]] Expected<mc::MCExprRef>
symbolDifference(mc::MCSymbolRef Minuend, mc::MCSymbolRef Subtrahend, std::int64_t Addend) {
  auto Left = symbolExpression(std::move(Minuend));
  if (!Left)
    return std::unexpected(Left.error());
  auto Right = symbolExpression(std::move(Subtrahend));
  if (!Right)
    return std::unexpected(Right.error());
  auto Difference =
      mc::MCExpr::binary(mc::MCBinaryOperator::Subtract, std::move(*Left), std::move(*Right));
  if (!Difference)
    return std::unexpected(Difference.error());
  return addConstant(std::move(*Difference), Addend);
}

[[nodiscard]] Expected<mc::MCCFIInstruction>
lowerCFIInstruction(const MachineCFIInstruction &Instruction, MachineMCContext &Context) {
  mc::MCCFIInstruction Result;
  const auto Register = [&Result](const MachineCFIRegisterRule &Rule) {
    Result.Operands.push_back(static_cast<std::int64_t>(Rule.Register.value()));
  };
  const auto Offset = [&Result](const MachineCFIOffsetRule &Rule) {
    Result.Operands.push_back(Rule.Offset);
  };
  const auto RegisterOffset = [&Result](const MachineCFIRegisterOffsetRule &Rule) {
    Result.Operands.push_back(static_cast<std::int64_t>(Rule.Register.value()));
    Result.Operands.push_back(Rule.Offset);
  };

  switch (Instruction.operation()) {
  case MachineCFIOperation::SameValue:
    Result.Operation = mc::MCCFIOperation::SameValue;
    Register(*Instruction.getIf<MachineCFIRegisterRule>());
    break;
  case MachineCFIOperation::RememberState:
    Result.Operation = mc::MCCFIOperation::RememberState;
    break;
  case MachineCFIOperation::RestoreState:
    Result.Operation = mc::MCCFIOperation::RestoreState;
    break;
  case MachineCFIOperation::Offset:
    Result.Operation = mc::MCCFIOperation::Offset;
    RegisterOffset(*Instruction.getIf<MachineCFIRegisterOffsetRule>());
    break;
  case MachineCFIOperation::DefCFARegister:
    Result.Operation = mc::MCCFIOperation::DefCFARegister;
    Register(*Instruction.getIf<MachineCFIRegisterRule>());
    break;
  case MachineCFIOperation::DefCFAOffset:
    Result.Operation = mc::MCCFIOperation::DefCFAOffset;
    Offset(*Instruction.getIf<MachineCFIOffsetRule>());
    break;
  case MachineCFIOperation::DefCFA:
    Result.Operation = mc::MCCFIOperation::DefCFA;
    RegisterOffset(*Instruction.getIf<MachineCFIRegisterOffsetRule>());
    break;
  case MachineCFIOperation::RelativeOffset:
    Result.Operation = mc::MCCFIOperation::RelativeOffset;
    RegisterOffset(*Instruction.getIf<MachineCFIRegisterOffsetRule>());
    break;
  case MachineCFIOperation::AdjustCFAOffset:
    Result.Operation = mc::MCCFIOperation::AdjustCFAOffset;
    Offset(*Instruction.getIf<MachineCFIOffsetRule>());
    break;
  case MachineCFIOperation::Escape:
    Result.Operation = mc::MCCFIOperation::Escape;
    Result.Bytes = Instruction.getIf<MachineCFIEscapeRule>()->Bytes;
    break;
  case MachineCFIOperation::Restore:
    Result.Operation = mc::MCCFIOperation::Restore;
    Register(*Instruction.getIf<MachineCFIRegisterRule>());
    break;
  case MachineCFIOperation::Undefined:
    Result.Operation = mc::MCCFIOperation::Undefined;
    Register(*Instruction.getIf<MachineCFIRegisterRule>());
    break;
  case MachineCFIOperation::Register: {
    Result.Operation = mc::MCCFIOperation::Register;
    const auto &Rule = *Instruction.getIf<MachineCFIRegisterRegisterRule>();
    Result.Operands.push_back(static_cast<std::int64_t>(Rule.Register.value()));
    Result.Operands.push_back(static_cast<std::int64_t>(Rule.SourceRegister.value()));
    break;
  }
  case MachineCFIOperation::WindowSave:
    Result.Operation = mc::MCCFIOperation::WindowSave;
    break;
  case MachineCFIOperation::NegateReturnAddressState:
    Result.Operation = mc::MCCFIOperation::NegateReturnAddressState;
    break;
  case MachineCFIOperation::NegateReturnAddressStateWithPC:
    Result.Operation = mc::MCCFIOperation::NegateReturnAddressStateWithPC;
    break;
  case MachineCFIOperation::GNUArgsSize:
    Result.Operation = mc::MCCFIOperation::GNUArgsSize;
    Offset(*Instruction.getIf<MachineCFIOffsetRule>());
    break;
  case MachineCFIOperation::Label: {
    Result.Operation = mc::MCCFIOperation::Label;
    auto Symbol = Context.symbol(Instruction.getIf<MachineCFILabelRule>()->Symbol,
                                 mc::MCSymbolKind::Temporary);
    if (!Symbol)
      return std::unexpected(Symbol.error());
    Result.Symbol = std::move(*Symbol);
    break;
  }
  case MachineCFIOperation::ValueOffset:
    Result.Operation = mc::MCCFIOperation::ValueOffset;
    RegisterOffset(*Instruction.getIf<MachineCFIRegisterOffsetRule>());
    break;
  case MachineCFIOperation::LLVMDefAddressSpaceCFA:
  case MachineCFIOperation::LLVMRegisterPair:
  case MachineCFIOperation::LLVMVectorRegisters:
  case MachineCFIOperation::LLVMVectorOffset:
  case MachineCFIOperation::LLVMVectorRegisterMask:
    return makeUnexpected(ErrorCode::Unsupported,
                          "assembly emission does not support this LLVM-specific CFI operation");
  case MachineCFIOperation::Count:
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "machine CFI instruction has an invalid operation");
  }

  auto Verification = Result.verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  return Result;
}

[[nodiscard]] Expected<void> emitNamedLabel(const MachineOperand &Operand,
                                            MachineMCContext &Context, mc::MCStreamer &Streamer) {
  const auto *SymbolOperand = Operand.getIf<MCSymbolOperand>();
  if (SymbolOperand == nullptr || SymbolOperand->Offset != 0 || Operand.targetFlags() != 0) {
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "label pseudo requires an unmodified MC symbol operand");
  }
  auto Symbol = Context.symbol(SymbolOperand->Name, mc::MCSymbolKind::Temporary);
  if (!Symbol)
    return std::unexpected(Symbol.error());
  return Streamer.emitLabel(std::move(*Symbol));
}

[[nodiscard]] bool validValueSize(std::uint64_t Size) noexcept {
  return Size == 1 || Size == 2 || Size == 4 || Size == 8;
}

[[nodiscard]] Expected<void> emitGlobalAttributes(const GlobalObjectDesc &Object,
                                                  const mc::MCSymbolRef &Symbol,
                                                  mc::MCStreamer &Streamer) {
  switch (Object.Linkage) {
  case GlobalLinkage::External:
  case GlobalLinkage::Appending:
  case GlobalLinkage::Common: {
    auto Emitted = Streamer.emitSymbolAttribute(Symbol, mc::MCSymbolAttribute::Global);
    if (!Emitted)
      return std::unexpected(Emitted.error());
    break;
  }
  case GlobalLinkage::LinkOnceAny:
  case GlobalLinkage::LinkOnceODR:
  case GlobalLinkage::WeakAny:
  case GlobalLinkage::WeakODR:
  case GlobalLinkage::ExternalWeak: {
    auto Emitted = Streamer.emitSymbolAttribute(Symbol, mc::MCSymbolAttribute::Weak);
    if (!Emitted)
      return std::unexpected(Emitted.error());
    break;
  }
  case GlobalLinkage::Internal:
  case GlobalLinkage::Private: {
    auto Emitted = Streamer.emitSymbolAttribute(Symbol, mc::MCSymbolAttribute::Local);
    if (!Emitted)
      return std::unexpected(Emitted.error());
    break;
  }
  case GlobalLinkage::AvailableExternally:
    break;
  case GlobalLinkage::Count:
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "global object has invalid linkage during assembly emission");
  }

  if (Object.Visibility == GlobalVisibility::Hidden) {
    auto Emitted = Streamer.emitSymbolAttribute(Symbol, mc::MCSymbolAttribute::Hidden);
    if (!Emitted)
      return std::unexpected(Emitted.error());
  } else if (Object.Visibility == GlobalVisibility::Protected) {
    auto Emitted = Streamer.emitSymbolAttribute(Symbol, mc::MCSymbolAttribute::Protected);
    if (!Emitted)
      return std::unexpected(Emitted.error());
  }
  return Streamer.emitSymbolAttribute(Symbol, Object.TLSModel == GlobalTLSModel::None
                                                  ? mc::MCSymbolAttribute::Object
                                                  : mc::MCSymbolAttribute::TLSObject);
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
                            "MC expression requires an integer no wider than 64 bits");
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
                            "assembly cannot lower this generic constant expression");
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

} // namespace

Expected<void> AsmPrinter::emitStartOfAsmFile() {
  return {};
}

Expected<void> AsmPrinter::emitEndOfAsmFile() {
  return {};
}

Expected<void> AsmPrinter::emitTargetConstant(const TargetConstant &) {
  return makeUnexpected(ErrorCode::Unsupported,
                        "target does not support assembly emission for this target constant");
}

Expected<void> AsmPrinter::emitTargetJumpTable(const MachineFunction &,
                                               const MachineJumpTableEntry &) {
  return makeUnexpected(ErrorCode::Unsupported, "target does not support this jump-table encoding");
}

Expected<void> AsmPrinter::emitGenericConstant(const GenericConstantData &Constant,
                                               std::uint64_t AllocationSize, std::size_t Depth) {
  if (Depth > 1024) {
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "generic constant exceeds its emission nesting limit");
  }
  if (Depth == 0) {
    auto Verification = Constant.verify();
    if (!Verification)
      return std::unexpected(Verification.error());
    auto ExpectedSize = Layout.getTypeAllocSize(Constant.Type);
    if (!ExpectedSize)
      return std::unexpected(ExpectedSize.error());
    if (*ExpectedSize != AllocationSize) {
      return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                            "generic constant size does not match the target data layout");
    }
  }

  switch (Constant.Kind) {
  case GenericConstantKind::Integer: {
    if (!Constant.IntegerValue)
      return makeUnexpected(ErrorCode::InternalError, "integer constant has no value");
    return Streamer->emitBytes(bytesFromWords(Constant.IntegerValue->words(), AllocationSize));
  }
  case GenericConstantKind::Floating: {
    if (!Constant.FloatingValue)
      return makeUnexpected(ErrorCode::InternalError, "floating constant has no value");
    return Streamer->emitBytes(bytesFromWords(Constant.FloatingValue->words(), AllocationSize));
  }
  case GenericConstantKind::ByteString: {
    if (Constant.ByteValue.size() != AllocationSize) {
      return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                            "byte-string constant size does not match its target layout");
    }
    std::vector<std::uint8_t> Bytes;
    Bytes.reserve(Constant.ByteValue.size());
    for (const std::byte Byte : Constant.ByteValue)
      Bytes.push_back(std::to_integer<std::uint8_t>(Byte));
    return Streamer->emitBytes(Bytes);
  }
  case GenericConstantKind::NullPointer:
  case GenericConstantKind::ZeroInitializer:
  case GenericConstantKind::Undef:
  case GenericConstantKind::Poison:
    return Streamer->emitZeroFill(AllocationSize);
  case GenericConstantKind::Aggregate: {
    const GenericConstantType &Type = Constant.Type.resolved();
    std::uint64_t Offset = 0;
    for (const GenericConstantData &Element : Constant.Elements) {
      auto ElementSize = Layout.getTypeAllocSize(Element.Type);
      auto ElementAlignment = Layout.getABITypeAlign(Element.Type);
      if (!ElementSize)
        return std::unexpected(ElementSize.error());
      if (!ElementAlignment)
        return std::unexpected(ElementAlignment.error());
      const std::uint64_t Alignment =
          Type.Kind == GenericConstantTypeKind::Struct && Type.Packed ? 1 : *ElementAlignment;
      const std::uint64_t AlignmentMask = Alignment - 1U;
      if (Offset > std::numeric_limits<std::uint64_t>::max() - AlignmentMask) {
        return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                              "aggregate constant exceeds its target layout");
      }
      const std::uint64_t ElementOffset = (Offset + AlignmentMask) & ~AlignmentMask;
      if (ElementOffset > AllocationSize || *ElementSize > AllocationSize - ElementOffset) {
        return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                              "aggregate constant exceeds its target layout");
      }
      auto Padding = Streamer->emitZeroFill(ElementOffset - Offset);
      if (!Padding)
        return std::unexpected(Padding.error());
      auto Emitted = emitGenericConstant(Element, *ElementSize, Depth + 1);
      if (!Emitted)
        return std::unexpected(Emitted.error());
      Offset = ElementOffset + *ElementSize;
    }
    if (Offset > AllocationSize) {
      return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                            "aggregate constant exceeds its target layout");
    }
    return Streamer->emitZeroFill(AllocationSize - Offset);
  }
  case GenericConstantKind::GlobalAddress:
  case GenericConstantKind::BlockAddress:
  case GenericConstantKind::DSOLocalEquivalent:
  case GenericConstantKind::NoCFI:
  case GenericConstantKind::Expression: {
    if (!validValueSize(AllocationSize)) {
      return makeUnexpected(ErrorCode::Unsupported,
                            "symbolic constant has an unsupported storage size");
    }
    auto Expression = lowerConstantExpression(Constant, *Context);
    if (!Expression)
      return std::unexpected(Expression.error());
    return Streamer->emitValue(std::move(*Expression), static_cast<std::uint8_t>(AllocationSize));
  }
  case GenericConstantKind::PointerAuth:
    return makeUnexpected(ErrorCode::Unsupported,
                          "assembly does not support pointer-auth constants");
  case GenericConstantKind::Invalid:
    return makeUnexpected(ErrorCode::InternalError, "generic constant kind is invalid");
  }
  return makeUnexpected(ErrorCode::InternalError, "generic constant kind is invalid");
}

Expected<void> AsmPrinter::emitGlobalInitializer(const GlobalInitializer &Initializer,
                                                 std::size_t Depth) {
  if (Depth > 1024) {
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "global initializer exceeds its emission nesting limit");
  }
  if (Depth == 0) {
    auto Verification = Initializer.verify();
    if (!Verification)
      return std::unexpected(Verification.error());
  }

  switch (Initializer.kind()) {
  case GlobalInitializer::Kind::Integer: {
    const auto *Value = Initializer.getIf<APInt>();
    if (Value == nullptr)
      return makeUnexpected(ErrorCode::InternalError, "integer initializer has no value");
    const std::uint64_t Size = (static_cast<std::uint64_t>(Value->bitWidth()) + 7U) / 8U;
    return Streamer->emitBytes(bytesFromWords(Value->words(), Size));
  }
  case GlobalInitializer::Kind::Floating: {
    const auto *Value = Initializer.getIf<APFloat>();
    if (Value == nullptr)
      return makeUnexpected(ErrorCode::InternalError, "floating initializer has no value");
    const std::uint64_t Size = (static_cast<std::uint64_t>(Value->bitWidth()) + 7U) / 8U;
    return Streamer->emitBytes(bytesFromWords(Value->words(), Size));
  }
  case GlobalInitializer::Kind::Bytes: {
    const auto *Value = Initializer.getIf<std::vector<std::byte>>();
    if (Value == nullptr)
      return makeUnexpected(ErrorCode::InternalError, "byte initializer has no value");
    std::vector<std::uint8_t> Bytes;
    Bytes.reserve(Value->size());
    for (const std::byte Byte : *Value)
      Bytes.push_back(std::to_integer<std::uint8_t>(Byte));
    return Streamer->emitBytes(Bytes);
  }
  case GlobalInitializer::Kind::ZeroFill: {
    const auto *Value = Initializer.getIf<GlobalZeroFill>();
    return Value == nullptr
               ? makeUnexpected(ErrorCode::InternalError, "zero-fill initializer has no value")
               : Streamer->emitZeroFill(Value->Size);
  }
  case GlobalInitializer::Kind::Repeated: {
    const auto *Value = Initializer.getIf<GlobalRepeatedInitializer>();
    if (Value == nullptr || !Value->Value)
      return makeUnexpected(ErrorCode::InternalError, "repeated initializer has no value");
    for (std::uint64_t Index = 0; Index < Value->Count; ++Index) {
      auto Emitted = emitGlobalInitializer(*Value->Value, Depth + 1);
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
      auto Emitted = emitGlobalInitializer(*Element, Depth + 1);
      if (!Emitted)
        return std::unexpected(Emitted.error());
    }
    return {};
  }
  case GlobalInitializer::Kind::SymbolAddress: {
    const auto *Value = Initializer.getIf<GlobalSymbolAddress>();
    if (Value == nullptr)
      return makeUnexpected(ErrorCode::InternalError, "symbol initializer has no value");
    auto Symbol = Context->symbol(Value->Symbol, mc::MCSymbolKind::Object);
    if (!Symbol)
      return std::unexpected(Symbol.error());
    auto Expression = symbolExpression(std::move(*Symbol));
    if (!Expression)
      return std::unexpected(Expression.error());
    auto WithAddend = addConstant(std::move(*Expression), Value->Addend);
    if (!WithAddend)
      return std::unexpected(WithAddend.error());
    auto Lowered = Lowering->lowerSymbolExpression(std::move(*WithAddend), Value->TargetFlags);
    if (!Lowered)
      return std::unexpected(Lowered.error());
    return Streamer->emitValue(std::move(*Lowered), Value->Size);
  }
  case GlobalInitializer::Kind::SymbolDifference: {
    const auto *Value = Initializer.getIf<GlobalSymbolDifference>();
    if (Value == nullptr)
      return makeUnexpected(ErrorCode::InternalError, "symbol difference has no value");
    auto Minuend = Context->symbol(Value->Minuend);
    if (!Minuend)
      return std::unexpected(Minuend.error());
    auto Subtrahend = Context->symbol(Value->Subtrahend);
    if (!Subtrahend)
      return std::unexpected(Subtrahend.error());
    auto Difference = symbolDifference(std::move(*Minuend), std::move(*Subtrahend), Value->Addend);
    if (!Difference)
      return std::unexpected(Difference.error());
    return Streamer->emitValue(std::move(*Difference), Value->Size);
  }
  case GlobalInitializer::Kind::Target: {
    const auto *Value = Initializer.getIf<TargetConstant>();
    if (Value == nullptr)
      return makeUnexpected(ErrorCode::InternalError, "target initializer has no value");
    return emitTargetConstant(*Value);
  }
  case GlobalInitializer::Kind::AlignmentPadding: {
    const auto *Value = Initializer.getIf<GlobalAlignmentPadding>();
    return Value == nullptr
               ? makeUnexpected(ErrorCode::InternalError, "padding initializer has no value")
               : emitRepeatedByte(Value->Fill, Value->Size, *Streamer);
  }
  case GlobalInitializer::Kind::UndefinedStorage: {
    const auto *Value = Initializer.getIf<GlobalUndefinedStorage>();
    return Value == nullptr
               ? makeUnexpected(ErrorCode::InternalError, "undefined initializer has no value")
               : Streamer->emitZeroFill(Value->Size);
  }
  case GlobalInitializer::Kind::Invalid:
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed, "global initializer is invalid");
  }
  return makeUnexpected(ErrorCode::InternalError, "global initializer kind is invalid");
}

Expected<void> AsmPrinter::verifyFunction(const MachineFunction &Function) const {
  const MachineFunctionProperties Required = {
      MachineFunctionProperty::NoPHIs,
      MachineFunctionProperty::Legalized,
      MachineFunctionProperty::RegBankSelected,
      MachineFunctionProperty::Selected,
      MachineFunctionProperty::TiedOpsRewritten,
      MachineFunctionProperty::NoVRegs,
      MachineFunctionProperty::FrameFinalized,
      MachineFunctionProperty::NoFrameIndices,
      MachineFunctionProperty::NoExecutablePseudos,
  };
  if (!Function.properties().containsAll(Required)) {
    const MachineFunctionProperties Missing = Required.missingFrom(Function.properties());
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "assembly emission requires finalized MIR properties: " +
                              Missing.toString());
  }
  if (!Options.VerifyInput)
    return {};
  return MachineVerifier::verify(Function, *Target,
                                 MachineVerifierOptions{.Mode = MachineVerifierMode::Generic,
                                                        .Stage = PipelineStage::Emission,
                                                        .VerifyProperties = true});
}

Expected<void> AsmPrinter::emitCompilationUnit(const CompilationUnit &Unit) {
  if (EmissionStarted) {
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "an AsmPrinter can only emit one compilation unit or function");
  }
  EmissionStarted = true;

  auto Initialized = Context->initialize(Unit);
  if (!Initialized)
    return std::unexpected(Initialized.error());
  auto Header = emitStartOfAsmFile();
  if (!Header)
    return std::unexpected(Header.error());

  auto Globals = emitGlobalObjects(Unit);
  if (!Globals)
    return std::unexpected(Globals.error());

  for (std::size_t Index = 0; Index < Unit.functionCount(); ++Index) {
    const MachineFunction *Function = Unit.functionAt(Index);
    if (Function == nullptr) {
      return makeUnexpected(ErrorCode::InternalError,
                            "compilation unit contains a null function during assembly emission");
    }
    auto Emitted = emitFunctionImpl(*Function);
    if (!Emitted)
      return std::unexpected(Emitted.error());
  }

  auto Trailer = emitEndOfAsmFile();
  if (!Trailer)
    return std::unexpected(Trailer.error());
  auto Finished = Streamer->finish();
  if (!Finished)
    return std::unexpected(Finished.error());
  EmissionFinished = true;
  return {};
}

Expected<void> AsmPrinter::emitGlobalObjects(const CompilationUnit &Unit) {
  for (std::size_t Index = 0; Index < Unit.globalObjectCount(); ++Index) {
    const GlobalObjectDesc *Object = Unit.globalObjectAt(Index);
    if (Object == nullptr)
      return makeUnexpected(ErrorCode::InternalError, "compilation unit contains a null global");
    if (Object->Linkage == GlobalLinkage::AvailableExternally)
      continue;

    const mc::MCSymbolRef Symbol = Context->globalSymbol(*Object);
    if (!Symbol)
      return makeUnexpected(ErrorCode::InternalError, "global object has no MC symbol");
    auto Attributes = emitGlobalAttributes(*Object, Symbol, *Streamer);
    if (!Attributes)
      return std::unexpected(Attributes.error());
    if (Object->isDeclaration())
      continue;

    if (Object->Linkage == GlobalLinkage::Common) {
      auto Common = Streamer->emitCommonSymbol(Symbol, Object->Size, Object->Align.bytes());
      if (!Common)
        return std::unexpected(Common.error());
      continue;
    }

    auto Description = ObjectFile->globalSection(*Object, *Context);
    if (!Description)
      return std::unexpected(Description.error());
    auto Section = Context->context().getOrCreateSection(std::move(*Description));
    if (!Section)
      return std::unexpected(Section.error());
    auto Switched = Streamer->switchSection(std::move(*Section));
    if (!Switched)
      return std::unexpected(Switched.error());
    auto Aligned = Streamer->emitAlignment(Object->Align.bytes());
    if (!Aligned)
      return std::unexpected(Aligned.error());
    auto Label = Streamer->emitLabel(Symbol);
    if (!Label)
      return std::unexpected(Label.error());
    if (!Object->Initializer) {
      return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                            "defined global object has no initializer");
    }
    auto Initializer = emitGlobalInitializer(*Object->Initializer);
    if (!Initializer)
      return std::unexpected(Initializer.error());
    auto Size = Streamer->emitSymbolSize(
        Symbol, mc::MCExpr::constant(static_cast<std::int64_t>(Object->Size)));
    if (!Size)
      return std::unexpected(Size.error());
  }
  return {};
}

Expected<void> AsmPrinter::emitFunction(const MachineFunction &Function) {
  if (EmissionStarted) {
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "an AsmPrinter can only emit one compilation unit or function");
  }
  EmissionStarted = true;
  auto Header = emitStartOfAsmFile();
  if (!Header)
    return std::unexpected(Header.error());
  auto Emitted = emitFunctionImpl(Function);
  if (!Emitted)
    return std::unexpected(Emitted.error());
  auto Trailer = emitEndOfAsmFile();
  if (!Trailer)
    return std::unexpected(Trailer.error());
  auto Finished = Streamer->finish();
  if (!Finished)
    return std::unexpected(Finished.error());
  EmissionFinished = true;
  return {};
}

Expected<void> AsmPrinter::emitFunctionImpl(const MachineFunction &Function) {
  if (EmissionFinished) {
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "cannot emit a function after the assembly stream is finalized");
  }
  auto Verification = verifyFunction(Function);
  if (!Verification)
    return std::unexpected(Verification.error());
  auto Initialized = Context->initializeFunction(Function);
  if (!Initialized)
    return std::unexpected(Initialized.error());

  auto SectionDescription = ObjectFile->textSection(Function);
  if (!SectionDescription)
    return std::unexpected(SectionDescription.error());
  auto Section = Context->context().getOrCreateSection(std::move(*SectionDescription));
  if (!Section)
    return std::unexpected(Section.error());
  auto Switched = Streamer->switchSection(std::move(*Section));
  if (!Switched)
    return std::unexpected(Switched.error());

  const mc::MCSymbolRef FunctionSymbol = Context->functionSymbol(Function);
  if (!FunctionSymbol) {
    return makeUnexpected(ErrorCode::InternalError,
                          "function has no MC symbol after context initialization");
  }
  if (Options.EmitFunctionDirectives) {
    auto Global = Streamer->emitSymbolAttribute(FunctionSymbol, mc::MCSymbolAttribute::Global);
    if (!Global)
      return std::unexpected(Global.error());
    auto Type = Streamer->emitSymbolAttribute(FunctionSymbol, mc::MCSymbolAttribute::Function);
    if (!Type)
      return std::unexpected(Type.error());
  }
  if (const auto Alignment = Function.attributes().alignment();
      Alignment && Alignment->bytes() > 1) {
    auto Aligned = Streamer->emitAlignment(Alignment->bytes());
    if (!Aligned)
      return std::unexpected(Aligned.error());
  }
  auto FunctionLabel = Streamer->emitLabel(FunctionSymbol);
  if (!FunctionLabel)
    return std::unexpected(FunctionLabel.error());
  if (Options.EmitCFI) {
    mc::MCCFIInstruction StartProc;
    StartProc.Operation = mc::MCCFIOperation::StartProc;
    auto CFIStart = Streamer->emitCFI(StartProc);
    if (!CFIStart)
      return std::unexpected(CFIStart.error());
  }

  auto Body = emitFunctionBody(Function);
  if (!Body)
    return std::unexpected(Body.error());
  if (Options.EmitCFI) {
    mc::MCCFIInstruction EndProc;
    EndProc.Operation = mc::MCCFIOperation::EndProc;
    auto CFIEnd = Streamer->emitCFI(EndProc);
    if (!CFIEnd)
      return std::unexpected(CFIEnd.error());
  }

  auto EndSymbol = Context->symbol(".Lfunc_end" + std::to_string(Function.stableID().value()),
                                   mc::MCSymbolKind::Temporary);
  if (!EndSymbol)
    return std::unexpected(EndSymbol.error());
  auto EndLabel = Streamer->emitLabel(*EndSymbol);
  if (!EndLabel)
    return std::unexpected(EndLabel.error());
  if (Options.EmitFunctionDirectives) {
    auto Size = symbolDifference(*EndSymbol, FunctionSymbol, 0);
    if (!Size)
      return std::unexpected(Size.error());
    auto EmittedSize = Streamer->emitSymbolSize(FunctionSymbol, std::move(*Size));
    if (!EmittedSize)
      return std::unexpected(EmittedSize.error());
  }

  auto Constants = emitConstantPool(Function);
  if (!Constants)
    return std::unexpected(Constants.error());
  return emitJumpTables(Function);
}

Expected<void> AsmPrinter::emitFunctionBody(const MachineFunction &Function) {
  for (const MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
       Block = Block->next()) {
    if (Block->attributes().alignment().bytes() > 1) {
      auto Aligned = Streamer->emitAlignment(Block->attributes().alignment().bytes());
      if (!Aligned)
        return std::unexpected(Aligned.error());
    }
    if (Options.EmitBlockLabels) {
      const mc::MCSymbolRef Symbol = Context->blockSymbol(*Block);
      if (!Symbol)
        return makeUnexpected(ErrorCode::InternalError, "machine block has no MC symbol");
      auto Label = Streamer->emitLabel(Symbol);
      if (!Label)
        return std::unexpected(Label.error());
    }

    for (const MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      const InstructionDescriptor *Descriptor =
          Target->instructionDescriptor(Instruction->opcode());
      if (Descriptor == nullptr) {
        return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                              "assembly emission encountered an unknown opcode");
      }
      if (Options.EmitInstructionSymbols) {
        if (const auto SymbolName = Instruction->preInstrSymbol()) {
          auto Symbol = Context->symbol(std::string(*SymbolName), mc::MCSymbolKind::Temporary);
          if (!Symbol)
            return std::unexpected(Symbol.error());
          auto Label = Streamer->emitLabel(std::move(*Symbol));
          if (!Label)
            return std::unexpected(Label.error());
        }
      }

      Expected<void> Emitted;
      if (Instruction->opcode() == opcode(GenericOpcode::CFI_INSTRUCTION)) {
        const auto *Index = Instruction->operandAt(0) == nullptr
                                ? nullptr
                                : Instruction->operandAt(0)->getIf<CFIIndexOperand>();
        const MachineCFIInstruction *CFI =
            Index == nullptr ? nullptr : Function.cfiInfo().instruction(Index->Index);
        if (CFI == nullptr) {
          return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                                "CFI pseudo references an invalid CFI instruction");
        }
        if (Options.EmitCFI) {
          auto Lowered = lowerCFIInstruction(*CFI, *Context);
          if (!Lowered)
            return std::unexpected(Lowered.error());
          Emitted = Streamer->emitCFI(*Lowered);
        }
      } else if (Instruction->opcode() == opcode(GenericOpcode::EH_LABEL) ||
                 Instruction->opcode() == opcode(GenericOpcode::GC_LABEL) ||
                 Instruction->opcode() == opcode(GenericOpcode::ANNOTATION_LABEL)) {
        if (Instruction->operandAt(0) == nullptr) {
          return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                                "label pseudo has no symbol operand");
        }
        Emitted = emitNamedLabel(*Instruction->operandAt(0), *Context, *Streamer);
      } else if (Descriptor->Flags.has(InstructionFlag::TerminalEmission)) {
        Emitted = Lowering->emitTerminalInstruction(*Instruction, *Context, *Streamer);
      } else if (Descriptor->Flags.has(InstructionFlag::Meta)) {
        Emitted = {};
      } else if (Descriptor->Flags.has(InstructionFlag::ProducesCode)) {
        auto Lowered = Lowering->lowerInstruction(*Instruction, *Context);
        if (!Lowered)
          return std::unexpected(Lowered.error());
        Emitted = Streamer->emitInstruction(*Lowered);
      } else {
        return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                              "non-meta instruction has no assembly emission behavior");
      }
      if (!Emitted)
        return std::unexpected(Emitted.error());

      if (Options.EmitInstructionSymbols) {
        if (const auto SymbolName = Instruction->postInstrSymbol()) {
          auto Symbol = Context->symbol(std::string(*SymbolName), mc::MCSymbolKind::Temporary);
          if (!Symbol)
            return std::unexpected(Symbol.error());
          auto Label = Streamer->emitLabel(std::move(*Symbol));
          if (!Label)
            return std::unexpected(Label.error());
        }
      }
    }
  }
  return {};
}

Expected<void> AsmPrinter::emitConstantPool(const MachineFunction &Function) {
  for (const MachineConstantPoolEntry &Entry : Function.constantPool().entries()) {
    const MachineConstantPoolEntryDesc *Description = Entry.description();
    if (Description == nullptr)
      continue;
    auto SectionDescription = ObjectFile->constantPoolSection(Function, Entry);
    if (!SectionDescription)
      return std::unexpected(SectionDescription.error());
    auto Section = Context->context().getOrCreateSection(std::move(*SectionDescription));
    if (!Section)
      return std::unexpected(Section.error());
    auto Switched = Streamer->switchSection(std::move(*Section));
    if (!Switched)
      return std::unexpected(Switched.error());
    auto Aligned = Streamer->emitAlignment(Description->Align.bytes());
    if (!Aligned)
      return std::unexpected(Aligned.error());

    const mc::MCSymbolRef Symbol = Context->constantPoolSymbol(Function, Entry.index());
    if (!Symbol)
      return makeUnexpected(ErrorCode::InternalError, "constant-pool entry has no MC symbol");
    auto Label = Streamer->emitLabel(Symbol);
    if (!Label)
      return std::unexpected(Label.error());

    Expected<void> Emitted;
    if (const auto *Generic = std::get_if<GenericConstantData>(&Description->Value)) {
      Emitted = emitGenericConstant(*Generic, Description->Size);
    } else if (const auto *TargetValue = std::get_if<TargetConstant>(&Description->Value)) {
      Emitted = emitTargetConstant(*TargetValue);
    } else if (const auto *Difference =
                   std::get_if<SymbolDifferenceConstant>(&Description->Value)) {
      if (!validValueSize(Description->Size)) {
        return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                              "symbol-difference constant has an unsupported storage size");
      }
      auto Minuend = Context->symbol(Difference->Minuend);
      if (!Minuend)
        return std::unexpected(Minuend.error());
      auto Subtrahend = Context->symbol(Difference->Subtrahend);
      if (!Subtrahend)
        return std::unexpected(Subtrahend.error());
      auto Expression = symbolDifference(*Minuend, *Subtrahend, Difference->Addend);
      if (!Expression)
        return std::unexpected(Expression.error());
      Emitted =
          Streamer->emitValue(std::move(*Expression), static_cast<std::uint8_t>(Description->Size));
    } else if (const auto *Address = std::get_if<BlockAddressConstant>(&Description->Value)) {
      if (!validValueSize(Description->Size)) {
        return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                              "block-address constant has an unsupported storage size");
      }
      auto AddressSymbol = Context->blockAddressSymbol(Address->FunctionName, Address->BlockName);
      if (!AddressSymbol)
        return std::unexpected(AddressSymbol.error());
      auto Expression = symbolExpression(std::move(*AddressSymbol));
      if (!Expression)
        return std::unexpected(Expression.error());
      Expression = addConstant(std::move(*Expression), Address->Addend);
      if (!Expression)
        return std::unexpected(Expression.error());
      Emitted =
          Streamer->emitValue(std::move(*Expression), static_cast<std::uint8_t>(Description->Size));
    } else if (const auto *TargetExpression =
                   std::get_if<TargetExpressionConstant>(&Description->Value)) {
      Emitted = emitTargetConstant(TargetExpression->Expression);
    } else {
      return makeUnexpected(ErrorCode::InternalError, "constant-pool value has an invalid kind");
    }
    if (!Emitted)
      return std::unexpected(Emitted.error());
  }
  return {};
}

Expected<void> AsmPrinter::emitJumpTables(const MachineFunction &Function) {
  for (const MachineJumpTableEntry &Entry : Function.jumpTableInfo().tables()) {
    const MachineJumpTableDesc *Description = Entry.description();
    if (Description == nullptr)
      continue;
    auto SectionDescription = ObjectFile->jumpTableSection(Function, Entry);
    if (!SectionDescription)
      return std::unexpected(SectionDescription.error());
    auto Section = Context->context().getOrCreateSection(std::move(*SectionDescription));
    if (!Section)
      return std::unexpected(Section.error());
    auto Switched = Streamer->switchSection(std::move(*Section));
    if (!Switched)
      return std::unexpected(Switched.error());
    auto Aligned = Streamer->emitAlignment(Description->Align.bytes());
    if (!Aligned)
      return std::unexpected(Aligned.error());

    const mc::MCSymbolRef TableSymbol = Context->jumpTableSymbol(Function, Entry.index());
    if (!TableSymbol)
      return makeUnexpected(ErrorCode::InternalError, "jump-table entry has no MC symbol");
    auto Label = Streamer->emitLabel(TableSymbol);
    if (!Label)
      return std::unexpected(Label.error());

    if (Description->Encoding != JumpTableEntryEncoding::BlockAddress &&
        Description->Encoding != JumpTableEntryEncoding::LabelDifference32 &&
        Description->Encoding != JumpTableEntryEncoding::LabelDifference64) {
      auto TargetTable = emitTargetJumpTable(Function, Entry);
      if (!TargetTable)
        return std::unexpected(TargetTable.error());
      continue;
    }

    for (const MachineBasicBlock *TargetBlock : Description->Targets) {
      if (TargetBlock == nullptr)
        return makeUnexpected(ErrorCode::AssemblyLoweringFailed, "jump table has a null target");
      const mc::MCSymbolRef TargetSymbol = Context->blockSymbol(*TargetBlock);
      if (!TargetSymbol)
        return makeUnexpected(ErrorCode::InternalError, "jump-table target has no MC symbol");

      Expected<mc::MCExprRef> Expression = symbolExpression(TargetSymbol);
      if (Description->Encoding == JumpTableEntryEncoding::LabelDifference32 ||
          Description->Encoding == JumpTableEntryEncoding::LabelDifference64)
        Expression = symbolDifference(TargetSymbol, TableSymbol, 0);
      if (!Expression)
        return std::unexpected(Expression.error());
      auto Value = Streamer->emitValue(std::move(*Expression), Description->EntrySize);
      if (!Value)
        return std::unexpected(Value.error());
    }
  }
  return {};
}

} // namespace rcc
