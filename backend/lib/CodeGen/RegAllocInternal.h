// SPDX-License-Identifier: GPL-3.0-or-later

// The private header has no stable include-root-relative guard name.
// NOLINTNEXTLINE(llvm-header-guard)
#ifndef RCC_LIB_CODEGEN_REGALLOCINTERNAL_H
#define RCC_LIB_CODEGEN_REGALLOCINTERNAL_H

#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineFunctionCloner.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineLiveness.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/MachineVerifier.h"
#include "rcc/CodeGen/RegAllocCommon.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/CodeGen/TargetRegisterAllocationInfo.h"
#include "rcc/CodeGen/TargetRegisterInfo.h"
#include "rcc/CodeGen/VirtRegMap.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rcc::regalloc_detail {

struct AllocationRunResult final {
  bool Changed = false;
  bool ChangedInstructions = false;
  bool ChangedFrame = false;
};

[[nodiscard]] inline Error withPrefix(const Error &Failure, std::string_view Prefix) {
  std::string Message(Prefix);
  Message.append(Failure.message());
  return Error(Failure.code(), std::move(Message));
}

[[nodiscard]] inline Expected<Register> virtualRegisterAt(std::size_t Index) {
  auto Reg = Register::makeVirtual(Index);
  if (!Reg)
    return std::unexpected(Reg.error());
  return *Reg;
}

[[nodiscard]] inline Expected<RegisterClassID> registerClassFor(const MachineFunction &Function,
                                                                Register VirtualReg) {
  auto Info = Function.registerInfo().virtualRegisterInfo(VirtualReg);
  if (!Info)
    return std::unexpected(Info.error());
  const auto ClassID = Info->get().RegClass;
  if (!ClassID) {
    return makeUnexpected(ErrorCode::RegisterAllocationFailed,
                          "virtual register has no register class after instruction selection");
  }
  return ClassID.value();
}

[[nodiscard]] inline bool isUnavailablePhysicalRegister(const MachineFunction &Function,
                                                        const TargetRegisterInfo &Registers,
                                                        PhysicalRegister Reg) noexcept {
  const PhysicalRegisterDesc *Description = Registers.physicalRegister(Reg);
  return Description == nullptr || Description->IsConstant || Description->IsArtificial ||
         Description->IsReserved || Registers.isReservedRegister(Reg) ||
         Function.registerInfo().isReserved(Reg);
}

[[nodiscard]] inline bool candidateSupportsSubRegisters(const MachineFunction &Function,
                                                        const TargetRegisterInfo &Registers,
                                                        Register VirtualReg,
                                                        PhysicalRegister Candidate) noexcept {
  for (const MachineOperand &Operand :
       const_cast<MachineRegisterInfo &>(Function.registerInfo()).regOperands(VirtualReg)) {
    if (Operand.subReg().value() != 0 && !Registers.subRegister(Candidate, Operand.subReg()))
      return false;
  }
  return true;
}

[[nodiscard]] inline Expected<std::vector<PhysicalRegister>>
allocationOrder(const MachineFunction &Function, const TargetDescription &Target,
                const TargetRegisterAllocationInfo &TargetInfo, Register VirtualReg) {
  auto ClassID = registerClassFor(Function, VirtualReg);
  if (!ClassID)
    return std::unexpected(ClassID.error());
  const RegisterClassDesc *Class = Target.registers().registerClass(*ClassID);
  if (Class == nullptr || !Class->Allocatable || Class->AllocationOrder.empty()) {
    return makeUnexpected(ErrorCode::RegisterAllocationFailed,
                          "virtual register class has no allocatable physical registers");
  }

  auto Info = Function.registerInfo().virtualRegisterInfo(VirtualReg);
  if (!Info)
    return std::unexpected(Info.error());
  std::vector<PhysicalRegister> Result;
  Result.reserve(Class->AllocationOrder.size());
  for (const PhysicalRegister Hint : Info->get().AllocationHints) {
    if (std::ranges::find(Class->AllocationOrder, Hint) != Class->AllocationOrder.end() &&
        std::ranges::find(Result, Hint) == Result.end())
      Result.push_back(Hint);
  }
  for (const PhysicalRegister Candidate : Class->AllocationOrder) {
    if (std::ranges::find(Result, Candidate) == Result.end())
      Result.push_back(Candidate);
  }
  std::erase_if(Result, [&](PhysicalRegister Candidate) {
    return isUnavailablePhysicalRegister(Function, Target.registers(), Candidate) ||
           TargetInfo.isReservedForFunction(Function, Candidate, Target) ||
           !candidateSupportsSubRegisters(Function, Target.registers(), VirtualReg, Candidate);
  });
  return Result;
}

[[nodiscard]] inline std::vector<PhysicalRegister>
functionReservedRegisters(const MachineFunction &Function, const TargetDescription &Target,
                          const TargetRegisterAllocationInfo &TargetInfo) {
  std::vector<PhysicalRegister> Roots;
  for (const PhysicalRegisterDesc &Description : Target.registers().physicalRegisters()) {
    if (TargetInfo.isReservedForFunction(Function, Description.Reg, Target))
      Roots.push_back(Description.Reg);
  }
  std::vector<PhysicalRegister> Result;
  for (const PhysicalRegisterDesc &Description : Target.registers().physicalRegisters()) {
    if (std::ranges::any_of(Roots, [&](PhysicalRegister Root) {
          return Target.registers().regsOverlap(Description.Reg, Root);
        })) {
      Result.push_back(Description.Reg);
    }
  }
  return Result;
}

