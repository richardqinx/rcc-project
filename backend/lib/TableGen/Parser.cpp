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
                    "assembly_formats", "calling_conventions", "instructions", "selection_rules",
                    "combiner_rules", "macro_fusion_rules", "mc_pseudo_expansions"},
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
    const yaml::Node *SelectionRulesNode = optionalNode(*Root, "selection_rules");
    const yaml::Node *CombinerRulesNode = optionalNode(*Root, "combiner_rules");
    const yaml::Node *MacroFusionRulesNode = optionalNode(*Root, "macro_fusion_rules");
    const yaml::Node *MCPseudoExpansionsNode = optionalNode(*Root, "mc_pseudo_expansions");
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
        !parseInstructions(*InstructionsNode, Result.Instructions) ||
        (SelectionRulesNode != nullptr &&
         !parseSelectionRules(*SelectionRulesNode, Result.SelectionRules)) ||
        (CombinerRulesNode != nullptr &&
         !parseCombinerRules(*CombinerRulesNode, Result.CombinerRules)) ||
        (MacroFusionRulesNode != nullptr &&
         !parseMacroFusionRules(*MacroFusionRulesNode, Result.MacroFusionRules)) ||
        (MCPseudoExpansionsNode != nullptr &&
         !parseMCPseudoExpansions(*MCPseudoExpansionsNode, Result.MCPseudoExpansions))) {
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

  [[nodiscard]] std::optional<std::vector<std::uint32_t>>
  unsignedInteger32Sequence(const yaml::Node &Node, std::string_view Context) {
    if (!requireSequence(&Node, Context))
      return std::nullopt;
    std::vector<std::uint32_t> Result;
    for (const yaml::NodeID Value : Node.sequence()) {
      const yaml::Node *Element = Input.node(Value);
      if (Element == nullptr)
        return std::nullopt;
      const auto Parsed = unsignedInteger32(*Element, Context);
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
                      "superclasses", "allocatable", "copy_cost"},
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
          .SuperClassSymbols = {},
          .Range = Element->range(),
      };
      if (const yaml::Node *SuperClasses = optionalNode(*Element, "superclasses")) {
        const auto Values = scalarSequence(*SuperClasses, "register class superclasses");
        if (!Values)
          return false;
        RegisterClass.SuperClassSymbols = *Values;
      }
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

  [[nodiscard]] std::optional<SourceSelectionTypePredicate>
  parseSelectionTypePredicate(const yaml::Node &Node, std::string_view Context) {
    if (!requireMapping(&Node, Context))
      return std::nullopt;
    if (!checkKeys(Node, {"kind", "bits"}, Context))
      return std::nullopt;
    const auto Kind = requiredScalar(Node, "kind", Context);
    const yaml::Node *BitsNode = requiredNode(Node, "bits", Context);
    if (!Kind || BitsNode == nullptr)
      return std::nullopt;
    const auto Bits = unsignedInteger32(*BitsNode, std::string(Context) + " bits");
    if (!Bits)
      return std::nullopt;
    return SourceSelectionTypePredicate{
        .Kind = *Kind,
        .Bits = *Bits,
        .Range = Node.range(),
    };
  }

  [[nodiscard]] bool
  parseSelectionOperandTypes(const yaml::Node &Node,
                             std::vector<SourceSelectionOperandTypePredicate> &Result) {
    if (!requireSequence(&Node, "selection operand type predicates"))
      return false;
    for (const yaml::NodeID ElementID : Node.sequence()) {
      const yaml::Node *Element = Input.node(ElementID);
      if (!requireMapping(Element, "selection operand type predicate") ||
          !checkKeys(*Element, {"index", "kind", "bits"}, "selection operand type predicate")) {
        return false;
      }
      const yaml::Node *IndexNode =
          requiredNode(*Element, "index", "selection operand type predicate");
      const auto Kind = requiredScalar(*Element, "kind", "selection operand type predicate");
      const yaml::Node *BitsNode =
          requiredNode(*Element, "bits", "selection operand type predicate");
      if (IndexNode == nullptr || !Kind || BitsNode == nullptr)
        return false;
      const auto Index = unsignedInteger32(*IndexNode, "selection operand type index");
      const auto Bits = unsignedInteger32(*BitsNode, "selection operand type bits");
      if (!Index || !Bits)
        return false;
      Result.push_back({
          .OperandIndex = *Index,
          .Type = {.Kind = *Kind, .Bits = *Bits, .Range = Element->range()},
          .Range = Element->range(),
      });
    }
    return true;
  }

  [[nodiscard]] bool
  parseSelectionOperandBanks(const yaml::Node &Node,
                             std::vector<SourceSelectionOperandBankPredicate> &Result) {
    if (!requireSequence(&Node, "selection operand bank predicates"))
      return false;
    for (const yaml::NodeID ElementID : Node.sequence()) {
      const yaml::Node *Element = Input.node(ElementID);
      if (!requireMapping(Element, "selection operand bank predicate") ||
          !checkKeys(*Element, {"index", "bank"}, "selection operand bank predicate")) {
        return false;
      }
      const yaml::Node *IndexNode =
          requiredNode(*Element, "index", "selection operand bank predicate");
      const auto Bank = requiredScalar(*Element, "bank", "selection operand bank predicate");
      if (IndexNode == nullptr || !Bank)
        return false;
      const auto Index = unsignedInteger32(*IndexNode, "selection operand bank index");
      if (!Index)
        return false;
      Result.push_back({
          .OperandIndex = *Index,
          .BankSymbol = *Bank,
          .Range = Element->range(),
      });
    }
    return true;
  }

  [[nodiscard]] bool
  parseSelectionOperandClasses(const yaml::Node &Node,
                               std::vector<SourceSelectionOperandClassPredicate> &Result) {
    if (!requireSequence(&Node, "selection operand class predicates"))
      return false;
    for (const yaml::NodeID ElementID : Node.sequence()) {
      const yaml::Node *Element = Input.node(ElementID);
      if (!requireMapping(Element, "selection operand class predicate") ||
          !checkKeys(*Element, {"index", "class"}, "selection operand class predicate")) {
        return false;
      }
      const yaml::Node *IndexNode =
          requiredNode(*Element, "index", "selection operand class predicate");
      const auto Class = requiredScalar(*Element, "class", "selection operand class predicate");
      if (IndexNode == nullptr || !Class)
        return false;
      const auto Index = unsignedInteger32(*IndexNode, "selection operand class index");
      if (!Index)
        return false;
      Result.push_back({
          .OperandIndex = *Index,
          .ClassSymbol = *Class,
          .Range = Element->range(),
      });
    }
    return true;
  }

  [[nodiscard]] bool
  parseSelectionIntegerPredicate(const yaml::Node &Node,
                                 std::optional<SourceSelectionIntegerPredicate> &Result) {
    if (!requireMapping(&Node, "selection integer predicate") ||
        !checkKeys(Node, {"index", "value"}, "selection integer predicate")) {
      return false;
    }
    const yaml::Node *IndexNode = requiredNode(Node, "index", "selection integer predicate");
    const auto Value = requiredScalar(Node, "value", "selection integer predicate");
    if (IndexNode == nullptr || !Value)
      return false;
    const auto Index = unsignedInteger32(*IndexNode, "selection integer predicate index");
    if (!Index)
      return false;
    Result = SourceSelectionIntegerPredicate{
        .OperandIndex = *Index,
        .Predicate = *Value,
        .Range = Node.range(),
    };
    return true;
  }

  [[nodiscard]] bool
  parseSelectionFloatingPredicate(const yaml::Node &Node,
                                  std::optional<SourceSelectionFloatingPredicate> &Result) {
    if (!requireMapping(&Node, "selection floating predicate") ||
        !checkKeys(Node, {"index", "value"}, "selection floating predicate")) {
      return false;
    }
    const yaml::Node *IndexNode = requiredNode(Node, "index", "selection floating predicate");
    const auto Value = requiredScalar(Node, "value", "selection floating predicate");
    if (IndexNode == nullptr || !Value)
      return false;
    const auto Index = unsignedInteger32(*IndexNode, "selection floating predicate index");
    if (!Index)
      return false;
    Result = SourceSelectionFloatingPredicate{
        .OperandIndex = *Index,
        .Predicate = *Value,
        .Range = Node.range(),
    };
    return true;
  }

  [[nodiscard]] bool
  parseSelectionMemoryPredicate(const yaml::Node &Node,
                                std::optional<SourceSelectionMemoryPredicate> &Result) {
    if (!requireMapping(&Node, "selection memory predicate") ||
        !checkKeys(Node, {"index", "bytes"}, "selection memory predicate")) {
      return false;
    }
    const yaml::Node *IndexNode = requiredNode(Node, "index", "selection memory predicate");
    const yaml::Node *BytesNode = requiredNode(Node, "bytes", "selection memory predicate");
    if (IndexNode == nullptr || BytesNode == nullptr)
      return false;
    const auto Index = unsignedInteger32(*IndexNode, "selection memory predicate index");
    const auto Bytes = unsignedInteger32(*BytesNode, "selection memory predicate bytes");
    if (!Index || !Bytes)
      return false;
    Result = SourceSelectionMemoryPredicate{
        .MemoryOperandIndex = *Index,
        .Bytes = *Bytes,
        .Range = Node.range(),
    };
    return true;
  }

  [[nodiscard]] bool
  parseSelectionTargetPredicates(const yaml::Node &Node,
                                 std::vector<SourceSelectionTargetPredicate> &Result) {
    if (!requireSequence(&Node, "selection target predicates"))
      return false;
    for (const yaml::NodeID ElementID : Node.sequence()) {
      const yaml::Node *Element = Input.node(ElementID);
      if (Element == nullptr)
        return false;
      const auto Symbol = scalar(*Element, "selection target predicate");
      if (!Symbol)
        return false;
      Result.push_back({
          .Symbol = *Symbol,
          .Range = Element->range(),
      });
    }
    return true;
  }

  [[nodiscard]] bool parseSelectionConstantRanges(const yaml::Node &Node,
                                                  std::vector<SourceSelectionConstantRange> &Result,
                                                  std::string_view Context) {
    if (!requireSequence(&Node, Context) || Node.sequence().empty())
      return false;
    for (const yaml::NodeID RangeID : Node.sequence()) {
      const yaml::Node *Range = Input.node(RangeID);
      const std::string RangeContext = std::string(Context) + " range";
      if (!requireMapping(Range, RangeContext) ||
          !checkKeys(*Range, {"min", "max"}, RangeContext)) {
        return false;
      }
      const yaml::Node *MinimumNode = requiredNode(*Range, "min", RangeContext);
      const yaml::Node *MaximumNode = requiredNode(*Range, "max", RangeContext);
      if (MinimumNode == nullptr || MaximumNode == nullptr)
        return false;
      const auto Minimum = signedInteger(*MinimumNode, "selection constant range minimum");
      const auto Maximum = signedInteger(*MaximumNode, "selection constant range maximum");
      if (!Minimum || !Maximum || *Minimum > *Maximum) {
        if (Minimum && Maximum)
          fail(Range->range(), "selection constant range is invalid");
        return false;
      }
      Result.push_back({
          .Minimum = *Minimum,
          .Maximum = *Maximum,
          .SourceRange = Range->range(),
      });
    }
    return true;
  }

  [[nodiscard]] bool
  parseSelectionOperandImmediates(const yaml::Node &Node,
                                  std::vector<SourceSelectionOperandImmediatePredicate> &Result) {
    if (!requireSequence(&Node, "selection operand immediates"))
      return false;
    for (const yaml::NodeID ElementID : Node.sequence()) {
      const yaml::Node *Element = Input.node(ElementID);
      if (!requireMapping(Element, "selection operand immediate") ||
          !checkKeys(*Element, {"index", "ranges"}, "selection operand immediate")) {
        return false;
      }
      const yaml::Node *IndexNode = requiredNode(*Element, "index", "selection operand immediate");
      const yaml::Node *RangesNode =
          requiredNode(*Element, "ranges", "selection operand immediate");
      if (IndexNode == nullptr || RangesNode == nullptr)
        return false;
      const auto Index = unsignedInteger32(*IndexNode, "selection operand immediate index");
      if (!Index)
        return false;

      SourceSelectionOperandImmediatePredicate Predicate{
          .OperandIndex = *Index,
          .Ranges = {},
          .Range = Element->range(),
      };
      if (!parseSelectionConstantRanges(*RangesNode, Predicate.Ranges,
                                        "selection operand immediate ranges")) {
        return false;
      }
      Result.push_back(std::move(Predicate));
    }
    return true;
  }

  [[nodiscard]] bool parseSelectionOperandDefConstants(
      const yaml::Node &Node, std::vector<SourceSelectionOperandDefConstantPredicate> &Result) {
    if (!requireSequence(&Node, "selection operand definition constants"))
      return false;
    for (const yaml::NodeID ElementID : Node.sequence()) {
      const yaml::Node *Element = Input.node(ElementID);
      if (!requireMapping(Element, "selection operand definition constant") ||
          !checkKeys(*Element, {"index", "ranges"}, "selection operand definition constant")) {
        return false;
      }
      const yaml::Node *IndexNode =
          requiredNode(*Element, "index", "selection operand definition constant");
      const yaml::Node *RangesNode =
          requiredNode(*Element, "ranges", "selection operand definition constant");
      if (IndexNode == nullptr || RangesNode == nullptr)
        return false;
      const auto Index =
          unsignedInteger32(*IndexNode, "selection operand definition constant index");
      if (!Index) {
        return false;
      }

      SourceSelectionOperandDefConstantPredicate Predicate{
          .OperandIndex = *Index,
          .Ranges = {},
          .Range = Element->range(),
      };
      if (!parseSelectionConstantRanges(*RangesNode, Predicate.Ranges,
                                        "selection operand definition constant ranges")) {
        return false;
      }
      Result.push_back(std::move(Predicate));
    }
    return true;
  }

  [[nodiscard]] bool
  parseSelectionDefUsePredicates(const yaml::Node &Node,
                                 std::vector<SourceSelectionDefUsePredicate> &Result) {
    if (!requireSequence(&Node, "selection def-use predicates"))
      return false;
    for (const yaml::NodeID ElementID : Node.sequence()) {
      const yaml::Node *Element = Input.node(ElementID);
      if (!requireMapping(Element, "selection def-use predicate") ||
          !checkKeys(
              *Element,
              {"operand", "opcode", "integer_predicate", "captures", "operand_def_constants"},
              "selection def-use predicate")) {
        return false;
      }
      const yaml::Node *OperandNode =
          requiredNode(*Element, "operand", "selection def-use predicate");
      const auto Opcode = requiredScalar(*Element, "opcode", "selection def-use predicate");
      if (OperandNode == nullptr || !Opcode)
        return false;
      const auto Operand = unsignedInteger32(*OperandNode, "selection def-use operand");
      if (!Operand)
        return false;

      SourceSelectionDefUsePredicate Predicate{
          .RootOperandIndex = *Operand,
          .OpcodeSymbol = *Opcode,
          .IntegerPredicate = std::nullopt,
          .Captures = {},
          .OperandDefConstants = {},
          .Range = Element->range(),
      };
      if (const yaml::Node *IntegerPredicate = optionalNode(*Element, "integer_predicate")) {
        if (!parseSelectionIntegerPredicate(*IntegerPredicate, Predicate.IntegerPredicate))
          return false;
      }
      if (const yaml::Node *Captures = optionalNode(*Element, "captures")) {
        if (!requireSequence(Captures, "selection def-use captures"))
          return false;
        for (const yaml::NodeID CaptureID : Captures->sequence()) {
          const yaml::Node *Capture = Input.node(CaptureID);
          if (!requireMapping(Capture, "selection def-use capture") ||
              !checkKeys(*Capture, {"id", "operand"}, "selection def-use capture")) {
            return false;
          }
          const auto Symbol = requiredScalar(*Capture, "id", "selection def-use capture");
          const yaml::Node *CaptureOperand =
              requiredNode(*Capture, "operand", "selection def-use capture");
          if (!Symbol || CaptureOperand == nullptr)
            return false;
          const auto Index =
              unsignedInteger32(*CaptureOperand, "selection def-use capture operand");
          if (!Index)
            return false;
          Predicate.Captures.push_back({
              .Symbol = *Symbol,
              .OperandIndex = *Index,
              .Range = Capture->range(),
          });
        }
      }
      if (const yaml::Node *Constants = optionalNode(*Element, "operand_def_constants")) {
        if (!parseSelectionOperandDefConstants(*Constants, Predicate.OperandDefConstants))
          return false;
      }
      if (!Predicate.IntegerPredicate && Predicate.Captures.empty() &&
          Predicate.OperandDefConstants.empty()) {
        fail(Element->range(),
             "selection def-use predicate requires a capture or constant predicate");
        return false;
      }
      Result.push_back(std::move(Predicate));
    }
    return true;
  }

  [[nodiscard]] bool
  parseSelectionOperandRenderers(const yaml::Node &Node,
                                 std::vector<SourceSelectionOperandRenderer> &Result) {
    if (!requireSequence(&Node, "selection operand renderers"))
      return false;
    for (const yaml::NodeID ElementID : Node.sequence()) {
      const yaml::Node *Element = Input.node(ElementID);
      if (!requireMapping(Element, "selection operand renderer") ||
          !checkKeys(*Element,
                     {"operand", "complex", "physical_register", "temporary", "captured_operand",
                      "captured_immediate", "immediate", "transform", "flags"},
                     "selection operand renderer")) {
        return false;
      }
      const yaml::Node *OperandNode = optionalNode(*Element, "operand");
      const yaml::Node *ComplexNode = optionalNode(*Element, "complex");
      const yaml::Node *PhysicalRegisterNode = optionalNode(*Element, "physical_register");
      const yaml::Node *TemporaryNode = optionalNode(*Element, "temporary");
      const yaml::Node *CapturedOperandNode = optionalNode(*Element, "captured_operand");
      const yaml::Node *CapturedImmediateNode = optionalNode(*Element, "captured_immediate");
      const yaml::Node *ImmediateNode = optionalNode(*Element, "immediate");
      const std::uint32_t SourceCount =
          static_cast<std::uint32_t>(ComplexNode != nullptr) +
          static_cast<std::uint32_t>(ComplexNode == nullptr && OperandNode != nullptr) +
          static_cast<std::uint32_t>(PhysicalRegisterNode != nullptr) +
          static_cast<std::uint32_t>(TemporaryNode != nullptr) +
          static_cast<std::uint32_t>(CapturedOperandNode != nullptr) +
          static_cast<std::uint32_t>(CapturedImmediateNode != nullptr) +
          static_cast<std::uint32_t>(ImmediateNode != nullptr);
      if (SourceCount != 1) {
        fail(Element->range(), "selection operand renderer requires exactly one source");
        return false;
      }
      SourceSelectionOperandRenderer Renderer{
          .OperandIndex = std::nullopt,
          .ComplexOperandIndex = std::nullopt,
          .PhysicalRegisterSymbol = {},
          .ComplexRendererSymbol = {},
          .TemporarySymbol = {},
          .CapturedOperandSymbol = {},
          .CapturedImmediateOperand = std::nullopt,
          .Immediate = std::nullopt,
          .ImmediateTransform = "identity",
          .Flags = {},
          .Range = Element->range(),
      };
      if (ComplexNode != nullptr) {
        if (OperandNode == nullptr) {
          fail(Element->range(), "complex selection operand renderer requires an operand index");
          return false;
        }
        const auto Name = scalar(*ComplexNode, "selection operand renderer complex matcher");
        const auto Index = unsignedInteger32(*OperandNode, "selection operand renderer operand");
        if (!Name || !Index)
          return false;
        Renderer.ComplexRendererSymbol = *Name;
        Renderer.ComplexOperandIndex = Index;
      } else if (OperandNode != nullptr) {
        const auto Index = unsignedInteger32(*OperandNode, "selection operand renderer operand");
        if (!Index)
          return false;
        Renderer.OperandIndex = Index;
      } else if (PhysicalRegisterNode != nullptr) {
        const auto Symbol =
            scalar(*PhysicalRegisterNode, "selection operand renderer physical register");
        if (!Symbol)
          return false;
        Renderer.PhysicalRegisterSymbol = *Symbol;
      } else if (TemporaryNode != nullptr) {
        const auto Symbol = scalar(*TemporaryNode, "selection operand renderer temporary");
        if (!Symbol)
          return false;
        Renderer.TemporarySymbol = *Symbol;
      } else if (CapturedOperandNode != nullptr) {
        const auto Symbol =
            scalar(*CapturedOperandNode, "selection operand renderer captured operand");
        if (!Symbol)
          return false;
        Renderer.CapturedOperandSymbol = *Symbol;
      } else if (CapturedImmediateNode != nullptr) {
        const auto Index = unsignedInteger32(*CapturedImmediateNode,
                                             "selection operand renderer captured immediate");
        if (!Index)
          return false;
        Renderer.CapturedImmediateOperand = Index;
      } else {
        const auto Value = signedInteger(*ImmediateNode, "selection operand renderer immediate");
        if (!Value)
          return false;
        Renderer.Immediate = Value;
      }
      if (const yaml::Node *TransformNode = optionalNode(*Element, "transform")) {
        const auto Transform = scalar(*TransformNode, "selection operand renderer transform");
        if (!Transform)
          return false;
        Renderer.ImmediateTransform = *Transform;
      }
      if (!Renderer.CapturedImmediateOperand && Renderer.ImmediateTransform != "identity") {
        fail(Element->range(),
             "selection operand renderer transform requires a captured immediate");
        return false;
      }
      if (const yaml::Node *FlagsNode = optionalNode(*Element, "flags")) {
        const auto Flags = scalarSequence(*FlagsNode, "selection operand renderer flags");
        if (!Flags)
          return false;
        Renderer.Flags = *Flags;
      }
      Result.push_back(std::move(Renderer));
    }
    return true;
  }

  [[nodiscard]] bool parseSelectionTemporaries(const yaml::Node &Node,
                                               std::vector<SourceSelectionTemporary> &Result) {
    if (!requireSequence(&Node, "selection temporaries"))
      return false;
    for (const yaml::NodeID ElementID : Node.sequence()) {
      const yaml::Node *Element = Input.node(ElementID);
      if (!requireMapping(Element, "selection temporary") ||
          !checkKeys(*Element, {"id", "type_from_operand", "type"}, "selection temporary")) {
        return false;
      }
      const auto Symbol = requiredScalar(*Element, "id", "selection temporary");
      const yaml::Node *TypeFromOperandNode = optionalNode(*Element, "type_from_operand");
      const yaml::Node *TypeNode = optionalNode(*Element, "type");
      if (!Symbol)
        return false;
      if ((TypeFromOperandNode == nullptr) == (TypeNode == nullptr)) {
        fail(Element->range(),
             "selection temporary requires exactly one of type_from_operand and type");
        return false;
      }
      std::optional<std::uint32_t> TypeFromOperand;
      std::optional<SourceSelectionTypePredicate> Type;
      if (TypeFromOperandNode != nullptr) {
        TypeFromOperand =
            unsignedInteger32(*TypeFromOperandNode, "selection temporary type operand");
        if (!TypeFromOperand)
          return false;
      } else {
        Type = parseSelectionTypePredicate(*TypeNode, "selection temporary type");
        if (!Type)
          return false;
      }
      Result.push_back({
          .Symbol = *Symbol,
          .TypeFromOperand = TypeFromOperand,
          .Type = std::move(Type),
          .Range = Element->range(),
      });
    }
    return true;
  }

  [[nodiscard]] bool parseSelectionActions(const yaml::Node &Node,
                                           std::vector<SourceSelectionAction> &Result) {
    if (!requireSequence(&Node, "selection instructions"))
      return false;
    for (const yaml::NodeID ElementID : Node.sequence()) {
      const yaml::Node *Element = Input.node(ElementID);
      if (!requireMapping(Element, "selection instruction") ||
          !checkKeys(*Element, {"opcode", "root", "operands"}, "selection instruction")) {
        return false;
      }
      const auto Opcode = requiredScalar(*Element, "opcode", "selection instruction");
      const yaml::Node *RootNode = requiredNode(*Element, "root", "selection instruction");
      const yaml::Node *OperandsNode = requiredNode(*Element, "operands", "selection instruction");
      if (!Opcode || RootNode == nullptr || OperandsNode == nullptr)
        return false;
      const auto ReplacesRoot = boolean(*RootNode, "selection instruction root");
      if (!ReplacesRoot)
        return false;
      std::vector<SourceSelectionOperandRenderer> Renderers;
      if (!parseSelectionOperandRenderers(*OperandsNode, Renderers))
        return false;
      Result.push_back({
          .ResultOpcodeSymbol = *Opcode,
          .ReplacesRoot = *ReplacesRoot,
          .OperandRenderers = std::move(Renderers),
          .Range = Element->range(),
      });
    }
    return true;
  }

  [[nodiscard]] bool parseSelectionRuleMatch(const yaml::Node &Node, SourceSelectionRule &Result) {
    if (!requireMapping(&Node, "selection rule match") ||
        !checkKeys(Node,
                   {"opcode", "result_type", "integer_predicate", "floating_predicate", "memory",
                    "target_predicates", "operand_types", "operand_banks", "operand_classes",
                    "operand_immediates", "operand_def_constants", "def_use"},
                   "selection rule match")) {
      return false;
    }
    const auto Opcode = requiredScalar(Node, "opcode", "selection rule match");
    if (!Opcode)
      return false;
    Result.MatchOpcodeSymbol = *Opcode;
    if (const yaml::Node *ResultType = optionalNode(Node, "result_type")) {
      auto Parsed = parseSelectionTypePredicate(*ResultType, "selection result type predicate");
      if (!Parsed)
        return false;
      Result.ResultType = std::move(*Parsed);
    }
    if (const yaml::Node *Predicate = optionalNode(Node, "integer_predicate")) {
      if (!parseSelectionIntegerPredicate(*Predicate, Result.IntegerPredicate))
        return false;
    }
    if (const yaml::Node *Predicate = optionalNode(Node, "floating_predicate")) {
      if (!parseSelectionFloatingPredicate(*Predicate, Result.FloatingPredicate))
        return false;
    }
    if (const yaml::Node *Predicate = optionalNode(Node, "memory")) {
      if (!parseSelectionMemoryPredicate(*Predicate, Result.MemoryPredicate))
        return false;
    }
    if (const yaml::Node *TargetPredicates = optionalNode(Node, "target_predicates")) {
      if (!parseSelectionTargetPredicates(*TargetPredicates, Result.TargetPredicates))
        return false;
    }
    if (const yaml::Node *OperandTypes = optionalNode(Node, "operand_types")) {
      if (!parseSelectionOperandTypes(*OperandTypes, Result.OperandTypes))
        return false;
    }
    if (const yaml::Node *OperandBanks = optionalNode(Node, "operand_banks")) {
      if (!parseSelectionOperandBanks(*OperandBanks, Result.OperandBanks))
        return false;
    }
    if (const yaml::Node *OperandClasses = optionalNode(Node, "operand_classes")) {
      if (!parseSelectionOperandClasses(*OperandClasses, Result.OperandClasses))
        return false;
    }
    if (const yaml::Node *OperandImmediates = optionalNode(Node, "operand_immediates")) {
      if (!parseSelectionOperandImmediates(*OperandImmediates, Result.OperandImmediates))
        return false;
    }
    if (const yaml::Node *OperandDefConstants = optionalNode(Node, "operand_def_constants")) {
      if (!parseSelectionOperandDefConstants(*OperandDefConstants, Result.OperandDefConstants))
        return false;
    }
    if (const yaml::Node *DefUse = optionalNode(Node, "def_use")) {
      if (!parseSelectionDefUsePredicates(*DefUse, Result.DefUsePredicates))
        return false;
    }
    return true;
  }

  [[nodiscard]] bool parseSelectionRuleApply(const yaml::Node &Node, SourceSelectionRule &Result) {
    if (!requireMapping(&Node, "selection rule apply") ||
        !checkKeys(Node, {"temporaries", "instructions", "erase_operand_definitions"},
                   "selection rule apply")) {
      return false;
    }
    const yaml::Node *Instructions = requiredNode(Node, "instructions", "selection rule apply");
    if (Instructions == nullptr)
      return false;
    if (const yaml::Node *Temporaries = optionalNode(Node, "temporaries")) {
      if (!parseSelectionTemporaries(*Temporaries, Result.Temporaries))
        return false;
    }
    if (!parseSelectionActions(*Instructions, Result.Actions))
      return false;
    if (const yaml::Node *Erase = optionalNode(Node, "erase_operand_definitions")) {
      auto Indices = unsignedInteger32Sequence(*Erase, "selection operand definitions");
      if (!Indices)
        return false;
      Result.EraseOperandDefinitions = std::move(*Indices);
    }
    return true;
  }

  [[nodiscard]] bool parseSelectionRules(const yaml::Node &Node,
                                         std::vector<SourceSelectionRule> &Result) {
    if (!requireSequence(&Node, "selection_rules"))
      return false;
    for (const yaml::NodeID ElementID : Node.sequence()) {
      const yaml::Node *Element = Input.node(ElementID);
      if (!requireMapping(Element, "selection rule") ||
          !checkKeys(*Element, {"id", "priority", "match", "apply"}, "selection rule")) {
        return false;
      }
      const auto Symbol = requiredScalar(*Element, "id", "selection rule");
      const yaml::Node *PriorityNode = requiredNode(*Element, "priority", "selection rule");
      const yaml::Node *MatchNode = requiredNode(*Element, "match", "selection rule");
      const yaml::Node *ApplyNode = requiredNode(*Element, "apply", "selection rule");
      if (!Symbol || PriorityNode == nullptr || MatchNode == nullptr || ApplyNode == nullptr)
        return false;
      const auto Priority = unsignedInteger32(*PriorityNode, "selection rule priority");
      if (!Priority)
        return false;
      SourceSelectionRule Rule;
      Rule.Symbol = *Symbol;
      Rule.Priority = *Priority;
      Rule.Range = Element->range();
      if (!parseSelectionRuleMatch(*MatchNode, Rule) ||
          !parseSelectionRuleApply(*ApplyNode, Rule)) {
        return false;
      }
      Result.push_back(std::move(Rule));
    }
    return true;
  }

  [[nodiscard]] bool
  parseCombinerOperandRenderers(const yaml::Node &Node,
                                std::vector<SourceCombinerOperandRenderer> &Result) {
    if (!requireSequence(&Node, "combiner operand renderers"))
      return false;
    for (const yaml::NodeID ElementID : Node.sequence()) {
      const yaml::Node *Element = Input.node(ElementID);
      if (!requireMapping(Element, "combiner operand renderer") ||
          !checkKeys(*Element, {"operand", "captured_operand", "captured_type_bits", "immediate"},
                     "combiner operand renderer")) {
        return false;
      }
      const yaml::Node *Operand = optionalNode(*Element, "operand");
      const yaml::Node *CapturedOperand = optionalNode(*Element, "captured_operand");
      const yaml::Node *CapturedTypeBits = optionalNode(*Element, "captured_type_bits");
      const yaml::Node *Immediate = optionalNode(*Element, "immediate");
      const std::uint32_t SourceCount = static_cast<std::uint32_t>(Operand != nullptr) +
                                        static_cast<std::uint32_t>(CapturedOperand != nullptr) +
                                        static_cast<std::uint32_t>(CapturedTypeBits != nullptr) +
                                        static_cast<std::uint32_t>(Immediate != nullptr);
      if (SourceCount != 1) {
        fail(Element->range(), "combiner operand renderer requires exactly one source");
        return false;
      }
      SourceCombinerOperandRenderer Renderer{.SourceKind =
                                                 SourceCombinerOperandRenderer::Kind::RootOperand,
                                             .OperandIndex = 0,
                                             .CaptureSymbol = {},
                                             .Immediate = 0,
                                             .Range = Element->range()};
      if (Operand != nullptr) {
        const auto Index = unsignedInteger32(*Operand, "combiner operand renderer operand");
        if (!Index)
          return false;
        Renderer.OperandIndex = *Index;
      } else if (CapturedOperand != nullptr) {
        const auto Symbol = scalar(*CapturedOperand, "combiner operand renderer captured operand");
        if (!Symbol)
          return false;
        Renderer.SourceKind = SourceCombinerOperandRenderer::Kind::CapturedOperand;
        Renderer.CaptureSymbol = *Symbol;
      } else if (CapturedTypeBits != nullptr) {
        const auto Symbol =
            scalar(*CapturedTypeBits, "combiner operand renderer captured type bits");
        if (!Symbol)
          return false;
        Renderer.SourceKind = SourceCombinerOperandRenderer::Kind::CapturedTypeBits;
        Renderer.CaptureSymbol = *Symbol;
      } else {
        const auto Value = signedInteger(*Immediate, "combiner operand renderer immediate");
        if (!Value)
          return false;
        Renderer.SourceKind = SourceCombinerOperandRenderer::Kind::Immediate;
        Renderer.Immediate = *Value;
      }
      Result.push_back(std::move(Renderer));
    }
    return true;
  }

  [[nodiscard]] bool parseCombinerRuleMatch(const yaml::Node &Node, SourceCombinerRule &Result) {
    if (!requireMapping(&Node, "combiner rule match") ||
        !checkKeys(
            Node,
            {"opcode", "result_type", "operand_types", "target_predicates", "flags", "def_use"},
            "combiner rule match")) {
      return false;
    }
    const auto Opcode = requiredScalar(Node, "opcode", "combiner rule match");
    if (!Opcode)
      return false;
    Result.MatchOpcodeSymbol = *Opcode;
    if (const yaml::Node *Type = optionalNode(Node, "result_type")) {
      auto Parsed = parseSelectionTypePredicate(*Type, "combiner result type predicate");
      if (!Parsed)
        return false;
      Result.ResultType = std::move(*Parsed);
    }
    if (const yaml::Node *Types = optionalNode(Node, "operand_types")) {
      if (!parseSelectionOperandTypes(*Types, Result.OperandTypes))
        return false;
    }
    if (const yaml::Node *Predicates = optionalNode(Node, "target_predicates")) {
      if (!parseSelectionTargetPredicates(*Predicates, Result.TargetPredicates))
        return false;
    }
    if (const yaml::Node *Flags = optionalNode(Node, "flags")) {
      const auto Parsed = scalarSequence(*Flags, "combiner instruction flags");
      if (!Parsed)
        return false;
      Result.InstructionFlags = *Parsed;
    }
    if (const yaml::Node *DefUse = optionalNode(Node, "def_use")) {
      if (!parseSelectionDefUsePredicates(*DefUse, Result.DefUsePredicates))
        return false;
    }
    return true;
  }

  [[nodiscard]] bool parseCombinerRuleApply(const yaml::Node &Node, SourceCombinerRule &Result) {
    if (!requireMapping(&Node, "combiner rule apply") ||
        !checkKeys(
            Node,
            {"action", "capture", "opcode", "operands", "clear_flags", "erase_operand_definitions"},
            "combiner rule apply")) {
      return false;
    }
    const auto Action = requiredScalar(Node, "action", "combiner rule apply");
    if (!Action)
      return false;
    if (*Action == "replace_root_with_capture") {
      const auto Capture = requiredScalar(Node, "capture", "combiner rule apply");
      if (!Capture)
        return false;
      Result.Action.Kind = SourceCombinerActionKind::ReplaceRootWithCapturedOperand;
      Result.Action.CapturedOperandSymbol = *Capture;
    } else if (*Action == "mutate_root") {
      const auto Opcode = requiredScalar(Node, "opcode", "combiner rule apply");
      const yaml::Node *Operands = requiredNode(Node, "operands", "combiner rule apply");
      if (!Opcode || Operands == nullptr)
        return false;
      Result.Action.Kind = SourceCombinerActionKind::MutateRoot;
      Result.Action.ResultOpcodeSymbol = *Opcode;
      if (!parseCombinerOperandRenderers(*Operands, Result.Action.OperandRenderers))
        return false;
    } else {
      fail(Node.range(),
           "combiner rule apply action must be replace_root_with_capture or mutate_root");
      return false;
    }
    if (const yaml::Node *ClearFlags = optionalNode(Node, "clear_flags")) {
      const auto Parsed = scalarSequence(*ClearFlags, "combiner action clear flags");
      if (!Parsed)
        return false;
      Result.Action.ClearInstructionFlags = *Parsed;
    }
    if (const yaml::Node *Erase = optionalNode(Node, "erase_operand_definitions")) {
      auto Indices = unsignedInteger32Sequence(*Erase, "combiner operand definitions");
      if (!Indices)
        return false;
      Result.EraseOperandDefinitions = std::move(*Indices);
    }
    return true;
  }

  [[nodiscard]] bool parseCombinerRules(const yaml::Node &Node,
                                        std::vector<SourceCombinerRule> &Result) {
    if (!requireSequence(&Node, "combiner_rules"))
      return false;
    for (const yaml::NodeID ElementID : Node.sequence()) {
      const yaml::Node *Element = Input.node(ElementID);
      if (!requireMapping(Element, "combiner rule") ||
          !checkKeys(*Element, {"id", "priority", "match", "apply"}, "combiner rule")) {
        return false;
      }
      const auto Symbol = requiredScalar(*Element, "id", "combiner rule");
      const yaml::Node *PriorityNode = requiredNode(*Element, "priority", "combiner rule");
      const yaml::Node *MatchNode = requiredNode(*Element, "match", "combiner rule");
      const yaml::Node *ApplyNode = requiredNode(*Element, "apply", "combiner rule");
      if (!Symbol || PriorityNode == nullptr || MatchNode == nullptr || ApplyNode == nullptr)
        return false;
      const auto Priority = unsignedInteger32(*PriorityNode, "combiner rule priority");
      if (!Priority)
        return false;
      SourceCombinerRule Rule;
      Rule.Symbol = *Symbol;
      Rule.Priority = *Priority;
      Rule.Range = Element->range();
      if (!parseCombinerRuleMatch(*MatchNode, Rule) || !parseCombinerRuleApply(*ApplyNode, Rule))
        return false;
      Result.push_back(std::move(Rule));
    }
    return true;
  }

  [[nodiscard]] bool parseMacroFusionOpcodePairs(const yaml::Node &Node,
                                                 std::vector<SourceMacroFusionOpcodePair> &Result) {
    if (!requireSequence(&Node, "macro fusion opcode pairs"))
      return false;
    for (const yaml::NodeID ElementID : Node.sequence()) {
      const yaml::Node *Element = Input.node(ElementID);
      if (!requireMapping(Element, "macro fusion opcode pair") ||
          !checkKeys(*Element, {"first", "second"}, "macro fusion opcode pair")) {
        return false;
      }
      const auto First = requiredScalar(*Element, "first", "macro fusion opcode pair");
      const auto Second = requiredScalar(*Element, "second", "macro fusion opcode pair");
      if (!First || !Second)
        return false;
      Result.push_back({*First, *Second, Element->range()});
    }
    return true;
  }

  [[nodiscard]] bool
  parseMacroFusionImmediatePredicates(const yaml::Node &Node,
                                      std::vector<SourceMacroFusionImmediatePredicate> &Result) {
    if (!requireSequence(&Node, "macro fusion immediate predicates"))
      return false;
    for (const yaml::NodeID ElementID : Node.sequence()) {
      const yaml::Node *Element = Input.node(ElementID);
      if (!requireMapping(Element, "macro fusion immediate predicate") ||
          !checkKeys(*Element, {"instruction", "operand", "min", "max"},
                     "macro fusion immediate predicate")) {
        return false;
      }
      const auto Instruction =
          requiredScalar(*Element, "instruction", "macro fusion immediate predicate");
      const yaml::Node *OperandNode =
          requiredNode(*Element, "operand", "macro fusion immediate predicate");
      const yaml::Node *MinimumNode =
          requiredNode(*Element, "min", "macro fusion immediate predicate");
      const yaml::Node *MaximumNode =
          requiredNode(*Element, "max", "macro fusion immediate predicate");
      if (!Instruction || OperandNode == nullptr || MinimumNode == nullptr ||
          MaximumNode == nullptr) {
        return false;
      }
      const auto Operand = unsignedInteger32(*OperandNode, "macro fusion immediate operand");
      const auto Minimum = signedInteger(*MinimumNode, "macro fusion immediate minimum");
      const auto Maximum = signedInteger(*MaximumNode, "macro fusion immediate maximum");
      if (!Operand || !Minimum || !Maximum)
        return false;
      Result.push_back({*Instruction, *Operand, *Minimum, *Maximum, Element->range()});
    }
    return true;
  }

  [[nodiscard]] bool
  parseMacroFusionRegisterInequalities(const yaml::Node &Node,
                                       std::vector<SourceMacroFusionRegisterInequality> &Result) {
    if (!requireSequence(&Node, "macro fusion register inequalities"))
      return false;
    for (const yaml::NodeID ElementID : Node.sequence()) {
      const yaml::Node *Element = Input.node(ElementID);
      if (!requireMapping(Element, "macro fusion register inequality") ||
          !checkKeys(*Element, {"instruction", "left", "right"},
                     "macro fusion register inequality")) {
        return false;
      }
      const auto Instruction =
          requiredScalar(*Element, "instruction", "macro fusion register inequality");
      const yaml::Node *LeftNode =
          requiredNode(*Element, "left", "macro fusion register inequality");
      const yaml::Node *RightNode =
          requiredNode(*Element, "right", "macro fusion register inequality");
      if (!Instruction || LeftNode == nullptr || RightNode == nullptr)
        return false;
      const auto Left = unsignedInteger32(*LeftNode, "macro fusion left register operand");
      const auto Right = unsignedInteger32(*RightNode, "macro fusion right register operand");
      if (!Left || !Right)
        return false;
      Result.push_back({*Instruction, *Left, *Right, Element->range()});
    }
    return true;
  }

  [[nodiscard]] bool
  parseMacroFusionImmediateRelations(const yaml::Node &Node,
                                     std::vector<SourceMacroFusionImmediateRelation> &Result) {
    if (!requireSequence(&Node, "macro fusion immediate relations"))
      return false;
    for (const yaml::NodeID ElementID : Node.sequence()) {
      const yaml::Node *Element = Input.node(ElementID);
      if (!requireMapping(Element, "macro fusion immediate relation") ||
          !checkKeys(*Element, {"kind", "first_operand", "second_operand"},
                     "macro fusion immediate relation")) {
        return false;
      }
      const auto Kind = requiredScalar(*Element, "kind", "macro fusion immediate relation");
      const yaml::Node *FirstNode =
          requiredNode(*Element, "first_operand", "macro fusion immediate relation");
      const yaml::Node *SecondNode =
          requiredNode(*Element, "second_operand", "macro fusion immediate relation");
      if (!Kind || FirstNode == nullptr || SecondNode == nullptr)
        return false;
      const auto First = unsignedInteger32(*FirstNode, "macro fusion first immediate operand");
      const auto Second = unsignedInteger32(*SecondNode, "macro fusion second immediate operand");
      if (!First || !Second)
        return false;
      Result.push_back({*Kind, *First, *Second, Element->range()});
    }
    return true;
  }

  [[nodiscard]] bool parseMacroFusionLink(const yaml::Node &Node, SourceMacroFusionRule &Result) {
    if (!requireMapping(&Node, "macro fusion link") ||
        !checkKeys(Node,
                   {"first_definition", "second_use", "second_definition", "commutable_second_use",
                    "one_non_debug_use", "second_destination_tied"},
                   "macro fusion link")) {
      return false;
    }
    const yaml::Node *FirstNode = requiredNode(Node, "first_definition", "macro fusion link");
    const yaml::Node *SecondUseNode = requiredNode(Node, "second_use", "macro fusion link");
    const yaml::Node *SecondDefinitionNode =
        requiredNode(Node, "second_definition", "macro fusion link");
    const yaml::Node *SingleUseNode = requiredNode(Node, "one_non_debug_use", "macro fusion link");
    const yaml::Node *TiedNode = requiredNode(Node, "second_destination_tied", "macro fusion link");
    if (FirstNode == nullptr || SecondUseNode == nullptr || SecondDefinitionNode == nullptr ||
        SingleUseNode == nullptr || TiedNode == nullptr) {
      return false;
    }
    const auto First = unsignedInteger32(*FirstNode, "macro fusion first definition");
    const auto SecondUse = unsignedInteger32(*SecondUseNode, "macro fusion second use");
    const auto SecondDefinition =
        unsignedInteger32(*SecondDefinitionNode, "macro fusion second definition");
    const auto SingleUse = boolean(*SingleUseNode, "macro fusion one_non_debug_use");
    const auto Tied = boolean(*TiedNode, "macro fusion second_destination_tied");
    if (!First || !SecondUse || !SecondDefinition || !SingleUse || !Tied)
      return false;
    Result.FirstDefinitionOperand = *First;
    Result.SecondUseOperand = *SecondUse;
    Result.SecondDefinitionOperand = *SecondDefinition;
    Result.RequireSingleNonDebugUse = *SingleUse;
    Result.RequireSecondDestinationTie = *Tied;
    if (const yaml::Node *Commutable = optionalNode(Node, "commutable_second_use")) {
      const auto Value = unsignedInteger32(*Commutable, "macro fusion commutable second use");
      if (!Value)
        return false;
      Result.CommutableSecondUseOperand = Value;
    }
    return true;
  }

  [[nodiscard]] bool parseMacroFusionRules(const yaml::Node &Node,
                                           std::vector<SourceMacroFusionRule> &Result) {
    if (!requireSequence(&Node, "macro_fusion_rules"))
      return false;
    for (const yaml::NodeID ElementID : Node.sequence()) {
      const yaml::Node *Element = Input.node(ElementID);
      if (!requireMapping(Element, "macro fusion rule") ||
          !checkKeys(*Element,
                     {"id", "feature", "first_opcodes", "second_opcodes", "opcode_pairs",
                      "immediates", "register_not_equal", "immediate_relations", "link"},
                     "macro fusion rule")) {
        return false;
      }
      const auto Symbol = requiredScalar(*Element, "id", "macro fusion rule");
      const auto Feature = requiredScalar(*Element, "feature", "macro fusion rule");
      const yaml::Node *FirstNode = requiredNode(*Element, "first_opcodes", "macro fusion rule");
      const yaml::Node *SecondNode = requiredNode(*Element, "second_opcodes", "macro fusion rule");
      const yaml::Node *LinkNode = requiredNode(*Element, "link", "macro fusion rule");
      if (!Symbol || !Feature || FirstNode == nullptr || SecondNode == nullptr ||
          LinkNode == nullptr) {
        return false;
      }
      auto FirstOpcodes = scalarSequence(*FirstNode, "macro fusion first opcodes");
      auto SecondOpcodes = scalarSequence(*SecondNode, "macro fusion second opcodes");
      if (!FirstOpcodes || !SecondOpcodes)
        return false;
      SourceMacroFusionRule Rule;
      Rule.Symbol = *Symbol;
      Rule.Feature = *Feature;
      Rule.FirstOpcodeSymbols = std::move(*FirstOpcodes);
      Rule.SecondOpcodeSymbols = std::move(*SecondOpcodes);
      Rule.Range = Element->range();
      if (!parseMacroFusionLink(*LinkNode, Rule))
        return false;
      if (const yaml::Node *Pairs = optionalNode(*Element, "opcode_pairs")) {
        if (!parseMacroFusionOpcodePairs(*Pairs, Rule.OpcodePairs))
          return false;
      }
      if (const yaml::Node *Immediates = optionalNode(*Element, "immediates")) {
        if (!parseMacroFusionImmediatePredicates(*Immediates, Rule.ImmediatePredicates))
          return false;
      }
      if (const yaml::Node *Inequalities = optionalNode(*Element, "register_not_equal")) {
        if (!parseMacroFusionRegisterInequalities(*Inequalities, Rule.RegisterInequalities))
          return false;
      }
      if (const yaml::Node *Relations = optionalNode(*Element, "immediate_relations")) {
        if (!parseMacroFusionImmediateRelations(*Relations, Rule.ImmediateRelations))
          return false;
      }
      Result.push_back(std::move(Rule));
    }
    return true;
  }

  [[nodiscard]] bool parseMCPseudoOperands(const yaml::Node &Node,
                                           std::vector<SourceMCPseudoOperand> &Result) {
    if (!requireSequence(&Node, "MC pseudo expansion operands"))
      return false;
    for (const yaml::NodeID ElementID : Node.sequence()) {
      const yaml::Node *Element = Input.node(ElementID);
      if (!requireMapping(Element, "MC pseudo expansion operand") ||
          !checkKeys(*Element, {"operand", "register", "immediate"},
                     "MC pseudo expansion operand")) {
        return false;
      }
      SourceMCPseudoOperand Operand;
      Operand.Range = Element->range();
      if (const yaml::Node *SourceNode = optionalNode(*Element, "operand")) {
        const auto Source = unsignedInteger32(*SourceNode, "MC pseudo source operand");
        if (!Source)
          return false;
        Operand.SourceOperandIndex = Source;
      }
      if (const yaml::Node *RegisterNode = optionalNode(*Element, "register")) {
        const auto Register = scalar(*RegisterNode, "MC pseudo fixed register");
        if (!Register)
          return false;
        Operand.RegisterSymbol = *Register;
      }
      if (const yaml::Node *ImmediateNode = optionalNode(*Element, "immediate")) {
        const auto Immediate = signedInteger(*ImmediateNode, "MC pseudo fixed immediate");
        if (!Immediate)
          return false;
        Operand.Immediate = Immediate;
      }
      Result.push_back(std::move(Operand));
    }
    return true;
  }

  [[nodiscard]] bool parseMCPseudoExpansions(const yaml::Node &Node,
                                             std::vector<SourceMCPseudoExpansion> &Result) {
    if (!requireSequence(&Node, "mc_pseudo_expansions"))
      return false;
    for (const yaml::NodeID ElementID : Node.sequence()) {
      const yaml::Node *Element = Input.node(ElementID);
      if (!requireMapping(Element, "MC pseudo expansion") ||
          !checkKeys(*Element, {"pseudo", "opcode", "operands"}, "MC pseudo expansion")) {
        return false;
      }
      const auto Pseudo = requiredScalar(*Element, "pseudo", "MC pseudo expansion");
      const auto Opcode = requiredScalar(*Element, "opcode", "MC pseudo expansion");
      const yaml::Node *OperandsNode = requiredNode(*Element, "operands", "MC pseudo expansion");
      if (!Pseudo || !Opcode || OperandsNode == nullptr)
        return false;
      SourceMCPseudoExpansion Expansion;
      Expansion.PseudoOpcodeSymbol = *Pseudo;
      Expansion.ResultOpcodeSymbol = *Opcode;
      Expansion.Range = Element->range();
      if (!parseMCPseudoOperands(*OperandsNode, Expansion.Operands))
        return false;
      Result.push_back(std::move(Expansion));
    }
    return true;
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
