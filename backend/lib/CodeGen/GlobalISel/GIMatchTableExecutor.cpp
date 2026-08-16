// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/GlobalISel/GIMatchTableExecutor.h"

#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/GlobalISel/Utils.h"
#include "rcc/CodeGen/LowLevelType.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/TargetDescription.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
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

[[nodiscard]] std::optional<std::int64_t> immediateValue(const MachineOperand &Operand) {
  if (const std::int64_t *Immediate = Operand.getIf<std::int64_t>())
    return *Immediate;
  const auto *Integer = Operand.getIf<IntegerImmediateOperand>();
  if (Integer == nullptr)
    return std::optional<std::int64_t>{};
  const std::string Text = Integer->Value.toSignedDecimal();
  std::int64_t Parsed = 0;
  const auto [End, Error] = std::from_chars(Text.data(), Text.data() + Text.size(), Parsed);
  if (Error != std::errc{} || End != Text.data() + Text.size())
    return std::optional<std::int64_t>{};
  return Parsed;
}

[[nodiscard]] Expected<std::optional<std::int64_t>>
operandDefConstantValue(const MachineInstr &Instruction, const MachineFunction &Function,
                        std::size_t OperandIndex) {
  const MachineOperand *Operand = Instruction.operandAt(OperandIndex);
  const Register *Reg = Operand == nullptr ? nullptr : Operand->getIf<Register>();
  if (Reg == nullptr || !Reg->isVirtual())
    return std::optional<std::int64_t>{};
  const MachineInstr *Definition = Function.registerInfo().getVRegDef(*Reg);
  if (Definition == nullptr || Definition->opcode() != opcode(GenericOpcode::G_CONSTANT))
    return std::optional<std::int64_t>{};
  const MachineOperand *Value = Definition->operandAt(1);
  if (Value == nullptr)
    return std::optional<std::int64_t>{};
  return immediateValue(*Value);
}

[[nodiscard]] Expected<std::optional<std::int64_t>>
operandOrDefConstantValue(const MachineInstr &Instruction, const MachineFunction &Function,
                          std::size_t OperandIndex) {
  const MachineOperand *Operand = Instruction.operandAt(OperandIndex);
  if (Operand == nullptr)
    return std::optional<std::int64_t>{};
  if (const auto Value = immediateValue(*Operand))
    return Value;
  return operandDefConstantValue(Instruction, Function, OperandIndex);
}

[[nodiscard]] Expected<bool> matchesConstantRanges(const GIMatchTable &Table, std::size_t First,
                                                   std::size_t Count,
                                                   std::optional<std::int64_t> Value) {
  if (Count == 0 || First > Table.ConstantRanges.size() ||
      Count > Table.ConstantRanges.size() - First) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "generated match table has an invalid constant range");
  }
  if (!Value)
    return false;
  for (std::size_t Index = First; Index < First + Count; ++Index) {
    const GIMatchTableConstantRange &Range = Table.ConstantRanges[Index];
    if (Range.Minimum > Range.Maximum) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "generated match table has an inverted constant range");
    }
    if (*Value >= Range.Minimum && *Value <= Range.Maximum)
      return true;
  }
  return false;
}

[[nodiscard]] Expected<bool> matchesOperandImmediateRanges(const GIMatchTable &Table,
                                                           const MachineInstr &Instruction,
                                                           const GIMatchTableInstruction &Step) {
  const MachineOperand *Operand = Instruction.operandAt(Step.OperandIndex);
  const std::optional<std::int64_t> Value =
      Operand == nullptr ? std::optional<std::int64_t>{} : immediateValue(*Operand);
  return matchesConstantRanges(Table, Step.Value, Step.Auxiliary, Value);
}

[[nodiscard]] Expected<bool> matchesOperandDefConstantRanges(const GIMatchTable &Table,
                                                             const MachineInstr &Instruction,
                                                             const MachineFunction &Function,
                                                             const GIMatchTableInstruction &Step) {
  auto Value = operandDefConstantValue(Instruction, Function, Step.OperandIndex);
  if (!Value)
    return std::unexpected(Value.error());
  return matchesConstantRanges(Table, Step.Value, Step.Auxiliary, *Value);
}

[[nodiscard]] const MachineInstr *operandDefinition(const MachineInstr &Instruction,
                                                    const MachineFunction &Function,
                                                    std::size_t OperandIndex) {
  const MachineOperand *Operand = Instruction.operandAt(OperandIndex);
  const Register *Reg = Operand == nullptr ? nullptr : Operand->getIf<Register>();
  return Reg != nullptr && Reg->isVirtual() ? Function.registerInfo().getVRegDef(*Reg) : nullptr;
}

[[nodiscard]] Expected<bool> matchesOperandDefOpcode(const MachineInstr &Instruction,
                                                     const MachineFunction &Function,
                                                     const GIMatchTableInstruction &Step) {
  if (Step.Value == 0)
    return makeUnexpected(ErrorCode::VerificationError,
                          "generated match table has an invalid def-use opcode");
  const MachineInstr *Definition = operandDefinition(Instruction, Function, Step.OperandIndex);
  return Definition != nullptr && Definition->opcode().value() == Step.Value;
}

