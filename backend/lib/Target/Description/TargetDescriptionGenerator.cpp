// SPDX-License-Identifier: GPL-3.0-or-later

#include "TargetDescriptionGenerator.h"

#include "rcc/Support/YAML.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rcc::targetgen {
namespace {

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

[[nodiscard]] std::string lowerASCII(std::string_view Value) {
  std::string Result(Value);
  for (char &Character : Result) {
    if (Character >= 'A' && Character <= 'Z')
      Character = static_cast<char>(Character - 'A' + 'a');
  }
  return Result;
}

[[nodiscard]] bool isIdentifier(std::string_view Value) noexcept {
  if (Value.empty())
    return false;
  const auto IsHead = [](unsigned char Character) {
    return std::isalpha(Character) != 0 || Character == '_';
  };
  const auto IsTail = [](unsigned char Character) {
    return std::isalnum(Character) != 0 || Character == '_';
  };
  if (!IsHead(static_cast<unsigned char>(Value.front())))
    return false;
  return std::ranges::all_of(Value.substr(1), [IsTail](char Character) {
    return IsTail(static_cast<unsigned char>(Character));
  });
}

[[nodiscard]] bool isAssemblyMnemonic(std::string_view Value) noexcept {
  if (Value.empty())
    return false;
  if (std::islower(static_cast<unsigned char>(Value.front())) == 0)
    return false;
  if (std::islower(static_cast<unsigned char>(Value.back())) == 0 &&
      std::isdigit(static_cast<unsigned char>(Value.back())) == 0)
    return false;
  return std::ranges::all_of(Value, [](char Character) {
    const unsigned char Code = static_cast<unsigned char>(Character);
    return std::islower(Code) != 0 || std::isdigit(Code) != 0 || Character == '.' ||
           Character == '_';
  });
}

[[nodiscard]] std::string upperIdentifier(std::string_view Value) {
  std::string Result;
  Result.reserve(Value.size());
  for (const char Character : Value) {
    if (Character >= 'a' && Character <= 'z')
      Result.push_back(static_cast<char>(Character - 'a' + 'A'));
    else if ((Character >= 'A' && Character <= 'Z') || (Character >= '0' && Character <= '9'))
      Result.push_back(Character);
    else
      Result.push_back('_');
  }
  return Result;
}

void appendQuoted(std::ostream &Output, std::string_view Value) {
  Output << '"';
  for (const char Character : Value) {
    switch (Character) {
    case '\\':
      Output << "\\\\";
      break;
    case '"':
      Output << "\\\"";
      break;
    case '\n':
      Output << "\\n";
      break;
    case '\r':
      Output << "\\r";
      break;
    case '\t':
      Output << "\\t";
      break;
    default:
      Output << Character;
      break;
    }
  }
  Output << '"';
}

class ASTParser final {
public:
  explicit ASTParser(const yaml::Stream &Input) : Input(Input) {}

  [[nodiscard]] std::optional<DescriptionAST> parse() {
    const std::span<const yaml::Document> Documents = Input.documents();
    if (Documents.size() != 1) {
      fail(Input.source().range(0, Input.source().text().size()),
           "target description must contain exactly one YAML document");
      return std::nullopt;
    }

    const yaml::Node *Root = Input.node(Documents.front().Root);
    if (!requireMapping(Root, "target description root"))
      return std::nullopt;
    if (!checkKeys(*Root,
                   {"schema", "target", "register_units", "registers", "register_banks",
                    "register_classes", "register_masks", "mir_operand_flags", "named_operands",
                    "immediate_predicates", "scheduling_classes", "encoding_categories",
                    "assembly_formats", "calling_conventions", "instructions"},
                   "target description root")) {
      return std::nullopt;
    }

    const yaml::Node *SchemaNode = requiredNode(*Root, "schema", "target description root");
    if (SchemaNode == nullptr)
      return std::nullopt;
    const auto Schema = scalar(*SchemaNode, "target description schema");
    if (!Schema || *Schema != "rcc-target-description-v2") {
      if (Schema)
        fail(SchemaNode->range(), "unsupported target description schema");
      return std::nullopt;
    }

    DescriptionAST Result;
    const yaml::Node *TargetNode = requiredNode(*Root, "target", "target description root");
    if (TargetNode == nullptr || !parseTarget(*TargetNode, Result.Target))
      return std::nullopt;
    const yaml::Node *UnitsNode = requiredNode(*Root, "register_units", "target description root");
    const yaml::Node *RegistersNode = requiredNode(*Root, "registers", "target description root");
    const yaml::Node *BanksNode = requiredNode(*Root, "register_banks", "target description root");
    const yaml::Node *ClassesNode =
        requiredNode(*Root, "register_classes", "target description root");
    const yaml::Node *MasksNode = requiredNode(*Root, "register_masks", "target description root");
    const yaml::Node *MIROperandFlagsNode = optionalNode(*Root, "mir_operand_flags");
    const yaml::Node *NamedOperandsNode =
        requiredNode(*Root, "named_operands", "target description root");
    const yaml::Node *ImmediatePredicatesNode =
        requiredNode(*Root, "immediate_predicates", "target description root");
    const yaml::Node *SchedulingClassesNode =
        requiredNode(*Root, "scheduling_classes", "target description root");
    const yaml::Node *EncodingCategoriesNode =
        requiredNode(*Root, "encoding_categories", "target description root");
    const yaml::Node *AssemblyFormatsNode =
        requiredNode(*Root, "assembly_formats", "target description root");
    const yaml::Node *CallingConventionsNode =
        requiredNode(*Root, "calling_conventions", "target description root");
    const yaml::Node *InstructionsNode =
        requiredNode(*Root, "instructions", "target description root");
    if (UnitsNode == nullptr || RegistersNode == nullptr || BanksNode == nullptr ||
        ClassesNode == nullptr || MasksNode == nullptr || NamedOperandsNode == nullptr ||
        ImmediatePredicatesNode == nullptr || SchedulingClassesNode == nullptr ||
        EncodingCategoriesNode == nullptr || AssemblyFormatsNode == nullptr ||
        CallingConventionsNode == nullptr || InstructionsNode == nullptr ||
        !parseRegisterUnits(*UnitsNode, Result.RegisterUnits) ||
        !parseRegisters(*RegistersNode, Result.Registers) ||
        !parseRegisterBanks(*BanksNode, Result.RegisterBanks) ||
        !parseRegisterClasses(*ClassesNode, Result.RegisterClasses) ||
        !parseRegisterMasks(*MasksNode, Result.RegisterMasks) ||
        (MIROperandFlagsNode != nullptr &&
         !parseMIROperandFlags(*MIROperandFlagsNode, Result.MIROperandFlags)) ||
        !parseNamedOperands(*NamedOperandsNode, Result.NamedOperands) ||
        !parseImmediatePredicates(*ImmediatePredicatesNode, Result.ImmediatePredicates) ||
        !parseSchedulingClasses(*SchedulingClassesNode, Result.SchedulingClasses) ||
        !parseEncodingCategories(*EncodingCategoriesNode, Result.EncodingCategories) ||
        !parseAssemblyFormats(*AssemblyFormatsNode, Result.AssemblyFormats) ||
        !parseCallingConventions(*CallingConventionsNode, Result.CallingConventions) ||
        !parseInstructions(*InstructionsNode, Result.Instructions)) {
      return std::nullopt;
    }
    return Result;
  }

  [[nodiscard]] std::vector<TextDiagnostic> takeDiagnostics() {
    return std::move(Diagnostics);
  }

private:
  void fail(TextRange Range, std::string Message) {
    Diagnostics.push_back({
        .Severity = DiagnosticSeverity::Error,
        .Category = "target-description",
        .Message = std::move(Message),
        .SourceName = std::string(Input.source().name()),
        .Range = Range,
    });
  }

  [[nodiscard]] bool requireMapping(const yaml::Node *Node, std::string_view Context) {
    if (Node != nullptr && Node->kind() == yaml::NodeKind::Mapping)
      return true;
    fail(Node == nullptr ? Input.source().range(0, 0) : Node->range(),
         std::string(Context) + " must be a mapping");
    return false;
  }

  [[nodiscard]] bool requireSequence(const yaml::Node *Node, std::string_view Context) {
    if (Node != nullptr && Node->kind() == yaml::NodeKind::Sequence)
      return true;
    fail(Node == nullptr ? Input.source().range(0, 0) : Node->range(),
         std::string(Context) + " must be a sequence");
    return false;
  }

