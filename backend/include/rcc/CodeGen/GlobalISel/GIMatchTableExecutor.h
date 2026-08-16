// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_GLOBALISEL_GIMATCHTABLEEXECUTOR_H
#define RCC_CODEGEN_GLOBALISEL_GIMATCHTABLEEXECUTOR_H

#include "rcc/CodeGen/MachineIRTypes.h"
#include "rcc/Support/Error.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace rcc {

class MachineFunction;
class MachineInstr;
class TargetDescription;

enum class GIMatchTypeKind : std::uint8_t {
  Any,
  Scalar,
  Pointer,
};

enum class GIMatchTableOpcode : std::uint8_t {
  CheckOpcode,
  CheckOperandType,
  CheckOperandBank,
  CheckOperandClass,
  CheckIntegerPredicate,
  CheckOperandDefConstant,
  MutateOpcode,
  RenderOperand,
  RenderPhysicalRegister,
  AppendImmediate,
  EraseOperandDefinition,
  Complete,
};

struct GIMatchTableInstruction final {
  GIMatchTableOpcode Opcode = GIMatchTableOpcode::Complete;
  std::uint16_t OperandIndex = 0;
  std::uint32_t Value = 0;
  std::uint32_t Auxiliary = 0;
  std::int64_t Immediate = 0;
};

struct GIMatchTableRule final {
  std::string_view Name;
  std::uint32_t Priority = 0;
  std::uint32_t FirstInstruction = 0;
  std::uint32_t InstructionCount = 0;
};

struct GIMatchTable final {
  std::span<const GIMatchTableInstruction> Instructions;
  std::span<const GIMatchTableRule> Rules;
};

class GIMatchTableExecutor {
public:
  [[nodiscard]] Expected<bool> match(const GIMatchTable &Table, const MachineInstr &Instruction,
                                     const MachineFunction &Function,
                                     const TargetDescription &Target) const;

  // A false result means that no table rule matched. The executor completes all
  // predicates before applying the selected rule to MachineIR.
  [[nodiscard]] Expected<bool> select(const GIMatchTable &Table, MachineInstr &Instruction,
                                      MachineFunction &Function,
                                      const TargetDescription &Target) const;
};

} // namespace rcc

#endif // RCC_CODEGEN_GLOBALISEL_GIMATCHTABLEEXECUTOR_H
