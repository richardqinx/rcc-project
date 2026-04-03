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
#include "atp/infer/substitution.h"
#include "atp/infer/unification.h"

#include <gtest/gtest.h>

namespace atp {
namespace {

class ResolutionTest : public ::testing::Test {
  protected:
    SymbolTable symbols;
    TermBank bank{symbols};

    SymbolId sym_a{}, sym_b{}, sym_c{};
    SymbolId sym_f{};
    SymbolId sym_P{}, sym_Q{}, sym_R{};
    SymbolId sym_X{}, sym_Y{}, sym_Z{};

    TermId a{}, b{}, c{};
    TermId X{}, Y{}, Z{};

    void SetUp() override {
        sym_a = symbols.intern("a", SymbolKind::kConstant);
        sym_b = symbols.intern("b", SymbolKind::kConstant);
        sym_c = symbols.intern("c", SymbolKind::kConstant);
        sym_f = symbols.intern("f", SymbolKind::kFunction, 1);
        sym_P = symbols.intern("P", SymbolKind::kPredicate, 1);
        sym_Q = symbols.intern("Q", SymbolKind::kPredicate, 1);
        sym_R = symbols.intern("R", SymbolKind::kPredicate, 2);
        sym_X = symbols.intern("X", SymbolKind::kVariable);
        sym_Y = symbols.intern("Y", SymbolKind::kVariable);
        sym_Z = symbols.intern("Z", SymbolKind::kVariable);

        a = bank.makeTerm(sym_a, {});
        b = bank.makeTerm(sym_b, {});
        c = bank.makeTerm(sym_c, {});
        X = bank.makeVar(sym_X);
        Y = bank.makeVar(sym_Y);
        Z = bank.makeVar(sym_Z);
    }

    TermId P(TermId arg) { return bank.makeTerm(sym_P, {{arg}}); }
    TermId Q(TermId arg) { return bank.makeTerm(sym_Q, {{arg}}); }
    TermId R(TermId a1, TermId a2) { return bank.makeTerm(sym_R, {{a1, a2}}); }
    TermId f(TermId arg) { return bank.makeTerm(sym_f, {{arg}}); }

    // Helper: make a clause from literals
    Clause makeClause(std::vector<Literal> lits, ClauseId id = kInvalidId) {
        Clause c;
        c.id = id;
        c.literals = std::move(lits);
        return c;
    }

    Literal pos(TermId atom) { return {.atom = atom, .is_positive = true}; }
    Literal neg(TermId atom) { return {.atom = atom, .is_positive = false}; }
};

// ═══════════════════════════════════════════════════════════════════════════
// Basic Resolution Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(ResolutionTest, SimpleGroundResolution) {
    // {P(a)} and {¬P(a), Q(b)}  → {Q(b)}
    Clause c1 = makeClause({pos(P(a))}, 0);
    Clause c2 = makeClause({neg(P(a)), pos(Q(b))}, 1);

    auto result = resolve(bank, c1, 0, c2, 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->literals.size(), 1);
    EXPECT_EQ(result->literals[0].atom, Q(b));
    EXPECT_TRUE(result->literals[0].is_positive);
}

TEST_F(ResolutionTest, ResolutionWithUnification) {
    // {P(X)} and {¬P(a)} → {} (empty clause!)
    Clause c1 = makeClause({pos(P(X))}, 0);
    Clause c2 = makeClause({neg(P(a))}, 1);

    auto result = resolve(bank, c1, 0, c2, 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->isEmpty()) << "Should derive empty clause (contradiction)";
}

TEST_F(ResolutionTest, ResolutionFailsSamePolarity) {
    // {P(a)} and {P(b)} → no resolution (both positive)
    Clause c1 = makeClause({pos(P(a))}, 0);
    Clause c2 = makeClause({pos(P(b))}, 1);

    auto result = resolve(bank, c1, 0, c2, 0);
    EXPECT_FALSE(result.has_value());
}

TEST_F(ResolutionTest, ResolutionFailsNoUnifier) {
    // {P(a)} and {¬P(b)} → can't unify a with b
    Clause c1 = makeClause({pos(P(a))}, 0);
    Clause c2 = makeClause({neg(P(b))}, 1);

    auto result = resolve(bank, c1, 0, c2, 0);
    EXPECT_FALSE(result.has_value());
}

TEST_F(ResolutionTest, ResolutionAppliesSubstToRemainingLiterals) {
    // {P(X), Q(X)} and {¬P(a)} → {Q(a)}
    Clause c1 = makeClause({pos(P(X)), pos(Q(X))}, 0);
    Clause c2 = makeClause({neg(P(a))}, 1);

    auto result = resolve(bank, c1, 0, c2, 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->literals.size(), 1);
    EXPECT_EQ(result->literals[0].atom, Q(a));
}

TEST_F(ResolutionTest, ResolutionMultipleLiterals) {
    // {P(X), Q(b)} and {¬P(a), R(a, c)} → {Q(b), R(a, c)}
    Clause c1 = makeClause({pos(P(X)), pos(Q(b))}, 0);
    Clause c2 = makeClause({neg(P(a)), pos(R(a, c))}, 1);

    auto result = resolve(bank, c1, 0, c2, 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->literals.size(), 2);
}

TEST_F(ResolutionTest, VariableRenamingWorks) {
    // Both clauses use variable X
    // {P(X)} and {¬P(X), Q(X)} → {Q(a)} only if X is renamed in c2
    Clause c1 = makeClause({pos(P(a))}, 0);
    Clause c2 = makeClause({neg(P(X)), pos(Q(X))}, 1);

    auto result = resolve(bank, c1, 0, c2, 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->literals.size(), 1);
    // Q(X) should become Q(a) after substitution {X→a}
    EXPECT_EQ(result->literals[0].atom, Q(a));
}

TEST_F(ResolutionTest, VariableRenamingPreventsClash) {
    // {R(X, a)} and {¬R(b, X)}
    // Without renaming: X would clash between c1 and c2
    // With renaming: c2's X → _R0, then unify R(X,a) with R(b,_R0) → {X→b, _R0→a}
    Clause c1 = makeClause({pos(R(X, a))}, 0);
    Clause c2 = makeClause({neg(R(b, X))}, 1);

    auto result = resolve(bank, c1, 0, c2, 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->isEmpty());
}

// ═══════════════════════════════════════════════════════════════════════════
// AllResolvents Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(ResolutionTest, AllResolventsFindsAll) {
    // {P(a), Q(a)} and {¬P(a), ¬Q(a)}
    // Can resolve on P or Q → 2 resolvents
    Clause c1 = makeClause({pos(P(a)), pos(Q(a))}, 0);
    Clause c2 = makeClause({neg(P(a)), neg(Q(a))}, 1);

