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

#include "atp/core/symbol_table.h"
#include "atp/core/term_bank.h"
#include "atp/core/types.h"
#include "atp/infer/substitution.h"
#include "atp/infer/unification.h"

#include <gtest/gtest.h>

namespace atp {
namespace {

// ═══════════════════════════════════════════════════════════════════════════
// Test Fixture
// ═══════════════════════════════════════════════════════════════════════════

class UnificationTest : public ::testing::Test {
  protected:
    SymbolTable symbols_;
    TermBank bank_{symbols_};

    // Symbols
    SymbolId sym_a_{}, sym_b_{}, sym_c_{};
    SymbolId sym_f_{}, sym_g_{}, sym_h_{};
    SymbolId sym_p_{};
    SymbolId sym_x_{}, sym_y_{}, sym_z_{};

    // Terms (created in SetUp)
    TermId a_{}, b_{}, c_{};
    TermId x_{}, y_{}, z_{};

    void SetUp() override {
        sym_a_ = symbols_.intern("a", SymbolKind::kConstant);
        sym_b_ = symbols_.intern("b", SymbolKind::kConstant);
        sym_c_ = symbols_.intern("c", SymbolKind::kConstant);
        sym_f_ = symbols_.intern("f", SymbolKind::kFunction, 1);
        sym_g_ = symbols_.intern("g", SymbolKind::kFunction, 2);
        sym_h_ = symbols_.intern("h", SymbolKind::kFunction, 1);
        sym_p_ = symbols_.intern("P", SymbolKind::kPredicate, 2);
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

    // Helper: make f(arg)
    TermId f(TermId arg) { return bank_.makeTerm(sym_f_, {{arg}}); }
    // Helper: make g(arg1, arg2)
    TermId g(TermId a1, TermId a2) { return bank_.makeTerm(sym_g_, {{a1, a2}}); }
    // Helper: make h(arg)
    TermId h(TermId arg) { return bank_.makeTerm(sym_h_, {{arg}}); }
    // Helper: make P(arg1, arg2)
    TermId p(TermId a1, TermId a2) { return bank_.makeTerm(sym_p_, {{a1, a2}}); }
};

// ═══════════════════════════════════════════════════════════════════════════
// Substitution Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(UnificationTest, SubstBindAndLookup) {
    Substitution subst;
    EXPECT_TRUE(subst.bind(x_, a_));
    EXPECT_EQ(subst.lookup(x_), a_);
    EXPECT_EQ(subst.lookup(y_), kInvalidId);  // unbound
}

TEST_F(UnificationTest, SubstBindSameTermSucceeds) {
    Substitution subst;
    EXPECT_TRUE(subst.bind(x_, a_));
    EXPECT_TRUE(subst.bind(x_, a_));  // same binding → OK
}

TEST_F(UnificationTest, SubstBindDifferentTermFails) {
    Substitution subst;
    EXPECT_TRUE(subst.bind(x_, a_));
    EXPECT_FALSE(subst.bind(x_, b_));  // different binding → fail
}

TEST_F(UnificationTest, SubstResolveChain) {
    // X → Y → a
    Substitution subst;
    subst.bind(x_, y_);
    subst.bind(y_, a_);
    EXPECT_EQ(subst.resolve(x_, bank_), a_);
}

TEST_F(UnificationTest, SubstResolveUnbound) {
    Substitution const subst;
    EXPECT_EQ(subst.resolve(x_, bank_), x_);  // unbound → returns itself
}

TEST_F(UnificationTest, SubstResolveNonVariable) {
    Substitution const subst;
    EXPECT_EQ(subst.resolve(a_, bank_), a_);  // constant → returns itself
}

TEST_F(UnificationTest, SubstApplySimple) {
    // σ = {X → a}    apply to f(X) → f(a)
    Substitution subst;
    subst.bind(x_, a_);
    TermId const fx = f(x_);
    TermId const result = subst.apply(fx, bank_);
    TermId const expected = f(a_);
    EXPECT_EQ(result, expected);
}

TEST_F(UnificationTest, SubstApplyLeavesUnbound) {
    // σ = {X → a}    apply to g(X, Y) → g(a, Y)
    Substitution subst;
    subst.bind(x_, a_);
    TermId const gxy = g(x_, y_);
    TermId const result = subst.apply(gxy, bank_);
    TermId const expected = g(a_, y_);
    EXPECT_EQ(result, expected);
}

TEST_F(UnificationTest, SubstApplyNested) {
    // σ = {X → f(a)}    apply to g(X, b) → g(f(a), b)
    Substitution subst;
    subst.bind(x_, f(a_));
    TermId const result = subst.apply(g(x_, b_), bank_);
    EXPECT_EQ(result, g(f(a_), b_));
}

TEST_F(UnificationTest, SubstOccursIn) {
    Substitution const subst;
    // X occurs in f(X)
    EXPECT_TRUE(subst.occursIn(x_, f(x_), bank_));
    // X occurs in g(a, f(X))
    EXPECT_TRUE(subst.occursIn(x_, g(a_, f(x_)), bank_));
    // X does NOT occur in f(a)
    EXPECT_FALSE(subst.occursIn(x_, f(a_), bank_));
    // X does NOT occur in Y (different variable)
    EXPECT_FALSE(subst.occursIn(x_, y_, bank_));
}

TEST_F(UnificationTest, SubstClearAndSize) {
    Substitution subst;
    EXPECT_TRUE(subst.empty());
    subst.bind(x_, a_);
    subst.bind(y_, b_);
    EXPECT_EQ(subst.size(), 2);
    subst.clear();
    EXPECT_TRUE(subst.empty());
    EXPECT_EQ(subst.lookup(x_), kInvalidId);
}

// ═══════════════════════════════════════════════════════════════════════════
// Unification Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(UnificationTest, UnifyIdenticalConstants) {
    Substitution subst;
    EXPECT_TRUE(unify(bank_, a_, a_, subst));
    EXPECT_TRUE(subst.empty());
}

TEST_F(UnificationTest, UnifyDifferentConstants) {
    Substitution subst;
    EXPECT_FALSE(unify(bank_, a_, b_, subst));
}

TEST_F(UnificationTest, UnifyVarWithConstant) {
    Substitution subst;
    EXPECT_TRUE(unify(bank_, x_, a_, subst));
    EXPECT_EQ(subst.resolve(x_, bank_), a_);
}

TEST_F(UnificationTest, UnifyConstantWithVar) {
    // Symmetric: unify(a, X) should also work
    Substitution subst;
    EXPECT_TRUE(unify(bank_, a_, x_, subst));
    EXPECT_EQ(subst.resolve(x_, bank_), a_);
}

TEST_F(UnificationTest, UnifyTwoVariables) {
    Substitution subst;
    EXPECT_TRUE(unify(bank_, x_, y_, subst));
    // One should be bound to the other
    TermId const rx = subst.resolve(x_, bank_);
    TermId const ry = subst.resolve(y_, bank_);
    EXPECT_EQ(rx, ry);
}

TEST_F(UnificationTest, UnifyCompoundTerms) {
    // f(X) =? f(a)  → {X → a}
    Substitution subst;
    EXPECT_TRUE(unify(bank_, f(x_), f(a_), subst));
    EXPECT_EQ(subst.resolve(x_, bank_), a_);
}

TEST_F(UnificationTest, UnifyMultipleArgs) {
    // g(X, Y) =? g(a, b)  → {X → a, Y → b}
    Substitution subst;
    EXPECT_TRUE(unify(bank_, g(x_, y_), g(a_, b_), subst));
    EXPECT_EQ(subst.resolve(x_, bank_), a_);
    EXPECT_EQ(subst.resolve(y_, bank_), b_);
}

TEST_F(UnificationTest, UnifyConflictingBindings) {
    // g(X, X) =? g(a, b)  → fails (X can't be both a and b)
    Substitution subst;
    EXPECT_FALSE(unify(bank_, g(x_, x_), g(a_, b_), subst));
}

TEST_F(UnificationTest, UnifyConsistentRepeatedVar) {
    // g(X, X) =? g(a, a)  → {X → a}
    Substitution subst;
    EXPECT_TRUE(unify(bank_, g(x_, x_), g(a_, a_), subst));
    EXPECT_EQ(subst.resolve(x_, bank_), a_);
}

TEST_F(UnificationTest, UnifyDifferentHeadSymbols) {
    // f(X) =? h(X) → fails
    Substitution subst;
    EXPECT_FALSE(unify(bank_, f(x_), h(x_), subst));
}

TEST_F(UnificationTest, UnifyDifferentArity) {
    // f(X) =? g(X, Y) → fails (arity 1 vs 2)
    Substitution subst;
    EXPECT_FALSE(unify(bank_, f(x_), g(x_, y_), subst));
}

TEST_F(UnificationTest, UnifyNestedTerms) {
    // f(g(X, a)) =? f(Y)  → {Y → g(X, a)}
    Substitution subst;
    EXPECT_TRUE(unify(bank_, f(g(x_, a_)), f(y_), subst));
    EXPECT_EQ(subst.resolve(y_, bank_), g(x_, a_));
}

TEST_F(UnificationTest, UnifyVarWithCompound) {
    // X =? f(a) → {X → f(a)}
    Substitution subst;
    EXPECT_TRUE(unify(bank_, x_, f(a_), subst));
    EXPECT_EQ(subst.resolve(x_, bank_), f(a_));
}

TEST_F(UnificationTest, UnifyDeepNesting) {
    // f(f(f(X))) =? f(f(f(a))) → {X → a}
    Substitution subst;
    EXPECT_TRUE(unify(bank_, f(f(f(x_))), f(f(f(a_))), subst));
    EXPECT_EQ(subst.resolve(x_, bank_), a_);
}

TEST_F(UnificationTest, UnifyChainedVariables) {
    // g(X, Y) =? g(Y, a) → {X → Y, Y → a} effectively X → a
    Substitution subst;
    EXPECT_TRUE(unify(bank_, g(x_, y_), g(y_, a_), subst));
    EXPECT_EQ(subst.resolve(x_, bank_), a_);
    EXPECT_EQ(subst.resolve(y_, bank_), a_);
}

// ═══════════════════════════════════════════════════════════════════════════
// Occurs Check Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(UnificationTest, OccursCheckBlocks) {
    // X =? f(X) with occurs check → fails
    Substitution subst;
    UnificationConfig const config{.enable_occurs_check_ = true};
    EXPECT_FALSE(unify(bank_, x_, f(x_), subst, config));
}

