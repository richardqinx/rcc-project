// SPDX-License-Identifier: GPL-3.0-or-later

#include "CodeGenTarget.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace rcc::targetgen {
namespace {

[[nodiscard]] const NormalizedInstruction *findInstruction(const NormalizedTargetModel &Model,
                                                           std::string_view Symbol) noexcept {
  const auto Position =
      std::ranges::find(Model.Instructions, Symbol, &NormalizedInstruction::Symbol);
  return Position == Model.Instructions.end() ? nullptr : &*Position;
}

[[nodiscard]] bool isOperandKind(const NormalizedInstruction &Instruction,
                                 std::uint32_t OperandIndex, std::string_view Kind) noexcept {
  return OperandIndex < Instruction.Operands.size() &&
         std::ranges::contains(Instruction.Operands[OperandIndex].Kinds, Kind);
}

[[nodiscard]] std::optional<NormalizedMacroFusionInstruction>
normalizeInstructionSide(std::string_view Side) noexcept {
  if (Side == "first")
    return NormalizedMacroFusionInstruction::First;
  if (Side == "second")
    return NormalizedMacroFusionInstruction::Second;
  return std::nullopt;
}

} // namespace

bool CodeGenTarget::normalizeMacroFusionRules(NormalizedTargetModel &Result) {
  std::unordered_set<std::string> Symbols;
  std::unordered_set<std::string> Features;
  for (const SourceMacroFusionRule &Rule : Input.MacroFusionRules) {
    if (!isIdentifier(Rule.Symbol) || Rule.Feature.empty() || Rule.FirstOpcodeSymbols.empty() ||
        Rule.SecondOpcodeSymbols.empty() || !Symbols.insert(Rule.Symbol).second ||
        !Features.insert(Rule.Feature).second) {
      fail(Rule.Range,
           "macro fusion rules require unique identifiers and features plus nonempty opcode sets");
      return false;
    }

    NormalizedMacroFusionRule Normalized{
        .Symbol = Rule.Symbol,
        .Feature = Rule.Feature,
        .FirstOpcodes = {},
        .SecondOpcodes = {},
        .OpcodePairs = {},
        .ImmediatePredicates = {},
        .RegisterInequalities = {},
        .ImmediateRelations = {},
        .FirstDefinitionOperand = Rule.FirstDefinitionOperand,
        .SecondUseOperand = Rule.SecondUseOperand,
        .SecondDefinitionOperand = Rule.SecondDefinitionOperand,
        .CommutableSecondUseOperand = Rule.CommutableSecondUseOperand,
        .RequireSingleNonDebugUse = Rule.RequireSingleNonDebugUse,
        .RequireSecondDestinationTie = Rule.RequireSecondDestinationTie,
    };
    std::unordered_set<std::uint32_t> FirstOpcodes;
    std::unordered_set<std::uint32_t> SecondOpcodes;
    std::vector<const NormalizedInstruction *> FirstInstructions;
    std::vector<const NormalizedInstruction *> SecondInstructions;
    const auto NormalizeOpcodes =
        [this, &Result](const std::vector<std::string> &OpcodeSymbols,
                        std::unordered_set<std::uint32_t> &Opcodes,
                        std::vector<std::uint32_t> &Output,
                        std::vector<const NormalizedInstruction *> &Instructions, TextRange Range) {
          for (const std::string &Symbol : OpcodeSymbols) {
            const NormalizedInstruction *Instruction = findInstruction(Result, Symbol);
            if (Instruction == nullptr || !Opcodes.insert(Instruction->ID).second) {
              fail(Range, "macro fusion opcodes must name distinct declared target instructions");
              return false;
            }
            Output.push_back(Instruction->ID);
            Instructions.push_back(Instruction);
          }
          return true;
        };
    if (!NormalizeOpcodes(Rule.FirstOpcodeSymbols, FirstOpcodes, Normalized.FirstOpcodes,
                          FirstInstructions, Rule.Range) ||
        !NormalizeOpcodes(Rule.SecondOpcodeSymbols, SecondOpcodes, Normalized.SecondOpcodes,
                          SecondInstructions, Rule.Range)) {
      return false;
    }

    const auto AllHaveOperandKind = [](const auto &Instructions, std::uint32_t Operand,
                                       std::string_view Kind) {
      return std::ranges::all_of(Instructions, [Operand, Kind](const auto *Instruction) {
        return isOperandKind(*Instruction, Operand, Kind);
      });
    };
    if (!AllHaveOperandKind(FirstInstructions, Rule.FirstDefinitionOperand, "Register") ||
        !AllHaveOperandKind(SecondInstructions, Rule.SecondUseOperand, "Register") ||
        (Rule.RequireSecondDestinationTie &&
         !AllHaveOperandKind(SecondInstructions, Rule.SecondDefinitionOperand, "Register")) ||
        (Rule.CommutableSecondUseOperand &&
         !AllHaveOperandKind(SecondInstructions, *Rule.CommutableSecondUseOperand, "Register"))) {
      fail(Rule.Range, "macro fusion linkage must reference register operands of every opcode");
      return false;
    }

    std::unordered_set<std::uint64_t> OpcodePairs;
    for (const SourceMacroFusionOpcodePair &Pair : Rule.OpcodePairs) {
      const NormalizedInstruction *First = findInstruction(Result, Pair.FirstOpcodeSymbol);
      const NormalizedInstruction *Second = findInstruction(Result, Pair.SecondOpcodeSymbol);
      if (First == nullptr || Second == nullptr || !FirstOpcodes.contains(First->ID) ||
          !SecondOpcodes.contains(Second->ID)) {
        fail(Pair.Range, "macro fusion opcode pair must belong to the rule opcode sets");
        return false;
      }
      const std::uint64_t Key = (static_cast<std::uint64_t>(First->ID) << 32U) | Second->ID;
      if (!OpcodePairs.insert(Key).second) {
        fail(Pair.Range, "macro fusion opcode pairs must be distinct");
        return false;
      }
      Normalized.OpcodePairs.push_back({First->ID, Second->ID});
    }

    for (const SourceMacroFusionImmediatePredicate &Predicate : Rule.ImmediatePredicates) {
      const auto Side = normalizeInstructionSide(Predicate.Instruction);
      const auto &Instructions =
          Side == NormalizedMacroFusionInstruction::First ? FirstInstructions : SecondInstructions;
      if (!Side || Predicate.Minimum > Predicate.Maximum ||
          !AllHaveOperandKind(Instructions, Predicate.OperandIndex, "Immediate")) {
        fail(
            Predicate.Range,
            "macro fusion immediate predicate requires a valid side, range, and immediate operand");
        return false;
      }
      Normalized.ImmediatePredicates.push_back(
          {*Side, Predicate.OperandIndex, Predicate.Minimum, Predicate.Maximum});
    }

    for (const SourceMacroFusionRegisterInequality &Predicate : Rule.RegisterInequalities) {
      const auto Side = normalizeInstructionSide(Predicate.Instruction);
      const auto &Instructions =
          Side == NormalizedMacroFusionInstruction::First ? FirstInstructions : SecondInstructions;
      if (!Side || Predicate.LeftOperand == Predicate.RightOperand ||
          !AllHaveOperandKind(Instructions, Predicate.LeftOperand, "Register") ||
          !AllHaveOperandKind(Instructions, Predicate.RightOperand, "Register")) {
        fail(
            Predicate.Range,
            "macro fusion register inequality requires distinct register operands on a valid side");
        return false;
      }
      Normalized.RegisterInequalities.push_back(
          {*Side, Predicate.LeftOperand, Predicate.RightOperand});
    }

    for (const SourceMacroFusionImmediateRelation &Relation : Rule.ImmediateRelations) {
      if (Relation.Kind != "less_equal" ||
          !AllHaveOperandKind(FirstInstructions, Relation.FirstOperand, "Immediate") ||
          !AllHaveOperandKind(SecondInstructions, Relation.SecondOperand, "Immediate")) {
        fail(Relation.Range,
             "macro fusion immediate relation must be less_equal over immediate operands");
        return false;
      }
      Normalized.ImmediateRelations.push_back({Relation.FirstOperand, Relation.SecondOperand});
    }
    Result.MacroFusionRules.push_back(std::move(Normalized));
  }
  return true;
}

} // namespace rcc::targetgen