    auto results = allResolvents(bank, c1, c2);
    EXPECT_EQ(results.size(), 2);
}

TEST_F(ResolutionTest, AllResolventsEmpty) {
    // {P(a)} and {Q(b)} → no complementary pairs
    Clause c1 = makeClause({pos(P(a))}, 0);
    Clause c2 = makeClause({pos(Q(b))}, 1);

    auto results = allResolvents(bank, c1, c2);
    EXPECT_TRUE(results.empty());
}

// ═══════════════════════════════════════════════════════════════════════════
// Factoring Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(ResolutionTest, SimpleFactoring) {
    // {P(X), P(a), Q(X)} → factor P(X) with P(a): {P(a), Q(a)}
    Clause c = makeClause({pos(P(X)), pos(P(a)), pos(Q(X))}, 0);

    auto results = factor(bank, c);
    ASSERT_EQ(results.size(), 1);
    EXPECT_EQ(results[0].literals.size(), 2);
    // After factoring: {P(a), Q(a)}
    EXPECT_EQ(results[0].literals[0].atom, P(a));
    EXPECT_EQ(results[0].literals[1].atom, Q(a));
}

TEST_F(ResolutionTest, NoFactoring) {
    // {P(a), Q(b)} → different predicates, can't factor
    Clause c = makeClause({pos(P(a)), pos(Q(b))}, 0);
    auto results = factor(bank, c);
    EXPECT_TRUE(results.empty());
}

TEST_F(ResolutionTest, FactoringDifferentPolarity) {
    // {P(a), ¬P(a)} → different polarity, can't factor
    Clause c = makeClause({pos(P(a)), neg(P(a))}, 0);
    auto results = factor(bank, c);
    EXPECT_TRUE(results.empty());
}

TEST_F(ResolutionTest, FactoringNonUnifiable) {
    // {P(a), P(b)} → can't unify a with b
    Clause c = makeClause({pos(P(a)), pos(P(b))}, 0);
    auto results = factor(bank, c);
    EXPECT_TRUE(results.empty());
}

// ═══════════════════════════════════════════════════════════════════════════
// Provenance Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(ResolutionTest, ResolventHasProvenance) {
    Clause c1 = makeClause({pos(P(X))}, 5);
    Clause c2 = makeClause({neg(P(a))}, 7);

    auto result = resolve(bank, c1, 0, c2, 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->rule, InferenceRule::kResolution);
    EXPECT_EQ(result->parent1, 5);
    EXPECT_EQ(result->parent2, 7);
}

TEST_F(ResolutionTest, FactoredHasProvenance) {
    Clause c = makeClause({pos(P(X)), pos(P(a))}, 3);
    auto results = factor(bank, c);
    ASSERT_FALSE(results.empty());
    EXPECT_EQ(results[0].rule, InferenceRule::kFactoring);
    EXPECT_EQ(results[0].parent1, 3);
}

}  // namespace
}  // namespace atp
