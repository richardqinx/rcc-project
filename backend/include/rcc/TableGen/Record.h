// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_TABLEGEN_RECORD_H
#define RCC_TABLEGEN_RECORD_H

#include "rcc/Support/TextDiagnostic.h"

#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace rcc::targetgen {

struct SourceTarget final {
  std::string Namespace;
  std::string Name;
  std::string Triple;
  std::string CPU;
  std::string ABI;
  std::string DataLayout;
  std::vector<std::string> Features;
  TextRange Range;
};

struct SourceRegisterUnit final {
  std::uint32_t ID = 0;
  std::string Symbol;
  std::string Name;
  TextRange Range;
};

struct SourceRegisterUnitLane final {
  std::string UnitSymbol;
  std::uint64_t Lanes = 0;
  TextRange Range;
};

struct SourceRegister final {
  std::uint32_t ID = 0;
  std::string Symbol;
  std::string Name;
  std::uint32_t Encoding = 0;
  std::uint32_t DwarfRegister = 0;
  std::vector<SourceRegisterUnitLane> Units;
  bool IsConstant = false;
  bool IsArtificial = false;
  bool IsReserved = false;
  std::vector<std::string> AlternativeNames;
  TextRange Range;
};

struct SourceRegisterBank final {
  std::uint32_t ID = 0;
  std::string Symbol;
  std::string Name;
  std::uint32_t MaximumSizeInBits = 0;
  TextRange Range;
};

struct SourceRegisterClass final {
  std::uint32_t ID = 0;
  std::string Symbol;
  std::string Name;
  std::string BankSymbol;
  std::uint32_t SizeInBits = 0;
  std::uint32_t SpillSizeInBits = 0;
  std::uint32_t SpillAlignmentInBits = 0;
  std::uint64_t Lanes = 0;
  std::vector<std::string> RegisterSymbols;
  std::vector<std::string> AllocationOrderSymbols;
  bool Allocatable = true;
  std::uint32_t CopyCost = 1;
  TextRange Range;
};

struct SourceRegisterMask final {
  std::string Name;
  std::vector<std::string> PreservedRegisterSymbols;
  TextRange Range;
};

struct SourceMIROperandFlag final {
  std::string Name;
  std::uint32_t Value = 0;
  TextRange Range;
};

struct SourceMIROperandFlags final {
  std::uint32_t DirectMask = 0;
  std::vector<SourceMIROperandFlag> Direct;
  std::vector<SourceMIROperandFlag> Bitmask;
  TextRange Range;
};

struct SourceCallingConvention final {
  std::uint32_t ID = 0;
  std::string Symbol;
  std::string Name;
  std::string IntegerRegisterClassSymbol;
  std::string FloatingRegisterClassSymbol;
  std::vector<std::string> IntegerArgumentRegisterSymbols;
  std::vector<std::string> FloatingArgumentRegisterSymbols;
  std::vector<std::string> IntegerReturnRegisterSymbols;
  std::vector<std::string> FloatingReturnRegisterSymbols;
  std::string PreservedMaskName;
  std::uint32_t StackAlignmentBytes = 0;
  std::uint32_t StackArgumentAlignmentBytes = 0;
  std::uint32_t ShadowSpaceBytes = 0;
  bool SupportsVarArgs = false;
  bool SupportsTailCalls = false;
  TextRange Range;
};

struct SourceNamedOperand final {
  std::uint32_t ID = 0;
  std::string Symbol;
  std::string Name;
  TextRange Range;
};

struct SourceImmediatePredicate final {
  std::uint32_t ID = 0;
  std::string Symbol;
  std::string Name;
  std::string Kind;
  std::uint32_t Bits = 0;
  std::uint64_t Alignment = 1;
  bool NonZero = false;
  std::vector<std::int64_t> AllowedValues;
  TextRange Range;
};

struct SourceSchedulingClass final {
  std::uint32_t ID = 0;
  std::string Symbol;
  std::string Name;
  std::uint32_t MicroOps = 0;
  std::uint32_t Latency = 0;
  TextRange Range;
};

struct SourceEncodingCategory final {
  std::uint32_t ID = 0;
  std::string Symbol;
  std::string Name;
  std::uint32_t SizeInBytes = 0;
  TextRange Range;
};

struct SourceAssemblyFormat final {
  std::uint32_t ID = 0;
  std::string Symbol;
  std::string Name;
  TextRange Range;
};

struct SourceOperand final {
  std::vector<std::string> Kinds;
  std::string RegisterRole = "none";
  std::optional<std::string> RegisterClassSymbol;
  std::optional<std::string> RegisterBankSymbol;
  std::optional<std::string> ImmediatePredicateSymbol;
  std::string MemoryRole = "none";
  std::optional<std::uint32_t> DefaultTiedOperand;
  bool EarlyClobber = false;
  bool Optional = false;
  bool VariadicGroup = false;
  bool AllowSubReg = false;
  std::optional<std::string> NameSymbol;
  std::optional<std::uint16_t> TypeIndex;
  std::uint32_t AllowedTargetFlags = 0;
  TextRange Range;
};

struct SourceImplicitRegister final {
  std::string RegisterSymbol;
  bool EarlyClobber = false;
  bool Renamable = false;
  TextRange Range;
};

struct SourceInstruction final {
  std::uint32_t ID = 0;
  std::string Symbol;
  std::string Name;
  std::string AssemblyMnemonic;
  std::optional<std::uint32_t> BinaryEncoding;
  std::vector<SourceOperand> Operands;
  std::uint32_t NumExplicitDefs = 0;
  bool Variadic = false;
  std::vector<SourceImplicitRegister> ImplicitDefs;
  std::vector<SourceImplicitRegister> ImplicitUses;
  bool AllowExtraImplicitOperands = false;
  std::vector<std::string> Flags;
  std::string SchedulingClassSymbol;
  std::string EncodingCategorySymbol;
  std::uint32_t SizeInBytes = 0;
  std::string AssemblyFormatSymbol;
  bool RequiresMemoryOperands = false;
  std::string Owner = "none";
  std::string Deadline = "emission";
  TextRange Range;
};

struct DescriptionAST final {
  SourceTarget Target;
  std::vector<SourceRegisterUnit> RegisterUnits;
  std::vector<SourceRegister> Registers;
  std::vector<SourceRegisterBank> RegisterBanks;
  std::vector<SourceRegisterClass> RegisterClasses;
  std::vector<SourceRegisterMask> RegisterMasks;
  SourceMIROperandFlags MIROperandFlags;
  std::vector<SourceCallingConvention> CallingConventions;
  std::vector<SourceNamedOperand> NamedOperands;
  std::vector<SourceImmediatePredicate> ImmediatePredicates;
  std::vector<SourceSchedulingClass> SchedulingClasses;
  std::vector<SourceEncodingCategory> EncodingCategories;
  std::vector<SourceAssemblyFormat> AssemblyFormats;
  std::vector<SourceInstruction> Instructions;
};

[[nodiscard]] std::string lowerASCII(std::string_view Value);
[[nodiscard]] bool isIdentifier(std::string_view Value) noexcept;
[[nodiscard]] bool isAssemblyMnemonic(std::string_view Value) noexcept;
[[nodiscard]] std::string upperIdentifier(std::string_view Value);
void appendQuoted(std::ostream &Output, std::string_view Value);

} // namespace rcc::targetgen

#endif // RCC_TABLEGEN_RECORD_H
