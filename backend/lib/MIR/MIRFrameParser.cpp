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

bool MIRDocumentParser::parseStackObjects(const yaml::Node &Root, MachineFunction &Function,
                                          MIParserContext &Context) {
  auto ParseList = [this, &Function, &Context](const yaml::Node &List, bool Fixed) -> bool {
    if (requireKind(List, yaml::NodeKind::Sequence,
                    Fixed ? "fixed stack objects" : "stack objects")) {
      return true;
    }
    constexpr std::array FixedKeys{
        std::string_view{"id"},
        std::string_view{"type"},
        std::string_view{"offset"},
        std::string_view{"size"},
        std::string_view{"alignment"},
        std::string_view{"stack-id"},
        std::string_view{"isImmutable"},
        std::string_view{"isAliased"},
        std::string_view{"callee-saved-register"},
        std::string_view{"callee-saved-restored"},
        std::string_view{"debug-info-variable"},
        std::string_view{"debug-info-expression"},
        std::string_view{"debug-info-location"},
    };
    constexpr std::array StackKeys{
        std::string_view{"id"},
        std::string_view{"name"},
        std::string_view{"type"},
        std::string_view{"offset"},
        std::string_view{"size"},
        std::string_view{"alignment"},
        std::string_view{"stack-id"},
        std::string_view{"callee-saved-register"},
        std::string_view{"callee-saved-restored"},
        std::string_view{"local-offset"},
        std::string_view{"debug-info-variable"},
        std::string_view{"debug-info-expression"},
        std::string_view{"debug-info-location"},
    };
    std::unordered_set<std::uint32_t> IDs;
    for (const yaml::NodeID ElementID : List.sequence()) {
      const yaml::Node *Element = node(ElementID);
      if (Element == nullptr || requireKind(*Element, yaml::NodeKind::Mapping, "stack object") ||
          (Fixed ? validateKeys(*Element, FixedKeys, "fixed stack object")
                 : validateKeys(*Element, StackKeys, "stack object"))) {
        return true;
      }
      const yaml::Node *IDNode = mappingValue(*Element, "id");
      if (IDNode == nullptr)
        return fail(Element->range(), "stack object requires 'id'");
      std::uint64_t WideID = 0;
      if (unsignedValue(*IDNode, WideID, "a 32-bit stack object ID") ||
          WideID > std::numeric_limits<std::uint32_t>::max()) {
        return true;
      }
      const auto TextID = static_cast<std::uint32_t>(WideID);
      if (!IDs.insert(TextID).second)
        return fail(IDNode->range(), "duplicate stack object ID");

      StackObjectDesc Description;
      Description.Kind = Fixed ? StackObjectKind::FixedABI : StackObjectKind::Default;
      Description.Offset = 0;
      if (const yaml::Node *NameNode = mappingValue(*Element, "name")) {
        std::string_view Name;
        if (scalar(*NameNode, Name, "stack object name") || Name.empty())
          return fail(NameNode->range(), "stack object name cannot be empty");
        Description.AllocaName = std::string(Name);
      }
      if (const yaml::Node *TypeNode = mappingValue(*Element, "type")) {
        std::string_view Type;
        if (scalar(*TypeNode, Type, "stack object type"))
          return true;
        if (Type == "default")
          Description.Kind = Fixed ? StackObjectKind::FixedABI : StackObjectKind::Default;
        else if (Type == "spill-slot")
          Description.Kind = Fixed ? StackObjectKind::FixedSpillSlot : StackObjectKind::SpillSlot;
        else if (!Fixed && Type == "variable-sized")
          Description.Kind = StackObjectKind::VariableSized;
        else
          return fail(TypeNode->range(), "unknown stack object type '" + std::string(Type) + "'");
      }
      if (const yaml::Node *OffsetNode = mappingValue(*Element, "offset")) {
        std::int64_t Offset = 0;
        if (signedValue(*OffsetNode, Offset, "a signed stack object offset"))
          return true;
        Description.Offset = Offset;
      } else if (Fixed) {
        Description.Offset = 0;
      }
      if (Description.Kind != StackObjectKind::VariableSized) {
        const yaml::Node *SizeNode = mappingValue(*Element, "size");
        if (SizeNode == nullptr) {
          if (!Fixed)
            return fail(Element->range(), "fixed-size stack object requires 'size'");
          Description.Size = 0;
        } else {
          std::uint64_t Size = 0;
          if (unsignedValue(*SizeNode, Size, "an unsigned stack object size"))
            return true;
          Description.Size = Size;
        }
      }
      if (const yaml::Node *AlignmentNode = mappingValue(*Element, "alignment")) {
        if (alignment(*AlignmentNode, Description.Align, true))
          return true;
      }
      if (const yaml::Node *StackIDNode = mappingValue(*Element, "stack-id")) {
        std::string_view Name;
        if (scalar(*StackIDNode, Name, "stack ID"))
          return true;
        const auto Stack = parseStackIDName(Name);
        if (!Stack)
          return fail(StackIDNode->range(), "unknown stack ID '" + std::string(Name) + "'");
        Description.Stack = *Stack;
      }
      if (const yaml::Node *ImmutableNode = mappingValue(*Element, "isImmutable")) {
        if (boolean(*ImmutableNode, Description.IsImmutable))
          return true;
      }
      if (const yaml::Node *AliasedNode = mappingValue(*Element, "isAliased")) {
        if (boolean(*AliasedNode, Description.IsAliased))
          return true;
      }
      if (const yaml::Node *SavedNode = mappingValue(*Element, "callee-saved-register")) {
        PhysicalRegister Reg;
        if (physicalRegister(*SavedNode, Reg))
          return true;
        Description.CalleeSavedRegister = Reg;
      }
      if (const yaml::Node *RestoredNode = mappingValue(*Element, "callee-saved-restored")) {
        if (boolean(*RestoredNode, Description.CalleeSavedRestored))
          return true;
      }
      if (const yaml::Node *LocalNode = mappingValue(*Element, "local-offset")) {
        std::int64_t Offset = 0;
        if (signedValue(*LocalNode, Offset, "a signed local frame offset"))
          return true;
        Description.LocalOffset = Offset;
      }
      MachineVariableDebugInfo DebugInfo;
      if (parseVariableDebugInfo(*Element, Function, DebugInfo))
        return true;
      if (DebugInfo.Variable || DebugInfo.Expression || DebugInfo.Location)
        Description.DebugInfo = std::move(DebugInfo);

      auto Created = Function.frameInfo().createObject(std::move(Description));
      if (!Created)
        return importError(Element->range(), Created.error());
      MIFrameObjectSlot Slot{
          .TextID = TextID,
          .Index = *Created,
          .Name = std::nullopt,
      };
      if (const StackObjectDesc *CreatedDescription =
              Function.frameInfo().object(*Created)->description();
          CreatedDescription != nullptr && CreatedDescription->AllocaName) {
        Slot.Name = *CreatedDescription->AllocaName;
      }
      (Fixed ? Context.FixedStackObjects : Context.StackObjects).push_back(std::move(Slot));
    }
    return false;
  };

  if (const yaml::Node *Fixed = mappingValue(Root, "fixedStack")) {
    if (ParseList(*Fixed, true))
      return true;
  }
  if (const yaml::Node *Stack = mappingValue(Root, "stack")) {
    if (ParseList(*Stack, false))
      return true;
  }
  return false;
}

