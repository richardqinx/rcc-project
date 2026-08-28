// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachinePassManager.h"

#include "rcc/CodeGen/CompilationUnit.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineVerifier.h"
#include "rcc/CodeGen/TargetDescription.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rcc {

namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] std::uint64_t elapsedNanoseconds(Clock::time_point Start) noexcept {
  const auto Elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - Start);
  if (Elapsed.count() <= 0)
    return 0;
  return static_cast<std::uint64_t>(Elapsed.count());
}

[[nodiscard]] bool hasDuplicateAnalysisIDs(std::span<const AnalysisID> IDs) {
  std::unordered_set<AnalysisID> Unique;
  for (const AnalysisID ID : IDs) {
    if (ID == nullptr || !Unique.insert(ID).second)
      return true;
  }
  return false;
}

[[nodiscard]] Expected<void> validateParameters(std::span<const PassParameter> Parameters) {
  std::unordered_set<std::string> Names;
  for (const PassParameter &Parameter : Parameters) {
    if (Parameter.Name.empty())
      return makeUnexpected(ErrorCode::InvalidArgument, "pass parameter name cannot be empty");
    if (!Names.insert(Parameter.Name).second)
      return makeUnexpected(ErrorCode::InvalidArgument, "pass parameter names must be unique");
    if (Parameter.Required && Parameter.DefaultValue) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "a required pass parameter cannot have a default value");
    }
  }
  return {};
}

void combineFingerprint(std::uint64_t &State, std::uint64_t Value) noexcept {
  State ^= Value + 0x9e3779b97f4a7c15ULL + (State << 6U) + (State >> 2U);
}

[[nodiscard]] std::uint64_t machineFunctionFingerprint(const MachineFunction &Function) noexcept {
  std::uint64_t Result = 0xcbf29ce484222325ULL;
  combineFingerprint(Result, Function.stableID().value());
  combineFingerprint(Result, Function.properties().rawValue());
  combineFingerprint(Result, Function.blockCount());
  combineFingerprint(Result, Function.allInstructions().size());
  combineFingerprint(Result, Function.allMemOperands().size());

  for (const MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
       Block = Block->next()) {
    combineFingerprint(Result, Block->stableID().value());
    combineFingerprint(Result, Block->instructionCount());
    combineFingerprint(Result, Block->successors().size());
    for (const MachineSuccessor &Successor : Block->successors()) {
      combineFingerprint(Result,
                         Successor.Block == nullptr ? 0 : Successor.Block->stableID().value());
      combineFingerprint(Result, Successor.Probability.rawValue());
    }
    for (const MachineBasicBlockLiveIn &LiveIn : Block->liveIns()) {
      combineFingerprint(Result, LiveIn.PhysReg.number());
      combineFingerprint(Result, LiveIn.Lanes.rawValue());
    }

    for (const MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      combineFingerprint(Result, Instruction->stableID().value());
      combineFingerprint(Result, Instruction->opcode().value());
      combineFingerprint(Result, Instruction->flags().rawValue());
      combineFingerprint(Result, Instruction->operandCount());
      combineFingerprint(Result, Instruction->memOperands().size());
      for (const MachineOperand &Operand : Instruction->operands()) {
        combineFingerprint(Result, static_cast<std::uint64_t>(Operand.kind()));
        combineFingerprint(Result, Operand.targetFlags());
        combineFingerprint(Result, Operand.registerFlags().rawValue());
        combineFingerprint(Result, Operand.subReg().value());
        if (const Register *Reg = Operand.getIf<Register>())
          combineFingerprint(Result, Reg->opaqueValue());
        else if (const std::int64_t *Immediate = Operand.getIf<std::int64_t>())
          combineFingerprint(Result, static_cast<std::uint64_t>(*Immediate));
      }
    }
  }
  return Result;
}

