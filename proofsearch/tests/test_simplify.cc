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
#include "atp/simplify/subsumption.h"
#include "atp/simplify/tautology.h"

#include <gtest/gtest.h>
#include <utility>
#include <vector>

namespace atp {
namespace {

class SimplifyTest : public ::testing::Test {
  protected:
    SymbolTable symbols_;
    TermBank bank_{symbols_};

    SymbolId sym_a_{}, sym_b_{}, sym_c_{};
    SymbolId sym_p_{}, sym_q_{}, sym_r_{};
    SymbolId sym_x_{};

    TermId a_{}, b_{}, c_{}, x_{};

    void SetUp() override {
        sym_a_ = symbols_.intern("a", SymbolKind::kConstant);
        sym_b_ = symbols_.intern("b", SymbolKind::kConstant);
        sym_c_ = symbols_.intern("c", SymbolKind::kConstant);
        sym_p_ = symbols_.intern("P", SymbolKind::kPredicate, 1);
        sym_q_ = symbols_.intern("Q", SymbolKind::kPredicate, 1);
        sym_r_ = symbols_.intern("R", SymbolKind::kPredicate, 2);
        sym_x_ = symbols_.intern("X", SymbolKind::kVariable);

        a_ = bank_.makeTerm(sym_a_, {});
        b_ = bank_.makeTerm(sym_b_, {});
        c_ = bank_.makeTerm(sym_c_, {});
        x_ = bank_.makeVar(sym_x_);
    }

    TermId p(TermId arg) { return bank_.makeTerm(sym_p_, {{arg}}); }
    TermId q(TermId arg) { return bank_.makeTerm(sym_q_, {{arg}}); }
    TermId r(TermId a1, TermId a2) { return bank_.makeTerm(sym_r_, {{a1, a2}}); }

    static Literal pos(TermId atom) { return {.atom_ = atom, .is_positive_ = true}; }
    static Literal neg(TermId atom) { return {.atom_ = atom, .is_positive_ = false}; }

    static Clause makeClause(std::vector<Literal> lits) {
        Clause cl;
        cl.literals_ = std::move(lits);
        return cl;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// Tautology Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(SimplifyTest, TautologyComplementaryLiterals) {
    // {P(a), ¬P(a)} → tautology
    Clause const cl = makeClause({pos(p(a_)), neg(p(a_))});
    EXPECT_TRUE(isTautology(cl));
}

TEST_F(SimplifyTest, TautologyWithExtraLiterals) {
    // {Q(b), P(a), ¬P(a)} → tautology
    Clause const cl = makeClause({pos(q(b_)), pos(p(a_)), neg(p(a_))});
    EXPECT_TRUE(isTautology(cl));
}

TEST_F(SimplifyTest, NotTautologySamePolarity) {
    // {P(a), P(a)} → NOT tautology (same polarity)
    Clause const cl = makeClause({pos(p(a_)), pos(p(a_))});
    EXPECT_FALSE(isTautology(cl));
}

TEST_F(SimplifyTest, NotTautologyDifferentAtoms) {
    // {P(a), ¬P(b)} → NOT tautology (different atoms)
    Clause const cl = makeClause({pos(p(a_)), neg(p(b_))});
    EXPECT_FALSE(isTautology(cl));
}

TEST_F(SimplifyTest, NotTautologyDifferentPredicates) {
    // {P(a), ¬Q(a)} → NOT tautology (different predicates)
    Clause const cl = makeClause({pos(p(a_)), neg(q(a_))});
    EXPECT_FALSE(isTautology(cl));
}

TEST_F(SimplifyTest, NotTautologyEmptyClause) {
    Clause const cl = makeClause({});
    EXPECT_FALSE(isTautology(cl));
}

TEST_F(SimplifyTest, NotTautologySingleLiteral) {
    Clause const cl = makeClause({pos(p(a_))});
    EXPECT_FALSE(isTautology(cl));
}

TEST_F(SimplifyTest, TautologyVariableAtom) {
    // {P(X), ¬P(X)} → tautology (same variable term)
    Clause const cl = makeClause({pos(p(x_)), neg(p(x_))});
    EXPECT_TRUE(isTautology(cl));
}

TEST_F(SimplifyTest, NotTautologyVarVsConst) {
    // {P(X), ¬P(a)} → NOT tautology (syntactically different)
    Clause const cl = makeClause({pos(p(x_)), neg(p(a_))});
    EXPECT_FALSE(isTautology(cl));
}

// ═══════════════════════════════════════════════════════════════════════════
// Subsumption Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(SimplifyTest, SubsumesSubset) {
    // {P(a)} subsumes {P(a), Q(b)}
    Clause const gen = makeClause({pos(p(a_))});
    Clause const spec = makeClause({pos(p(a_)), pos(q(b_))});
    EXPECT_TRUE(subsumes(gen, spec));
}

TEST_F(SimplifyTest, SubsumesIdentical) {
    // {P(a), Q(b)} subsumes {P(a), Q(b)}
    Clause const c1 = makeClause({pos(p(a_)), pos(q(b_))});
    Clause const c2 = makeClause({pos(p(a_)), pos(q(b_))});
    EXPECT_TRUE(subsumes(c1, c2));
}

TEST_F(SimplifyTest, SubsumesEmptyClause) {
    // {} subsumes everything
    Clause const empty = makeClause({});
    Clause const cl = makeClause({pos(p(a_)), neg(q(b_))});
    EXPECT_TRUE(subsumes(empty, cl));
}

TEST_F(SimplifyTest, SubsumesNotLarger) {
    // {P(a), Q(b)} does NOT subsume {P(a)}
    Clause const gen = makeClause({pos(p(a_)), pos(q(b_))});
    Clause const spec = makeClause({pos(p(a_))});
    EXPECT_FALSE(subsumes(gen, spec));
}

TEST_F(SimplifyTest, SubsumesRequiresSamePolarity) {
    // {P(a)} does NOT subsume {¬P(a)}
    Clause const gen = makeClause({pos(p(a_))});
    Clause const spec = makeClause({neg(p(a_))});
    EXPECT_FALSE(subsumes(gen, spec));
}

TEST_F(SimplifyTest, SubsumesRequiresSameAtom) {
    // {P(a)} does NOT subsume {P(b)}
    Clause const gen = makeClause({pos(p(a_))});
    Clause const spec = makeClause({pos(p(b_))});
    EXPECT_FALSE(subsumes(gen, spec));
}

TEST_F(SimplifyTest, SubsumesNegativeLiterals) {
    // {¬P(a)} subsumes {¬P(a), Q(b)}
    Clause const gen = makeClause({neg(p(a_))});
    Clause const spec = makeClause({neg(p(a_)), pos(q(b_))});
    EXPECT_TRUE(subsumes(gen, spec));
}

TEST_F(SimplifyTest, SubsumesOrderIndependent) {
    // {Q(b), P(a)} subsumes {P(a), Q(b), R(a,b)} regardless of order
    Clause const gen = makeClause({pos(q(b_)), pos(p(a_))});
    Clause const spec = makeClause({pos(p(a_)), pos(q(b_)), pos(r(a_, b_))});
    EXPECT_TRUE(subsumes(gen, spec));
}

}  // namespace
}  // namespace atp
