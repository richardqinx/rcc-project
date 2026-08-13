// SPDX-License-Identifier: GPL-3.0-or-later

// The private header follows the source-tree ownership of LLVM TableGen Common.
// NOLINTNEXTLINE(llvm-header-guard)
#ifndef RCC_UTILS_TABLEGEN_COMMON_CODEGENTARGET_H
#define RCC_UTILS_TABLEGEN_COMMON_CODEGENTARGET_H

#include "rcc/TableGen/Record.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rcc::targetgen {

struct NormalizedRegisterUnitLane final {
  std::uint32_t UnitID = 0;
  std::uint64_t Lanes = 0;
};

struct NormalizedRegister final {
  std::uint32_t ID = 0;
  std::string Symbol;
  std::string Name;
  std::uint32_t Encoding = 0;
  std::uint32_t DwarfRegister = 0;
  std::vector<NormalizedRegisterUnitLane> Units;
  bool IsConstant = false;
  bool IsArtificial = false;
  bool IsReserved = false;
  std::vector<std::string> AlternativeNames;
};

struct NormalizedRegisterBank final {
  std::uint32_t ID = 0;
  std::string Symbol;
  std::string Name;
  std::uint32_t MaximumSizeInBits = 0;
  std::vector<std::uint32_t> RegisterClasses;
};

struct NormalizedRegisterClass final {
  std::uint32_t ID = 0;
  std::string Symbol;
  std::string Name;
  std::uint32_t BankID = 0;
  std::uint32_t SizeInBits = 0;
  std::uint32_t SpillSizeInBits = 0;
  std::uint32_t SpillAlignmentInBits = 0;
  std::uint64_t Lanes = 0;
  std::vector<std::uint32_t> Registers;
  std::vector<std::uint32_t> AllocationOrder;
  bool Allocatable = true;
  std::uint32_t CopyCost = 1;
};

struct NormalizedRegisterMask final {
  std::string Name;
  std::vector<std::uint32_t> PreservedRegisters;
};

struct NormalizedCallingConvention final {
  std::uint32_t ID = 0;
  std::string Symbol;
  std::string Name;
  std::uint32_t IntegerRegisterClass = 0;
  std::uint32_t FloatingRegisterClass = 0;
  std::vector<std::uint32_t> IntegerArgumentRegisters;
  std::vector<std::uint32_t> FloatingArgumentRegisters;
  std::vector<std::uint32_t> IntegerReturnRegisters;
  std::vector<std::uint32_t> FloatingReturnRegisters;
  std::string PreservedMaskName;
  std::uint32_t StackAlignmentBytes = 0;
  std::uint32_t StackArgumentAlignmentBytes = 0;
  std::uint32_t ShadowSpaceBytes = 0;
  bool SupportsVarArgs = false;
  bool SupportsTailCalls = false;
};

struct NormalizedNamedOperand final {
  std::uint32_t ID = 0;
  std::string Symbol;
  std::string Name;
};

struct NormalizedImmediatePredicate final {
  std::uint32_t ID = 0;
  std::string Symbol;
  std::string Name;
  bool IsSigned = false;
  std::uint32_t Bits = 0;
  std::uint64_t Alignment = 1;
  bool NonZero = false;
  std::vector<std::int64_t> AllowedValues;
};

struct NormalizedSchedulingClass final {
  std::uint32_t ID = 0;
  std::string Symbol;
  std::string Name;
  std::uint32_t MicroOps = 0;
  std::uint32_t Latency = 0;
};

struct NormalizedEncodingCategory final {
  std::uint32_t ID = 0;
  std::string Symbol;
  std::string Name;
  std::uint32_t SizeInBytes = 0;
};

struct NormalizedAssemblyFormat final {
  std::uint32_t ID = 0;
  std::string Symbol;
  std::string Name;
};

struct NormalizedOperand final {
  std::vector<std::string> Kinds;
  std::string RegisterRole;
  std::optional<std::uint32_t> RegisterClass;
  std::optional<std::uint32_t> RegisterBank;
  std::optional<std::uint32_t> ImmediatePredicate;
  std::string MemoryRole;
  std::optional<std::uint32_t> DefaultTiedOperand;
  bool EarlyClobber = false;
  bool Optional = false;
  bool VariadicGroup = false;
  bool AllowSubReg = false;
  std::optional<std::uint32_t> Name;
  std::uint32_t AllowedTargetFlags = 0;
};

struct NormalizedImplicitRegister final {
  std::uint32_t Register = 0;
  bool EarlyClobber = false;
  bool Renamable = false;
};

struct NormalizedInstruction final {
  std::uint32_t ID = 0;
  std::string Symbol;
  std::string Name;
  std::string AssemblyMnemonic;
  std::uint32_t BinaryEncoding = 0;
  std::vector<NormalizedOperand> Operands;
  std::uint32_t NumExplicitDefs = 0;
  bool Variadic = false;
  std::vector<NormalizedImplicitRegister> ImplicitDefs;
  std::vector<NormalizedImplicitRegister> ImplicitUses;
  bool AllowExtraImplicitOperands = false;
  std::vector<std::string> Flags;
  std::uint32_t SchedulingClass = 0;
  std::uint32_t EncodingCategory = 0;
  std::uint32_t SizeInBytes = 0;
  std::uint32_t AssemblyFormat = 0;
  bool RequiresMemoryOperands = false;
  std::string Owner;
  std::string Deadline;
};

