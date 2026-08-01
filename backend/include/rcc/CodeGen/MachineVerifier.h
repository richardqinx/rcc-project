// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINEVERIFIER_H
#define RCC_CODEGEN_MACHINEVERIFIER_H

#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/Support/Error.h"

#include <cstdint>
#include <optional>

namespace rcc {

class MachineFunction;

enum class MachineVerifierMode : std::uint8_t {
  Generic,
  PreLegalizer,
};

struct MachineVerifierOptions final {
  MachineVerifierMode Mode = MachineVerifierMode::Generic;
  std::optional<PipelineStage> Stage;
  bool VerifyProperties = true;
};

class MachineVerifier final {
public:
  [[nodiscard]] static Expected<void> verify(const MachineFunction &Function,
                                             const InstructionSet &Instructions,
                                             MachineVerifierOptions Options = {});
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINEVERIFIER_H
