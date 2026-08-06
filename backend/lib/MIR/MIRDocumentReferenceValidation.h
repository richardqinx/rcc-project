// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_LIB_MIR_MIRDOCUMENTREFERENCEVALIDATION_H
#define RCC_LIB_MIR_MIRDOCUMENTREFERENCEVALIDATION_H

#include "rcc/Support/Error.h"

namespace rcc {

class MachineFunction;

namespace mir {

class MIRDocument;

[[nodiscard]] Expected<void> verifyMIRDocumentReferences(const MIRDocument &Document,
                                                         const MachineFunction &Function,
                                                         bool UseEmbeddedIR);

} // namespace mir
} // namespace rcc

#endif // RCC_LIB_MIR_MIRDOCUMENTREFERENCEVALIDATION_H
