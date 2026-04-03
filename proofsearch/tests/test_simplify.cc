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
#include "atp/core/literal.h"
#include "atp/core/symbol_table.h"
#include "atp/core/term_bank.h"
#include "atp/core/types.h"
#include "atp/simplify/subsumption.h"
#include "atp/simplify/tautology.h"

namespace atp {
namespace {

class SimplifyTest : public ::testing::Test {
  protected:
    SymbolTable symbols;
    TermBank bank{symbols};

    SymbolId sym_a{}, sym_b{}, sym_c{};
    SymbolId sym_P{}, sym_Q{}, sym_R{};
    SymbolId sym_X{};

    TermId a{}, b{}, c{}, X{};

    void SetUp() override {
        sym_a = symbols.intern("a", SymbolKind::kConstant);
        sym_b = symbols.intern("b", SymbolKind::kConstant);
        sym_c = symbols.intern("c", SymbolKind::kConstant);
        sym_P = symbols.intern("P", SymbolKind::kPredicate, 1);
        sym_Q = symbols.intern("Q", SymbolKind::kPredicate, 1);
        sym_R = symbols.intern("R", SymbolKind::kPredicate, 2);
        sym_X = symbols.intern("X", SymbolKind::kVariable);

        a = bank.makeTerm(sym_a, {});
        b = bank.makeTerm(sym_b, {});
        c = bank.makeTerm(sym_c, {});
        X = bank.makeVar(sym_X);
    }

    TermId P(TermId arg) { return bank.makeTerm(sym_P, {{arg}}); }
    TermId Q(TermId arg) { return bank.makeTerm(sym_Q, {{arg}}); }
    TermId R(TermId a1, TermId a2) { return bank.makeTerm(sym_R, {{a1, a2}}); }

    Literal pos(TermId atom) { return {.atom = atom, .is_positive = true}; }
    Literal neg(TermId atom) { return {.atom = atom, .is_positive = false}; }

    Clause makeClause(std::vector<Literal> lits) {
        Clause cl;
        cl.literals = std::move(lits);
        return cl;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// Tautology Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(SimplifyTest, TautologyComplementaryLiterals) {
    // {P(a), ¬P(a)} → tautology
    Clause cl = makeClause({pos(P(a)), neg(P(a))});
    EXPECT_TRUE(isTautology(cl));
}

TEST_F(SimplifyTest, TautologyWithExtraLiterals) {
    // {Q(b), P(a), ¬P(a)} → tautology
    Clause cl = makeClause({pos(Q(b)), pos(P(a)), neg(P(a))});
    EXPECT_TRUE(isTautology(cl));
}

TEST_F(SimplifyTest, NotTautologySamePolarity) {
    // {P(a), P(a)} → NOT tautology (same polarity)
    Clause cl = makeClause({pos(P(a)), pos(P(a))});
    EXPECT_FALSE(isTautology(cl));
}

TEST_F(SimplifyTest, NotTautologyDifferentAtoms) {
    // {P(a), ¬P(b)} → NOT tautology (different atoms)
    Clause cl = makeClause({pos(P(a)), neg(P(b))});
    EXPECT_FALSE(isTautology(cl));
}

TEST_F(SimplifyTest, NotTautologyDifferentPredicates) {
    // {P(a), ¬Q(a)} → NOT tautology (different predicates)
    Clause cl = makeClause({pos(P(a)), neg(Q(a))});
    EXPECT_FALSE(isTautology(cl));
}

TEST_F(SimplifyTest, NotTautologyEmptyClause) {
    Clause cl = makeClause({});
    EXPECT_FALSE(isTautology(cl));
}

TEST_F(SimplifyTest, NotTautologySingleLiteral) {
    Clause cl = makeClause({pos(P(a))});
    EXPECT_FALSE(isTautology(cl));
}

TEST_F(SimplifyTest, TautologyVariableAtom) {
    // {P(X), ¬P(X)} → tautology (same variable term)
    Clause cl = makeClause({pos(P(X)), neg(P(X))});
    EXPECT_TRUE(isTautology(cl));
}

TEST_F(SimplifyTest, NotTautologyVarVsConst) {
    // {P(X), ¬P(a)} → NOT tautology (syntactically different)
    Clause cl = makeClause({pos(P(X)), neg(P(a))});
    EXPECT_FALSE(isTautology(cl));
}

// ═══════════════════════════════════════════════════════════════════════════
// Subsumption Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(SimplifyTest, SubsumesSubset) {
    // {P(a)} subsumes {P(a), Q(b)}
    Clause gen = makeClause({pos(P(a))});
    Clause spec = makeClause({pos(P(a)), pos(Q(b))});
    EXPECT_TRUE(subsumes(gen, spec));
}

TEST_F(SimplifyTest, SubsumesIdentical) {
    // {P(a), Q(b)} subsumes {P(a), Q(b)}
    Clause c1 = makeClause({pos(P(a)), pos(Q(b))});
    Clause c2 = makeClause({pos(P(a)), pos(Q(b))});
    EXPECT_TRUE(subsumes(c1, c2));
}

TEST_F(SimplifyTest, SubsumesEmptyClause) {
    // {} subsumes everything
    Clause empty = makeClause({});
    Clause cl = makeClause({pos(P(a)), neg(Q(b))});
    EXPECT_TRUE(subsumes(empty, cl));
}

TEST_F(SimplifyTest, SubsumesNotLarger) {
    // {P(a), Q(b)} does NOT subsume {P(a)}
    Clause gen = makeClause({pos(P(a)), pos(Q(b))});
    Clause spec = makeClause({pos(P(a))});
    EXPECT_FALSE(subsumes(gen, spec));
}

TEST_F(SimplifyTest, SubsumesRequiresSamePolarity) {
    // {P(a)} does NOT subsume {¬P(a)}
    Clause gen = makeClause({pos(P(a))});
    Clause spec = makeClause({neg(P(a))});
    EXPECT_FALSE(subsumes(gen, spec));
}

TEST_F(SimplifyTest, SubsumesRequiresSameAtom) {
    // {P(a)} does NOT subsume {P(b)}
    Clause gen = makeClause({pos(P(a))});
    Clause spec = makeClause({pos(P(b))});
    EXPECT_FALSE(subsumes(gen, spec));
}

TEST_F(SimplifyTest, SubsumesNegativeLiterals) {
    // {¬P(a)} subsumes {¬P(a), Q(b)}
    Clause gen = makeClause({neg(P(a))});
    Clause spec = makeClause({neg(P(a)), pos(Q(b))});
    EXPECT_TRUE(subsumes(gen, spec));
}

TEST_F(SimplifyTest, SubsumesOrderIndependent) {
    // {Q(b), P(a)} subsumes {P(a), Q(b), R(a,b)} regardless of order
    Clause gen = makeClause({pos(Q(b)), pos(P(a))});
    Clause spec = makeClause({pos(P(a)), pos(Q(b)), pos(R(a, b))});
    EXPECT_TRUE(subsumes(gen, spec));
}

}  // namespace
}  // namespace atp
