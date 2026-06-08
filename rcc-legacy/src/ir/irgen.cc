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

/// @file irgen.cc
/// @brief AST -> SSA IR: Main framework, Braun SSA, statement translation

#include "rcc/irgen.h"

#include <cstddef>

namespace rcc {

// BType -> K class
int IRGen::btype_to_cls(BType t) {
    return (t == BType::Float) ? Ks : Kw;
}

// Constant / Ref Construction
Fn* IRGen::new_fn(const char* name) {
    auto* fn = static_cast<Fn*>(alloc(ctx_, sizeof(Fn)));
    memset(fn, 0, sizeof(Fn));
    fn->name = strf(ctx_, PFn, "%s", name);

    // Initialize tmp array
    fn->ntmp = 0;
    fn->tmp = static_cast<Tmp*>(vnew(ctx_, 0, sizeof(Tmp), PFn));
    for (int i = 0; i < Tmp0; i++) {
        int cls = (T.fpr0 <= i && i < T.fpr0 + T.nfpr) ? Kd : Kl;
        newtmp(ctx_, nullptr, cls, fn);
    }

    // Initialize con array: con[0] = CUndef (corresponds to R/UNDEF)
    fn->ncon = 2;
    fn->con = static_cast<Con*>(vnew(ctx_, static_cast<ulong>(fn->ncon), sizeof(Con), PFn));
    memset(fn->con, 0, 2 * sizeof(Con));
    fn->con[0].type = Con::CUndef;
    // con[1] = CBits{0} (corresponds to CON_Z)
    fn->con[1].type = Con::CBits;
    fn->con[1].bits.i = 0;

    fn->retty = -1;
    fn->slot = 0;
    fn->salign = 4;
    // All user functions visible to linker
    fn->lnk.export_ = 1;
    return fn;
}

Blk* IRGen::new_blk(const char* name) {
    auto* b = static_cast<Blk*>(alloc(ctx_, sizeof(Blk)));
    memset(b, 0, sizeof(Blk));
    b->id = cur_fn_->nblk++;
    b->name = strf(ctx_, PFn, "%s.%u", name, b->id);
    b->ins = static_cast<Ins*>(vnew(ctx_, 4, sizeof(Ins), PFn));
    b->nins = 0;
    b->visit = 0;

    // Append to function block list tail
    if (!cur_fn_->start) {
        cur_fn_->start = b;
    } else {
        Blk* tail = cur_fn_->start;
        while (tail->link)
            tail = tail->link;
        tail->link = b;
    }

    blk_vec_.push_back(b);
    return b;
}

Blk* IRGen::get_blk(uint id) {
    return blk_vec_[id];
}

bool IRGen::is_sealed(Blk* b) const {
    auto it = sealed_.find(b->id);
    return it != sealed_.end() && it->second;
}

// Constant / Ref Construction
Ref IRGen::make_int_ref(int32_t val) {
    return getcon(ctx_, static_cast<int64_t>(val), cur_fn_);
}

Ref IRGen::make_float_ref(float val) {
    Con c;
    memset(&c, 0, sizeof(Con));
    c.type = Con::CBits;
    c.flt = 1;
    c.bits.s = val;
    return newcon(ctx_, &c, cur_fn_);
}

Ref IRGen::make_global_ref(const char* name) {
    Con c;
    memset(&c, 0, sizeof(Con));
    c.type = Con::CAddr;
    c.sym.type = Sym::SGlo;
    c.sym.id = intern(name);
    return newcon(ctx_, &c, cur_fn_);
}

Ref IRGen::make_global_addr_ref(const char* name) {
    Ref sym = make_global_ref(name);
    Ref dst = new_tmp_ref(Kl);
    Ins i{};
    i.op = Oaddr;
    i.cls = Kl;
    i.to = dst;
    i.arg[0] = sym;
    i.arg[1] = Ref::none();
    push_ins(i);
    return dst;
}

Ref IRGen::new_tmp_ref(int cls) {
    return newtmp(ctx_, "t", cls, cur_fn_);
}

// Entry
IROutput IRGen::generate(CompUnitNode* root) {
    output_ = {};
    sym_.register_builtins();
    sym_.enter_scope();
    visit_comp_unit(root);
    sym_.leave_scope();
    return std::move(output_);
}

void IRGen::visit_comp_unit(CompUnitNode* node) {
    for (auto& child : node->decls_and_funcs) {
        if (child->kind == NodeKind::FuncDef)
            visit_func_def(static_cast<FuncDefNode*>(child.get()));
        else if (child->kind == NodeKind::VarDecl)
            visit_global_decl(static_cast<VarDeclNode*>(child.get()));
    }
}

// Function Definition
void IRGen::visit_func_def(FuncDefNode* node) {
    cur_fn_ = new_fn(node->name.c_str());
    cur_fn_->leaf = 1;
    blk_vec_.clear();
    current_def_.clear();
    incomplete_phis_.clear();
    sealed_.clear();

    bool is_void = (node->ret_type == BType::Void);

    cur_fn_ret_cls_ = is_void ? Kx : btype_to_cls(node->ret_type);

    // Register function signature
    FuncSig sig;
    sig.name = node->name;
    sig.ret_type = node->ret_type;

    // Entry block
    Blk* entry = new_blk("entry");
    cur_blk_ = entry;
    sealed_[entry->id] = true;

    sym_.enter_scope();

    // Process formal parameters
    for (auto& p : node->params) {
        auto* fp = static_cast<FuncFParamNode*>(p.get());
        int cls = fp->is_array ? Kl : btype_to_cls(fp->btype);
        sig.params.push_back({fp->btype, fp->is_array});

        Symbol sym;
        sym.name = fp->name;
        sym.type = fp->btype;
        sym.is_param = true;
        sym.is_array = fp->is_array;
        sym.uid = sym_.allocate_uid();
        if (fp->is_array) {
            for (auto& d : fp->dims)
                sym.dims.push_back(eval_const_int(d.get()));
        }

        // Parameter as SSA value: emit Opar instruction (one %t =cls par per parameter)
        Ref param_ref = new_tmp_ref(cls);
        Ins par_i;
        par_i.op = Opar;
        par_i.cls = static_cast<uint>(cls);
        par_i.to = param_ref;
        par_i.arg[0] = Ref::none();
        par_i.arg[1] = Ref::none();
        push_ins(par_i);

        write_variable(sym.uid, cur_blk_, param_ref);
        sym_.insert(fp->name, std::move(sym));
    }

    sym_.register_func(std::move(sig));

    // Traverse function body
    visit_block(static_cast<BlockNode*>(node->body.get()));

    // Ensure last block is terminated
    if (!cur_blk_terminated()) {
        if (is_void) {
            emit_ret_void();
        } else {
            Ref zero = (cur_fn_ret_cls_ == Ks) ? make_float_ref(0.0f) : make_int_ref(0);
            emit_ret(zero, cur_fn_ret_cls_);
        }
    }

    sym_.leave_scope();
    output_.functions.push_back(cur_fn_);
    cur_fn_ = nullptr;
}

// Global Declaration
void IRGen::visit_global_decl(VarDeclNode* node) {
    for (auto& d : node->defs) {
        auto* def = static_cast<VarDefNode*>(d.get());
        Symbol sym;
        sym.name = def->name;
        sym.type = node->btype;
        sym.is_const = node->is_const;
        sym.is_global = true;

        for (auto& dim_expr : def->dims)
            sym.dims.push_back(eval_const_int(dim_expr.get()));
        sym.is_array = !sym.dims.empty();

        GlobalData gd;
        gd.name = def->name;
        gd.elem_cls = btype_to_cls(node->btype);
        gd.is_const = node->is_const;
        gd.total_elems = 1;
        for (int d2 : sym.dims)
            gd.total_elems *= static_cast<size_t>(d2);
        if (!sym.is_array)
            gd.total_elems = 1;

        if (def->init_val) {
            if (sym.is_array) {
                std::vector<ASTNode*> flat;
                flatten_init(sym.dims, def->init_val.get(), flat);
                for (size_t i = 0; i < gd.total_elems; i++) {
                    if (i < flat.size() && flat[i]) {
                        if (node->btype == BType::Int)
                            gd.init_vals.push_back(eval_const_int(flat[i]));
                        else
                            gd.init_vals.push_back(eval_const_float(flat[i]));
                    } else {
                        if (node->btype == BType::Int)
                            gd.init_vals.push_back(static_cast<int32_t>(0));
                        else
                            gd.init_vals.push_back(0.0f);
                    }
                }
            } else {
                if (node->btype == BType::Int)
                    gd.init_vals.push_back(eval_const_int(def->init_val.get()));
                else
                    gd.init_vals.push_back(eval_const_float(def->init_val.get()));
            }
        } else {
            gd.is_zeroinit = true;
        }

        if (node->is_const && !sym.is_array && def->init_val) {
            if (node->btype == BType::Int)
                sym.const_val = eval_const_int(def->init_val.get());
            else
                sym.const_val = eval_const_float(def->init_val.get());
        }

        sym_.insert(def->name, std::move(sym));
        output_.globals.push_back(std::move(gd));
    }
}

// Local Declaration
void IRGen::visit_local_decl(VarDeclNode* node) {
    for (auto& d : node->defs) {
        auto* def = static_cast<VarDefNode*>(d.get());
        Symbol sym;
        sym.name = def->name;
        sym.type = node->btype;
        sym.is_const = node->is_const;
        sym.uid = sym_.allocate_uid();

        for (auto& dim_expr : def->dims)
            sym.dims.push_back(eval_const_int(dim_expr.get()));
        sym.is_array = !sym.dims.empty();

        int cls = btype_to_cls(node->btype);

        if (sym.is_array) {
            int total = 1;
            for (int d2 : sym.dims)
                total *= d2;
            Ref size_ref = make_int_ref(total * 4);
            // Arrays are always 8B aligned
            Ref addr = emit_ins(Oalloc8, Kl, size_ref, Ref::none());
            sym.alloca_temp_id = static_cast<int>(addr.val);

            if (def->init_val) {
                std::vector<ASTNode*> flat;
                flatten_init(sym.dims, def->init_val.get(), flat);
                // Count implicit zero tail
                constexpr int kBulkZeroThreshold = 65536;
                size_t nonzero_tail = flat.size();
                while (nonzero_tail > 0 && flat[nonzero_tail - 1] == nullptr)
                    nonzero_tail--;
                int zero_count = total - static_cast<int>(nonzero_tail);
                bool bulk = zero_count >= kBulkZeroThreshold;
                if (bulk)
                    emit_bulk_zero(addr, total * 4);
                size_t up_to = bulk ? nonzero_tail : static_cast<size_t>(total);
                for (size_t i = 0; i < up_to; i++) {
                    bool has_init = i < flat.size() && flat[i] != nullptr;
                    if (bulk && !has_init)
                        continue;  // bulk-zero covered
                    Ref off = make_int_ref(static_cast<int>(i * 4));
                    Ref elem_addr = emit_ins(Oadd, Kl, addr, off);
                    Ref val;
                    if (has_init) {
                        val = visit_expr(flat[i]);
                        val = coerce(val, cls);
                    } else {
                        val = (cls == Ks) ? make_float_ref(0.0f) : make_int_ref(0);
                    }
                    emit_store(val, elem_addr, cls);
                }
            }
            sym_.insert(def->name, std::move(sym));
        } else {
            if (node->is_const && def->init_val) {
                if (node->btype == BType::Int)
                    sym.const_val = eval_const_int(def->init_val.get());
                else
                    sym.const_val = eval_const_float(def->init_val.get());
            }
            int uid = sym.uid;
            sym_.insert(def->name, std::move(sym));

            if (def->init_val) {
                Ref val = visit_expr(def->init_val.get());
                val = coerce(val, cls);
                write_variable(uid, cur_blk_, val);
            }
        }
    }
}

// Statements
void IRGen::visit_block(BlockNode* node) {
    sym_.enter_scope();
    for (auto& item : node->items) {
        if (cur_blk_terminated())
            break;
        visit_stmt(item.get());
    }
    sym_.leave_scope();
}

void IRGen::visit_stmt(ASTNode* node) {
    if (cur_blk_terminated())
        return;
    switch (node->kind) {
        case NodeKind::VarDecl:
            visit_local_decl(static_cast<VarDeclNode*>(node));
            break;
        case NodeKind::AssignStmt:
            visit_assign(static_cast<AssignStmtNode*>(node));
            break;
        case NodeKind::ExprStmt: {
            auto* es = static_cast<ExprStmtNode*>(node);
            if (es->expr)
                visit_expr(es->expr.get());
            break;
        }
        case NodeKind::Block:
            visit_block(static_cast<BlockNode*>(node));
            break;
        case NodeKind::IfStmt:
            visit_if(static_cast<IfStmtNode*>(node));
            break;
        case NodeKind::WhileStmt:
            visit_while(static_cast<WhileStmtNode*>(node));
            break;
        case NodeKind::ReturnStmt:
            visit_return(static_cast<ReturnStmtNode*>(node));
            break;
        case NodeKind::BreakStmt:
            if (!loop_stack_.empty())
                emit_jmp(loop_stack_.back().exit_block);
            break;
        case NodeKind::ContinueStmt:
            if (!loop_stack_.empty())
                emit_jmp(loop_stack_.back().cond_block);
            break;
        default:
            break;
    }
}

void IRGen::visit_assign(AssignStmtNode* node) {
    Ref rhs = visit_expr(node->expr.get());
    auto* lv = static_cast<LValNode*>(node->lval.get());
    Symbol* sym = sym_.lookup(lv->name);
    int cls = btype_to_cls(sym->type);
    // Cross-boundary type coordination
    rhs = coerce(rhs, cls);

    if (!lv->indices.empty() || sym->is_array || sym->is_global) {
        Ref addr;
        if (!lv->indices.empty())
            addr = compute_array_addr(lv);
        else
            addr = get_var_addr_ref(sym);
        emit_store(rhs, addr, cls);
    } else {
        write_variable(sym->uid, cur_blk_, rhs);
    }
}

void IRGen::visit_if(IfStmtNode* node) {
    Blk* then_bb = new_blk("if.then");
    Blk* else_bb = node->else_stmt ? new_blk("if.else") : nullptr;
    Blk* merge_bb = new_blk("if.merge");
    Blk* false_target = else_bb ? else_bb : merge_bb;

    visit_cond(node->cond.get(), then_bb, false_target);

    seal_block(then_bb);
    cur_blk_ = then_bb;
    visit_stmt(node->then_stmt.get());
    if (!cur_blk_terminated())
        emit_jmp(merge_bb);

    if (else_bb) {
        seal_block(else_bb);
        cur_blk_ = else_bb;
        visit_stmt(node->else_stmt.get());
        if (!cur_blk_terminated())
            emit_jmp(merge_bb);
    }

    seal_block(merge_bb);
    cur_blk_ = merge_bb;
}

void IRGen::visit_while(WhileStmtNode* node) {
    Blk* cond_bb = new_blk("while.cond");
    Blk* body_bb = new_blk("while.body");
    Blk* exit_bb = new_blk("while.exit");

    emit_jmp(cond_bb);

    cur_blk_ = cond_bb;
    visit_cond(node->cond.get(), body_bb, exit_bb);

    seal_block(body_bb);
    cur_blk_ = body_bb;
    loop_stack_.push_back({cond_bb, exit_bb});
    visit_stmt(node->body.get());
    loop_stack_.pop_back();
    if (!cur_blk_terminated())
        emit_jmp(cond_bb);

    seal_block(cond_bb);
    seal_block(exit_bb);
    cur_blk_ = exit_bb;
}

void IRGen::visit_return(ReturnStmtNode* node) {
    if (node->expr) {
        Ref val = visit_expr(node->expr.get());
        // cur_fn_ret_cls_ is set at visit_func_def entry based on AST ret_type; void functions
        // won't reach here.
        assert(cur_fn_ret_cls_ != Kx && "non-void return in void function");
        val = coerce(val, cur_fn_ret_cls_);
        emit_ret(val, cur_fn_ret_cls_);
    } else {
        emit_ret_void();
    }
}

// Braun SSA. Variable identity key is Symbol::uid(int), avoids name collision across scopes.
void IRGen::write_variable(int uid, Blk* blk, Ref val) {
    current_def_[uid][blk->id] = val;
}

Ref IRGen::read_variable(int uid, Blk* blk, int cls) {
    auto it = current_def_[uid].find(blk->id);
    if (it != current_def_[uid].end())
        return it->second;
    return read_variable_recursive(uid, blk, cls);
}

Ref IRGen::read_variable_recursive(int uid, Blk* blk, int cls) {
    Ref val;
    uint npred = blk->npred;

    if (!is_sealed(blk)) {
        // Block not sealed: insert incomplete Phi
        auto* phi = static_cast<Phi*>(alloc(ctx_, sizeof(Phi)));
        memset(phi, 0, sizeof(Phi));
        phi->cls = static_cast<short>(cls);
        phi->to = new_tmp_ref(cls);
        phi->link = blk->phi;
        blk->phi = phi;
        val = phi->to;
        incomplete_phis_.push_back({uid, phi, blk->id, cls});
    } else if (npred == 1) {
        val = read_variable(uid, blk->pred[0], cls);
    } else if (npred == 0) {
        // Entry block, variable undefined
        val = (cls == Ks) ? make_float_ref(0.0f) : make_int_ref(0);
    } else {
        // Multiple predecessors: insert Phi
        auto* phi = static_cast<Phi*>(alloc(ctx_, sizeof(Phi)));
        memset(phi, 0, sizeof(Phi));
        phi->cls = static_cast<short>(cls);
        phi->to = new_tmp_ref(cls);
        phi->link = blk->phi;
        blk->phi = phi;
        val = phi->to;

        write_variable(uid, blk, val);  // Prevent infinite loop

        // Populate phi arguments
        phi->narg = npred;
        phi->arg = static_cast<Ref*>(alloc(ctx_, npred * sizeof(Ref)));
        phi->blk = static_cast<Blk**>(alloc(ctx_, npred * sizeof(Blk*)));
        for (uint i = 0; i < npred; i++) {
            phi->blk[i] = blk->pred[i];
            phi->arg[i] = read_variable(uid, blk->pred[i], cls);
        }

        val = try_remove_trivial_phi(phi);
    }

    write_variable(uid, blk, val);
    return val;
}

void IRGen::seal_block(Blk* blk) {
    // Snapshot to_process
    std::vector<IncompletePhi> to_process;
    for (auto it = incomplete_phis_.begin(); it != incomplete_phis_.end();) {
        if (it->block_id == blk->id) {
            to_process.push_back(std::move(*it));
            it = incomplete_phis_.erase(it);
        } else {
            ++it;
        }
    }
    sealed_[blk->id] = true;
    for (auto& p : to_process)
        add_phi_operands(p.var_uid, blk, p.phi, p.cls);
}

void IRGen::add_phi_operands(int uid, Blk* blk, Phi* phi, int cls) {
    uint npred = blk->npred;
    phi->narg = npred;
    phi->arg = static_cast<Ref*>(alloc(ctx_, npred * sizeof(Ref)));
    phi->blk = static_cast<Blk**>(alloc(ctx_, npred * sizeof(Blk*)));
    for (uint i = 0; i < npred; i++) {
        phi->blk[i] = blk->pred[i];
        phi->arg[i] = read_variable(uid, blk->pred[i], cls);
    }
    try_remove_trivial_phi(phi);
}

Ref IRGen::try_remove_trivial_phi(Phi* phi) {
    Ref same = Ref::none();
    bool same_set = false;
    for (uint i = 0; i < phi->narg; i++) {
        if (req(phi->arg[i], phi->to))
            continue;
        if (same_set && !req(phi->arg[i], same))
            return phi->to;
        same = phi->arg[i];
        same_set = true;
    }
    return same_set ? same : phi->to;
}

// Emission helpers
void IRGen::push_ins(Ins i) {
    vgrow(ctx_, &cur_blk_->ins, cur_blk_->nins + 1);
    cur_blk_->ins[cur_blk_->nins++] = i;
}

Ref IRGen::emit_ins(int op, int cls, Ref a0, Ref a1) {
    Ref dst = new_tmp_ref(cls);
    Ins i;
    i.op = static_cast<uint>(op);
    i.cls = static_cast<uint>(cls);
    i.to = dst;
    i.arg[0] = a0;
    i.arg[1] = a1;
    push_ins(i);
    return dst;
}

void IRGen::emit_store(Ref val, Ref addr, int cls) {
    int op = (cls == Ks) ? Ostores : Ostorew;
    Ins i;
    i.op = static_cast<uint>(op);
    i.cls = Kw;
    i.to = Ref::none();
    i.arg[0] = val;
    i.arg[1] = addr;
    push_ins(i);
}

Ref IRGen::emit_load(Ref addr, int cls) {
    return emit_ins(Oload, cls, addr, Ref::none());
}

void IRGen::emit_bulk_zero(Ref addr, int size_bytes) {
    // Rewrite as Ocall __rcc_memset
    Ins arg_dst{};
    arg_dst.op = Oarg;
    arg_dst.cls = Kl;
    arg_dst.to = Ref::none();
    arg_dst.arg[0] = addr;
    arg_dst.arg[1] = Ref::none();
    push_ins(arg_dst);

    Ins arg_val{};
    arg_val.op = Oarg;
    arg_val.cls = Kw;
    arg_val.to = Ref::none();
    arg_val.arg[0] = make_int_ref(0);
    arg_val.arg[1] = Ref::none();
    push_ins(arg_val);

    Ins arg_sz{};
    arg_sz.op = Oarg;
    arg_sz.cls = Kl;
    arg_sz.to = Ref::none();
    arg_sz.arg[0] = make_int_ref(size_bytes);
    arg_sz.arg[1] = Ref::none();
    push_ins(arg_sz);

    Ins call{};
    call.op = Ocall;
    call.cls = Kw;
    call.to = Ref::none();
    call.arg[0] = make_global_ref("__rcc_memset");
    call.arg[1] = Ref::none();
    push_ins(call);

    cur_fn_->leaf = 0;
}

void IRGen::emit_jmp(Blk* target) {
    if (cur_blk_terminated())
        return;
    cur_blk_->jmp.type = Jjmp;
    cur_blk_->s1 = target;
    add_edge(cur_blk_, target);
}

void IRGen::emit_jnz(Ref cond, Blk* true_bb, Blk* false_bb) {
    if (cur_blk_terminated())
        return;
    cur_blk_->jmp.type = Jjnz;
    cur_blk_->jmp.arg = cond;
    cur_blk_->s1 = true_bb;
    cur_blk_->s2 = false_bb;
    add_edge(cur_blk_, true_bb);
    add_edge(cur_blk_, false_bb);
}

void IRGen::emit_ret(Ref val, int cls) {
    // Choose retw/rets based on cls
    int jtype = (cls == Ks) ? Jrets : Jretw;
    cur_blk_->jmp.type = static_cast<short>(jtype);
    cur_blk_->jmp.arg = val;
}

void IRGen::emit_ret_void() {
    cur_blk_->jmp.type = Jret0;
}

bool IRGen::cur_blk_terminated() const {
    return cur_blk_ && cur_blk_->jmp.type != Jxxx;
}

void IRGen::add_edge(Blk* from, Blk* to) {
    // Add to 'to's pred list
    uint n = to->npred;
    if (n == 0) {
        to->pred = static_cast<Blk**>(vnew(ctx_, 2, sizeof(Blk*), PFn));
    } else {
        vgrow(ctx_, &to->pred, n + 1);
    }
    to->pred[n] = from;
    to->npred = n + 1;
}

Ref IRGen::get_var_addr_ref(Symbol* sym) {
    if (sym->is_global)
        return make_global_ref(sym->name.c_str());
    if (sym->is_param && sym->is_array)
        return read_variable(sym->uid, cur_blk_, Kl);
    return Ref::tmp(static_cast<uint>(sym->alloca_temp_id));
}

}  // namespace rcc
