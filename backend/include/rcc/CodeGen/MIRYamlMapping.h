// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MIRYAMLMAPPING_H
#define RCC_CODEGEN_MIRYAMLMAPPING_H

#include "rcc/CodeGen/MIRParser/MIRParser.h"
#include "rcc/CodeGen/MachineDebugInfo.h"
#include "rcc/CodeGen/MachineFrameInfo.h"
#include "rcc/CodeGen/MachineJumpTableInfo.h"
#include "rcc/CodeGen/MachineMetadata.h"
#include "rcc/CodeGen/TargetMIRTextInfo.h"
#include "rcc/MIR/LLVMConstantSyntax.h"
#include "rcc/Support/Error.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rcc {

class MachineBasicBlock;
class MachineFunction;
class MachineInstr;

namespace mir {

struct PendingJumpTable final {
  JumpTableIndex Index;
  std::uint32_t TextID = 0;
  JumpTableEntryEncoding Encoding = JumpTableEntryEncoding::BlockAddress;
  std::uint8_t EntrySize = 8;
  std::vector<std::string> Blocks;
};

class MIRDocumentParser final {
public:
  MIRDocumentParser(const TargetDescription &Target, std::string SourceName, std::string SourceText,
                    MIRDocumentParserOptions Options)
      : Target(Target), Options(std::move(Options)),
        Result(std::move(SourceName), std::move(SourceText)) {}

  [[nodiscard]] MIRDocumentParseResult run();

private:
  [[nodiscard]] bool fail(const TextRange &Range, std::string Message,
                          std::string Category = "mir-yaml");
  [[nodiscard]] bool importError(const TextRange &Range, const Error &Failure,
                                 std::string Category = "mir-yaml");
  [[nodiscard]] const yaml::Node *node(yaml::NodeID ID) const noexcept {
    return Stream->node(ID);
  }
  [[nodiscard]] const yaml::Node *mappingValue(const yaml::Node &Mapping,
                                               std::string_view Key) const noexcept;
  [[nodiscard]] bool requireKind(const yaml::Node &Node, yaml::NodeKind Kind,
                                 std::string_view Description);
  [[nodiscard]] bool validateKeys(const yaml::Node &Mapping, std::span<const std::string_view> Keys,
                                  std::string_view Description);
  [[nodiscard]] bool scalar(const yaml::Node &Node, std::string_view &Value,
                            std::string_view Description);
  [[nodiscard]] bool boolean(const yaml::Node &Node, bool &Value);
  [[nodiscard]] bool unsignedValue(const yaml::Node &Node, std::uint64_t &Value,
                                   std::string_view Description);
  [[nodiscard]] bool signedValue(const yaml::Node &Node, std::int64_t &Value,
                                 std::string_view Description);
  [[nodiscard]] bool alignment(const yaml::Node &Node, Alignment &Value, bool AllowZero);
  [[nodiscard]] bool physicalRegister(const yaml::Node &Node, PhysicalRegister &Reg);
  [[nodiscard]] bool physicalRegister(std::string_view Text, const TextRange &Range,
                                      PhysicalRegister &Reg);
  [[nodiscard]] bool virtualRegister(std::string_view Text, const TextRange &Range,
                                     MachineFunction &Function, MIParserContext &Context,
                                     Register &Reg);
  [[nodiscard]] Expected<MachineMetadataReference>
  resolveMetadataReference(const MachineFunction &Function, std::uint32_t Slot) const;
  [[nodiscard]] bool parseVariableDebugInfo(const yaml::Node &Entry, MachineFunction &Function,
                                            MachineVariableDebugInfo &DebugInfo);
  [[nodiscard]] bool parseFunction(const yaml::Node &Root, MIRDocument &Document);
  [[nodiscard]] bool parseFunctionScalars(const yaml::Node &Root, MachineFunction &Function,
                                          MIRFunctionDocumentInfo &DocumentInfo);
  [[nodiscard]] bool recomputeProperties(const yaml::Node &Root, MachineFunction &Function,
                                         const MIRFunctionDocumentInfo &DocumentInfo);
  [[nodiscard]] bool parseRegisters(const yaml::Node &Root, MachineFunction &Function,
                                    MIParserContext &Context);
  [[nodiscard]] bool parseMetadata(const yaml::Node &Root, MachineFunction &Function,
                                   MIParserContext &Context);
  [[nodiscard]] bool parseStackObjects(const yaml::Node &Root, MachineFunction &Function,
                                       MIParserContext &Context);
  [[nodiscard]] bool parseConstants(const yaml::Node &Root, MachineFunction &Function,
                                    MIParserContext &Context);
  [[nodiscard]] bool parseFrameState(const yaml::Node &Root, MachineFunction &Function,
                                     const MIParserContext &Context);
  [[nodiscard]] bool reserveJumpTables(const yaml::Node &Root, MachineFunction &Function,
                                       MIParserContext &Context,
                                       std::vector<PendingJumpTable> &Pending);
  [[nodiscard]] bool defineJumpTables(MachineFunction &Function,
                                      std::span<const PendingJumpTable> Pending);
  [[nodiscard]] bool parseBody(const yaml::Node &Root, MachineFunction &Function,
                               MIParserContext &Context);
  [[nodiscard]] TextRange scalarDiagnosticRange(const yaml::Node &Scalar,
                                                const TextRange &ScalarRange) const noexcept;
  [[nodiscard]] bool parseFunctionRecords(const yaml::Node &Root, MachineFunction &Function);
  [[nodiscard]] bool parseTargetState(const yaml::Node &Root, MachineFunction &Function);
  [[nodiscard]] bool convertTargetNode(const yaml::Node &Source, MIRTargetNode &Destination);
  [[nodiscard]] bool validateDocumentReferences(const MachineFunction &Function);
  [[nodiscard]] std::string_view effectiveDataLayout() const noexcept;
  [[nodiscard]] LLVMConstantSyntaxOptions pointerLayoutOptions() const noexcept;

