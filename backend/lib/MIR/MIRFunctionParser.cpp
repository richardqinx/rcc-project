// SPDX-License-Identifier: GPL-3.0-or-later

#include "MIRDocumentParser.h"
#include "MIRDocumentReferenceValidation.h"
#include "MIRMetadataTextParser.h"
#include "MIRParserUtilities.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineConstantPool.h"
#include "rcc/CodeGen/MachineFrameInfo.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineFunctionAttributes.h"
#include "rcc/CodeGen/MachineFunctionProperties.h"
#include "rcc/CodeGen/MachineFunctionRecords.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineJumpTableInfo.h"
#include "rcc/CodeGen/MachineMetadata.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/MachineVerifier.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/CodeGen/TargetMIRTextInfo.h"
#include "rcc/CodeGen/TargetRegisterInfo.h"
#include "rcc/MIR/LLVMConstantSyntax.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rcc::mir {

using parser_detail::parseBool;
using parser_detail::parsePrefixedID;
using parser_detail::parseSigned;
using parser_detail::parseUnsigned;
using parser_detail::parseUnsigned32;
using parser_detail::trim;

bool MIRDocumentParser::parseFunctionScalars(const yaml::Node &Root, MachineFunction &Function,
                                             MIRFunctionDocumentInfo &DocumentInfo) {
  if (const yaml::Node *AlignNode = mappingValue(Root, "alignment")) {
    Alignment Align;
    if (alignment(*AlignNode, Align, true))
      return true;
    if (Align.bytes() != 1 || AlignNode->scalar() != "0")
      Function.attributes().setAlignment(Align);
  }

  constexpr std::array PropertyBindings{
      std::pair{"legalized", MachineFunctionProperty::Legalized},
      std::pair{"regBankSelected", MachineFunctionProperty::RegBankSelected},
      std::pair{"selected", MachineFunctionProperty::Selected},
      std::pair{"failedISel", MachineFunctionProperty::FailedISel},
      std::pair{"tracksRegLiveness", MachineFunctionProperty::TracksLiveness},
      std::pair{"noPhis", MachineFunctionProperty::NoPHIs},
      std::pair{"isSSA", MachineFunctionProperty::IsSSA},
      std::pair{"noVRegs", MachineFunctionProperty::NoVRegs},
      std::pair{"failsVerification", MachineFunctionProperty::FailsVerification},
  };
  for (const auto &[Key, Property] : PropertyBindings) {
    const yaml::Node *ValueNode = mappingValue(Root, Key);
    if (ValueNode == nullptr)
      continue;
    bool Value = false;
    if (boolean(*ValueNode, Value))
      return true;
    if (Value)
      Function.properties().set(Property);
    else
      Function.properties().clear(Property);
    if (Key == std::string_view("noPhis"))
      DocumentInfo.NoPHIs = Value;
    else if (Key == std::string_view("isSSA"))
      DocumentInfo.IsSSA = Value;
    else if (Key == std::string_view("noVRegs"))
      DocumentInfo.NoVRegs = Value;
  }

  constexpr std::array AttributeBindings{
      std::pair{"exposesReturnsTwice", MachineFunctionAttribute::ExposesReturnsTwice},
      std::pair{"hasWinCFI", MachineFunctionAttribute::HasWinCFI},
      std::pair{"hasFakeUses", MachineFunctionAttribute::HasFakeUses},
      std::pair{"callsEHReturn", MachineFunctionAttribute::CallsEHReturn},
      std::pair{"callsUnwindInit", MachineFunctionAttribute::CallsUnwindInit},
      std::pair{"hasEHContTarget", MachineFunctionAttribute::HasEHContTarget},
      std::pair{"hasEHScopes", MachineFunctionAttribute::HasEHScopes},
      std::pair{"hasEHFunclets", MachineFunctionAttribute::HasEHFunclets},
      std::pair{"isOutlined", MachineFunctionAttribute::IsOutlined},
      std::pair{"debugInstrRef", MachineFunctionAttribute::DebugInstrRef},
      std::pair{"tracksDebugUserValues", MachineFunctionAttribute::TracksDebugUserValues},
  };
  for (const auto &[Key, Attribute] : AttributeBindings) {
    const yaml::Node *ValueNode = mappingValue(Root, Key);
    if (ValueNode == nullptr)
      continue;
    bool Value = false;
    if (boolean(*ValueNode, Value))
      return true;
    if (Value)
      Function.attributes().set(Attribute);
    else
      Function.attributes().clear(Attribute);
    if (Key == std::string_view("hasFakeUses"))
      DocumentInfo.HasFakeUses = Value;
  }

  return false;
}