bool MIRDocumentParser::parseFrameReference(const yaml::Node &Node, const MIParserContext &Context,
                                            FrameIndex &Index) {
  std::string_view Text;
  if (scalar(Node, Text, "frame index"))
    return true;
  bool Fixed = false;
  std::optional<std::uint32_t> ID = parsePrefixedID(Text, "%stack.");
  if (!ID) {
    ID = parsePrefixedID(Text, "%fixed-stack.");
    Fixed = true;
  }
  if (!ID)
    return fail(Node.range(), "frame index must use '%stack.N' or '%fixed-stack.N'");
  const auto &Slots = Fixed ? Context.FixedStackObjects : Context.StackObjects;
  const auto Found = std::ranges::find_if(
      Slots, [ID](const MIFrameObjectSlot &Slot) { return Slot.TextID == *ID; });
  if (Found == Slots.end())
    return fail(Node.range(), "reference to undefined frame object");
  Index = Found->Index;
  return false;
}

MachineBasicBlock *MIRDocumentParser::blockAt(MachineFunction &Function,
                                              std::uint32_t Number) const noexcept {
  for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr; Block = Block->next()) {
    if (Block->denseNumber() == Number)
      return Block;
  }
  return nullptr;
}

MachineInstr *MIRDocumentParser::instructionAt(MachineFunction &Function, std::uint32_t BlockNumber,
                                               std::uint32_t Offset) const noexcept {
  MachineBasicBlock *Block = blockAt(Function, BlockNumber);
  if (Block == nullptr)
    return nullptr;
  MachineInstr *Instruction = Block->firstInstruction();
  for (std::uint32_t Index = 0; Instruction != nullptr && Index < Offset; ++Index)
    Instruction = Instruction->next();
  return Instruction;
}

