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

#include <cstddef>
#include <gtest/gtest.h>
#include <span>
#include <stdexcept>
#include <utility>

namespace atp {
namespace {

// ═══════════════════════════════════════════════════════════════════════════
// SymbolTable Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(SymbolTableTest, InternReturnsConsistentId) {
    SymbolTable st;
    SymbolId const id1 = st.intern("foo", SymbolKind::kFunction, 2);
    SymbolId const id2 = st.intern("foo", SymbolKind::kFunction, 2);
    EXPECT_EQ(id1, id2);
}

TEST(SymbolTableTest, DifferentNamesGetDifferentIds) {
    SymbolTable st;
    SymbolId const a = st.intern("a", SymbolKind::kConstant);
    SymbolId const b = st.intern("b", SymbolKind::kConstant);
    EXPECT_NE(a, b);
}

TEST(SymbolTableTest, GetNameRoundTrips) {
    SymbolTable st;
    SymbolId const id = st.intern("hello", SymbolKind::kFunction, 1);
    EXPECT_EQ(st.getName(id), "hello");
}

TEST(SymbolTableTest, GetInfoPreservesMetadata) {
    SymbolTable st;
    SymbolId const id = st.intern("P", SymbolKind::kPredicate, 3, 42);
    const SymbolInfo& info = st.getInfo(id);
    EXPECT_EQ(info.name_, "P");
    EXPECT_EQ(info.kind_, SymbolKind::kPredicate);
    EXPECT_EQ(info.arity_, 3);
    EXPECT_EQ(info.sort_, 42);
}

TEST(SymbolTableTest, IsVariableDetectsKind) {
    SymbolTable st;
    SymbolId const var = st.intern("X", SymbolKind::kVariable);
    SymbolId const con = st.intern("a", SymbolKind::kConstant);
    SymbolId const fun = st.intern("f", SymbolKind::kFunction, 1);
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
    SymbolTable symbols_;
    TermBank bank_{symbols_};

    SymbolId sym_a_{}, sym_b_{}, sym_f_{}, sym_g_{}, sym_x_id_{}, sym_y_id_{};

    void SetUp() override {
        sym_a_ = symbols_.intern("a", SymbolKind::kConstant);
        sym_b_ = symbols_.intern("b", SymbolKind::kConstant);
        sym_f_ = symbols_.intern("f", SymbolKind::kFunction, 1);
        sym_g_ = symbols_.intern("g", SymbolKind::kFunction, 2);
        sym_x_id_ = symbols_.intern("X", SymbolKind::kVariable);
        sym_y_id_ = symbols_.intern("Y", SymbolKind::kVariable);
    }
};

TEST_F(TermBankTest, HashConsingReturnsSameId) {
    const TermId kA1 = bank_.makeTerm(sym_a_, {});
    const TermId kA2 = bank_.makeTerm(sym_a_, {});
    EXPECT_EQ(kA1, kA2) << "Hash consing failed: same term got different IDs";
}

TEST_F(TermBankTest, DifferentTermsGetDifferentIds) {
    const TermId kA = bank_.makeTerm(sym_a_, {});
    const TermId kB = bank_.makeTerm(sym_b_, {});
    EXPECT_NE(kA, kB);
}

TEST_F(TermBankTest, CompoundTermHashConsing) {
    const TermId kA = bank_.makeTerm(sym_a_, {});
    const TermId kB = bank_.makeTerm(sym_b_, {});
    // f(a)
    const TermId kFA1 = bank_.makeTerm(sym_f_, {{kA}});
    const TermId kFA2 = bank_.makeTerm(sym_f_, {{kA}});
    EXPECT_EQ(kFA1, kFA2);
    // f(b) != f(a)
    const TermId kFB = bank_.makeTerm(sym_f_, {{kB}});
    EXPECT_NE(kFA1, kFB);
}

TEST_F(TermBankTest, NestedTermHashConsing) {
    const TermId kA = bank_.makeTerm(sym_a_, {});
    const TermId kFA = bank_.makeTerm(sym_f_, {{kA}});
    // g(f(a), a)
    const TermId kGFAA1 = bank_.makeTerm(sym_g_, {{kFA, kA}});
    const TermId kGFAA2 = bank_.makeTerm(sym_g_, {{kFA, kA}});
    EXPECT_EQ(kGFAA1, kGFAA2);
}

TEST_F(TermBankTest, GetTermReturnsCorrectSymbol) {
    const TermId kA = bank_.makeTerm(sym_a_, {});
    const Term& t = bank_.getTerm(kA);
    EXPECT_EQ(t.symbol_id_, sym_a_);
    EXPECT_EQ(t.arity(), 0);
}

TEST_F(TermBankTest, GetTermReturnsCorrectArgs) {
    const TermId kA = bank_.makeTerm(sym_a_, {});
    const TermId kB = bank_.makeTerm(sym_b_, {});
    const TermId kGAB = bank_.makeTerm(sym_g_, {{kA, kB}});

    const Term& t = bank_.getTerm(kGAB);
    EXPECT_EQ(t.symbol_id_, sym_g_);
    EXPECT_EQ(t.arity(), 2);
    ASSERT_EQ(t.args_.size(), 2);
    EXPECT_EQ(t.args_[0], kA);
    EXPECT_EQ(t.args_[1], kB);
}

TEST_F(TermBankTest, MakeVarCreatesZeroArityTerm) {
    const TermId kX = bank_.makeVar(sym_x_id_);
    const Term& t = bank_.getTerm(kX);
    EXPECT_EQ(t.symbol_id_, sym_x_id_);
    EXPECT_EQ(t.arity(), 0);
    EXPECT_TRUE(t.args_.empty());
}

TEST_F(TermBankTest, IsVariableDistinguishesFromConstants) {
    const TermId kX = bank_.makeVar(sym_x_id_);
    const TermId kA = bank_.makeTerm(sym_a_, {});

    EXPECT_TRUE(bank_.isVariable(kX)) << "Variable should be detected as variable";
    EXPECT_FALSE(bank_.isVariable(kA)) << "Constant should NOT be detected as variable";
}

TEST_F(TermBankTest, VariableHashConsing) {
    const TermId kX1 = bank_.makeVar(sym_x_id_);
    const TermId kX2 = bank_.makeVar(sym_x_id_);
    EXPECT_EQ(kX1, kX2);

    const TermId kY = bank_.makeVar(sym_y_id_);
    EXPECT_NE(kX1, kY);
}

TEST_F(TermBankTest, SizeTracksUniqueTerms) {
    EXPECT_EQ(bank_.size(), 0);
    const TermId kA = bank_.makeTerm(sym_a_, {});
    EXPECT_EQ(bank_.size(), 1);
    bank_.makeTerm(sym_a_, {});  // duplicate
    EXPECT_EQ(bank_.size(), 1);
    bank_.makeTerm(sym_b_, {});
    EXPECT_EQ(bank_.size(), 2);
    bank_.makeTerm(sym_f_, {{kA}});
    EXPECT_EQ(bank_.size(), 3);
}

TEST_F(TermBankTest, CompoundTermWithVariableArgs) {
    // f(X) — a function applied to a variable
    const TermId kX = bank_.makeVar(sym_x_id_);
    const TermId kFX = bank_.makeTerm(sym_f_, {{kX}});

    const Term& t = bank_.getTerm(kFX);
    EXPECT_EQ(t.symbol_id_, sym_f_);
    EXPECT_EQ(t.arity(), 1);
    EXPECT_EQ(t.args_[0], kX);

    EXPECT_FALSE(bank_.isVariable(kFX));
    EXPECT_TRUE(bank_.isVariable(kX));
}

// ═══════════════════════════════════════════════════════════════════════════
// ClauseStore Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(ClauseStoreTest, AddAndRetrieve) {
    ClauseStore store;
    Clause c;
    c.literals_.push_back(Literal{.atom_ = 0, .is_positive_ = true});
    c.literals_.push_back(Literal{.atom_ = 1, .is_positive_ = false});