bool MIRDocumentParser::recomputeProperties(const yaml::Node &Root, MachineFunction &Function,
                                            const MIRFunctionDocumentInfo &DocumentInfo) {
  bool HasPHI = false;
  bool HasFakeUse = false;
  bool IsSSA = true;
  bool HasTiedOperands = false;
  bool AllTiedOperandsRewritten = true;
  bool HasFrameIndex = false;
  bool HasExecutablePseudo = false;
  std::unordered_set<Register> Definitions;
  for (const MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
       Block = Block->next()) {
    for (const MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction->opcode());
      if (Descriptor == nullptr)
        return fail(Root.range(), "instruction has no target descriptor");
      HasPHI |= Descriptor->Flags.has(InstructionFlag::Phi);
      HasFakeUse |= Descriptor->Flags.has(InstructionFlag::FakeUse);
      HasExecutablePseudo |= Descriptor->Flags.has(InstructionFlag::Pseudo) &&
                             !Descriptor->Flags.has(InstructionFlag::Meta) &&
                             !Descriptor->Flags.has(InstructionFlag::TerminalEmission);
      const auto Operands = Instruction->operands();
      for (std::size_t Index = 0; Index < Operands.size(); ++Index) {
        const MachineOperand &Operand = Operands[Index];
        HasFrameIndex |= Operand.getIf<FrameIndexOperand>() != nullptr;
        const auto Reg = Operand.registerValue();
        if (Reg && Reg->isVirtual() && Operand.isDef() && !Definitions.insert(*Reg).second) {
          IsSSA = false;
        }
        if (!Operand.tiedOperandIndex() || !Operand.isUse())
          continue;
        HasTiedOperands = true;
        const MachineOperand *Definition = Instruction->operandAt(*Operand.tiedOperandIndex());
        if (Definition == nullptr || Definition->registerValue() != Reg)
          AllTiedOperandsRewritten = false;
      }
      for (const MachineMemOperand *Memory : Instruction->memOperands())
        HasFrameIndex |= Memory->description().Base.frameIndex().has_value();
    }
  }
  const bool NoPHIs = !HasPHI;
  const bool NoVRegs =
      std::ranges::none_of(Function.registerInfo().virtualRegisters(),
                           [](const VirtualRegisterInfo &Info) { return !Info.IsTombstone; });

  const auto ApplyComputed = [this, &Root, &Function](std::optional<bool> Explicit, bool Computed,
                                                      MachineFunctionProperty Property,
                                                      std::string_view Conflict) -> bool {
    if (Explicit && *Explicit && !Computed) {
      return fail(Root.range(), std::string(Function.name()) + std::string(Conflict));
    }
    if (Explicit.value_or(Computed))
      Function.properties().set(Property);
    else
      Function.properties().clear(Property);
    return false;
  };
  if (ApplyComputed(DocumentInfo.NoPHIs, NoPHIs, MachineFunctionProperty::NoPHIs,
                    " has explicit property NoPhi, but contains at least one PHI") ||
      ApplyComputed(DocumentInfo.IsSSA, IsSSA, MachineFunctionProperty::IsSSA,
                    " has explicit property IsSSA, but is not valid SSA") ||
      ApplyComputed(DocumentInfo.NoVRegs, NoVRegs, MachineFunctionProperty::NoVRegs,
                    " has explicit property NoVRegs, but contains virtual registers")) {
    return true;
  }

  if (DocumentInfo.HasFakeUses && !*DocumentInfo.HasFakeUses && HasFakeUse) {
    return fail(Root.range(), std::string(Function.name()) +
                                  " has explicit property hasFakeUses=false, but contains fake uses");
  }
  if (DocumentInfo.HasFakeUses.value_or(HasFakeUse))
    Function.attributes().set(MachineFunctionAttribute::HasFakeUses);
  else
    Function.attributes().clear(MachineFunctionAttribute::HasFakeUses);

  const bool Selected = Function.properties().has(MachineFunctionProperty::Selected);
  const bool StructurallyPostSSA =
      Selected && NoPHIs && !Function.properties().has(MachineFunctionProperty::IsSSA);
  if (AllTiedOperandsRewritten && (HasTiedOperands || StructurallyPostSSA))
    Function.properties().set(MachineFunctionProperty::TiedOpsRewritten);
  else
    Function.properties().clear(MachineFunctionProperty::TiedOpsRewritten);

  if (HasFrameIndex)
    Function.properties().clear(MachineFunctionProperty::NoFrameIndices);
  else
    Function.properties().set(MachineFunctionProperty::NoFrameIndices);

  const MachineFrameState &FrameState = Function.frameInfo().state();
  const bool StructurallyFinalizedFrame = Selected && NoVRegs && !HasFrameIndex &&
                                          FrameState.IsCalleeSavedInfoValid &&
                                          FrameState.MaxAlignment.has_value();
  if (StructurallyFinalizedFrame) {
    if (!Function.frameInfo().isFrozen()) {
      auto Frozen = Function.frameInfo().freeze(FrameState.StackSize, *FrameState.MaxAlignment);
      if (!Frozen)
        return importError(Root.range(), Frozen.error(), "mir-frame-finalization");
    }
    Function.properties().set(MachineFunctionProperty::FrameFinalized);
  } else {
    Function.properties().clear(MachineFunctionProperty::FrameFinalized);
  }

  if (StructurallyFinalizedFrame && !HasExecutablePseudo)
    Function.properties().set(MachineFunctionProperty::NoExecutablePseudos);
  else
    Function.properties().clear(MachineFunctionProperty::NoExecutablePseudos);
  return false;
}

