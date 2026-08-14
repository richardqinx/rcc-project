// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_GLOBALISEL_CSEMIRBUILDER_H
#define RCC_CODEGEN_GLOBALISEL_CSEMIRBUILDER_H

#include "rcc/CodeGen/MachineIRBuilder.h"

#include <cstdint>
#include <span>

namespace rcc {

class CSEMIRBuilder final : public MachineIRBuilder {
public:
  using MachineIRBuilder::buildConstant;
  using MachineIRBuilder::buildInstr;
  using MachineIRBuilder::MachineIRBuilder;

  [[nodiscard]] Expected<MachineInstr *>
  buildInstr(Opcode InstructionOpcode, std::span<const MachineIRBuilderResult> Destinations,
             std::span<const Register> Sources) const override;
  [[nodiscard]] Expected<MachineInstr *> buildConstant(MachineIRBuilderResult Destination,
                                                       APInt Value) const override;
  [[nodiscard]] Expected<MachineInstr *> buildSExtInReg(MachineIRBuilderResult Destination,
                                                        Register Source,
                                                        std::uint32_t Width) const override;
  [[nodiscard]] Expected<MachineInstr *> buildExtract(MachineIRBuilderResult Destination,
                                                      Register Source,
                                                      std::uint64_t Offset) const override;

private:
  [[nodiscard]] bool canPerformCSE(Opcode InstructionOpcode) const noexcept;
  [[nodiscard]] bool dominatesInsertionPoint(const MachineInstr &Instruction) const noexcept;
  [[nodiscard]] Expected<void> makeInstructionDominate(MachineInstr &Instruction) const;
  [[nodiscard]] Expected<MachineInstr *>
  generateCopyIfRequired(const MachineIRBuilderResult &Destination,
                         MachineInstr &Instruction) const;
  [[nodiscard]] Expected<MachineInstr *> memoize(MachineInstr &Instruction) const;
};

} // namespace rcc

#endif // RCC_CODEGEN_GLOBALISEL_CSEMIRBUILDER_H
