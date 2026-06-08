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
/// @file irgen_util.cc
/// @brief Constant evaluation, array init flattening, array address computation

#include "rcc/irgen.h"

#include <cstddef>

namespace rcc {

// Compile-time constant evaluation

int32_t IRGen::eval_const_int(ASTNode* node) {
    switch (node->kind) {
        case NodeKind::IntLiteral:
            return static_cast<IntLiteralNode*>(node)->value;
        case NodeKind::FloatLiteral:
            return static_cast<int32_t>(static_cast<FloatLiteralNode*>(node)->value);
        case NodeKind::LVal: {
            auto* lv = static_cast<LValNode*>(node);
            Symbol* sym = sym_.lookup(lv->name);
            if (sym && sym->is_const && !sym->is_array) {
                if (auto* iv = std::get_if<int32_t>(&sym->const_val))
                    return *iv;
                if (auto* fv = std::get_if<float>(&sym->const_val))
                    return static_cast<int32_t>(*fv);
            }
            return 0;
        }
        case NodeKind::UnaryExpr: {
            auto* u = static_cast<UnaryExprNode*>(node);
            int32_t v = eval_const_int(u->operand.get());
            if (u->op == UnaryOp::Neg)
                return -v;
            if (u->op == UnaryOp::Not)
                return !v;
            return v;
        }
        case NodeKind::BinaryExpr: {
            auto* b = static_cast<BinaryExprNode*>(node);
            int32_t l = eval_const_int(b->lhs.get());
            int32_t r = eval_const_int(b->rhs.get());
            switch (b->op) {
                case BinOp::Add:
                    return l + r;
                case BinOp::Sub:
                    return l - r;
                case BinOp::Mul:
                    return l * r;
                case BinOp::Div:
                    return r ? l / r : 0;
                case BinOp::Mod:
                    return r ? l % r : 0;
                case BinOp::Lt:
                    return l < r;
                case BinOp::Gt:
                    return l > r;
                case BinOp::Le:
                    return l <= r;
                case BinOp::Ge:
                    return l >= r;
                case BinOp::Eq:
                    return l == r;
                case BinOp::Ne:
                    return l != r;
                case BinOp::And:
                    return l && r;
                case BinOp::Or:
                    return l || r;
            }
            return 0;
        }
        default:
            return 0;
    }
}

float IRGen::eval_const_float(ASTNode* node) {
    switch (node->kind) {
        case NodeKind::FloatLiteral:
            return static_cast<FloatLiteralNode*>(node)->value;
        case NodeKind::IntLiteral:
            return static_cast<float>(static_cast<IntLiteralNode*>(node)->value);
        case NodeKind::LVal: {
            auto* lv = static_cast<LValNode*>(node);
            Symbol* sym = sym_.lookup(lv->name);
            if (sym && sym->is_const && !sym->is_array) {
                if (auto* fv = std::get_if<float>(&sym->const_val))
                    return *fv;
                if (auto* iv = std::get_if<int32_t>(&sym->const_val))
                    return static_cast<float>(*iv);
            }
            return 0.0f;
        }
        case NodeKind::UnaryExpr: {
            auto* u = static_cast<UnaryExprNode*>(node);
            float v = eval_const_float(u->operand.get());
            if (u->op == UnaryOp::Neg)
                return -v;
            return v;
        }
        case NodeKind::BinaryExpr: {
            auto* b = static_cast<BinaryExprNode*>(node);
            float l = eval_const_float(b->lhs.get());
            float r = eval_const_float(b->rhs.get());
            switch (b->op) {
                case BinOp::Add:
                    return l + r;
                case BinOp::Sub:
                    return l - r;
                case BinOp::Mul:
                    return l * r;
                case BinOp::Div:
                    return r != 0.0f ? l / r : 0.0f;
                default:
                    return 0.0f;
            }
        }
        default:
            return 0.0f;
    }
}

// Array initialization flattening

void IRGen::flatten_init(const std::vector<int>& dims, ASTNode* init, std::vector<ASTNode*>& out) {
    if (!init)
        return;

    if (init->kind != NodeKind::InitList) {
        out.push_back(init);
        return;
    }

    auto* list = static_cast<InitListNode*>(init);
    if (dims.empty())
        return;

    int sub_size = 1;
    for (size_t i = 1; i < dims.size(); i++)
        sub_size *= dims[i];

    int total = dims[0] * sub_size;
    int pos = 0;

    for (auto& item : list->items) {
        if (pos >= total)
            break;

        if (item->kind == NodeKind::InitList) {
            if (dims.size() > 1) {
                std::vector<int> sub_dims(dims.begin() + 1, dims.end());
                int before = static_cast<int>(out.size());
                flatten_init(sub_dims, item.get(), out);
                int filled = static_cast<int>(out.size()) - before;
                while (filled < sub_size) {
                    out.push_back(nullptr);
                    filled++;
                }
                pos += sub_size;
            } else {
                auto* inner = static_cast<InitListNode*>(item.get());
                for (auto& elem : inner->items) {
                    if (pos < total) {
                        out.push_back(elem.get());
                        pos++;
                    }
                }
            }
        } else {
            out.push_back(item.get());
            pos++;
        }
    }

    while (pos < total) {
        out.push_back(nullptr);
        pos++;
    }
}

// Array address calculation

Ref IRGen::compute_array_addr(LValNode* node) {
    Symbol* sym = sym_.lookup(node->name);
    Ref base = get_var_addr_ref(sym);

    if (sym->is_param && sym->is_array)
        base = read_variable(sym->uid, cur_blk_, Kl);

    const auto& dims = sym->dims;
    size_t ndims = dims.size();

    Ref offset = Ref::none();
    bool offset_valid = false;

    for (size_t i = 0; i < node->indices.size(); i++) {
        Ref idx = visit_expr(node->indices[i].get());

        int stride = 1;
        size_t dim_idx = sym->is_param ? i : (i + 1);
        for (size_t j = dim_idx; j < ndims; j++)
            stride *= dims[j];

        // Extend index to 'l' type for pointer arithmetic
        int idx_cls = Kw;
        if (rtype(idx) == RTmp && idx.val >= Tmp0)
            idx_cls = cur_fn_->tmp[idx.val].cls;
        if (idx_cls == Kw)
            idx = emit_ins(Oextsw, Kl, idx, Ref::none());

        Ref byte_off = emit_ins(Omul, Kl, idx, make_int_ref(stride * 4));

        if (offset_valid) {
            offset = emit_ins(Oadd, Kl, offset, byte_off);
        } else {
            offset = byte_off;
            offset_valid = true;
        }
    }

    if (offset_valid)
        return emit_ins(Oadd, Kl, base, offset);
    return base;
}

}  // namespace rcc
