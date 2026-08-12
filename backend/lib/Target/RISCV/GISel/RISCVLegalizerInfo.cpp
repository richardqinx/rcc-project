// SPDX-License-Identifier: GPL-3.0-or-later

#include "RISCVLegalizerInfo.h"

#include "RISCVCallLowering.h"
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
#include "rcc/Target/RISCV/RISCVSubtarget.h"
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

[[nodiscard]] Expected<IntegerImmediate> extendIntegerImmediate(const IntegerImmediate &Value,
                                                                std::uint32_t DestinationWidth,
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

[[nodiscard]] Expected<void>
foldConstantExtension(MachineInstr &Instruction, MachineFunction &Function, GenericOpcode Generic) {
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
  const Register SourceRegister = *Source;
  const Register DestinationRegister = *Destination;
  const MachineOperand *SourceDefinition = Function.registerInfo().getUniqueVRegDef(SourceRegister);
  const MachineInstr *SourceDef =
      SourceDefinition == nullptr ? nullptr : SourceDefinition->parent();
  if (SourceDef == nullptr || SourceDef->opcode() != opcode(GenericOpcode::G_CONSTANT))
    return makeUnexpected(ErrorCode::Unsupported,
                          "RISC-V constant extension requires a single constant definition");
  const MachineOperand *ConstantOperand = SourceDef->operandAt(1);
  const auto *Integer =
      ConstantOperand == nullptr ? nullptr : ConstantOperand->getIf<IntegerImmediateOperand>();
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

  if (Function.registerInfo().hasOneUse(SourceRegister)) {
    MachineInstr *Def = const_cast<MachineInstr *>(SourceDef);
    if (auto Replaced = Def->operandAt(0)->setRegister(DestinationRegister); !Replaced)
      return std::unexpected(Replaced.error());
    auto Erased = Def->eraseOperand(1);
    if (!Erased)
      return std::unexpected(Erased.error());
    auto Added = Def->addOperand(MachineOperand::integerImmediate(std::move(*Extended)));
    if (!Added)
      return std::unexpected(Added.error());
    MachineBasicBlock *Parent = Instruction.parent();
    if (Parent == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V constant extension is detached during folding");
    return Parent->removeInstruction(Instruction);
  }

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

  if (Function.registerInfo().useEmpty(SourceRegister)) {
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
  if (Unmerge->opcode() != opcode(GenericOpcode::G_UNMERGE_VALUES) || Unmerge->operandCount() < 3)
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

[[nodiscard]] Expected<IntegerImmediate> wideConstantWord(const IntegerImmediate &Value,
                                                          std::size_t WordIndex,
                                                          std::uint32_t Width, bool SignExtend) {
  std::uint64_t Raw = 0;
  if (WordIndex < Value.words().size())
    Raw = Value.words()[WordIndex];
  else if (SignExtend && Value.isNegative())
    Raw = std::numeric_limits<std::uint64_t>::max();
  const std::size_t WordBitOffset = WordIndex * 64U;
  if (SignExtend && Value.isNegative() && WordBitOffset < Value.bitWidth()) {
    const std::uint32_t Remaining = Value.bitWidth() - static_cast<std::uint32_t>(WordBitOffset);
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
  const auto *Integer =
      ImmediateOperand == nullptr ? nullptr : ImmediateOperand->getIf<IntegerImmediateOperand>();
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
    auto Replaced = Function.registerInfo().replaceRegWith(Plan->Outputs[Index],
                                                           Plan->Outputs[Representatives[Index]]);
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

[[nodiscard]] Expected<Register> buildIntegerConstantBefore(MachineIRBuilder &Builder,
                                                            MachineInstr &Anchor,
                                                            std::uint32_t Width,
                                                            std::uint64_t Value);

[[nodiscard]] Expected<Register> buildBinaryBefore(MachineIRBuilder &Builder, MachineInstr &Anchor,
                                                   GenericOpcode Generic, LLT Type, Register Left,
                                                   Register Right);

[[nodiscard]] Expected<Register> buildUnaryBefore(MachineIRBuilder &Builder, MachineInstr &Anchor,
                                                  GenericOpcode Generic, LLT Type, Register Source);

[[nodiscard]] Expected<void> eraseInstruction(MachineInstr &Instruction);

[[nodiscard]] Expected<void> rewriteUnary(MachineInstr &Instruction, GenericOpcode Generic,
                                          Register Destination, Register Source);

struct NarrowAnyExtBinaryPlan final {
  MachineInstr *Binary = nullptr;
  GenericOpcode Generic = GenericOpcode::Invalid;
  std::array<Register, 2> WideInputs{};
  std::vector<MachineInstr *> Artifacts;
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
  MachineInstr *Binary = SourceDefinition == nullptr
                             ? nullptr
                             : const_cast<MachineInstr *>(SourceDefinition->parent());
  if (Binary == nullptr || Binary->operandCount() != 3)
    return std::nullopt;
  const GenericOpcode Generic = static_cast<GenericOpcode>(Binary->opcode().value());
  if (Generic != GenericOpcode::G_ADD && Generic != GenericOpcode::G_SUB &&
      Generic != GenericOpcode::G_MUL && Generic != GenericOpcode::G_AND &&
      Generic != GenericOpcode::G_OR && Generic != GenericOpcode::G_XOR &&
      Generic != GenericOpcode::G_SHL)
    return std::nullopt;

  NarrowAnyExtBinaryPlan Plan;
  Plan.Binary = Binary;
  Plan.Generic = Generic;
  Plan.SourceWidth = SourceType.scalarBitWidth();
  const auto AddArtifact = [&Plan](MachineInstr *Artifact) {
    if (std::ranges::find(Plan.Artifacts, Artifact) == Plan.Artifacts.end())
      Plan.Artifacts.push_back(Artifact);
  };
  const auto TruncatedWideSource = [&](MachineInstr *Truncation,
                                       MachineInstr *OnlyUse) -> std::optional<Register> {
    if (Truncation == nullptr || Truncation->opcode() != opcode(GenericOpcode::G_TRUNC) ||
        Truncation->operandCount() != 2)
      return std::nullopt;
    const MachineOperand *NarrowOperand = Truncation->operandAt(0);
    const Register *Narrow = NarrowOperand == nullptr ? nullptr : NarrowOperand->getIf<Register>();
    const MachineOperand *WideOperand = Truncation->operandAt(1);
    const Register *Wide = WideOperand == nullptr ? nullptr : WideOperand->getIf<Register>();
    const std::optional<LLT> WideType =
        WideOperand == nullptr ? std::nullopt : registerType(Function, *WideOperand);
    if (Narrow == nullptr || Wide == nullptr || !Wide->isVirtual() || !WideType ||
        *WideType != DestinationType || singleRegisterUse(Function, *Narrow) != OnlyUse)
      return std::nullopt;
    return *Wide;
  };
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
    MachineInstr *Def =
        Definition == nullptr ? nullptr : const_cast<MachineInstr *>(Definition->parent());
    if (auto Wide = TruncatedWideSource(Def, Binary)) {
      Plan.WideInputs[Index] = *Wide;
      AddArtifact(Def);
      continue;
    }
    if (Def != nullptr &&
        (Def->opcode() == opcode(GenericOpcode::COPY) ||
         Def->opcode() == opcode(GenericOpcode::G_ANYEXT)) &&
        Def->operandCount() == 2 && singleRegisterUse(Function, *Reg) == Binary) {
      const MachineOperand *NarrowOperand = Def->operandAt(1);
      const Register *Narrow =
          NarrowOperand == nullptr ? nullptr : NarrowOperand->getIf<Register>();
      const MachineOperand *NarrowDefinition =
          Narrow == nullptr ? nullptr : Function.registerInfo().getUniqueVRegDef(*Narrow);
      MachineInstr *Truncation = NarrowDefinition == nullptr
                                     ? nullptr
                                     : const_cast<MachineInstr *>(NarrowDefinition->parent());
      if (auto Wide = TruncatedWideSource(Truncation, Def)) {
        Plan.WideInputs[Index] = *Wide;
        AddArtifact(Def);
        AddArtifact(Truncation);
        continue;
      }
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
  const auto *Immediate =
      ImmediateOperand == nullptr ? nullptr : ImmediateOperand->getIf<IntegerImmediateOperand>();
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
  const Register OriginalDestination = *Destination;

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
  MachineInstr *FoldedExtension = singleRegisterUse(Function, OriginalDestination);
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
  if (auto Added = Truncation->addDef(OriginalDestination); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Truncation->addUse(WideDestination); !Added)
    return std::unexpected(Added.error());
  auto Built = Truncation->build();
  if (!Built)
    return std::unexpected(Built.error());
  return {};
}

[[nodiscard]] Expected<void>
lowerAtomicCompareExchangeWithSuccess(MachineInstr &Instruction, MachineFunction &Function,
                                      const TargetDescription &Target) {
  const MachineOperand *OldValueOperand = Instruction.operandAt(0);
  const MachineOperand *SuccessOperand = Instruction.operandAt(1);
  const MachineOperand *AddressOperand = Instruction.operandAt(2);
  const MachineOperand *CompareOperand = Instruction.operandAt(3);
  const MachineOperand *NewValueOperand = Instruction.operandAt(4);
  const Register *OldValue =
      OldValueOperand == nullptr ? nullptr : OldValueOperand->getIf<Register>();
  const Register *Success = SuccessOperand == nullptr ? nullptr : SuccessOperand->getIf<Register>();
  const Register *Address = AddressOperand == nullptr ? nullptr : AddressOperand->getIf<Register>();
  const Register *Compare = CompareOperand == nullptr ? nullptr : CompareOperand->getIf<Register>();
  const Register *NewValue =
      NewValueOperand == nullptr ? nullptr : NewValueOperand->getIf<Register>();
  const std::optional<LLT> ValueType = resultType(Function, Instruction);
  if (OldValue == nullptr || Success == nullptr || Address == nullptr || Compare == nullptr ||
      NewValue == nullptr || !ValueType || Instruction.memOperands().size() != 1)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V compare-exchange-with-success has invalid operands");

  auto AtomicResult = Function.registerInfo().createVirtualRegister(*ValueType);
  if (!AtomicResult)
    return std::unexpected(AtomicResult.error());
  MachineIRBuilder Builder(Function, Target);
  auto Point = Builder.setInsertPointBefore(Instruction);
  if (!Point)
    return std::unexpected(Point.error());
  auto Atomic = Builder.buildInstr(opcode(GenericOpcode::G_ATOMIC_CMPXCHG));
  if (!Atomic)
    return std::unexpected(Atomic.error());
  if (auto Added = Atomic->addDef(*AtomicResult); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Atomic->addUse(*Address); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Atomic->addUse(*Compare); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Atomic->addUse(*NewValue); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Atomic->addMemOperand(*Instruction.memOperands().front()); !Added)
    return std::unexpected(Added.error());
  auto BuiltAtomic = Atomic->build();
  if (!BuiltAtomic)
    return std::unexpected(BuiltAtomic.error());

  Point = Builder.setInsertPointBefore(Instruction);
  if (!Point)
    return std::unexpected(Point.error());
  auto CompareResult = Builder.buildInstr(opcode(GenericOpcode::G_ICMP));
  if (!CompareResult)
    return std::unexpected(CompareResult.error());
  if (auto Added = CompareResult->addDef(*Success); !Added)
    return std::unexpected(Added.error());
  if (auto Added = CompareResult->addIntegerPredicate(IntegerPredicate::Equal); !Added)
    return std::unexpected(Added.error());
  if (auto Added = CompareResult->addUse(*AtomicResult); !Added)
    return std::unexpected(Added.error());
  if (auto Added = CompareResult->addUse(*Compare); !Added)
    return std::unexpected(Added.error());
  auto BuiltCompare = CompareResult->build();
  if (!BuiltCompare)
    return std::unexpected(BuiltCompare.error());

  while (!Instruction.memOperands().empty()) {
    auto Erased = Instruction.eraseMemOperand(0);
    if (!Erased)
      return std::unexpected(Erased.error());
  }
  return rewriteUnary(Instruction, GenericOpcode::COPY, *OldValue, *AtomicResult);
}

[[nodiscard]] Expected<Register> widenIntegerCompareSource(MachineInstr &Instruction,
                                                           MachineFunction &Function,
                                                           MachineIRBuilder &Builder,
                                                           Register Source, LLT WideType) {
  const auto SourceInfo = Function.registerInfo().virtualRegisterInfo(Source);
  if (!SourceInfo || !SourceInfo->get().Type)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V integer compare source has no scalar type");
  const LLT SourceType = *SourceInfo->get().Type;
  if (SourceType.scalarBitWidth() == WideType.scalarBitWidth())
    return Source;

  const MachineOperand *Definition = Function.registerInfo().getUniqueVRegDef(Source);
  MachineInstr *Def =
      Definition == nullptr ? nullptr : const_cast<MachineInstr *>(Definition->parent());
  std::optional<Register> ExistingExtension;
  bool AmbiguousExtension = false;
  auto &RegisterInfo = Function.registerInfo();
  for (MachineOperand &Use : RegisterInfo.useOperands(Source)) {
    MachineInstr *User = Use.parent();
    if (User == nullptr || User == &Instruction || User->parent() == nullptr ||
        (User->opcode() != opcode(GenericOpcode::G_ANYEXT) &&
         User->opcode() != opcode(GenericOpcode::G_SEXT))) {
      continue;
    }
    const MachineOperand *ExtendedOperand = User->operandAt(0);
    const Register *Extended =
        ExtendedOperand == nullptr ? nullptr : ExtendedOperand->getIf<Register>();
    const std::optional<LLT> ExtendedType =
        ExtendedOperand == nullptr ? std::nullopt : registerType(Function, *ExtendedOperand);
    if (Extended == nullptr || !ExtendedType || *ExtendedType != WideType ||
        (Def == nullptr || (Def->opcode() != opcode(GenericOpcode::G_CONSTANT) &&
                            User->opcode() != opcode(GenericOpcode::G_SEXT)))) {
      continue;
    }
    if (ExistingExtension) {
      AmbiguousExtension = true;
      break;
    }
    ExistingExtension = *Extended;
  }
  if (ExistingExtension && !AmbiguousExtension)
    return *ExistingExtension;
  if (Def != nullptr && Def->opcode() == opcode(GenericOpcode::G_CONSTANT) &&
      singleRegisterUse(Function, Source) == &Instruction) {
    auto Widened = widenConstantToRV64(*Def, Function);
    if (!Widened)
      return std::unexpected(Widened.error());
    return Source;
  }

  if (Def != nullptr && Def->opcode() == opcode(GenericOpcode::G_TRUNC) &&
      Def->operandCount() == 2) {
    const MachineOperand *WideOperand = Def->operandAt(1);
    const Register *WideSource = WideOperand == nullptr ? nullptr : WideOperand->getIf<Register>();
    const std::optional<LLT> ExistingWideType =
        WideOperand == nullptr ? std::nullopt : registerType(Function, *WideOperand);
    if (WideSource != nullptr && ExistingWideType && *ExistingWideType == WideType) {
      auto Result = Function.registerInfo().createVirtualRegister(WideType);
      if (!Result)
        return std::unexpected(Result.error());
      auto Point = Builder.setInsertPointBefore(Instruction);
      if (!Point)
        return std::unexpected(Point.error());
      auto Pending = Builder.buildInstr(opcode(GenericOpcode::G_SEXT_INREG));
      if (!Pending)
        return std::unexpected(Pending.error());
      if (auto Added = Pending->addDef(*Result); !Added)
        return std::unexpected(Added.error());
      if (auto Added = Pending->addUse(*WideSource); !Added)
        return std::unexpected(Added.error());
      if (auto Added =
              Pending->addImmediate(static_cast<std::int64_t>(SourceType.scalarBitWidth()));
          !Added) {
        return std::unexpected(Added.error());
      }
      auto Built = Pending->build();
      if (!Built)
        return std::unexpected(Built.error());
      return *Result;
    }
  }
  return buildUnaryBefore(Builder, Instruction, GenericOpcode::G_SEXT, WideType, Source);
}

[[nodiscard]] Expected<void> lowerIntegerCompare(MachineInstr &Instruction,
                                                 MachineFunction &Function,
                                                 const TargetDescription &Target) {
  const MachineOperand *DestinationOperand = Instruction.operandAt(0);
  const MachineOperand *LeftOperand = Instruction.operandAt(2);
  const MachineOperand *RightOperand = Instruction.operandAt(3);
  const Register *Destination =
      DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
  const Register *Left = LeftOperand == nullptr ? nullptr : LeftOperand->getIf<Register>();
  const Register *Right = RightOperand == nullptr ? nullptr : RightOperand->getIf<Register>();
  if (Destination == nullptr || Left == nullptr || Right == nullptr)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V integer compare has invalid register operands");
  const Register OriginalDestination = *Destination;
  const Register OriginalLeft = *Left;
  const Register OriginalRight = *Right;

  auto WideType = LLT::makeAnyScalar(64);
  if (!WideType)
    return std::unexpected(WideType.error());
  MachineIRBuilder Builder(Function, Target);
  auto WideLeft =
      widenIntegerCompareSource(Instruction, Function, Builder, OriginalLeft, *WideType);
  if (!WideLeft)
    return std::unexpected(WideLeft.error());
  auto WideRight =
      widenIntegerCompareSource(Instruction, Function, Builder, OriginalRight, *WideType);
  if (!WideRight)
    return std::unexpected(WideRight.error());
  if (auto Replaced = Instruction.operandAt(2)->setRegister(*WideLeft); !Replaced)
    return std::unexpected(Replaced.error());
  if (auto Replaced = Instruction.operandAt(3)->setRegister(*WideRight); !Replaced)
    return std::unexpected(Replaced.error());

  const auto DestinationInfo = Function.registerInfo().virtualRegisterInfo(OriginalDestination);
  if (!DestinationInfo || !DestinationInfo->get().Type)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V integer compare destination has no scalar type");
  if (DestinationInfo->get().Type->scalarBitWidth() == 64)
    return {};

  MachineInstr *FoldedExtension = singleRegisterUse(Function, OriginalDestination);
  if (FoldedExtension != nullptr &&
      (FoldedExtension->opcode() == opcode(GenericOpcode::G_ANYEXT) ||
       FoldedExtension->opcode() == opcode(GenericOpcode::G_SEXT) ||
       FoldedExtension->opcode() == opcode(GenericOpcode::G_ZEXT)) &&
      operandType(Function, *FoldedExtension, 0)
          .transform([](LLT Type) { return Type.scalarBitWidth() == 64; })
          .value_or(false)) {
    const MachineOperand *ExtendedOperand = FoldedExtension->operandAt(0);
    const Register *Extended =
        ExtendedOperand == nullptr ? nullptr : ExtendedOperand->getIf<Register>();
    if (Extended == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V integer compare extension has no destination");
    const Register WideDestination = *Extended;
    if (auto Replaced = Instruction.operandAt(0)->setRegister(WideDestination); !Replaced)
      return std::unexpected(Replaced.error());
    return eraseInstruction(*FoldedExtension);
  }

  auto WideDestination = Function.registerInfo().createVirtualRegister(*WideType);
  if (!WideDestination)
    return std::unexpected(WideDestination.error());
  if (auto Replaced = Instruction.operandAt(0)->setRegister(*WideDestination); !Replaced)
    return std::unexpected(Replaced.error());
  if (Function.registerInfo().useEmpty(OriginalDestination))
    return {};
  auto Point = Builder.setInsertPointAfter(Instruction);
  if (!Point)
    return std::unexpected(Point.error());
  auto Truncation = Builder.buildInstr(opcode(GenericOpcode::G_TRUNC));
  if (!Truncation)
    return std::unexpected(Truncation.error());
  if (auto Added = Truncation->addDef(OriginalDestination); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Truncation->addUse(*WideDestination); !Added)
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
  for (MachineInstr *Artifact : Plan->Artifacts) {
    if (Artifact->parent() == nullptr)
      continue;
    auto Removed = Artifact->parent()->removeInstruction(*Artifact);
    if (!Removed)
      return std::unexpected(Removed.error());
  }

  if (Plan->Generic == GenericOpcode::G_SHL) {
    Register Amount = Plan->WideInputs[1];
    if (Plan->SourceWidth < 32 || Plan->SourceWidth > 32) {
      const std::uint64_t Mask = Plan->SourceWidth == 64
                                     ? std::numeric_limits<std::uint64_t>::max()
                                     : (std::uint64_t{1} << Plan->SourceWidth) - 1U;
      auto WideType = LLT::makeAnyScalar(64);
      if (!WideType)
        return std::unexpected(WideType.error());
      MachineIRBuilder Builder(Function, Target);
      auto MaskValue = buildIntegerConstantBefore(Builder, Extension, 64, Mask);
      if (!MaskValue)
        return std::unexpected(MaskValue.error());
      auto MaskedAmount = buildBinaryBefore(Builder, Extension, GenericOpcode::G_AND, *WideType,
                                            Amount, *MaskValue);
      if (!MaskedAmount)
        return std::unexpected(MaskedAmount.error());
      Amount = *MaskedAmount;
    }
    auto SetOpcode =
        Extension.setOpcode(Plan->SourceWidth == 32 ? targetOpcode(InstructionOpcode::G_SLLW)
                                                    : opcode(GenericOpcode::G_SHL));
    if (!SetOpcode)
      return std::unexpected(SetOpcode.error());
    while (Extension.operandCount() > 1) {
      auto Erased = Extension.eraseOperand(1);
      if (!Erased)
        return std::unexpected(Erased.error());
    }
    for (Register Input : {Plan->WideInputs[0], Amount}) {
      auto Operand =
          MachineOperand::registerOperand(Input, RegisterOperandFlags{RegisterOperandFlag::Use});
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
    auto Operand =
        MachineOperand::registerOperand(Input, RegisterOperandFlags{RegisterOperandFlag::Use});
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

[[nodiscard]] Expected<void> lowerConstantToPool(MachineInstr &Instruction,
                                                 MachineFunction &Function,
                                                 const TargetDescription &Target) {
  const MachineOperand *DestinationOperand = Instruction.operandAt(0);
  const Register *Destination =
      DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
  const MachineOperand *ImmediateOperand = Instruction.operandAt(1);
  const auto *Immediate =
      ImmediateOperand == nullptr ? nullptr : ImmediateOperand->getIf<IntegerImmediateOperand>();
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
  auto AddressUse =
      MachineOperand::registerOperand(*Address, RegisterOperandFlags{RegisterOperandFlag::Use});
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

[[nodiscard]] bool allDefsUnused(const MachineInstr &Instruction, const MachineFunction &Function) {
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
      if (Value != opcode(GenericOpcode::G_TRUNC) && Value != opcode(GenericOpcode::G_ANYEXT) &&
          Value != opcode(GenericOpcode::G_SEXT) && Value != opcode(GenericOpcode::G_ZEXT) &&
          Value != opcode(GenericOpcode::G_MERGE_VALUES) &&
          Value != opcode(GenericOpcode::G_UNMERGE_VALUES) && Value != opcode(GenericOpcode::COPY))
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
  auto Def = MachineOperand::registerOperand(Destination,
                                             RegisterOperandFlags{RegisterOperandFlag::Define});
  if (!Def)
    return std::unexpected(Def.error());
  if (auto Added = Instruction.addOperand(std::move(*Def)); !Added)
    return std::unexpected(Added.error());
  auto Use =
      MachineOperand::registerOperand(Source, RegisterOperandFlags{RegisterOperandFlag::Use});
  if (!Use)
    return std::unexpected(Use.error());
  return Instruction.addOperand(std::move(*Use));
}

[[nodiscard]] Expected<void> widenNarrowMultiply(MachineInstr &Instruction,
                                                 MachineFunction &Function,
                                                 const TargetDescription &Target) {
  const std::optional<LLT> Type = resultType(Function, Instruction);
  const MachineOperand *DestinationOperand = Instruction.operandAt(0);
  const MachineOperand *LeftOperand = Instruction.operandAt(1);
  const MachineOperand *RightOperand = Instruction.operandAt(2);
  const Register *Destination =
      DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
  const Register *Left = LeftOperand == nullptr ? nullptr : LeftOperand->getIf<Register>();
  const Register *Right = RightOperand == nullptr ? nullptr : RightOperand->getIf<Register>();
  if (!Type || Destination == nullptr || Left == nullptr || Right == nullptr ||
      Type->scalarBitWidth() >= 64) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V narrow multiply has invalid operands");
  }

  MachineInstr *Extension = singleRegisterUse(Function, *Destination);
  if (Extension != nullptr && Extension->opcode() == opcode(GenericOpcode::G_ANYEXT)) {
    const auto Plan = narrowAnyExtBinaryPlan(*Extension, Function);
    if (Plan && Plan->Binary == &Instruction)
      return foldNarrowAnyExtBinary(*Extension, Function, Target);
  }

  auto WideType = LLT::makeAnyScalar(64);
  if (!WideType)
    return std::unexpected(WideType.error());
  MachineIRBuilder Builder(Function, Target);
  auto WideLeft = buildUnaryBefore(Builder, Instruction, GenericOpcode::G_ANYEXT, *WideType, *Left);
  if (!WideLeft)
    return std::unexpected(WideLeft.error());
  auto WideRight =
      buildUnaryBefore(Builder, Instruction, GenericOpcode::G_ANYEXT, *WideType, *Right);
  if (!WideRight)
    return std::unexpected(WideRight.error());
  auto Product = buildBinaryBefore(Builder, Instruction, GenericOpcode::G_MUL, *WideType, *WideLeft,
                                   *WideRight);
  if (!Product)
    return std::unexpected(Product.error());
  return rewriteUnary(Instruction, GenericOpcode::G_TRUNC, *Destination, *Product);
}

[[nodiscard]] Expected<void> lowerMultiplyLibcall(MachineInstr &Instruction,
                                                  MachineFunction &Function,
                                                  const TargetDescription &Target,
                                                  CallingConvention Convention,
                                                  const TargetCallLowering &CallLowering) {
  const std::optional<LLT> Type = resultType(Function, Instruction);
  const MachineOperand *DestinationOperand = Instruction.operandAt(0);
  const MachineOperand *LeftOperand = Instruction.operandAt(1);
  const MachineOperand *RightOperand = Instruction.operandAt(2);
  const Register *Destination =
      DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
  const Register *Left = LeftOperand == nullptr ? nullptr : LeftOperand->getIf<Register>();
  const Register *Right = RightOperand == nullptr ? nullptr : RightOperand->getIf<Register>();
  if (!Type || Destination == nullptr || Left == nullptr || Right == nullptr ||
      Type->scalarBitWidth() != 64) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V multiply libcall requires three s64 registers");
  }
  auto ABIAlignment = Alignment::create(8);
  auto Callee = MachineCallTarget::externalSymbol("__muldi3");
  if (!ABIAlignment || !Callee)
    return makeUnexpected(ErrorCode::InternalError,
                          "RISC-V multiply libcall ABI facts are unavailable");
  const auto Value = [Type = *Type, Alignment = *ABIAlignment](Register Reg, std::uint32_t Number) {
    return ABIValue{
        .Value = Reg,
        .Type = Type,
        .OriginalValue = Number,
        .FragmentIndex = 0,
        .FragmentCount = 1,
        .FragmentOffsetBytes = 0,
        .OriginalAlignment = Alignment,
        .Extension = ABIExtensionKind::None,
        .Flags = {},
        .ByValSizeBytes = 0,
        .ByValAlignment = Alignment,
    };
  };

  CallLoweringRequest Request;
  Request.Convention = callingConventionID(Convention);
  Request.Callee = std::move(*Callee);
  Request.Arguments = {Value(*Left, 0), Value(*Right, 1)};
  Request.Results = {Value(*Destination, 0)};
  Request.FixedArgumentCount = Request.Arguments.size();

  MachineIRBuilder Builder(Function, Target);
  auto Point = Builder.setInsertPointBefore(Instruction);
  if (!Point)
    return std::unexpected(Point.error());
  auto Lowered = CallLowering.lowerCall(Builder, Request);
  if (!Lowered)
    return std::unexpected(Lowered.error());
  return eraseInstruction(Instruction);
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

[[nodiscard]] Expected<Register> zeroExtendToRV64(MachineIRBuilder &Builder, MachineInstr &Anchor,
                                                  MachineFunction &Function, Register Source,
                                                  std::vector<MachineInstr *> &DeadCandidates) {
  const auto Info = Function.registerInfo().virtualRegisterInfo(Source);
  if (!Info || !Info->get().Type)
    return makeUnexpected(ErrorCode::VerificationError, "RISC-V merge source has no scalar type");
  const LLT SourceType = *Info->get().Type;
  if (SourceType.scalarBitWidth() == 64)
    return Source;
  const MachineOperand *Definition = Function.registerInfo().getUniqueVRegDef(Source);
  MachineInstr *Def =
      Definition == nullptr ? nullptr : const_cast<MachineInstr *>(Definition->parent());
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
    const auto ResultInfo =
        TruncResult == nullptr
            ? Expected<std::reference_wrapper<const VirtualRegisterInfo>>(
                  std::unexpected(Error(ErrorCode::InvalidArgument, "missing truncation result")))
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

[[nodiscard]] Expected<void> lowerMergeValues(MachineInstr &Instruction, MachineFunction &Function,
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
        operandType(Function, *Use, 0)
            .transform([](LLT Type) { return Type.scalarBitWidth() == 64; })
            .value_or(false)) {
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
      return makeUnexpected(ErrorCode::VerificationError, "RISC-V merge source is not a register");
    auto WideSource = zeroExtendToRV64(Builder, Instruction, Function, *Source, DeadCandidates);
    if (!WideSource)
      return std::unexpected(WideSource.error());
    if (Index == 1) {
      Terms.push_back(*WideSource);
      continue;
    }
    const std::optional<LLT> SourceType = registerType(Function, *Operand);
    if (!SourceType)
      return makeUnexpected(ErrorCode::VerificationError, "RISC-V merge source has no scalar type");
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
    auto Rewritten =
        rewriteBinary(Instruction, GenericOpcode::G_OR, Result, Accumulator, Terms.back());
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

[[nodiscard]] Expected<void> foldWideExtendedUnmerge(MachineInstr &Instruction,
                                                     MachineFunction &Function) {
  const std::size_t OutputCount = Instruction.operandCount() - 1U;
  const MachineOperand *SourceOperand = Instruction.operandAt(OutputCount);
  const Register *Source = SourceOperand == nullptr ? nullptr : SourceOperand->getIf<Register>();
  const MachineOperand *Definition =
      Source == nullptr ? nullptr : Function.registerInfo().getUniqueVRegDef(*Source);
  MachineInstr *Extension =
      Definition == nullptr ? nullptr : const_cast<MachineInstr *>(Definition->parent());
  if (Extension == nullptr || (Extension->opcode() != opcode(GenericOpcode::G_ZEXT) &&
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
      return makeUnexpected(ErrorCode::VerificationError, "RISC-V wide unmerge output has no type");
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
        return makeUnexpected(
            ErrorCode::Unsupported,
            "RISC-V wide unmerge result is only representable through truncation");
      const MachineOperand *ResultOperand = Trunc->operandAt(0);
      const Register *Result =
          ResultOperand == nullptr ? nullptr : ResultOperand->getIf<Register>();
      const auto ResultInfo =
          Result == nullptr
              ? Expected<std::reference_wrapper<const VirtualRegisterInfo>>(
                    std::unexpected(Error(ErrorCode::InvalidArgument, "missing truncation result")))
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
    return makeUnexpected(ErrorCode::VerificationError, "RISC-V unmerge source is not a register");
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
    const std::uint64_t Offset = static_cast<std::uint64_t>(Index) * OutputType->scalarBitWidth();
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
    auto Truncated =
        buildUnaryBefore(Builder, Instruction, GenericOpcode::G_TRUNC, *OutputType, Extracted);
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

[[nodiscard]] Expected<WideValueParts>
collectWideValueParts(Register Value, MachineFunction &Function, unsigned Depth = 0) {
  if (Depth > 16)
    return makeUnexpected(ErrorCode::Unsupported, "RISC-V wide-value artifact chain is too deep");
  const auto Info = Function.registerInfo().virtualRegisterInfo(Value);
  if (!Info || !Info->get().Type)
    return makeUnexpected(ErrorCode::VerificationError, "RISC-V wide value has no scalar type");
  const LLT Type = *Info->get().Type;
  if ((!Type.isInteger() && !Type.isAnyScalar()) || Type.scalarBitWidth() == 0)
    return makeUnexpected(ErrorCode::Unsupported,
                          "RISC-V wide-value decomposition requires an integer scalar");
  if (Type.scalarBitWidth() <= 64)
    return WideValueParts{.BitWidth = Type.scalarBitWidth(), .Parts = {Value}, .Artifacts = {}};

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

[[nodiscard]] Expected<WideResultPlan> wideResultPlan(Register Result, MachineFunction &Function,
                                                      std::size_t RequiredParts) {
  MachineInstr *Use = singleUseInstruction(Function, Result);
  WideResultPlan Plan;
  if (Use != nullptr && (Use->opcode() == opcode(GenericOpcode::G_ANYEXT) ||
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

[[nodiscard]] Expected<Register> buildIntegerCompareBefore(MachineIRBuilder &Builder,
                                                           MachineInstr &Anchor,
                                                           IntegerPredicate Predicate,
                                                           Register Left, Register Right) {
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

[[nodiscard]] Expected<Register> buildSelectBefore(MachineIRBuilder &Builder, MachineInstr &Anchor,
                                                   LLT Type, Register Condition, Register TrueValue,
                                                   Register FalseValue) {
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
emitTwoLimbLeftShiftBody(MachineIRBuilder &Builder, MachineInstr &Anchor, Register Low,
                         Register High, Register Amount, Register AmountExcess, Register AmountLack,
                         Register Zero, Register IsShort, Register IsZero) {
  auto Type = LLT::makeAnyScalar(64);
  if (!Type)
    return std::unexpected(Type.error());
  auto LowShort = buildBinaryBefore(Builder, Anchor, GenericOpcode::G_SHL, *Type, Low, Amount);
  if (!LowShort)
    return std::unexpected(LowShort.error());
  auto CrossLow = buildBinaryBefore(Builder, Anchor, GenericOpcode::G_LSHR, *Type, Low, AmountLack);
  if (!CrossLow)
    return std::unexpected(CrossLow.error());
  auto HighShort = buildBinaryBefore(Builder, Anchor, GenericOpcode::G_SHL, *Type, High, Amount);
  if (!HighShort)
    return std::unexpected(HighShort.error());
  auto ShortHigh =
      buildBinaryBefore(Builder, Anchor, GenericOpcode::G_OR, *Type, *CrossLow, *HighShort);
  if (!ShortHigh)
    return std::unexpected(ShortHigh.error());
  auto LongHigh =
      buildBinaryBefore(Builder, Anchor, GenericOpcode::G_SHL, *Type, Low, AmountExcess);
  if (!LongHigh)
    return std::unexpected(LongHigh.error());
  auto ResultLow = buildSelectBefore(Builder, Anchor, *Type, IsShort, *LowShort, Zero);
  if (!ResultLow)
    return std::unexpected(ResultLow.error());
  auto ShiftedHigh = buildSelectBefore(Builder, Anchor, *Type, IsShort, *ShortHigh, *LongHigh);
  if (!ShiftedHigh)
    return std::unexpected(ShiftedHigh.error());
  auto ResultHigh = buildSelectBefore(Builder, Anchor, *Type, IsZero, High, *ShiftedHigh);
  if (!ResultHigh)
    return std::unexpected(ResultHigh.error());
  return TwoLimbShiftResult{.Low = *ResultLow, .High = *ResultHigh};
}

[[nodiscard]] Expected<TwoLimbShiftResult> emitTwoLimbLeftShift(MachineIRBuilder &Builder,
                                                                MachineInstr &Anchor, Register Low,
                                                                Register High, Register Amount) {
  auto Type = LLT::makeAnyScalar(64);
  if (!Type)
    return std::unexpected(Type.error());
  auto Boundary = buildIntegerConstantBefore(Builder, Anchor, 64, 64);
  if (!Boundary)
    return std::unexpected(Boundary.error());
  auto AmountExcess =
      buildBinaryBefore(Builder, Anchor, GenericOpcode::G_SUB, *Type, Amount, *Boundary);
  if (!AmountExcess)
    return std::unexpected(AmountExcess.error());
  auto AmountLack =
      buildBinaryBefore(Builder, Anchor, GenericOpcode::G_SUB, *Type, *Boundary, Amount);
  if (!AmountLack)
    return std::unexpected(AmountLack.error());
  auto Zero = buildIntegerConstantBefore(Builder, Anchor, 64, 0);
  if (!Zero)
    return std::unexpected(Zero.error());
  auto IsShort = buildIntegerCompareBefore(Builder, Anchor, IntegerPredicate::UnsignedLessThan,
                                           Amount, *Boundary);
  if (!IsShort)
    return std::unexpected(IsShort.error());
  auto IsZero = buildIntegerCompareBefore(Builder, Anchor, IntegerPredicate::Equal, Amount, *Zero);
  if (!IsZero)
    return std::unexpected(IsZero.error());
  return emitTwoLimbLeftShiftBody(Builder, Anchor, Low, High, Amount, *AmountExcess, *AmountLack,
                                  *Zero, *IsShort, *IsZero);
}

[[nodiscard]] Expected<Register>
emitLowLimbOfTwoLimbLogicalRightShift(MachineIRBuilder &Builder, MachineInstr &Anchor, Register Low,
                                      Register High, Register Amount, Register Boundary,
                                      Register Zero) {
  auto Type = LLT::makeAnyScalar(64);
  if (!Type)
    return std::unexpected(Type.error());
  auto AmountExcess =
      buildBinaryBefore(Builder, Anchor, GenericOpcode::G_SUB, *Type, Amount, Boundary);
  if (!AmountExcess)
    return std::unexpected(AmountExcess.error());
  auto AmountLack =
      buildBinaryBefore(Builder, Anchor, GenericOpcode::G_SUB, *Type, Boundary, Amount);
  if (!AmountLack)
    return std::unexpected(AmountLack.error());
  auto IsShort = buildIntegerCompareBefore(Builder, Anchor, IntegerPredicate::UnsignedLessThan,
                                           Amount, Boundary);
  if (!IsShort)
    return std::unexpected(IsShort.error());
  auto IsZero = buildIntegerCompareBefore(Builder, Anchor, IntegerPredicate::Equal, Amount, Zero);
  if (!IsZero)
    return std::unexpected(IsZero.error());
  auto LowShort = buildBinaryBefore(Builder, Anchor, GenericOpcode::G_LSHR, *Type, Low, Amount);
  if (!LowShort)
    return std::unexpected(LowShort.error());
  auto HighShort =
      buildBinaryBefore(Builder, Anchor, GenericOpcode::G_SHL, *Type, High, *AmountLack);
  if (!HighShort)
    return std::unexpected(HighShort.error());
  auto ShortValue =
      buildBinaryBefore(Builder, Anchor, GenericOpcode::G_OR, *Type, *LowShort, *HighShort);
  if (!ShortValue)
    return std::unexpected(ShortValue.error());
  auto LongValue =
      buildBinaryBefore(Builder, Anchor, GenericOpcode::G_LSHR, *Type, High, *AmountExcess);
  if (!LongValue)
    return std::unexpected(LongValue.error());
  auto Shifted = buildSelectBefore(Builder, Anchor, *Type, *IsShort, *ShortValue, *LongValue);
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
  auto OuterExcess =
      buildBinaryBefore(Builder, Anchor, GenericOpcode::G_SUB, *Type, Amount, *OuterBoundary);
  if (!OuterExcess)
    return std::unexpected(OuterExcess.error());
  auto OuterLack =
      buildBinaryBefore(Builder, Anchor, GenericOpcode::G_SUB, *Type, *OuterBoundary, Amount);
  if (!OuterLack)
    return std::unexpected(OuterLack.error());
  auto Zero = buildIntegerConstantBefore(Builder, Anchor, 64, 0);
  if (!Zero)
    return std::unexpected(Zero.error());
  auto OuterIsShort = buildIntegerCompareBefore(Builder, Anchor, IntegerPredicate::UnsignedLessThan,
                                                Amount, *OuterBoundary);
  if (!OuterIsShort)
    return std::unexpected(OuterIsShort.error());
  auto OuterIsZero =
      buildIntegerCompareBefore(Builder, Anchor, IntegerPredicate::Equal, Amount, *Zero);
  if (!OuterIsZero)
    return std::unexpected(OuterIsZero.error());

  auto InnerBoundary = buildIntegerConstantBefore(Builder, Anchor, 64, 64);
  if (!InnerBoundary)
    return std::unexpected(InnerBoundary.error());
  auto InnerExcess =
      buildBinaryBefore(Builder, Anchor, GenericOpcode::G_SUB, *Type, Amount, *InnerBoundary);
  if (!InnerExcess)
    return std::unexpected(InnerExcess.error());
  auto InnerLack =
      buildBinaryBefore(Builder, Anchor, GenericOpcode::G_SUB, *Type, *InnerBoundary, Amount);
  if (!InnerLack)
    return std::unexpected(InnerLack.error());
  auto InnerIsShort = buildIntegerCompareBefore(Builder, Anchor, IntegerPredicate::UnsignedLessThan,
                                                Amount, *InnerBoundary);
  if (!InnerIsShort)
    return std::unexpected(InnerIsShort.error());
  auto InnerIsZero =
      buildIntegerCompareBefore(Builder, Anchor, IntegerPredicate::Equal, Amount, *Zero);
  if (!InnerIsZero)
    return std::unexpected(InnerIsZero.error());
  auto Lower = emitTwoLimbLeftShiftBody(Builder, Anchor, Parts[0], Parts[1], Amount, *InnerExcess,
                                        *InnerLack, *Zero, *InnerIsShort, *InnerIsZero);
  if (!Lower)
    return std::unexpected(Lower.error());

  auto Cross = emitLowLimbOfTwoLimbLogicalRightShift(Builder, Anchor, Parts[0], Parts[1],
                                                     *OuterLack, *InnerBoundary, *Zero);
  if (!Cross)
    return std::unexpected(Cross.error());
  auto HighIsShort = buildIntegerCompareBefore(Builder, Anchor, IntegerPredicate::UnsignedLessThan,
                                               Amount, *InnerBoundary);
  if (!HighIsShort)
    return std::unexpected(HighIsShort.error());
  auto HighShort =
      buildBinaryBefore(Builder, Anchor, GenericOpcode::G_SHL, *Type, Parts[2], Amount);
  if (!HighShort)
    return std::unexpected(HighShort.error());
  auto BoundedHigh = buildSelectBefore(Builder, Anchor, *Type, *HighIsShort, *HighShort, *Zero);
  if (!BoundedHigh)
    return std::unexpected(BoundedHigh.error());
  auto ShortHigh =
      buildBinaryBefore(Builder, Anchor, GenericOpcode::G_OR, *Type, *Cross, *BoundedHigh);
  if (!ShortHigh)
    return std::unexpected(ShortHigh.error());

  auto LongIsShort = buildIntegerCompareBefore(Builder, Anchor, IntegerPredicate::UnsignedLessThan,
                                               *OuterExcess, *InnerBoundary);
  if (!LongIsShort)
    return std::unexpected(LongIsShort.error());
  auto LongShift =
      buildBinaryBefore(Builder, Anchor, GenericOpcode::G_SHL, *Type, Parts[0], *OuterExcess);
  if (!LongShift)
    return std::unexpected(LongShift.error());
  auto LongHigh = buildSelectBefore(Builder, Anchor, *Type, *LongIsShort, *LongShift, *Zero);
  if (!LongHigh)
    return std::unexpected(LongHigh.error());

  auto ResultLow = buildSelectBefore(Builder, Anchor, *Type, *OuterIsShort, Lower->Low, *Zero);
  if (!ResultLow)
    return std::unexpected(ResultLow.error());
  auto ResultMiddle = buildSelectBefore(Builder, Anchor, *Type, *OuterIsShort, Lower->High, *Zero);
  if (!ResultMiddle)
    return std::unexpected(ResultMiddle.error());
  auto ShiftedHigh =
      buildSelectBefore(Builder, Anchor, *Type, *OuterIsShort, *ShortHigh, *LongHigh);
  if (!ShiftedHigh)
    return std::unexpected(ShiftedHigh.error());
  auto ResultHigh = buildSelectBefore(Builder, Anchor, *Type, *OuterIsZero, Parts[2], *ShiftedHigh);
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
  auto ResultPlan = wideResultPlan(*Destination, Function, ValueParts->Parts.size());
  if (!ResultPlan)
    return std::unexpected(ResultPlan.error());

  MachineIRBuilder Builder(Function, Target);
  Expected<std::vector<Register>> Results =
      ValueParts->Parts.size() == 2
          ? emitTwoLimbLeftShift(Builder, Instruction, ValueParts->Parts[0], ValueParts->Parts[1],
                                 AmountParts->Parts[0])
                .transform([](TwoLimbShiftResult Result) {
                  return std::vector<Register>{Result.Low, Result.High};
                })
      : ValueParts->Parts.size() == 3
          ? emitThreeLimbLeftShift(Builder, Instruction, ValueParts->Parts, AmountParts->Parts[0])
          : Expected<std::vector<Register>>(std::unexpected(
                Error(ErrorCode::Unsupported,
                      "RISC-V wide shift currently requires two or three 64-bit limbs")));
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
  for (std::size_t Index = 1; Index < AmountParts->Parts.size(); ++Index) {
    const MachineOperand *Definition =
        Function.registerInfo().getUniqueVRegDef(AmountParts->Parts[Index]);
    if (Definition != nullptr && Definition->parent() != nullptr)
      AmountParts->Artifacts.push_back(const_cast<MachineInstr *>(Definition->parent()));
  }
  ValueParts->Artifacts.insert(ValueParts->Artifacts.end(), AmountParts->Artifacts.begin(),
                               AmountParts->Artifacts.end());
  eraseDeadArtifacts(ValueParts->Artifacts, Function);
  return {};
}

[[nodiscard]] Expected<std::vector<Register>> emitWideAdd(MachineIRBuilder &Builder,
                                                          MachineInstr &Anchor,
                                                          std::span<const Register> Left,
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
  auto Carry = buildIntegerCompareBefore(Builder, Anchor, IntegerPredicate::UnsignedLessThan, *Low,
                                         Right[0]);
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
      auto Compared = buildIntegerCompareBefore(Builder, Anchor, IntegerPredicate::UnsignedLessThan,
                                                *Base, Left[Index]);
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
      auto IsZero =
          buildIntegerCompareBefore(Builder, Anchor, IntegerPredicate::Equal, *Sum, *Zero);
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

[[nodiscard]] Expected<std::vector<Register>> emitWideSub(MachineIRBuilder &Builder,
                                                          MachineInstr &Anchor,
                                                          std::span<const Register> Left,
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
      auto Compared = buildIntegerCompareBefore(
          Builder, Anchor, IntegerPredicate::UnsignedGreaterThan, *Base, Left[Index]);
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

[[nodiscard]] Expected<void> lowerWideBinary(MachineInstr &Instruction, MachineFunction &Function,
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
  if (Instruction.opcode() != opcode(GenericOpcode::G_BRCOND) || Instruction.operandCount() < 2)
    return false;
  const MachineOperand *ConditionOperand = Instruction.operandAt(0);
  const Register *Condition =
      ConditionOperand == nullptr ? nullptr : ConditionOperand->getIf<Register>();
  if (Condition == nullptr || !Condition->isVirtual() ||
      singleRegisterUse(Function, *Condition) != &Instruction)
    return false;
  const auto ConditionInfo = Function.registerInfo().virtualRegisterInfo(*Condition);
  const MachineOperand *Definition = Function.registerInfo().getUniqueVRegDef(*Condition);
  const MachineInstr *Truncation = Definition == nullptr ? nullptr : Definition->parent();
  if (!ConditionInfo || !ConditionInfo->get().Type ||
      ConditionInfo->get().Type->scalarBitWidth() != 1 || Truncation == nullptr ||
      Truncation->opcode() != opcode(GenericOpcode::G_TRUNC) || Truncation->operandCount() != 2)
    return false;
  return operandType(Function, *Truncation, 1)
      .transform([](LLT Type) { return isScalarGPRType(Type) && Type.scalarBitWidth() == 64; })
      .value_or(false);
}

[[nodiscard]] Expected<void> lowerNarrowBranchCondition(MachineInstr &Instruction,
                                                        MachineFunction &Function,
                                                        const TargetDescription &Target) {
  if (!canLowerNarrowBranchCondition(Instruction, Function))
    return makeUnexpected(ErrorCode::Unsupported,
                          "RISC-V branch condition is not a narrow truncation");
  const MachineOperand *ConditionOperand = Instruction.operandAt(0);
  const Register Condition = *ConditionOperand->getIf<Register>();
  const MachineOperand *Definition = Function.registerInfo().getUniqueVRegDef(Condition);
  MachineInstr *Truncation = const_cast<MachineInstr *>(Definition->parent());
  const Register Source = *Truncation->operandAt(1)->getIf<Register>();
  auto Type = LLT::makeAnyScalar(64);
  if (!Type)
    return std::unexpected(Type.error());
  MachineIRBuilder Builder(Function, Target);
  auto One = buildIntegerConstantBefore(Builder, Instruction, 64, 1);
  if (!One)
    return std::unexpected(One.error());
  auto Masked = buildBinaryBefore(Builder, Instruction, GenericOpcode::G_AND, *Type, Source, *One);
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
  while (Instruction != nullptr && Instruction->opcode() == opcode(GenericOpcode::COPY))
    Instruction = Instruction->next();
  return Instruction;
}

[[nodiscard]] Expected<Register>
buildIncomingConversion(MachineFunction &Function, const TargetDescription &Target,
                        MachineBasicBlock &Block, MachineInstr *Anchor, GenericOpcode Generic,
                        LLT Type, Register Source) {
  auto Result = Function.registerInfo().createVirtualRegister(Type);
  if (!Result)
    return std::unexpected(Result.error());
  MachineIRBuilder Builder(Function, Target);
  auto Point =
      Anchor == nullptr ? Builder.setInsertPoint(Block) : Builder.setInsertPointBefore(*Anchor);
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

[[nodiscard]] Expected<void> lowerNarrowPhi(MachineInstr &Instruction, MachineFunction &Function,
                                            const TargetDescription &Target) {
  const std::optional<LLT> OriginalType = resultType(Function, Instruction);
  const MachineOperand *DestinationOperand = Instruction.operandAt(0);
  const Register *Destination =
      DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
  if (!OriginalType || Destination == nullptr || OriginalType->scalarBitWidth() >= 32 ||
      Instruction.operandCount() < 3 || (Instruction.operandCount() % 2U) == 0)
    return makeUnexpected(ErrorCode::VerificationError, "RISC-V narrow PHI has invalid operands");
  auto LegalType = LLT::makeAnyScalar(32);
  if (!LegalType)
    return std::unexpected(LegalType.error());

  std::vector<NarrowPhiIncoming> Incoming;
  Incoming.reserve((Instruction.operandCount() - 1U) / 2U);
  for (std::size_t Index = 1; Index < Instruction.operandCount(); Index += 2) {
    const MachineOperand *ValueOperand = Instruction.operandAt(Index);
    const Register *Value = ValueOperand == nullptr ? nullptr : ValueOperand->getIf<Register>();
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
        NarrowPhiIncoming{.Value = *Value, .Block = Block->Block, .ReusableTruncation = Reusable});
  }
  for (NarrowPhiIncoming &Entry : Incoming) {
    if (Entry.ReusableTruncation == nullptr)
      continue;
    const std::size_t Count = static_cast<std::size_t>(std::ranges::count(
        Incoming, Entry.ReusableTruncation, &NarrowPhiIncoming::ReusableTruncation));
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
    if (std::ranges::find(Anchors, Entry.Block, &decltype(Anchors)::value_type::first) ==
        Anchors.end()) {
      Anchors.emplace_back(Entry.Block, firstInstructionAfterLeadingCopies(*Entry.Block));
    }
  }
  std::vector<Register> LegalIncoming;
  LegalIncoming.reserve(Incoming.size());
  for (NarrowPhiIncoming &Entry : Incoming) {
    const auto AnchorPosition =
        std::ranges::find(Anchors, Entry.Block, &decltype(Anchors)::value_type::first);
    MachineInstr *Anchor = AnchorPosition->second;
    if (Entry.ReusableTruncation != nullptr) {
      auto SetType = Function.registerInfo().setType(Entry.Value, *LegalType);
      if (!SetType)
        return std::unexpected(SetType.error());
      auto Inserted =
          Anchor == nullptr
              ? Entry.Block->appendInstruction(*Entry.ReusableTruncation)
              : Entry.Block->insertInstructionBefore(*Anchor, *Entry.ReusableTruncation);
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
    auto Converted = buildIncomingConversion(Function, Target, *Entry.Block, Anchor, Conversion,
                                             *LegalType, Entry.Value);
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

[[nodiscard]] Expected<void> lowerWidePhi(MachineInstr &Instruction, MachineFunction &Function,
                                          const TargetDescription &Target) {
  const MachineOperand *DestinationOperand = Instruction.operandAt(0);
  const Register *Destination =
      DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
  if (Destination == nullptr || Instruction.operandCount() < 5 ||
      (Instruction.operandCount() % 2U) == 0)
    return makeUnexpected(ErrorCode::VerificationError, "RISC-V wide PHI has invalid operands");

  std::vector<WideValueParts> IncomingParts;
  std::vector<MachineBasicBlock *> IncomingBlocks;
  for (std::size_t Index = 1; Index < Instruction.operandCount(); Index += 2) {
    const MachineOperand *ValueOperand = Instruction.operandAt(Index);
    const Register *Value = ValueOperand == nullptr ? nullptr : ValueOperand->getIf<Register>();
    const MachineOperand *BlockOperand = Instruction.operandAt(Index + 1U);
    const auto *Block =
        BlockOperand == nullptr ? nullptr : BlockOperand->getIf<BasicBlockOperand>();
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
    for (std::size_t IncomingIndex = 0; IncomingIndex < IncomingParts.size(); ++IncomingIndex) {
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

struct RV64ScalarLayout final {
  std::uint64_t StoreSize = 0;
  std::uint64_t AllocationSize = 0;
  std::uint64_t ABIAlignment = 0;
};

[[nodiscard]] std::optional<RV64ScalarLayout> rv64ScalarLayout(LLT Type) noexcept {
  if (Type.isPointer()) {
    return Type.addressSpace() == 0 && Type.scalarBitWidth() == 64
               ? std::optional<RV64ScalarLayout>{{8, 8, 8}}
               : std::nullopt;
  }
  if ((!isSupportedIntegerType(Type) && !isRVFloatType(Type)) || Type.scalarBitWidth() == 0) {
    return std::nullopt;
  }
  const std::uint64_t StoreBytes = (Type.scalarBitWidth() + 7U) / 8U;
  const std::uint64_t AllocationBytes = std::bit_ceil(StoreBytes);
  return RV64ScalarLayout{StoreBytes, AllocationBytes, std::min<std::uint64_t>(AllocationBytes, 8)};
}

[[nodiscard]] Expected<const MachineMemOperand *>
createMemoryAccess(MachineFunction &Function, MemoryAccessFlag Access, LLT Type,
                   std::uint64_t SizeBytes, std::uint64_t AlignmentBytes) {
  auto Size = MemorySize::fixed(SizeBytes);
  auto Align = Alignment::create(AlignmentBytes);
  if (!Size || !Align)
    return makeUnexpected(ErrorCode::InternalError, "RISC-V memory facts are invalid");
  MachineMemOperandDesc Description;
  Description.Accesses = {Access};
  Description.Size = *Size;
  Description.MemoryType = Type;
  Description.Align = *Align;
  return Function.createMemOperand(std::move(Description));
}

struct VAArgPlan final {
  Register Destination;
  Register ListPointer;
  LLT DestinationType;
  LLT PointerType;
  std::uint64_t Alignment = 0;
  RV64ScalarLayout ValueLayout;
};

[[nodiscard]] std::optional<VAArgPlan> vaargPlan(const MachineInstr &Instruction,
                                                 const MachineFunction &Function) {
  if (Instruction.opcode() != opcode(GenericOpcode::G_VAARG) || Instruction.operandCount() != 3 ||
      !Instruction.memOperands().empty()) {
    return std::nullopt;
  }
  const MachineOperand *DestinationOperand = Instruction.operandAt(0);
  const MachineOperand *ListOperand = Instruction.operandAt(1);
  const MachineOperand *AlignmentOperand = Instruction.operandAt(2);
  const Register *Destination =
      DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
  const Register *ListPointer = ListOperand == nullptr ? nullptr : ListOperand->getIf<Register>();
  const std::optional<LLT> DestinationType = resultType(Function, Instruction);
  const std::optional<LLT> PointerType = operandType(Function, Instruction, 1);
  const auto AlignmentValue = AlignmentOperand == nullptr
                                  ? Expected<std::int64_t>(std::unexpected(
                                        Error(ErrorCode::InvalidArgument, "missing alignment")))
                                  : signedImmediate(*AlignmentOperand);
  const std::optional<RV64ScalarLayout> ValueLayout =
      DestinationType ? rv64ScalarLayout(*DestinationType) : std::nullopt;
  if (Destination == nullptr || ListPointer == nullptr || !Destination->isVirtual() ||
      !ListPointer->isVirtual() || !DestinationOperand->isDef() || !ListOperand->isUse() ||
      !DestinationType || !PointerType || !PointerType->isPointer() ||
      PointerType->addressSpace() != 0 || PointerType->scalarBitWidth() != 64 || !AlignmentValue ||
      *AlignmentValue <= 0 || !std::has_single_bit(static_cast<std::uint64_t>(*AlignmentValue)) ||
      !ValueLayout) {
    return std::nullopt;
  }

  return VAArgPlan{*Destination,
                   *ListPointer,
                   *DestinationType,
                   *PointerType,
                   static_cast<std::uint64_t>(*AlignmentValue),
                   *ValueLayout};
}

[[nodiscard]] Expected<void> lowerVAArg(MachineInstr &Instruction, MachineFunction &Function,
                                        const TargetDescription &Target) {
  const auto Plan = vaargPlan(Instruction, Function);
  if (!Plan) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V va_arg legalization requires a scalar result, p0 list, and "
                          "power-of-two alignment");
  }

  auto ListLoadMemory =
      createMemoryAccess(Function, MemoryAccessFlag::Load, Plan->PointerType, 8, 8);
  if (!ListLoadMemory)
    return std::unexpected(ListLoadMemory.error());
  auto ListStoreMemory =
      createMemoryAccess(Function, MemoryAccessFlag::Store, Plan->PointerType, 8, 8);
  if (!ListStoreMemory)
    return std::unexpected(ListStoreMemory.error());

  auto CurrentList = Function.registerInfo().createVirtualRegister(Plan->PointerType);
  if (!CurrentList)
    return std::unexpected(CurrentList.error());
  MachineIRBuilder Builder(Function, Target);
  if (auto Point = Builder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());
  auto ListLoad = Builder.buildInstr(opcode(GenericOpcode::G_LOAD));
  if (!ListLoad)
    return std::unexpected(ListLoad.error());
  if (auto Added = ListLoad->addDef(*CurrentList); !Added)
    return std::unexpected(Added.error());
  if (auto Added = ListLoad->addUse(Plan->ListPointer); !Added)
    return std::unexpected(Added.error());
  if (auto Added = ListLoad->addMemOperand(**ListLoadMemory); !Added)
    return std::unexpected(Added.error());
  if (auto Built = ListLoad->build(); !Built)
    return std::unexpected(Built.error());

  Register ValueAddress = *CurrentList;
  if (Plan->Alignment > 1) {
    auto AlignmentOffset =
        buildIntegerConstantBefore(Builder, Instruction, 64, Plan->Alignment - 1U);
    if (!AlignmentOffset)
      return std::unexpected(AlignmentOffset.error());
    auto BiasedAddress = buildBinaryBefore(Builder, Instruction, GenericOpcode::G_PTR_ADD,
                                           Plan->PointerType, ValueAddress, *AlignmentOffset);
    if (!BiasedAddress)
      return std::unexpected(BiasedAddress.error());
    auto AlignmentMask =
        buildIntegerConstantBefore(Builder, Instruction, 64, ~(Plan->Alignment - std::uint64_t{1}));
    if (!AlignmentMask)
      return std::unexpected(AlignmentMask.error());
    auto AlignedAddress = buildBinaryBefore(Builder, Instruction, GenericOpcode::G_PTRMASK,
                                            Plan->PointerType, *BiasedAddress, *AlignmentMask);
    if (!AlignedAddress)
      return std::unexpected(AlignedAddress.error());
    ValueAddress = *AlignedAddress;
  }

  auto Increment =
      buildIntegerConstantBefore(Builder, Instruction, 64, Plan->ValueLayout.AllocationSize);
  if (!Increment)
    return std::unexpected(Increment.error());
  auto NextList = buildBinaryBefore(Builder, Instruction, GenericOpcode::G_PTR_ADD,
                                    Plan->PointerType, ValueAddress, *Increment);
  if (!NextList)
    return std::unexpected(NextList.error());
  auto ListStore = Builder.buildInstr(opcode(GenericOpcode::G_STORE));
  if (!ListStore)
    return std::unexpected(ListStore.error());
  if (auto Added = ListStore->addUse(*NextList); !Added)
    return std::unexpected(Added.error());
  if (auto Added = ListStore->addUse(Plan->ListPointer); !Added)
    return std::unexpected(Added.error());
  if (auto Added = ListStore->addMemOperand(**ListStoreMemory); !Added)
    return std::unexpected(Added.error());
  if (auto Built = ListStore->build(); !Built)
    return std::unexpected(Built.error());

  if (!Function.registerInfo().useEmpty(Plan->Destination)) {
    auto ValueLoadMemory =
        createMemoryAccess(Function, MemoryAccessFlag::Load, Plan->DestinationType,
                           Plan->ValueLayout.StoreSize, Plan->ValueLayout.ABIAlignment);
    if (!ValueLoadMemory)
      return std::unexpected(ValueLoadMemory.error());
    auto ValueLoad = Builder.buildInstr(opcode(GenericOpcode::G_LOAD));
    if (!ValueLoad)
      return std::unexpected(ValueLoad.error());
    if (auto Added = ValueLoad->addDef(Plan->Destination); !Added)
      return std::unexpected(Added.error());
    if (auto Added = ValueLoad->addUse(ValueAddress); !Added)
      return std::unexpected(Added.error());
    if (auto Added = ValueLoad->addMemOperand(**ValueLoadMemory); !Added)
      return std::unexpected(Added.error());
    if (auto Built = ValueLoad->build(); !Built)
      return std::unexpected(Built.error());
  }
  return eraseInstruction(Instruction);
}

struct VACopyPlan final {
  Register Destination;
  Register Source;
  LLT PointerType;
};

[[nodiscard]] std::optional<VACopyPlan> vacopyPlan(const MachineInstr &Instruction,
                                                   const MachineFunction &Function) {
  if (Instruction.opcode() != opcode(GenericOpcode::G_INTRINSIC_W_SIDE_EFFECTS) ||
      Instruction.operandCount() != 3 || !Instruction.memOperands().empty()) {
    return std::nullopt;
  }
  const MachineOperand *IntrinsicIDOperand = Instruction.operandAt(0);
  const MachineOperand *DestinationOperand = Instruction.operandAt(1);
  const MachineOperand *SourceOperand = Instruction.operandAt(2);
  const auto *Intrinsic =
      IntrinsicIDOperand == nullptr ? nullptr : IntrinsicIDOperand->getIf<IntrinsicOperand>();
  const Register *Destination =
      DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
  const Register *Source = SourceOperand == nullptr ? nullptr : SourceOperand->getIf<Register>();
  const std::optional<LLT> DestinationType = operandType(Function, Instruction, 1);
  const std::optional<LLT> SourceType = operandType(Function, Instruction, 2);
  if (Intrinsic == nullptr || Intrinsic->ID != intrinsicID(GenericIntrinsic::VACopy) ||
      Destination == nullptr || Source == nullptr || !Destination->isVirtual() ||
      !Source->isVirtual() || !DestinationOperand->isUse() || !SourceOperand->isUse() ||
      !DestinationType || !SourceType || *DestinationType != *SourceType ||
      !DestinationType->isPointer() || DestinationType->addressSpace() != 0 ||
      DestinationType->scalarBitWidth() != 64) {
    return std::nullopt;
  }
  return VACopyPlan{*Destination, *Source, *DestinationType};
}

[[nodiscard]] Expected<void> lowerVACopy(MachineInstr &Instruction, MachineFunction &Function,
                                         const TargetDescription &Target) {
  const auto Plan = vacopyPlan(Instruction, Function);
  if (!Plan) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V va_copy legalization requires two p0 operands");
  }
  auto Size = MemorySize::fixed(8);
  auto Align = Alignment::create(8);
  if (!Size || !Align)
    return makeUnexpected(ErrorCode::InternalError, "RISC-V pointer memory facts are invalid");

  MachineMemOperandDesc LoadDescription;
  LoadDescription.Accesses = {MemoryAccessFlag::Load};
  LoadDescription.Size = *Size;
  LoadDescription.MemoryType = Plan->PointerType;
  LoadDescription.Align = *Align;
  auto LoadMemory = Function.createMemOperand(std::move(LoadDescription));
  if (!LoadMemory)
    return std::unexpected(LoadMemory.error());

  MachineMemOperandDesc StoreDescription;
  StoreDescription.Accesses = {MemoryAccessFlag::Store};
  StoreDescription.Size = *Size;
  StoreDescription.MemoryType = Plan->PointerType;
  StoreDescription.Align = *Align;
  auto StoreMemory = Function.createMemOperand(std::move(StoreDescription));
  if (!StoreMemory)
    return std::unexpected(StoreMemory.error());

  auto Value = Function.registerInfo().createVirtualRegister(Plan->PointerType);
  if (!Value)
    return std::unexpected(Value.error());
  MachineIRBuilder Builder(Function, Target);
  if (auto Point = Builder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());

  auto Load = Builder.buildInstr(opcode(GenericOpcode::G_LOAD));
  if (!Load)
    return std::unexpected(Load.error());
  if (auto Added = Load->addDef(*Value); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Load->addUse(Plan->Source); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Load->addMemOperand(**LoadMemory); !Added)
    return std::unexpected(Added.error());
  if (auto Built = Load->build(); !Built)
    return std::unexpected(Built.error());

  auto Store = Builder.buildInstr(opcode(GenericOpcode::G_STORE));
  if (!Store)
    return std::unexpected(Store.error());
  if (auto Added = Store->addUse(*Value); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Store->addUse(Plan->Destination); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Store->addMemOperand(**StoreMemory); !Added)
    return std::unexpected(Added.error());
  if (auto Built = Store->build(); !Built)
    return std::unexpected(Built.error());
  return eraseInstruction(Instruction);
}

} // namespace

Expected<LegalizationAction> RISCVLegalizerInfo::classify(const MachineInstr &Instruction,
                                                          const MachineFunction &Function,
                                                          const TargetDescription &) const {
  if (!Subtarget.features().has(RISCVExtension::I)) {
    return makeUnexpected(ErrorCode::InternalError,
                          "RISC-V legalizer requires the base I extension");
  }
  if (!isGenericOpcode(Instruction.opcode()))
    return LegalizationAction::Legal;

  const GenericOpcode Generic = static_cast<GenericOpcode>(Instruction.opcode().value());
  const std::optional<LLT> Type = resultType(Function, Instruction);
  switch (Generic) {
  case GenericOpcode::G_INTRINSIC_W_SIDE_EFFECTS:
    return vacopyPlan(Instruction, Function) ? LegalizationAction::Custom
                                             : LegalizationAction::Unsupported;
  case GenericOpcode::G_VAARG:
    return vaargPlan(Instruction, Function) ? LegalizationAction::Custom
                                            : LegalizationAction::Unsupported;
  case GenericOpcode::G_FRAME_INDEX:
    return Type && Type->isPointer() && Type->addressSpace() == 0 && Type->scalarBitWidth() == 64
               ? LegalizationAction::Legal
               : LegalizationAction::Unsupported;
  case GenericOpcode::G_BR:
    return LegalizationAction::Legal;
  case GenericOpcode::G_BRCOND: {
    const std::optional<LLT> Condition = operandType(Function, Instruction, 0);
    if (!Condition || !isScalarGPRType(*Condition))
      return LegalizationAction::Unsupported;
    return canLowerNarrowBranchCondition(Instruction, Function) ? LegalizationAction::Custom
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
      if (Instruction.memOperands().size() == 1 && Instruction.memOperands().front()->isAtomic()) {
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
      if (Instruction.memOperands().size() == 1 && Instruction.memOperands().front()->isAtomic()) {
        const std::uint64_t Bytes = Type->scalarBitWidth() / 8U;
        return isSupportedIntegerType(*Type) &&
                       isSupportedAtomicMemoryAccess(Instruction, AtomicOperation::Load, Bytes)
                   ? LegalizationAction::Legal
                   : LegalizationAction::Unsupported;
      }
      if ((isSupportedIntegerType(*Type) || isRVFloatType(*Type)) &&
          isSupportedNonAtomicMemoryAccess(Instruction) && memoryMatchesType(Instruction, *Type)) {
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
      const Register *Destination =
          DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
      MachineInstr *OnlyUse =
          Destination == nullptr ? nullptr : singleUseInstruction(Function, *Destination);
      return (Type->isInteger() || Type->isAnyScalar()) &&
                     (Source->isInteger() || Source->isAnyScalar()) &&
                     Type->scalarBitWidth() > 64 &&
                     Source->scalarBitWidth() < Type->scalarBitWidth() && OnlyUse != nullptr &&
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
    return Type && Source && isSupportedIntegerScalar(*Type) && isSupportedIntegerScalar(*Source) &&
                   Type->scalarBitWidth() != 0 && Source->scalarBitWidth() >= Type->scalarBitWidth()
               ? LegalizationAction::Legal
               : LegalizationAction::Unsupported;
  }
  case GenericOpcode::G_MERGE_VALUES:
  case GenericOpcode::G_UNMERGE_VALUES: {
    if (Instruction.operandCount() < 3)
      return LegalizationAction::Unsupported;
    const std::size_t SmallTypeIndex = Generic == GenericOpcode::G_MERGE_VALUES ? 1 : 0;
    const std::size_t BigTypeIndex =
        Generic == GenericOpcode::G_MERGE_VALUES ? 0 : Instruction.operandCount() - 1U;
    const std::optional<LLT> SmallType = operandType(Function, Instruction, SmallTypeIndex);
    const std::optional<LLT> BigType = operandType(Function, Instruction, BigTypeIndex);
    if (SmallType && BigType && Generic == GenericOpcode::G_MERGE_VALUES &&
        BigType->scalarBitWidth() > 64 && !canFoldWideMergeImmediately(Instruction, Function))
      return LegalizationAction::Legal;
    return SmallType && BigType && (SmallType->isInteger() || SmallType->isAnyScalar()) &&
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
    if (!Type || !isSupportedIntegerScalar(*Type) || IntegerPredicate == nullptr ||
        IntegerPredicate->integerPredicate() == nullptr || !Left || !Right || *Left != *Right ||
        !isSupportedIntegerType(*Left)) {
      return LegalizationAction::Unsupported;
    }
    return Type->scalarBitWidth() == 64 && Left->scalarBitWidth() == 64
               ? LegalizationAction::Legal
               : LegalizationAction::Custom;
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
        !Address->isPointer() || Address->addressSpace() != 0 || Address->scalarBitWidth() != 64 ||
        !isSupportedAtomicContainerAccess(Instruction, AtomicOperation::CompareExchange,
                                          (Type->scalarBitWidth() + 7U) / 8U)) {
      return LegalizationAction::Unsupported;
    }
    return Type->scalarBitWidth() == 64 ? LegalizationAction::Legal : LegalizationAction::Custom;
  }
  case GenericOpcode::G_ATOMIC_CMPXCHG_WITH_SUCCESS: {
    const std::optional<LLT> Success = operandType(Function, Instruction, 1);
    const std::optional<LLT> Address = operandType(Function, Instruction, 2);
    const std::optional<LLT> Compare = operandType(Function, Instruction, 3);
    const std::optional<LLT> NewValue = operandType(Function, Instruction, 4);
    return Type && Success && Address && Compare && NewValue && isI1Type(*Success) &&
                   *Compare == *Type && *NewValue == *Type &&
                   (Type->isInteger() || Type->isAnyScalar()) &&
                   (Type->scalarBitWidth() == 8 || Type->scalarBitWidth() == 16 ||
                    Type->scalarBitWidth() == 32 || Type->scalarBitWidth() == 64) &&
                   Address->isPointer() && Address->addressSpace() == 0 &&
                   Address->scalarBitWidth() == 64 &&
                   isSupportedAtomicContainerAccess(Instruction, AtomicOperation::CompareExchange,
                                                    (Type->scalarBitWidth() + 7U) / 8U)
               ? LegalizationAction::Custom
               : LegalizationAction::Unsupported;
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
    return Type && Type->isPointer() && Type->addressSpace() == 0 && Type->scalarBitWidth() == 64 &&
                   Pool != nullptr && Function.constantPool().entry(Pool->Index) != nullptr
               ? LegalizationAction::Legal
               : LegalizationAction::Unsupported;
  }
  case GenericOpcode::G_GLOBAL_VALUE: {
    const MachineOperand *GlobalOperand = Instruction.operandAt(1);
    const auto *Global =
        GlobalOperand == nullptr ? nullptr : GlobalOperand->getIf<GlobalSymbolOperand>();
    return Type && Type->isPointer() && Type->addressSpace() == 0 && Type->scalarBitWidth() == 64 &&
                   Global != nullptr && !Global->Name.empty()
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
    if (Generic == GenericOpcode::G_MUL && !Subtarget.features().has(RISCVExtension::M)) {
      if (Type->scalarBitWidth() == 64)
        return LegalizationAction::Libcall;
      const MachineOperand *DestinationOperand = Instruction.operandAt(0);
      const Register *Destination =
          DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
      MachineInstr *Extension =
          Destination == nullptr ? nullptr : singleRegisterUse(Function, *Destination);
      const std::optional<LLT> ExtensionType =
          Extension == nullptr ? std::nullopt : resultType(Function, *Extension);
      if (Extension != nullptr && Extension->opcode() == opcode(GenericOpcode::G_ANYEXT) &&
          ExtensionType && ExtensionType->scalarBitWidth() == 64) {
        return LegalizationAction::Legal;
      }
      return LegalizationAction::Custom;
    }
    return isSupportedIntegerScalar(*Type) ? LegalizationAction::Legal
                                           : LegalizationAction::Unsupported;
  }
  case GenericOpcode::G_PTR_ADD: {
    const std::optional<LLT> Pointer = operandType(Function, Instruction, 1);
    const std::optional<LLT> Offset = operandType(Function, Instruction, 2);
    return Type && Pointer && Offset && Type->isPointer() && Type->addressSpace() == 0 &&
                   Type->scalarBitWidth() == 64 && *Pointer == *Type &&
                   isSupportedIntegerScalar(*Offset) && Offset->scalarBitWidth() == 64
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

Expected<void> RISCVLegalizerInfo::lower(MachineInstr &Instruction, MachineFunction &Function,
                                         const TargetDescription &Target,
                                         LegalizationAction Action) const {
  if (Action == LegalizationAction::Custom &&
      Instruction.opcode() == opcode(GenericOpcode::G_INTRINSIC_W_SIDE_EFFECTS)) {
    return lowerVACopy(Instruction, Function, Target);
  }
  if (Action == LegalizationAction::Custom &&
      Instruction.opcode() == opcode(GenericOpcode::G_VAARG)) {
    return lowerVAArg(Instruction, Function, Target);
  }
  if (Instruction.opcode() == opcode(GenericOpcode::G_MUL) &&
      !Subtarget.features().has(RISCVExtension::M)) {
    if (Action == LegalizationAction::Custom)
      return widenNarrowMultiply(Instruction, Function, Target);
    if (Action == LegalizationAction::Libcall)
      return lowerMultiplyLibcall(Instruction, Function, Target,
                                  Subtarget.features().has(RISCVExtension::D)
                                      ? CallingConvention::LP64D
                                      : CallingConvention::LP64,
                                  *Subtarget.getCallLowering());
  }
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
  if (Action == LegalizationAction::Custom && Instruction.opcode() == opcode(GenericOpcode::G_PHI))
    return lowerNarrowPhi(Instruction, Function, Target);
  if (Action == LegalizationAction::Custom && Instruction.opcode() == opcode(GenericOpcode::G_ICMP))
    return lowerIntegerCompare(Instruction, Function, Target);
  if (Action == LegalizationAction::Custom &&
      Instruction.opcode() == opcode(GenericOpcode::G_ATOMIC_CMPXCHG))
    return lowerAtomicCompareExchange(Instruction, Function, Target);
  if (Action == LegalizationAction::Custom &&
      Instruction.opcode() == opcode(GenericOpcode::G_ATOMIC_CMPXCHG_WITH_SUCCESS))
    return lowerAtomicCompareExchangeWithSuccess(Instruction, Function, Target);
  if (Action == LegalizationAction::Custom &&
      (Instruction.opcode() == opcode(GenericOpcode::G_ANYEXT) ||
       Instruction.opcode() == opcode(GenericOpcode::G_SEXT) ||
       Instruction.opcode() == opcode(GenericOpcode::G_ZEXT))) {
    if (Instruction.opcode() == opcode(GenericOpcode::G_ANYEXT) &&
        narrowAnyExtBinaryPlan(Instruction, Function).has_value())
      return foldNarrowAnyExtBinary(Instruction, Function, Target);
    return foldConstantExtension(Instruction, Function,
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

} // namespace rcc::riscv
