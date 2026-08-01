// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineFunctionRecords.h"

#include "rcc/CodeGen/MachineFunctionCloner.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_map>
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

[[nodiscard]] constexpr std::uint64_t
debugReferenceKey(MachineDebugValueReference Reference) noexcept {
  return (static_cast<std::uint64_t>(Reference.InstructionNumber) << 32U) |
         static_cast<std::uint64_t>(Reference.OperandNumber);
}

[[nodiscard]] Expected<void>
verifyDebugValueSubstitution(const MachineDebugValueSubstitution &Substitution) {
  if (Substitution.Source.InstructionNumber == 0 ||
      Substitution.Destination.InstructionNumber == 0) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "debug value substitution has a zero instruction number");
  }
  if (Substitution.Source.InstructionNumber == Substitution.Destination.InstructionNumber) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "debug value substitution cannot replace a value within one instruction");
  }
  if (Substitution.Source.OperandNumber == MachineDebugMemoryOperandNumber) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "debug value substitution cannot originate from the memory operand");
  }
  return {};
}

[[nodiscard]] Expected<void> verifyEntryValue(const MachineEntryValueInfo &EntryValue) {
  if (!EntryValue.Reg.isValid()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "entry-value record requires a physical register");
  }
  return EntryValue.DebugInfo.verifyStructure();
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

Expected<void>
MachineFunctionRecords::addDebugValueSubstitution(MachineDebugValueSubstitution Substitution) {
  auto Verification = verifyDebugValueSubstitution(Substitution);
  if (!Verification)
    return std::unexpected(Verification.error());
  if (std::ranges::any_of(DebugValueSubstitutions,
                          [&Substitution](const MachineDebugValueSubstitution &Existing) {
                            return Existing.Source == Substitution.Source;
                          })) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "debug value source already has a substitution");
  }
  DebugValueSubstitutions.push_back(Substitution);
  return {};
}

Expected<void> MachineFunctionRecords::addEntryValue(MachineEntryValueInfo EntryValue) {
  auto Verification = verifyEntryValue(EntryValue);
  if (!Verification)
    return std::unexpected(Verification.error());
  EntryValues.push_back(std::move(EntryValue));
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

  std::unordered_map<std::uint64_t, std::uint64_t> DebugSubstitutionEdges;
  for (const MachineDebugValueSubstitution &Substitution : DebugValueSubstitutions) {
    auto SubstitutionVerification = verifyDebugValueSubstitution(Substitution);
    if (!SubstitutionVerification)
      return SubstitutionVerification;
    const std::uint64_t Source = debugReferenceKey(Substitution.Source);
    const std::uint64_t Destination = debugReferenceKey(Substitution.Destination);
    if (!DebugSubstitutionEdges.emplace(Source, Destination).second) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "multiple debug substitutions have the same source");
    }
  }

  std::unordered_set<std::uint64_t> CompletedDebugReferences;
  for (const auto &Edge : DebugSubstitutionEdges) {
    const std::uint64_t Start = Edge.first;
    std::unordered_set<std::uint64_t> Path;
    std::uint64_t Current = Start;
    while (!CompletedDebugReferences.contains(Current)) {
      const auto Position = DebugSubstitutionEdges.find(Current);
      if (Position == DebugSubstitutionEdges.end())
        break;
      if (!Path.insert(Current).second) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "debug value substitutions contain a cycle");
      }
      Current = Position->second;
    }
    CompletedDebugReferences.insert(Path.begin(), Path.end());
  }

  for (const MachineEntryValueInfo &EntryValue : EntryValues) {
    auto EntryValueVerification = verifyEntryValue(EntryValue);
    if (!EntryValueVerification)
      return EntryValueVerification;
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
  for (const MachineDebugValueSubstitution Source : DebugValueSubstitutions) {
    auto AddResult = Result.addDebugValueSubstitution(Source);
    if (!AddResult)
      return std::unexpected(AddResult.error());
  }
  for (const MachineEntryValueInfo &Source : EntryValues) {
    auto AddResult = Result.addEntryValue(Source);
    if (!AddResult)
      return std::unexpected(AddResult.error());
  }
  return Result;
}

} // namespace rcc