TEST_F(UnificationTest, OccursCheckDisabledAllows) {
    // X =? f(X) without occurs check → succeeds (unsound but fast)
    Substitution subst;
    UnificationConfig const config{.enable_occurs_check_ = false};
    EXPECT_TRUE(unify(bank_, x_, f(x_), subst, config));
}

TEST_F(UnificationTest, OccursCheckDeep) {
    // X =? f(g(X, a)) with occurs check → fails
    Substitution subst;
    UnificationConfig const config{.enable_occurs_check_ = true};
    EXPECT_FALSE(unify(bank_, x_, f(g(x_, a_)), subst, config));
}

// ═══════════════════════════════════════════════════════════════════════════
// applySubstitution Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(UnificationTest, ApplySubstitutionSimple) {
    Substitution subst;
    subst.bind(x_, a_);
    EXPECT_EQ(applySubstitution(subst, f(x_), bank_), f(a_));
}

TEST_F(UnificationTest, ApplySubstitutionNested) {
    Substitution subst;
    subst.bind(x_, a_);
    subst.bind(y_, b_);
    EXPECT_EQ(applySubstitution(subst, g(f(x_), y_), bank_), g(f(a_), b_));
}

TEST_F(UnificationTest, ApplySubstitutionPreservesUnbound) {
    Substitution subst;
    subst.bind(x_, a_);
    // Y is unbound → stays as Y
    EXPECT_EQ(applySubstitution(subst, g(x_, y_), bank_), g(a_, y_));
}

