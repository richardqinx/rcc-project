// SPDX-License-Identifier: GPL-3.0-or-later

#include "Common/CodeGenTarget.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace rcc::targetgen {

[[nodiscard]] std::optional<std::string_view>
operandKindEnumerator(std::string_view Value) noexcept {
  static constexpr std::array<std::pair<std::string_view, std::string_view>, 22> Kinds = {{
      {"register", "Register"},
      {"immediate", "Immediate"},
      {"integer_immediate", "IntegerImmediate"},
      {"floating_immediate", "FloatingImmediate"},
      {"basic_block", "BasicBlock"},
      {"frame_index", "FrameIndex"},
      {"constant_pool_index", "ConstantPoolIndex"},
      {"jump_table_index", "JumpTableIndex"},
      {"global_symbol", "GlobalSymbol"},
      {"external_symbol", "ExternalSymbol"},
      {"block_address", "BlockAddress"},
      {"register_mask", "RegisterMask"},
      {"live_out_register_mask", "LiveOutRegisterMask"},
      {"metadata", "Metadata"},
      {"target_index", "TargetIndex"},
      {"mc_symbol", "MCSymbol"},
      {"cfi_index", "CFIIndex"},
      {"intrinsic", "Intrinsic"},
      {"predicate", "Predicate"},
      {"shuffle_mask", "ShuffleMask"},
      {"debug_instr_ref", "DebugInstrRef"},
      {"lane_mask", "LaneMask"},
  }};
  const auto *const Position = std::ranges::find(Kinds, Value, &decltype(Kinds)::value_type::first);
  if (Position == Kinds.end())
    return std::nullopt;
  return Position->second;
}

[[nodiscard]] std::optional<std::string_view>
registerRoleEnumerator(std::string_view Value) noexcept {
  if (Value == "none")
    return "None";
  if (Value == "def")
    return "Def";
  if (Value == "use")
    return "Use";
  if (Value == "def_use")
    return "DefUse";
  return std::nullopt;
}

[[nodiscard]] std::optional<std::string_view>
memoryRoleEnumerator(std::string_view Value) noexcept {
  if (Value == "none")
    return "None";
  if (Value == "address")
    return "Address";
  if (Value == "value")
    return "Value";
  if (Value == "size")
    return "Size";
  if (Value == "ordering")
    return "Ordering";
  return std::nullopt;
}

[[nodiscard]] std::optional<std::string_view>
instructionFlagEnumerator(std::string_view Value) noexcept {
  static constexpr std::array<std::pair<std::string_view, std::string_view>, 25> Flags = {{
      {"terminator", "Terminator"},
      {"branch", "Branch"},
      {"indirect_branch", "IndirectBranch"},
      {"call", "Call"},
      {"return", "Return"},
      {"barrier", "Barrier"},
      {"may_load", "MayLoad"},
      {"may_store", "MayStore"},
      {"has_side_effects", "HasSideEffects"},
      {"pseudo", "Pseudo"},
      {"meta", "Meta"},
      {"allowed_pre_legalizer", "AllowedPreLegalizer"},
      {"produces_code", "ProducesCode"},
      {"commutable", "Commutable"},
      {"rematerializable", "Rematerializable"},
      {"predicable", "Predicable"},
      {"not_duplicable", "NotDuplicable"},
      {"convergent", "Convergent"},
      {"unmodeled_side_effects", "UnmodeledSideEffects"},
      {"may_raise_fp_exception", "MayRaiseFPException"},
      {"phi", "Phi"},
      {"fake_use", "FakeUse"},
      {"generic", "Generic"},
      {"terminal_emission", "TerminalEmission"},
      {"uses_custom_inserter", "UsesCustomInserter"},
  }};
  const auto *const Position = std::ranges::find(Flags, Value, &decltype(Flags)::value_type::first);
  if (Position == Flags.end())
    return std::nullopt;
  return Position->second;
}

