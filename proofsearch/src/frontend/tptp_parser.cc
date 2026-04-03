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

#include "atp/frontend/tptp_parser.h"

#include "atp/core/clause.h"
#include "atp/core/literal.h"
#include "atp/core/term_bank.h"
#include "atp/core/types.h"

#include <cctype>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace atp {

FormulaRole parseRole(std::string_view role_str) {
    if (role_str == "axiom")
        return FormulaRole::kAxiom;
    if (role_str == "hypothesis")
        return FormulaRole::kHypothesis;
    if (role_str == "definition")
        return FormulaRole::kDefinition;
    if (role_str == "assumption")
        return FormulaRole::kAssumption;
    if (role_str == "lemma")
        return FormulaRole::kLemma;
    if (role_str == "theorem")
        return FormulaRole::kTheorem;
    if (role_str == "conjecture")
        return FormulaRole::kConjecture;
    if (role_str == "negated_conjecture")
        return FormulaRole::kNegatedConjecture;
    if (role_str == "plain")
        return FormulaRole::kPlain;
    return FormulaRole::kPlain;
}

enum class TokenType { kWord, kLParen, kRParen, kOr, kNot, kDot, kComma, kEOF, kUnkown };

struct Token {
    TokenType type;
    std::string_view value;
    size_t line;
    size_t column;
};

class TptpLexer {
  public:
    explicit TptpLexer(std::string_view input) : source_(input) {}

    Token next_token() {
        skip_whitespace_and_comments();

        if (pos_ >= source_.length()) {
            return {.type = TokenType::kEOF, .value = "", .line = line_, .column = column_};
        }

        size_t start_pos = pos_;
        [[maybe_unused]] size_t start_col = column_;
        char c = source_[pos_];

        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            while (
                pos_ < source_.size() &&
                (std::isalnum(static_cast<unsigned char>(source_[pos_])) || source_[pos_] == '_')) {
                advance_char();
            }
            return {.type = TokenType::kWord,
                    .value = source_.substr(start_pos, pos_ - start_pos),
                    .line = line_,
                    .column = column_};
        }

        advance_char();
        switch (c) {
            case '(':
                return {.type = TokenType::kLParen, .value = "(", .line = line_, .column = column_};
            case ')':
                return {.type = TokenType::kRParen, .value = ")", .line = line_, .column = column_};
            case ',':
                return {.type = TokenType::kComma, .value = ",", .line = line_, .column = column_};
            case '.':
                return {.type = TokenType::kDot, .value = ".", .line = line_, .column = column_};
            case '~':
                return {.type = TokenType::kNot, .value = "~", .line = line_, .column = column_};
            case '|':
                return {.type = TokenType::kOr, .value = "|", .line = line_, .column = column_};
            default:
                return {.type = TokenType::kUnkown,
                        .value = source_.substr(start_pos, 1),
                        .line = line_,
                        .column = column_};
        }
    }

  private:
    std::string_view source_;
    size_t pos_ = 0;
    size_t line_ = 1;
    size_t column_ = 1;

    void advance_char() {
        if (pos_ < source_.size()) {
            if (source_[pos_] == '\n') {
                line_++;
                column_ = 1;
            } else {
                column_++;
            }
            pos_++;
        }
    }

    void skip_whitespace_and_comments() {
        while (pos_ < source_.size()) {
            char c = source_[pos_];
            if (std::isspace(static_cast<unsigned char>(c))) {
                advance_char();
            } else if (c == '%') {
                while (pos_ < source_.size() && source_[pos_] != '\n') {
                    advance_char();
                }
            } else {
                break;
            }
        }
    }
};

class TptpParser {
  public:
    explicit TptpParser(std::string_view input, TermBank& bank, SymbolTable& symbols)
        : lexer_(input), bank_(bank), symbols_(symbols) {
        advance();
    }