  [[nodiscard]] MachineBasicBlock *blockAt(MachineFunction &Function,
                                           std::uint32_t Number) const noexcept;
  [[nodiscard]] MachineInstr *instructionAt(MachineFunction &Function, std::uint32_t Block,
                                            std::uint32_t Offset) const noexcept;
  [[nodiscard]] bool parseBlockReference(std::string_view Text, const TextRange &Range,
                                         MachineFunction &Function, MachineBasicBlock *&Block);
  [[nodiscard]] bool parseFrameReference(const yaml::Node &Node, const MIParserContext &Context,
                                         FrameIndex &Index);
  [[nodiscard]] bool parseSaveRestorePoints(const yaml::Node &Node, MachineFunction &Function,
                                            std::vector<MachineFrameSaveRestorePoint> &Points);

  const TargetDescription &Target;
  MIRDocumentParserOptions Options;
  MIRDocumentParseResult Result;
  const yaml::Stream *Stream = nullptr;
  MIRDocument *ActiveDocument = nullptr;
  MachineFunction *ActiveFunction = nullptr;
};

[[nodiscard]] Expected<void> verifyMIRDocumentReferences(const MIRDocument &Document,
                                                         const MachineFunction &Function,
                                                         bool UseEmbeddedIR);

[[nodiscard]] std::optional<std::uint64_t>
parseDIExpressionOpcodeName(std::string_view Name) noexcept;
[[nodiscard]] std::optional<std::uint64_t>
parseDIExpressionAttributeEncodingName(std::string_view Name) noexcept;
[[nodiscard]] std::string invalidDIExpressionElementMessage(std::string_view Name);
[[nodiscard]] std::string formatDIExpression(std::span<const std::uint64_t> Elements);

} // namespace mir
} // namespace rcc

#endif // RCC_CODEGEN_MIRYAMLMAPPING_H
