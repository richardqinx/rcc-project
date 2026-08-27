// SPDX-License-Identifier: GPL-3.0-or-later

#include "RISCV.h"
#include "RISCVImmediateMaterialization.h"
#include "RISCVInstrInfo.h"
#include "RISCVRegisterBankInfo.h"
#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/GlobalISel/GISelValueTracking.h"
#include "rcc/CodeGen/GlobalISel/InstructionSelector.h"
#include "rcc/CodeGen/GlobalObject.h"
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

constexpr std::uint32_t AbsoluteHighFlag = 32;
constexpr std::uint32_t AbsoluteLowFlag = 64;

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

[[nodiscard]] Expected<InstructionOpcode> floatingCompareOpcode(FloatingPredicate Predicate,
                                                                std::uint32_t BitWidth) {
  if (BitWidth == 32) {
    switch (Predicate) {
    case FloatingPredicate::OrderedEqual:
      return InstructionOpcode::FEQ_S;
    case FloatingPredicate::OrderedLessThan:
      return InstructionOpcode::FLT_S;
    case FloatingPredicate::OrderedLessEqual:
      return InstructionOpcode::FLE_S;
    default:
      break;
    }
  } else if (BitWidth == 64) {
    switch (Predicate) {
    case FloatingPredicate::OrderedEqual:
      return InstructionOpcode::FEQ_D;
    case FloatingPredicate::OrderedLessThan:
      return InstructionOpcode::FLT_D;
    case FloatingPredicate::OrderedLessEqual:
      return InstructionOpcode::FLE_D;
    default:
      break;
    }
  }
  return makeUnexpected(ErrorCode::Unsupported,
                        "RISC-V floating comparison predicate is not directly supported");
}

// Convert a floating predicate by swapping or inverting it to one supported by
// a scalar RISC-V comparison instruction.
[[nodiscard]] bool legalizeFloatingPredicate(Register &Left, Register &Right,
                                             FloatingPredicate &Predicate,
                                             bool &NeedsInvert) noexcept {
  switch (Predicate) {
  case FloatingPredicate::OrderedGreaterThan:
    Predicate = FloatingPredicate::OrderedLessThan;
    std::swap(Left, Right);
    return true;
  case FloatingPredicate::OrderedGreaterEqual:
    Predicate = FloatingPredicate::OrderedLessEqual;
    std::swap(Left, Right);
    return true;
  case FloatingPredicate::UnorderedNotEqual:
    Predicate = FloatingPredicate::OrderedEqual;
    NeedsInvert = true;
    return true;
  case FloatingPredicate::UnorderedGreaterThan:
    Predicate = FloatingPredicate::OrderedLessEqual;
    NeedsInvert = true;
    return true;
  case FloatingPredicate::UnorderedGreaterEqual:
    Predicate = FloatingPredicate::OrderedLessThan;
    NeedsInvert = true;
    return true;
  case FloatingPredicate::UnorderedLessThan:
    Predicate = FloatingPredicate::OrderedLessEqual;
    NeedsInvert = true;
    std::swap(Left, Right);
    return true;
  case FloatingPredicate::UnorderedLessEqual:
    Predicate = FloatingPredicate::OrderedLessThan;
    NeedsInvert = true;
    std::swap(Left, Right);
    return true;
  default:
    return false;
  }
}

[[nodiscard]] bool isSupportedIntegerType(LowLevelType Type) noexcept {
  return (Type.isCategoryNeutralScalar() && Type.scalarBitWidth() != 0 &&
          Type.scalarBitWidth() <= 64) ||
         (Type.isPointer() && Type.scalarBitWidth() == 64);
}

