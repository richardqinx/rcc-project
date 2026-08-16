// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/GlobalISel/GIMatchTableExecutor.h"

#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/GlobalISel/Utils.h"
#include "rcc/CodeGen/LowLevelType.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/TargetDescription.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rcc {
namespace {

[[nodiscard]] Expected<std::optional<LowLevelType>> operandType(const MachineInstr &Instruction,
                                                                const MachineFunction &Function,
                                                                std::size_t OperandIndex) {
  const MachineOperand *Operand = Instruction.operandAt(OperandIndex);
  const Register *Reg = Operand == nullptr ? nullptr : Operand->getIf<Register>();
  if (Reg == nullptr || !Reg->isVirtual())
    return std::optional<LowLevelType>{};
  auto Info = Function.registerInfo().virtualRegisterInfo(*Reg);
  if (!Info)
    return std::unexpected(Info.error());
  return Info->get().Type;
}

[[nodiscard]] Expected<bool> matchesType(const MachineInstr &Instruction,
                                         const MachineFunction &Function,
                                         const GIMatchTableInstruction &Step) {
  auto Type = operandType(Instruction, Function, Step.OperandIndex);
  if (!Type)
    return std::unexpected(Type.error());
  if (!*Type)
    return false;

  const auto Kind = static_cast<GIMatchTypeKind>(Step.Auxiliary);
  switch (Kind) {
  case GIMatchTypeKind::Any:
    return Step.Value == 0 || (*Type)->scalarBitWidth() == Step.Value;
  case GIMatchTypeKind::Scalar:
    return (*Type)->isCategoryNeutralScalar() && (*Type)->scalarBitWidth() == Step.Value;
  case GIMatchTypeKind::Pointer:
    return (*Type)->isPointer() && (*Type)->scalarBitWidth() == Step.Value;
  }
  return makeUnexpected(ErrorCode::VerificationError,
                        "generated match table has an invalid type predicate");
}

[[nodiscard]] Expected<bool> matchesBank(const MachineInstr &Instruction,
                                         const MachineFunction &Function,
                                         const GIMatchTableInstruction &Step) {
  const MachineOperand *Operand = Instruction.operandAt(Step.OperandIndex);
  const Register *Reg = Operand == nullptr ? nullptr : Operand->getIf<Register>();
  if (Reg == nullptr || !Reg->isVirtual())
    return false;
  auto Info = Function.registerInfo().virtualRegisterInfo(*Reg);
  if (!Info)
    return std::unexpected(Info.error());
  const VirtualRegisterInfo &RegisterInfo = Info->get();
  if (!RegisterInfo.Bank)
    return false;
  return RegisterInfo.Bank->value() == Step.Value;
}

[[nodiscard]] Expected<bool> matchesClass(const MachineInstr &Instruction,
                                          const MachineFunction &Function,
                                          const GIMatchTableInstruction &Step) {
  const MachineOperand *Operand = Instruction.operandAt(Step.OperandIndex);
  const Register *Reg = Operand == nullptr ? nullptr : Operand->getIf<Register>();
  if (Reg == nullptr || !Reg->isVirtual())
    return false;
  auto Info = Function.registerInfo().virtualRegisterInfo(*Reg);
  if (!Info)
    return std::unexpected(Info.error());
  const VirtualRegisterInfo &RegisterInfo = Info->get();
  if (!RegisterInfo.RegClass)
    return false;
  return RegisterInfo.RegClass->value() == Step.Value;
}

[[nodiscard]] Expected<bool> matchesIntegerPredicate(const MachineInstr &Instruction,
                                                     const GIMatchTableInstruction &Step) {
  if (Step.Value >= static_cast<std::uint32_t>(IntegerPredicate::Count)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "generated match table has an invalid integer predicate");
  }
  const MachineOperand *Operand = Instruction.operandAt(Step.OperandIndex);
  const auto *Predicate = Operand == nullptr ? nullptr : Operand->getIf<PredicateOperand>();
  const IntegerPredicate *Integer = Predicate == nullptr ? nullptr : Predicate->integerPredicate();
  return Integer != nullptr && static_cast<std::uint32_t>(*Integer) == Step.Value;
}

