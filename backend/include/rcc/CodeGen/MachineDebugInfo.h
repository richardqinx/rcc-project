// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINEDEBUGINFO_H
#define RCC_CODEGEN_MACHINEDEBUGINFO_H

#include "rcc/CodeGen/MachineMetadata.h"
#include "rcc/Support/Error.h"

#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace rcc {

class MachineDIExpression final {
public:
  MachineDIExpression() = default;
  explicit MachineDIExpression(std::vector<std::uint64_t> Elements)
      : Elements(std::move(Elements)) {}

  [[nodiscard]] std::span<const std::uint64_t> elements() const noexcept {
    return Elements;
  }

  friend bool operator==(const MachineDIExpression &Left,
                         const MachineDIExpression &Right) = default;

private:
  std::vector<std::uint64_t> Elements;
};

class MachineMetadataValue final {
public:
  MachineMetadataValue() = default;

  [[nodiscard]] static Expected<MachineMetadataValue> reference(MachineMetadataReference Reference);
  [[nodiscard]] static MachineMetadataValue expression(MachineDIExpression Expression);

  [[nodiscard]] bool isValid() const noexcept {
    return !std::holds_alternative<std::monostate>(Storage);
  }

  [[nodiscard]] const MachineMetadataReference *reference() const noexcept {
    return std::get_if<MachineMetadataReference>(&Storage);
  }

  [[nodiscard]] const MachineDIExpression *expression() const noexcept {
    return std::get_if<MachineDIExpression>(&Storage);
  }

  [[nodiscard]] Expected<void> verify(const MachineMetadataInfo &Metadata) const;

  friend bool operator==(const MachineMetadataValue &Left,
                         const MachineMetadataValue &Right) = default;

private:
  explicit MachineMetadataValue(MachineMetadataReference Reference) : Storage(Reference) {}
  explicit MachineMetadataValue(MachineDIExpression Expression) : Storage(std::move(Expression)) {}

  std::variant<std::monostate, MachineMetadataReference, MachineDIExpression> Storage;
};

struct MachineDILocationFrame final {
  std::uint32_t Line = 0;
  std::uint16_t Column = 0;
  MachineMetadataReference Scope;
  bool IsImplicitCode = false;
  std::uint64_t AtomGroup = 0;
  std::uint8_t AtomRank = 0;

  friend constexpr bool operator==(MachineDILocationFrame Left,
                                   MachineDILocationFrame Right) noexcept = default;
};

class MachineDILocation final {
public:
  static constexpr std::uint64_t MaxAtomGroup = (std::uint64_t{1} << 61) - 1;

  [[nodiscard]] static Expected<MachineDILocation>
  create(std::vector<MachineDILocationFrame> InlineChain,
         std::optional<MachineMetadataReference> TerminalInlinedAt = std::nullopt);

  [[nodiscard]] std::span<const MachineDILocationFrame> inlineChain() const noexcept {
    return InlineChain;
  }

  [[nodiscard]] std::optional<MachineMetadataReference> terminalInlinedAt() const noexcept {
    return TerminalInlinedAt;
  }

  [[nodiscard]] Expected<void> verify(const MachineMetadataInfo &Metadata) const;

  friend bool operator==(const MachineDILocation &Left, const MachineDILocation &Right) = default;

private:
  MachineDILocation(std::vector<MachineDILocationFrame> InlineChain,
                    std::optional<MachineMetadataReference> TerminalInlinedAt)
      : InlineChain(std::move(InlineChain)), TerminalInlinedAt(TerminalInlinedAt) {}

  [[nodiscard]] Expected<void> verifyStructure() const;

  std::vector<MachineDILocationFrame> InlineChain;
  std::optional<MachineMetadataReference> TerminalInlinedAt;
};

class MachineDebugLocation final {
public:
  MachineDebugLocation() = default;

  [[nodiscard]] static Expected<MachineDebugLocation> reference(MachineMetadataReference Reference);
  [[nodiscard]] static MachineDebugLocation inlineLocation(MachineDILocation Location);

  [[nodiscard]] bool isValid() const noexcept {
    return !std::holds_alternative<std::monostate>(Storage);
  }

  [[nodiscard]] const MachineMetadataReference *reference() const noexcept {
    return std::get_if<MachineMetadataReference>(&Storage);
  }

  [[nodiscard]] const MachineDILocation *inlineLocation() const noexcept {
    return std::get_if<MachineDILocation>(&Storage);
  }

  [[nodiscard]] Expected<void> verify(const MachineMetadataInfo &Metadata) const;

  friend bool operator==(const MachineDebugLocation &Left,
                         const MachineDebugLocation &Right) = default;

private:
  explicit MachineDebugLocation(MachineMetadataReference Reference) : Storage(Reference) {}
  explicit MachineDebugLocation(MachineDILocation Location) : Storage(std::move(Location)) {}

  std::variant<std::monostate, MachineMetadataReference, MachineDILocation> Storage;
};

struct MachineVariableDebugInfo final {
  std::optional<MachineMetadataReference> Variable;
  std::optional<MachineMetadataValue> Expression;
  std::optional<MachineDebugLocation> Location;

  [[nodiscard]] Expected<void> verifyStructure() const;
  [[nodiscard]] Expected<void> verify(const MachineMetadataInfo &Metadata) const;

  friend bool operator==(const MachineVariableDebugInfo &Left,
                         const MachineVariableDebugInfo &Right) = default;
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINEDEBUGINFO_H
