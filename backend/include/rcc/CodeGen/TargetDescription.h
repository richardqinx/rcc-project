// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_TARGETDESCRIPTION_H
#define RCC_CODEGEN_TARGETDESCRIPTION_H

#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/TargetMIRTextInfo.h"
#include "rcc/CodeGen/TargetRegisterInfo.h"
#include "rcc/Support/Error.h"

#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace rcc {

class MachineBasicBlock;
class MachineCFIInfo;
class MachineFrameInfo;
class MachineInstr;
class MachineMemOperand;
class MachineRegisterInfo;
struct VirtualRegisterInfo;

class TargetDescription final {
public:
  TargetDescription(const TargetDescription &) = delete;
  TargetDescription &operator=(const TargetDescription &) = delete;
  TargetDescription(TargetDescription &&) noexcept = default;
  TargetDescription &operator=(TargetDescription &&) noexcept = default;
  ~TargetDescription() = default;

  [[nodiscard]] static Expected<TargetDescription> create(InstructionSet Instructions,
                                                          TargetRegisterInfo Registers,
                                                          TargetMIRTextInfoDesc MIRText = {});

  [[nodiscard]] const InstructionSet &instructions() const noexcept {
    return Instructions;
  }

  [[nodiscard]] const TargetRegisterInfo &registers() const noexcept {
    return Registers;
  }

  [[nodiscard]] const TargetMIRTextInfo &mirText() const noexcept {
    return MIRText;
  }

  [[nodiscard]] const InstructionDescriptor *
  instructionDescriptor(Opcode InstructionOpcode) const noexcept {
    return Instructions.descriptor(InstructionOpcode);
  }

  [[nodiscard]] Expected<void> validateExplicitOperands(Opcode InstructionOpcode,
                                                        std::span<const MachineOperand> Operands,
                                                        const MachineRegisterInfo &RegInfo) const;
  [[nodiscard]] Expected<void> validateOperandTies(Opcode InstructionOpcode,
                                                   std::span<const MachineOperand> ExplicitOperands,
                                                   std::span<const MachineOperandTie> Ties,
                                                   const MachineRegisterInfo &RegInfo) const;
  [[nodiscard]] Expected<void>
  validateImplicitOperands(Opcode InstructionOpcode,
                           std::span<const MachineOperand *const> ImplicitOperands,
                           const MachineRegisterInfo &RegInfo) const;
  [[nodiscard]] Expected<void>
  validateEarlyClobber(std::span<const MachineOperand> ExplicitOperands,
                       std::span<const MachineOperand *const> ImplicitOperands,
                       std::span<const MachineOperandTie> Ties) const;
  [[nodiscard]] Expected<void>
  validateMemOperands(Opcode InstructionOpcode,
                      std::span<const MachineMemOperand *const> MemOperands) const;
  [[nodiscard]] Expected<void> validateInstruction(const MachineInstr &Instruction,
                                                   const MachineRegisterInfo &RegInfo) const;
  [[nodiscard]] Expected<void> validateCFIInfo(const MachineCFIInfo &Info) const;
  [[nodiscard]] Expected<void> validateFrameInfo(const MachineFrameInfo &Info) const;
  [[nodiscard]] Expected<void> validateRegisterInfo(const MachineRegisterInfo &RegInfo) const;
  [[nodiscard]] Expected<void> validateBlockLiveIns(const MachineBasicBlock &Block) const;

  [[nodiscard]] Expected<std::vector<MachineOperand>>
  createDefaultImplicitOperands(const InstructionDescriptor &Descriptor) const {
    return Instructions.createDefaultImplicitOperands(Descriptor);
  }

private:
  TargetDescription(InstructionSet Instructions, TargetRegisterInfo Registers,
                    TargetMIRTextInfo MIRText)
      : Instructions(std::move(Instructions)), Registers(std::move(Registers)),
        MIRText(std::move(MIRText)) {}

  [[nodiscard]] Expected<void> validateTargetOperand(const OperandDescriptor *Descriptor,
                                                     const MachineOperand &Operand,
                                                     const MachineRegisterInfo &RegInfo) const;
  [[nodiscard]] Expected<void> validateVirtualRegister(Register Reg,
                                                       const VirtualRegisterInfo &Info) const;
  [[nodiscard]] std::optional<RegisterClassID>
  effectiveRegisterClass(const MachineOperand &Operand, const MachineRegisterInfo &RegInfo) const;
  [[nodiscard]] Expected<void> validateTieCompatibility(const MachineOperand &Def,
                                                        const MachineOperand &Use,
                                                        const MachineRegisterInfo &RegInfo) const;

  InstructionSet Instructions;
  TargetRegisterInfo Registers;
  TargetMIRTextInfo MIRText;
};

} // namespace rcc

#endif // RCC_CODEGEN_TARGETDESCRIPTION_H
