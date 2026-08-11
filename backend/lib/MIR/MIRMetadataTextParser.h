// SPDX-License-Identifier: GPL-3.0-or-later

// NOLINTNEXTLINE(llvm-header-guard)
#ifndef RCC_LIB_MIR_MIRMETADATATEXTPARSER_H
#define RCC_LIB_MIR_MIRMETADATATEXTPARSER_H

#include "rcc/CodeGen/MachineDebugInfo.h"
#include "rcc/CodeGen/MachineMetadata.h"
#include "rcc/MIR/MILexer.h"
#include "rcc/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rcc::mir {

class MetadataTextParser final {
public:
  using Resolver = std::function<Expected<MachineMetadataReference>(std::uint32_t Slot)>;

  MetadataTextParser(std::string Text, Resolver Resolve)
      : Lexer("MIR metadata scalar", std::move(Text)), Resolve(std::move(Resolve)) {
    do {
      Tokens.push_back(Lexer.next());
    } while (!Tokens.back().isErrorOrEnd());
  }

  [[nodiscard]] Expected<MachineMetadataReference> parseReferenceOnly();
  [[nodiscard]] Expected<MachineMetadataValue> parseValue();
  [[nodiscard]] Expected<MachineDebugLocation> parseLocation();

  [[nodiscard]] const TextRange &diagnosticRange() const noexcept {
    return token().range();
  }

private:
  [[nodiscard]] const MIToken &token() const noexcept {
    return Tokens[Position];
  }
  [[nodiscard]] bool at(MITokenKind Kind) const noexcept {
    return token().is(Kind);
  }
  void advance() noexcept {
    if (Position + 1U < Tokens.size())
      ++Position;
  }
  [[nodiscard]] bool consume(MITokenKind Kind) noexcept {
    if (!at(Kind))
      return false;
    advance();
    return true;
  }
  [[nodiscard]] Expected<void> expect(MITokenKind Kind, std::string_view Description);
  [[nodiscard]] Expected<void> finish();
  [[nodiscard]] Expected<MachineMetadataReference> parseReference();
  [[nodiscard]] Expected<MachineDIExpression> parseExpression();
  [[nodiscard]] Expected<void> parseDILocation(std::vector<MachineDILocationFrame> &Frames,
                                               std::optional<MachineMetadataReference> &Terminal);

  MILexer Lexer;
  Resolver Resolve;
  std::vector<MIToken> Tokens;
  std::size_t Position = 0;
};

} // namespace rcc::mir

#endif // RCC_LIB_MIR_MIRMETADATATEXTPARSER_H
