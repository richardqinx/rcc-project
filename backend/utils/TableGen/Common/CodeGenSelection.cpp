// SPDX-License-Identifier: GPL-3.0-or-later

#include "CodeGenTarget.h"
#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/LowLevelType.h"
#include "rcc/CodeGen/MachineOperand.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
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

[[nodiscard]] std::optional<std::uint32_t> integerPredicateValue(std::string_view Symbol) noexcept {
  static constexpr std::array<std::pair<std::string_view, IntegerPredicate>, 2> Predicates = {{
      {"eq", IntegerPredicate::Equal},
      {"ne", IntegerPredicate::NotEqual},
  }};
  if (Symbol == "sgt")
    return static_cast<std::uint32_t>(IntegerPredicate::SignedGreaterThan);
  if (Symbol == "sge")
    return static_cast<std::uint32_t>(IntegerPredicate::SignedGreaterEqual);
  if (Symbol == "sle")
    return static_cast<std::uint32_t>(IntegerPredicate::SignedLessEqual);
  if (Symbol == "slt")
    return static_cast<std::uint32_t>(IntegerPredicate::SignedLessThan);
  if (Symbol == "ugt")
    return static_cast<std::uint32_t>(IntegerPredicate::UnsignedGreaterThan);
  if (Symbol == "uge")
    return static_cast<std::uint32_t>(IntegerPredicate::UnsignedGreaterEqual);
  if (Symbol == "ult")
    return static_cast<std::uint32_t>(IntegerPredicate::UnsignedLessThan);
  if (Symbol == "ule")
    return static_cast<std::uint32_t>(IntegerPredicate::UnsignedLessEqual);
  const auto *const Position =
      std::ranges::find(Predicates, Symbol, &decltype(Predicates)::value_type::first);
  if (Position == Predicates.end())
    return std::nullopt;
  return static_cast<std::uint32_t>(Position->second);
}

[[nodiscard]] const NormalizedRegister *findRegisterBySymbol(const NormalizedTargetModel &Model,
                                                             std::string_view Symbol) {
  const auto Position = std::ranges::find(Model.Registers, Symbol, &NormalizedRegister::Symbol);
  return Position == Model.Registers.end() ? nullptr : &*Position;
}

[[nodiscard]] std::optional<RegisterOperandFlags>
registerOperandFlags(std::span<const std::string> Names) noexcept {
  RegisterOperandFlags Flags;
  for (const std::string &Name : Names) {
    const auto Set = [&Flags](RegisterOperandFlag Flag) {
      Flags.set(Flag);
      return true;
    };
    if (Name == "define")
      Set(RegisterOperandFlag::Define);
    else if (Name == "use")
      Set(RegisterOperandFlag::Use);
    else if (Name == "implicit")
      Set(RegisterOperandFlag::Implicit);
    else if (Name == "kill")
      Set(RegisterOperandFlag::Kill);
    else if (Name == "dead")
      Set(RegisterOperandFlag::Dead);
    else if (Name == "undef")
      Set(RegisterOperandFlag::Undef);
    else if (Name == "early_clobber")
      Set(RegisterOperandFlag::EarlyClobber);
    else if (Name == "internal_read")
      Set(RegisterOperandFlag::InternalRead);
    else if (Name == "renamable")
      Set(RegisterOperandFlag::Renamable);
    else if (Name == "debug")
      Set(RegisterOperandFlag::Debug);
    else
      return std::nullopt;
  }
  return Flags;
}

