// SPDX-License-Identifier: GPL-3.0-or-later

#include "TargetDescriptionGenerator.h"

#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>

namespace {

class TestContext final {
public:
  void expect(bool Condition, std::string_view Message) {
    if (Condition)
      return;
    std::cerr << Message << '\n';
    ++FailureCount;
  }

  [[nodiscard]] int result() const noexcept {
    return FailureCount == 0 ? 0 : 1;
  }

private:
  int FailureCount = 0;
};

[[nodiscard]] std::string validDescription(std::string_view Aliases = "[acc]") {
  return std::string(R"(schema: rcc-target-description-v2
target:
  namespace: tiny
  name: Tiny
  triple: tiny-unknown-none
  cpu: tiny-cpu
  abi: tiny-abi
  data_layout: e-p:64:64
  features: [base]
register_units:
  - { id: 1, symbol: U_R1, name: r1.unit }
registers:
  - { id: 1, symbol: R1, name: r1, encoding: 0, dwarf_register: 0, units: [{ unit: U_R1, lanes: 1 }], aliases: )") +
         std::string(Aliases) + R"( }
register_banks:
  - { id: 1, symbol: GPR, name: GPR, maximum_size_bits: 64 }
register_classes:
  - { id: 1, symbol: GPR64, name: GPR64, bank: GPR, size_bits: 64, spill_size_bits: 64, spill_alignment_bits: 64, lanes: 1, registers: [R1], allocation_order: [R1] }
register_masks:
  - { name: CSR_TINY, preserved: [R1] }
mir_operand_flags:
  direct_mask: 31
  direct: [{ name: tiny-call, value: 1 }]
  bitmask: [{ name: tiny-relax, value: 32 }]
calling_conventions:
  - { id: 1, symbol: TINY, name: tiny, integer_register_class: GPR64, floating_register_class: GPR64, integer_argument_registers: [R1], floating_argument_registers: [R1], integer_return_registers: [R1], floating_return_registers: [R1], preserved_mask: CSR_TINY, stack_alignment_bytes: 8, stack_argument_alignment_bytes: 8, shadow_space_bytes: 0, supports_varargs: true, supports_tail_calls: true }
named_operands:
  - { id: 1, symbol: RD, name: rd }
  - { id: 2, symbol: IMM, name: imm }
immediate_predicates:
  - { id: 1, symbol: SIMM8, name: simm8, kind: signed, bits: 8 }
scheduling_classes:
  - { id: 1, symbol: ALU, name: alu, micro_ops: 1, latency: 1 }
encoding_categories:
  - { id: 1, symbol: I, name: i, size_bytes: 4 }
assembly_formats:
  - { id: 1, symbol: RD_IMM, name: rd-imm }
instructions:
  - { id: 1024, symbol: LOAD_IMM, name: LOAD_IMM, assembly_mnemonic: load.imm, operands: [{ kinds: [register], role: def, register_class: GPR64, register_bank: GPR, name: RD }, { kinds: [immediate], immediate_predicate: SIMM8, name: IMM, allowed_target_flags: 33 }], num_explicit_defs: 1, flags: [produces_code], scheduling_class: ALU, encoding_category: I, assembly_format: RD_IMM }
)";
}

void testGeneration(TestContext &Context) {
  const auto First = rcc::targetgen::generateDescription("tiny.rccdesc", validDescription());
  const auto Second = rcc::targetgen::generateDescription("tiny.rccdesc", validDescription());
  if (!First.succeeded()) {
    for (const rcc::TextDiagnostic &Diagnostic : First.Diagnostics)
      std::cerr << Diagnostic.Message << '\n';
  }
  Context.expect(First.succeeded() && Second.succeeded(),
                 "a complete restricted target description must generate successfully");
  if (!First.succeeded() || !Second.succeeded() || !First.Output || !Second.Output)
    return;
  const auto &FirstOutput = First.Output.value();
  const auto &SecondOutput = Second.Output.value();

  Context.expect(FirstOutput.RegisterHeader == SecondOutput.RegisterHeader &&
                     FirstOutput.InstructionHeader == SecondOutput.InstructionHeader &&
                     FirstOutput.TargetTables == SecondOutput.TargetTables,
                 "target description generation must be deterministic");
  Context.expect(
      FirstOutput.RegisterHeader.find("enum class PhysicalRegisterID") != std::string::npos &&
          FirstOutput.RegisterHeader.find("R1 = 1") != std::string::npos &&
          FirstOutput.InstructionHeader.find("enum class InstructionOpcode") != std::string::npos &&
          FirstOutput.InstructionHeader.find("LOAD_IMM = 1024") != std::string::npos &&
          FirstOutput.TargetTables.find("appendGeneratedRegisterInfo") != std::string::npos &&
          FirstOutput.TargetTables.find("appendGeneratedInstructionInfo") != std::string::npos &&
          FirstOutput.TargetTables.find("GeneratedAssemblyInstructions") != std::string::npos &&
          FirstOutput.TargetTables.find("Description.DirectOperandFlagMask = 31") !=
              std::string::npos &&
          FirstOutput.TargetTables.find("\"tiny-call\", 1") != std::string::npos &&
          FirstOutput.TargetTables.find("\"load.imm\"") != std::string::npos &&
          FirstOutput.TargetTables.find("makePreservedMask") != std::string::npos,
      "generated output must contain typed target IDs, assembly mnemonics, and static target "
      "tables");
}

void testSemanticRejection(TestContext &Context) {
  const auto CollidingAlias =
      rcc::targetgen::generateDescription("bad-alias.rccdesc", validDescription("[r1]"));
  if (CollidingAlias.succeeded())
    std::cerr << "colliding alias description unexpectedly generated\n";
  Context.expect(!CollidingAlias.succeeded() && !CollidingAlias.Diagnostics.empty() &&
                     CollidingAlias.Diagnostics.front().Message.find("aliases") !=
                         std::string::npos,
                 "an ABI alias that collides with a canonical name must be rejected");

  std::string UnknownUnit = validDescription();
  constexpr std::string_view UnitReference = "unit: U_R1";
  const std::size_t UnitPosition = UnknownUnit.find(UnitReference);
  if (UnitPosition != std::string::npos)
    UnknownUnit.replace(UnitPosition, UnitReference.size(), "unit: U_MISSING");
  const auto BadUnit = rcc::targetgen::generateDescription("bad-unit.rccdesc", UnknownUnit);
  Context.expect(!BadUnit.succeeded() && !BadUnit.Diagnostics.empty() &&
                     BadUnit.Diagnostics.front().Message.find("declared units") !=
                         std::string::npos,
                 "a register unit reference must resolve during semantic validation");

  std::string UnknownClass = validDescription();
  constexpr std::string_view ClassReference = "register_class: GPR64";
  const std::size_t ClassPosition = UnknownClass.find(ClassReference);
  if (ClassPosition != std::string::npos)
    UnknownClass.replace(ClassPosition, ClassReference.size(), "register_class: MISSING");
  const auto BadClass =
      rcc::targetgen::generateDescription("bad-instruction-class.rccdesc", UnknownClass);
  Context.expect(!BadClass.succeeded() && !BadClass.Diagnostics.empty() &&
                     BadClass.Diagnostics.front().Message.find("register class") !=
                         std::string::npos,
                 "instruction register classes must resolve during semantic validation");

  std::string BadAllowedValue = validDescription();
  constexpr std::string_view Predicate = "kind: signed, bits: 8";
  const std::size_t PredicatePosition = BadAllowedValue.find(Predicate);
  if (PredicatePosition != std::string::npos) {
    BadAllowedValue.replace(PredicatePosition, Predicate.size(),
                            "kind: signed, bits: 8, allowed_values: [128]");
  }
  const auto InvalidAllowedValue =
      rcc::targetgen::generateDescription("bad-allowed-value.rccdesc", BadAllowedValue);
  Context.expect(!InvalidAllowedValue.succeeded() && !InvalidAllowedValue.Diagnostics.empty() &&
                     InvalidAllowedValue.Diagnostics.front().Message.find("allowed_values") !=
                         std::string::npos,
                 "immediate predicates must reject allowed values outside their signed range");

  std::string ContradictoryNonZero = validDescription();
  if (PredicatePosition != std::string::npos) {
    const std::size_t Position = ContradictoryNonZero.find(Predicate);
    ContradictoryNonZero.replace(Position, Predicate.size(),
                                 "kind: signed, bits: 8, nonzero: true, allowed_values: [0]");
  }
  const auto InvalidNonZero =
      rcc::targetgen::generateDescription("bad-nonzero.rccdesc", ContradictoryNonZero);
  Context.expect(!InvalidNonZero.succeeded() && !InvalidNonZero.Diagnostics.empty() &&
                     InvalidNonZero.Diagnostics.front().Message.find("allowed_values") !=
                         std::string::npos,
                 "nonzero immediate predicates must reject an explicit zero value");

  std::string BadStackAlignment = validDescription();
  constexpr std::string_view StackAlignment =
      "stack_alignment_bytes: 8, stack_argument_alignment_bytes: 8";
  const std::size_t StackPosition = BadStackAlignment.find(StackAlignment);
  if (StackPosition != std::string::npos) {
    BadStackAlignment.replace(StackPosition, StackAlignment.size(),
                              "stack_alignment_bytes: 8, stack_argument_alignment_bytes: 16");
  }
  const auto InvalidStackAlignment =
      rcc::targetgen::generateDescription("bad-stack-alignment.rccdesc", BadStackAlignment);
  Context.expect(!InvalidStackAlignment.succeeded() && !InvalidStackAlignment.Diagnostics.empty() &&
                     InvalidStackAlignment.Diagnostics.front().Message.find("stack alignments") !=
                         std::string::npos,
                 "calling conventions must reject incompatible stack alignments");

  std::string BadMnemonic = validDescription();
  constexpr std::string_view Mnemonic = "assembly_mnemonic: load.imm";
  const std::size_t MnemonicPosition = BadMnemonic.find(Mnemonic);
  if (MnemonicPosition != std::string::npos)
    BadMnemonic.replace(MnemonicPosition, Mnemonic.size(), "assembly_mnemonic: .load");
  const auto InvalidMnemonic =
      rcc::targetgen::generateDescription("bad-mnemonic.rccdesc", BadMnemonic);
  Context.expect(!InvalidMnemonic.succeeded() && !InvalidMnemonic.Diagnostics.empty() &&
                     InvalidMnemonic.Diagnostics.front().Message.find("assembly mnemonics") !=
                         std::string::npos,
                 "assembly mnemonics must use a canonical lowercase spelling");
}

} // namespace

int main() {
  TestContext Context;
  testGeneration(Context);
  testSemanticRejection(Context);
  return Context.result();
}
