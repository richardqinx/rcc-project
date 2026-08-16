// SPDX-License-Identifier: GPL-3.0-or-later

#include "RISCV.h"
#include "RISCVImmediateMaterialization.h"
#include "RISCVRegisterBankInfo.h"
#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/GlobalISel/InstructionSelector.h"
#include "rcc/CodeGen/LowLevelType.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/Target/RISCV/RISCVSubtarget.h"
#include "rcc/Target/RISCV/RISCVTarget.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace rcc::riscv {
namespace {

#define GET_GLOBALISEL_MATCH_TABLE
#include "RISCVGenGlobalISel.inc"
#undef GET_GLOBALISEL_MATCH_TABLE

[[nodiscard]] std::optional<LowLevelType> registerType(const MachineFunction &Function,
                                                       const MachineOperand &Operand) {
  const Register *Reg = Operand.getIf<Register>();
  if (Reg == nullptr || !Reg->isVirtual())
    return std::nullopt;
  auto Info = Function.registerInfo().virtualRegisterInfo(*Reg);
  if (!Info)
    return std::nullopt;
  return Info->get().Type;
}

[[nodiscard]] std::optional<LowLevelType> resultType(const MachineFunction &Function,
                                                     const MachineInstr &Instruction) {
  const MachineOperand *Result = Instruction.operandAt(0);
  return Result == nullptr ? std::nullopt : registerType(Function, *Result);
}

[[nodiscard]] std::optional<LowLevelType>
operandType(const MachineFunction &Function, const MachineInstr &Instruction, std::size_t Index) {
  const MachineOperand *Operand = Instruction.operandAt(Index);
  return Operand == nullptr ? std::nullopt : registerType(Function, *Operand);
}

[[nodiscard]] bool isSupportedFloatingScalar(LowLevelType Type) noexcept {
  return Type.isCategoryNeutralScalar() &&
         (Type.scalarBitWidth() == 32 || Type.scalarBitWidth() == 64);
}

[[nodiscard]] bool isSupportedIntegerType(LowLevelType Type) noexcept {
  return (Type.isCategoryNeutralScalar() && Type.scalarBitWidth() != 0 &&
          Type.scalarBitWidth() <= 64) ||
         (Type.isPointer() && Type.scalarBitWidth() == 64);
}

[[nodiscard]] bool isSupportedIntegerScalar(LowLevelType Type) noexcept {
  return Type.isCategoryNeutralScalar() && Type.scalarBitWidth() != 0 &&
         Type.scalarBitWidth() <= 64;
}

[[nodiscard]] constexpr RegisterBankID fprBank() noexcept {
  return RegisterBankID::fromValue(static_cast<std::uint32_t>(RegisterBank::FPR));
}

[[nodiscard]] Expected<RegisterBankID> registerBank(const MachineFunction &Function, Register Reg) {
  if (!Reg.isVirtual()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V generic instruction requires a virtual register");
  }
  auto Info = Function.registerInfo().virtualRegisterInfo(Reg);
  if (!Info)
    return std::unexpected(Info.error());
  if (!Info->get().Bank) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V instruction selection requires an assigned register bank");
  }
  return *Info->get().Bank;
}

[[nodiscard]] std::optional<std::uint64_t>
memoryAccessBytes(const MachineInstr &Instruction) noexcept {
  if (Instruction.memOperands().size() != 1)
    return std::nullopt;
  const MachineMemOperandDesc &Description = Instruction.memOperands().front()->description();
  if (!Description.Size.isKnown() || Description.Size.isScalable())
    return std::nullopt;
  return Description.Size.minimumBytes();
}

[[nodiscard]] Expected<void> appendImmediate(MachineInstr &Instruction, std::int64_t Value) {
  return Instruction.addOperand(MachineOperand::immediate(Value));
}

[[nodiscard]] Expected<void> insertPhysicalRegister(MachineInstr &Instruction, std::size_t Index,
                                                    PhysicalRegister Reg,
                                                    RegisterOperandFlag Flag) {
  auto Operand = MachineOperand::registerOperand(Register::fromPhysical(Reg), {Flag});
  if (!Operand)
    return std::unexpected(Operand.error());
  return Instruction.insertOperand(Index, std::move(*Operand));
}

[[nodiscard]] Expected<std::int64_t> signedImmediate(const MachineOperand &Operand) {
  if (const std::int64_t *Value = Operand.getIf<std::int64_t>())
    return *Value;
  const auto *Integer = Operand.getIf<IntegerImmediateOperand>();
  if (Integer == nullptr || Integer->Value.bitWidth() == 0 || Integer->Value.bitWidth() > 64)
    return makeUnexpected(ErrorCode::Unsupported,
                          "RISC-V constant selection requires an immediate no wider than 64 bits");
  std::uint64_t Raw = Integer->Value.words().empty() ? 0 : Integer->Value.words().front();
  const std::uint32_t Width = Integer->Value.bitWidth();
  if (Width < 64 && (Raw & (std::uint64_t{1} << (Width - 1U))) != 0)
    Raw |= ~std::uint64_t{0} << Width;
  return std::bit_cast<std::int64_t>(Raw);
}

[[nodiscard]] std::optional<std::int64_t>
directIntegerConstant(const MachineRegisterInfo &RegisterInfo, Register ValueRegister) {
  if (!ValueRegister.isVirtual())
    return std::nullopt;
  const MachineInstr *Definition = RegisterInfo.getVRegDef(ValueRegister);
  if (Definition == nullptr || Definition->opcode() != opcode(GenericOpcode::G_CONSTANT))
    return std::nullopt;
  const MachineOperand *ValueOperand = Definition->operandAt(1);
  if (ValueOperand == nullptr)
    return std::nullopt;
  auto Value = signedImmediate(*ValueOperand);
  return Value ? std::optional<std::int64_t>(*Value) : std::nullopt;
}

[[nodiscard]] bool fitsIntegerImmediate(std::int64_t Value, bool Signed,
                                        std::uint32_t BitWidth) noexcept {
  if (BitWidth == 0 || BitWidth >= 64)
    return BitWidth == 64 && (Signed || Value >= 0);
  if (Signed) {
    const std::int64_t Minimum = -(std::int64_t{1} << (BitWidth - 1U));
    const std::int64_t Maximum = (std::int64_t{1} << (BitWidth - 1U)) - 1;
    return Value >= Minimum && Value <= Maximum;
  }
  return Value >= 0 && static_cast<std::uint64_t>(Value) < (std::uint64_t{1} << BitWidth);
}

class RISCVInstructionSelector final : public InstructionSelector {
public:
  RISCVInstructionSelector(const RISCVSubtarget &Subtarget,
                           const RISCVRegisterBankInfo &RegisterBanks)
      : Subtarget(Subtarget), RegisterBanks(RegisterBanks) {}

  [[nodiscard]] Expected<void>
  constrainSelectedInstruction(MachineInstr &Instruction, MachineFunction &Function,
                               const TargetDescription &Target) const override {
    return RegisterBanks.constrainSelectedInstruction(Instruction, Function, Target);
  }

  [[nodiscard]] Expected<void> canSelect(const MachineInstr &Instruction,
                                         const MachineFunction &Function,
                                         const TargetDescription &Target) const override {
    auto Generated =
        GIMatchTableExecutor::match(generatedGlobalISelMatchTable(), Instruction, Function, Target);
    if (!Generated)
      return std::unexpected(Generated.error());
    if (*Generated)
      return {};
    if (!isGenericOpcode(Instruction.opcode())) {
      const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction.opcode());
      if (Descriptor != nullptr && !Descriptor->Flags.has(InstructionFlag::Generic))
        return {};
      return makeUnexpected(ErrorCode::Unsupported,
                            "RISC-V instruction selector has no generated rule for target pseudo");
    }
    const GenericOpcode Generic = static_cast<GenericOpcode>(Instruction.opcode().value());
    const std::optional<LowLevelType> Type = resultType(Function, Instruction);
    const auto Integer = [Type] { return Type && isSupportedIntegerType(*Type); };
    const auto Float = [Type] { return Type && isSupportedFloatingScalar(*Type); };
    switch (Generic) {
    case GenericOpcode::G_ADD:
    case GenericOpcode::G_SUB:
    case GenericOpcode::G_MUL:
    case GenericOpcode::G_SDIV:
    case GenericOpcode::G_UDIV:
    case GenericOpcode::G_SREM:
    case GenericOpcode::G_UREM:
    case GenericOpcode::G_AND:
    case GenericOpcode::G_OR:
    case GenericOpcode::G_XOR:
    case GenericOpcode::G_SHL:
    case GenericOpcode::G_LSHR:
    case GenericOpcode::G_ASHR:
    case GenericOpcode::G_PTR_ADD:
    case GenericOpcode::G_PTRMASK:
    case GenericOpcode::G_CONSTANT:
      if (!Integer())
        return makeUnexpected(ErrorCode::Unsupported,
                              "RISC-V selector requires an i1-i64 integer or 64-bit pointer value");
      if (Generic == GenericOpcode::G_CONSTANT) {
        const MachineOperand *Immediate = Instruction.operandAt(1);
        if (Immediate == nullptr || (Immediate->kind() != MachineOperand::Kind::Immediate &&
                                     Immediate->kind() != MachineOperand::Kind::IntegerImmediate))
          return makeUnexpected(ErrorCode::VerificationError,
                                "RISC-V integer constant requires an immediate operand");
      }
      return {};
    case GenericOpcode::G_LOAD:
    case GenericOpcode::G_SEXTLOAD:
    case GenericOpcode::G_ZEXTLOAD:
    case GenericOpcode::G_FPEXTLOAD:
    case GenericOpcode::G_FPTRUNCSTORE:
    case GenericOpcode::G_STORE:
    case GenericOpcode::G_BR:
    case GenericOpcode::G_BRCOND:
      return {};
    case GenericOpcode::G_FCOPYSIGN:
      if (!Float())
        return makeUnexpected(ErrorCode::Unsupported,
                              "RISC-V selector requires f32 or f64 floating values");
      return {};
    case GenericOpcode::G_FCONSTANT: {
      const MachineOperand *Immediate = Instruction.operandAt(1);
      const auto *Floating =
          Immediate == nullptr ? nullptr : Immediate->getIf<FloatingImmediateOperand>();
      if (!Float() || Floating == nullptr || Floating->Bits.bitWidth() != Type->scalarBitWidth())
        return makeUnexpected(ErrorCode::Unsupported,
                              "RISC-V selector requires a matching floating constant");
      return {};
    }
    case GenericOpcode::G_ANYEXT:
    case GenericOpcode::G_ASSERT_SEXT:
    case GenericOpcode::G_ASSERT_ZEXT:
    case GenericOpcode::G_ASSERT_ALIGN:
    case GenericOpcode::G_FREEZE:
    case GenericOpcode::G_CONSTANT_FOLD_BARRIER:
    case GenericOpcode::G_PTRTOINT:
    case GenericOpcode::G_INTTOPTR:
    case GenericOpcode::G_TRUNC:
    case GenericOpcode::G_SEXT:
    case GenericOpcode::G_SEXT_INREG:
    case GenericOpcode::G_ZEXT:
    case GenericOpcode::G_FCMP:
    case GenericOpcode::G_SDIVREM:
    case GenericOpcode::G_UDIVREM:
    case GenericOpcode::G_SMULH:
    case GenericOpcode::G_UMULH:
    case GenericOpcode::G_SMIN:
    case GenericOpcode::G_SMAX:
    case GenericOpcode::G_UMIN:
    case GenericOpcode::G_UMAX:
    case GenericOpcode::G_SELECT:
    case GenericOpcode::G_ABS:
    case GenericOpcode::G_UADDO:
    case GenericOpcode::G_USUBO:
    case GenericOpcode::G_SADDO:
    case GenericOpcode::G_SSUBO:
    case GenericOpcode::G_UMULO:
    case GenericOpcode::G_SMULO:
    case GenericOpcode::G_UADDE:
    case GenericOpcode::G_USUBE:
    case GenericOpcode::G_SADDE:
    case GenericOpcode::G_SSUBE:
    case GenericOpcode::G_ATOMICRMW_XCHG:
    case GenericOpcode::G_ATOMICRMW_ADD:
    case GenericOpcode::G_ATOMICRMW_SUB:
    case GenericOpcode::G_ATOMICRMW_AND:
    case GenericOpcode::G_ATOMICRMW_OR:
    case GenericOpcode::G_ATOMICRMW_XOR:
    case GenericOpcode::G_ATOMICRMW_MAX:
    case GenericOpcode::G_ATOMICRMW_MIN:
    case GenericOpcode::G_ATOMICRMW_UMAX:
    case GenericOpcode::G_ATOMICRMW_UMIN:
    case GenericOpcode::G_FENCE:
    case GenericOpcode::G_BRINDIRECT:
    case GenericOpcode::G_TRAP:
    case GenericOpcode::G_DEBUGTRAP:
    case GenericOpcode::G_UBSANTRAP:
    case GenericOpcode::G_PHI:
    case GenericOpcode::G_IMPLICIT_DEF:
      return {};
    default:
      return makeUnexpected(ErrorCode::Unsupported,
                            "RISC-V instruction selector has no rule for generic opcode");
    }
  }

  [[nodiscard]] Expected<void> select(MachineInstr &Instruction, MachineFunction &Function,
                                      const TargetDescription &Target) const override {
    auto Generated = GIMatchTableExecutor::select(generatedGlobalISelMatchTable(), Instruction,
                                                  Function, Target);
    if (!Generated)
      return std::unexpected(Generated.error());
    if (*Generated)
      return {};
    auto Match = canSelect(Instruction, Function, Target);
    if (!Match)
      return std::unexpected(Match.error());
    const GenericOpcode Generic = static_cast<GenericOpcode>(Instruction.opcode().value());
    const std::optional<LowLevelType> Type = resultType(Function, Instruction);

    const auto SelectDirect = [&Instruction](InstructionOpcode Selected) {
      return Instruction.setOpcode(instructionOpcode(Selected));
    };
    switch (Generic) {
    case GenericOpcode::G_ADD:
      return selectIntegerBinary(Instruction, Function, Type, InstructionOpcode::ADD,
                                 InstructionOpcode::ADDW, InstructionOpcode::ADDI,
                                 InstructionOpcode::ADDIW, true, 12, 12);
    case GenericOpcode::G_SUB:
      return selectIntegerBinary(Instruction, Type, InstructionOpcode::SUB,
                                 InstructionOpcode::SUBW);
    case GenericOpcode::G_MUL:
      return selectIntegerBinary(Instruction, Type, InstructionOpcode::MUL,
                                 InstructionOpcode::MULW);
    case GenericOpcode::G_SDIV:
      return selectNormalizedIntegerBinary(Instruction, Function, Target, Type,
                                           InstructionOpcode::DIV, InstructionOpcode::DIVW, true,
                                           true);
    case GenericOpcode::G_UDIV:
      return selectNormalizedIntegerBinary(Instruction, Function, Target, Type,
                                           InstructionOpcode::DIVU, InstructionOpcode::DIVUW, false,
                                           true);
    case GenericOpcode::G_SREM:
      return selectNormalizedIntegerBinary(Instruction, Function, Target, Type,
                                           InstructionOpcode::REM, InstructionOpcode::REMW, true,
                                           true);
    case GenericOpcode::G_UREM:
      return selectNormalizedIntegerBinary(Instruction, Function, Target, Type,
                                           InstructionOpcode::REMU, InstructionOpcode::REMUW, false,
                                           true);
    case GenericOpcode::G_SDIVREM:
      return selectDivRem(Instruction, Function, Target, Type, true);
    case GenericOpcode::G_UDIVREM:
      return selectDivRem(Instruction, Function, Target, Type, false);
    case GenericOpcode::G_AND:
      return selectIntegerBinary(Instruction, Function, Type, InstructionOpcode::AND,
                                 InstructionOpcode::AND, InstructionOpcode::ANDI,
                                 InstructionOpcode::ANDI, true, 12, 12);
    case GenericOpcode::G_OR:
      return selectIntegerBinary(Instruction, Function, Type, InstructionOpcode::OR,
                                 InstructionOpcode::OR, InstructionOpcode::ORI,
                                 InstructionOpcode::ORI, true, 12, 12);
    case GenericOpcode::G_XOR:
      return selectIntegerBinary(Instruction, Function, Type, InstructionOpcode::XOR,
                                 InstructionOpcode::XOR, InstructionOpcode::XORI,
                                 InstructionOpcode::XORI, true, 12, 12);
    case GenericOpcode::G_SHL:
      return selectIntegerBinary(Instruction, Function, Type, InstructionOpcode::SLL,
                                 InstructionOpcode::SLLW, InstructionOpcode::SLLI,
                                 InstructionOpcode::SLLIW, false, 6, 5);
    case GenericOpcode::G_LSHR:
      if (auto Selected = selectRV64WordShift(Instruction, Function, Type, Generic); !Selected) {
        return std::unexpected(Selected.error());
      } else if (*Selected) {
        return {};
      }
      return selectNormalizedIntegerBinary(
          Instruction, Function, Target, Type, InstructionOpcode::SRL, InstructionOpcode::SRLW,
          false, false, InstructionOpcode::SRLI, InstructionOpcode::SRLIW, false, 6, 5);
    case GenericOpcode::G_ASHR:
      if (auto Selected = selectRV64WordShift(Instruction, Function, Type, Generic); !Selected) {
        return std::unexpected(Selected.error());
      } else if (*Selected) {
        return {};
      }
      return selectNormalizedIntegerBinary(
          Instruction, Function, Target, Type, InstructionOpcode::SRA, InstructionOpcode::SRAW,
          true, false, InstructionOpcode::SRAI, InstructionOpcode::SRAIW, false, 6, 5);
    case GenericOpcode::G_PTR_ADD:
      return SelectDirect(InstructionOpcode::ADD);
    case GenericOpcode::G_PTRMASK:
      return SelectDirect(InstructionOpcode::AND);
    case GenericOpcode::G_LOAD:
      return selectLoad(Instruction, Function, Type);
    case GenericOpcode::G_SEXTLOAD:
      return selectExtLoad(Instruction, Type, true);
    case GenericOpcode::G_ZEXTLOAD:
      return selectExtLoad(Instruction, Type, false);
    case GenericOpcode::G_FPEXTLOAD:
      return selectFloatExtLoad(Instruction, Function, Target);
    case GenericOpcode::G_STORE:
      return selectStore(Instruction, Function);
    case GenericOpcode::G_FPTRUNCSTORE:
      return selectFloatTruncStore(Instruction, Function, Target);
    case GenericOpcode::G_BR:
      return selectBranch(Instruction);
    case GenericOpcode::G_BRCOND:
      return selectConditionalBranch(Instruction);
    case GenericOpcode::G_CONSTANT:
      return selectIntegerConstant(Instruction, Function, Target, Type);
    case GenericOpcode::G_FCONSTANT:
      return selectFloatingConstant(Instruction, Function, Target, Type);
    case GenericOpcode::G_ASSERT_SEXT:
    case GenericOpcode::G_ASSERT_ZEXT:
    case GenericOpcode::G_ASSERT_ALIGN:
      if (Instruction.operandCount() == 3) {
        auto EraseImmediate = Instruction.eraseOperand(2);
        if (!EraseImmediate)
          return std::unexpected(EraseImmediate.error());
      }
      return Instruction.setOpcode(opcode(GenericOpcode::COPY));
    case GenericOpcode::G_FREEZE:
    case GenericOpcode::G_CONSTANT_FOLD_BARRIER:
    case GenericOpcode::G_PTRTOINT:
    case GenericOpcode::G_INTTOPTR:
      return Instruction.setOpcode(opcode(GenericOpcode::COPY));
    case GenericOpcode::G_FCOPYSIGN:
      return selectCopySign(Instruction, Function, Target, Type);
    case GenericOpcode::G_ANYEXT:
    case GenericOpcode::G_SEXT:
    case GenericOpcode::G_ZEXT:
      return selectIntegerExtension(Instruction, Function, Target, Generic, Type);
    case GenericOpcode::G_TRUNC:
      return Instruction.setOpcode(opcode(GenericOpcode::COPY));
    case GenericOpcode::G_SEXT_INREG:
      return selectSignExtendInRegister(Instruction, Function, Target, Type);
    case GenericOpcode::G_FCMP:
      return selectFloatingCompare(Instruction, Function, Target);
    case GenericOpcode::G_SMULH:
      return selectMultiplyHigh(Instruction, Function, Target, Type, true);
    case GenericOpcode::G_UMULH:
      return selectMultiplyHigh(Instruction, Function, Target, Type, false);
    case GenericOpcode::G_SMIN:
    case GenericOpcode::G_SMAX:
    case GenericOpcode::G_UMIN:
    case GenericOpcode::G_UMAX:
      return selectIntegerMinMax(Instruction, Function, Target, Type, Generic);
    case GenericOpcode::G_SELECT:
      return selectValue(Instruction, Function, Target, Type);
    case GenericOpcode::G_ABS:
      return selectAbsoluteValue(Instruction, Function, Target, Type);
    case GenericOpcode::G_UADDO:
    case GenericOpcode::G_USUBO:
    case GenericOpcode::G_SADDO:
    case GenericOpcode::G_SSUBO:
    case GenericOpcode::G_UMULO:
    case GenericOpcode::G_SMULO:
      return selectOverflow(Instruction, Function, Target, Type, Generic);
    case GenericOpcode::G_UADDE:
    case GenericOpcode::G_USUBE:
    case GenericOpcode::G_SADDE:
    case GenericOpcode::G_SSUBE:
      return selectExtendedOverflow(Instruction, Function, Target, Type, Generic);
    case GenericOpcode::G_ATOMICRMW_XCHG:
    case GenericOpcode::G_ATOMICRMW_ADD:
    case GenericOpcode::G_ATOMICRMW_SUB:
    case GenericOpcode::G_ATOMICRMW_AND:
    case GenericOpcode::G_ATOMICRMW_OR:
    case GenericOpcode::G_ATOMICRMW_XOR:
    case GenericOpcode::G_ATOMICRMW_MAX:
    case GenericOpcode::G_ATOMICRMW_MIN:
    case GenericOpcode::G_ATOMICRMW_UMAX:
    case GenericOpcode::G_ATOMICRMW_UMIN:
      return selectAtomicRMW(Instruction, Function, Target, Type, Generic);
    case GenericOpcode::G_FENCE:
      return selectFence(Instruction);
    case GenericOpcode::G_BRINDIRECT:
      return selectIndirectBranch(Instruction);
    case GenericOpcode::G_TRAP:
    case GenericOpcode::G_DEBUGTRAP:
    case GenericOpcode::G_UBSANTRAP:
      return selectTrap(Instruction);
    case GenericOpcode::G_PHI:
      return Instruction.setOpcode(opcode(GenericOpcode::PHI));
    case GenericOpcode::G_IMPLICIT_DEF:
      return Instruction.setOpcode(opcode(GenericOpcode::IMPLICIT_DEF));
    case GenericOpcode::COPY:
    case GenericOpcode::IMPLICIT_DEF:
    case GenericOpcode::PHI:
    case GenericOpcode::REG_SEQUENCE:
    case GenericOpcode::INSERT_SUBREG:
    case GenericOpcode::EXTRACT_SUBREG:
    case GenericOpcode::SUBREG_TO_REG:
    case GenericOpcode::KILL:
    case GenericOpcode::BUNDLE:
    case GenericOpcode::INLINEASM:
    case GenericOpcode::CORE_PSEUDO_END:
      break;
    default:
      return makeUnexpected(ErrorCode::Unsupported,
                            "RISC-V instruction selector has no rule for generic opcode");
    }
    return makeUnexpected(ErrorCode::Unsupported,
                          "RISC-V instruction selector has no rule for generic opcode");
  }

