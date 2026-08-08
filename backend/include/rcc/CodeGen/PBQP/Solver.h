// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_PBQP_SOLVER_H
#define RCC_CODEGEN_PBQP_SOLVER_H

#include "rcc/CodeGen/PBQP/ReductionRules.h"
#include "rcc/CodeGen/PBQP/Solution.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace rcc::pbqp {

class DeterministicSolver final {
public:
  [[nodiscard]] Expected<Solution> solve(const Graph &Problem) const {
    auto Verified = Problem.verify();
    if (!Verified)
      return std::unexpected(Verified.error());

    WorkingProblem Work(Problem);
    std::vector<ReductionRecord> Reductions;
    Reductions.reserve(Problem.nodeCount());
    while (Work.activeNodeCount() != 0) {
      const NodeID Selected = Work.selectReductionNode();
      const std::size_t Degree = Work.degree(Selected);
      Expected<ReductionRecord> Reduced = Degree == 0   ? Work.reduceDegreeZero(Selected)
                                          : Degree == 1 ? Work.reduceDegreeOne(Selected)
                                          : Degree == 2 ? Work.reduceDegreeTwo(Selected)
                                                        : Work.reduceHeuristically(Selected);
      if (!Reduced)
        return std::unexpected(Reduced.error());
      Reductions.push_back(std::move(*Reduced));
    }

    Solution Result(Problem.nodeCount());
    for (auto Position = Reductions.rbegin(); Position != Reductions.rend(); ++Position) {
      const ReductionRecord &Record = *Position;
      std::size_t Choice = Record.FixedSelection;
      if (Record.Kind != ReductionKind::Heuristic) {
        Cost Best = InfiniteCost;
        for (std::size_t Option = 0; Option < Record.NodeCosts.size(); ++Option) {
          Cost Candidate = Record.NodeCosts[Option];
          if (Record.Kind == ReductionKind::DegreeOne || Record.Kind == ReductionKind::DegreeTwo) {
            const auto Neighbor = Result.selection(Record.FirstNeighbor);
            if (!Neighbor)
              return makeUnexpected(ErrorCode::InternalError,
                                    "PBQP back-propagation encountered an unsolved neighbor");
            Candidate = addCosts(Candidate, Record.FirstCosts.at(Option, *Neighbor));
          }
          if (Record.Kind == ReductionKind::DegreeTwo) {
            const auto Neighbor = Result.selection(Record.SecondNeighbor);
            if (!Neighbor)
              return makeUnexpected(ErrorCode::InternalError,
                                    "PBQP back-propagation encountered an unsolved neighbor");
            Candidate = addCosts(Candidate, Record.SecondCosts.at(Option, *Neighbor));
          }
          if (Candidate < Best) {
            Best = Candidate;
            Choice = Option;
          }
        }
        if (isInfinite(Best))
          return makeUnexpected(ErrorCode::RegisterAllocationFailed,
                                "PBQP reductions produced no finite option");
      }
      auto Set = Result.setSelection(Record.Node, Choice);
      if (!Set)
        return std::unexpected(Set.error());
    }

    Cost Total = 0;
    for (const Node &NodeValue : Problem.nodes()) {
      const auto Selection = Result.selection(NodeValue.ID);
      if (!Selection) {
        return makeUnexpected(ErrorCode::InternalError,
                              "PBQP solver did not select a reduced node option");
      }
      Total = addCosts(Total, NodeValue.Costs[Selection.value()]);
    }
    for (const Edge &EdgeValue : Problem.edges()) {
      const auto FirstSelection = Result.selection(EdgeValue.First);
      const auto SecondSelection = Result.selection(EdgeValue.Second);
      if (!FirstSelection || !SecondSelection) {
        return makeUnexpected(ErrorCode::InternalError,
                              "PBQP solver did not select an edge endpoint option");
      }
      Total = addCosts(Total, EdgeValue.Costs.at(FirstSelection.value(), SecondSelection.value()));
    }
    if (isInfinite(Total))
      return makeUnexpected(ErrorCode::RegisterAllocationFailed,
                            "PBQP problem has no finite solution");
    Result.setTotalCost(Total);
    auto ResultVerification = Result.verify(Problem);
    if (!ResultVerification)
      return std::unexpected(ResultVerification.error());
    return Result;
  }

private:
  struct WorkingEdge final {
    NodeID First;
    NodeID Second;
    CostMatrix Costs;
  };

