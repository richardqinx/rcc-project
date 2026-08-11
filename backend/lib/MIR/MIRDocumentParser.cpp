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

bool MIRDocumentParser::fail(const TextRange &Range, std::string Message, std::string Category) {
  if (Result.Diagnostics.empty()) {
    Result.Diagnostics.push_back(TextDiagnostic{
        .Severity = DiagnosticSeverity::Error,
        .Category = std::move(Category),
        .Message = std::move(Message),
        .SourceName = std::string(Result.Source.name()),
        .Range = Range,
    });
  }
  return true;
}

bool MIRDocumentParser::importError(const TextRange &Range, const Error &Failure,
                                    std::string Category) {
  return fail(Range, std::string(Failure.message()), std::move(Category));
}

const yaml::Node *MIRDocumentParser::mappingValue(const yaml::Node &Mapping,
                                                  std::string_view Key) const noexcept {
  const auto Value = Mapping.mappingValue(Key);
  return Value ? node(*Value) : nullptr;
}

std::string_view MIRDocumentParser::effectiveDataLayout() const noexcept {
  if (!Options.DataLayout.empty())
    return Options.DataLayout;
  if (ActiveDocument != nullptr) {
    if (const EmbeddedIRReferenceIndex *Embedded = ActiveDocument->embeddedIR()) {
      if (const auto DataLayout = Embedded->dataLayout())
        return *DataLayout;
    }
  }
  return {};
}

LLVMConstantSyntaxOptions MIRDocumentParser::pointerLayoutOptions() const noexcept {
  return {
      .DataLayout = effectiveDataLayout(),
      .NamedTypes = {},
  };
}

bool MIRDocumentParser::requireKind(const yaml::Node &Node, yaml::NodeKind Kind,
                                    std::string_view Description) {
  if (Node.kind() == Kind)
    return false;
  return fail(Node.range(), std::string(Description) + " has the wrong YAML node kind");
}

bool MIRDocumentParser::validateKeys(const yaml::Node &Mapping,
                                     std::span<const std::string_view> Keys,
                                     std::string_view Description) {
  for (const yaml::MappingEntry &Entry : Mapping.mapping()) {
    if (std::ranges::find(Keys, Entry.Key) == Keys.end()) {
      return fail(Entry.KeyRange,
                  "unknown " + std::string(Description) + " key '" + Entry.Key + "'");
    }
  }
  return false;
}

bool MIRDocumentParser::scalar(const yaml::Node &Node, std::string_view &Value,
                               std::string_view Description) {
  if (requireKind(Node, yaml::NodeKind::Scalar, Description))
    return true;
  Value = Node.scalar();
  return false;
}

bool MIRDocumentParser::boolean(const yaml::Node &Node, bool &Value) {
  std::string_view Text;
  if (scalar(Node, Text, "boolean") || !parseBool(Text, Value))
    return fail(Node.range(), "expected 'true' or 'false'");
  return false;
}

bool MIRDocumentParser::unsignedValue(const yaml::Node &Node, std::uint64_t &Value,
                                      std::string_view Description) {
  std::string_view Text;
  if (scalar(Node, Text, Description) || !parseUnsigned(Text, Value))
    return fail(Node.range(), "expected " + std::string(Description));
  return false;
}

bool MIRDocumentParser::signedValue(const yaml::Node &Node, std::int64_t &Value,
                                    std::string_view Description) {
  std::string_view Text;
  if (scalar(Node, Text, Description) || !parseSigned(Text, Value))
    return fail(Node.range(), "expected " + std::string(Description));
  return false;
}

bool MIRDocumentParser::alignment(const yaml::Node &Node, Alignment &Value, bool AllowZero) {
  std::uint64_t Bytes = 0;
  if (unsignedValue(Node, Bytes, "an unsigned alignment"))
    return true;
  if (Bytes == 0 && AllowZero) {
    Value = Alignment{};
    return false;
  }
  auto Parsed = Alignment::create(Bytes);
  if (!Parsed)
    return importError(Node.range(), Parsed.error());
  Value = *Parsed;
  return false;
}

bool MIRDocumentParser::physicalRegister(std::string_view Text, const TextRange &Range,
                                         PhysicalRegister &Reg) {
  Text = trim(Text);
  if (!Text.starts_with('$') || Text.size() == 1)
    return fail(Range, "physical register must use the '$name' MIR spelling");
  const PhysicalRegisterDesc *Description = Target.registers().physicalRegister(Text.substr(1));
  if (Description == nullptr)
    return fail(Range, "unknown physical register '" + std::string(Text) + "'");
  Reg = Description->Reg;
  return false;
}

bool MIRDocumentParser::physicalRegister(const yaml::Node &Node, PhysicalRegister &Reg) {
  std::string_view Text;
  return scalar(Node, Text, "physical register") || physicalRegister(Text, Node.range(), Reg);
}

