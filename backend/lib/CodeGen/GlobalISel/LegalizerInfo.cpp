// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/GlobalISel/LegalizerInfo.h"

#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/TargetDescription.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <tuple>
#include <utility>
#include <vector>

namespace rcc {
namespace {

[[nodiscard]] LegalityPredicate typeInSet(std::size_t TypeIndex, std::vector<LowLevelType> Types) {
  return [TypeIndex, Types = std::move(Types)](const LegalizationQuery &Query) {
    return TypeIndex < Query.Types.size() &&
           std::ranges::find(Types, Query.Types[TypeIndex]) != Types.end();
  };
}

[[nodiscard]] LegalityPredicate
typePairInSet(std::vector<std::pair<LowLevelType, LowLevelType>> Types) {
  return [Types = std::move(Types)](const LegalizationQuery &Query) {
    return Query.Types.size() >= 2 &&
           std::ranges::find(Types, std::pair{Query.Types[0], Query.Types[1]}) != Types.end();
  };
}

[[nodiscard]] LegalityPredicate
typeTripleInSet(std::vector<std::tuple<LowLevelType, LowLevelType, LowLevelType>> Types) {
  return [Types = std::move(Types)](const LegalizationQuery &Query) {
    return Query.Types.size() >= 3 &&
           std::ranges::find(Types, std::tuple{Query.Types[0], Query.Types[1], Query.Types[2]}) !=
               Types.end();
  };
}

[[nodiscard]] LowLevelType scalarType(std::uint32_t BitWidth) {
  auto Type = LowLevelType::makeCategoryNeutralScalar(BitWidth);
  return Type ? *Type : LowLevelType{};
}

[[nodiscard]] std::uint32_t nextPowerOfTwo(std::uint32_t Width, std::uint32_t MinimumBitWidth) {
  return std::max(std::bit_ceil(Width), MinimumBitWidth);
}

[[nodiscard]] bool memoryRuleMatches(const LegalizationQuery &Query,
                                     const LegalizeMemoryRule &Rule) {
  if (Query.Types.size() < 2 || Query.MemoryOperands.size() != 1 ||
      Query.Types[0] != Rule.ValueType || Query.Types[1] != Rule.AddressType)
    return false;
  const LegalizationMemoryDesc &Memory = Query.MemoryOperands.front();
  if (Memory.AlignmentInBits < Rule.MinimumAlignmentInBits)
    return false;
  if (Memory.Type)
    return Memory.Type->minimumBitWidth() == Rule.MemoryType.minimumBitWidth();
  return Memory.Size.isKnown() && !Memory.Size.isScalable() &&
         Memory.Size.minimumBytes() * 8U == Rule.MemoryType.scalarBitWidth();
}

} // namespace

LegalizeRule::LegalizeRule(LegalityPredicate Predicate, LegalizationAction Action,
                           LegalizeMutation Mutation)
    : Predicate(std::move(Predicate)), Action(Action), Mutation(std::move(Mutation)) {}

bool LegalizeRule::matches(const LegalizationQuery &Query) const {
  return Predicate(Query);
}

LegalizationDecision LegalizeRule::apply(const LegalizationQuery &Query) const {
  if (!Mutation)
    return {.Action = Action, .TypeIndex = 0, .NewType = {}};
  auto [TypeIndex, NewType] = Mutation(Query);
  return {.Action = Action, .TypeIndex = TypeIndex, .NewType = NewType};
}

LegalizeRuleSet &LegalizeRuleSet::actionIf(LegalizationAction Action, LegalityPredicate Predicate,
                                           LegalizeMutation Mutation) {
  Rules.emplace_back(std::move(Predicate), Action, std::move(Mutation));
  return *this;
}

LegalizeRuleSet &LegalizeRuleSet::actionFor(LegalizationAction Action,
                                            std::initializer_list<LowLevelType> Types) {
  return actionIf(Action, typeInSet(0, std::vector<LowLevelType>(Types)));
}

LegalizeRuleSet &
LegalizeRuleSet::actionFor(LegalizationAction Action,
                           std::initializer_list<std::pair<LowLevelType, LowLevelType>> Types) {
  return actionIf(Action, typePairInSet(std::vector<std::pair<LowLevelType, LowLevelType>>(Types)));
}

LegalizeRuleSet &LegalizeRuleSet::actionFor(
    LegalizationAction Action,
    std::initializer_list<std::tuple<LowLevelType, LowLevelType, LowLevelType>> Types) {
  return actionIf(
      Action,
      typeTripleInSet(std::vector<std::tuple<LowLevelType, LowLevelType, LowLevelType>>(Types)));
}

LegalizeRuleSet &LegalizeRuleSet::legalIf(LegalityPredicate Predicate) {
  return actionIf(LegalizationAction::Legal, std::move(Predicate));
}

LegalizeRuleSet &LegalizeRuleSet::customIf(LegalityPredicate Predicate) {
  return actionIf(LegalizationAction::Custom, std::move(Predicate));
}

LegalizeRuleSet &LegalizeRuleSet::lowerIf(LegalityPredicate Predicate) {
  return actionIf(LegalizationAction::Lower, std::move(Predicate));
}

LegalizeRuleSet &LegalizeRuleSet::libcallIf(LegalityPredicate Predicate) {
  return actionIf(LegalizationAction::Libcall, std::move(Predicate));
}

LegalizeRuleSet &LegalizeRuleSet::unsupportedIf(LegalityPredicate Predicate) {
  return actionIf(LegalizationAction::Unsupported, std::move(Predicate));
}

LegalizeRuleSet &LegalizeRuleSet::widenScalarIf(LegalityPredicate Predicate,
                                                LegalizeMutation Mutation) {
  return actionIf(LegalizationAction::WidenScalar, std::move(Predicate), std::move(Mutation));
}

LegalizeRuleSet &LegalizeRuleSet::narrowScalarIf(LegalityPredicate Predicate,
                                                 LegalizeMutation Mutation) {
  return actionIf(LegalizationAction::NarrowScalar, std::move(Predicate), std::move(Mutation));
}

LegalizeRuleSet &LegalizeRuleSet::moreElementsIf(LegalityPredicate Predicate,
                                                 LegalizeMutation Mutation) {
  return actionIf(LegalizationAction::MoreElements, std::move(Predicate), std::move(Mutation));
}

LegalizeRuleSet &LegalizeRuleSet::fewerElementsIf(LegalityPredicate Predicate,
                                                  LegalizeMutation Mutation) {
  return actionIf(LegalizationAction::FewerElements, std::move(Predicate), std::move(Mutation));
}

LegalizeRuleSet &LegalizeRuleSet::bitcastIf(LegalityPredicate Predicate,
                                            LegalizeMutation Mutation) {
  return actionIf(LegalizationAction::Bitcast, std::move(Predicate), std::move(Mutation));
}

LegalizeRuleSet &LegalizeRuleSet::legalFor(std::initializer_list<LowLevelType> Types) {
  return actionFor(LegalizationAction::Legal, Types);
}

LegalizeRuleSet &LegalizeRuleSet::legalFor(bool Enabled,
                                           std::initializer_list<LowLevelType> Types) {
  return Enabled ? legalFor(Types) : *this;
}

LegalizeRuleSet &
LegalizeRuleSet::legalFor(std::initializer_list<std::pair<LowLevelType, LowLevelType>> Types) {
  return actionFor(LegalizationAction::Legal, Types);
}

LegalizeRuleSet &
LegalizeRuleSet::legalFor(bool Enabled,
                          std::initializer_list<std::pair<LowLevelType, LowLevelType>> Types) {
  return Enabled ? legalFor(Types) : *this;
}

LegalizeRuleSet &LegalizeRuleSet::legalFor(
    std::initializer_list<std::tuple<LowLevelType, LowLevelType, LowLevelType>> Types) {
  return actionFor(LegalizationAction::Legal, Types);
}

LegalizeRuleSet &LegalizeRuleSet::customFor(std::initializer_list<LowLevelType> Types) {
  return actionFor(LegalizationAction::Custom, Types);
}

LegalizeRuleSet &LegalizeRuleSet::customFor(bool Enabled,
                                            std::initializer_list<LowLevelType> Types) {
  return Enabled ? customFor(Types) : *this;
}

LegalizeRuleSet &
LegalizeRuleSet::customFor(std::initializer_list<std::pair<LowLevelType, LowLevelType>> Types) {
  return actionFor(LegalizationAction::Custom, Types);
}

LegalizeRuleSet &
LegalizeRuleSet::customFor(bool Enabled,
                           std::initializer_list<std::pair<LowLevelType, LowLevelType>> Types) {
  return Enabled ? customFor(Types) : *this;
}

LegalizeRuleSet &LegalizeRuleSet::lowerFor(std::initializer_list<LowLevelType> Types) {
  return actionFor(LegalizationAction::Lower, Types);
}

LegalizeRuleSet &
LegalizeRuleSet::lowerFor(std::initializer_list<std::pair<LowLevelType, LowLevelType>> Types) {
  return actionFor(LegalizationAction::Lower, Types);
}

LegalizeRuleSet &LegalizeRuleSet::libcallFor(std::initializer_list<LowLevelType> Types) {
  return actionFor(LegalizationAction::Libcall, Types);
}

LegalizeRuleSet &LegalizeRuleSet::libcallFor(bool Enabled,
                                             std::initializer_list<LowLevelType> Types) {
  return Enabled ? libcallFor(Types) : *this;
}

LegalizeRuleSet &
LegalizeRuleSet::libcallFor(std::initializer_list<std::pair<LowLevelType, LowLevelType>> Types) {
  return actionFor(LegalizationAction::Libcall, Types);
}

LegalizeRuleSet &
LegalizeRuleSet::libcallFor(bool Enabled,
                            std::initializer_list<std::pair<LowLevelType, LowLevelType>> Types) {
  return Enabled ? libcallFor(Types) : *this;
}

LegalizeRuleSet &
LegalizeRuleSet::legalForTypesWithMemDesc(std::initializer_list<LegalizeMemoryRule> MemoryRules) {
  const std::vector<LegalizeMemoryRule> MemoryRuleList(MemoryRules);
  return legalIf([MemoryRuleList](const LegalizationQuery &Query) {
    return std::ranges::any_of(MemoryRuleList, [&Query](const LegalizeMemoryRule &Rule) {
      return memoryRuleMatches(Query, Rule);
    });
  });
}

LegalizeRuleSet &LegalizeRuleSet::widenScalarToNextPow2(std::size_t TypeIndex,
                                                        std::uint32_t MinimumBitWidth) {
  return widenScalarIf(
      [TypeIndex, MinimumBitWidth](const LegalizationQuery &Query) {
        if (TypeIndex >= Query.Types.size() || !Query.Types[TypeIndex].isCategoryNeutralScalar())
          return false;
        const std::uint32_t Width = Query.Types[TypeIndex].scalarBitWidth();
        return nextPowerOfTwo(Width, MinimumBitWidth) > Width;
      },
      [TypeIndex, MinimumBitWidth](const LegalizationQuery &Query) {
        return std::pair{TypeIndex, scalarType(nextPowerOfTwo(
                                        Query.Types[TypeIndex].scalarBitWidth(), MinimumBitWidth))};
      });
}

LegalizeRuleSet &LegalizeRuleSet::minScalar(std::size_t TypeIndex, LowLevelType MinimumType) {
  return widenScalarIf(
      [TypeIndex, MinimumType](const LegalizationQuery &Query) {
        return TypeIndex < Query.Types.size() && Query.Types[TypeIndex].isCategoryNeutralScalar() &&
               Query.Types[TypeIndex].scalarBitWidth() < MinimumType.scalarBitWidth();
      },
      [TypeIndex, MinimumType](const LegalizationQuery &) {
        return std::pair{TypeIndex, MinimumType};
      });
}

LegalizeRuleSet &LegalizeRuleSet::minScalar(bool Enabled, std::size_t TypeIndex,
                                            LowLevelType MinimumType) {
  return Enabled ? minScalar(TypeIndex, MinimumType) : *this;
}

LegalizeRuleSet &LegalizeRuleSet::maxScalar(std::size_t TypeIndex, LowLevelType MaximumType) {
  return narrowScalarIf(
      [TypeIndex, MaximumType](const LegalizationQuery &Query) {
        return TypeIndex < Query.Types.size() && Query.Types[TypeIndex].isCategoryNeutralScalar() &&
               Query.Types[TypeIndex].scalarBitWidth() > MaximumType.scalarBitWidth();
      },
      [TypeIndex, MaximumType](const LegalizationQuery &) {
        return std::pair{TypeIndex, MaximumType};
      });
}

LegalizeRuleSet &LegalizeRuleSet::maxScalar(bool Enabled, std::size_t TypeIndex,
                                            LowLevelType MaximumType) {
  return Enabled ? maxScalar(TypeIndex, MaximumType) : *this;
}

LegalizeRuleSet &LegalizeRuleSet::clampScalar(std::size_t TypeIndex, LowLevelType MinimumType,
                                              LowLevelType MaximumType) {
  return minScalar(TypeIndex, MinimumType).maxScalar(TypeIndex, MaximumType);
}

LegalizeRuleSet &LegalizeRuleSet::scalarSameSizeAs(std::size_t TypeIndex,
                                                   std::size_t ReferenceTypeIndex) {
  const auto changeToReference = [TypeIndex, ReferenceTypeIndex](const LegalizationQuery &Query) {
    return std::pair{TypeIndex, scalarType(Query.Types[ReferenceTypeIndex].scalarBitWidth())};
  };
  widenScalarIf(
      [TypeIndex, ReferenceTypeIndex](const LegalizationQuery &Query) {
        return TypeIndex < Query.Types.size() && ReferenceTypeIndex < Query.Types.size() &&
               Query.Types[TypeIndex].isCategoryNeutralScalar() &&
               Query.Types[ReferenceTypeIndex].isCategoryNeutralScalar() &&
               Query.Types[TypeIndex].scalarBitWidth() <
                   Query.Types[ReferenceTypeIndex].scalarBitWidth();
      },
      changeToReference);
  return narrowScalarIf(
      [TypeIndex, ReferenceTypeIndex](const LegalizationQuery &Query) {
        return TypeIndex < Query.Types.size() && ReferenceTypeIndex < Query.Types.size() &&
               Query.Types[TypeIndex].isCategoryNeutralScalar() &&
               Query.Types[ReferenceTypeIndex].isCategoryNeutralScalar() &&
               Query.Types[TypeIndex].scalarBitWidth() >
                   Query.Types[ReferenceTypeIndex].scalarBitWidth();
      },
      changeToReference);
}

LegalizeRuleSet &LegalizeRuleSet::lowerIfMemSizeNotByteSizePow2(std::size_t MemoryIndex) {
  return lowerIf([MemoryIndex](const LegalizationQuery &Query) {
    if (MemoryIndex >= Query.MemoryOperands.size())
      return false;
    const MemorySize Size = Query.MemoryOperands[MemoryIndex].Size;
    return !Size.isKnown() || Size.isScalable() || !std::has_single_bit(Size.minimumBytes());
  });
}

LegalizeRuleSet &LegalizeRuleSet::alwaysLegal() {
  return legalIf([](const LegalizationQuery &) { return true; });
}

LegalizeRuleSet &LegalizeRuleSet::lower() {
  return lowerIf([](const LegalizationQuery &) { return true; });
}

LegalizeRuleSet &LegalizeRuleSet::libcall() {
  return libcallIf([](const LegalizationQuery &) { return true; });
}

std::optional<LegalizationDecision> LegalizeRuleSet::apply(const LegalizationQuery &Query) const {
  for (const LegalizeRule &Rule : Rules) {
    if (Rule.matches(Query))
      return Rule.apply(Query);
  }
  return std::nullopt;
}

LegalizeRuleSet &LegalizerInfo::getActionDefinitionsBuilder(Opcode InstructionOpcode) {
  return RuleSets[InstructionOpcode];
}

LegalizeRuleSet &
LegalizerInfo::getActionDefinitionsBuilder(std::initializer_list<Opcode> InstructionOpcodes) {
  const Opcode Representative = *InstructionOpcodes.begin();
  for (const Opcode InstructionOpcode : InstructionOpcodes) {
    if (InstructionOpcode != Representative)
      RuleAliases.emplace(InstructionOpcode, Representative);
  }
  return getActionDefinitionsBuilder(Representative);
}

Expected<LegalizationQuery> LegalizerInfo::buildQuery(const MachineInstr &Instruction,
                                                      const MachineFunction &Function,
                                                      const TargetDescription &Target) {
  const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction.opcode());
  if (Descriptor == nullptr)
    return makeUnexpected(ErrorCode::VerificationError,
                          "legalization query requires a registered instruction opcode");