bool MIRDocumentParser::parseBlockReference(std::string_view Text, const TextRange &Range,
                                            MachineFunction &Function, MachineBasicBlock *&Block) {
  const auto Number = parsePrefixedID(Text, "%bb.");
  if (!Number)
    return fail(Range, "basic block reference must use the '%bb.N' MIR spelling");
  Block = blockAt(Function, *Number);
  if (Block == nullptr)
    return fail(Range, "reference to undefined machine basic block");
  return false;
}

bool MIRDocumentParser::parseSaveRestorePoints(const yaml::Node &Node, MachineFunction &Function,
                                               std::vector<MachineFrameSaveRestorePoint> &Points) {
  if (requireKind(Node, yaml::NodeKind::Sequence, "frame save/restore points"))
    return true;
  constexpr std::array Keys{
      std::string_view{"point"},
      std::string_view{"registers"},
  };
  for (const yaml::NodeID PointID : Node.sequence()) {
    const yaml::Node *PointNode = node(PointID);
    if (PointNode == nullptr ||
        requireKind(*PointNode, yaml::NodeKind::Mapping, "frame save/restore point") ||
        validateKeys(*PointNode, Keys, "frame save/restore point")) {
      return true;
    }
    const yaml::Node *BlockNode = mappingValue(*PointNode, "point");
    if (BlockNode == nullptr)
      return fail(PointNode->range(), "frame save/restore point requires 'point'");
    std::string_view BlockText;
    MachineBasicBlock *Block = nullptr;
    if (scalar(*BlockNode, BlockText, "basic block reference") ||
        parseBlockReference(BlockText, BlockNode->range(), Function, Block)) {
      return true;
    }
    if (Block == nullptr)
      return fail(BlockNode->range(), "reference to undefined machine basic block");
    MachineFrameSaveRestorePoint Point{.Block = Block->stableID(), .Registers = {}};
    if (const yaml::Node *RegistersNode = mappingValue(*PointNode, "registers")) {
      if (requireKind(*RegistersNode, yaml::NodeKind::Sequence, "save/restore point registers")) {
        return true;
      }
      for (const yaml::NodeID RegID : RegistersNode->sequence()) {
        const yaml::Node *RegNode = node(RegID);
        PhysicalRegister Reg;
        if (RegNode == nullptr || physicalRegister(*RegNode, Reg))
          return true;
        Point.Registers.push_back(Reg);
      }
    }
    Points.push_back(std::move(Point));
  }
  return false;
}

