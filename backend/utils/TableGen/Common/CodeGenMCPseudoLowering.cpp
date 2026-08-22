// SPDX-License-Identifier: GPL-3.0-or-later

#include "CodeGenTarget.h"

#include <algorithm>
#include <cstdint>
#include <ranges>
#include <string_view>
#include <unordered_set>

namespace rcc::targetgen {
namespace {

[[nodiscard]] const NormalizedInstruction *findInstruction(const NormalizedTargetModel &Model,
                                                           std::string_view Symbol) noexcept {
  const auto Position =
      std::ranges::find(Model.Instructions, Symbol, &NormalizedInstruction::Symbol);
  return Position == Model.Instructions.end() ? nullptr : &*Position;
}

[[nodiscard]] bool hasFlag(const NormalizedInstruction &Instruction,
                           std::string_view Flag) noexcept {
  return std::ranges::contains(Instruction.Flags, Flag);
}

[[nodiscard]] bool hasCompatibleKind(const NormalizedOperand &Source,
                                     const NormalizedOperand &Result) noexcept {
  return std::ranges::any_of(Source.Kinds, [&Result](const std::string &Kind) {
    return std::ranges::contains(Result.Kinds, Kind);
  });
}

} // namespace

bool CodeGenTarget::normalizeMCPseudoExpansions(NormalizedTargetModel &Result) {
  std::unordered_set<std::uint32_t> PseudoOpcodes;
  for (const SourceMCPseudoExpansion &Expansion : Input.MCPseudoExpansions) {
    const NormalizedInstruction *Pseudo = findInstruction(Result, Expansion.PseudoOpcodeSymbol);
    const NormalizedInstruction *ResultInstruction =
        findInstruction(Result, Expansion.ResultOpcodeSymbol);
    if (Pseudo == nullptr || ResultInstruction == nullptr ||
        !PseudoOpcodes.insert(Pseudo == nullptr ? 0 : Pseudo->ID).second) {
      fail(Expansion.Range,
           "MC pseudo expansions require a unique declared pseudo and result instruction");
      return false;
    }
    if (!hasFlag(*Pseudo, "Pseudo") || !hasFlag(*Pseudo, "TerminalEmission") ||
        Pseudo->Owner != "AsmPrinter" || Pseudo->Deadline != "Emission" ||
        hasFlag(*ResultInstruction, "Pseudo") || ResultInstruction->AssemblyMnemonic.empty()) {
      fail(Expansion.Range,
           "MC pseudo expansions require an AsmPrinter-owned terminal pseudo and encoded result");
      return false;
    }
    if (Expansion.Operands.size() != ResultInstruction->Operands.size()) {
      fail(Expansion.Range, "MC pseudo expansion operand count must match the result instruction");
      return false;
    }

    NormalizedMCPseudoExpansion Normalized{
        .PseudoOpcode = Pseudo->ID,
        .ResultOpcode = ResultInstruction->ID,
        .Operands = {},
    };
    Normalized.Operands.reserve(Expansion.Operands.size());
    for (std::size_t Index = 0; Index < Expansion.Operands.size(); ++Index) {
      const SourceMCPseudoOperand &Operand = Expansion.Operands[Index];
      const unsigned SourceCount = static_cast<unsigned>(Operand.SourceOperandIndex.has_value()) +
                                   static_cast<unsigned>(!Operand.RegisterSymbol.empty()) +
                                   static_cast<unsigned>(Operand.Immediate.has_value());
      if (SourceCount != 1) {
        fail(Operand.Range,
             "MC pseudo expansion operands require exactly one operand, register, or immediate");
        return false;
      }
      const NormalizedOperand &ResultOperand = ResultInstruction->Operands[Index];
      if (Operand.SourceOperandIndex) {
        if (*Operand.SourceOperandIndex >= Pseudo->Operands.size() ||
            !hasCompatibleKind(Pseudo->Operands[*Operand.SourceOperandIndex], ResultOperand)) {
          fail(Operand.Range,
               "MC pseudo source operand must exist and match the result operand kind");
          return false;
        }
        Normalized.Operands.push_back({
            .Kind = NormalizedMCPseudoOperandKind::SourceOperand,
            .Value = *Operand.SourceOperandIndex,
            .Immediate = 0,
        });
        continue;
      }
      if (!Operand.RegisterSymbol.empty()) {
        const auto Register = RegisterBySymbol.find(Operand.RegisterSymbol);
        if (Register == RegisterBySymbol.end() ||
            !std::ranges::contains(ResultOperand.Kinds, "Register")) {
          fail(Operand.Range,
               "MC pseudo fixed register must be declared and target a register operand");
          return false;
        }
        Normalized.Operands.push_back({
            .Kind = NormalizedMCPseudoOperandKind::Register,
            .Value = Register->second,
            .Immediate = 0,
        });
        continue;
      }
      if (!std::ranges::contains(ResultOperand.Kinds, "Immediate")) {
        fail(Operand.Range, "MC pseudo fixed immediate must target an immediate operand");
        return false;
      }
      Normalized.Operands.push_back({
          .Kind = NormalizedMCPseudoOperandKind::Immediate,
          .Value = 0,
          .Immediate = *Operand.Immediate,
      });
    }
    Result.MCPseudoExpansions.push_back(std::move(Normalized));
  }
  std::ranges::sort(Result.MCPseudoExpansions, {}, &NormalizedMCPseudoExpansion::PseudoOpcode);
  return true;
}

} // namespace rcc::targetgen
