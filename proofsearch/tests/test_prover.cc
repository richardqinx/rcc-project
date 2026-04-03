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

#include <gtest/gtest.h>

#include "atp/core/clause.h"
#include "atp/core/clause_store.h"
#include "atp/core/literal.h"
#include "atp/core/symbol_table.h"
#include "atp/core/term_bank.h"
#include "atp/core/types.h"
#include "atp/search/prover.h"

namespace atp {
namespace {

class ProverTest : public ::testing::Test {
  protected:
    SymbolTable symbols;
    TermBank bank{symbols};
    ClauseStore store;

    SymbolId sym_a{}, sym_b{};
    SymbolId sym_f{};
    SymbolId sym_P{}, sym_Q{}, sym_R{};
    SymbolId sym_X{}, sym_Y{};

    TermId a{}, b{}, X{}, Y{};

    void SetUp() override {
        sym_a = symbols.intern("a", SymbolKind::kConstant);
        sym_b = symbols.intern("b", SymbolKind::kConstant);
        sym_f = symbols.intern("f", SymbolKind::kFunction, 1);
        sym_P = symbols.intern("P", SymbolKind::kPredicate, 1);
        sym_Q = symbols.intern("Q", SymbolKind::kPredicate, 1);
        sym_R = symbols.intern("R", SymbolKind::kPredicate, 2);
        sym_X = symbols.intern("X", SymbolKind::kVariable);
        sym_Y = symbols.intern("Y", SymbolKind::kVariable);

        a = bank.makeTerm(sym_a, {});
        b = bank.makeTerm(sym_b, {});
        X = bank.makeVar(sym_X);
        Y = bank.makeVar(sym_Y);
    }

    TermId P(TermId arg) { return bank.makeTerm(sym_P, {{arg}}); }
    TermId Q(TermId arg) { return bank.makeTerm(sym_Q, {{arg}}); }
    TermId R(TermId a1, TermId a2) { return bank.makeTerm(sym_R, {{a1, a2}}); }
    TermId f(TermId arg) { return bank.makeTerm(sym_f, {{arg}}); }

    Literal pos(TermId atom) { return {.atom = atom, .is_positive = true}; }
    Literal neg(TermId atom) { return {.atom = atom, .is_positive = false}; }

    Clause makeClause(std::vector<Literal> lits) {
        Clause cl;
        cl.literals = std::move(lits);
        return cl;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// Trivial refutation: {P(a)} and {¬P(a)} → empty clause
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(ProverTest, TrivialRefutation) {
    Prover prover(bank, store);
    prover.addClauses({
        makeClause({pos(P(a))}),
        makeClause({neg(P(a))})
    });

    ProverResult result = prover.prove();
    EXPECT_EQ(result, ProverResult::kTheorem);
    EXPECT_TRUE(prover.getEmptyClauseId().has_value());
}

// ═══════════════════════════════════════════════════════════════════════════
// Unification required: {P(X)} and {¬P(a)} → empty clause
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(ProverTest, RefutationWithUnification) {
    Prover prover(bank, store);
    prover.addClauses({
        makeClause({pos(P(X))}),
        makeClause({neg(P(a))})
    });

    ProverResult result = prover.prove();
    EXPECT_EQ(result, ProverResult::kTheorem);
}

// ═══════════════════════════════════════════════════════════════════════════
// Multi-step: {P(a)}, {¬P(X), Q(X)}, {¬Q(a)}
// Step 1: resolve P(a) with ¬P(X)∨Q(X) → Q(a)
// Step 2: resolve Q(a) with ¬Q(a) → empty
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(ProverTest, TwoStepRefutation) {
    Prover prover(bank, store);
    prover.addClauses({
        makeClause({pos(P(a))}),
        makeClause({neg(P(X)), pos(Q(X))}),
        makeClause({neg(Q(a))})
    });

    ProverResult result = prover.prove();
    EXPECT_EQ(result, ProverResult::kTheorem);
}

// ═══════════════════════════════════════════════════════════════════════════
// Saturation: {P(a)}, {Q(b)} — no complementary literals
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(ProverTest, Saturation) {
    Prover prover(bank, store);
    prover.addClauses({
        makeClause({pos(P(a))}),
        makeClause({pos(Q(b))})
    });

    ProverResult result = prover.prove();
    EXPECT_EQ(result, ProverResult::kSaturation);
}

// ═══════════════════════════════════════════════════════════════════════════
// Timeout: set very low limits
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(ProverTest, Timeout) {
    ProverConfig config;
    config.max_iterations = 1;

    // Need enough clauses to not saturate in 1 iteration
    Prover prover(bank, store, config);
    prover.addClauses({
        makeClause({pos(P(X)), pos(Q(X))}),
        makeClause({neg(P(a)), pos(Q(a))}),
        makeClause({neg(Q(X)), pos(P(f(X)))}),
        makeClause({neg(P(X)), neg(Q(X))})
    });

    ProverResult result = prover.prove();
    EXPECT_EQ(result, ProverResult::kTimeout);
}

// ═══════════════════════════════════════════════════════════════════════════
// Chain reasoning: P(a), ¬P(X)∨Q(X), ¬Q(X)∨R(X), ¬R(a)
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(ProverTest, ThreeStepChain) {
    SymbolId sym_S = symbols.intern("S", SymbolKind::kPredicate, 1);
    auto S = [&](TermId arg) { return bank.makeTerm(sym_S, {{arg}}); };

    Prover prover(bank, store);
    prover.addClauses({
        makeClause({pos(P(a))}),                     // P(a)
        makeClause({neg(P(X)), pos(Q(X))}),           // ¬P(X) ∨ Q(X)
        makeClause({neg(Q(X)), pos(S(X))}),           // ¬Q(X) ∨ S(X)
        makeClause({neg(S(a))})                       // ¬S(a)
    });

    ProverResult result = prover.prove();
    EXPECT_EQ(result, ProverResult::kTheorem);
}

// ═══════════════════════════════════════════════════════════════════════════
// Empty clause in input directly
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(ProverTest, EmptyClauseInInput) {
    Prover prover(bank, store);
    prover.addClauses({
        makeClause({}),  // empty clause directly
        makeClause({pos(P(a))})
    });

    // Empty clause should be detected immediately when selected from queue
    ProverResult result = prover.prove();
    EXPECT_EQ(result, ProverResult::kTheorem);
}

}  // namespace
}  // namespace atp
