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

/// @file ast.cc
/// @brief AST debug printing and built-in function registration.

#include "rcc/ast.h"

#include "rcc/symtab.h"

#include <cstdio>

namespace rcc {

// AST debug printing

static void indent(int n) {
    for (int i = 0; i < n; i++)
        fprintf(stderr, "  ");
}

void ast_dump(const ASTNode* node, int depth) {
    if (!node) {
        indent(depth);
        fprintf(stderr, "(null)\n");
        return;
    }

    indent(depth);
    switch (node->kind) {
        case NodeKind::CompUnit:
            fprintf(stderr, "CompUnit\n");
            for (auto& c : static_cast<const CompUnitNode*>(node)->decls_and_funcs)
                ast_dump(c.get(), depth + 1);
            break;

        case NodeKind::FuncDef: {
            auto* f = static_cast<const FuncDefNode*>(node);
            fprintf(stderr, "FuncDef '%s' -> %s\n", f->name.c_str(), btype_str(f->ret_type));
            for (auto& p : f->params)
                ast_dump(p.get(), depth + 1);
            ast_dump(f->body.get(), depth + 1);
            break;
        }

        case NodeKind::FuncFParam: {
            auto* p = static_cast<const FuncFParamNode*>(node);
            fprintf(stderr, "Param '%s' : %s%s\n", p->name.c_str(), btype_str(p->btype),
                    p->is_array ? "[]" : "");
            break;
        }

        case NodeKind::VarDecl: {
            auto* d = static_cast<const VarDeclNode*>(node);
            fprintf(stderr, "%s %s\n", d->is_const ? "ConstDecl" : "VarDecl", btype_str(d->btype));
            for (auto& def : d->defs)
                ast_dump(def.get(), depth + 1);
            break;
        }

        case NodeKind::VarDef: {
            auto* d = static_cast<const VarDefNode*>(node);
            fprintf(stderr, "VarDef '%s'", d->name.c_str());
            if (!d->dims.empty())
                fprintf(stderr, " (array, %zu dims)", d->dims.size());
            fprintf(stderr, "\n");
            if (d->init_val)
                ast_dump(d->init_val.get(), depth + 1);
            break;
        }

        case NodeKind::InitList:
            fprintf(stderr, "InitList\n");
            for (auto& i : static_cast<const InitListNode*>(node)->items)
                ast_dump(i.get(), depth + 1);
            break;

        case NodeKind::Block:
            fprintf(stderr, "Block\n");
            for (auto& i : static_cast<const BlockNode*>(node)->items)
                ast_dump(i.get(), depth + 1);
            break;

        case NodeKind::AssignStmt:
            fprintf(stderr, "Assign\n");
            ast_dump(static_cast<const AssignStmtNode*>(node)->lval.get(), depth + 1);
            ast_dump(static_cast<const AssignStmtNode*>(node)->expr.get(), depth + 1);
            break;

        case NodeKind::ExprStmt:
            fprintf(stderr, "ExprStmt\n");
            if (static_cast<const ExprStmtNode*>(node)->expr)
                ast_dump(static_cast<const ExprStmtNode*>(node)->expr.get(), depth + 1);
            break;

        case NodeKind::IfStmt: {
            auto* s = static_cast<const IfStmtNode*>(node);
            fprintf(stderr, "If\n");
            ast_dump(s->cond.get(), depth + 1);
            ast_dump(s->then_stmt.get(), depth + 1);
            if (s->else_stmt)
                ast_dump(s->else_stmt.get(), depth + 1);
            break;
        }

        case NodeKind::WhileStmt:
            fprintf(stderr, "While\n");
            ast_dump(static_cast<const WhileStmtNode*>(node)->cond.get(), depth + 1);
            ast_dump(static_cast<const WhileStmtNode*>(node)->body.get(), depth + 1);
            break;

        case NodeKind::BreakStmt:
            fprintf(stderr, "Break\n");
            break;
        case NodeKind::ContinueStmt:
            fprintf(stderr, "Continue\n");
            break;

        case NodeKind::ReturnStmt:
            fprintf(stderr, "Return\n");
            if (static_cast<const ReturnStmtNode*>(node)->expr)
                ast_dump(static_cast<const ReturnStmtNode*>(node)->expr.get(), depth + 1);
            break;

        case NodeKind::IntLiteral:
            fprintf(stderr, "IntLit %d\n", static_cast<const IntLiteralNode*>(node)->value);
            break;

        case NodeKind::FloatLiteral:
            fprintf(stderr, "FloatLit %f\n",
                    static_cast<double>(static_cast<const FloatLiteralNode*>(node)->value));
            break;

        case NodeKind::LVal: {
            auto* l = static_cast<const LValNode*>(node);
            fprintf(stderr, "LVal '%s'\n", l->name.c_str());
            for (auto& idx : l->indices)
                ast_dump(idx.get(), depth + 1);
            break;
        }

        case NodeKind::FuncCall: {
            auto* c = static_cast<const FuncCallNode*>(node);
            fprintf(stderr, "Call '%s'\n", c->name.c_str());
            for (auto& a : c->args)
                ast_dump(a.get(), depth + 1);
            break;
        }

        case NodeKind::BinaryExpr: {
            auto* e = static_cast<const BinaryExprNode*>(node);
            const char* ops[] = {"Add", "Sub", "Mul", "Div", "Mod", "Lt", "Gt",
                                 "Le",  "Ge",  "Eq",  "Ne",  "And", "Or"};
            fprintf(stderr, "BinExpr %s\n", ops[static_cast<int>(e->op)]);
            ast_dump(e->lhs.get(), depth + 1);
            ast_dump(e->rhs.get(), depth + 1);
            break;
        }

        case NodeKind::UnaryExpr: {
            auto* e = static_cast<const UnaryExprNode*>(node);
            const char* ops[] = {"Pos", "Neg", "Not"};
            fprintf(stderr, "UnaryExpr %s\n", ops[static_cast<int>(e->op)]);
            ast_dump(e->operand.get(), depth + 1);
            break;
        }

        case NodeKind::ImplicitCast: {
            auto* c = static_cast<const ImplicitCastNode*>(node);
            fprintf(stderr, "ImplicitCast -> %s\n", btype_str(c->target_type));
            ast_dump(c->operand.get(), depth + 1);
            break;
        }
    }
}

// built-in functions

void SymbolTable::register_builtins() {
    // I/O functions
    register_func({"getint", BType::Int, {}});
    register_func({"getch", BType::Int, {}});
    register_func({"getfloat", BType::Float, {}});
    register_func({"getarray", BType::Int, {{BType::Int, true}}});
    register_func({"getfarray", BType::Int, {{BType::Float, true}}});
    register_func({"putint", BType::Void, {{BType::Int, false}}});
    register_func({"putch", BType::Void, {{BType::Int, false}}});
    register_func({"putfloat", BType::Void, {{BType::Float, false}}});
    register_func({"putarray", BType::Void, {{BType::Int, false}, {BType::Int, true}}});
    register_func({"putfarray", BType::Void, {{BType::Int, false}, {BType::Float, true}}});
    register_func({"putf", BType::Void, {}});  // variable parameters

    // timing functions (actually call _sysy_starttime / _sysy_stoptime)
    register_func({"starttime", BType::Void, {}});
    register_func({"stoptime", BType::Void, {}});
}

}  // namespace rcc