[[nodiscard]] Expected<bool>
matchesOperandDefOperandConstantRanges(const GIMatchTable &Table, const MachineInstr &Instruction,
                                       const MachineFunction &Function,
                                       const GIMatchTableInstruction &Step) {
  if (Step.Immediate <= 0 ||
      static_cast<std::uint64_t>(Step.Immediate) > std::numeric_limits<std::uint32_t>::max()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "generated match table has an invalid def-use constant range count");
  }
  const std::size_t Count = static_cast<std::size_t>(Step.Immediate);
  const MachineInstr *Definition = operandDefinition(Instruction, Function, Step.OperandIndex);
  if (Definition == nullptr)
    return false;
  auto Value = operandOrDefConstantValue(*Definition, Function, Step.Auxiliary);
  if (!Value)
    return std::unexpected(Value.error());
  return matchesConstantRanges(Table, Step.Value, Count, *Value);
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
  struct CapturedOperand final {
    std::uint32_t ID = 0;
    MachineOperand Operand;
  };

  struct Temporary final {
    std::uint32_t ID = 0;
    std::uint32_t TypeFromOperand = 0;
  };

  struct RenderedOperand final {
    enum class Kind : std::uint8_t {
      RootOperand,
      ComplexOperand,
      CapturedOperand,
      PhysicalRegister,
      Temporary,
      CapturedImmediate,
      Immediate,
    };

    Kind SourceKind = Kind::RootOperand;
    std::uint32_t Value = 0;
    std::uint32_t Flags = 0;
    std::string_view ComplexRenderer;
    GIMatchImmediateTransform Transform = GIMatchImmediateTransform::Identity;
    std::int64_t Immediate = 0;
  };

  struct Instruction final {
    Opcode ResultOpcode;
    bool ReplacesRoot = false;
    std::vector<RenderedOperand> Operands;
  };

  std::vector<Temporary> Temporaries;
  std::vector<CapturedOperand> CapturedOperands;
  std::vector<Instruction> Instructions;
  std::vector<Register> EraseOperandDefinitions;
};

[[nodiscard]] bool hasTemporary(const MatchedRule &Rule, std::uint32_t ID) {
  return std::ranges::any_of(Rule.Temporaries, [ID](const MatchedRule::Temporary &Temporary) {
    return Temporary.ID == ID;
  });
}

[[nodiscard]] bool hasCapturedOperand(const MatchedRule &Rule, std::uint32_t ID) {
  return std::ranges::any_of(
      Rule.CapturedOperands,
      [ID](const MatchedRule::CapturedOperand &Capture) { return Capture.ID == ID; });
}

[[nodiscard]] const MatchedRule::CapturedOperand *
findCapturedOperand(const std::vector<MatchedRule::CapturedOperand> &Captures, std::uint32_t ID) {
  const auto Position = std::ranges::find(Captures, ID, &MatchedRule::CapturedOperand::ID);
  return Position == Captures.end() ? nullptr : &*Position;
}

