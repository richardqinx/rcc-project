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

#include "atp/core/clause.h"
#include "atp/core/clause_store.h"
#include "atp/core/literal.h"
#include "atp/core/symbol_table.h"
#include "atp/core/term_bank.h"
#include "atp/core/types.h"
#include "atp/search/prover.h"

#include <gtest/gtest.h>
#include <utility>
#include <vector>

namespace atp {
namespace {

class ProverTest : public ::testing::Test {
  protected:
    SymbolTable symbols_;
    TermBank bank_{symbols_};
    ClauseStore store_;

    SymbolId sym_a_{}, sym_b_{};
    SymbolId sym_f_{};
    SymbolId sym_p_{}, sym_q_{}, sym_r_{};
    SymbolId sym_x_{}, sym_y_{};

    TermId a_{}, b_{}, x_{}, y_{};


    void SetUp() override {
        sym_a_ = symbols_.intern("a", SymbolKind::kConstant);
        sym_b_ = symbols_.intern("b", SymbolKind::kConstant);
        sym_f_ = symbols_.intern("f", SymbolKind::kFunction, 1);
        sym_p_ = symbols_.intern("P", SymbolKind::kPredicate, 1);
        sym_q_ = symbols_.intern("Q", SymbolKind::kPredicate, 1);
        sym_r_ = symbols_.intern("R", SymbolKind::kPredicate, 2);
        sym_x_ = symbols_.intern("X", SymbolKind::kVariable);
        sym_y_ = symbols_.intern("Y", SymbolKind::kVariable);

        a_ = bank_.makeTerm(sym_a_, {});
        b_ = bank_.makeTerm(sym_b_, {});
        x_ = bank_.makeVar(sym_x_);
        y_ = bank_.makeVar(sym_y_);
    }

    TermId p(TermId arg) { return bank_.makeTerm(sym_p_, {{arg}}); }
    TermId q(TermId arg) { return bank_.makeTerm(sym_q_, {{arg}}); }
    TermId r(TermId a1, TermId a2) { return bank_.makeTerm(sym_r_, {{a1, a2}}); }
    TermId f(TermId arg) { return bank_.makeTerm(sym_f_, {{arg}}); }

    static Literal pos(TermId atom) { return {.atom_ = atom, .is_positive_ = true}; }
    static Literal neg(TermId atom) { return {.atom_ = atom, .is_positive_ = false}; }

    static Clause makeClause(std::vector<Literal> lits) {
        Clause cl;
        cl.literals_ = std::move(lits);
        return cl;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// Trivial refutation: {P(a)} and {¬P(a)} → empty clause
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(ProverTest, TrivialRefutation) {
    Prover prover(bank_, store_);
    prover.addClauses({makeClause({pos(p(a_))}), makeClause({neg(p(a_))})});

    const ProverResult kResult = prover.prove();
    EXPECT_EQ(kResult, ProverResult::kTheorem);
    EXPECT_TRUE(prover.getEmptyClauseId().has_value());
}

// ═══════════════════════════════════════════════════════════════════════════
// Unification required: {P(X)} and {¬P(a)} → empty clause
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(ProverTest, RefutationWithUnification) {
    Prover prover(bank_, store_);
    prover.addClauses({makeClause({pos(p(x_))}), makeClause({neg(p(a_))})});

    const ProverResult kResult = prover.prove();
    EXPECT_EQ(kResult, ProverResult::kTheorem);
}

// ═══════════════════════════════════════════════════════════════════════════
// Multi-step: {P(a)}, {¬P(X), Q(X)}, {¬Q(a)}
// Step 1: resolve P(a) with ¬P(X)∨Q(X) → Q(a)
// Step 2: resolve Q(a) with ¬Q(a) → empty
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(ProverTest, TwoStepRefutation) {
    Prover prover(bank_, store_);
    prover.addClauses(
        {makeClause({pos(p(a_))}), makeClause({neg(p(x_)), pos(q(x_))}), makeClause({neg(q(a_))})});

    const ProverResult kResult = prover.prove();
    EXPECT_EQ(kResult, ProverResult::kTheorem);
}

// ═══════════════════════════════════════════════════════════════════════════
// Saturation: {P(a)}, {Q(b)} — no complementary literals
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(ProverTest, Saturation) {
    Prover prover(bank_, store_);
    prover.addClauses({makeClause({pos(p(a_))}), makeClause({pos(q(b_))})});

    const ProverResult kResult = prover.prove();
    EXPECT_EQ(kResult, ProverResult::kSaturation);
}

// ═══════════════════════════════════════════════════════════════════════════
// Timeout: set very low limits
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(ProverTest, Timeout) {
    ProverConfig config;
    config.max_iterations_ = 1;

    // Need enough clauses to not saturate in 1 iteration
    Prover prover(bank_, store_, config);
    prover.addClauses({makeClause({pos(p(x_)), pos(q(x_))}), makeClause({neg(p(a_)), pos(q(a_))}),
                       makeClause({neg(q(x_)), pos(p(f(x_)))}), makeClause({neg(p(x_)), neg(q(x_))})});

    const ProverResult kResult = prover.prove();
    EXPECT_EQ(kResult, ProverResult::kTimeout);
}

// ═══════════════════════════════════════════════════════════════════════════
// Chain reasoning: P(a), ¬P(X)∨Q(X), ¬Q(X)∨R(X), ¬R(a)
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(ProverTest, ThreeStepChain) {
    const SymbolId kSymS = symbols_.intern("S", SymbolKind::kPredicate, 1);
    auto s = [&](TermId arg) { return bank_.makeTerm(kSymS, {{arg}}); };

    Prover prover(bank_, store_);
    prover.addClauses({
        makeClause({pos(p(a_))}),             // P(a)
        makeClause({neg(p(x_)), pos(q(x_))}),  // ¬P(X) ∨ Q(X)
        makeClause({neg(q(x_)), pos(s(x_))}),  // ¬Q(X) ∨ S(X)
        makeClause({neg(s(a_))})              // ¬S(a)
    });

    const ProverResult kResult = prover.prove();
    EXPECT_EQ(kResult, ProverResult::kTheorem);
}

// ═══════════════════════════════════════════════════════════════════════════
// Empty clause in input directly
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(ProverTest, EmptyClauseInInput) {
    Prover prover(bank_, store_);
    prover.addClauses({makeClause({}),  // empty clause directly
                       makeClause({pos(p(a_))})});

    // Empty clause should be detected immediately when selected from queue
    const ProverResult kResult = prover.prove();
    EXPECT_EQ(kResult, ProverResult::kTheorem);
}

}  // namespace
}  // namespace atp
