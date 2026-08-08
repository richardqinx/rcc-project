// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_PBQP_GRAPH_H
#define RCC_CODEGEN_PBQP_GRAPH_H

#include "rcc/CodeGen/PBQP/Math.h"
#include "rcc/Support/Error.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace rcc::pbqp {

class NodeID final {
public:
  constexpr NodeID() noexcept = default;
  [[nodiscard]] static constexpr NodeID fromValue(std::uint32_t Value) noexcept {
    return NodeID(Value);
  }
  [[nodiscard]] constexpr bool isValid() const noexcept {
    return Value != InvalidValue;
  }
  [[nodiscard]] constexpr std::uint32_t value() const noexcept {
    return Value;
  }
  friend constexpr bool operator==(NodeID, NodeID) noexcept = default;
  friend constexpr auto operator<=>(NodeID, NodeID) noexcept = default;

private:
  static constexpr std::uint32_t InvalidValue = std::numeric_limits<std::uint32_t>::max();
  explicit constexpr NodeID(std::uint32_t Value) noexcept : Value(Value) {}
  std::uint32_t Value = InvalidValue;
};

class EdgeID final {
public:
  constexpr EdgeID() noexcept = default;
  [[nodiscard]] static constexpr EdgeID fromValue(std::uint32_t Value) noexcept {
    return EdgeID(Value);
  }
  [[nodiscard]] constexpr bool isValid() const noexcept {
    return Value != InvalidValue;
  }
  [[nodiscard]] constexpr std::uint32_t value() const noexcept {
    return Value;
  }
  friend constexpr bool operator==(EdgeID, EdgeID) noexcept = default;
  friend constexpr auto operator<=>(EdgeID, EdgeID) noexcept = default;

private:
  static constexpr std::uint32_t InvalidValue = std::numeric_limits<std::uint32_t>::max();
  explicit constexpr EdgeID(std::uint32_t Value) noexcept : Value(Value) {}
  std::uint32_t Value = InvalidValue;
};

struct Node final {
  NodeID ID;
  std::uint64_t StableKey = 0;
  CostVector Costs;
  std::vector<EdgeID> AdjacentEdges;
};

struct Edge final {
  EdgeID ID;
  NodeID First;
  NodeID Second;
  std::uint64_t StableKey = 0;
  CostMatrix Costs;
};

class Graph final {
public:
  [[nodiscard]] Expected<NodeID> addNode(CostVector Costs, std::uint64_t StableKey = 0) {
    if (Costs.empty())
      return makeUnexpected(ErrorCode::InvalidArgument, "PBQP node requires at least one option");
    if (Nodes.size() >= std::numeric_limits<std::uint32_t>::max())
      return makeUnexpected(ErrorCode::Unsupported, "PBQP node ID space is exhausted");
    const NodeID ID = NodeID::fromValue(static_cast<std::uint32_t>(Nodes.size()));
    Nodes.push_back(Node{ID, StableKey, std::move(Costs), {}});
    return ID;
  }

  [[nodiscard]] Expected<EdgeID> addEdge(NodeID First, NodeID Second, CostMatrix Costs,
                                         std::uint64_t StableKey = 0) {
    const Node *FirstNode = node(First);
    const Node *SecondNode = node(Second);
    if (FirstNode == nullptr || SecondNode == nullptr || First == Second) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "PBQP edge requires two distinct known nodes");
    }
    if (edgeBetween(First, Second))
      return makeUnexpected(ErrorCode::InvalidArgument, "PBQP graph already contains this edge");

    if (Second < First) {
      std::swap(First, Second);
      Costs = Costs.transpose();
      std::swap(FirstNode, SecondNode);
    }
    if (Costs.rows() != FirstNode->Costs.size() || Costs.columns() != SecondNode->Costs.size()) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "PBQP edge matrix dimensions do not match its nodes");
    }
    if (Edges.size() >= std::numeric_limits<std::uint32_t>::max())
      return makeUnexpected(ErrorCode::Unsupported, "PBQP edge ID space is exhausted");
    const EdgeID ID = EdgeID::fromValue(static_cast<std::uint32_t>(Edges.size()));
    Edges.push_back(Edge{ID, First, Second, StableKey, std::move(Costs)});
    Nodes[First.value()].AdjacentEdges.push_back(ID);
    Nodes[Second.value()].AdjacentEdges.push_back(ID);
    return ID;
  }

  [[nodiscard]] const Node *node(NodeID ID) const noexcept {
    return ID.isValid() && ID.value() < Nodes.size() ? &Nodes[ID.value()] : nullptr;
  }
  [[nodiscard]] const Edge *edge(EdgeID ID) const noexcept {
    return ID.isValid() && ID.value() < Edges.size() ? &Edges[ID.value()] : nullptr;
  }

  [[nodiscard]] const Edge *edgeBetween(NodeID Left, NodeID Right) const noexcept {
    const Node *LeftNode = node(Left);
    if (LeftNode == nullptr || node(Right) == nullptr)
      return nullptr;
    for (const EdgeID ID : LeftNode->AdjacentEdges) {
      const Edge &Candidate = Edges[ID.value()];
      if ((Candidate.First == Left && Candidate.Second == Right) ||
          (Candidate.First == Right && Candidate.Second == Left))
        return &Candidate;
    }
    return nullptr;
  }

  [[nodiscard]] std::span<const Node> nodes() const noexcept {
    return Nodes;
  }
  [[nodiscard]] std::span<const Edge> edges() const noexcept {
    return Edges;
  }
  [[nodiscard]] std::size_t nodeCount() const noexcept {
    return Nodes.size();
  }

  [[nodiscard]] Expected<void> verify() const {
    for (std::size_t Index = 0; Index < Nodes.size(); ++Index) {
      const Node &Value = Nodes[Index];
      if (Value.ID.value() != Index || Value.Costs.empty())
        return makeUnexpected(ErrorCode::VerificationError, "PBQP graph contains an invalid node");
      for (const EdgeID ID : Value.AdjacentEdges) {
        const Edge *Adjacent = edge(ID);
        if (Adjacent == nullptr || (Adjacent->First != Value.ID && Adjacent->Second != Value.ID)) {
          return makeUnexpected(ErrorCode::VerificationError,
                                "PBQP node has an invalid adjacency entry");
        }
      }
    }
    for (std::size_t Index = 0; Index < Edges.size(); ++Index) {
      const Edge &Value = Edges[Index];
      const Node *First = node(Value.First);
      const Node *Second = node(Value.Second);
      if (Value.ID.value() != Index || First == nullptr || Second == nullptr ||
          !(Value.First < Value.Second) || Value.Costs.rows() != First->Costs.size() ||
          Value.Costs.columns() != Second->Costs.size()) {
        return makeUnexpected(ErrorCode::VerificationError, "PBQP graph contains an invalid edge");
      }
    }
    return {};
  }

private:
  std::vector<Node> Nodes;
  std::vector<Edge> Edges;
};

} // namespace rcc::pbqp

template <> struct std::hash<rcc::pbqp::NodeID> {
  [[nodiscard]] std::size_t operator()(rcc::pbqp::NodeID ID) const noexcept {
    return std::hash<std::uint32_t>{}(ID.value());
  }
};

template <> struct std::hash<rcc::pbqp::EdgeID> {
  [[nodiscard]] std::size_t operator()(rcc::pbqp::EdgeID ID) const noexcept {
    return std::hash<std::uint32_t>{}(ID.value());
  }
};

#endif // RCC_CODEGEN_PBQP_GRAPH_H
