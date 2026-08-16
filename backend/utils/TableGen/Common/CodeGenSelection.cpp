// SPDX-License-Identifier: GPL-3.0-or-later

#include "CodeGenTarget.h"
#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/LowLevelType.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace rcc::targetgen {
namespace {

[[nodiscard]] std::optional<std::uint32_t> genericOpcodeValue(std::string_view Symbol) {
#define RCC_CORE_OPCODE(Name)                                                                      \
  if (Symbol == #Name)                                                                             \
    return static_cast<std::uint32_t>(GenericOpcode::Name);
#define RCC_GENERIC_OPCODE(Name) RCC_CORE_OPCODE(Name)
#include "rcc/CodeGen/GenericOpcode.def"
#undef RCC_GENERIC_OPCODE
#undef RCC_CORE_OPCODE
  return std::nullopt;
}

[[nodiscard]] const NormalizedInstruction *
findInstructionBySymbol(const NormalizedTargetModel &Model, std::string_view Symbol) {
  const auto Position =
      std::ranges::find(Model.Instructions, Symbol, &NormalizedInstruction::Symbol);
  return Position == Model.Instructions.end() ? nullptr : &*Position;
}

[[nodiscard]] std::optional<std::uint32_t> resolveOpcode(const NormalizedTargetModel &Model,
                                                         std::string_view Symbol) {
  if (const NormalizedInstruction *Instruction = findInstructionBySymbol(Model, Symbol))
    return Instruction->ID;
  return genericOpcodeValue(Symbol);
}

[[nodiscard]] bool isRegisterOperand(const NormalizedInstruction &Instruction,
                                     std::uint32_t Index) {
  return Index < Instruction.Operands.size() &&
         std::ranges::contains(Instruction.Operands[Index].Kinds, "Register");
}

} // namespace

bool CodeGenTarget::normalizeSelectionRules(NormalizedTargetModel &Result) {
  std::unordered_set<std::string> Symbols;
  for (const SourceSelectionRule &Rule : Input.SelectionRules) {
    const auto MatchOpcode = resolveOpcode(Result, Rule.MatchOpcodeSymbol);
    const auto ResultOpcode = resolveOpcode(Result, Rule.ResultOpcodeSymbol);
    const NormalizedInstruction *MatchInstruction =
        findInstructionBySymbol(Result, Rule.MatchOpcodeSymbol);
    const NormalizedInstruction *ResultInstruction =
        findInstructionBySymbol(Result, Rule.ResultOpcodeSymbol);
    if (!isIdentifier(Rule.Symbol) || !Symbols.insert(Rule.Symbol).second || !MatchOpcode ||
        !ResultOpcode) {
      fail(Rule.Range,
           "selection rules require a unique identifier and declared match/result opcodes");
      return false;
    }
    if (MatchInstruction != nullptr && !std::ranges::contains(MatchInstruction->Flags, "Generic")) {
      fail(Rule.Range, "selection rule match opcode must be generic");
      return false;
    }
    if (isGenericOpcode(Opcode::fromValue(*ResultOpcode)) ||
        (ResultInstruction != nullptr &&
         std::ranges::contains(ResultInstruction->Flags, "Generic"))) {
      fail(Rule.Range, "selection rule result opcode must not be generic");
      return false;
    }
    if (MatchInstruction != nullptr && ResultInstruction != nullptr &&
        ResultInstruction->Operands.size() !=
            MatchInstruction->Operands.size() + Rule.AppendedImmediates.size()) {
      fail(Rule.Range, "selection rule result opcode has an incompatible operand count");
      return false;
    }

    const auto NormalizeType = [this](const SourceSelectionTypePredicate &Source)
        -> std::optional<NormalizedSelectionTypePredicate> {
      NormalizedSelectionTypeKind Kind = NormalizedSelectionTypeKind::Any;
      if (Source.Kind == "scalar")
        Kind = NormalizedSelectionTypeKind::Scalar;
      else if (Source.Kind == "pointer")
        Kind = NormalizedSelectionTypeKind::Pointer;
      else if (Source.Kind != "any") {
        fail(Source.Range, "selection type kind must be 'any', 'scalar', or 'pointer'");
        return std::nullopt;
      }
      if (Source.Bits > LowLevelType::MaxBitWidth ||
          (Kind != NormalizedSelectionTypeKind::Any && Source.Bits == 0)) {
        fail(Source.Range, "selection type bit width is invalid for its kind");
        return std::nullopt;
      }
      return NormalizedSelectionTypePredicate{.Kind = Kind, .Bits = Source.Bits};
    };

    NormalizedSelectionRule Normalized;
    Normalized.Symbol = Rule.Symbol;
    Normalized.Priority = Rule.Priority;
    Normalized.Specificity = 1;
    Normalized.MatchOpcode = *MatchOpcode;
    Normalized.ResultOpcode = *ResultOpcode;
    Normalized.AppendedImmediates = Rule.AppendedImmediates;
    if (Rule.ResultType) {
      auto Type = NormalizeType(*Rule.ResultType);
      if (!Type)
        return false;
      if (MatchInstruction != nullptr && !isRegisterOperand(*MatchInstruction, 0)) {
        fail(Rule.ResultType->Range,
             "selection result type predicate requires register operand zero");
        return false;
      }
      Normalized.ResultType = *Type;
      ++Normalized.Specificity;
    }

    std::unordered_set<std::uint32_t> TypeOperands;
    for (const SourceSelectionOperandTypePredicate &Predicate : Rule.OperandTypes) {
      auto Type = NormalizeType(Predicate.Type);
      if (!Type)
        return false;
      if (!TypeOperands.insert(Predicate.OperandIndex).second ||
          (MatchInstruction != nullptr &&
           !isRegisterOperand(*MatchInstruction, Predicate.OperandIndex))) {
        fail(Predicate.Range,
             "selection operand type predicates require distinct register operands");
        return false;
      }
      Normalized.OperandTypes.push_back({.OperandIndex = Predicate.OperandIndex, .Type = *Type});
      ++Normalized.Specificity;
    }

    std::unordered_set<std::uint32_t> BankOperands;
    for (const SourceSelectionOperandBankPredicate &Predicate : Rule.OperandBanks) {
      const auto Bank = BankBySymbol.find(Predicate.BankSymbol);
      if (!BankOperands.insert(Predicate.OperandIndex).second || Bank == BankBySymbol.end() ||
          (MatchInstruction != nullptr &&
           !isRegisterOperand(*MatchInstruction, Predicate.OperandIndex))) {
        fail(Predicate.Range,
             "selection operand bank predicates require distinct register operands and declared "
             "banks");
        return false;
      }
      Normalized.OperandBanks.push_back(
          {.OperandIndex = Predicate.OperandIndex, .Bank = Bank->second});
      ++Normalized.Specificity;
    }

    std::unordered_set<std::uint32_t> ClassOperands;
    for (const SourceSelectionOperandClassPredicate &Predicate : Rule.OperandClasses) {
      const auto RegClass = ClassBySymbol.find(Predicate.ClassSymbol);
      if (!ClassOperands.insert(Predicate.OperandIndex).second || RegClass == ClassBySymbol.end() ||
          (MatchInstruction != nullptr &&
           !isRegisterOperand(*MatchInstruction, Predicate.OperandIndex))) {
        fail(Predicate.Range,
             "selection operand class predicates require distinct register operands and declared "
             "classes");
        return false;
      }
      Normalized.OperandClasses.push_back(
          {.OperandIndex = Predicate.OperandIndex, .RegClass = RegClass->second});
      ++Normalized.Specificity;
    }
    Result.SelectionRules.push_back(std::move(Normalized));
  }

  std::ranges::sort(Result.SelectionRules,
                    [](const NormalizedSelectionRule &Left, const NormalizedSelectionRule &Right) {
                      if (Left.Priority != Right.Priority)
                        return Left.Priority > Right.Priority;
                      if (Left.Specificity != Right.Specificity)
                        return Left.Specificity > Right.Specificity;
                      return Left.Symbol < Right.Symbol;
                    });
  return true;
}

} // namespace rcc::targetgen
