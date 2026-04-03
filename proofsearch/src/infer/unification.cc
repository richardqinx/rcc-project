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


#include "atp/infer/unification.h"
#include "atp/core/types.h"
#include "atp/infer/substitution.h"

namespace atp {

    bool unify(const TermBank& bank, TermId t1, TermId t2, Substitution& subst,
           const UnificationConfig& config) {
        t1 = subst.resolve(t1, bank);
        t2 = subst.resolve(t2, bank);

        if (t1 == t2) {
            return true;
        }

        if (bank.isVariable(t1)) {
            if (config.enable_occurs_check && subst.occursIn(t1, t2, bank)) {
                return false;
            }
            subst.bind(t1, t2);
            return true;
        }

        if (bank.isVariable(t2)) {
            if (config.enable_occurs_check && subst.occursIn(t2, t1, bank)) {
                return false;
            }
            subst.bind(t2, t1);
            return true;
        }

        const Term& term1 = bank.getTerm(t1);
        const Term& term2 = bank.getTerm(t2);

        if (term1.symbol_id != term2.symbol_id || term1.arity() != term2.arity()) {
            return false;
        }

        for (size_t i = 0; i < term1.arity(); ++i) {
            if (!unify(bank, term1.args[i], term2.args[i], subst, config)) {
                return false;
            }
        }

        return true;
    }

    TermId applySubstitution(const Substitution& subst, TermId term, TermBank& bank) {
        term = subst.resolve(term, bank);
        if (bank.isVariable(term)) {
            return term;
        }

        // IMPORTANT: Copy args and symbol_id BEFORE recursing.
        // Recursive calls may call bank.makeTerm() → realloc terms_ → invalidate refs.
        const Term& t = bank.getTerm(term);
        SymbolId sym = t.symbol_id;
        std::vector<TermId> old_args(t.args);  // copy!

        std::vector<TermId> new_args;
        new_args.reserve(old_args.size());
        for (TermId arg : old_args) {
            new_args.push_back(applySubstitution(subst, arg, bank));
        }
        return bank.makeTerm(sym, new_args);
    }

}  // namespace atp