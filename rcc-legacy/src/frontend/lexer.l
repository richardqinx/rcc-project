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

%{
/* lexer.l - SysY2022 Lexer */
#include "rcc/ast.h"
#include "parser.tab.h"

#include <cstdlib>
#include <cstring>

// Line number tracking
int yylineno_val = 1;
#define YY_USER_ACTION  yylloc.first_line = yylineno_val;

using namespace rcc;
%}

%option noyywrap nounput noinput

/* Exclusive start condition: multi-line comments */
%x COMMENT

%%

 /* ─── Whitespace and Newlines ─── */
[ \t\r]+        { /* skip whitespace */ }
\n              { yylineno_val++; }

 /* ─── Comments ─── */
"//"[^\n]*      { /* single-line comment */ }
"/*"            { BEGIN(COMMENT); }
<COMMENT>"*/"   { BEGIN(INITIAL); }
<COMMENT>\n     { yylineno_val++; }
<COMMENT>.      { /* eat anything inside comment */ }

 /* ─── Keywords ─── */
"int"           { return T_INT; }
"float"         { return T_FLOAT; }
"void"          { return T_VOID; }
"const"         { return T_CONST; }
"if"            { return T_IF; }
"else"          { return T_ELSE; }
"while"         { return T_WHILE; }
"break"         { return T_BREAK; }
"continue"      { return T_CONTINUE; }
"return"        { return T_RETURN; }

 /* ─── Integer Constants (Hex > Octal > Decimal, order is important) ─── */
0[xX][0-9a-fA-F]+       { yylval.int_val = static_cast<int32_t>(strtoul(yytext, nullptr, 16)); return INT_CONST; }
0[0-7]+                  { yylval.int_val = static_cast<int32_t>(strtoul(yytext, nullptr, 8));  return INT_CONST; }
[0-9]+                   { yylval.int_val = static_cast<int32_t>(strtoul(yytext, nullptr, 10)); return INT_CONST; }

 /* ─── Floating Point Constants (Hex float + Decimal float) ─── */
0[xX][0-9a-fA-F]*\.[0-9a-fA-F]+([pP][+-]?[0-9]+)?  { yylval.float_val = strtof(yytext, nullptr); return FLOAT_CONST; }
0[xX][0-9a-fA-F]+\.[0-9a-fA-F]*([pP][+-]?[0-9]+)?  { yylval.float_val = strtof(yytext, nullptr); return FLOAT_CONST; }
0[xX][0-9a-fA-F]+[pP][+-]?[0-9]+                     { yylval.float_val = strtof(yytext, nullptr); return FLOAT_CONST; }
[0-9]*\.[0-9]+([eE][+-]?[0-9]+)?    { yylval.float_val = strtof(yytext, nullptr); return FLOAT_CONST; }
[0-9]+\.[0-9]*([eE][+-]?[0-9]+)?    { yylval.float_val = strtof(yytext, nullptr); return FLOAT_CONST; }
[0-9]+[eE][+-]?[0-9]+               { yylval.float_val = strtof(yytext, nullptr); return FLOAT_CONST; }

 /* ─── Identifiers ─── */
[a-zA-Z_][a-zA-Z0-9_]*  { yylval.str_val = strdup(yytext); return IDENT; }

 /* ─── Multi-character Operators ─── */
"&&"    { return T_AND; }
"||"    { return T_OR; }
"<="    { return T_LE; }
">="    { return T_GE; }
"=="    { return T_EQ; }
"!="    { return T_NE; }

 /* ─── Format Strings (for putf only) ─── */
\"[^\"]*\"  { yylval.str_val = strdup(yytext); return STRING_CONST; }

 /* ─── Single-character Operators and Delimiters, return ASCII directly ─── */
.       { return yytext[0]; }

%%
