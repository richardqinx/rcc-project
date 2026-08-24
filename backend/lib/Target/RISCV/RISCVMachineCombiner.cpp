// SPDX-License-Identifier: GPL-3.0-or-later

#include "RISCV.h"
#include "rcc/Target/RISCV/RISCVTarget.h"

#include <cstdlib>
#include <vector>

namespace rcc::riscv {
namespace {

[[nodiscard]] MachineCombinerPattern
fusedMultiplyAdd(const char *Name, InstructionOpcode RootOpcode, InstructionOpcode ProducerOpcode,
                 InstructionOpcode ReplacementOpcode, std::size_t ProducerOperand,
                 std::size_t AddendOperand) {
  return {
      .Name = Name,
      .RootOpcode = instructionOpcode(RootOpcode),
      .ProducerOpcode = instructionOpcode(ProducerOpcode),
      .ReplacementOpcode = instructionOpcode(ReplacementOpcode),
      .ProducerOperand = ProducerOperand,
      .AddendOperand = AddendOperand,
  };
}

[[nodiscard]] std::vector<MachineCombinerPattern> createPatterns() {
  return {
      fusedMultiplyAdd("fmadd-s-ax", InstructionOpcode::FADD_S, InstructionOpcode::FMUL_S,
                       InstructionOpcode::FMADD_S, 1, 2),
      fusedMultiplyAdd("fmadd-s-xa", InstructionOpcode::FADD_S, InstructionOpcode::FMUL_S,
                       InstructionOpcode::FMADD_S, 2, 1),
      fusedMultiplyAdd("fmsub-s", InstructionOpcode::FSUB_S, InstructionOpcode::FMUL_S,
                       InstructionOpcode::FMSUB_S, 1, 2),
      fusedMultiplyAdd("fnmsub-s", InstructionOpcode::FSUB_S, InstructionOpcode::FMUL_S,
                       InstructionOpcode::FNMSUB_S, 2, 1),
      fusedMultiplyAdd("fmadd-d-ax", InstructionOpcode::FADD_D, InstructionOpcode::FMUL_D,
                       InstructionOpcode::FMADD_D, 1, 2),
      fusedMultiplyAdd("fmadd-d-xa", InstructionOpcode::FADD_D, InstructionOpcode::FMUL_D,
                       InstructionOpcode::FMADD_D, 2, 1),
      fusedMultiplyAdd("fmsub-d", InstructionOpcode::FSUB_D, InstructionOpcode::FMUL_D,
                       InstructionOpcode::FMSUB_D, 1, 2),
      fusedMultiplyAdd("fnmsub-d", InstructionOpcode::FSUB_D, InstructionOpcode::FMUL_D,
                       InstructionOpcode::FNMSUB_D, 2, 1),
  };
}

} // namespace

const TargetMachineCombinerInfo &machineCombinerInfo() noexcept {
  static const Expected<TargetMachineCombinerInfo> Info =
      TargetMachineCombinerInfo::create(createPatterns());
  if (!Info)
    std::abort();
  return *Info;
}

} // namespace rcc::riscv