template <typename TargetPredicateMatcher>
[[nodiscard]] Expected<std::optional<MatchedRule>>
matchRule(const GIMatchTable &Table, const GIMatchTableRule &Rule, const MachineInstr &Instruction,
          const MachineFunction &Function, const TargetDescription &Target,
          TargetPredicateMatcher &&MatchesTargetPredicate) {
  const std::size_t First = Rule.FirstInstruction;
  const std::size_t Count = Rule.InstructionCount;
  if (First > Table.Instructions.size() || Count > Table.Instructions.size() - First) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "generated match table rule has an invalid instruction range");
  }

  MatchedRule Result;
  bool Applying = false;
  bool Complete = false;
  for (std::size_t Index = First; Index < First + Count; ++Index) {
    const GIMatchTableInstruction &Step = Table.Instructions[Index];
    switch (Step.Opcode) {
    case GIMatchTableOpcode::CheckOpcode:
      if (Applying) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generated match table performs a predicate after an action");
      }
      if (Instruction.opcode().value() != Step.Value)
        return std::optional<MatchedRule>{};
      break;
    case GIMatchTableOpcode::CheckOperandType: {
      if (Applying) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generated match table performs a predicate after an action");
      }
      auto Matched = matchesType(Instruction, Function, Step);
      if (!Matched)
        return std::unexpected(Matched.error());
      if (!*Matched)
        return std::optional<MatchedRule>{};
      break;
    }
    case GIMatchTableOpcode::CheckOperandBank: {
      if (Applying) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generated match table performs a predicate after an action");
      }
      auto Matched = matchesBank(Instruction, Function, Step);
      if (!Matched)
        return std::unexpected(Matched.error());
      if (!*Matched)
        return std::optional<MatchedRule>{};
      break;
    }
    case GIMatchTableOpcode::CheckOperandClass: {
      if (Applying) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generated match table performs a predicate after an action");
      }
      auto Matched = matchesClass(Instruction, Function, Step);
      if (!Matched)
        return std::unexpected(Matched.error());
      if (!*Matched)
        return std::optional<MatchedRule>{};
      break;
    }
    case GIMatchTableOpcode::CheckIntegerPredicate: {
      if (Applying) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generated match table performs a predicate after an action");
      }
      auto Matched = matchesIntegerPredicate(Instruction, Step);
      if (!Matched)
        return std::unexpected(Matched.error());
      if (!*Matched)
        return std::optional<MatchedRule>{};
      break;
    }
    case GIMatchTableOpcode::CheckTargetPredicate: {
      if (Applying || Step.Symbol.empty()) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generated match table has an invalid target predicate");
      }
      auto Matched = MatchesTargetPredicate(Step.Symbol, Instruction, Function, Target);
      if (!Matched)
        return std::unexpected(Matched.error());
      if (!*Matched)
        return std::optional<MatchedRule>{};
      break;
    }
    case GIMatchTableOpcode::CheckOperandImmediateRanges: {
      if (Applying) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generated match table performs a predicate after an action");
      }
      auto Matched = matchesOperandImmediateRanges(Table, Instruction, Step);
      if (!Matched)
        return std::unexpected(Matched.error());
      if (!*Matched)
        return std::optional<MatchedRule>{};
      break;
    }
    case GIMatchTableOpcode::CheckOperandDefConstantRanges: {
      if (Applying) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generated match table performs a predicate after an action");
      }
      auto Matched = matchesOperandDefConstantRanges(Table, Instruction, Function, Step);
      if (!Matched)
        return std::unexpected(Matched.error());
      if (!*Matched)
        return std::optional<MatchedRule>{};
      break;
    }
    case GIMatchTableOpcode::CheckOperandDefOpcode: {
      if (Applying) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generated match table performs a predicate after an action");
      }
      auto Matched = matchesOperandDefOpcode(Instruction, Function, Step);
      if (!Matched)
        return std::unexpected(Matched.error());
      if (!*Matched)
        return std::optional<MatchedRule>{};
      break;
    }
    case GIMatchTableOpcode::CheckOperandDefOperandConstantRanges: {
      if (Applying) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generated match table performs a predicate after an action");
      }
      auto Matched = matchesOperandDefOperandConstantRanges(Table, Instruction, Function, Step);
      if (!Matched)
        return std::unexpected(Matched.error());
      if (!*Matched)
        return std::optional<MatchedRule>{};
      break;
    }
    case GIMatchTableOpcode::CaptureDefOperand: {
      if (Applying || Complete || Step.Value == 0 || hasCapturedOperand(Result, Step.Value)) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generated match table has an invalid def-use capture");
      }
      const MachineInstr *Definition = operandDefinition(Instruction, Function, Step.OperandIndex);
      const MachineOperand *Operand =
          Definition == nullptr ? nullptr : Definition->operandAt(Step.Auxiliary);
      if (Operand == nullptr || Operand->isImplicit())
        return std::optional<MatchedRule>{};
      Result.CapturedOperands.push_back({
          .ID = Step.Value,
          .Operand = Operand->cloneDetached(),
      });
      break;
    }
    case GIMatchTableOpcode::CreateTemporary: {
      if (Complete || Step.Value == 0 || hasTemporary(Result, Step.Value)) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generated match table has an invalid temporary");
      }
      auto Type = operandType(Instruction, Function, Step.OperandIndex);
      if (!Type)
        return std::unexpected(Type.error());
      if (!*Type) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generated match table temporary has no type source");
      }
      Applying = true;
      Result.Temporaries.push_back({
          .ID = Step.Value,
          .TypeFromOperand = Step.OperandIndex,
      });
      break;
    }
    case GIMatchTableOpcode::MutateOpcode:
      if (Complete || Step.Value == 0 || !Result.Instructions.empty()) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generated match table has an invalid root mutation");
      }
      Applying = true;
      Result.Instructions.push_back({
          .ResultOpcode = Opcode::fromValue(Step.Value),
          .ReplacesRoot = true,
          .Operands = {},
      });
      break;
    case GIMatchTableOpcode::BuildInstruction:
      if (Complete || Step.Value == 0 || Result.Instructions.empty()) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generated match table has an invalid instruction action");
      }
      Applying = true;
      Result.Instructions.push_back({
          .ResultOpcode = Opcode::fromValue(Step.Value),
          .ReplacesRoot = false,
          .Operands = {},
      });
      break;
    case GIMatchTableOpcode::RenderOperand:
      if (Complete || Result.Instructions.empty() ||
          Instruction.operandAt(Step.OperandIndex) == nullptr ||
          Instruction.operandAt(Step.OperandIndex)->isImplicit()) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generated match table has an invalid operand renderer");
      }
      Result.Instructions.back().Operands.push_back({
          .SourceKind = MatchedRule::RenderedOperand::Kind::RootOperand,
          .Value = Step.OperandIndex,
          .ComplexRenderer = {},
      });
      break;
    case GIMatchTableOpcode::RenderComplexOperand:
      if (Complete || Result.Instructions.empty() || Step.Symbol.empty() ||
          Instruction.operandAt(Step.OperandIndex) == nullptr ||
          Instruction.operandAt(Step.OperandIndex)->isImplicit()) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generated match table has an invalid complex operand renderer");
      }
      Result.Instructions.back().Operands.push_back({
          .SourceKind = MatchedRule::RenderedOperand::Kind::ComplexOperand,
          .Value = Step.OperandIndex,
          .ComplexRenderer = Step.Symbol,
      });
      break;
    case GIMatchTableOpcode::RenderCapturedOperand:
      if (Complete || Result.Instructions.empty() || Step.Value == 0 ||
          !hasCapturedOperand(Result, Step.Value)) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generated match table has an invalid captured operand renderer");
      }
      Result.Instructions.back().Operands.push_back({
          .SourceKind = MatchedRule::RenderedOperand::Kind::CapturedOperand,
          .Value = Step.Value,
          .ComplexRenderer = {},
      });
      break;
    case GIMatchTableOpcode::RenderPhysicalRegister:
      if (Complete || Result.Instructions.empty() || Step.Value == 0) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generated match table has an invalid physical register renderer");
      }
      Result.Instructions.back().Operands.push_back({
          .SourceKind = MatchedRule::RenderedOperand::Kind::PhysicalRegister,
          .Value = Step.Value,
          .Flags = Step.Auxiliary,
          .ComplexRenderer = {},
      });
      break;
    case GIMatchTableOpcode::RenderTemporary:
      if (Complete || Result.Instructions.empty() || !hasTemporary(Result, Step.Value)) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generated match table has an invalid temporary renderer");
      }
      Result.Instructions.back().Operands.push_back({
          .SourceKind = MatchedRule::RenderedOperand::Kind::Temporary,
          .Value = Step.Value,
          .Flags = Step.Auxiliary,
          .ComplexRenderer = {},
      });
      break;
    case GIMatchTableOpcode::RenderCapturedImmediate: {
      if (Complete || Result.Instructions.empty() ||
          Step.Value > static_cast<std::uint32_t>(GIMatchImmediateTransform::PlusOne)) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generated match table has an invalid captured immediate renderer");
      }
      auto Value = operandDefConstantValue(Instruction, Function, Step.OperandIndex);
      if (!Value)
        return std::unexpected(Value.error());
      if (!*Value) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generated match table lost a captured constant");
      }
      Result.Instructions.back().Operands.push_back({
          .SourceKind = MatchedRule::RenderedOperand::Kind::CapturedImmediate,
          .Value = Step.OperandIndex,
          .ComplexRenderer = {},
          .Transform = static_cast<GIMatchImmediateTransform>(Step.Value),
      });
      break;
    }
    case GIMatchTableOpcode::RenderImmediate:
      if (Complete || Result.Instructions.empty()) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generated match table has an invalid immediate renderer");
      }
      Result.Instructions.back().Operands.push_back({
          .SourceKind = MatchedRule::RenderedOperand::Kind::Immediate,
          .ComplexRenderer = {},
          .Immediate = Step.Immediate,
      });
      break;
    case GIMatchTableOpcode::EraseOperandDefinition: {
      if (Complete || Result.Instructions.empty()) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generated match table has an invalid erased definition");
      }
      const MachineOperand *Operand = Instruction.operandAt(Step.OperandIndex);
      const Register *Reg = Operand == nullptr ? nullptr : Operand->getIf<Register>();
      if (Reg == nullptr || !Reg->isVirtual()) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generated match table erases a non-virtual operand definition");
      }
      Result.EraseOperandDefinitions.push_back(*Reg);
      break;
    }
    case GIMatchTableOpcode::Complete:
      if (Complete || Result.Instructions.empty() || Index + 1 != First + Count) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generated match table has an invalid completion action");
      }
      Complete = true;
      break;
    }
  }

  if (!Complete || Result.Instructions.empty() || !Result.Instructions.front().ReplacesRoot) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "generated match table rule does not complete rendering");
  }
  return std::optional<MatchedRule>(std::move(Result));
}

