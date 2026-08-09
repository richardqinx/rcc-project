// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_SUPPORT_ERROR_H
#define RCC_SUPPORT_ERROR_H

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace rcc {

enum class ErrorCode : std::uint8_t {
  InvalidArgument,
  ParseError,
  VerificationError,
  Unsupported,
  RegisterAllocationFailed,
  IoError,
  InternalError,
  FrameLayoutFailed,
  SchedulingFailed,
  BranchRelaxationFailed,
  PseudoExpansionFailed,
  FinalEncodabilityFailed,
  CFGOptimizationFailed,
  AssemblyLoweringFailed,
};

[[nodiscard]] std::string_view getErrorCodeName(ErrorCode Code) noexcept;

class Error final {
public:
  Error(ErrorCode Code, std::string Message);

  [[nodiscard]] ErrorCode code() const noexcept {
    return Code;
  }
  [[nodiscard]] std::string_view message() const noexcept {
    return Message;
  }

private:
  ErrorCode Code;
  std::string Message;
};

template <typename ValueT> using Expected = std::expected<ValueT, Error>;

[[nodiscard]] inline std::unexpected<Error> makeUnexpected(ErrorCode Code, std::string Message) {
  return std::unexpected<Error>(Error(Code, std::move(Message)));
}

} // namespace rcc

#endif // RCC_SUPPORT_ERROR_H
