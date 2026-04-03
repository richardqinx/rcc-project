/*
 * richard-atp-engine: A saturation-based automated theorem prover for first-order logic.
 * Copyright (C) 2026 Richard Qin
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/// @file test_end_to_end.cc
/// @brief End-to-end tests: TPTP string → parse → prove → verify result.

#include "atp/core/clause_store.h"
#include "atp/core/symbol_table.h"
#include "atp/core/term_bank.h"
#include "atp/frontend/tptp_parser.h"
#include "atp/proof/proof_trace.h"
#include "atp/search/prover.h"

#include <gtest/gtest.h>
#include <string>

namespace atp {
namespace {

/// Helper: parse a TPTP string and run the prover, return the result.
struct E2EResult {
    ProverResult result;
    std::string proof_output;
};

E2EResult runProver(const std::string& tptp_input) {
    SymbolTable symbols;
    TermBank bank(symbols);
    ClauseStore store;

    auto problem = parseTptpString(tptp_input, bank, symbols);
    auto clauses = prepareForProving(problem, bank, symbols);

    Prover prover(bank, store);
    prover.addClauses(std::move(clauses));
    ProverResult result = prover.prove();

    std::string proof_output;
    if (result == ProverResult::kTheorem) {
        auto eid = prover.getEmptyClauseId();
        if (eid.has_value()) {
            auto proof = extractProof(store, *eid);
            proof_output = formatProof(proof, store, bank);
        }
    }

    return {result, proof_output};
}

// ═══════════════════════════════════════════════════════════════════════════
// Simple Propositional
// ═══════════════════════════════════════════════════════════════════════════

TEST(EndToEnd, SimplePropositional) {
    auto [result, proof] = runProver(R"(
        cnf(fact, axiom, p(a)).
        cnf(rule, axiom, ~p(X) | q(X)).
        cnf(goal, negated_conjecture, ~q(a)).
    )");

    EXPECT_EQ(result, ProverResult::kTheorem);
    EXPECT_FALSE(proof.empty());
    // Proof should contain the key elements
    EXPECT_NE(proof.find("p(a)"), std::string::npos) << proof;
    EXPECT_NE(proof.find("q(a)"), std::string::npos) << proof;
}

// ═══════════════════════════════════════════════════════════════════════════
// Trivial: direct contradiction
// ═══════════════════════════════════════════════════════════════════════════

TEST(EndToEnd, DirectContradiction) {
    auto [result, proof] = runProver(R"(
        cnf(pos, axiom, p(a)).
        cnf(neg, negated_conjecture, ~p(a)).
    )");

    EXPECT_EQ(result, ProverResult::kTheorem);
}

// ═══════════════════════════════════════════════════════════════════════════
// Satisfiable: no contradiction possible
// ═══════════════════════════════════════════════════════════════════════════

TEST(EndToEnd, Satisfiable) {
    auto [result, proof] = runProver(R"(
        cnf(a1, axiom, p(a)).
        cnf(a2, axiom, q(b)).
    )");

    EXPECT_EQ(result, ProverResult::kSaturation);
}

// ═══════════════════════════════════════════════════════════════════════════
// Three-step chain
// ═══════════════════════════════════════════════════════════════════════════

TEST(EndToEnd, ThreeStepChain) {
    auto [result, proof] = runProver(R"(
        cnf(a1, axiom, p(a)).
        cnf(a2, axiom, ~p(X) | q(X)).
        cnf(a3, axiom, ~q(X) | r(X)).
        cnf(goal, negated_conjecture, ~r(a)).
    )");

    EXPECT_EQ(result, ProverResult::kTheorem);
    EXPECT_NE(proof.find("r(a)"), std::string::npos) << proof;
}

// ═══════════════════════════════════════════════════════════════════════════
// Proof output format
// ═══════════════════════════════════════════════════════════════════════════

TEST(EndToEnd, ProofOutputContainsAllSteps) {
    auto [result, proof] = runProver(R"(
        cnf(fact, axiom, p(a)).
        cnf(rule, axiom, ~p(X) | q(X)).
        cnf(goal, negated_conjecture, ~q(a)).
    )");

    ASSERT_EQ(result, ProverResult::kTheorem);

    // Must contain [input] lines for axioms
    EXPECT_NE(proof.find("[input]"), std::string::npos) << proof;
    // Must contain [resolution] for derived clauses
    EXPECT_NE(proof.find("[resolution]"), std::string::npos) << proof;
    // Must contain empty clause marker
    std::string empty_marker = "□";
    EXPECT_NE(proof.find(empty_marker), std::string::npos) << proof;
    // Must contain [X, Y] provenance (sequential numbers)
    EXPECT_NE(proof.find("["), std::string::npos) << proof;
}

// ═══════════════════════════════════════════════════════════════════════════
// Monkey and Banana Problem (the flagship test!)
// ═══════════════════════════════════════════════════════════════════════════

TEST(EndToEnd, MonkeyBanana) {
    auto [result, proof] = runProver(R"(
        % Initial state
        cnf(init_state, axiom, state(a, b, c, h0, s0)).

        % Action: Go(X)
        cnf(action_go, axiom, ~state(M, B, G, h0, S) | state(X, B, G, h0, do(go(X), S))).

        % Action: Push(X)
        cnf(action_push, axiom, ~state(M, M, G, h0, S) | state(X, X, G, h0, do(push(X), S))).

        % Action: Climb
        cnf(action_climb, axiom, ~state(M, M, G, h0, S) | state(M, M, G, h1, do(climb, S))).

        % Action: Down
        cnf(action_down, axiom, ~state(M, B, G, h1, S) | state(M, B, G, h0, do(down, S))).

        % Action: Reach
        cnf(action_reach, axiom, ~state(G, G, G, h1, S) | goal_reached(do(reach, S))).

        % Negated goal
        cnf(negated_goal, negated_conjecture, ~goal_reached(S)).
    )");

    EXPECT_EQ(result, ProverResult::kTheorem) << "ATP should prove the monkey can get the banana!\n"
                                              << "Proof output:\n"
                                              << proof;
}

}  // namespace
}  // namespace atp
