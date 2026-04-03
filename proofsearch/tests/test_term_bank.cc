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
#include "atp/core/term.h"
#include "atp/core/term_bank.h"
#include "atp/core/types.h"

#include <gtest/gtest.h>

namespace atp {
namespace {

// ═══════════════════════════════════════════════════════════════════════════
// SymbolTable Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(SymbolTableTest, InternReturnsConsistentId) {
    SymbolTable st;
    SymbolId id1 = st.intern("foo", SymbolKind::kFunction, 2);
    SymbolId id2 = st.intern("foo", SymbolKind::kFunction, 2);
    EXPECT_EQ(id1, id2);
}

TEST(SymbolTableTest, DifferentNamesGetDifferentIds) {
    SymbolTable st;
    SymbolId a = st.intern("a", SymbolKind::kConstant);
    SymbolId b = st.intern("b", SymbolKind::kConstant);
    EXPECT_NE(a, b);
}

TEST(SymbolTableTest, GetNameRoundTrips) {
    SymbolTable st;
    SymbolId id = st.intern("hello", SymbolKind::kFunction, 1);
    EXPECT_EQ(st.getName(id), "hello");
}

TEST(SymbolTableTest, GetInfoPreservesMetadata) {
    SymbolTable st;
    SymbolId id = st.intern("P", SymbolKind::kPredicate, 3, 42);
    const SymbolInfo& info = st.getInfo(id);
    EXPECT_EQ(info.name, "P");
    EXPECT_EQ(info.kind, SymbolKind::kPredicate);
    EXPECT_EQ(info.arity, 3);
    EXPECT_EQ(info.sort, 42);
}

TEST(SymbolTableTest, IsVariableDetectsKind) {
    SymbolTable st;
    SymbolId var = st.intern("X", SymbolKind::kVariable);
    SymbolId con = st.intern("a", SymbolKind::kConstant);
    SymbolId fun = st.intern("f", SymbolKind::kFunction, 1);
    EXPECT_TRUE(st.isVariable(var));
    EXPECT_FALSE(st.isVariable(con));
    EXPECT_FALSE(st.isVariable(fun));
}

TEST(SymbolTableTest, MismatchKindThrows) {
    SymbolTable st;
    st.intern("x", SymbolKind::kConstant);
    EXPECT_THROW(st.intern("x", SymbolKind::kVariable), std::runtime_error);
}

TEST(SymbolTableTest, MismatchArityThrows) {
    SymbolTable st;
    st.intern("f", SymbolKind::kFunction, 1);
    EXPECT_THROW(st.intern("f", SymbolKind::kFunction, 2), std::runtime_error);
}

TEST(SymbolTableTest, SizeTracksInternedSymbols) {
    SymbolTable st;
    EXPECT_EQ(st.size(), 0);
    st.intern("a", SymbolKind::kConstant);
    EXPECT_EQ(st.size(), 1);
    st.intern("b", SymbolKind::kConstant);
    EXPECT_EQ(st.size(), 2);
    st.intern("a", SymbolKind::kConstant);  // duplicate
    EXPECT_EQ(st.size(), 2);
}

// ═══════════════════════════════════════════════════════════════════════════
// TermBank Tests
// ═══════════════════════════════════════════════════════════════════════════

class TermBankTest : public ::testing::Test {
  protected:
    SymbolTable symbols;
    TermBank bank{symbols};

    SymbolId sym_a, sym_b, sym_f, sym_g, sym_X, sym_Y;

