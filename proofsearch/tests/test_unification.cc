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

#include "atp/core/symbol_table.h"
#include "atp/core/term_bank.h"
#include "atp/core/types.h"
#include "atp/infer/substitution.h"
#include "atp/infer/unification.h"

namespace atp {
namespace {

// ═══════════════════════════════════════════════════════════════════════════
// Test Fixture
// ═══════════════════════════════════════════════════════════════════════════

class UnificationTest : public ::testing::Test {
  protected:
    SymbolTable symbols;
    TermBank bank{symbols};

    // Symbols
    SymbolId sym_a{}, sym_b{}, sym_c{};
    SymbolId sym_f{}, sym_g{}, sym_h{};
    SymbolId sym_P{};
    SymbolId sym_X{}, sym_Y{}, sym_Z{};

    // Terms (created in SetUp)
    TermId a{}, b{}, c{};
    TermId X{}, Y{}, Z{};

    void SetUp() override {
        sym_a = symbols.intern("a", SymbolKind::kConstant);
        sym_b = symbols.intern("b", SymbolKind::kConstant);
        sym_c = symbols.intern("c", SymbolKind::kConstant);
        sym_f = symbols.intern("f", SymbolKind::kFunction, 1);
        sym_g = symbols.intern("g", SymbolKind::kFunction, 2);
        sym_h = symbols.intern("h", SymbolKind::kFunction, 1);
        sym_P = symbols.intern("P", SymbolKind::kPredicate, 2);
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

    // Helper: make f(arg)
    TermId f(TermId arg) { return bank.makeTerm(sym_f, {{arg}}); }
    // Helper: make g(arg1, arg2)
    TermId g(TermId a1, TermId a2) { return bank.makeTerm(sym_g, {{a1, a2}}); }
    // Helper: make h(arg)
    TermId h(TermId arg) { return bank.makeTerm(sym_h, {{arg}}); }
    // Helper: make P(arg1, arg2)
    TermId P(TermId a1, TermId a2) { return bank.makeTerm(sym_P, {{a1, a2}}); }
};

// ═══════════════════════════════════════════════════════════════════════════
// Substitution Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(UnificationTest, SubstBindAndLookup) {
    Substitution subst;
    EXPECT_TRUE(subst.bind(X, a));
    EXPECT_EQ(subst.lookup(X), a);
    EXPECT_EQ(subst.lookup(Y), kInvalidId);  // unbound
}

TEST_F(UnificationTest, SubstBindSameTermSucceeds) {
    Substitution subst;
    EXPECT_TRUE(subst.bind(X, a));
    EXPECT_TRUE(subst.bind(X, a));  // same binding → OK
}

TEST_F(UnificationTest, SubstBindDifferentTermFails) {
    Substitution subst;
    EXPECT_TRUE(subst.bind(X, a));
    EXPECT_FALSE(subst.bind(X, b));  // different binding → fail
}

TEST_F(UnificationTest, SubstResolveChain) {
    // X → Y → a
    Substitution subst;
    subst.bind(X, Y);
    subst.bind(Y, a);
    EXPECT_EQ(subst.resolve(X, bank), a);
}

TEST_F(UnificationTest, SubstResolveUnbound) {
    Substitution subst;
    EXPECT_EQ(subst.resolve(X, bank), X);  // unbound → returns itself
}

TEST_F(UnificationTest, SubstResolveNonVariable) {
    Substitution subst;
    EXPECT_EQ(subst.resolve(a, bank), a);  // constant → returns itself
}

TEST_F(UnificationTest, SubstApplySimple) {
    // σ = {X → a}    apply to f(X) → f(a)
    Substitution subst;
    subst.bind(X, a);
    TermId fx = f(X);
    TermId result = subst.apply(fx, bank);
    TermId expected = f(a);
    EXPECT_EQ(result, expected);
}

TEST_F(UnificationTest, SubstApplyLeavesUnbound) {
    // σ = {X → a}    apply to g(X, Y) → g(a, Y)
    Substitution subst;
    subst.bind(X, a);
    TermId gxy = g(X, Y);
    TermId result = subst.apply(gxy, bank);
    TermId expected = g(a, Y);
    EXPECT_EQ(result, expected);
}

TEST_F(UnificationTest, SubstApplyNested) {
    // σ = {X → f(a)}    apply to g(X, b) → g(f(a), b)
    Substitution subst;
    subst.bind(X, f(a));
    TermId result = subst.apply(g(X, b), bank);
    EXPECT_EQ(result, g(f(a), b));
}

TEST_F(UnificationTest, SubstOccursIn) {
    Substitution subst;
    // X occurs in f(X)
    EXPECT_TRUE(subst.occursIn(X, f(X), bank));
    // X occurs in g(a, f(X))
    EXPECT_TRUE(subst.occursIn(X, g(a, f(X)), bank));
    // X does NOT occur in f(a)
    EXPECT_FALSE(subst.occursIn(X, f(a), bank));
    // X does NOT occur in Y (different variable)
    EXPECT_FALSE(subst.occursIn(X, Y, bank));
}

TEST_F(UnificationTest, SubstClearAndSize) {
    Substitution subst;
    EXPECT_TRUE(subst.empty());
    subst.bind(X, a);
    subst.bind(Y, b);
    EXPECT_EQ(subst.size(), 2);
    subst.clear();
    EXPECT_TRUE(subst.empty());
    EXPECT_EQ(subst.lookup(X), kInvalidId);
}

// ═══════════════════════════════════════════════════════════════════════════
// Unification Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(UnificationTest, UnifyIdenticalConstants) {
    Substitution subst;
    EXPECT_TRUE(unify(bank, a, a, subst));
    EXPECT_TRUE(subst.empty());
}

TEST_F(UnificationTest, UnifyDifferentConstants) {
    Substitution subst;
    EXPECT_FALSE(unify(bank, a, b, subst));
}

TEST_F(UnificationTest, UnifyVarWithConstant) {
    Substitution subst;
    EXPECT_TRUE(unify(bank, X, a, subst));
    EXPECT_EQ(subst.resolve(X, bank), a);
}

TEST_F(UnificationTest, UnifyConstantWithVar) {
    // Symmetric: unify(a, X) should also work
    Substitution subst;
    EXPECT_TRUE(unify(bank, a, X, subst));
    EXPECT_EQ(subst.resolve(X, bank), a);
}

TEST_F(UnificationTest, UnifyTwoVariables) {
    Substitution subst;
    EXPECT_TRUE(unify(bank, X, Y, subst));
    // One should be bound to the other
    TermId rx = subst.resolve(X, bank);
    TermId ry = subst.resolve(Y, bank);
    EXPECT_EQ(rx, ry);
}

TEST_F(UnificationTest, UnifyCompoundTerms) {
    // f(X) =? f(a)  → {X → a}
    Substitution subst;
    EXPECT_TRUE(unify(bank, f(X), f(a), subst));
    EXPECT_EQ(subst.resolve(X, bank), a);
}

TEST_F(UnificationTest, UnifyMultipleArgs) {
    // g(X, Y) =? g(a, b)  → {X → a, Y → b}
    Substitution subst;
    EXPECT_TRUE(unify(bank, g(X, Y), g(a, b), subst));
    EXPECT_EQ(subst.resolve(X, bank), a);
    EXPECT_EQ(subst.resolve(Y, bank), b);
}

TEST_F(UnificationTest, UnifyConflictingBindings) {
    // g(X, X) =? g(a, b)  → fails (X can't be both a and b)
    Substitution subst;
    EXPECT_FALSE(unify(bank, g(X, X), g(a, b), subst));
}

TEST_F(UnificationTest, UnifyConsistentRepeatedVar) {
    // g(X, X) =? g(a, a)  → {X → a}
    Substitution subst;
    EXPECT_TRUE(unify(bank, g(X, X), g(a, a), subst));
    EXPECT_EQ(subst.resolve(X, bank), a);
}

TEST_F(UnificationTest, UnifyDifferentHeadSymbols) {
    // f(X) =? h(X) → fails
    Substitution subst;
    EXPECT_FALSE(unify(bank, f(X), h(X), subst));
}

TEST_F(UnificationTest, UnifyDifferentArity) {
    // f(X) =? g(X, Y) → fails (arity 1 vs 2)
    Substitution subst;
    EXPECT_FALSE(unify(bank, f(X), g(X, Y), subst));
}

TEST_F(UnificationTest, UnifyNestedTerms) {
    // f(g(X, a)) =? f(Y)  → {Y → g(X, a)}
    Substitution subst;
    EXPECT_TRUE(unify(bank, f(g(X, a)), f(Y), subst));
    EXPECT_EQ(subst.resolve(Y, bank), g(X, a));
}

TEST_F(UnificationTest, UnifyVarWithCompound) {
    // X =? f(a) → {X → f(a)}
    Substitution subst;
    EXPECT_TRUE(unify(bank, X, f(a), subst));
    EXPECT_EQ(subst.resolve(X, bank), f(a));
}

TEST_F(UnificationTest, UnifyDeepNesting) {
    // f(f(f(X))) =? f(f(f(a))) → {X → a}
    Substitution subst;
    EXPECT_TRUE(unify(bank, f(f(f(X))), f(f(f(a))), subst));
    EXPECT_EQ(subst.resolve(X, bank), a);
}

TEST_F(UnificationTest, UnifyChainedVariables) {
    // g(X, Y) =? g(Y, a) → {X → Y, Y → a} effectively X → a
    Substitution subst;
    EXPECT_TRUE(unify(bank, g(X, Y), g(Y, a), subst));
    EXPECT_EQ(subst.resolve(X, bank), a);
    EXPECT_EQ(subst.resolve(Y, bank), a);
}

// ═══════════════════════════════════════════════════════════════════════════
// Occurs Check Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(UnificationTest, OccursCheckBlocks) {
    // X =? f(X) with occurs check → fails
    Substitution subst;
    UnificationConfig config{.enable_occurs_check = true};
    EXPECT_FALSE(unify(bank, X, f(X), subst, config));
}

