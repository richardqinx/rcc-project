// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineFunctionRecords.h"

#include "rcc/CodeGen/MachineFunctionCloner.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_set>
#include <utility>

namespace rcc {
namespace {

[[nodiscard]] Expected<void>
verifyCalleeSavedRegisters(std::span<const PhysicalRegister> Registers) {
  std::unordered_set<PhysicalRegister> Seen;
  for (const PhysicalRegister Reg : Registers) {
    if (!Reg.isValid()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "callee-saved register list contains noreg");
    }
    if (!Seen.insert(Reg).second) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "callee-saved register list contains a duplicate register");
    }
  }
  return {};
}

[[nodiscard]] Expected<void> verifyCallSite(const MachineCallSiteInfo &CallSite) {
  if (!CallSite.CallInstruction.isValid()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "call-site record has an invalid instruction StableID");
  }
  for (const MachineCallSiteArgument &Argument : CallSite.ForwardedArguments) {
    if (!Argument.Reg.isValid()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "call-site forwarded argument contains noreg");
    }
  }
  return {};
}

[[nodiscard]] Expected<void> verifyCalledGlobal(const MachineCalledGlobal &CalledGlobal) {
  if (!CalledGlobal.CallInstruction.isValid()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "called-global record has an invalid instruction StableID");
  }
  if (CalledGlobal.Callee.empty()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "called-global record has an empty callee name");
  }
  return {};
}

} // namespace

Expected<void>
MachineFunctionRecords::setCalleeSavedRegisters(std::vector<PhysicalRegister> Registers) {
  auto Verification = verifyCalleeSavedRegisters(Registers);
  if (!Verification)
    return std::unexpected(Verification.error());
  CalleeSavedRegisters = std::move(Registers);
  return {};
}

Expected<void> MachineFunctionRecords::addCallSite(MachineCallSiteInfo CallSite) {
  auto Verification = verifyCallSite(CallSite);
  if (!Verification)
    return std::unexpected(Verification.error());
  if (std::ranges::any_of(CallSites, [&CallSite](const MachineCallSiteInfo &Existing) {
        return Existing.CallInstruction == CallSite.CallInstruction;
      })) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "call instruction already has a call-site record");
  }
  CallSites.push_back(std::move(CallSite));
  return {};
}

Expected<void> MachineFunctionRecords::addCalledGlobal(MachineCalledGlobal CalledGlobal) {
  auto Verification = verifyCalledGlobal(CalledGlobal);
  if (!Verification)
    return std::unexpected(Verification.error());
  if (std::ranges::any_of(CalledGlobals, [&CalledGlobal](const MachineCalledGlobal &Existing) {
        return Existing.CallInstruction == CalledGlobal.CallInstruction;
      })) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "call instruction already has a called-global record");
  }
  CalledGlobals.push_back(std::move(CalledGlobal));
  return {};
}

Expected<void> MachineFunctionRecords::addPrefetchTarget(MachinePrefetchTarget Target) {
  if (std::ranges::find(PrefetchTargets, Target) != PrefetchTargets.end()) {
    return makeUnexpected(ErrorCode::InvalidArgument, "prefetch target is already recorded");
  }
  PrefetchTargets.push_back(Target);
  return {};
}

Expected<void> MachineFunctionRecords::verify() const {
  if (CalleeSavedRegisters) {
    auto CalleeSavedVerification = verifyCalleeSavedRegisters(*CalleeSavedRegisters);
    if (!CalleeSavedVerification)
      return CalleeSavedVerification;
  }

  std::unordered_set<MachineInstrStableID> CallInstructions;
  for (const MachineCallSiteInfo &CallSite : CallSites) {
    auto CallSiteVerification = verifyCallSite(CallSite);
    if (!CallSiteVerification)
      return CallSiteVerification;
    if (!CallInstructions.insert(CallSite.CallInstruction).second) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "multiple call-site records reference one call instruction");
    }
  }

  std::unordered_set<MachineInstrStableID> CalledGlobalInstructions;
  for (const MachineCalledGlobal &CalledGlobal : CalledGlobals) {
    auto CalledGlobalVerification = verifyCalledGlobal(CalledGlobal);
    if (!CalledGlobalVerification)
      return CalledGlobalVerification;
    if (!CalledGlobalInstructions.insert(CalledGlobal.CallInstruction).second) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "multiple called-global records reference one call instruction");
    }
  }

  for (std::size_t Index = 0; Index < PrefetchTargets.size(); ++Index) {
    if (std::ranges::find(PrefetchTargets.begin() + static_cast<std::ptrdiff_t>(Index + 1),
                          PrefetchTargets.end(), PrefetchTargets[Index]) != PrefetchTargets.end()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "prefetch target list contains a duplicate entry");
    }
  }
  return {};
}

Expected<MachineFunctionRecords>
MachineFunctionRecords::clone(const MachineFunctionCloneContext &CloneContext) const {
  auto Verification = verify();
  if (!Verification)
    return std::unexpected(Verification.error());

  MachineFunctionRecords Result;
  if (CalleeSavedRegisters) {
    auto SetResult = Result.setCalleeSavedRegisters(*CalleeSavedRegisters);
    if (!SetResult)
      return std::unexpected(SetResult.error());
  }

  for (const MachineCallSiteInfo &Source : CallSites) {
    auto Instruction = CloneContext.mapInstructionStableID(Source.CallInstruction);
    if (!Instruction)
      return std::unexpected(Instruction.error());
    MachineCallSiteInfo Destination = Source;
    Destination.CallInstruction = *Instruction;
    auto AddResult = Result.addCallSite(std::move(Destination));
    if (!AddResult)
      return std::unexpected(AddResult.error());
  }

  for (const MachineCalledGlobal &Source : CalledGlobals) {
    auto Instruction = CloneContext.mapInstructionStableID(Source.CallInstruction);
    if (!Instruction)
      return std::unexpected(Instruction.error());
    MachineCalledGlobal Destination = Source;
    Destination.CallInstruction = *Instruction;
    auto AddResult = Result.addCalledGlobal(std::move(Destination));
    if (!AddResult)
      return std::unexpected(AddResult.error());
  }

  for (const MachinePrefetchTarget Source : PrefetchTargets) {
    auto AddResult = Result.addPrefetchTarget(Source);
    if (!AddResult)
      return std::unexpected(AddResult.error());
  }
  return Result;
}

} // namespace rcc