  class WorkingProblem final {
  public:
    explicit WorkingProblem(const Graph &Problem) {
      Costs.reserve(Problem.nodeCount());
      StableKeys.reserve(Problem.nodeCount());
      Active.assign(Problem.nodeCount(), true);
      for (const Node &NodeValue : Problem.nodes()) {
        Costs.push_back(NodeValue.Costs);
        StableKeys.push_back(NodeValue.StableKey);
      }
      for (const Edge &EdgeValue : Problem.edges())
        Edges.emplace(key(EdgeValue.First, EdgeValue.Second),
                      WorkingEdge{EdgeValue.First, EdgeValue.Second, EdgeValue.Costs});
      ActiveCount = Problem.nodeCount();
    }

    [[nodiscard]] std::size_t activeNodeCount() const noexcept {
      return ActiveCount;
    }

    [[nodiscard]] std::size_t degree(NodeID ID) const noexcept {
      std::size_t Result = 0;
      for (const auto &[EdgeKey, EdgeValue] : Edges) {
        (void)EdgeKey;
        if (Active[EdgeValue.First.value()] && Active[EdgeValue.Second.value()] &&
            (EdgeValue.First == ID || EdgeValue.Second == ID))
          ++Result;
      }
      return Result;
    }

    [[nodiscard]] NodeID selectReductionNode() const noexcept {
      std::optional<std::tuple<bool, std::size_t, std::uint64_t, std::uint32_t>> Best;
      NodeID Result;
      for (std::size_t Index = 0; Index < Active.size(); ++Index) {
        if (!Active[Index])
          continue;
        const NodeID ID = NodeID::fromValue(static_cast<std::uint32_t>(Index));
        const std::size_t Degree = degree(ID);
        const auto Rank = std::tuple{Degree > 2, Degree, StableKeys[Index], ID.value()};
        if (!Best || Rank < *Best) {
          Best = Rank;
          Result = ID;
        }
      }
      return Result;
    }

    [[nodiscard]] Expected<ReductionRecord> reduceDegreeZero(NodeID ID) {
      ReductionRecord Record;
      Record.Kind = ReductionKind::DegreeZero;
      Record.Node = ID;
      Record.NodeCosts = Costs[ID.value()];
      deactivate(ID);
      return Record;
    }

    [[nodiscard]] Expected<ReductionRecord> reduceDegreeOne(NodeID ID) {
      const std::vector<WorkingEdge> Incident = incidentEdges(ID);
      if (Incident.size() != 1)
        return makeUnexpected(ErrorCode::InternalError,
                              "PBQP degree-one reduction is inconsistent");
      const WorkingEdge &EdgeValue = Incident.front();
      const NodeID Neighbor = other(EdgeValue, ID);
      const CostMatrix Oriented = orientFrom(EdgeValue, ID);

      ReductionRecord Record;
      Record.Kind = ReductionKind::DegreeOne;
      Record.Node = ID;
      Record.FirstNeighbor = Neighbor;
      Record.NodeCosts = Costs[ID.value()];
      Record.FirstCosts = Oriented;

      for (std::size_t NeighborOption = 0; NeighborOption < Costs[Neighbor.value()].size();
           ++NeighborOption) {
        Cost Minimum = InfiniteCost;
        for (std::size_t Option = 0; Option < Costs[ID.value()].size(); ++Option) {
          Minimum = std::min(
              Minimum, addCosts(Costs[ID.value()][Option], Oriented.at(Option, NeighborOption)));
        }
        Costs[Neighbor.value()][NeighborOption] =
            addCosts(Costs[Neighbor.value()][NeighborOption], Minimum);
      }
      removeIncidentEdges(ID);
      deactivate(ID);
      return Record;
    }

    [[nodiscard]] Expected<ReductionRecord> reduceDegreeTwo(NodeID ID) {
      const std::vector<WorkingEdge> Incident = incidentEdges(ID);
      if (Incident.size() != 2)
        return makeUnexpected(ErrorCode::InternalError,
                              "PBQP degree-two reduction is inconsistent");
      NodeID First = other(Incident[0], ID);
      NodeID Second = other(Incident[1], ID);
      CostMatrix FirstCosts = orientFrom(Incident[0], ID);
      CostMatrix SecondCosts = orientFrom(Incident[1], ID);
      if (Second < First) {
        std::swap(First, Second);
        std::swap(FirstCosts, SecondCosts);
      }

      ReductionRecord Record;
      Record.Kind = ReductionKind::DegreeTwo;
      Record.Node = ID;
      Record.FirstNeighbor = First;
      Record.SecondNeighbor = Second;
      Record.NodeCosts = Costs[ID.value()];
      Record.FirstCosts = FirstCosts;
      Record.SecondCosts = SecondCosts;

      CostMatrix Delta(Costs[First.value()].size(), Costs[Second.value()].size(), InfiniteCost);
      for (std::size_t FirstOption = 0; FirstOption < Delta.rows(); ++FirstOption) {
        for (std::size_t SecondOption = 0; SecondOption < Delta.columns(); ++SecondOption) {
          for (std::size_t Option = 0; Option < Costs[ID.value()].size(); ++Option) {
            const Cost Candidate =
                addCosts(Costs[ID.value()][Option], addCosts(FirstCosts.at(Option, FirstOption),
                                                             SecondCosts.at(Option, SecondOption)));
            Delta.at(FirstOption, SecondOption) =
                std::min(Delta.at(FirstOption, SecondOption), Candidate);
          }
        }
      }
      removeIncidentEdges(ID);
      const auto Position = Edges.find(key(First, Second));
      if (Position == Edges.end())
        Edges.emplace(key(First, Second), WorkingEdge{First, Second, std::move(Delta)});
      else
        Position->second.Costs.add(Delta);
      deactivate(ID);
      return Record;
    }