template <typename ComplexRenderer>
[[nodiscard]] Expected<bool>
complexRenderersMatch(const MatchedRule &Rule, const MachineInstr &Instruction,
                      const MachineFunction &Function, const TargetDescription &Target,
                      ComplexRenderer &&RenderComplex) {
  for (const MatchedRule::Instruction &Action : Rule.Instructions) {
    for (const MatchedRule::RenderedOperand &Rendered : Action.Operands) {
      if (Rendered.SourceKind != MatchedRule::RenderedOperand::Kind::ComplexOperand)
        continue;
      auto Complex =
          RenderComplex(Rendered.ComplexRenderer, Instruction, Function, Target, Rendered.Value);
      if (!Complex)
        return std::unexpected(Complex.error());
      if (!*Complex)
        return false;
    }
  }
  return true;
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

struct MaterializedTemporary final {
  std::uint32_t ID = 0;
  Register Reg;
};

struct RegisterConstraintSnapshot final {
  Register Reg;
  std::optional<RegisterBankID> Bank;
  std::optional<RegisterClassID> RegClass;
};

[[nodiscard]] const MaterializedTemporary *
findTemporary(const std::vector<MaterializedTemporary> &Temporaries, std::uint32_t ID) {
  const auto Position = std::ranges::find(Temporaries, ID, &MaterializedTemporary::ID);
  return Position == Temporaries.end() ? nullptr : &*Position;
}

[[nodiscard]] Expected<void>
eraseTemporaries(MachineRegisterInfo &Registers,
                 const std::vector<MaterializedTemporary> &Temporaries) {
  for (auto Iterator = Temporaries.rbegin(); Iterator != Temporaries.rend(); ++Iterator) {
    if (!Registers.isKnownVirtualRegister(Iterator->Reg))
      continue;
    auto Erased = Registers.eraseVirtualRegister(Iterator->Reg);
    if (!Erased)
      return std::unexpected(Erased.error());
  }
  return {};
}

[[nodiscard]] Expected<void> eraseRegisters(MachineRegisterInfo &Registers,
                                            const std::vector<Register> &RegistersToErase) {
  for (auto Iterator = RegistersToErase.rbegin(); Iterator != RegistersToErase.rend(); ++Iterator) {
    if (!Registers.isKnownVirtualRegister(*Iterator))
      continue;
    auto Erased = Registers.eraseVirtualRegister(*Iterator);
    if (!Erased)
      return std::unexpected(Erased.error());
  }
  return {};
}

[[nodiscard]] Expected<void>
restoreRegisterConstraints(MachineRegisterInfo &Registers,
                           const std::vector<RegisterConstraintSnapshot> &Snapshots) {
  for (auto Iterator = Snapshots.rbegin(); Iterator != Snapshots.rend(); ++Iterator) {
    if (!Registers.isKnownVirtualRegister(Iterator->Reg))
      continue;
    if (Iterator->Bank) {
      auto Restored = Registers.setRegisterBank(Iterator->Reg, *Iterator->Bank);
      if (!Restored)
        return std::unexpected(Restored.error());
    } else {
      auto Cleared = Registers.clearRegisterBank(Iterator->Reg);
      if (!Cleared)
        return std::unexpected(Cleared.error());
    }
    if (Iterator->RegClass) {
      auto Restored = Registers.setRegisterClass(Iterator->Reg, *Iterator->RegClass);
      if (!Restored)
        return std::unexpected(Restored.error());
    } else {
      auto Cleared = Registers.clearRegisterClass(Iterator->Reg);
      if (!Cleared)
        return std::unexpected(Cleared.error());
    }
  }
  return {};
}

[[nodiscard]] Expected<void>
constrainRenderedInstruction(const InstructionDescriptor &Descriptor,
                             std::span<const MachineOperand> Operands, MachineFunction &Function,
                             const TargetDescription &Target,
                             std::vector<RegisterConstraintSnapshot> &Snapshots) {
  if (Descriptor.Operands.empty() && !Operands.empty()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "generated match table result has no operand descriptors");
  }

  MachineRegisterInfo &Registers = Function.registerInfo();
  const auto Remember = [&Snapshots](Register Reg, const VirtualRegisterInfo &Info) {
    if (std::ranges::find(Snapshots, Reg, &RegisterConstraintSnapshot::Reg) == Snapshots.end())
      Snapshots.push_back({.Reg = Reg, .Bank = Info.Bank, .RegClass = Info.RegClass});
  };

  for (std::size_t Index = 0; Index < Operands.size(); ++Index) {
    const Register *Reg = Operands[Index].getIf<Register>();
    if (Reg == nullptr || !Reg->isVirtual())
      continue;

    const OperandDescriptor &OperandDescriptor = Index < Descriptor.Operands.size()
                                                     ? Descriptor.Operands[Index]
                                                     : Descriptor.Operands.back();
    if (!OperandDescriptor.RegBank && !OperandDescriptor.RegClass)
      continue;

    auto Info = Registers.virtualRegisterInfo(*Reg);
    if (!Info)
      return std::unexpected(Info.error());
    if (OperandDescriptor.RegBank) {
      if (Info->get().Bank && *Info->get().Bank != *OperandDescriptor.RegBank) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generated match table result has an incompatible register bank");
      }
      if (!Info->get().Bank) {
        Remember(*Reg, Info->get());
        auto SetBank = Registers.setRegisterBank(*Reg, *OperandDescriptor.RegBank);
        if (!SetBank)
          return std::unexpected(SetBank.error());
      }
    }
    if (OperandDescriptor.RegClass) {
      if (Info->get().RegClass &&
          !Target.registers().isSubclass(*Info->get().RegClass, *OperandDescriptor.RegClass)) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generated match table result has an incompatible register class");
      }
      if (!Info->get().RegClass) {
        Remember(*Reg, Info->get());
        auto SetClass = Registers.setRegisterClass(*Reg, *OperandDescriptor.RegClass);
        if (!SetClass)
          return std::unexpected(SetClass.error());
      }
    }
  }
  return {};
}

