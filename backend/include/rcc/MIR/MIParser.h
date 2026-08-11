// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_MIR_MIPARSER_H
#define RCC_MIR_MIPARSER_H

#include "rcc/CodeGen/MachineIRTypes.h"
#include "rcc/CodeGen/MachineMetadata.h"
#include "rcc/CodeGen/Register.h"
#include "rcc/Support/Error.h"
#include "rcc/Support/TextDiagnostic.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rcc {

class MachineFunction;
class TargetDescription;

namespace mir {

struct MIVirtualRegisterSlot final {
  std::uint32_t TextID = 0;
  Register Reg;
};

struct MIFrameObjectSlot final {
  std::uint32_t TextID = 0;
  FrameIndex Index;
  std::optional<std::string> Name;
};

struct MIConstantPoolSlot final {
  std::uint32_t TextID = 0;
  ConstantPoolIndex Index;
};

struct MIJumpTableSlot final {
  std::uint32_t TextID = 0;
  JumpTableIndex Index;
};

struct MIMetadataSlot final {
  std::uint32_t TextID = 0;
  MachineMetadataReference Reference;
};

struct MISyncScopeSlot final {
  std::string Name;
  SyncScopeID ID;
};

struct MIParserContext final {
  std::vector<MIVirtualRegisterSlot> VirtualRegisters;
  std::vector<MIFrameObjectSlot> StackObjects;
  std::vector<MIFrameObjectSlot> FixedStackObjects;
  std::vector<MIConstantPoolSlot> ConstantPoolItems;
  std::vector<MIJumpTableSlot> JumpTables;
  std::vector<MIMetadataSlot> Metadata;
  std::vector<MISyncScopeSlot> SyncScopes;
  std::function<bool(std::string_view)> HasIRBlock;
  std::function<Expected<SyncScopeID>(std::string_view)> InternSyncScope;
  std::function<Expected<std::uint32_t>(std::uint32_t)> PointerSizeInBits;
  std::uint32_t DefaultPointerSizeInBits = 0;
};

struct MIParserOptions final {
  std::size_t MaximumTokens = std::size_t{1} << 24U;
  std::size_t MaximumTokenLength = std::size_t{1} << 24U;
  std::size_t MaximumBlocks = std::size_t{1} << 20U;
  std::size_t MaximumInstructions = std::size_t{1} << 24U;
  std::size_t MaximumOperandsPerInstruction = std::size_t{1} << 20U;
  std::size_t MaximumMemoryOperandsPerInstruction = std::size_t{1} << 20U;
  bool VerifyAfterParse = true;
};

class MIParser final {
public:
  MIParser(MachineFunction &Function, const TargetDescription &Target, std::string SourceName,
           std::string SourceText, MIParserContext Context = {}, MIParserOptions Options = {});
  MIParser(const MIParser &) = delete;
  MIParser &operator=(const MIParser &) = delete;
  MIParser(MIParser &&) noexcept;
  MIParser &operator=(MIParser &&) noexcept;
  ~MIParser();

  [[nodiscard]] Expected<void> parse();

  [[nodiscard]] const TextSource &source() const noexcept;
  [[nodiscard]] std::span<const TextDiagnostic> diagnostics() const noexcept;
  [[nodiscard]] bool failed() const noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> Implementation;
};

} // namespace mir
} // namespace rcc

#endif // RCC_MIR_MIPARSER_H