    [[nodiscard]] Expected<ReductionRecord> reduceHeuristically(NodeID ID) {
      const std::vector<WorkingEdge> Incident = incidentEdges(ID);
      Cost Best = InfiniteCost;
      std::size_t Choice = 0;
      for (std::size_t Option = 0; Option < Costs[ID.value()].size(); ++Option) {
        Cost Candidate = Costs[ID.value()][Option];
        for (const WorkingEdge &EdgeValue : Incident) {
          const NodeID Neighbor = other(EdgeValue, ID);
          const CostMatrix Oriented = orientFrom(EdgeValue, ID);
          Cost NeighborMinimum = InfiniteCost;
          for (std::size_t NeighborOption = 0; NeighborOption < Costs[Neighbor.value()].size();
               ++NeighborOption) {
            NeighborMinimum = std::min(NeighborMinimum, Oriented.at(Option, NeighborOption));
          }
          Candidate = addCosts(Candidate, NeighborMinimum);
        }
        if (Candidate < Best) {
          Best = Candidate;
          Choice = Option;
        }
      }
      if (isInfinite(Best))
        return makeUnexpected(ErrorCode::RegisterAllocationFailed,
                              "PBQP heuristic reduction has no finite option");

      ReductionRecord Record;
      Record.Kind = ReductionKind::Heuristic;
      Record.Node = ID;
      Record.FixedSelection = Choice;
      Record.NodeCosts = Costs[ID.value()];
      for (const WorkingEdge &EdgeValue : Incident) {
        const NodeID Neighbor = other(EdgeValue, ID);
        const CostMatrix Oriented = orientFrom(EdgeValue, ID);
        for (std::size_t Option = 0; Option < Costs[Neighbor.value()].size(); ++Option) {
          Costs[Neighbor.value()][Option] =
              addCosts(Costs[Neighbor.value()][Option], Oriented.at(Choice, Option));
        }
      }
      removeIncidentEdges(ID);
      deactivate(ID);
      return Record;
    }

  private:
    using EdgeKey = std::pair<std::uint32_t, std::uint32_t>;

    [[nodiscard]] static EdgeKey key(NodeID First, NodeID Second) noexcept {
      if (Second < First)
        std::swap(First, Second);
      return {First.value(), Second.value()};
    }

    [[nodiscard]] static NodeID other(const WorkingEdge &EdgeValue, NodeID ID) noexcept {
      return EdgeValue.First == ID ? EdgeValue.Second : EdgeValue.First;
    }

    [[nodiscard]] static CostMatrix orientFrom(const WorkingEdge &EdgeValue, NodeID ID) {
      return EdgeValue.First == ID ? EdgeValue.Costs : EdgeValue.Costs.transpose();
    }

    [[nodiscard]] std::vector<WorkingEdge> incidentEdges(NodeID ID) const {
      std::vector<WorkingEdge> Result;
      for (const auto &[EdgeKeyValue, EdgeValue] : Edges) {
        (void)EdgeKeyValue;
        if (Active[EdgeValue.First.value()] && Active[EdgeValue.Second.value()] &&
            (EdgeValue.First == ID || EdgeValue.Second == ID))
          Result.push_back(EdgeValue);
      }
      return Result;
    }

    void removeIncidentEdges(NodeID ID) {
      for (auto Position = Edges.begin(); Position != Edges.end();) {
        if (Position->second.First == ID || Position->second.Second == ID)
          Position = Edges.erase(Position);
        else
          ++Position;
      }
    }

    void deactivate(NodeID ID) noexcept {
      Active[ID.value()] = false;
      --ActiveCount;
    }

    std::vector<CostVector> Costs;
    std::vector<std::uint64_t> StableKeys;
    std::vector<bool> Active;
    std::size_t ActiveCount = 0;
    std::map<EdgeKey, WorkingEdge> Edges;
  };
};

} // namespace rcc::pbqp

#endif // RCC_CODEGEN_PBQP_SOLVER_H