[[nodiscard]] Expected<bool> matchesOperandDefConstant(const MachineInstr &Instruction,
                                                       const MachineFunction &Function,
                                                       const GIMatchTableInstruction &Step) {
  const MachineOperand *Operand = Instruction.operandAt(Step.OperandIndex);
  const Register *Reg = Operand == nullptr ? nullptr : Operand->getIf<Register>();
  if (Reg == nullptr || !Reg->isVirtual())
    return false;
  const MachineInstr *Definition = Function.registerInfo().getVRegDef(*Reg);
  if (Definition == nullptr || Definition->opcode() != opcode(GenericOpcode::G_CONSTANT))
    return false;
  const MachineOperand *Value = Definition->operandAt(1);
  if (Value == nullptr)
    return false;
  if (const std::int64_t *Immediate = Value->getIf<std::int64_t>())
    return *Immediate == Step.Immediate;
  const auto *Integer = Value->getIf<IntegerImmediateOperand>();
  if (Integer == nullptr)
    return false;
  if (Step.Immediate == 0)
    return Integer->Value.isZero();
  const std::string Text = Integer->Value.toSignedDecimal();
  std::int64_t Parsed = 0;
  const auto [End, Error] = std::from_chars(Text.data(), Text.data() + Text.size(), Parsed);
  return Error == std::errc{} && End == Text.data() + Text.size() && Parsed == Step.Immediate;
}

[[nodiscard]] Expected<RegisterOperandFlags> registerFlagsFromRaw(std::uint32_t Raw) {
  constexpr std::uint32_t ValidBits =
      (std::uint32_t{1} << static_cast<std::uint16_t>(RegisterOperandFlag::Count)) - 1U;
  if ((Raw & ~ValidBits) != 0) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "generated match table has invalid register flags");
  }
  RegisterOperandFlags Flags;
  for (std::uint16_t Index = 0; Index < static_cast<std::uint16_t>(RegisterOperandFlag::Count);
       ++Index) {
    if ((Raw & (std::uint32_t{1} << Index)) != 0)
      Flags.set(static_cast<RegisterOperandFlag>(Index));
  }
  return Flags;
}

struct MatchedRule final {
  struct RenderedOperand final {
    enum class Kind : std::uint8_t {
      RootOperand,
      PhysicalRegister,
    };

    Kind SourceKind = Kind::RootOperand;
    std::uint32_t Value = 0;
    std::uint32_t Flags = 0;
  };

  Opcode ResultOpcode;
  std::optional<std::vector<RenderedOperand>> OperandLayout;
  std::vector<std::int64_t> AppendedImmediates;
  std::vector<Register> EraseOperandDefinitions;
};