TEST_F(UnificationTest, ApplySubstitutionToConstant) {
    Substitution subst;
    subst.bind(x_, a_);
    // Constant is unchanged
    EXPECT_EQ(applySubstitution(subst, b_, bank_), b_);
}

TEST_F(UnificationTest, ApplySubstitutionChain) {
    // σ = {X → Y, Y → a}   apply to f(X) → f(a)
    Substitution subst;
    subst.bind(x_, y_);
    subst.bind(y_, a_);
    EXPECT_EQ(applySubstitution(subst, f(x_), bank_), f(a_));
}

TEST_F(UnificationTest, UnifyThenApply) {
    // Full round-trip: unify P(X, f(Y)) with P(a, f(b)), then apply
    Substitution subst;
    TermId const lhs = p(x_, f(y_));
    TermId const rhs = p(a_, f(b_));
    ASSERT_TRUE(unify(bank_, lhs, rhs, subst));

    EXPECT_EQ(applySubstitution(subst, lhs, bank_), p(a_, f(b_)));
    EXPECT_EQ(applySubstitution(subst, rhs, bank_), p(a_, f(b_)));
    // Both sides should produce the same term after applying the MGU
    EXPECT_EQ(applySubstitution(subst, lhs, bank_), applySubstitution(subst, rhs, bank_));
}

}  // namespace
}  // namespace atp