[[nodiscard]] Expected<MachineOperand>
renderOperand(const MatchedRule::RenderedOperand &Rendered, const MachineInstr &Instruction,
              const MachineFunction &Function, const TargetDescription &Target,
              const std::vector<MaterializedTemporary> &Temporaries,
              const std::vector<MatchedRule::CapturedOperand> &CapturedOperands,
              std::size_t OriginalExplicitCount) {
  switch (Rendered.SourceKind) {
  case MatchedRule::RenderedOperand::Kind::RootOperand:
    if (Rendered.Value >= OriginalExplicitCount) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "generated match table renders an implicit or missing operand");
    }
    return Instruction.operandAt(Rendered.Value)->cloneDetached();
  case MatchedRule::RenderedOperand::Kind::ComplexOperand:
    return makeUnexpected(ErrorCode::VerificationError,
                          "generated match table did not resolve a complex operand renderer");
  case MatchedRule::RenderedOperand::Kind::CapturedOperand: {
    const MatchedRule::CapturedOperand *Captured =
        findCapturedOperand(CapturedOperands, Rendered.Value);
    if (Captured == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "generated match table renders an unknown captured operand");
    }
    return Captured->Operand.cloneDetached();
  }
  case MatchedRule::RenderedOperand::Kind::PhysicalRegister: {
    auto Physical = PhysicalRegister::create(Rendered.Value);
    if (!Physical || Target.registers().physicalRegister(*Physical) == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "generated match table renders an unknown physical register");
    }
    auto Flags = registerFlagsFromRaw(Rendered.Flags);
    if (!Flags)
      return std::unexpected(Flags.error());
    return MachineOperand::registerOperand(Register::fromPhysical(*Physical), *Flags);
  }
  case MatchedRule::RenderedOperand::Kind::Temporary: {
    const MaterializedTemporary *Temporary = findTemporary(Temporaries, Rendered.Value);
    if (Temporary == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "generated match table renders an unknown temporary");
    }
    auto Flags = registerFlagsFromRaw(Rendered.Flags);
    if (!Flags)
      return std::unexpected(Flags.error());
    return MachineOperand::registerOperand(Temporary->Reg, *Flags);
  }
  case MatchedRule::RenderedOperand::Kind::CapturedImmediate: {
    auto Captured = operandDefConstantValue(Instruction, Function, Rendered.Value);
    if (!Captured)
      return std::unexpected(Captured.error());
    if (!*Captured) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "generated match table lost a captured constant");
    }
    std::int64_t Value = **Captured;
    switch (Rendered.Transform) {
    case GIMatchImmediateTransform::Identity:
      break;
    case GIMatchImmediateTransform::Negate:
      if (Value == std::numeric_limits<std::int64_t>::min()) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generated match table immediate negation overflows");
      }
      Value = -Value;
      break;
    case GIMatchImmediateTransform::PlusOne:
      if (Value == std::numeric_limits<std::int64_t>::max()) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generated match table immediate increment overflows");
      }
      ++Value;
      break;
    }
    return MachineOperand::immediate(Value);
  }
  case MatchedRule::RenderedOperand::Kind::Immediate:
    return MachineOperand::immediate(Rendered.Immediate);
  }
  return makeUnexpected(ErrorCode::VerificationError,
                        "generated match table has an invalid operand renderer");
}

