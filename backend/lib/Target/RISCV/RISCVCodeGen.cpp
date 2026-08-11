// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/Target/RISCV/RISCVCodeGen.h"

#include "RISCVImmediateMaterialization.h"

#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/LowLevelType.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/Target/RISCV/RISCVTarget.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace rcc::riscv {
namespace {

[[nodiscard]] Opcode targetOpcode(InstructionOpcode Value) noexcept {
  return Opcode::fromValue(static_cast<std::uint32_t>(Value));
}

[[nodiscard]] std::optional<LLT> registerType(const MachineFunction &Function,
                                              const MachineOperand &Operand) {
  const Register *Reg = Operand.getIf<Register>();
  if (Reg == nullptr || !Reg->isVirtual())
    return std::nullopt;
  auto Info = Function.registerInfo().virtualRegisterInfo(*Reg);
  if (!Info)
    return std::nullopt;
  return Info->get().Type;
}

[[nodiscard]] std::optional<LLT> resultType(const MachineFunction &Function,
                                            const MachineInstr &Instruction) {
  const MachineOperand *Result = Instruction.operandAt(0);
  return Result == nullptr ? std::nullopt : registerType(Function, *Result);
}

[[nodiscard]] std::optional<LLT> operandType(const MachineFunction &Function,
                                             const MachineInstr &Instruction, std::size_t Index) {
  const MachineOperand *Operand = Instruction.operandAt(Index);
  return Operand == nullptr ? std::nullopt : registerType(Function, *Operand);
}

[[nodiscard]] bool isRVFloatType(LLT Type) noexcept {
  if (!Type.isFloat())
    return false;
  const std::optional<FloatSemantics> Semantics = Type.floatSemantics();
  return Semantics &&
         (*Semantics == FloatSemantics::IEEESingle || *Semantics == FloatSemantics::IEEEDouble);
}

[[nodiscard]] bool isSupportedIntegerType(LLT Type) noexcept {
  return ((Type.isInteger() || Type.isAnyScalar()) && Type.scalarBitWidth() != 0 &&
          Type.scalarBitWidth() <= 64) ||
         (Type.isPointer() && Type.scalarBitWidth() == 64);
}

[[nodiscard]] bool isSupportedIntegerScalar(LLT Type) noexcept {
  return (Type.isInteger() || Type.isAnyScalar()) && Type.scalarBitWidth() != 0 &&
         Type.scalarBitWidth() <= 64;
}

[[nodiscard]] bool isNativeIntegerScalar(LLT Type) noexcept {
  return (Type.isInteger() || Type.isAnyScalar()) &&
         (Type.scalarBitWidth() == 32 || Type.scalarBitWidth() == 64);
}

[[nodiscard]] bool isScalarGPRType(LLT Type) noexcept {
  return ((Type.isInteger() || Type.isAnyScalar()) && Type.scalarBitWidth() != 0 &&
          Type.scalarBitWidth() <= 64) ||
         (Type.isPointer() && Type.scalarBitWidth() == 64);
}

[[nodiscard]] bool isI1Type(LLT Type) noexcept {
  return (Type.isInteger() || Type.isAnyScalar()) && Type.scalarBitWidth() == 1;
}

[[nodiscard]] bool isScalarIntegerExtension(LLT Destination, LLT Source) noexcept {
  return (Destination.isInteger() || Destination.isAnyScalar()) &&
         (Source.isInteger() || Source.isAnyScalar()) && Destination.scalarBitWidth() != 0 &&
         Source.scalarBitWidth() != 0 && Source.scalarBitWidth() < Destination.scalarBitWidth() &&
         Destination.scalarBitWidth() <= 64;
}

[[nodiscard]] Expected<IntegerImmediate>
extendIntegerImmediate(const IntegerImmediate &Value, std::uint32_t DestinationWidth,
                       bool SignExtend) {
  if (DestinationWidth == 0 || DestinationWidth > 64)
    return makeUnexpected(ErrorCode::Unsupported,
                          "RISC-V constant extension requires a destination no wider than 64 bits");

  std::uint64_t Raw = Value.words().empty() ? 0 : Value.words().front();
  const std::uint32_t SourceWidth = Value.bitWidth();
  if (SignExtend && Value.isNegative() && SourceWidth < 64)
    Raw |= ~std::uint64_t{0} << SourceWidth;
  if (DestinationWidth < 64)
    Raw &= (std::uint64_t{1} << DestinationWidth) - 1U;
  return IntegerImmediate::create(DestinationWidth, std::span<const std::uint64_t>(&Raw, 1));
}

[[nodiscard]] Expected<void> foldConstantExtension(MachineInstr &Instruction,
                                                    MachineFunction &Function,
                                                    GenericOpcode Generic) {
  const MachineOperand *SourceOperand = Instruction.operandAt(1);
  const MachineOperand *DestinationOperand = Instruction.operandAt(0);
  const Register *Source = SourceOperand == nullptr ? nullptr : SourceOperand->getIf<Register>();
  const Register *Destination =
      DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
  if (Source == nullptr || Destination == nullptr || !Source->isVirtual() ||
      !Destination->isVirtual()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V constant extension requires virtual-register operands");
  }
  const MachineOperand *SourceDefinition = Function.registerInfo().getUniqueVRegDef(*Source);
  const MachineInstr *SourceDef = SourceDefinition == nullptr ? nullptr : SourceDefinition->parent();
  if (SourceDef == nullptr || SourceDef->opcode() != opcode(GenericOpcode::G_CONSTANT))
    return makeUnexpected(ErrorCode::Unsupported,
                          "RISC-V constant extension requires a single constant definition");
  const MachineOperand *ConstantOperand = SourceDef->operandAt(1);
  const auto *Integer = ConstantOperand == nullptr
                            ? nullptr
                            : ConstantOperand->getIf<IntegerImmediateOperand>();
  if (Integer == nullptr)
    return makeUnexpected(ErrorCode::Unsupported,
                          "RISC-V constant extension requires an integer constant");
  const auto DestinationInfo = Function.registerInfo().virtualRegisterInfo(*Destination);
  if (!DestinationInfo || !DestinationInfo->get().Type)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V constant extension destination has no type");
  const std::uint32_t DestinationWidth = DestinationInfo->get().Type->scalarBitWidth();
  const bool SignExtend = Generic != GenericOpcode::G_ZEXT;
  auto Extended = extendIntegerImmediate(Integer->Value, DestinationWidth, SignExtend);
  if (!Extended)
    return std::unexpected(Extended.error());

  auto SetOpcode = Instruction.setOpcode(opcode(GenericOpcode::G_CONSTANT));
  if (!SetOpcode)
    return std::unexpected(SetOpcode.error());
  while (Instruction.operandCount() > 1) {
    auto Erased = Instruction.eraseOperand(1);
    if (!Erased)
      return std::unexpected(Erased.error());
  }
  auto Added = Instruction.addOperand(MachineOperand::integerImmediate(std::move(*Extended)));
  if (!Added)
    return std::unexpected(Added.error());

  if (Function.registerInfo().useEmpty(*Source)) {
    MachineInstr *Def = const_cast<MachineInstr *>(SourceDef);
    MachineBasicBlock *Parent = Def->parent();
    if (Parent == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V constant extension source definition is detached");
    auto Removed = Parent->removeInstruction(*Def);
    if (!Removed)
      return std::unexpected(Removed.error());
    // Keep the source register record as a tombstone-free historical vreg until
    // the legalizer worklist transaction has completed.
  }
  return {};
}

struct WideConstantUnmergePlan final {
  MachineInstr *Extension = nullptr;
  MachineInstr *Unmerge = nullptr;
  std::vector<Register> Outputs;
  bool SignExtend = false;
};

[[nodiscard]] MachineInstr *singleRegisterUse(const MachineFunction &Function, Register Reg) {
  MachineInstr *UseInstruction = nullptr;
  auto &RegisterInfo = const_cast<MachineRegisterInfo &>(Function.registerInfo());
  for (MachineOperand &Operand : RegisterInfo.useOperands(Reg)) {
    if (Operand.parent() == nullptr)
      continue;
    if (UseInstruction != nullptr)
      return nullptr;
    UseInstruction = Operand.parent();
  }
  return UseInstruction;
}

[[nodiscard]] std::optional<WideConstantUnmergePlan>
wideConstantUnmergePlan(const MachineInstr &Constant, const MachineFunction &Function) {
  const MachineOperand *ResultOperand = Constant.operandAt(0);
  const Register *Result = ResultOperand == nullptr ? nullptr : ResultOperand->getIf<Register>();
  if (Result == nullptr || !Result->isVirtual())
    return std::nullopt;

  MachineInstr *FirstUse = singleRegisterUse(Function, *Result);
  if (FirstUse == nullptr)
    return std::nullopt;
  MachineInstr *Extension = nullptr;
  MachineInstr *Unmerge = FirstUse;
  if (FirstUse->opcode() == opcode(GenericOpcode::G_ANYEXT) ||
      FirstUse->opcode() == opcode(GenericOpcode::G_SEXT) ||
      FirstUse->opcode() == opcode(GenericOpcode::G_ZEXT)) {
    Extension = FirstUse;
    const MachineOperand *ExtendedOperand = Extension->operandAt(0);
    const Register *Extended =
        ExtendedOperand == nullptr ? nullptr : ExtendedOperand->getIf<Register>();
    if (Extended == nullptr || !Extended->isVirtual())
      return std::nullopt;
    Unmerge = singleRegisterUse(Function, *Extended);
    if (Unmerge == nullptr)
      return std::nullopt;
  }
  if (Unmerge->opcode() != opcode(GenericOpcode::G_UNMERGE_VALUES) ||
      Unmerge->operandCount() < 3)
    return std::nullopt;

  WideConstantUnmergePlan Plan;
  Plan.Extension = Extension;
  Plan.Unmerge = Unmerge;
  Plan.SignExtend = Extension != nullptr && Extension->opcode() != opcode(GenericOpcode::G_ZEXT);
  for (std::size_t Index = 0; Index + 1U < Unmerge->operandCount(); ++Index) {
    const MachineOperand *Operand = Unmerge->operandAt(Index);
    const Register *Reg = Operand == nullptr ? nullptr : Operand->getIf<Register>();
    if (Reg == nullptr || !Reg->isVirtual() || !Operand->isDef())
      return std::nullopt;
    const auto Info = Function.registerInfo().virtualRegisterInfo(*Reg);
    if (!Info || !Info->get().Type || Info->get().Type->scalarBitWidth() == 0 ||
        Info->get().Type->scalarBitWidth() > 64 ||
        (!Info->get().Type->isInteger() && !Info->get().Type->isAnyScalar()))
      return std::nullopt;
    Plan.Outputs.push_back(*Reg);
  }
  const MachineOperand *SourceOperand = Unmerge->operandAt(Unmerge->operandCount() - 1U);
  const Register *Source = SourceOperand == nullptr ? nullptr : SourceOperand->getIf<Register>();
  const Register *ExpectedSource = Result;
  if (Extension != nullptr) {
    const MachineOperand *ExtendedOperand = Extension->operandAt(0);
    ExpectedSource = ExtendedOperand == nullptr ? nullptr : ExtendedOperand->getIf<Register>();
  }
  if (Source == nullptr || ExpectedSource == nullptr || *Source != *ExpectedSource)
    return std::nullopt;
  return Plan;
}

[[nodiscard]] Expected<IntegerImmediate>
wideConstantWord(const IntegerImmediate &Value, std::size_t WordIndex, std::uint32_t Width,
                 bool SignExtend) {
  std::uint64_t Raw = 0;
  if (WordIndex < Value.words().size())
    Raw = Value.words()[WordIndex];
  else if (SignExtend && Value.isNegative())
    Raw = std::numeric_limits<std::uint64_t>::max();
  const std::size_t WordBitOffset = WordIndex * 64U;
  if (SignExtend && Value.isNegative() && WordBitOffset < Value.bitWidth()) {
    const std::uint32_t Remaining =
        Value.bitWidth() - static_cast<std::uint32_t>(WordBitOffset);
    if (Remaining < 64)
      Raw |= ~std::uint64_t{0} << Remaining;
  }
  if (Width < 64)
    Raw &= (std::uint64_t{1} << Width) - 1U;
  return IntegerImmediate::create(Width, std::span<const std::uint64_t>(&Raw, 1));
}

[[nodiscard]] Expected<void> foldWideConstant(MachineInstr &Instruction, MachineFunction &Function,
                                               const TargetDescription &Target) {
  const auto Plan = wideConstantUnmergePlan(Instruction, Function);
  if (!Plan)
    return makeUnexpected(ErrorCode::Unsupported,
                          "RISC-V wide constant requires a G_UNMERGE_VALUES use");
  const MachineOperand *ImmediateOperand = Instruction.operandAt(1);
  const auto *Integer = ImmediateOperand == nullptr
                            ? nullptr
                            : ImmediateOperand->getIf<IntegerImmediateOperand>();
  if (Integer == nullptr)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V wide constant has no integer immediate");

  std::vector<IntegerImmediate> Words;
  Words.reserve(Plan->Outputs.size());
  for (std::size_t Index = 0; Index < Plan->Outputs.size(); ++Index) {
    const auto Info = Function.registerInfo().virtualRegisterInfo(Plan->Outputs[Index]);
    if (!Info || !Info->get().Type)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V wide constant output has no virtual-register type");
    auto Word = wideConstantWord(Integer->Value, Index, Info->get().Type->scalarBitWidth(),
                                 Plan->SignExtend);
    if (!Word)
      return std::unexpected(Word.error());
    Words.push_back(std::move(*Word));
  }

  std::vector<std::size_t> Representatives(Plan->Outputs.size());
  for (std::size_t Index = 0; Index < Representatives.size(); ++Index) {
    Representatives[Index] = Index;
    for (std::size_t Previous = 0; Previous < Index; ++Previous) {
      if (Words[Previous] == Words[Index]) {
        Representatives[Index] = Previous;
        break;
      }
    }
  }

  const Register FirstOutput = Plan->Outputs.front();
  auto Retargeted = Instruction.operandAt(0)->setRegister(FirstOutput);
  if (!Retargeted)
    return std::unexpected(Retargeted.error());
  auto SetOpcode = Instruction.setOpcode(opcode(GenericOpcode::G_CONSTANT));
  if (!SetOpcode)
    return std::unexpected(SetOpcode.error());
  while (Instruction.operandCount() > 1) {
    auto Erased = Instruction.eraseOperand(1);
    if (!Erased)
      return std::unexpected(Erased.error());
  }
  auto Added = Instruction.addOperand(MachineOperand::integerImmediate(std::move(Words.front())));
  if (!Added)
    return std::unexpected(Added.error());

  MachineBasicBlock *Block = Instruction.parent();
  if (Block == nullptr)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V wide constant is detached during legalization");
  if (Plan->Unmerge != &Instruction) {
    auto RemovedUnmerge = Plan->Unmerge->parent()->removeInstruction(*Plan->Unmerge);
    if (!RemovedUnmerge)
      return std::unexpected(RemovedUnmerge.error());
  }
  if (Plan->Extension != nullptr) {
    auto RemovedExtension = Plan->Extension->parent()->removeInstruction(*Plan->Extension);
    if (!RemovedExtension)
      return std::unexpected(RemovedExtension.error());
  }

  for (std::size_t Index = 1; Index < Plan->Outputs.size(); ++Index) {
    if (Representatives[Index] == Index)
      continue;
    auto Replaced = Function.registerInfo().replaceRegWith(
        Plan->Outputs[Index], Plan->Outputs[Representatives[Index]]);
    if (!Replaced)
      return std::unexpected(Replaced.error());
  }

  MachineInstr *Last = &Instruction;
  for (std::size_t Index = 1; Index < Plan->Outputs.size(); ++Index) {
    if (Representatives[Index] != Index)
      continue;
    MachineIRBuilder Builder(Function, Target);
    auto Point = Builder.setInsertPointAfter(*Last);
    if (!Point)
      return std::unexpected(Point.error());
    auto Pending = Builder.buildInstr(opcode(GenericOpcode::G_CONSTANT));
    if (!Pending)
      return std::unexpected(Pending.error());
    auto Def = Pending->addDef(Plan->Outputs[Index]);
    if (!Def)
      return std::unexpected(Def.error());
    auto Value = Pending->addIntegerImmediate(std::move(Words[Index]));
    if (!Value)
      return std::unexpected(Value.error());
    auto Built = Pending->build();
    if (!Built)
      return std::unexpected(Built.error());
    Last = *Built;
  }
  return {};
}

[[nodiscard]] Expected<Register>
buildIntegerConstantBefore(MachineIRBuilder &Builder, MachineInstr &Anchor,
                           std::uint32_t Width, std::uint64_t Value);

[[nodiscard]] Expected<Register>
buildBinaryBefore(MachineIRBuilder &Builder, MachineInstr &Anchor,
                  GenericOpcode Generic, LLT Type, Register Left,
                  Register Right);

[[nodiscard]] Expected<Register> buildUnaryBefore(MachineIRBuilder &Builder, MachineInstr &Anchor,
                                                  GenericOpcode Generic, LLT Type, Register Source);

[[nodiscard]] Expected<void> eraseInstruction(MachineInstr &Instruction);

struct NarrowAnyExtBinaryPlan final {
  MachineInstr *Binary = nullptr;
  GenericOpcode Generic = GenericOpcode::Invalid;
  std::array<Register, 2> WideInputs{};
  std::vector<MachineInstr *> Truncations;
  std::vector<MachineInstr *> Constants;
  std::uint32_t SourceWidth = 0;
};

[[nodiscard]] std::optional<NarrowAnyExtBinaryPlan>
narrowAnyExtBinaryPlan(const MachineInstr &Extension, const MachineFunction &Function) {
  const MachineOperand *DestinationOperand = Extension.operandAt(0);
  const MachineOperand *SourceOperand = Extension.operandAt(1);
  const Register *Destination =
      DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
  const Register *Source = SourceOperand == nullptr ? nullptr : SourceOperand->getIf<Register>();
  if (Destination == nullptr || Source == nullptr || !Destination->isVirtual() ||
      !Source->isVirtual() || singleRegisterUse(Function, *Source) != &Extension)
    return std::nullopt;
  const auto DestinationInfo = Function.registerInfo().virtualRegisterInfo(*Destination);
  const auto SourceInfo = Function.registerInfo().virtualRegisterInfo(*Source);
  if (!DestinationInfo || !SourceInfo || !DestinationInfo->get().Type || !SourceInfo->get().Type)
    return std::nullopt;
  const LLT DestinationType = *DestinationInfo->get().Type;
  const LLT SourceType = *SourceInfo->get().Type;
  if (!isScalarIntegerExtension(DestinationType, SourceType) ||
      DestinationType.scalarBitWidth() != 64)
    return std::nullopt;

  const MachineOperand *SourceDefinition = Function.registerInfo().getUniqueVRegDef(*Source);
  MachineInstr *Binary =
      SourceDefinition == nullptr ? nullptr : const_cast<MachineInstr *>(SourceDefinition->parent());
  if (Binary == nullptr || Binary->operandCount() != 3)
    return std::nullopt;
  const GenericOpcode Generic = static_cast<GenericOpcode>(Binary->opcode().value());
  if (Generic != GenericOpcode::G_ADD && Generic != GenericOpcode::G_SUB &&
      Generic != GenericOpcode::G_AND && Generic != GenericOpcode::G_OR &&
      Generic != GenericOpcode::G_XOR && Generic != GenericOpcode::G_SHL)
    return std::nullopt;

  NarrowAnyExtBinaryPlan Plan;
  Plan.Binary = Binary;
  Plan.Generic = Generic;
  Plan.SourceWidth = SourceType.scalarBitWidth();
  for (std::size_t Index = 0; Index != Plan.WideInputs.size(); ++Index) {
    const MachineOperand *Operand = Binary->operandAt(Index + 1U);
    const Register *Reg = Operand == nullptr ? nullptr : Operand->getIf<Register>();
    if (Reg == nullptr || !Reg->isVirtual())
      return std::nullopt;
    const auto Info = Function.registerInfo().virtualRegisterInfo(*Reg);
    if (!Info || !Info->get().Type)
      return std::nullopt;
    if (*Info->get().Type == DestinationType) {
      Plan.WideInputs[Index] = *Reg;
      continue;
    }
    if (*Info->get().Type != SourceType)
      return std::nullopt;

    const MachineOperand *Definition = Function.registerInfo().getUniqueVRegDef(*Reg);
    MachineInstr *Def = Definition == nullptr ? nullptr : const_cast<MachineInstr *>(Definition->parent());
    if (Def != nullptr && Def->opcode() == opcode(GenericOpcode::G_TRUNC) &&
        Def->operandCount() == 2) {
      const MachineOperand *WideOperand = Def->operandAt(1);
      const Register *Wide = WideOperand == nullptr ? nullptr : WideOperand->getIf<Register>();
      if (Wide == nullptr || !Wide->isVirtual())
        return std::nullopt;
      const auto WideInfo = Function.registerInfo().virtualRegisterInfo(*Wide);
      if (!WideInfo || !WideInfo->get().Type || *WideInfo->get().Type != DestinationType)
        return std::nullopt;
      Plan.WideInputs[Index] = *Wide;
      Plan.Truncations.push_back(Def);
      continue;
    }
    if (Def != nullptr && Def->opcode() == opcode(GenericOpcode::G_CONSTANT) &&
        singleRegisterUse(Function, *Reg) == Binary) {
      Plan.WideInputs[Index] = *Reg;
      Plan.Constants.push_back(Def);
      continue;
    }
    return std::nullopt;
  }
  return Plan;
}

[[nodiscard]] Expected<void> widenConstantToRV64(MachineInstr &Instruction,
                                                  MachineFunction &Function) {
  const MachineOperand *DestinationOperand = Instruction.operandAt(0);
  const Register *Destination =
      DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
  const MachineOperand *ImmediateOperand = Instruction.operandAt(1);
  const auto *Immediate = ImmediateOperand == nullptr
                              ? nullptr
                              : ImmediateOperand->getIf<IntegerImmediateOperand>();
  if (Destination == nullptr || !Destination->isVirtual() || Immediate == nullptr)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V narrow constant legalization has invalid operands");
  auto Type = LLT::makeAnyScalar(64);
  if (!Type)
    return std::unexpected(Type.error());
  auto SetType = Function.registerInfo().setType(*Destination, *Type);
  if (!SetType)
    return std::unexpected(SetType.error());
  auto Extended = extendIntegerImmediate(Immediate->Value, 64, true);
  if (!Extended)
    return std::unexpected(Extended.error());
  auto Erased = Instruction.eraseOperand(1);
  if (!Erased)
    return std::unexpected(Erased.error());
  auto Added = Instruction.addOperand(MachineOperand::integerImmediate(std::move(*Extended)));
  if (!Added)
    return std::unexpected(Added.error());
  return {};
}

[[nodiscard]] Expected<Register> widenAtomicCompareExchangeSource(MachineInstr &Instruction,
                                                                  MachineFunction &Function,
                                                                  MachineIRBuilder &Builder,
                                                                  Register Source, LLT WideType) {
  const auto SourceInfo = Function.registerInfo().virtualRegisterInfo(Source);
  if (!SourceInfo || !SourceInfo->get().Type)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V compare-exchange source has no scalar type");
  if (SourceInfo->get().Type->scalarBitWidth() == WideType.scalarBitWidth())
    return Source;

  const MachineOperand *Definition = Function.registerInfo().getUniqueVRegDef(Source);
  MachineInstr *Def =
      Definition == nullptr ? nullptr : const_cast<MachineInstr *>(Definition->parent());
  if (Def != nullptr && Def->opcode() == opcode(GenericOpcode::G_CONSTANT) &&
      singleRegisterUse(Function, Source) == &Instruction) {
    auto Widened = widenConstantToRV64(*Def, Function);
    if (!Widened)
      return std::unexpected(Widened.error());
    return Source;
  }
  return buildUnaryBefore(Builder, Instruction, GenericOpcode::G_ANYEXT, WideType, Source);
}

[[nodiscard]] Expected<void> lowerAtomicCompareExchange(MachineInstr &Instruction,
                                                        MachineFunction &Function,
                                                        const TargetDescription &Target) {
  const std::optional<LLT> OriginalType = resultType(Function, Instruction);
  const MachineOperand *DestinationOperand = Instruction.operandAt(0);
  const MachineOperand *CompareOperand = Instruction.operandAt(2);
  const MachineOperand *NewValueOperand = Instruction.operandAt(3);
  const Register *Destination =
      DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
  const Register *Compare = CompareOperand == nullptr ? nullptr : CompareOperand->getIf<Register>();
  const Register *NewValue =
      NewValueOperand == nullptr ? nullptr : NewValueOperand->getIf<Register>();
  if (!OriginalType || Destination == nullptr || Compare == nullptr || NewValue == nullptr ||
      OriginalType->scalarBitWidth() >= 64)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V narrow compare-exchange has invalid operands");

  auto WideType = LLT::makeAnyScalar(64);
  if (!WideType)
    return std::unexpected(WideType.error());
  MachineIRBuilder Builder(Function, Target);
  auto WideCompare =
      widenAtomicCompareExchangeSource(Instruction, Function, Builder, *Compare, *WideType);
  if (!WideCompare)
    return std::unexpected(WideCompare.error());
  auto WideNewValue =
      widenAtomicCompareExchangeSource(Instruction, Function, Builder, *NewValue, *WideType);
  if (!WideNewValue)
    return std::unexpected(WideNewValue.error());

  Register WideDestination;
  MachineInstr *FoldedExtension = singleRegisterUse(Function, *Destination);
  if (FoldedExtension != nullptr && FoldedExtension->opcode() == opcode(GenericOpcode::G_ANYEXT) &&
      operandType(Function, *FoldedExtension, 0)
          .transform([](LLT Type) { return Type.scalarBitWidth() == 64; })
          .value_or(false)) {
    const MachineOperand *WideDestinationOperand = FoldedExtension->operandAt(0);
    const Register *FoldedDestination =
        WideDestinationOperand == nullptr ? nullptr : WideDestinationOperand->getIf<Register>();
    if (FoldedDestination == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V compare-exchange extension has no destination");
    WideDestination = *FoldedDestination;
  } else {
    auto Created = Function.registerInfo().createVirtualRegister(*WideType);
    if (!Created)
      return std::unexpected(Created.error());
    WideDestination = *Created;
    FoldedExtension = nullptr;
  }

  if (auto Replaced = Instruction.operandAt(0)->setRegister(WideDestination); !Replaced)
    return std::unexpected(Replaced.error());
  if (auto Replaced = Instruction.operandAt(2)->setRegister(*WideCompare); !Replaced)
    return std::unexpected(Replaced.error());
  if (auto Replaced = Instruction.operandAt(3)->setRegister(*WideNewValue); !Replaced)
    return std::unexpected(Replaced.error());

  if (FoldedExtension != nullptr)
    return eraseInstruction(*FoldedExtension);

  auto Point = Builder.setInsertPointAfter(Instruction);
  if (!Point)
    return std::unexpected(Point.error());
  auto Truncation = Builder.buildInstr(opcode(GenericOpcode::G_TRUNC));
  if (!Truncation)
    return std::unexpected(Truncation.error());
  if (auto Added = Truncation->addDef(*Destination); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Truncation->addUse(WideDestination); !Added)
    return std::unexpected(Added.error());
  auto Built = Truncation->build();
  if (!Built)
    return std::unexpected(Built.error());
  return {};
}

[[nodiscard]] Expected<void> foldNarrowAnyExtBinary(MachineInstr &Extension,
                                                     MachineFunction &Function,
                                                     const TargetDescription &Target) {
  const auto Plan = narrowAnyExtBinaryPlan(Extension, Function);
  if (!Plan)
    return makeUnexpected(ErrorCode::Unsupported,
                          "RISC-V narrow any-extension does not have a legal binary chain");
  for (MachineInstr *Constant : Plan->Constants) {
    auto Widened = widenConstantToRV64(*Constant, Function);
    if (!Widened)
      return std::unexpected(Widened.error());
  }
  for (std::size_t Index = 0; Index != Plan->WideInputs.size(); ++Index) {
    auto Replaced = Plan->Binary->operandAt(Index + 1U)->setRegister(Plan->WideInputs[Index]);
    if (!Replaced)
      return std::unexpected(Replaced.error());
  }
  for (MachineInstr *Truncation : Plan->Truncations) {
    auto Removed = Truncation->parent()->removeInstruction(*Truncation);
    if (!Removed)
      return std::unexpected(Removed.error());
  }

  if (Plan->Generic == GenericOpcode::G_SHL) {
    Register Amount = Plan->WideInputs[1];
    if (Plan->SourceWidth < 32 || Plan->SourceWidth > 32) {
      const std::uint64_t Mask =
          Plan->SourceWidth == 64
              ? std::numeric_limits<std::uint64_t>::max()
              : (std::uint64_t{1} << Plan->SourceWidth) - 1U;
      auto WideType = LLT::makeAnyScalar(64);
      if (!WideType)
        return std::unexpected(WideType.error());
      MachineIRBuilder Builder(Function, Target);
      auto MaskValue =
          buildIntegerConstantBefore(Builder, Extension, 64, Mask);
      if (!MaskValue)
        return std::unexpected(MaskValue.error());
      auto MaskedAmount = buildBinaryBefore(Builder, Extension,
                                            GenericOpcode::G_AND, *WideType,
                                            Amount, *MaskValue);
      if (!MaskedAmount)
        return std::unexpected(MaskedAmount.error());
      Amount = *MaskedAmount;
    }
    auto SetOpcode = Extension.setOpcode(
        Plan->SourceWidth == 32 ? targetOpcode(InstructionOpcode::G_SLLW)
                                : opcode(GenericOpcode::G_SHL));
    if (!SetOpcode)
      return std::unexpected(SetOpcode.error());
    while (Extension.operandCount() > 1) {
      auto Erased = Extension.eraseOperand(1);
      if (!Erased)
        return std::unexpected(Erased.error());
    }
    for (Register Input : {Plan->WideInputs[0], Amount}) {
      auto Operand = MachineOperand::registerOperand(
          Input, RegisterOperandFlags{RegisterOperandFlag::Use});
      if (!Operand)
        return std::unexpected(Operand.error());
      auto Added = Extension.addOperand(std::move(*Operand));
      if (!Added)
        return std::unexpected(Added.error());
    }
    MachineBasicBlock *BinaryBlock = Plan->Binary->parent();
    if (BinaryBlock == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V narrow shift is detached during legalization");
    return BinaryBlock->removeInstruction(*Plan->Binary);
  }

  if (Plan->SourceWidth == 32 &&
      (Plan->Generic == GenericOpcode::G_ADD || Plan->Generic == GenericOpcode::G_SUB)) {
    auto WideType = LLT::makeAnyScalar(64);
    if (!WideType)
      return std::unexpected(WideType.error());
    auto Temporary = Function.registerInfo().createVirtualRegister(*WideType);
    if (!Temporary)
      return std::unexpected(Temporary.error());
    auto Retargeted = Plan->Binary->operandAt(0)->setRegister(*Temporary);
    if (!Retargeted)
      return std::unexpected(Retargeted.error());
    auto SetOpcode = Extension.setOpcode(opcode(GenericOpcode::G_SEXT_INREG));
    if (!SetOpcode)
      return std::unexpected(SetOpcode.error());
    auto ReplacedSource = Extension.operandAt(1)->setRegister(*Temporary);
    if (!ReplacedSource)
      return std::unexpected(ReplacedSource.error());
    auto Width = Extension.addOperand(MachineOperand::immediate(32));
    if (!Width)
      return std::unexpected(Width.error());
    return {};
  }

  auto SetOpcode = Extension.setOpcode(opcode(Plan->Generic));
  if (!SetOpcode)
    return std::unexpected(SetOpcode.error());
  while (Extension.operandCount() > 1) {
    auto Erased = Extension.eraseOperand(1);
    if (!Erased)
      return std::unexpected(Erased.error());
  }
  for (Register Input : Plan->WideInputs) {
    auto Operand = MachineOperand::registerOperand(
        Input, RegisterOperandFlags{RegisterOperandFlag::Use});
    if (!Operand)
      return std::unexpected(Operand.error());
    auto Added = Extension.addOperand(std::move(*Operand));
    if (!Added)
      return std::unexpected(Added.error());
  }
  MachineBasicBlock *BinaryBlock = Plan->Binary->parent();
  if (BinaryBlock == nullptr)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V narrow binary instruction is detached during legalization");
  auto Removed = BinaryBlock->removeInstruction(*Plan->Binary);
  if (!Removed)
    return std::unexpected(Removed.error());
  (void)Target;
  return {};
}

[[nodiscard]] bool fitsSigned(std::int64_t Value, std::uint32_t Bits) noexcept {
  const std::int64_t Minimum = -(std::int64_t{1} << (Bits - 1U));
  const std::int64_t Maximum = (std::int64_t{1} << (Bits - 1U)) - 1;
  return Value >= Minimum && Value <= Maximum;
}

[[nodiscard]] Expected<void> lowerConstantToPool(MachineInstr &Instruction,
                                                  MachineFunction &Function,
                                                  const TargetDescription &Target) {
  const MachineOperand *DestinationOperand = Instruction.operandAt(0);
  const Register *Destination =
      DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
  const MachineOperand *ImmediateOperand = Instruction.operandAt(1);
  const auto *Immediate = ImmediateOperand == nullptr
                              ? nullptr
                              : ImmediateOperand->getIf<IntegerImmediateOperand>();
  if (Destination == nullptr || !Destination->isVirtual() || Immediate == nullptr)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V constant-pool legalization has invalid operands");
  const auto DestinationInfo = Function.registerInfo().virtualRegisterInfo(*Destination);
  if (!DestinationInfo || !DestinationInfo->get().Type ||
      DestinationInfo->get().Type->scalarBitWidth() != 64)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V constant-pool legalization requires a 64-bit destination");
  const LLT DestinationType = *DestinationInfo->get().Type;
  auto Value = GenericConstantData::integer(Immediate->Value);
  if (!Value)
    return std::unexpected(Value.error());
  auto Align = Alignment::create(8);
  if (!Align)
    return std::unexpected(Align.error());
  MachineConstantPoolEntryDesc Description;
  Description.Value = std::move(*Value);
  Description.Size = 8;
  Description.Align = *Align;
  auto PoolIndex = Function.constantPool().addEntry(std::move(Description));
  if (!PoolIndex)
    return std::unexpected(PoolIndex.error());

  auto PointerType = LLT::makePointer(0, 64);
  if (!PointerType)
    return std::unexpected(PointerType.error());
  auto Address = Function.registerInfo().createVirtualRegister(*PointerType);
  if (!Address)
    return std::unexpected(Address.error());
  MachineIRBuilder Builder(Function, Target);
  auto Point = Builder.setInsertPointBefore(Instruction);
  if (!Point)
    return std::unexpected(Point.error());
  auto AddressInstruction = Builder.buildInstr(opcode(GenericOpcode::G_CONSTANT_POOL));
  if (!AddressInstruction)
    return std::unexpected(AddressInstruction.error());
  auto AddressDef = AddressInstruction->addDef(*Address);
  if (!AddressDef)
    return std::unexpected(AddressDef.error());
  auto AddressPool = AddressInstruction->addConstantPool(*PoolIndex);
  if (!AddressPool)
    return std::unexpected(AddressPool.error());
  auto BuiltAddress = AddressInstruction->build();
  if (!BuiltAddress)
    return std::unexpected(BuiltAddress.error());

  MachineMemOperandDesc MemoryDescription;
  MemoryDescription.Accesses = {MemoryAccessFlag::Load};
  MemoryDescription.Size = *MemorySize::fixed(8);
  MemoryDescription.MemoryType = DestinationType;
  MemoryDescription.Align = *Align;
  MemoryDescription.Base = MemoryBase::constantPoolPseudo();
  auto Memory = Function.createMemOperand(std::move(MemoryDescription));
  if (!Memory)
    return std::unexpected(Memory.error());
  auto SetOpcode = Instruction.setOpcode(opcode(GenericOpcode::G_LOAD));
  if (!SetOpcode)
    return std::unexpected(SetOpcode.error());
  while (Instruction.operandCount() > 1) {
    auto Erased = Instruction.eraseOperand(1);
    if (!Erased)
      return std::unexpected(Erased.error());
  }
  auto AddressUse = MachineOperand::registerOperand(
      *Address, RegisterOperandFlags{RegisterOperandFlag::Use});
  if (!AddressUse)
    return std::unexpected(AddressUse.error());
  auto AddedAddress = Instruction.addOperand(std::move(*AddressUse));
  if (!AddedAddress)
    return std::unexpected(AddedAddress.error());
  auto AddedMemory = Instruction.addMemOperand(**Memory);
  if (!AddedMemory)
    return std::unexpected(AddedMemory.error());
  return {};
}

[[nodiscard]] bool isSupportedNonAtomicMemoryAccess(const MachineInstr &Instruction) noexcept {
  if (Instruction.memOperands().size() != 1)
    return false;
  const MachineMemOperand &Memory = *Instruction.memOperands().front();
  const MachineMemOperandDesc &Description = Memory.description();
  return !Memory.isAtomic() && Description.AddressSpace == 0 && Description.Size.isKnown() &&
         !Description.Size.isScalable() && Description.Size.minimumBytes() != 0;
}

[[nodiscard]] bool isSupportedAtomicMemoryAccess(const MachineInstr &Instruction,
                                                 AtomicOperation Operation,
                                                 std::uint64_t Bytes) noexcept {
  if (Instruction.memOperands().size() != 1 || (Bytes != 4 && Bytes != 8))
    return false;
  const MachineMemOperand &Memory = *Instruction.memOperands().front();
  const MachineMemOperandDesc &Description = Memory.description();
  return Description.AtomicKind == Operation && Description.AddressSpace == 0 &&
         Description.Size.isKnown() && !Description.Size.isScalable() &&
         Description.Size.minimumBytes() == Bytes && Description.Align.bytes() >= Bytes &&
         Description.SyncScope.value() == 0;
}

[[nodiscard]] bool isSupportedAtomicContainerAccess(const MachineInstr &Instruction,
                                                    AtomicOperation Operation,
                                                    std::uint64_t ContainerBytes) noexcept {
  if (Instruction.memOperands().size() != 1 || ContainerBytes == 0)
    return false;
  const MachineMemOperandDesc &Description = Instruction.memOperands().front()->description();
  if (Description.AtomicKind != Operation || Description.AddressSpace != 0 ||
      !Description.Size.isKnown() || Description.Size.isScalable() ||
      Description.SyncScope.value() != 0)
    return false;
  const std::uint64_t MemoryBytes = Description.Size.minimumBytes();
  return (MemoryBytes == 1 || MemoryBytes == 2 || MemoryBytes == 4 || MemoryBytes == 8) &&
         MemoryBytes <= ContainerBytes && Description.Align.bytes() >= MemoryBytes;
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

[[nodiscard]] bool memoryMatchesType(const MachineInstr &Instruction, LLT Type) noexcept {
  const auto Bytes = memoryAccessBytes(Instruction);
  if (!Bytes || (!isSupportedIntegerType(Type) && !isRVFloatType(Type)))
    return false;
  if (isRVFloatType(Type))
    return Type.scalarBitWidth() == 32 ? *Bytes == 4 : *Bytes == 8;
  const std::uint64_t ValueBytes = (Type.scalarBitWidth() + 7U) / 8U;
  return (*Bytes == 1 || *Bytes == 2 || *Bytes == 4 || *Bytes == 8) && *Bytes <= ValueBytes;
}

[[nodiscard]] Expected<RegisterClassID> requiredRegisterClass(const MachineFunction &Function,
                                                              Register Reg,
                                                              const TargetDescription &Target) {
  if (!Reg.isVirtual())
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "RISC-V register-class selection requires a virtual register");
  auto Info = Function.registerInfo().virtualRegisterInfo(Reg);
  if (!Info)
    return std::unexpected(Info.error());
  if (!Info->get().Type) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V selection requires a type for every virtual register");
  }
  const VirtualRegisterInfo &RegisterInfo = Info->get();
  const LLT Type = RegisterInfo.Type.value_or(LLT{});
  RegisterClassID RequiredClass;
  if (isScalarGPRType(Type)) {
    RequiredClass = RegisterClassID::fromValue(static_cast<std::uint32_t>(RegisterClass::GPR));
  } else if (Type.isFloat() && Type.scalarBitWidth() == 32) {
    RequiredClass = RegisterClassID::fromValue(static_cast<std::uint32_t>(RegisterClass::FPR32));
  } else if (Type.isFloat() && Type.scalarBitWidth() == 64) {
    RequiredClass = RegisterClassID::fromValue(static_cast<std::uint32_t>(RegisterClass::FPR64));
  } else {
    return makeUnexpected(ErrorCode::Unsupported,
                          "RISC-V RV64GC has no register class for type '" + Type.toString() + "'");
  }

  if (Target.registers().registerClass(RequiredClass) == nullptr) {
    return makeUnexpected(ErrorCode::InternalError,
                          "RISC-V target description is missing a required register class");
  }
  if (RegisterInfo.Bank && !Target.registers().bankCoversClass(*RegisterInfo.Bank, RequiredClass)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V virtual register bank cannot cover its required class");
  }
  if (RegisterInfo.RegClass &&
      !Target.registers().isSubclass(*RegisterInfo.RegClass, RequiredClass)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V virtual register has a conflicting register class");
  }
  return RequiredClass;
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

[[nodiscard]] Expected<void> insertVirtualUse(MachineInstr &Instruction, std::size_t Index,
                                              Register Reg) {
  auto Operand = MachineOperand::registerOperand(Reg, {RegisterOperandFlag::Use});
  if (!Operand)
    return std::unexpected(Operand.error());
  return Instruction.insertOperand(Index, std::move(*Operand));
}

[[nodiscard]] Expected<Register> createTemporary(MachineFunction &Function, LLT Type,
                                                 const TargetDescription &Target) {
  auto Temporary = Function.registerInfo().createVirtualRegister(Type);
  if (!Temporary)
    return std::unexpected(Temporary.error());
  auto RequiredClass = requiredRegisterClass(Function, *Temporary, Target);
  if (!RequiredClass)
    return std::unexpected(RequiredClass.error());
  const RegisterBank BankKind = Type.isFloat() ? RegisterBank::FPR : RegisterBank::GPR;
  auto Bank = Function.registerInfo().setRegisterBank(
      *Temporary, RegisterBankID::fromValue(static_cast<std::uint32_t>(BankKind)));
  if (!Bank)
    return std::unexpected(Bank.error());
  auto Class = Function.registerInfo().setRegisterClass(*Temporary, *RequiredClass);
  if (!Class)
    return std::unexpected(Class.error());
  return *Temporary;
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

[[nodiscard]] Expected<void> replaceRegisterUses(MachineFunction &Function, Register OldReg,
                                                 Register NewReg) {
  std::vector<MachineOperand *> Uses;
  for (MachineOperand &Operand : Function.registerInfo().useOperands(OldReg))
    Uses.push_back(&Operand);
  for (MachineOperand *Operand : Uses) {
    auto Replaced = Operand->setRegister(NewReg);
    if (!Replaced)
      return std::unexpected(Replaced.error());
  }
  return {};
}

[[nodiscard]] Expected<void> eraseInstruction(MachineInstr &Instruction) {
  MachineBasicBlock *Block = Instruction.parent();
  if (Block == nullptr)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V legalizer cannot erase a detached instruction");
  return Block->removeInstruction(Instruction);
}

[[nodiscard]] bool allDefsUnused(const MachineInstr &Instruction,
                                 const MachineFunction &Function) {
  for (const MachineOperand &Operand : Instruction.operands()) {
    const Register *Reg = Operand.getIf<Register>();
    if (Reg != nullptr && Operand.isDef() && Reg->isVirtual() &&
        !Function.registerInfo().useEmpty(*Reg))
      return false;
  }
  return true;
}

void eraseDeadArtifacts(std::span<MachineInstr *const> Candidates,
                        const MachineFunction &Function) {
  bool Changed = false;
  do {
    Changed = false;
    for (auto It = Candidates.rbegin(); It != Candidates.rend(); ++It) {
      MachineInstr *Instruction = *It;
      if (Instruction == nullptr || Instruction->parent() == nullptr ||
          !allDefsUnused(*Instruction, Function))
        continue;
      const Opcode Value = Instruction->opcode();
      if (Value != opcode(GenericOpcode::G_TRUNC) &&
          Value != opcode(GenericOpcode::G_ANYEXT) &&
          Value != opcode(GenericOpcode::G_SEXT) && Value != opcode(GenericOpcode::G_ZEXT) &&
          Value != opcode(GenericOpcode::G_MERGE_VALUES) &&
          Value != opcode(GenericOpcode::G_UNMERGE_VALUES) &&
          Value != opcode(GenericOpcode::COPY))
        continue;
      if (eraseInstruction(*Instruction))
        Changed = true;
    }
  } while (Changed);
}

[[nodiscard]] Expected<Register> buildIntegerConstantBefore(MachineIRBuilder &Builder,
                                                            MachineInstr &Anchor,
                                                            std::uint32_t Width,
                                                            std::uint64_t Value) {
  auto Type = LLT::makeAnyScalar(Width);
  if (!Type)
    return std::unexpected(Type.error());
  auto Result = Builder.function().registerInfo().createVirtualRegister(*Type);
  if (!Result)
    return std::unexpected(Result.error());
  auto Immediate = IntegerImmediate::fromUnsigned(Width, Value);
  if (!Immediate)
    return std::unexpected(Immediate.error());
  auto Point = Builder.setInsertPointBefore(Anchor);
  if (!Point)
    return std::unexpected(Point.error());
  auto Pending = Builder.buildInstr(opcode(GenericOpcode::G_CONSTANT));
  if (!Pending)
    return std::unexpected(Pending.error());
  if (auto Added = Pending->addDef(*Result); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Pending->addIntegerImmediate(std::move(*Immediate)); !Added)
    return std::unexpected(Added.error());
  auto Built = Pending->build();
  if (!Built)
    return std::unexpected(Built.error());
  return *Result;
}

[[nodiscard]] Expected<Register> buildUnaryBefore(MachineIRBuilder &Builder, MachineInstr &Anchor,
                                                  GenericOpcode Generic, LLT Type,
                                                  Register Source) {
  auto Result = Builder.function().registerInfo().createVirtualRegister(Type);
  if (!Result)
    return std::unexpected(Result.error());
  auto Point = Builder.setInsertPointBefore(Anchor);
  if (!Point)
    return std::unexpected(Point.error());
  auto Pending = Builder.buildInstr(opcode(Generic));
  if (!Pending)
    return std::unexpected(Pending.error());
  if (auto Added = Pending->addDef(*Result); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Pending->addUse(Source); !Added)
    return std::unexpected(Added.error());
  auto Built = Pending->build();
  if (!Built)
    return std::unexpected(Built.error());
  return *Result;
}

[[nodiscard]] Expected<Register> buildBinaryBefore(MachineIRBuilder &Builder, MachineInstr &Anchor,
                                                   GenericOpcode Generic, LLT Type, Register Left,
                                                   Register Right) {
  auto Result = Builder.function().registerInfo().createVirtualRegister(Type);
  if (!Result)
    return std::unexpected(Result.error());
  auto Point = Builder.setInsertPointBefore(Anchor);
  if (!Point)
    return std::unexpected(Point.error());
  auto Pending = Builder.buildInstr(opcode(Generic));
  if (!Pending)
    return std::unexpected(Pending.error());
  if (auto Added = Pending->addDef(*Result); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Pending->addUse(Left); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Pending->addUse(Right); !Added)
    return std::unexpected(Added.error());
  auto Built = Pending->build();
  if (!Built)
    return std::unexpected(Built.error());
  return *Result;
}

[[nodiscard]] Expected<void> rewriteUnary(MachineInstr &Instruction, GenericOpcode Generic,
                                          Register Destination, Register Source) {
  auto SetOpcode = Instruction.setOpcode(opcode(Generic));
  if (!SetOpcode)
    return std::unexpected(SetOpcode.error());
  while (Instruction.operandCount() != 0) {
    auto Erased = Instruction.eraseOperand(Instruction.operandCount() - 1U);
    if (!Erased)
      return std::unexpected(Erased.error());
  }
  auto Def = MachineOperand::registerOperand(
      Destination, RegisterOperandFlags{RegisterOperandFlag::Define});
  if (!Def)
    return std::unexpected(Def.error());
  if (auto Added = Instruction.addOperand(std::move(*Def)); !Added)
    return std::unexpected(Added.error());
  auto Use = MachineOperand::registerOperand(Source, RegisterOperandFlags{RegisterOperandFlag::Use});
  if (!Use)
    return std::unexpected(Use.error());
  return Instruction.addOperand(std::move(*Use));
}

[[nodiscard]] Expected<void> rewriteBinary(MachineInstr &Instruction, GenericOpcode Generic,
                                           Register Destination, Register Left, Register Right) {
  auto Rewritten = rewriteUnary(Instruction, Generic, Destination, Left);
  if (!Rewritten)
    return std::unexpected(Rewritten.error());
  auto Use = MachineOperand::registerOperand(Right, RegisterOperandFlags{RegisterOperandFlag::Use});
  if (!Use)
    return std::unexpected(Use.error());
  return Instruction.addOperand(std::move(*Use));
}

[[nodiscard]] bool isKnownZeroExtended(const MachineFunction &Function, Register Reg,
                                       std::uint32_t ActiveBits, unsigned Depth = 0) {
  if (Depth > 8 || !Reg.isVirtual())
    return false;
  const auto Info = Function.registerInfo().virtualRegisterInfo(Reg);
  if (!Info || !Info->get().Type)
    return false;
  const std::uint32_t Width = Info->get().Type->scalarBitWidth();
  if (Width <= ActiveBits)
    return true;
  const MachineOperand *Definition = Function.registerInfo().getUniqueVRegDef(Reg);
  const MachineInstr *Def = Definition == nullptr ? nullptr : Definition->parent();
  if (Def == nullptr)
    return false;
  if (Def->opcode() == opcode(GenericOpcode::G_ASSERT_ZEXT)) {
    const MachineOperand *BitsOperand = Def->operandAt(2);
    auto Bits = BitsOperand == nullptr
                    ? Expected<std::int64_t>(std::unexpected(
                          Error(ErrorCode::InvalidArgument, "missing asserted extension width")))
                    : signedImmediate(*BitsOperand);
    return Bits && *Bits >= 0 && static_cast<std::uint64_t>(*Bits) <= ActiveBits;
  }
  if (Def->opcode() == opcode(GenericOpcode::G_ZEXT)) {
    const MachineOperand *SourceOperand = Def->operandAt(1);
    const Register *Source = SourceOperand == nullptr ? nullptr : SourceOperand->getIf<Register>();
    if (Source == nullptr)
      return false;
    const auto SourceInfo = Function.registerInfo().virtualRegisterInfo(*Source);
    return SourceInfo && SourceInfo->get().Type &&
           SourceInfo->get().Type->scalarBitWidth() <= ActiveBits;
  }
  if (Def->opcode() == opcode(GenericOpcode::G_TRUNC)) {
    const MachineOperand *SourceOperand = Def->operandAt(1);
    const Register *Source = SourceOperand == nullptr ? nullptr : SourceOperand->getIf<Register>();
    return Source != nullptr && isKnownZeroExtended(Function, *Source, ActiveBits, Depth + 1U);
  }
  return false;
}

[[nodiscard]] Expected<Register>
zeroExtendToRV64(MachineIRBuilder &Builder, MachineInstr &Anchor, MachineFunction &Function,
                 Register Source, std::vector<MachineInstr *> &DeadCandidates) {
  const auto Info = Function.registerInfo().virtualRegisterInfo(Source);
  if (!Info || !Info->get().Type)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V merge source has no scalar type");
  const LLT SourceType = *Info->get().Type;
  if (SourceType.scalarBitWidth() == 64)
    return Source;
  const MachineOperand *Definition = Function.registerInfo().getUniqueVRegDef(Source);
  MachineInstr *Def = Definition == nullptr ? nullptr : const_cast<MachineInstr *>(Definition->parent());
  if (Def != nullptr && Def->opcode() == opcode(GenericOpcode::G_TRUNC)) {
    const MachineOperand *WideOperand = Def->operandAt(1);
    const Register *Wide = WideOperand == nullptr ? nullptr : WideOperand->getIf<Register>();
    if (Wide != nullptr) {
      const auto WideInfo = Function.registerInfo().virtualRegisterInfo(*Wide);
      if (WideInfo && WideInfo->get().Type && WideInfo->get().Type->scalarBitWidth() == 64 &&
          isKnownZeroExtended(Function, *Wide, SourceType.scalarBitWidth())) {
        DeadCandidates.push_back(Def);
        return *Wide;
      }
    }
  }
  auto WideType = LLT::makeAnyScalar(64);
  if (!WideType)
    return std::unexpected(WideType.error());
  return buildUnaryBefore(Builder, Anchor, GenericOpcode::G_ZEXT, *WideType, Source);
}

[[nodiscard]] MachineInstr *singleUseInstruction(const MachineFunction &Function, Register Reg) {
  MachineInstr *Result = nullptr;
  auto &RegisterInfo = const_cast<MachineRegisterInfo &>(Function.registerInfo());
  for (MachineOperand &Operand : RegisterInfo.useOperands(Reg)) {
    MachineInstr *Instruction = Operand.parent();
    if (Instruction == nullptr || Instruction->parent() == nullptr)
      continue;
    if (Result != nullptr && Result != Instruction)
      return nullptr;
    Result = Instruction;
  }
  return Result;
}

[[nodiscard]] Expected<void> lowerWideMergeValues(MachineInstr &Instruction,
                                                  MachineFunction &Function) {
  const MachineOperand *DestinationOperand = Instruction.operandAt(0);
  const Register *Destination =
      DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
  const MachineOperand *LowOperand = Instruction.operandAt(1);
  const Register *Low = LowOperand == nullptr ? nullptr : LowOperand->getIf<Register>();
  if (Destination == nullptr || Low == nullptr)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V wide merge has invalid register operands");

  std::vector<MachineInstr *> Truncations;
  auto &RegisterInfo = Function.registerInfo();
  std::vector<MachineOperand *> Uses;
  for (MachineOperand &Use : RegisterInfo.useOperands(*Destination))
    Uses.push_back(&Use);
  for (MachineOperand *Use : Uses) {
    MachineInstr *Trunc = Use->parent();
    if (Trunc == nullptr || Trunc->parent() == nullptr ||
        Trunc->opcode() != opcode(GenericOpcode::G_TRUNC) || Trunc->operandAt(1) != Use)
      return makeUnexpected(ErrorCode::Unsupported,
                            "RISC-V wide merge is only representable through low-part truncation");
    const MachineOperand *TruncResultOperand = Trunc->operandAt(0);
    const Register *TruncResult =
        TruncResultOperand == nullptr ? nullptr : TruncResultOperand->getIf<Register>();
    const auto LowInfo = RegisterInfo.virtualRegisterInfo(*Low);
    const auto ResultInfo = TruncResult == nullptr
                                ? Expected<std::reference_wrapper<const VirtualRegisterInfo>>(
                                      std::unexpected(Error(ErrorCode::InvalidArgument,
                                                            "missing truncation result")))
                                : RegisterInfo.virtualRegisterInfo(*TruncResult);
    if (!LowInfo || !LowInfo->get().Type || !ResultInfo || !ResultInfo->get().Type ||
        ResultInfo->get().Type->scalarBitWidth() > LowInfo->get().Type->scalarBitWidth())
      return makeUnexpected(ErrorCode::Unsupported,
                            "RISC-V wide merge truncation spans more than its low source");
    if (*ResultInfo->get().Type == *LowInfo->get().Type) {
      auto Replaced = replaceRegisterUses(Function, *TruncResult, *Low);
      if (!Replaced)
        return std::unexpected(Replaced.error());
      Truncations.push_back(Trunc);
    } else {
      auto Replaced = Use->setRegister(*Low);
      if (!Replaced)
        return std::unexpected(Replaced.error());
    }
  }
  for (MachineInstr *Trunc : Truncations) {
    auto Erased = eraseInstruction(*Trunc);
    if (!Erased)
      return std::unexpected(Erased.error());
  }
  return eraseInstruction(Instruction);
}

[[nodiscard]] Expected<void> lowerMergeValues(MachineInstr &Instruction,
                                              MachineFunction &Function,
                                              const TargetDescription &Target) {
  const std::optional<LLT> DestinationType = resultType(Function, Instruction);
  if (!DestinationType || DestinationType->scalarBitWidth() == 0)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V merge destination has no scalar type");
  if (DestinationType->scalarBitWidth() > 64)
    return lowerWideMergeValues(Instruction, Function);

  const MachineOperand *DestinationOperand = Instruction.operandAt(0);
  const Register *Destination =
      DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
  if (Destination == nullptr || Instruction.operandCount() < 3)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V merge has invalid register operands");

  Register Result = *Destination;
  MachineInstr *FoldedExtension = nullptr;
  if (DestinationType->scalarBitWidth() < 64) {
    MachineInstr *Use = singleUseInstruction(Function, *Destination);
    if (Use != nullptr && Use->opcode() == opcode(GenericOpcode::G_ZEXT) &&
        operandType(Function, *Use, 0).transform([](LLT Type) {
          return Type.scalarBitWidth() == 64;
        }).value_or(false)) {
      const MachineOperand *ExtendedOperand = Use->operandAt(0);
      const Register *Extended =
          ExtendedOperand == nullptr ? nullptr : ExtendedOperand->getIf<Register>();
      if (Extended != nullptr) {
        Result = *Extended;
        FoldedExtension = Use;
      }
    }
  }

  auto WideType = LLT::makeAnyScalar(64);
  if (!WideType)
    return std::unexpected(WideType.error());
  MachineIRBuilder Builder(Function, Target);
  std::vector<MachineInstr *> DeadCandidates;
  std::vector<Register> Terms;
  Terms.reserve(Instruction.operandCount() - 1U);
  for (std::size_t Index = 1; Index < Instruction.operandCount(); ++Index) {
    const MachineOperand *Operand = Instruction.operandAt(Index);
    const Register *Source = Operand == nullptr ? nullptr : Operand->getIf<Register>();
    if (Source == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V merge source is not a register");
    auto WideSource = zeroExtendToRV64(Builder, Instruction, Function, *Source, DeadCandidates);
    if (!WideSource)
      return std::unexpected(WideSource.error());
    if (Index == 1) {
      Terms.push_back(*WideSource);
      continue;
    }
    const std::optional<LLT> SourceType = registerType(Function, *Operand);
    if (!SourceType)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V merge source has no scalar type");
    const std::uint64_t Shift =
        static_cast<std::uint64_t>(SourceType->scalarBitWidth()) * (Index - 1U);
    auto Amount = buildIntegerConstantBefore(Builder, Instruction, 64, Shift);
    if (!Amount)
      return std::unexpected(Amount.error());
    auto Shifted = buildBinaryBefore(Builder, Instruction, GenericOpcode::G_SHL, *WideType,
                                     *WideSource, *Amount);
    if (!Shifted)
      return std::unexpected(Shifted.error());
    Terms.push_back(*Shifted);
  }

  Register Accumulator = Terms.front();
  for (std::size_t Index = 1; Index + 1U < Terms.size(); ++Index) {
    auto Combined = buildBinaryBefore(Builder, Instruction, GenericOpcode::G_OR, *WideType,
                                      Accumulator, Terms[Index]);
    if (!Combined)
      return std::unexpected(Combined.error());
    Accumulator = *Combined;
  }
  if (DestinationType->scalarBitWidth() == 64 || FoldedExtension != nullptr) {
    auto Rewritten = rewriteBinary(Instruction, GenericOpcode::G_OR, Result, Accumulator,
                                   Terms.back());
    if (!Rewritten)
      return std::unexpected(Rewritten.error());
  } else {
    auto Combined = buildBinaryBefore(Builder, Instruction, GenericOpcode::G_OR, *WideType,
                                      Accumulator, Terms.back());
    if (!Combined)
      return std::unexpected(Combined.error());
    auto Rewritten = rewriteUnary(Instruction, GenericOpcode::G_TRUNC, *Destination, *Combined);
    if (!Rewritten)
      return std::unexpected(Rewritten.error());
  }
  if (FoldedExtension != nullptr) {
    auto Erased = eraseInstruction(*FoldedExtension);
    if (!Erased)
      return std::unexpected(Erased.error());
  }
  eraseDeadArtifacts(DeadCandidates, Function);
  return {};
}

[[nodiscard]] Expected<void>
foldWideExtendedUnmerge(MachineInstr &Instruction, MachineFunction &Function) {
  const std::size_t OutputCount = Instruction.operandCount() - 1U;
  const MachineOperand *SourceOperand = Instruction.operandAt(OutputCount);
  const Register *Source = SourceOperand == nullptr ? nullptr : SourceOperand->getIf<Register>();
  const MachineOperand *Definition =
      Source == nullptr ? nullptr : Function.registerInfo().getUniqueVRegDef(*Source);
  MachineInstr *Extension =
      Definition == nullptr ? nullptr : const_cast<MachineInstr *>(Definition->parent());
  if (Extension == nullptr ||
      (Extension->opcode() != opcode(GenericOpcode::G_ZEXT) &&
       Extension->opcode() != opcode(GenericOpcode::G_ANYEXT) &&
       Extension->opcode() != opcode(GenericOpcode::G_SEXT)))
    return makeUnexpected(ErrorCode::Unsupported,
                          "RISC-V wide unmerge requires an extension-defined source");
  const MachineOperand *NarrowOperand = Extension->operandAt(1);
  const Register *Narrow = NarrowOperand == nullptr ? nullptr : NarrowOperand->getIf<Register>();
  if (Narrow == nullptr)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V wide unmerge extension has no register source");
  const auto NarrowInfo = Function.registerInfo().virtualRegisterInfo(*Narrow);
  if (!NarrowInfo || !NarrowInfo->get().Type)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V wide unmerge extension source has no type");
  const std::uint32_t NarrowWidth = NarrowInfo->get().Type->scalarBitWidth();

  std::vector<MachineInstr *> DeadUsers;
  for (std::size_t Index = 0; Index < OutputCount; ++Index) {
    const MachineOperand *OutputOperand = Instruction.operandAt(Index);
    const Register *Output = OutputOperand == nullptr ? nullptr : OutputOperand->getIf<Register>();
    if (Output == nullptr || Function.registerInfo().useEmpty(*Output))
      continue;
    const auto OutputInfo = Function.registerInfo().virtualRegisterInfo(*Output);
    if (!OutputInfo || !OutputInfo->get().Type)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V wide unmerge output has no type");
    const std::uint64_t Offset =
        static_cast<std::uint64_t>(Index) * OutputInfo->get().Type->scalarBitWidth();
    if (Offset != 0)
      return makeUnexpected(ErrorCode::Unsupported,
                            "RISC-V wide unmerge cannot materialize a used high wide part");
    if (OutputInfo->get().Type->scalarBitWidth() == NarrowWidth) {
      auto Replaced = replaceRegisterUses(Function, *Output, *Narrow);
      if (!Replaced)
        return std::unexpected(Replaced.error());
      continue;
    }
    if (OutputInfo->get().Type->scalarBitWidth() < NarrowWidth)
      return makeUnexpected(ErrorCode::Unsupported,
                            "RISC-V wide unmerge output unexpectedly narrows its source");
    std::vector<MachineOperand *> Uses;
    for (MachineOperand &Use : Function.registerInfo().useOperands(*Output))
      Uses.push_back(&Use);
    for (MachineOperand *Use : Uses) {
      MachineInstr *Trunc = Use->parent();
      if (Trunc == nullptr || Trunc->parent() == nullptr ||
          Trunc->opcode() != opcode(GenericOpcode::G_TRUNC) || Trunc->operandAt(1) != Use)
        return makeUnexpected(ErrorCode::Unsupported,
                              "RISC-V wide unmerge result is only representable through truncation");
      const MachineOperand *ResultOperand = Trunc->operandAt(0);
      const Register *Result = ResultOperand == nullptr ? nullptr : ResultOperand->getIf<Register>();
      const auto ResultInfo = Result == nullptr
                                  ? Expected<std::reference_wrapper<const VirtualRegisterInfo>>(
                                        std::unexpected(Error(ErrorCode::InvalidArgument,
                                                              "missing truncation result")))
                                  : Function.registerInfo().virtualRegisterInfo(*Result);
      if (!ResultInfo || !ResultInfo->get().Type ||
          ResultInfo->get().Type->scalarBitWidth() > NarrowWidth)
        return makeUnexpected(ErrorCode::Unsupported,
                              "RISC-V wide unmerge truncation exceeds its source width");
      if (ResultInfo->get().Type->scalarBitWidth() == NarrowWidth) {
        auto Replaced = replaceRegisterUses(Function, *Result, *Narrow);
        if (!Replaced)
          return std::unexpected(Replaced.error());
        DeadUsers.push_back(Trunc);
      } else {
        auto Replaced = Use->setRegister(*Narrow);
        if (!Replaced)
          return std::unexpected(Replaced.error());
      }
    }
  }
  for (MachineInstr *User : DeadUsers) {
    auto Erased = eraseInstruction(*User);
    if (!Erased)
      return std::unexpected(Erased.error());
  }
  auto Erased = eraseInstruction(Instruction);
  if (!Erased)
    return std::unexpected(Erased.error());
  if (Function.registerInfo().useEmpty(*Source))
    return eraseInstruction(*Extension);
  return {};
}

[[nodiscard]] Expected<void> lowerUnmergeValues(MachineInstr &Instruction,
                                                MachineFunction &Function,
                                                const TargetDescription &Target) {
  if (Instruction.operandCount() < 3)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V unmerge has invalid register operands");
  const std::size_t OutputCount = Instruction.operandCount() - 1U;
  const std::optional<LLT> OutputType = operandType(Function, Instruction, 0);
  const std::optional<LLT> SourceType = operandType(Function, Instruction, OutputCount);
  if (!OutputType || !SourceType)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V unmerge operands have no scalar types");
  if (SourceType->scalarBitWidth() > 64 || OutputType->scalarBitWidth() > 64)
    return foldWideExtendedUnmerge(Instruction, Function);

  const MachineOperand *SourceOperand = Instruction.operandAt(OutputCount);
  const Register *Source = SourceOperand == nullptr ? nullptr : SourceOperand->getIf<Register>();
  if (Source == nullptr)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V unmerge source is not a register");
  auto WideType = LLT::makeAnyScalar(64);
  if (!WideType)
    return std::unexpected(WideType.error());
  MachineIRBuilder Builder(Function, Target);
  std::vector<MachineInstr *> FoldedExtensions;
  for (std::size_t Index = 0; Index < OutputCount; ++Index) {
    const MachineOperand *OutputOperand = Instruction.operandAt(Index);
    const Register *Output = OutputOperand == nullptr ? nullptr : OutputOperand->getIf<Register>();
    if (Output == nullptr || Function.registerInfo().useEmpty(*Output))
      continue;
    Register Extracted = *Source;
    const std::uint64_t Offset =
        static_cast<std::uint64_t>(Index) * OutputType->scalarBitWidth();
    if (Offset != 0) {
      auto Amount = buildIntegerConstantBefore(Builder, Instruction, 64, Offset);
      if (!Amount)
        return std::unexpected(Amount.error());
      auto Shifted = buildBinaryBefore(Builder, Instruction, GenericOpcode::G_LSHR, *WideType,
                                       *Source, *Amount);
      if (!Shifted)
        return std::unexpected(Shifted.error());
      Extracted = *Shifted;
    }

    MachineInstr *Extension = singleUseInstruction(Function, *Output);
    const bool FoldZExt = Extension != nullptr &&
                          Extension->opcode() == opcode(GenericOpcode::G_ZEXT) &&
                          operandType(Function, *Extension, 0)
                              .transform([](LLT Type) { return Type.scalarBitWidth() == 64; })
                              .value_or(false);
    if (FoldZExt) {
      const MachineOperand *ExtendedOperand = Extension->operandAt(0);
      const Register *Extended =
          ExtendedOperand == nullptr ? nullptr : ExtendedOperand->getIf<Register>();
      if (Extended == nullptr)
        return makeUnexpected(ErrorCode::VerificationError,
                              "RISC-V unmerge extension has no destination");
      const std::uint64_t Mask = OutputType->scalarBitWidth() == 64
                                     ? std::numeric_limits<std::uint64_t>::max()
                                     : (std::uint64_t{1} << OutputType->scalarBitWidth()) - 1U;
      auto MaskReg = buildIntegerConstantBefore(Builder, Instruction, 64, Mask);
      if (!MaskReg)
        return std::unexpected(MaskReg.error());
      auto Masked = buildBinaryBefore(Builder, Instruction, GenericOpcode::G_AND, *WideType,
                                      Extracted, *MaskReg);
      if (!Masked)
        return std::unexpected(Masked.error());
      auto Replaced = replaceRegisterUses(Function, *Extended, *Masked);
      if (!Replaced)
        return std::unexpected(Replaced.error());
      FoldedExtensions.push_back(Extension);
      continue;
    }
    auto Truncated = buildUnaryBefore(Builder, Instruction, GenericOpcode::G_TRUNC, *OutputType,
                                      Extracted);
    if (!Truncated)
      return std::unexpected(Truncated.error());
    auto Replaced = replaceRegisterUses(Function, *Output, *Truncated);
    if (!Replaced)
      return std::unexpected(Replaced.error());
  }
  for (MachineInstr *Extension : FoldedExtensions) {
    auto Erased = eraseInstruction(*Extension);
    if (!Erased)
      return std::unexpected(Erased.error());
  }
  return eraseInstruction(Instruction);
}

struct WideValueParts final {
  std::uint32_t BitWidth = 0;
  std::vector<Register> Parts;
  std::vector<MachineInstr *> Artifacts;
};

[[nodiscard]] Expected<WideValueParts> collectWideValueParts(Register Value,
                                                             MachineFunction &Function,
                                                             unsigned Depth = 0) {
  if (Depth > 16)
    return makeUnexpected(ErrorCode::Unsupported,
                          "RISC-V wide-value artifact chain is too deep");
  const auto Info = Function.registerInfo().virtualRegisterInfo(Value);
  if (!Info || !Info->get().Type)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V wide value has no scalar type");
  const LLT Type = *Info->get().Type;
  if ((!Type.isInteger() && !Type.isAnyScalar()) || Type.scalarBitWidth() == 0)
    return makeUnexpected(ErrorCode::Unsupported,
                          "RISC-V wide-value decomposition requires an integer scalar");
  if (Type.scalarBitWidth() <= 64)
    return WideValueParts{
        .BitWidth = Type.scalarBitWidth(), .Parts = {Value}, .Artifacts = {}};

  const MachineOperand *Definition = Function.registerInfo().getUniqueVRegDef(Value);
  MachineInstr *Def =
      Definition == nullptr ? nullptr : const_cast<MachineInstr *>(Definition->parent());
  if (Def == nullptr)
    return makeUnexpected(ErrorCode::Unsupported,
                          "RISC-V wide value has no decomposable definition");
  if (Def->opcode() == opcode(GenericOpcode::G_MERGE_VALUES)) {
    WideValueParts Result;
    Result.BitWidth = Type.scalarBitWidth();
    for (std::size_t Index = 1; Index < Def->operandCount(); ++Index) {
      const MachineOperand *Operand = Def->operandAt(Index);
      const Register *Part = Operand == nullptr ? nullptr : Operand->getIf<Register>();
      const std::optional<LLT> PartType =
          Operand == nullptr ? std::nullopt : registerType(Function, *Operand);
      if (Part == nullptr || !PartType || PartType->scalarBitWidth() != 64)
        return makeUnexpected(ErrorCode::Unsupported,
                              "RISC-V wide merge must already use 64-bit limbs");
      Result.Parts.push_back(*Part);
    }
    Result.Artifacts.push_back(Def);
    return Result;
  }
  if (Def->opcode() == opcode(GenericOpcode::G_TRUNC)) {
    const MachineOperand *SourceOperand = Def->operandAt(1);
    const Register *Source = SourceOperand == nullptr ? nullptr : SourceOperand->getIf<Register>();
    if (Source == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V wide truncation has no source register");
    auto Result = collectWideValueParts(*Source, Function, Depth + 1U);
    if (!Result)
      return std::unexpected(Result.error());
    Result->BitWidth = Type.scalarBitWidth();
    const std::size_t RequiredParts = (Type.scalarBitWidth() + 63U) / 64U;
    if (Result->Parts.size() < RequiredParts)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V wide truncation source has too few limbs");
    Result->Parts.resize(RequiredParts);
    Result->Artifacts.push_back(Def);
    return Result;
  }
  return makeUnexpected(ErrorCode::Unsupported,
                        "RISC-V wide value is not defined by merge/trunc artifacts");
}

struct WideResultPlan final {
  std::vector<Register> Outputs;
  MachineInstr *Extension = nullptr;
  MachineInstr *Unmerge = nullptr;
};

[[nodiscard]] Expected<WideResultPlan> wideResultPlan(Register Result,
                                                      MachineFunction &Function,
                                                      std::size_t RequiredParts) {
  MachineInstr *Use = singleUseInstruction(Function, Result);
  WideResultPlan Plan;
  if (Use != nullptr &&
      (Use->opcode() == opcode(GenericOpcode::G_ANYEXT) ||
       Use->opcode() == opcode(GenericOpcode::G_SEXT) ||
       Use->opcode() == opcode(GenericOpcode::G_ZEXT))) {
    Plan.Extension = Use;
    const MachineOperand *ExtendedOperand = Use->operandAt(0);
    const Register *Extended =
        ExtendedOperand == nullptr ? nullptr : ExtendedOperand->getIf<Register>();
    Use = Extended == nullptr ? nullptr : singleUseInstruction(Function, *Extended);
  }
  if (Use == nullptr || Use->opcode() != opcode(GenericOpcode::G_UNMERGE_VALUES))
    return makeUnexpected(ErrorCode::Unsupported,
                          "RISC-V wide result must feed an unmerge artifact");
  Plan.Unmerge = Use;
  const std::size_t OutputCount = Use->operandCount() - 1U;
  if (OutputCount < RequiredParts)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V wide result unmerge has too few outputs");
  Plan.Outputs.reserve(RequiredParts);
  for (std::size_t Index = 0; Index < RequiredParts; ++Index) {
    const MachineOperand *Operand = Use->operandAt(Index);
    const Register *Output = Operand == nullptr ? nullptr : Operand->getIf<Register>();
    const std::optional<LLT> Type =
        Operand == nullptr ? std::nullopt : registerType(Function, *Operand);
    if (Output == nullptr || !Type || Type->scalarBitWidth() != 64)
      return makeUnexpected(ErrorCode::Unsupported,
                            "RISC-V wide result must unmerge to 64-bit limbs");
    Plan.Outputs.push_back(*Output);
  }
  return Plan;
}

[[nodiscard]] Expected<Register>
buildIntegerCompareBefore(MachineIRBuilder &Builder, MachineInstr &Anchor,
                          IntegerPredicate Predicate, Register Left, Register Right) {
  auto Type = LLT::makeAnyScalar(64);
  if (!Type)
    return std::unexpected(Type.error());
  auto Result = Builder.function().registerInfo().createVirtualRegister(*Type);
  if (!Result)
    return std::unexpected(Result.error());
  auto Point = Builder.setInsertPointBefore(Anchor);
  if (!Point)
    return std::unexpected(Point.error());
  auto Pending = Builder.buildInstr(opcode(GenericOpcode::G_ICMP));
  if (!Pending)
    return std::unexpected(Pending.error());
  if (auto Added = Pending->addDef(*Result); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Pending->addIntegerPredicate(Predicate); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Pending->addUse(Left); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Pending->addUse(Right); !Added)
    return std::unexpected(Added.error());
  auto Built = Pending->build();
  if (!Built)
    return std::unexpected(Built.error());
  return *Result;
}

[[nodiscard]] Expected<Register>
buildSelectBefore(MachineIRBuilder &Builder, MachineInstr &Anchor, LLT Type,
                  Register Condition, Register TrueValue, Register FalseValue) {
  auto Result = Builder.function().registerInfo().createVirtualRegister(Type);
  if (!Result)
    return std::unexpected(Result.error());
  auto Point = Builder.setInsertPointBefore(Anchor);
  if (!Point)
    return std::unexpected(Point.error());
  auto Pending = Builder.buildInstr(opcode(GenericOpcode::G_SELECT));
  if (!Pending)
    return std::unexpected(Pending.error());
  if (auto Added = Pending->addDef(*Result); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Pending->addUse(Condition); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Pending->addUse(TrueValue); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Pending->addUse(FalseValue); !Added)
    return std::unexpected(Added.error());
  auto Built = Pending->build();
  if (!Built)
    return std::unexpected(Built.error());
  return *Result;
}

struct TwoLimbShiftResult final {
  Register Low;
  Register High;
};

[[nodiscard]] Expected<TwoLimbShiftResult>
emitTwoLimbLeftShiftBody(MachineIRBuilder &Builder, MachineInstr &Anchor,
                         Register Low, Register High, Register Amount,
                         Register AmountExcess, Register AmountLack,
                         Register Zero, Register IsShort, Register IsZero) {
  auto Type = LLT::makeAnyScalar(64);
  if (!Type)
    return std::unexpected(Type.error());
  auto LowShort =
      buildBinaryBefore(Builder, Anchor, GenericOpcode::G_SHL, *Type, Low, Amount);
  if (!LowShort)
    return std::unexpected(LowShort.error());
  auto CrossLow =
      buildBinaryBefore(Builder, Anchor, GenericOpcode::G_LSHR, *Type, Low, AmountLack);
  if (!CrossLow)
    return std::unexpected(CrossLow.error());
  auto HighShort =
      buildBinaryBefore(Builder, Anchor, GenericOpcode::G_SHL, *Type, High, Amount);
  if (!HighShort)
    return std::unexpected(HighShort.error());
  auto ShortHigh = buildBinaryBefore(Builder, Anchor, GenericOpcode::G_OR, *Type,
                                     *CrossLow, *HighShort);
  if (!ShortHigh)
    return std::unexpected(ShortHigh.error());
  auto LongHigh =
      buildBinaryBefore(Builder, Anchor, GenericOpcode::G_SHL, *Type, Low, AmountExcess);
  if (!LongHigh)
    return std::unexpected(LongHigh.error());
  auto ResultLow =
      buildSelectBefore(Builder, Anchor, *Type, IsShort, *LowShort, Zero);
  if (!ResultLow)
    return std::unexpected(ResultLow.error());
  auto ShiftedHigh =
      buildSelectBefore(Builder, Anchor, *Type, IsShort, *ShortHigh, *LongHigh);
  if (!ShiftedHigh)
    return std::unexpected(ShiftedHigh.error());
  auto ResultHigh =
      buildSelectBefore(Builder, Anchor, *Type, IsZero, High, *ShiftedHigh);
  if (!ResultHigh)
    return std::unexpected(ResultHigh.error());
  return TwoLimbShiftResult{.Low = *ResultLow, .High = *ResultHigh};
}

[[nodiscard]] Expected<TwoLimbShiftResult>
emitTwoLimbLeftShift(MachineIRBuilder &Builder, MachineInstr &Anchor,
                     Register Low, Register High, Register Amount) {
  auto Type = LLT::makeAnyScalar(64);
  if (!Type)
    return std::unexpected(Type.error());
  auto Boundary = buildIntegerConstantBefore(Builder, Anchor, 64, 64);
  if (!Boundary)
    return std::unexpected(Boundary.error());
  auto AmountExcess = buildBinaryBefore(Builder, Anchor, GenericOpcode::G_SUB, *Type,
                                        Amount, *Boundary);
  if (!AmountExcess)
    return std::unexpected(AmountExcess.error());
  auto AmountLack = buildBinaryBefore(Builder, Anchor, GenericOpcode::G_SUB, *Type,
                                      *Boundary, Amount);
  if (!AmountLack)
    return std::unexpected(AmountLack.error());
  auto Zero = buildIntegerConstantBefore(Builder, Anchor, 64, 0);
  if (!Zero)
    return std::unexpected(Zero.error());
  auto IsShort = buildIntegerCompareBefore(Builder, Anchor,
                                           IntegerPredicate::UnsignedLessThan,
                                           Amount, *Boundary);
  if (!IsShort)
    return std::unexpected(IsShort.error());
  auto IsZero = buildIntegerCompareBefore(Builder, Anchor, IntegerPredicate::Equal,
                                          Amount, *Zero);
  if (!IsZero)
    return std::unexpected(IsZero.error());
  return emitTwoLimbLeftShiftBody(Builder, Anchor, Low, High, Amount,
                                  *AmountExcess, *AmountLack, *Zero, *IsShort,
                                  *IsZero);
}

[[nodiscard]] Expected<Register>
emitLowLimbOfTwoLimbLogicalRightShift(
    MachineIRBuilder &Builder, MachineInstr &Anchor, Register Low, Register High,
    Register Amount, Register Boundary, Register Zero) {
  auto Type = LLT::makeAnyScalar(64);
  if (!Type)
    return std::unexpected(Type.error());
  auto AmountExcess = buildBinaryBefore(Builder, Anchor, GenericOpcode::G_SUB, *Type,
                                        Amount, Boundary);
  if (!AmountExcess)
    return std::unexpected(AmountExcess.error());
  auto AmountLack = buildBinaryBefore(Builder, Anchor, GenericOpcode::G_SUB, *Type,
                                      Boundary, Amount);
  if (!AmountLack)
    return std::unexpected(AmountLack.error());
  auto IsShort = buildIntegerCompareBefore(Builder, Anchor,
                                           IntegerPredicate::UnsignedLessThan,
                                           Amount, Boundary);
  if (!IsShort)
    return std::unexpected(IsShort.error());
  auto IsZero = buildIntegerCompareBefore(Builder, Anchor, IntegerPredicate::Equal,
                                          Amount, Zero);
  if (!IsZero)
    return std::unexpected(IsZero.error());
  auto LowShort =
      buildBinaryBefore(Builder, Anchor, GenericOpcode::G_LSHR, *Type, Low, Amount);
  if (!LowShort)
    return std::unexpected(LowShort.error());
  auto HighShort =
      buildBinaryBefore(Builder, Anchor, GenericOpcode::G_SHL, *Type, High, *AmountLack);
  if (!HighShort)
    return std::unexpected(HighShort.error());
  auto ShortValue = buildBinaryBefore(Builder, Anchor, GenericOpcode::G_OR, *Type,
                                      *LowShort, *HighShort);
  if (!ShortValue)
    return std::unexpected(ShortValue.error());
  auto LongValue = buildBinaryBefore(Builder, Anchor, GenericOpcode::G_LSHR, *Type,
                                     High, *AmountExcess);
  if (!LongValue)
    return std::unexpected(LongValue.error());
  auto Shifted = buildSelectBefore(Builder, Anchor, *Type, *IsShort, *ShortValue,
                                   *LongValue);
  if (!Shifted)
    return std::unexpected(Shifted.error());
  return buildSelectBefore(Builder, Anchor, *Type, *IsZero, Low, *Shifted);
}

[[nodiscard]] Expected<std::vector<Register>>
emitThreeLimbLeftShift(MachineIRBuilder &Builder, MachineInstr &Anchor,
                       std::span<const Register> Parts, Register Amount) {
  if (Parts.size() != 3)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V three-limb shift requires exactly three limbs");
  auto Type = LLT::makeAnyScalar(64);
  if (!Type)
    return std::unexpected(Type.error());

  auto OuterBoundary = buildIntegerConstantBefore(Builder, Anchor, 64, 128);
  if (!OuterBoundary)
    return std::unexpected(OuterBoundary.error());
  auto OuterExcess = buildBinaryBefore(Builder, Anchor, GenericOpcode::G_SUB, *Type,
                                       Amount, *OuterBoundary);
  if (!OuterExcess)
    return std::unexpected(OuterExcess.error());
  auto OuterLack = buildBinaryBefore(Builder, Anchor, GenericOpcode::G_SUB, *Type,
                                     *OuterBoundary, Amount);
  if (!OuterLack)
    return std::unexpected(OuterLack.error());
  auto Zero = buildIntegerConstantBefore(Builder, Anchor, 64, 0);
  if (!Zero)
    return std::unexpected(Zero.error());
  auto OuterIsShort = buildIntegerCompareBefore(
      Builder, Anchor, IntegerPredicate::UnsignedLessThan, Amount, *OuterBoundary);
  if (!OuterIsShort)
    return std::unexpected(OuterIsShort.error());
  auto OuterIsZero = buildIntegerCompareBefore(Builder, Anchor, IntegerPredicate::Equal,
                                               Amount, *Zero);
  if (!OuterIsZero)
    return std::unexpected(OuterIsZero.error());

  auto InnerBoundary = buildIntegerConstantBefore(Builder, Anchor, 64, 64);
  if (!InnerBoundary)
    return std::unexpected(InnerBoundary.error());
  auto InnerExcess = buildBinaryBefore(Builder, Anchor, GenericOpcode::G_SUB, *Type,
                                       Amount, *InnerBoundary);
  if (!InnerExcess)
    return std::unexpected(InnerExcess.error());
  auto InnerLack = buildBinaryBefore(Builder, Anchor, GenericOpcode::G_SUB, *Type,
                                     *InnerBoundary, Amount);
  if (!InnerLack)
    return std::unexpected(InnerLack.error());
  auto InnerIsShort = buildIntegerCompareBefore(
      Builder, Anchor, IntegerPredicate::UnsignedLessThan, Amount, *InnerBoundary);
  if (!InnerIsShort)
    return std::unexpected(InnerIsShort.error());
  auto InnerIsZero = buildIntegerCompareBefore(Builder, Anchor, IntegerPredicate::Equal,
                                               Amount, *Zero);
  if (!InnerIsZero)
    return std::unexpected(InnerIsZero.error());
  auto Lower = emitTwoLimbLeftShiftBody(
      Builder, Anchor, Parts[0], Parts[1], Amount, *InnerExcess, *InnerLack,
      *Zero, *InnerIsShort, *InnerIsZero);
  if (!Lower)
    return std::unexpected(Lower.error());

  auto Cross = emitLowLimbOfTwoLimbLogicalRightShift(
      Builder, Anchor, Parts[0], Parts[1], *OuterLack, *InnerBoundary, *Zero);
  if (!Cross)
    return std::unexpected(Cross.error());
  auto HighIsShort = buildIntegerCompareBefore(
      Builder, Anchor, IntegerPredicate::UnsignedLessThan, Amount, *InnerBoundary);
  if (!HighIsShort)
    return std::unexpected(HighIsShort.error());
  auto HighShort = buildBinaryBefore(Builder, Anchor, GenericOpcode::G_SHL, *Type,
                                     Parts[2], Amount);
  if (!HighShort)
    return std::unexpected(HighShort.error());
  auto BoundedHigh = buildSelectBefore(Builder, Anchor, *Type, *HighIsShort,
                                       *HighShort, *Zero);
  if (!BoundedHigh)
    return std::unexpected(BoundedHigh.error());
  auto ShortHigh = buildBinaryBefore(Builder, Anchor, GenericOpcode::G_OR, *Type,
                                     *Cross, *BoundedHigh);
  if (!ShortHigh)
    return std::unexpected(ShortHigh.error());

  auto LongIsShort = buildIntegerCompareBefore(
      Builder, Anchor, IntegerPredicate::UnsignedLessThan, *OuterExcess,
      *InnerBoundary);
  if (!LongIsShort)
    return std::unexpected(LongIsShort.error());
  auto LongShift = buildBinaryBefore(Builder, Anchor, GenericOpcode::G_SHL, *Type,
                                     Parts[0], *OuterExcess);
  if (!LongShift)
    return std::unexpected(LongShift.error());
  auto LongHigh = buildSelectBefore(Builder, Anchor, *Type, *LongIsShort,
                                    *LongShift, *Zero);
  if (!LongHigh)
    return std::unexpected(LongHigh.error());

  auto ResultLow = buildSelectBefore(Builder, Anchor, *Type, *OuterIsShort,
                                     Lower->Low, *Zero);
  if (!ResultLow)
    return std::unexpected(ResultLow.error());
  auto ResultMiddle = buildSelectBefore(Builder, Anchor, *Type, *OuterIsShort,
                                        Lower->High, *Zero);
  if (!ResultMiddle)
    return std::unexpected(ResultMiddle.error());
  auto ShiftedHigh = buildSelectBefore(Builder, Anchor, *Type, *OuterIsShort,
                                       *ShortHigh, *LongHigh);
  if (!ShiftedHigh)
    return std::unexpected(ShiftedHigh.error());
  auto ResultHigh = buildSelectBefore(Builder, Anchor, *Type, *OuterIsZero,
                                      Parts[2], *ShiftedHigh);
  if (!ResultHigh)
    return std::unexpected(ResultHigh.error());
  return std::vector<Register>{*ResultLow, *ResultMiddle, *ResultHigh};
}

[[nodiscard]] Expected<void> lowerWideShiftLeft(MachineInstr &Instruction,
                                                MachineFunction &Function,
                                                const TargetDescription &Target) {
  const MachineOperand *DestinationOperand = Instruction.operandAt(0);
  const MachineOperand *ValueOperand = Instruction.operandAt(1);
  const MachineOperand *AmountOperand = Instruction.operandAt(2);
  const Register *Destination =
      DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
  const Register *Value = ValueOperand == nullptr ? nullptr : ValueOperand->getIf<Register>();
  const Register *Amount = AmountOperand == nullptr ? nullptr : AmountOperand->getIf<Register>();
  if (Destination == nullptr || Value == nullptr || Amount == nullptr)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V wide shift has invalid register operands");
  auto ValueParts = collectWideValueParts(*Value, Function);
  if (!ValueParts)
    return std::unexpected(ValueParts.error());
  auto AmountParts = collectWideValueParts(*Amount, Function);
  if (!AmountParts)
    return std::unexpected(AmountParts.error());
  if (ValueParts->Parts.size() < 2 || AmountParts->Parts.empty())
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V wide shift has an invalid limb decomposition");
  auto ResultPlan =
      wideResultPlan(*Destination, Function, ValueParts->Parts.size());
  if (!ResultPlan)
    return std::unexpected(ResultPlan.error());

  MachineIRBuilder Builder(Function, Target);
  Expected<std::vector<Register>> Results =
      ValueParts->Parts.size() == 2
          ? emitTwoLimbLeftShift(Builder, Instruction, ValueParts->Parts[0],
                                 ValueParts->Parts[1], AmountParts->Parts[0])
                .transform([](TwoLimbShiftResult Result) {
                  return std::vector<Register>{Result.Low, Result.High};
                })
      : ValueParts->Parts.size() == 3
          ? emitThreeLimbLeftShift(Builder, Instruction, ValueParts->Parts,
                                   AmountParts->Parts[0])
          : Expected<std::vector<Register>>(std::unexpected(
                Error(ErrorCode::Unsupported,
                      "RISC-V wide shift currently requires two or three 64-bit limbs")));
  if (!Results)
    return std::unexpected(Results.error());
  for (std::size_t Index = 0; Index < Results->size(); ++Index) {
    auto Replaced = replaceRegisterUses(Function, ResultPlan->Outputs[Index],
                                        (*Results)[Index]);
    if (!Replaced)
      return std::unexpected(Replaced.error());
  }
  if (auto Erased = eraseInstruction(*ResultPlan->Unmerge); !Erased)
    return std::unexpected(Erased.error());
  if (ResultPlan->Extension != nullptr) {
    if (auto Erased = eraseInstruction(*ResultPlan->Extension); !Erased)
      return std::unexpected(Erased.error());
  }
  if (auto Erased = eraseInstruction(Instruction); !Erased)
    return std::unexpected(Erased.error());
  for (std::size_t Index = 1; Index < AmountParts->Parts.size(); ++Index) {
    const MachineOperand *Definition =
        Function.registerInfo().getUniqueVRegDef(AmountParts->Parts[Index]);
    if (Definition != nullptr && Definition->parent() != nullptr)
      AmountParts->Artifacts.push_back(
          const_cast<MachineInstr *>(Definition->parent()));
  }
  ValueParts->Artifacts.insert(ValueParts->Artifacts.end(),
                               AmountParts->Artifacts.begin(),
                               AmountParts->Artifacts.end());
  eraseDeadArtifacts(ValueParts->Artifacts, Function);
  return {};
}

[[nodiscard]] Expected<std::vector<Register>>
emitWideAdd(MachineIRBuilder &Builder, MachineInstr &Anchor, std::span<const Register> Left,
            std::span<const Register> Right) {
  if (Left.size() != Right.size() || Left.size() < 2)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V wide add requires matching multi-limb operands");
  auto Type = LLT::makeAnyScalar(64);
  if (!Type)
    return std::unexpected(Type.error());
  std::vector<Register> Results;
  Results.reserve(Left.size());
  auto Low = buildBinaryBefore(Builder, Anchor, GenericOpcode::G_ADD, *Type, Left[0], Right[0]);
  if (!Low)
    return std::unexpected(Low.error());
  auto Carry = buildIntegerCompareBefore(Builder, Anchor, IntegerPredicate::UnsignedLessThan,
                                         *Low, Right[0]);
  if (!Carry)
    return std::unexpected(Carry.error());
  auto LowCopy = buildUnaryBefore(Builder, Anchor, GenericOpcode::COPY, *Type, *Low);
  if (!LowCopy)
    return std::unexpected(LowCopy.error());
  Results.push_back(*LowCopy);
  for (std::size_t Index = 1; Index < Left.size(); ++Index) {
    auto Base =
        buildBinaryBefore(Builder, Anchor, GenericOpcode::G_ADD, *Type, Left[Index], Right[Index]);
    if (!Base)
      return std::unexpected(Base.error());
    std::optional<Register> BaseCarry;
    if (Index + 1U < Left.size()) {
      auto Compared = buildIntegerCompareBefore(Builder, Anchor,
                                                IntegerPredicate::UnsignedLessThan, *Base,
                                                Left[Index]);
      if (!Compared)
        return std::unexpected(Compared.error());
      BaseCarry = *Compared;
    }
    auto Sum = buildBinaryBefore(Builder, Anchor, GenericOpcode::G_ADD, *Type, *Base, *Carry);
    if (!Sum)
      return std::unexpected(Sum.error());
    if (Index + 1U < Left.size()) {
      auto Zero = buildIntegerConstantBefore(Builder, Anchor, 64, 0);
      if (!Zero)
        return std::unexpected(Zero.error());
      auto IsZero = buildIntegerCompareBefore(Builder, Anchor, IntegerPredicate::Equal, *Sum, *Zero);
      if (!IsZero)
        return std::unexpected(IsZero.error());
      auto CarryFromIncoming =
          buildBinaryBefore(Builder, Anchor, GenericOpcode::G_AND, *Type, *IsZero, *Carry);
      if (!CarryFromIncoming)
        return std::unexpected(CarryFromIncoming.error());
      auto NextCarry = buildBinaryBefore(Builder, Anchor, GenericOpcode::G_OR, *Type, *BaseCarry,
                                         *CarryFromIncoming);
      if (!NextCarry)
        return std::unexpected(NextCarry.error());
      Carry = *NextCarry;
    }
    auto Copy = buildUnaryBefore(Builder, Anchor, GenericOpcode::COPY, *Type, *Sum);
    if (!Copy)
      return std::unexpected(Copy.error());
    Results.push_back(*Copy);
  }
  return Results;
}

[[nodiscard]] Expected<std::vector<Register>>
emitWideSub(MachineIRBuilder &Builder, MachineInstr &Anchor, std::span<const Register> Left,
            std::span<const Register> Right) {
  if (Left.size() != Right.size() || Left.size() < 2)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V wide sub requires matching multi-limb operands");
  auto Type = LLT::makeAnyScalar(64);
  if (!Type)
    return std::unexpected(Type.error());
  std::vector<Register> Results;
  Results.reserve(Left.size());
  auto Low = buildBinaryBefore(Builder, Anchor, GenericOpcode::G_SUB, *Type, Left[0], Right[0]);
  if (!Low)
    return std::unexpected(Low.error());
  Results.push_back(*Low);
  auto Borrow = buildIntegerCompareBefore(Builder, Anchor, IntegerPredicate::UnsignedLessThan,
                                          Left[0], Right[0]);
  if (!Borrow)
    return std::unexpected(Borrow.error());
  for (std::size_t Index = 1; Index < Left.size(); ++Index) {
    auto Base =
        buildBinaryBefore(Builder, Anchor, GenericOpcode::G_SUB, *Type, Left[Index], Right[Index]);
    if (!Base)
      return std::unexpected(Base.error());
    std::optional<Register> BaseBorrow;
    if (Index + 1U < Left.size()) {
      auto Compared = buildIntegerCompareBefore(Builder, Anchor,
                                                IntegerPredicate::UnsignedGreaterThan, *Base,
                                                Left[Index]);
      if (!Compared)
        return std::unexpected(Compared.error());
      BaseBorrow = *Compared;
    }
    auto Difference =
        buildBinaryBefore(Builder, Anchor, GenericOpcode::G_SUB, *Type, *Base, *Borrow);
    if (!Difference)
      return std::unexpected(Difference.error());
    Results.push_back(*Difference);
    if (Index + 1U < Left.size()) {
      auto Zero = buildIntegerConstantBefore(Builder, Anchor, 64, 0);
      if (!Zero)
        return std::unexpected(Zero.error());
      auto IsZero =
          buildIntegerCompareBefore(Builder, Anchor, IntegerPredicate::Equal, *Base, *Zero);
      if (!IsZero)
        return std::unexpected(IsZero.error());
      auto BorrowFromIncoming =
          buildBinaryBefore(Builder, Anchor, GenericOpcode::G_AND, *Type, *IsZero, *Borrow);
      if (!BorrowFromIncoming)
        return std::unexpected(BorrowFromIncoming.error());
      auto NextBorrow = buildBinaryBefore(Builder, Anchor, GenericOpcode::G_OR, *Type, *BaseBorrow,
                                          *BorrowFromIncoming);
      if (!NextBorrow)
        return std::unexpected(NextBorrow.error());
      Borrow = *NextBorrow;
    }
  }
  return Results;
}

[[nodiscard]] Expected<std::vector<Register>>
emitWideBitwise(MachineIRBuilder &Builder, MachineInstr &Anchor, GenericOpcode Generic,
                std::span<const Register> Left, std::span<const Register> Right) {
  if (Left.size() != Right.size() || Left.size() < 2)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V wide bitwise operation requires matching multi-limb operands");
  auto Type = LLT::makeAnyScalar(64);
  if (!Type)
    return std::unexpected(Type.error());
  std::vector<Register> Results;
  Results.reserve(Left.size());
  for (std::size_t Index = 0; Index < Left.size(); ++Index) {
    auto Part = buildBinaryBefore(Builder, Anchor, Generic, *Type, Left[Index], Right[Index]);
    if (!Part)
      return std::unexpected(Part.error());
    Results.push_back(*Part);
  }
  return Results;
}

[[nodiscard]] Expected<void> lowerWideBinary(MachineInstr &Instruction,
                                             MachineFunction &Function,
                                             const TargetDescription &Target,
                                             GenericOpcode Generic) {
  const MachineOperand *DestinationOperand = Instruction.operandAt(0);
  const MachineOperand *LeftOperand = Instruction.operandAt(1);
  const MachineOperand *RightOperand = Instruction.operandAt(2);
  const Register *Destination =
      DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
  const Register *Left = LeftOperand == nullptr ? nullptr : LeftOperand->getIf<Register>();
  const Register *Right = RightOperand == nullptr ? nullptr : RightOperand->getIf<Register>();
  if (Destination == nullptr || Left == nullptr || Right == nullptr)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V wide binary instruction has invalid operands");
  auto LeftParts = collectWideValueParts(*Left, Function);
  if (!LeftParts)
    return std::unexpected(LeftParts.error());
  auto RightParts = collectWideValueParts(*Right, Function);
  if (!RightParts)
    return std::unexpected(RightParts.error());
  if (LeftParts->Parts.size() != RightParts->Parts.size())
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V wide binary operands have different limb counts");
  auto ResultPlan = wideResultPlan(*Destination, Function, LeftParts->Parts.size());
  if (!ResultPlan)
    return std::unexpected(ResultPlan.error());

  MachineIRBuilder Builder(Function, Target);
  Expected<std::vector<Register>> Results =
      Generic == GenericOpcode::G_ADD
          ? emitWideAdd(Builder, Instruction, LeftParts->Parts, RightParts->Parts)
      : Generic == GenericOpcode::G_SUB
          ? emitWideSub(Builder, Instruction, LeftParts->Parts, RightParts->Parts)
          : emitWideBitwise(Builder, Instruction, Generic, LeftParts->Parts, RightParts->Parts);
  if (!Results)
    return std::unexpected(Results.error());
  for (std::size_t Index = 0; Index < Results->size(); ++Index) {
    auto Replaced = replaceRegisterUses(Function, ResultPlan->Outputs[Index], (*Results)[Index]);
    if (!Replaced)
      return std::unexpected(Replaced.error());
  }
  if (auto Erased = eraseInstruction(*ResultPlan->Unmerge); !Erased)
    return std::unexpected(Erased.error());
  if (ResultPlan->Extension != nullptr) {
    if (auto Erased = eraseInstruction(*ResultPlan->Extension); !Erased)
      return std::unexpected(Erased.error());
  }
  if (auto Erased = eraseInstruction(Instruction); !Erased)
    return std::unexpected(Erased.error());
  LeftParts->Artifacts.insert(LeftParts->Artifacts.end(), RightParts->Artifacts.begin(),
                              RightParts->Artifacts.end());
  eraseDeadArtifacts(LeftParts->Artifacts, Function);
  return {};
}

[[nodiscard]] bool canLowerNarrowBranchCondition(const MachineInstr &Instruction,
                                                 const MachineFunction &Function) {
  if (Instruction.opcode() != opcode(GenericOpcode::G_BRCOND) ||
      Instruction.operandCount() < 2)
    return false;
  const MachineOperand *ConditionOperand = Instruction.operandAt(0);
  const Register *Condition =
      ConditionOperand == nullptr ? nullptr : ConditionOperand->getIf<Register>();
  if (Condition == nullptr || !Condition->isVirtual() ||
      singleRegisterUse(Function, *Condition) != &Instruction)
    return false;
  const auto ConditionInfo = Function.registerInfo().virtualRegisterInfo(*Condition);
  const MachineOperand *Definition =
      Function.registerInfo().getUniqueVRegDef(*Condition);
  const MachineInstr *Truncation =
      Definition == nullptr ? nullptr : Definition->parent();
  if (!ConditionInfo || !ConditionInfo->get().Type ||
      ConditionInfo->get().Type->scalarBitWidth() != 1 || Truncation == nullptr ||
      Truncation->opcode() != opcode(GenericOpcode::G_TRUNC) ||
      Truncation->operandCount() != 2)
    return false;
  return operandType(Function, *Truncation, 1)
      .transform([](LLT Type) { return isScalarGPRType(Type) && Type.scalarBitWidth() == 64; })
      .value_or(false);
}

[[nodiscard]] Expected<void>
lowerNarrowBranchCondition(MachineInstr &Instruction, MachineFunction &Function,
                           const TargetDescription &Target) {
  if (!canLowerNarrowBranchCondition(Instruction, Function))
    return makeUnexpected(ErrorCode::Unsupported,
                          "RISC-V branch condition is not a narrow truncation");
  const MachineOperand *ConditionOperand = Instruction.operandAt(0);
  const Register Condition = *ConditionOperand->getIf<Register>();
  const MachineOperand *Definition =
      Function.registerInfo().getUniqueVRegDef(Condition);
  MachineInstr *Truncation =
      const_cast<MachineInstr *>(Definition->parent());
  const Register Source = *Truncation->operandAt(1)->getIf<Register>();
  auto Type = LLT::makeAnyScalar(64);
  if (!Type)
    return std::unexpected(Type.error());
  MachineIRBuilder Builder(Function, Target);
  auto One = buildIntegerConstantBefore(Builder, Instruction, 64, 1);
  if (!One)
    return std::unexpected(One.error());
  auto Masked = buildBinaryBefore(Builder, Instruction, GenericOpcode::G_AND, *Type,
                                  Source, *One);
  if (!Masked)
    return std::unexpected(Masked.error());
  auto Replaced = Instruction.operandAt(0)->setRegister(*Masked);
  if (!Replaced)
    return std::unexpected(Replaced.error());
  if (Function.registerInfo().useEmpty(Condition))
    return eraseInstruction(*Truncation);
  return {};
}

struct NarrowPhiIncoming final {
  Register Value;
  MachineBasicBlock *Block = nullptr;
  MachineInstr *ReusableTruncation = nullptr;
};

[[nodiscard]] MachineInstr *firstInstructionAfterLeadingCopies(MachineBasicBlock &Block) {
  MachineInstr *Instruction = Block.firstInstruction();
  while (Instruction != nullptr &&
         Instruction->opcode() == opcode(GenericOpcode::COPY))
    Instruction = Instruction->next();
  return Instruction;
}

[[nodiscard]] Expected<Register>
buildIncomingConversion(MachineFunction &Function, const TargetDescription &Target,
                        MachineBasicBlock &Block, MachineInstr *Anchor,
                        GenericOpcode Generic, LLT Type, Register Source) {
  auto Result = Function.registerInfo().createVirtualRegister(Type);
  if (!Result)
    return std::unexpected(Result.error());
  MachineIRBuilder Builder(Function, Target);
  auto Point = Anchor == nullptr ? Builder.setInsertPoint(Block)
                                 : Builder.setInsertPointBefore(*Anchor);
  if (!Point)
    return std::unexpected(Point.error());
  auto Pending = Builder.buildInstr(opcode(Generic));
  if (!Pending)
    return std::unexpected(Pending.error());
  if (auto Added = Pending->addDef(*Result); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Pending->addUse(Source); !Added)
    return std::unexpected(Added.error());
  auto Built = Pending->build();
  if (!Built)
    return std::unexpected(Built.error());
  return *Result;
}

[[nodiscard]] Expected<void> lowerNarrowPhi(MachineInstr &Instruction,
                                            MachineFunction &Function,
                                            const TargetDescription &Target) {
  const std::optional<LLT> OriginalType = resultType(Function, Instruction);
  const MachineOperand *DestinationOperand = Instruction.operandAt(0);
  const Register *Destination =
      DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
  if (!OriginalType || Destination == nullptr || OriginalType->scalarBitWidth() >= 32 ||
      Instruction.operandCount() < 3 || (Instruction.operandCount() % 2U) == 0)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V narrow PHI has invalid operands");
  auto LegalType = LLT::makeAnyScalar(32);
  if (!LegalType)
    return std::unexpected(LegalType.error());

  std::vector<NarrowPhiIncoming> Incoming;
  Incoming.reserve((Instruction.operandCount() - 1U) / 2U);
  for (std::size_t Index = 1; Index < Instruction.operandCount(); Index += 2) {
    const MachineOperand *ValueOperand = Instruction.operandAt(Index);
    const Register *Value =
        ValueOperand == nullptr ? nullptr : ValueOperand->getIf<Register>();
    const MachineOperand *BlockOperand = Instruction.operandAt(Index + 1U);
    const auto *Block =
        BlockOperand == nullptr ? nullptr : BlockOperand->getIf<BasicBlockOperand>();
    if (Value == nullptr || Block == nullptr || Block->Block == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V narrow PHI has an invalid incoming edge");
    MachineInstr *Reusable = nullptr;
    const MachineOperand *Definition =
        Value->isVirtual() ? Function.registerInfo().getUniqueVRegDef(*Value) : nullptr;
    MachineInstr *Def =
        Definition == nullptr ? nullptr : const_cast<MachineInstr *>(Definition->parent());
    if (Def != nullptr && Def->opcode() == opcode(GenericOpcode::G_TRUNC) &&
        Def->operandCount() == 2 && singleRegisterUse(Function, *Value) == &Instruction &&
        operandType(Function, *Def, 1)
            .transform([](LLT Type) { return Type.scalarBitWidth() >= 32; })
            .value_or(false)) {
      Reusable = Def;
    }
    Incoming.push_back(
        NarrowPhiIncoming{.Value = *Value, .Block = Block->Block,
                          .ReusableTruncation = Reusable});
  }
  for (NarrowPhiIncoming &Entry : Incoming) {
    if (Entry.ReusableTruncation == nullptr)
      continue;
    const std::size_t Count = static_cast<std::size_t>(std::ranges::count(
        Incoming, Entry.ReusableTruncation,
        &NarrowPhiIncoming::ReusableTruncation));
    if (Count != 1)
      Entry.ReusableTruncation = nullptr;
  }
  for (NarrowPhiIncoming &Entry : Incoming) {
    if (Entry.ReusableTruncation == nullptr)
      continue;
    MachineBasicBlock *OriginalBlock = Entry.ReusableTruncation->parent();
    if (OriginalBlock == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V narrow PHI input truncation is detached");
    auto Removed = OriginalBlock->removeInstruction(*Entry.ReusableTruncation);
    if (!Removed)
      return std::unexpected(Removed.error());
  }

  std::vector<std::pair<MachineBasicBlock *, MachineInstr *>> Anchors;
  for (const NarrowPhiIncoming &Entry : Incoming) {
    if (std::ranges::find(Anchors, Entry.Block,
                          &decltype(Anchors)::value_type::first) == Anchors.end()) {
      Anchors.emplace_back(Entry.Block,
                           firstInstructionAfterLeadingCopies(*Entry.Block));
    }
  }
  std::vector<Register> LegalIncoming;
  LegalIncoming.reserve(Incoming.size());
  for (NarrowPhiIncoming &Entry : Incoming) {
    const auto AnchorPosition = std::ranges::find(
        Anchors, Entry.Block, &decltype(Anchors)::value_type::first);
    MachineInstr *Anchor = AnchorPosition->second;
    if (Entry.ReusableTruncation != nullptr) {
      auto SetType = Function.registerInfo().setType(Entry.Value, *LegalType);
      if (!SetType)
        return std::unexpected(SetType.error());
      auto Inserted = Anchor == nullptr
                          ? Entry.Block->appendInstruction(*Entry.ReusableTruncation)
                          : Entry.Block->insertInstructionBefore(
                                *Anchor, *Entry.ReusableTruncation);
      if (!Inserted)
        return std::unexpected(Inserted.error());
      LegalIncoming.push_back(Entry.Value);
      continue;
    }
    const auto ValueInfo = Function.registerInfo().virtualRegisterInfo(Entry.Value);
    if (!ValueInfo || !ValueInfo->get().Type)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V narrow PHI input has no scalar type");
    const std::uint32_t Width = ValueInfo->get().Type->scalarBitWidth();
    const GenericOpcode Conversion = Width < 32   ? GenericOpcode::G_ANYEXT
                                     : Width > 32 ? GenericOpcode::G_TRUNC
                                                  : GenericOpcode::COPY;
    auto Converted = buildIncomingConversion(Function, Target, *Entry.Block, Anchor,
                                              Conversion, *LegalType, Entry.Value);
    if (!Converted)
      return std::unexpected(Converted.error());
    LegalIncoming.push_back(*Converted);
  }

  auto LegalResult = Function.registerInfo().createVirtualRegister(*LegalType);
  if (!LegalResult)
    return std::unexpected(LegalResult.error());
  MachineIRBuilder Builder(Function, Target);
  auto Point = Builder.setInsertPointBefore(Instruction);
  if (!Point)
    return std::unexpected(Point.error());
  auto Pending = Builder.buildInstr(opcode(GenericOpcode::G_PHI));
  if (!Pending)
    return std::unexpected(Pending.error());
  if (auto Added = Pending->addDef(*LegalResult); !Added)
    return std::unexpected(Added.error());
  for (std::size_t Index = 0; Index < Incoming.size(); ++Index) {
    if (auto Added = Pending->addUse(LegalIncoming[Index]); !Added)
      return std::unexpected(Added.error());
    if (auto Added = Pending->addBasicBlock(*Incoming[Index].Block); !Added)
      return std::unexpected(Added.error());
  }
  auto Built = Pending->build();
  if (!Built)
    return std::unexpected(Built.error());
  auto Replaced = replaceRegisterUses(Function, *Destination, *LegalResult);
  if (!Replaced)
    return std::unexpected(Replaced.error());
  return eraseInstruction(Instruction);
}

[[nodiscard]] Expected<void> lowerWidePhi(MachineInstr &Instruction,
                                          MachineFunction &Function,
                                          const TargetDescription &Target) {
  const MachineOperand *DestinationOperand = Instruction.operandAt(0);
  const Register *Destination =
      DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
  if (Destination == nullptr || Instruction.operandCount() < 5 ||
      (Instruction.operandCount() % 2U) == 0)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V wide PHI has invalid operands");

  std::vector<WideValueParts> IncomingParts;
  std::vector<MachineBasicBlock *> IncomingBlocks;
  for (std::size_t Index = 1; Index < Instruction.operandCount(); Index += 2) {
    const MachineOperand *ValueOperand = Instruction.operandAt(Index);
    const Register *Value = ValueOperand == nullptr ? nullptr : ValueOperand->getIf<Register>();
    const MachineOperand *BlockOperand = Instruction.operandAt(Index + 1U);
    const auto *Block = BlockOperand == nullptr ? nullptr : BlockOperand->getIf<BasicBlockOperand>();
    if (Value == nullptr || Block == nullptr || Block->Block == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V wide PHI has an invalid incoming edge");
    auto Parts = collectWideValueParts(*Value, Function);
    if (!Parts)
      return std::unexpected(Parts.error());
    if (!IncomingParts.empty() && Parts->Parts.size() != IncomingParts.front().Parts.size())
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V wide PHI incoming values have different limb counts");
    IncomingParts.push_back(std::move(*Parts));
    IncomingBlocks.push_back(Block->Block);
  }
  const std::size_t PartCount = IncomingParts.front().Parts.size();
  auto ResultPlan = wideResultPlan(*Destination, Function, PartCount);
  if (!ResultPlan)
    return std::unexpected(ResultPlan.error());
  auto Type = LLT::makeAnyScalar(64);
  if (!Type)
    return std::unexpected(Type.error());
  MachineIRBuilder Builder(Function, Target);
  std::vector<Register> Results;
  Results.reserve(PartCount);
  for (std::size_t PartIndex = 0; PartIndex < PartCount; ++PartIndex) {
    auto Result = Function.registerInfo().createVirtualRegister(*Type);
    if (!Result)
      return std::unexpected(Result.error());
    auto Point = Builder.setInsertPointBefore(Instruction);
    if (!Point)
      return std::unexpected(Point.error());
    auto Pending = Builder.buildInstr(opcode(GenericOpcode::G_PHI));
    if (!Pending)
      return std::unexpected(Pending.error());
    if (auto Added = Pending->addDef(*Result); !Added)
      return std::unexpected(Added.error());
    for (std::size_t IncomingIndex = 0; IncomingIndex < IncomingParts.size();
         ++IncomingIndex) {
      if (auto Added = Pending->addUse(IncomingParts[IncomingIndex].Parts[PartIndex]); !Added)
        return std::unexpected(Added.error());
      if (auto Added = Pending->addBasicBlock(*IncomingBlocks[IncomingIndex]); !Added)
        return std::unexpected(Added.error());
    }
    auto Built = Pending->build();
    if (!Built)
      return std::unexpected(Built.error());
    Results.push_back(*Result);
  }
  for (std::size_t Index = 0; Index < PartCount; ++Index) {
    auto Replaced = replaceRegisterUses(Function, ResultPlan->Outputs[Index], Results[Index]);
    if (!Replaced)
      return std::unexpected(Replaced.error());
  }
  if (auto Erased = eraseInstruction(*ResultPlan->Unmerge); !Erased)
    return std::unexpected(Erased.error());
  if (ResultPlan->Extension != nullptr) {
    if (auto Erased = eraseInstruction(*ResultPlan->Extension); !Erased)
      return std::unexpected(Erased.error());
  }
  if (auto Erased = eraseInstruction(Instruction); !Erased)
    return std::unexpected(Erased.error());
  std::vector<MachineInstr *> Artifacts;
  for (WideValueParts &Parts : IncomingParts)
    Artifacts.insert(Artifacts.end(), Parts.Artifacts.begin(), Parts.Artifacts.end());
  eraseDeadArtifacts(Artifacts, Function);
  return {};
}

[[nodiscard]] bool canFoldWideMergeImmediately(const MachineInstr &Instruction,
                                               const MachineFunction &Function) {
  const MachineOperand *LowOperand = Instruction.operandAt(1);
  const Register *Low = LowOperand == nullptr ? nullptr : LowOperand->getIf<Register>();
  if (Low == nullptr)
    return false;
  const auto LowInfo = Function.registerInfo().virtualRegisterInfo(*Low);
  if (!LowInfo || !LowInfo->get().Type)
    return false;
  const MachineOperand *DestinationOperand = Instruction.operandAt(0);
  const Register *Destination =
      DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
  if (Destination == nullptr)
    return false;
  auto &RegisterInfo = const_cast<MachineRegisterInfo &>(Function.registerInfo());
  for (MachineOperand &Use : RegisterInfo.useOperands(*Destination)) {
    MachineInstr *Trunc = Use.parent();
    if (Trunc == nullptr || Trunc->opcode() != opcode(GenericOpcode::G_TRUNC) ||
        Trunc->operandAt(1) != &Use)
      return false;
    const std::optional<LLT> Result = resultType(Function, *Trunc);
    if (!Result || Result->scalarBitWidth() > LowInfo->get().Type->scalarBitWidth())
      return false;
  }
  return true;
}

class RISCVLegalizerInfo final : public TargetLegalizerInfo {
public:
  [[nodiscard]] Expected<LegalizationAction> classify(const MachineInstr &Instruction,
                                                      const MachineFunction &Function,
                                                      const TargetDescription &) const override {
    if (!isGenericOpcode(Instruction.opcode()))
      return LegalizationAction::Legal;

    const GenericOpcode Generic = static_cast<GenericOpcode>(Instruction.opcode().value());
    const std::optional<LLT> Type = resultType(Function, Instruction);
    switch (Generic) {
    case GenericOpcode::G_BR:
      return LegalizationAction::Legal;
    case GenericOpcode::G_BRCOND: {
      const std::optional<LLT> Condition = operandType(Function, Instruction, 0);
      if (!Condition || !isScalarGPRType(*Condition))
        return LegalizationAction::Unsupported;
      return canLowerNarrowBranchCondition(Instruction, Function)
                 ? LegalizationAction::Custom
                 : LegalizationAction::Legal;
    }
    case GenericOpcode::G_PHI: {
      if (!Type)
        return LegalizationAction::Unsupported;
      for (std::size_t Index = 1; Index < Instruction.operandCount(); Index += 2) {
        const std::optional<LLT> Incoming = operandType(Function, Instruction, Index);
        if (!Incoming || *Incoming != *Type)
          return LegalizationAction::Unsupported;
      }
      if ((Type->isInteger() || Type->isAnyScalar()) && Type->scalarBitWidth() > 64)
        return LegalizationAction::Custom;
      if ((Type->isInteger() || Type->isAnyScalar()) && Type->scalarBitWidth() < 32)
        return LegalizationAction::Custom;
      if (!isSupportedIntegerType(*Type) && !isRVFloatType(*Type))
        return LegalizationAction::Unsupported;
      return LegalizationAction::Legal;
    }
    case GenericOpcode::G_IMPLICIT_DEF:
      return Type && (isSupportedIntegerType(*Type) || isRVFloatType(*Type))
                 ? LegalizationAction::Legal
                 : LegalizationAction::Unsupported;
    case GenericOpcode::G_STORE:
      if (Instruction.operandCount() == 0)
        return LegalizationAction::Unsupported;
      if (const std::optional<LLT> ValueType = registerType(Function, *Instruction.operandAt(0));
          ValueType) {
        const std::optional<LLT> AddressType = operandType(Function, Instruction, 1);
        if (!AddressType || !AddressType->isPointer() || AddressType->addressSpace() != 0 ||
            AddressType->scalarBitWidth() != 64) {
          return LegalizationAction::Unsupported;
        }
        if (Instruction.memOperands().size() == 1 &&
            Instruction.memOperands().front()->isAtomic()) {
          const std::uint64_t Bytes = ValueType->scalarBitWidth() / 8U;
          return isSupportedIntegerType(*ValueType) &&
                         isSupportedAtomicMemoryAccess(Instruction, AtomicOperation::Store, Bytes)
                     ? LegalizationAction::Legal
                     : LegalizationAction::Unsupported;
        }
        if ((isSupportedIntegerType(*ValueType) || isRVFloatType(*ValueType)) &&
            isSupportedNonAtomicMemoryAccess(Instruction) &&
            memoryMatchesType(Instruction, *ValueType)) {
          return LegalizationAction::Legal;
        }
      }
      return LegalizationAction::Unsupported;
    case GenericOpcode::G_SEXTLOAD:
    case GenericOpcode::G_ZEXTLOAD: {
      const auto Bytes = memoryAccessBytes(Instruction);
      const std::optional<LLT> AddressType = operandType(Function, Instruction, 1);
      return Type && Bytes && AddressType && isSupportedIntegerType(*Type) &&
                     AddressType->isPointer() && AddressType->addressSpace() == 0 &&
                     AddressType->scalarBitWidth() == 64 &&
                     *Bytes < ((Type->scalarBitWidth() + 7U) / 8U) &&
                     (*Bytes == 1 || *Bytes == 2 || *Bytes == 4) &&
                     isSupportedNonAtomicMemoryAccess(Instruction)
                 ? LegalizationAction::Legal
                 : LegalizationAction::Unsupported;
    }
    case GenericOpcode::G_FPEXTLOAD: {
      const auto Bytes = memoryAccessBytes(Instruction);
      const std::optional<LLT> AddressType = operandType(Function, Instruction, 1);
      return Type && Bytes && AddressType && Type->isFloat() && Type->scalarBitWidth() == 64 &&
                     AddressType->isPointer() && AddressType->addressSpace() == 0 &&
                     AddressType->scalarBitWidth() == 64 && *Bytes == 4 &&
                     isSupportedNonAtomicMemoryAccess(Instruction)
                 ? LegalizationAction::Legal
                 : LegalizationAction::Unsupported;
    }
    case GenericOpcode::G_FPTRUNCSTORE: {
      const std::optional<LLT> ValueType = operandType(Function, Instruction, 0);
      const std::optional<LLT> AddressType = operandType(Function, Instruction, 1);
      const auto Bytes = memoryAccessBytes(Instruction);
      return ValueType && AddressType && Bytes && ValueType->isFloat() &&
                     ValueType->scalarBitWidth() == 64 && AddressType->isPointer() &&
                     AddressType->addressSpace() == 0 && AddressType->scalarBitWidth() == 64 &&
                     *Bytes == 4 && isSupportedNonAtomicMemoryAccess(Instruction)
                 ? LegalizationAction::Legal
                 : LegalizationAction::Unsupported;
    }
    case GenericOpcode::G_LOAD:
      if (Type) {
        const std::optional<LLT> AddressType = operandType(Function, Instruction, 1);
        if (!AddressType || !AddressType->isPointer() || AddressType->addressSpace() != 0 ||
            AddressType->scalarBitWidth() != 64) {
          return LegalizationAction::Unsupported;
        }
        if (Instruction.memOperands().size() == 1 &&
            Instruction.memOperands().front()->isAtomic()) {
          const std::uint64_t Bytes = Type->scalarBitWidth() / 8U;
          return isSupportedIntegerType(*Type) &&
                         isSupportedAtomicMemoryAccess(Instruction, AtomicOperation::Load, Bytes)
                     ? LegalizationAction::Legal
                     : LegalizationAction::Unsupported;
        }
        if ((isSupportedIntegerType(*Type) || isRVFloatType(*Type)) &&
            isSupportedNonAtomicMemoryAccess(Instruction) &&
            memoryMatchesType(Instruction, *Type)) {
          return LegalizationAction::Legal;
        }
      }
      return LegalizationAction::Unsupported;
    case GenericOpcode::G_FADD:
    case GenericOpcode::G_FSUB:
    case GenericOpcode::G_FMUL:
    case GenericOpcode::G_FDIV: {
      const std::optional<LLT> Left = operandType(Function, Instruction, 1);
      const std::optional<LLT> Right = operandType(Function, Instruction, 2);
      return Type && Left && Right && isRVFloatType(*Type) && *Left == *Type && *Right == *Type
                 ? LegalizationAction::Legal
                 : LegalizationAction::Unsupported;
    }
    case GenericOpcode::G_FMA: {
      const std::optional<LLT> First = operandType(Function, Instruction, 1);
      const std::optional<LLT> Second = operandType(Function, Instruction, 2);
      const std::optional<LLT> Third = operandType(Function, Instruction, 3);
      return Type && First && Second && Third && isRVFloatType(*Type) && *First == *Type &&
                     *Second == *Type && *Third == *Type
                 ? LegalizationAction::Legal
                 : LegalizationAction::Unsupported;
    }
    case GenericOpcode::G_FNEG:
    case GenericOpcode::G_FABS:
    case GenericOpcode::G_FSQRT: {
      const std::optional<LLT> Source = operandType(Function, Instruction, 1);
      return Type && Source && isRVFloatType(*Type) && *Source == *Type
                 ? LegalizationAction::Legal
                 : LegalizationAction::Unsupported;
    }
    case GenericOpcode::G_FCOPYSIGN: {
      const std::optional<LLT> Left = operandType(Function, Instruction, 1);
      const std::optional<LLT> Right = operandType(Function, Instruction, 2);
      return Type && Left && Right && isRVFloatType(*Type) && *Left == *Type && *Right == *Type
                 ? LegalizationAction::Legal
                 : LegalizationAction::Unsupported;
    }
    case GenericOpcode::G_FCONSTANT: {
      const MachineOperand *Immediate = Instruction.operandAt(1);
      const auto *Floating =
          Immediate == nullptr ? nullptr : Immediate->getIf<FloatingImmediateOperand>();
      const std::optional<FloatSemantics> Semantics = Type ? Type->floatSemantics() : std::nullopt;
      return Type && isRVFloatType(*Type) && Floating != nullptr && Semantics &&
                     Floating->Bits.semantics() == *Semantics
                 ? LegalizationAction::Legal
                 : LegalizationAction::Unsupported;
    }
    case GenericOpcode::G_ASSERT_SEXT:
    case GenericOpcode::G_ASSERT_ZEXT:
    case GenericOpcode::G_ASSERT_ALIGN: {
      const std::optional<LLT> Source = operandType(Function, Instruction, 1);
      return Type && Source && *Type == *Source && isScalarGPRType(*Type) &&
                     Instruction.operandCount() == 3
                 ? LegalizationAction::Legal
                 : LegalizationAction::Unsupported;
    }
    case GenericOpcode::G_FREEZE:
    case GenericOpcode::G_CONSTANT_FOLD_BARRIER: {
      const std::optional<LLT> Source = operandType(Function, Instruction, 1);
      return Type && Source && *Type == *Source &&
                     (isSupportedIntegerType(*Type) || isRVFloatType(*Type))
                 ? LegalizationAction::Legal
                 : LegalizationAction::Unsupported;
    }
    case GenericOpcode::G_PTRTOINT: {
      const std::optional<LLT> Source = operandType(Function, Instruction, 1);
      return Type && Source && Type->isInteger() && Type->scalarBitWidth() == 64 &&
                     Source->isPointer() && Source->scalarBitWidth() == 64
                 ? LegalizationAction::Legal
                 : LegalizationAction::Unsupported;
    }
    case GenericOpcode::G_INTTOPTR: {
      const std::optional<LLT> Source = operandType(Function, Instruction, 1);
      return Type && Source && Type->isPointer() && Type->scalarBitWidth() == 64 &&
                     Source->isInteger() && Source->scalarBitWidth() == 64
                 ? LegalizationAction::Legal
                 : LegalizationAction::Unsupported;
    }
    case GenericOpcode::G_BITCAST: {
      const std::optional<LLT> Source = operandType(Function, Instruction, 1);
      const bool IntegerFloatPair =
          Type && Source && Type->scalarBitWidth() == Source->scalarBitWidth() &&
          ((Type->isFloat() && Source->isInteger()) || (Type->isInteger() && Source->isFloat())) &&
          (Type->scalarBitWidth() == 32 || Type->scalarBitWidth() == 64);
      return IntegerFloatPair ? LegalizationAction::Legal : LegalizationAction::Unsupported;
    }
    case GenericOpcode::G_ANYEXT:
    case GenericOpcode::G_SEXT:
    case GenericOpcode::G_ZEXT: {
      const std::optional<LLT> Source = operandType(Function, Instruction, 1);
      if (!Type || !Source)
        return LegalizationAction::Unsupported;
      if (!isScalarIntegerExtension(*Type, *Source)) {
        const MachineOperand *DestinationOperand = Instruction.operandAt(0);
        const Register *Destination = DestinationOperand == nullptr
                                          ? nullptr
                                          : DestinationOperand->getIf<Register>();
        MachineInstr *OnlyUse = Destination == nullptr
                                    ? nullptr
                                    : singleUseInstruction(Function, *Destination);
        return (Type->isInteger() || Type->isAnyScalar()) &&
                       (Source->isInteger() || Source->isAnyScalar()) &&
                       Type->scalarBitWidth() > 64 &&
                       Source->scalarBitWidth() < Type->scalarBitWidth() &&
                       OnlyUse != nullptr &&
                       OnlyUse->opcode() == opcode(GenericOpcode::G_UNMERGE_VALUES)
                   ? LegalizationAction::Legal
                   : LegalizationAction::Unsupported;
      }
      const MachineOperand *SourceOperand = Instruction.operandAt(1);
      const Register *SourceRegister =
          SourceOperand == nullptr ? nullptr : SourceOperand->getIf<Register>();
      const MachineOperand *SourceDefinition =
          SourceRegister == nullptr || !SourceRegister->isVirtual()
              ? nullptr
              : Function.registerInfo().getUniqueVRegDef(*SourceRegister);
      const MachineInstr *SourceDef =
          SourceDefinition == nullptr ? nullptr : SourceDefinition->parent();
      if (SourceDef != nullptr && SourceDef->opcode() == opcode(GenericOpcode::G_CONSTANT))
        return LegalizationAction::Custom;
      if (Generic == GenericOpcode::G_ANYEXT &&
          narrowAnyExtBinaryPlan(Instruction, Function).has_value())
        return LegalizationAction::Custom;
      return LegalizationAction::Legal;
    }
    case GenericOpcode::G_TRUNC: {
      const std::optional<LLT> Source = operandType(Function, Instruction, 1);
      if (Type && Source && (Type->isInteger() || Type->isAnyScalar()) &&
          (Source->isInteger() || Source->isAnyScalar()) && Type->scalarBitWidth() > 64 &&
          Source->scalarBitWidth() > Type->scalarBitWidth())
        return LegalizationAction::Legal;
      return Type && Source && isSupportedIntegerScalar(*Type) &&
                     isSupportedIntegerScalar(*Source) && Type->scalarBitWidth() != 0 &&
                     Source->scalarBitWidth() >= Type->scalarBitWidth()
                 ? LegalizationAction::Legal
                 : LegalizationAction::Unsupported;
    }
    case GenericOpcode::G_MERGE_VALUES:
    case GenericOpcode::G_UNMERGE_VALUES: {
      if (Instruction.operandCount() < 3)
        return LegalizationAction::Unsupported;
      const std::size_t SmallTypeIndex =
          Generic == GenericOpcode::G_MERGE_VALUES ? 1 : 0;
      const std::size_t BigTypeIndex = Generic == GenericOpcode::G_MERGE_VALUES
                                           ? 0
                                           : Instruction.operandCount() - 1U;
      const std::optional<LLT> SmallType = operandType(Function, Instruction, SmallTypeIndex);
      const std::optional<LLT> BigType = operandType(Function, Instruction, BigTypeIndex);
      if (SmallType && BigType && Generic == GenericOpcode::G_MERGE_VALUES &&
          BigType->scalarBitWidth() > 64 &&
          !canFoldWideMergeImmediately(Instruction, Function))
        return LegalizationAction::Legal;
      return SmallType && BigType &&
                     (SmallType->isInteger() || SmallType->isAnyScalar()) &&
                     (BigType->isInteger() || BigType->isAnyScalar()) &&
                     SmallType->scalarBitWidth() != 0 && BigType->scalarBitWidth() != 0
                 ? LegalizationAction::Custom
                 : LegalizationAction::Unsupported;
    }
    case GenericOpcode::G_SEXT_INREG: {
      const std::optional<LLT> Source = operandType(Function, Instruction, 1);
      const MachineOperand *WidthOperand = Instruction.operandAt(2);
      const auto Width =
          WidthOperand == nullptr
              ? Expected<std::int64_t>(std::unexpected(
                    Error(ErrorCode::InvalidArgument, "missing in-register extension width")))
              : signedImmediate(*WidthOperand);
      return Type && Source && *Type == *Source && isScalarGPRType(*Type) && Width && *Width > 0 &&
                     static_cast<std::uint64_t>(*Width) <= Type->scalarBitWidth()
                 ? LegalizationAction::Legal
                 : LegalizationAction::Unsupported;
    }
    case GenericOpcode::G_ICMP: {
      const MachineOperand *Predicate = Instruction.operandAt(1);
      const auto *IntegerPredicate =
          Predicate == nullptr ? nullptr : Predicate->getIf<PredicateOperand>();
      const std::optional<LLT> Left = operandType(Function, Instruction, 2);
      const std::optional<LLT> Right = operandType(Function, Instruction, 3);
      return Type && isScalarGPRType(*Type) && IntegerPredicate != nullptr &&
                     IntegerPredicate->integerPredicate() != nullptr && Left && Right &&
                     *Left == *Right && isSupportedIntegerType(*Left)
                 ? LegalizationAction::Legal
                 : LegalizationAction::Unsupported;
    }
    case GenericOpcode::G_FCMP: {
      const MachineOperand *Predicate = Instruction.operandAt(1);
      const auto *FloatingPredicate =
          Predicate == nullptr ? nullptr : Predicate->getIf<PredicateOperand>();
      const std::optional<LLT> Left = operandType(Function, Instruction, 2);
      const std::optional<LLT> Right = operandType(Function, Instruction, 3);
      return Type && isI1Type(*Type) && FloatingPredicate != nullptr &&
                     FloatingPredicate->floatingPredicate() != nullptr && Left && Right &&
                     *Left == *Right && isRVFloatType(*Left)
                 ? LegalizationAction::Legal
                 : LegalizationAction::Unsupported;
    }
    case GenericOpcode::G_FPEXT:
    case GenericOpcode::G_FPTRUNC: {
      const std::optional<LLT> Source = operandType(Function, Instruction, 1);
      const bool IsExtension = Generic == GenericOpcode::G_FPEXT;
      return Type && Source && isRVFloatType(*Type) && isRVFloatType(*Source) &&
                     ((IsExtension && Source->scalarBitWidth() == 32 &&
                       Type->scalarBitWidth() == 64) ||
                      (!IsExtension && Source->scalarBitWidth() == 64 &&
                       Type->scalarBitWidth() == 32))
                 ? LegalizationAction::Legal
                 : LegalizationAction::Unsupported;
    }
    case GenericOpcode::G_FPTOSI:
    case GenericOpcode::G_FPTOUI: {
      const std::optional<LLT> Source = operandType(Function, Instruction, 1);
      return Type && Source && isSupportedIntegerType(*Type) && isRVFloatType(*Source)
                 ? LegalizationAction::Legal
                 : LegalizationAction::Unsupported;
    }
    case GenericOpcode::G_SITOFP:
    case GenericOpcode::G_UITOFP: {
      const std::optional<LLT> Source = operandType(Function, Instruction, 1);
      return Type && Source && isRVFloatType(*Type) && isSupportedIntegerType(*Source)
                 ? LegalizationAction::Legal
                 : LegalizationAction::Unsupported;
    }
    case GenericOpcode::G_FMINNUM:
    case GenericOpcode::G_FMAXNUM: {
      const std::optional<LLT> Left = operandType(Function, Instruction, 1);
      const std::optional<LLT> Right = operandType(Function, Instruction, 2);
      return Type && Left && Right && isRVFloatType(*Type) && *Left == *Type && *Right == *Type
                 ? LegalizationAction::Legal
                 : LegalizationAction::Unsupported;
    }
    case GenericOpcode::G_SELECT: {
      const std::optional<LLT> Condition = operandType(Function, Instruction, 1);
      const std::optional<LLT> TrueValue = operandType(Function, Instruction, 2);
      const std::optional<LLT> FalseValue = operandType(Function, Instruction, 3);
      return Type && Condition && TrueValue && FalseValue && isScalarGPRType(*Condition) &&
                     *TrueValue == *Type && *FalseValue == *Type &&
                     (isSupportedIntegerType(*Type) || isRVFloatType(*Type))
                 ? LegalizationAction::Legal
                 : LegalizationAction::Unsupported;
    }
    case GenericOpcode::G_PTRMASK: {
      const std::optional<LLT> Pointer = operandType(Function, Instruction, 1);
      const std::optional<LLT> Mask = operandType(Function, Instruction, 2);
      return Type && Pointer && Mask && Type->isPointer() && Type->addressSpace() == 0 &&
                     Type->scalarBitWidth() == 64 && *Pointer == *Type &&
                     isSupportedIntegerScalar(*Mask) && Mask->scalarBitWidth() == 64
                 ? LegalizationAction::Legal
                 : LegalizationAction::Unsupported;
    }
    case GenericOpcode::G_ABS: {
      const std::optional<LLT> Source = operandType(Function, Instruction, 1);
      return Type && Source && *Source == *Type && isSupportedIntegerScalar(*Type)
                 ? LegalizationAction::Legal
                 : LegalizationAction::Unsupported;
    }
    case GenericOpcode::G_SDIVREM:
    case GenericOpcode::G_UDIVREM: {
      const std::optional<LLT> Remainder = operandType(Function, Instruction, 1);
      const std::optional<LLT> Left = operandType(Function, Instruction, 2);
      const std::optional<LLT> Right = operandType(Function, Instruction, 3);
      return Type && Remainder && Left && Right && isSupportedIntegerScalar(*Type) &&
                     *Remainder == *Type && *Left == *Type && *Right == *Type
                 ? LegalizationAction::Legal
                 : LegalizationAction::Unsupported;
    }
    case GenericOpcode::G_SMULH:
    case GenericOpcode::G_UMULH: {
      const std::optional<LLT> Left = operandType(Function, Instruction, 1);
      const std::optional<LLT> Right = operandType(Function, Instruction, 2);
      return Type && Left && Right && isNativeIntegerScalar(*Type) && *Left == *Type &&
                     *Right == *Type
                 ? LegalizationAction::Legal
                 : LegalizationAction::Unsupported;
    }
    case GenericOpcode::G_SMIN:
    case GenericOpcode::G_SMAX:
    case GenericOpcode::G_UMIN:
    case GenericOpcode::G_UMAX: {
      const std::optional<LLT> Left = operandType(Function, Instruction, 1);
      const std::optional<LLT> Right = operandType(Function, Instruction, 2);
      return Type && Left && Right && isSupportedIntegerScalar(*Type) && *Left == *Type &&
                     *Right == *Type
                 ? LegalizationAction::Legal
                 : LegalizationAction::Unsupported;
    }
    case GenericOpcode::G_UADDO:
    case GenericOpcode::G_USUBO:
    case GenericOpcode::G_SADDO:
    case GenericOpcode::G_SSUBO:
    case GenericOpcode::G_UMULO:
    case GenericOpcode::G_SMULO: {
      const std::optional<LLT> Overflow = operandType(Function, Instruction, 1);
      const std::optional<LLT> Left = operandType(Function, Instruction, 2);
      const std::optional<LLT> Right = operandType(Function, Instruction, 3);
      return Type && Overflow && Left && Right && isNativeIntegerScalar(*Type) &&
                     isI1Type(*Overflow) && *Left == *Type && *Right == *Type
                 ? LegalizationAction::Legal
                 : LegalizationAction::Unsupported;
    }
    case GenericOpcode::G_UADDE:
    case GenericOpcode::G_USUBE:
    case GenericOpcode::G_SADDE:
    case GenericOpcode::G_SSUBE: {
      const std::optional<LLT> CarryOut = operandType(Function, Instruction, 1);
      const std::optional<LLT> Left = operandType(Function, Instruction, 2);
      const std::optional<LLT> Right = operandType(Function, Instruction, 3);
      const std::optional<LLT> CarryIn = operandType(Function, Instruction, 4);
      return Type && CarryOut && Left && Right && CarryIn && isNativeIntegerScalar(*Type) &&
                     isI1Type(*CarryOut) && isI1Type(*CarryIn) && *Left == *Type && *Right == *Type
                 ? LegalizationAction::Legal
                 : LegalizationAction::Unsupported;
    }
    case GenericOpcode::G_ATOMICRMW_XCHG:
    case GenericOpcode::G_ATOMICRMW_ADD:
    case GenericOpcode::G_ATOMICRMW_SUB:
    case GenericOpcode::G_ATOMICRMW_AND:
    case GenericOpcode::G_ATOMICRMW_OR:
    case GenericOpcode::G_ATOMICRMW_XOR:
    case GenericOpcode::G_ATOMICRMW_MAX:
    case GenericOpcode::G_ATOMICRMW_MIN:
    case GenericOpcode::G_ATOMICRMW_UMAX:
    case GenericOpcode::G_ATOMICRMW_UMIN: {
      const std::optional<LLT> Address = operandType(Function, Instruction, 1);
      const std::optional<LLT> Value = operandType(Function, Instruction, 2);
      const std::uint64_t Bytes = Type ? Type->scalarBitWidth() / 8U : 0;
      return Type && Address && Value && isNativeIntegerScalar(*Type) && *Value == *Type &&
                     Address->isPointer() && Address->addressSpace() == 0 &&
                     Address->scalarBitWidth() == 64 &&
                     isSupportedAtomicMemoryAccess(Instruction, AtomicOperation::ReadModifyWrite,
                                                   Bytes)
                 ? LegalizationAction::Legal
                 : LegalizationAction::Unsupported;
    }
    case GenericOpcode::G_ATOMIC_CMPXCHG: {
      const std::optional<LLT> Address = operandType(Function, Instruction, 1);
      const std::optional<LLT> Compare = operandType(Function, Instruction, 2);
      const std::optional<LLT> NewValue = operandType(Function, Instruction, 3);
      if (!Type || !Address || !Compare || !NewValue || *Compare != *Type || *NewValue != *Type ||
          (!Type->isInteger() && !Type->isAnyScalar()) ||
          (Type->scalarBitWidth() != 8 && Type->scalarBitWidth() != 16 &&
           Type->scalarBitWidth() != 32 && Type->scalarBitWidth() != 64) ||
          !Address->isPointer() || Address->addressSpace() != 0 ||
          Address->scalarBitWidth() != 64 ||
          !isSupportedAtomicContainerAccess(Instruction, AtomicOperation::CompareExchange,
                                            (Type->scalarBitWidth() + 7U) / 8U)) {
        return LegalizationAction::Unsupported;
      }
      return Type->scalarBitWidth() == 64 ? LegalizationAction::Legal : LegalizationAction::Custom;
    }
    case GenericOpcode::G_FENCE: {
      const MachineOperand *OrderingOperand = Instruction.operandAt(0);
      const MachineOperand *ScopeOperand = Instruction.operandAt(1);
      const auto Ordering = OrderingOperand == nullptr
                                ? Expected<std::int64_t>(std::unexpected(
                                      Error(ErrorCode::InvalidArgument, "missing fence ordering")))
                                : signedImmediate(*OrderingOperand);
      const auto Scope = ScopeOperand == nullptr
                             ? Expected<std::int64_t>(std::unexpected(
                                   Error(ErrorCode::InvalidArgument, "missing fence scope")))
                             : signedImmediate(*ScopeOperand);
      return Ordering && Scope && *Ordering >= 4 && *Ordering <= 7 && *Scope == 0
                 ? LegalizationAction::Legal
                 : LegalizationAction::Unsupported;
    }
    case GenericOpcode::G_CONSTANT_POOL: {
      const MachineOperand *PoolOperand = Instruction.operandAt(1);
      const auto *Pool = PoolOperand == nullptr ? nullptr : PoolOperand->getIf<ConstantPoolOperand>();
      return Type && Type->isPointer() && Type->addressSpace() == 0 &&
                     Type->scalarBitWidth() == 64 && Pool != nullptr &&
                     Function.constantPool().entry(Pool->Index) != nullptr
                 ? LegalizationAction::Legal
                 : LegalizationAction::Unsupported;
    }
    case GenericOpcode::G_GLOBAL_VALUE: {
      const MachineOperand *GlobalOperand = Instruction.operandAt(1);
      const auto *Global = GlobalOperand == nullptr
                               ? nullptr
                               : GlobalOperand->getIf<GlobalSymbolOperand>();
      return Type && Type->isPointer() && Type->addressSpace() == 0 &&
                     Type->scalarBitWidth() == 64 && Global != nullptr &&
                     !Global->Name.empty()
                 ? LegalizationAction::Legal
                 : LegalizationAction::Unsupported;
    }
    case GenericOpcode::G_BRINDIRECT:
      return operandType(Function, Instruction, 0)
                     .transform([](LLT Value) {
                       return Value.isPointer() && Value.addressSpace() == 0 &&
                              Value.scalarBitWidth() == 64;
                     })
                     .value_or(false)
                 ? LegalizationAction::Legal
                 : LegalizationAction::Unsupported;
    case GenericOpcode::G_TRAP:
    case GenericOpcode::G_DEBUGTRAP:
      return Instruction.operandCount() == 0 ? LegalizationAction::Legal
                                             : LegalizationAction::Unsupported;
    case GenericOpcode::G_UBSANTRAP:
      return Instruction.operandCount() == 1 ? LegalizationAction::Legal
                                             : LegalizationAction::Unsupported;
    case GenericOpcode::G_CONSTANT: {
      const MachineOperand *Immediate = Instruction.operandAt(1);
      if (!Type || Immediate == nullptr ||
          (Immediate->kind() != MachineOperand::Kind::Immediate &&
           Immediate->kind() != MachineOperand::Kind::IntegerImmediate))
        return LegalizationAction::Unsupported;
      if (Type->scalarBitWidth() > 64)
        return wideConstantUnmergePlan(Instruction, Function).has_value()
                   ? LegalizationAction::Custom
                   : LegalizationAction::Unsupported;
      if ((Type->isInteger() || Type->isAnyScalar()) && Type->scalarBitWidth() == 64) {
        auto Value = signedImmediate(*Immediate);
        if (Value && shouldUseRV64IntegerConstantPool(*Value))
          return LegalizationAction::Custom;
      }
      return isSupportedIntegerType(*Type) ? LegalizationAction::Legal
                                            : LegalizationAction::Unsupported;
    }
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
    case GenericOpcode::G_ASHR: {
      const std::optional<LLT> Left = operandType(Function, Instruction, 1);
      const std::optional<LLT> Right = operandType(Function, Instruction, 2);
      if (!Type || !Left || !Right || *Left != *Type || *Right != *Type ||
          (!Type->isInteger() && !Type->isAnyScalar()))
        return LegalizationAction::Unsupported;
      if (Type->scalarBitWidth() > 64) {
        return Generic == GenericOpcode::G_ADD || Generic == GenericOpcode::G_SUB ||
                       Generic == GenericOpcode::G_AND || Generic == GenericOpcode::G_OR ||
                       Generic == GenericOpcode::G_XOR || Generic == GenericOpcode::G_SHL
                   ? LegalizationAction::Custom
                   : LegalizationAction::Unsupported;
      }
      return isSupportedIntegerScalar(*Type) ? LegalizationAction::Legal
                                              : LegalizationAction::Unsupported;
    }
    case GenericOpcode::G_PTR_ADD: {
      const std::optional<LLT> Pointer = operandType(Function, Instruction, 1);
      const std::optional<LLT> Offset = operandType(Function, Instruction, 2);
      return Type && Pointer && Offset && Type->isPointer() && Type->addressSpace() == 0 &&
                     Type->scalarBitWidth() == 64 && *Pointer == *Type && Offset->isInteger() &&
                     Offset->scalarBitWidth() == 64
                 ? LegalizationAction::Legal
                 : LegalizationAction::Unsupported;
    }
    default:
      break;
    }

    // Keep the unsupported policy mechanically synchronized with the complete
    // target-independent opcode inventory. Supported cases return above.
    switch (Generic) {
#define RCC_CORE_OPCODE(Name)
#define RCC_GENERIC_OPCODE(Name) case GenericOpcode::Name:
#include "rcc/CodeGen/GenericOpcode.def"
#undef RCC_GENERIC_OPCODE
#undef RCC_CORE_OPCODE
      return LegalizationAction::Unsupported;
    case GenericOpcode::Invalid:
    case GenericOpcode::CORE_PSEUDO_END:
    case GenericOpcode::G_GENERIC_END:
    default:
      break;
    }
    return makeUnexpected(ErrorCode::InternalError,
                          "RISC-V legalizer received an invalid generic opcode value");
  }

  [[nodiscard]] Expected<void> lower(MachineInstr &Instruction, MachineFunction &Function,
                                     const TargetDescription &Target,
                                     LegalizationAction Action) const override {
    if (Action == LegalizationAction::Custom &&
        resultType(Function, Instruction)
            .transform([](LLT Type) { return Type.scalarBitWidth() > 64; })
            .value_or(false)) {
      const Opcode Value = Instruction.opcode();
      if (Value == opcode(GenericOpcode::G_ADD) || Value == opcode(GenericOpcode::G_SUB) ||
          Value == opcode(GenericOpcode::G_AND) || Value == opcode(GenericOpcode::G_OR) ||
          Value == opcode(GenericOpcode::G_XOR)) {
        return lowerWideBinary(Instruction, Function, Target,
                               static_cast<GenericOpcode>(Value.value()));
      }
      if (Value == opcode(GenericOpcode::G_SHL))
        return lowerWideShiftLeft(Instruction, Function, Target);
      if (Value == opcode(GenericOpcode::G_PHI))
        return lowerWidePhi(Instruction, Function, Target);
    }
    if (Action == LegalizationAction::Custom &&
        Instruction.opcode() == opcode(GenericOpcode::G_MERGE_VALUES))
      return lowerMergeValues(Instruction, Function, Target);
    if (Action == LegalizationAction::Custom &&
        Instruction.opcode() == opcode(GenericOpcode::G_UNMERGE_VALUES))
      return lowerUnmergeValues(Instruction, Function, Target);
    if (Action == LegalizationAction::Custom &&
        Instruction.opcode() == opcode(GenericOpcode::G_BRCOND))
      return lowerNarrowBranchCondition(Instruction, Function, Target);
    if (Action == LegalizationAction::Custom &&
        Instruction.opcode() == opcode(GenericOpcode::G_PHI))
      return lowerNarrowPhi(Instruction, Function, Target);
    if (Action == LegalizationAction::Custom &&
        Instruction.opcode() == opcode(GenericOpcode::G_ATOMIC_CMPXCHG))
      return lowerAtomicCompareExchange(Instruction, Function, Target);
    if (Action == LegalizationAction::Custom &&
        (Instruction.opcode() == opcode(GenericOpcode::G_ANYEXT) ||
         Instruction.opcode() == opcode(GenericOpcode::G_SEXT) ||
         Instruction.opcode() == opcode(GenericOpcode::G_ZEXT))) {
      if (Instruction.opcode() == opcode(GenericOpcode::G_ANYEXT) &&
          narrowAnyExtBinaryPlan(Instruction, Function).has_value())
        return foldNarrowAnyExtBinary(Instruction, Function, Target);
      return foldConstantExtension(
          Instruction, Function,
          static_cast<GenericOpcode>(Instruction.opcode().value()));
    }
    if (Action == LegalizationAction::Custom &&
        Instruction.opcode() == opcode(GenericOpcode::G_CONSTANT)) {
      const MachineOperand *Immediate = Instruction.operandAt(1);
      const std::optional<LLT> Type = resultType(Function, Instruction);
      if (Immediate != nullptr && Type && (Type->isInteger() || Type->isAnyScalar()) &&
          Type->scalarBitWidth() == 64) {
        auto Value = signedImmediate(*Immediate);
        if (Value && shouldUseRV64IntegerConstantPool(*Value))
          return lowerConstantToPool(Instruction, Function, Target);
      }
      return foldWideConstant(Instruction, Function, Target);
    }
    return makeUnexpected(ErrorCode::Unsupported,
                          "RISC-V legalizer has no lowering rule for this instruction");
  }
};

class RISCVRegisterBankInfo final : public TargetRegisterBankInfo {
public:
  [[nodiscard]] Expected<RegisterBankID> selectBank(const MachineFunction &, Register,
                                                    const VirtualRegisterInfo &Info,
                                                    const MachineInstr *,
                                                    const TargetDescription &) const override {
    if (!Info.Type) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V register-bank selection requires a virtual-register type");
    }
    if (isScalarGPRType(*Info.Type)) {
      return RegisterBankID::fromValue(static_cast<std::uint32_t>(RegisterBank::GPR));
    }
    if (isRVFloatType(*Info.Type))
      return RegisterBankID::fromValue(static_cast<std::uint32_t>(RegisterBank::FPR));
    return makeUnexpected(ErrorCode::Unsupported,
                          "RISC-V RV64GC has no register bank for virtual register type '" +
                              Info.Type->toString() + "'");
  }

  [[nodiscard]] Expected<void>
  constrainInstruction(MachineInstr &Instruction, MachineFunction &Function,
                       const TargetDescription &Target) const override {
    std::vector<std::pair<Register, RegisterClassID>> PlannedClasses;
    for (const MachineOperand &Operand : Instruction.operands()) {
      const Register *Reg = Operand.getIf<Register>();
      if (Reg == nullptr || !Reg->isVirtual())
        continue;
      auto Required = requiredRegisterClass(Function, *Reg, Target);
      if (!Required)
        return std::unexpected(Required.error());
      if (std::ranges::find_if(PlannedClasses, [Reg](const auto &Entry) {
            return Entry.first == *Reg;
          }) == PlannedClasses.end())
        PlannedClasses.emplace_back(*Reg, *Required);
    }
    for (const auto &[Reg, Required] : PlannedClasses) {
      auto Info = Function.registerInfo().virtualRegisterInfo(Reg);
      if (!Info)
        return std::unexpected(Info.error());
      if (Info->get().RegClass)
        continue;
      auto SetClass = Function.registerInfo().setRegisterClass(Reg, Required);
      if (!SetClass)
        return std::unexpected(SetClass.error());
    }
    return {};
  }

  [[nodiscard]] Expected<void> validateInstruction(const MachineInstr &Instruction,
                                                   const MachineFunction &Function,
                                                   const TargetDescription &Target) const override {
    for (const MachineOperand &Operand : Instruction.operands()) {
      const Register *Reg = Operand.getIf<Register>();
      if (Reg == nullptr || !Reg->isVirtual())
        continue;
      auto Required = requiredRegisterClass(Function, *Reg, Target);
      if (!Required)
        return std::unexpected(Required.error());
    }
    return {};
  }
};

class RISCVInstructionSelector final : public TargetInstructionSelector {
public:
  [[nodiscard]] Expected<void>
  constrainSelectedInstruction(MachineInstr &Instruction, MachineFunction &Function,
                               const TargetDescription &Target) const override {
    return registerBankInfo().constrainInstruction(Instruction, Function, Target);
  }

  [[nodiscard]] Expected<void> canSelect(const MachineInstr &Instruction,
                                         const MachineFunction &Function,
                                         const TargetDescription &Target) const override {
    if (Instruction.opcode() == targetOpcode(InstructionOpcode::G_SLLW)) {
      const std::optional<LLT> Type = resultType(Function, Instruction);
      const std::optional<LLT> Left = operandType(Function, Instruction, 1);
      const std::optional<LLT> Right = operandType(Function, Instruction, 2);
      return Type && Left && Right && isScalarGPRType(*Type) &&
                     Type->scalarBitWidth() == 64 && *Left == *Type && *Right == *Type
                 ? Expected<void>{}
                 : makeUnexpected(ErrorCode::Unsupported,
                                  "RISC-V G_SLLW requires three 64-bit GPR operands");
    }
    if (!isGenericOpcode(Instruction.opcode()))
      return {};
    const GenericOpcode Generic = static_cast<GenericOpcode>(Instruction.opcode().value());
    const std::optional<LLT> Type = resultType(Function, Instruction);
    const auto Integer = [Type] { return Type && isSupportedIntegerType(*Type); };
    const auto Float = [Type] { return Type && isRVFloatType(*Type); };
    auto Classification = legalizerInfo().classify(Instruction, Function, Target);
    if (!Classification)
      return std::unexpected(Classification.error());
    if (*Classification != LegalizationAction::Legal) {
      std::string Message = "RISC-V selector cannot select opcode '" +
                            std::string(getGenericOpcodeName(Generic)) + "'";
      if (Type)
        Message += " with result type '" + Type->toString() + "'";
      Message += ": the legalization rule explicitly marks this combination unsupported";
      return makeUnexpected(ErrorCode::Unsupported, std::move(Message));
    }
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
    case GenericOpcode::G_FADD:
    case GenericOpcode::G_FSUB:
    case GenericOpcode::G_FMUL:
    case GenericOpcode::G_FDIV:
    case GenericOpcode::G_FMA:
    case GenericOpcode::G_FSQRT:
    case GenericOpcode::G_FNEG:
    case GenericOpcode::G_FABS:
    case GenericOpcode::G_FCOPYSIGN:
      if (!Float())
        return makeUnexpected(ErrorCode::Unsupported,
                              "RISC-V selector requires f32 or f64 floating values");
      return {};
    case GenericOpcode::G_FCONSTANT: {
      const MachineOperand *Immediate = Instruction.operandAt(1);
      const auto *Floating =
          Immediate == nullptr ? nullptr : Immediate->getIf<FloatingImmediateOperand>();
      const std::optional<FloatSemantics> Semantics = Type ? Type->floatSemantics() : std::nullopt;
      if (!Float() || Floating == nullptr || !Semantics || Floating->Bits.semantics() != *Semantics)
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
    case GenericOpcode::G_BITCAST:
    case GenericOpcode::G_TRUNC:
    case GenericOpcode::G_SEXT:
    case GenericOpcode::G_SEXT_INREG:
    case GenericOpcode::G_ZEXT:
    case GenericOpcode::G_ICMP:
    case GenericOpcode::G_FCMP:
    case GenericOpcode::G_FPEXT:
    case GenericOpcode::G_FPTRUNC:
    case GenericOpcode::G_FPTOSI:
    case GenericOpcode::G_FPTOUI:
    case GenericOpcode::G_SITOFP:
    case GenericOpcode::G_UITOFP:
    case GenericOpcode::G_FMINNUM:
    case GenericOpcode::G_FMAXNUM:
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
    auto Match = canSelect(Instruction, Function, Target);
    if (!Match)
      return std::unexpected(Match.error());
    if (Instruction.opcode() == targetOpcode(InstructionOpcode::G_SLLW))
      return Instruction.setOpcode(targetOpcode(InstructionOpcode::SLLW));
    const GenericOpcode Generic = static_cast<GenericOpcode>(Instruction.opcode().value());
    const std::optional<LLT> Type = resultType(Function, Instruction);

    const auto SelectDirect = [&Instruction](InstructionOpcode Selected) {
      return Instruction.setOpcode(targetOpcode(Selected));
    };
    switch (Generic) {
    case GenericOpcode::G_ADD:
      return selectIntegerBinary(Instruction, Type, InstructionOpcode::ADD,
                                 InstructionOpcode::ADDW);
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
      return SelectDirect(InstructionOpcode::AND);
    case GenericOpcode::G_OR:
      return SelectDirect(InstructionOpcode::OR);
    case GenericOpcode::G_XOR:
      return SelectDirect(InstructionOpcode::XOR);
    case GenericOpcode::G_SHL:
      return selectIntegerBinary(Instruction, Type, InstructionOpcode::SLL,
                                 InstructionOpcode::SLLW);
    case GenericOpcode::G_LSHR:
      return selectNormalizedIntegerBinary(Instruction, Function, Target, Type,
                                           InstructionOpcode::SRL, InstructionOpcode::SRLW, false,
                                           false);
    case GenericOpcode::G_ASHR:
      return selectNormalizedIntegerBinary(Instruction, Function, Target, Type,
                                           InstructionOpcode::SRA, InstructionOpcode::SRAW, true,
                                           false);
    case GenericOpcode::G_PTR_ADD:
      return SelectDirect(InstructionOpcode::ADD);
    case GenericOpcode::G_PTRMASK:
      return SelectDirect(InstructionOpcode::AND);
    case GenericOpcode::G_LOAD:
      return selectLoad(Instruction, Type);
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
    case GenericOpcode::G_BITCAST:
      return selectBitcast(Instruction, Function);
    case GenericOpcode::G_FADD:
      return selectRoundingBinary(Instruction, Type, InstructionOpcode::FADD_S,
                                  InstructionOpcode::FADD_D);
    case GenericOpcode::G_FSUB:
      return selectRoundingBinary(Instruction, Type, InstructionOpcode::FSUB_S,
                                  InstructionOpcode::FSUB_D);
    case GenericOpcode::G_FMUL:
      return selectRoundingBinary(Instruction, Type, InstructionOpcode::FMUL_S,
                                  InstructionOpcode::FMUL_D);
    case GenericOpcode::G_FDIV:
      return selectRoundingBinary(Instruction, Type, InstructionOpcode::FDIV_S,
                                  InstructionOpcode::FDIV_D);
    case GenericOpcode::G_FMA:
      return selectRoundingTernary(Instruction, Type, InstructionOpcode::FMADD_S,
                                   InstructionOpcode::FMADD_D);
    case GenericOpcode::G_FSQRT:
      return selectRoundingUnary(Instruction, Type, InstructionOpcode::FSQRT_S,
                                 InstructionOpcode::FSQRT_D);
    case GenericOpcode::G_FNEG:
      return selectSignInjection(Instruction, Type, InstructionOpcode::FSGNJN_S,
                                 InstructionOpcode::FSGNJN_D);
    case GenericOpcode::G_FABS:
      return selectSignInjection(Instruction, Type, InstructionOpcode::FSGNJX_S,
                                 InstructionOpcode::FSGNJX_D);
    case GenericOpcode::G_FCOPYSIGN:
      return selectFloatingMinMax(Instruction, Type, InstructionOpcode::FSGNJ_S,
                                  InstructionOpcode::FSGNJ_D);
    case GenericOpcode::G_ANYEXT:
    case GenericOpcode::G_SEXT:
    case GenericOpcode::G_ZEXT:
      return selectIntegerExtension(Instruction, Function, Target, Generic, Type);
    case GenericOpcode::G_TRUNC:
      return Instruction.setOpcode(opcode(GenericOpcode::COPY));
    case GenericOpcode::G_SEXT_INREG:
      return selectSignExtendInRegister(Instruction, Function, Target, Type);
    case GenericOpcode::G_ICMP:
      return selectIntegerCompare(Instruction, Function, Target);
    case GenericOpcode::G_FCMP:
      return selectFloatingCompare(Instruction, Function, Target);
    case GenericOpcode::G_FPEXT:
      return selectRoundingUnary(Instruction, Type, InstructionOpcode::FCVT_D_S,
                                 InstructionOpcode::FCVT_D_S);
    case GenericOpcode::G_FPTRUNC:
      return selectRoundingUnary(Instruction, Type, InstructionOpcode::FCVT_S_D,
                                 InstructionOpcode::FCVT_S_D);
    case GenericOpcode::G_FPTOSI:
    case GenericOpcode::G_FPTOUI:
      return selectFloatToInteger(Instruction, Function, Type, Generic == GenericOpcode::G_FPTOUI);
    case GenericOpcode::G_SITOFP:
    case GenericOpcode::G_UITOFP:
      return selectIntegerToFloat(Instruction, Function, Target, Type,
                                  Generic == GenericOpcode::G_UITOFP);
    case GenericOpcode::G_FMINNUM:
      return selectFloatingMinMax(Instruction, Type, InstructionOpcode::FMIN_S,
                                  InstructionOpcode::FMIN_D);
    case GenericOpcode::G_FMAXNUM:
      return selectFloatingMinMax(Instruction, Type, InstructionOpcode::FMAX_S,
                                  InstructionOpcode::FMAX_D);
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
  [[nodiscard]] static Expected<void> replaceImmediate(MachineInstr &Instruction, std::size_t Index,
                                                       std::int64_t Value) {
    if (Instruction.operandAt(Index) == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V instruction is missing an immediate operand");
    auto Erased = Instruction.eraseOperand(Index);
    if (!Erased)
      return std::unexpected(Erased.error());
    return Instruction.insertOperand(Index, MachineOperand::immediate(Value));
  }

  [[nodiscard]] static Expected<MachineInstr *>
  buildBinaryAfter(MachineIRBuilder &Builder, MachineInstr &Anchor, InstructionOpcode Opcode,
                   Register Def, Register Left, Register Right) {
    auto Point = Builder.setInsertPointAfter(Anchor);
    if (!Point)
      return std::unexpected(Point.error());
    auto Pending = Builder.buildInstr(targetOpcode(Opcode));
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
    return *Built;
  }

  [[nodiscard]] static Expected<MachineInstr *>
  buildImmediateAfter(MachineIRBuilder &Builder, MachineInstr &Anchor, InstructionOpcode Opcode,
                      Register Def, Register Source, std::int64_t Value) {
    auto Point = Builder.setInsertPointAfter(Anchor);
    if (!Point)
      return std::unexpected(Point.error());
    auto Pending = Builder.buildInstr(targetOpcode(Opcode));
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
    return *Built;
  }

  [[nodiscard]] static Expected<MachineInstr *>
  buildImmediateBefore(MachineIRBuilder &Builder, MachineInstr &Anchor, InstructionOpcode Opcode,
                       Register Def, Register Source, std::int64_t Value) {
    auto Point = Builder.setInsertPointBefore(Anchor);
    if (!Point)
      return std::unexpected(Point.error());
    auto Pending = Builder.buildInstr(targetOpcode(Opcode));
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
    return *Built;
  }

  [[nodiscard]] static Expected<Register>
  normalizeIntegerBefore(MachineInstr &Anchor, MachineFunction &Function,
                         const TargetDescription &Target, Register Source, LLT Type, bool Signed) {
    if (!isSupportedIntegerScalar(Type))
      return makeUnexpected(ErrorCode::Unsupported,
                            "RISC-V integer normalization requires a scalar i1-i64 type");
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

  [[nodiscard]] static Expected<MachineInstr *> buildUnaryAfter(MachineIRBuilder &Builder,
                                                                MachineInstr &Anchor,
                                                                InstructionOpcode Opcode,
                                                                Register Def, Register Source) {
    auto Point = Builder.setInsertPointAfter(Anchor);
    if (!Point)
      return std::unexpected(Point.error());
    auto Pending = Builder.buildInstr(targetOpcode(Opcode));
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
    return *Built;
  }

  [[nodiscard]] static Expected<MachineInstr *>
  buildGPRSelectAfter(MachineIRBuilder &Builder, MachineInstr &Anchor, MachineFunction &Function,
                      const TargetDescription &Target, LLT ValueType, Register Destination,
                      Register Condition, Register TrueValue, Register FalseValue) {
    auto IntegerType = LLT::makeInteger(ValueType.scalarBitWidth());
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

  [[nodiscard]] static Expected<void> selectDivRem(MachineInstr &Instruction,
                                                   MachineFunction &Function,
                                                   const TargetDescription &Target,
                                                   std::optional<LLT> Type, bool Signed) {
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
    auto SetDivision = Instruction.setOpcode(targetOpcode(Division));
    if (!SetDivision)
      return std::unexpected(SetDivision.error());
    MachineIRBuilder Builder(Function, Target);
    auto BuiltRemainder = buildBinaryAfter(Builder, Instruction, RemainderOpcode, RemainderRegister,
                                           *LeftRegister, *RightRegister);
    if (!BuiltRemainder)
      return std::unexpected(BuiltRemainder.error());
    return {};
  }

  [[nodiscard]] static Expected<void> selectMultiplyHigh(MachineInstr &Instruction,
                                                         MachineFunction &Function,
                                                         const TargetDescription &Target,
                                                         std::optional<LLT> Type, bool Signed) {
    if (!Type)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V high multiplication has no result type");
    if (Type->scalarBitWidth() == 64) {
      return Instruction.setOpcode(
          targetOpcode(Signed ? InstructionOpcode::MULH : InstructionOpcode::MULHU));
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
    auto I64 = LLT::makeInteger(64);
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
      auto SetOpcode = Instruction.setOpcode(targetOpcode(InstructionOpcode::ADDIW));
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
      auto SetOpcode = Instruction.setOpcode(targetOpcode(InstructionOpcode::SLLI));
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

  [[nodiscard]] static Expected<void> selectIntegerMinMax(MachineInstr &Instruction,
                                                          MachineFunction &Function,
                                                          const TargetDescription &Target,
                                                          std::optional<LLT> Type,
                                                          GenericOpcode Generic) {
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
    auto I1 = LLT::makeInteger(1);
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
        targetOpcode(Unsigned ? InstructionOpcode::SLTU : InstructionOpcode::SLT));
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

  [[nodiscard]] static Expected<void> selectValue(MachineInstr &Instruction,
                                                  MachineFunction &Function,
                                                  const TargetDescription &Target,
                                                  std::optional<LLT> Type) {
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
    auto I1 = LLT::makeInteger(1);
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
    auto SetNormalize = Instruction.setOpcode(targetOpcode(InstructionOpcode::ANDI));
    if (!SetNormalize)
      return std::unexpected(SetNormalize.error());

    MachineIRBuilder Builder(Function, Target);
    if (!Type->isFloat()) {
      auto Selected =
          buildGPRSelectAfter(Builder, Instruction, Function, Target, *Type, DestinationRegister,
                              *NormalizedCondition, TrueRegister, FalseRegister);
      if (!Selected)
        return std::unexpected(Selected.error());
      return {};
    }

    auto IntegerType = LLT::makeInteger(Type->scalarBitWidth());
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

  [[nodiscard]] static Expected<void> selectAbsoluteValue(MachineInstr &Instruction,
                                                          MachineFunction &Function,
                                                          const TargetDescription &Target,
                                                          std::optional<LLT> Type) {
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
        targetOpcode(IsWord ? InstructionOpcode::SRAIW : InstructionOpcode::SRAI));
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

  [[nodiscard]] static Expected<std::pair<Register, MachineInstr *>>
  buildSignedOverflowAfter(MachineIRBuilder &Builder, MachineInstr &Anchor,
                           MachineFunction &Function, const TargetDescription &Target, LLT Type,
                           Register Result, Register Left, Register Right, bool IsAdd) {
    auto FirstDifference = createTemporary(Function, Type, Target);
    auto SecondDifference = createTemporary(Function, Type, Target);
    auto SignBits = createTemporary(Function, Type, Target);
    auto I1 = LLT::makeInteger(1);
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

  [[nodiscard]] static Expected<void>
  selectOverflow(MachineInstr &Instruction, MachineFunction &Function,
                 const TargetDescription &Target, std::optional<LLT> Type, GenericOpcode Generic) {
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
    auto SetArithmetic = Instruction.setOpcode(targetOpcode(Arithmetic));
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

    auto I64 = LLT::makeInteger(64);
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

  [[nodiscard]] static Expected<void> selectExtendedOverflow(MachineInstr &Instruction,
                                                             MachineFunction &Function,
                                                             const TargetDescription &Target,
                                                             std::optional<LLT> Type,
                                                             GenericOpcode Generic) {
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
    auto I1 = LLT::makeInteger(1);
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
    auto SetArithmetic = Instruction.setOpcode(targetOpcode(Arithmetic));
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

  [[nodiscard]] static Expected<void>
  selectAtomicRMW(MachineInstr &Instruction, MachineFunction &Function,
                  const TargetDescription &Target, std::optional<LLT> Type, GenericOpcode Generic) {
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
      auto Pending = Builder.buildInstr(
          targetOpcode(IsWord ? InstructionOpcode::SUBW : InstructionOpcode::SUB));
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
    return Instruction.setOpcode(targetOpcode(Selected));
  }

  [[nodiscard]] static Expected<void> selectIntegerExtension(MachineInstr &Instruction,
                                                             MachineFunction &Function,
                                                             const TargetDescription &Target,
                                                             GenericOpcode Generic,
                                                             std::optional<LLT> Type) {
    const std::optional<LLT> SourceType = operandType(Function, Instruction, 1);
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
      return Instruction.setOpcode(targetOpcode(InstructionOpcode::ADDIW));
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
    auto SetOpcode = Instruction.setOpcode(targetOpcode(ShiftLeft));
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

  [[nodiscard]] static Expected<void> selectSignExtendInRegister(MachineInstr &Instruction,
                                                                 MachineFunction &Function,
                                                                 const TargetDescription &Target,
                                                                 std::optional<LLT> Type) {
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
      return Instruction.setOpcode(targetOpcode(InstructionOpcode::ADDIW));
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
    auto SetOpcode = Instruction.setOpcode(targetOpcode(ShiftLeft));
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

  [[nodiscard]] static Expected<void> selectIntegerCompare(MachineInstr &Instruction,
                                                           MachineFunction &Function,
                                                           const TargetDescription &Target) {
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
    const std::optional<LLT> LeftType = operandType(Function, Instruction, 2);
    if (Predicate == nullptr || Predicate->integerPredicate() == nullptr ||
        Destination == nullptr || Left == nullptr || Right == nullptr || !LeftType)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V integer comparison has invalid operands");
    const IntegerPredicate Condition = *Predicate->integerPredicate();
    const Register DestinationRegister = *Destination;
    const bool SignedOperands = Condition == IntegerPredicate::SignedGreaterThan ||
                                Condition == IntegerPredicate::SignedGreaterEqual ||
                                Condition == IntegerPredicate::SignedLessThan ||
                                Condition == IntegerPredicate::SignedLessEqual;
    auto LeftRegister =
        normalizeIntegerBefore(Instruction, Function, Target, *Left, *LeftType, SignedOperands);
    auto RightRegister =
        normalizeIntegerBefore(Instruction, Function, Target, *Right, *LeftType, SignedOperands);
    if (!LeftRegister || !RightRegister)
      return std::unexpected(!LeftRegister ? LeftRegister.error() : RightRegister.error());
    auto ErasePredicate = Instruction.eraseOperand(1);
    if (!ErasePredicate)
      return std::unexpected(ErasePredicate.error());

    const auto SetSources = [&Instruction](Register NewLeft, Register NewRight) -> Expected<void> {
      auto SetLeft = Instruction.operandAt(1)->setRegister(NewLeft);
      if (!SetLeft)
        return std::unexpected(SetLeft.error());
      return Instruction.operandAt(2)->setRegister(NewRight);
    };
    const auto StrictOpcode = [](IntegerPredicate Value) -> std::optional<InstructionOpcode> {
      switch (Value) {
      case IntegerPredicate::SignedLessThan:
        return InstructionOpcode::SLT;
      case IntegerPredicate::UnsignedLessThan:
        return InstructionOpcode::SLTU;
      default:
        return std::nullopt;
      }
    };

    IntegerPredicate StrictCondition = Condition;
    bool Swap = false;
    bool Invert = false;
    switch (Condition) {
    case IntegerPredicate::Equal:
    case IntegerPredicate::NotEqual:
      break;
    case IntegerPredicate::SignedGreaterThan:
      StrictCondition = IntegerPredicate::SignedLessThan;
      Swap = true;
      break;
    case IntegerPredicate::SignedGreaterEqual:
      StrictCondition = IntegerPredicate::SignedLessThan;
      Invert = true;
      break;
    case IntegerPredicate::SignedLessThan:
      StrictCondition = IntegerPredicate::SignedLessThan;
      break;
    case IntegerPredicate::SignedLessEqual:
      StrictCondition = IntegerPredicate::SignedLessThan;
      Swap = true;
      Invert = true;
      break;
    case IntegerPredicate::UnsignedGreaterThan:
      StrictCondition = IntegerPredicate::UnsignedLessThan;
      Swap = true;
      break;
    case IntegerPredicate::UnsignedGreaterEqual:
      StrictCondition = IntegerPredicate::UnsignedLessThan;
      Invert = true;
      break;
    case IntegerPredicate::UnsignedLessThan:
      StrictCondition = IntegerPredicate::UnsignedLessThan;
      break;
    case IntegerPredicate::UnsignedLessEqual:
      StrictCondition = IntegerPredicate::UnsignedLessThan;
      Swap = true;
      Invert = true;
      break;
    case IntegerPredicate::Count:
      return makeUnexpected(ErrorCode::Unsupported,
                            "RISC-V integer comparison predicate is invalid");
    }

    MachineIRBuilder Builder(Function, Target);
    if (Condition == IntegerPredicate::Equal || Condition == IntegerPredicate::NotEqual) {
      auto Temporary = createTemporary(Function, *LeftType, Target);
      if (!Temporary)
        return std::unexpected(Temporary.error());
      auto SetDestination = Instruction.operandAt(0)->setRegister(*Temporary);
      if (!SetDestination)
        return std::unexpected(SetDestination.error());
      auto Set = SetSources(*LeftRegister, *RightRegister);
      if (!Set)
        return std::unexpected(Set.error());
      auto SetOpcode = Instruction.setOpcode(targetOpcode(InstructionOpcode::XOR));
      if (!SetOpcode)
        return std::unexpected(SetOpcode.error());
      if (Condition == IntegerPredicate::Equal) {
        auto Result = buildImmediateAfter(Builder, Instruction, InstructionOpcode::SLTIU,
                                          DestinationRegister, *Temporary, 1);
        if (!Result)
          return std::unexpected(Result.error());
      } else {
        auto Zero = physicalRegister(PhysicalRegisterID::X0);
        if (!Zero)
          return std::unexpected(Zero.error());
        auto Result =
            buildBinaryAfter(Builder, Instruction, InstructionOpcode::SLTU, DestinationRegister,
                             Register::fromPhysical(*Zero), *Temporary);
        if (!Result)
          return std::unexpected(Result.error());
      }
      return {};
    }

    const std::optional<InstructionOpcode> OpcodeValue = StrictOpcode(StrictCondition);
    if (!OpcodeValue)
      return makeUnexpected(ErrorCode::Unsupported,
                            "RISC-V integer comparison predicate is invalid");
    if (Invert) {
      auto Temporary = createTemporary(Function, *LeftType, Target);
      if (!Temporary)
        return std::unexpected(Temporary.error());
      auto SetDestination = Instruction.operandAt(0)->setRegister(*Temporary);
      if (!SetDestination)
        return std::unexpected(SetDestination.error());
      const Register Base = *Temporary;
      if (Swap) {
        auto Set = SetSources(*RightRegister, *LeftRegister);
        if (!Set)
          return std::unexpected(Set.error());
      } else {
        auto Set = SetSources(*LeftRegister, *RightRegister);
        if (!Set)
          return std::unexpected(Set.error());
      }
      auto SetOpcode = Instruction.setOpcode(targetOpcode(*OpcodeValue));
      if (!SetOpcode)
        return std::unexpected(SetOpcode.error());
      auto Result = buildImmediateAfter(Builder, Instruction, InstructionOpcode::XORI,
                                        DestinationRegister, Base, 1);
      if (!Result)
        return std::unexpected(Result.error());
      return {};
    }
    if (Swap) {
      auto Set = SetSources(*RightRegister, *LeftRegister);
      if (!Set)
        return std::unexpected(Set.error());
    } else {
      auto Set = SetSources(*LeftRegister, *RightRegister);
      if (!Set)
        return std::unexpected(Set.error());
    }
    return Instruction.setOpcode(targetOpcode(*OpcodeValue));
  }

  [[nodiscard]] static Expected<void> selectFloatingCompare(MachineInstr &Instruction,
                                                            MachineFunction &Function,
                                                            const TargetDescription &Target) {
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
    const std::optional<LLT> FloatType = operandType(Function, Instruction, 2);
    const std::optional<LLT> ResultType = resultType(Function, Instruction);
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
      return Instruction.setOpcode(targetOpcode(OpcodeValue));
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
      return Instruction.setOpcode(targetOpcode(InstructionOpcode::ADDI));
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

  [[nodiscard]] static Expected<void> selectFloatToInteger(MachineInstr &Instruction,
                                                           MachineFunction &Function,
                                                           std::optional<LLT> Type, bool Unsigned) {
    const std::optional<LLT> Source = operandType(Function, Instruction, 1);
    if (!Type || !Source)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V floating-to-integer conversion has invalid types");
    auto Rounding = appendImmediate(Instruction, 1);
    if (!Rounding)
      return std::unexpected(Rounding.error());
    const bool IsWord = Type->scalarBitWidth() <= 32;
    if (Source->scalarBitWidth() == 32) {
      return Instruction.setOpcode(targetOpcode(
          IsWord ? (Unsigned ? InstructionOpcode::FCVT_WU_S : InstructionOpcode::FCVT_W_S)
                 : (Unsigned ? InstructionOpcode::FCVT_LU_S : InstructionOpcode::FCVT_L_S)));
    }
    return Instruction.setOpcode(targetOpcode(
        IsWord ? (Unsigned ? InstructionOpcode::FCVT_WU_D : InstructionOpcode::FCVT_W_D)
               : (Unsigned ? InstructionOpcode::FCVT_LU_D : InstructionOpcode::FCVT_L_D)));
  }

  [[nodiscard]] static Expected<void> selectIntegerToFloat(MachineInstr &Instruction,
                                                           MachineFunction &Function,
                                                           const TargetDescription &Target,
                                                           std::optional<LLT> Type, bool Unsigned) {
    const std::optional<LLT> Source = operandType(Function, Instruction, 1);
    const MachineOperand *SourceOperand = Instruction.operandAt(1);
    const Register *SourceRegister =
        SourceOperand == nullptr ? nullptr : SourceOperand->getIf<Register>();
    if (!Type || !Source || SourceRegister == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V integer-to-floating conversion has invalid types");
    auto Normalized =
        normalizeIntegerBefore(Instruction, Function, Target, *SourceRegister, *Source, !Unsigned);
    if (!Normalized)
      return std::unexpected(Normalized.error());
    auto SetSource = Instruction.operandAt(1)->setRegister(*Normalized);
    if (!SetSource)
      return std::unexpected(SetSource.error());
    auto Rounding = appendImmediate(Instruction, 0);
    if (!Rounding)
      return std::unexpected(Rounding.error());
    const bool IsWord = Source->scalarBitWidth() <= 32;
    if (Type->scalarBitWidth() == 32) {
      return Instruction.setOpcode(targetOpcode(
          IsWord ? (Unsigned ? InstructionOpcode::FCVT_S_WU : InstructionOpcode::FCVT_S_W)
                 : (Unsigned ? InstructionOpcode::FCVT_S_LU : InstructionOpcode::FCVT_S_L)));
    }
    return Instruction.setOpcode(targetOpcode(
        IsWord ? (Unsigned ? InstructionOpcode::FCVT_D_WU : InstructionOpcode::FCVT_D_W)
               : (Unsigned ? InstructionOpcode::FCVT_D_LU : InstructionOpcode::FCVT_D_L)));
  }

  [[nodiscard]] static Expected<void> selectFloatingMinMax(MachineInstr &Instruction,
                                                           std::optional<LLT> Type,
                                                           InstructionOpcode Single,
                                                           InstructionOpcode Double) {
    if (Type && Type->isFloat() && Type->scalarBitWidth() == 32)
      return Instruction.setOpcode(targetOpcode(Single));
    if (Type && Type->isFloat() && Type->scalarBitWidth() == 64)
      return Instruction.setOpcode(targetOpcode(Double));
    return makeUnexpected(ErrorCode::Unsupported,
                          "RISC-V cannot select this floating min/max type");
  }

  [[nodiscard]] static Expected<void> selectFence(MachineInstr &Instruction) {
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
    return Instruction.setOpcode(targetOpcode(InstructionOpcode::FENCE));
  }

  [[nodiscard]] static Expected<void> selectIndirectBranch(MachineInstr &Instruction) {
    auto Zero = physicalRegister(PhysicalRegisterID::X0);
    if (!Zero)
      return std::unexpected(Zero.error());
    auto Inserted = insertPhysicalRegister(Instruction, 0, *Zero, RegisterOperandFlag::Define);
    if (!Inserted)
      return std::unexpected(Inserted.error());
    auto Immediate = appendImmediate(Instruction, 0);
    if (!Immediate)
      return std::unexpected(Immediate.error());
    return Instruction.setOpcode(targetOpcode(InstructionOpcode::JALR));
  }

  [[nodiscard]] static Expected<void> selectTrap(MachineInstr &Instruction) {
    while (Instruction.operandCount() != 0) {
      auto Erased = Instruction.eraseOperand(Instruction.operandCount() - 1U);
      if (!Erased)
        return std::unexpected(Erased.error());
    }
    return Instruction.setOpcode(targetOpcode(InstructionOpcode::EBREAK));
  }

  [[nodiscard]] static Expected<void> selectBitcast(MachineInstr &Instruction,
                                                    MachineFunction &Function) {
    const std::optional<LLT> DestinationType = resultType(Function, Instruction);
    const std::optional<LLT> SourceType = operandType(Function, Instruction, 1);
    if (!DestinationType || !SourceType)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V bitcast has invalid register types");
    if (DestinationType->isInteger() && SourceType->isFloat()) {
      return Instruction.setOpcode(targetOpcode(SourceType->scalarBitWidth() == 32
                                                    ? InstructionOpcode::FMV_X_W
                                                    : InstructionOpcode::FMV_X_D));
    }
    if (DestinationType->isFloat() && SourceType->isInteger()) {
      return Instruction.setOpcode(targetOpcode(DestinationType->scalarBitWidth() == 32
                                                    ? InstructionOpcode::FMV_W_X
                                                    : InstructionOpcode::FMV_D_X));
    }
    return makeUnexpected(ErrorCode::Unsupported, "RISC-V bitcast pair is unsupported");
  }

  [[nodiscard]] static Expected<void> selectIntegerBinary(MachineInstr &Instruction,
                                                          std::optional<LLT> Type,
                                                          InstructionOpcode DoubleWord,
                                                          InstructionOpcode Word) {
    if (!Type || !isSupportedIntegerType(*Type))
      return makeUnexpected(ErrorCode::Unsupported,
                            "RISC-V integer instruction requires a supported scalar integer");
    const bool IsWord = Type->isInteger() && Type->scalarBitWidth() <= 32;
    return Instruction.setOpcode(targetOpcode(IsWord ? Word : DoubleWord));
  }

  [[nodiscard]] static Expected<void>
  selectNormalizedIntegerBinary(MachineInstr &Instruction, MachineFunction &Function,
                                const TargetDescription &Target, std::optional<LLT> Type,
                                InstructionOpcode DoubleWord, InstructionOpcode Word, bool Signed,
                                bool NormalizeRight) {
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
    Register SelectedRight = *Right;
    if (NormalizeRight) {
      auto NormalizedRight =
          normalizeIntegerBefore(Instruction, Function, Target, *Right, *Type, Signed);
      if (!NormalizedRight)
        return std::unexpected(NormalizedRight.error());
      SelectedRight = *NormalizedRight;
    }
    auto SetLeft = Instruction.operandAt(1)->setRegister(*NormalizedLeft);
    if (!SetLeft)
      return std::unexpected(SetLeft.error());
    auto SetRight = Instruction.operandAt(2)->setRegister(SelectedRight);
    if (!SetRight)
      return std::unexpected(SetRight.error());
    const bool IsWord = Type->scalarBitWidth() <= 32;
    return Instruction.setOpcode(targetOpcode(IsWord ? Word : DoubleWord));
  }

  [[nodiscard]] static Expected<void> selectLoad(MachineInstr &Instruction,
                                                 std::optional<LLT> Type) {
    if (!Type)
      return makeUnexpected(ErrorCode::VerificationError,
                            "load result has no virtual-register type");
    const auto Bytes = memoryAccessBytes(Instruction);
    if (!Bytes)
      return makeUnexpected(ErrorCode::Unsupported,
                            "RISC-V load size does not match its result type");
    if (Instruction.memOperands().front()->isAtomic()) {
      if (!isSupportedIntegerType(*Type) || (*Bytes != 4 && *Bytes != 8))
        return makeUnexpected(ErrorCode::Unsupported,
                              "RISC-V atomic load requires a 32-bit or 64-bit GPR value");
      return Instruction.setOpcode(
          targetOpcode(*Bytes == 4 ? InstructionOpcode::LR_W : InstructionOpcode::LR_D));
    }
    auto AppendOffset = appendImmediate(Instruction, 0);
    if (!AppendOffset)
      return std::unexpected(AppendOffset.error());
    if (isSupportedIntegerType(*Type)) {
      const InstructionOpcode Selected = *Bytes == 1   ? InstructionOpcode::LB
                                         : *Bytes == 2 ? InstructionOpcode::LH
                                         : *Bytes == 4 ? InstructionOpcode::LW
                                         : *Bytes == 8 ? InstructionOpcode::LD
                                                       : InstructionOpcode::Invalid;
      if (Selected == InstructionOpcode::Invalid)
        return makeUnexpected(ErrorCode::Unsupported, "RISC-V integer load width is unsupported");
      return Instruction.setOpcode(targetOpcode(Selected));
    }
    if (Type->isFloat() && Type->scalarBitWidth() == 32)
      return Instruction.setOpcode(targetOpcode(InstructionOpcode::FLW));
    if (Type->isFloat() && Type->scalarBitWidth() == 64)
      return Instruction.setOpcode(targetOpcode(InstructionOpcode::FLD));
    return makeUnexpected(ErrorCode::Unsupported, "RISC-V cannot select this generic load type");
  }

  [[nodiscard]] static Expected<void> selectExtLoad(MachineInstr &Instruction,
                                                    std::optional<LLT> Type, bool Signed) {
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
    return Instruction.setOpcode(targetOpcode(Selected));
  }

  [[nodiscard]] static Expected<void> selectFloatExtLoad(MachineInstr &Instruction,
                                                         MachineFunction &Function,
                                                         const TargetDescription &Target) {
    const MachineOperand *DestinationOperand = Instruction.operandAt(0);
    const Register *Destination =
        DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
    if (Destination == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V floating extension load has no destination register");
    const Register DestinationRegister = *Destination;
    auto F32 = LLT::makeFloat(FloatSemantics::IEEESingle);
    if (!F32)
      return std::unexpected(F32.error());
    auto Temporary = createTemporary(Function, *F32, Target);
    if (!Temporary)
      return std::unexpected(Temporary.error());
    auto SetDestination = Instruction.operandAt(0)->setRegister(*Temporary);
    if (!SetDestination)
      return std::unexpected(SetDestination.error());
    auto Immediate = appendImmediate(Instruction, 0);
    if (!Immediate)
      return std::unexpected(Immediate.error());
    auto SetOpcode = Instruction.setOpcode(targetOpcode(InstructionOpcode::FLW));
    if (!SetOpcode)
      return std::unexpected(SetOpcode.error());
    MachineIRBuilder Builder(Function, Target);
    auto Conversion = buildImmediateAfter(Builder, Instruction, InstructionOpcode::FCVT_D_S,
                                          DestinationRegister, *Temporary, 0);
    if (!Conversion)
      return std::unexpected(Conversion.error());
    return {};
  }

  [[nodiscard]] static Expected<void> selectFloatTruncStore(MachineInstr &Instruction,
                                                            MachineFunction &Function,
                                                            const TargetDescription &Target) {
    const MachineOperand *ValueOperand = Instruction.operandAt(0);
    const Register *Value = ValueOperand == nullptr ? nullptr : ValueOperand->getIf<Register>();
    if (Value == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V floating truncation store has no value register");
    const Register ValueRegister = *Value;
    auto F32 = LLT::makeFloat(FloatSemantics::IEEESingle);
    if (!F32)
      return std::unexpected(F32.error());
    auto Temporary = createTemporary(Function, *F32, Target);
    if (!Temporary)
      return std::unexpected(Temporary.error());
    MachineIRBuilder Builder(Function, Target);
    auto Point = Builder.setInsertPointBefore(Instruction);
    if (!Point)
      return std::unexpected(Point.error());
    auto Conversion = Builder.buildInstr(targetOpcode(InstructionOpcode::FCVT_S_D));
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
    auto SetValue = Instruction.operandAt(0)->setRegister(*Temporary);
    if (!SetValue)
      return std::unexpected(SetValue.error());
    auto Immediate = appendImmediate(Instruction, 0);
    if (!Immediate)
      return std::unexpected(Immediate.error());
    return Instruction.setOpcode(targetOpcode(InstructionOpcode::FSW));
  }

  [[nodiscard]] static Expected<void> selectStore(MachineInstr &Instruction,
                                                  MachineFunction &Function) {
    const MachineOperand *Value = Instruction.operandAt(0);
    const std::optional<LLT> Type =
        Value == nullptr ? std::nullopt : registerType(Function, *Value);
    if (!Type)
      return makeUnexpected(ErrorCode::VerificationError,
                            "store value has no virtual-register type");
    const auto Bytes = memoryAccessBytes(Instruction);
    if (!Bytes)
      return makeUnexpected(ErrorCode::Unsupported,
                            "RISC-V store size does not match its value type");
    if (Instruction.memOperands().front()->isAtomic()) {
      if (!isSupportedIntegerType(*Type) || (*Bytes != 4 && *Bytes != 8))
        return makeUnexpected(ErrorCode::Unsupported,
                              "RISC-V atomic store requires a 32-bit or 64-bit GPR value");
      auto Zero = physicalRegister(PhysicalRegisterID::X0);
      if (!Zero)
        return std::unexpected(Zero.error());
      auto Destination = insertPhysicalRegister(Instruction, 0, *Zero, RegisterOperandFlag::Define);
      if (!Destination)
        return std::unexpected(Destination.error());
      return Instruction.setOpcode(
          targetOpcode(*Bytes == 4 ? InstructionOpcode::AMOSWAP_W : InstructionOpcode::AMOSWAP_D));
    }
    auto AppendOffset = appendImmediate(Instruction, 0);
    if (!AppendOffset)
      return std::unexpected(AppendOffset.error());
    if (isSupportedIntegerType(*Type)) {
      const InstructionOpcode Selected = *Bytes == 1   ? InstructionOpcode::SB
                                         : *Bytes == 2 ? InstructionOpcode::SH
                                         : *Bytes == 4 ? InstructionOpcode::SW
                                         : *Bytes == 8 ? InstructionOpcode::SD
                                                       : InstructionOpcode::Invalid;
      if (Selected == InstructionOpcode::Invalid)
        return makeUnexpected(ErrorCode::Unsupported, "RISC-V integer store width is unsupported");
      return Instruction.setOpcode(targetOpcode(Selected));
    }
    if (Type->isFloat() && Type->scalarBitWidth() == 32)
      return Instruction.setOpcode(targetOpcode(InstructionOpcode::FSW));
    if (Type->isFloat() && Type->scalarBitWidth() == 64)
      return Instruction.setOpcode(targetOpcode(InstructionOpcode::FSD));
    return makeUnexpected(ErrorCode::Unsupported, "RISC-V cannot select this generic store type");
  }

  [[nodiscard]] static Expected<void> selectBranch(MachineInstr &Instruction) {
    auto Zero = physicalRegister(PhysicalRegisterID::X0);
    if (!Zero)
      return std::unexpected(Zero.error());
    auto Inserted = insertPhysicalRegister(Instruction, 0, *Zero, RegisterOperandFlag::Define);
    if (!Inserted)
      return std::unexpected(Inserted.error());
    return Instruction.setOpcode(targetOpcode(InstructionOpcode::JAL));
  }

  [[nodiscard]] static Expected<void> selectConditionalBranch(MachineInstr &Instruction) {
    auto Zero = physicalRegister(PhysicalRegisterID::X0);
    if (!Zero)
      return std::unexpected(Zero.error());
    auto Inserted = insertPhysicalRegister(Instruction, 1, *Zero, RegisterOperandFlag::Use);
    if (!Inserted)
      return std::unexpected(Inserted.error());
    return Instruction.setOpcode(targetOpcode(InstructionOpcode::BNE));
  }

  [[nodiscard]] static Expected<void> selectIntegerConstant(MachineInstr &Instruction,
                                                            MachineFunction &Function,
                                                            const TargetDescription &Target,
                                                            std::optional<LLT> Type) {
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
    const std::int64_t Value = *ImmediateValue;
    const std::uint32_t ImmediateTargetFlags = ImmediateOperand->targetFlags();
    const bool IsWord = Type->isInteger() && Type->scalarBitWidth() <= 32;
    const InstructionOpcode AddImmediate =
        IsWord ? InstructionOpcode::ADDIW : InstructionOpcode::ADDI;
    const InstructionOpcode ShiftLeftImmediate =
        IsWord ? InstructionOpcode::SLLIW : InstructionOpcode::SLLI;
    if (fitsSigned(Value, 12)) {
      auto Zero = physicalRegister(PhysicalRegisterID::X0);
      if (!Zero)
        return std::unexpected(Zero.error());
      auto Source = insertPhysicalRegister(Instruction, 1, *Zero, RegisterOperandFlag::Use);
      if (!Source)
        return std::unexpected(Source.error());
      auto EraseImmediate = Instruction.eraseOperand(2);
      if (!EraseImmediate)
        return std::unexpected(EraseImmediate.error());
      auto NormalizeImmediate = Instruction.insertOperand(
          2, MachineOperand::immediate(Value, ImmediateTargetFlags));
      if (!NormalizeImmediate)
        return std::unexpected(NormalizeImmediate.error());
      return Instruction.setOpcode(targetOpcode(AddImmediate));
    }

    // Recursively decompose the value into signed 12-bit chunks.  The
    // quotient is computed from an unsigned bit pattern with explicit sign
    // extension so both INT64_MIN and INT64_MAX remain well-defined.
    std::uint64_t Remaining = static_cast<std::uint64_t>(Value);
    std::vector<std::int64_t> Digits;
    while (!fitsSigned(std::bit_cast<std::int64_t>(Remaining), 12)) {
      const std::uint64_t LowBits = Remaining & 0xFFFU;
      std::int64_t Digit = static_cast<std::int64_t>(LowBits);
      if (Digit >= 0x800)
        Digit -= 0x1000;
      Digits.push_back(Digit);

      const std::uint64_t Adjusted = Remaining - static_cast<std::uint64_t>(Digit);
      std::uint64_t Quotient = Adjusted >> 12U;
      if ((Remaining & (std::uint64_t{1} << 63U)) != 0)
        Quotient |= ~std::uint64_t{0} << 52U;
      Remaining = Quotient;
    }
    const std::int64_t Base = std::bit_cast<std::int64_t>(Remaining);

    auto Temporary = createTemporary(Function, *Type, Target);
    if (!Temporary)
      return std::unexpected(Temporary.error());
    auto *Parent = Instruction.parent();
    if (Parent == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V constant selection received a detached instruction");
    auto SetOpcode = Instruction.setOpcode(targetOpcode(AddImmediate));
    if (!SetOpcode)
      return std::unexpected(SetOpcode.error());
    auto SetDestination = Instruction.operandAt(0)->setRegister(*Temporary);
    if (!SetDestination)
      return std::unexpected(SetDestination.error());
    auto EraseImmediate = Instruction.eraseOperand(1);
    if (!EraseImmediate)
      return std::unexpected(EraseImmediate.error());
    auto Zero = physicalRegister(PhysicalRegisterID::X0);
    if (!Zero)
      return std::unexpected(Zero.error());
    auto AddSource = insertPhysicalRegister(Instruction, 1, *Zero, RegisterOperandFlag::Use);
    if (!AddSource)
      return std::unexpected(AddSource.error());
    auto AddBase = appendImmediate(Instruction, Base);
    if (!AddBase)
      return std::unexpected(AddBase.error());

    MachineIRBuilder Builder(Function, Target);
    auto InsertPoint = Builder.setInsertPointAfter(Instruction);
    if (!InsertPoint)
      return std::unexpected(InsertPoint.error());

    Register Current = *Temporary;
    for (std::size_t Reverse = Digits.size(); Reverse != 0; --Reverse) {
      const std::size_t Index = Reverse - 1U;
      auto ShiftResult = createTemporary(Function, *Type, Target);
      if (!ShiftResult)
        return std::unexpected(ShiftResult.error());
      auto Shift = Builder.buildInstr(targetOpcode(ShiftLeftImmediate));
      if (!Shift)
        return std::unexpected(Shift.error());
      auto ShiftDef = Shift->addDef(*ShiftResult);
      if (!ShiftDef)
        return std::unexpected(ShiftDef.error());
      auto ShiftUse = Shift->addUse(Current);
      if (!ShiftUse)
        return std::unexpected(ShiftUse.error());
      auto ShiftAmount = Shift->addImmediate(12);
      if (!ShiftAmount)
        return std::unexpected(ShiftAmount.error());
      auto BuiltShift = Shift->build();
      if (!BuiltShift)
        return std::unexpected(BuiltShift.error());

      const bool IsFinal = Index == 0;
      Register AddDestination;
      if (IsFinal) {
        AddDestination = Destination;
      } else {
        auto AddTemporary = createTemporary(Function, *Type, Target);
        if (!AddTemporary)
          return std::unexpected(AddTemporary.error());
        AddDestination = *AddTemporary;
      }
      auto Add = Builder.setInsertPointAfter(**BuiltShift);
      if (!Add)
        return std::unexpected(Add.error());
      auto AddInstruction = Builder.buildInstr(targetOpcode(AddImmediate));
      if (!AddInstruction)
        return std::unexpected(AddInstruction.error());
      auto AddDef = AddInstruction->addDef(AddDestination);
      if (!AddDef)
        return std::unexpected(AddDef.error());
      auto AddUse = AddInstruction->addUse(*ShiftResult);
      if (!AddUse)
        return std::unexpected(AddUse.error());
      auto AddImmediateResult = AddInstruction->addImmediate(Digits[Index]);
      if (!AddImmediateResult)
        return std::unexpected(AddImmediateResult.error());
      auto BuiltAdd = AddInstruction->build();
      if (!BuiltAdd)
        return std::unexpected(BuiltAdd.error());
      Current = AddDestination;
    }
    return {};
  }

  [[nodiscard]] static Expected<void> selectFloatingConstant(MachineInstr &Instruction,
                                                             MachineFunction &Function,
                                                             const TargetDescription &Target,
                                                             std::optional<LLT> Type) {
    if (!Type || !isRVFloatType(*Type))
      return makeUnexpected(ErrorCode::Unsupported, "RISC-V floating constant requires f32 or f64");
    const MachineOperand *Immediate = Instruction.operandAt(1);
    const auto *Floating =
        Immediate == nullptr ? nullptr : Immediate->getIf<FloatingImmediateOperand>();
    const std::optional<FloatSemantics> Semantics = Type->floatSemantics();
    if (Floating == nullptr || !Semantics || Floating->Bits.semantics() != *Semantics)
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
    auto Integer = IntegerImmediate::fromUnsigned(64, Bits);
    if (!Integer)
      return std::unexpected(Integer.error());
    auto IntegerType = LLT::makeInteger(64);
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
    const InstructionOpcode Move =
        BitWidth == 32 ? InstructionOpcode::FMV_W_X : InstructionOpcode::FMV_D_X;
    auto MoveInstruction = Builder.buildInstr(targetOpcode(Move));
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
    return {};
  }

  [[nodiscard]] static Expected<void> selectRoundingBinary(MachineInstr &Instruction,
                                                           std::optional<LLT> Type,
                                                           InstructionOpcode Single,
                                                           InstructionOpcode Double) {
    auto RoundingMode = appendImmediate(Instruction, 0);
    if (!RoundingMode)
      return std::unexpected(RoundingMode.error());
    if (Type && Type->isFloat() && Type->scalarBitWidth() == 32)
      return Instruction.setOpcode(targetOpcode(Single));
    if (Type && Type->isFloat() && Type->scalarBitWidth() == 64)
      return Instruction.setOpcode(targetOpcode(Double));
    return makeUnexpected(ErrorCode::Unsupported, "RISC-V cannot select this floating binary type");
  }

  [[nodiscard]] static Expected<void> selectRoundingTernary(MachineInstr &Instruction,
                                                            std::optional<LLT> Type,
                                                            InstructionOpcode Single,
                                                            InstructionOpcode Double) {
    return selectRoundingBinary(Instruction, Type, Single, Double);
  }

  [[nodiscard]] static Expected<void> selectRoundingUnary(MachineInstr &Instruction,
                                                          std::optional<LLT> Type,
                                                          InstructionOpcode Single,
                                                          InstructionOpcode Double) {
    return selectRoundingBinary(Instruction, Type, Single, Double);
  }

  [[nodiscard]] static Expected<void> selectSignInjection(MachineInstr &Instruction,
                                                          std::optional<LLT> Type,
                                                          InstructionOpcode Single,
                                                          InstructionOpcode Double) {
    const MachineOperand *Source = Instruction.operandAt(1);
    const Register *SourceReg = Source == nullptr ? nullptr : Source->getIf<Register>();
    if (SourceReg == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V floating sign operation requires a register source");
    auto Duplicate = insertVirtualUse(Instruction, 2, *SourceReg);
    if (!Duplicate)
      return std::unexpected(Duplicate.error());
    if (Type && Type->isFloat() && Type->scalarBitWidth() == 32)
      return Instruction.setOpcode(targetOpcode(Single));
    if (Type && Type->isFloat() && Type->scalarBitWidth() == 64)
      return Instruction.setOpcode(targetOpcode(Double));
    return makeUnexpected(ErrorCode::Unsupported, "RISC-V cannot select this floating unary type");
  }
};

class RISCVSSAInfo final : public TargetSSAInfo {
public:
  [[nodiscard]] Expected<bool> isFallthroughEdge(const MachineBasicBlock &From,
                                                 const MachineBasicBlock &To,
                                                 const MachineFunction &,
                                                 const TargetDescription &Target) const override {
    if (From.next() != &To ||
        std::ranges::none_of(From.successors(), [&To](const MachineSuccessor &Successor) {
          return Successor.Block == &To;
        })) {
      return false;
    }

    bool CanFallThrough = true;
    for (const MachineInstr *Instruction = From.firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction->opcode());
      if (Descriptor == nullptr) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "RISC-V fallthrough analysis encountered an unknown opcode");
      }
      if (!Descriptor->Flags.has(InstructionFlag::Terminator))
        continue;
      CanFallThrough = isConditionalBranch(Instruction->opcode());
    }
    return CanFallThrough;
  }

  [[nodiscard]] Expected<bool> rewriteBranchTarget(MachineBasicBlock &Block,
                                                   MachineBasicBlock &OldTarget,
                                                   MachineBasicBlock &NewTarget,
                                                   MachineFunction &Function,
                                                   const TargetDescription &Target) const override {
    if (&Block.function() != &Function || &OldTarget.function() != &Function ||
        &NewTarget.function() != &Function) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "RISC-V branch rewrite references another machine function");
    }

    bool Changed = false;
    for (MachineInstr *Instruction = Block.firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction->opcode());
      if (Descriptor == nullptr) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "RISC-V branch rewrite encountered an unknown opcode");
      }
      if (!Descriptor->Flags.has(InstructionFlag::Branch) ||
          Descriptor->Flags.has(InstructionFlag::IndirectBranch)) {
        continue;
      }
      for (std::size_t Index = 0; Index < Instruction->operandCount(); ++Index) {
        MachineOperand *Operand = Instruction->operandAt(Index);
        const BasicBlockOperand *TargetOperand =
            Operand == nullptr ? nullptr : Operand->getIf<BasicBlockOperand>();
        if (TargetOperand == nullptr || TargetOperand->Block != &OldTarget)
          continue;
        auto Rewritten = Operand->setBasicBlock(NewTarget);
        if (!Rewritten)
          return std::unexpected(Rewritten.error());
        Changed = true;
      }
    }
    return Changed;
  }

  [[nodiscard]] Expected<void>
  insertUnconditionalBranch(MachineBasicBlock &Block, MachineBasicBlock &TargetBlock,
                            MachineFunction &Function,
                            const TargetDescription &Target) const override {
    if (&Block.function() != &Function || &TargetBlock.function() != &Function) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "RISC-V branch insertion references another machine function");
    }
    auto Zero = physicalRegister(PhysicalRegisterID::X0);
    if (!Zero)
      return std::unexpected(Zero.error());
    MachineIRBuilder Builder(Function, Target);
    auto Point = Builder.setInsertPoint(Block);
    if (!Point)
      return std::unexpected(Point.error());
    auto Branch = Builder.buildInstr(targetOpcode(InstructionOpcode::JAL));
    if (!Branch)
      return std::unexpected(Branch.error());
    auto Link = Branch->addDef(Register::fromPhysical(*Zero));
    if (!Link)
      return std::unexpected(Link.error());
    auto Destination = Branch->addBasicBlock(TargetBlock);
    if (!Destination)
      return std::unexpected(Destination.error());
    auto Built = Branch->build();
    if (!Built)
      return std::unexpected(Built.error());
    return {};
  }

  [[nodiscard]] Expected<void> validateCopy(Register Destination, SubRegIndex DestinationSubReg,
                                            Register Source, SubRegIndex SourceSubReg,
                                            const MachineFunction &Function,
                                            const TargetDescription &Target) const override {
    if (Destination.isNoRegister() || Source.isNoRegister()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V COPY requires two nonzero registers");
    }
    if (DestinationSubReg.value() != 0 || SourceSubReg.value() != 0) {
      return makeUnexpected(ErrorCode::Unsupported, "RV64GC does not model register subindices");
    }

    auto DestinationShape = copyShape(Destination, Function, Target);
    if (!DestinationShape)
      return std::unexpected(DestinationShape.error());
    auto SourceShape = copyShape(Source, Function, Target);
    if (!SourceShape)
      return std::unexpected(SourceShape.error());
    if (DestinationShape->Bank == SourceShape->Bank) {
      if (DestinationShape->Width && SourceShape->Width &&
          DestinationShape->Width != SourceShape->Width) {
        return makeUnexpected(ErrorCode::Unsupported,
                              "RISC-V same-bank COPY has incompatible register widths");
      }
      return {};
    }

    const RegisterBankID GPR =
        RegisterBankID::fromValue(static_cast<std::uint32_t>(RegisterBank::GPR));
    const RegisterBankID FPR =
        RegisterBankID::fromValue(static_cast<std::uint32_t>(RegisterBank::FPR));
    const bool IsCrossBankMove = (DestinationShape->Bank == GPR && SourceShape->Bank == FPR) ||
                                 (DestinationShape->Bank == FPR && SourceShape->Bank == GPR);
    if (!IsCrossBankMove) {
      return makeUnexpected(ErrorCode::Unsupported,
                            "RISC-V COPY crosses unsupported register banks");
    }
    auto SupportedWidth = [](std::optional<std::uint32_t> Width) {
      return !Width || *Width == 32 || *Width == 64;
    };
    if (!SupportedWidth(DestinationShape->Width) || !SupportedWidth(SourceShape->Width)) {
      return makeUnexpected(ErrorCode::Unsupported,
                            "RISC-V GPR/FPR COPY requires 32-bit or 64-bit values");
    }
    return {};
  }

private:
  struct CopyShape final {
    RegisterBankID Bank;
    std::optional<std::uint32_t> Width;
  };

  [[nodiscard]] static bool isConditionalBranch(Opcode Value) noexcept {
    return Value == targetOpcode(InstructionOpcode::BEQ) ||
           Value == targetOpcode(InstructionOpcode::BNE) ||
           Value == targetOpcode(InstructionOpcode::BLT) ||
           Value == targetOpcode(InstructionOpcode::BGE) ||
           Value == targetOpcode(InstructionOpcode::BLTU) ||
           Value == targetOpcode(InstructionOpcode::BGEU);
  }

  [[nodiscard]] static Expected<CopyShape> copyShape(Register Reg, const MachineFunction &Function,
                                                     const TargetDescription &Target) {
    if (Reg.isVirtual()) {
      auto Info = Function.registerInfo().virtualRegisterInfo(Reg);
      if (!Info)
        return std::unexpected(Info.error());
      const std::optional<RegisterBankID> &Bank = Info->get().Bank;
      const std::optional<RegisterClassID> &RegClass = Info->get().RegClass;
      if (!Bank || !RegClass) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "RISC-V COPY virtual register is not fully constrained");
      }
      const RegisterClassDesc *Class = Target.registers().registerClass(RegClass.value());
      if (Class == nullptr) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "RISC-V COPY references an unknown register class");
      }
      return CopyShape{Bank.value(), Class->SizeInBits};
    }

    const std::optional<PhysicalRegister> Physical = Reg.physicalRegister();
    if (!Physical || !Target.registers().isKnownPhysicalRegister(*Physical)) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V COPY references an unknown physical register");
    }
    for (const RegisterBankDesc &Bank : Target.registers().registerBanks()) {
      if (Target.registers().bankContainsRegister(Bank.ID, *Physical))
        return CopyShape{Bank.ID, std::nullopt};
    }
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V physical register is not assigned to a register bank");
  }
};

} // namespace

const TargetLegalizerInfo &legalizerInfo() noexcept {
  static const RISCVLegalizerInfo Value;
  return Value;
}

const TargetRegisterBankInfo &registerBankInfo() noexcept {
  static const RISCVRegisterBankInfo Value;
  return Value;
}

const TargetInstructionSelector &instructionSelector() noexcept {
  static const RISCVInstructionSelector Value;
  return Value;
}

const TargetSSAInfo &ssaInfo() noexcept {
  static const RISCVSSAInfo Value;
  return Value;
}

} // namespace rcc::riscv