    void SetUp() override {
        sym_a = symbols.intern("a", SymbolKind::kConstant);
        sym_b = symbols.intern("b", SymbolKind::kConstant);
        sym_f = symbols.intern("f", SymbolKind::kFunction, 1);
        sym_g = symbols.intern("g", SymbolKind::kFunction, 2);
        sym_X = symbols.intern("X", SymbolKind::kVariable);
        sym_Y = symbols.intern("Y", SymbolKind::kVariable);
    }
};

TEST_F(TermBankTest, HashConsingReturnsSameId) {
    TermId a1 = bank.makeTerm(sym_a, {});
    TermId a2 = bank.makeTerm(sym_a, {});
    EXPECT_EQ(a1, a2) << "Hash consing failed: same term got different IDs";
}

TEST_F(TermBankTest, DifferentTermsGetDifferentIds) {
    TermId a = bank.makeTerm(sym_a, {});
    TermId b = bank.makeTerm(sym_b, {});
    EXPECT_NE(a, b);
}

TEST_F(TermBankTest, CompoundTermHashConsing) {
    TermId a = bank.makeTerm(sym_a, {});
    TermId b = bank.makeTerm(sym_b, {});
    // f(a)
    TermId fa1 = bank.makeTerm(sym_f, {{a}});
    TermId fa2 = bank.makeTerm(sym_f, {{a}});
    EXPECT_EQ(fa1, fa2);
    // f(b) != f(a)
    TermId fb = bank.makeTerm(sym_f, {{b}});
    EXPECT_NE(fa1, fb);
}

TEST_F(TermBankTest, NestedTermHashConsing) {
    TermId a = bank.makeTerm(sym_a, {});
    TermId fa = bank.makeTerm(sym_f, {{a}});
    // g(f(a), a)
    TermId gfa_a_1 = bank.makeTerm(sym_g, {{fa, a}});
    TermId gfa_a_2 = bank.makeTerm(sym_g, {{fa, a}});
    EXPECT_EQ(gfa_a_1, gfa_a_2);
}

TEST_F(TermBankTest, GetTermReturnsCorrectSymbol) {
    TermId a = bank.makeTerm(sym_a, {});
    const Term& t = bank.getTerm(a);
    EXPECT_EQ(t.symbol_id, sym_a);
    EXPECT_EQ(t.arity(), 0);
}

TEST_F(TermBankTest, GetTermReturnsCorrectArgs) {
    TermId a = bank.makeTerm(sym_a, {});
    TermId b = bank.makeTerm(sym_b, {});
    TermId gab = bank.makeTerm(sym_g, {{a, b}});

    const Term& t = bank.getTerm(gab);
    EXPECT_EQ(t.symbol_id, sym_g);
    EXPECT_EQ(t.arity(), 2);
    ASSERT_EQ(t.args.size(), 2);
    EXPECT_EQ(t.args[0], a);
    EXPECT_EQ(t.args[1], b);
}

TEST_F(TermBankTest, MakeVarCreatesZeroArityTerm) {
    TermId x = bank.makeVar(sym_X);
    const Term& t = bank.getTerm(x);
    EXPECT_EQ(t.symbol_id, sym_X);
    EXPECT_EQ(t.arity(), 0);
    EXPECT_TRUE(t.args.empty());
}

TEST_F(TermBankTest, IsVariableDistinguishesFromConstants) {
    TermId x = bank.makeVar(sym_X);
    TermId a = bank.makeTerm(sym_a, {});

    EXPECT_TRUE(bank.isVariable(x)) << "Variable should be detected as variable";
    EXPECT_FALSE(bank.isVariable(a)) << "Constant should NOT be detected as variable";
}

TEST_F(TermBankTest, VariableHashConsing) {
    TermId x1 = bank.makeVar(sym_X);
    TermId x2 = bank.makeVar(sym_X);
    EXPECT_EQ(x1, x2);

    TermId y = bank.makeVar(sym_Y);
    EXPECT_NE(x1, y);
}

TEST_F(TermBankTest, SizeTracksUniqueTerms) {
    EXPECT_EQ(bank.size(), 0);
    TermId a = bank.makeTerm(sym_a, {});
    EXPECT_EQ(bank.size(), 1);
    bank.makeTerm(sym_a, {});  // duplicate
    EXPECT_EQ(bank.size(), 1);
    bank.makeTerm(sym_b, {});
    EXPECT_EQ(bank.size(), 2);
    bank.makeTerm(sym_f, {{a}});
    EXPECT_EQ(bank.size(), 3);
}

TEST_F(TermBankTest, CompoundTermWithVariableArgs) {
    // f(X) — a function applied to a variable
    TermId x = bank.makeVar(sym_X);
    TermId fx = bank.makeTerm(sym_f, {{x}});

    const Term& t = bank.getTerm(fx);
    EXPECT_EQ(t.symbol_id, sym_f);
    EXPECT_EQ(t.arity(), 1);
    EXPECT_EQ(t.args[0], x);

    EXPECT_FALSE(bank.isVariable(fx));
    EXPECT_TRUE(bank.isVariable(x));
}

// ═══════════════════════════════════════════════════════════════════════════
// ClauseStore Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(ClauseStoreTest, AddAndRetrieve) {
    ClauseStore store;
    Clause c;
    c.literals.push_back(Literal{.atom = 0, .is_positive = true});
    c.literals.push_back(Literal{.atom = 1, .is_positive = false});

