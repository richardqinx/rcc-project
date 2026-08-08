// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/PBQP/Graph.h"
#include "rcc/CodeGen/PBQP/Math.h"
#include "rcc/CodeGen/PBQP/Solver.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

class TestContext final {
public:
  void expect(bool Condition, std::string_view Message) {
    if (Condition)
      return;
    std::cerr << Message << '\n';
    ++Failures;
  }

  [[nodiscard]] int result() const noexcept {
    return Failures == 0 ? 0 : 1;
  }

private:
  int Failures = 0;
};

void testMath(TestContext &Context) {
  using namespace rcc::pbqp;
  Context.expect(
      addCosts(InfiniteCost, -4) == InfiniteCost && addCosts(InfiniteCost - 1, 2) == InfiniteCost &&
          addCosts(NegativeInfiniteCost + 1, -2) == NegativeInfiniteCost && addCosts(7, -3) == 4,
      "PBQP cost arithmetic must saturate deterministically");

  CostVector Vector(std::vector<Cost>{8, -2, 4});
  Context.expect(Vector.minimumIndex() == 1,
                 "PBQP cost vectors must choose the first stable minimum");
  CostMatrix Matrix(2, 3);
  Matrix.at(0, 0) = 1;
  Matrix.at(0, 1) = 2;
  Matrix.at(0, 2) = 3;
  Matrix.at(1, 0) = 4;
  Matrix.at(1, 1) = 5;
  Matrix.at(1, 2) = 6;
  const CostMatrix Transposed = Matrix.transpose();
  Context.expect(Transposed.rows() == 3 && Transposed.columns() == 2 && Transposed.at(2, 1) == 6 &&
                     Transposed.at(1, 0) == 2,
                 "PBQP matrix transposition must preserve orientation");
}

void testGraphValidation(TestContext &Context) {
  using namespace rcc::pbqp;
  Graph Problem;
  auto Empty = Problem.addNode(CostVector(0));
  auto First = Problem.addNode(CostVector(std::vector<Cost>{0, 1}), 20);
  auto Second = Problem.addNode(CostVector(std::vector<Cost>{2, 0, 3}), 10);
  Context.expect(!Empty && First && Second,
                 "PBQP graphs must reject empty nodes and accept stable nonempty nodes");
  if (!First || !Second)
    return;
  auto Self = Problem.addEdge(*First, *First, CostMatrix(2, 2));
  auto WrongShape = Problem.addEdge(*First, *Second, CostMatrix(2, 2));
  CostMatrix EdgeCosts(2, 3);
  EdgeCosts.at(1, 2) = 7;
  auto Edge = Problem.addEdge(*Second, *First, EdgeCosts.transpose(), 99);
  auto Duplicate = Problem.addEdge(*First, *Second, CostMatrix(2, 3));
  Context.expect(!Self && !WrongShape && Edge && !Duplicate && Problem.verify().has_value(),
                 "PBQP graph edges must enforce identity, dimensions, orientation, and uniqueness");
  if (!Edge)
    return;
  const rcc::pbqp::Edge *Stored = Problem.edge(*Edge);
  Context.expect(Stored != nullptr && Stored->First == *First && Stored->Second == *Second &&
                     Stored->Costs.rows() == 2 && Stored->Costs.columns() == 3 &&
                     Stored->Costs.at(1, 2) == 7,
                 "PBQP graph insertion must normalize reversed edge orientation");
}

