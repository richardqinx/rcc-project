// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINEMETADATA_H
#define RCC_CODEGEN_MACHINEMETADATA_H

#include "rcc/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace rcc {

enum class MachineMetadataScope : std::uint8_t {
  EmbeddedIR,
  MachineFunction,
};

class MachineMetadataReference final {
public:
  constexpr MachineMetadataReference() noexcept = default;

  [[nodiscard]] static constexpr MachineMetadataReference embeddedIR(std::uint32_t Slot) noexcept {
    return MachineMetadataReference(MachineMetadataScope::EmbeddedIR, Slot);
  }

  [[nodiscard]] static constexpr MachineMetadataReference
  machineFunction(std::uint32_t Slot) noexcept {
    return MachineMetadataReference(MachineMetadataScope::MachineFunction, Slot);
  }

  [[nodiscard]] constexpr bool isValid() const noexcept {
    return Valid;
  }

  [[nodiscard]] constexpr MachineMetadataScope scope() const noexcept {
    return Scope;
  }

  [[nodiscard]] constexpr std::uint32_t slot() const noexcept {
    return Slot;
  }

  friend constexpr bool operator==(MachineMetadataReference Left,
                                   MachineMetadataReference Right) noexcept = default;

private:
  constexpr MachineMetadataReference(MachineMetadataScope Scope, std::uint32_t Slot) noexcept
      : Scope(Scope), Slot(Slot), Valid(true) {}

  MachineMetadataScope Scope = MachineMetadataScope::EmbeddedIR;
  std::uint32_t Slot = 0;
  bool Valid = false;
};

class MachineMetadataElement final {
public:
  MachineMetadataElement() = default;

  [[nodiscard]] static Expected<MachineMetadataElement>
  reference(MachineMetadataReference Reference);
  [[nodiscard]] static MachineMetadataElement string(std::string Value);

  [[nodiscard]] bool isValid() const noexcept {
    return !std::holds_alternative<std::monostate>(Storage);
  }

  [[nodiscard]] const MachineMetadataReference *reference() const noexcept {
    return std::get_if<MachineMetadataReference>(&Storage);
  }

  [[nodiscard]] const std::string *string() const noexcept {
    return std::get_if<std::string>(&Storage);
  }

  friend bool operator==(const MachineMetadataElement &Left,
                         const MachineMetadataElement &Right) = default;

private:
  explicit MachineMetadataElement(MachineMetadataReference Reference) : Storage(Reference) {}
  explicit MachineMetadataElement(std::string Value) : Storage(std::move(Value)) {}

  std::variant<std::monostate, MachineMetadataReference, std::string> Storage;
};

struct MachineMetadataNode final {
  std::uint32_t Slot = 0;
  bool IsDistinct = false;
  std::vector<MachineMetadataElement> Elements;

  friend bool operator==(const MachineMetadataNode &Left,
                         const MachineMetadataNode &Right) = default;
};

class MachineMetadataInfo final {
public:
  MachineMetadataInfo() = default;
  MachineMetadataInfo(const MachineMetadataInfo &) = delete;
  MachineMetadataInfo &operator=(const MachineMetadataInfo &) = delete;
  MachineMetadataInfo(MachineMetadataInfo &&) noexcept = default;
  MachineMetadataInfo &operator=(MachineMetadataInfo &&) noexcept = default;
  ~MachineMetadataInfo() = default;

  [[nodiscard]] Expected<void> addNode(MachineMetadataNode Node);

  [[nodiscard]] const MachineMetadataNode *node(std::uint32_t Slot) const noexcept;

  [[nodiscard]] std::span<const MachineMetadataNode> nodes() const noexcept {
    return Nodes;
  }

  [[nodiscard]] Expected<void> verify() const;
  [[nodiscard]] Expected<MachineMetadataInfo> clone() const;

  friend bool operator==(const MachineMetadataInfo &Left, const MachineMetadataInfo &Right) {
    return Left.Nodes == Right.Nodes;
  }

private:
  std::vector<MachineMetadataNode> Nodes;
  std::unordered_map<std::uint32_t, std::size_t> SlotToIndex;
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINEMETADATA_H
