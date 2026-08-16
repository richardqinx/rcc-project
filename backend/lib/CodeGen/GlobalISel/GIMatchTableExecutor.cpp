// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/GlobalISel/GIMatchTableExecutor.h"

#include "rcc/CodeGen/LowLevelType.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/TargetDescription.h"

#include <cstddef>
#include <optional>
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

struct MatchedRule final {
  Opcode ResultOpcode;
  std::vector<std::int64_t> AppendedImmediates;
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
    case GIMatchTableOpcode::MutateOpcode:
      if (Complete || HasResultOpcode || Step.Value == 0)
        return makeUnexpected(ErrorCode::VerificationError,
                              "generated match table has an invalid opcode mutation");
      Applying = true;
      Result.ResultOpcode = Opcode::fromValue(Step.Value);
      HasResultOpcode = true;
      break;
    case GIMatchTableOpcode::AppendImmediate:
      if (Complete)
        return makeUnexpected(ErrorCode::VerificationError,
                              "generated match table renders after completion");
      Applying = true;
      Result.AppendedImmediates.push_back(Step.Immediate);
      break;
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

[[nodiscard]] Expected<void> applyRule(const MatchedRule &Rule, MachineInstr &Instruction,
                                       const TargetDescription &Target) {
  const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Rule.ResultOpcode);
  if (Descriptor == nullptr)
    return makeUnexpected(ErrorCode::VerificationError,
                          "generated match table selects an unknown instruction opcode");
  if (!Descriptor->Variadic &&
      Descriptor->Operands.size() != Instruction.operandCount() + Rule.AppendedImmediates.size()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "generated match table result has an incompatible operand count");
  }

  const Opcode OriginalOpcode = Instruction.opcode();
  auto SetOpcode = Instruction.setOpcode(Rule.ResultOpcode);
  if (!SetOpcode)
    return std::unexpected(SetOpcode.error());
  const std::size_t OriginalOperandCount = Instruction.operandCount();
  for (const std::int64_t Immediate : Rule.AppendedImmediates) {
    auto Added = Instruction.addOperand(MachineOperand::immediate(Immediate));
    if (Added)
      continue;
    while (Instruction.operandCount() > OriginalOperandCount) {
      auto Removed = Instruction.eraseOperand(Instruction.operandCount() - 1U);
      if (!Removed)
        return std::unexpected(Removed.error());
    }
    auto Restored = Instruction.setOpcode(OriginalOpcode);
    if (!Restored)
      return std::unexpected(Restored.error());
    return std::unexpected(Added.error());
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
    auto Applied = applyRule(**Match, Instruction, Target);
    if (!Applied)
      return std::unexpected(Applied.error());
    return true;
  }
  return false;
}

} // namespace rcc