    const ClauseId kId = store.addClause(std::move(c));
    EXPECT_EQ(kId, 0);

    const Clause& retrieved = store.getClause(kId);
    EXPECT_EQ(retrieved.id_, kId);
    EXPECT_EQ(retrieved.literals_.size(), 2);
    EXPECT_EQ(retrieved.literals_[0].atom_, 0);
    EXPECT_TRUE(retrieved.literals_[0].is_positive_);
    EXPECT_EQ(retrieved.literals_[1].atom_, 1);
    EXPECT_FALSE(retrieved.literals_[1].is_positive_);
}

TEST(ClauseStoreTest, AssignsSequentialIds) {
    ClauseStore store;
    const ClauseId kId0 = store.addClause(Clause{});
    const ClauseId kId1 = store.addClause(Clause{});
    const ClauseId kId2 = store.addClause(Clause{});
    EXPECT_EQ(kId0, 0);
    EXPECT_EQ(kId1, 1);
    EXPECT_EQ(kId2, 2);
    EXPECT_EQ(store.size(), 3);
}

TEST(ClauseStoreTest, StampsClauseWithId) {
    ClauseStore store;
    Clause c;
    c.id_ = kInvalidId;  // unset
    const ClauseId kId = store.addClause(std::move(c));
    EXPECT_EQ(store.getClause(kId).id_, kId);
}

TEST(ClauseStoreTest, MutableAccess) {
    ClauseStore store;
    Clause c;
    c.depth_ = 0;
    const ClauseId kId = store.addClause(std::move(c));

    store.getMutableClause(kId).depth_ = 42;
    EXPECT_EQ(store.getClause(kId).depth_, 42);
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
    c.literals_.push_back(Literal{.atom_ = 0, .is_positive_ = true});
    EXPECT_FALSE(c.isEmpty());
    EXPECT_EQ(c.size(), 1);
}

TEST(ClauseStoreTest, ProvenancePreserved) {
    ClauseStore store;
    Clause c;
    c.rule_ = InferenceRule::kResolution;
    c.parent1_ = 3;
    c.parent2_ = 7;
    c.depth_ = 5;

    ClauseId const id = store.addClause(std::move(c));
    const Clause& retrieved = store.getClause(id);
    EXPECT_EQ(retrieved.rule_, InferenceRule::kResolution);
    EXPECT_EQ(retrieved.parent1_, 3);
    EXPECT_EQ(retrieved.parent2_, 7);
    EXPECT_EQ(retrieved.depth_, 5);
}

// ═══════════════════════════════════════════════════════════════════════════
// Literal Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(LiteralTest, Complement) {
    const Literal kPos{.atom_ = 42, .is_positive_ = true};
    const Literal kNeg = kPos.complement();
    EXPECT_EQ(kNeg.atom_, 42);
    EXPECT_FALSE(kNeg.is_positive_);

    const Literal kBack = kNeg.complement();
    EXPECT_EQ(kBack, kPos);
}

TEST(LiteralTest, Equality) {
    const Literal kA{.atom_ = 1, .is_positive_ = true};
    const Literal kB{.atom_ = 1, .is_positive_ = true};
    const Literal kC{.atom_ = 1, .is_positive_ = false};
    const Literal kD{.atom_ = 2, .is_positive_ = true};
    EXPECT_EQ(kA, kB);
    EXPECT_NE(kA, kC);
    EXPECT_NE(kA, kD);
}

}  // namespace
}  // namespace atp
