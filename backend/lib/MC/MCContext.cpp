// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/MC/MCContext.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace rcc::mc {
namespace {

[[nodiscard]] bool isValidName(std::string_view Name) noexcept {
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

Expected<MCSymbolRef> MCContext::getOrCreateSymbol(std::string Name, MCSymbolKind Kind) {
  if (!isValidName(Name))
    return makeUnexpected(ErrorCode::InvalidArgument, "MC symbol has an invalid name");

  const auto Position = Symbols.find(Name);
  if (Position != Symbols.end()) {
    const MCSymbolKind ExistingKind = Position->second->kind();
    if (Kind != MCSymbolKind::Generic && ExistingKind != MCSymbolKind::Generic &&
        ExistingKind != Kind) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "MC symbol name is already used by a different symbol kind");
    }
    return Position->second;
  }

  MCSymbolRef Result(new MCSymbol(std::move(Name), Kind));
  Symbols.emplace(std::string(Result->name()), Result);
  return Result;
}

Expected<MCSymbolRef> MCContext::createTemporarySymbol(std::string_view Prefix) {
  if (!isValidName(Prefix))
    return makeUnexpected(ErrorCode::InvalidArgument, "temporary MC symbol prefix is invalid");
  if (NextTemporarySymbol == std::numeric_limits<std::uint64_t>::max()) {
    return makeUnexpected(ErrorCode::InternalError, "temporary MC symbol sequence is exhausted");
  }

  while (true) {
    std::string Name(Prefix);
    Name += std::to_string(NextTemporarySymbol++);
    if (Symbols.contains(Name))
      continue;
    return getOrCreateSymbol(std::move(Name), MCSymbolKind::Temporary);
  }
}

MCSymbolRef MCContext::symbol(std::string_view Name) const noexcept {
  const auto Position = Symbols.find(Name);
  return Position == Symbols.end() ? MCSymbolRef{} : Position->second;
}

Expected<MCSectionRef> MCContext::getOrCreateSection(MCSectionDesc Description) {
  auto Verification = Description.verify();
  if (!Verification)
    return std::unexpected(Verification.error());

  const auto Position = Sections.find(Description.Name);
  if (Position != Sections.end()) {
    MCSectionDesc ExistingDescription = Position->second->description();
    ExistingDescription.Alignment = Description.Alignment;
    if (ExistingDescription != Description) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "MC section name is already used by a different description");
    }
    return Position->second;
  }

  MCSectionRef Result(new MCSection(std::move(Description)));
  Sections.emplace(std::string(Result->name()), Result);
  return Result;
}

MCSectionRef MCContext::section(std::string_view Name) const noexcept {
  const auto Position = Sections.find(Name);
  return Position == Sections.end() ? MCSectionRef{} : Position->second;
}

} // namespace rcc::mc