[[nodiscard]] std::optional<std::string_view>
pseudoOwnerEnumerator(std::string_view Value) noexcept {
  static constexpr std::array<std::pair<std::string_view, std::string_view>, 12> Owners = {{
      {"none", "None"},
      {"legalizer", "Legalizer"},
      {"register_bank_select", "RegisterBankSelect"},
      {"instruction_select", "InstructionSelect"},
      {"phi_elimination", "PhiElimination"},
      {"two_address_rewrite", "TwoAddressRewrite"},
      {"pre_ra_expansion", "PreRAExpansion"},
      {"register_allocator", "RegisterAllocator"},
      {"prologue_epilogue_insertion", "PrologueEpilogueInsertion"},
      {"post_ra_expansion", "PostRAExpansion"},
      {"finalization", "Finalization"},
      {"target_late_expansion", "TargetLateExpansion"},
  }};
  if (Value == "asm_printer")
    return "AsmPrinter";
  const auto *const Position =
      std::ranges::find(Owners, Value, &decltype(Owners)::value_type::first);
  if (Position == Owners.end())
    return std::nullopt;
  return Position->second;
}

[[nodiscard]] std::optional<std::string_view>
pipelineStageEnumerator(std::string_view Value) noexcept {
  static constexpr std::array<std::pair<std::string_view, std::string_view>, 10> Stages = {{
      {"machine_ir_input", "MachineIRInput"},
      {"legalized", "Legalized"},
      {"reg_bank_selected", "RegBankSelected"},
      {"selected", "Selected"},
      {"ssa_eliminated", "SSAEliminated"},
      {"tied_ops_rewritten", "TiedOpsRewritten"},
      {"register_allocated", "RegisterAllocated"},
      {"frame_finalized", "FrameFinalized"},
      {"post_ra_finalized", "PostRAFinalized"},
      {"emission", "Emission"},
  }};
  const auto *const Position =
      std::ranges::find(Stages, Value, &decltype(Stages)::value_type::first);
  if (Position == Stages.end())
    return std::nullopt;
  return Position->second;
}

[[nodiscard]] bool CodeGenTarget::normalizeMIROperandFlags(NormalizedTargetModel &Result) {
  const SourceMIROperandFlags &Flags = Result.MIROperandFlags;
  if (Flags.Direct.empty() != (Flags.DirectMask == 0)) {
    fail(Flags.Range, "direct MIR operand flags and their direct mask must be present together");
    return false;
  }

  std::unordered_set<std::string> Names;
  std::unordered_set<std::uint32_t> DirectValues;
  for (const SourceMIROperandFlag &Flag : Flags.Direct) {
    if (Flag.Name.empty() || Flag.Value == 0 || (Flag.Value & ~Flags.DirectMask) != 0 ||
        !Names.insert(Flag.Name).second || !DirectValues.insert(Flag.Value).second) {
      fail(Flag.Range,
           "direct MIR operand flags require unique names and nonzero values inside the mask");
      return false;
    }
  }

  std::uint32_t BitmaskValues = 0;
  for (const SourceMIROperandFlag &Flag : Flags.Bitmask) {
    if (Flag.Name.empty() || !std::has_single_bit(Flag.Value) ||
        (Flag.Value & Flags.DirectMask) != 0 || !Names.insert(Flag.Name).second ||
        (BitmaskValues & Flag.Value) != 0) {
      fail(Flag.Range,
           "bitmask MIR operand flags require unique names and distinct bits outside the "
           "direct mask");
      return false;
    }
    BitmaskValues |= Flag.Value;
  }
  return true;
}

