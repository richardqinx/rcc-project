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

#include "atp/infer/substitution.h"

#include "atp/core/term.h"
#include "atp/core/term_bank.h"
#include "atp/core/types.h"

namespace atp {

bool Substitution::bind(TermId var, TermId term) {
    auto [it, inserted] = bindings_.emplace(var, term);
    if (!inserted) {
        // Already bound — succeed only if bound to the same term
        return it->second == term;
    }
    return true;
}

TermId Substitution::lookup(TermId var) const {
    auto it = bindings_.find(var);
    if (it != bindings_.end()) {
        return it->second;
    }
    return kInvalidId;
}

TermId Substitution::resolve(TermId id, const TermBank& bank) const {
    // Walk the chain: X → Y → Z → f(a)
    // Stop when we hit a non-variable or an unbound variable.
    while (bank.isVariable(id)) {
        auto it = bindings_.find(id);
        if (it == bindings_.end()) {
            break;  // Unbound variable — stop here
        }
        id = it->second;
    }
    return id;
}

TermId Substitution::apply(TermId term, TermBank& bank) const {
    // First, resolve through variable chain
    term = resolve(term, bank);

    if (bank.isVariable(term)) {
        // Free variable (unbound) — leave as-is
        return term;
    }

    // IMPORTANT: Copy symbol_id and args BEFORE recursing.
    // Recursive apply() calls may trigger bank.makeTerm() → realloc → dangling ref.
    const Term& t = bank.getTerm(term);
    SymbolId sym = t.symbol_id;
    std::vector<TermId> old_args(t.args);  // copy!

    if (old_args.empty()) {
        // Constant or 0-arity — no substitution needed
        return term;
    }

    // Recursively apply to all arguments
    std::vector<TermId> new_args;
    new_args.reserve(old_args.size());
    bool changed = false;
    for (TermId arg : old_args) {
        TermId new_arg = apply(arg, bank);
        new_args.push_back(new_arg);
        if (new_arg != arg) {
            changed = true;
        }
    }

    if (!changed) {
        return term;
    }

    return bank.makeTerm(sym, new_args);
}

bool Substitution::occursIn(TermId var, TermId term, const TermBank& bank) const {
    term = resolve(term, bank);

    if (var == term) {
        return true;
    }

    if (bank.isVariable(term)) {
        return false;  // Different unbound variable
    }

    const Term& t = bank.getTerm(term);
    for (TermId arg : t.args) {
        if (occursIn(var, arg, bank)) {
            return true;
        }
    }
    return false;
}

void Substitution::clear() {
    bindings_.clear();
}

}  // namespace atp