[[nodiscard]] constexpr RegisterBankID fprBank() noexcept {
  return RegisterBankID::fromValue(static_cast<std::uint32_t>(RegisterBank::FPR));
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

struct SelectedAddress final {
  MachineOperand Base;
  std::int64_t Offset = 0;
};

[[nodiscard]] std::optional<SelectedAddress> selectAddress(const MachineInstr &Instruction,
                                                           const MachineFunction &Function,
                                                           std::uint32_t OperandIndex) {
  const MachineOperand *Root = Instruction.operandAt(OperandIndex);
  const Register *RootRegister = Root == nullptr ? nullptr : Root->getIf<Register>();
  if (RootRegister == nullptr || !RootRegister->isVirtual())
    return std::nullopt;

  const MachineRegisterInfo &Registers = Function.registerInfo();
  const MachineInstr *RootDefinition = Registers.getVRegDef(*RootRegister);
  if (RootDefinition != nullptr &&
      RootDefinition->opcode() == opcode(GenericOpcode::G_FRAME_INDEX)) {
    const MachineOperand *FrameIndex = RootDefinition->operandAt(1);
    if (FrameIndex == nullptr)
      return std::nullopt;
    return SelectedAddress{.Base = FrameIndex->cloneDetached(), .Offset = 0};
  }

  if (RootDefinition != nullptr && RootDefinition->opcode() == opcode(GenericOpcode::G_PTR_ADD)) {
    const MachineOperand *Base = RootDefinition->operandAt(1);
    const MachineOperand *Offset = RootDefinition->operandAt(2);
    const Register *BaseRegister = Base == nullptr ? nullptr : Base->getIf<Register>();
    const Register *OffsetRegister = Offset == nullptr ? nullptr : Offset->getIf<Register>();
    if (BaseRegister != nullptr && OffsetRegister != nullptr) {
      const std::optional<std::int64_t> Constant =
          directIntegerConstant(Registers, *OffsetRegister);
      if (Constant && *Constant >= -2048 && *Constant <= 2047) {
        const MachineInstr *BaseDefinition =
            BaseRegister->isVirtual() ? Registers.getVRegDef(*BaseRegister) : nullptr;
        if (BaseDefinition != nullptr &&
            BaseDefinition->opcode() == opcode(GenericOpcode::G_FRAME_INDEX)) {
          const MachineOperand *FrameIndex = BaseDefinition->operandAt(1);
          if (FrameIndex == nullptr)
            return std::nullopt;
          return SelectedAddress{.Base = FrameIndex->cloneDetached(), .Offset = *Constant};
        }
        return SelectedAddress{.Base = Base->cloneDetached(), .Offset = *Constant};
      }
    }
  }

  return SelectedAddress{.Base = Root->cloneDetached(), .Offset = 0};
}

struct SelectedIntegerCondition final {
  Register Left;
  Register Right;
  BranchConditionCode Code = BranchConditionCode::NotEqual;
};

[[nodiscard]] Expected<std::optional<SelectedIntegerCondition>>
selectIntegerCondition(const MachineInstr &Instruction, const MachineFunction &Function,
                       std::uint32_t OperandIndex) {
  const MachineOperand *ConditionOperand = Instruction.operandAt(OperandIndex);
  const Register *Condition =
      ConditionOperand == nullptr ? nullptr : ConditionOperand->getIf<Register>();
  if (Condition == nullptr)
    return std::optional<SelectedIntegerCondition>{};

  auto Zero = physicalRegister(PhysicalRegisterID::X0);
  if (!Zero)
    return std::unexpected(Zero.error());
  SelectedIntegerCondition Result{
      .Left = *Condition,
      .Right = Register::fromPhysical(*Zero),
      .Code = BranchConditionCode::NotEqual,
  };
  const MachineInstr *Comparison =
      Condition->isVirtual() ? Function.registerInfo().getVRegDef(*Condition) : nullptr;
  if (Comparison == nullptr || Comparison->opcode() != opcode(GenericOpcode::G_ICMP))
    return std::optional<SelectedIntegerCondition>{Result};

  const MachineOperand *PredicateMO = Comparison->operandAt(1);
  const auto *Predicate = PredicateMO == nullptr ? nullptr : PredicateMO->getIf<PredicateOperand>();
  const IntegerPredicate *Integer = Predicate == nullptr ? nullptr : Predicate->integerPredicate();
  const MachineOperand *LeftOperand = Comparison->operandAt(2);
  const MachineOperand *RightOperand = Comparison->operandAt(3);
  const Register *Left = LeftOperand == nullptr ? nullptr : LeftOperand->getIf<Register>();
  const Register *Right = RightOperand == nullptr ? nullptr : RightOperand->getIf<Register>();
  if (Integer == nullptr || Left == nullptr || Right == nullptr) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V integer comparison has malformed branch operands");
  }
  Result.Left = *Left;
  Result.Right = *Right;

  if (const std::optional<std::int64_t> Constant =
          directIntegerConstant(Function.registerInfo(), *Right)) {
    if (*Integer == IntegerPredicate::SignedGreaterThan && *Constant == -1) {
      Result.Right = Register::fromPhysical(*Zero);
      Result.Code = BranchConditionCode::SignedGreaterEqual;
      return std::optional<SelectedIntegerCondition>{Result};
    }
    if (*Integer == IntegerPredicate::SignedLessThan && *Constant == 1) {
      Result.Right = Result.Left;
      Result.Left = Register::fromPhysical(*Zero);
      Result.Code = BranchConditionCode::SignedGreaterEqual;
      return std::optional<SelectedIntegerCondition>{Result};
    }
  }

  switch (*Integer) {
  case IntegerPredicate::Equal:
    Result.Code = BranchConditionCode::Equal;
    break;
  case IntegerPredicate::NotEqual:
    Result.Code = BranchConditionCode::NotEqual;
    break;
  case IntegerPredicate::SignedLessThan:
    Result.Code = BranchConditionCode::SignedLessThan;
    break;
  case IntegerPredicate::SignedGreaterEqual:
    Result.Code = BranchConditionCode::SignedGreaterEqual;
    break;
  case IntegerPredicate::UnsignedLessThan:
    Result.Code = BranchConditionCode::UnsignedLessThan;
    break;
  case IntegerPredicate::UnsignedGreaterEqual:
    Result.Code = BranchConditionCode::UnsignedGreaterEqual;
    break;
  case IntegerPredicate::SignedGreaterThan:
    std::swap(Result.Left, Result.Right);
    Result.Code = BranchConditionCode::SignedLessThan;
    break;
  case IntegerPredicate::SignedLessEqual:
    std::swap(Result.Left, Result.Right);
    Result.Code = BranchConditionCode::SignedGreaterEqual;
    break;
  case IntegerPredicate::UnsignedGreaterThan:
    std::swap(Result.Left, Result.Right);
    Result.Code = BranchConditionCode::UnsignedLessThan;
    break;
  case IntegerPredicate::UnsignedLessEqual:
    std::swap(Result.Left, Result.Right);
    Result.Code = BranchConditionCode::UnsignedGreaterEqual;
    break;
  case IntegerPredicate::Count:
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V select has an invalid integer predicate");
  }
  return std::optional<SelectedIntegerCondition>{Result};
}