[[nodiscard]] Expected<std::optional<MatchedRule>> matchRule(const GIMatchTable &Table,
                                                             const GIMatchTableRule &Rule,
                                                             const MachineInstr &Instruction,
                                                             const MachineFunction &Function) {
  const std::size_t First = Rule.FirstInstruction;
  const std::size_t Count = Rule.InstructionCount;
  if (First > Table.Instructions.size() || Count > Table.Instructions.size() - First) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "generated match table rule has an invalid instruction range");
  }

  MatchedRule Result;
  bool HasResultOpcode = false;
  bool Applying = false;
  bool Complete = false;
  for (std::size_t Index = First; Index < First + Count; ++Index) {
    const GIMatchTableInstruction &Step = Table.Instructions[Index];
    switch (Step.Opcode) {
    case GIMatchTableOpcode::CheckOpcode:
      if (Applying)
        return makeUnexpected(ErrorCode::VerificationError,
                              "generated match table performs a predicate after rendering");
      if (Instruction.opcode().value() != Step.Value)
        return std::optional<MatchedRule>{};
      break;
    case GIMatchTableOpcode::CheckOperandType: {
      if (Applying)
        return makeUnexpected(ErrorCode::VerificationError,
                              "generated match table performs a predicate after rendering");
      auto Matched = matchesType(Instruction, Function, Step);
      if (!Matched)
        return std::unexpected(Matched.error());
      if (!*Matched)
        return std::optional<MatchedRule>{};
      break;
    }
    case GIMatchTableOpcode::CheckOperandBank: {
      if (Applying)
        return makeUnexpected(ErrorCode::VerificationError,
                              "generated match table performs a predicate after rendering");
      auto Matched = matchesBank(Instruction, Function, Step);
      if (!Matched)
        return std::unexpected(Matched.error());
      if (!*Matched)
        return std::optional<MatchedRule>{};
      break;
    }
    case GIMatchTableOpcode::CheckOperandClass: {
      if (Applying)
        return makeUnexpected(ErrorCode::VerificationError,
                              "generated match table performs a predicate after rendering");
      auto Matched = matchesClass(Instruction, Function, Step);
      if (!Matched)
        return std::unexpected(Matched.error());
      if (!*Matched)
        return std::optional<MatchedRule>{};
      break;
    }
    case GIMatchTableOpcode::CheckIntegerPredicate: {
      if (Applying)
        return makeUnexpected(ErrorCode::VerificationError,
                              "generated match table performs a predicate after rendering");
      auto Matched = matchesIntegerPredicate(Instruction, Step);
      if (!Matched)
        return std::unexpected(Matched.error());
      if (!*Matched)
        return std::optional<MatchedRule>{};
      break;
    }
    case GIMatchTableOpcode::CheckOperandDefConstant: {
      if (Applying)
        return makeUnexpected(ErrorCode::VerificationError,
                              "generated match table performs a predicate after rendering");
      auto Matched = matchesOperandDefConstant(Instruction, Function, Step);
      if (!Matched)
        return std::unexpected(Matched.error());
      if (!*Matched)
        return std::optional<MatchedRule>{};
      break;
    }
    case GIMatchTableOpcode::MutateOpcode:
      if (Complete || HasResultOpcode || Step.Value == 0)
        return makeUnexpected(ErrorCode::VerificationError,
                              "generated match table has an invalid opcode mutation");
      Applying = true;
      Result.ResultOpcode = Opcode::fromValue(Step.Value);
      HasResultOpcode = true;
      break;
    case GIMatchTableOpcode::RenderOperand:
      if (Complete || !HasResultOpcode)
        return makeUnexpected(ErrorCode::VerificationError,
                              "generated match table has an invalid operand renderer");
      Applying = true;
      if (!Result.OperandLayout)
        Result.OperandLayout.emplace();
      Result.OperandLayout->push_back({
          .SourceKind = MatchedRule::RenderedOperand::Kind::RootOperand,
          .Value = Step.OperandIndex,
          .Flags = 0,
      });
      break;
    case GIMatchTableOpcode::RenderPhysicalRegister:
      if (Complete || !HasResultOpcode || Step.Value == 0)
        return makeUnexpected(ErrorCode::VerificationError,
                              "generated match table has an invalid physical register renderer");
      Applying = true;
      if (!Result.OperandLayout)
        Result.OperandLayout.emplace();
      Result.OperandLayout->push_back({
          .SourceKind = MatchedRule::RenderedOperand::Kind::PhysicalRegister,
          .Value = Step.Value,
          .Flags = Step.Auxiliary,
      });
      break;
    case GIMatchTableOpcode::AppendImmediate:
      if (Complete || Result.OperandLayout)
        return makeUnexpected(ErrorCode::VerificationError,
                              "generated match table renders after completion");
      Applying = true;
      Result.AppendedImmediates.push_back(Step.Immediate);
      break;
    case GIMatchTableOpcode::EraseOperandDefinition: {
      if (Complete || !HasResultOpcode)
        return makeUnexpected(ErrorCode::VerificationError,
                              "generated match table has an invalid erased definition");
      const MachineOperand *Operand = Instruction.operandAt(Step.OperandIndex);
      const Register *Reg = Operand == nullptr ? nullptr : Operand->getIf<Register>();
      if (Reg == nullptr || !Reg->isVirtual()) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generated match table erases a non-virtual operand definition");
      }
      Applying = true;
      Result.EraseOperandDefinitions.push_back(*Reg);
      break;
    }
    case GIMatchTableOpcode::Complete:
      if (Complete || !HasResultOpcode || Index + 1 != First + Count)
        return makeUnexpected(ErrorCode::VerificationError,
                              "generated match table has an invalid completion action");
      Complete = true;
      break;
    }
  }

  if (!Complete || !HasResultOpcode)
    return makeUnexpected(ErrorCode::VerificationError,
                          "generated match table rule does not complete rendering");
  return std::optional<MatchedRule>(std::move(Result));
}

