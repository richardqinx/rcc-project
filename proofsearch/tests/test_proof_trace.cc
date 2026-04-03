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
#include "atp/proof/proof_trace.h"
#include "atp/search/prover.h"

#include <gtest/gtest.h>
#include <string>

namespace atp {
namespace {

class ProofTraceTest : public ::testing::Test {
  protected:
    SymbolTable symbols;
    TermBank bank{symbols};
    ClauseStore store;

    SymbolId sym_a{}, sym_b{};
    SymbolId sym_P{}, sym_Q{};
    SymbolId sym_X{};

    TermId a_term{}, b_term{}, X_term{};

    void SetUp() override {
        sym_a = symbols.intern("a", SymbolKind::kConstant);
        sym_b = symbols.intern("b", SymbolKind::kConstant);
        sym_P = symbols.intern("P", SymbolKind::kPredicate, 1);
        sym_Q = symbols.intern("Q", SymbolKind::kPredicate, 1);
        sym_X = symbols.intern("X", SymbolKind::kVariable);

        a_term = bank.makeTerm(sym_a, {});
        b_term = bank.makeTerm(sym_b, {});
        X_term = bank.makeVar(sym_X);
    }

    TermId P(TermId arg) { return bank.makeTerm(sym_P, {{arg}}); }
    TermId Q(TermId arg) { return bank.makeTerm(sym_Q, {{arg}}); }

    Literal pos(TermId atom) { return {.atom = atom, .is_positive = true}; }
    Literal neg(TermId atom) { return {.atom = atom, .is_positive = false}; }

    Clause makeClause(std::vector<Literal> lits) {
        Clause cl;
        cl.literals = std::move(lits);
        return cl;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// termToString Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(ProofTraceTest, TermToStringConstant) {
    EXPECT_EQ(termToString(a_term, bank), "a");
}

TEST_F(ProofTraceTest, TermToStringVariable) {
    EXPECT_EQ(termToString(X_term, bank), "X");
}

TEST_F(ProofTraceTest, TermToStringCompound) {
    TermId pa = P(a_term);
    EXPECT_EQ(termToString(pa, bank), "P(a)");
}

TEST_F(ProofTraceTest, TermToStringNested) {
    SymbolId sym_f = symbols.intern("f", SymbolKind::kFunction, 1);
    TermId fa = bank.makeTerm(sym_f, {{a_term}});
    TermId pfa = P(fa);
    EXPECT_EQ(termToString(pfa, bank), "P(f(a))");
}

// ═══════════════════════════════════════════════════════════════════════════
// clauseToString Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(ProofTraceTest, ClauseToStringEmpty) {
    Clause empty = makeClause({});
    EXPECT_EQ(clauseToString(empty, bank), "□");
}

TEST_F(ProofTraceTest, ClauseToStringSinglePositive) {
    Clause c = makeClause({pos(P(a_term))});
    EXPECT_EQ(clauseToString(c, bank), "P(a)");
}

TEST_F(ProofTraceTest, ClauseToStringSingleNegative) {
    Clause c = makeClause({neg(P(a_term))});
    EXPECT_EQ(clauseToString(c, bank), "¬P(a)");
}

TEST_F(ProofTraceTest, ClauseToStringMultipleLiterals) {
    Clause c = makeClause({neg(P(X_term)), pos(Q(X_term))});
    std::string s = clauseToString(c, bank);
    EXPECT_EQ(s, "¬P(X) ∨ Q(X)");
}

// ═══════════════════════════════════════════════════════════════════════════
// extractProof Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(ProofTraceTest, ExtractProofTrivial) {
    // Run a trivial proof: {P(a)} + {¬P(a)} → □
    Prover prover(bank, store);
    prover.addClauses({makeClause({pos(P(a_term))}), makeClause({neg(P(a_term))})});
    ProverResult result = prover.prove();
    ASSERT_EQ(result, ProverResult::kTheorem);

    auto eid = prover.getEmptyClauseId();
    ASSERT_TRUE(eid.has_value());

    auto proof = extractProof(store, *eid);
    ASSERT_GE(proof.size(), 3u);  // At least: 2 axioms + 1 empty clause

    // First steps should be axioms (kInput)
    EXPECT_EQ(proof.front().rule, InferenceRule::kInput);
    // Last step should be the empty clause
    EXPECT_EQ(proof.back().clause_id, *eid);
    // Last step should be resolution
    EXPECT_EQ(proof.back().rule, InferenceRule::kResolution);
}

TEST_F(ProofTraceTest, ExtractProofTwoStep) {
    // {P(a)}, {¬P(X), Q(X)}, {¬Q(a)} → □ in 2 resolution steps
    Prover prover(bank, store);
    prover.addClauses({makeClause({pos(P(a_term))}), makeClause({neg(P(X_term)), pos(Q(X_term))}),
                       makeClause({neg(Q(a_term))})});
    ProverResult result = prover.prove();
    ASSERT_EQ(result, ProverResult::kTheorem);

    auto eid = prover.getEmptyClauseId();
    ASSERT_TRUE(eid.has_value());

    auto proof = extractProof(store, *eid);
    // Should have axioms + intermediate + empty clause
    ASSERT_GE(proof.size(), 4u);  // 3 axioms + at least 1 intermediate + empty

    // Last step is the empty clause
    EXPECT_EQ(proof.back().clause_id, *eid);
}

// ═══════════════════════════════════════════════════════════════════════════
// formatProof Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(ProofTraceTest, FormatProofContainsExpectedElements) {
    Prover prover(bank, store);
    prover.addClauses({makeClause({pos(P(a_term))}), makeClause({neg(P(a_term))})});
    prover.prove();

    auto eid = prover.getEmptyClauseId();
    ASSERT_TRUE(eid.has_value());

    auto proof = extractProof(store, *eid);
    std::string output = formatProof(proof, store, bank);

    // Should contain key elements
    EXPECT_NE(output.find("input"), std::string::npos) << output;
    EXPECT_NE(output.find("resolution"), std::string::npos) << output;
    EXPECT_NE(output.find("P(a)"), std::string::npos) << output;
    // Empty clause should appear
    std::string empty_marker = "□";
    EXPECT_NE(output.find(empty_marker), std::string::npos) << output;
}

TEST_F(ProofTraceTest, FormatProofNoParentsForAxioms) {
    Prover prover(bank, store);
    prover.addClauses({makeClause({pos(P(a_term))}), makeClause({neg(P(a_term))})});
    prover.prove();

    auto eid = prover.getEmptyClauseId();
    auto proof = extractProof(store, *eid);
    std::string output = formatProof(proof, store, bank);

    // The first line (an axiom) should NOT have "[from ...]"
    // Find the first newline
    auto first_line = output.substr(0, output.find('\n'));
    EXPECT_EQ(first_line.find("[from"), std::string::npos)
        << "Axiom should not have parent info: " << first_line;
}

}  // namespace
}  // namespace atp
