// SPDX-License-Identifier: GPL-3.0-or-later

#include "TargetDescriptionGenerator.h"

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
  return std::string(R"(schema: rcc-target-description-v1
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
  if (!First.succeeded() || !Second.succeeded())
    return;

  Context.expect(First.Output->RegisterHeader == Second.Output->RegisterHeader &&
                     First.Output->TargetTables == Second.Output->TargetTables,
                 "target description generation must be deterministic");
  Context.expect(
      First.Output->RegisterHeader.find("enum class PhysicalRegisterID") != std::string::npos &&
          First.Output->RegisterHeader.find("R1 = 1") != std::string::npos &&
          First.Output->TargetTables.find("appendGeneratedRegisterInfo") != std::string::npos &&
          First.Output->TargetTables.find("makePreservedMask") != std::string::npos,
      "generated output must contain typed register IDs and static target tables");
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

  const std::string UnknownUnit = R"(schema: rcc-target-description-v1
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
  - { id: 1, symbol: R1, name: r1, encoding: 0, dwarf_register: 0, units: [{ unit: U_MISSING, lanes: 1 }] }
register_banks:
  - { id: 1, symbol: GPR, name: GPR, maximum_size_bits: 64 }
register_classes:
  - { id: 1, symbol: GPR64, name: GPR64, bank: GPR, size_bits: 64, spill_size_bits: 64, spill_alignment_bits: 64, lanes: 1, registers: [R1], allocation_order: [R1] }
register_masks: []
)";
  const auto BadUnit = rcc::targetgen::generateDescription("bad-unit.rccdesc", UnknownUnit);
  Context.expect(!BadUnit.succeeded() && !BadUnit.Diagnostics.empty() &&
                     BadUnit.Diagnostics.front().Message.find("declared units") !=
                         std::string::npos,
                 "a register unit reference must resolve during semantic validation");
}

} // namespace

int main() {
  TestContext Context;
  testGeneration(Context);
  testSemanticRejection(Context);
  return Context.result();
}