[[nodiscard]] Expected<void> verifyCompilationUnit(const CompilationUnit &Unit,
                                                   const TargetDescription &Target) {
  auto UnitVerification = Unit.verify();
  if (!UnitVerification)
    return std::unexpected(UnitVerification.error());
  for (std::size_t Index = 0; Index < Unit.functionCount(); ++Index) {
    const MachineFunction *Function = Unit.functionAt(Index);
    if (Function == nullptr)
      return makeUnexpected(ErrorCode::InternalError,
                            "compilation unit function order contains a null entry");
    auto FunctionVerification = MachineVerifier::verify(*Function, Target);
    if (!FunctionVerification)
      return std::unexpected(FunctionVerification.error());
  }
  return {};
}

} // namespace

Expected<void>
PassManagerCallbacks<MachineFunction, MachineFunctionPass, MachinePassManagerOptions>::validate(
    const MachineFunctionPass &Pass) {
  const MachinePassContract &Contract = Pass.contract();
  if (Contract.Name.empty())
    return makeUnexpected(ErrorCode::InvalidArgument, "machine pass must have a non-empty name");
  if (Contract.Sets.intersects(Contract.Clears)) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "machine pass cannot set and clear the same property");
  }
  if (hasDuplicateAnalysisIDs(Contract.RequiredAnalyses)) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "machine pass required analyses must be non-null and unique");
  }
  return validateParameters(Contract.Parameters);
}

