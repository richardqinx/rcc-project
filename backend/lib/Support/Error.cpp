// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/Support/Error.h"

namespace rcc {

std::string_view getErrorCodeName(ErrorCode Code) noexcept {
  switch (Code) {
  case ErrorCode::InvalidArgument:
    return "invalid argument";
  case ErrorCode::ParseError:
    return "parse error";
  case ErrorCode::VerificationError:
    return "verification error";
  case ErrorCode::Unsupported:
    return "unsupported";
  case ErrorCode::IoError:
    return "I/O error";
  case ErrorCode::InternalError:
    return "internal error";
  }

  return "unknown error";
}

Error::Error(ErrorCode Code, std::string Message) : Code(Code), Message(std::move(Message)) {}

} // namespace rcc