template <typename Finalizer, typename ComplexRenderer>
[[nodiscard]] Expected<void> applyRule(const MatchedRule &Rule, MachineInstr &Instruction,
                                       MachineFunction &Function, const TargetDescription &Target,
                                       Finalizer &&Finalize, ComplexRenderer &&RenderComplex) {
  const InstructionDescriptor *InputDescriptor = Target.instructionDescriptor(Instruction.opcode());
  if (InputDescriptor == nullptr) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "generated match table input has no instruction descriptor");
  }
  const MachineInstrFlags InputFlags = Instruction.flags();
  const bool InputMayRaiseFPException =
      InputDescriptor->Flags.has(InstructionFlag::MayRaiseFPException);
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

  std::vector<MaterializedTemporary> Temporaries;
  Temporaries.reserve(Rule.Temporaries.size());
  for (const MatchedRule::Temporary &Temporary : Rule.Temporaries) {
    const MachineOperand *SourceOperand = Instruction.operandAt(Temporary.TypeFromOperand);
    const Register *Source = SourceOperand == nullptr ? nullptr : SourceOperand->getIf<Register>();
    if (Source == nullptr || !Source->isVirtual()) {
      auto Cleanup = eraseTemporaries(Function.registerInfo(), Temporaries);
      if (!Cleanup)
        return std::unexpected(Cleanup.error());
      return makeUnexpected(ErrorCode::VerificationError,
                            "generated match table temporary has an invalid type source");
    }
    auto SourceInfo = Function.registerInfo().virtualRegisterInfo(*Source);
    if (!SourceInfo) {
      auto Cleanup = eraseTemporaries(Function.registerInfo(), Temporaries);
      if (!Cleanup)
        return std::unexpected(Cleanup.error());
      return std::unexpected(SourceInfo.error());
    }
    const std::optional<LowLevelType> Type = SourceInfo->get().Type;
    if (!Type) {
      auto Cleanup = eraseTemporaries(Function.registerInfo(), Temporaries);
      if (!Cleanup)
        return std::unexpected(Cleanup.error());
      return makeUnexpected(ErrorCode::VerificationError,
                            "generated match table temporary type source has no type");
    }
    auto Reg = Function.registerInfo().createVirtualRegister(*Type);
    if (!Reg) {
      auto Cleanup = eraseTemporaries(Function.registerInfo(), Temporaries);
      if (!Cleanup)
        return std::unexpected(Cleanup.error());
      return std::unexpected(Reg.error());
    }
    Temporaries.push_back({.ID = Temporary.ID, .Reg = *Reg});
  }

  struct PendingComplexInstruction final {
    Opcode ResultOpcode;
    std::vector<MachineOperand> Operands;
  };

  std::vector<Register> ComplexRegisters;
  std::vector<PendingComplexInstruction> PendingComplexInstructions;
  std::vector<MachineInstr *> AddedInstructions;
  const auto CleanupGeneratedRegisters = [&]() -> Expected<void> {
    auto ErasedTemporaries = eraseTemporaries(Function.registerInfo(), Temporaries);
    if (!ErasedTemporaries)
      return std::unexpected(ErasedTemporaries.error());
    return eraseRegisters(Function.registerInfo(), ComplexRegisters);
  };

  std::vector<std::vector<MachineOperand>> RenderedInstructions;
  RenderedInstructions.reserve(Rule.Instructions.size());
  std::vector<std::optional<std::size_t>> SourceToOutput(OriginalOperandCount);
  for (std::size_t InstructionIndex = 0; InstructionIndex < Rule.Instructions.size();
       ++InstructionIndex) {
    const MatchedRule::Instruction &Planned = Rule.Instructions[InstructionIndex];
    const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Planned.ResultOpcode);
    if (Descriptor == nullptr ||
        (!Descriptor->Variadic && Descriptor->Operands.size() != Planned.Operands.size())) {
      auto Cleanup = CleanupGeneratedRegisters();
      if (!Cleanup)
        return std::unexpected(Cleanup.error());
      return makeUnexpected(ErrorCode::VerificationError,
                            "generated match table result has an incompatible descriptor");
    }
    std::vector<MachineOperand> Operands;
    Operands.reserve(Planned.Operands.size());
    for (const MatchedRule::RenderedOperand &Rendered : Planned.Operands) {
      Expected<MachineOperand> Operand = [&]() -> Expected<MachineOperand> {
        if (Rendered.SourceKind != MatchedRule::RenderedOperand::Kind::ComplexOperand)
          return renderOperand(Rendered, Instruction, Function, Target, Temporaries,
                               Rule.CapturedOperands, OriginalExplicitCount);

        auto Complex =
            RenderComplex(Rendered.ComplexRenderer, Instruction, Function, Target, Rendered.Value);
        if (!Complex)
          return std::unexpected(Complex.error());
        if (!*Complex)
          return makeUnexpected(ErrorCode::VerificationError,
                                "generated match table complex operand unexpectedly did not match");
        if ((*Complex)->Operand && (*Complex)->Instruction)
          return makeUnexpected(ErrorCode::VerificationError,
                                "generated match table complex operand has two renderings");
        if ((*Complex)->Operand)
          return std::move(*(*Complex)->Operand);
        if (!(*Complex)->Instruction)
          return makeUnexpected(ErrorCode::VerificationError,
                                "generated match table complex operand has no rendering");

        GIMatchComplexInstruction &Plan = *(*Complex)->Instruction;
        if (!Plan.ResultType.isValid())
          return makeUnexpected(ErrorCode::VerificationError,
                                "generated match table complex instruction has no result type");
        auto Result = Function.registerInfo().createVirtualRegister(Plan.ResultType);
        if (!Result)
          return std::unexpected(Result.error());
        ComplexRegisters.push_back(*Result);
        auto Definition = MachineOperand::registerOperand(*Result, {RegisterOperandFlag::Define});
        if (!Definition)
          return std::unexpected(Definition.error());
        std::vector<MachineOperand> PlannedOperands;
        PlannedOperands.reserve(Plan.Operands.size() + 1U);
        PlannedOperands.push_back(std::move(*Definition));
        for (MachineOperand &PlanOperand : Plan.Operands)
          PlannedOperands.push_back(std::move(PlanOperand));
        PendingComplexInstructions.push_back({
            .ResultOpcode = Plan.ResultOpcode,
            .Operands = std::move(PlannedOperands),
        });
        return MachineOperand::registerOperand(*Result, {RegisterOperandFlag::Use});
      }();
      if (!Operand) {
        auto Cleanup = CleanupGeneratedRegisters();
        if (!Cleanup)
          return std::unexpected(Cleanup.error());
        return std::unexpected(Operand.error());
      }
      if (InstructionIndex == 0 &&
          Rendered.SourceKind == MatchedRule::RenderedOperand::Kind::RootOperand) {
        if (SourceToOutput[Rendered.Value] &&
            Instruction.operandAt(Rendered.Value)->tiedOperandIndex()) {
          auto Cleanup = CleanupGeneratedRegisters();
          if (!Cleanup)
            return std::unexpected(Cleanup.error());
          return makeUnexpected(ErrorCode::VerificationError,
                                "generated match table duplicates a tied operand");
        }
        SourceToOutput[Rendered.Value] = Operands.size();
      }
      Operands.push_back(std::move(*Operand));
    }
    RenderedInstructions.push_back(std::move(Operands));
  }

  std::vector<RegisterConstraintSnapshot> ConstraintSnapshots;
  bool RootModified = false;
  const auto Rollback = [&](Error Failure) -> Expected<void> {
    for (auto Iterator = AddedInstructions.rbegin(); Iterator != AddedInstructions.rend();
         ++Iterator) {
      if ((*Iterator)->parent() == nullptr)
        continue;
      auto Erased = eraseInstr(**Iterator, Function.registerInfo(), Target);
      if (!Erased)
        return std::unexpected(Erased.error());
    }
    if (RootModified) {
      auto Restored = restoreInstruction(Instruction, OriginalOpcode, OriginalOperands, Ties);
      if (!Restored)
        return std::unexpected(Restored.error());
    }
    auto RestoredConstraints =
        restoreRegisterConstraints(Function.registerInfo(), ConstraintSnapshots);
    if (!RestoredConstraints)
      return std::unexpected(RestoredConstraints.error());
    auto ErasedGenerated = CleanupGeneratedRegisters();
    if (!ErasedGenerated)
      return std::unexpected(ErasedGenerated.error());
    return std::unexpected(std::move(Failure));
  };

  for (const PendingComplexInstruction &Pending : PendingComplexInstructions) {
    const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Pending.ResultOpcode);
    if (Descriptor == nullptr ||
        (!Descriptor->Variadic && Descriptor->Operands.size() != Pending.Operands.size())) {
      return Rollback(Error(ErrorCode::VerificationError,
                            "generated match table complex renderer has an incompatible result"));
    }
    auto Constraint = constrainRenderedInstruction(*Descriptor, Pending.Operands, Function, Target,
                                                   ConstraintSnapshots);
    if (!Constraint)
      return Rollback(Constraint.error());
  }

  for (std::size_t Index = 0; Index < Rule.Instructions.size(); ++Index) {
    const InstructionDescriptor *Descriptor =
        Target.instructionDescriptor(Rule.Instructions[Index].ResultOpcode);
    if (Descriptor == nullptr)
      return Rollback(Error(ErrorCode::VerificationError,
                            "generated match table selects an unknown instruction opcode"));
    auto Constraint = constrainRenderedInstruction(*Descriptor, RenderedInstructions[Index],
                                                   Function, Target, ConstraintSnapshots);
    if (!Constraint)
      return Rollback(Constraint.error());
  }

  MachineIRBuilder Builder(Function, Target);
  MachineInstr *ComplexAnchor = nullptr;
  for (PendingComplexInstruction &Pending : PendingComplexInstructions) {
    auto Point = ComplexAnchor == nullptr ? Builder.setInsertPointBefore(Instruction)
                                          : Builder.setInsertPointAfter(*ComplexAnchor);
    if (!Point)
      return Rollback(Point.error());
    auto Built = Builder.buildInstr(Pending.ResultOpcode);
    if (!Built)
      return Rollback(Built.error());
    for (MachineOperand &Operand : Pending.Operands) {
      auto Added = Built->addOperand(std::move(Operand));
      if (!Added)
        return Rollback(Added.error());
    }
    auto Complete = Built->build();
    if (!Complete)
      return Rollback(Complete.error());
    AddedInstructions.push_back(*Complete);
    auto Finalized = Finalize(**Complete, InputFlags, InputMayRaiseFPException);
    if (!Finalized)
      return Rollback(Finalized.error());
    ComplexAnchor = *Complete;
  }

  MachineInstr *Anchor = &Instruction;
  for (std::size_t Index = 1; Index < Rule.Instructions.size(); ++Index) {
    auto Point = Builder.setInsertPointAfter(*Anchor);
    if (!Point)
      return Rollback(Point.error());
    auto Pending = Builder.buildInstr(Rule.Instructions[Index].ResultOpcode);
    if (!Pending)
      return Rollback(Pending.error());
    for (MachineOperand &Operand : RenderedInstructions[Index]) {
      auto Added = Pending->addOperand(std::move(Operand));
      if (!Added)
        return Rollback(Added.error());
    }
    auto Built = Pending->build();
    if (!Built)
      return Rollback(Built.error());
    AddedInstructions.push_back(*Built);
    auto Finalized = Finalize(**Built, InputFlags, InputMayRaiseFPException);
    if (!Finalized)
      return Rollback(Finalized.error());
    Anchor = *Built;
  }

  const MatchedRule::Instruction &RootAction = Rule.Instructions.front();
  RootModified = true;
  auto SetOpcode = Instruction.setOpcode(RootAction.ResultOpcode);
  if (!SetOpcode)
    return Rollback(SetOpcode.error());
  while (Instruction.operandCount() != 0) {
    auto Erased = Instruction.eraseOperand(Instruction.operandCount() - 1U);
    if (!Erased)
      return Rollback(Erased.error());
  }
  for (MachineOperand &Operand : RenderedInstructions.front()) {
    auto Added = Instruction.addOperand(std::move(Operand));
    if (!Added)
      return Rollback(Added.error());
  }
  for (std::size_t Index = OriginalExplicitCount; Index < OriginalOperandCount; ++Index) {
    SourceToOutput[Index] = Instruction.operandCount();
    auto Added = Instruction.addOperand(OriginalOperands[Index].cloneDetached());
    if (!Added)
      return Rollback(Added.error());
  }
  for (const auto [OldDef, OldUse] : Ties) {
    const std::optional<std::size_t> &NewDef = SourceToOutput[OldDef];
    const std::optional<std::size_t> &NewUse = SourceToOutput[OldUse];
    if (!NewDef || !NewUse)
      continue;
    auto Tied = Instruction.tieOperands(*NewDef, *NewUse);
    if (!Tied)
      return Rollback(Tied.error());
  }

  for (const Register Reg : Rule.EraseOperandDefinitions) {
    MachineInstr *Definition = Function.registerInfo().getVRegDef(Reg);
    if (Definition == nullptr || Definition == &Instruction ||
        !isTriviallyDead(*Definition, Function.registerInfo(), Target)) {
      continue;
    }
    auto Erased = eraseInstr(*Definition, Function.registerInfo(), Target);
    if (!Erased)
      return Rollback(Erased.error());
  }
  return {};
}

} // namespace

