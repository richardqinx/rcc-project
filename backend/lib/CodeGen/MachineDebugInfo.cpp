// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineDebugInfo.h"

#include <utility>

namespace rcc {

Expected<MachineMetadataValue> MachineMetadataValue::reference(MachineMetadataReference Reference) {
  if (!Reference.isValid()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "machine metadata value reference must be valid");
  }
  return MachineMetadataValue(Reference);
}

MachineMetadataValue MachineMetadataValue::expression(MachineDIExpression Expression) {
  return MachineMetadataValue(std::move(Expression));
}

Expected<void> MachineMetadataValue::verify(const MachineMetadataInfo &Metadata) const {
  if (!isValid()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "machine metadata value has no representation");
  }
  const MachineMetadataReference *Reference = reference();
  if (Reference == nullptr)
    return {};
  return verifyMachineMetadataReference(*Reference, Metadata);
}

Expected<MachineDILocation>
MachineDILocation::create(std::vector<MachineDILocationFrame> InlineChain,
                          std::optional<MachineMetadataReference> TerminalInlinedAt) {
  MachineDILocation Result(std::move(InlineChain), TerminalInlinedAt);
  auto Verification = Result.verifyStructure();
  if (!Verification)
    return std::unexpected(Verification.error());
  return Result;
}

Expected<void> MachineDILocation::verifyStructure() const {
  if (InlineChain.empty()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "inline debug location requires at least one frame");
  }
  for (const MachineDILocationFrame &Frame : InlineChain) {
    if (!Frame.Scope.isValid()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "inline debug location frame has an invalid scope");
    }
    if (Frame.Scope.scope() != MachineMetadataScope::EmbeddedIR) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "inline debug location scope must reference embedded IR metadata");
    }
    if (Frame.AtomGroup > MaxAtomGroup) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "inline debug location atom group exceeds 61 bits");
    }
    if (Frame.AtomRank > 7) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "inline debug location atom rank exceeds three bits");
    }
  }
  if (TerminalInlinedAt && !TerminalInlinedAt->isValid()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "inline debug location has an invalid terminal inlined-at reference");
  }
  if (TerminalInlinedAt && TerminalInlinedAt->scope() != MachineMetadataScope::EmbeddedIR) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "inline debug location inlined-at must reference embedded IR metadata");
  }
  return {};
}

Expected<void> MachineDILocation::verify(const MachineMetadataInfo &Metadata) const {
  auto Structure = verifyStructure();
  if (!Structure)
    return Structure;
  for (const MachineDILocationFrame &Frame : InlineChain) {
    auto Scope = verifyMachineMetadataReference(Frame.Scope, Metadata);
    if (!Scope)
      return Scope;
  }
  if (TerminalInlinedAt)
    return verifyMachineMetadataReference(*TerminalInlinedAt, Metadata);
  return {};
}

Expected<MachineDebugLocation> MachineDebugLocation::reference(MachineMetadataReference Reference) {
  if (!Reference.isValid()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "machine debug location reference must be valid");
  }
  if (Reference.scope() != MachineMetadataScope::EmbeddedIR) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "machine debug location must reference embedded IR metadata");
  }
  return MachineDebugLocation(Reference);
}

MachineDebugLocation MachineDebugLocation::inlineLocation(MachineDILocation Location) {
  return MachineDebugLocation(std::move(Location));
}

Expected<void> MachineDebugLocation::verify(const MachineMetadataInfo &Metadata) const {
  if (!isValid()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "machine debug location has no representation");
  }
  if (const MachineMetadataReference *Reference = reference())
    return verifyMachineMetadataReference(*Reference, Metadata);
  return inlineLocation()->verify(Metadata);
}

Expected<void> MachineVariableDebugInfo::verifyStructure() const {
  if (!Variable && !Expression && !Location) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "variable debug information has no metadata");
  }
  if (Variable && !Variable->isValid()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "variable debug information has an invalid variable reference");
  }
  if (Variable && Variable->scope() != MachineMetadataScope::EmbeddedIR) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "variable debug information must reference embedded IR metadata");
  }
  if (Expression && !Expression->isValid()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "variable debug information has an invalid expression");
  }
  if (Expression && Expression->reference() != nullptr &&
      Expression->reference()->scope() != MachineMetadataScope::EmbeddedIR) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "variable debug expression must reference embedded IR metadata");
  }
  if (Location && !Location->isValid()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "variable debug information has an invalid location");
  }
  return {};
}

Expected<void> MachineVariableDebugInfo::verify(const MachineMetadataInfo &Metadata) const {
  auto Structure = verifyStructure();
  if (!Structure)
    return Structure;
  if (Variable) {
    auto VariableResult = verifyMachineMetadataReference(*Variable, Metadata);
    if (!VariableResult)
      return VariableResult;
  }
  if (Expression) {
    auto ExpressionResult = Expression->verify(Metadata);
    if (!ExpressionResult)
      return ExpressionResult;
  }
  if (Location)
    return Location->verify(Metadata);
  return {};
}

} // namespace rcc