void testReductions(TestContext &Context) {
  using namespace rcc::pbqp;
  DeterministicSolver Solver;

  Graph DegreeZero;
  auto Only = DegreeZero.addNode(CostVector(std::vector<Cost>{4, 1, 1}), 3);
  auto ZeroSolution = Solver.solve(DegreeZero);
  Context.expect(Only && ZeroSolution && ZeroSolution->selection(*Only) == 1 &&
                     ZeroSolution->totalCost() == 1,
                 "PBQP R0 reduction must select the first minimum-cost option");

  Graph DegreeOne;
  auto Left = DegreeOne.addNode(CostVector(std::vector<Cost>{3, 0}), 1);
  auto Right = DegreeOne.addNode(CostVector(std::vector<Cost>{0, 2}), 2);
  CostMatrix Pair(2, 2);
  Pair.at(1, 0) = InfiniteCost;
  auto PairEdge = DegreeOne.addEdge(*Left, *Right, std::move(Pair));
  auto OneSolution = Solver.solve(DegreeOne);
  Context.expect(Left && Right && PairEdge && OneSolution && OneSolution->selection(*Left) == 1 &&
                     OneSolution->selection(*Right) == 1 && OneSolution->totalCost() == 2,
                 "PBQP R1 reduction must propagate unary costs to its neighbor");

  Graph DegreeTwo;
  auto A = DegreeTwo.addNode(CostVector(std::vector<Cost>{0, 2}), 1);
  auto B = DegreeTwo.addNode(CostVector(std::vector<Cost>{0, 0}), 2);
  auto C = DegreeTwo.addNode(CostVector(std::vector<Cost>{2, 0}), 3);
  CostMatrix AB(2, 2);
  CostMatrix BC(2, 2);
  AB.at(0, 1) = 5;
  AB.at(1, 0) = 5;
  BC.at(0, 1) = 5;
  BC.at(1, 0) = 5;
  auto ABEdge = DegreeTwo.addEdge(*A, *B, std::move(AB));
  auto BCEdge = DegreeTwo.addEdge(*B, *C, std::move(BC));
  auto TwoSolution = Solver.solve(DegreeTwo);
  Context.expect(A && B && C && ABEdge && BCEdge && TwoSolution &&
                     TwoSolution->selection(*A) == 0 && TwoSolution->selection(*B) == 0 &&
                     TwoSolution->selection(*C) == 0 && TwoSolution->totalCost() == 2,
                 "PBQP R2 reduction must add the induced neighbor edge cost");
}

[[nodiscard]] rcc::Expected<rcc::pbqp::Solution> solveClique() {
  using namespace rcc::pbqp;
  Graph Problem;
  std::vector<NodeID> Nodes;
  for (std::uint64_t Index = 0; Index < 4; ++Index) {
    auto Node = Problem.addNode(CostVector(std::vector<Cost>{9, 0, 0}), 100 - Index);
    if (!Node)
      return std::unexpected(Node.error());
    Nodes.push_back(*Node);
  }
  for (std::size_t Left = 0; Left < Nodes.size(); ++Left) {
    for (std::size_t Right = Left + 1; Right < Nodes.size(); ++Right) {
      CostMatrix Costs(3, 3);
      Costs.at(1, 1) = InfiniteCost;
      Costs.at(2, 2) = InfiniteCost;
      auto Edge = Problem.addEdge(Nodes[Left], Nodes[Right], std::move(Costs),
                                  static_cast<std::uint64_t>(Left * Nodes.size() + Right));
      if (!Edge)
        return std::unexpected(Edge.error());
    }
  }
  return DeterministicSolver{}.solve(Problem);
}

void testHeuristicAndDeterminism(TestContext &Context) {
  auto First = solveClique();
  auto Second = solveClique();
  Context.expect(First && Second && std::ranges::equal(First->selections(), Second->selections()) &&
                     First->totalCost() == Second->totalCost(),
                 "PBQP higher-degree reduction must be deterministic");
  if (First) {
    Context.expect(First->totalCost() == 18,
                   "a four-node two-register clique must spill exactly two nodes");
  }

  rcc::pbqp::Graph Impossible;
  auto Node = Impossible.addNode(
      rcc::pbqp::CostVector(std::vector<rcc::pbqp::Cost>{rcc::pbqp::InfiniteCost}));
  auto Failure = rcc::pbqp::DeterministicSolver{}.solve(Impossible);
  Context.expect(Node && !Failure &&
                     Failure.error().code() == rcc::ErrorCode::RegisterAllocationFailed,
                 "PBQP solver must diagnose a problem with no finite assignment");
}

} // namespace

int main() {
  TestContext Context;
  testMath(Context);
  testGraphValidation(Context);
  testReductions(Context);
  testHeuristicAndDeterminism(Context);
  return Context.result();
}
