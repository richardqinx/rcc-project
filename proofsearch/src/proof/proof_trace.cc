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
#include "atp/core/clause_store.h"
#include "atp/core/symbol_table.h"
#include "atp/core/term.h"
#include "atp/core/term_bank.h"
#include "atp/core/types.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <stack>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace atp {

// extractProof
// DFS from empty clause back to axioms, collect all participating clauses,
// then reverse for topological order (axioms first).

std::vector<ProofStep> extractProof(const ClauseStore& store, ClauseId empty_clause_id) {
    std::vector<ProofStep> steps;
    std::unordered_set<ClauseId> visited;
    std::stack<ClauseId> worklist;
    worklist.push(empty_clause_id);

    while (!worklist.empty()) {
        const ClauseId kCid = worklist.top();
        worklist.pop();

        if (kCid == kInvalidId) {
            continue;
        }
        if (visited.contains(kCid)) {
            continue;
        }
        visited.insert(kCid);

        const Clause& c = store.getClause(kCid);
        steps.push_back({
            .clause_id_ = kCid,
            .rule_ = c.rule_,
            .parent1_ = c.parent1_,
            .parent2_ = c.parent2_,
        });

        // Recurse into parents (axioms have kInvalidId parents, which are
        // filtered by the check at the top of the loop)
        worklist.push(c.parent1_);
        worklist.push(c.parent2_);
    }

    // Reverse: axioms (leaves) first → empty clause (root) last
    std::ranges::reverse(steps);
    return steps;
}

// termToString

std::string termToString(TermId id, const TermBank& bank) {
    const Term& t = bank.getTerm(id);
    std::string name(bank.symbols().getName(t.symbol_id_));

    if (t.args_.empty()) {
        return name;
    }

    std::string result = name + "(";
    for (size_t i = 0; i < t.args_.size(); ++i) {
        if (i > 0) {
            result += ",";
        }
        result += termToString(t.args_[i], bank);
    }
    result += ")";
    return result;
}

// Pretty variable names

namespace {

/// Generate a human-readable variable name from an index.
/// 0→X, 1→Y, 2→Z, 3→W, 4→V, 5→U, 6→X1, 7→Y1, ...
std::string prettyVarName(size_t index) {
    static constexpr std::array<char, 6> kBaseNames = {'X', 'Y', 'Z', 'W', 'V', 'U'};
    static constexpr size_t kBaseCount = kBaseNames.size();

    const char kBase = kBaseNames[index % kBaseCount];
    const size_t kSuffix = index / kBaseCount;
    if (kSuffix == 0) {
        return std::string{kBase};
    }
    return std::string{kBase} + std::to_string(kSuffix);
}

/// Pretty-print a term, renaming variables to readable names.
/// `var_map` is built incrementally: each new variable gets the next name.
std::string termToPretty(TermId id, const TermBank& bank,
                         std::unordered_map<TermId, std::string>& var_map) {
    if (bank.isVariable(id)) {
        auto it = var_map.find(id);
        if (it != var_map.end()) {
            return it->second;
        }
        const std::string kName = prettyVarName(var_map.size());
        var_map[id] = kName;
        return kName;
    }

    const Term& t = bank.getTerm(id);
    std::string name(bank.symbols().getName(t.symbol_id_));

    if (t.args_.empty()) {
        return name;
    }

    std::string result = name + "(";
    for (size_t i = 0; i < t.args_.size(); ++i) {
        if (i > 0) {
            result += ",";
        }
        result += termToPretty(t.args_[i], bank, var_map);
    }
    result += ")";
    return result;
}

// ruleToString

std::string ruleToString(InferenceRule rule) {
    switch (rule) {
        case InferenceRule::kInput:
            return "input";
        case InferenceRule::kResolution:
            return "resolution";
        case InferenceRule::kFactoring:
            return "factoring";
        default:
            return "unknown";
    }
}

}  // namespace

// clauseToString

std::string clauseToString(const Clause& clause, const TermBank& bank) {
    if (clause.isEmpty()) {
        return "□";
    }

    // Per-clause variable renaming: _R109 → X, _R286 → Y, etc.
    std::unordered_map<TermId, std::string> var_map;

    std::string result;
    for (size_t i = 0; i < clause.literals_.size(); ++i) {
        if (i > 0) {
            result += " ∨ ";
        }
        if (!clause.literals_[i].is_positive_) {
            result += "¬";
        }
        result += termToPretty(clause.literals_[i].atom_, bank, var_map);
    }
    return result;
}

// formatProof

std::string formatProof(const std::vector<ProofStep>& proof, const ClauseStore& store,
                        const TermBank& bank) {
    // Build a local renumbering: internal ClauseId → sequential proof step number
    std::unordered_map<ClauseId, size_t> id_to_step;
    for (size_t i = 0; i < proof.size(); ++i) {
        id_to_step[proof[i].clause_id_] = i + 1;  // 1-based
    }

    auto local_name = [&](ClauseId cid) -> std::string {
        auto it = id_to_step.find(cid);
        if (it != id_to_step.end()) {
            return std::to_string(it->second);
        }
        // Fallback: shouldn't happen in a valid proof
        return "?" + std::to_string(cid);
    };

    std::string result;
    for (size_t i = 0; i < proof.size(); ++i) {
        const auto& step = proof[i];
        const Clause& c = store.getClause(step.clause_id_);

        result += std::to_string(i + 1) + ". ";
        result += "[" + ruleToString(step.rule_) + "] ";
        result += clauseToString(c, bank);

        // Show parents for derived clauses
        if (step.parent1_ != kInvalidId) {
            result += "  [" + local_name(step.parent1_);
            if (step.parent2_ != kInvalidId) {
                result += ", " + local_name(step.parent2_);
            }
            result += "]";
        }

        result += "\n";
    }
    return result;
}

}  // namespace atp