  [[nodiscard]] bool checkKeys(const yaml::Node &Mapping,
                               std::initializer_list<std::string_view> Allowed,
                               std::string_view Context) {
    for (const yaml::MappingEntry &Entry : Mapping.mapping()) {
      const bool IsAllowed =
          std::ranges::any_of(Allowed, [&Entry](std::string_view Key) { return Entry.Key == Key; });
      if (!IsAllowed) {
        fail(Entry.KeyRange, std::string(Context) + " has unknown key '" + Entry.Key + "'");
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] const yaml::Node *requiredNode(const yaml::Node &Mapping, std::string_view Key,
                                               std::string_view Context) {
    const std::optional<yaml::NodeID> Value = Mapping.mappingValue(Key);
    if (Value) {
      const yaml::Node *Node = Input.node(*Value);
      if (Node != nullptr)
        return Node;
    }
    fail(Mapping.range(),
         std::string(Context) + " is missing required key '" + std::string(Key) + "'");
    return nullptr;
  }

  [[nodiscard]] std::optional<std::string>
  requiredScalar(const yaml::Node &Mapping, std::string_view Key, std::string_view Context) {
    const yaml::Node *Node = requiredNode(Mapping, Key, Context);
    if (Node == nullptr)
      return std::nullopt;
    return scalar(*Node, std::string(Context) + " key '" + std::string(Key) + "'");
  }

  [[nodiscard]] std::optional<std::string> scalar(const yaml::Node &Node,
                                                  std::string_view Context) {
    if (Node.kind() != yaml::NodeKind::Scalar) {
      fail(Node.range(), std::string(Context) + " must be a scalar");
      return std::nullopt;
    }
    return std::string(Node.scalar());
  }

  [[nodiscard]] std::optional<std::uint64_t> unsignedInteger(const yaml::Node &Node,
                                                             std::string_view Context) {
    const auto Text = scalar(Node, Context);
    if (!Text)
      return std::nullopt;
    std::uint64_t Result = 0;
    const auto [End, Error] =
        std::from_chars(Text->data(), Text->data() + Text->size(), Result, 10);
    if (Error != std::errc{} || End != Text->data() + Text->size()) {
      fail(Node.range(), std::string(Context) + " must be an unsigned decimal integer");
      return std::nullopt;
    }
    return Result;
  }

  [[nodiscard]] std::optional<std::uint32_t> unsignedInteger32(const yaml::Node &Node,
                                                               std::string_view Context) {
    const auto Value = unsignedInteger(Node, Context);
    if (!Value)
      return std::nullopt;
    if (*Value > std::numeric_limits<std::uint32_t>::max()) {
      fail(Node.range(), std::string(Context) + " is outside the uint32 range");
      return std::nullopt;
    }
    return static_cast<std::uint32_t>(*Value);
  }

  [[nodiscard]] std::optional<std::int64_t> signedInteger(const yaml::Node &Node,
                                                          std::string_view Context) {
    const auto Text = scalar(Node, Context);
    if (!Text)
      return std::nullopt;
    std::int64_t Result = 0;
    const auto [End, Error] =
        std::from_chars(Text->data(), Text->data() + Text->size(), Result, 10);
    if (Error != std::errc{} || End != Text->data() + Text->size()) {
      fail(Node.range(), std::string(Context) + " must be a signed decimal integer");
      return std::nullopt;
    }
    return Result;
  }

  [[nodiscard]] std::optional<bool> boolean(const yaml::Node &Node, std::string_view Context) {
    const auto Text = scalar(Node, Context);
    if (!Text)
      return std::nullopt;
    if (*Text == "true")
      return true;
    if (*Text == "false")
      return false;
    fail(Node.range(), std::string(Context) + " must be 'true' or 'false'");
    return std::nullopt;
  }

  [[nodiscard]] std::optional<std::vector<std::string>> scalarSequence(const yaml::Node &Node,
                                                                       std::string_view Context) {
    if (!requireSequence(&Node, Context))
      return std::nullopt;
    std::vector<std::string> Result;
    for (const yaml::NodeID Value : Node.sequence()) {
      const yaml::Node *Element = Input.node(Value);
      if (Element == nullptr)
        return std::nullopt;
      const auto Text = scalar(*Element, Context);
      if (!Text)
        return std::nullopt;
      Result.push_back(*Text);
    }
    return Result;
  }

  [[nodiscard]] std::optional<std::vector<std::int64_t>>
  signedIntegerSequence(const yaml::Node &Node, std::string_view Context) {
    if (!requireSequence(&Node, Context))
      return std::nullopt;
    std::vector<std::int64_t> Result;
    for (const yaml::NodeID Value : Node.sequence()) {
      const yaml::Node *Element = Input.node(Value);
      if (Element == nullptr)
        return std::nullopt;
      const auto Parsed = signedInteger(*Element, Context);
      if (!Parsed)
        return std::nullopt;
      Result.push_back(*Parsed);
    }
    return Result;
  }

  [[nodiscard]] bool parseTarget(const yaml::Node &Node, SourceTarget &Result) {
    if (!requireMapping(&Node, "target"))
      return false;
    if (!checkKeys(Node, {"namespace", "name", "triple", "cpu", "abi", "data_layout", "features"},
                   "target")) {
      return false;
    }
    const auto Namespace = requiredScalar(Node, "namespace", "target");
    const auto Name = requiredScalar(Node, "name", "target");
    const auto Triple = requiredScalar(Node, "triple", "target");
    const auto CPU = requiredScalar(Node, "cpu", "target");
    const auto ABI = requiredScalar(Node, "abi", "target");
    const auto DataLayout = requiredScalar(Node, "data_layout", "target");
    const yaml::Node *Features = requiredNode(Node, "features", "target");
    if (!Namespace || !Name || !Triple || !CPU || !ABI || !DataLayout || Features == nullptr)
      return false;
    const auto ParsedFeatures = scalarSequence(*Features, "target features");
    if (!ParsedFeatures)
      return false;
    Result = {
        .Namespace = *Namespace,
        .Name = *Name,
        .Triple = *Triple,
        .CPU = *CPU,
        .ABI = *ABI,
        .DataLayout = *DataLayout,
        .Features = *ParsedFeatures,
        .Range = Node.range(),
    };
    return true;
  }

  [[nodiscard]] bool parseRegisterUnits(const yaml::Node &Node,
                                        std::vector<SourceRegisterUnit> &Result) {
    if (!requireSequence(&Node, "register_units"))
      return false;
    for (const yaml::NodeID ElementID : Node.sequence()) {
      const yaml::Node *Element = Input.node(ElementID);
      if (!requireMapping(Element, "register unit"))
        return false;
      if (!checkKeys(*Element, {"id", "symbol", "name"}, "register unit"))
        return false;
      const yaml::Node *IDNode = requiredNode(*Element, "id", "register unit");
      const auto Symbol = requiredScalar(*Element, "symbol", "register unit");
      const auto Name = requiredScalar(*Element, "name", "register unit");
      if (IDNode == nullptr || !Symbol || !Name)
        return false;
      const auto ID = unsignedInteger32(*IDNode, "register unit id");
      if (!ID)
        return false;
      Result.push_back({.ID = *ID, .Symbol = *Symbol, .Name = *Name, .Range = Element->range()});
    }
    return true;
  }

  [[nodiscard]] std::optional<std::vector<SourceRegisterUnitLane>>
  parseRegisterUnitLanes(const yaml::Node &Node) {
    if (!requireSequence(&Node, "register units"))
      return std::nullopt;
    std::vector<SourceRegisterUnitLane> Result;
    for (const yaml::NodeID ElementID : Node.sequence()) {
      const yaml::Node *Element = Input.node(ElementID);
      if (!requireMapping(Element, "register unit lane"))
        return std::nullopt;
      if (!checkKeys(*Element, {"unit", "lanes"}, "register unit lane"))
        return std::nullopt;
      const auto Unit = requiredScalar(*Element, "unit", "register unit lane");
      const yaml::Node *LanesNode = requiredNode(*Element, "lanes", "register unit lane");
      if (!Unit || LanesNode == nullptr)
        return std::nullopt;
      const auto Lanes = unsignedInteger(*LanesNode, "register unit lane lanes");
      if (!Lanes)
        return std::nullopt;
      Result.push_back({.UnitSymbol = *Unit, .Lanes = *Lanes, .Range = Element->range()});
    }
    return Result;
  }

  [[nodiscard]] bool parseRegisters(const yaml::Node &Node, std::vector<SourceRegister> &Result) {
    if (!requireSequence(&Node, "registers"))
      return false;
    for (const yaml::NodeID ElementID : Node.sequence()) {
      const yaml::Node *Element = Input.node(ElementID);
      if (!requireMapping(Element, "register"))
        return false;
      if (!checkKeys(*Element,
                     {"id", "symbol", "name", "encoding", "dwarf_register", "units", "constant",
                      "artificial", "reserved", "aliases"},
                     "register")) {
        return false;
      }
      const yaml::Node *IDNode = requiredNode(*Element, "id", "register");
      const auto Symbol = requiredScalar(*Element, "symbol", "register");
      const auto Name = requiredScalar(*Element, "name", "register");
      const yaml::Node *EncodingNode = requiredNode(*Element, "encoding", "register");
      const yaml::Node *DwarfNode = requiredNode(*Element, "dwarf_register", "register");
      const yaml::Node *UnitsNode = requiredNode(*Element, "units", "register");
      if (IDNode == nullptr || !Symbol || !Name || EncodingNode == nullptr ||
          DwarfNode == nullptr || UnitsNode == nullptr) {
        return false;
      }
      const auto ID = unsignedInteger32(*IDNode, "register id");
      const auto Encoding = unsignedInteger32(*EncodingNode, "register encoding");
      const auto DwarfRegister = unsignedInteger32(*DwarfNode, "register DWARF number");
      const auto Units = parseRegisterUnitLanes(*UnitsNode);
      if (!ID || !Encoding || !DwarfRegister || !Units)
        return false;

      SourceRegister Register{
          .ID = *ID,
          .Symbol = *Symbol,
          .Name = *Name,
          .Encoding = *Encoding,
          .DwarfRegister = *DwarfRegister,
          .Units = *Units,
          .IsConstant = false,
          .IsArtificial = false,
          .IsReserved = false,
          .AlternativeNames = {},
          .Range = Element->range(),
      };
      if (const yaml::Node *Constant = optionalNode(*Element, "constant")) {
        const auto Value = boolean(*Constant, "register constant");
        if (!Value)
          return false;
        Register.IsConstant = *Value;
      }
      if (const yaml::Node *Artificial = optionalNode(*Element, "artificial")) {
        const auto Value = boolean(*Artificial, "register artificial");
        if (!Value)
          return false;
        Register.IsArtificial = *Value;
      }
      if (const yaml::Node *Reserved = optionalNode(*Element, "reserved")) {
        const auto Value = boolean(*Reserved, "register reserved");
        if (!Value)
          return false;
        Register.IsReserved = *Value;
      }
      if (const yaml::Node *Aliases = optionalNode(*Element, "aliases")) {
        const auto Values = scalarSequence(*Aliases, "register aliases");
        if (!Values)
          return false;
        Register.AlternativeNames = *Values;
      }
      Result.push_back(std::move(Register));
    }
    return true;
  }

  [[nodiscard]] bool parseRegisterBanks(const yaml::Node &Node,
                                        std::vector<SourceRegisterBank> &Result) {
    if (!requireSequence(&Node, "register_banks"))
      return false;
    for (const yaml::NodeID ElementID : Node.sequence()) {
      const yaml::Node *Element = Input.node(ElementID);
      if (!requireMapping(Element, "register bank"))
        return false;
      if (!checkKeys(*Element, {"id", "symbol", "name", "maximum_size_bits"}, "register bank"))
        return false;
      const yaml::Node *IDNode = requiredNode(*Element, "id", "register bank");
      const auto Symbol = requiredScalar(*Element, "symbol", "register bank");
      const auto Name = requiredScalar(*Element, "name", "register bank");
      const yaml::Node *SizeNode = requiredNode(*Element, "maximum_size_bits", "register bank");
      if (IDNode == nullptr || !Symbol || !Name || SizeNode == nullptr)
        return false;
      const auto ID = unsignedInteger32(*IDNode, "register bank id");
      const auto Size = unsignedInteger32(*SizeNode, "register bank maximum_size_bits");
      if (!ID || !Size)
        return false;
      Result.push_back({.ID = *ID,
                        .Symbol = *Symbol,
                        .Name = *Name,
                        .MaximumSizeInBits = *Size,
                        .Range = Element->range()});
    }
    return true;
  }

  [[nodiscard]] bool parseRegisterClasses(const yaml::Node &Node,
                                          std::vector<SourceRegisterClass> &Result) {
    if (!requireSequence(&Node, "register_classes"))
      return false;
    for (const yaml::NodeID ElementID : Node.sequence()) {
      const yaml::Node *Element = Input.node(ElementID);
      if (!requireMapping(Element, "register class"))
        return false;
      if (!checkKeys(*Element,
                     {"id", "symbol", "name", "bank", "size_bits", "spill_size_bits",
                      "spill_alignment_bits", "lanes", "registers", "allocation_order",
                      "allocatable", "copy_cost"},
                     "register class")) {
        return false;
      }
      const yaml::Node *IDNode = requiredNode(*Element, "id", "register class");
      const auto Symbol = requiredScalar(*Element, "symbol", "register class");
      const auto Name = requiredScalar(*Element, "name", "register class");
      const auto Bank = requiredScalar(*Element, "bank", "register class");
      const yaml::Node *SizeNode = requiredNode(*Element, "size_bits", "register class");
      const yaml::Node *SpillSizeNode = requiredNode(*Element, "spill_size_bits", "register class");
      const yaml::Node *SpillAlignmentNode =
          requiredNode(*Element, "spill_alignment_bits", "register class");
      const yaml::Node *LanesNode = requiredNode(*Element, "lanes", "register class");
      const yaml::Node *RegistersNode = requiredNode(*Element, "registers", "register class");
      const yaml::Node *AllocationNode =
          requiredNode(*Element, "allocation_order", "register class");
      if (IDNode == nullptr || !Symbol || !Name || !Bank || SizeNode == nullptr ||
          SpillSizeNode == nullptr || SpillAlignmentNode == nullptr || LanesNode == nullptr ||
          RegistersNode == nullptr || AllocationNode == nullptr) {
        return false;
      }
      const auto ID = unsignedInteger32(*IDNode, "register class id");
      const auto Size = unsignedInteger32(*SizeNode, "register class size_bits");
      const auto SpillSize = unsignedInteger32(*SpillSizeNode, "register class spill_size_bits");
      const auto SpillAlignment =
          unsignedInteger32(*SpillAlignmentNode, "register class spill_alignment_bits");
      const auto Lanes = unsignedInteger(*LanesNode, "register class lanes");
      const auto Registers = scalarSequence(*RegistersNode, "register class registers");
      const auto AllocationOrder =
          scalarSequence(*AllocationNode, "register class allocation_order");
      if (!ID || !Size || !SpillSize || !SpillAlignment || !Lanes || !Registers ||
          !AllocationOrder) {
        return false;
      }
      SourceRegisterClass RegisterClass{
          .ID = *ID,
          .Symbol = *Symbol,
          .Name = *Name,
          .BankSymbol = *Bank,
          .SizeInBits = *Size,
          .SpillSizeInBits = *SpillSize,
          .SpillAlignmentInBits = *SpillAlignment,
          .Lanes = *Lanes,
          .RegisterSymbols = *Registers,
          .AllocationOrderSymbols = *AllocationOrder,
          .Range = Element->range(),
      };
      if (const yaml::Node *Allocatable = optionalNode(*Element, "allocatable")) {
        const auto Value = boolean(*Allocatable, "register class allocatable");
        if (!Value)
          return false;
        RegisterClass.Allocatable = *Value;
      }
      if (const yaml::Node *CopyCost = optionalNode(*Element, "copy_cost")) {
        const auto Value = unsignedInteger32(*CopyCost, "register class copy_cost");
        if (!Value)
          return false;
        RegisterClass.CopyCost = *Value;
      }
      Result.push_back(std::move(RegisterClass));
    }
    return true;
  }

  [[nodiscard]] bool parseRegisterMasks(const yaml::Node &Node,
                                        std::vector<SourceRegisterMask> &Result) {
    if (!requireSequence(&Node, "register_masks"))
      return false;
    for (const yaml::NodeID ElementID : Node.sequence()) {
      const yaml::Node *Element = Input.node(ElementID);
      if (!requireMapping(Element, "register mask"))
        return false;
      if (!checkKeys(*Element, {"name", "preserved"}, "register mask"))
        return false;
      const auto Name = requiredScalar(*Element, "name", "register mask");
      const yaml::Node *PreservedNode = requiredNode(*Element, "preserved", "register mask");
      if (!Name || PreservedNode == nullptr)
        return false;
      const auto Preserved = scalarSequence(*PreservedNode, "register mask preserved");
      if (!Preserved)
        return false;
      Result.push_back(
          {.Name = *Name, .PreservedRegisterSymbols = *Preserved, .Range = Element->range()});
    }
    return true;
  }

  [[nodiscard]] bool parseMIROperandFlagSequence(const yaml::Node &Node, std::string_view Context,
                                                 std::vector<SourceMIROperandFlag> &Result) {
    if (!requireSequence(&Node, Context))
      return false;
    for (const yaml::NodeID ElementID : Node.sequence()) {
      const yaml::Node *Element = Input.node(ElementID);
      if (!requireMapping(Element, Context) || !checkKeys(*Element, {"name", "value"}, Context)) {
        return false;
      }
      const auto Name = requiredScalar(*Element, "name", Context);
      const yaml::Node *ValueNode = requiredNode(*Element, "value", Context);
      if (!Name || ValueNode == nullptr)
        return false;
      const auto Value = unsignedInteger32(*ValueNode, std::string(Context) + " value");
      if (!Value)
        return false;
      Result.push_back({.Name = *Name, .Value = *Value, .Range = Element->range()});
    }
    return true;
  }

  [[nodiscard]] bool parseMIROperandFlags(const yaml::Node &Node, SourceMIROperandFlags &Result) {
    if (!requireMapping(&Node, "MIR operand flags") ||
        !checkKeys(Node, {"direct_mask", "direct", "bitmask"}, "MIR operand flags")) {
      return false;
    }
    const yaml::Node *DirectMaskNode = requiredNode(Node, "direct_mask", "MIR operand flags");
    const yaml::Node *DirectNode = requiredNode(Node, "direct", "MIR operand flags");
    const yaml::Node *BitmaskNode = requiredNode(Node, "bitmask", "MIR operand flags");
    if (DirectMaskNode == nullptr || DirectNode == nullptr || BitmaskNode == nullptr)
      return false;
    const auto DirectMask = unsignedInteger32(*DirectMaskNode, "MIR operand flag direct_mask");
    if (!DirectMask ||
        !parseMIROperandFlagSequence(*DirectNode, "direct MIR operand flag", Result.Direct) ||
        !parseMIROperandFlagSequence(*BitmaskNode, "bitmask MIR operand flag", Result.Bitmask)) {
      return false;
    }
    Result.DirectMask = *DirectMask;
    Result.Range = Node.range();
    return true;
  }

  [[nodiscard]] bool parseNamedOperands(const yaml::Node &Node,
                                        std::vector<SourceNamedOperand> &Result) {
    if (!requireSequence(&Node, "named_operands"))
      return false;
    for (const yaml::NodeID ElementID : Node.sequence()) {
      const yaml::Node *Element = Input.node(ElementID);
      if (!requireMapping(Element, "named operand"))
        return false;
      if (!checkKeys(*Element, {"id", "symbol", "name"}, "named operand"))
        return false;
      const yaml::Node *IDNode = requiredNode(*Element, "id", "named operand");
      const auto Symbol = requiredScalar(*Element, "symbol", "named operand");
      const auto Name = requiredScalar(*Element, "name", "named operand");
      if (IDNode == nullptr || !Symbol || !Name)
        return false;
      const auto ID = unsignedInteger32(*IDNode, "named operand id");
      if (!ID)
        return false;
      Result.push_back({.ID = *ID, .Symbol = *Symbol, .Name = *Name, .Range = Element->range()});
    }
    return true;
  }

  [[nodiscard]] bool parseImmediatePredicates(const yaml::Node &Node,
                                              std::vector<SourceImmediatePredicate> &Result) {
    if (!requireSequence(&Node, "immediate_predicates"))
      return false;
    for (const yaml::NodeID ElementID : Node.sequence()) {
      const yaml::Node *Element = Input.node(ElementID);
      if (!requireMapping(Element, "immediate predicate"))
        return false;
      if (!checkKeys(
              *Element,
              {"id", "symbol", "name", "kind", "bits", "alignment", "nonzero", "allowed_values"},
              "immediate predicate")) {
        return false;
      }
      const yaml::Node *IDNode = requiredNode(*Element, "id", "immediate predicate");
      const auto Symbol = requiredScalar(*Element, "symbol", "immediate predicate");
      const auto Name = requiredScalar(*Element, "name", "immediate predicate");
      const auto Kind = requiredScalar(*Element, "kind", "immediate predicate");
      const yaml::Node *BitsNode = requiredNode(*Element, "bits", "immediate predicate");
      if (IDNode == nullptr || !Symbol || !Name || !Kind || BitsNode == nullptr)
        return false;
      const auto ID = unsignedInteger32(*IDNode, "immediate predicate id");
      const auto Bits = unsignedInteger32(*BitsNode, "immediate predicate bits");
      if (!ID || !Bits)
        return false;
      SourceImmediatePredicate Predicate{
          .ID = *ID,
          .Symbol = *Symbol,
          .Name = *Name,
          .Kind = *Kind,
          .Bits = *Bits,
          .Alignment = 1,
          .NonZero = false,
          .AllowedValues = {},
          .Range = Element->range(),
      };
      if (const yaml::Node *Alignment = optionalNode(*Element, "alignment")) {
        const auto Value = unsignedInteger(*Alignment, "immediate predicate alignment");
        if (!Value)
          return false;
        Predicate.Alignment = *Value;
      }
      if (const yaml::Node *NonZero = optionalNode(*Element, "nonzero")) {
        const auto Value = boolean(*NonZero, "immediate predicate nonzero");
        if (!Value)
          return false;
        Predicate.NonZero = *Value;
      }
      if (const yaml::Node *AllowedValues = optionalNode(*Element, "allowed_values")) {
        const auto Values =
            signedIntegerSequence(*AllowedValues, "immediate predicate allowed_values");
        if (!Values || Values->empty()) {
          if (Values)
            fail(AllowedValues->range(), "immediate predicate allowed_values must be nonempty");
          return false;
        }
        Predicate.AllowedValues = *Values;
      }
      Result.push_back(std::move(Predicate));
    }
    return true;
  }

  [[nodiscard]] bool parseSchedulingClasses(const yaml::Node &Node,
                                            std::vector<SourceSchedulingClass> &Result) {
    if (!requireSequence(&Node, "scheduling_classes"))
      return false;
    for (const yaml::NodeID ElementID : Node.sequence()) {
      const yaml::Node *Element = Input.node(ElementID);
      if (!requireMapping(Element, "scheduling class"))
        return false;
      if (!checkKeys(*Element, {"id", "symbol", "name", "micro_ops", "latency"},
                     "scheduling class")) {
        return false;
      }
      const yaml::Node *IDNode = requiredNode(*Element, "id", "scheduling class");
      const auto Symbol = requiredScalar(*Element, "symbol", "scheduling class");
      const auto Name = requiredScalar(*Element, "name", "scheduling class");
      const yaml::Node *MicroOpsNode = requiredNode(*Element, "micro_ops", "scheduling class");
      const yaml::Node *LatencyNode = requiredNode(*Element, "latency", "scheduling class");
      if (IDNode == nullptr || !Symbol || !Name || MicroOpsNode == nullptr ||
          LatencyNode == nullptr)
        return false;
      const auto ID = unsignedInteger32(*IDNode, "scheduling class id");
      const auto MicroOps = unsignedInteger32(*MicroOpsNode, "scheduling class micro_ops");
      const auto Latency = unsignedInteger32(*LatencyNode, "scheduling class latency");
      if (!ID || !MicroOps || !Latency)
        return false;
      Result.push_back({
          .ID = *ID,
          .Symbol = *Symbol,
          .Name = *Name,
          .MicroOps = *MicroOps,
          .Latency = *Latency,
          .Range = Element->range(),
      });
    }
    return true;
  }

  [[nodiscard]] bool parseEncodingCategories(const yaml::Node &Node,
                                             std::vector<SourceEncodingCategory> &Result) {
    if (!requireSequence(&Node, "encoding_categories"))
      return false;
    for (const yaml::NodeID ElementID : Node.sequence()) {
      const yaml::Node *Element = Input.node(ElementID);
      if (!requireMapping(Element, "encoding category"))
        return false;
      if (!checkKeys(*Element, {"id", "symbol", "name", "size_bytes"}, "encoding category"))
        return false;
      const yaml::Node *IDNode = requiredNode(*Element, "id", "encoding category");
      const auto Symbol = requiredScalar(*Element, "symbol", "encoding category");
      const auto Name = requiredScalar(*Element, "name", "encoding category");
      const yaml::Node *SizeNode = requiredNode(*Element, "size_bytes", "encoding category");
      if (IDNode == nullptr || !Symbol || !Name || SizeNode == nullptr)
        return false;
      const auto ID = unsignedInteger32(*IDNode, "encoding category id");
      const auto Size = unsignedInteger32(*SizeNode, "encoding category size_bytes");
      if (!ID || !Size)
        return false;
      Result.push_back({
          .ID = *ID,
          .Symbol = *Symbol,
          .Name = *Name,
          .SizeInBytes = *Size,
          .Range = Element->range(),
      });
    }
    return true;
  }

  [[nodiscard]] bool parseAssemblyFormats(const yaml::Node &Node,
                                          std::vector<SourceAssemblyFormat> &Result) {
    if (!requireSequence(&Node, "assembly_formats"))
      return false;
    for (const yaml::NodeID ElementID : Node.sequence()) {
      const yaml::Node *Element = Input.node(ElementID);
      if (!requireMapping(Element, "assembly format"))
        return false;
      if (!checkKeys(*Element, {"id", "symbol", "name"}, "assembly format"))
        return false;
      const yaml::Node *IDNode = requiredNode(*Element, "id", "assembly format");
      const auto Symbol = requiredScalar(*Element, "symbol", "assembly format");
      const auto Name = requiredScalar(*Element, "name", "assembly format");
      if (IDNode == nullptr || !Symbol || !Name)
        return false;
      const auto ID = unsignedInteger32(*IDNode, "assembly format id");
      if (!ID)
        return false;
      Result.push_back({.ID = *ID, .Symbol = *Symbol, .Name = *Name, .Range = Element->range()});
    }
    return true;
  }

  [[nodiscard]] bool parseCallingConventions(const yaml::Node &Node,
                                             std::vector<SourceCallingConvention> &Result) {
    if (!requireSequence(&Node, "calling_conventions"))
      return false;
    for (const yaml::NodeID ElementID : Node.sequence()) {
      const yaml::Node *Element = Input.node(ElementID);
      if (!requireMapping(Element, "calling convention"))
        return false;
      if (!checkKeys(*Element,
                     {"id", "symbol", "name", "integer_register_class", "floating_register_class",
                      "integer_argument_registers", "floating_argument_registers",
                      "integer_return_registers", "floating_return_registers", "preserved_mask",
                      "stack_alignment_bytes", "stack_argument_alignment_bytes",
                      "shadow_space_bytes", "supports_varargs", "supports_tail_calls"},
                     "calling convention")) {
        return false;
      }
      const yaml::Node *IDNode = requiredNode(*Element, "id", "calling convention");
      const auto Symbol = requiredScalar(*Element, "symbol", "calling convention");
      const auto Name = requiredScalar(*Element, "name", "calling convention");
      const auto IntegerClass =
          requiredScalar(*Element, "integer_register_class", "calling convention");
      const auto FloatingClass =
          requiredScalar(*Element, "floating_register_class", "calling convention");
      const yaml::Node *IntegerArguments =
          requiredNode(*Element, "integer_argument_registers", "calling convention");
      const yaml::Node *FloatingArguments =
          requiredNode(*Element, "floating_argument_registers", "calling convention");
      const yaml::Node *IntegerReturns =
          requiredNode(*Element, "integer_return_registers", "calling convention");
      const yaml::Node *FloatingReturns =
          requiredNode(*Element, "floating_return_registers", "calling convention");
      const auto PreservedMask = requiredScalar(*Element, "preserved_mask", "calling convention");
      const yaml::Node *StackAlignment =
          requiredNode(*Element, "stack_alignment_bytes", "calling convention");
      const yaml::Node *StackArgumentAlignment =
          requiredNode(*Element, "stack_argument_alignment_bytes", "calling convention");
      const yaml::Node *ShadowSpace =
          requiredNode(*Element, "shadow_space_bytes", "calling convention");
      const yaml::Node *SupportsVarArgs =
          requiredNode(*Element, "supports_varargs", "calling convention");
      const yaml::Node *SupportsTailCalls =
          requiredNode(*Element, "supports_tail_calls", "calling convention");
      if (IDNode == nullptr || !Symbol || !Name || !IntegerClass || !FloatingClass ||
          IntegerArguments == nullptr || FloatingArguments == nullptr ||
          IntegerReturns == nullptr || FloatingReturns == nullptr || !PreservedMask ||
          StackAlignment == nullptr || StackArgumentAlignment == nullptr ||
          ShadowSpace == nullptr || SupportsVarArgs == nullptr || SupportsTailCalls == nullptr) {
        return false;
      }
      const auto ID = unsignedInteger32(*IDNode, "calling convention id");
      const auto IntegerArgumentList =
          scalarSequence(*IntegerArguments, "calling convention integer arguments");
      const auto FloatingArgumentList =
          scalarSequence(*FloatingArguments, "calling convention floating arguments");
      const auto IntegerReturnList =
          scalarSequence(*IntegerReturns, "calling convention integer returns");
      const auto FloatingReturnList =
          scalarSequence(*FloatingReturns, "calling convention floating returns");
      const auto StackAlignmentValue =
          unsignedInteger32(*StackAlignment, "calling convention stack alignment");
      const auto StackArgumentAlignmentValue =
          unsignedInteger32(*StackArgumentAlignment, "calling convention stack argument alignment");
      const auto ShadowSpaceValue =
          unsignedInteger32(*ShadowSpace, "calling convention shadow space");
      const auto SupportsVarArgsValue = boolean(*SupportsVarArgs, "calling convention varargs");
      const auto SupportsTailCallsValue =
          boolean(*SupportsTailCalls, "calling convention tail calls");
      if (!ID || !IntegerArgumentList || !FloatingArgumentList || !IntegerReturnList ||
          !FloatingReturnList || !StackAlignmentValue || !StackArgumentAlignmentValue ||
          !ShadowSpaceValue || !SupportsVarArgsValue || !SupportsTailCallsValue) {
        return false;
      }
      Result.push_back({
          .ID = *ID,
          .Symbol = *Symbol,
          .Name = *Name,
          .IntegerRegisterClassSymbol = *IntegerClass,
          .FloatingRegisterClassSymbol = *FloatingClass,
          .IntegerArgumentRegisterSymbols = *IntegerArgumentList,
          .FloatingArgumentRegisterSymbols = *FloatingArgumentList,
          .IntegerReturnRegisterSymbols = *IntegerReturnList,
          .FloatingReturnRegisterSymbols = *FloatingReturnList,
          .PreservedMaskName = *PreservedMask,
          .StackAlignmentBytes = *StackAlignmentValue,
          .StackArgumentAlignmentBytes = *StackArgumentAlignmentValue,
          .ShadowSpaceBytes = *ShadowSpaceValue,
          .SupportsVarArgs = *SupportsVarArgsValue,
          .SupportsTailCalls = *SupportsTailCallsValue,
          .Range = Element->range(),
      });
    }
    return true;
  }

  [[nodiscard]] std::optional<SourceOperand> parseOperand(const yaml::Node &Node) {
    if (!requireMapping(&Node, "instruction operand"))
      return std::nullopt;
    if (!checkKeys(Node,
                   {"kinds", "role", "register_class", "register_bank", "immediate_predicate",
                    "memory_role", "default_tied_operand", "early_clobber", "optional",
                    "variadic_group", "allow_subreg", "name", "allowed_target_flags"},
                   "instruction operand")) {
      return std::nullopt;
    }
    const yaml::Node *KindsNode = requiredNode(Node, "kinds", "instruction operand");
    if (KindsNode == nullptr)
      return std::nullopt;
    const auto Kinds = scalarSequence(*KindsNode, "instruction operand kinds");
    if (!Kinds)
      return std::nullopt;

    SourceOperand Result;
    Result.Kinds = *Kinds;
    Result.Range = Node.range();
    if (const yaml::Node *Role = optionalNode(Node, "role")) {
      const auto Value = scalar(*Role, "instruction operand role");
      if (!Value)
        return std::nullopt;
      Result.RegisterRole = *Value;
    }
    if (const yaml::Node *RegisterClass = optionalNode(Node, "register_class")) {
      const auto Value = scalar(*RegisterClass, "instruction operand register_class");
      if (!Value)
        return std::nullopt;
      Result.RegisterClassSymbol = *Value;
    }
    if (const yaml::Node *RegisterBank = optionalNode(Node, "register_bank")) {
      const auto Value = scalar(*RegisterBank, "instruction operand register_bank");
      if (!Value)
        return std::nullopt;
      Result.RegisterBankSymbol = *Value;
    }
    if (const yaml::Node *Predicate = optionalNode(Node, "immediate_predicate")) {
      const auto Value = scalar(*Predicate, "instruction operand immediate_predicate");
      if (!Value)
        return std::nullopt;
      Result.ImmediatePredicateSymbol = *Value;
    }
    if (const yaml::Node *MemoryRole = optionalNode(Node, "memory_role")) {
      const auto Value = scalar(*MemoryRole, "instruction operand memory_role");
      if (!Value)
        return std::nullopt;
      Result.MemoryRole = *Value;
    }
    if (const yaml::Node *Tie = optionalNode(Node, "default_tied_operand")) {
      const auto Value = unsignedInteger32(*Tie, "instruction operand default_tied_operand");
      if (!Value)
        return std::nullopt;
      Result.DefaultTiedOperand = Value;
    }
    if (const yaml::Node *EarlyClobber = optionalNode(Node, "early_clobber")) {
      const auto Value = boolean(*EarlyClobber, "instruction operand early_clobber");
      if (!Value)
        return std::nullopt;
      Result.EarlyClobber = *Value;
    }
    if (const yaml::Node *Optional = optionalNode(Node, "optional")) {
      const auto Value = boolean(*Optional, "instruction operand optional");
      if (!Value)
        return std::nullopt;
      Result.Optional = *Value;
    }
    if (const yaml::Node *VariadicGroup = optionalNode(Node, "variadic_group")) {
      const auto Value = boolean(*VariadicGroup, "instruction operand variadic_group");
      if (!Value)
        return std::nullopt;
      Result.VariadicGroup = *Value;
    }
    if (const yaml::Node *AllowSubReg = optionalNode(Node, "allow_subreg")) {
      const auto Value = boolean(*AllowSubReg, "instruction operand allow_subreg");
      if (!Value)
        return std::nullopt;
      Result.AllowSubReg = *Value;
    }
    if (const yaml::Node *Name = optionalNode(Node, "name")) {
      const auto Value = scalar(*Name, "instruction operand name");
      if (!Value)
        return std::nullopt;
      Result.NameSymbol = *Value;
    }
    if (const yaml::Node *TargetFlags = optionalNode(Node, "allowed_target_flags")) {
      const auto Value =
          unsignedInteger32(*TargetFlags, "instruction operand allowed_target_flags");
      if (!Value)
        return std::nullopt;
      Result.AllowedTargetFlags = *Value;
    }
    return Result;
  }

  [[nodiscard]] std::optional<std::vector<SourceOperand>> parseOperands(const yaml::Node &Node) {
    if (!requireSequence(&Node, "instruction operands"))
      return std::nullopt;
    std::vector<SourceOperand> Result;
    for (const yaml::NodeID ElementID : Node.sequence()) {
      const yaml::Node *Element = Input.node(ElementID);
      if (Element == nullptr)
        return std::nullopt;
      const auto Operand = parseOperand(*Element);
      if (!Operand)
        return std::nullopt;
      Result.push_back(*Operand);
    }
    return Result;
  }

  [[nodiscard]] std::optional<SourceImplicitRegister>
  parseImplicitRegister(const yaml::Node &Node, std::string_view Context) {
    if (!requireMapping(&Node, Context))
      return std::nullopt;
    if (!checkKeys(Node, {"register", "early_clobber", "renamable"}, Context))
      return std::nullopt;
    const auto Register = requiredScalar(Node, "register", Context);
    if (!Register)
      return std::nullopt;
    SourceImplicitRegister Result{
        .RegisterSymbol = *Register,
        .Range = Node.range(),
    };
    if (const yaml::Node *EarlyClobber = optionalNode(Node, "early_clobber")) {
      const auto Value = boolean(*EarlyClobber, std::string(Context) + " early_clobber");
      if (!Value)
        return std::nullopt;
      Result.EarlyClobber = *Value;
    }
    if (const yaml::Node *Renamable = optionalNode(Node, "renamable")) {
      const auto Value = boolean(*Renamable, std::string(Context) + " renamable");
      if (!Value)
        return std::nullopt;
      Result.Renamable = *Value;
    }
    return Result;
  }

  [[nodiscard]] std::optional<std::vector<SourceImplicitRegister>>
  parseImplicitRegisters(const yaml::Node &Node, std::string_view Context) {
    if (!requireSequence(&Node, Context))
      return std::nullopt;
    std::vector<SourceImplicitRegister> Result;
    for (const yaml::NodeID ElementID : Node.sequence()) {
      const yaml::Node *Element = Input.node(ElementID);
      if (Element == nullptr)
        return std::nullopt;
      const auto Register = parseImplicitRegister(*Element, Context);
      if (!Register)
        return std::nullopt;
      Result.push_back(*Register);
    }
    return Result;
  }

  [[nodiscard]] bool parseInstructions(const yaml::Node &Node,
                                       std::vector<SourceInstruction> &Result) {
    if (!requireSequence(&Node, "instructions"))
      return false;
    for (const yaml::NodeID ElementID : Node.sequence()) {
      const yaml::Node *Element = Input.node(ElementID);
      if (!requireMapping(Element, "instruction"))
        return false;
      if (!checkKeys(*Element,
                     {"id", "symbol", "name", "assembly_mnemonic", "encoding", "operands",
                      "num_explicit_defs", "variadic", "implicit_defs", "implicit_uses",
                      "allow_extra_implicit_operands", "flags", "scheduling_class",
                      "encoding_category", "assembly_format", "requires_memory_operands", "owner",
                      "deadline"},
                     "instruction")) {
        return false;
      }
      const yaml::Node *IDNode = requiredNode(*Element, "id", "instruction");
      const auto Symbol = requiredScalar(*Element, "symbol", "instruction");
      const auto Name = requiredScalar(*Element, "name", "instruction");
      const yaml::Node *OperandsNode = requiredNode(*Element, "operands", "instruction");
      const yaml::Node *DefsNode = requiredNode(*Element, "num_explicit_defs", "instruction");
      const yaml::Node *FlagsNode = requiredNode(*Element, "flags", "instruction");
      if (IDNode == nullptr || !Symbol || !Name || OperandsNode == nullptr || DefsNode == nullptr ||
          FlagsNode == nullptr) {
        return false;
      }
      const auto ID = unsignedInteger32(*IDNode, "instruction id");
      const auto Defs = unsignedInteger32(*DefsNode, "instruction num_explicit_defs");
      const auto Operands = parseOperands(*OperandsNode);
      const auto Flags = scalarSequence(*FlagsNode, "instruction flags");
      if (!ID || !Defs || !Operands || !Flags)
        return false;
      SourceInstruction Instruction;
      Instruction.ID = *ID;
      Instruction.Symbol = *Symbol;
      Instruction.Name = *Name;
      Instruction.Operands = *Operands;
      Instruction.NumExplicitDefs = *Defs;
      Instruction.Flags = *Flags;
      Instruction.Range = Element->range();
      if (const yaml::Node *Mnemonic = optionalNode(*Element, "assembly_mnemonic")) {
        const auto Value = scalar(*Mnemonic, "instruction assembly_mnemonic");
        if (!Value)
          return false;
        Instruction.AssemblyMnemonic = *Value;
      }
      if (const yaml::Node *Encoding = optionalNode(*Element, "encoding")) {
        const auto Value = unsignedInteger32(*Encoding, "instruction encoding");
        if (!Value)
          return false;
        Instruction.BinaryEncoding = Value;
      }
      if (const yaml::Node *SchedulingClass = optionalNode(*Element, "scheduling_class")) {
        const auto Value = scalar(*SchedulingClass, "instruction scheduling_class");
        if (!Value)
          return false;
        Instruction.SchedulingClassSymbol = *Value;
      }
      if (const yaml::Node *EncodingCategory = optionalNode(*Element, "encoding_category")) {
        const auto Value = scalar(*EncodingCategory, "instruction encoding_category");
        if (!Value)
          return false;
        Instruction.EncodingCategorySymbol = *Value;
      }
      if (const yaml::Node *AssemblyFormat = optionalNode(*Element, "assembly_format")) {
        const auto Value = scalar(*AssemblyFormat, "instruction assembly_format");
        if (!Value)
          return false;
        Instruction.AssemblyFormatSymbol = *Value;
      }
      if (const yaml::Node *Variadic = optionalNode(*Element, "variadic")) {
        const auto Value = boolean(*Variadic, "instruction variadic");
        if (!Value)
          return false;
        Instruction.Variadic = *Value;
      }
      if (const yaml::Node *ImplicitDefs = optionalNode(*Element, "implicit_defs")) {
        const auto Values = parseImplicitRegisters(*ImplicitDefs, "instruction implicit_defs");
        if (!Values)
          return false;
        Instruction.ImplicitDefs = *Values;
      }
      if (const yaml::Node *ImplicitUses = optionalNode(*Element, "implicit_uses")) {
        const auto Values = parseImplicitRegisters(*ImplicitUses, "instruction implicit_uses");
        if (!Values)
          return false;
        Instruction.ImplicitUses = *Values;
      }
      if (const yaml::Node *AllowExtra = optionalNode(*Element, "allow_extra_implicit_operands")) {
        const auto Value = boolean(*AllowExtra, "instruction allow_extra_implicit_operands");
        if (!Value)
          return false;
        Instruction.AllowExtraImplicitOperands = *Value;
      }
      if (const yaml::Node *MemoryOperands = optionalNode(*Element, "requires_memory_operands")) {
        const auto Value = boolean(*MemoryOperands, "instruction requires_memory_operands");
        if (!Value)
          return false;
        Instruction.RequiresMemoryOperands = *Value;
      }
      if (const yaml::Node *Owner = optionalNode(*Element, "owner")) {
        const auto Value = scalar(*Owner, "instruction owner");
        if (!Value)
          return false;
        Instruction.Owner = *Value;
      }
      if (const yaml::Node *Deadline = optionalNode(*Element, "deadline")) {
        const auto Value = scalar(*Deadline, "instruction deadline");
        if (!Value)
          return false;
        Instruction.Deadline = *Value;
      }
      Result.push_back(std::move(Instruction));
    }
    return true;
  }

  [[nodiscard]] const yaml::Node *optionalNode(const yaml::Node &Mapping,
                                               std::string_view Key) const noexcept {
    const std::optional<yaml::NodeID> Value = Mapping.mappingValue(Key);
    return Value ? Input.node(*Value) : nullptr;
  }

  const yaml::Stream &Input;
  std::vector<TextDiagnostic> Diagnostics;
};

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
  static constexpr std::array<std::pair<std::string_view, std::string_view>, 23> Flags = {{
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
      {"phi", "Phi"},
      {"fake_use", "FakeUse"},
      {"generic", "Generic"},
      {"terminal_emission", "TerminalEmission"},
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
      {"register_allocator", "RegisterAllocator"},
      {"pre_pei_expansion", "PrePEIExpansion"},
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

[[nodiscard]] bool isPowerOfTwo(std::uint64_t Value) noexcept {
  return Value != 0 && (Value & (Value - 1U)) == 0;
}

class ModelNormalizer final {
public:
  ModelNormalizer(const DescriptionAST &Input, std::string SourceName)
      : Input(Input), SourceName(std::move(SourceName)) {}

  [[nodiscard]] std::optional<NormalizedTargetModel> normalize() {
    if (!validateTarget())
      return std::nullopt;

    NormalizedTargetModel Result;
    Result.Target = Input.Target;
    Result.RegisterUnits = Input.RegisterUnits;
    Result.MIROperandFlags = Input.MIROperandFlags;
    if (!normalizeRegisterUnits() || !normalizeRegisters(Result) ||
        !normalizeRegisterBanks(Result) || !normalizeRegisterClasses(Result) ||
        !normalizeRegisterMasks(Result) || !normalizeMIROperandFlags(Result) ||
        !normalizeCallingConventions(Result) || !normalizeNamedOperands(Result) ||
        !normalizeImmediatePredicates(Result) || !normalizeSchedulingClasses(Result) ||
        !normalizeEncodingCategories(Result) || !normalizeAssemblyFormats(Result) ||
        !normalizeInstructions(Result)) {
      return std::nullopt;
    }
    std::ranges::sort(Result.RegisterUnits, {}, &SourceRegisterUnit::ID);
    std::ranges::sort(Result.Registers, {}, &NormalizedRegister::ID);
    std::ranges::sort(Result.RegisterBanks, {}, &NormalizedRegisterBank::ID);
    std::ranges::sort(Result.RegisterClasses, {}, &NormalizedRegisterClass::ID);
    std::ranges::sort(Result.CallingConventions, {}, &NormalizedCallingConvention::ID);
    std::ranges::sort(Result.NamedOperands, {}, &NormalizedNamedOperand::ID);
    std::ranges::sort(Result.ImmediatePredicates, {}, &NormalizedImmediatePredicate::ID);
    std::ranges::sort(Result.SchedulingClasses, {}, &NormalizedSchedulingClass::ID);
    std::ranges::sort(Result.EncodingCategories, {}, &NormalizedEncodingCategory::ID);
    std::ranges::sort(Result.AssemblyFormats, {}, &NormalizedAssemblyFormat::ID);
    return Result;
  }

  [[nodiscard]] std::vector<TextDiagnostic> takeDiagnostics() {
    return std::move(Diagnostics);
  }

private:
  void fail(TextRange Range, std::string Message) {
    Diagnostics.push_back({
        .Severity = DiagnosticSeverity::Error,
        .Category = "target-description",
        .Message = std::move(Message),
        .SourceName = SourceName,
        .Range = Range,
    });
  }

  [[nodiscard]] bool validateTarget() {
    if (!isIdentifier(Input.Target.Namespace)) {
      fail(Input.Target.Range, "target namespace must be a C++ identifier");
      return false;
    }
    if (Input.Target.Name.empty() || Input.Target.Triple.empty() || Input.Target.CPU.empty() ||
        Input.Target.ABI.empty() || Input.Target.DataLayout.empty() ||
        Input.Target.Features.empty()) {
      fail(Input.Target.Range, "target metadata fields and feature list must be nonempty");
      return false;
    }
    std::unordered_set<std::string> Features;
    for (const std::string &Feature : Input.Target.Features) {
      if (Feature.empty() || !Features.insert(Feature).second) {
        fail(Input.Target.Range, "target feature names must be nonempty and unique");
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] bool normalizeRegisterUnits() {
    if (Input.RegisterUnits.empty()) {
      fail(Input.Target.Range, "target description requires at least one register unit");
      return false;
    }
    std::unordered_set<std::uint32_t> IDs;
    std::unordered_set<std::string> Symbols;
    std::unordered_set<std::string> Names;
    for (const SourceRegisterUnit &Unit : Input.RegisterUnits) {
      if (Unit.ID == 0 || !isIdentifier(Unit.Symbol) || Unit.Name.empty() ||
          !IDs.insert(Unit.ID).second || !Symbols.insert(Unit.Symbol).second ||
          !Names.insert(lowerASCII(Unit.Name)).second) {
        fail(Unit.Range,
             "register units require nonzero unique IDs, unique symbols, and unique names");
        return false;
      }
      UnitBySymbol.emplace(Unit.Symbol, Unit.ID);
    }
    return true;
  }

  [[nodiscard]] bool normalizeRegisters(NormalizedTargetModel &Result) {
    if (Input.Registers.empty()) {
      fail(Input.Target.Range, "target description requires at least one physical register");
      return false;
    }
    std::unordered_set<std::uint32_t> IDs;
    std::unordered_set<std::string> Symbols;
    std::unordered_set<std::string> Names;
    std::unordered_set<std::uint32_t> DwarfRegisters;
    for (const SourceRegister &Register : Input.Registers) {
      if (Register.ID == 0 || !isIdentifier(Register.Symbol) || Register.Name.empty() ||
          !IDs.insert(Register.ID).second || !Symbols.insert(Register.Symbol).second ||
          !Names.insert(lowerASCII(Register.Name)).second ||
          !DwarfRegisters.insert(Register.DwarfRegister).second || Register.Units.empty()) {
        fail(Register.Range, "registers require nonzero unique IDs, symbols, canonical names, "
                             "DWARF numbers, and units");
        return false;
      }
      RegisterBySymbol.emplace(Register.Symbol, Register.ID);
      NormalizedRegister Normalized{
          .ID = Register.ID,
          .Symbol = Register.Symbol,
          .Name = Register.Name,
          .Encoding = Register.Encoding,
          .DwarfRegister = Register.DwarfRegister,
          .Units = {},
          .IsConstant = Register.IsConstant,
          .IsArtificial = Register.IsArtificial,
          .IsReserved = Register.IsReserved,
          .AlternativeNames = Register.AlternativeNames,
      };
      std::unordered_set<std::uint32_t> Units;
      for (const SourceRegisterUnitLane &Unit : Register.Units) {
        const auto Position = UnitBySymbol.find(Unit.UnitSymbol);
        if (Position == UnitBySymbol.end() || Unit.Lanes == 0 ||
            !Units.insert(Position->second).second) {
          fail(Unit.Range,
               "register units must name distinct declared units with nonzero lane masks");
          return false;
        }
        Normalized.Units.push_back({.UnitID = Position->second, .Lanes = Unit.Lanes});
      }
      for (const std::string &Alias : Register.AlternativeNames) {
        if (Alias.empty() || !Names.insert(lowerASCII(Alias)).second) {
          fail(Register.Range,
               "register ABI aliases must be nonempty and unique across all register names");
          return false;
        }
      }
      Result.Registers.push_back(std::move(Normalized));
    }
    return true;
  }

  [[nodiscard]] bool normalizeRegisterBanks(NormalizedTargetModel &Result) {
    if (Input.RegisterBanks.empty()) {
      fail(Input.Target.Range, "target description requires at least one register bank");
      return false;
    }
    std::unordered_set<std::uint32_t> IDs;
    std::unordered_set<std::string> Symbols;
    std::unordered_set<std::string> Names;
    for (const SourceRegisterBank &Bank : Input.RegisterBanks) {
      if (Bank.ID == 0 || !isIdentifier(Bank.Symbol) || Bank.Name.empty() ||
          Bank.MaximumSizeInBits == 0 || !IDs.insert(Bank.ID).second ||
          !Symbols.insert(Bank.Symbol).second || !Names.insert(lowerASCII(Bank.Name)).second) {
        fail(Bank.Range,
             "register banks require nonzero unique IDs, symbols, names, and maximum sizes");
        return false;
      }
      BankBySymbol.emplace(Bank.Symbol, Bank.ID);
      Result.RegisterBanks.push_back({
          .ID = Bank.ID,
          .Symbol = Bank.Symbol,
          .Name = Bank.Name,
          .MaximumSizeInBits = Bank.MaximumSizeInBits,
          .RegisterClasses = {},
      });
    }
    return true;
  }

  [[nodiscard]] bool normalizeRegisterClasses(NormalizedTargetModel &Result) {
    if (Input.RegisterClasses.empty()) {
      fail(Input.Target.Range, "target description requires at least one register class");
      return false;
    }
    std::unordered_set<std::uint32_t> IDs;
    std::unordered_set<std::string> Symbols;
    std::unordered_set<std::string> Names;
    for (const SourceRegisterClass &Class : Input.RegisterClasses) {
      const auto Bank = BankBySymbol.find(Class.BankSymbol);
      if (Class.ID == 0 || !isIdentifier(Class.Symbol) || Class.Name.empty() ||
          Bank == BankBySymbol.end() || Class.SizeInBits == 0 || Class.SpillSizeInBits == 0 ||
          Class.SpillAlignmentInBits == 0 || Class.Lanes == 0 || Class.RegisterSymbols.empty() ||
          !IDs.insert(Class.ID).second || !Symbols.insert(Class.Symbol).second ||
          !Names.insert(lowerASCII(Class.Name)).second) {
        fail(Class.Range, "register classes require unique IDs, symbols, names, a declared bank, "
                          "sizes, lanes, and members");
        return false;
      }
      ClassBySymbol.emplace(Class.Symbol, Class.ID);
      NormalizedRegisterClass Normalized{
          .ID = Class.ID,
          .Symbol = Class.Symbol,
          .Name = Class.Name,
          .BankID = Bank->second,
          .SizeInBits = Class.SizeInBits,
          .SpillSizeInBits = Class.SpillSizeInBits,
          .SpillAlignmentInBits = Class.SpillAlignmentInBits,
          .Lanes = Class.Lanes,
          .Registers = {},
          .AllocationOrder = {},
          .Allocatable = Class.Allocatable,
          .CopyCost = Class.CopyCost,
      };
      std::unordered_set<std::uint32_t> Members;
      for (const std::string &Symbol : Class.RegisterSymbols) {
        const auto Register = RegisterBySymbol.find(Symbol);
        if (Register == RegisterBySymbol.end() || !Members.insert(Register->second).second) {
          fail(Class.Range, "register class members must reference distinct declared registers");
          return false;
        }
        Normalized.Registers.push_back(Register->second);
      }
      std::unordered_set<std::uint32_t> Allocation;
      for (const std::string &Symbol : Class.AllocationOrderSymbols) {
        const auto Register = RegisterBySymbol.find(Symbol);
        if (Register == RegisterBySymbol.end() || !Members.contains(Register->second) ||
            !Allocation.insert(Register->second).second) {
          fail(Class.Range,
               "register class allocation order must contain distinct members of the same class");
          return false;
        }
        Normalized.AllocationOrder.push_back(Register->second);
      }
      if ((Normalized.Allocatable && Normalized.AllocationOrder.empty()) ||
          (!Normalized.Allocatable && !Normalized.AllocationOrder.empty())) {
        fail(Class.Range, "allocatable register classes require an allocation order and "
                          "nonallocatable classes forbid one");
        return false;
      }
      Result.RegisterClasses.push_back(std::move(Normalized));
    }
    for (NormalizedRegisterBank &Bank : Result.RegisterBanks) {
      for (const NormalizedRegisterClass &Class : Result.RegisterClasses) {
        if (Class.BankID == Bank.ID)
          Bank.RegisterClasses.push_back(Class.ID);
      }
    }
    return true;
  }

  [[nodiscard]] bool normalizeRegisterMasks(NormalizedTargetModel &Result) {
    std::unordered_set<std::string> Names;
    for (const SourceRegisterMask &Mask : Input.RegisterMasks) {
      if (Mask.Name.empty() || !Names.insert(Mask.Name).second) {
        fail(Mask.Range, "register mask names must be nonempty and unique");
        return false;
      }
      NormalizedRegisterMask Normalized{.Name = Mask.Name, .PreservedRegisters = {}};
      std::unordered_set<std::uint32_t> Preserved;
      for (const std::string &Symbol : Mask.PreservedRegisterSymbols) {
        const auto Register = RegisterBySymbol.find(Symbol);
        if (Register == RegisterBySymbol.end() || !Preserved.insert(Register->second).second) {
          fail(Mask.Range, "register mask entries must name distinct declared registers");
          return false;
        }
        Normalized.PreservedRegisters.push_back(Register->second);
      }
      Result.RegisterMasks.push_back(std::move(Normalized));
    }
    return true;
  }

  [[nodiscard]] bool normalizeMIROperandFlags(NormalizedTargetModel &Result) {
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

  [[nodiscard]] bool normalizeCallingConventions(NormalizedTargetModel &Result) {
    if (Input.CallingConventions.empty()) {
      fail(Input.Target.Range, "target description requires at least one calling convention");
      return false;
    }
    std::unordered_set<std::uint32_t> IDs;
    std::unordered_set<std::string> Symbols;
    std::unordered_set<std::string> Names;
    for (const SourceCallingConvention &Convention : Input.CallingConventions) {
      const auto IntegerClass = ClassBySymbol.find(Convention.IntegerRegisterClassSymbol);
      const auto FloatingClass = ClassBySymbol.find(Convention.FloatingRegisterClassSymbol);
      const auto PreservedMask = std::ranges::find(
          Result.RegisterMasks, Convention.PreservedMaskName, &NormalizedRegisterMask::Name);
      if (Convention.ID == 0 || !isIdentifier(Convention.Symbol) || Convention.Name.empty() ||
          IntegerClass == ClassBySymbol.end() || FloatingClass == ClassBySymbol.end() ||
          PreservedMask == Result.RegisterMasks.end() || Convention.StackAlignmentBytes == 0 ||
          Convention.StackArgumentAlignmentBytes == 0 ||
          !isPowerOfTwo(Convention.StackAlignmentBytes) ||
          !isPowerOfTwo(Convention.StackArgumentAlignmentBytes) ||
          Convention.StackAlignmentBytes % Convention.StackArgumentAlignmentBytes != 0 ||
          Convention.ShadowSpaceBytes % Convention.StackArgumentAlignmentBytes != 0 ||
          !IDs.insert(Convention.ID).second || !Symbols.insert(Convention.Symbol).second ||
          !Names.insert(Convention.Name).second) {
        fail(
            Convention.Range,
            "calling conventions require unique IDs, symbols, names, declared register classes and "
            "preserved masks, compatible power-of-two stack alignments, and aligned shadow space");
        return false;
      }
      const auto IntegerClassDescription = std::ranges::find(
          Result.RegisterClasses, IntegerClass->second, &NormalizedRegisterClass::ID);
      const auto FloatingClassDescription = std::ranges::find(
          Result.RegisterClasses, FloatingClass->second, &NormalizedRegisterClass::ID);
      if (IntegerClassDescription == Result.RegisterClasses.end() ||
          FloatingClassDescription == Result.RegisterClasses.end()) {
        fail(Convention.Range, "calling convention register classes must resolve");
        return false;
      }
      const auto NormalizeRegisterList = [this,
                                          &Convention](const std::vector<std::string> &Symbols,
                                                       const NormalizedRegisterClass &RegisterClass,
                                                       std::vector<std::uint32_t> &Output) {
        std::unordered_set<std::uint32_t> Registers;
        for (const std::string &Symbol : Symbols) {
          const auto Register = RegisterBySymbol.find(Symbol);
          if (Register == RegisterBySymbol.end() ||
              !std::ranges::contains(RegisterClass.Registers, Register->second) ||
              !Registers.insert(Register->second).second) {
            fail(Convention.Range,
                 "calling convention register lists must contain distinct members of their class");
            return false;
          }
          Output.push_back(Register->second);
        }
        return true;
      };
      NormalizedCallingConvention Normalized{
          .ID = Convention.ID,
          .Symbol = Convention.Symbol,
          .Name = Convention.Name,
          .IntegerRegisterClass = IntegerClass->second,
          .FloatingRegisterClass = FloatingClass->second,
          .IntegerArgumentRegisters = {},
          .FloatingArgumentRegisters = {},
          .IntegerReturnRegisters = {},
          .FloatingReturnRegisters = {},
          .PreservedMaskName = Convention.PreservedMaskName,
          .StackAlignmentBytes = Convention.StackAlignmentBytes,
          .StackArgumentAlignmentBytes = Convention.StackArgumentAlignmentBytes,
          .ShadowSpaceBytes = Convention.ShadowSpaceBytes,
          .SupportsVarArgs = Convention.SupportsVarArgs,
          .SupportsTailCalls = Convention.SupportsTailCalls,
      };
      if (!NormalizeRegisterList(Convention.IntegerArgumentRegisterSymbols,
                                 *IntegerClassDescription, Normalized.IntegerArgumentRegisters) ||
          !NormalizeRegisterList(Convention.FloatingArgumentRegisterSymbols,
                                 *FloatingClassDescription, Normalized.FloatingArgumentRegisters) ||
          !NormalizeRegisterList(Convention.IntegerReturnRegisterSymbols, *IntegerClassDescription,
                                 Normalized.IntegerReturnRegisters) ||
          !NormalizeRegisterList(Convention.FloatingReturnRegisterSymbols,
                                 *FloatingClassDescription, Normalized.FloatingReturnRegisters)) {
        return false;
      }
      CallingConventionBySymbol.emplace(Convention.Symbol, Convention.ID);
      Result.CallingConventions.push_back(std::move(Normalized));
    }
    return true;
  }

  [[nodiscard]] bool normalizeNamedOperands(NormalizedTargetModel &Result) {
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

  [[nodiscard]] bool normalizeImmediatePredicates(NormalizedTargetModel &Result) {
    std::unordered_set<std::uint32_t> IDs;
    std::unordered_set<std::string> Symbols;
    std::unordered_set<std::string> Names;
    for (const SourceImmediatePredicate &Predicate : Input.ImmediatePredicates) {
      const bool IsSigned = Predicate.Kind == "signed";
      const bool IsUnsigned = Predicate.Kind == "unsigned";
      if (Predicate.ID == 0 || !isIdentifier(Predicate.Symbol) || Predicate.Name.empty() ||
          (!IsSigned && !IsUnsigned) || Predicate.Bits == 0 || Predicate.Bits > 63 ||
          !isPowerOfTwo(Predicate.Alignment) || !IDs.insert(Predicate.ID).second ||
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

  [[nodiscard]] bool normalizeSchedulingClasses(NormalizedTargetModel &Result) {
    if (Input.SchedulingClasses.empty()) {
      fail(Input.Target.Range, "target description requires at least one scheduling class");
      return false;
    }
    std::unordered_set<std::uint32_t> IDs;
    std::unordered_set<std::string> Symbols;
    std::unordered_set<std::string> Names;
    for (const SourceSchedulingClass &Class : Input.SchedulingClasses) {
      if (Class.ID == 0 || !isIdentifier(Class.Symbol) || Class.Name.empty() ||
          Class.MicroOps == 0 || Class.Latency == 0 || !IDs.insert(Class.ID).second ||
          !Symbols.insert(Class.Symbol).second || !Names.insert(Class.Name).second) {
        fail(Class.Range,
             "scheduling classes require nonzero unique IDs, symbols, names, micro-op counts, "
             "and latencies");
        return false;
      }
      SchedulingClassBySymbol.emplace(Class.Symbol, Class.ID);
      Result.SchedulingClasses.push_back({
          .ID = Class.ID,
          .Symbol = Class.Symbol,
          .Name = Class.Name,
          .MicroOps = Class.MicroOps,
          .Latency = Class.Latency,
      });
    }
    return true;
  }

  [[nodiscard]] bool normalizeEncodingCategories(NormalizedTargetModel &Result) {
    if (Input.EncodingCategories.empty()) {
      fail(Input.Target.Range, "target description requires at least one encoding category");
      return false;
    }
    std::unordered_set<std::uint32_t> IDs;
    std::unordered_set<std::string> Symbols;
    std::unordered_set<std::string> Names;
    for (const SourceEncodingCategory &Category : Input.EncodingCategories) {
      if (Category.ID == 0 || !isIdentifier(Category.Symbol) || Category.Name.empty() ||
          Category.SizeInBytes == 0 || !IDs.insert(Category.ID).second ||
          !Symbols.insert(Category.Symbol).second || !Names.insert(Category.Name).second) {
        fail(Category.Range,
             "encoding categories require nonzero unique IDs, symbols, names, and sizes");
        return false;
      }
      EncodingCategoryBySymbol.emplace(Category.Symbol, Category.ID);
      Result.EncodingCategories.push_back({
          .ID = Category.ID,
          .Symbol = Category.Symbol,
          .Name = Category.Name,
          .SizeInBytes = Category.SizeInBytes,
      });
    }
    return true;
  }

  [[nodiscard]] bool normalizeAssemblyFormats(NormalizedTargetModel &Result) {
    if (Input.AssemblyFormats.empty()) {
      fail(Input.Target.Range, "target description requires at least one assembly format");
      return false;
    }
    std::unordered_set<std::uint32_t> IDs;
    std::unordered_set<std::string> Symbols;
    std::unordered_set<std::string> Names;
    for (const SourceAssemblyFormat &Format : Input.AssemblyFormats) {
      if (Format.ID == 0 || !isIdentifier(Format.Symbol) || Format.Name.empty() ||
          !IDs.insert(Format.ID).second || !Symbols.insert(Format.Symbol).second ||
          !Names.insert(Format.Name).second) {
        fail(Format.Range, "assembly formats require nonzero unique IDs, symbols, and names");
        return false;
      }
      AssemblyFormatBySymbol.emplace(Format.Symbol, Format.ID);
      Result.AssemblyFormats.push_back({
          .ID = Format.ID,
          .Symbol = Format.Symbol,
          .Name = Format.Name,
      });
    }
    return true;
  }

  [[nodiscard]] bool normalizeOperand(const SourceOperand &InputOperand,
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
    Result.AllowedTargetFlags = InputOperand.AllowedTargetFlags;
    return true;
  }

  [[nodiscard]] bool
  normalizeImplicitRegisters(const std::vector<SourceImplicitRegister> &InputRegisters, bool IsDef,
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

  [[nodiscard]] bool normalizeInstructions(NormalizedTargetModel &Result) {
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
      const bool IsTerminalEmissionSource =
          std::ranges::find(Instruction.Flags, "terminal_emission") != Instruction.Flags.end();
      if (HasAnyMachineFact != HasAllMachineFacts ||
          ((!IsPseudoSource || IsTerminalEmissionSource) && !HasAllMachineFacts)) {
        fail(Instruction.Range,
             "encoded instructions and terminal-emission pseudos require complete scheduling, "
             "encoding, mnemonic, and assembly-format facts");
        return false;
      }
      const auto SchedulingClass = SchedulingClassBySymbol.find(Instruction.SchedulingClassSymbol);
      const auto EncodingCategory =
          EncodingCategoryBySymbol.find(Instruction.EncodingCategorySymbol);
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
        const bool IsRegisterOnly =
            Operand.Kinds.size() == 1 && Operand.Kinds.front() == "Register";
        const bool OtherIsRegisterOnly =
            Other.Kinds.size() == 1 && Other.Kinds.front() == "Register";
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
      if (Normalized.RequiresMemoryOperands && !HasFlag("MayLoad") && !HasFlag("MayStore")) {
        fail(Instruction.Range,
             "memory operand requirements need a load or store instruction flag");
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
                                                     std::uint32_t ID) {
  const auto Position = std::ranges::find(Model.Registers, ID, &NormalizedRegister::ID);
  return *Position;
}

[[nodiscard]] const SourceRegisterUnit &findRegisterUnit(const NormalizedTargetModel &Model,
                                                         std::uint32_t ID) {
  const auto Position = std::ranges::find(Model.RegisterUnits, ID, &SourceRegisterUnit::ID);
  return *Position;
}

[[nodiscard]] const NormalizedRegisterBank &findRegisterBank(const NormalizedTargetModel &Model,
                                                             std::uint32_t ID) {
  const auto Position = std::ranges::find(Model.RegisterBanks, ID, &NormalizedRegisterBank::ID);
  return *Position;
}

[[nodiscard]] std::string emitRegisterHeader(const NormalizedTargetModel &Model,
                                             std::string_view SourceName) {
  const std::string Guard = "RCC_TARGET_" + upperIdentifier(Model.Target.Namespace) + "_" +
                            upperIdentifier(Model.Target.Name) + "REGISTERS_H";
  std::ostringstream Output;
  Output << "// SPDX-License-Identifier: GPL-3.0-or-later\n\n";
  Output << "// This file is generated by rcc-backend-descgen from ";
  appendQuoted(Output, SourceName);
  Output << ". Do not edit it manually.\n\n";
  Output << "#ifndef " << Guard << "\n#define " << Guard << "\n\n";
  Output << "#include <cstdint>\n#include <string_view>\n\n";
  Output << "namespace rcc::" << Model.Target.Namespace << " {\n\n";
  Output << "inline constexpr std::string_view TargetName = ";
  appendQuoted(Output, Model.Target.Name);
  Output << ";\ninline constexpr std::string_view TargetTriple = ";
  appendQuoted(Output, Model.Target.Triple);
  Output << ";\ninline constexpr std::string_view TargetCPU = ";
  appendQuoted(Output, Model.Target.CPU);
  Output << ";\ninline constexpr std::string_view TargetABI = ";
  appendQuoted(Output, Model.Target.ABI);
  Output << ";\ninline constexpr std::string_view TargetDataLayout = ";
  appendQuoted(Output, Model.Target.DataLayout);
  Output << ";\n\ninline constexpr std::string_view TargetFeatures[] = {\n";
  for (const std::string &Feature : Model.Target.Features) {
    Output << "    ";
    appendQuoted(Output, Feature);
    Output << ",\n";
  }
  Output << "};\n\n";

  Output << "enum class PhysicalRegisterID : std::uint32_t {\n  NoRegister = 0,\n";
  for (const NormalizedRegister &Register : Model.Registers)
    Output << "  " << Register.Symbol << " = " << Register.ID << ",\n";
  Output << "};\n\n";
  Output << "enum class RegisterUnit : std::uint32_t {\n";
  for (const SourceRegisterUnit &Unit : Model.RegisterUnits)
    Output << "  " << Unit.Symbol << " = " << Unit.ID << ",\n";
  Output << "};\n\n";
  Output << "enum class RegisterBank : std::uint32_t {\n";
  for (const NormalizedRegisterBank &Bank : Model.RegisterBanks)
    Output << "  " << Bank.Symbol << " = " << Bank.ID << ",\n";
  Output << "};\n\n";
  Output << "enum class RegisterClass : std::uint32_t {\n";
  for (const NormalizedRegisterClass &Class : Model.RegisterClasses)
    Output << "  " << Class.Symbol << " = " << Class.ID << ",\n";
  Output << "};\n\n";
  Output << "} // namespace rcc::" << Model.Target.Namespace << "\n\n";
  Output << "#endif // " << Guard << "\n";
  return Output.str();
}

[[nodiscard]] std::string emitInstructionHeader(const NormalizedTargetModel &Model,
                                                std::string_view SourceName) {
  const std::string Guard = "RCC_TARGET_" + upperIdentifier(Model.Target.Namespace) + "_" +
                            upperIdentifier(Model.Target.Name) + "INSTRUCTIONS_H";
  std::ostringstream Output;
  Output << "// SPDX-License-Identifier: GPL-3.0-or-later\n\n";
  Output << "// This file is generated by rcc-backend-descgen from ";
  appendQuoted(Output, SourceName);
  Output << ". Do not edit it manually.\n\n";
  Output << "#ifndef " << Guard << "\n#define " << Guard << "\n\n";
  Output << "#include <cstdint>\n\n";
  Output << "namespace rcc::" << Model.Target.Namespace << " {\n\n";

  Output << "enum class InstructionOpcode : std::uint32_t {\n  Invalid = 0,\n";
  for (const NormalizedInstruction &Instruction : Model.Instructions)
    Output << "  " << Instruction.Symbol << " = " << Instruction.ID << ",\n";
  Output << "};\n\n";

  Output << "enum class ImmediatePredicate : std::uint32_t {\n  None = 0,\n";
  for (const NormalizedImmediatePredicate &Predicate : Model.ImmediatePredicates)
    Output << "  " << Predicate.Symbol << " = " << Predicate.ID << ",\n";
  Output << "};\n\n";

  Output << "enum class NamedOperand : std::uint32_t {\n  None = 0,\n";
  for (const NormalizedNamedOperand &Operand : Model.NamedOperands)
    Output << "  " << Operand.Symbol << " = " << Operand.ID << ",\n";
  Output << "};\n\n";

  Output << "enum class SchedulingClass : std::uint32_t {\n  None = 0,\n";
  for (const NormalizedSchedulingClass &Class : Model.SchedulingClasses)
    Output << "  " << Class.Symbol << " = " << Class.ID << ",\n";
  Output << "};\n\n";

  Output << "enum class EncodingCategory : std::uint32_t {\n  None = 0,\n";
  for (const NormalizedEncodingCategory &Category : Model.EncodingCategories)
    Output << "  " << Category.Symbol << " = " << Category.ID << ",\n";
  Output << "};\n\n";

  Output << "enum class AssemblyFormat : std::uint32_t {\n  None = 0,\n";
  for (const NormalizedAssemblyFormat &Format : Model.AssemblyFormats)
    Output << "  " << Format.Symbol << " = " << Format.ID << ",\n";
  Output << "};\n\n";

  Output << "enum class CallingConvention : std::uint32_t {\n  None = 0,\n";
  for (const NormalizedCallingConvention &Convention : Model.CallingConventions)
    Output << "  " << Convention.Symbol << " = " << Convention.ID << ",\n";
  Output << "};\n\n";

  Output << "} // namespace rcc::" << Model.Target.Namespace << "\n\n";
  Output << "#endif // " << Guard << "\n";
  return Output.str();
}

void emitRegisterIDList(std::ostream &Output, const NormalizedTargetModel &Model,
                        std::span<const std::uint32_t> IDs) {
  for (const std::uint32_t ID : IDs)
    Output << "PhysicalRegisterID::" << findRegister(Model, ID).Symbol << ", ";
}

void emitPhysicalRegisterArray(std::ostream &Output, const NormalizedTargetModel &Model,
                               std::string_view Name, std::span<const std::uint32_t> IDs) {
  Output << "const std::array<PhysicalRegister, " << IDs.size() << "> " << Name << " = {{\n";
  for (const std::uint32_t ID : IDs)
    Output << "    physical(PhysicalRegisterID::" << findRegister(Model, ID).Symbol << "),\n";
  Output << "}};\n";
}

void emitOperandDescriptor(std::ostream &Output, const NormalizedOperand &Operand) {
  Output << "          {.Kinds = OperandKindSet{";
  for (const std::string &Kind : Operand.Kinds)
    Output << "MachineOperand::Kind::" << Kind << ", ";
  Output << "}, .RegisterRole = RegisterOperandRole::" << Operand.RegisterRole;
  Output << ", .RegClass = ";
  if (Operand.RegisterClass)
    Output << "RegisterClassID::fromValue(" << *Operand.RegisterClass << ")";
  else
    Output << "std::nullopt";
  Output << ", .RegBank = ";
  if (Operand.RegisterBank)
    Output << "RegisterBankID::fromValue(" << *Operand.RegisterBank << ")";
  else
    Output << "std::nullopt";
  Output << ", .ImmediatePredicate = ";
  if (Operand.ImmediatePredicate)
    Output << "ImmediatePredicateID::fromValue(" << *Operand.ImmediatePredicate << ")";
  else
    Output << "std::nullopt";
  Output << ", .MemoryRole = MemoryOperandRole::" << Operand.MemoryRole;
  Output << ", .DefaultTiedOperand = ";
  if (Operand.DefaultTiedOperand)
    Output << "std::size_t{" << *Operand.DefaultTiedOperand << "}";
  else
    Output << "std::nullopt";
  Output << ", .EarlyClobber = " << (Operand.EarlyClobber ? "true" : "false")
         << ", .Optional = " << (Operand.Optional ? "true" : "false")
         << ", .VariadicGroup = " << (Operand.VariadicGroup ? "true" : "false")
         << ", .AllowSubReg = " << (Operand.AllowSubReg ? "true" : "false");
  Output << ", .Name = ";
  if (Operand.Name)
    Output << "NamedOperandID::fromValue(" << *Operand.Name << ")";
  else
    Output << "std::nullopt";
  Output << ", .TargetOperandType = std::nullopt, .AllowedTargetFlags = "
         << Operand.AllowedTargetFlags << ", .TypeConstraint = std::nullopt},\n";
}

void emitImplicitRegisterList(std::ostream &Output, const NormalizedTargetModel &Model,
                              std::span<const NormalizedImplicitRegister> Registers) {
  Output << "{";
  for (const NormalizedImplicitRegister &Register : Registers) {
    Output << "{physical(PhysicalRegisterID::" << findRegister(Model, Register.Register).Symbol
           << "), " << (Register.EarlyClobber ? "true" : "false") << ", "
           << (Register.Renamable ? "true" : "false") << "}, ";
  }
  Output << "}";
}

void emitImmediatePredicate(std::ostream &Output, const NormalizedImmediatePredicate &Predicate) {
  const std::int64_t PositiveLimit = std::int64_t{1} << (Predicate.Bits - 1U);
  const std::int64_t Minimum = Predicate.IsSigned ? -PositiveLimit : 0;
  const std::int64_t Maximum =
      Predicate.IsSigned
          ? PositiveLimit - 1
          : static_cast<std::int64_t>((std::uint64_t{1} << Predicate.Bits) - std::uint64_t{1});
  Output << "      {.ID = ImmediatePredicateID::fromValue(" << Predicate.ID << "), .Name = ";
  appendQuoted(Output, Predicate.Name);
  Output << ", .Predicate = [](const MachineOperand &Operand) {\n"
         << "          const auto *Value = Operand.getIf<std::int64_t>();\n"
         << "          return Value != nullptr && *Value >= " << Minimum
         << " && *Value <= " << Maximum;
  if (Predicate.Alignment != 1)
    Output << " && (*Value % " << Predicate.Alignment << ") == 0";
  if (Predicate.NonZero)
    Output << " && *Value != 0";
  if (!Predicate.AllowedValues.empty()) {
    Output << " && (";
    for (std::size_t Index = 0; Index < Predicate.AllowedValues.size(); ++Index) {
      if (Index != 0)
        Output << " || ";
      Output << "*Value == " << Predicate.AllowedValues[Index];
    }
    Output << ")";
  }
  Output << ";\n      }},\n";
}

[[nodiscard]] std::string emitTargetTables(const NormalizedTargetModel &Model,
                                           std::string_view SourceName) {
  std::ostringstream Output;
  Output << "// SPDX-License-Identifier: GPL-3.0-or-later\n\n";
  Output << "// This file is generated by rcc-backend-descgen from ";
  appendQuoted(Output, SourceName);
  Output << ". Do not edit it manually.\n\n";
  Output << "void appendGeneratedRegisterInfo(TargetRegisterInfoDesc &Description) {\n";
  Output << "  Description.RegisterUnits = {\n";
  for (const SourceRegisterUnit &Unit : Model.RegisterUnits) {
    Output << "      {RegisterUnitID::fromValue(" << Unit.ID << "), ";
    appendQuoted(Output, Unit.Name);
    Output << "},\n";
  }
  Output << "  };\n\n  Description.PhysicalRegisters = {\n";
  for (const NormalizedRegister &Register : Model.Registers) {
    Output << "      {physical(PhysicalRegisterID::" << Register.Symbol << "), ";
    appendQuoted(Output, Register.Name);
    Output << ", " << Register.Encoding << ", {";
    for (const NormalizedRegisterUnitLane &Unit : Register.Units) {
      Output << "unitLane(RegisterUnit::" << findRegisterUnit(Model, Unit.UnitID).Symbol << ", "
             << Unit.Lanes << "), ";
    }
    Output << "}, " << (Register.IsConstant ? "true" : "false") << ", "
           << (Register.IsArtificial ? "true" : "false") << ", "
           << (Register.IsReserved ? "true" : "false") << ", {";
    for (const std::string &Alias : Register.AlternativeNames) {
      appendQuoted(Output, Alias);
      Output << ", ";
    }
    Output << "}},\n";
  }
  Output << "  };\n\n  Description.RegisterBanks = {\n";
  for (const NormalizedRegisterBank &Bank : Model.RegisterBanks) {
    Output << "      {RegisterBankID::fromValue(" << Bank.ID << "), ";
    appendQuoted(Output, Bank.Name);
    Output << ", " << Bank.MaximumSizeInBits << ", {";
    for (const std::uint32_t ClassID : Bank.RegisterClasses) {
      const auto Position =
          std::ranges::find(Model.RegisterClasses, ClassID, &NormalizedRegisterClass::ID);
      Output << "RegisterClassID::fromValue(" << Position->ID << "), ";
    }
    Output << "}},\n";
  }
  Output << "  };\n\n  Description.RegisterClasses = {\n";
  for (const NormalizedRegisterClass &Class : Model.RegisterClasses) {
    Output << "      registerClass(RegisterClass::" << Class.Symbol << ", ";
    appendQuoted(Output, Class.Name);
    Output << ", {";
    emitRegisterIDList(Output, Model, Class.Registers);
    Output << "}, {";
    emitRegisterIDList(Output, Model, Class.AllocationOrder);
    Output << "}, RegisterBank::" << findRegisterBank(Model, Class.BankID).Symbol << ", "
           << Class.SizeInBits << ", " << Class.SpillSizeInBits << ", "
           << Class.SpillAlignmentInBits << ", " << Class.Lanes << ", "
           << (Class.Allocatable ? "true" : "false") << ", " << Class.CopyCost << "),\n";
  }
  Output << "  };\n}\n\n";
  Output << "void appendGeneratedMIRTextInfo(TargetMIRTextInfoDesc &Description) {\n";
  Output << "  Description.DirectOperandFlagMask = " << Model.MIROperandFlags.DirectMask
         << ";\n  Description.DirectOperandFlags = {\n";
  for (const SourceMIROperandFlag &Flag : Model.MIROperandFlags.Direct) {
    Output << "      {";
    appendQuoted(Output, Flag.Name);
    Output << ", " << Flag.Value << "},\n";
  }
  Output << "  };\n  Description.BitmaskOperandFlags = {\n";
  for (const SourceMIROperandFlag &Flag : Model.MIROperandFlags.Bitmask) {
    Output << "      {";
    appendQuoted(Output, Flag.Name);
    Output << ", " << Flag.Value << "},\n";
  }
  Output << "  };\n";
  Output << "  Description.RegisterMasks = {\n";
  for (const NormalizedRegisterMask &Mask : Model.RegisterMasks) {
    Output << "      {";
    appendQuoted(Output, Mask.Name);
    Output << ", makePreservedMask({";
    emitRegisterIDList(Output, Model, Mask.PreservedRegisters);
    Output << "})},\n";
  }
  Output << "  };\n  Description.CFIRegisters = {\n";
  for (const NormalizedRegister &Register : Model.Registers) {
    Output << "      {physical(PhysicalRegisterID::" << Register.Symbol << "), "
           << Register.DwarfRegister << ", true},\n";
  }
  Output << "  };\n}\n\n";

  Output
      << "void appendGeneratedInstructionInfo(std::vector<rcc::InstructionDescriptor> "
         "&Descriptors,\n"
      << "                                    std::vector<rcc::ImmediatePredicate> &Predicates) {\n"
      << "  Predicates = {\n";
  for (const NormalizedImmediatePredicate &Predicate : Model.ImmediatePredicates)
    emitImmediatePredicate(Output, Predicate);
  Output << "  };\n  Descriptors = {\n";
  for (const NormalizedInstruction &Instruction : Model.Instructions) {
    Output << "      {.InstructionOpcode = Opcode::fromValue(" << Instruction.ID << "), .Name = ";
    appendQuoted(Output, Instruction.Name);
    Output << ", .Operands = {\n";
    for (const NormalizedOperand &Operand : Instruction.Operands)
      emitOperandDescriptor(Output, Operand);
    Output << "      }, .NumExplicitDefs = " << Instruction.NumExplicitDefs
           << ", .Variadic = " << (Instruction.Variadic ? "true" : "false")
           << ", .MinimumVariadicRepetitions = 0, .ImplicitDefs = ";
    emitImplicitRegisterList(Output, Model, Instruction.ImplicitDefs);
    Output << ", .ImplicitUses = ";
    emitImplicitRegisterList(Output, Model, Instruction.ImplicitUses);
    Output << ", .AllowExtraImplicitOperands = "
           << (Instruction.AllowExtraImplicitOperands ? "true" : "false")
           << ", .Flags = InstructionFlags{";
    for (const std::string &Flag : Instruction.Flags)
      Output << "InstructionFlag::" << Flag << ", ";
    Output << "}, .SchedulingClass = SchedulingClassID::fromValue(" << Instruction.SchedulingClass
           << "), .EncodingCategory = EncodingCategoryID::fromValue("
           << Instruction.EncodingCategory << "), .AssemblyFormat = AssemblyFormatID::fromValue("
           << Instruction.AssemblyFormat << "), .RequiresMemoryOperands = "
           << (Instruction.RequiresMemoryOperands ? "true" : "false")
           << ", .Owner = PseudoOwner::" << Instruction.Owner
           << ", .Deadline = PipelineStage::" << Instruction.Deadline
           << ", .VerifyExplicitOperands = nullptr, "
              ".VerifyInstructionSemantics = nullptr},\n";
  }
  Output << "  };\n}\n\n";

  Output << "constexpr std::array<SchedulingClassInfo, " << Model.SchedulingClasses.size()
         << "> GeneratedSchedulingClasses = {{\n";
  for (const NormalizedSchedulingClass &Class : Model.SchedulingClasses) {
    Output << "    {SchedulingClassID::fromValue(" << Class.ID << "), ";
    appendQuoted(Output, Class.Name);
    Output << ", " << Class.MicroOps << ", " << Class.Latency << "},\n";
  }
  Output << "}};\n\n";

  Output << "constexpr std::array<EncodingCategoryInfo, " << Model.EncodingCategories.size()
         << "> GeneratedEncodingCategories = {{\n";
  for (const NormalizedEncodingCategory &Category : Model.EncodingCategories) {
    Output << "    {EncodingCategoryID::fromValue(" << Category.ID << "), ";
    appendQuoted(Output, Category.Name);
    Output << ", " << Category.SizeInBytes << "},\n";
  }
  Output << "}};\n\n";

  Output << "constexpr std::array<AssemblyFormatInfo, " << Model.AssemblyFormats.size()
         << "> GeneratedAssemblyFormats = {{\n";
  for (const NormalizedAssemblyFormat &Format : Model.AssemblyFormats) {
    Output << "    {AssemblyFormatID::fromValue(" << Format.ID << "), ";
    appendQuoted(Output, Format.Name);
    Output << "},\n";
  }
  Output << "}};\n\n";

  const auto AssemblyInstructionCount =
      std::ranges::count_if(Model.Instructions, [](const NormalizedInstruction &Instruction) {
        return !Instruction.AssemblyMnemonic.empty();
      });
  Output << "constexpr std::array<AssemblyInstructionInfo, " << AssemblyInstructionCount
         << "> GeneratedAssemblyInstructions = {{\n";
  for (const NormalizedInstruction &Instruction : Model.Instructions) {
    if (Instruction.AssemblyMnemonic.empty())
      continue;
    Output << "    {Opcode::fromValue(" << Instruction.ID << "), ";
    appendQuoted(Output, Instruction.AssemblyMnemonic);
    Output << ", AssemblyFormatID::fromValue(" << Instruction.AssemblyFormat << "), "
           << Instruction.BinaryEncoding << "},\n";
  }
  Output << "}};\n\n";

  for (const NormalizedCallingConvention &Convention : Model.CallingConventions) {
    const std::string Prefix = "CC" + std::to_string(Convention.ID);
    emitPhysicalRegisterArray(Output, Model, Prefix + "IntegerArguments",
                              Convention.IntegerArgumentRegisters);
    emitPhysicalRegisterArray(Output, Model, Prefix + "FloatingArguments",
                              Convention.FloatingArgumentRegisters);
    emitPhysicalRegisterArray(Output, Model, Prefix + "IntegerReturns",
                              Convention.IntegerReturnRegisters);
    emitPhysicalRegisterArray(Output, Model, Prefix + "FloatingReturns",
                              Convention.FloatingReturnRegisters);
  }
  Output << "const std::array<CallingConventionInfo, " << Model.CallingConventions.size()
         << "> GeneratedCallingConventions = {{\n";
  for (const NormalizedCallingConvention &Convention : Model.CallingConventions) {
    const std::string Prefix = "CC" + std::to_string(Convention.ID);
    Output << "    {CallingConvention::" << Convention.Symbol << ", ";
    appendQuoted(Output, Convention.Name);
    Output << ", RegisterClassID::fromValue(" << Convention.IntegerRegisterClass
           << "), RegisterClassID::fromValue(" << Convention.FloatingRegisterClass << "), "
           << Prefix << "IntegerArguments, " << Prefix << "FloatingArguments, " << Prefix
           << "IntegerReturns, " << Prefix << "FloatingReturns, ";
    appendQuoted(Output, Convention.PreservedMaskName);
    Output << ", " << Convention.StackAlignmentBytes << ", "
           << Convention.StackArgumentAlignmentBytes << ", " << Convention.ShadowSpaceBytes << ", "
           << (Convention.SupportsVarArgs ? "true" : "false") << ", "
           << (Convention.SupportsTailCalls ? "true" : "false") << "},\n";
  }
  Output << "}};\n";
  return Output.str();
}

} // namespace

DescriptionGenerationResult generateDescription(const std::string &SourceName,
                                                std::string SourceText) {
  yaml::ParseResult YAMLResult = yaml::parse(SourceName, std::move(SourceText));
  if (!YAMLResult.succeeded()) {
    DescriptionGenerationResult Result;
    for (const TextDiagnostic &Diagnostic : YAMLResult.diagnostics())
      Result.Diagnostics.push_back(Diagnostic);
    return Result;
  }

  ASTParser Parser(*YAMLResult.stream());
  const auto AST = Parser.parse();
  if (!AST) {
    DescriptionGenerationResult Result;
    Result.Diagnostics = Parser.takeDiagnostics();
    return Result;
  }

  ModelNormalizer Normalizer(*AST, SourceName);
  const auto Model = Normalizer.normalize();
  if (!Model) {
    DescriptionGenerationResult Result;
    Result.Diagnostics = Normalizer.takeDiagnostics();
    return Result;
  }

  const std::size_t Separator = SourceName.find_last_of("/\\");
  const std::string_view DisplayName = Separator == std::string::npos
                                           ? std::string_view(SourceName)
                                           : std::string_view(SourceName).substr(Separator + 1);
  return {
      .Output =
          GeneratedDescription{
              .RegisterHeader = emitRegisterHeader(*Model, DisplayName),
              .InstructionHeader = emitInstructionHeader(*Model, DisplayName),
              .TargetTables = emitTargetTables(*Model, DisplayName),
          },
      .Diagnostics = {},
  };
}

} // namespace rcc::targetgen
