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
#include "atp/core/symbol_table.h"
#include "atp/core/term_bank.h"
#include "atp/core/types.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <ios>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace atp {

FormulaRole parseRole(std::string_view role_str) {
    if (role_str == "axiom") {
        return FormulaRole::kAxiom;
    }
    if (role_str == "hypothesis") {
        return FormulaRole::kHypothesis;
    }
    if (role_str == "definition") {
        return FormulaRole::kDefinition;
    }
    if (role_str == "assumption") {
        return FormulaRole::kAssumption;
    }
    if (role_str == "lemma") {
        return FormulaRole::kLemma;
    }
    if (role_str == "theorem") {
        return FormulaRole::kTheorem;
    }
    if (role_str == "conjecture") {
        return FormulaRole::kConjecture;
    }
    if (role_str == "negated_conjecture") {
        return FormulaRole::kNegatedConjecture;
    }
    if (role_str == "plain") {
        return FormulaRole::kPlain;
    }
    return FormulaRole::kPlain;
}

enum class TokenType : std::uint8_t { kWord, kLParen, kRParen, kOr, kNot, kDot, kComma, kEOF, kUnkown };

struct Token {
    TokenType type_{};
    std::string_view value_;
    size_t line_{};
    size_t column_{};
};

class TptpLexer {
  public:
    explicit TptpLexer(std::string_view input) : source_(input) {}

    Token nextToken() {
        skipWhitespaceAndComments();

        if (pos_ >= source_.length()) {
            return {.type_ = TokenType::kEOF, .value_ = "", .line_ = line_, .column_ = column_};
        }

        size_t const start_pos = pos_;
        [[maybe_unused]] size_t const start_col = column_;
        char const c = source_[pos_];

        if ((std::isalpha(static_cast<unsigned char>(c)) != 0) || c == '_') {
            while (
                pos_ < source_.size() &&
                ((std::isalnum(static_cast<unsigned char>(source_[pos_])) != 0) || source_[pos_] == '_')) {
                advanceChar();
            }
            return {.type_ = TokenType::kWord,
                    .value_ = source_.substr(start_pos, pos_ - start_pos),
                    .line_ = line_,
                    .column_ = column_};
        }

        advanceChar();
        switch (c) {
            case '(':
                return {.type_ = TokenType::kLParen, .value_ = "(", .line_ = line_, .column_ = column_};
            case ')':
                return {.type_ = TokenType::kRParen, .value_ = ")", .line_ = line_, .column_ = column_};
            case ',':
                return {.type_ = TokenType::kComma, .value_ = ",", .line_ = line_, .column_ = column_};
            case '.':
                return {.type_ = TokenType::kDot, .value_ = ".", .line_ = line_, .column_ = column_};
            case '~':
                return {.type_ = TokenType::kNot, .value_ = "~", .line_ = line_, .column_ = column_};
            case '|':
                return {.type_ = TokenType::kOr, .value_ = "|", .line_ = line_, .column_ = column_};
            default:
                return {.type_ = TokenType::kUnkown,
                        .value_ = source_.substr(start_pos, 1),
                        .line_ = line_,
                        .column_ = column_};
        }
    }

  private:
    std::string_view source_;
    size_t pos_ = 0;
    size_t line_ = 1;
    size_t column_ = 1;

