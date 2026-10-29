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
#include "atp/core/literal.h"
#include "atp/core/symbol_table.h"
#include "atp/core/term_bank.h"
#include "atp/core/types.h"
#include "atp/infer/resolution.h"

#include <gtest/gtest.h>
#include <utility>
#include <vector>

namespace atp {
namespace {

class ResolutionTest : public ::testing::Test {
  protected:
    SymbolTable symbols_;
    TermBank bank_{symbols_};

    SymbolId sym_a_{}, sym_b_{}, sym_c_{};
    SymbolId sym_f_{};
    SymbolId sym_p_{}, sym_q_{}, sym_r_{};
    SymbolId sym_x_{}, sym_y_{}, sym_z_{};

    TermId a_{}, b_{}, c_{};
    TermId x_{}, y_{}, z_{};

    void SetUp() override {
        sym_a_ = symbols_.intern("a", SymbolKind::kConstant);
        sym_b_ = symbols_.intern("b", SymbolKind::kConstant);
        sym_c_ = symbols_.intern("c", SymbolKind::kConstant);
        sym_f_ = symbols_.intern("f", SymbolKind::kFunction, 1);
        sym_p_ = symbols_.intern("P", SymbolKind::kPredicate, 1);
        sym_q_ = symbols_.intern("Q", SymbolKind::kPredicate, 1);
        sym_r_ = symbols_.intern("R", SymbolKind::kPredicate, 2);
        sym_x_ = symbols_.intern("X", SymbolKind::kVariable);
        sym_y_ = symbols_.intern("Y", SymbolKind::kVariable);
        sym_z_ = symbols_.intern("Z", SymbolKind::kVariable);

        a_ = bank_.makeTerm(sym_a_, {});
        b_ = bank_.makeTerm(sym_b_, {});
        c_ = bank_.makeTerm(sym_c_, {});
        x_ = bank_.makeVar(sym_x_);
        y_ = bank_.makeVar(sym_y_);
        z_ = bank_.makeVar(sym_z_);
    }

    TermId p(TermId arg) { return bank_.makeTerm(sym_p_, {{arg}}); }
    TermId q(TermId arg) { return bank_.makeTerm(sym_q_, {{arg}}); }
    TermId r(TermId a1, TermId a2) { return bank_.makeTerm(sym_r_, {{a1, a2}}); }
    TermId f(TermId arg) { return bank_.makeTerm(sym_f_, {{arg}}); }

    // Helper: make a clause from literals
    static Clause makeClause(std::vector<Literal> lits, ClauseId id = kInvalidId) {
        Clause c;
        c.id_ = id;
        c.literals_ = std::move(lits);
        return c;
    }

    static Literal pos(TermId atom) { return {.atom_ = atom, .is_positive_ = true}; }
    static Literal neg(TermId atom) { return {.atom_ = atom, .is_positive_ = false}; }
};

// ═══════════════════════════════════════════════════════════════════════════
// Basic Resolution Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(ResolutionTest, SimpleGroundResolution) {
    // {P(a)} and {¬P(a), Q(b)}  → {Q(b)}
    Clause const c1 = makeClause({pos(p(a_))}, 0);
    Clause const c2 = makeClause({neg(p(a_)), pos(q(b_))}, 1);

