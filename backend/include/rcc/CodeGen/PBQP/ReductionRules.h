// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_PBQP_REDUCTIONRULES_H
#define RCC_CODEGEN_PBQP_REDUCTIONRULES_H

#include "rcc/CodeGen/PBQP/Graph.h"

#include <cstdint>

namespace rcc::pbqp {

enum class ReductionKind : std::uint8_t {
  DegreeZero,
  DegreeOne,
  DegreeTwo,
  Heuristic,
};

struct ReductionRecord final {
  ReductionKind Kind = ReductionKind::DegreeZero;
  NodeID Node;
  NodeID FirstNeighbor;
  NodeID SecondNeighbor;
  std::size_t FixedSelection = 0;
  CostVector NodeCosts{1};
  CostMatrix FirstCosts{1, 1};
  CostMatrix SecondCosts{1, 1};
};

} // namespace rcc::pbqp

#endif // RCC_CODEGEN_PBQP_REDUCTIONRULES_H
