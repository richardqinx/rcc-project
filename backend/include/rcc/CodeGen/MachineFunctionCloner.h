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

class InstructionSet;
class MachineFunction;

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

[[nodiscard]] Expected<MachineFunctionCloneResult>
cloneMachineFunction(const MachineFunction &Source, const InstructionSet &Instructions,
                     MachineFunctionCloneOptions Options);

} // namespace rcc

#endif // RCC_CODEGEN_MACHINEFUNCTIONCLONER_H
