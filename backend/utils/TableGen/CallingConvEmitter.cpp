// SPDX-License-Identifier: GPL-3.0-or-later

#include "TableGenBackends.h"

#include <cstdint>
#include <ostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>

namespace rcc::targetgen {
namespace {

void emitPhysicalRegisterArray(std::ostream &Output, const NormalizedTargetModel &Model,
                               std::string_view Name, std::span<const std::uint32_t> IDs) {
  Output << "const std::array<PhysicalRegister, " << IDs.size() << "> " << Name << " = {{\n";
  for (const std::uint32_t ID : IDs)
    Output << "    physical(PhysicalRegisterID::" << findRegister(Model, ID).Symbol << "),\n";
  Output << "}};\n";
}

} // namespace

std::string emitCallingConventions(const NormalizedTargetModel &Model) {
  std::ostringstream Output;
  for (const NormalizedCallingConvention &Convention : Model.CallingConventions) {
    const std::string Prefix = "CC" + std::to_string(Convention.ID);
    emitPhysicalRegisterArray(Output, Model, Prefix + "IntegerArguments",
                              Convention.IntegerArgumentRegisters);
    emitPhysicalRegisterArray(Output, Model, Prefix + "FloatingArguments",
                              Convention.FloatingArgumentRegisters);
    emitPhysicalRegisterArray(Output, Model, Prefix + "IntegerReturns",
                              Convention.IntegerReturnRegisters);
    emitPhysicalRegisterArray(Output, Model, Prefix + "FloatingReturns",
                              Convention.FloatingReturnRegisters);
  }
  Output << "const std::array<CallingConventionInfo, " << Model.CallingConventions.size()
         << "> GeneratedCallingConventions = {{\n";
  for (const NormalizedCallingConvention &Convention : Model.CallingConventions) {
    const std::string Prefix = "CC" + std::to_string(Convention.ID);
    Output << "    {CallingConvention::" << Convention.Symbol << ", ";
    appendQuoted(Output, Convention.Name);
    Output << ", RegisterClassID::fromValue(" << Convention.IntegerRegisterClass
           << "), RegisterClassID::fromValue(" << Convention.FloatingRegisterClass << "), "
           << Prefix << "IntegerArguments, " << Prefix << "FloatingArguments, " << Prefix
           << "IntegerReturns, " << Prefix << "FloatingReturns, ";
    appendQuoted(Output, Convention.PreservedMaskName);
    Output << ", " << Convention.StackAlignmentBytes << ", "
           << Convention.StackArgumentAlignmentBytes << ", " << Convention.ShadowSpaceBytes << ", "
           << (Convention.SupportsVarArgs ? "true" : "false") << ", "
           << (Convention.SupportsTailCalls ? "true" : "false") << "},\n";
  }
  Output << "}};\n";
  return Output.str();
}

} // namespace rcc::targetgen