  std::size_t ExplicitCount = 0;
  while (ExplicitCount < Instruction.operandCount() &&
         !Instruction.operandAt(ExplicitCount)->isImplicit()) {
    ++ExplicitCount;
  }
  auto OperandDescriptors = resolveExplicitOperandDescriptors(*Descriptor, ExplicitCount);
  if (!OperandDescriptors)
    return std::unexpected(OperandDescriptors.error());

  std::size_t TypeCount = 0;
  for (const OperandDescriptor *Operand : *OperandDescriptors) {
    if (Operand->TypeConstraint && Operand->TypeConstraint->EqualityGroup) {
      TypeCount = std::max(TypeCount,
                           static_cast<std::size_t>(*Operand->TypeConstraint->EqualityGroup) + 1U);
    }
  }

  std::vector<std::optional<LowLevelType>> OptionalTypes(TypeCount);
  std::vector<std::vector<std::size_t>> TypeOperands(TypeCount);
  for (std::size_t Index = 0; Index < ExplicitCount; ++Index) {
    const OperandDescriptor &OperandDescriptor = *(*OperandDescriptors)[Index];
    if (!OperandDescriptor.TypeConstraint || !OperandDescriptor.TypeConstraint->EqualityGroup) {
      continue;
    }
    const std::size_t TypeIndex = *OperandDescriptor.TypeConstraint->EqualityGroup;
    const Register *Reg = Instruction.operandAt(Index)->getIf<Register>();
    if (Reg == nullptr || !Reg->isVirtual())
      return makeUnexpected(ErrorCode::VerificationError,
                            "legalization type operand must name a virtual register");
    auto Info = Function.registerInfo().virtualRegisterInfo(*Reg);
    if (!Info)
      return std::unexpected(Info.error());
    if (!Info->get().Type)
      return makeUnexpected(ErrorCode::VerificationError,
                            "legalization type operand has no LowLevelType");
    if (OptionalTypes[TypeIndex] && *OptionalTypes[TypeIndex] != *Info->get().Type)
      return makeUnexpected(ErrorCode::VerificationError,
                            "legalization type operands disagree within one type index");
    OptionalTypes[TypeIndex] = *Info->get().Type;
    TypeOperands[TypeIndex].push_back(Index);
  }