Expected<void>
PassManagerCallbacks<MachineFunction, MachineFunctionPass, MachinePassManagerOptions>::run(
    const PassList &Passes, const MachinePassManagerOptions &Options, MachineFunction &Function,
    const TargetDescription &Target, MachineFunctionAnalysisManager &Analyses,
    MachinePassInstrumentation *Instrumentation) {
  Analyses.setInstrumentation(Instrumentation);
  for (const std::unique_ptr<MachineFunctionPass> &Pass : Passes) {
    const MachinePassContract &Contract = Pass->contract();
    const std::string_view Name = Contract.Name;

    if (const MachineFunctionProperties Missing =
            Contract.Requires.missingFrom(Function.properties());
        !Missing.empty()) {
      Error Failure(ErrorCode::VerificationError,
                    "pass '" + std::string(Name) + "' requires properties: " + Missing.toString());
      if (Instrumentation != nullptr)
        Instrumentation->failure(Name, Function, Failure);
      Analyses.setInstrumentation(nullptr);
      return std::unexpected(std::move(Failure));
    }

    if (Options.VerifyBefore) {
      auto Verification = MachineVerifier::verify(Function, Target);
      if (!Verification) {
        Error Failure = withErrorContext(
            Verification.error(), "pass '" + std::string(Name) + "' received invalid MachineIR: ");
        if (Instrumentation != nullptr)
          Instrumentation->failure(Name, Function, Failure);
        Analyses.setInstrumentation(nullptr);
        return std::unexpected(std::move(Failure));
      }
    }

    for (const AnalysisID Required : Contract.RequiredAnalyses) {
      auto Result = Analyses.getResultByID(Required, Function, Target);
      if (!Result) {
        Error Failure =
            withErrorContext(Result.error(), "pass '" + std::string(Name) +
                                                 "' could not materialize a required analysis: ");
        if (Instrumentation != nullptr)
          Instrumentation->failure(Name, Function, Failure);
        Analyses.setInstrumentation(nullptr);
        return std::unexpected(std::move(Failure));
      }
    }

    const std::uint64_t FingerprintBefore =
        Options.DetectUnreportedChanges ? machineFunctionFingerprint(Function) : 0;
    if (Instrumentation != nullptr)
      Instrumentation->before(Name, Function);
    const Clock::time_point Start = Clock::now();
    Analyses.setActivePass(std::string(Name));
    auto PassResult = Pass->run(Function, Target, Analyses);
    Analyses.setActivePass({});
    const std::uint64_t Duration = elapsedNanoseconds(Start);
    if (!PassResult) {
      Error Failure =
          withErrorContext(PassResult.error(), "pass '" + std::string(Name) + "' failed: ");
      if (Instrumentation != nullptr)
        Instrumentation->failure(Name, Function, Failure, Duration);
      Analyses.clear(Function);
      Analyses.setInstrumentation(nullptr);
      return std::unexpected(std::move(Failure));
    }

    if (PassResult->Changed != !PassResult->Changes.empty()) {
      Error Failure(ErrorCode::VerificationError,
                    "pass '" + std::string(Name) +
                        "' reported inconsistent changed state and modification categories");
      if (Instrumentation != nullptr)
        Instrumentation->failure(Name, Function, Failure, Duration);
      Analyses.clear(Function);
      Analyses.setInstrumentation(nullptr);
      return std::unexpected(std::move(Failure));
    }
    if (!Contract.DeclaredChanges.contains(PassResult->Changes)) {
      Error Failure(ErrorCode::VerificationError,
                    "pass '" + std::string(Name) +
                        "' reported a modification category absent from its registration");
      if (Instrumentation != nullptr)
        Instrumentation->failure(Name, Function, Failure, Duration);
      Analyses.clear(Function);
      Analyses.setInstrumentation(nullptr);
      return std::unexpected(std::move(Failure));
    }

    bool FingerprintChanged = PassResult->Changed;
    if (Options.DetectUnreportedChanges) {
      FingerprintChanged = FingerprintBefore != machineFunctionFingerprint(Function);
      if (FingerprintChanged && !PassResult->Changed) {
        Error Failure(ErrorCode::VerificationError,
                      "pass '" + std::string(Name) +
                          "' changed MachineIR without reporting a modification");
        if (Instrumentation != nullptr)
          Instrumentation->failure(Name, Function, Failure, Duration);
        Analyses.clear(Function);
        Analyses.setInstrumentation(nullptr);
        return std::unexpected(std::move(Failure));
      }
    }
    if (Instrumentation != nullptr)
      Instrumentation->change(Name, Function, PassResult->Changes, FingerprintChanged);

    auto Transition =
        Function.properties().applyTransition(Contract.Requires, Contract.Sets, Contract.Clears);
    if (!Transition) {
      Error Failure =
          withErrorContext(Transition.error(), "pass '" + std::string(Name) +
                                                   "' produced an invalid property transition: ");
      if (Instrumentation != nullptr)
        Instrumentation->failure(Name, Function, Failure, Duration);
      Analyses.clear(Function);
      Analyses.setInstrumentation(nullptr);
      return std::unexpected(std::move(Failure));
    }
    Analyses.invalidate(Function, PassResult->Preserved, PassResult->Changes);

    if (Options.VerifyEach) {
      MachineVerifierOptions VerifyOptions;
      VerifyOptions.Stage = Contract.VerificationStage;
      auto Verification = MachineVerifier::verify(Function, Target, VerifyOptions);
      if (!Verification) {
        Error Failure = withErrorContext(Verification.error(), "pass '" + std::string(Name) +
                                                                   "' left invalid MachineIR: ");
        if (Instrumentation != nullptr)
          Instrumentation->failure(Name, Function, Failure, Duration);
        Analyses.clear(Function);
        Analyses.setInstrumentation(nullptr);
        return std::unexpected(std::move(Failure));
      }
    }
    if (Instrumentation != nullptr)
      Instrumentation->after(Name, Function, *PassResult, Duration);
  }
  Analyses.setInstrumentation(nullptr);
  return {};
}