[[nodiscard]] bool CodeGenTarget::normalizeNamedOperands(NormalizedTargetModel &Result) {
  std::unordered_set<std::uint32_t> IDs;
  std::unordered_set<std::string> Symbols;
  std::unordered_set<std::string> Names;
  for (const SourceNamedOperand &Operand : Input.NamedOperands) {
    if (Operand.ID == 0 || !isIdentifier(Operand.Symbol) || Operand.Name.empty() ||
        !IDs.insert(Operand.ID).second || !Symbols.insert(Operand.Symbol).second ||
        !Names.insert(Operand.Name).second) {
      fail(Operand.Range, "named operands require nonzero unique IDs, symbols, and names");
      return false;
    }
    NamedOperandBySymbol.emplace(Operand.Symbol, Operand.ID);
    Result.NamedOperands.push_back({
        .ID = Operand.ID,
        .Symbol = Operand.Symbol,
        .Name = Operand.Name,
    });
  }
  return true;
}

[[nodiscard]] bool CodeGenTarget::normalizeImmediatePredicates(NormalizedTargetModel &Result) {
  std::unordered_set<std::uint32_t> IDs;
  std::unordered_set<std::string> Symbols;
  std::unordered_set<std::string> Names;
  for (const SourceImmediatePredicate &Predicate : Input.ImmediatePredicates) {
    const bool IsSigned = Predicate.Kind == "signed";
    const bool IsUnsigned = Predicate.Kind == "unsigned";
    if (Predicate.ID == 0 || !isIdentifier(Predicate.Symbol) || Predicate.Name.empty() ||
        (!IsSigned && !IsUnsigned) || Predicate.Bits == 0 || Predicate.Bits > 63 ||
        !std::has_single_bit(Predicate.Alignment) || !IDs.insert(Predicate.ID).second ||
        !Symbols.insert(Predicate.Symbol).second || !Names.insert(Predicate.Name).second) {
      fail(Predicate.Range,
           "immediate predicates require nonzero unique IDs, symbols, names, a signed or "
           "unsigned kind, a width in [1, 63], and power-of-two alignment");
      return false;
    }
    const std::int64_t PositiveLimit = std::int64_t{1} << (Predicate.Bits - 1U);
    const std::int64_t Minimum = IsSigned ? -PositiveLimit : 0;
    const std::int64_t Maximum =
        IsSigned ? PositiveLimit - 1
                 : static_cast<std::int64_t>((std::uint64_t{1} << Predicate.Bits) - 1U);
    std::unordered_set<std::int64_t> AllowedValues;
    for (const std::int64_t Value : Predicate.AllowedValues) {
      if (Value < Minimum || Value > Maximum || (Predicate.NonZero && Value == 0) ||
          !AllowedValues.insert(Value).second) {
        fail(Predicate.Range,
             "immediate predicate allowed_values must be unique, in range, and satisfy nonzero");
        return false;
      }
    }
    ImmediatePredicateBySymbol.emplace(Predicate.Symbol, Predicate.ID);
    Result.ImmediatePredicates.push_back({
        .ID = Predicate.ID,
        .Symbol = Predicate.Symbol,
        .Name = Predicate.Name,
        .IsSigned = IsSigned,
        .Bits = Predicate.Bits,
        .Alignment = Predicate.Alignment,
        .NonZero = Predicate.NonZero,
        .AllowedValues = Predicate.AllowedValues,
    });
  }
  return true;
}