  std::vector<LowLevelType> Types;
  Types.reserve(TypeCount);
  for (const std::optional<LowLevelType> &Type : OptionalTypes) {
    if (!Type)
      return makeUnexpected(ErrorCode::VerificationError,
                            "legalization query contains a sparse type index");
    Types.push_back(*Type);
  }

  std::vector<LegalizationMemoryDesc> MemoryOperands;
  MemoryOperands.reserve(Instruction.memOperands().size());
  for (const MachineMemOperand *Memory : Instruction.memOperands()) {
    const MachineMemOperandDesc &Description = Memory->description();
    MemoryOperands.push_back({Description.MemoryType, Description.Size,
                              Description.Align.bytes() * 8U, Description.AddressSpace,
                              Description.AtomicKind, Description.SuccessOrdering,
                              Description.FailureOrdering, Description.SyncScope});
  }

  LegalizationQuery Query(Instruction.opcode(), std::move(Types), std::move(MemoryOperands));
  Query.TypeOperands = std::move(TypeOperands);
  return Query;
}

Expected<LegalizationDecision> LegalizerInfo::getDecision(const MachineInstr &Instruction,
                                                          const MachineFunction &Function,
                                                          const TargetDescription &Target) const {
  auto Query = buildQuery(Instruction, Function, Target);
  if (!Query)
    return std::unexpected(Query.error());
  return getDecision(*Query);
}