Expected<void>
PassManagerCallbacks<MachineFunction, MachineFunctionPass, MachinePassManagerOptions>::runFunctions(
    const PassList &Passes, const MachinePassManagerOptions &Options, CompilationUnit &Unit,
    const TargetDescription &Target, const MachineFunctionAnalysisManager &AnalysisPrototype,
    std::size_t ThreadCount, MachinePassInstrumentation *Instrumentation) {
  if (ThreadCount == 0)
    return makeUnexpected(ErrorCode::InvalidArgument, "machine pass thread count must be nonzero");
  auto UnitVerification = verifyCompilationUnit(Unit, Target);
  if (!UnitVerification)
    return std::unexpected(UnitVerification.error());
  const std::size_t FunctionCount = Unit.functionCount();
  if (FunctionCount == 0)
    return {};
  const bool RequiresSerialInstrumentation =
      Instrumentation != nullptr && Instrumentation->requiresSerialFunctionExecution();
  const std::size_t WorkerCount =
      RequiresSerialInstrumentation ? 1 : std::min(ThreadCount, FunctionCount);
  if (WorkerCount > 1) {
    for (const std::unique_ptr<MachineFunctionPass> &Pass : Passes) {
      if (Pass->contract().ThreadSafety != PassThreadSafety::ThreadSafe) {
        return makeUnexpected(ErrorCode::InvalidArgument,
                              "parallel machine pipeline contains a pass not marked thread-safe");
      }
      auto AnalysisSafety =
          AnalysisPrototype.validateParallelRequirements(Pass->contract().RequiredAnalyses);
      if (!AnalysisSafety)
        return std::unexpected(AnalysisSafety.error());
    }
  }
  std::vector<std::unique_ptr<MachineFunctionAnalysisManager>> Managers;
  Managers.reserve(FunctionCount);
  for (std::size_t Index = 0; Index < FunctionCount; ++Index) {
    auto Manager = AnalysisPrototype.forkEmpty();
    if (!Manager)
      return std::unexpected(Manager.error());
    Managers.push_back(std::move(*Manager));
  }

  std::vector<std::optional<Error>> Failures(FunctionCount);
  std::atomic<std::size_t> NextFunction = 0;
  const auto Worker = [&]() {
    while (true) {
      const std::size_t Index = NextFunction.fetch_add(1, std::memory_order_relaxed);
      if (Index >= FunctionCount)
        return;
      MachineFunction *Function = Unit.functionAt(Index);
      if (Function == nullptr) {
        Failures[Index] = Error(ErrorCode::InternalError,
                                "compilation unit contains a null function during parallel run");
        continue;
      }
      auto Result = PassManagerCallbacks::run(Passes, Options, *Function, Target, *Managers[Index],
                                              Instrumentation);
      if (!Result)
        Failures[Index] = Result.error();
    }
  };

  std::vector<std::thread> Workers;
  Workers.reserve(WorkerCount);
  for (std::size_t Index = 0; Index < WorkerCount; ++Index)
    Workers.emplace_back(Worker);
  for (std::thread &WorkerThread : Workers)
    WorkerThread.join();

  for (std::size_t Index = 0; Index < FunctionCount; ++Index) {
    const auto &Failure = Failures[Index];
    if (Failure) {
      return std::unexpected(withErrorContext(
          *Failure, "function '" + std::string(Unit.functionAt(Index)->name()) + "' failed: "));
    }
  }
  return {};
}

Expected<void>
PassManagerCallbacks<CompilationUnit, CompilationUnitPass,
                     CompilationUnitPassManagerOptions>::validate(const CompilationUnitPass &Pass) {
  const CompilationUnitPassContract &Contract = Pass.contract();
  if (Contract.Name.empty()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "compilation-unit pass must have a non-empty name");
  }
  if (hasDuplicateAnalysisIDs(Contract.RequiredAnalyses)) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "compilation-unit pass required analyses must be non-null and unique");
  }
  auto Parameters = validateParameters(Contract.Parameters);
  if (!Parameters)
    return std::unexpected(Parameters.error());
  return {};
}