[[nodiscard]] bool validRegisterOperandFlags(RegisterOperandFlags Flags) noexcept {
  const bool IsDef = Flags.has(RegisterOperandFlag::Define);
  const bool IsUse = Flags.has(RegisterOperandFlag::Use);
  return (IsDef || IsUse) && (!Flags.has(RegisterOperandFlag::Kill) || IsUse) &&
         (!Flags.has(RegisterOperandFlag::Dead) || IsDef) &&
         (!Flags.has(RegisterOperandFlag::Undef) || IsUse) &&
         (!Flags.has(RegisterOperandFlag::EarlyClobber) || IsDef) &&
         (!Flags.has(RegisterOperandFlag::InternalRead) || (IsDef && IsUse)) &&
         (!(IsDef && IsUse) || Flags.has(RegisterOperandFlag::InternalRead)) &&
         (!Flags.has(RegisterOperandFlag::Debug) || (IsUse && !IsDef));
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
    if (!Rule.OperandRenderers && MatchInstruction != nullptr && ResultInstruction != nullptr &&
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
    Normalized.EraseOperandDefinitions = Rule.EraseOperandDefinitions;
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

    if (Rule.IntegerPredicate) {
      const auto Predicate = integerPredicateValue(Rule.IntegerPredicate->Predicate);
      if (!Predicate) {
        fail(Rule.IntegerPredicate->Range,
             "selection integer predicate is not a recognized integer predicate");
        return false;
      }
      if (MatchInstruction != nullptr &&
          (Rule.IntegerPredicate->OperandIndex >= MatchInstruction->Operands.size() ||
           !std::ranges::contains(
               MatchInstruction->Operands[Rule.IntegerPredicate->OperandIndex].Kinds,
               "Predicate"))) {
        fail(Rule.IntegerPredicate->Range,
             "selection integer predicate requires a predicate operand");
        return false;
      }
      Normalized.IntegerPredicate = {
          .OperandIndex = Rule.IntegerPredicate->OperandIndex,
          .Predicate = *Predicate,
      };
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

    std::unordered_set<std::uint32_t> ConstantOperands;
    for (const SourceSelectionOperandDefConstantPredicate &Predicate : Rule.OperandDefConstants) {
      if (!ConstantOperands.insert(Predicate.OperandIndex).second ||
          (MatchInstruction != nullptr &&
           !isRegisterOperand(*MatchInstruction, Predicate.OperandIndex))) {
        fail(Predicate.Range,
             "selection operand definition constant predicates require distinct register "
             "operands");
        return false;
      }
      Normalized.OperandDefConstants.push_back(
          {.OperandIndex = Predicate.OperandIndex, .Value = Predicate.Value});
      ++Normalized.Specificity;
    }

    if (Rule.OperandRenderers) {
      if (!Rule.AppendedImmediates.empty()) {
        fail(Rule.Range, "selection rule cannot combine operand renderers and appended immediates");
        return false;
      }
      std::vector<NormalizedSelectionOperandRenderer> Renderers;
      Renderers.reserve(Rule.OperandRenderers->size());
      for (const SourceSelectionOperandRenderer &Renderer : *Rule.OperandRenderers) {
        if (Renderer.OperandIndex) {
          if (MatchInstruction != nullptr &&
              *Renderer.OperandIndex >= MatchInstruction->Operands.size()) {
            fail(Renderer.Range, "selection operand renderer references an unknown operand");
            return false;
          }
          Renderers.push_back({
              .SourceKind = NormalizedSelectionOperandRenderer::Kind::Operand,
              .Value = *Renderer.OperandIndex,
              .Flags = 0,
          });
          continue;
        }

        const NormalizedRegister *Physical =
            findRegisterBySymbol(Result, Renderer.PhysicalRegisterSymbol);
        const auto Flags = registerOperandFlags(Renderer.Flags);
        if (Physical == nullptr || Physical->ID == 0 || !Flags) {
          fail(Renderer.Range,
               "selection physical register renderer requires a declared register and valid "
               "register flags");
          return false;
        }
        if (!validRegisterOperandFlags(*Flags)) {
          fail(Renderer.Range, "selection physical register renderer is invalid");
          return false;
        }
        Renderers.push_back({
            .SourceKind = NormalizedSelectionOperandRenderer::Kind::PhysicalRegister,
            .Value = Physical->ID,
            .Flags = Flags->rawValue(),
        });
      }
      if (ResultInstruction != nullptr && ResultInstruction->Operands.size() != Renderers.size()) {
        fail(Rule.Range, "selection operand renderer has an incompatible operand count");
        return false;
      }
      Normalized.OperandRenderers = std::move(Renderers);
    } else if (ResultInstruction != nullptr && MatchInstruction != nullptr &&
               ResultInstruction->Operands.size() !=
                   MatchInstruction->Operands.size() + Rule.AppendedImmediates.size()) {
      fail(Rule.Range, "selection rule result opcode has an incompatible operand count");
      return false;
    }

    std::unordered_set<std::uint32_t> ErasedOperands;
    for (const std::uint32_t OperandIndex : Rule.EraseOperandDefinitions) {
      if (!ErasedOperands.insert(OperandIndex).second ||
          (MatchInstruction != nullptr && !isRegisterOperand(*MatchInstruction, OperandIndex))) {
        fail(Rule.Range, "selection erased definitions require distinct register operands");
        return false;
      }
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
