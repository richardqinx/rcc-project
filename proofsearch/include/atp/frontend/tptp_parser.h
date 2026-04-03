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

#pragma once

/// @file tptp_parser.h
/// @brief TPTP format parser — converts .p files to internal representations.
///
/// The parser interns all symbols (predicates, functions, variables) into the
/// SymbolTable and builds terms in the TermBank during parsing. This is the
/// E Prover approach: parse and intern simultaneously, avoiding a separate
/// resolution pass.
///
/// V1: Hand-written recursive descent for TPTP CNF/FOF subset.
///     (ANTLR4 integration deferred — avoids build complexity for V1.)

#include "atp/core/clause.h"
#include "atp/core/symbol_table.h"
#include "atp/core/term_bank.h"
#include "atp/normalize/formula.h"

#include <string>
#include <vector>

namespace atp {

/// Role of a TPTP formula.
/// See TPTP spec: https://www.tptp.org/TPTP/SyntaxBNF.html
enum class FormulaRole : uint8_t {
    kAxiom,              ///< Background truth
    kHypothesis,         ///< Problem-specific assumption
    kDefinition,         ///< Symbol definition (can be unfolded)
    kAssumption,         ///< Assumed for this proof attempt
    kLemma,              ///< Derived intermediate result
    kTheorem,            ///< Previously proved statement (used as axiom)
    kConjecture,         ///< The statement to prove
    kNegatedConjecture,  ///< Already negated conjecture (input was cnf)
    kPlain,              ///< No semantic role specified
};

/// Map a TPTP role string to enum. Returns kPlain for unknown roles.
FormulaRole parseRole(std::string_view role_str);

/// A parsed TPTP formula with its name and role.
struct AnnotatedFormula {
    std::string name;                  ///< Formula name (e.g., "axiom1")
    FormulaRole role;                  ///< Semantic role
    std::unique_ptr<Formula> formula;  ///< The FOL formula (for fof(...))
    std::vector<Clause> clauses;       ///< Direct clauses (for cnf(...))
    bool is_cnf = false;               ///< true if input was cnf(), false if fof()
};

/// A complete parsed TPTP problem.
struct ParsedProblem {
    std::vector<AnnotatedFormula> formulas;

    /// Does any formula contain an equality predicate?
    bool has_equality = false;

    /// Original file path (for error messages).
    std::string source_file;
};

/// Parse a TPTP file.
///
/// For `cnf(...)` entries: clauses are built directly (no clausification needed).
/// For `fof(...)` entries: Formula AST is built (clausification happens later).
///
/// All symbols are interned into `symbols` and terms into `bank` during parsing.
ParsedProblem parseTptpFile(const std::string& filepath, TermBank& bank, SymbolTable& symbols);

/// Parse a TPTP string (for testing).
ParsedProblem parseTptpString(const std::string& input, TermBank& bank, SymbolTable& symbols);

/// Prepare a parsed problem for proving:
///   1. Negate conjecture formulas.
///   2. Clausify all fof() formulas.
///   3. Collect all cnf() clauses directly.
///   4. Assign ClauseIds and set provenance to kInput.
///
/// Returns the final clause set ready for the Prover.
std::vector<Clause> prepareForProving(ParsedProblem& problem, TermBank& bank, SymbolTable& symbols);

}  // namespace atp
