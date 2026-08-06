// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineJumpTableInfo.h"

#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace rcc {

Expected<void>
MachineJumpTableInfo::verifyDescription(const MachineJumpTableDesc &Description) const {
  if (Description.Encoding >= JumpTableEntryEncoding::Count)
    return makeUnexpected(ErrorCode::VerificationError, "jump table encoding is invalid");
  if (Description.EntrySize == 0 || Description.EntrySize > 16 ||
      !std::has_single_bit(Description.EntrySize)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "jump table entry size must be a power of two no larger than 16 bytes");
  }
  if (Description.Targets.empty())
    return makeUnexpected(ErrorCode::VerificationError, "jump table must have at least one target");
  if (Description.Placement.MaximumDistance && *Description.Placement.MaximumDistance == 0) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "jump table maximum distance must be nonzero");
  }
  if ((Description.Encoding == JumpTableEntryEncoding::Target) !=
      (Description.TargetEncoding != 0)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "target jump table encoding and target encoding ID are inconsistent");
  }
  if ((Description.Encoding == JumpTableEntryEncoding::GPRelative32BlockAddress ||
       Description.Encoding == JumpTableEntryEncoding::LabelDifference32 ||
       Description.Encoding == JumpTableEntryEncoding::Custom32) &&
      Description.EntrySize != 4) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "32-bit jump table encodings require four-byte entries");
  }
  if ((Description.Encoding == JumpTableEntryEncoding::GPRelative64BlockAddress ||
       Description.Encoding == JumpTableEntryEncoding::LabelDifference64) &&
      Description.EntrySize != 8) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "64-bit jump table encodings require eight-byte entries");
  }

  for (const MachineBasicBlock *Target : Description.Targets) {
    if (Target == nullptr || &Target->function() != Owner || !Target->isLinked()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "jump table target must be linked to its owning function");
    }
  }
  return {};
}

Expected<JumpTableIndex> MachineJumpTableInfo::createTable(MachineJumpTableDesc Description) {
  if (Frozen)
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "frozen jump table info cannot create tables");
  auto Verification = verifyDescription(Description);
  if (!Verification)
    return std::unexpected(Verification.error());
  if (Tables.size() > std::numeric_limits<std::uint32_t>::max())
    return makeUnexpected(ErrorCode::Unsupported, "jump table index space is exhausted");

  const JumpTableIndex Index = JumpTableIndex::fromValue(static_cast<std::uint32_t>(Tables.size()));
  Tables.push_back(MachineJumpTableEntry(Index, std::move(Description)));
  return Index;
}

Expected<JumpTableIndex> MachineJumpTableInfo::reserveTable() {
  if (Frozen) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "frozen jump table info cannot reserve tables");
  }
  if (Tables.size() > std::numeric_limits<std::uint32_t>::max())
    return makeUnexpected(ErrorCode::Unsupported, "jump table index space is exhausted");

  const JumpTableIndex Index = JumpTableIndex::fromValue(static_cast<std::uint32_t>(Tables.size()));
  Tables.push_back(MachineJumpTableEntry(Index));
  return Index;
}

Expected<void>
MachineJumpTableInfo::defineReservedTable(JumpTableIndex Index,
                                          MachineJumpTableDesc Description) {
  if (Frozen) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "frozen jump table info cannot define reserved tables");
  }
  if (Index.value() >= Tables.size() || !Tables[Index.value()].isPending()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "jump table index is not an unresolved reservation");
  }
  auto Verification = verifyDescription(Description);
  if (!Verification)
    return std::unexpected(Verification.error());
  Tables[Index.value()].Description.emplace<MachineJumpTableDesc>(std::move(Description));
  return {};
}

Expected<MachineJumpTableEntry *> MachineJumpTableInfo::mutableTable(JumpTableIndex Index) {
  if (Frozen)
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "frozen jump table info cannot modify tables");
  if (Index.value() >= Tables.size())
    return makeUnexpected(ErrorCode::InvalidArgument, "jump table index is out of range");
  MachineJumpTableEntry &Entry = Tables[Index.value()];
  if (Entry.isTombstone() || Entry.isPending()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "jump table index has no defined description");
  }
  return &Entry;
}

Expected<void> MachineJumpTableInfo::updateTable(JumpTableIndex Index,
                                                 MachineJumpTableDesc Description) {
  auto Verification = verifyDescription(Description);
  if (!Verification)
    return std::unexpected(Verification.error());
  auto Entry = mutableTable(Index);
  if (!Entry)
    return std::unexpected(Entry.error());
  (*Entry)->Description.emplace<MachineJumpTableDesc>(std::move(Description));
  return {};
}

Expected<void> MachineJumpTableInfo::eraseTable(JumpTableIndex Index) {
  auto Entry = mutableTable(Index);
  if (!Entry)
    return std::unexpected(Entry.error());
  (*Entry)->Description.emplace<std::monostate>();
  return {};
}

const MachineJumpTableEntry *MachineJumpTableInfo::table(JumpTableIndex Index) const noexcept {
  if (Index.value() >= Tables.size() || Tables[Index.value()].isTombstone())
    return nullptr;
  return &Tables[Index.value()];
}

std::size_t MachineJumpTableInfo::liveTableCount() const noexcept {
  std::size_t Count = 0;
  for (const MachineJumpTableEntry &Entry : Tables) {
    if (!Entry.isTombstone())
      ++Count;
  }
  return Count;
}

Expected<void> MachineJumpTableInfo::freeze() {
  if (Frozen)
    return makeUnexpected(ErrorCode::InvalidArgument, "jump table info is already frozen");
  auto Verification = verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  Frozen = true;
  return {};
}

Expected<void> MachineJumpTableInfo::verify() const {
  if (Owner == nullptr)
    return makeUnexpected(ErrorCode::VerificationError, "jump table info has no owner");
  for (std::size_t Position = 0; Position < Tables.size(); ++Position) {
    const MachineJumpTableEntry &Entry = Tables[Position];
    if (Entry.index() != JumpTableIndex::fromValue(static_cast<std::uint32_t>(Position))) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "jump table index does not match its stable table position");
    }
    const MachineJumpTableDesc *Description = Entry.description();
    if (Entry.isPending()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "jump table contains an unresolved reservation");
    }
    if (Description == nullptr)
      continue;
    auto Verification = verifyDescription(*Description);
    if (!Verification)
      return std::unexpected(Verification.error());
  }
  return {};
}

} // namespace rcc
