// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/MC/MCSection.h"

#include <bit>
#include <cstdint>
#include <string_view>

namespace rcc::mc {
namespace {

[[nodiscard]] bool isValidAssemblyName(std::string_view Name) noexcept {
  if (Name.empty())
    return false;
  for (const char RawCharacter : Name) {
    const auto Character = static_cast<unsigned char>(RawCharacter);
    if (Character == 0 || Character == '\n' || Character == '\r' || Character == 0x7fU)
      return false;
  }
  return true;
}

} // namespace

Expected<void> MCSectionDesc::verify() const {
  if (!isValidAssemblyName(Name))
    return makeUnexpected(ErrorCode::InvalidArgument, "MC section has an invalid name");
  if (!std::has_single_bit(Alignment)) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "MC section alignment must be a nonzero power of two");
  }
  if (Flags.has(MCSectionFlag::Strings) && !Flags.has(MCSectionFlag::Merge)) {
    return makeUnexpected(ErrorCode::InvalidArgument, "string MC sections must also be mergeable");
  }
  if (Flags.has(MCSectionFlag::Merge) && EntrySize == 0) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "mergeable MC sections require a nonzero entry size");
  }
  if ((Kind == MCSectionKind::TLSData || Kind == MCSectionKind::TLSBSS) &&
      !Flags.has(MCSectionFlag::TLS)) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "TLS MC sections must carry the TLS section flag");
  }
  if (Flags.has(MCSectionFlag::Group) != static_cast<bool>(Group)) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "MC section group flag and group symbol must agree");
  }
  if (Group && Group->name().empty()) {
    return makeUnexpected(ErrorCode::InvalidArgument, "MC section group symbol must have a name");
  }
  if (UniqueID && *UniqueID == 0) {
    return makeUnexpected(ErrorCode::InvalidArgument, "unique MC section identity must be nonzero");
  }
  return {};
}

} // namespace rcc::mc
