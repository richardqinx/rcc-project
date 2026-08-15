// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/TableGen/Parser.h"

#include "rcc/Support/YAML.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rcc::targetgen {
namespace {

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
                    "variadic_group", "allow_subreg", "name", "type_index", "allowed_target_flags"},
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
    if (const yaml::Node *TypeIndex = optionalNode(Node, "type_index")) {
      const auto Value = unsignedInteger32(*TypeIndex, "instruction operand type_index");
      if (!Value)
        return std::nullopt;
      if (*Value > std::numeric_limits<std::uint16_t>::max()) {
        fail(TypeIndex->range(), "instruction operand type_index is outside the uint16 range");
        return std::nullopt;
      }
      Result.TypeIndex = static_cast<std::uint16_t>(*Value);
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
                      "encoding_category", "size", "assembly_format", "requires_memory_operands",
                      "owner", "deadline"},
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
      if (const yaml::Node *Size = optionalNode(*Element, "size")) {
        const auto Value = unsignedInteger32(*Size, "instruction size");
        if (!Value)
          return false;
        Instruction.SizeInBytes = *Value;
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

} // namespace

DescriptionParseResult parseDescription(std::string SourceName, std::string SourceText) {
  yaml::ParseResult YAMLResult = yaml::parse(std::move(SourceName), std::move(SourceText));
  if (!YAMLResult.succeeded()) {
    DescriptionParseResult Result;
    Result.Diagnostics.assign(YAMLResult.diagnostics().begin(), YAMLResult.diagnostics().end());
    return Result;
  }

  ASTParser Parser(*YAMLResult.stream());
  auto Records = Parser.parse();
  if (!Records)
    return {.Records = std::nullopt, .Diagnostics = Parser.takeDiagnostics()};
  return {.Records = std::move(Records), .Diagnostics = {}};
}

} // namespace rcc::targetgen
