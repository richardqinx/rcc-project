// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/Target/RISCV/RISCVFunctionInfo.h"

#include "rcc/CodeGen/TargetMIRTextInfo.h"

#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace rcc::riscv {
namespace {

[[nodiscard]] Expected<std::int32_t> parseSignedField(const MIRTargetNode &Node,
                                                      std::string_view Name) {
  if (Node.Kind != MIRTargetNodeKind::Scalar)
    return makeUnexpected(ErrorCode::ParseError,
                          "RISC-V machineFunctionInfo field '" + std::string(Name) +
                              "' must be a scalar");
  std::int32_t Value = 0;
  const auto Result =
      std::from_chars(Node.Scalar.data(), Node.Scalar.data() + Node.Scalar.size(), Value, 10);
  if (Result.ec != std::errc{} || Result.ptr != Node.Scalar.data() + Node.Scalar.size()) {
    return makeUnexpected(ErrorCode::ParseError,
                          "RISC-V machineFunctionInfo field '" + std::string(Name) +
                              "' must be a 32-bit signed integer");
  }
  return Value;
}

[[nodiscard]] Expected<TargetFunctionState> parseFunctionInfo(const MIRTargetNode &Node) {
  if (Node.Kind != MIRTargetNodeKind::Mapping)
    return makeUnexpected(ErrorCode::ParseError,
                          "RISC-V machineFunctionInfo must be a mapping");

  std::int32_t VarArgsFrameIndex = 0;
  std::int32_t VarArgsSaveSize = 0;
  for (const MIRTargetMappingEntry &Entry : Node.Mapping) {
    if (Entry.Key == "varArgsFrameIndex") {
      auto Value = parseSignedField(Entry.Value, Entry.Key);
      if (!Value)
        return std::unexpected(Value.error());
      VarArgsFrameIndex = *Value;
      continue;
    }
    if (Entry.Key == "varArgsSaveSize") {
      auto Value = parseSignedField(Entry.Value, Entry.Key);
      if (!Value)
        return std::unexpected(Value.error());
      VarArgsSaveSize = *Value;
      continue;
    }
    return makeUnexpected(ErrorCode::ParseError,
                          "unknown RISC-V machineFunctionInfo field '" + Entry.Key + "'");
  }
  return TargetFunctionState::create(
      RISCVFunctionInfo(VarArgsFrameIndex, VarArgsSaveSize));
}

[[nodiscard]] Expected<MIRTargetNode>
printFunctionInfo(const TargetFunctionState &State) {
  const auto *Info = State.getIf<RISCVFunctionInfo>();
  if (Info == nullptr)
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "target function state is not RISC-V function information");
  std::vector<MIRTargetMappingEntry> Entries;
  Entries.push_back({"varArgsFrameIndex",
                     MIRTargetNode::scalar(std::to_string(Info->varArgsFrameIndex()))});
  Entries.push_back({"varArgsSaveSize",
                     MIRTargetNode::scalar(std::to_string(Info->varArgsSaveSize()))});
  return MIRTargetNode::mapping(std::move(Entries));
}

void appendWord(std::vector<std::byte> &Payload, std::int32_t Value) {
  const std::uint32_t Bits = std::bit_cast<std::uint32_t>(Value);
  for (std::uint32_t Shift = 0; Shift != 32; Shift += 8)
    Payload.push_back(static_cast<std::byte>((Bits >> Shift) & 0xFFU));
}

[[nodiscard]] std::int32_t readWord(std::span<const std::byte> Payload,
                                    std::size_t Offset) noexcept {
  std::uint32_t Bits = 0;
  for (std::uint32_t Byte = 0; Byte != 4; ++Byte) {
    Bits |= std::to_integer<std::uint32_t>(Payload[Offset + Byte]) << (Byte * 8U);
  }
  return std::bit_cast<std::int32_t>(Bits);
}

} // namespace

Expected<void> RISCVFunctionInfo::verify() const {
  return {};
}

Expected<RISCVFunctionInfo>
RISCVFunctionInfo::clone(const MachineFunctionCloneContext &) const {
  return RISCVFunctionInfo(VarArgsFrameIndex, VarArgsSaveSize);
}

std::string RISCVFunctionInfo::print() const {
  return "riscv-varargs:" + std::to_string(VarArgsFrameIndex) + ":" +
         std::to_string(VarArgsSaveSize);
}

Expected<std::vector<std::byte>> RISCVFunctionInfo::encode() const {
  std::vector<std::byte> Payload;
  Payload.reserve(8);
  appendWord(Payload, VarArgsFrameIndex);
  appendWord(Payload, VarArgsSaveSize);
  return Payload;
}

Expected<RISCVFunctionInfo>
RISCVFunctionInfo::decode(std::uint32_t Version, std::span<const std::byte> Payload) {
  if (Version != schemaVersion())
    return makeUnexpected(ErrorCode::ParseError,
                          "unsupported RISC-V function-info schema version");
  if (Payload.size() != 8)
    return makeUnexpected(ErrorCode::ParseError,
                          "RISC-V function-info payload must contain eight bytes");
  return RISCVFunctionInfo(readWord(Payload, 0), readWord(Payload, 4));
}

void configureFunctionInfoMIRSchema(TargetMIRTextInfoDesc &Description) {
  Description.FunctionStateType = RISCVFunctionInfo::stableTypeID();
  Description.ParseFunctionState = parseFunctionInfo;
  Description.PrintFunctionState = printFunctionInfo;
}

} // namespace rcc::riscv