bool MIRDocumentParser::virtualRegister(std::string_view Text, const TextRange &Range,
                                        MachineFunction &Function, MIParserContext &Context,
                                        Register &Reg) {
  const auto ID = parsePrefixedID(Text, "%");
  if (!ID)
    return fail(Range, "virtual register must use the '%N' MIR spelling");
  auto Found =
      std::ranges::find_if(Context.VirtualRegisters,
                           [ID](const MIVirtualRegisterSlot &Slot) { return Slot.TextID == *ID; });
  if (Found == Context.VirtualRegisters.end()) {
    auto Created = Function.registerInfo().createVirtualRegister();
    if (!Created)
      return importError(Range, Created.error());
    Context.VirtualRegisters.push_back({*ID, *Created});
    Reg = *Created;
    return false;
  }
  Reg = Found->Reg;
  return false;
}

MIRDocumentParseResult MIRDocumentParser::run() {
  yaml::ParseResult YAMLResult = yaml::parse(std::string(Result.Source.name()),
                                             std::string(Result.Source.text()), Options.YAML);
  if (!YAMLResult.succeeded()) {
    const auto Diagnostics = YAMLResult.diagnostics();
    Result.Diagnostics.assign(Diagnostics.begin(), Diagnostics.end());
    return std::move(Result);
  }
  Stream = YAMLResult.stream();
  if (Stream == nullptr || Stream->documents().empty()) {
    static_cast<void>(fail(Result.Source.range(0, 0), "MIR stream contains no YAML documents"));
    return std::move(Result);
  }

  MIRDocument Staging;
  ActiveDocument = &Staging;
  const auto IsEmptyDocument = [this](std::size_t Index) {
    const yaml::Node *Root = node(Stream->documents()[Index].Root);
    return Root != nullptr && Root->kind() == yaml::NodeKind::Null &&
           Root->range().Begin.Offset == Root->range().End.Offset;
  };
  std::size_t FirstContent = 0;
  while (FirstContent < Stream->documents().size() && IsEmptyDocument(FirstContent))
    ++FirstContent;
  if (FirstContent == Stream->documents().size()) {
    static_cast<void>(fail(Stream->documents().front().Range,
                           "MIR stream must contain at least one machine function document"));
    return std::move(Result);
  }

  std::size_t FirstFunction = FirstContent;
  const yaml::Node *FirstRoot = node(Stream->documents()[FirstContent].Root);
  if (FirstRoot != nullptr && FirstRoot->kind() == yaml::NodeKind::Scalar) {
    EmbeddedIRIndexResult Embedded = indexEmbeddedIR(
        std::string(Result.Source.name()) + ":embedded-ir", std::string(FirstRoot->scalar()));
    if (!Embedded.succeeded()) {
      const auto Diagnostics = Embedded.diagnostics();
      if (Diagnostics.empty()) {
        static_cast<void>(
            fail(FirstRoot->range(), "embedded LLVM IR indexing failed", "mir-embedded-ir"));
      } else {
        const TextDiagnostic &Diagnostic = Diagnostics.front();
        const TextRange Range = scalarDiagnosticRange(*FirstRoot, Diagnostic.Range);
        static_cast<void>(fail(Range, Diagnostic.Message, Diagnostic.Category));
      }
      return std::move(Result);
    }
    Staging.EmbeddedIR = std::move(Embedded).takeIndex();
    FirstFunction = FirstContent + 1;
  }

  if (!effectiveDataLayout().empty()) {
    auto PointerSize = llvmPointerSizeInBits(0, pointerLayoutOptions());
    if (!PointerSize) {
      static_cast<void>(importError(Stream->documents()[FirstContent].Range, PointerSize.error(),
                                    "mir-data-layout"));
      return std::move(Result);
    }
  }

  bool SawFunction = false;
  for (std::size_t Index = FirstFunction; Index < Stream->documents().size(); ++Index) {
    if (IsEmptyDocument(Index))
      continue;
    const yaml::Node *Root = node(Stream->documents()[Index].Root);
    if (Root == nullptr || requireKind(*Root, yaml::NodeKind::Mapping, "machine function") ||
        parseFunction(*Root, Staging)) {
      return std::move(Result);
    }
    SawFunction = true;
  }
  if (!SawFunction) {
    static_cast<void>(fail(Stream->documents()[FirstContent].Range,
                           "MIR stream must contain at least one machine function document"));
    return std::move(Result);
  }

  auto UnitResult = Staging.Unit.verify();
  if (!UnitResult) {
    static_cast<void>(
        fail(Stream->documents().back().Range, "parsed compilation unit failed verification: " +
                                                   std::string(UnitResult.error().message())));
    return std::move(Result);
  }
  ActiveDocument = nullptr;
  Result.Document = std::move(Staging);
  return std::move(Result);
}

