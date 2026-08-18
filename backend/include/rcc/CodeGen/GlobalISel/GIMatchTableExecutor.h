// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_GLOBALISEL_GIMATCHTABLEEXECUTOR_H
#define RCC_CODEGEN_GLOBALISEL_GIMATCHTABLEEXECUTOR_H

#include "rcc/CodeGen/MachineIRTypes.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/Support/Error.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace rcc {

class MachineFunction;
class MachineInstr;
class TargetDescription;

enum class GIMatchTypeKind : std::uint8_t {
  Any,
  Scalar,
  Pointer,
};

enum class GIMatchImmediateTransform : std::uint8_t {
  Identity,
  Negate,
  PlusOne,
};

enum class GIMatchTableOpcode : std::uint8_t {
  CheckOpcode,
  CheckOperandType,
  CheckOperandBank,
  CheckOperandClass,
  CheckIntegerPredicate,
  CheckFloatingPredicate,
  CheckTargetPredicate,
  CheckOperandImmediateRanges,
  CheckOperandDefConstantRanges,
  CheckOperandDefOpcode,
  CheckOperandDefIntegerPredicate,
  CheckOperandDefOperandConstantRanges,
  CaptureDefOperand,
  CreateTemporary,
  MutateOpcode,
  BuildInstruction,
  RenderOperand,
  RenderComplexOperand,
  RenderCapturedOperand,
  RenderPhysicalRegister,
  RenderTemporary,
  RenderCapturedImmediate,
  RenderImmediate,
  EraseOperandDefinition,
  Complete,
};

struct GIMatchTableInstruction final {
  GIMatchTableOpcode Opcode = GIMatchTableOpcode::Complete;
  std::uint16_t OperandIndex = 0;
  std::uint32_t Value = 0;
  std::uint32_t Auxiliary = 0;
  std::int64_t Immediate = 0;
  std::string_view Symbol;
};

struct GIMatchComplexInstruction final {
  Opcode ResultOpcode;
  LowLevelType ResultType;
  std::vector<MachineOperand> Operands;
};

struct GIMatchComplexOperand final {
  std::optional<MachineOperand> Operand;
  std::optional<GIMatchComplexInstruction> Instruction;
};

struct GIMatchTableConstantRange final {
  std::int64_t Minimum = 0;
  std::int64_t Maximum = 0;
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
  std::span<const GIMatchTableConstantRange> ConstantRanges;
};

class GIMatchTableExecutor {
public:
  virtual ~GIMatchTableExecutor() = default;

  [[nodiscard]] Expected<bool> match(const GIMatchTable &Table, const MachineInstr &Instruction,
                                     const MachineFunction &Function,
                                     const TargetDescription &Target) const;

  // A false result means that no table rule matched. The executor completes all
  // predicates before applying the selected rule to MachineIR.
  [[nodiscard]] Expected<bool> select(const GIMatchTable &Table, MachineInstr &Instruction,
                                      MachineFunction &Function,
                                      const TargetDescription &Target) const;

protected:
  [[nodiscard]] virtual Expected<bool>
  matchesTargetPredicate(std::string_view Name, const MachineInstr &Instruction,
                         const MachineFunction &Function, const TargetDescription &Target) const;

  [[nodiscard]] virtual Expected<std::optional<GIMatchComplexOperand>>
  renderComplexOperand(std::string_view Name, const MachineInstr &Instruction,
                       const MachineFunction &Function, const TargetDescription &Target,
                       std::uint32_t OperandIndex) const;

  [[nodiscard]] virtual Expected<void>
  finalizeGeneratedInstruction(MachineInstr &Output, MachineFunction &Function,
                               const TargetDescription &Target, MachineInstrFlags InputFlags,
                               bool InputMayRaiseFPException) const;
};

} // namespace rcc

#endif // RCC_CODEGEN_GLOBALISEL_GIMATCHTABLEEXECUTOR_H