private:
  [[nodiscard]] Expected<Register>
  createTemporary(MachineFunction &Function, LowLevelType Type, const TargetDescription &Target,
                  std::optional<RegisterBankID> Bank = std::nullopt) const {
    auto Temporary = Function.registerInfo().createVirtualRegister(Type);
    if (!Temporary)
      return std::unexpected(Temporary.error());
    if (Bank) {
      auto SetBank = Function.registerInfo().setRegisterBank(*Temporary, *Bank);
      if (!SetBank)
        return std::unexpected(SetBank.error());
    }
    auto Constraint = RegisterBanks.constrainRegister(Function, *Temporary, Target);
    if (!Constraint)
      return std::unexpected(Constraint.error());
    return *Temporary;
  }

  [[nodiscard]] Expected<void>
  constrainBuilderRegisters(MachineFunction &Function, const TargetDescription &Target,
                            std::initializer_list<Register> Registers) const {
    for (Register Reg : Registers) {
      if (!Reg.isVirtual())
        continue;
      auto Constraint = RegisterBanks.constrainRegister(Function, Reg, Target);
      if (!Constraint)
        return std::unexpected(Constraint.error());
    }
    return {};
  }

  [[nodiscard]] Expected<void> replaceImmediate(MachineInstr &Instruction, std::size_t Index,
                                                std::int64_t Value) const {
    if (Instruction.operandAt(Index) == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V instruction is missing an immediate operand");
    auto Erased = Instruction.eraseOperand(Index);
    if (!Erased)
      return std::unexpected(Erased.error());
    return Instruction.insertOperand(Index, MachineOperand::immediate(Value));
  }

  [[nodiscard]] Expected<MachineInstr *> buildBinaryAfter(MachineIRBuilder &Builder,
                                                          MachineInstr &Anchor,
                                                          InstructionOpcode Opcode, Register Def,
                                                          Register Left, Register Right) const {
    auto Point = Builder.setInsertPointAfter(Anchor);
    if (!Point)
      return std::unexpected(Point.error());
    auto RegisterConstraints =
        constrainBuilderRegisters(Builder.function(), Builder.target(), {Def, Left, Right});
    if (!RegisterConstraints)
      return std::unexpected(RegisterConstraints.error());
    auto Pending = Builder.buildInstr(instructionOpcode(Opcode));
    if (!Pending)
      return std::unexpected(Pending.error());
    auto DefResult = Pending->addDef(Def);
    if (!DefResult)
      return std::unexpected(DefResult.error());
    auto LeftResult = Pending->addUse(Left);
    if (!LeftResult)
      return std::unexpected(LeftResult.error());
    auto RightResult = Pending->addUse(Right);
    if (!RightResult)
      return std::unexpected(RightResult.error());
    auto Built = Pending->build();
    if (!Built)
      return std::unexpected(Built.error());
    auto FlagPropagation = propagateSelectionFlags(**Built, Anchor, Builder.target());
    if (!FlagPropagation)
      return std::unexpected(FlagPropagation.error());
    auto Constraint = constrainSelectedInstruction(**Built, Builder.function(), Builder.target());
    if (!Constraint)
      return std::unexpected(Constraint.error());
    return *Built;
  }

  [[nodiscard]] Expected<MachineInstr *>
  buildImmediateAfter(MachineIRBuilder &Builder, MachineInstr &Anchor, InstructionOpcode Opcode,
                      Register Def, Register Source, std::int64_t Value) const {
    auto Point = Builder.setInsertPointAfter(Anchor);
    if (!Point)
      return std::unexpected(Point.error());
    auto RegisterConstraints =
        constrainBuilderRegisters(Builder.function(), Builder.target(), {Def, Source});
    if (!RegisterConstraints)
      return std::unexpected(RegisterConstraints.error());
    auto Pending = Builder.buildInstr(instructionOpcode(Opcode));
    if (!Pending)
      return std::unexpected(Pending.error());
    auto DefResult = Pending->addDef(Def);
    if (!DefResult)
      return std::unexpected(DefResult.error());
    auto SourceResult = Pending->addUse(Source);
    if (!SourceResult)
      return std::unexpected(SourceResult.error());
    auto ImmediateResult = Pending->addImmediate(Value);
    if (!ImmediateResult)
      return std::unexpected(ImmediateResult.error());
    auto Built = Pending->build();
    if (!Built)
      return std::unexpected(Built.error());
    auto FlagPropagation = propagateSelectionFlags(**Built, Anchor, Builder.target());
    if (!FlagPropagation)
      return std::unexpected(FlagPropagation.error());
    auto Constraint = constrainSelectedInstruction(**Built, Builder.function(), Builder.target());
    if (!Constraint)
      return std::unexpected(Constraint.error());
    return *Built;
  }

  [[nodiscard]] Expected<MachineInstr *>
  buildImmediateOnlyAfter(MachineIRBuilder &Builder, MachineInstr &Anchor, InstructionOpcode Opcode,
                          Register Def, std::int64_t Value) const {
    auto Point = Builder.setInsertPointAfter(Anchor);
    if (!Point)
      return std::unexpected(Point.error());
    auto RegisterConstraints =
        constrainBuilderRegisters(Builder.function(), Builder.target(), {Def});
    if (!RegisterConstraints)
      return std::unexpected(RegisterConstraints.error());
    auto Pending = Builder.buildInstr(instructionOpcode(Opcode));
    if (!Pending)
      return std::unexpected(Pending.error());
    auto DefResult = Pending->addDef(Def);
    if (!DefResult)
      return std::unexpected(DefResult.error());
    auto ImmediateResult = Pending->addImmediate(Value);
    if (!ImmediateResult)
      return std::unexpected(ImmediateResult.error());
    auto Built = Pending->build();
    if (!Built)
      return std::unexpected(Built.error());
    auto FlagPropagation = propagateSelectionFlags(**Built, Anchor, Builder.target());
    if (!FlagPropagation)
      return std::unexpected(FlagPropagation.error());
    auto Constraint = constrainSelectedInstruction(**Built, Builder.function(), Builder.target());
    if (!Constraint)
      return std::unexpected(Constraint.error());
    return *Built;
  }

  [[nodiscard]] Expected<MachineInstr *>
  buildImmediateBefore(MachineIRBuilder &Builder, MachineInstr &Anchor, InstructionOpcode Opcode,
                       Register Def, Register Source, std::int64_t Value) const {
    auto Point = Builder.setInsertPointBefore(Anchor);
    if (!Point)
      return std::unexpected(Point.error());
    auto RegisterConstraints =
        constrainBuilderRegisters(Builder.function(), Builder.target(), {Def, Source});
    if (!RegisterConstraints)
      return std::unexpected(RegisterConstraints.error());
    auto Pending = Builder.buildInstr(instructionOpcode(Opcode));
    if (!Pending)
      return std::unexpected(Pending.error());
    auto DefResult = Pending->addDef(Def);
    if (!DefResult)
      return std::unexpected(DefResult.error());
    auto SourceResult = Pending->addUse(Source);
    if (!SourceResult)
      return std::unexpected(SourceResult.error());
    auto ImmediateResult = Pending->addImmediate(Value);
    if (!ImmediateResult)
      return std::unexpected(ImmediateResult.error());
    auto Built = Pending->build();
    if (!Built)
      return std::unexpected(Built.error());
    auto FlagPropagation = propagateSelectionFlags(**Built, Anchor, Builder.target());
    if (!FlagPropagation)
      return std::unexpected(FlagPropagation.error());
    auto Constraint = constrainSelectedInstruction(**Built, Builder.function(), Builder.target());
    if (!Constraint)
      return std::unexpected(Constraint.error());
    return *Built;
  }

  [[nodiscard]] Expected<Register> normalizeIntegerBefore(MachineInstr &Anchor,
                                                          MachineFunction &Function,
                                                          const TargetDescription &Target,
                                                          Register Source, LowLevelType Type,
                                                          bool Signed) const {
    if (!isSupportedIntegerType(Type))
      return makeUnexpected(
          ErrorCode::Unsupported,
          "RISC-V integer normalization requires an integer scalar or a 64-bit pointer");
    const std::uint32_t Bits = Type.scalarBitWidth();
    if (Bits == 64)
      return Source;

    MachineIRBuilder Builder(Function, Target);
    if (Signed && Bits == 32) {
      auto Result = createTemporary(Function, Type, Target);
      if (!Result)
        return std::unexpected(Result.error());
      auto Extended =
          buildImmediateBefore(Builder, Anchor, InstructionOpcode::ADDIW, *Result, Source, 0);
      if (!Extended)
        return std::unexpected(Extended.error());
      return *Result;
    }

    const std::uint32_t ContainerBits = Bits < 32 ? 32 : 64;
    const std::int64_t Shift = static_cast<std::int64_t>(ContainerBits - Bits);
    auto Shifted = createTemporary(Function, Type, Target);
    auto Result = createTemporary(Function, Type, Target);
    if (!Shifted || !Result)
      return std::unexpected(!Shifted ? Shifted.error() : Result.error());
    const InstructionOpcode ShiftLeft =
        ContainerBits == 32 ? InstructionOpcode::SLLIW : InstructionOpcode::SLLI;
    const InstructionOpcode ShiftRight =
        Signed ? (ContainerBits == 32 ? InstructionOpcode::SRAIW : InstructionOpcode::SRAI)
               : (ContainerBits == 32 ? InstructionOpcode::SRLIW : InstructionOpcode::SRLI);
    auto Left = buildImmediateBefore(Builder, Anchor, ShiftLeft, *Shifted, Source, Shift);
    if (!Left)
      return std::unexpected(Left.error());
    auto Right = buildImmediateBefore(Builder, Anchor, ShiftRight, *Result, *Shifted, Shift);
    if (!Right)
      return std::unexpected(Right.error());
    return *Result;
  }

  [[nodiscard]] Expected<MachineInstr *> buildUnaryAfter(MachineIRBuilder &Builder,
                                                         MachineInstr &Anchor,
                                                         InstructionOpcode Opcode, Register Def,
                                                         Register Source) const {
    auto Point = Builder.setInsertPointAfter(Anchor);
    if (!Point)
      return std::unexpected(Point.error());
    auto RegisterConstraints =
        constrainBuilderRegisters(Builder.function(), Builder.target(), {Def, Source});
    if (!RegisterConstraints)
      return std::unexpected(RegisterConstraints.error());
    auto Pending = Builder.buildInstr(instructionOpcode(Opcode));
    if (!Pending)
      return std::unexpected(Pending.error());
    auto DefResult = Pending->addDef(Def);
    if (!DefResult)
      return std::unexpected(DefResult.error());
    auto SourceResult = Pending->addUse(Source);
    if (!SourceResult)
      return std::unexpected(SourceResult.error());
    auto Built = Pending->build();
    if (!Built)
      return std::unexpected(Built.error());
    auto FlagPropagation = propagateSelectionFlags(**Built, Anchor, Builder.target());
    if (!FlagPropagation)
      return std::unexpected(FlagPropagation.error());
    auto Constraint = constrainSelectedInstruction(**Built, Builder.function(), Builder.target());
    if (!Constraint)
      return std::unexpected(Constraint.error());
    return *Built;
  }

  [[nodiscard]] Expected<MachineInstr *>
  buildGPRSelectAfter(MachineIRBuilder &Builder, MachineInstr &Anchor, MachineFunction &Function,
                      const TargetDescription &Target, LowLevelType ValueType, Register Destination,
                      Register Condition, Register TrueValue, Register FalseValue) const {
    auto IntegerType = LowLevelType::makeCategoryNeutralScalar(ValueType.scalarBitWidth());
    if (!IntegerType)
      return std::unexpected(IntegerType.error());
    auto Mask = createTemporary(Function, *IntegerType, Target);
    auto Difference = createTemporary(Function, *IntegerType, Target);
    auto MaskedDifference = createTemporary(Function, *IntegerType, Target);
    if (!Mask || !Difference || !MaskedDifference) {
      return std::unexpected(
          (!Mask ? Mask.error() : (!Difference ? Difference.error() : MaskedDifference.error())));
    }
    auto Zero = physicalRegister(PhysicalRegisterID::X0);
    if (!Zero)
      return std::unexpected(Zero.error());
    const InstructionOpcode Subtract =
        ValueType.scalarBitWidth() == 32 ? InstructionOpcode::SUBW : InstructionOpcode::SUB;
    auto MaskInstruction = buildBinaryAfter(Builder, Anchor, Subtract, *Mask,
                                            Register::fromPhysical(*Zero), Condition);
    if (!MaskInstruction)
      return std::unexpected(MaskInstruction.error());
    auto DifferenceInstruction = buildBinaryAfter(
        Builder, **MaskInstruction, InstructionOpcode::XOR, *Difference, TrueValue, FalseValue);
    if (!DifferenceInstruction)
      return std::unexpected(DifferenceInstruction.error());
    auto MaskInstructionResult =
        buildBinaryAfter(Builder, **DifferenceInstruction, InstructionOpcode::AND,
                         *MaskedDifference, *Difference, *Mask);
    if (!MaskInstructionResult)
      return std::unexpected(MaskInstructionResult.error());
    return buildBinaryAfter(Builder, **MaskInstructionResult, InstructionOpcode::XOR, Destination,
                            FalseValue, *MaskedDifference);
  }

  [[nodiscard]] Expected<void> selectDivRem(MachineInstr &Instruction, MachineFunction &Function,
                                            const TargetDescription &Target,
                                            std::optional<LowLevelType> Type, bool Signed) const {
    const MachineOperand *RemainderOperand = Instruction.operandAt(1);
    const MachineOperand *LeftOperand = Instruction.operandAt(2);
    const MachineOperand *RightOperand = Instruction.operandAt(3);
    const Register *Remainder =
        RemainderOperand == nullptr ? nullptr : RemainderOperand->getIf<Register>();
    const Register *Left = LeftOperand == nullptr ? nullptr : LeftOperand->getIf<Register>();
    const Register *Right = RightOperand == nullptr ? nullptr : RightOperand->getIf<Register>();
    if (!Type || Remainder == nullptr || Left == nullptr || Right == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V division-with-remainder has invalid operands");
    const Register RemainderRegister = *Remainder;
    auto LeftRegister = normalizeIntegerBefore(Instruction, Function, Target, *Left, *Type, Signed);
    auto RightRegister =
        normalizeIntegerBefore(Instruction, Function, Target, *Right, *Type, Signed);
    if (!LeftRegister || !RightRegister)
      return std::unexpected(!LeftRegister ? LeftRegister.error() : RightRegister.error());
    auto RemoveRemainder = Instruction.eraseOperand(1);
    if (!RemoveRemainder)
      return std::unexpected(RemoveRemainder.error());
    auto SetLeft = Instruction.operandAt(1)->setRegister(*LeftRegister);
    if (!SetLeft)
      return std::unexpected(SetLeft.error());
    auto SetRight = Instruction.operandAt(2)->setRegister(*RightRegister);
    if (!SetRight)
      return std::unexpected(SetRight.error());
    const bool IsWord = Type->scalarBitWidth() <= 32;
    const InstructionOpcode Division =
        IsWord ? (Signed ? InstructionOpcode::DIVW : InstructionOpcode::DIVUW)
               : (Signed ? InstructionOpcode::DIV : InstructionOpcode::DIVU);
    const InstructionOpcode RemainderOpcode =
        IsWord ? (Signed ? InstructionOpcode::REMW : InstructionOpcode::REMUW)
               : (Signed ? InstructionOpcode::REM : InstructionOpcode::REMU);
    auto SetDivision = Instruction.setOpcode(instructionOpcode(Division));
    if (!SetDivision)
      return std::unexpected(SetDivision.error());
    MachineIRBuilder Builder(Function, Target);
    auto BuiltRemainder = buildBinaryAfter(Builder, Instruction, RemainderOpcode, RemainderRegister,
                                           *LeftRegister, *RightRegister);
    if (!BuiltRemainder)
      return std::unexpected(BuiltRemainder.error());
    return {};
  }

  [[nodiscard]] Expected<void> selectMultiplyHigh(MachineInstr &Instruction,
                                                  MachineFunction &Function,
                                                  const TargetDescription &Target,
                                                  std::optional<LowLevelType> Type,
                                                  bool Signed) const {
    if (!Type)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V high multiplication has no result type");
    if (Type->scalarBitWidth() == 64) {
      return Instruction.setOpcode(
          instructionOpcode(Signed ? InstructionOpcode::MULH : InstructionOpcode::MULHU));
    }

    const MachineOperand *DestinationOperand = Instruction.operandAt(0);
    const MachineOperand *LeftOperand = Instruction.operandAt(1);
    const MachineOperand *RightOperand = Instruction.operandAt(2);
    const Register *Destination =
        DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
    const Register *Left = LeftOperand == nullptr ? nullptr : LeftOperand->getIf<Register>();
    const Register *Right = RightOperand == nullptr ? nullptr : RightOperand->getIf<Register>();
    if (Destination == nullptr || Left == nullptr || Right == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V high multiplication has invalid register operands");
    const Register DestinationRegister = *Destination;
    const Register RightRegister = *Right;
    auto I64 = LowLevelType::makeCategoryNeutralScalar(64);
    if (!I64)
      return std::unexpected(I64.error());
    auto ExtendedLeft = createTemporary(Function, *I64, Target);
    auto ExtendedRight = createTemporary(Function, *I64, Target);
    auto Product = createTemporary(Function, *I64, Target);
    if (!ExtendedLeft || !ExtendedRight || !Product) {
      return std::unexpected(!ExtendedLeft
                                 ? ExtendedLeft.error()
                                 : (!ExtendedRight ? ExtendedRight.error() : Product.error()));
    }

    auto RemoveRight = Instruction.eraseOperand(2);
    if (!RemoveRight)
      return std::unexpected(RemoveRight.error());
    MachineIRBuilder Builder(Function, Target);
    MachineInstr *Anchor = &Instruction;
    if (Signed) {
      auto SetDestination = Instruction.operandAt(0)->setRegister(*ExtendedLeft);
      if (!SetDestination)
        return std::unexpected(SetDestination.error());
      auto Immediate = appendImmediate(Instruction, 0);
      if (!Immediate)
        return std::unexpected(Immediate.error());
      auto SetOpcode = Instruction.setOpcode(instructionOpcode(InstructionOpcode::ADDIW));
      if (!SetOpcode)
        return std::unexpected(SetOpcode.error());
      auto ExtendRight = buildImmediateAfter(Builder, *Anchor, InstructionOpcode::ADDIW,
                                             *ExtendedRight, RightRegister, 0);
      if (!ExtendRight)
        return std::unexpected(ExtendRight.error());
      Anchor = *ExtendRight;
    } else {
      auto ShiftedLeft = createTemporary(Function, *I64, Target);
      auto ShiftedRight = createTemporary(Function, *I64, Target);
      if (!ShiftedLeft || !ShiftedRight)
        return std::unexpected(!ShiftedLeft ? ShiftedLeft.error() : ShiftedRight.error());
      auto SetDestination = Instruction.operandAt(0)->setRegister(*ShiftedLeft);
      if (!SetDestination)
        return std::unexpected(SetDestination.error());
      auto Immediate = appendImmediate(Instruction, 32);
      if (!Immediate)
        return std::unexpected(Immediate.error());
      auto SetOpcode = Instruction.setOpcode(instructionOpcode(InstructionOpcode::SLLI));
      if (!SetOpcode)
        return std::unexpected(SetOpcode.error());
      auto ExtendLeft = buildImmediateAfter(Builder, *Anchor, InstructionOpcode::SRLI,
                                            *ExtendedLeft, *ShiftedLeft, 32);
      if (!ExtendLeft)
        return std::unexpected(ExtendLeft.error());
      auto ShiftRight = buildImmediateAfter(Builder, **ExtendLeft, InstructionOpcode::SLLI,
                                            *ShiftedRight, RightRegister, 32);
      if (!ShiftRight)
        return std::unexpected(ShiftRight.error());
      auto ExtendRight = buildImmediateAfter(Builder, **ShiftRight, InstructionOpcode::SRLI,
                                             *ExtendedRight, *ShiftedRight, 32);
      if (!ExtendRight)
        return std::unexpected(ExtendRight.error());
      Anchor = *ExtendRight;
    }
    auto Multiply = buildBinaryAfter(Builder, *Anchor, InstructionOpcode::MUL, *Product,
                                     *ExtendedLeft, *ExtendedRight);
    if (!Multiply)
      return std::unexpected(Multiply.error());
    auto Extract = buildImmediateAfter(Builder, **Multiply,
                                       Signed ? InstructionOpcode::SRAI : InstructionOpcode::SRLI,
                                       DestinationRegister, *Product, 32);
    if (!Extract)
      return std::unexpected(Extract.error());
    return {};
  }

  [[nodiscard]] Expected<void> selectIntegerMinMax(MachineInstr &Instruction,
                                                   MachineFunction &Function,
                                                   const TargetDescription &Target,
                                                   std::optional<LowLevelType> Type,
                                                   GenericOpcode Generic) const {
    const MachineOperand *DestinationOperand = Instruction.operandAt(0);
    const MachineOperand *LeftOperand = Instruction.operandAt(1);
    const MachineOperand *RightOperand = Instruction.operandAt(2);
    const Register *Destination =
        DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
    const Register *Left = LeftOperand == nullptr ? nullptr : LeftOperand->getIf<Register>();
    const Register *Right = RightOperand == nullptr ? nullptr : RightOperand->getIf<Register>();
    if (!Type || Destination == nullptr || Left == nullptr || Right == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V integer min/max has invalid operands");
    auto I1 = LowLevelType::makeCategoryNeutralScalar(1);
    if (!I1)
      return std::unexpected(I1.error());
    auto Condition = createTemporary(Function, *I1, Target);
    if (!Condition)
      return std::unexpected(Condition.error());
    const Register DestinationRegister = *Destination;
    const Register LeftRegister = *Left;
    const Register RightRegister = *Right;
    const bool Unsigned = Generic == GenericOpcode::G_UMIN || Generic == GenericOpcode::G_UMAX;
    auto NormalizedLeft =
        normalizeIntegerBefore(Instruction, Function, Target, LeftRegister, *Type, !Unsigned);
    auto NormalizedRight =
        normalizeIntegerBefore(Instruction, Function, Target, RightRegister, *Type, !Unsigned);
    if (!NormalizedLeft || !NormalizedRight)
      return std::unexpected(!NormalizedLeft ? NormalizedLeft.error() : NormalizedRight.error());
    auto SetCondition = Instruction.operandAt(0)->setRegister(*Condition);
    if (!SetCondition)
      return std::unexpected(SetCondition.error());
    auto SetLeft = Instruction.operandAt(1)->setRegister(*NormalizedLeft);
    if (!SetLeft)
      return std::unexpected(SetLeft.error());
    auto SetRight = Instruction.operandAt(2)->setRegister(*NormalizedRight);
    if (!SetRight)
      return std::unexpected(SetRight.error());
    auto SetCompare = Instruction.setOpcode(
        instructionOpcode(Unsigned ? InstructionOpcode::SLTU : InstructionOpcode::SLT));
    if (!SetCompare)
      return std::unexpected(SetCompare.error());
    const bool IsMaximum = Generic == GenericOpcode::G_SMAX || Generic == GenericOpcode::G_UMAX;
    const Register TrueValue = IsMaximum ? RightRegister : LeftRegister;
    const Register FalseValue = IsMaximum ? LeftRegister : RightRegister;
    MachineIRBuilder Builder(Function, Target);
    auto Selected = buildGPRSelectAfter(Builder, Instruction, Function, Target, *Type,
                                        DestinationRegister, *Condition, TrueValue, FalseValue);
    if (!Selected)
      return std::unexpected(Selected.error());
    return {};
  }

  [[nodiscard]] Expected<void> selectValue(MachineInstr &Instruction, MachineFunction &Function,
                                           const TargetDescription &Target,
                                           std::optional<LowLevelType> Type) const {
    const MachineOperand *DestinationOperand = Instruction.operandAt(0);
    const MachineOperand *ConditionOperand = Instruction.operandAt(1);
    const MachineOperand *TrueOperand = Instruction.operandAt(2);
    const MachineOperand *FalseOperand = Instruction.operandAt(3);
    const Register *Destination =
        DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
    const Register *Condition =
        ConditionOperand == nullptr ? nullptr : ConditionOperand->getIf<Register>();
    const Register *TrueValue = TrueOperand == nullptr ? nullptr : TrueOperand->getIf<Register>();
    const Register *FalseValue =
        FalseOperand == nullptr ? nullptr : FalseOperand->getIf<Register>();
    if (!Type || Destination == nullptr || Condition == nullptr || TrueValue == nullptr ||
        FalseValue == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V value selection has invalid operands");
    }
    const Register DestinationRegister = *Destination;
    const Register TrueRegister = *TrueValue;
    const Register FalseRegister = *FalseValue;
    auto I1 = LowLevelType::makeCategoryNeutralScalar(1);
    if (!I1)
      return std::unexpected(I1.error());
    auto NormalizedCondition = createTemporary(Function, *I1, Target);
    if (!NormalizedCondition)
      return std::unexpected(NormalizedCondition.error());
    auto SetCondition = Instruction.operandAt(0)->setRegister(*NormalizedCondition);
    if (!SetCondition)
      return std::unexpected(SetCondition.error());
    auto EraseFalse = Instruction.eraseOperand(3);
    if (!EraseFalse)
      return std::unexpected(EraseFalse.error());
    auto EraseTrue = Instruction.eraseOperand(2);
    if (!EraseTrue)
      return std::unexpected(EraseTrue.error());
    auto Mask = appendImmediate(Instruction, 1);
    if (!Mask)
      return std::unexpected(Mask.error());
    auto SetNormalize = Instruction.setOpcode(instructionOpcode(InstructionOpcode::ANDI));
    if (!SetNormalize)
      return std::unexpected(SetNormalize.error());

    auto DestinationBank = registerBank(Function, DestinationRegister);
    if (!DestinationBank)
      return std::unexpected(DestinationBank.error());

    MachineIRBuilder Builder(Function, Target);
    if (*DestinationBank != fprBank()) {
      auto Selected =
          buildGPRSelectAfter(Builder, Instruction, Function, Target, *Type, DestinationRegister,
                              *NormalizedCondition, TrueRegister, FalseRegister);
      if (!Selected)
        return std::unexpected(Selected.error());
      return {};
    }

    auto IntegerType = LowLevelType::makeCategoryNeutralScalar(Type->scalarBitWidth());
    if (!IntegerType)
      return std::unexpected(IntegerType.error());
    auto IntegerTrue = createTemporary(Function, *IntegerType, Target);
    auto IntegerFalse = createTemporary(Function, *IntegerType, Target);
    auto IntegerResult = createTemporary(Function, *IntegerType, Target);
    if (!IntegerTrue || !IntegerFalse || !IntegerResult) {
      return std::unexpected(!IntegerTrue
                                 ? IntegerTrue.error()
                                 : (!IntegerFalse ? IntegerFalse.error() : IntegerResult.error()));
    }
    const InstructionOpcode ToInteger =
        Type->scalarBitWidth() == 32 ? InstructionOpcode::FMV_X_W : InstructionOpcode::FMV_X_D;
    const InstructionOpcode ToFloat =
        Type->scalarBitWidth() == 32 ? InstructionOpcode::FMV_W_X : InstructionOpcode::FMV_D_X;
    auto MoveTrue = buildUnaryAfter(Builder, Instruction, ToInteger, *IntegerTrue, TrueRegister);
    if (!MoveTrue)
      return std::unexpected(MoveTrue.error());
    auto MoveFalse = buildUnaryAfter(Builder, **MoveTrue, ToInteger, *IntegerFalse, FalseRegister);
    if (!MoveFalse)
      return std::unexpected(MoveFalse.error());
    auto Selected =
        buildGPRSelectAfter(Builder, **MoveFalse, Function, Target, *IntegerType, *IntegerResult,
                            *NormalizedCondition, *IntegerTrue, *IntegerFalse);
    if (!Selected)
      return std::unexpected(Selected.error());
    auto MoveResult =
        buildUnaryAfter(Builder, **Selected, ToFloat, DestinationRegister, *IntegerResult);
    if (!MoveResult)
      return std::unexpected(MoveResult.error());
    return {};
  }

  [[nodiscard]] Expected<void> selectAbsoluteValue(MachineInstr &Instruction,
                                                   MachineFunction &Function,
                                                   const TargetDescription &Target,
                                                   std::optional<LowLevelType> Type) const {
    const MachineOperand *DestinationOperand = Instruction.operandAt(0);
    const MachineOperand *SourceOperand = Instruction.operandAt(1);
    const Register *Destination =
        DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
    const Register *Source = SourceOperand == nullptr ? nullptr : SourceOperand->getIf<Register>();
    if (!Type || Destination == nullptr || Source == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V absolute value has invalid operands");
    const Register DestinationRegister = *Destination;
    const Register SourceRegister = *Source;
    auto NormalizedSource =
        normalizeIntegerBefore(Instruction, Function, Target, SourceRegister, *Type, true);
    if (!NormalizedSource)
      return std::unexpected(NormalizedSource.error());
    auto SignMask = createTemporary(Function, *Type, Target);
    auto Flipped = createTemporary(Function, *Type, Target);
    if (!SignMask || !Flipped)
      return std::unexpected(!SignMask ? SignMask.error() : Flipped.error());
    auto SetMask = Instruction.operandAt(0)->setRegister(*SignMask);
    if (!SetMask)
      return std::unexpected(SetMask.error());
    auto SetSource = Instruction.operandAt(1)->setRegister(*NormalizedSource);
    if (!SetSource)
      return std::unexpected(SetSource.error());
    const std::uint32_t ContainerBits = Type->scalarBitWidth() <= 32 ? 32 : 64;
    auto Shift = appendImmediate(Instruction, ContainerBits - 1U);
    if (!Shift)
      return std::unexpected(Shift.error());
    const bool IsWord = Type->scalarBitWidth() <= 32;
    auto SetShift = Instruction.setOpcode(
        instructionOpcode(IsWord ? InstructionOpcode::SRAIW : InstructionOpcode::SRAI));
    if (!SetShift)
      return std::unexpected(SetShift.error());
    MachineIRBuilder Builder(Function, Target);
    auto Flip = buildBinaryAfter(Builder, Instruction, InstructionOpcode::XOR, *Flipped,
                                 *NormalizedSource, *SignMask);
    if (!Flip)
      return std::unexpected(Flip.error());
    auto Result =
        buildBinaryAfter(Builder, **Flip, IsWord ? InstructionOpcode::SUBW : InstructionOpcode::SUB,
                         DestinationRegister, *Flipped, *SignMask);
    if (!Result)
      return std::unexpected(Result.error());
    return {};
  }

  [[nodiscard]] Expected<std::pair<Register, MachineInstr *>>
  buildSignedOverflowAfter(MachineIRBuilder &Builder, MachineInstr &Anchor,
                           MachineFunction &Function, const TargetDescription &Target,
                           LowLevelType Type, Register Result, Register Left, Register Right,
                           bool IsAdd) const {
    auto FirstDifference = createTemporary(Function, Type, Target);
    auto SecondDifference = createTemporary(Function, Type, Target);
    auto SignBits = createTemporary(Function, Type, Target);
    auto I1 = LowLevelType::makeCategoryNeutralScalar(1);
    if (!I1)
      return std::unexpected(I1.error());
    auto Overflow = createTemporary(Function, *I1, Target);
    if (!FirstDifference || !SecondDifference || !SignBits || !Overflow) {
      return std::unexpected(!FirstDifference    ? FirstDifference.error()
                             : !SecondDifference ? SecondDifference.error()
                             : !SignBits         ? SignBits.error()
                                                 : Overflow.error());
    }
    const Register FirstLeft = IsAdd ? Result : Left;
    const Register FirstRight = IsAdd ? Left : Right;
    const Register SecondLeft = Result;
    const Register SecondRight = IsAdd ? Right : Left;
    auto First = buildBinaryAfter(Builder, Anchor, InstructionOpcode::XOR, *FirstDifference,
                                  FirstLeft, FirstRight);
    if (!First)
      return std::unexpected(First.error());
    auto Second = buildBinaryAfter(Builder, **First, InstructionOpcode::XOR, *SecondDifference,
                                   SecondLeft, SecondRight);
    if (!Second)
      return std::unexpected(Second.error());
    auto Both = buildBinaryAfter(Builder, **Second, InstructionOpcode::AND, *SignBits,
                                 *FirstDifference, *SecondDifference);
    if (!Both)
      return std::unexpected(Both.error());
    const InstructionOpcode Shift =
        Type.scalarBitWidth() == 32 ? InstructionOpcode::SRLIW : InstructionOpcode::SRLI;
    auto Extract = buildImmediateAfter(Builder, **Both, Shift, *Overflow, *SignBits,
                                       Type.scalarBitWidth() - 1U);
    if (!Extract)
      return std::unexpected(Extract.error());
    return std::pair{*Overflow, *Extract};
  }

  [[nodiscard]] Expected<void> selectOverflow(MachineInstr &Instruction, MachineFunction &Function,
                                              const TargetDescription &Target,
                                              std::optional<LowLevelType> Type,
                                              GenericOpcode Generic) const {
    const MachineOperand *ResultOperand = Instruction.operandAt(0);
    const MachineOperand *OverflowOperand = Instruction.operandAt(1);
    const MachineOperand *LeftOperand = Instruction.operandAt(2);
    const MachineOperand *RightOperand = Instruction.operandAt(3);
    const Register *Result = ResultOperand == nullptr ? nullptr : ResultOperand->getIf<Register>();
    const Register *Overflow =
        OverflowOperand == nullptr ? nullptr : OverflowOperand->getIf<Register>();
    const Register *Left = LeftOperand == nullptr ? nullptr : LeftOperand->getIf<Register>();
    const Register *Right = RightOperand == nullptr ? nullptr : RightOperand->getIf<Register>();
    if (!Type || Result == nullptr || Overflow == nullptr || Left == nullptr || Right == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V overflow operation has invalid operands");
    }
    const Register ResultRegister = *Result;
    const Register OverflowRegister = *Overflow;
    const Register LeftRegister = *Left;
    const Register RightRegister = *Right;
    auto RemoveOverflow = Instruction.eraseOperand(1);
    if (!RemoveOverflow)
      return std::unexpected(RemoveOverflow.error());
    const bool IsWord = Type->scalarBitWidth() <= 32;
    const bool IsAdd = Generic == GenericOpcode::G_UADDO || Generic == GenericOpcode::G_SADDO;
    const bool IsSub = Generic == GenericOpcode::G_USUBO || Generic == GenericOpcode::G_SSUBO;
    const bool IsUnsigned = Generic == GenericOpcode::G_UADDO ||
                            Generic == GenericOpcode::G_USUBO || Generic == GenericOpcode::G_UMULO;
    const InstructionOpcode Arithmetic =
        IsAdd   ? (IsWord ? InstructionOpcode::ADDW : InstructionOpcode::ADD)
        : IsSub ? (IsWord ? InstructionOpcode::SUBW : InstructionOpcode::SUB)
                : (IsWord ? InstructionOpcode::MULW : InstructionOpcode::MUL);
    auto SetArithmetic = Instruction.setOpcode(instructionOpcode(Arithmetic));
    if (!SetArithmetic)
      return std::unexpected(SetArithmetic.error());
    MachineIRBuilder Builder(Function, Target);
    if (IsUnsigned && IsAdd) {
      auto Carry = buildBinaryAfter(Builder, Instruction, InstructionOpcode::SLTU, OverflowRegister,
                                    ResultRegister, LeftRegister);
      if (!Carry)
        return std::unexpected(Carry.error());
      return {};
    }
    if (IsUnsigned && IsSub) {
      auto Borrow = buildBinaryAfter(Builder, Instruction, InstructionOpcode::SLTU,
                                     OverflowRegister, LeftRegister, RightRegister);
      if (!Borrow)
        return std::unexpected(Borrow.error());
      return {};
    }
    if (IsAdd || IsSub) {
      auto FirstDifference = createTemporary(Function, *Type, Target);
      auto SecondDifference = createTemporary(Function, *Type, Target);
      auto SignBits = createTemporary(Function, *Type, Target);
      if (!FirstDifference || !SecondDifference || !SignBits) {
        return std::unexpected(
            !FirstDifference ? FirstDifference.error()
                             : (!SecondDifference ? SecondDifference.error() : SignBits.error()));
      }
      const Register FirstLeft = IsAdd ? ResultRegister : LeftRegister;
      const Register FirstRight = IsAdd ? LeftRegister : RightRegister;
      const Register SecondLeft = ResultRegister;
      const Register SecondRight = IsAdd ? RightRegister : LeftRegister;
      auto First = buildBinaryAfter(Builder, Instruction, InstructionOpcode::XOR, *FirstDifference,
                                    FirstLeft, FirstRight);
      if (!First)
        return std::unexpected(First.error());
      auto Second = buildBinaryAfter(Builder, **First, InstructionOpcode::XOR, *SecondDifference,
                                     SecondLeft, SecondRight);
      if (!Second)
        return std::unexpected(Second.error());
      auto Both = buildBinaryAfter(Builder, **Second, InstructionOpcode::AND, *SignBits,
                                   *FirstDifference, *SecondDifference);
      if (!Both)
        return std::unexpected(Both.error());
      auto OverflowResult = buildImmediateAfter(
          Builder, **Both, IsWord ? InstructionOpcode::SRLIW : InstructionOpcode::SRLI,
          OverflowRegister, *SignBits, Type->scalarBitWidth() - 1U);
      if (!OverflowResult)
        return std::unexpected(OverflowResult.error());
      return {};
    }

    auto I64 = LowLevelType::makeCategoryNeutralScalar(64);
    if (!I64)
      return std::unexpected(I64.error());
    auto Zero = physicalRegister(PhysicalRegisterID::X0);
    if (!Zero)
      return std::unexpected(Zero.error());
    if (!IsWord) {
      auto High = createTemporary(Function, *I64, Target);
      if (!High)
        return std::unexpected(High.error());
      auto HighProduct = buildBinaryAfter(
          Builder, Instruction, IsUnsigned ? InstructionOpcode::MULHU : InstructionOpcode::MULH,
          *High, LeftRegister, RightRegister);
      if (!HighProduct)
        return std::unexpected(HighProduct.error());
      if (IsUnsigned) {
        auto OverflowResult =
            buildBinaryAfter(Builder, **HighProduct, InstructionOpcode::SLTU, OverflowRegister,
                             Register::fromPhysical(*Zero), *High);
        if (!OverflowResult)
          return std::unexpected(OverflowResult.error());
        return {};
      }
      auto ExpectedHigh = createTemporary(Function, *I64, Target);
      auto Difference = createTemporary(Function, *I64, Target);
      if (!ExpectedHigh || !Difference)
        return std::unexpected(!ExpectedHigh ? ExpectedHigh.error() : Difference.error());
      auto Sign = buildImmediateAfter(Builder, **HighProduct, InstructionOpcode::SRAI,
                                      *ExpectedHigh, ResultRegister, 63);
      if (!Sign)
        return std::unexpected(Sign.error());
      auto Compare = buildBinaryAfter(Builder, **Sign, InstructionOpcode::XOR, *Difference, *High,
                                      *ExpectedHigh);
      if (!Compare)
        return std::unexpected(Compare.error());
      auto OverflowResult =
          buildBinaryAfter(Builder, **Compare, InstructionOpcode::SLTU, OverflowRegister,
                           Register::fromPhysical(*Zero), *Difference);
      if (!OverflowResult)
        return std::unexpected(OverflowResult.error());
      return {};
    }

    auto ExtendedLeft = createTemporary(Function, *I64, Target);
    auto ExtendedRight = createTemporary(Function, *I64, Target);
    auto FullProduct = createTemporary(Function, *I64, Target);
    if (!ExtendedLeft || !ExtendedRight || !FullProduct) {
      return std::unexpected(!ExtendedLeft
                                 ? ExtendedLeft.error()
                                 : (!ExtendedRight ? ExtendedRight.error() : FullProduct.error()));
    }
    MachineInstr *Anchor = &Instruction;
    if (IsUnsigned) {
      auto ShiftedLeft = createTemporary(Function, *I64, Target);
      auto ShiftedRight = createTemporary(Function, *I64, Target);
      if (!ShiftedLeft || !ShiftedRight)
        return std::unexpected(!ShiftedLeft ? ShiftedLeft.error() : ShiftedRight.error());
      auto LeftShift = buildImmediateAfter(Builder, *Anchor, InstructionOpcode::SLLI, *ShiftedLeft,
                                           LeftRegister, 32);
      if (!LeftShift)
        return std::unexpected(LeftShift.error());
      auto LeftExtend = buildImmediateAfter(Builder, **LeftShift, InstructionOpcode::SRLI,
                                            *ExtendedLeft, *ShiftedLeft, 32);
      if (!LeftExtend)
        return std::unexpected(LeftExtend.error());
      auto RightShift = buildImmediateAfter(Builder, **LeftExtend, InstructionOpcode::SLLI,
                                            *ShiftedRight, RightRegister, 32);
      if (!RightShift)
        return std::unexpected(RightShift.error());
      auto RightExtend = buildImmediateAfter(Builder, **RightShift, InstructionOpcode::SRLI,
                                             *ExtendedRight, *ShiftedRight, 32);
      if (!RightExtend)
        return std::unexpected(RightExtend.error());
      Anchor = *RightExtend;
    } else {
      auto LeftExtend = buildImmediateAfter(Builder, *Anchor, InstructionOpcode::ADDIW,
                                            *ExtendedLeft, LeftRegister, 0);
      if (!LeftExtend)
        return std::unexpected(LeftExtend.error());
      auto RightExtend = buildImmediateAfter(Builder, **LeftExtend, InstructionOpcode::ADDIW,
                                             *ExtendedRight, RightRegister, 0);
      if (!RightExtend)
        return std::unexpected(RightExtend.error());
      Anchor = *RightExtend;
    }
    auto Product = buildBinaryAfter(Builder, *Anchor, InstructionOpcode::MUL, *FullProduct,
                                    *ExtendedLeft, *ExtendedRight);
    if (!Product)
      return std::unexpected(Product.error());
    if (IsUnsigned) {
      auto High = createTemporary(Function, *I64, Target);
      if (!High)
        return std::unexpected(High.error());
      auto ExtractHigh =
          buildImmediateAfter(Builder, **Product, InstructionOpcode::SRLI, *High, *FullProduct, 32);
      if (!ExtractHigh)
        return std::unexpected(ExtractHigh.error());
      auto OverflowResult =
          buildBinaryAfter(Builder, **ExtractHigh, InstructionOpcode::SLTU, OverflowRegister,
                           Register::fromPhysical(*Zero), *High);
      if (!OverflowResult)
        return std::unexpected(OverflowResult.error());
      return {};
    }
    auto Difference = createTemporary(Function, *I64, Target);
    if (!Difference)
      return std::unexpected(Difference.error());
    auto Compare = buildBinaryAfter(Builder, **Product, InstructionOpcode::XOR, *Difference,
                                    *FullProduct, ResultRegister);
    if (!Compare)
      return std::unexpected(Compare.error());
    auto OverflowResult =
        buildBinaryAfter(Builder, **Compare, InstructionOpcode::SLTU, OverflowRegister,
                         Register::fromPhysical(*Zero), *Difference);
    if (!OverflowResult)
      return std::unexpected(OverflowResult.error());
    return {};
  }

  [[nodiscard]] Expected<void> selectExtendedOverflow(MachineInstr &Instruction,
                                                      MachineFunction &Function,
                                                      const TargetDescription &Target,
                                                      std::optional<LowLevelType> Type,
                                                      GenericOpcode Generic) const {
    const MachineOperand *ResultOperand = Instruction.operandAt(0);
    const MachineOperand *CarryOutOperand = Instruction.operandAt(1);
    const MachineOperand *LeftOperand = Instruction.operandAt(2);
    const MachineOperand *RightOperand = Instruction.operandAt(3);
    const MachineOperand *CarryInOperand = Instruction.operandAt(4);
    const Register *Result = ResultOperand == nullptr ? nullptr : ResultOperand->getIf<Register>();
    const Register *CarryOut =
        CarryOutOperand == nullptr ? nullptr : CarryOutOperand->getIf<Register>();
    const Register *Left = LeftOperand == nullptr ? nullptr : LeftOperand->getIf<Register>();
    const Register *Right = RightOperand == nullptr ? nullptr : RightOperand->getIf<Register>();
    const Register *CarryIn =
        CarryInOperand == nullptr ? nullptr : CarryInOperand->getIf<Register>();
    if (!Type || Result == nullptr || CarryOut == nullptr || Left == nullptr || Right == nullptr ||
        CarryIn == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V extended overflow operation has invalid operands");
    }
    const Register ResultRegister = *Result;
    const Register CarryOutRegister = *CarryOut;
    const Register LeftRegister = *Left;
    const Register RightRegister = *Right;
    const Register CarryInRegister = *CarryIn;
    const bool IsAdd = Generic == GenericOpcode::G_UADDE || Generic == GenericOpcode::G_SADDE;
    const bool IsUnsigned = Generic == GenericOpcode::G_UADDE || Generic == GenericOpcode::G_USUBE;
    const bool IsWord = Type->scalarBitWidth() <= 32;
    const InstructionOpcode Arithmetic =
        IsAdd ? (IsWord ? InstructionOpcode::ADDW : InstructionOpcode::ADD)
              : (IsWord ? InstructionOpcode::SUBW : InstructionOpcode::SUB);
    auto Partial = createTemporary(Function, *Type, Target);
    auto I1 = LowLevelType::makeCategoryNeutralScalar(1);
    if (!Partial || !I1)
      return std::unexpected(!Partial ? Partial.error() : I1.error());
    auto NormalizedCarry = createTemporary(Function, *I1, Target);
    if (!NormalizedCarry)
      return std::unexpected(NormalizedCarry.error());
    auto SetPartial = Instruction.operandAt(0)->setRegister(*Partial);
    if (!SetPartial)
      return std::unexpected(SetPartial.error());
    auto RemoveCarryOut = Instruction.eraseOperand(1);
    if (!RemoveCarryOut)
      return std::unexpected(RemoveCarryOut.error());
    auto RemoveCarryIn = Instruction.eraseOperand(3);
    if (!RemoveCarryIn)
      return std::unexpected(RemoveCarryIn.error());
    auto SetArithmetic = Instruction.setOpcode(instructionOpcode(Arithmetic));
    if (!SetArithmetic)
      return std::unexpected(SetArithmetic.error());
    MachineIRBuilder Builder(Function, Target);
    auto Normalize = buildImmediateAfter(Builder, Instruction, InstructionOpcode::ANDI,
                                         *NormalizedCarry, CarryInRegister, 1);
    if (!Normalize)
      return std::unexpected(Normalize.error());
    auto Final = buildBinaryAfter(Builder, **Normalize, Arithmetic, ResultRegister, *Partial,
                                  *NormalizedCarry);
    if (!Final)
      return std::unexpected(Final.error());

    if (IsUnsigned) {
      auto FirstCarry = createTemporary(Function, *I1, Target);
      auto SecondCarry = createTemporary(Function, *I1, Target);
      if (!FirstCarry || !SecondCarry)
        return std::unexpected(!FirstCarry ? FirstCarry.error() : SecondCarry.error());
      auto First =
          buildBinaryAfter(Builder, **Final, InstructionOpcode::SLTU, *FirstCarry,
                           IsAdd ? *Partial : LeftRegister, IsAdd ? LeftRegister : RightRegister);
      if (!First)
        return std::unexpected(First.error());
      auto Second =
          buildBinaryAfter(Builder, **First, InstructionOpcode::SLTU, *SecondCarry,
                           IsAdd ? ResultRegister : *Partial, IsAdd ? *Partial : *NormalizedCarry);
      if (!Second)
        return std::unexpected(Second.error());
      auto Combined = buildBinaryAfter(Builder, **Second, InstructionOpcode::OR, CarryOutRegister,
                                       *FirstCarry, *SecondCarry);
      if (!Combined)
        return std::unexpected(Combined.error());
      return {};
    }

    auto FirstOverflow = buildSignedOverflowAfter(Builder, **Final, Function, Target, *Type,
                                                  *Partial, LeftRegister, RightRegister, IsAdd);
    if (!FirstOverflow)
      return std::unexpected(FirstOverflow.error());
    auto SecondOverflow =
        buildSignedOverflowAfter(Builder, *FirstOverflow->second, Function, Target, *Type,
                                 ResultRegister, *Partial, *NormalizedCarry, IsAdd);
    if (!SecondOverflow)
      return std::unexpected(SecondOverflow.error());
    auto Combined = buildBinaryAfter(Builder, *SecondOverflow->second, InstructionOpcode::OR,
                                     CarryOutRegister, FirstOverflow->first, SecondOverflow->first);
    if (!Combined)
      return std::unexpected(Combined.error());
    return {};
  }

  [[nodiscard]] Expected<void> selectAtomicRMW(MachineInstr &Instruction, MachineFunction &Function,
                                               const TargetDescription &Target,
                                               std::optional<LowLevelType> Type,
                                               GenericOpcode Generic) const {
    const MachineOperand *AddressOperand = Instruction.operandAt(1);
    const MachineOperand *ValueOperand = Instruction.operandAt(2);
    const Register *Address =
        AddressOperand == nullptr ? nullptr : AddressOperand->getIf<Register>();
    const Register *Value = ValueOperand == nullptr ? nullptr : ValueOperand->getIf<Register>();
    if (!Type || Address == nullptr || Value == nullptr)
      return makeUnexpected(ErrorCode::VerificationError, "RISC-V atomic RMW has invalid operands");
    Register SelectedValue = *Value;
    const bool IsWord = Type->scalarBitWidth() == 32;
    if (Generic == GenericOpcode::G_ATOMICRMW_SUB) {
      auto Negated = createTemporary(Function, *Type, Target);
      auto Zero = physicalRegister(PhysicalRegisterID::X0);
      if (!Negated || !Zero)
        return std::unexpected(!Negated ? Negated.error() : Zero.error());
      MachineIRBuilder Builder(Function, Target);
      auto Point = Builder.setInsertPointBefore(Instruction);
      if (!Point)
        return std::unexpected(Point.error());
      auto RegisterConstraints = constrainBuilderRegisters(Function, Target, {*Negated, *Value});
      if (!RegisterConstraints)
        return std::unexpected(RegisterConstraints.error());
      auto Pending = Builder.buildInstr(
          instructionOpcode(IsWord ? InstructionOpcode::SUBW : InstructionOpcode::SUB));
      if (!Pending)
        return std::unexpected(Pending.error());
      auto Def = Pending->addDef(*Negated);
      if (!Def)
        return std::unexpected(Def.error());
      auto Left = Pending->addUse(Register::fromPhysical(*Zero));
      if (!Left)
        return std::unexpected(Left.error());
      auto Right = Pending->addUse(*Value);
      if (!Right)
        return std::unexpected(Right.error());
      auto Built = Pending->build();
      if (!Built)
        return std::unexpected(Built.error());
      auto Constraint = constrainSelectedInstruction(**Built, Function, Target);
      if (!Constraint)
        return std::unexpected(Constraint.error());
      SelectedValue = *Negated;
    }
    const auto SelectOpcode = [Generic, IsWord]() -> InstructionOpcode {
      switch (Generic) {
      case GenericOpcode::G_ATOMICRMW_XCHG:
        return IsWord ? InstructionOpcode::AMOSWAP_W : InstructionOpcode::AMOSWAP_D;
      case GenericOpcode::G_ATOMICRMW_ADD:
      case GenericOpcode::G_ATOMICRMW_SUB:
        return IsWord ? InstructionOpcode::AMOADD_W : InstructionOpcode::AMOADD_D;
      case GenericOpcode::G_ATOMICRMW_AND:
        return IsWord ? InstructionOpcode::AMOAND_W : InstructionOpcode::AMOAND_D;
      case GenericOpcode::G_ATOMICRMW_OR:
        return IsWord ? InstructionOpcode::AMOOR_W : InstructionOpcode::AMOOR_D;
      case GenericOpcode::G_ATOMICRMW_XOR:
        return IsWord ? InstructionOpcode::AMOXOR_W : InstructionOpcode::AMOXOR_D;
      case GenericOpcode::G_ATOMICRMW_MAX:
        return IsWord ? InstructionOpcode::AMOMAX_W : InstructionOpcode::AMOMAX_D;
      case GenericOpcode::G_ATOMICRMW_MIN:
        return IsWord ? InstructionOpcode::AMOMIN_W : InstructionOpcode::AMOMIN_D;
      case GenericOpcode::G_ATOMICRMW_UMAX:
        return IsWord ? InstructionOpcode::AMOMAXU_W : InstructionOpcode::AMOMAXU_D;
      case GenericOpcode::G_ATOMICRMW_UMIN:
        return IsWord ? InstructionOpcode::AMOMINU_W : InstructionOpcode::AMOMINU_D;
      default:
        return InstructionOpcode::Invalid;
      }
    };
    const InstructionOpcode Selected = SelectOpcode();
    if (Selected == InstructionOpcode::Invalid)
      return makeUnexpected(ErrorCode::InternalError,
                            "RISC-V atomic RMW selection reached an unhandled opcode");
    auto SetValue = Instruction.operandAt(1)->setRegister(SelectedValue);
    if (!SetValue)
      return std::unexpected(SetValue.error());
    auto SetAddress = Instruction.operandAt(2)->setRegister(*Address);
    if (!SetAddress)
      return std::unexpected(SetAddress.error());
    return Instruction.setOpcode(instructionOpcode(Selected));
  }

  [[nodiscard]] Expected<void> selectIntegerExtension(MachineInstr &Instruction,
                                                      MachineFunction &Function,
                                                      const TargetDescription &Target,
                                                      GenericOpcode Generic,
                                                      std::optional<LowLevelType> Type) const {
    const std::optional<LowLevelType> SourceType = operandType(Function, Instruction, 1);
    const MachineOperand *DestinationOperand = Instruction.operandAt(0);
    const MachineOperand *SourceOperand = Instruction.operandAt(1);
    const Register *Destination =
        DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
    const Register *Source = SourceOperand == nullptr ? nullptr : SourceOperand->getIf<Register>();
    if (!Type || !SourceType || Destination == nullptr || Source == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V integer extension has invalid register operands");
    const Register DestinationRegister = *Destination;
    if (Generic == GenericOpcode::G_ANYEXT)
      return Instruction.setOpcode(opcode(GenericOpcode::COPY));

    const std::uint32_t DestinationBits = Type->scalarBitWidth();
    const std::uint32_t SourceBits = SourceType->scalarBitWidth();
    if (Generic == GenericOpcode::G_SEXT && DestinationBits == 64 && SourceBits == 32) {
      auto ImmediateResult = appendImmediate(Instruction, 0);
      if (!ImmediateResult)
        return std::unexpected(ImmediateResult.error());
      return Instruction.setOpcode(instructionOpcode(InstructionOpcode::ADDIW));
    }

    const std::uint32_t ContainerBits = DestinationBits <= 32 ? 32 : 64;
    const std::int64_t Shift = static_cast<std::int64_t>(ContainerBits - SourceBits);
    const InstructionOpcode ShiftLeft =
        ContainerBits == 32 ? InstructionOpcode::SLLIW : InstructionOpcode::SLLI;
    const InstructionOpcode ShiftRight =
        Generic == GenericOpcode::G_SEXT
            ? (ContainerBits == 32 ? InstructionOpcode::SRAIW : InstructionOpcode::SRAI)
            : (ContainerBits == 32 ? InstructionOpcode::SRLIW : InstructionOpcode::SRLI);
    auto Temporary = createTemporary(Function, *Type, Target);
    if (!Temporary)
      return std::unexpected(Temporary.error());
    auto SetDestination = Instruction.operandAt(0)->setRegister(*Temporary);
    if (!SetDestination)
      return std::unexpected(SetDestination.error());
    auto SetOpcode = Instruction.setOpcode(instructionOpcode(ShiftLeft));
    if (!SetOpcode)
      return std::unexpected(SetOpcode.error());
    auto ShiftImmediate = appendImmediate(Instruction, Shift);
    if (!ShiftImmediate)
      return std::unexpected(ShiftImmediate.error());

    MachineIRBuilder Builder(Function, Target);
    auto Shifted = buildImmediateAfter(Builder, Instruction, ShiftRight, DestinationRegister,
                                       *Temporary, Shift);
    if (!Shifted)
      return std::unexpected(Shifted.error());
    return {};
  }

  [[nodiscard]] Expected<void> selectSignExtendInRegister(MachineInstr &Instruction,
                                                          MachineFunction &Function,
                                                          const TargetDescription &Target,
                                                          std::optional<LowLevelType> Type) const {
    const MachineOperand *DestinationOperand = Instruction.operandAt(0);
    const MachineOperand *SourceOperand = Instruction.operandAt(1);
    const Register *Destination =
        DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
    const Register *Source = SourceOperand == nullptr ? nullptr : SourceOperand->getIf<Register>();
    std::optional<std::int64_t> Width;
    if (const MachineOperand *WidthOperand = Instruction.operandAt(2); WidthOperand != nullptr) {
      auto ParsedWidth = signedImmediate(*WidthOperand);
      if (ParsedWidth)
        Width = *ParsedWidth;
    }
    if (!Type || Destination == nullptr || Source == nullptr || !Width || *Width <= 0 ||
        static_cast<std::uint64_t>(*Width) > Type->scalarBitWidth())
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V sign-extension-in-register has invalid operands");
    const Register DestinationRegister = *Destination;
    if (static_cast<std::uint64_t>(*Width) == Type->scalarBitWidth()) {
      auto EraseWidth = Instruction.eraseOperand(2);
      if (!EraseWidth)
        return std::unexpected(EraseWidth.error());
      return Instruction.setOpcode(opcode(GenericOpcode::COPY));
    }

    const std::uint32_t TypeBits = Type->scalarBitWidth();
    if (TypeBits == 64 && *Width == 32) {
      auto ImmediateResult = replaceImmediate(Instruction, 2, 0);
      if (!ImmediateResult)
        return std::unexpected(ImmediateResult.error());
      return Instruction.setOpcode(instructionOpcode(InstructionOpcode::ADDIW));
    }
    const std::uint32_t ContainerBits = TypeBits <= 32 ? 32 : 64;
    const std::int64_t Shift = static_cast<std::int64_t>(ContainerBits) - *Width;
    const InstructionOpcode ShiftLeft =
        ContainerBits == 32 ? InstructionOpcode::SLLIW : InstructionOpcode::SLLI;
    const InstructionOpcode ShiftRight =
        ContainerBits == 32 ? InstructionOpcode::SRAIW : InstructionOpcode::SRAI;
    auto Temporary = createTemporary(Function, *Type, Target);
    if (!Temporary)
      return std::unexpected(Temporary.error());
    auto SetDestination = Instruction.operandAt(0)->setRegister(*Temporary);
    if (!SetDestination)
      return std::unexpected(SetDestination.error());
    auto SetOpcode = Instruction.setOpcode(instructionOpcode(ShiftLeft));
    if (!SetOpcode)
      return std::unexpected(SetOpcode.error());
    auto ShiftImmediate = replaceImmediate(Instruction, 2, Shift);
    if (!ShiftImmediate)
      return std::unexpected(ShiftImmediate.error());

    MachineIRBuilder Builder(Function, Target);
    auto Shifted = buildImmediateAfter(Builder, Instruction, ShiftRight, DestinationRegister,
                                       *Temporary, Shift);
    if (!Shifted)
      return std::unexpected(Shifted.error());
    return {};
  }

  [[nodiscard]] Expected<void> selectFloatingCompare(MachineInstr &Instruction,
                                                     MachineFunction &Function,
                                                     const TargetDescription &Target) const {
    const MachineOperand *PredicateMO = Instruction.operandAt(1);
    const auto *Predicate =
        PredicateMO == nullptr ? nullptr : PredicateMO->getIf<PredicateOperand>();
    const MachineOperand *DestinationOperand = Instruction.operandAt(0);
    const MachineOperand *LeftOperand = Instruction.operandAt(2);
    const MachineOperand *RightOperand = Instruction.operandAt(3);
    const Register *Destination =
        DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
    const Register *Left = LeftOperand == nullptr ? nullptr : LeftOperand->getIf<Register>();
    const Register *Right = RightOperand == nullptr ? nullptr : RightOperand->getIf<Register>();
    const std::optional<LowLevelType> FloatType = operandType(Function, Instruction, 2);
    const std::optional<LowLevelType> ResultType = resultType(Function, Instruction);
    if (Predicate == nullptr || Predicate->floatingPredicate() == nullptr ||
        Destination == nullptr || Left == nullptr || Right == nullptr || !FloatType || !ResultType)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V floating comparison has invalid operands");
    const FloatingPredicate Condition = *Predicate->floatingPredicate();
    const Register DestinationRegister = *Destination;
    const Register LeftRegister = *Left;
    const Register RightRegister = *Right;
    const InstructionOpcode EqualOpcode =
        FloatType->scalarBitWidth() == 32 ? InstructionOpcode::FEQ_S : InstructionOpcode::FEQ_D;
    const InstructionOpcode LessOpcode =
        FloatType->scalarBitWidth() == 32 ? InstructionOpcode::FLT_S : InstructionOpcode::FLT_D;
    const InstructionOpcode LessEqualOpcode =
        FloatType->scalarBitWidth() == 32 ? InstructionOpcode::FLE_S : InstructionOpcode::FLE_D;

    auto ErasePredicate = Instruction.eraseOperand(1);
    if (!ErasePredicate)
      return std::unexpected(ErasePredicate.error());
    const auto SetBase = [&Instruction](InstructionOpcode OpcodeValue, Register Def,
                                        Register NewLeft, Register NewRight) -> Expected<void> {
      auto SetDestination = Instruction.operandAt(0)->setRegister(Def);
      if (!SetDestination)
        return std::unexpected(SetDestination.error());
      auto SetLeft = Instruction.operandAt(1)->setRegister(NewLeft);
      if (!SetLeft)
        return std::unexpected(SetLeft.error());
      auto SetRight = Instruction.operandAt(2)->setRegister(NewRight);
      if (!SetRight)
        return std::unexpected(SetRight.error());
      return Instruction.setOpcode(instructionOpcode(OpcodeValue));
    };
    const auto BuildInverted = [&](InstructionOpcode BaseOpcode, Register BaseLeft,
                                   Register BaseRight) -> Expected<void> {
      auto Temporary = createTemporary(Function, *ResultType, Target);
      if (!Temporary)
        return std::unexpected(Temporary.error());
      auto Base = SetBase(BaseOpcode, *Temporary, BaseLeft, BaseRight);
      if (!Base)
        return std::unexpected(Base.error());
      MachineIRBuilder Builder(Function, Target);
      auto Invert = buildImmediateAfter(Builder, Instruction, InstructionOpcode::XORI,
                                        DestinationRegister, *Temporary, 1);
      if (!Invert)
        return std::unexpected(Invert.error());
      return {};
    };
    const auto BuildSelfAnd = [&](bool Invert) -> Expected<void> {
      auto First = createTemporary(Function, *ResultType, Target);
      auto Second = createTemporary(Function, *ResultType, Target);
      if (!First || !Second)
        return std::unexpected((!First ? First.error() : Second.error()));
      auto Base = SetBase(EqualOpcode, *First, LeftRegister, LeftRegister);
      if (!Base)
        return std::unexpected(Base.error());
      MachineIRBuilder Builder(Function, Target);
      auto SecondInstruction = buildBinaryAfter(Builder, Instruction, EqualOpcode, *Second,
                                                RightRegister, RightRegister);
      if (!SecondInstruction)
        return std::unexpected(SecondInstruction.error());
      Register CombinedDestination = DestinationRegister;
      if (Invert) {
        auto Temporary = createTemporary(Function, *ResultType, Target);
        if (!Temporary)
          return std::unexpected(Temporary.error());
        CombinedDestination = *Temporary;
      }
      auto Combined = buildBinaryAfter(Builder, **SecondInstruction, InstructionOpcode::AND,
                                       CombinedDestination, *First, *Second);
      if (!Combined)
        return std::unexpected(Combined.error());
      if (!Invert)
        return {};
      auto Inverted = buildImmediateAfter(Builder, **Combined, InstructionOpcode::XORI,
                                          DestinationRegister, CombinedDestination, 1);
      if (!Inverted)
        return std::unexpected(Inverted.error());
      return {};
    };
    const auto BuildOrderedNotEqual = [&](bool Invert) -> Expected<void> {
      auto First = createTemporary(Function, *ResultType, Target);
      auto Second = createTemporary(Function, *ResultType, Target);
      if (!First || !Second)
        return std::unexpected((!First ? First.error() : Second.error()));
      auto Base = SetBase(LessOpcode, *First, LeftRegister, RightRegister);
      if (!Base)
        return std::unexpected(Base.error());
      MachineIRBuilder Builder(Function, Target);
      auto SecondInstruction =
          buildBinaryAfter(Builder, Instruction, LessOpcode, *Second, RightRegister, LeftRegister);
      if (!SecondInstruction)
        return std::unexpected(SecondInstruction.error());
      Register CombinedDestination = DestinationRegister;
      if (Invert) {
        auto Temporary = createTemporary(Function, *ResultType, Target);
        if (!Temporary)
          return std::unexpected(Temporary.error());
        CombinedDestination = *Temporary;
      }
      auto Combined = buildBinaryAfter(Builder, **SecondInstruction, InstructionOpcode::OR,
                                       CombinedDestination, *First, *Second);
      if (!Combined)
        return std::unexpected(Combined.error());
      if (!Invert)
        return {};
      auto Inverted = buildImmediateAfter(Builder, **Combined, InstructionOpcode::XORI,
                                          DestinationRegister, CombinedDestination, 1);
      if (!Inverted)
        return std::unexpected(Inverted.error());
      return {};
    };

    switch (Condition) {
    case FloatingPredicate::AlwaysFalse:
    case FloatingPredicate::AlwaysTrue: {
      auto EraseRight = Instruction.eraseOperand(2);
      if (!EraseRight)
        return std::unexpected(EraseRight.error());
      auto EraseLeft = Instruction.eraseOperand(1);
      if (!EraseLeft)
        return std::unexpected(EraseLeft.error());
      auto Zero = physicalRegister(PhysicalRegisterID::X0);
      if (!Zero)
        return std::unexpected(Zero.error());
      auto InsertZero = insertPhysicalRegister(Instruction, 1, *Zero, RegisterOperandFlag::Use);
      if (!InsertZero)
        return std::unexpected(InsertZero.error());
      auto Immediate =
          appendImmediate(Instruction, Condition == FloatingPredicate::AlwaysTrue ? 1 : 0);
      if (!Immediate)
        return std::unexpected(Immediate.error());
      return Instruction.setOpcode(instructionOpcode(InstructionOpcode::ADDI));
    }
    case FloatingPredicate::OrderedEqual:
      return SetBase(EqualOpcode, DestinationRegister, LeftRegister, RightRegister);
    case FloatingPredicate::OrderedGreaterThan:
      return SetBase(LessOpcode, DestinationRegister, RightRegister, LeftRegister);
    case FloatingPredicate::OrderedGreaterEqual:
      return SetBase(LessEqualOpcode, DestinationRegister, RightRegister, LeftRegister);
    case FloatingPredicate::OrderedLessThan:
      return SetBase(LessOpcode, DestinationRegister, LeftRegister, RightRegister);
    case FloatingPredicate::OrderedLessEqual:
      return SetBase(LessEqualOpcode, DestinationRegister, LeftRegister, RightRegister);
    case FloatingPredicate::OrderedNotEqual:
      return BuildOrderedNotEqual(false);
    case FloatingPredicate::Ordered:
      return BuildSelfAnd(false);
    case FloatingPredicate::Unordered:
      return BuildSelfAnd(true);
    case FloatingPredicate::UnorderedEqual:
      return BuildOrderedNotEqual(true);
    case FloatingPredicate::UnorderedGreaterThan:
      return BuildInverted(LessEqualOpcode, LeftRegister, RightRegister);
    case FloatingPredicate::UnorderedGreaterEqual:
      return BuildInverted(LessOpcode, LeftRegister, RightRegister);
    case FloatingPredicate::UnorderedLessThan:
      return BuildInverted(LessEqualOpcode, RightRegister, LeftRegister);
    case FloatingPredicate::UnorderedLessEqual:
      return BuildInverted(LessOpcode, RightRegister, LeftRegister);
    case FloatingPredicate::UnorderedNotEqual:
      return BuildInverted(EqualOpcode, LeftRegister, RightRegister);
    case FloatingPredicate::Count:
      return makeUnexpected(ErrorCode::Unsupported,
                            "RISC-V floating comparison predicate is invalid");
    }
    return makeUnexpected(ErrorCode::Unsupported,
                          "RISC-V floating comparison predicate is invalid");
  }

  [[nodiscard]] Expected<void> selectCopySign(MachineInstr &Instruction, MachineFunction &Function,
                                              const TargetDescription &Target,
                                              std::optional<LowLevelType> Type) const {
    const MachineOperand *SignOperand = Instruction.operandAt(2);
    const Register *SignRegister =
        SignOperand == nullptr ? nullptr : SignOperand->getIf<Register>();
    const std::optional<LowLevelType> SignType = operandType(Function, Instruction, 2);
    if (!Type || !SignType || SignRegister == nullptr || !Type->isCategoryNeutralScalar() ||
        !SignType->isCategoryNeutralScalar()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V floating copy-sign has invalid operand types");
    }

    const std::uint32_t ResultWidth = Type->scalarBitWidth();
    const std::uint32_t SignWidth = SignType->scalarBitWidth();
    if ((ResultWidth != 32 && ResultWidth != 64) || (SignWidth != 32 && SignWidth != 64)) {
      return makeUnexpected(ErrorCode::Unsupported,
                            "RISC-V cannot select this floating copy-sign type");
    }

    if (ResultWidth != SignWidth) {
      auto Temporary = createTemporary(Function, *Type, Target, fprBank());
      if (!Temporary)
        return std::unexpected(Temporary.error());
      MachineIRBuilder Builder(Function, Target);
      const InstructionOpcode Conversion =
          ResultWidth == 32 ? InstructionOpcode::FCVT_S_D : InstructionOpcode::FCVT_D_S;
      const std::int64_t RoundingMode = ResultWidth == 32 ? 7 : 0;
      auto Built = buildImmediateBefore(Builder, Instruction, Conversion, *Temporary, *SignRegister,
                                        RoundingMode);
      if (!Built)
        return std::unexpected(Built.error());
      auto ReplaceSign = Instruction.operandAt(2)->setRegister(*Temporary);
      if (!ReplaceSign)
        return std::unexpected(ReplaceSign.error());
    }

    return Instruction.setOpcode(instructionOpcode(ResultWidth == 32 ? InstructionOpcode::FSGNJ_S
                                                                     : InstructionOpcode::FSGNJ_D));
  }

  [[nodiscard]] Expected<void> selectFence(MachineInstr &Instruction) const {
    const auto Ordering = signedImmediate(*Instruction.operandAt(0));
    if (!Ordering)
      return std::unexpected(Ordering.error());
    std::int64_t Pred = 0;
    std::int64_t Succ = 0;
    switch (*Ordering) {
    case 4:
      Pred = 1;
      Succ = 3;
      break;
    case 5:
      Pred = 3;
      Succ = 2;
      break;
    case 6:
    case 7:
      Pred = 3;
      Succ = 3;
      break;
    default:
      return makeUnexpected(ErrorCode::Unsupported, "RISC-V fence ordering is unsupported");
    }
    auto SetPred = replaceImmediate(Instruction, 0, Pred);
    if (!SetPred)
      return std::unexpected(SetPred.error());
    auto SetSucc = replaceImmediate(Instruction, 1, Succ);
    if (!SetSucc)
      return std::unexpected(SetSucc.error());
    return Instruction.setOpcode(instructionOpcode(InstructionOpcode::FENCE));
  }

  [[nodiscard]] Expected<void> selectIndirectBranch(MachineInstr &Instruction) const {
    auto Zero = physicalRegister(PhysicalRegisterID::X0);
    if (!Zero)
      return std::unexpected(Zero.error());
    auto Inserted = insertPhysicalRegister(Instruction, 0, *Zero, RegisterOperandFlag::Define);
    if (!Inserted)
      return std::unexpected(Inserted.error());
    auto Immediate = appendImmediate(Instruction, 0);
    if (!Immediate)
      return std::unexpected(Immediate.error());
    return Instruction.setOpcode(instructionOpcode(InstructionOpcode::JALR));
  }

  [[nodiscard]] Expected<void> selectTrap(MachineInstr &Instruction) const {
    while (Instruction.operandCount() != 0) {
      auto Erased = Instruction.eraseOperand(Instruction.operandCount() - 1U);
      if (!Erased)
        return std::unexpected(Erased.error());
    }
    return Instruction.setOpcode(instructionOpcode(InstructionOpcode::EBREAK));
  }

  [[nodiscard]] Expected<void> selectIntegerBinary(MachineInstr &Instruction,
                                                   std::optional<LowLevelType> Type,
                                                   InstructionOpcode DoubleWord,
                                                   InstructionOpcode Word) const {
    if (!Type || !isSupportedIntegerType(*Type))
      return makeUnexpected(ErrorCode::Unsupported,
                            "RISC-V integer instruction requires a supported scalar integer");
    const bool IsWord = Type->isCategoryNeutralScalar() && Type->scalarBitWidth() <= 32;
    return Instruction.setOpcode(instructionOpcode(IsWord ? Word : DoubleWord));
  }

  [[nodiscard]] Expected<bool> foldRightConstantToImmediate(MachineInstr &Instruction,
                                                            const MachineFunction &Function,
                                                            bool Signed,
                                                            std::uint32_t BitWidth) const {
    const MachineOperand *RightOperand = Instruction.operandAt(2);
    const Register *Right = RightOperand == nullptr ? nullptr : RightOperand->getIf<Register>();
    if (Right == nullptr)
      return false;
    const std::optional<std::int64_t> Value =
        directIntegerConstant(Function.registerInfo(), *Right);
    if (!Value || !fitsIntegerImmediate(*Value, Signed, BitWidth))
      return false;
    auto EraseRight = Instruction.eraseOperand(2);
    if (!EraseRight)
      return std::unexpected(EraseRight.error());
    auto AddImmediate = Instruction.addOperand(MachineOperand::immediate(*Value));
    if (!AddImmediate)
      return std::unexpected(AddImmediate.error());
    return true;
  }

  [[nodiscard]] Expected<bool> selectRV64WordShift(MachineInstr &Instruction,
                                                   MachineFunction &Function,
                                                   std::optional<LowLevelType> Type,
                                                   GenericOpcode Generic) const {
    if (!Type || !Type->isCategoryNeutralScalar() || Type->scalarBitWidth() != 64)
      return false;
    const MachineOperand *LeftOperand = Instruction.operandAt(1);
    const MachineOperand *RightOperand = Instruction.operandAt(2);
    const Register *Left = LeftOperand == nullptr ? nullptr : LeftOperand->getIf<Register>();
    const Register *Right = RightOperand == nullptr ? nullptr : RightOperand->getIf<Register>();
    if (Left == nullptr || Right == nullptr || !Right->isVirtual())
      return false;
    const std::optional<std::int64_t> ShiftAmount =
        directIntegerConstant(Function.registerInfo(), *Right);
    if (!ShiftAmount || !fitsIntegerImmediate(*ShiftAmount, false, 5))
      return false;

    const MachineInstr *LeftDefinition = Function.registerInfo().getVRegDef(*Left);
    if (LeftDefinition == nullptr)
      return false;
    const MachineOperand *SourceOperand = LeftDefinition->operandAt(1);
    const Register *Source = SourceOperand == nullptr ? nullptr : SourceOperand->getIf<Register>();
    if (Source == nullptr)
      return false;

    InstructionOpcode Selected = InstructionOpcode::Invalid;
    if (Generic == GenericOpcode::G_LSHR &&
        LeftDefinition->opcode() == opcode(GenericOpcode::G_AND)) {
      const MachineOperand *MaskOperand = LeftDefinition->operandAt(2);
      const Register *Mask = MaskOperand == nullptr ? nullptr : MaskOperand->getIf<Register>();
      const std::optional<std::int64_t> MaskValue =
          Mask == nullptr ? std::nullopt : directIntegerConstant(Function.registerInfo(), *Mask);
      if (MaskValue && *MaskValue == std::int64_t{0xffffffff})
        Selected = InstructionOpcode::SRLIW;
    } else if (Generic == GenericOpcode::G_ASHR &&
               LeftDefinition->opcode() == opcode(GenericOpcode::G_SEXT_INREG)) {
      const MachineOperand *WidthOperand = LeftDefinition->operandAt(2);
      const auto *Width = WidthOperand == nullptr ? nullptr : WidthOperand->getIf<std::int64_t>();
      if (Width != nullptr && *Width == 32)
        Selected = InstructionOpcode::SRAIW;
    }
    if (Selected == InstructionOpcode::Invalid)
      return false;

    auto SetLeft = Instruction.operandAt(1)->setRegister(*Source);
    if (!SetLeft)
      return std::unexpected(SetLeft.error());
    auto EraseRight = Instruction.eraseOperand(2);
    if (!EraseRight)
      return std::unexpected(EraseRight.error());
    auto AddImmediate = Instruction.addOperand(MachineOperand::immediate(*ShiftAmount));
    if (!AddImmediate)
      return std::unexpected(AddImmediate.error());
    auto SetOpcode = Instruction.setOpcode(instructionOpcode(Selected));
    if (!SetOpcode)
      return std::unexpected(SetOpcode.error());
    return true;
  }

  [[nodiscard]] Expected<void>
  selectIntegerBinary(MachineInstr &Instruction, MachineFunction &Function,
                      std::optional<LowLevelType> Type, InstructionOpcode DoubleWord,
                      InstructionOpcode Word, InstructionOpcode DoubleWordImmediate,
                      InstructionOpcode WordImmediate, bool SignedImmediate,
                      std::uint32_t DoubleWordBits, std::uint32_t WordBits) const {
    if (!Type || !isSupportedIntegerType(*Type))
      return makeUnexpected(ErrorCode::Unsupported,
                            "RISC-V integer instruction requires a supported scalar integer");
    const bool IsWord = Type->isCategoryNeutralScalar() && Type->scalarBitWidth() <= 32;
    const auto Folded = foldRightConstantToImmediate(Instruction, Function, SignedImmediate,
                                                     IsWord ? WordBits : DoubleWordBits);
    if (!Folded)
      return std::unexpected(Folded.error());
    if (*Folded)
      return Instruction.setOpcode(instructionOpcode(IsWord ? WordImmediate : DoubleWordImmediate));
    return Instruction.setOpcode(instructionOpcode(IsWord ? Word : DoubleWord));
  }

  [[nodiscard]] Expected<void> selectNormalizedIntegerBinary(
      MachineInstr &Instruction, MachineFunction &Function, const TargetDescription &Target,
      std::optional<LowLevelType> Type, InstructionOpcode DoubleWord, InstructionOpcode Word,
      bool Signed, bool NormalizeRight,
      std::optional<InstructionOpcode> DoubleWordImmediate = std::nullopt,
      std::optional<InstructionOpcode> WordImmediate = std::nullopt, bool ImmediateSigned = false,
      std::uint32_t DoubleWordImmediateBits = 0, std::uint32_t WordImmediateBits = 0) const {
    const MachineOperand *LeftOperand = Instruction.operandAt(1);
    const MachineOperand *RightOperand = Instruction.operandAt(2);
    const Register *Left = LeftOperand == nullptr ? nullptr : LeftOperand->getIf<Register>();
    const Register *Right = RightOperand == nullptr ? nullptr : RightOperand->getIf<Register>();
    if (!Type || !isSupportedIntegerScalar(*Type) || Left == nullptr || Right == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V normalized integer operation has invalid operands");
    }
    auto NormalizedLeft =
        normalizeIntegerBefore(Instruction, Function, Target, *Left, *Type, Signed);
    if (!NormalizedLeft)
      return std::unexpected(NormalizedLeft.error());
    const Register RightRegister = *Right;
    bool FoldedImmediate = false;
    if (DoubleWordImmediate && WordImmediate) {
      auto Folded = foldRightConstantToImmediate(
          Instruction, Function, ImmediateSigned,
          Type->scalarBitWidth() <= 32 ? WordImmediateBits : DoubleWordImmediateBits);
      if (!Folded)
        return std::unexpected(Folded.error());
      FoldedImmediate = *Folded;
    }
    Register SelectedRight = RightRegister;
    if (!FoldedImmediate && NormalizeRight) {
      auto NormalizedRight =
          normalizeIntegerBefore(Instruction, Function, Target, RightRegister, *Type, Signed);
      if (!NormalizedRight)
        return std::unexpected(NormalizedRight.error());
      SelectedRight = *NormalizedRight;
    }
    auto SetLeft = Instruction.operandAt(1)->setRegister(*NormalizedLeft);
    if (!SetLeft)
      return std::unexpected(SetLeft.error());
    if (!FoldedImmediate) {
      auto SetRight = Instruction.operandAt(2)->setRegister(SelectedRight);
      if (!SetRight)
        return std::unexpected(SetRight.error());
    }
    const bool IsWord = Type->scalarBitWidth() <= 32;
    if (FoldedImmediate)
      return Instruction.setOpcode(
          instructionOpcode(IsWord ? *WordImmediate : *DoubleWordImmediate));
    return Instruction.setOpcode(instructionOpcode(IsWord ? Word : DoubleWord));
  }

  [[nodiscard]] Expected<void> selectLoad(MachineInstr &Instruction, MachineFunction &Function,
                                          std::optional<LowLevelType> Type) const {
    if (!Type)
      return makeUnexpected(ErrorCode::VerificationError,
                            "load result has no virtual-register type");
    const auto Bytes = memoryAccessBytes(Instruction);
    if (!Bytes)
      return makeUnexpected(ErrorCode::Unsupported,
                            "RISC-V load size does not match its result type");
    const MachineOperand *DestinationOperand = Instruction.operandAt(0);
    const Register *Destination =
        DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
    if (Destination == nullptr)
      return makeUnexpected(ErrorCode::VerificationError, "RISC-V load has no destination");
    auto Bank = registerBank(Function, *Destination);
    if (!Bank)
      return std::unexpected(Bank.error());
    if (Instruction.memOperands().front()->isAtomic()) {
      if (*Bank == fprBank() || !isSupportedIntegerType(*Type) || (*Bytes != 4 && *Bytes != 8))
        return makeUnexpected(ErrorCode::Unsupported,
                              "RISC-V atomic load requires a 32-bit or 64-bit GPR value");
      return Instruction.setOpcode(
          instructionOpcode(*Bytes == 4 ? InstructionOpcode::LR_W : InstructionOpcode::LR_D));
    }
    auto AppendOffset = appendImmediate(Instruction, 0);
    if (!AppendOffset)
      return std::unexpected(AppendOffset.error());
    if (*Bank != fprBank() && isSupportedIntegerType(*Type)) {
      const InstructionOpcode Selected = *Bytes == 1   ? InstructionOpcode::LB
                                         : *Bytes == 2 ? InstructionOpcode::LH
                                         : *Bytes == 4 ? InstructionOpcode::LW
                                         : *Bytes == 8 ? InstructionOpcode::LD
                                                       : InstructionOpcode::Invalid;
      if (Selected == InstructionOpcode::Invalid)
        return makeUnexpected(ErrorCode::Unsupported, "RISC-V integer load width is unsupported");
      return Instruction.setOpcode(instructionOpcode(Selected));
    }
    if (*Bank == fprBank() && Type->scalarBitWidth() == 32 && *Bytes == 4)
      return Instruction.setOpcode(instructionOpcode(InstructionOpcode::FLW));
    if (*Bank == fprBank() && Type->scalarBitWidth() == 64 && *Bytes == 8)
      return Instruction.setOpcode(instructionOpcode(InstructionOpcode::FLD));
    return makeUnexpected(ErrorCode::Unsupported, "RISC-V cannot select this generic load type");
  }

  [[nodiscard]] Expected<void> selectExtLoad(MachineInstr &Instruction,
                                             std::optional<LowLevelType> Type, bool Signed) const {
    if (!Type || !isSupportedIntegerType(*Type))
      return makeUnexpected(ErrorCode::Unsupported,
                            "RISC-V extension load requires an integer result");
    const auto Bytes = memoryAccessBytes(Instruction);
    if (!Bytes)
      return makeUnexpected(ErrorCode::Unsupported, "RISC-V extension load has an invalid width");
    const InstructionOpcode Selected =
        *Bytes == 1   ? (Signed ? InstructionOpcode::LB : InstructionOpcode::LBU)
        : *Bytes == 2 ? (Signed ? InstructionOpcode::LH : InstructionOpcode::LHU)
        : *Bytes == 4 ? (Signed ? InstructionOpcode::LW : InstructionOpcode::LWU)
                      : InstructionOpcode::Invalid;
    if (Selected == InstructionOpcode::Invalid)
      return makeUnexpected(ErrorCode::Unsupported, "RISC-V extension load width is unsupported");
    auto Immediate = appendImmediate(Instruction, 0);
    if (!Immediate)
      return std::unexpected(Immediate.error());
    return Instruction.setOpcode(instructionOpcode(Selected));
  }

  [[nodiscard]] Expected<void> selectFloatExtLoad(MachineInstr &Instruction,
                                                  MachineFunction &Function,
                                                  const TargetDescription &Target) const {
    const MachineOperand *DestinationOperand = Instruction.operandAt(0);
    const Register *Destination =
        DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
    if (Destination == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V floating extension load has no destination register");
    const Register DestinationRegister = *Destination;
    auto F32 = LowLevelType::makeCategoryNeutralScalar(32);
    if (!F32)
      return std::unexpected(F32.error());
    auto Temporary = createTemporary(Function, *F32, Target, fprBank());
    if (!Temporary)
      return std::unexpected(Temporary.error());
    auto SetDestination = Instruction.operandAt(0)->setRegister(*Temporary);
    if (!SetDestination)
      return std::unexpected(SetDestination.error());
    auto Immediate = appendImmediate(Instruction, 0);
    if (!Immediate)
      return std::unexpected(Immediate.error());
    auto SetOpcode = Instruction.setOpcode(instructionOpcode(InstructionOpcode::FLW));
    if (!SetOpcode)
      return std::unexpected(SetOpcode.error());
    MachineIRBuilder Builder(Function, Target);
    auto Conversion = buildImmediateAfter(Builder, Instruction, InstructionOpcode::FCVT_D_S,
                                          DestinationRegister, *Temporary, 0);
    if (!Conversion)
      return std::unexpected(Conversion.error());
    return {};
  }

  [[nodiscard]] Expected<void> selectFloatTruncStore(MachineInstr &Instruction,
                                                     MachineFunction &Function,
                                                     const TargetDescription &Target) const {
    const MachineOperand *ValueOperand = Instruction.operandAt(0);
    const Register *Value = ValueOperand == nullptr ? nullptr : ValueOperand->getIf<Register>();
    if (Value == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V floating truncation store has no value register");
    const Register ValueRegister = *Value;
    auto F32 = LowLevelType::makeCategoryNeutralScalar(32);
    if (!F32)
      return std::unexpected(F32.error());
    auto Temporary = createTemporary(Function, *F32, Target, fprBank());
    if (!Temporary)
      return std::unexpected(Temporary.error());
    MachineIRBuilder Builder(Function, Target);
    auto Point = Builder.setInsertPointBefore(Instruction);
    if (!Point)
      return std::unexpected(Point.error());
    auto RegisterConstraints =
        constrainBuilderRegisters(Function, Target, {*Temporary, ValueRegister});
    if (!RegisterConstraints)
      return std::unexpected(RegisterConstraints.error());
    auto Conversion = Builder.buildInstr(instructionOpcode(InstructionOpcode::FCVT_S_D));
    if (!Conversion)
      return std::unexpected(Conversion.error());
    auto Def = Conversion->addDef(*Temporary);
    if (!Def)
      return std::unexpected(Def.error());
    auto Use = Conversion->addUse(ValueRegister);
    if (!Use)
      return std::unexpected(Use.error());
    auto Rounding = Conversion->addImmediate(0);
    if (!Rounding)
      return std::unexpected(Rounding.error());
    auto Built = Conversion->build();
    if (!Built)
      return std::unexpected(Built.error());
    auto Constraint = constrainSelectedInstruction(**Built, Function, Target);
    if (!Constraint)
      return std::unexpected(Constraint.error());
    auto SetValue = Instruction.operandAt(0)->setRegister(*Temporary);
    if (!SetValue)
      return std::unexpected(SetValue.error());
    auto Immediate = appendImmediate(Instruction, 0);
    if (!Immediate)
      return std::unexpected(Immediate.error());
    return Instruction.setOpcode(instructionOpcode(InstructionOpcode::FSW));
  }

  [[nodiscard]] Expected<void> selectStore(MachineInstr &Instruction,
                                           MachineFunction &Function) const {
    const MachineOperand *Value = Instruction.operandAt(0);
    const std::optional<LowLevelType> Type =
        Value == nullptr ? std::nullopt : registerType(Function, *Value);
    if (!Type)
      return makeUnexpected(ErrorCode::VerificationError,
                            "store value has no virtual-register type");
    const auto Bytes = memoryAccessBytes(Instruction);
    if (!Bytes)
      return makeUnexpected(ErrorCode::Unsupported,
                            "RISC-V store size does not match its value type");
    const Register *ValueRegister = Value->getIf<Register>();
    if (ValueRegister == nullptr)
      return makeUnexpected(ErrorCode::VerificationError, "RISC-V store has no value register");
    auto Bank = registerBank(Function, *ValueRegister);
    if (!Bank)
      return std::unexpected(Bank.error());
    if (Instruction.memOperands().front()->isAtomic()) {
      if (*Bank == fprBank() || !isSupportedIntegerType(*Type) || (*Bytes != 4 && *Bytes != 8))
        return makeUnexpected(ErrorCode::Unsupported,
                              "RISC-V atomic store requires a 32-bit or 64-bit GPR value");
      auto Zero = physicalRegister(PhysicalRegisterID::X0);
      if (!Zero)
        return std::unexpected(Zero.error());
      auto Destination = insertPhysicalRegister(Instruction, 0, *Zero, RegisterOperandFlag::Define);
      if (!Destination)
        return std::unexpected(Destination.error());
      return Instruction.setOpcode(instructionOpcode(*Bytes == 4 ? InstructionOpcode::AMOSWAP_W
                                                                 : InstructionOpcode::AMOSWAP_D));
    }
    auto AppendOffset = appendImmediate(Instruction, 0);
    if (!AppendOffset)
      return std::unexpected(AppendOffset.error());
    if (*Bank != fprBank() && isSupportedIntegerType(*Type)) {
      const InstructionOpcode Selected = *Bytes == 1   ? InstructionOpcode::SB
                                         : *Bytes == 2 ? InstructionOpcode::SH
                                         : *Bytes == 4 ? InstructionOpcode::SW
                                         : *Bytes == 8 ? InstructionOpcode::SD
                                                       : InstructionOpcode::Invalid;
      if (Selected == InstructionOpcode::Invalid)
        return makeUnexpected(ErrorCode::Unsupported, "RISC-V integer store width is unsupported");
      return Instruction.setOpcode(instructionOpcode(Selected));
    }
    if (*Bank == fprBank() && Type->scalarBitWidth() == 32 && *Bytes == 4)
      return Instruction.setOpcode(instructionOpcode(InstructionOpcode::FSW));
    if (*Bank == fprBank() && Type->scalarBitWidth() == 64 && *Bytes == 8)
      return Instruction.setOpcode(instructionOpcode(InstructionOpcode::FSD));
    return makeUnexpected(ErrorCode::Unsupported, "RISC-V cannot select this generic store type");
  }

  [[nodiscard]] Expected<void> selectBranch(MachineInstr &Instruction) const {
    auto Zero = physicalRegister(PhysicalRegisterID::X0);
    if (!Zero)
      return std::unexpected(Zero.error());
    auto Inserted = insertPhysicalRegister(Instruction, 0, *Zero, RegisterOperandFlag::Define);
    if (!Inserted)
      return std::unexpected(Inserted.error());
    return Instruction.setOpcode(instructionOpcode(InstructionOpcode::JAL));
  }

  [[nodiscard]] Expected<void> selectConditionalBranch(MachineInstr &Instruction) const {
    auto Zero = physicalRegister(PhysicalRegisterID::X0);
    if (!Zero)
      return std::unexpected(Zero.error());
    auto Inserted = insertPhysicalRegister(Instruction, 1, *Zero, RegisterOperandFlag::Use);
    if (!Inserted)
      return std::unexpected(Inserted.error());
    return Instruction.setOpcode(instructionOpcode(InstructionOpcode::BNE));
  }

  [[nodiscard]] Expected<void> selectIntegerConstant(MachineInstr &Instruction,
                                                     MachineFunction &Function,
                                                     const TargetDescription &Target,
                                                     std::optional<LowLevelType> Type) const {
    if (!Type || !isSupportedIntegerType(*Type)) {
      return makeUnexpected(ErrorCode::Unsupported,
                            "RISC-V constant selection requires an i1-i64 or pointer result");
    }
    const MachineOperand *ImmediateOperand = Instruction.operandAt(1);
    if (ImmediateOperand == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V generic constant has no immediate operand");
    auto ImmediateValue = signedImmediate(*ImmediateOperand);
    if (!ImmediateValue)
      return std::unexpected(ImmediateValue.error());
    const MachineOperand *DestinationOperand = Instruction.operandAt(0);
    const Register *DestinationValue =
        DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
    if (DestinationValue == nullptr || !DestinationValue->isVirtual()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V generic constant must define a virtual register");
    }
    const Register Destination = *DestinationValue;
    auto DestinationConstraint = constrainBuilderRegisters(Function, Target, {Destination});
    if (!DestinationConstraint)
      return std::unexpected(DestinationConstraint.error());
    const std::int64_t Value = *ImmediateValue;
    auto Zero = physicalRegister(PhysicalRegisterID::X0);
    if (!Zero)
      return std::unexpected(Zero.error());
    if (Value == 0) {
      auto EraseImmediate = Instruction.eraseOperand(1);
      if (!EraseImmediate)
        return std::unexpected(EraseImmediate.error());
      auto Source = insertPhysicalRegister(Instruction, 1, *Zero, RegisterOperandFlag::Use);
      if (!Source)
        return std::unexpected(Source.error());
      return Instruction.setOpcode(opcode(GenericOpcode::COPY));
    }

    ImmediateMaterializationSequence Sequence =
        generateRV64IntegerMaterialization(Value, Subtarget.features());
    if (Sequence.empty()) {
      return makeUnexpected(ErrorCode::InternalError,
                            "RISC-V integer materialization produced an empty sequence");
    }
    Register Current = Destination;
    if (Sequence.size() != 1) {
      auto Temporary = createTemporary(Function, *Type, Target);
      if (!Temporary)
        return std::unexpected(Temporary.error());
      Current = *Temporary;
    }

    const auto SelectHead = [&](const ImmediateMaterializationInstruction &Step,
                                Register Definition) -> Expected<void> {
      auto SetOpcode = Instruction.setOpcode(instructionOpcode(Step.Opcode));
      if (!SetOpcode)
        return std::unexpected(SetOpcode.error());
      auto SetDestination = Instruction.operandAt(0)->setRegister(Definition);
      if (!SetDestination)
        return std::unexpected(SetDestination.error());
      auto EraseImmediate = Instruction.eraseOperand(1);
      if (!EraseImmediate)
        return std::unexpected(EraseImmediate.error());
      if (Step.OperandKind == ImmediateMaterializationOperandKind::Immediate)
        return Instruction.addOperand(MachineOperand::immediate(Step.Immediate));
      auto Source = insertPhysicalRegister(Instruction, 1, *Zero, RegisterOperandFlag::Use);
      if (!Source)
        return std::unexpected(Source.error());
      return Instruction.addOperand(MachineOperand::immediate(Step.Immediate));
    };
    auto Head = SelectHead(Sequence.front(), Current);
    if (!Head)
      return std::unexpected(Head.error());

    MachineIRBuilder Builder(Function, Target);
    MachineInstr *Anchor = &Instruction;
    for (std::size_t Index = 1; Index < Sequence.size(); ++Index) {
      const bool IsFinal = Index + 1U == Sequence.size();
      Register Next = Destination;
      if (!IsFinal) {
        auto Temporary = createTemporary(Function, *Type, Target);
        if (!Temporary)
          return std::unexpected(Temporary.error());
        Next = *Temporary;
      }
      const ImmediateMaterializationInstruction &Step = Sequence[Index];
      Expected<MachineInstr *> Built =
          Step.OperandKind == ImmediateMaterializationOperandKind::Immediate
              ? buildImmediateOnlyAfter(Builder, *Anchor, Step.Opcode, Next, Step.Immediate)
              : buildImmediateAfter(Builder, *Anchor, Step.Opcode, Next, Current, Step.Immediate);
      if (!Built)
        return std::unexpected(Built.error());
      Anchor = *Built;
      Current = Next;
    }
    return {};
  }

  [[nodiscard]] Expected<void> selectFloatingConstant(MachineInstr &Instruction,
                                                      MachineFunction &Function,
                                                      const TargetDescription &Target,
                                                      std::optional<LowLevelType> Type) const {
    if (!Type || !isSupportedFloatingScalar(*Type))
      return makeUnexpected(ErrorCode::Unsupported, "RISC-V floating constant requires f32 or f64");
    const MachineOperand *Immediate = Instruction.operandAt(1);
    const auto *Floating =
        Immediate == nullptr ? nullptr : Immediate->getIf<FloatingImmediateOperand>();
    if (Floating == nullptr || Floating->Bits.bitWidth() != Type->scalarBitWidth())
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V floating constant has mismatched bit semantics");
    const MachineOperand *DestinationOperand = Instruction.operandAt(0);
    const Register *Destination =
        DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
    if (Destination == nullptr || !Destination->isVirtual())
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V floating constant must define a virtual register");
    const Register DestinationRegister = *Destination;

    const std::uint32_t BitWidth = Type->scalarBitWidth();
    const std::uint64_t Bits = Floating->Bits.words().empty() ? 0 : Floating->Bits.words().front();
    auto Integer = APInt::fromUnsigned(64, Bits);
    if (!Integer)
      return std::unexpected(Integer.error());
    auto IntegerType = LowLevelType::makeCategoryNeutralScalar(64);
    if (!IntegerType)
      return std::unexpected(IntegerType.error());
    auto Temporary = createTemporary(Function, *IntegerType, Target);
    if (!Temporary)
      return std::unexpected(Temporary.error());

    auto SetOpcode = Instruction.setOpcode(opcode(GenericOpcode::G_CONSTANT));
    if (!SetOpcode)
      return std::unexpected(SetOpcode.error());
    auto SetDestination = Instruction.operandAt(0)->setRegister(*Temporary);
    if (!SetDestination)
      return std::unexpected(SetDestination.error());
    auto EraseImmediate = Instruction.eraseOperand(1);
    if (!EraseImmediate)
      return std::unexpected(EraseImmediate.error());
    auto AddInteger = Instruction.addOperand(MachineOperand::integerImmediate(*Integer));
    if (!AddInteger)
      return std::unexpected(AddInteger.error());
    auto IntegerSelection = selectIntegerConstant(Instruction, Function, Target, *IntegerType);
    if (!IntegerSelection)
      return std::unexpected(IntegerSelection.error());

    MachineInstr *IntegerDefinition = nullptr;
    for (MachineOperand &DefOperand : Function.registerInfo().defOperands(*Temporary)) {
      IntegerDefinition = DefOperand.parent();
      break;
    }
    if (IntegerDefinition == nullptr)
      return makeUnexpected(ErrorCode::InternalError,
                            "RISC-V floating constant lost its integer materialization");
    MachineIRBuilder Builder(Function, Target);
    auto InsertPoint = Builder.setInsertPointAfter(*IntegerDefinition);
    if (!InsertPoint)
      return std::unexpected(InsertPoint.error());
    auto RegisterConstraints =
        constrainBuilderRegisters(Function, Target, {DestinationRegister, *Temporary});
    if (!RegisterConstraints)
      return std::unexpected(RegisterConstraints.error());
    const InstructionOpcode Move =
        BitWidth == 32 ? InstructionOpcode::FMV_W_X : InstructionOpcode::FMV_D_X;
    auto MoveInstruction = Builder.buildInstr(instructionOpcode(Move));
    if (!MoveInstruction)
      return std::unexpected(MoveInstruction.error());
    auto MoveDef = MoveInstruction->addDef(DestinationRegister);
    if (!MoveDef)
      return std::unexpected(MoveDef.error());
    auto MoveUse = MoveInstruction->addUse(*Temporary);
    if (!MoveUse)
      return std::unexpected(MoveUse.error());
    auto BuiltMove = MoveInstruction->build();
    if (!BuiltMove)
      return std::unexpected(BuiltMove.error());
    auto Constraint = constrainSelectedInstruction(**BuiltMove, Function, Target);
    if (!Constraint)
      return std::unexpected(Constraint.error());
    return {};
  }

  const RISCVSubtarget &Subtarget;
  const RISCVRegisterBankInfo &RegisterBanks;
};

} // namespace

InstructionSelector *createRISCVInstructionSelector(const RISCVSubtarget &Subtarget,
                                                    const RISCVRegisterBankInfo &RegisterBanks) {
  return new RISCVInstructionSelector(Subtarget, RegisterBanks);
}

} // namespace rcc::riscv