TEST_F(UnificationTest, OccursCheckDisabledAllows) {
    // X =? f(X) without occurs check → succeeds (unsound but fast)
    Substitution subst;
    UnificationConfig config{.enable_occurs_check = false};
    EXPECT_TRUE(unify(bank, X, f(X), subst, config));
}

TEST_F(UnificationTest, OccursCheckDeep) {
    // X =? f(g(X, a)) with occurs check → fails
    Substitution subst;
    UnificationConfig config{.enable_occurs_check = true};
    EXPECT_FALSE(unify(bank, X, f(g(X, a)), subst, config));
}

// ═══════════════════════════════════════════════════════════════════════════
// applySubstitution Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(UnificationTest, ApplySubstitutionSimple) {
    Substitution subst;
    subst.bind(X, a);
    EXPECT_EQ(applySubstitution(subst, f(X), bank), f(a));
}

TEST_F(UnificationTest, ApplySubstitutionNested) {
    Substitution subst;
    subst.bind(X, a);
    subst.bind(Y, b);
    EXPECT_EQ(applySubstitution(subst, g(f(X), Y), bank), g(f(a), b));
}

TEST_F(UnificationTest, ApplySubstitutionPreservesUnbound) {
    Substitution subst;
    subst.bind(X, a);
    // Y is unbound → stays as Y
    EXPECT_EQ(applySubstitution(subst, g(X, Y), bank), g(a, Y));
}