struct NormalizedTargetModel final {
  SourceTarget Target;
  std::vector<SourceRegisterUnit> RegisterUnits;
  std::vector<NormalizedRegister> Registers;
  std::vector<NormalizedRegisterBank> RegisterBanks;
  std::vector<NormalizedRegisterClass> RegisterClasses;
  std::vector<NormalizedRegisterMask> RegisterMasks;
  SourceMIROperandFlags MIROperandFlags;
  std::vector<NormalizedCallingConvention> CallingConventions;
  std::vector<NormalizedNamedOperand> NamedOperands;
  std::vector<NormalizedImmediatePredicate> ImmediatePredicates;
  std::vector<NormalizedSchedulingClass> SchedulingClasses;
  std::vector<NormalizedEncodingCategory> EncodingCategories;
  std::vector<NormalizedAssemblyFormat> AssemblyFormats;
  std::vector<NormalizedInstruction> Instructions;
};

[[nodiscard]] std::optional<std::string_view>
operandKindEnumerator(std::string_view Value) noexcept;
[[nodiscard]] std::optional<std::string_view>
registerRoleEnumerator(std::string_view Value) noexcept;
[[nodiscard]] std::optional<std::string_view> memoryRoleEnumerator(std::string_view Value) noexcept;
[[nodiscard]] std::optional<std::string_view>
instructionFlagEnumerator(std::string_view Value) noexcept;
[[nodiscard]] std::optional<std::string_view>
pseudoOwnerEnumerator(std::string_view Value) noexcept;
[[nodiscard]] std::optional<std::string_view>
pipelineStageEnumerator(std::string_view Value) noexcept;
[[nodiscard]] bool isPowerOfTwo(std::uint64_t Value) noexcept;

class CodeGenTarget final {
public:
  CodeGenTarget(const DescriptionAST &Input, std::string SourceName);
  [[nodiscard]] std::optional<NormalizedTargetModel> normalize();
  [[nodiscard]] std::vector<TextDiagnostic> takeDiagnostics();

private:
  void fail(TextRange Range, std::string Message);
  [[nodiscard]] bool validateTarget();
  [[nodiscard]] bool normalizeRegisterUnits();
  [[nodiscard]] bool normalizeRegisters(NormalizedTargetModel &Result);
  [[nodiscard]] bool normalizeRegisterBanks(NormalizedTargetModel &Result);
  [[nodiscard]] bool normalizeRegisterClasses(NormalizedTargetModel &Result);
  [[nodiscard]] bool normalizeRegisterMasks(NormalizedTargetModel &Result);
  [[nodiscard]] bool normalizeMIROperandFlags(NormalizedTargetModel &Result);
  [[nodiscard]] bool normalizeCallingConventions(NormalizedTargetModel &Result);
  [[nodiscard]] bool normalizeNamedOperands(NormalizedTargetModel &Result);
  [[nodiscard]] bool normalizeImmediatePredicates(NormalizedTargetModel &Result);
  [[nodiscard]] bool normalizeSchedulingClasses(NormalizedTargetModel &Result);
  [[nodiscard]] bool normalizeEncodingCategories(NormalizedTargetModel &Result);
  [[nodiscard]] bool normalizeAssemblyFormats(NormalizedTargetModel &Result);
  [[nodiscard]] bool normalizeOperand(const SourceOperand &InputOperand, NormalizedOperand &Result);
  [[nodiscard]] bool
  normalizeImplicitRegisters(const std::vector<SourceImplicitRegister> &InputRegisters, bool IsDef,
                             std::vector<NormalizedImplicitRegister> &Result);
  [[nodiscard]] bool normalizeInstructions(NormalizedTargetModel &Result);

  const DescriptionAST &Input;
  std::string SourceName;
  std::unordered_map<std::string, std::uint32_t> UnitBySymbol;
  std::unordered_map<std::string, std::uint32_t> RegisterBySymbol;
  std::unordered_map<std::string, std::uint32_t> BankBySymbol;
  std::unordered_map<std::string, std::uint32_t> ClassBySymbol;
  std::unordered_map<std::string, std::uint32_t> NamedOperandBySymbol;
  std::unordered_map<std::string, std::uint32_t> ImmediatePredicateBySymbol;
  std::unordered_map<std::string, std::uint32_t> SchedulingClassBySymbol;
  std::unordered_map<std::string, std::uint32_t> EncodingCategoryBySymbol;
  std::unordered_map<std::string, std::uint32_t> AssemblyFormatBySymbol;
  std::unordered_map<std::string, std::uint32_t> CallingConventionBySymbol;
  std::vector<TextDiagnostic> Diagnostics;
};

[[nodiscard]] const NormalizedRegister &findRegister(const NormalizedTargetModel &Model,
                                                     std::uint32_t ID);
[[nodiscard]] const SourceRegisterUnit &findRegisterUnit(const NormalizedTargetModel &Model,
                                                         std::uint32_t ID);
[[nodiscard]] const NormalizedRegisterBank &findRegisterBank(const NormalizedTargetModel &Model,
                                                             std::uint32_t ID);

} // namespace rcc::targetgen

#endif // RCC_UTILS_TABLEGEN_COMMON_CODEGENTARGET_H