bool MIRDocumentParser::parseRegisters(const yaml::Node &Root, MachineFunction &Function,
                                       MIParserContext &Context) {
  const yaml::Node *Registers = mappingValue(Root, "registers");
  if (Registers != nullptr) {
    if (requireKind(*Registers, yaml::NodeKind::Sequence, "registers"))
      return true;
    constexpr std::array RegisterKeys{
        std::string_view{"id"},
        std::string_view{"class"},
        std::string_view{"preferred-register"},
        std::string_view{"flags"},
    };
    std::unordered_set<std::uint32_t> IDs;
    for (const yaml::NodeID ElementID : Registers->sequence()) {
      const yaml::Node *Element = node(ElementID);
      if (Element == nullptr || requireKind(*Element, yaml::NodeKind::Mapping, "register entry") ||
          validateKeys(*Element, RegisterKeys, "register")) {
        return true;
      }
      const yaml::Node *IDNode = mappingValue(*Element, "id");
      const yaml::Node *ClassNode = mappingValue(*Element, "class");
      if (IDNode == nullptr || ClassNode == nullptr)
        return fail(Element->range(), "register entry requires 'id' and 'class'");
      std::uint64_t WideID = 0;
      std::string_view ClassName;
      if (unsignedValue(*IDNode, WideID, "a 32-bit virtual-register ID") ||
          WideID > std::numeric_limits<std::uint32_t>::max() ||
          scalar(*ClassNode, ClassName, "register class or bank")) {
        return true;
      }
      const auto TextID = static_cast<std::uint32_t>(WideID);
      if (!IDs.insert(TextID).second)
        return fail(IDNode->range(), "duplicate virtual register ID");
      auto Created = Function.registerInfo().createVirtualRegister();
      if (!Created)
        return importError(IDNode->range(), Created.error());
      Context.VirtualRegisters.push_back({TextID, *Created});

      if (ClassName != "_") {
        if (const RegisterClassDesc *Class = Target.registers().registerClass(ClassName)) {
          auto SetResult = Function.registerInfo().setRegisterClass(*Created, Class->ID);
          if (!SetResult)
            return importError(ClassNode->range(), SetResult.error());
          SetResult = Function.registerInfo().setRegisterBank(*Created, Class->DefaultBank);
          if (!SetResult)
            return importError(ClassNode->range(), SetResult.error());
        } else if (const RegisterBankDesc *Bank = Target.registers().registerBank(ClassName)) {
          auto SetResult = Function.registerInfo().setRegisterBank(*Created, Bank->ID);
          if (!SetResult)
            return importError(ClassNode->range(), SetResult.error());
        } else {
          return fail(ClassNode->range(),
                      "unknown register class or bank '" + std::string(ClassName) + "'");
        }
      }

      if (const yaml::Node *Preferred = mappingValue(*Element, "preferred-register")) {
        std::string_view PreferredName;
        PhysicalRegister Reg;
        if (scalar(*Preferred, PreferredName, "physical register"))
          return true;
        if (!trim(PreferredName).empty()) {
          if (physicalRegister(PreferredName, Preferred->range(), Reg))
            return true;
          auto AddResult = Function.registerInfo().addAllocationHint(*Created, Reg);
          if (!AddResult)
            return importError(Preferred->range(), AddResult.error());
        }
      }
      if (const yaml::Node *Flags = mappingValue(*Element, "flags")) {
        if (requireKind(*Flags, yaml::NodeKind::Sequence, "virtual-register flags"))
          return true;
        std::uint8_t Mask = 0;
        for (const yaml::NodeID FlagID : Flags->sequence()) {
          const yaml::Node *FlagNode = node(FlagID);
          std::string_view Name;
          if (FlagNode == nullptr || scalar(*FlagNode, Name, "virtual-register flag"))
            return true;
          const auto Flag = Target.mirText().virtualRegisterFlag(Name);
          if (!Flag)
            return fail(FlagNode->range(),
                        "use of undefined register flag '" + std::string(Name) + "'");
          if ((Mask & *Flag) != 0)
            return fail(FlagNode->range(), "duplicate virtual-register flag");
          Mask = static_cast<std::uint8_t>(Mask | *Flag);
        }
        auto SetResult = Function.registerInfo().setTargetRegisterFlags(*Created, Mask);
        if (!SetResult)
          return importError(Flags->range(), SetResult.error());
      }
    }
  }

  const yaml::Node *LiveIns = mappingValue(Root, "liveins");
  if (LiveIns != nullptr) {
    if (requireKind(*LiveIns, yaml::NodeKind::Sequence, "liveins"))
      return true;
    constexpr std::array LiveInKeys{
        std::string_view{"reg"},
        std::string_view{"virtual-reg"},
    };
    for (const yaml::NodeID ElementID : LiveIns->sequence()) {
      const yaml::Node *Element = node(ElementID);
      if (Element == nullptr || requireKind(*Element, yaml::NodeKind::Mapping, "live-in entry") ||
          validateKeys(*Element, LiveInKeys, "live-in")) {
        return true;
      }
      const yaml::Node *RegNode = mappingValue(*Element, "reg");
      if (RegNode == nullptr)
        return fail(Element->range(), "live-in entry requires 'reg'");
      PhysicalRegister Reg;
      if (physicalRegister(*RegNode, Reg))
        return true;
      std::optional<Register> Initial;
      if (const yaml::Node *VirtualNode = mappingValue(*Element, "virtual-reg")) {
        std::string_view Text;
        if (scalar(*VirtualNode, Text, "virtual register"))
          return true;
        if (!trim(Text).empty()) {
          Register Virtual;
          if (virtualRegister(Text, VirtualNode->range(), Context, Virtual))
            return true;
          Initial = Virtual;
        }
      }
      auto AddResult = Function.registerInfo().addLiveIn(Reg, Initial);
      if (!AddResult)
        return importError(Element->range(), AddResult.error());
    }
  }

  if (const yaml::Node *CalleeSaved = mappingValue(Root, "calleeSavedRegisters")) {
    if (requireKind(*CalleeSaved, yaml::NodeKind::Sequence, "callee-saved registers"))
      return true;
    std::vector<PhysicalRegister> RegistersValue;
    for (const yaml::NodeID RegID : CalleeSaved->sequence()) {
      const yaml::Node *RegNode = node(RegID);
      PhysicalRegister Reg;
      if (RegNode == nullptr || physicalRegister(*RegNode, Reg))
        return true;
      RegistersValue.push_back(Reg);
    }
    auto SetResult = Function.records().setCalleeSavedRegisters(std::move(RegistersValue));
    if (!SetResult)
      return importError(CalleeSaved->range(), SetResult.error());
  }
  return false;
}

