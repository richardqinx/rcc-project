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
  case ErrorCode::RegisterAllocationFailed:
    return "register allocation failed";
  case ErrorCode::FrameLayoutFailed:
    return "frame layout failed";
  case ErrorCode::SchedulingFailed:
    return "scheduling failed";
  case ErrorCode::BranchRelaxationFailed:
    return "branch relaxation failed";
  case ErrorCode::PseudoExpansionFailed:
    return "pseudo expansion failed";
  case ErrorCode::FinalEncodabilityFailed:
    return "final encodability failed";
  case ErrorCode::CFGOptimizationFailed:
    return "CFG optimization failed";
  case ErrorCode::AssemblyLoweringFailed:
    return "assembly lowering failed";
  case ErrorCode::InstructionEncodingFailed:
    return "instruction encoding failed";
  case ErrorCode::FixupApplicationFailed:
    return "fixup application failed";
  case ErrorCode::RelocationFailed:
    return "relocation failed";
  case ErrorCode::ObjectWritingFailed:
    return "object writing failed";
  case ErrorCode::InvalidInvocation:
    return "invalid backend invocation";
  case ErrorCode::ManifestError:
    return "manifest error";
  case ErrorCode::CacheMiss:
    return "cache miss";
  case ErrorCode::ReproducerError:
    return "reproducer error";
  case ErrorCode::ReductionFailed:
    return "reduction failed";
  case ErrorCode::FuzzingFailure:
    return "fuzzing failure";
  case ErrorCode::IoError:
    return "I/O error";
  case ErrorCode::InternalError:
    return "internal error";
  }

  return "unknown error";
}

Error::Error(ErrorCode Code, std::string Message) : Code(Code), Message(std::move(Message)) {}

} // namespace rcc
