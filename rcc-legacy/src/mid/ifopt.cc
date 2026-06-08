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

/// @file ifopt.cc
/// @brief If-conversion: collapses if-then[-else] diamond CFG into straight-line block + sel
/// instructions.
///
/// See docs/ifopt.md for architectural notes.

#include "rcc/all.h"

namespace rcc {

namespace {

enum {
    MaxIns = 2,
    MaxPhis = 2,
};

bool okbranch(Blk* b) {
    int n = 0;
    for (Ins* i = b->ins; i < &b->ins[b->nins]; ++i) {
        if (i->op == Odbgloc)
            continue;
        if (pinned(i))
            return false;
        if (i->op != Onop)
            ++n;
    }
    return n <= MaxIns;
}

bool okjoin(Blk* b) {
    int n = 0;
    for (Phi* p = b->phi; p; p = p->link) {
        if (KBASE(p->cls) != 0)
            return false;
        ++n;
    }
    return n <= MaxPhis;
}

bool okgraph(Blk* ifb, Blk* thenb, Blk* elseb, Blk* joinb) {
    assert(thenb != elseb);
    return joinb->npred == 2 && okjoin(joinb) && (thenb == ifb || okbranch(thenb)) &&
           (elseb == ifb || okbranch(elseb));
}

void convert(Ctx* ctx, Blk* ifb, Blk* thenb, Blk* elseb, Blk* joinb) {
    Ins* ins = static_cast<Ins*>(vnew(ctx, 0, sizeof ins[0], PHeap));
    uint nins = 0;

    addbins(ctx, &ins, &nins, ifb);
    if (thenb != ifb)
        addbins(ctx, &ins, &nins, thenb);
    if (elseb != ifb)
        addbins(ctx, &ins, &nins, elseb);

    assert(joinb->npred == 2);
    Ins sel = {};
    if (joinb->phi) {
        sel.op = Osel0;
        sel.cls = static_cast<uint>(Kw);
        sel.arg[0] = ifb->jmp.arg;
        addins(ctx, &ins, &nins, &sel);
    }

    sel = {};
    sel.op = Osel1;
    for (Phi* p = joinb->phi; p; p = p->link) {
        sel.to = p->to;
        sel.cls = static_cast<uint>(p->cls);
        sel.arg[0] = phiarg(p, thenb);
        sel.arg[1] = phiarg(p, elseb);
        addins(ctx, &ins, &nins, &sel);
    }

    idup(ctx, ifb, ins, nins);
    ifb->jmp.type = Jjmp;
    ifb->jmp.arg = Ref::none();
    ifb->s1 = joinb;
    ifb->s2 = nullptr;
    joinb->npred = 1;
    joinb->pred[0] = ifb;
    joinb->phi = nullptr;
    vfree(ins);
}

}  // namespace

void ifconvert(Ctx* ctx, Fn* fn) {
    if (debug['K'])
        fputs("\n> If-conversion:\n", ctx->dbg_buf);

    Blk *thenb, *elseb, *joinb;
    for (Blk* ifb = fn->start; ifb; ifb = ifb->link)
        if (ifgraph(ifb, &thenb, &elseb, &joinb) && okgraph(ifb, thenb, elseb, joinb)) {
            if (debug['K'])
                fprintf(ctx->dbg_buf, "    @%s -> @%s, @%s -> @%s\n", ifb->name, thenb->name,
                        elseb->name, joinb->name);
            convert(ctx, ifb, thenb, elseb, joinb);
        }

    if (debug['K']) {
        fprintf(ctx->dbg_buf, "\n> After if-conversion:\n");
        printfn(fn, ctx->dbg_buf);
    }
}

}  // namespace rcc
