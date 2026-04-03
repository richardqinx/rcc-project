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

#pragma once

/// @file types.h
/// @brief Core type aliases for the ATP engine.

#include <cstdint>

namespace atp {

/// Unique identifier for an interned symbol (predicate/function/constant name).
using SymbolId = uint32_t;

/// Unique identifier for a term in the Term Bank (hash-consed).
using TermId = uint32_t;

/// Unique identifier for a clause in the Clause Store.
using ClauseId = uint32_t;

/// Unique identifier for a sort/type (for sorted logic).
using SortId = uint16_t;

/// Sentinel value indicating an invalid / unassigned ID.
inline constexpr uint32_t kInvalidId = UINT32_MAX;

/// Default sort (unsorted logic).
inline constexpr SortId kUnsorted = 0;

/// Symbol kind — distinguishes variables from function/predicate symbols.
enum class SymbolKind : uint8_t {
    kVariable,   ///< Universally quantified variable
    kConstant,   ///< 0-arity function symbol (constant)
    kFunction,   ///< Function symbol (arity >= 1)
    kPredicate,  ///< Predicate symbol (returns bool)
    kSkolem,     ///< Skolem function/constant (introduced by Skolemization)
};

/// Inference rule that produced a clause.
/// Deliberately uses uint16_t for extensibility toward algebra/analysis rules.
enum class InferenceRule : uint16_t {
    // ── Core resolution rules ──
    kInput = 0,       ///< Original input clause (axiom / negated conjecture)
    kResolution = 1,  ///< Binary resolution
    kFactoring = 2,   ///< Factoring

    // ── Equality rules (algebra) ──
    kParamodulation = 100,      ///< Paramodulation (equality resolution)
    kDemodulation = 101,        ///< Demodulation (simplification by rewriting)
    kEqualityResolution = 102,  ///< s != t resolved with unifier
    kEqualityFactoring = 103,   ///< Factoring with equality

    // ── Simplification rules ──
    kSubsumption = 200,      ///< Removed by subsumption
    kTautologyElim = 201,    ///< Removed as tautology
    kPureLiteralElim = 202,  ///< Pure literal elimination

    // ── Theory rules (analysis, arithmetic) ──
    kTheoryResolution = 300,  ///< Theory-specific resolution
    kTheoryRewrite = 301,     ///< Theory-driven rewriting
};

}  // namespace atp
