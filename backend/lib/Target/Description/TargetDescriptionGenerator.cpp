// SPDX-License-Identifier: GPL-3.0-or-later

#include "TargetDescriptionGenerator.h"

#include "rcc/Support/YAML.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
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

struct DescriptionAST final {
  SourceTarget Target;
  std::vector<SourceRegisterUnit> RegisterUnits;
  std::vector<SourceRegister> Registers;
  std::vector<SourceRegisterBank> RegisterBanks;
  std::vector<SourceRegisterClass> RegisterClasses;
  std::vector<SourceRegisterMask> RegisterMasks;
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

struct NormalizedTargetModel final {
  SourceTarget Target;
  std::vector<SourceRegisterUnit> RegisterUnits;
  std::vector<NormalizedRegister> Registers;
  std::vector<NormalizedRegisterBank> RegisterBanks;
  std::vector<NormalizedRegisterClass> RegisterClasses;
  std::vector<NormalizedRegisterMask> RegisterMasks;
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
                    "register_classes", "register_masks"},
                   "target description root")) {
      return std::nullopt;
    }

    const yaml::Node *SchemaNode = requiredNode(*Root, "schema", "target description root");
    if (SchemaNode == nullptr)
      return std::nullopt;
    const auto Schema = scalar(*SchemaNode, "target description schema");
    if (!Schema || *Schema != "rcc-target-description-v1") {
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
    if (UnitsNode == nullptr || RegistersNode == nullptr || BanksNode == nullptr ||
        ClassesNode == nullptr || MasksNode == nullptr ||
        !parseRegisterUnits(*UnitsNode, Result.RegisterUnits) ||
        !parseRegisters(*RegistersNode, Result.Registers) ||
        !parseRegisterBanks(*BanksNode, Result.RegisterBanks) ||
        !parseRegisterClasses(*ClassesNode, Result.RegisterClasses) ||
        !parseRegisterMasks(*MasksNode, Result.RegisterMasks)) {
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

  [[nodiscard]] const yaml::Node *optionalNode(const yaml::Node &Mapping,
                                               std::string_view Key) const noexcept {
    const std::optional<yaml::NodeID> Value = Mapping.mappingValue(Key);
    return Value ? Input.node(*Value) : nullptr;
  }

  const yaml::Stream &Input;
  std::vector<TextDiagnostic> Diagnostics;
};

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
    if (!normalizeRegisterUnits() || !normalizeRegisters(Result) ||
        !normalizeRegisterBanks(Result) || !normalizeRegisterClasses(Result) ||
        !normalizeRegisterMasks(Result)) {
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

  const DescriptionAST &Input;
  std::string SourceName;
  std::unordered_map<std::string, std::uint32_t> UnitBySymbol;
  std::unordered_map<std::string, std::uint32_t> RegisterBySymbol;
  std::unordered_map<std::string, std::uint32_t> BankBySymbol;
  std::unordered_map<std::string, std::uint32_t> ClassBySymbol;
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

void emitRegisterIDList(std::ostream &Output, const NormalizedTargetModel &Model,
                        std::span<const std::uint32_t> IDs) {
  for (const std::uint32_t ID : IDs)
    Output << "PhysicalRegisterID::" << findRegister(Model, ID).Symbol << ", ";
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
  Output << "  };\n}\n";
  return Output.str();
}

} // namespace

DescriptionGenerationResult generateDescription(std::string SourceName, std::string SourceText) {
  yaml::ParseResult YAMLResult = yaml::parse(SourceName, SourceText);
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
              .TargetTables = emitTargetTables(*Model, DisplayName),
          },
      .Diagnostics = {},
  };
}

} // namespace rcc::targetgen