[[nodiscard]] bool CodeGenTarget::normalizeOperand(const SourceOperand &InputOperand,
                                                   NormalizedOperand &Result) {
  if (InputOperand.Kinds.empty()) {
    fail(InputOperand.Range, "instruction operand requires at least one kind");
    return false;
  }
  std::unordered_set<std::string> Kinds;
  bool HasRegisterKind = false;
  for (const std::string &Kind : InputOperand.Kinds) {
    const auto Enumerator = operandKindEnumerator(Kind);
    if (!Enumerator || !Kinds.insert(std::string(*Enumerator)).second) {
      fail(InputOperand.Range, "instruction operand kinds must be known and unique");
      return false;
    }
    HasRegisterKind |= *Enumerator == "Register";
    Result.Kinds.push_back(std::string(*Enumerator));
  }

  const auto Role = registerRoleEnumerator(InputOperand.RegisterRole);
  const auto MemoryRole = memoryRoleEnumerator(InputOperand.MemoryRole);
  if (!Role || !MemoryRole || HasRegisterKind != (*Role != "None")) {
    fail(InputOperand.Range,
         "instruction operand register role and memory role must match its declared kinds");
    return false;
  }
  Result.RegisterRole = std::string(*Role);
  Result.MemoryRole = std::string(*MemoryRole);

  const bool IsDefRole = *Role == "Def" || *Role == "DefUse";
  const bool IsRegisterOnly = Result.Kinds.size() == 1 && Result.Kinds.front() == "Register";
  if (IsDefRole && !IsRegisterOnly) {
    fail(InputOperand.Range, "register definition operands must accept only registers");
    return false;
  }
  if (InputOperand.TypeIndex && !IsRegisterOnly) {
    fail(InputOperand.Range, "instruction operand type_index requires exactly one register kind");
    return false;
  }
  if (!HasRegisterKind && (InputOperand.RegisterClassSymbol || InputOperand.RegisterBankSymbol ||
                           InputOperand.EarlyClobber || InputOperand.AllowSubReg)) {
    fail(InputOperand.Range, "non-register operands cannot have register constraints");
    return false;
  }
  if (InputOperand.EarlyClobber && !IsDefRole) {
    fail(InputOperand.Range, "early-clobber operands must define a register");
    return false;
  }

  if (InputOperand.RegisterClassSymbol) {
    const auto Position = ClassBySymbol.find(*InputOperand.RegisterClassSymbol);
    if (Position == ClassBySymbol.end()) {
      fail(InputOperand.Range, "instruction operand names an unknown register class");
      return false;
    }
    Result.RegisterClass = Position->second;
  }
  if (InputOperand.RegisterBankSymbol) {
    const auto Position = BankBySymbol.find(*InputOperand.RegisterBankSymbol);
    if (Position == BankBySymbol.end()) {
      fail(InputOperand.Range, "instruction operand names an unknown register bank");
      return false;
    }
    Result.RegisterBank = Position->second;
  }
  if (InputOperand.ImmediatePredicateSymbol) {
    const auto Position = ImmediatePredicateBySymbol.find(*InputOperand.ImmediatePredicateSymbol);
    if (Position == ImmediatePredicateBySymbol.end() ||
        std::ranges::find(Result.Kinds, "Immediate") == Result.Kinds.end()) {
      fail(InputOperand.Range,
           "instruction immediate predicates must name a declaration and constrain an immediate");
      return false;
    }
    Result.ImmediatePredicate = Position->second;
  }
  if (InputOperand.NameSymbol) {
    const auto Position = NamedOperandBySymbol.find(*InputOperand.NameSymbol);
    if (Position == NamedOperandBySymbol.end()) {
      fail(InputOperand.Range, "instruction operand names an unknown named operand");
      return false;
    }
    Result.Name = Position->second;
  }

  Result.DefaultTiedOperand = InputOperand.DefaultTiedOperand;
  Result.EarlyClobber = InputOperand.EarlyClobber;
  Result.Optional = InputOperand.Optional;
  Result.VariadicGroup = InputOperand.VariadicGroup;
  Result.AllowSubReg = InputOperand.AllowSubReg;
  Result.TypeIndex = InputOperand.TypeIndex;
  Result.AllowedTargetFlags = InputOperand.AllowedTargetFlags;
  return true;
}

