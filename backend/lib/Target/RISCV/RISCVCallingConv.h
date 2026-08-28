// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_LIB_TARGET_RISCV_RISCVCALLINGCONV_H
#define RCC_LIB_TARGET_RISCV_RISCVCALLINGCONV_H

#include "rcc/CodeGen/TargetCallLowering.h"
#include "rcc/Target/RISCV/RISCVInstructions.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace rcc::riscv {

struct CallingConventionInfo;

inline constexpr std::uint64_t XLenBytes = 8;
inline constexpr std::uint64_t ABIStackAlignmentBytes = 16;

[[nodiscard]] constexpr CallingConventionID
callingConventionID(CallingConvention Convention) noexcept {
  return CallingConventionID::fromValue(static_cast<std::uint32_t>(Convention));
}

struct ArgumentAssignmentResult final {
  std::vector<CCAssignment> Assignments;
  StackArgumentArea StackArea;
  std::size_t FirstUnallocatedGPR = 0;
};

[[nodiscard]] Expected<std::size_t> verifyValueSequence(std::span<const ABIValue> Values,
                                                        bool RequireRegisters);
[[nodiscard]] Expected<ArgumentAssignmentResult>
assignArguments(std::span<const ABIValue> Values, std::size_t FixedArgumentCount, bool IsVariadic,
                const CallingConventionInfo &Convention);
[[nodiscard]] Expected<std::vector<CCAssignment>>
assignReturns(std::span<const ABIValue> Values, bool RequireRegisters,
              const CallingConventionInfo &Convention);
[[nodiscard]] bool hasStructReturn(std::span<const ABIValue> Arguments) noexcept;
[[nodiscard]] Expected<std::string>
tailCallRejection(const CallLoweringRequest &Request, const ArgumentAssignmentResult &Arguments,
                  std::span<const CCAssignment> Results,
                  std::span<const ByValCopyAssignment> ByValCopies,
                  const CallingConventionInfo &Convention);
[[nodiscard]] Expected<const CallingConventionInfo *>
riscvConvention(CallingConventionID Convention);

} // namespace rcc::riscv

#endif // RCC_LIB_TARGET_RISCV_RISCVCALLINGCONV_H
