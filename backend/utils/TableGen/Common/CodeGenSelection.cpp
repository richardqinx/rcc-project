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
#include <string>
#include <string_view>
#include <unordered_map>
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

[[nodiscard]] std::optional<std::uint32_t>
floatingPredicateValue(std::string_view Symbol) noexcept {
  static constexpr std::array<std::pair<std::string_view, FloatingPredicate>, 16> Predicates = {{
      {"false", FloatingPredicate::AlwaysFalse},
      {"oeq", FloatingPredicate::OrderedEqual},
      {"ogt", FloatingPredicate::OrderedGreaterThan},
      {"oge", FloatingPredicate::OrderedGreaterEqual},
      {"olt", FloatingPredicate::OrderedLessThan},
      {"ole", FloatingPredicate::OrderedLessEqual},
      {"one", FloatingPredicate::OrderedNotEqual},
      {"ord", FloatingPredicate::Ordered},
      {"uno", FloatingPredicate::Unordered},
      {"ueq", FloatingPredicate::UnorderedEqual},
      {"ugt", FloatingPredicate::UnorderedGreaterThan},
      {"uge", FloatingPredicate::UnorderedGreaterEqual},
      {"ult", FloatingPredicate::UnorderedLessThan},
      {"ule", FloatingPredicate::UnorderedLessEqual},
      {"une", FloatingPredicate::UnorderedNotEqual},
      {"true", FloatingPredicate::AlwaysTrue},
  }};
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
    const NormalizedInstruction *MatchInstruction =
        findInstructionBySymbol(Result, Rule.MatchOpcodeSymbol);
    if (!isIdentifier(Rule.Symbol) || !Symbols.insert(Rule.Symbol).second || !MatchOpcode) {
      fail(Rule.Range, "selection rules require a unique identifier and declared match opcode");
      return false;
    }
    if (MatchInstruction != nullptr && !std::ranges::contains(MatchInstruction->Flags, "Generic")) {
      fail(Rule.Range, "selection rule match opcode must be generic");
      return false;
    }
    if (Rule.Actions.empty()) {
      fail(Rule.Range, "selection rule requires at least one generated instruction");
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
    const auto NormalizeRanges = [this](const std::vector<SourceSelectionConstantRange> &Ranges,
                                        TextRange PredicateRange, std::string_view PredicateName)
        -> std::optional<std::vector<NormalizedSelectionConstantRange>> {
      if (Ranges.empty()) {
        fail(PredicateRange, std::string(PredicateName) + " requires at least one range");
        return std::nullopt;
      }
      std::vector<NormalizedSelectionConstantRange> NormalizedRanges;
      NormalizedRanges.reserve(Ranges.size());
      for (const SourceSelectionConstantRange &Range : Ranges) {
        if (Range.Minimum > Range.Maximum) {
          fail(Range.SourceRange, std::string(PredicateName) + " range is invalid");
          return std::nullopt;
        }
        NormalizedRanges.push_back({
            .Minimum = Range.Minimum,
            .Maximum = Range.Maximum,
        });
      }
      std::ranges::sort(NormalizedRanges, {}, &NormalizedSelectionConstantRange::Minimum);
      for (std::size_t Index = 1; Index < NormalizedRanges.size(); ++Index) {
        const auto &Previous = NormalizedRanges[Index - 1];
        const auto &Current = NormalizedRanges[Index];
        if (Current.Minimum <= Previous.Maximum) {
          fail(PredicateRange, std::string(PredicateName) + " ranges overlap");
          return std::nullopt;
        }
      }
      return NormalizedRanges;
    };

    NormalizedSelectionRule Normalized;
    Normalized.Symbol = Rule.Symbol;
    Normalized.Priority = Rule.Priority;
    Normalized.Specificity = 1;
    Normalized.MatchOpcode = *MatchOpcode;
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

    if (Rule.IntegerPredicate && Rule.FloatingPredicate) {
      fail(Rule.Range, "selection rule cannot require both integer and floating predicates");
      return false;
    }
    if (Rule.FloatingPredicate) {
      const auto Predicate = floatingPredicateValue(Rule.FloatingPredicate->Predicate);
      if (!Predicate) {
        fail(Rule.FloatingPredicate->Range,
             "selection floating predicate is not a recognized floating predicate");
        return false;
      }
      if (MatchInstruction != nullptr &&
          (Rule.FloatingPredicate->OperandIndex >= MatchInstruction->Operands.size() ||
           !std::ranges::contains(
               MatchInstruction->Operands[Rule.FloatingPredicate->OperandIndex].Kinds,
               "Predicate"))) {
        fail(Rule.FloatingPredicate->Range,
             "selection floating predicate requires a predicate operand");
        return false;
      }
      Normalized.FloatingPredicate = {
          .OperandIndex = Rule.FloatingPredicate->OperandIndex,
          .Predicate = *Predicate,
      };
      ++Normalized.Specificity;
    }

    if (Rule.MemoryPredicate) {
      if (Rule.MemoryPredicate->Bytes == 0 ||
          (MatchInstruction != nullptr && !MatchInstruction->RequiresMemoryOperands)) {
        fail(Rule.MemoryPredicate->Range,
             "selection memory predicate requires a nonzero size and a memory opcode");
        return false;
      }
      Normalized.MemoryPredicate = {
          .MemoryOperandIndex = Rule.MemoryPredicate->MemoryOperandIndex,
          .Bytes = Rule.MemoryPredicate->Bytes,
      };
      ++Normalized.Specificity;
    }

    std::unordered_set<std::string> TargetPredicateSymbols;
    for (const SourceSelectionTargetPredicate &Predicate : Rule.TargetPredicates) {
      if (!isIdentifier(Predicate.Symbol) ||
          !TargetPredicateSymbols.insert(Predicate.Symbol).second) {
        fail(Predicate.Range, "selection target predicates require distinct identifiers");
        return false;
      }
      Normalized.TargetPredicates.push_back(Predicate.Symbol);
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

    std::unordered_set<std::uint32_t> ImmediateOperands;
    for (const SourceSelectionOperandImmediatePredicate &Predicate : Rule.OperandImmediates) {
      if (!ImmediateOperands.insert(Predicate.OperandIndex).second) {
        fail(Predicate.Range, "selection operand immediate predicates require distinct operands");
        return false;
      }
      auto Ranges = NormalizeRanges(Predicate.Ranges, Predicate.Range,
                                    "selection operand immediate predicate");
      if (!Ranges)
        return false;
      Normalized.OperandImmediates.push_back({
          .OperandIndex = Predicate.OperandIndex,
          .Ranges = std::move(*Ranges),
      });
      ++Normalized.Specificity;
    }

    std::unordered_set<std::uint32_t> ConstantOperands;
    for (const SourceSelectionOperandDefConstantPredicate &Predicate : Rule.OperandDefConstants) {
      if (!ConstantOperands.insert(Predicate.OperandIndex).second ||
          (MatchInstruction != nullptr &&
           !isRegisterOperand(*MatchInstruction, Predicate.OperandIndex))) {
        fail(Predicate.Range,
             "selection operand definition constant predicates require distinct register operands");
        return false;
      }
      auto Ranges = NormalizeRanges(Predicate.Ranges, Predicate.Range,
                                    "selection operand definition constant predicate");
      if (!Ranges)
        return false;
      Normalized.OperandDefConstants.push_back({
          .OperandIndex = Predicate.OperandIndex,
          .Ranges = std::move(*Ranges),
      });
      ++Normalized.Specificity;
    }

    std::unordered_map<std::string, std::uint32_t> CaptureIDs;
    std::unordered_set<std::uint32_t> DefUseRootOperands;
    for (const SourceSelectionDefUsePredicate &Predicate : Rule.DefUsePredicates) {
      const auto DefOpcode = resolveOpcode(Result, Predicate.OpcodeSymbol);
      if (!DefOpcode || !isGenericOpcode(Opcode::fromValue(*DefOpcode)) ||
          !DefUseRootOperands.insert(Predicate.RootOperandIndex).second ||
          (MatchInstruction != nullptr &&
           !isRegisterOperand(*MatchInstruction, Predicate.RootOperandIndex))) {
        fail(Predicate.Range,
             "selection def-use predicates require distinct register operands and generic opcodes");
        return false;
      }

      NormalizedSelectionDefUsePredicate NormalizedPredicate{
          .RootOperandIndex = Predicate.RootOperandIndex,
          .Opcode = *DefOpcode,
          .IntegerPredicate = std::nullopt,
          .Captures = {},
          .OperandDefConstants = {},
      };
      if (Predicate.IntegerPredicate) {
        const auto Integer = integerPredicateValue(Predicate.IntegerPredicate->Predicate);
        if (!Integer) {
          fail(Predicate.IntegerPredicate->Range,
               "selection def-use integer predicate is not a recognized integer predicate");
          return false;
        }
        NormalizedPredicate.IntegerPredicate = {
            .OperandIndex = Predicate.IntegerPredicate->OperandIndex,
            .Predicate = *Integer,
        };
      }
      std::unordered_set<std::uint32_t> CapturedOperands;
      for (const SourceSelectionDefUseCapture &Capture : Predicate.Captures) {
        if (!isIdentifier(Capture.Symbol) ||
            !CapturedOperands.insert(Capture.OperandIndex).second ||
            !CaptureIDs.emplace(Capture.Symbol, CaptureIDs.size() + 1U).second) {
          fail(Capture.Range,
               "selection def-use captures require unique identifiers and distinct operands");
          return false;
        }
        NormalizedPredicate.Captures.push_back({
            .ID = CaptureIDs.at(Capture.Symbol),
            .OperandIndex = Capture.OperandIndex,
        });
      }

      std::unordered_set<std::uint32_t> ConstantOperandsInDef;
      for (const SourceSelectionOperandDefConstantPredicate &Constant :
           Predicate.OperandDefConstants) {
        if (!ConstantOperandsInDef.insert(Constant.OperandIndex).second) {
          fail(Constant.Range, "selection def-use constant predicates require distinct operands");
          return false;
        }
        auto Ranges = NormalizeRanges(Constant.Ranges, Constant.Range,
                                      "selection def-use constant predicate");
        if (!Ranges)
          return false;
        NormalizedPredicate.OperandDefConstants.push_back({
            .OperandIndex = Constant.OperandIndex,
            .Ranges = std::move(*Ranges),
        });
      }
      Normalized.DefUsePredicates.push_back(std::move(NormalizedPredicate));
      Normalized.Specificity += 1U +
                                static_cast<std::uint32_t>(Predicate.IntegerPredicate.has_value()) +
                                static_cast<std::uint32_t>(Predicate.Captures.size()) +
                                static_cast<std::uint32_t>(Predicate.OperandDefConstants.size());
    }

    std::unordered_map<std::string, std::uint32_t> TemporaryIDs;
    for (const SourceSelectionTemporary &Temporary : Rule.Temporaries) {
      if (!isIdentifier(Temporary.Symbol) ||
          !TemporaryIDs.emplace(Temporary.Symbol, TemporaryIDs.size() + 1U).second ||
          (MatchInstruction != nullptr &&
           !isRegisterOperand(*MatchInstruction, Temporary.TypeFromOperand))) {
        fail(Temporary.Range,
             "selection temporary requires a unique identifier and a register type source");
        return false;
      }
      Normalized.Temporaries.push_back({
          .ID = TemporaryIDs.at(Temporary.Symbol),
          .TypeFromOperand = Temporary.TypeFromOperand,
      });
    }

    bool HasRootAction = false;
    for (std::size_t ActionIndex = 0; ActionIndex < Rule.Actions.size(); ++ActionIndex) {
      const SourceSelectionAction &Action = Rule.Actions[ActionIndex];
      const auto ResultOpcode = resolveOpcode(Result, Action.ResultOpcodeSymbol);
      const NormalizedInstruction *ResultInstruction =
          findInstructionBySymbol(Result, Action.ResultOpcodeSymbol);
      const bool IsCorePseudo =
          ResultOpcode && isCorePseudoOpcode(Opcode::fromValue(*ResultOpcode));
      if (!ResultOpcode || (ResultInstruction == nullptr && !IsCorePseudo) ||
          (ResultInstruction != nullptr &&
           (isGenericOpcode(Opcode::fromValue(*ResultOpcode)) ||
            std::ranges::contains(ResultInstruction->Flags, "Generic")))) {
        fail(Action.Range,
             "selection instruction opcode must name a declared target instruction or core pseudo");
        return false;
      }
      if (Action.ReplacesRoot == HasRootAction || (Action.ReplacesRoot && ActionIndex != 0)) {
        fail(Action.Range,
             "selection instructions require exactly one root replacement as their first action");
        return false;
      }
      HasRootAction |= Action.ReplacesRoot;
      if (ResultInstruction != nullptr &&
          ResultInstruction->Operands.size() != Action.OperandRenderers.size()) {
        fail(Action.Range, "selection instruction has an incompatible operand count");
        return false;
      }

      NormalizedSelectionAction NormalizedAction{
          .ResultOpcode = *ResultOpcode,
          .ReplacesRoot = Action.ReplacesRoot,
          .OperandRenderers = {},
      };
      NormalizedAction.OperandRenderers.reserve(Action.OperandRenderers.size());
      for (const SourceSelectionOperandRenderer &Renderer : Action.OperandRenderers) {
        if (Renderer.ComplexOperandIndex) {
          if (Renderer.ComplexRendererSymbol.empty() || !Renderer.Flags.empty() ||
              Renderer.ImmediateTransform != "identity") {
            fail(Renderer.Range,
                 "selection complex operand renderer requires a matcher and no flags or transform");
            return false;
          }
          NormalizedAction.OperandRenderers.push_back({
              .SourceKind = NormalizedSelectionOperandRenderer::Kind::ComplexOperand,
              .Value = *Renderer.ComplexOperandIndex,
              .ComplexRenderer = Renderer.ComplexRendererSymbol,
              .Flags = 0,
          });
          continue;
        }

        if (Renderer.OperandIndex) {
          if (MatchInstruction != nullptr &&
              *Renderer.OperandIndex >= MatchInstruction->Operands.size()) {
            fail(Renderer.Range, "selection operand renderer references an unknown operand");
            return false;
          }
          if (!Renderer.Flags.empty()) {
            fail(Renderer.Range, "selection operand renderer cannot replace source register flags");
            return false;
          }
          NormalizedAction.OperandRenderers.push_back({
              .SourceKind = NormalizedSelectionOperandRenderer::Kind::Operand,
              .Value = *Renderer.OperandIndex,
              .ComplexRenderer = {},
              .Flags = 0,
          });
          continue;
        }

        if (!Renderer.PhysicalRegisterSymbol.empty()) {
          const NormalizedRegister *Physical =
              findRegisterBySymbol(Result, Renderer.PhysicalRegisterSymbol);
          const auto Flags = registerOperandFlags(Renderer.Flags);
          if (Physical == nullptr || Physical->ID == 0 || !Flags ||
              !validRegisterOperandFlags(*Flags)) {
            fail(Renderer.Range,
                 "selection physical register renderer requires a declared register and valid "
                 "register flags");
            return false;
          }
          NormalizedAction.OperandRenderers.push_back({
              .SourceKind = NormalizedSelectionOperandRenderer::Kind::PhysicalRegister,
              .Value = Physical->ID,
              .ComplexRenderer = {},
              .Flags = Flags->rawValue(),
          });
          continue;
        }

        if (!Renderer.TemporarySymbol.empty()) {
          const auto Temporary = TemporaryIDs.find(Renderer.TemporarySymbol);
          const auto Flags = registerOperandFlags(Renderer.Flags);
          if (Temporary == TemporaryIDs.end() || !Flags || !validRegisterOperandFlags(*Flags)) {
            fail(Renderer.Range,
                 "selection temporary renderer requires a declared temporary and valid register "
                 "flags");
            return false;
          }
          NormalizedAction.OperandRenderers.push_back({
              .SourceKind = NormalizedSelectionOperandRenderer::Kind::Temporary,
              .Value = Temporary->second,
              .ComplexRenderer = {},
              .Flags = Flags->rawValue(),
          });
          continue;
        }

        if (!Renderer.CapturedOperandSymbol.empty()) {
          const auto Capture = CaptureIDs.find(Renderer.CapturedOperandSymbol);
          if (Capture == CaptureIDs.end() || !Renderer.Flags.empty() ||
              Renderer.ImmediateTransform != "identity") {
            fail(Renderer.Range,
                 "selection captured operand renderer requires a matched capture and no flags");
            return false;
          }
          NormalizedAction.OperandRenderers.push_back({
              .SourceKind = NormalizedSelectionOperandRenderer::Kind::CapturedOperand,
              .Value = Capture->second,
              .ComplexRenderer = {},
              .Flags = 0,
          });
          continue;
        }

        if (Renderer.CapturedImmediateOperand) {
          const auto Transform =
              [&Renderer]() -> std::optional<NormalizedSelectionImmediateTransform> {
            if (Renderer.ImmediateTransform == "identity")
              return NormalizedSelectionImmediateTransform::Identity;
            if (Renderer.ImmediateTransform == "negate")
              return NormalizedSelectionImmediateTransform::Negate;
            if (Renderer.ImmediateTransform == "plus_one")
              return NormalizedSelectionImmediateTransform::PlusOne;
            return std::nullopt;
          }();
          if (!ConstantOperands.contains(*Renderer.CapturedImmediateOperand) || !Transform ||
              !Renderer.Flags.empty()) {
            fail(Renderer.Range,
                 "selection captured immediate renderer requires a matched constant and known "
                 "transform");
            return false;
          }
          NormalizedAction.OperandRenderers.push_back({
              .SourceKind = NormalizedSelectionOperandRenderer::Kind::CapturedImmediate,
              .Value = *Renderer.CapturedImmediateOperand,
              .ComplexRenderer = {},
              .Flags = 0,
              .Transform = *Transform,
          });
          continue;
        }

        if (Renderer.Immediate) {
          if (!Renderer.Flags.empty() || Renderer.ImmediateTransform != "identity") {
            fail(Renderer.Range, "selection immediate renderer cannot carry register flags");
            return false;
          }
          NormalizedAction.OperandRenderers.push_back({
              .SourceKind = NormalizedSelectionOperandRenderer::Kind::Immediate,
              .Value = 0,
              .ComplexRenderer = {},
              .Flags = 0,
              .Immediate = *Renderer.Immediate,
          });
          continue;
        }

        fail(Renderer.Range, "selection operand renderer has no normalized source");
        return false;
      }
      Normalized.Actions.push_back(std::move(NormalizedAction));
    }
    if (!HasRootAction) {
      fail(Rule.Range, "selection instructions require exactly one root replacement");
      return false;
    }

    std::unordered_set<std::uint32_t> ErasedOperands;
    for (const std::uint32_t OperandIndex : Rule.EraseOperandDefinitions) {
      if (!ErasedOperands.insert(OperandIndex).second ||
          (MatchInstruction != nullptr && !isRegisterOperand(*MatchInstruction, OperandIndex))) {
        fail(Rule.Range, "selection erased definitions require distinct register operands");
        return false;
      }
      if (!ConstantOperands.contains(OperandIndex) && !DefUseRootOperands.contains(OperandIndex)) {
        fail(Rule.Range,
             "selection erased definition requires a matched constant or def-use operand");
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

bool CodeGenTarget::normalizeCombinerRules(NormalizedTargetModel &Result) {
  std::unordered_set<std::string> Symbols;
  const auto NormalizeType = [this](const SourceSelectionTypePredicate &Source)
      -> std::optional<NormalizedSelectionTypePredicate> {
    NormalizedSelectionTypeKind Kind = NormalizedSelectionTypeKind::Any;
    if (Source.Kind == "scalar")
      Kind = NormalizedSelectionTypeKind::Scalar;
    else if (Source.Kind == "pointer")
      Kind = NormalizedSelectionTypeKind::Pointer;
    else if (Source.Kind != "any") {
      fail(Source.Range, "combiner type kind must be 'any', 'scalar', or 'pointer'");
      return std::nullopt;
    }
    if (Source.Bits > LowLevelType::MaxBitWidth ||
        (Kind != NormalizedSelectionTypeKind::Any && Source.Bits == 0)) {
      fail(Source.Range, "combiner type bit width is invalid for its kind");
      return std::nullopt;
    }
    return NormalizedSelectionTypePredicate{.Kind = Kind, .Bits = Source.Bits};
  };
  const auto NormalizeRanges =
      [this](const std::vector<SourceSelectionConstantRange> &Ranges,
             TextRange Range) -> std::optional<std::vector<NormalizedSelectionConstantRange>> {
    if (Ranges.empty()) {
      fail(Range, "combiner def-use constant predicate requires at least one range");
      return std::nullopt;
    }
    std::vector<NormalizedSelectionConstantRange> ResultRanges;
    ResultRanges.reserve(Ranges.size());
    for (const SourceSelectionConstantRange &Source : Ranges) {
      if (Source.Minimum > Source.Maximum) {
        fail(Source.SourceRange, "combiner def-use constant range is inverted");
        return std::nullopt;
      }
      ResultRanges.push_back({.Minimum = Source.Minimum, .Maximum = Source.Maximum});
    }
    std::ranges::sort(ResultRanges, {}, &NormalizedSelectionConstantRange::Minimum);
    for (std::size_t Index = 1; Index < ResultRanges.size(); ++Index) {
      if (ResultRanges[Index].Minimum <= ResultRanges[Index - 1].Maximum) {
        fail(Range, "combiner def-use constant ranges overlap");
        return std::nullopt;
      }
    }
    return ResultRanges;
  };
  const auto FlagValue = [](std::string_view Name) -> std::optional<std::uint32_t> {
    static constexpr std::array<std::pair<std::string_view, MachineInstrFlag>, 3> Flags = {{
        {"nneg", MachineInstrFlag::NonNeg},
        {"nsw", MachineInstrFlag::NoSignedWrap},
        {"nuw", MachineInstrFlag::NoUnsignedWrap},
    }};
    const auto Position = std::ranges::find(Flags, Name, &decltype(Flags)::value_type::first);
    if (Position == Flags.end())
      return std::nullopt;
    return std::uint32_t{1} << static_cast<std::uint8_t>(Position->second);
  };
  const auto FlagMask = [&FlagValue](std::span<const std::string> Names,
                                     TextRange Range) -> std::optional<std::uint32_t> {
    std::uint32_t Mask = 0;
    for (const std::string &Name : Names) {
      const auto Value = FlagValue(Name);
      if (!Value || (Mask & *Value) != 0)
        return std::nullopt;
      Mask |= *Value;
    }
    if (Names.size() != 0 && Mask == 0)
      return std::nullopt;
    (void)Range;
    return Mask;
  };

  for (const SourceCombinerRule &Rule : Input.CombinerRules) {
    const auto MatchOpcode = resolveOpcode(Result, Rule.MatchOpcodeSymbol);
    const NormalizedInstruction *MatchInstruction =
        findInstructionBySymbol(Result, Rule.MatchOpcodeSymbol);
    if (!isIdentifier(Rule.Symbol) || !Symbols.insert(Rule.Symbol).second || !MatchOpcode) {
      fail(Rule.Range, "combiner rules require a unique identifier and declared match opcode");
      return false;
    }
    if (MatchInstruction != nullptr && !std::ranges::contains(MatchInstruction->Flags, "Generic")) {
      fail(Rule.Range, "combiner rule match opcode must be generic");
      return false;
    }

    NormalizedCombinerRule Normalized;
    Normalized.Symbol = Rule.Symbol;
    Normalized.Priority = Rule.Priority;
    Normalized.Specificity = 1;
    Normalized.MatchOpcode = *MatchOpcode;
    if (Rule.ResultType) {
      auto Type = NormalizeType(*Rule.ResultType);
      if (!Type)
        return false;
      if (MatchInstruction != nullptr && !isRegisterOperand(*MatchInstruction, 0)) {
        fail(Rule.ResultType->Range,
             "combiner result type predicate requires register operand zero");
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
             "combiner operand type predicates require distinct register operands");
        return false;
      }
      Normalized.OperandTypes.push_back({.OperandIndex = Predicate.OperandIndex, .Type = *Type});
      ++Normalized.Specificity;
    }

    std::unordered_set<std::string> TargetPredicates;
    for (const SourceSelectionTargetPredicate &Predicate : Rule.TargetPredicates) {
      if (!isIdentifier(Predicate.Symbol) || !TargetPredicates.insert(Predicate.Symbol).second) {
        fail(Predicate.Range, "combiner target predicates require distinct identifiers");
        return false;
      }
      Normalized.TargetPredicates.push_back(Predicate.Symbol);
      ++Normalized.Specificity;
    }

    std::uint32_t InstructionFlags = 0;
    for (const std::string &Name : Rule.InstructionFlags) {
      const auto Value = FlagValue(Name);
      if (!Value) {
        fail(Rule.Range, "combiner instruction flag is not supported");
        return false;
      }
      if ((InstructionFlags & *Value) != 0) {
        fail(Rule.Range, "combiner instruction flags must be unique");
        return false;
      }
      InstructionFlags |= *Value;
      ++Normalized.Specificity;
    }
    if (InstructionFlags != 0)
      Normalized.InstructionFlags.push_back(InstructionFlags);

    std::unordered_set<std::uint32_t> DefUseRoots;
    std::unordered_set<std::uint32_t> CaptureIDs;
    std::unordered_map<std::string, std::uint32_t> Captures;
    std::uint32_t NextCaptureID = 1;
    for (const SourceSelectionDefUsePredicate &Predicate : Rule.DefUsePredicates) {
      const auto DefOpcode = resolveOpcode(Result, Predicate.OpcodeSymbol);
      const NormalizedInstruction *DefInstruction =
          findInstructionBySymbol(Result, Predicate.OpcodeSymbol);
      if (!DefOpcode || !DefUseRoots.insert(Predicate.RootOperandIndex).second ||
          (MatchInstruction != nullptr &&
           !isRegisterOperand(*MatchInstruction, Predicate.RootOperandIndex)) ||
          (DefInstruction != nullptr && !std::ranges::contains(DefInstruction->Flags, "Generic"))) {
        fail(Predicate.Range, "combiner def-use predicate has an invalid root or opcode");
        return false;
      }
      NormalizedSelectionDefUsePredicate DefUse{
          .RootOperandIndex = Predicate.RootOperandIndex,
          .Opcode = *DefOpcode,
          .IntegerPredicate = std::nullopt,
          .Captures = {},
          .OperandDefConstants = {},
      };
      for (const SourceSelectionDefUseCapture &Capture : Predicate.Captures) {
        if (!isIdentifier(Capture.Symbol) || Captures.contains(Capture.Symbol) ||
            !CaptureIDs.insert(NextCaptureID).second ||
            (DefInstruction != nullptr &&
             !isRegisterOperand(*DefInstruction, Capture.OperandIndex))) {
          fail(Capture.Range, "combiner captures require unique identifiers and register operands");
          return false;
        }
        Captures.emplace(Capture.Symbol, NextCaptureID);
        DefUse.Captures.push_back({.ID = NextCaptureID, .OperandIndex = Capture.OperandIndex});
        ++NextCaptureID;
      }
      for (const SourceSelectionOperandDefConstantPredicate &PredicateConstant :
           Predicate.OperandDefConstants) {
        auto Ranges = NormalizeRanges(PredicateConstant.Ranges, PredicateConstant.Range);
        if (!Ranges)
          return false;
        if (DefInstruction != nullptr &&
            (PredicateConstant.OperandIndex >= DefInstruction->Operands.size() ||
             (!std::ranges::contains(DefInstruction->Operands[PredicateConstant.OperandIndex].Kinds,
                                     "Immediate") &&
              !std::ranges::contains(DefInstruction->Operands[PredicateConstant.OperandIndex].Kinds,
                                     "IntegerImmediate")))) {
          fail(PredicateConstant.Range, "combiner def-use constant operand is not immediate");
          return false;
        }
        DefUse.OperandDefConstants.push_back({
            .OperandIndex = PredicateConstant.OperandIndex,
            .Ranges = std::move(*Ranges),
        });
      }
      Normalized.DefUsePredicates.push_back(std::move(DefUse));
      ++Normalized.Specificity;
    }
    if (Rule.DefUsePredicates.empty()) {
      fail(Rule.Range, "combiner rule requires a def-use predicate");
      return false;
    }

    NormalizedCombinerAction Action;
    if (Rule.Action.Kind == SourceCombinerActionKind::ReplaceRootWithCapturedOperand) {
      const auto Capture = Captures.find(Rule.Action.CapturedOperandSymbol);
      if (Capture == Captures.end()) {
        fail(Rule.Action.Range, "combiner replacement requires a matched capture");
        return false;
      }
      Action.Kind = NormalizedCombinerActionKind::ReplaceRootWithCapturedOperand;
      Action.CapturedOperand = Capture->second;
    } else {
      const auto ResultOpcode = resolveOpcode(Result, Rule.Action.ResultOpcodeSymbol);
      const NormalizedInstruction *ResultInstruction =
          findInstructionBySymbol(Result, Rule.Action.ResultOpcodeSymbol);
      if (!ResultOpcode || (ResultInstruction != nullptr &&
                            !std::ranges::contains(ResultInstruction->Flags, "Generic"))) {
        fail(Rule.Action.Range, "combiner mutation requires a declared generic result opcode");
        return false;
      }
      Action.Kind = NormalizedCombinerActionKind::MutateRoot;
      Action.ResultOpcode = *ResultOpcode;
      for (const SourceCombinerOperandRenderer &Renderer : Rule.Action.OperandRenderers) {
        NormalizedCombinerOperandRenderer NormalizedRenderer;
        switch (Renderer.SourceKind) {
        case SourceCombinerOperandRenderer::Kind::RootOperand:
          NormalizedRenderer.SourceKind = NormalizedCombinerOperandRenderer::Kind::RootOperand;
          NormalizedRenderer.Value = Renderer.OperandIndex;
          break;
        case SourceCombinerOperandRenderer::Kind::CapturedOperand: {
          const auto Capture = Captures.find(Renderer.CaptureSymbol);
          if (Capture == Captures.end()) {
            fail(Renderer.Range, "combiner renderer requires a matched capture");
            return false;
          }
          NormalizedRenderer.SourceKind = NormalizedCombinerOperandRenderer::Kind::CapturedOperand;
          NormalizedRenderer.Value = Capture->second;
          break;
        }
        case SourceCombinerOperandRenderer::Kind::CapturedTypeBits: {
          const auto Capture = Captures.find(Renderer.CaptureSymbol);
          if (Capture == Captures.end()) {
            fail(Renderer.Range, "combiner type-width renderer requires a matched capture");
            return false;
          }
          NormalizedRenderer.SourceKind = NormalizedCombinerOperandRenderer::Kind::CapturedTypeBits;
          NormalizedRenderer.Value = Capture->second;
          break;
        }
        case SourceCombinerOperandRenderer::Kind::Immediate:
          NormalizedRenderer.SourceKind = NormalizedCombinerOperandRenderer::Kind::Immediate;
          NormalizedRenderer.Immediate = Renderer.Immediate;
          break;
        }
        Action.OperandRenderers.push_back(NormalizedRenderer);
      }
      if (Action.OperandRenderers.empty()) {
        fail(Rule.Action.Range, "combiner mutation requires operand renderers");
        return false;
      }
    }
    if (!Rule.Action.ClearInstructionFlags.empty()) {
      const auto ClearFlags = FlagMask(Rule.Action.ClearInstructionFlags, Rule.Action.Range);
      if (!ClearFlags) {
        fail(Rule.Action.Range, "combiner action clear flags are invalid or duplicated");
        return false;
      }
      Action.ClearInstructionFlags = *ClearFlags;
    }
    Normalized.Action = std::move(Action);
    std::unordered_set<std::uint32_t> Erased;
    for (const std::uint32_t OperandIndex : Rule.EraseOperandDefinitions) {
      if (!Erased.insert(OperandIndex).second ||
          (MatchInstruction != nullptr && !isRegisterOperand(*MatchInstruction, OperandIndex)) ||
          !DefUseRoots.contains(OperandIndex)) {
        fail(Rule.Range, "combiner erased definitions must be distinct def-use operands");
        return false;
      }
      Normalized.EraseOperandDefinitions.push_back(OperandIndex);
    }
    Result.CombinerRules.push_back(std::move(Normalized));
  }

  std::ranges::sort(Result.CombinerRules,
                    [](const NormalizedCombinerRule &Left, const NormalizedCombinerRule &Right) {
                      if (Left.Priority != Right.Priority)
                        return Left.Priority > Right.Priority;
                      if (Left.Specificity != Right.Specificity)
                        return Left.Specificity > Right.Specificity;
                      return Left.Symbol < Right.Symbol;
                    });
  return true;
}

} // namespace rcc::targetgen
