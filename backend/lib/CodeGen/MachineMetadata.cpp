// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineMetadata.h"

#include <cstddef>
#include <string>
#include <unordered_set>
#include <utility>

namespace rcc {
namespace {

[[nodiscard]] Expected<void> verifyNodeElements(const MachineMetadataNode &Node) {
  for (const MachineMetadataElement &Element : Node.Elements) {
    if (!Element.isValid()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "machine metadata tuple contains an invalid element");
    }
    const MachineMetadataReference *Reference = Element.reference();
    if (Reference != nullptr && !Reference->isValid()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "machine metadata tuple contains an invalid reference");
    }
  }
  return {};
}

} // namespace

Expected<MachineMetadataElement>
MachineMetadataElement::reference(MachineMetadataReference Reference) {
  if (!Reference.isValid()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "machine metadata element reference must be valid");
  }
  return MachineMetadataElement(Reference);
}

MachineMetadataElement MachineMetadataElement::string(std::string Value) {
  return MachineMetadataElement(std::move(Value));
}

Expected<void> MachineMetadataInfo::addNode(MachineMetadataNode Node) {
  auto Verification = verifyNodeElements(Node);
  if (!Verification)
    return std::unexpected(Verification.error());
  if (SlotToIndex.contains(Node.Slot)) {
    return makeUnexpected(ErrorCode::InvalidArgument, "machine metadata slot is already defined");
  }

  const std::size_t Index = Nodes.size();
  SlotToIndex.emplace(Node.Slot, Index);
  Nodes.push_back(std::move(Node));
  return {};
}

const MachineMetadataNode *MachineMetadataInfo::node(std::uint32_t Slot) const noexcept {
  const auto Position = SlotToIndex.find(Slot);
  if (Position == SlotToIndex.end())
    return nullptr;
  return &Nodes[Position->second];
}

Expected<void> MachineMetadataInfo::verify() const {
  if (SlotToIndex.size() != Nodes.size()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "machine metadata slot index has an inconsistent size");
  }

  std::unordered_set<std::uint32_t> Slots;
  for (std::size_t Index = 0; Index < Nodes.size(); ++Index) {
    const MachineMetadataNode &Node = Nodes[Index];
    if (!Slots.insert(Node.Slot).second) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "machine metadata contains duplicate slot definitions");
    }
    const auto Position = SlotToIndex.find(Node.Slot);
    if (Position == SlotToIndex.end() || Position->second != Index) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "machine metadata slot index is inconsistent");
    }
    auto ElementVerification = verifyNodeElements(Node);
    if (!ElementVerification)
      return ElementVerification;
  }

  for (const MachineMetadataNode &Node : Nodes) {
    for (const MachineMetadataElement &Element : Node.Elements) {
      const MachineMetadataReference *Reference = Element.reference();
      if (Reference != nullptr && Reference->scope() == MachineMetadataScope::MachineFunction &&
          !Slots.contains(Reference->slot())) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "machine metadata tuple references an undefined function slot");
      }
    }
  }
  return {};
}

Expected<MachineMetadataInfo> MachineMetadataInfo::clone() const {
  auto Verification = verify();
  if (!Verification)
    return std::unexpected(Verification.error());

  MachineMetadataInfo Result;
  for (const MachineMetadataNode &Node : Nodes) {
    auto AddResult = Result.addNode(Node);
    if (!AddResult)
      return std::unexpected(AddResult.error());
  }
  return Result;
}

} // namespace rcc
