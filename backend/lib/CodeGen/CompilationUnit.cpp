// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/CompilationUnit.h"

#include "rcc/CodeGen/MachineFunction.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace rcc {

CompilationUnit::CompilationUnit() = default;

CompilationUnit::CompilationUnit(CompilationUnit &&) noexcept = default;

CompilationUnit &CompilationUnit::operator=(CompilationUnit &&) noexcept = default;

CompilationUnit::~CompilationUnit() = default;

Expected<MachineFunction *> CompilationUnit::createMachineFunction(std::string Name) {
  if (Name.empty())
    return makeUnexpected(ErrorCode::InvalidArgument, "machine function name cannot be empty");
  if (FunctionNameToIndex.contains(Name)) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "machine function name is already present in the compilation unit");
  }
  if (GlobalNameToIndex.contains(Name)) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "machine function name conflicts with a global object");
  }

  auto StableID = FunctionIDGenerator.allocate();
  if (!StableID)
    return std::unexpected(StableID.error());
  auto Function = MachineFunction::create(std::move(Name), *StableID);
  if (!Function)
    return std::unexpected(Function.error());

  MachineFunction *Result = Function->get();
  const std::size_t Index = Functions.size();
  FunctionNameToIndex.emplace(std::string(Result->name()), Index);
  Functions.push_back(std::move(*Function));
  return Result;
}

Expected<GlobalObjectDesc *> CompilationUnit::addGlobalObject(GlobalObjectDesc Object) {
  auto Verification = Object.verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  if (FunctionNameToIndex.contains(Object.Symbol)) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "global object symbol conflicts with a machine function");
  }
  if (GlobalNameToIndex.contains(Object.Symbol)) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "global object symbol is already present in the compilation unit");
  }

  const std::size_t Index = GlobalObjects.size();
  auto Storage = std::make_unique<GlobalObjectDesc>(std::move(Object));
  GlobalObjectDesc *Result = Storage.get();
  GlobalNameToIndex.emplace(Result->Symbol, Index);
  GlobalObjects.push_back(std::move(Storage));
  return Result;
}

MachineFunction *CompilationUnit::functionAt(std::size_t Index) noexcept {
  if (Index >= Functions.size())
    return nullptr;
  return Functions[Index].get();
}

const MachineFunction *CompilationUnit::functionAt(std::size_t Index) const noexcept {
  if (Index >= Functions.size())
    return nullptr;
  return Functions[Index].get();
}

MachineFunction *CompilationUnit::function(std::string_view Name) noexcept {
  const auto Position = FunctionNameToIndex.find(Name);
  if (Position == FunctionNameToIndex.end())
    return nullptr;
  return Functions[Position->second].get();
}

const MachineFunction *CompilationUnit::function(std::string_view Name) const noexcept {
  const auto Position = FunctionNameToIndex.find(Name);
  if (Position == FunctionNameToIndex.end())
    return nullptr;
  return Functions[Position->second].get();
}

MachineFunction *CompilationUnit::function(MachineFunctionStableID ID) noexcept {
  for (const std::unique_ptr<MachineFunction> &Function : Functions) {
    if (Function->stableID() == ID)
      return Function.get();
  }
  return nullptr;
}

const MachineFunction *CompilationUnit::function(MachineFunctionStableID ID) const noexcept {
  for (const std::unique_ptr<MachineFunction> &Function : Functions) {
    if (Function->stableID() == ID)
      return Function.get();
  }
  return nullptr;
}

GlobalObjectDesc *CompilationUnit::globalObjectAt(std::size_t Index) noexcept {
  return Index < GlobalObjects.size() ? GlobalObjects[Index].get() : nullptr;
}

const GlobalObjectDesc *CompilationUnit::globalObjectAt(std::size_t Index) const noexcept {
  return Index < GlobalObjects.size() ? GlobalObjects[Index].get() : nullptr;
}

GlobalObjectDesc *CompilationUnit::globalObject(std::string_view Name) noexcept {
  const auto Position = GlobalNameToIndex.find(Name);
  return Position == GlobalNameToIndex.end() ? nullptr : GlobalObjects[Position->second].get();
}

const GlobalObjectDesc *CompilationUnit::globalObject(std::string_view Name) const noexcept {
  const auto Position = GlobalNameToIndex.find(Name);
  return Position == GlobalNameToIndex.end() ? nullptr : GlobalObjects[Position->second].get();
}

Expected<void> CompilationUnit::verify() const {
  if (FunctionNameToIndex.size() != Functions.size()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "compilation unit function name index has an inconsistent size");
  }
  if (GlobalNameToIndex.size() != GlobalObjects.size()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "compilation unit global object name index has an inconsistent size");
  }

  std::unordered_set<MachineFunctionStableID> StableIDs;
  for (std::size_t Index = 0; Index < Functions.size(); ++Index) {
    const std::unique_ptr<MachineFunction> &Function = Functions[Index];
    if (Function == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "compilation unit contains a null machine function");
    }
    if (Function->name().empty() || !Function->stableID().isValid()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "compilation unit contains an invalid machine function identity");
    }
    if (!StableIDs.insert(Function->stableID()).second) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "compilation unit contains duplicate machine function StableIDs");
    }

    const auto Position = FunctionNameToIndex.find(Function->name());
    if (Position == FunctionNameToIndex.end() || Position->second != Index) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "compilation unit function name index is inconsistent");
    }
    if (GlobalNameToIndex.contains(Function->name())) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "function name conflicts with a global object symbol");
    }
  }

  for (std::size_t Index = 0; Index < GlobalObjects.size(); ++Index) {
    const std::unique_ptr<GlobalObjectDesc> &Object = GlobalObjects[Index];
    if (Object == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "compilation unit contains a null global object");
    }
    auto ObjectVerification = Object->verify();
    if (!ObjectVerification)
      return std::unexpected(ObjectVerification.error());
    const auto Position = GlobalNameToIndex.find(Object->Symbol);
    if (Position == GlobalNameToIndex.end() || Position->second != Index) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "compilation unit global object name index is inconsistent");
    }
    if (FunctionNameToIndex.contains(Object->Symbol)) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "global object symbol conflicts with a function name");
    }
  }
  return {};
}

} // namespace rcc