[[nodiscard]] bool
CodeGenTarget::normalizeImplicitRegisters(const std::vector<SourceImplicitRegister> &InputRegisters,
                                          bool IsDef,
                                          std::vector<NormalizedImplicitRegister> &Result) {
  std::unordered_set<std::uint32_t> Registers;
  for (const SourceImplicitRegister &Register : InputRegisters) {
    const auto Position = RegisterBySymbol.find(Register.RegisterSymbol);
    if (Position == RegisterBySymbol.end() || !Registers.insert(Position->second).second ||
        (!IsDef && Register.EarlyClobber)) {
      fail(Register.Range,
           "implicit registers must be declared and unique; implicit uses cannot early-clobber");
      return false;
    }
    Result.push_back({
        .Register = Position->second,
        .EarlyClobber = Register.EarlyClobber,
        .Renamable = Register.Renamable,
    });
  }
  return true;
}

[[nodiscard]] bool CodeGenTarget::normalizeInstructions(NormalizedTargetModel &Result) {
  if (Input.Instructions.empty()) {
    fail(Input.Target.Range, "target description requires at least one instruction");
    return false;
  }
  std::unordered_set<std::uint32_t> IDs;
  std::unordered_set<std::string> Symbols;
  std::unordered_set<std::string> Names;
  for (const SourceInstruction &Instruction : Input.Instructions) {
    const bool HasMnemonic = !Instruction.AssemblyMnemonic.empty();
    const bool HasBinaryEncoding = Instruction.BinaryEncoding.has_value();
    const bool HasSchedulingClass = !Instruction.SchedulingClassSymbol.empty();
    const bool HasEncodingCategory = !Instruction.EncodingCategorySymbol.empty();
    const bool HasAssemblyFormat = !Instruction.AssemblyFormatSymbol.empty();
    const bool HasAllMachineFacts = HasMnemonic && HasBinaryEncoding && HasSchedulingClass &&
                                    HasEncodingCategory && HasAssemblyFormat;
    const bool HasAnyMachineFact = HasMnemonic || HasBinaryEncoding || HasSchedulingClass ||
                                   HasEncodingCategory || HasAssemblyFormat;
    const bool IsPseudoSource =
        std::ranges::find(Instruction.Flags, "pseudo") != Instruction.Flags.end();
    if ((!IsPseudoSource && !HasAllMachineFacts) || HasAnyMachineFact != HasAllMachineFacts) {
      fail(Instruction.Range,
           "encoded instructions require complete scheduling, encoding, mnemonic, and "
           "assembly-format facts");
      return false;
    }
    const auto SchedulingClass = SchedulingClassBySymbol.find(Instruction.SchedulingClassSymbol);
    const auto EncodingCategory = EncodingCategoryBySymbol.find(Instruction.EncodingCategorySymbol);
    const auto AssemblyFormat = AssemblyFormatBySymbol.find(Instruction.AssemblyFormatSymbol);
    const auto Owner = pseudoOwnerEnumerator(Instruction.Owner);
    const auto Deadline = pipelineStageEnumerator(Instruction.Deadline);
    if (Instruction.ID == 0 || !isIdentifier(Instruction.Symbol) || Instruction.Name.empty() ||
        (HasMnemonic && !isAssemblyMnemonic(Instruction.AssemblyMnemonic)) ||
        (HasSchedulingClass && SchedulingClass == SchedulingClassBySymbol.end()) ||
        (HasEncodingCategory && EncodingCategory == EncodingCategoryBySymbol.end()) ||
        (HasAssemblyFormat && AssemblyFormat == AssemblyFormatBySymbol.end()) || !Owner ||
        !Deadline || !IDs.insert(Instruction.ID).second ||
        !Symbols.insert(Instruction.Symbol).second || !Names.insert(Instruction.Name).second) {
      fail(Instruction.Range,
           "instructions require nonzero unique IDs, symbols, lowercase assembly mnemonics, and "
           "declared format IDs");
      return false;
    }
    if (Instruction.NumExplicitDefs > Instruction.Operands.size()) {
      fail(Instruction.Range, "instruction explicit def count is outside its operand range");
      return false;
    }

    NormalizedInstruction Normalized{
        .ID = Instruction.ID,
        .Symbol = Instruction.Symbol,
        .Name = Instruction.Name,
        .AssemblyMnemonic = Instruction.AssemblyMnemonic,
        .BinaryEncoding = Instruction.BinaryEncoding.value_or(0),
        .Operands = {},
        .NumExplicitDefs = Instruction.NumExplicitDefs,
        .Variadic = Instruction.Variadic,
        .ImplicitDefs = {},
        .ImplicitUses = {},
        .AllowExtraImplicitOperands = Instruction.AllowExtraImplicitOperands,
        .Flags = {},
        .SchedulingClass = HasSchedulingClass ? SchedulingClass->second : 0,
        .EncodingCategory = HasEncodingCategory ? EncodingCategory->second : 0,
        .SizeInBytes = Instruction.SizeInBytes,
        .AssemblyFormat = HasAssemblyFormat ? AssemblyFormat->second : 0,
        .RequiresMemoryOperands = Instruction.RequiresMemoryOperands,
        .Owner = std::string(*Owner),
        .Deadline = std::string(*Deadline),
    };
    Normalized.Operands.reserve(Instruction.Operands.size());
    bool SawOptional = false;
    for (std::size_t Index = 0; Index < Instruction.Operands.size(); ++Index) {
      NormalizedOperand Operand;
      if (!normalizeOperand(Instruction.Operands[Index], Operand))
        return false;
      const bool IsDef = Operand.RegisterRole == "Def" || Operand.RegisterRole == "DefUse";
      if ((Index < Instruction.NumExplicitDefs) != IsDef) {
        fail(Instruction.Operands[Index].Range,
             "instruction explicit register definitions must form the leading operand prefix");
        return false;
      }
      if (Instruction.Variadic && Operand.Optional) {
        fail(Instruction.Operands[Index].Range,
             "variadic instructions cannot contain optional operands");
        return false;
      }
      if (Operand.Optional)
        SawOptional = true;
      else if (SawOptional && !Operand.VariadicGroup) {
        fail(Instruction.Operands[Index].Range,
             "optional instruction operands must form a trailing suffix");
        return false;
      }
      if (Operand.VariadicGroup &&
          (!Instruction.Variadic || Index + 1 != Instruction.Operands.size())) {
        fail(Instruction.Operands[Index].Range,
             "a variadic operand group must be the final instruction operand");
        return false;
      }
      if (Operand.VariadicGroup && (Operand.Optional || Operand.DefaultTiedOperand)) {
        fail(Instruction.Operands[Index].Range,
             "a variadic operand group cannot be optional or have a default tie");
        return false;
      }
      Normalized.Operands.push_back(std::move(Operand));
    }
    if (Instruction.Variadic &&
        (Normalized.Operands.empty() || !Normalized.Operands.back().VariadicGroup)) {
      fail(Instruction.Range, "variadic instructions require a final variadic operand group");
      return false;
    }
    if (!Instruction.Variadic &&
        std::ranges::any_of(Normalized.Operands, [](const NormalizedOperand &Operand) {
          return Operand.VariadicGroup;
        })) {
      fail(Instruction.Range, "non-variadic instructions cannot have a variadic operand group");
      return false;
    }

    std::unordered_set<std::uint32_t> NamedOperands;
    for (std::size_t Index = 0; Index < Normalized.Operands.size(); ++Index) {
      const NormalizedOperand &Operand = Normalized.Operands[Index];
      if (Operand.Name && !NamedOperands.insert(*Operand.Name).second) {
        fail(Instruction.Operands[Index].Range,
             "instruction operands must use distinct named operand IDs");
        return false;
      }
      if (!Operand.DefaultTiedOperand)
        continue;
      const std::uint32_t OtherIndex = *Operand.DefaultTiedOperand;
      if (OtherIndex >= Normalized.Operands.size() || OtherIndex == Index ||
          !Normalized.Operands[OtherIndex].DefaultTiedOperand ||
          *Normalized.Operands[OtherIndex].DefaultTiedOperand != Index) {
        fail(Instruction.Operands[Index].Range,
             "instruction default ties must be reciprocal and in range");
        return false;
      }
      const NormalizedOperand &Other = Normalized.Operands[OtherIndex];
      const bool IsRegisterOnly = Operand.Kinds.size() == 1 && Operand.Kinds.front() == "Register";
      const bool OtherIsRegisterOnly = Other.Kinds.size() == 1 && Other.Kinds.front() == "Register";
      const bool IsDefUsePair = (Operand.RegisterRole == "Def" && Other.RegisterRole == "Use") ||
                                (Operand.RegisterRole == "Use" && Other.RegisterRole == "Def");
      if (Operand.Optional || Other.Optional || !IsRegisterOnly || !OtherIsRegisterOnly ||
          !IsDefUsePair) {
        fail(Instruction.Operands[Index].Range,
             "instruction default ties must connect required register defs and uses");
        return false;
      }
    }

    std::unordered_set<std::string> Flags;
    for (const std::string &Flag : Instruction.Flags) {
      const auto Enumerator = instructionFlagEnumerator(Flag);
      if (!Enumerator || !Flags.insert(std::string(*Enumerator)).second) {
        fail(Instruction.Range, "instruction flags must be known and unique");
        return false;
      }
      Normalized.Flags.push_back(std::string(*Enumerator));
    }
    const auto HasFlag = [&Normalized](std::string_view Flag) {
      return std::ranges::find(Normalized.Flags, Flag) != Normalized.Flags.end();
    };
    const bool IsTerminator = HasFlag("Terminator");
    if (!HasFlag("Generic") &&
        std::ranges::any_of(Normalized.Operands, [](const NormalizedOperand &Operand) {
          return Operand.TypeIndex.has_value();
        })) {
      fail(Instruction.Range,
           "instruction type_index operands require the generic instruction flag");
      return false;
    }
    if ((HasFlag("Branch") || HasFlag("Return") || HasFlag("Barrier") ||
         HasFlag("IndirectBranch")) &&
        !IsTerminator) {
      fail(Instruction.Range, "branch, return, and barrier instructions must be terminators");
      return false;
    }
    if (HasFlag("IndirectBranch") && !HasFlag("Branch")) {
      fail(Instruction.Range, "indirect branch instructions must also be branches");
      return false;
    }
    const bool IsPseudo = HasFlag("Pseudo");
    const bool IsMeta = HasFlag("Meta");
    if ((IsMeta && (!IsPseudo || HasFlag("ProducesCode"))) ||
        (!IsPseudo && Normalized.Owner != "None") ||
        (IsPseudo && !IsMeta && Normalized.Owner == "None") ||
        (HasFlag("TerminalEmission") &&
         (!IsPseudo || Normalized.Owner != "AsmPrinter" || !HasFlag("ProducesCode")))) {
      fail(Instruction.Range, "instruction pseudo flags, owner, and deadline are inconsistent");
      return false;
    }
    if (Normalized.SizeInBytes != 0 && !HasFlag("ProducesCode")) {
      fail(Instruction.Range, "only code-producing instructions can declare a byte size");
      return false;
    }
    if (Normalized.RequiresMemoryOperands && !HasFlag("MayLoad") && !HasFlag("MayStore")) {
      fail(Instruction.Range, "memory operand requirements need a load or store instruction flag");
      return false;
    }
    if (!normalizeImplicitRegisters(Instruction.ImplicitDefs, true, Normalized.ImplicitDefs) ||
        !normalizeImplicitRegisters(Instruction.ImplicitUses, false, Normalized.ImplicitUses)) {
      return false;
    }
    Result.Instructions.push_back(std::move(Normalized));
  }
  std::ranges::sort(Result.Instructions, {}, &NormalizedInstruction::ID);
  return true;
}

} // namespace rcc::targetgen
