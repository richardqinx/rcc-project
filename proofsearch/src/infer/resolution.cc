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

#include "atp/infer/resolution.h"

#include "atp/core/clause.h"
#include "atp/core/symbol_table.h"
#include "atp/core/term.h"
#include "atp/core/term_bank.h"
#include "atp/core/types.h"
#include "atp/infer/substitution.h"
#include "atp/infer/unification.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace atp {

namespace {

/// Recursively collect all variable TermIds appearing in a term.
void collectVariables(TermId term, const TermBank& bank, std::unordered_map<TermId, bool>& seen) {
    if (bank.isVariable(term)) {
        seen[term] = true;
        return;
    }
    // Copy args to avoid dangling ref (bank is const here, but be safe)
    const Term& t = bank.getTerm(term);
    for (TermId const arg : t.args_) {
        collectVariables(arg, bank, seen);
    }
}

}  // namespace

Clause renameVariables(const Clause& c, TermBank& bank, SymbolTable& symbols,
                       uint32_t& next_var_id) {
    // 1. Collect all variables in the clause
    std::unordered_map<TermId, bool> var_set;
    for (const auto& lit : c.literals_) {
        collectVariables(lit.atom_, bank, var_set);
    }

    if (var_set.empty()) {
        return c;  // Ground clause — nothing to rename
    }

    // 2. Create fresh variables and build a substitution
    Substitution rename_subst;
    for (auto& [old_var_id, _] : var_set) {
        std::string const fresh_name = "_R" + std::to_string(next_var_id++);
        SymbolId const fresh_sym = symbols.intern(fresh_name, SymbolKind::kVariable);
        TermId const fresh_var = bank.makeVar(fresh_sym);
        rename_subst.bind(old_var_id, fresh_var);
    }

    // 3. Apply the renaming substitution to all literals
    Clause renamed;
    renamed.id_ = c.id_;
    renamed.rule_ = c.rule_;
    renamed.parent1_ = c.parent1_;
    renamed.parent2_ = c.parent2_;
    renamed.depth_ = c.depth_;
    for (const auto& lit : c.literals_) {
        TermId const new_atom = applySubstitution(rename_subst, lit.atom_, bank);
        renamed.literals_.push_back({.atom_ = new_atom, .is_positive_ = lit.is_positive_});
    }
    return renamed;
}

std::optional<Clause> resolve(TermBank& bank, const Clause& c1, size_t lit_idx1, const Clause& c2,
                              size_t lit_idx2, const UnificationConfig& uconfig) {
    // Must have opposite polarity
    if (c1.literals_[lit_idx1].is_positive_ == c2.literals_[lit_idx2].is_positive_) {
        return std::nullopt;
    }

    // Rename c2's variables to avoid clashes with c1
    // Use a static counter — safe for single-threaded prover
    static uint32_t var_counter = 0;
    auto& symbols = bank.mutableSymbols();
    Clause c2r = renameVariables(c2, bank, symbols, var_counter);

    // Unify the complementary atoms
    Substitution subst;
    if (!unify(bank, c1.literals_[lit_idx1].atom_, c2r.literals_[lit_idx2].atom_, subst, uconfig)) {
        return std::nullopt;
    }

    // Build the resolvent: all literals except the resolved pair, with subst applied
    Clause resolvent;
    resolvent.rule_ = InferenceRule::kResolution;
    resolvent.parent1_ = c1.id_;
    resolvent.parent2_ = c2.id_;

    for (size_t i = 0; i < c1.literals_.size(); ++i) {
        if (i != lit_idx1) {
            TermId const new_atom = applySubstitution(subst, c1.literals_[i].atom_, bank);
            resolvent.literals_.push_back(
                {.atom_ = new_atom, .is_positive_ = c1.literals_[i].is_positive_});
        }
    }
    for (size_t i = 0; i < c2r.literals_.size(); ++i) {
        if (i != lit_idx2) {
            TermId const new_atom = applySubstitution(subst, c2r.literals_[i].atom_, bank);
            resolvent.literals_.push_back(
                {.atom_ = new_atom, .is_positive_ = c2r.literals_[i].is_positive_});
        }
    }
    return resolvent;
}

std::vector<Clause> allResolvents(TermBank& bank, const Clause& c1, const Clause& c2,
                                  const UnificationConfig& uconfig) {
    std::vector<Clause> results;
    for (size_t i = 0; i < c1.literals_.size(); ++i) {
        for (size_t j = 0; j < c2.literals_.size(); ++j) {
            if (c1.literals_[i].is_positive_ != c2.literals_[j].is_positive_) {
                if (auto r = resolve(bank, c1, i, c2, j, uconfig)) {
                    results.push_back(std::move(*r));
                }
            }
        }
    }
    return results;
}

std::vector<Clause> factor(TermBank& bank, const Clause& c, const UnificationConfig& uconfig) {
    std::vector<Clause> results;
    for (size_t i = 0; i < c.literals_.size(); ++i) {
        for (size_t j = i + 1; j < c.literals_.size(); ++j) {
            if (c.literals_[i].is_positive_ != c.literals_[j].is_positive_) {
                continue;  // Must have same polarity
            }
            Substitution subst;
            if (!unify(bank, c.literals_[i].atom_, c.literals_[j].atom_, subst, uconfig)) {
                continue;
            }
            // Build factored clause: drop literal j, apply subst to all
            Clause factored;
            factored.rule_ = InferenceRule::kFactoring;
            factored.parent1_ = c.id_;
            for (size_t k = 0; k < c.literals_.size(); ++k) {
                if (k != j) {
                    TermId const new_atom = applySubstitution(subst, c.literals_[k].atom_, bank);
                    factored.literals_.push_back(
                        {.atom_ = new_atom, .is_positive_ = c.literals_[k].is_positive_});
                }
            }
            results.push_back(std::move(factored));
        }
    }
    return results;
}

}  // namespace atp
