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

/// @file irgen_expr.cc
/// @brief Expression evaluation + short-circuit conditional jump

#include "rcc/irgen.h"

namespace rcc {

int IRGen::ref_cls_of(Ref r, Fn* fn) {
    if (rtype(r) == RTmp && r.val >= Tmp0)
        return fn->tmp[r.val].cls;
    if (rtype(r) == RCon) {
        Con* c = &fn->con[r.val];
        if (c->type == Con::CBits && c->flt)
            return Ks;
    }
    return Kw;
}

Ref IRGen::coerce(Ref v, int target_cls) {
    int cur = ref_cls_of(v, cur_fn_);
    if (cur == target_cls)
        return v;
    if (target_cls == Ks && cur == Kw)
        return emit_ins(Oswtof, Ks, v, Ref::none());
    if (target_cls == Kw && cur == Ks)
        return emit_ins(Ostosi, Kw, v, Ref::none());
    return v;
}

// Expression evaluation

Ref IRGen::visit_expr(ASTNode* node) {
    switch (node->kind) {
        case NodeKind::IntLiteral:
            return make_int_ref(static_cast<IntLiteralNode*>(node)->value);

        case NodeKind::FloatLiteral:
            return make_float_ref(static_cast<FloatLiteralNode*>(node)->value);

        case NodeKind::LVal: {
            auto* lv = static_cast<LValNode*>(node);
            Symbol* sym = sym_.lookup(lv->name);
            if (!sym)
                die("undefined variable");

            // Constant scalar: return constant value directly
            if (sym->is_const && !sym->is_array && lv->indices.empty()) {
                if (auto* iv = std::get_if<int32_t>(&sym->const_val))
                    return make_int_ref(*iv);
                if (auto* fv = std::get_if<float>(&sym->const_val))
                    return make_float_ref(*fv);
            }

            // Array without index -> return address
            if (sym->is_array && lv->indices.empty())
                return get_var_addr_ref(sym);

            if (sym->is_array && !lv->indices.empty()) {
                if (lv->indices.size() < sym->dims.size() + (sym->is_param ? 1 : 0))
                    return compute_array_addr(lv);
                Ref addr = compute_array_addr(lv);
                return emit_load(addr, btype_to_cls(sym->type));
            }

            // Global variable
            if (sym->is_global) {
                Ref addr = make_global_ref(sym->name.c_str());
                return emit_load(addr, btype_to_cls(sym->type));
            }

            // Alloca variable
            if (sym->alloca_temp_id >= 0) {
                Ref addr = Ref::tmp(static_cast<uint>(sym->alloca_temp_id));
                return emit_load(addr, btype_to_cls(sym->type));
            }

            // Normal local scalar: Braun
            int cls = btype_to_cls(sym->type);
            return read_variable(sym->uid, cur_blk_, cls);
        }

        case NodeKind::BinaryExpr: {
            auto* bin = static_cast<BinaryExprNode*>(node);

            // Logical operation -> short-circuit evaluation -> phi(1,0)
            if (bin->op == BinOp::And || bin->op == BinOp::Or) {
                Blk* true_bb = new_blk("cond.true");
                Blk* false_bb = new_blk("cond.false");
                Blk* merge_bb = new_blk("cond.merge");

                visit_cond(node, true_bb, false_bb);

                seal_block(true_bb);
                cur_blk_ = true_bb;
                emit_jmp(merge_bb);

                seal_block(false_bb);
                cur_blk_ = false_bb;
                emit_jmp(merge_bb);

                seal_block(merge_bb);
                cur_blk_ = merge_bb;

                // Phi: true→1, false→0
                auto* phi = static_cast<Phi*>(alloc(ctx_, sizeof(Phi)));
                memset(phi, 0, sizeof(Phi));
                phi->cls = Kw;
                phi->to = new_tmp_ref(Kw);
                phi->narg = 2;
                phi->arg = static_cast<Ref*>(alloc(ctx_, 2 * sizeof(Ref)));
                phi->blk = static_cast<Blk**>(alloc(ctx_, 2 * sizeof(Blk*)));
                phi->arg[0] = make_int_ref(1);
                phi->blk[0] = true_bb;
                phi->arg[1] = make_int_ref(0);
                phi->blk[1] = false_bb;
                phi->link = merge_bb->phi;
                merge_bb->phi = phi;
                return phi->to;
            }

            Ref lhs = visit_expr(bin->lhs.get());
            Ref rhs = visit_expr(bin->rhs.get());

            int lk = ref_cls_of(lhs, cur_fn_);
            int rk = ref_cls_of(rhs, cur_fn_);

            bool is_float = (lk == Ks || rk == Ks);
            if (is_float && lk == Kw)
                lhs = emit_ins(Oswtof, Ks, lhs, Ref::none());
            if (is_float && rk == Kw)
                rhs = emit_ins(Oswtof, Ks, rhs, Ref::none());

            int rt = is_float ? Ks : Kw;
            switch (bin->op) {
                case BinOp::Add:
                    return emit_ins(Oadd, rt, lhs, rhs);
                case BinOp::Sub:
                    return emit_ins(Osub, rt, lhs, rhs);
                case BinOp::Mul:
                    return emit_ins(Omul, rt, lhs, rhs);
                case BinOp::Div:
                    return emit_ins(Odiv, rt, lhs, rhs);
                case BinOp::Mod:
                    return emit_ins(Orem, rt, lhs, rhs);
                case BinOp::Lt:
                    return emit_ins(is_float ? Oclts : Ocsltw, Kw, lhs, rhs);
                case BinOp::Gt:
                    return emit_ins(is_float ? Ocgts : Ocsgtw, Kw, lhs, rhs);
                case BinOp::Le:
                    return emit_ins(is_float ? Ocles : Ocslew, Kw, lhs, rhs);
                case BinOp::Ge:
                    return emit_ins(is_float ? Ocges : Ocsgew, Kw, lhs, rhs);
                case BinOp::Eq:
                    return emit_ins(is_float ? Oceqs : Oceqw, Kw, lhs, rhs);
                case BinOp::Ne:
                    return emit_ins(is_float ? Ocnes : Ocnew, Kw, lhs, rhs);
                default:
                    return make_int_ref(0);
            }
        }

        case NodeKind::UnaryExpr: {
            auto* u = static_cast<UnaryExprNode*>(node);
            Ref operand = visit_expr(u->operand.get());
            switch (u->op) {
                case UnaryOp::Pos:
                    return operand;
                case UnaryOp::Neg: {
                    int k = ref_cls_of(operand, cur_fn_);
                    if (k == Ks)
                        return emit_ins(Oneg, Ks, operand, Ref::none());
                    return emit_ins(Osub, Kw, make_int_ref(0), operand);
                }
                case UnaryOp::Not:
                    return emit_ins(Oceqw, Kw, operand, make_int_ref(0));
            }
            return make_int_ref(0);
        }

        case NodeKind::FuncCall: {
            auto* call_node = static_cast<FuncCallNode*>(node);

            // starttime/stoptime special handling
            if (call_node->name == "starttime" || call_node->name == "stoptime") {
                std::string real_name = "_sysy_" + call_node->name;
                Ref fn_ref = make_global_ref(real_name.c_str());
                Ref line_ref = make_int_ref(call_node->lineno);

                // arg w, line
                Ins arg_i;
                arg_i.op = Oarg;
                arg_i.cls = Kw;
                arg_i.to = Ref::none();
                arg_i.arg[0] = line_ref;
                arg_i.arg[1] = Ref::none();
                push_ins(arg_i);

                // call
                Ins call_i;
                call_i.op = Ocall;
                call_i.cls = Kw;
                call_i.to = Ref::none();
                call_i.arg[0] = fn_ref;
                call_i.arg[1] = Ref::none();
                push_ins(call_i);
                return make_int_ref(0);
            }

            FuncSig* fsig = sym_.lookup_func(call_node->name);
            bool is_void = fsig && fsig->ret_type == BType::Void;
            int ret_cls = fsig ? btype_to_cls(fsig->ret_type) : Kw;

            std::vector<Ref> arg_refs;
            std::vector<int> arg_clses;
            arg_refs.reserve(call_node->args.size());
            arg_clses.reserve(call_node->args.size());
            for (size_t i = 0; i < call_node->args.size(); i++) {
                Ref v = visit_expr(call_node->args[i].get());
                int acls = Kw;
                if (fsig && i < fsig->params.size())
                    acls = fsig->params[i].second ? Kl : btype_to_cls(fsig->params[i].first);

                if (acls == Kw || acls == Ks)
                    v = coerce(v, acls);
                arg_refs.push_back(v);
                arg_clses.push_back(acls);
            }
            for (size_t i = 0; i < arg_refs.size(); i++) {
                Ins arg_i;
                arg_i.op = Oarg;
                arg_i.cls = static_cast<uint>(arg_clses[i]);
                arg_i.to = Ref::none();
                arg_i.arg[0] = arg_refs[i];
                arg_i.arg[1] = Ref::none();
                push_ins(arg_i);
            }

            // call instruction
            Ref fn_ref = make_global_ref(call_node->name.c_str());
            if (is_void) {
                Ins call_i;
                call_i.op = Ocall;
                call_i.cls = Kw;
                call_i.to = Ref::none();
                call_i.arg[0] = fn_ref;
                call_i.arg[1] = Ref::none();
                push_ins(call_i);
                cur_fn_->leaf = 0;
                return make_int_ref(0);
            }

            Ref dst = new_tmp_ref(ret_cls);
            Ins call_i;
            call_i.op = Ocall;
            call_i.cls = static_cast<uint>(ret_cls);
            call_i.to = dst;
            call_i.arg[0] = fn_ref;
            call_i.arg[1] = Ref::none();
            push_ins(call_i);
            cur_fn_->leaf = 0;
            return dst;
        }

        case NodeKind::ImplicitCast: {
            auto* c = static_cast<ImplicitCastNode*>(node);
            Ref v = visit_expr(c->operand.get());
            int k = ref_cls_of(v, cur_fn_);
            if (c->target_type == BType::Float && k == Kw)
                return emit_ins(Oswtof, Ks, v, Ref::none());
            if (c->target_type == BType::Int && k == Ks)
                return emit_ins(Ostosi, Kw, v, Ref::none());
            return v;
        }

        default:
            return make_int_ref(0);
    }
}

// Short-circuit conditional jump

void IRGen::visit_cond(ASTNode* cond, Blk* true_bb, Blk* false_bb) {
    if (auto* bin = dynamic_cast<BinaryExprNode*>(cond)) {
        if (bin->op == BinOp::And)
            return visit_cond_and(bin, true_bb, false_bb);
        if (bin->op == BinOp::Or)
            return visit_cond_or(bin, true_bb, false_bb);
    }
    Ref val = visit_expr(cond);
    emit_jnz(val, true_bb, false_bb);
}

void IRGen::visit_cond_and(BinaryExprNode* node, Blk* true_bb, Blk* false_bb) {
    Blk* rhs_bb = new_blk("and.rhs");
    visit_cond(node->lhs.get(), rhs_bb, false_bb);
    seal_block(rhs_bb);
    cur_blk_ = rhs_bb;
    visit_cond(node->rhs.get(), true_bb, false_bb);
}

void IRGen::visit_cond_or(BinaryExprNode* node, Blk* true_bb, Blk* false_bb) {
    Blk* rhs_bb = new_blk("or.rhs");
    visit_cond(node->lhs.get(), true_bb, rhs_bb);
    seal_block(rhs_bb);
    cur_blk_ = rhs_bb;
    visit_cond(node->rhs.get(), true_bb, false_bb);
}

}  // namespace rcc