[[nodiscard]] inline Expected<bool>
reserveFunctionRegisters(MachineFunction &Function, std::span<const PhysicalRegister> Registers) {
  bool Changed = false;
  for (const PhysicalRegister Reg : Registers) {
    if (Function.registerInfo().isReserved(Reg))
      continue;
    auto Reserved = Function.registerInfo().reservePhysicalRegister(Reg);
    if (!Reserved)
      return std::unexpected(Reserved.error());
    Changed = true;
  }
  return Changed;
}

[[nodiscard]] inline MachineInstr *firstTerminator(MachineBasicBlock &Block,
                                                   const TargetDescription &Target) noexcept {
  for (MachineInstr *Instruction = Block.firstInstruction(); Instruction != nullptr;
       Instruction = Instruction->next()) {
    const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction->opcode());
    if (Descriptor != nullptr && Descriptor->Flags.has(InstructionFlag::Terminator))
      return Instruction;
  }
  return nullptr;
}

[[nodiscard]] inline bool hasTerminatorDefinition(const MachineFunction &Function,
                                                  const TargetDescription &Target,
                                                  Register VirtualReg) noexcept {
  for (const MachineOperand &Operand :
       const_cast<MachineRegisterInfo &>(Function.registerInfo()).defOperands(VirtualReg)) {
    const MachineInstr *Instruction = Operand.parent();
    const InstructionDescriptor *Descriptor =
        Instruction == nullptr ? nullptr : Target.instructionDescriptor(Instruction->opcode());
    if (Descriptor != nullptr && Descriptor->Flags.has(InstructionFlag::Terminator))
      return true;
  }
  return false;
}

[[nodiscard]] inline std::uint64_t allocationFingerprint(const MachineFunction &Function) noexcept {
  std::uint64_t Hash = 1469598103934665603ULL;
  auto Mix = [&Hash](std::uint64_t Value) {
    Hash ^= Value;
    Hash *= 1099511628211ULL;
  };
  for (const MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
       Block = Block->next()) {
    Mix(Block->stableID().value());
    for (const MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      Mix(Instruction->opcode().value());
      Mix(Instruction->stableID().value());
      for (const MachineOperand &Operand : Instruction->operands()) {
        Mix(static_cast<std::uint64_t>(Operand.kind()));
        if (const Register *Reg = Operand.getIf<Register>())
          Mix(Reg->opaqueValue());
      }
    }
  }
  return Hash;
}

[[nodiscard]] inline Expected<void> initializeMapping(MachineFunction &Function,
                                                      VirtRegMap &Mapping) {
  for (std::size_t Index = 0; Index < Function.registerInfo().virtualRegisters().size(); ++Index) {
    auto Reg = virtualRegisterAt(Index);
    if (!Reg)
      return std::unexpected(Reg.error());
    if (!Function.registerInfo().isKnownVirtualRegister(*Reg))
      continue;
    auto Added = Mapping.add(*Reg);
    if (!Added)
      return std::unexpected(Added.error());
  }
  return {};
}

[[nodiscard]] inline MachinePassResult passResult(const AllocationRunResult &Run) {
  MachinePassResult Result;
  Result.Changed = Run.Changed;
  if (Run.Changed) {
    Result.Changes.add(MachinePassChange::Registers);
    Result.Changes.add(MachinePassChange::Operands);
  }
  if (Run.ChangedInstructions)
    Result.Changes.add(MachinePassChange::Instructions);
  if (Run.ChangedFrame)
    Result.Changes.add(MachinePassChange::Frame);
  Result.Preserved = PreservedAnalyses::none();
  return Result;
}

[[nodiscard]] inline Expected<void>
requireAllocationProperties(const MachineFunction &Function, const MachinePassContract &Contract) {
  const MachineFunctionProperties Missing = Contract.Requires.missingFrom(Function.properties());
  if (!Missing.empty()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          Contract.Name + " requires properties: " + Missing.toString());
  }
  return {};
}

[[nodiscard]] inline Expected<void> verifyAllocationTrial(MachineFunction &Function,
                                                          const TargetDescription &Target,
                                                          const MachinePassContract &Contract) {
  auto Transition =
      Function.properties().applyTransition(Contract.Requires, Contract.Sets, Contract.Clears);
  if (!Transition)
    return std::unexpected(Transition.error());
  return MachineVerifier::verify(Function, Target,
                                 MachineVerifierOptions{.Mode = MachineVerifierMode::Generic,
                                                        .Stage = Contract.VerificationStage,
                                                        .VerifyProperties = true});
}

[[nodiscard]] inline Expected<MachineFunctionCloneResult>
cloneForAllocation(const MachineFunction &Function, const TargetDescription &Target) {
  return cloneMachineFunction(
      Function, Target,
      MachineFunctionCloneOptions{.NewStableID = Function.stableID(), .NewName = std::nullopt});
}

[[nodiscard]] bool
intervalsOverlapOnPhysicalRegisters(const LiveInterval &Left, PhysicalRegister LeftReg,
                                    const LiveInterval &Right, PhysicalRegister RightReg,
                                    const TargetRegisterInfo &Registers) noexcept;

} // namespace rcc::regalloc_detail

#endif // RCC_LIB_CODEGEN_REGALLOCINTERNAL_H
