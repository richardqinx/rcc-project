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

/// @file tre.cc
/// @brief Tail Recursion Elimination (TRE)

#include "rcc/all.h"

#include <cstring>
#include <vector>

namespace rcc {

namespace {

struct Site {
    Blk* b;
    Ins* call;       // Points to Ocall in b->ins
    Ins* arg_first;  // [arg_first..call) is Oarg sequence
    std::vector<Ref> args;
};

bool is_self_call(Fn* fn, Ins* call, uint32_t self_id) {
    if (call->op != Ocall)
        return false;
    Ref tgt = call->arg[0];
    if (rtype(tgt) != RCon)
        return false;
    Con* c = &fn->con[tgt.val];
    if (c->type != Con::CAddr)
        return false;
    if (c->sym.type != Sym::SGlo)
        return false;
    return c->sym.id == self_id;
}

// Locate Ocall and arg-group start in b.
bool collect_site(Fn* fn, Blk* b, int npar, uint32_t self_id, Site& out) {
    if (b->nins == 0)
        return false;
    Ins* last = &b->ins[b->nins - 1];
    if (!is_self_call(fn, last, self_id))
        return false;

    // ret form matches call.to
    if (b->jmp.type == Jret0) {
        if (!req(last->to, Ref::none()))
            return false;
    } else if (b->jmp.type == Jretw || b->jmp.type == Jretl || b->jmp.type == Jrets ||
               b->jmp.type == Jretd) {
        if (!req(b->jmp.arg, last->to))
            return false;
    } else {
        return false;
    }

    // Verify all instructions before call are Oarg and count matches.
    Ins *i0 = nullptr, *i1 = nullptr;
    igroup(b, last, &i0, &i1);
    if (i1 != last + 1)
        return false;
    int argc = static_cast<int>(last - i0);
    if (argc != npar)
        return false;
    out.b = b;
    out.call = last;
    out.arg_first = i0;
    out.args.clear();
    out.args.reserve(static_cast<size_t>(npar));
    for (Ins* j = i0; j < last; j++) {
        if (!isarg(j->op))
            return false;
        out.args.push_back(j->arg[0]);
    }
    return true;
}

}  // namespace

void tre(Ctx* ctx, Fn* fn) {
    if (fn->vararg)
        return;
    if (fn->name && std::strcmp(fn->name, "main") == 0)
        return;

    // exit if any block contains isalloc
    for (Blk* b = fn->start; b; b = b->link)
        for (Ins* i = b->ins; i < &b->ins[b->nins]; i++)
            if (isalloc(i->op))
                return;

    // fn->start cannot already have phi
    if (fn->start->phi)
        return;

    // Collect Opar from start block
    std::vector<Ins*> par_inss;
    Ins* ie0 = fn->start->ins + fn->start->nins;
    for (Ins* i = fn->start->ins; i < ie0 && i->op == Opar; i++)
        par_inss.push_back(i);
    int npar = static_cast<int>(par_inss.size());

    // Reject theoretically allowed Opar variants (struct by-value) not used by SysY
    for (Ins* i = fn->start->ins; i < ie0; i++) {
        if (i->op == Oparc || i->op == Opare)
            return;
    }

    uint32_t self_id = intern(fn->name);

    // Scan qualified sites
    std::vector<Site> sites;
    for (Blk* b = fn->start; b; b = b->link) {
        if (!isret(b->jmp.type))
            continue;
        Site s;
        if (collect_site(fn, b, npar, self_id, s))
            sites.push_back(std::move(s));
    }
    if (sites.empty())
        return;

    // Transformation
    Blk* loop_header = fn->start;

    // Prepare %p_init_i for each Opar, record original %p_i and cls
    auto upar = static_cast<size_t>(npar);
    std::vector<Ref> par_init_refs(upar);
    std::vector<Ref> par_orig_refs(upar);
    std::vector<int> par_clses(upar);
    for (int i = 0; i < npar; i++) {
        size_t ui = static_cast<size_t>(i);
        par_orig_refs[ui] = par_inss[ui]->to;
        par_clses[ui] = static_cast<int>(par_inss[ui]->cls);
        par_init_refs[ui] = newtmp(ctx, "p_init", par_clses[ui], fn);
    }

    // Create @tre_entry
    Blk* tre_entry = newblk(ctx);
    tre_entry->name = strf(ctx, PFn, "tre_entry");
    for (int i = 0; i < npar; i++) {
        size_t ui = static_cast<size_t>(i);
        Ins ni = *par_inss[ui];
        ni.to = par_init_refs[ui];
        addins(ctx, &tre_entry->ins, &tre_entry->nins, &ni);
    }
    tre_entry->jmp.type = Jjmp;
    tre_entry->jmp.arg = Ref::none();
    tre_entry->s1 = loop_header;
    tre_entry->s2 = nullptr;
    tre_entry->link = loop_header;
    fn->start = tre_entry;

    // Remove head npar Opars from loop_header
    int remaining = static_cast<int>(loop_header->nins) - npar;
    if (remaining > 0)
        std::memmove(loop_header->ins, loop_header->ins + npar,
                     static_cast<size_t>(remaining) * sizeof(Ins));
    loop_header->nins = static_cast<uint>(remaining);

    // Insert npar phis at loop_header head
    int nphi_args = 1 + static_cast<int>(sites.size());
    for (int i = npar - 1; i >= 0; i--) {
        size_t ui = static_cast<size_t>(i);
        auto* p = static_cast<Phi*>(alloc(ctx, sizeof(Phi)));
        std::memset(p, 0, sizeof(Phi));
        p->to = par_orig_refs[ui];
        p->cls = static_cast<short>(par_clses[ui]);
        p->narg = static_cast<uint>(nphi_args);
        p->arg = static_cast<Ref*>(vnew(ctx, static_cast<ulong>(nphi_args), sizeof(Ref), PFn));
        p->blk = static_cast<Blk**>(vnew(ctx, static_cast<ulong>(nphi_args), sizeof(Blk*), PFn));
        p->arg[0] = par_init_refs[ui];
        p->blk[0] = tre_entry;
        for (size_t k = 0; k < sites.size(); k++) {
            p->arg[1 + k] = sites[k].args[ui];
            p->blk[1 + k] = sites[k].b;
        }
        p->link = loop_header->phi;
        loop_header->phi = p;
    }

    // Truncate block at each site and redirect jmp to loop_header
    for (auto& s : sites) {
        int new_nins = static_cast<int>(s.arg_first - s.b->ins);
        s.b->nins = static_cast<uint>(new_nins);
        s.b->jmp.type = Jjmp;
        s.b->jmp.arg = Ref::none();
        s.b->s1 = loop_header;
        s.b->s2 = nullptr;
    }

    if (debug['T']) {
        fprintf(stderr, "\n>>> after tre (%s) — %zu site(s):\n", fn->name, sites.size());
        printfn(fn, stderr);
    }
}

}  // namespace rcc