[[nodiscard]] std::size_t explicitOperandCount(const MachineInstr &Instruction) noexcept {
  std::size_t Count = 0;
  while (Count < Instruction.operandCount() && !Instruction.operandAt(Count)->isImplicit())
    ++Count;
  return Count;
}

[[nodiscard]] Expected<void>
restoreInstruction(MachineInstr &Instruction, Opcode OriginalOpcode,
                   const std::vector<MachineOperand> &OriginalOperands,
                   const std::vector<std::pair<std::size_t, std::size_t>> &Ties) {
  while (Instruction.operandCount() != 0) {
    auto Erased = Instruction.eraseOperand(Instruction.operandCount() - 1U);
    if (!Erased)
      return std::unexpected(Erased.error());
  }
  auto RestoredOpcode = Instruction.setOpcode(OriginalOpcode);
  if (!RestoredOpcode)
    return std::unexpected(RestoredOpcode.error());
  for (const MachineOperand &Operand : OriginalOperands) {
    auto Added = Instruction.addOperand(Operand.cloneDetached());
    if (!Added)
      return std::unexpected(Added.error());
  }
  for (const auto [DefIndex, UseIndex] : Ties) {
    auto Tied = Instruction.tieOperands(DefIndex, UseIndex);
    if (!Tied)
      return std::unexpected(Tied.error());
  }
  return {};
}