Expected<void> GIMatchTableExecutor::finalizeGeneratedInstruction(MachineInstr &, MachineFunction &,
                                                                  const TargetDescription &,
                                                                  MachineInstrFlags, bool) const {
  return {};
}

Expected<bool> GIMatchTableExecutor::matchesTargetPredicate(std::string_view Name,
                                                            const MachineInstr &,
                                                            const MachineFunction &,
                                                            const TargetDescription &) const {
  return makeUnexpected(ErrorCode::VerificationError,
                        "generated match table uses unknown target predicate '" +
                            std::string(Name) + "'");
}

Expected<std::optional<GIMatchComplexOperand>>
GIMatchTableExecutor::renderComplexOperand(std::string_view, const MachineInstr &,
                                           const MachineFunction &, const TargetDescription &,
                                           std::uint32_t) const {
  return std::optional<GIMatchComplexOperand>{};
}

Expected<bool> GIMatchTableExecutor::match(const GIMatchTable &Table,
                                           const MachineInstr &Instruction,
                                           const MachineFunction &Function,
                                           const TargetDescription &Target) const {
  for (const GIMatchTableRule &Rule : Table.Rules) {
    auto Match = matchRule(Table, Rule, Instruction, Function, Target,
                           [this](std::string_view Name, const MachineInstr &Input,
                                  const MachineFunction &InputFunction,
                                  const TargetDescription &InputTarget) {
                             return matchesTargetPredicate(Name, Input, InputFunction, InputTarget);
                           });
    if (!Match)
      return std::unexpected(Match.error());
    if (!*Match)
      continue;
    auto Complex = complexRenderersMatch(
        **Match, Instruction, Function, Target,
        [this](std::string_view Name, const MachineInstr &Input,
               const MachineFunction &InputFunction, const TargetDescription &InputTarget,
               std::uint32_t OperandIndex) {
          return renderComplexOperand(Name, Input, InputFunction, InputTarget, OperandIndex);
        });
    if (!Complex)
      return std::unexpected(Complex.error());
    if (!*Complex)
      continue;
    for (const MatchedRule::Instruction &Action : (*Match)->Instructions) {
      if (Target.instructionDescriptor(Action.ResultOpcode) == nullptr) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generated match table selects an unknown instruction opcode");
      }
    }
    return true;
  }
  return false;
}

