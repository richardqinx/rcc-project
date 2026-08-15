// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_GLOBALISEL_LEGALIZERINFO_H
#define RCC_CODEGEN_GLOBALISEL_LEGALIZERINFO_H

#include "rcc/CodeGen/LowLevelType.h"
#include "rcc/CodeGen/MachineIRTypes.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rcc {

class MachineFunction;
class MachineInstr;
class TargetDescription;
class LegalizerHelper;
enum class GenericOpcode : std::uint32_t;

enum class LegalizationAction : std::uint8_t {
  Legal,
  Unsupported,
  Custom,
  Lower,
  Libcall,
  WidenScalar,
  NarrowScalar,
  MoreElements,
  FewerElements,
  Bitcast,
  ChangeElementType,
  ChangeAddressSpace,
};

struct LegalizationMemoryDesc final {
  std::optional<LowLevelType> Type;
  MemorySize Size;
  std::uint64_t AlignmentInBits = 0;
  std::uint32_t AddressSpace = 0;
  AtomicOperation Operation = AtomicOperation::None;
  AtomicOrdering SuccessOrdering = AtomicOrdering::NotAtomic;
  AtomicOrdering FailureOrdering = AtomicOrdering::NotAtomic;
  SyncScopeID SyncScope;
};

struct LegalizeMemoryRule final {
  LowLevelType ValueType;
  LowLevelType AddressType;
  LowLevelType MemoryType;
  std::uint64_t MinimumAlignmentInBits = 0;
};

struct LegalizationQuery final {
  Opcode InstructionOpcode;
  std::vector<LowLevelType> Types;
  std::vector<std::vector<std::size_t>> TypeOperands;
  std::vector<LegalizationMemoryDesc> MemoryOperands;

  LegalizationQuery(Opcode InstructionOpcode, std::vector<LowLevelType> Types,
                    std::vector<LegalizationMemoryDesc> MemoryOperands = {})
      : InstructionOpcode(InstructionOpcode), Types(std::move(Types)),
        MemoryOperands(std::move(MemoryOperands)) {}
};

struct LegalizationDecision final {
  LegalizationAction Action = LegalizationAction::Unsupported;
  std::size_t TypeIndex = 0;
  LowLevelType NewType;
};

using LegalityPredicate = std::function<bool(const LegalizationQuery &)>;
using LegalizeMutation =
    std::function<std::pair<std::size_t, LowLevelType>(const LegalizationQuery &)>;

class LegalizeRule final {
public:
  LegalizeRule(LegalityPredicate Predicate, LegalizationAction Action,
               LegalizeMutation Mutation = {});

  [[nodiscard]] bool matches(const LegalizationQuery &Query) const;
  [[nodiscard]] LegalizationDecision apply(const LegalizationQuery &Query) const;

private:
  LegalityPredicate Predicate;
  LegalizationAction Action;
  LegalizeMutation Mutation;
};

class LegalizeRuleSet final {
public:
  LegalizeRuleSet &actionIf(LegalizationAction Action, LegalityPredicate Predicate,
                            LegalizeMutation Mutation = {});
  LegalizeRuleSet &actionFor(LegalizationAction Action, std::initializer_list<LowLevelType> Types);
  LegalizeRuleSet &actionFor(LegalizationAction Action,
                             std::initializer_list<std::pair<LowLevelType, LowLevelType>> Types);
  LegalizeRuleSet &
  actionFor(LegalizationAction Action,
            std::initializer_list<std::tuple<LowLevelType, LowLevelType, LowLevelType>> Types);
  LegalizeRuleSet &legalIf(LegalityPredicate Predicate);
  LegalizeRuleSet &customIf(LegalityPredicate Predicate);
  LegalizeRuleSet &lowerIf(LegalityPredicate Predicate);
  LegalizeRuleSet &libcallIf(LegalityPredicate Predicate);
  LegalizeRuleSet &unsupportedIf(LegalityPredicate Predicate);
  LegalizeRuleSet &widenScalarIf(LegalityPredicate Predicate, LegalizeMutation Mutation);
  LegalizeRuleSet &narrowScalarIf(LegalityPredicate Predicate, LegalizeMutation Mutation);
  LegalizeRuleSet &moreElementsIf(LegalityPredicate Predicate, LegalizeMutation Mutation);
  LegalizeRuleSet &fewerElementsIf(LegalityPredicate Predicate, LegalizeMutation Mutation);
  LegalizeRuleSet &bitcastIf(LegalityPredicate Predicate, LegalizeMutation Mutation);