    auto result = resolve(bank_, c1, 0, c2, 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->literals_.size(), 1);
    EXPECT_EQ(result->literals_[0].atom_, q(b_));
    EXPECT_TRUE(result->literals_[0].is_positive_);
}

TEST_F(ResolutionTest, ResolutionWithUnification) {
    // {P(X)} and {¬P(a)} → {} (empty clause!)
    Clause const c1 = makeClause({pos(p(x_))}, 0);
    Clause const c2 = makeClause({neg(p(a_))}, 1);

    auto result = resolve(bank_, c1, 0, c2, 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->isEmpty()) << "Should derive empty clause (contradiction)";
}

TEST_F(ResolutionTest, ResolutionFailsSamePolarity) {
    // {P(a)} and {P(b)} → no resolution (both positive)
    Clause const c1 = makeClause({pos(p(a_))}, 0);
    Clause const c2 = makeClause({pos(p(b_))}, 1);

    auto result = resolve(bank_, c1, 0, c2, 0);
    EXPECT_FALSE(result.has_value());
}

TEST_F(ResolutionTest, ResolutionFailsNoUnifier) {
    // {P(a)} and {¬P(b)} → can't unify a with b
    Clause const c1 = makeClause({pos(p(a_))}, 0);
    Clause const c2 = makeClause({neg(p(b_))}, 1);

    auto result = resolve(bank_, c1, 0, c2, 0);
    EXPECT_FALSE(result.has_value());
}

TEST_F(ResolutionTest, ResolutionAppliesSubstToRemainingLiterals) {
    // {P(X), Q(X)} and {¬P(a)} → {Q(a)}
    Clause const c1 = makeClause({pos(p(x_)), pos(q(x_))}, 0);
    Clause const c2 = makeClause({neg(p(a_))}, 1);

    auto result = resolve(bank_, c1, 0, c2, 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->literals_.size(), 1);
    EXPECT_EQ(result->literals_[0].atom_, q(a_));
}

TEST_F(ResolutionTest, ResolutionMultipleLiterals) {
    // {P(X), Q(b)} and {¬P(a), R(a, c)} → {Q(b), R(a, c)}
    Clause const c1 = makeClause({pos(p(x_)), pos(q(b_))}, 0);
    Clause const c2 = makeClause({neg(p(a_)), pos(r(a_, c_))}, 1);

    auto result = resolve(bank_, c1, 0, c2, 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->literals_.size(), 2);
}

TEST_F(ResolutionTest, VariableRenamingWorks) {
    // Both clauses use variable X
    // {P(X)} and {¬P(X), Q(X)} → {Q(a)} only if X is renamed in c2
    Clause const c1 = makeClause({pos(p(a_))}, 0);
    Clause const c2 = makeClause({neg(p(x_)), pos(q(x_))}, 1);

    auto result = resolve(bank_, c1, 0, c2, 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->literals_.size(), 1);
    // Q(X) should become Q(a) after substitution {X→a}
    EXPECT_EQ(result->literals_[0].atom_, q(a_));
}

TEST_F(ResolutionTest, VariableRenamingPreventsClash) {
    // {R(X, a)} and {¬R(b, X)}
    // Without renaming: X would clash between c1 and c2
    // With renaming: c2's X → _R0, then unify R(X,a) with R(b,_R0) → {X→b, _R0→a}
    Clause const c1 = makeClause({pos(r(x_, a_))}, 0);
    Clause const c2 = makeClause({neg(r(b_, x_))}, 1);

    auto result = resolve(bank_, c1, 0, c2, 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->isEmpty());
}

// ═══════════════════════════════════════════════════════════════════════════
// AllResolvents Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(ResolutionTest, AllResolventsFindsAll) {
    // {P(a), Q(a)} and {¬P(a), ¬Q(a)}
    // Can resolve on P or Q → 2 resolvents
    Clause const c1 = makeClause({pos(p(a_)), pos(q(a_))}, 0);
    Clause const c2 = makeClause({neg(p(a_)), neg(q(a_))}, 1);

    auto results = allResolvents(bank_, c1, c2);
    EXPECT_EQ(results.size(), 2);
}

TEST_F(ResolutionTest, AllResolventsEmpty) {
    // {P(a)} and {Q(b)} → no complementary pairs
    Clause const c1 = makeClause({pos(p(a_))}, 0);
    Clause const c2 = makeClause({pos(q(b_))}, 1);

    auto results = allResolvents(bank_, c1, c2);
    EXPECT_TRUE(results.empty());
}

// ═══════════════════════════════════════════════════════════════════════════
// Factoring Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(ResolutionTest, SimpleFactoring) {
    // {P(X), P(a), Q(X)} → factor P(X) with P(a): {P(a), Q(a)}
    Clause const c = makeClause({pos(p(x_)), pos(p(a_)), pos(q(x_))}, 0);

    auto results = factor(bank_, c);
    ASSERT_EQ(results.size(), 1);
    EXPECT_EQ(results[0].literals_.size(), 2);
    // After factoring: {P(a), Q(a)}
    EXPECT_EQ(results[0].literals_[0].atom_, p(a_));
    EXPECT_EQ(results[0].literals_[1].atom_, q(a_));
}

TEST_F(ResolutionTest, NoFactoring) {
    // {P(a), Q(b)} → different predicates, can't factor
    Clause const c = makeClause({pos(p(a_)), pos(q(b_))}, 0);
    auto results = factor(bank_, c);
    EXPECT_TRUE(results.empty());
}

TEST_F(ResolutionTest, FactoringDifferentPolarity) {
    // {P(a), ¬P(a)} → different polarity, can't factor
    Clause const c = makeClause({pos(p(a_)), neg(p(a_))}, 0);
    auto results = factor(bank_, c);
    EXPECT_TRUE(results.empty());
}

TEST_F(ResolutionTest, FactoringNonUnifiable) {
    // {P(a), P(b)} → can't unify a with b
    Clause const c = makeClause({pos(p(a_)), pos(p(b_))}, 0);
    auto results = factor(bank_, c);
    EXPECT_TRUE(results.empty());
}

// ═══════════════════════════════════════════════════════════════════════════
// Provenance Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(ResolutionTest, ResolventHasProvenance) {
    Clause const c1 = makeClause({pos(p(x_))}, 5);
    Clause const c2 = makeClause({neg(p(a_))}, 7);

    auto result = resolve(bank_, c1, 0, c2, 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->rule_, InferenceRule::kResolution);
    EXPECT_EQ(result->parent1_, 5);
    EXPECT_EQ(result->parent2_, 7);
}

TEST_F(ResolutionTest, FactoredHasProvenance) {
    Clause const c = makeClause({pos(p(x_)), pos(p(a_))}, 3);
    auto results = factor(bank_, c);
    ASSERT_FALSE(results.empty());
    EXPECT_EQ(results[0].rule_, InferenceRule::kFactoring);
    EXPECT_EQ(results[0].parent1_, 3);
}

}  // namespace
}  // namespace atp