bool MIRDocumentParser::parseFrameState(const yaml::Node &Root, MachineFunction &Function,
                                        const MIParserContext &Context) {
  const yaml::Node *Frame = mappingValue(Root, "frameInfo");
  if (Frame == nullptr)
    return false;
  if (requireKind(*Frame, yaml::NodeKind::Mapping, "frameInfo"))
    return true;
  constexpr std::array Keys{
      std::string_view{"isFrameAddressTaken"},
      std::string_view{"isReturnAddressTaken"},
      std::string_view{"hasStackMap"},
      std::string_view{"hasPatchPoint"},
      std::string_view{"stackSize"},
      std::string_view{"offsetAdjustment"},
      std::string_view{"maxAlignment"},
      std::string_view{"adjustsStack"},
      std::string_view{"hasCalls"},
      std::string_view{"framePointerPolicy"},
      std::string_view{"stackProtector"},
      std::string_view{"functionContext"},
      std::string_view{"maxCallFrameSize"},
      std::string_view{"cvBytesOfCalleeSavedRegisters"},
      std::string_view{"hasOpaqueSPAdjustment"},
      std::string_view{"hasVAStart"},
      std::string_view{"hasMustTailInVarArgFunc"},
      std::string_view{"hasTailCall"},
      std::string_view{"isCalleeSavedInfoValid"},
      std::string_view{"localFrameSize"},
      std::string_view{"savePoint"},
      std::string_view{"restorePoint"},
  };
  if (validateKeys(*Frame, Keys, "frameInfo"))
    return true;

  MachineFrameState State = Function.frameInfo().state();
  const std::array BoolBindings{
      std::pair{"isFrameAddressTaken", &State.IsFrameAddressTaken},
      std::pair{"isReturnAddressTaken", &State.IsReturnAddressTaken},
      std::pair{"hasStackMap", &State.HasStackMap},
      std::pair{"hasPatchPoint", &State.HasPatchPoint},
      std::pair{"adjustsStack", &State.AdjustsStack},
      std::pair{"hasCalls", &State.HasCalls},
      std::pair{"hasOpaqueSPAdjustment", &State.HasOpaqueSPAdjustment},
      std::pair{"hasVAStart", &State.HasVAStart},
      std::pair{"hasMustTailInVarArgFunc", &State.HasMustTailInVarArgFunc},
      std::pair{"hasTailCall", &State.HasTailCall},
      std::pair{"isCalleeSavedInfoValid", &State.IsCalleeSavedInfoValid},
  };
  for (const auto &[Key, Destination] : BoolBindings) {
    if (const yaml::Node *Value = mappingValue(*Frame, Key)) {
      if (boolean(*Value, *Destination))
        return true;
    }
  }

  if (const yaml::Node *Value = mappingValue(*Frame, "stackSize")) {
    if (unsignedValue(*Value, State.StackSize, "an unsigned stack size"))
      return true;
  }
  if (const yaml::Node *Value = mappingValue(*Frame, "offsetAdjustment")) {
    std::int64_t Wide = 0;
    if (signedValue(*Value, Wide, "a 32-bit signed offset adjustment") ||
        Wide < std::numeric_limits<std::int32_t>::min() ||
        Wide > std::numeric_limits<std::int32_t>::max()) {
      return fail(Value->range(), "offset adjustment is outside the 32-bit signed range");
    }
    State.OffsetAdjustment = static_cast<std::int32_t>(Wide);
  }
  if (const yaml::Node *Value = mappingValue(*Frame, "maxAlignment")) {
    std::uint64_t Bytes = 0;
    if (unsignedValue(*Value, Bytes, "an unsigned maximum alignment"))
      return true;
    if (Bytes == 0) {
      State.MaxAlignment.reset();
    } else {
      auto Parsed = Alignment::create(Bytes);
      if (!Parsed)
        return importError(Value->range(), Parsed.error());
      State.MaxAlignment = *Parsed;
    }
  }
  if (const yaml::Node *Value = mappingValue(*Frame, "framePointerPolicy")) {
    std::string_view Name;
    if (scalar(*Value, Name, "frame-pointer policy"))
      return true;
    const auto Policy = parseMachineFramePointerPolicy(Name);
    if (!Policy)
      return fail(Value->range(), "unknown frame-pointer policy '" + std::string(Name) + "'");
    State.FramePointerPolicy = *Policy;
  }
  for (const auto &[Key, Destination] : {std::pair{"stackProtector", &State.StackProtector},
                                         std::pair{"functionContext", &State.FunctionContext}}) {
    if (const yaml::Node *Value = mappingValue(*Frame, Key)) {
      FrameIndex Index;
      if (parseFrameReference(*Value, Context, Index))
        return true;
      *Destination = Index;
    }
  }
  if (const yaml::Node *Value = mappingValue(*Frame, "maxCallFrameSize")) {
    std::uint64_t Size = 0;
    if (unsignedValue(*Value, Size, "a 32-bit unsigned maximum call-frame size") ||
        Size > std::numeric_limits<std::uint32_t>::max()) {
      return fail(Value->range(), "maximum call-frame size is outside the 32-bit unsigned range");
    }
    State.MaxCallFrameSize = Size;
  }
  for (const auto &[Key, Destination] :
       {std::pair{"cvBytesOfCalleeSavedRegisters", &State.CVBytesOfCalleeSavedRegisters},
        std::pair{"localFrameSize", &State.LocalFrameSize}}) {
    if (const yaml::Node *Value = mappingValue(*Frame, Key)) {
      std::uint64_t Wide = 0;
      if (unsignedValue(*Value, Wide, "a 32-bit unsigned frame value") ||
          Wide > std::numeric_limits<std::uint32_t>::max()) {
        return fail(Value->range(), "frame value is outside the 32-bit unsigned range");
      }
      *Destination = static_cast<std::uint32_t>(Wide);
    }
  }
  if (const yaml::Node *Value = mappingValue(*Frame, "savePoint")) {
    if (parseSaveRestorePoints(*Value, Function, State.SavePoints))
      return true;
  }
  if (const yaml::Node *Value = mappingValue(*Frame, "restorePoint")) {
    if (parseSaveRestorePoints(*Value, Function, State.RestorePoints))
      return true;
  }
  auto StateResult = Function.frameInfo().setState(std::move(State));
  if (!StateResult)
    return importError(Frame->range(), StateResult.error());
  return false;
}

} // namespace rcc::mir
