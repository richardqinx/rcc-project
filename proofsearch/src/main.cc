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

/// @file main.cc
/// @brief ATP Engine command-line entry point.

#include "atp/core/clause_store.h"
#include "atp/core/symbol_table.h"
#include "atp/core/term_bank.h"
#include "atp/frontend/tptp_parser.h"
#include "atp/proof/proof_trace.h"
#include "atp/search/prover.h"

#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    std::cerr << "richard-atp-engine  Copyright (C) 2026  Richard Qin\n"
              << "This program comes with ABSOLUTELY NO WARRANTY.\n"
              << "This is free software, and you are welcome to redistribute it\n"
              << "under certain conditions; see LICENSE for details.\n\n";

    if (argc < 2) {
        std::cerr << "Usage: atp <file.p>\n";
        return EXIT_FAILURE;
    }

    const std::string filepath = argv[1];

    try {
        // 1. Initialize core modules
        atp::SymbolTable symbols;
        atp::TermBank bank(symbols);
        atp::ClauseStore store;

        // 2. Parse TPTP input
        auto problem = atp::parseTptpFile(filepath, bank, symbols);

        std::cerr << "% Parsed " << problem.formulas.size() << " formulas from " << filepath
                  << "\n";

        // 3. Prepare clauses for proving
        auto clauses = atp::prepareForProving(problem, bank, symbols);

        std::cerr << "% " << clauses.size() << " clauses prepared for proving\n";

        // 4. Run the prover
        atp::Prover prover(bank, store);
        prover.addClauses(std::move(clauses));
        atp::ProverResult result = prover.prove();

        // 5. Output result (SZS format)
        switch (result) {
            case atp::ProverResult::kTheorem: {
                std::cout << "% SZS status Theorem for " << filepath << "\n\n";

                // Extract and print proof
                auto eid = prover.getEmptyClauseId();
                if (eid.has_value()) {
                    auto proof = atp::extractProof(store, *eid);
                    std::cout << "% SZS output start Proof for " << filepath << "\n";
                    std::cout << atp::formatProof(proof, store, bank);
                    std::cout << "% SZS output end Proof for " << filepath << "\n";
                }
                break;
            }
            case atp::ProverResult::kSaturation:
                std::cout << "% SZS status Satisfiable for " << filepath << "\n";
                break;
            case atp::ProverResult::kTimeout:
                std::cout << "% SZS status Timeout for " << filepath << "\n";
                break;
        }

    } catch (const std::exception& e) {
        std::cerr << "% Error: " << e.what() << "\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