Expected<void>
PassManagerCallbacks<CompilationUnit, CompilationUnitPass, CompilationUnitPassManagerOptions>::run(
    const PassList &Passes, const CompilationUnitPassManagerOptions &Options, CompilationUnit &Unit,
    const TargetDescription &Target, CompilationUnitAnalysisManager &UnitAnalyses,
    MachineFunctionAnalysisManager &FunctionAnalyses, MachinePassInstrumentation *Instrumentation) {
  UnitAnalyses.setInstrumentation(Instrumentation);
  FunctionAnalyses.setInstrumentation(Instrumentation);
  for (const std::unique_ptr<CompilationUnitPass> &Pass : Passes) {
    const CompilationUnitPassContract &Contract = Pass->contract();
    const std::string_view Name = Contract.Name;

    if (Options.VerifyBefore) {
      auto Verification = verifyCompilationUnit(Unit, Target);
      if (!Verification) {
        Error Failure =
            withErrorContext(Verification.error(), "compilation-unit pass '" + std::string(Name) +
                                                       "' received invalid MachineIR: ");
        if (Instrumentation != nullptr)
          Instrumentation->unitFailure(Name, Unit, Failure);
        UnitAnalyses.setInstrumentation(nullptr);
        FunctionAnalyses.setInstrumentation(nullptr);
        return std::unexpected(std::move(Failure));
      }
    }

    for (const AnalysisID Required : Contract.RequiredAnalyses) {
      auto Result = UnitAnalyses.getResultByID(Required, Unit, Target);
      if (!Result) {
        Error Failure =
            withErrorContext(Result.error(), "compilation-unit pass '" + std::string(Name) +
                                                 "' could not materialize a required analysis: ");
        if (Instrumentation != nullptr)
          Instrumentation->unitFailure(Name, Unit, Failure);
        UnitAnalyses.setInstrumentation(nullptr);
        FunctionAnalyses.setInstrumentation(nullptr);
        return std::unexpected(std::move(Failure));
      }
    }

    if (Instrumentation != nullptr)
      Instrumentation->beforeUnit(Name, Unit);
    const Clock::time_point Start = Clock::now();
    auto Result = Pass->run(Unit, Target, UnitAnalyses, FunctionAnalyses);
    const std::uint64_t Duration = elapsedNanoseconds(Start);
    if (!Result) {
      Error Failure = withErrorContext(Result.error(), "compilation-unit pass '" +
                                                           std::string(Name) + "' failed: ");
      if (Instrumentation != nullptr)
        Instrumentation->unitFailure(Name, Unit, Failure, Duration);
      UnitAnalyses.clearAll();
      FunctionAnalyses.clearAll();
      UnitAnalyses.setInstrumentation(nullptr);
      FunctionAnalyses.setInstrumentation(nullptr);
      return std::unexpected(std::move(Failure));
    }
    if (Result->Changed != !Result->Changes.empty() ||
        !Contract.DeclaredChanges.contains(Result->Changes)) {
      Error Failure(ErrorCode::VerificationError,
                    "compilation-unit pass '" + std::string(Name) +
                        "' reported inconsistent or undeclared modifications");
      if (Instrumentation != nullptr)
        Instrumentation->unitFailure(Name, Unit, Failure, Duration);
      UnitAnalyses.clearAll();
      FunctionAnalyses.clearAll();
      UnitAnalyses.setInstrumentation(nullptr);
      FunctionAnalyses.setInstrumentation(nullptr);
      return std::unexpected(std::move(Failure));
    }

    UnitAnalyses.invalidate(Unit, Result->PreservedUnitAnalyses, Result->Changes);
    for (std::size_t Index = 0; Index < Unit.functionCount(); ++Index) {
      MachineFunction *Function = Unit.functionAt(Index);
      if (Function != nullptr) {
        FunctionAnalyses.invalidate(*Function, Result->PreservedFunctionAnalyses, Result->Changes);
      }
    }
    if (Options.VerifyEach) {
      auto Verification = verifyCompilationUnit(Unit, Target);
      if (!Verification) {
        Error Failure =
            withErrorContext(Verification.error(), "compilation-unit pass '" + std::string(Name) +
                                                       "' left invalid MachineIR: ");
        if (Instrumentation != nullptr)
          Instrumentation->unitFailure(Name, Unit, Failure, Duration);
        UnitAnalyses.clearAll();
        FunctionAnalyses.clearAll();
        UnitAnalyses.setInstrumentation(nullptr);
        FunctionAnalyses.setInstrumentation(nullptr);
        return std::unexpected(std::move(Failure));
      }
    }
    if (Instrumentation != nullptr)
      Instrumentation->afterUnit(Name, Unit, *Result, Duration);
  }
  UnitAnalyses.setInstrumentation(nullptr);
  FunctionAnalyses.setInstrumentation(nullptr);
  return {};
}

} // namespace rcc
