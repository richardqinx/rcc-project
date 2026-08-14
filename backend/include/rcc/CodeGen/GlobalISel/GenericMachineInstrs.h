// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_GLOBALISEL_GENERICMACHINEINSTRS_H
#define RCC_CODEGEN_GLOBALISEL_GENERICMACHINEINSTRS_H

#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/Register.h"

#include <cassert>
#include <cstddef>

namespace rcc {

class GenericMachineInstr : public MachineInstr {
public:
  GenericMachineInstr() = delete;

  [[nodiscard]] Register getReg(std::size_t Index) const {
    const MachineOperand *Operand = operandAt(Index);
    assert(Operand != nullptr && Operand->isRegister());
    const Register *Reg = Operand->getIf<Register>();
    assert(Reg != nullptr);
    return *Reg;
  }

  [[nodiscard]] static bool classof(const MachineInstr *Instruction) noexcept {
    return Instruction != nullptr && isGenericOpcode(Instruction->opcode());
  }
};

class GUnmerge : public GenericMachineInstr {
public:
  [[nodiscard]] std::size_t getNumDefs() const noexcept {
    return operandCount() - 1;
  }

  [[nodiscard]] Register getSourceReg() const {
    return getReg(getNumDefs());
  }

  [[nodiscard]] static bool classof(const MachineInstr *Instruction) noexcept {
    return Instruction != nullptr &&
           Instruction->opcode() == rcc::opcode(GenericOpcode::G_UNMERGE_VALUES);
  }
};

class GMergeLikeInstr : public GenericMachineInstr {
public:
  [[nodiscard]] std::size_t getNumSources() const noexcept {
    return operandCount() - 1;
  }

  [[nodiscard]] Register getSourceReg(std::size_t Index) const {
    return getReg(Index + 1);
  }

  [[nodiscard]] static bool classof(const MachineInstr *Instruction) noexcept {
    if (Instruction == nullptr)
      return false;
    switch (static_cast<GenericOpcode>(Instruction->opcode().value())) {
    case GenericOpcode::G_MERGE_VALUES:
    case GenericOpcode::G_CONCAT_VECTORS:
    case GenericOpcode::G_BUILD_VECTOR:
      return true;
    default:
      return false;
    }
  }
};

class GMerge : public GMergeLikeInstr {
public:
  [[nodiscard]] static bool classof(const MachineInstr *Instruction) noexcept {
    return Instruction != nullptr &&
           Instruction->opcode() == rcc::opcode(GenericOpcode::G_MERGE_VALUES);
  }
};

class GConcatVectors : public GMergeLikeInstr {
public:
  [[nodiscard]] static bool classof(const MachineInstr *Instruction) noexcept {
    return Instruction != nullptr &&
           Instruction->opcode() == rcc::opcode(GenericOpcode::G_CONCAT_VECTORS);
  }
};

class GBuildVector : public GMergeLikeInstr {
public:
  [[nodiscard]] static bool classof(const MachineInstr *Instruction) noexcept {
    return Instruction != nullptr &&
           Instruction->opcode() == rcc::opcode(GenericOpcode::G_BUILD_VECTOR);
  }
};

template <typename InstructionT>
[[nodiscard]] InstructionT &castGenericMachineInstr(MachineInstr &Instruction) {
  assert(InstructionT::classof(&Instruction));
  return static_cast<InstructionT &>(Instruction);
}

template <typename InstructionT>
[[nodiscard]] const InstructionT &castGenericMachineInstr(const MachineInstr &Instruction) {
  assert(InstructionT::classof(&Instruction));
  return static_cast<const InstructionT &>(Instruction);
}

} // namespace rcc

#endif // RCC_CODEGEN_GLOBALISEL_GENERICMACHINEINSTRS_H
