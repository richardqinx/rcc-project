// SPDX-License-Identifier: GPL-3.0-or-later

#include "RISCV.h"
#include "rcc/Target/RISCV/RISCVTarget.h"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace rcc::riscv {
namespace {

[[nodiscard]] constexpr MachineCombinerStageMask allOptimizationStages() noexcept {
  return machineCombinerStageMask(MachineCombinerStage::SelectedSSA) |
         machineCombinerStageMask(MachineCombinerStage::PreRegisterAllocation) |
         machineCombinerStageMask(MachineCombinerStage::PostRegisterAllocation) |
         machineCombinerStageMask(MachineCombinerStage::PostFrame);
}

[[nodiscard]] MachineCombinerPattern immediateIdentity(InstructionOpcode OpcodeValue,
                                                       std::string Name, std::int64_t Immediate) {
  return {
      .Name = std::move(Name),
      .RootOpcode = instructionOpcode(OpcodeValue),
      .Stages = allOptimizationStages(),
      .Predicates = {{MachineCombinerPredicateKind::ImmediateEquals, 2, 0, Immediate}},
      .Action = {MachineCombinerActionKind::ReplaceDefWithOperandAndErase, 0, 1},
  };
}

[[nodiscard]] MachineCombinerPattern zeroRegisterIdentity(InstructionOpcode OpcodeValue,
                                                          std::string Name, std::size_t ZeroOperand,
                                                          std::size_t SourceOperand) {
  return {
      .Name = std::move(Name),
      .RootOpcode = instructionOpcode(OpcodeValue),
      .Stages = allOptimizationStages(),
      .Predicates = {{MachineCombinerPredicateKind::PhysicalRegisterEquals, ZeroOperand, 0,
                      static_cast<std::int64_t>(PhysicalRegisterID::X0)}},
      .Action = {MachineCombinerActionKind::ReplaceDefWithOperandAndErase, 0, SourceOperand},
  };
}

[[nodiscard]] MachineCombinerPattern equalSourceIdentity(InstructionOpcode OpcodeValue,
                                                         std::string Name) {
  return {
      .Name = std::move(Name),
      .RootOpcode = instructionOpcode(OpcodeValue),
      .Stages = allOptimizationStages(),
      .Predicates = {{MachineCombinerPredicateKind::RegisterEquals, 1, 2, 0}},
      .Action = {MachineCombinerActionKind::ReplaceDefWithOperandAndErase, 0, 1},
  };
}

[[nodiscard]] std::vector<MachineCombinerPattern> createPatterns() {
  std::vector<MachineCombinerPattern> Patterns;
  Patterns.push_back(immediateIdentity(InstructionOpcode::ADDI, "addi-zero", 0));
  Patterns.push_back(immediateIdentity(InstructionOpcode::XORI, "xori-zero", 0));
  Patterns.push_back(immediateIdentity(InstructionOpcode::ORI, "ori-zero", 0));
  Patterns.push_back(immediateIdentity(InstructionOpcode::ANDI, "andi-all-ones", -1));
  Patterns.push_back(immediateIdentity(InstructionOpcode::SLLI, "slli-zero", 0));
  Patterns.push_back(immediateIdentity(InstructionOpcode::SRLI, "srli-zero", 0));
  Patterns.push_back(immediateIdentity(InstructionOpcode::SRAI, "srai-zero", 0));
  Patterns.push_back(zeroRegisterIdentity(InstructionOpcode::ADD, "add-zero-right", 2, 1));
  Patterns.push_back(zeroRegisterIdentity(InstructionOpcode::ADD, "add-zero-left", 1, 2));
  Patterns.push_back(zeroRegisterIdentity(InstructionOpcode::OR, "or-zero-right", 2, 1));
  Patterns.push_back(zeroRegisterIdentity(InstructionOpcode::OR, "or-zero-left", 1, 2));
  Patterns.push_back(zeroRegisterIdentity(InstructionOpcode::XOR, "xor-zero-right", 2, 1));
  Patterns.push_back(zeroRegisterIdentity(InstructionOpcode::XOR, "xor-zero-left", 1, 2));
  Patterns.push_back(zeroRegisterIdentity(InstructionOpcode::SUB, "sub-zero-right", 2, 1));
  Patterns.push_back(equalSourceIdentity(InstructionOpcode::FSGNJ_S, "fsgnj-s-copy"));
  Patterns.push_back(equalSourceIdentity(InstructionOpcode::FSGNJ_D, "fsgnj-d-copy"));
  return Patterns;
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
