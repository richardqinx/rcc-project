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

bool MIRDocumentParser::parseFunctionRecords(const yaml::Node &Root, MachineFunction &Function) {
  auto ParseLocation = [this, &Function](const yaml::Node &Entry,
                                         MachineInstr *&Instruction) -> bool {
    const yaml::Node *BlockNode = mappingValue(Entry, "bb");
    const yaml::Node *OffsetNode = mappingValue(Entry, "offset");
    if (BlockNode == nullptr || OffsetNode == nullptr)
      return fail(Entry.range(), "instruction location requires 'bb' and 'offset'");
    std::uint64_t Block = 0;
    std::uint64_t Offset = 0;
    if (unsignedValue(*BlockNode, Block, "a 32-bit basic block number") ||
        unsignedValue(*OffsetNode, Offset, "a 32-bit instruction offset") ||
        Block > std::numeric_limits<std::uint32_t>::max() ||
        Offset > std::numeric_limits<std::uint32_t>::max()) {
      return true;
    }
    Instruction = instructionAt(Function, static_cast<std::uint32_t>(Block),
                                static_cast<std::uint32_t>(Offset));
    if (Instruction == nullptr)
      return fail(Entry.range(), "instruction location is outside the machine function body");
    return false;
  };

  if (const yaml::Node *CallSites = mappingValue(Root, "callSites")) {
    if (requireKind(*CallSites, yaml::NodeKind::Sequence, "call-site records"))
      return true;
    constexpr std::array CallSiteKeys{
        std::string_view{"bb"},
        std::string_view{"offset"},
        std::string_view{"fwdArgRegs"},
        std::string_view{"calleeTypeIds"},
    };
    constexpr std::array ArgumentKeys{
        std::string_view{"arg"},
        std::string_view{"reg"},
    };
    for (const yaml::NodeID CallSiteID : CallSites->sequence()) {
      const yaml::Node *CallSiteNode = node(CallSiteID);
      if (CallSiteNode == nullptr ||
          requireKind(*CallSiteNode, yaml::NodeKind::Mapping, "call-site record") ||
          validateKeys(*CallSiteNode, CallSiteKeys, "call-site record")) {
        return true;
      }
      MachineInstr *Call = nullptr;
      if (ParseLocation(*CallSiteNode, Call))
        return true;
      if (Call == nullptr)
        return fail(CallSiteNode->range(),
                    "instruction location is outside the machine function body");
      MachineCallSiteInfo Info{
          .CallInstruction = Call->stableID(),
          .ForwardedArguments = {},
          .CalleeTypeIDs = {},
      };
      if (const yaml::Node *Arguments = mappingValue(*CallSiteNode, "fwdArgRegs")) {
        if (requireKind(*Arguments, yaml::NodeKind::Sequence, "forwarded call arguments")) {
          return true;
        }
        for (const yaml::NodeID ArgumentID : Arguments->sequence()) {
          const yaml::Node *Argument = node(ArgumentID);
          if (Argument == nullptr ||
              requireKind(*Argument, yaml::NodeKind::Mapping, "forwarded call argument") ||
              validateKeys(*Argument, ArgumentKeys, "forwarded call argument")) {
            return true;
          }
          const yaml::Node *NumberNode = mappingValue(*Argument, "arg");
          const yaml::Node *RegNode = mappingValue(*Argument, "reg");
          if (NumberNode == nullptr || RegNode == nullptr)
            return fail(Argument->range(), "forwarded call argument requires 'arg' and 'reg'");
          std::uint16_t Number = 0;
          std::uint64_t WideNumber = 0;
          PhysicalRegister Reg;
          if (unsignedValue(*NumberNode, WideNumber, "a 16-bit argument number") ||
              WideNumber > std::numeric_limits<std::uint16_t>::max() ||
              physicalRegister(*RegNode, Reg)) {
            return true;
          }
          Number = static_cast<std::uint16_t>(WideNumber);
          Info.ForwardedArguments.push_back({Reg, Number});
        }
      }
      if (const yaml::Node *TypeIDs = mappingValue(*CallSiteNode, "calleeTypeIds")) {
        if (requireKind(*TypeIDs, yaml::NodeKind::Sequence, "callee type IDs"))
          return true;
        for (const yaml::NodeID TypeID : TypeIDs->sequence()) {
          const yaml::Node *TypeNode = node(TypeID);
          std::uint64_t Value = 0;
          if (TypeNode == nullptr ||
              unsignedValue(*TypeNode, Value, "an unsigned callee type ID")) {
            return true;
          }
          Info.CalleeTypeIDs.push_back(Value);
        }
      }
      auto AddResult = Function.records().addCallSite(std::move(Info));
      if (!AddResult)
        return importError(CallSiteNode->range(), AddResult.error());
    }
  }

  if (const yaml::Node *CalledGlobals = mappingValue(Root, "calledGlobals")) {
    if (requireKind(*CalledGlobals, yaml::NodeKind::Sequence, "called-global records"))
      return true;
    constexpr std::array Keys{
        std::string_view{"bb"},
        std::string_view{"offset"},
        std::string_view{"callee"},
        std::string_view{"flags"},
    };
    for (const yaml::NodeID CalledID : CalledGlobals->sequence()) {
      const yaml::Node *CalledNode = node(CalledID);
      if (CalledNode == nullptr ||
          requireKind(*CalledNode, yaml::NodeKind::Mapping, "called-global record") ||
          validateKeys(*CalledNode, Keys, "called-global record")) {
        return true;
      }
      MachineInstr *Call = nullptr;
      if (ParseLocation(*CalledNode, Call))
        return true;
      if (Call == nullptr)
        return fail(CalledNode->range(),
                    "instruction location is outside the machine function body");
      const yaml::Node *CalleeNode = mappingValue(*CalledNode, "callee");
      const yaml::Node *FlagsNode = mappingValue(*CalledNode, "flags");
      if (CalleeNode == nullptr || FlagsNode == nullptr)
        return fail(CalledNode->range(), "called-global record requires 'callee' and 'flags'");
      std::string_view Callee;
      std::uint64_t WideFlags = 0;
      if (scalar(*CalleeNode, Callee, "callee name") ||
          unsignedValue(*FlagsNode, WideFlags, "32-bit called-global flags") ||
          WideFlags > std::numeric_limits<std::uint32_t>::max()) {
        return true;
      }
      if (ActiveDocument->EmbeddedIR && !ActiveDocument->EmbeddedIR->hasGlobalValue(Callee)) {
        return fail(CalleeNode->range(), "called global references an undefined embedded global");
      }
      auto AddResult =
          Function.records().addCalledGlobal({.CallInstruction = Call->stableID(),
                                              .Callee = std::string(Callee),
                                              .Flags = static_cast<std::uint32_t>(WideFlags)});
      if (!AddResult)
        return importError(CalledNode->range(), AddResult.error());
    }
  }

  if (const yaml::Node *Substitutions = mappingValue(Root, "debugValueSubstitutions")) {
    if (requireKind(*Substitutions, yaml::NodeKind::Sequence, "debug value substitutions")) {
      return true;
    }
    constexpr std::array Keys{
        std::string_view{"srcinst"}, std::string_view{"srcop"},  std::string_view{"dstinst"},
        std::string_view{"dstop"},   std::string_view{"subreg"},
    };
    for (const yaml::NodeID SubstitutionID : Substitutions->sequence()) {
      const yaml::Node *Substitution = node(SubstitutionID);
      if (Substitution == nullptr ||
          requireKind(*Substitution, yaml::NodeKind::Mapping, "debug value substitution") ||
          validateKeys(*Substitution, Keys, "debug value substitution")) {
        return true;
      }
      std::array<std::uint32_t, 5> Values{};
      for (std::size_t Index = 0; Index < Keys.size(); ++Index) {
        const yaml::Node *ValueNode = mappingValue(*Substitution, Keys[Index]);
        if (ValueNode == nullptr)
          return fail(Substitution->range(), "debug value substitution requires every fixed field");
        std::uint64_t Wide = 0;
        if (unsignedValue(*ValueNode, Wide, "a 32-bit debug substitution value") ||
            Wide > std::numeric_limits<std::uint32_t>::max()) {
          return true;
        }
        Values[Index] = static_cast<std::uint32_t>(Wide);
      }
      auto AddResult = Function.records().addDebugValueSubstitution(
          {.Source = {Values[0], Values[1]},
           .Destination = {Values[2], Values[3]},
           .Subregister = SubRegIndex::fromValue(Values[4])});
      if (!AddResult)
        return importError(Substitution->range(), AddResult.error());
    }
  }

  if (const yaml::Node *PrefetchTargets = mappingValue(Root, "prefetch-targets")) {
    if (requireKind(*PrefetchTargets, yaml::NodeKind::Sequence, "prefetch targets"))
      return true;
    for (const yaml::NodeID TargetID : PrefetchTargets->sequence()) {
      const yaml::Node *TargetNode = node(TargetID);
      std::string_view Text;
      if (TargetNode == nullptr || scalar(*TargetNode, Text, "prefetch target"))
        return true;

      std::size_t ContentOffset = TargetNode->range().Begin.Offset;
      if (TargetNode->scalarStyle() == yaml::ScalarStyle::SingleQuoted ||
          TargetNode->scalarStyle() == yaml::ScalarStyle::DoubleQuoted) {
        ++ContentOffset;
      }
      const auto ErrorRange = [this, ContentOffset](std::size_t Begin,
                                                     std::size_t End = 1U) {
        return Result.Source.range(ContentOffset + Begin, ContentOffset + Begin + End);
      };
      const auto SkipSpace = [&Text](std::size_t &Cursor) {
        while (Cursor < Text.size() && (Text[Cursor] == ' ' || Text[Cursor] == '\t'))
          ++Cursor;
      };
      const auto ParseNumber = [&Text, &SkipSpace](std::size_t &Cursor,
                                                   std::uint32_t &Value) {
        SkipSpace(Cursor);
        const std::size_t Begin = Cursor;
        while (Cursor < Text.size() && Text[Cursor] >= '0' && Text[Cursor] <= '9')
          ++Cursor;
        return Begin != Cursor && parseUnsigned32(Text.substr(Begin, Cursor - Begin), Value);
      };

      std::size_t Cursor = 0;
      SkipSpace(Cursor);
      if (!Text.substr(Cursor).starts_with("bb_id") ||
          (Cursor + std::string_view("bb_id").size() < Text.size() &&
           Text[Cursor + std::string_view("bb_id").size()] != ' ' &&
           Text[Cursor + std::string_view("bb_id").size()] != '\t')) {
        return fail(ErrorRange(Cursor), "expected 'bb_id'");
      }
      Cursor += std::string_view("bb_id").size();

      std::array<std::uint32_t, 3> Values{};
      if (!ParseNumber(Cursor, Values[0]))
        return fail(ErrorRange(0), "expected unsigned integer");

      SkipSpace(Cursor);
      if (Cursor < Text.size() && (Text[Cursor] == ',' || Text[Cursor] == '.')) {
        ++Cursor;
        if (!ParseNumber(Cursor, Values[1]))
          return fail(ErrorRange(0), "expected unsigned integer");
      } else if (Cursor < Text.size() && Text[Cursor] >= '0' && Text[Cursor] <= '9') {
        if (!ParseNumber(Cursor, Values[1]))
          return fail(ErrorRange(0), "expected unsigned integer");
      }

      SkipSpace(Cursor);
      if (Cursor >= Text.size() || Text[Cursor] != ',')
        return fail(ErrorRange(Cursor), "expected ','");
      ++Cursor;
      if (!ParseNumber(Cursor, Values[2]))
        return fail(ErrorRange(0), "expected unsigned integer");
      SkipSpace(Cursor);
      if (Cursor != Text.size())
        return fail(ErrorRange(0), "unexpected content after prefetch target");

      auto AddResult = Function.records().addPrefetchTarget(
          {.Block = {Values[0], Values[1]}, .CallsiteIndex = Values[2]});
      if (!AddResult)
        return importError(TargetNode->range(), AddResult.error());
    }
  }

  if (const yaml::Node *EntryValues = mappingValue(Root, "entry_values")) {
    if (requireKind(*EntryValues, yaml::NodeKind::Sequence, "entry values"))
      return true;
    constexpr std::array Keys{
        std::string_view{"entry-value-register"},
        std::string_view{"debug-info-variable"},
        std::string_view{"debug-info-expression"},
        std::string_view{"debug-info-location"},
    };
    for (const yaml::NodeID EntryID : EntryValues->sequence()) {
      const yaml::Node *Entry = node(EntryID);
      if (Entry == nullptr || requireKind(*Entry, yaml::NodeKind::Mapping, "entry value") ||
          validateKeys(*Entry, Keys, "entry value")) {
        return true;
      }
      const yaml::Node *RegNode = mappingValue(*Entry, "entry-value-register");
      const yaml::Node *VariableNode = mappingValue(*Entry, "debug-info-variable");
      const yaml::Node *ExpressionNode = mappingValue(*Entry, "debug-info-expression");
      const yaml::Node *LocationNode = mappingValue(*Entry, "debug-info-location");
      if (RegNode == nullptr || VariableNode == nullptr || ExpressionNode == nullptr ||
          LocationNode == nullptr) {
        return fail(Entry->range(), "entry value requires all four fixed fields");
      }
      PhysicalRegister Reg;
      if (physicalRegister(*RegNode, Reg))
        return true;
      MachineVariableDebugInfo DebugInfo;
      if (parseVariableDebugInfo(*Entry, Function, DebugInfo))
        return true;
      if (!DebugInfo.Variable || !DebugInfo.Expression || !DebugInfo.Location) {
        return fail(Entry->range(), "entry value debug metadata cannot be empty",
                    "mir-debug-metadata");
      }
      auto AddResult =
          Function.records().addEntryValue({.Reg = Reg, .DebugInfo = std::move(DebugInfo)});
      if (!AddResult)
        return importError(Entry->range(), AddResult.error());
    }
  }
  return false;
}

} // namespace rcc::mir