[[nodiscard]] Expected<void> applyRule(const MatchedRule &Rule, MachineInstr &Instruction,
                                       MachineFunction &Function, const TargetDescription &Target) {
  const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Rule.ResultOpcode);
  if (Descriptor == nullptr)
    return makeUnexpected(ErrorCode::VerificationError,
                          "generated match table selects an unknown instruction opcode");

  const std::size_t OriginalExplicitCount = explicitOperandCount(Instruction);
  const std::size_t OriginalOperandCount = Instruction.operandCount();
  const Opcode OriginalOpcode = Instruction.opcode();
  std::vector<MachineOperand> OriginalOperands;
  OriginalOperands.reserve(OriginalOperandCount);
  for (const MachineOperand &Operand : Instruction.operands())
    OriginalOperands.push_back(Operand.cloneDetached());

  std::vector<std::pair<std::size_t, std::size_t>> Ties;
  for (std::size_t Index = 0; Index < OriginalOperandCount; ++Index) {
    const std::optional<std::size_t> Other = Instruction.operandAt(Index)->tiedOperandIndex();
    if (Other && Index < *Other)
      Ties.emplace_back(Index, *Other);
  }

  std::vector<MachineOperand> NewOperands;
  std::vector<std::optional<std::size_t>> SourceToOutput(OriginalOperandCount);
  std::size_t NewExplicitCount = 0;
  if (Rule.OperandLayout) {
    NewOperands.reserve(Rule.OperandLayout->size() + OriginalOperandCount - OriginalExplicitCount);
    for (const MatchedRule::RenderedOperand &Rendered : *Rule.OperandLayout) {
      if (Rendered.SourceKind == MatchedRule::RenderedOperand::Kind::RootOperand) {
        if (Rendered.Value >= OriginalExplicitCount) {
          return makeUnexpected(ErrorCode::VerificationError,
                                "generated match table renders an implicit or missing operand");
        }
        if (SourceToOutput[Rendered.Value] &&
            Instruction.operandAt(Rendered.Value)->tiedOperandIndex()) {
          return makeUnexpected(ErrorCode::VerificationError,
                                "generated match table duplicates a tied operand");
        }
        SourceToOutput[Rendered.Value] = NewOperands.size();
        NewOperands.push_back(Instruction.operandAt(Rendered.Value)->cloneDetached());
      } else {
        auto Physical = PhysicalRegister::create(Rendered.Value);
        if (!Physical || Target.registers().physicalRegister(*Physical) == nullptr)
          return makeUnexpected(ErrorCode::VerificationError,
                                "generated match table renders an unknown physical register");
        auto Flags = registerFlagsFromRaw(Rendered.Flags);
        if (!Flags)
          return std::unexpected(Flags.error());
        auto Operand = MachineOperand::registerOperand(Register::fromPhysical(*Physical), *Flags);
        if (!Operand)
          return std::unexpected(Operand.error());
        NewOperands.push_back(std::move(*Operand));
      }
    }
    NewExplicitCount = NewOperands.size();
  } else {
    NewOperands.reserve(OriginalOperandCount + Rule.AppendedImmediates.size());
    for (std::size_t Index = 0; Index < OriginalExplicitCount; ++Index) {
      SourceToOutput[Index] = NewOperands.size();
      NewOperands.push_back(Instruction.operandAt(Index)->cloneDetached());
    }
    NewExplicitCount = NewOperands.size();
    for (const std::int64_t Immediate : Rule.AppendedImmediates)
      NewOperands.push_back(MachineOperand::immediate(Immediate));
    NewExplicitCount = NewOperands.size();
  }
  for (std::size_t Index = OriginalExplicitCount; Index < OriginalOperandCount; ++Index) {
    SourceToOutput[Index] = NewOperands.size();
    NewOperands.push_back(Instruction.operandAt(Index)->cloneDetached());
  }

  if (!Descriptor->Variadic && Descriptor->Operands.size() != NewExplicitCount) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "generated match table result has an incompatible operand count");
  }

  auto SetOpcode = Instruction.setOpcode(Rule.ResultOpcode);
  if (!SetOpcode)
    return std::unexpected(SetOpcode.error());
  while (Instruction.operandCount() != 0) {
    auto Erased = Instruction.eraseOperand(Instruction.operandCount() - 1U);
    if (!Erased) {
      auto Restored = restoreInstruction(Instruction, OriginalOpcode, OriginalOperands, Ties);
      if (!Restored)
        return std::unexpected(Restored.error());
      return std::unexpected(Erased.error());
    }
  }
  for (MachineOperand &Operand : NewOperands) {
    auto Added = Instruction.addOperand(std::move(Operand));
    if (!Added) {
      auto Restored = restoreInstruction(Instruction, OriginalOpcode, OriginalOperands, Ties);
      if (!Restored)
        return std::unexpected(Restored.error());
      return std::unexpected(Added.error());
    }
  }

  for (const auto [OldDef, OldUse] : Ties) {
    const std::optional<std::size_t> &NewDef = SourceToOutput[OldDef];
    const std::optional<std::size_t> &NewUse = SourceToOutput[OldUse];
    if (!NewDef || !NewUse)
      continue;
    auto Tied = Instruction.tieOperands(NewDef.value(), NewUse.value());
    if (!Tied) {
      auto Restored = restoreInstruction(Instruction, OriginalOpcode, OriginalOperands, Ties);
      if (!Restored)
        return std::unexpected(Restored.error());
      return std::unexpected(Tied.error());
    }
  }

  for (const Register Reg : Rule.EraseOperandDefinitions) {
    MachineInstr *Definition = Function.registerInfo().getVRegDef(Reg);
    if (Definition == nullptr || Definition == &Instruction ||
        !isTriviallyDead(*Definition, Function.registerInfo(), Target))
      continue;
    auto Erased = eraseInstr(*Definition, Function.registerInfo(), Target);
    if (!Erased)
      return std::unexpected(Erased.error());
  }
  return {};
}

} // namespace

Expected<bool> GIMatchTableExecutor::match(const GIMatchTable &Table,
                                           const MachineInstr &Instruction,
                                           const MachineFunction &Function,
                                           const TargetDescription &Target) const {
  for (const GIMatchTableRule &Rule : Table.Rules) {
    auto Match = matchRule(Table, Rule, Instruction, Function);
    if (!Match)
      return std::unexpected(Match.error());
    if (!*Match)
      continue;
    if (Target.instructionDescriptor((*Match)->ResultOpcode) == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "generated match table selects an unknown instruction opcode");
    }
    return true;
  }
  return false;
}

Expected<bool> GIMatchTableExecutor::select(const GIMatchTable &Table, MachineInstr &Instruction,
                                            MachineFunction &Function,
                                            const TargetDescription &Target) const {
  for (const GIMatchTableRule &Rule : Table.Rules) {
    auto Match = matchRule(Table, Rule, Instruction, Function);
    if (!Match)
      return std::unexpected(Match.error());
    if (!*Match)
      continue;
    auto Applied = applyRule(**Match, Instruction, Function, Target);
    if (!Applied)
      return std::unexpected(Applied.error());
    return true;
  }
  return false;
}

} // namespace rcc
