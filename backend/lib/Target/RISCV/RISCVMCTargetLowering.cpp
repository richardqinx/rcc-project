// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/Target/RISCV/RISCVMCTargetLowering.h"

#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineMCContext.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/Register.h"
#include "rcc/IR/Constants.h"
#include "rcc/MC/MCExpr.h"
#include "rcc/MC/MCInst.h"
#include "rcc/MC/MCStreamer.h"
#include "rcc/MC/MCSymbol.h"
#include "rcc/Target/RISCV/RISCVTarget.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string_view>
#include <utility>

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

Expected<mc::MCExprRef>
RISCVMCTargetLowering::lowerSymbolExpression(mc::MCExprRef Expression,
                                             std::uint32_t TargetFlags) const {
  return applyTargetFlags(std::move(Expression), TargetFlags);
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

} // namespace rcc::riscv
