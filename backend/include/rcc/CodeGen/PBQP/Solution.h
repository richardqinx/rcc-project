// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_PBQP_SOLUTION_H
#define RCC_CODEGEN_PBQP_SOLUTION_H

#include "rcc/CodeGen/PBQP/Graph.h"
#include "rcc/Support/Error.h"

#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace rcc::pbqp {

class Solution final {
public:
  explicit Solution(std::size_t NodeCount)
      : Selections(NodeCount, std::numeric_limits<std::size_t>::max()) {}

  [[nodiscard]] Expected<void> setSelection(NodeID ID, std::size_t Selection) {
    if (!ID.isValid() || ID.value() >= Selections.size())
      return makeUnexpected(ErrorCode::InvalidArgument, "PBQP solution received an unknown node");
    Selections[ID.value()] = Selection;
    return {};
  }

  [[nodiscard]] std::optional<std::size_t> selection(NodeID ID) const noexcept {
    if (!ID.isValid() || ID.value() >= Selections.size() ||
        Selections[ID.value()] == std::numeric_limits<std::size_t>::max())
      return std::nullopt;
    return Selections[ID.value()];
  }

  [[nodiscard]] std::span<const std::size_t> selections() const noexcept {
    return Selections;
  }
  [[nodiscard]] Cost totalCost() const noexcept {
    return TotalCost;
  }
  void setTotalCost(Cost Value) noexcept {
    TotalCost = Value;
  }

  [[nodiscard]] Expected<void> verify(const Graph &Problem) const {
    if (Selections.size() != Problem.nodeCount())
      return makeUnexpected(ErrorCode::VerificationError, "PBQP solution has the wrong node count");
    Cost Total = 0;
    for (const Node &NodeValue : Problem.nodes()) {
      const auto Selection = selection(NodeValue.ID);
      if (!Selection)
        return makeUnexpected(ErrorCode::VerificationError,
                              "PBQP solution omits a valid node selection");
      const std::size_t SelectedOption = Selection.value();
      if (SelectedOption >= NodeValue.Costs.size())
        return makeUnexpected(ErrorCode::VerificationError,
                              "PBQP solution selects an invalid node option");
      Total = addCosts(Total, NodeValue.Costs[SelectedOption]);
    }
    for (const Edge &EdgeValue : Problem.edges()) {
      const auto FirstSelection = selection(EdgeValue.First);
      const auto SecondSelection = selection(EdgeValue.Second);
      if (!FirstSelection || !SecondSelection) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "PBQP solution omits an edge endpoint selection");
      }
      Total = addCosts(Total, EdgeValue.Costs.at(FirstSelection.value(), SecondSelection.value()));
    }
    if (isInfinite(Total))
      return makeUnexpected(ErrorCode::RegisterAllocationFailed,
                            "PBQP problem has no finite solution");
    if (Total != TotalCost)
      return makeUnexpected(ErrorCode::VerificationError, "PBQP solution cost is inconsistent");
    return {};
  }

private:
  std::vector<std::size_t> Selections;
  Cost TotalCost = 0;
};

} // namespace rcc::pbqp

#endif // RCC_CODEGEN_PBQP_SOLUTION_H
