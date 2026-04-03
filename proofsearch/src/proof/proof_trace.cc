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

#include "atp/proof/proof_trace.h"
#include "atp/core/clause.h"
#include "atp/core/symbol_table.h"
#include "atp/core/term.h"
#include "atp/core/types.h"

#include <algorithm>
#include <stack>
#include <string>
#include <unordered_set>
#include <vector>

namespace atp {

// ─── extractProof 
// DFS from empty clause back to axioms, collect all participating clauses,
// then reverse for topological order (axioms first).

std::vector<ProofStep> extractProof(const ClauseStore& store, ClauseId empty_clause_id) {
    std::vector<ProofStep> steps;
    std::unordered_set<ClauseId> visited;
    std::stack<ClauseId> worklist;
    worklist.push(empty_clause_id);

    while (!worklist.empty()) {
        ClauseId cid = worklist.top();
        worklist.pop();

        if (cid == kInvalidId) {
            continue;
        }
        if (visited.count(cid) > 0) {
            continue;
        }
        visited.insert(cid);

        const Clause& c = store.getClause(cid);
        steps.push_back({
            .clause_id = cid,
            .rule = c.rule,
            .parent1 = c.parent1,
            .parent2 = c.parent2,
        });

        // Recurse into parents (axioms have kInvalidId parents, which are
        // filtered by the check at the top of the loop)
        worklist.push(c.parent1);
        worklist.push(c.parent2);
    }

    // Reverse: axioms (leaves) first → empty clause (root) last
    std::ranges::reverse(steps);
    return steps;
}

// ─── termToString 

std::string termToString(TermId id, const TermBank& bank) {
    const Term& t = bank.getTerm(id);
    std::string name(bank.symbols().getName(t.symbol_id));

    if (t.args.empty()) {
        return name;
    }

    std::string result = name + "(";
    for (size_t i = 0; i < t.args.size(); ++i) {
        if (i > 0) {
            result += ",";
        }
        result += termToString(t.args[i], bank);
    }
    result += ")";
    return result;
}

// ─── clauseToString 

std::string clauseToString(const Clause& clause, const TermBank& bank) {
    if (clause.isEmpty()) {
        return "□";
    }

    std::string result;
    for (size_t i = 0; i < clause.literals.size(); ++i) {
        if (i > 0) {
            result += " ∨ ";
        }
        if (!clause.literals[i].is_positive) {
            result += "¬";
        }
        result += termToString(clause.literals[i].atom, bank);
    }
    return result;
}

// ─── ruleToString 

static std::string ruleToString(InferenceRule rule) {
    switch (rule) {
        case InferenceRule::kInput:      return "input";
        case InferenceRule::kResolution: return "resolution";
        case InferenceRule::kFactoring:  return "factoring";
        default:                         return "unknown";
    }
}

// ─── formatProof 

std::string formatProof(const std::vector<ProofStep>& proof,
                        const ClauseStore& store, const TermBank& bank) {
    std::string result;
    for (size_t i = 0; i < proof.size(); ++i) {
        const auto& step = proof[i];
        const Clause& c = store.getClause(step.clause_id);

        result += std::to_string(i + 1) + ". ";
        result += "[" + ruleToString(step.rule) + "] ";
        result += "c" + std::to_string(step.clause_id) + ": ";
        result += clauseToString(c, bank);

        // Show parents for derived clauses
        if (step.parent1 != kInvalidId) {
            result += "  [from c" + std::to_string(step.parent1);
            if (step.parent2 != kInvalidId) {
                result += ", c" + std::to_string(step.parent2);
            }
            result += "]";
        }

        result += "\n";
    }
    return result;
}

}  // namespace atp