TextRange MIRDocumentParser::scalarDiagnosticRange(const yaml::Node &Scalar,
                                                   const TextRange &ScalarRange) const noexcept {
  if (Scalar.scalarStyle() != yaml::ScalarStyle::Literal &&
      Scalar.scalarStyle() != yaml::ScalarStyle::Folded) {
    std::size_t ContentOffset = Scalar.range().Begin.Offset;
    if (Scalar.scalarStyle() == yaml::ScalarStyle::SingleQuoted ||
        Scalar.scalarStyle() == yaml::ScalarStyle::DoubleQuoted) {
      ++ContentOffset;
    }
    return Result.Source.range(ContentOffset + ScalarRange.Begin.Offset,
                               ContentOffset + ScalarRange.End.Offset);
  }

  const std::size_t FirstBodyLine = Scalar.range().Begin.Line + 1U;
  const std::size_t LastBodyLine = Scalar.range().End.Line;
  if (FirstBodyLine > LastBodyLine || FirstBodyLine == 0)
    return Scalar.range();

  std::size_t Indent = 0;
  bool FoundContent = false;
  for (std::size_t Line = FirstBodyLine; Line <= LastBodyLine; ++Line) {
    const std::string_view Text = Result.Source.line(Line);
    std::size_t CurrentIndent = 0;
    while (CurrentIndent < Text.size() && Text[CurrentIndent] == ' ')
      ++CurrentIndent;
    if (CurrentIndent == Text.size())
      continue;
    Indent = CurrentIndent;
    FoundContent = true;
    break;
  }
  if (!FoundContent)
    return Scalar.range();

  const TextPosition Fallback = Scalar.range().Begin;
  const auto MapPosition = [this, FirstBodyLine, LastBodyLine, Indent,
                            Fallback](TextPosition Position) {
    const std::size_t SourceLine = FirstBodyLine + Position.Line - 1U;
    if (Position.Line == 0 || SourceLine > LastBodyLine || SourceLine > Result.Source.lineCount()) {
      return Fallback;
    }
    const std::string_view SourceLineText = Result.Source.line(SourceLine);
    const std::size_t BodyColumn = Position.Column == 0 ? 0 : Position.Column - 1U;
    const std::size_t SourceColumn = std::min(Indent + BodyColumn, SourceLineText.size());
    const std::size_t Offset = Result.Source.lineOffsets()[SourceLine - 1U] + SourceColumn;
    return Result.Source.positionAt(Offset);
  };
  return {.Begin = MapPosition(ScalarRange.Begin), .End = MapPosition(ScalarRange.End)};
}

