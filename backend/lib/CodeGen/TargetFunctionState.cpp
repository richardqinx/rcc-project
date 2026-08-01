// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/TargetFunctionState.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace rcc {

TargetFunctionState::TargetFunctionState(TargetFunctionState &&) noexcept = default;
TargetFunctionState &TargetFunctionState::operator=(TargetFunctionState &&) noexcept = default;
TargetFunctionState::~TargetFunctionState() = default;

std::optional<TargetFunctionStateTypeID> TargetFunctionState::typeID() const noexcept {
  if (Storage == nullptr)
    return std::nullopt;
  return Storage->typeID();
}

std::optional<std::uint32_t> TargetFunctionState::schemaVersion() const noexcept {
  if (Storage == nullptr)
    return std::nullopt;
  return Storage->schemaVersion();
}

Expected<void> TargetFunctionState::verify() const {
  if (Storage == nullptr)
    return makeUnexpected(ErrorCode::VerificationError, "target function state is empty");
  if (Storage->typeID().value() == 0 || Storage->schemaVersion() == 0) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "target function state identity or schema version is invalid");
  }
  return Storage->verify();
}

Expected<TargetFunctionState>
TargetFunctionState::clone(const MachineFunctionCloneContext &CloneContext) const {
  auto Verification = verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  auto ClonedStorage = Storage->cloneStorage(CloneContext);
  if (!ClonedStorage)
    return std::unexpected(ClonedStorage.error());
  TargetFunctionState Result(std::move(*ClonedStorage));
  auto CloneVerification = Result.verify();
  if (!CloneVerification)
    return std::unexpected(CloneVerification.error());
  return Result;
}

Expected<std::string> TargetFunctionState::print() const {
  auto Verification = verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  return Storage->print();
}

Expected<EncodedTargetFunctionState> TargetFunctionState::encode() const {
  auto Verification = verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  auto Payload = Storage->encodePayload();
  if (!Payload)
    return std::unexpected(Payload.error());
  return EncodedTargetFunctionState{Storage->typeID(), Storage->schemaVersion(),
                                    std::move(*Payload)};
}

bool TargetFunctionState::equals(const TargetFunctionState &Other) const noexcept {
  if (Storage == nullptr || Other.Storage == nullptr)
    return Storage == nullptr && Other.Storage == nullptr;
  if (Storage->typeID() != Other.Storage->typeID() ||
      Storage->localTypeToken() != Other.Storage->localTypeToken()) {
    return false;
  }
  return Storage->equals(*Other.Storage);
}

} // namespace rcc
