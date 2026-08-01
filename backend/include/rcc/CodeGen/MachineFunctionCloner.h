// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINEFUNCTIONCLONER_H
#define RCC_CODEGEN_MACHINEFUNCTIONCLONER_H

#include "rcc/CodeGen/MachineIRTypes.h"
#include "rcc/CodeGen/Register.h"
#include "rcc/CodeGen/StableID.h"
#include "rcc/Support/Error.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace rcc {

class MachineBasicBlock;
class MachineFunction;
class MachineInstr;
class TargetDescription;

struct MachineFunctionCloneOptions final {
  MachineFunctionStableID NewStableID;
  std::optional<std::string> NewName;
};

struct MachineFunctionCloneResult final {
  MachineFunctionCloneResult();
  MachineFunctionCloneResult(const MachineFunctionCloneResult &) = delete;
  MachineFunctionCloneResult &operator=(const MachineFunctionCloneResult &) = delete;
  MachineFunctionCloneResult(MachineFunctionCloneResult &&) noexcept;
  MachineFunctionCloneResult &operator=(MachineFunctionCloneResult &&) noexcept;
  ~MachineFunctionCloneResult();

  std::unique_ptr<MachineFunction> ClonedFunction;
  std::unordered_map<MachineBlockStableID, MachineBlockStableID> BlockStableIDs;
  std::unordered_map<MachineInstrStableID, MachineInstrStableID> InstructionStableIDs;
  std::unordered_map<Register, Register> VirtualRegisters;
  std::unordered_map<FrameIndex, FrameIndex> FrameIndices;
  std::unordered_map<ConstantPoolIndex, ConstantPoolIndex> ConstantPoolIndices;
  std::unordered_map<JumpTableIndex, JumpTableIndex> JumpTableIndices;
};

class MachineFunctionCloneContext final {
public:
  [[nodiscard]] const MachineFunction &sourceFunction() const noexcept {
    return Source;
  }

  [[nodiscard]] MachineFunction &destinationFunction() const noexcept {
    return Destination;
  }

  [[nodiscard]] Expected<MachineBasicBlock *> mapBlock(const MachineBasicBlock &SourceBlock) const;
  [[nodiscard]] Expected<MachineInstr *>
  mapInstruction(const MachineInstr &SourceInstruction) const;
  [[nodiscard]] Expected<MachineBlockStableID>
  mapBlockStableID(MachineBlockStableID SourceID) const;
  [[nodiscard]] Expected<MachineInstrStableID>
  mapInstructionStableID(MachineInstrStableID SourceID) const;
  [[nodiscard]] Expected<Register> mapRegister(Register SourceRegister) const;
  [[nodiscard]] Expected<FrameIndex> mapFrameIndex(FrameIndex SourceIndex) const;
  [[nodiscard]] Expected<ConstantPoolIndex>
  mapConstantPoolIndex(ConstantPoolIndex SourceIndex) const;
  [[nodiscard]] Expected<JumpTableIndex> mapJumpTableIndex(JumpTableIndex SourceIndex) const;

private:
  MachineFunctionCloneContext(
      const MachineFunction &Source, MachineFunction &Destination,
      const MachineFunctionCloneResult &Result,
      const std::unordered_map<const MachineBasicBlock *, MachineBasicBlock *> &Blocks,
      const std::unordered_map<const MachineInstr *, MachineInstr *> &Instructions) noexcept
      : Source(Source), Destination(Destination), Result(Result), Blocks(Blocks),
        Instructions(Instructions) {}

  const MachineFunction &Source;
  MachineFunction &Destination;
  const MachineFunctionCloneResult &Result;
  const std::unordered_map<const MachineBasicBlock *, MachineBasicBlock *> &Blocks;
  const std::unordered_map<const MachineInstr *, MachineInstr *> &Instructions;

  friend Expected<MachineFunctionCloneResult>
  cloneMachineFunction(const MachineFunction &Source, const TargetDescription &Target,
                       MachineFunctionCloneOptions Options);
};

[[nodiscard]] Expected<MachineFunctionCloneResult>
cloneMachineFunction(const MachineFunction &Source, const TargetDescription &Target,
                     MachineFunctionCloneOptions Options);

} // namespace rcc

#endif // RCC_CODEGEN_MACHINEFUNCTIONCLONER_H