TEST_F(UnificationTest, ApplySubstitutionToConstant) {
    Substitution subst;
    subst.bind(X, a);
    // Constant is unchanged
    EXPECT_EQ(applySubstitution(subst, b, bank), b);
}

TEST_F(UnificationTest, ApplySubstitutionChain) {
    // σ = {X → Y, Y → a}   apply to f(X) → f(a)
    Substitution subst;
    subst.bind(X, Y);
    subst.bind(Y, a);
    EXPECT_EQ(applySubstitution(subst, f(X), bank), f(a));
}

TEST_F(UnificationTest, UnifyThenApply) {
    // Full round-trip: unify P(X, f(Y)) with P(a, f(b)), then apply
    Substitution subst;
    TermId lhs = P(X, f(Y));
    TermId rhs = P(a, f(b));
    ASSERT_TRUE(unify(bank, lhs, rhs, subst));

    EXPECT_EQ(applySubstitution(subst, lhs, bank), P(a, f(b)));
    EXPECT_EQ(applySubstitution(subst, rhs, bank), P(a, f(b)));
    // Both sides should produce the same term after applying the MGU
    EXPECT_EQ(applySubstitution(subst, lhs, bank),
              applySubstitution(subst, rhs, bank));
}

}  // namespace
}  // namespace atp