bool MIRDocumentParser::parseBody(const yaml::Node &Root, MachineFunction &Function,
                                  MIParserContext &Context) {
  const yaml::Node *Body = mappingValue(Root, "body");
  std::string BodyText;
  TextRange BodyRange = Root.range();
  if (Body != nullptr) {
    std::string_view Text;
    if (scalar(*Body, Text, "machine function body"))
      return true;
    BodyText = Text;
    BodyRange = Body->range();
  }

  if (Options.DefaultPointerSizeInBits != 0)
    Context.DefaultPointerSizeInBits = Options.DefaultPointerSizeInBits;
  if (!effectiveDataLayout().empty()) {
    const LLVMConstantSyntaxOptions PointerOptions = pointerLayoutOptions();
    Context.PointerSizeInBits = [PointerOptions](std::uint32_t AddressSpace) {
      return llvmPointerSizeInBits(AddressSpace, PointerOptions);
    };
  }
  Context.SyncScopes = ActiveDocument->SyncScopes;
  Context.InternSyncScope = [this](std::string_view Name) -> Expected<SyncScopeID> {
    if (Name.empty())
      return makeUnexpected(ErrorCode::ParseError, "synchronization scope name cannot be empty");
    const auto Existing =
        std::ranges::find_if(ActiveDocument->SyncScopes,
                             [Name](const MISyncScopeSlot &Slot) { return Slot.Name == Name; });
    if (Existing != ActiveDocument->SyncScopes.end())
      return Existing->ID;
    if (ActiveDocument->SyncScopes.size() >=
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
      return makeUnexpected(ErrorCode::Unsupported,
                            "MIR document contains too many synchronization scopes");
    }
    const auto ID =
        SyncScopeID::fromValue(static_cast<std::uint32_t>(ActiveDocument->SyncScopes.size() + 1U));
    ActiveDocument->SyncScopes.push_back({.Name = std::string(Name), .ID = ID});
    return ID;
  };
  MIParserOptions BodyOptions = Options.MachineInstructions;
  BodyOptions.VerifyAfterParse = false;
  MIParser Parser(Function, Target,
                  std::string(Result.Source.name()) + ":" + std::string(Function.name()),
                  std::move(BodyText), Context, BodyOptions);
  auto Parsed = Parser.parse();
  if (!Parsed) {
    if (!Parser.diagnostics().empty()) {
      const TextRange Range = Body == nullptr
                                  ? BodyRange
                                  : scalarDiagnosticRange(*Body, Parser.diagnostics().front().Range);
      return fail(Range, Parser.diagnostics().front().Message, "mir-body");
    }
    return importError(BodyRange, Parsed.error(), "mir-body");
  }
  return false;
}

