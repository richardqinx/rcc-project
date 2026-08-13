// SPDX-License-Identifier: GPL-3.0-or-later

#include "TableGenBackends.h"

#include <sstream>
#include <string>

namespace rcc::targetgen {

std::string emitCodeEmitter(const NormalizedTargetModel &Model) {
  std::ostringstream Output;
  Output << "constexpr std::array<EncodingCategoryInfo, " << Model.EncodingCategories.size()
         << "> GeneratedEncodingCategories = {{\n";
  for (const NormalizedEncodingCategory &Category : Model.EncodingCategories) {
    Output << "    {EncodingCategoryID::fromValue(" << Category.ID << "), ";
    appendQuoted(Output, Category.Name);
    Output << ", " << Category.SizeInBytes << "},\n";
  }
  Output << "}};\n\n";
  return Output.str();
}

} // namespace rcc::targetgen
