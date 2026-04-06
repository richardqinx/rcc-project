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
#include <utility>
#include <vector>

namespace atp {
namespace {

class ProofTraceTest : public ::testing::Test {
  protected:
    SymbolTable symbols_;
    TermBank bank_{symbols_};
    ClauseStore store_;

    SymbolId sym_a_{}, sym_b_{};
    SymbolId sym_p_{}, sym_q_{};
    SymbolId sym_x_{};

    TermId a_term_{}, b_term_{}, x_term_{};

    void SetUp() override {
        sym_a_ = symbols_.intern("a", SymbolKind::kConstant);
        sym_b_ = symbols_.intern("b", SymbolKind::kConstant);
        sym_p_ = symbols_.intern("P", SymbolKind::kPredicate, 1);
        sym_q_ = symbols_.intern("Q", SymbolKind::kPredicate, 1);
        sym_x_ = symbols_.intern("X", SymbolKind::kVariable);

        a_term_ = bank_.makeTerm(sym_a_, {});
        b_term_ = bank_.makeTerm(sym_b_, {});
        x_term_ = bank_.makeVar(sym_x_);
    }

    TermId p(TermId arg) { return bank_.makeTerm(sym_p_, {{arg}}); }
    TermId q(TermId arg) { return bank_.makeTerm(sym_q_, {{arg}}); }

    static Literal pos(TermId atom) { return {.atom_ = atom, .is_positive_ = true}; }
    static Literal neg(TermId atom) { return {.atom_ = atom, .is_positive_ = false}; }

    static Clause makeClause(std::vector<Literal> lits) {
        Clause cl;
        cl.literals_ = std::move(lits);
        return cl;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// termToString Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(ProofTraceTest, TermToStringConstant) {
    EXPECT_EQ(termToString(a_term_, bank_), "a");
}

TEST_F(ProofTraceTest, TermToStringVariable) {
    EXPECT_EQ(termToString(x_term_, bank_), "X");
}

TEST_F(ProofTraceTest, TermToStringCompound) {
    TermId const pa = p(a_term_);
    EXPECT_EQ(termToString(pa, bank_), "P(a)");
}

TEST_F(ProofTraceTest, TermToStringNested) {
    SymbolId const sym_f = symbols_.intern("f", SymbolKind::kFunction, 1);
    TermId const fa = bank_.makeTerm(sym_f, {{a_term_}});
    TermId const pfa = p(fa);
    EXPECT_EQ(termToString(pfa, bank_), "P(f(a))");
}

// ═══════════════════════════════════════════════════════════════════════════
// clauseToString Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(ProofTraceTest, ClauseToStringEmpty) {
    Clause const empty = makeClause({});
    EXPECT_EQ(clauseToString(empty, bank_), "□");
}

TEST_F(ProofTraceTest, ClauseToStringSinglePositive) {
    Clause const c = makeClause({pos(p(a_term_))});
    EXPECT_EQ(clauseToString(c, bank_), "P(a)");
}

TEST_F(ProofTraceTest, ClauseToStringSingleNegative) {
    Clause const c = makeClause({neg(p(a_term_))});
    EXPECT_EQ(clauseToString(c, bank_), "¬P(a)");
}

TEST_F(ProofTraceTest, ClauseToStringMultipleLiterals) {
    Clause const c = makeClause({neg(p(x_term_)), pos(q(x_term_))});
    std::string const s = clauseToString(c, bank_);
    EXPECT_EQ(s, "¬P(X) ∨ Q(X)");
}

// ═══════════════════════════════════════════════════════════════════════════
// extractProof Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(ProofTraceTest, ExtractProofTrivial) {
    // Run a trivial proof: {P(a)} + {¬P(a)} → □
    Prover prover(bank_, store_);
    prover.addClauses({makeClause({pos(p(a_term_))}), makeClause({neg(p(a_term_))})});
    ProverResult const result = prover.prove();
    ASSERT_EQ(result, ProverResult::kTheorem);

    auto eid = prover.getEmptyClauseId();
    ASSERT_TRUE(eid.has_value());

    auto proof = extractProof(store_, *eid);
    ASSERT_GE(proof.size(), 3U);  // At least: 2 axioms + 1 empty clause

    // First steps should be axioms (kInput)
    EXPECT_EQ(proof.front().rule_, InferenceRule::kInput);
    // Last step should be the empty clause
    EXPECT_EQ(proof.back().clause_id_, *eid);
    // Last step should be resolution
    EXPECT_EQ(proof.back().rule_, InferenceRule::kResolution);
}

TEST_F(ProofTraceTest, ExtractProofTwoStep) {
    // {P(a)}, {¬P(X), Q(X)}, {¬Q(a)} → □ in 2 resolution steps
    Prover prover(bank_, store_);
    prover.addClauses({makeClause({pos(p(a_term_))}), makeClause({neg(p(x_term_)), pos(q(x_term_))}),
                       makeClause({neg(q(a_term_))})});
    ProverResult const result = prover.prove();
    ASSERT_EQ(result, ProverResult::kTheorem);

    auto eid = prover.getEmptyClauseId();
    ASSERT_TRUE(eid.has_value());

    auto proof = extractProof(store_, *eid);
    // Should have axioms + intermediate + empty clause
    ASSERT_GE(proof.size(), 4U);  // 3 axioms + at least 1 intermediate + empty

    // Last step is the empty clause
    EXPECT_EQ(proof.back().clause_id_, *eid);
}

// ═══════════════════════════════════════════════════════════════════════════
// formatProof Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(ProofTraceTest, FormatProofContainsExpectedElements) {
    Prover prover(bank_, store_);
    prover.addClauses({makeClause({pos(p(a_term_))}), makeClause({neg(p(a_term_))})});
    prover.prove();

    auto eid = prover.getEmptyClauseId();
    ASSERT_TRUE(eid.has_value());

    auto proof = extractProof(store_, *eid);
    std::string const output = formatProof(proof, store_, bank_);

    // Should contain key elements
    EXPECT_NE(output.find("input"), std::string::npos) << output;
    EXPECT_NE(output.find("resolution"), std::string::npos) << output;
    EXPECT_NE(output.find("P(a)"), std::string::npos) << output;
    // Empty clause should appear
    std::string const empty_marker = "□";
    EXPECT_NE(output.find(empty_marker), std::string::npos) << output;
}

TEST_F(ProofTraceTest, FormatProofNoParentsForAxioms) {
    Prover prover(bank_, store_);
    prover.addClauses({makeClause({pos(p(a_term_))}), makeClause({neg(p(a_term_))})});
    prover.prove();

    auto eid = prover.getEmptyClauseId();
    auto proof = extractProof(store_, *eid);
    std::string const output = formatProof(proof, store_, bank_);

    // The first line (an axiom) should NOT have "[from ...]"
    // Find the first newline
    auto first_line = output.substr(0, output.find('\n'));
    EXPECT_EQ(first_line.find("[from"), std::string::npos)
        << "Axiom should not have parent info: " << first_line;
}

}  // namespace
}  // namespace atp