bool MIRDocumentParser::parseFunction(const yaml::Node &Root, MIRDocument &Document) {
  constexpr std::array Keys{
      std::string_view{"name"},
      std::string_view{"alignment"},
      std::string_view{"exposesReturnsTwice"},
      std::string_view{"legalized"},
      std::string_view{"regBankSelected"},
      std::string_view{"selected"},
      std::string_view{"failedISel"},
      std::string_view{"tracksRegLiveness"},
      std::string_view{"hasWinCFI"},
      std::string_view{"noPhis"},
      std::string_view{"isSSA"},
      std::string_view{"noVRegs"},
      std::string_view{"hasFakeUses"},
      std::string_view{"callsEHReturn"},
      std::string_view{"callsUnwindInit"},
      std::string_view{"hasEHContTarget"},
      std::string_view{"hasEHScopes"},
      std::string_view{"hasEHFunclets"},
      std::string_view{"isOutlined"},
      std::string_view{"debugInstrRef"},
      std::string_view{"failsVerification"},
      std::string_view{"tracksDebugUserValues"},
      std::string_view{"registers"},
      std::string_view{"liveins"},
      std::string_view{"calleeSavedRegisters"},
      std::string_view{"frameInfo"},
      std::string_view{"fixedStack"},
      std::string_view{"stack"},
      std::string_view{"entry_values"},
      std::string_view{"callSites"},
      std::string_view{"debugValueSubstitutions"},
      std::string_view{"constants"},
      std::string_view{"machineFunctionInfo"},
      std::string_view{"jumpTable"},
      std::string_view{"machineMetadataNodes"},
      std::string_view{"calledGlobals"},
      std::string_view{"prefetch-targets"},
      std::string_view{"body"},
  };
  const yaml::Node *NameNode = mappingValue(Root, "name");
  if (NameNode == nullptr)
    return fail(Root.range(), "missing required key 'name'");
  if (validateKeys(Root, Keys, "machine function"))
    return true;
  std::string_view Name;
  if (scalar(*NameNode, Name, "machine function name"))
    return true;

  const EmbeddedIRFunction *IRFunction = nullptr;
  if (Document.EmbeddedIR) {
    if (Name.empty()) {
      std::size_t AnonymousOrdinal = 0;
      for (std::size_t Index = 0; Index < Document.Unit.functionCount(); ++Index) {
        const MachineFunction *Existing = Document.Unit.functionAt(Index);
        if (Existing != nullptr && Existing->name().empty())
          ++AnonymousOrdinal;
      }
      IRFunction = Document.EmbeddedIR->unnamedFunction(AnonymousOrdinal);
    } else {
      IRFunction = Document.EmbeddedIR->function(Name);
    }
    if (IRFunction == nullptr)
      return fail(NameNode->range(),
                  "function '" + std::string(Name) + "' isn't defined in the provided LLVM IR");
  }

  if (!Name.empty() && Document.Unit.function(Name) != nullptr)
    return fail(NameNode->range(), "redefinition of machine function '" + std::string(Name) + "'");

  auto Created = Document.Unit.createMachineFunction(std::string(Name));
  if (!Created)
    return importError(NameNode->range(), Created.error());
  MachineFunction &Function = **Created;
  ActiveFunction = &Function;

  MIRFunctionDocumentInfo DocumentInfo{
      .Function = Function.stableID(),
      .NoPHIs = std::nullopt,
      .IsSSA = std::nullopt,
      .NoVRegs = std::nullopt,
      .HasFakeUses = std::nullopt,
  };
  MIParserContext Context;
  if (IRFunction != nullptr) {
    Context.HasIRBlock = [IRFunction](std::string_view BlockName) {
      return IRFunction->block(BlockName) != nullptr;
    };
  }

  std::vector<PendingJumpTable> PendingJumpTables;
  if (parseFunctionScalars(Root, Function, DocumentInfo) ||
      parseMetadata(Root, Function, Context) || parseRegisters(Root, Function, Context) ||
      parseStackObjects(Root, Function, Context) || parseConstants(Root, Function, Context) ||
      reserveJumpTables(Root, Function, Context, PendingJumpTables) ||
      parseBody(Root, Function, Context) || defineJumpTables(Function, PendingJumpTables) ||
      parseFrameState(Root, Function, Context) ||
      recomputeProperties(Root, Function, DocumentInfo) || parseTargetState(Root, Function) ||
      parseFunctionRecords(Root, Function) || validateDocumentReferences(Function)) {
    return true;
  }

  if (Options.VerifyMachineFunctions) {
    auto Verified = MachineVerifier::verify(Function, Target);
    if (!Verified)
      return importError(Root.range(), Verified.error(), "mir-verifier");
  }
  Document.FunctionInfo.push_back(DocumentInfo);
  ActiveFunction = nullptr;
  return false;
}

} // namespace rcc::mir