Expected<bool> GIMatchTableExecutor::select(const GIMatchTable &Table, MachineInstr &Instruction,
                                            MachineFunction &Function,
                                            const TargetDescription &Target) const {
  for (const GIMatchTableRule &Rule : Table.Rules) {
    auto Match = matchRule(Table, Rule, Instruction, Function, Target,
                           [this](std::string_view Name, const MachineInstr &Input,
                                  const MachineFunction &InputFunction,
                                  const TargetDescription &InputTarget) {
                             return matchesTargetPredicate(Name, Input, InputFunction, InputTarget);
                           });
    if (!Match)
      return std::unexpected(Match.error());
    if (!*Match)
      continue;
    auto Complex = complexRenderersMatch(
        **Match, Instruction, Function, Target,
        [this](std::string_view Name, const MachineInstr &Input,
               const MachineFunction &InputFunction, const TargetDescription &InputTarget,
               std::uint32_t OperandIndex) {
          return renderComplexOperand(Name, Input, InputFunction, InputTarget, OperandIndex);
        });
    if (!Complex)
      return std::unexpected(Complex.error());
    if (!*Complex)
      continue;
    auto Applied = applyRule(
        **Match, Instruction, Function, Target,
        [this, &Function, &Target](MachineInstr &Output, MachineInstrFlags InputFlags,
                                   bool InputMayRaiseFPException) {
          return finalizeGeneratedInstruction(Output, Function, Target, InputFlags,
                                              InputMayRaiseFPException);
        },
        [this](std::string_view Name, const MachineInstr &Input,
               const MachineFunction &InputFunction, const TargetDescription &InputTarget,
               std::uint32_t OperandIndex) {
          return renderComplexOperand(Name, Input, InputFunction, InputTarget, OperandIndex);
        });
    if (!Applied)
      return std::unexpected(Applied.error());
    return true;
  }
  return false;
}

} // namespace rcc
