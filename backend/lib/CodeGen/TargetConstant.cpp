// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/TargetConstant.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>

namespace rcc {

Expected<void> TargetConstantEmission::verify() const {
  if (Bytes.empty())
    return makeUnexpected(ErrorCode::VerificationError,
                          "target constant emission must contain at least one byte");

  std::unordered_set<std::uint64_t> FixupOffsets;
  for (const TargetConstantFixup &Fixup : Fixups) {
    if (Fixup.Offset >= Bytes.size()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "target constant fixup offset is outside emitted bytes");
    }
    if (Fixup.Kind.value() == 0)
      return makeUnexpected(ErrorCode::VerificationError,
                            "target constant fixup relocation kind must be nonzero");
    if (Fixup.Symbol.empty())
      return makeUnexpected(ErrorCode::VerificationError,
                            "target constant fixup symbol cannot be empty");
    if (!FixupOffsets.insert(Fixup.Offset).second) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "target constant emission has duplicate fixup offsets");
    }
  }
  return {};
}

TargetConstant::TargetConstant(TargetConstant &&) noexcept = default;
TargetConstant &TargetConstant::operator=(TargetConstant &&) noexcept = default;
TargetConstant::~TargetConstant() = default;

std::optional<TargetConstantTypeID> TargetConstant::typeID() const noexcept {
  if (Storage == nullptr)
    return std::nullopt;
  return Storage->typeID();
}

std::optional<std::uint32_t> TargetConstant::schemaVersion() const noexcept {
  if (Storage == nullptr)
    return std::nullopt;
  return Storage->schemaVersion();
}

Expected<void> TargetConstant::verify() const {
  if (Storage == nullptr)
    return makeUnexpected(ErrorCode::VerificationError, "target constant is empty");
  if (Storage->typeID().value() == 0 || Storage->schemaVersion() == 0) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "target constant identity or schema version is invalid");
  }
  return Storage->verify();
}

Expected<TargetConstant> TargetConstant::clone() const {
  auto Verification = verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  auto ClonedStorage = Storage->cloneStorage();
  if (!ClonedStorage)
    return std::unexpected(ClonedStorage.error());
  TargetConstant Result(std::move(*ClonedStorage));
  auto CloneVerification = Result.verify();
  if (!CloneVerification)
    return std::unexpected(CloneVerification.error());
  return Result;
}

Expected<std::uint64_t> TargetConstant::stableHash() const {
  auto Verification = verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  return Storage->stableHash();
}

Expected<std::string> TargetConstant::print() const {
  auto Verification = verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  return Storage->print();
}

Expected<EncodedTargetConstant> TargetConstant::encode() const {
  auto Verification = verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  auto Payload = Storage->encodePayload();
  if (!Payload)
    return std::unexpected(Payload.error());
  return EncodedTargetConstant{Storage->typeID(), Storage->schemaVersion(), std::move(*Payload)};
}

Expected<TargetConstantEmission> TargetConstant::emit() const {
  auto Verification = verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  auto Emission = Storage->emit();
  if (!Emission)
    return std::unexpected(Emission.error());
  auto EmissionVerification = Emission->verify();
  if (!EmissionVerification)
    return std::unexpected(EmissionVerification.error());
  return Emission;
}

Expected<Alignment> TargetConstant::requiredAlignment() const {
  auto Verification = verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  return Storage->requiredAlignment();
}

Expected<std::optional<TargetRelocationKindID>> TargetConstant::relocationKind() const {
  auto Verification = verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  const std::optional<TargetRelocationKindID> Kind = Storage->relocationKind();
  if (Kind && Kind->value() == 0) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "target constant relocation kind must be nonzero");
  }
  return Kind;
}

bool TargetConstant::equals(const TargetConstant &Other) const noexcept {
  if (Storage == nullptr || Other.Storage == nullptr)
    return Storage == nullptr && Other.Storage == nullptr;
  if (Storage->typeID() != Other.Storage->typeID() ||
      Storage->localTypeToken() != Other.Storage->localTypeToken()) {
    return false;
  }
  return Storage->equals(*Other.Storage);
}

} // namespace rcc