    void advanceChar() {
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

    void skipWhitespaceAndComments() {
        while (pos_ < source_.size()) {
            char const c = source_[pos_];
            if (std::isspace(static_cast<unsigned char>(c)) != 0) {
                advanceChar();
            } else if (c == '%') {
                while (pos_ < source_.size() && source_[pos_] != '\n') {
                    advanceChar();
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
        while (current_token_.type_ != TokenType::kEOF) {
            if (current_token_.type_ == TokenType::kWord && current_token_.value_ == "cnf") {
                advance();
                expect(TokenType::kLParen);

                AnnotatedFormula af;
                af.is_cnf_ = true;

                af.name_ = std::string(current_token_.value_);
                expect(TokenType::kWord);
                expect(TokenType::kComma);

                af.role_ = parseRole(current_token_.value_);
                expect(TokenType::kWord);
                expect(TokenType::kComma);

                Clause clause;
                clause.literals_.push_back(parseLiteral());
                while (current_token_.type_ == TokenType::kOr) {
                    advance();
                    clause.literals_.push_back(parseLiteral());
                }
                af.clauses_.push_back(std::move(clause));

                expect(TokenType::kRParen);
                expect(TokenType::kDot);
                problem.formulas_.push_back(std::move(af));

                variable_cache_.clear();
            } else if (current_token_.value_ == "fof") {
                throw std::runtime_error("FOF parsing not implemented. Line " +
                                         std::to_string(current_token_.line_));
            } else {
                throw std::runtime_error("Unknown declaration: '" +
                                         std::string(current_token_.value_) + "'");
            }
        }
    }

  private:
    TptpLexer lexer_;
    TermBank& bank_;
    SymbolTable& symbols_;
    Token current_token_{};
    std::vector<Token> tokens_;
    std::unordered_map<std::string_view, SymbolId> variable_cache_;

    void advance() { current_token_ = lexer_.nextToken(); }

    void expect(TokenType type) {
        if (current_token_.type_ != type) {
            throw std::runtime_error("Parse error at line " + std::to_string(current_token_.line_) +
                                     ", col " + std::to_string(current_token_.column_) +
                                     ": unexpected token '" + std::string(current_token_.value_) +
                                     "'");
        }
        advance();
    }

    TermId parseTerm() {
        if (current_token_.type_ != TokenType::kWord) {
            throw std::runtime_error("Expected variable or function name at line " +
                                     std::to_string(current_token_.line_) + ", col " +
                                     std::to_string(current_token_.column_) +
                                     ": unexpected token '" + std::string(current_token_.value_) +
                                     "'");
        }

        std::string_view const name = current_token_.value_;
        bool const is_variable = std::isupper(static_cast<unsigned char>(name[0])) != 0;
        advance();

        if (is_variable) {
            // Check cache first — same variable name in one clause → same TermId
            auto it = variable_cache_.find(name);
            if (it != variable_cache_.end()) {
                return it->second;
            }
            SymbolId const sym = symbols_.intern(name, SymbolKind::kVariable);
            TermId const var_id = bank_.makeVar(sym);
            variable_cache_[name] = var_id;
            return var_id;
        }

        // Check if this is a function/predicate (has args) or a constant (no parens)
        if (current_token_.type_ == TokenType::kLParen) {
            advance();  // consume '('
            std::vector<TermId> args;
            if (current_token_.type_ != TokenType::kRParen) {
                args.push_back(parseTerm());
                while (current_token_.type_ == TokenType::kComma) {
                    advance();
                    args.push_back(parseTerm());
                }
            }
            expect(TokenType::kRParen);
            const SymbolId sym = symbols_.intern(name, SymbolKind::kFunction,
                                                 static_cast<uint16_t>(args.size()));
            return bank_.makeTerm(sym, args);
        }

        // Constant: no parentheses (e.g., a, b, h0, s0)
        const SymbolId sym = symbols_.intern(name, SymbolKind::kConstant, 0);
        return bank_.makeTerm(sym, {});
    }

    Literal parseLiteral() {
        bool is_positive = true;
        if (current_token_.type_ == TokenType::kNot) {
            is_positive = false;
            advance();
        }
        TermId const term = parseTerm();
        return Literal{.atom_ = term, .is_positive_ = is_positive};
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
    std::string const content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    ParsedProblem problem = parseTptpString(content, bank, symbols);
    problem.source_file_ = filename;
    return problem;
}

std::vector<Clause> prepareForProving(ParsedProblem& problem, TermBank&  /*bank*/,
                                      SymbolTable&  /*symbols*/) {
    std::vector<Clause> final_clauses;

    for (auto& af : problem.formulas_) {
        if (af.is_cnf_) {
            for (auto& clause : af.clauses_) {
                clause.rule_ = InferenceRule::kInput;
                clause.parent1_ = kInvalidId;
                clause.parent2_ = kInvalidId;
                final_clauses.push_back(std::move(clause));
            }
        }
    }
    return final_clauses;
}

}  // namespace atp
