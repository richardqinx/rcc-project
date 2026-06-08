/* Source code for RCC-Legacy.
   Copyright (C) 2026 Richard Qin

This file is part of RCC.

RCC is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 3, or (at your option) any later
version.

RCC is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with RCC; see the file LICENSE.  If not see
<http://www.gnu.org/licenses/>.  */

/// @file live.cc
/// @brief Backward dataflow liveness analysis.
///
/// See docs/live.md for architectural notes.

#include "rcc/all.h"

namespace rcc {

void liveon(BSet* v, Blk* b, Blk* s) {
    bscopy(v, s->in);

    // s's phi-defs.
    for (Phi* p = s->phi; p; p = p->link)
        if (rtype(p->to) == RTmp)
            bsclr(v, p->to.val);

    // phi-arg edge check.
    for (Phi* p = s->phi; p; p = p->link) {
        for (uint n = 0; n < p->narg; n++) {
            if (p->blk[n] != b)
                continue;
            if (rtype(p->arg[n]) != RTmp)
                continue;
            uint t = p->arg[n].val;
            bsset(v, t);
            bsset(b->gen, t);
        }
    }
}

namespace {

/// Add ref (if tmp) to b->gen and b->in; on first entry to b->in, increment live count for its K
/// bucket.
inline void use_ref(Ref r, Blk* b, int nlv[2], Tmp* tmp) {
    if (rtype(r) != RTmp)
        return;
    uint t = r.val;
    bsset(b->gen, t);
    if (!bshas(b->in, t)) {
        nlv[KBASE(tmp[t].cls)]++;
        bsset(b->in, t);
    }
}

/// Get maximum of two elements.
inline int imax(int a, int b) {
    return a > b ? a : b;
}

}  // namespace

void filllive(Ctx* ctx, Fn* fn) {
    BSet prev_out[1];
    BSet edge_set[1];

    bsinit(ctx, prev_out, static_cast<uint>(fn->ntmp));
    bsinit(ctx, edge_set, static_cast<uint>(fn->ntmp));

    // Initialize sets.
    for (Blk* b = fn->start; b; b = b->link) {
        bsinit(ctx, b->in, static_cast<uint>(fn->ntmp));
        bsinit(ctx, b->out, static_cast<uint>(fn->ntmp));
        bsinit(ctx, b->gen, static_cast<uint>(fn->ntmp));
    }

    // Fixpoint loop.
    bool changed = true;
    while (changed) {
        changed = false;

        // Reverse rpo scan.
        for (int n = static_cast<int>(fn->nblk) - 1; n >= 0; n--) {
            Blk* b = fn->rpo[n];

            // Merge successors liveon.
            bscopy(prev_out, b->out);

            if (b->s1) {
                liveon(edge_set, b, b->s1);
                bsunion(b->out, edge_set);
            }
            if (b->s2) {
                liveon(edge_set, b, b->s2);
                bsunion(b->out, edge_set);
            }
            if (!bsequal(b->out, prev_out))
                changed = true;

            // Initial in.
            int nlv[2] = {0, 0};
            b->out->t[0] |= T.rglob;
            bscopy(b->in, b->out);
            for (int t = 0; bsiter(b->in, &t); t++)
                nlv[KBASE(fn->tmp[t].cls)]++;

            // Terminal jump args.
            if (rtype(b->jmp.arg) == RCall) {
                assert(static_cast<int>(bscount(b->in)) == T.nrglob && b->in->t[0] == T.rglob);
                b->in->t[0] |= T.retregs(b->jmp.arg, nlv);
            } else {
                use_ref(b->jmp.arg, b, nlv, fn->tmp);
            }
            // Fused cmp+jnz check.
            use_ref(b->jmp.arg2, b, nlv, fn->tmp);

            // Initial nlive.
            for (int k = 0; k < 2; k++)
                b->nlive[k] = nlv[k];

            // Reverse scan instructions.
            for (int idx = static_cast<int>(b->nins) - 1; idx >= 0; idx--) {
                Ins* i = &b->ins[idx];

                // Post-ABI lowering call.
                if (i->op == Ocall && rtype(i->arg[1]) == RCall) {
                    int call_kill[2];
                    b->in->t[0] &= ~T.retregs(i->arg[1], call_kill);
                    for (int k = 0; k < 2; k++) {
                        nlv[k] -= call_kill[k];
                        // Midpoint call.
                        nlv[k] += T.nrsave[k];
                        b->nlive[k] = imax(b->nlive[k], nlv[k]);
                    }
                    int call_arg[2];
                    b->in->t[0] |= T.argregs(i->arg[1], call_arg);
                    for (int k = 0; k < 2; k++) {
                        nlv[k] -= T.nrsave[k];
                        nlv[k] += call_arg[k];
                    }
                }

                // def kill.
                if (!req(i->to, Ref::none())) {
                    assert(rtype(i->to) == RTmp);
                    uint t = i->to.val;
                    if (bshas(b->in, t))
                        nlv[KBASE(fn->tmp[t].cls)]--;
                    bsset(b->gen, t);
                    bsclr(b->in, t);
                }

                // args gen.
                for (int k = 0; k < 2; k++) {
                    Ref a = i->arg[k];
                    if (rtype(a) == RMem) {
                        Mem* m = &fn->mem[a.val];
                        use_ref(m->base, b, nlv, fn->tmp);
                        use_ref(m->index, b, nlv, fn->tmp);
                    } else {
                        use_ref(a, b, nlv, fn->tmp);
                    }
                }

                // Update peak.
                for (int k = 0; k < 2; k++)
                    b->nlive[k] = imax(b->nlive[k], nlv[k]);
            }
        }
    }

    // Debug dump.
    if (debug['L']) {
        FILE* f = ctx->dbg_buf;
        fprintf(f, "\n> Liveness analysis:\n");
        for (Blk* b = fn->start; b; b = b->link) {
            fprintf(f, "\t%-10sin:   ", b->name);
            dumpts(b->in, fn->tmp, f);
            fprintf(f, "\t          out:  ");
            dumpts(b->out, fn->tmp, f);
            fprintf(f, "\t          gen:  ");
            dumpts(b->gen, fn->tmp, f);
            fprintf(f, "\t          live: %d %d\n", b->nlive[0], b->nlive[1]);
        }
    }
}

}  // namespace rcc