class RISCVInstructionSelector final : public InstructionSelector {
public:
  RISCVInstructionSelector(const RISCVSubtarget &Subtarget,
                           const RISCVRegisterBankInfo &RegisterBanks)
      : Subtarget(Subtarget), RegisterBanks(RegisterBanks) {}

  [[nodiscard]] Expected<bool> matchesTargetPredicate(std::string_view Name, const MachineInstr &,
                                                      const MachineFunction &,
                                                      const TargetDescription &) const override {
    if (Name == "a")
      return Subtarget.features().has(RISCVExtension::A);
    if (Name == "zba")
      return Subtarget.features().has(RISCVExtension::Zba);
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V generated GlobalISel rule uses unknown target predicate '" +
                              std::string(Name) + "'");
  }

  [[nodiscard]] Expected<void>
  constrainSelectedInstruction(MachineInstr &Instruction, MachineFunction &Function,
                               const TargetDescription &Target) const override {
    return RegisterBanks.constrainSelectedInstruction(Instruction, Function, Target);
  }

  [[nodiscard]] Expected<std::optional<GIMatchComplexOperand>>
  renderComplexOperand(std::string_view Name, const MachineInstr &Instruction,
                       const MachineFunction &Function, const TargetDescription &Target,
                       std::uint32_t OperandIndex) const override {
    if (Name == "addr_reg_imm_base" || Name == "addr_reg_imm_offset") {
      std::optional<SelectedAddress> Address = selectAddress(Instruction, Function, OperandIndex);
      if (!Address)
        return std::optional<GIMatchComplexOperand>{};
      if (Name == "addr_reg_imm_offset") {
        return GIMatchComplexOperand{
            .Operand = MachineOperand::immediate(Address->Offset),
            .Instruction = std::nullopt,
        };
      }
      return GIMatchComplexOperand{
          .Operand = std::move(Address->Base),
          .Instruction = std::nullopt,
      };
    }
    if (Name == "select_condition_lhs" || Name == "select_condition_rhs" ||
        Name == "select_condition_code") {
      auto Condition = selectIntegerCondition(Instruction, Function, OperandIndex);
      if (!Condition)
        return std::unexpected(Condition.error());
      if (!*Condition)
        return std::optional<GIMatchComplexOperand>{};
      if (Name == "select_condition_code") {
        return GIMatchComplexOperand{
            .Operand = MachineOperand::immediate(static_cast<std::int64_t>((*Condition)->Code)),
            .Instruction = std::nullopt,
        };
      }
      const Register Value =
          Name == "select_condition_lhs" ? (*Condition)->Left : (*Condition)->Right;
      auto Operand = MachineOperand::registerOperand(Value, {RegisterOperandFlag::Use});
      if (!Operand)
        return std::unexpected(Operand.error());
      return GIMatchComplexOperand{
          .Operand = std::move(*Operand),
          .Instruction = std::nullopt,
      };
    }
    if (Name == "fcopysign_to_s32" || Name == "fcopysign_to_s64") {
      const MachineOperand *SourceOperand = Instruction.operandAt(OperandIndex);
      const Register *Source =
          SourceOperand == nullptr ? nullptr : SourceOperand->getIf<Register>();
      if (Source == nullptr || !Source->isVirtual())
        return std::optional<GIMatchComplexOperand>{};
      const std::uint32_t DestinationWidth = Name == "fcopysign_to_s32" ? 32U : 64U;
      auto DestinationType = LowLevelType::makeCategoryNeutralScalar(DestinationWidth);
      if (!DestinationType)
        return std::unexpected(DestinationType.error());
      auto SourceUse = MachineOperand::registerOperand(*Source, {RegisterOperandFlag::Use});
      if (!SourceUse)
        return std::unexpected(SourceUse.error());
      std::vector<MachineOperand> Operands;
      Operands.push_back(std::move(*SourceUse));
      Operands.push_back(MachineOperand::immediate(DestinationWidth == 32 ? 7 : 0));
      return GIMatchComplexOperand{
          .Operand = std::nullopt,
          .Instruction =
              GIMatchComplexInstruction{
                  .ResultOpcode =
                      instructionOpcode(DestinationWidth == 32 ? InstructionOpcode::FCVT_S_D
                                                               : InstructionOpcode::FCVT_D_S),
                  .ResultType = *DestinationType,
                  .Operands = std::move(Operands),
              },
      };
    }
    const std::uint64_t ShiftWidth = Name == "shift_mask_xlen" ? 64U
                                     : Name == "shift_mask_32" ? 32U
                                                               : 0U;
    if (ShiftWidth == 0)
      return std::optional<GIMatchComplexOperand>{};
    const MachineOperand *Root = Instruction.operandAt(OperandIndex);
    const Register *RootRegister = Root == nullptr ? nullptr : Root->getIf<Register>();
    if (RootRegister == nullptr || !RootRegister->isVirtual())
      return std::optional<GIMatchComplexOperand>{};

    Register ShiftRegister = *RootRegister;
    const auto Definition = [&Function](Register Reg) -> const MachineInstr * {
      return Reg.isVirtual() ? Function.registerInfo().getVRegDef(Reg) : nullptr;
    };
    const auto UseOperand = [](Register Reg) -> Expected<MachineOperand> {
      return MachineOperand::registerOperand(Reg, {RegisterOperandFlag::Use});
    };
    const auto DirectResult = [&UseOperand](Register Reg) -> Expected<GIMatchComplexOperand> {
      auto Operand = UseOperand(Reg);
      if (!Operand)
        return std::unexpected(Operand.error());
      return GIMatchComplexOperand{
          .Operand = std::move(*Operand),
          .Instruction = std::nullopt,
      };
    };

    if (const MachineInstr *Extension = Definition(ShiftRegister);
        Extension != nullptr && Extension->opcode() == opcode(GenericOpcode::G_ZEXT)) {
      const MachineOperand *SourceOperand = Extension->operandAt(1);
      const Register *Source =
          SourceOperand == nullptr ? nullptr : SourceOperand->getIf<Register>();
      if (Source != nullptr)
        ShiftRegister = *Source;
    }

    const std::uint64_t ShiftMask = ShiftWidth - 1U;
    if (const MachineInstr *And = Definition(ShiftRegister);
        And != nullptr && And->opcode() == opcode(GenericOpcode::G_AND)) {
      const MachineOperand *SourceOperand = And->operandAt(1);
      const MachineOperand *MaskOperand = And->operandAt(2);
      const Register *Source =
          SourceOperand == nullptr ? nullptr : SourceOperand->getIf<Register>();
      const Register *Mask = MaskOperand == nullptr ? nullptr : MaskOperand->getIf<Register>();
      const auto MaskValue =
          Mask == nullptr ? std::nullopt : directIntegerConstant(Function.registerInfo(), *Mask);
      if (Source != nullptr && MaskValue) {
        const std::uint64_t RawMask = std::bit_cast<std::uint64_t>(*MaskValue);
        bool ElideMask = (RawMask & ShiftMask) == ShiftMask;
        if (!ElideMask) {
          GISelValueTracking ValueTracking(Function, Target);
          auto KnownZeroes = ValueTracking.getKnownZeroes(*Source);
          if (!KnownZeroes)
            return std::unexpected(KnownZeroes.error());
          ElideMask = ((RawMask | KnownZeroes->word(0)) & ShiftMask) == ShiftMask;
        }
        if (ElideMask)
          ShiftRegister = *Source;
      }
    }

    const MachineInstr *ShiftDefinition = Definition(ShiftRegister);
    if (ShiftDefinition == nullptr)
      return DirectResult(ShiftRegister);
    const MachineOperand *LeftOperand = ShiftDefinition->operandAt(1);
    const MachineOperand *RightOperand = ShiftDefinition->operandAt(2);
    const Register *Left = LeftOperand == nullptr ? nullptr : LeftOperand->getIf<Register>();
    const Register *Right = RightOperand == nullptr ? nullptr : RightOperand->getIf<Register>();

    if (ShiftDefinition->opcode() == opcode(GenericOpcode::G_ADD) && Left != nullptr &&
        Right != nullptr) {
      const auto Immediate = directIntegerConstant(Function.registerInfo(), *Right);
      if (Immediate != std::nullopt && *Immediate != 0 &&
          std::bit_cast<std::uint64_t>(*Immediate) % ShiftWidth == 0)
        return DirectResult(*Left);
    }

    if (ShiftDefinition->opcode() != opcode(GenericOpcode::G_SUB) || Left == nullptr ||
        Right == nullptr)
      return DirectResult(ShiftRegister);
    const auto Immediate = directIntegerConstant(Function.registerInfo(), *Left);
    if (!Immediate || *Immediate == 0)
      return DirectResult(ShiftRegister);

    const std::uint64_t Remainder = std::bit_cast<std::uint64_t>(*Immediate) % ShiftWidth;
    if (Remainder != 0 && Remainder != ShiftMask)
      return DirectResult(ShiftRegister);
    auto Type = Function.registerInfo().getType(*Right);
    if (!Type)
      return std::unexpected(Type.error());
    std::vector<MachineOperand> Operands;
    if (Remainder == 0) {
      auto Zero = physicalRegister(PhysicalRegisterID::X0);
      if (!Zero)
        return std::unexpected(Zero.error());
      auto ZeroOperand = UseOperand(Register::fromPhysical(*Zero));
      auto SourceOperand = UseOperand(*Right);
      if (!ZeroOperand)
        return std::unexpected(ZeroOperand.error());
      if (!SourceOperand)
        return std::unexpected(SourceOperand.error());
      Operands.push_back(std::move(*ZeroOperand));
      Operands.push_back(std::move(*SourceOperand));
      return GIMatchComplexOperand{
          .Operand = std::nullopt,
          .Instruction =
              GIMatchComplexInstruction{
                  .ResultOpcode = instructionOpcode(InstructionOpcode::SUBW),
                  .ResultType = *Type,
                  .Operands = std::move(Operands),
              },
      };
    }

    auto SourceOperand = UseOperand(*Right);
    if (!SourceOperand)
      return std::unexpected(SourceOperand.error());
    Operands.push_back(std::move(*SourceOperand));
    Operands.push_back(MachineOperand::immediate(-1));
    return GIMatchComplexOperand{
        .Operand = std::nullopt,
        .Instruction =
            GIMatchComplexInstruction{
                .ResultOpcode = instructionOpcode(InstructionOpcode::XORI),
                .ResultType = *Type,
                .Operands = std::move(Operands),
            },
    };
  }

  [[nodiscard]] Expected<void> select(MachineInstr &Instruction, MachineFunction &Function,
                                      const TargetDescription &Target) const override {
    auto Generated = GIMatchTableExecutor::select(generatedGlobalISelMatchTable(), Instruction,
                                                  Function, Target);
    if (!Generated)
      return std::unexpected(Generated.error());
    if (*Generated)
      return {};
    if (Instruction.opcode() == opcode(GenericOpcode::COPY))
      return selectCopy(Instruction, Function, Target);
    const GenericOpcode Generic = static_cast<GenericOpcode>(Instruction.opcode().value());
    const std::optional<LowLevelType> Type = resultType(Function, Instruction);

    switch (Generic) {
    case GenericOpcode::G_FPEXTLOAD:
      return selectFloatExtLoad(Instruction, Function, Target);
    case GenericOpcode::G_FPTRUNCSTORE:
      return selectFloatTruncStore(Instruction, Function, Target);
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
    case GenericOpcode::G_ANYEXT:
      return Instruction.setOpcode(opcode(GenericOpcode::COPY));
    case GenericOpcode::G_TRUNC:
      return Instruction.setOpcode(opcode(GenericOpcode::COPY));
    case GenericOpcode::G_FCMP:
      return selectFloatingCompare(Instruction, Function, Target);
    case GenericOpcode::G_FENCE:
      return selectFence(Instruction);
    case GenericOpcode::G_GLOBAL_VALUE:
    case GenericOpcode::G_JUMP_TABLE:
    case GenericOpcode::G_CONSTANT_POOL:
      return selectGlobalAddress(Instruction, Function, Target);
    case GenericOpcode::G_PHI:
      return Instruction.setOpcode(opcode(GenericOpcode::PHI));
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
  [[nodiscard]] Expected<const MachineMemOperand *> createGOTLoad(MachineFunction &Function,
                                                                  LowLevelType Type) const {
    auto Size = MemorySize::fixed(Type.minimumBitWidth() / 8U);
    if (!Size)
      return std::unexpected(Size.error());
    auto Alignment = rcc::Alignment::create(Type.minimumBitWidth() / 8U);
    if (!Alignment)
      return std::unexpected(Alignment.error());
    MachineMemOperandDesc Description;
    Description.Accesses = {MemoryAccessFlag::Load, MemoryAccessFlag::Dereferenceable,
                            MemoryAccessFlag::Invariant};
    Description.Size = *Size;
    Description.MemoryType = Type;
    Description.Align = *Alignment;
    Description.Base = MemoryBase::got();
    return Function.createMemOperand(std::move(Description));
  }

  [[nodiscard]] Expected<void> selectGlobalAddress(MachineInstr &Instruction,
                                                   MachineFunction &Function,
                                                   const TargetDescription &Target) const {
    const MachineOperand *DestinationOperand = Instruction.operandAt(0);
    const MachineOperand *SymbolOperand = Instruction.operandAt(1);
    const Register *Destination =
        DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
    const std::optional<LowLevelType> Type = resultType(Function, Instruction);
    if (Destination == nullptr || SymbolOperand == nullptr || !Type || !Type->isPointer()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V address selection has malformed generic operands");
    }
    auto DestinationConstraint = constrainBuilderRegisters(Function, Target, {*Destination});
    if (!DestinationConstraint)
      return std::unexpected(DestinationConstraint.error());

    GlobalValueProperties Properties;
    if (Instruction.opcode() == opcode(GenericOpcode::G_GLOBAL_VALUE)) {
      const auto *Global = SymbolOperand->getIf<GlobalSymbolOperand>();
      if (Global == nullptr)
        return makeUnexpected(ErrorCode::VerificationError,
                              "RISC-V global address has no global symbol operand");
      Properties = Global->Properties;
      if (Properties.ThreadLocal) {
        return makeUnexpected(ErrorCode::Unsupported,
                              "RISC-V GlobalISel does not support thread-local global addresses");
      }
    }

    const bool TaggedGlobals = Subtarget.features().has(RISCVSubtargetOption::TaggedGlobals);
    const bool UseGOT =
        Subtarget.relocationModel() == RelocationModel::PositionIndependent || TaggedGlobals;
    const auto ReplaceWithPseudo = [&](InstructionOpcode OpcodeValue) -> Expected<void> {
      auto SetOpcode = Instruction.setOpcode(instructionOpcode(OpcodeValue));
      if (!SetOpcode)
        return std::unexpected(SetOpcode.error());
      return constrainSelectedInstruction(Instruction, Function, Target);
    };
    const auto BuildGOTLoad = [&]() -> Expected<void> {
      auto Memory = createGOTLoad(Function, *Type);
      if (!Memory)
        return std::unexpected(Memory.error());
      MachineIRBuilder Builder(Function, Target);
      auto Point = Builder.setInsertPointBefore(Instruction);
      if (!Point)
        return std::unexpected(Point.error());
      auto Pending = Builder.buildInstr(instructionOpcode(InstructionOpcode::PseudoLGA));
      if (!Pending)
        return std::unexpected(Pending.error());
      auto Def = Pending->addDef(*Destination);
      if (!Def)
        return std::unexpected(Def.error());
      auto Symbol = Pending->addOperand(SymbolOperand->cloneDetached());
      if (!Symbol)
        return std::unexpected(Symbol.error());
      auto MemoryAdded = Pending->addMemOperand(**Memory);
      if (!MemoryAdded)
        return std::unexpected(MemoryAdded.error());
      auto Result = Pending->build();
      if (!Result)
        return std::unexpected(Result.error());
      auto Flags = propagateSelectionFlags(**Result, Instruction, Target);
      if (!Flags)
        return std::unexpected(Flags.error());
      auto Constraint = constrainSelectedInstruction(**Result, Function, Target);
      if (!Constraint)
        return std::unexpected(Constraint.error());
      MachineBasicBlock *Block = Instruction.parent();
      if (Block == nullptr)
        return makeUnexpected(ErrorCode::InternalError,
                              "RISC-V global address is detached from its block");
      return Block->removeInstruction(Instruction);
    };

    if (UseGOT) {
      if (Properties.DSOLocal && !TaggedGlobals)
        return ReplaceWithPseudo(InstructionOpcode::PseudoLLA);
      return BuildGOTLoad();
    }

    if (Subtarget.codeModel() == CodeModel::Medium) {
      if (Properties.ExternalWeak)
        return BuildGOTLoad();
      return ReplaceWithPseudo(InstructionOpcode::PseudoLLA);
    }
    if (Subtarget.codeModel() != CodeModel::Small) {
      return makeUnexpected(ErrorCode::Unsupported,
                            "RISC-V GlobalISel does not support this global address code model");
    }

    auto Temporary = createTemporary(Function, *Type, Target);
    if (!Temporary)
      return std::unexpected(Temporary.error());
    MachineIRBuilder Builder(Function, Target);
    auto Point = Builder.setInsertPointBefore(Instruction);
    if (!Point)
      return std::unexpected(Point.error());
    auto HighPending = Builder.buildInstr(instructionOpcode(InstructionOpcode::LUI));
    if (!HighPending)
      return std::unexpected(HighPending.error());
    auto HighDef = HighPending->addDef(*Temporary);
    if (!HighDef)
      return std::unexpected(HighDef.error());
    MachineOperand HighSymbol = SymbolOperand->cloneDetached();
    HighSymbol.setTargetFlags(AbsoluteHighFlag);
    auto HighOperand = HighPending->addOperand(std::move(HighSymbol));
    if (!HighOperand)
      return std::unexpected(HighOperand.error());
    auto High = HighPending->build();
    if (!High)
      return std::unexpected(High.error());
    auto HighFlags = propagateSelectionFlags(**High, Instruction, Target);
    if (!HighFlags)
      return std::unexpected(HighFlags.error());
    auto HighConstraint = constrainSelectedInstruction(**High, Function, Target);
    if (!HighConstraint)
      return std::unexpected(HighConstraint.error());

    auto LowPending = Builder.buildInstr(instructionOpcode(InstructionOpcode::ADDI));
    if (!LowPending)
      return std::unexpected(LowPending.error());
    auto LowDef = LowPending->addDef(*Destination);
    if (!LowDef)
      return std::unexpected(LowDef.error());
    auto LowSource = LowPending->addUse(*Temporary);
    if (!LowSource)
      return std::unexpected(LowSource.error());
    MachineOperand LowSymbol = SymbolOperand->cloneDetached();
    LowSymbol.setTargetFlags(AbsoluteLowFlag);
    auto LowOperand = LowPending->addOperand(std::move(LowSymbol));
    if (!LowOperand)
      return std::unexpected(LowOperand.error());
    auto Low = LowPending->build();
    if (!Low)
      return std::unexpected(Low.error());
    auto LowFlags = propagateSelectionFlags(**Low, Instruction, Target);
    if (!LowFlags)
      return std::unexpected(LowFlags.error());
    auto LowConstraint = constrainSelectedInstruction(**Low, Function, Target);
    if (!LowConstraint)
      return std::unexpected(LowConstraint.error());
    MachineBasicBlock *Block = Instruction.parent();
    if (Block == nullptr)
      return makeUnexpected(ErrorCode::InternalError,
                            "RISC-V global address is detached from its block");
    return Block->removeInstruction(Instruction);
  }

  [[nodiscard]] Expected<void> selectCopy(MachineInstr &Instruction, MachineFunction &Function,
                                          const TargetDescription &Target) const {
    const MachineOperand *DestinationOperand = Instruction.operandAt(0);
    const Register *Destination =
        DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
    if (Destination == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V COPY selection requires a register destination");
    if (!Destination->isVirtual())
      return {};
    return RegisterBanks.constrainRegister(Function, *Destination, Target);
  }

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
    FloatingPredicate Condition = *Predicate->floatingPredicate();
    const Register DestinationRegister = *Destination;
    Register LeftRegister = *Left;
    Register RightRegister = *Right;
    MachineIRBuilder Builder(Function, Target);

    const auto BuildBinary = [&](InstructionOpcode OpcodeValue, Register Def, Register LeftValue,
                                 Register RightValue) -> Expected<MachineInstr *> {
      auto Point = Builder.setInsertPointBefore(Instruction);
      if (!Point)
        return std::unexpected(Point.error());
      auto Registers = constrainBuilderRegisters(Function, Target, {Def, LeftValue, RightValue});
      if (!Registers)
        return std::unexpected(Registers.error());
      auto Pending = Builder.buildInstr(instructionOpcode(OpcodeValue));
      if (!Pending)
        return std::unexpected(Pending.error());
      auto DefResult = Pending->addDef(Def);
      if (!DefResult)
        return std::unexpected(DefResult.error());
      auto LeftResult = Pending->addUse(LeftValue);
      if (!LeftResult)
        return std::unexpected(LeftResult.error());
      auto RightResult = Pending->addUse(RightValue);
      if (!RightResult)
        return std::unexpected(RightResult.error());
      auto Built = Pending->build();
      if (!Built)
        return std::unexpected(Built.error());
      auto Constraint = constrainSelectedInstruction(**Built, Function, Target);
      if (!Constraint)
        return std::unexpected(Constraint.error());
      return *Built;
    };
    const auto BuildImmediate = [&](InstructionOpcode OpcodeValue, Register Def, Register Source,
                                    std::int64_t Immediate) -> Expected<MachineInstr *> {
      auto Point = Builder.setInsertPointBefore(Instruction);
      if (!Point)
        return std::unexpected(Point.error());
      auto Registers = constrainBuilderRegisters(Function, Target, {Def, Source});
      if (!Registers)
        return std::unexpected(Registers.error());
      auto Pending = Builder.buildInstr(instructionOpcode(OpcodeValue));
      if (!Pending)
        return std::unexpected(Pending.error());
      auto DefResult = Pending->addDef(Def);
      if (!DefResult)
        return std::unexpected(DefResult.error());
      auto SourceResult = Pending->addUse(Source);
      if (!SourceResult)
        return std::unexpected(SourceResult.error());
      auto ImmediateResult = Pending->addImmediate(Immediate);
      if (!ImmediateResult)
        return std::unexpected(ImmediateResult.error());
      auto Built = Pending->build();
      if (!Built)
        return std::unexpected(Built.error());
      auto Constraint = constrainSelectedInstruction(**Built, Function, Target);
      if (!Constraint)
        return std::unexpected(Constraint.error());
      return *Built;
    };

    Register TemporaryResult = DestinationRegister;
    bool NeedsInvert = false;
    if (legalizeFloatingPredicate(LeftRegister, RightRegister, Condition, NeedsInvert)) {
      if (NeedsInvert) {
        auto Temporary = createTemporary(Function, *ResultType, Target);
        if (!Temporary)
          return std::unexpected(Temporary.error());
        TemporaryResult = *Temporary;
      }
      auto OpcodeValue = floatingCompareOpcode(Condition, FloatType->scalarBitWidth());
      if (!OpcodeValue)
        return std::unexpected(OpcodeValue.error());
      auto Comparison = BuildBinary(*OpcodeValue, TemporaryResult, LeftRegister, RightRegister);
      if (!Comparison)
        return std::unexpected(Comparison.error());
    } else if (Condition == FloatingPredicate::OrderedNotEqual ||
               Condition == FloatingPredicate::UnorderedEqual) {
      NeedsInvert = Condition == FloatingPredicate::UnorderedEqual;
      auto First = createTemporary(Function, *ResultType, Target);
      auto Second = createTemporary(Function, *ResultType, Target);
      if (!First || !Second)
        return std::unexpected((!First ? First.error() : Second.error()));
      auto LessOpcode =
          floatingCompareOpcode(FloatingPredicate::OrderedLessThan, FloatType->scalarBitWidth());
      if (!LessOpcode)
        return std::unexpected(LessOpcode.error());
      auto FirstComparison = BuildBinary(*LessOpcode, *First, LeftRegister, RightRegister);
      if (!FirstComparison)
        return std::unexpected(FirstComparison.error());
      auto SecondComparison = BuildBinary(*LessOpcode, *Second, RightRegister, LeftRegister);
      if (!SecondComparison)
        return std::unexpected(SecondComparison.error());
      if (NeedsInvert) {
        auto Temporary = createTemporary(Function, *ResultType, Target);
        if (!Temporary)
          return std::unexpected(Temporary.error());
        TemporaryResult = *Temporary;
      }
      auto Combined = BuildBinary(InstructionOpcode::OR, TemporaryResult, *First, *Second);
      if (!Combined)
        return std::unexpected(Combined.error());
    } else if (Condition == FloatingPredicate::Ordered ||
               Condition == FloatingPredicate::Unordered) {
      NeedsInvert = Condition == FloatingPredicate::Unordered;
      auto EqualOpcode =
          floatingCompareOpcode(FloatingPredicate::OrderedEqual, FloatType->scalarBitWidth());
      if (!EqualOpcode)
        return std::unexpected(EqualOpcode.error());
      if (NeedsInvert) {
        auto Temporary = createTemporary(Function, *ResultType, Target);
        if (!Temporary)
          return std::unexpected(Temporary.error());
        TemporaryResult = *Temporary;
      }
      if (LeftRegister == RightRegister) {
        auto Comparison = BuildBinary(*EqualOpcode, TemporaryResult, LeftRegister, LeftRegister);
        if (!Comparison)
          return std::unexpected(Comparison.error());
      } else {
        auto First = createTemporary(Function, *ResultType, Target);
        auto Second = createTemporary(Function, *ResultType, Target);
        if (!First || !Second)
          return std::unexpected((!First ? First.error() : Second.error()));
        auto FirstComparison = BuildBinary(*EqualOpcode, *First, LeftRegister, LeftRegister);
        if (!FirstComparison)
          return std::unexpected(FirstComparison.error());
        auto SecondComparison = BuildBinary(*EqualOpcode, *Second, RightRegister, RightRegister);
        if (!SecondComparison)
          return std::unexpected(SecondComparison.error());
        auto Combined = BuildBinary(InstructionOpcode::AND, TemporaryResult, *First, *Second);
        if (!Combined)
          return std::unexpected(Combined.error());
      }
    } else {
      return makeUnexpected(ErrorCode::Unsupported,
                            "RISC-V floating comparison predicate has no LLVM selection");
    }

    if (NeedsInvert) {
      auto Inverted =
          BuildImmediate(InstructionOpcode::XORI, DestinationRegister, TemporaryResult, 1);
      if (!Inverted)
        return std::unexpected(Inverted.error());
    }
    MachineBasicBlock *Block = Instruction.parent();
    if (Block == nullptr)
      return makeUnexpected(ErrorCode::InternalError,
                            "RISC-V floating comparison is detached from its block");
    return Block->removeInstruction(Instruction);
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
    const InstructionOpcode Move =
        BitWidth == 32 ? InstructionOpcode::FMV_W_X : InstructionOpcode::FMV_D_X;
    if (Bits == 0) {
      auto Zero = physicalRegister(PhysicalRegisterID::X0);
      if (!Zero)
        return std::unexpected(Zero.error());
      auto EraseImmediate = Instruction.eraseOperand(1);
      if (!EraseImmediate)
        return std::unexpected(EraseImmediate.error());
      auto Source = insertPhysicalRegister(Instruction, 1, *Zero, RegisterOperandFlag::Use);
      if (!Source)
        return std::unexpected(Source.error());
      return Instruction.setOpcode(instructionOpcode(Move));
    }

    auto Integer = APInt::fromUnsigned(BitWidth, Bits);
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