bool MIRDocumentParser::convertTargetNode(const yaml::Node &Source, MIRTargetNode &Destination) {
  switch (Source.kind()) {
  case yaml::NodeKind::Null:
    Destination = MIRTargetNode::null();
    return false;
  case yaml::NodeKind::Scalar:
    Destination = MIRTargetNode::scalar(std::string(Source.scalar()));
    return false;
  case yaml::NodeKind::Sequence: {
    std::vector<MIRTargetNode> Values;
    Values.reserve(Source.sequence().size());
    for (const yaml::NodeID ElementID : Source.sequence()) {
      const yaml::Node *Element = node(ElementID);
      if (Element == nullptr)
        return fail(Source.range(), "target MIR sequence contains an invalid node");
      MIRTargetNode Value;
      if (convertTargetNode(*Element, Value))
        return true;
      Values.push_back(std::move(Value));
    }
    Destination = MIRTargetNode::sequence(std::move(Values));
    return false;
  }
  case yaml::NodeKind::Mapping: {
    std::vector<MIRTargetMappingEntry> Entries;
    Entries.reserve(Source.mapping().size());
    for (const yaml::MappingEntry &SourceEntry : Source.mapping()) {
      const yaml::Node *ValueNode = node(SourceEntry.Value);
      if (ValueNode == nullptr)
        return fail(SourceEntry.KeyRange, "target MIR mapping contains an invalid node");
      MIRTargetNode Value;
      if (convertTargetNode(*ValueNode, Value))
        return true;
      Entries.push_back({SourceEntry.Key, std::move(Value)});
    }
    Destination = MIRTargetNode::mapping(std::move(Entries));
    return false;
  }
  }
  return fail(Source.range(), "target MIR node has an unknown kind");
}

bool MIRDocumentParser::parseTargetState(const yaml::Node &Root, MachineFunction &Function) {
  const yaml::Node *StateNode = mappingValue(Root, "machineFunctionInfo");
  if (StateNode == nullptr)
    return false;
  MIRTargetNode TargetNode;
  if (convertTargetNode(*StateNode, TargetNode))
    return true;
  auto State = Target.mirText().parseFunctionState(TargetNode);
  if (!State)
    return importError(StateNode->range(), State.error(), "mir-target-state");
  auto SetResult = Function.setTargetState(std::move(*State));
  if (!SetResult)
    return importError(StateNode->range(), SetResult.error(), "mir-target-state");
  return false;
}

bool MIRDocumentParser::validateDocumentReferences(const MachineFunction &Function) {
  auto Verification = verifyMIRDocumentReferences(*ActiveDocument, Function, true);
  if (Verification)
    return false;
  return importError(Result.Source.range(0, 0), Verification.error());
}

} // namespace rcc::mir