  LegalizeRuleSet &legalFor(std::initializer_list<LowLevelType> Types);
  LegalizeRuleSet &legalFor(bool Enabled, std::initializer_list<LowLevelType> Types);
  LegalizeRuleSet &legalFor(std::initializer_list<std::pair<LowLevelType, LowLevelType>> Types);
  LegalizeRuleSet &legalFor(bool Enabled,
                            std::initializer_list<std::pair<LowLevelType, LowLevelType>> Types);
  LegalizeRuleSet &
  legalFor(std::initializer_list<std::tuple<LowLevelType, LowLevelType, LowLevelType>> Types);
  LegalizeRuleSet &customFor(std::initializer_list<LowLevelType> Types);
  LegalizeRuleSet &customFor(bool Enabled, std::initializer_list<LowLevelType> Types);
  LegalizeRuleSet &customFor(std::initializer_list<std::pair<LowLevelType, LowLevelType>> Types);
  LegalizeRuleSet &customFor(bool Enabled,
                             std::initializer_list<std::pair<LowLevelType, LowLevelType>> Types);
  LegalizeRuleSet &lowerFor(std::initializer_list<LowLevelType> Types);
  LegalizeRuleSet &lowerFor(std::initializer_list<std::pair<LowLevelType, LowLevelType>> Types);
  LegalizeRuleSet &libcallFor(std::initializer_list<LowLevelType> Types);
  LegalizeRuleSet &libcallFor(bool Enabled, std::initializer_list<LowLevelType> Types);
  LegalizeRuleSet &libcallFor(std::initializer_list<std::pair<LowLevelType, LowLevelType>> Types);
  LegalizeRuleSet &libcallFor(bool Enabled,
                              std::initializer_list<std::pair<LowLevelType, LowLevelType>> Types);
  LegalizeRuleSet &legalForTypesWithMemDesc(std::initializer_list<LegalizeMemoryRule> Rules);

  LegalizeRuleSet &custom();
  LegalizeRuleSet &widenScalarToNextPow2(std::size_t TypeIndex, std::uint32_t MinimumBitWidth = 0);
  LegalizeRuleSet &scalarize(std::size_t TypeIndex);
  LegalizeRuleSet &minScalar(std::size_t TypeIndex, LowLevelType MinimumType);
  LegalizeRuleSet &minScalar(bool Enabled, std::size_t TypeIndex, LowLevelType MinimumType);
  LegalizeRuleSet &maxScalar(std::size_t TypeIndex, LowLevelType MaximumType);
  LegalizeRuleSet &maxScalar(bool Enabled, std::size_t TypeIndex, LowLevelType MaximumType);
  LegalizeRuleSet &clampScalar(std::size_t TypeIndex, LowLevelType MinimumType,
                               LowLevelType MaximumType);
  LegalizeRuleSet &scalarSameSizeAs(std::size_t TypeIndex, std::size_t ReferenceTypeIndex);
  LegalizeRuleSet &lowerIfMemSizeNotByteSizePow2(std::size_t MemoryIndex = 0);
  LegalizeRuleSet &alwaysLegal();
  LegalizeRuleSet &lower();
  LegalizeRuleSet &libcall();

  [[nodiscard]] std::optional<LegalizationDecision> apply(const LegalizationQuery &Query) const;

private:
  std::vector<LegalizeRule> Rules;
};

class LegalizerInfo {
public:
  virtual ~LegalizerInfo() = default;

  [[nodiscard]] static Expected<LegalizationQuery> buildQuery(const MachineInstr &Instruction,
                                                              const MachineFunction &Function,
                                                              const TargetDescription &Target);

  [[nodiscard]] Expected<LegalizationDecision> getDecision(const MachineInstr &Instruction,
                                                           const MachineFunction &Function,
                                                           const TargetDescription &Target) const;

  [[nodiscard]] Expected<LegalizationDecision> getDecision(const LegalizationQuery &Query) const;

  [[nodiscard]] Expected<bool> isLegal(const LegalizationQuery &Query) const;
  [[nodiscard]] Expected<bool> isUnsupported(const LegalizationQuery &Query) const;

  [[nodiscard]] virtual Expected<void>
  legalizeCustom(LegalizerHelper &Helper, MachineInstr &Instruction,
                 const LegalizationDecision &Decision) const = 0;

  // Target intrinsic lowering runs before the ordinary legality action.
  [[nodiscard]] virtual Expected<bool> legalizeIntrinsic(LegalizerHelper &, MachineInstr &) const {
    return true;
  }

  [[nodiscard]] virtual GenericOpcode getExtOpcodeForWideningConstant(LowLevelType SmallType) const;

  [[nodiscard]] virtual bool isSExtCheaperThanZExt(LowLevelType, LowLevelType) const {
    return false;
  }

  // This hook performs read-only target checks before a lowering mutation is committed.
  [[nodiscard]] virtual Expected<void> validateLowering(const MachineInstr &,
                                                        const MachineFunction &,
                                                        const TargetDescription &,
                                                        const LegalizationDecision &) const {
    return {};
  }

protected:
  [[nodiscard]] LegalizeRuleSet &getActionDefinitionsBuilder(Opcode InstructionOpcode);
  [[nodiscard]] LegalizeRuleSet &
  getActionDefinitionsBuilder(std::initializer_list<Opcode> InstructionOpcodes);

private:
  std::unordered_map<Opcode, LegalizeRuleSet> RuleSets;
  std::unordered_map<Opcode, Opcode> RuleAliases;
};

} // namespace rcc

#endif // RCC_CODEGEN_GLOBALISEL_LEGALIZERINFO_H