    ClauseId id = store.addClause(std::move(c));
    EXPECT_EQ(id, 0);

    const Clause& retrieved = store.getClause(id);
    EXPECT_EQ(retrieved.id, id);
    EXPECT_EQ(retrieved.literals.size(), 2);
    EXPECT_EQ(retrieved.literals[0].atom, 0);
    EXPECT_TRUE(retrieved.literals[0].is_positive);
    EXPECT_EQ(retrieved.literals[1].atom, 1);
    EXPECT_FALSE(retrieved.literals[1].is_positive);
}

TEST(ClauseStoreTest, AssignsSequentialIds) {
    ClauseStore store;
    ClauseId id0 = store.addClause(Clause{});
    ClauseId id1 = store.addClause(Clause{});
    ClauseId id2 = store.addClause(Clause{});
    EXPECT_EQ(id0, 0);
    EXPECT_EQ(id1, 1);
    EXPECT_EQ(id2, 2);
    EXPECT_EQ(store.size(), 3);
}

TEST(ClauseStoreTest, StampsClauseWithId) {
    ClauseStore store;
    Clause c;
    c.id = kInvalidId;  // unset
    ClauseId id = store.addClause(std::move(c));
    EXPECT_EQ(store.getClause(id).id, id);
}

TEST(ClauseStoreTest, MutableAccess) {
    ClauseStore store;
    Clause c;
    c.depth = 0;
    ClauseId id = store.addClause(std::move(c));

    store.getMutableClause(id).depth = 42;
    EXPECT_EQ(store.getClause(id).depth, 42);
}

TEST(ClauseStoreTest, ForEachVisitsAll) {
    ClauseStore store;
    store.addClause(Clause{});
    store.addClause(Clause{});
    store.addClause(Clause{});

    size_t count = 0;
    store.forEach([&count](const Clause&) { count++; });
    EXPECT_EQ(count, 3);
}

TEST(ClauseStoreTest, EmptyClauseDetection) {
    Clause c;
    EXPECT_TRUE(c.isEmpty());
    c.literals.push_back(Literal{.atom = 0, .is_positive = true});
    EXPECT_FALSE(c.isEmpty());
    EXPECT_EQ(c.size(), 1);
}

TEST(ClauseStoreTest, ProvenancePreserved) {
    ClauseStore store;
    Clause c;
    c.rule = InferenceRule::kResolution;
    c.parent1 = 3;
    c.parent2 = 7;
    c.depth = 5;

    ClauseId id = store.addClause(std::move(c));
    const Clause& retrieved = store.getClause(id);
    EXPECT_EQ(retrieved.rule, InferenceRule::kResolution);
    EXPECT_EQ(retrieved.parent1, 3);
    EXPECT_EQ(retrieved.parent2, 7);
    EXPECT_EQ(retrieved.depth, 5);
}

// ═══════════════════════════════════════════════════════════════════════════
// Literal Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(LiteralTest, Complement) {
    Literal pos{.atom = 42, .is_positive = true};
    Literal neg = pos.complement();
    EXPECT_EQ(neg.atom, 42);
    EXPECT_FALSE(neg.is_positive);

    Literal back = neg.complement();
    EXPECT_EQ(back, pos);
}

TEST(LiteralTest, Equality) {
    Literal a{.atom = 1, .is_positive = true};
    Literal b{.atom = 1, .is_positive = true};
    Literal c{.atom = 1, .is_positive = false};
    Literal d{.atom = 2, .is_positive = true};
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
    EXPECT_NE(a, d);
}

}  // namespace
}  // namespace atp
