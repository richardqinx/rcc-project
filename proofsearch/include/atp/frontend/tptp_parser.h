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
/// @brief Bridge from ANTLR4-generated parse tree to internal Formula AST.

#include "atp/core/clause.h"
#include "atp/core/term_bank.h"
#include "atp/normalize/formula.h"

#include <string>
#include <vector>

namespace atp {

/// Role of a TPTP formula (axiom, hypothesis, conjecture, etc.)
enum class FormulaRole : uint8_t {
    kAxiom,
    kHypothesis,
    kConjecture,
    kNegatedConjecture,
};

/// A parsed TPTP formula with its name and role.
struct AnnotatedFormula {
    std::string name;
    FormulaRole role;
    std::unique_ptr<Formula> formula;
};

/// Parse a TPTP file and return the annotated formulas.
std::vector<AnnotatedFormula> parseTptpFile(const std::string& filepath, TermBank& bank);

}  // namespace atp