    void parseProblem(ParsedProblem& problem) {
        while (current_token_.type != TokenType::kEOF) {
            if (current_token_.type == TokenType::kWord && current_token_.value == "cnf") {
                advance();
                expect(TokenType::kLParen);

                AnnotatedFormula af;
                af.is_cnf = true;

                af.name = std::string(current_token_.value);
                expect(TokenType::kWord);
                expect(TokenType::kComma);

                af.role = parseRole(current_token_.value);
                expect(TokenType::kWord);
                expect(TokenType::kComma);

                Clause clause;
                clause.literals.push_back(parseLiteral());
                while (current_token_.type == TokenType::kOr) {
                    advance();
                    clause.literals.push_back(parseLiteral());
                }
                af.clauses.push_back(std::move(clause));

                expect(TokenType::kRParen);
                expect(TokenType::kDot);
                problem.formulas.push_back(std::move(af));

                variable_cache_.clear();
            } else if (current_token_.value == "fof") {
                throw std::runtime_error("FOF parsing not implemented. Line " +
                                         std::to_string(current_token_.line));
            } else {
                throw std::runtime_error("Unknown declaration: '" +
                                         std::string(current_token_.value) + "'");
            }
        }
    }

  private:
    TptpLexer lexer_;
    TermBank& bank_;
    SymbolTable& symbols_;
    Token current_token_;

    std::unordered_map<std::string_view, TermId> variable_cache_;

    void advance() { current_token_ = lexer_.next_token(); }

    void expect(TokenType type) {
        if (current_token_.type != type) {
            throw std::runtime_error("Parse error at line " + std::to_string(current_token_.line) +
                                     ", col " + std::to_string(current_token_.column) +
                                     ": unexpected token '" + std::string(current_token_.value) +
                                     "'");
        }
        advance();
    }

    TermId parseTerm() {
        if (current_token_.type != TokenType::kWord) {
            throw std::runtime_error("Expected variable or function name at line " +
                                     std::to_string(current_token_.line) + ", col " +
                                     std::to_string(current_token_.column) +
                                     ": unexpected token '" + std::string(current_token_.value) +
                                     "'");
        }

        std::string_view name = current_token_.value;
        bool is_variable = std::isupper(static_cast<unsigned char>(name[0]));
        advance();

        if (is_variable) {
            // Check cache first — same variable name in one clause → same TermId
            auto it = variable_cache_.find(name);
            if (it != variable_cache_.end()) {
                return it->second;
            }
            SymbolId sym = symbols_.intern(name, SymbolKind::kVariable);
            TermId var_id = bank_.makeVar(sym);
            variable_cache_[name] = var_id;
            return var_id;
        } else {
            // Check if this is a function/predicate (has args) or a constant (no parens)
            if (current_token_.type == TokenType::kLParen) {
                advance();  // consume '('
                std::vector<TermId> args;
                if (current_token_.type != TokenType::kRParen) {
                    args.push_back(parseTerm());
                    while (current_token_.type == TokenType::kComma) {
                        advance();
                        args.push_back(parseTerm());
                    }
                }
                expect(TokenType::kRParen);
                SymbolId sym = symbols_.intern(name, SymbolKind::kFunction,
                                               static_cast<uint16_t>(args.size()));
                return bank_.makeTerm(sym, args);
            } else {
                // Constant: no parentheses (e.g., a, b, h0, s0)
                SymbolId sym = symbols_.intern(name, SymbolKind::kConstant, 0);
                return bank_.makeTerm(sym, {});
            }
        }
    }

    Literal parseLiteral() {
        bool is_positive = true;
        if (current_token_.type == TokenType::kNot) {
            is_positive = false;
            advance();
        }
        TermId term = parseTerm();
        return Literal{.atom = term, .is_positive = is_positive};
    }
};

ParsedProblem parseTptpString(const std::string& input, TermBank& bank, SymbolTable& symbols) {
    ParsedProblem problem;
    TptpParser parser(input, bank, symbols);
    parser.parseProblem(problem);
    return problem;
}

ParsedProblem parseTptpFile(const std::string& filename, TermBank& bank, SymbolTable& symbols) {
    std::ifstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file: " + filename);
    }
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    ParsedProblem problem = parseTptpString(content, bank, symbols);
    problem.source_file = filename;
    return problem;
}

std::vector<Clause> prepareForProving(ParsedProblem& problem, TermBank& bank,
                                      SymbolTable& symbols) {
    std::vector<Clause> final_clauses;

    for (auto& af : problem.formulas) {
        if (af.is_cnf) {
            for (auto& clause : af.clauses) {
                clause.rule = InferenceRule::kInput;
                clause.parent1 = kInvalidId;
                clause.parent2 = kInvalidId;
                final_clauses.push_back(std::move(clause));
            }
        }
    }
    return final_clauses;
}

}  // namespace atp
