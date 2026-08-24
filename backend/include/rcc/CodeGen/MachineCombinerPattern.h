// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINECOMBINERPATTERN_H
#define RCC_CODEGEN_MACHINECOMBINERPATTERN_H

#include "rcc/CodeGen/MachineIRTypes.h"
#include "rcc/Support/Error.h"

#include <cstddef>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace rcc {

struct MachineCombinerPattern final {
  std::string Name;
  Opcode RootOpcode;
  Opcode ProducerOpcode;
  Opcode ReplacementOpcode;
  std::size_t ProducerOperand = 0;
  std::size_t AddendOperand = 0;
};

class TargetMachineCombinerInfo final {
public:
  [[nodiscard]] static Expected<TargetMachineCombinerInfo>
  create(std::vector<MachineCombinerPattern> Patterns);

  [[nodiscard]] std::span<const MachineCombinerPattern> patterns(Opcode RootOpcode) const noexcept;

private:
  explicit TargetMachineCombinerInfo(std::vector<MachineCombinerPattern> Patterns)
      : Patterns(std::move(Patterns)) {}

  std::vector<MachineCombinerPattern> Patterns;
  std::unordered_map<Opcode, std::vector<MachineCombinerPattern>> MatchTable;
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINECOMBINERPATTERN_H