Expected<LegalizationDecision> LegalizerInfo::getDecision(const LegalizationQuery &Query) const {
  if (!isGenericOpcode(Query.InstructionOpcode))
    return LegalizationDecision{.Action = LegalizationAction::Legal, .TypeIndex = 0, .NewType = {}};

  Opcode RuleOpcode = Query.InstructionOpcode;
  if (const auto Alias = RuleAliases.find(RuleOpcode); Alias != RuleAliases.end())
    RuleOpcode = Alias->second;
  const auto RuleSet = RuleSets.find(RuleOpcode);
  if (RuleSet == RuleSets.end())
    return LegalizationDecision{
        .Action = LegalizationAction::Unsupported, .TypeIndex = 0, .NewType = {}};
  const std::optional<LegalizationDecision> Decision = RuleSet->second.apply(Query);
  return Decision.value_or(LegalizationDecision{
      .Action = LegalizationAction::Unsupported, .TypeIndex = 0, .NewType = {}});
}

Expected<bool> LegalizerInfo::isLegal(const LegalizationQuery &Query) const {
  auto Decision = getDecision(Query);
  if (!Decision)
    return std::unexpected(Decision.error());
  return Decision->Action == LegalizationAction::Legal;
}

Expected<bool> LegalizerInfo::isUnsupported(const LegalizationQuery &Query) const {
  auto Decision = getDecision(Query);
  if (!Decision)
    return std::unexpected(Decision.error());
  return Decision->Action == LegalizationAction::Unsupported;
}

GenericOpcode LegalizerInfo::getExtOpcodeForWideningConstant(LowLevelType SmallType) const {
  return SmallType.minimumBitWidth() % 8U == 0 ? GenericOpcode::G_SEXT : GenericOpcode::G_ZEXT;
}

} // namespace rcc
