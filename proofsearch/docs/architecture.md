# ATP Engine — Architecture Design Document

> **Version**: 1.0  
> **Target**: Extensible first-order logic theorem prover (saturation-based)  
> **Language**: C++20  
> **Build**: CMake 3.20+, GoogleTest, ANTLR4 (C++ target)

---

## Table of Contents

1. [Design Philosophy](#1-design-philosophy)
2. [System Architecture](#2-system-architecture)
3. [Directory Structure](#3-directory-structure)
4. [Module Specifications](#4-module-specifications)
5. [Data Flow](#5-data-flow)
6. [Known Limitations & Future Work](#6-known-limitations--future-work)
7. [References](#7-references)

---

## 1. Design Philosophy

### 1.1 Core Principles

| Principle | Rationale |
|-----------|-----------|
| **Bottom-up construction** | Build from the memory layer upward. A stable Term Bank is the foundation everything else rests on. |
| **Strict layered dependencies** | Dependencies flow in one direction only: `core → infer → index → simplify → search → proof`. No cycles. |
| **Interface-implementation separation** | Headers in `include/atp/` define interfaces; `src/` contains implementations. Modules communicate through stable APIs. |
| **Premature abstraction avoidance** | V1 hardcodes resolution + factoring. Abstract strategy selection (`strategy/`) is scaffolded but not wired in until V2. |
| **Parse once, reason forever** | Text parsing (ANTLR4) is a one-shot frontend concern. The core engine operates entirely on integer IDs, never on strings. |

### 1.2 Design Influences

This architecture draws from three production-grade theorem provers:

- **E Prover** (Stephan Schulz) — Given Clause Loop, term sharing via hash consing, age-weight clause selection.
- **Vampire** (Andrei Voronkov et al.) — AVATAR splitting, superposition calculus, discrimination tree indexing.
- **Otter** (William McCune) — Original set-of-support strategy, subsumption, demodulation.

### 1.3 Versioning Strategy

```
V1  →  Pure first-order resolution (no equality). Solve the Monkey-Banana problem.
V2  →  Pluggable inference rules (Calculus), age-weight scheduling, variable standardization.
V3  →  Equality reasoning (paramodulation/superposition), term orderings (KBO/LPO).
V4  →  AVATAR splitting, SAT solver integration, TPTP competition readiness.
```

---

## 2. System Architecture

### 2.1 Layered Architecture Overview

```
┌──────────────────────────────────────────────────────────────────────┐
│                         Frontend Layer                               │
│  TPTP File → ANTLR4 Lexer/Parser → AST Builder → tptp_parser.h     │
└────────────────────────────────┬─────────────────────────────────────┘
                                 │ Formula (internal AST)
┌────────────────────────────────▼─────────────────────────────────────┐
│                       Normalization Layer                             │
│  formula.h → clausifier.h (NNF → Skolem → CNF)                     │
└────────────────────────────────┬─────────────────────────────────────┘
                                 │ Clause set
┌────────────────────────────────▼─────────────────────────────────────┐
│                        Search Layer (Control)                        │
│                                                                      │
│   ┌──────────────┐  select   ┌─────────────────┐  resolve with all  │
│   │ Unprocessed  │─────────→ │  Given Clause   │─────────────────→  │
│   │ (pri queue)  │           │  (selected)     │                    │
│   └──────▲───────┘           └─────────────────┘                    │
│          │ enqueue survivors                                         │
│   ┌──────┴───────┐           ┌─────────────────┐                    │
│   │  Simplify &  │←──────────│  New Clauses    │                    │
│   │  Filter      │           │  (resolvents)   │                    │
│   └──────────────┘           └─────────────────┘                    │
│          │ if empty clause found                                     │
└──────────┼───────────────────────────────────────────────────────────┘
           ▼
┌──────────────────────────────────────────────────────────────────────┐
│                         Output Layer                                 │
│  proof_trace.h → DFS backtrack → format & print action sequence     │
└──────────────────────────────────────────────────────────────────────┘

═══════════════════════ Shared Foundation ══════════════════════════════
┌──────────────────────────────────────────────────────────────────────┐
│                    Core Layer (Data Plane)                            │
│  Symbol Table │ Term Bank (hash consing) │ Clause Store │ Types     │
└──────────────────────────────────────────────────────────────────────┘
```

### 2.2 Dependency Graph

```
frontend ──→ normalize ──→ core
                             ↑
search ──→ simplify ──→ index ──→ infer ──→ core
                                             ↑
                                         rewrite ─┘
  proof ──→ core/clause_store

  strategy/ ──→ (V2: wired into search)
```

All arrows point downward/inward. `proof` depends only on `clause_store`, not on `search` (no reverse dependency).

---

## 3. Directory Structure

```
richard-atp-engine/
├── CMakeLists.txt                # Top-level build (C++20, clang-tidy, clang-format, sanitizers)
├── .clang-format                 # Google-based, 4-space indent, 100-col
├── .clang-tidy                   # Strict checks, filtered for generated code
├── .gitignore
│
├── grammar/
│   └── TPTP.g4                   # ANTLR4 grammar for TPTP fof/cnf syntax
│
├── docs/
│   └── architecture.md           # This document
│
├── data/
│   └── monkey_banana.p           # First integration test (TPTP format)
│
├── include/atp/
│   ├── core/                     # Data plane — memory foundation
│   │   ├── types.h               #   TermId, ClauseId, SymbolId, SortId, SymbolKind, InferenceRule
│   │   ├── symbol_table.h        #   String interning with kind/sort metadata
│   │   ├── term.h                #   Flat term struct (symbol_id + args)
│   │   ├── literal.h             #   Polarity + atom TermId
│   │   ├── clause.h              #   Literal vector + provenance (parents, rule, depth)
│   │   ├── clause_store.h        #   Central clause ownership, ClauseId allocation
│   │   └── term_bank.h           #   Hash-consing pool for structural sharing
│   │
│   ├── normalize/                # Normalization — FOL formula to CNF
│   │   ├── formula.h             #   FOL AST nodes (And, Or, Not, Forall, Exists, ...)
│   │   └── clausifier.h          #   Eliminate →/↔ → NNF → Skolemize → distribute → flatten
│   │
│   ├── infer/                    # Compute plane — stateless inference operators
│   │   ├── substitution.h        #   Variable binding map (stack-lifetime)
│   │   ├── unification.h         #   MGU with optional occurs check
│   │   └── resolution.h          #   Binary resolution + factoring
│   │
│   ├── index/                    # Index plane — fast term retrieval
│   │   └── discrim_tree.h        #   Discrimination tree (unifiable / instance / generalization queries)
│   │
│   ├── simplify/                 # Redundancy elimination
│   │   ├── tautology.h           #   Complementary literal detection
│   │   └── subsumption.h         #   Forward/backward subsumption (backed by discrim tree)
│   │
│   ├── search/                   # Control plane — proof search scheduling
│   │   ├── clause_queue.h        #   Comparators (BFS, weight-based)
│   │   └── prover.h              #   Given Clause Loop (V1: hardcoded rules; V2: Calculus-driven)
│   │
│   ├── proof/                    # Output — proof reconstruction
│   │   └── proof_trace.h         #   DFS backtrack from empty clause, formatting
│   │
│   ├── rewrite/                  # Equality reasoning (V3 extension point)
│   │   ├── ordering.h            #   Abstract TermOrdering (KBO, LPO)
│   │   └── paramodulation.h      #   Paramodulation + demodulation
│   │
│   ├── strategy/                 # Dynamic algorithm selection (V2 extension point)
│   │   ├── inference_rule.h      #   GeneratingRule / SimplifyingRule interfaces
│   │   ├── calculus.h            #   Bundles rules + heuristics + ordering
│   │   └── problem_analyzer.h   #   Feature detection + calculus selection
│   │
│   └── utils/
│       ├── hash_utils.h          #   Hash combination (boost::hash_combine equivalent)
│       └── logger.h              #   Compile-time-filtered logging macros
│
├── src/                          # Implementation files (mirror include/ structure)
│   ├── core/
│   ├── normalize/
│   ├── infer/
│   ├── index/
│   ├── simplify/
│   ├── search/
│   ├── proof/
│   ├── rewrite/
│   ├── strategy/
│   ├── frontend/
│   └── main.cpp
│
└── tests/                        # GoogleTest unit tests
    ├── CMakeLists.txt
    ├── test_term_bank.cc
    ├── test_unification.cc
    ├── test_clausifier.cc
    ├── test_resolution.cc
    ├── test_discrim_tree.cc
    └── test_prover.cc           # End-to-end: Monkey-Banana problem
```

---

## 4. Module Specifications

### 4.1 Core Layer (`core/`)

The core layer owns all persistent data. Every other module receives `const&` or `&` references to core objects — never raw pointers, never ownership.

#### `types.h` — Type System Foundation

```cpp
using SymbolId = uint32_t;   // Index into SymbolTable
using TermId   = uint32_t;   // Index into TermBank (hash-consed)
using ClauseId = uint32_t;   // Index into ClauseStore
using SortId   = uint16_t;   // For sorted logic (V3+), default = kUnsorted

enum class SymbolKind : uint8_t {
    kVariable, kConstant, kFunction, kPredicate, kSkolem
};

enum class InferenceRule : uint16_t {
    kInput = 0, kResolution = 1, kFactoring = 2,       // Core
    kParamodulation = 100, kDemodulation = 101, ...     // Equality (V3)
    kSubsumption = 200, kTautologyElim = 201, ...       // Simplification
    kTheoryResolution = 300, ...                         // Theory (V4)
};
```

**Design decisions**:
- `uint32_t` IDs instead of pointers — enables flat array storage, trivial comparison, and cache-friendly iteration.
- `InferenceRule` uses `uint16_t` with reserved ranges (0-99 core, 100-199 equality, 200-299 simplification, 300+ theory) to avoid enum churn as the system grows.
- `SymbolKind` enables distinguishing variables from function symbols at the symbol level, critical for unification and sorted logic.
- `SortId` is present but unused in V1 (`kUnsorted`), ready for sorted unification in V3.

#### `symbol_table.h` — String Interning

```cpp
struct SymbolInfo {
    std::string name;
    SymbolKind kind;
    uint16_t arity;
    SortId sort;
};

class SymbolTable {
    SymbolId intern(string_view name, SymbolKind kind, uint16_t arity, SortId sort);
    string_view getName(SymbolId id) const;
    const SymbolInfo& getInfo(SymbolId id) const;
    bool isVariable(SymbolId id) const;
};
```

Each unique symbol string is stored exactly once. All downstream modules work with `SymbolId` integers. The `SymbolInfo` metadata (kind, arity, sort) enables type-checking during unification without string inspection.

#### `term_bank.h` — Hash Consing

```cpp
class TermBank {
    TermId makeTerm(SymbolId symbol, span<const TermId> args);
    TermId makeVar(SymbolId var_symbol);
    const Term& getTerm(TermId id) const;
    bool isVariable(TermId id) const;
};
```

**Hash consing guarantee**: If `makeTerm(f, {a, b})` is called twice with the same `f`, `a`, `b`, it returns the same `TermId`. This makes structural equality a single integer comparison (`O(1)`), which is the foundation for efficient indexing and subsumption.

**Memory layout**: Terms are stored in a flat arena. Each `Term` header (`symbol_id` + `arity`) is followed by its argument `TermId` array in contiguous memory. No heap allocation per term.

#### `clause_store.h` — Clause Ownership

```cpp
class ClauseStore {
    ClauseId addClause(Clause clause);
    const Clause& getClause(ClauseId id) const;
    Clause& getMutableClause(ClauseId id);
    void forEach(const function<void(const Clause&)>& visitor) const;
};
```

Separated from `Prover` so that proof extraction, simplification, and search all access clauses through the same stable interface. Changing the search strategy (OTTER loop → DISCOUNT loop) does not affect clause storage.

---

### 4.2 Normalization Layer (`normalize/`)

#### `formula.h` — First-Order Logic AST

```cpp
enum class FormulaKind : uint8_t {
    kAtom, kNot, kAnd, kOr, kImplies, kIff, kForall, kExists
};

struct Formula {
    FormulaKind kind;
    TermId atom;                              // for kAtom
    string var_name;                          // for quantifiers
    vector<unique_ptr<Formula>> children;     // sub-formulas
};
```

This is the **pre-clausification** representation. The parser builds `Formula` trees; the clausifier destroys them and produces `Clause` vectors. Formula nodes are short-lived — they exist only during the normalization phase.

#### `clausifier.h` — CNF Conversion Pipeline

```
Input Formula
    │
    ▼ eliminateImplication()     — P → Q  becomes  ¬P ∨ Q
    ▼ eliminateBiconditional()   — P ↔ Q  becomes  (¬P ∨ Q) ∧ (P ∨ ¬Q)
    ▼ pushNegation()             — ¬(P ∧ Q) becomes ¬P ∨ ¬Q       (NNF)
    ▼ skolemize()                — ∀x∃y.P(x,y) becomes ∀x.P(x, sk(x))
    ▼ dropUniversal()            — All remaining vars are implicitly ∀
    ▼ distribute()               — (P ∨ (Q ∧ R)) becomes (P ∨ Q) ∧ (P ∨ R)
    ▼ flatten()                  — Nested ∧ at top level → separate clauses
    │
    ▼
Output: vector<Clause>
```

**Critical**: `skolemize()` must correctly handle nested quantifier scopes. Given `∀x ∃y ∀z ∃w. P(x,y,z,w)`, the Skolem functions are `y = sk1(x)` and `w = sk2(x, z)` — each existential variable becomes a function of all universally quantified variables in its scope.

---

### 4.3 Inference Layer (`infer/`)

#### `substitution.h` — Variable Bindings

A substitution `σ = {X → f(a), Y → Z}` maps variables to terms. Key properties:

- **Stack lifetime**: Created during unification, applied to build the resolvent, then discarded. Never stored long-term.
- **Chain resolution**: `resolve(X)` walks the binding chain (`X → Y → f(a)`) to the final value.
- **Idempotence**: After applying a substitution, no variables in the result have bindings in the same substitution.

#### `unification.h` — Most General Unifier

```cpp
bool unify(const TermBank& bank, TermId t1, TermId t2, Substitution& subst,
           const UnificationConfig& config);
```

The Robinson unification algorithm, operating on `TermId` values via the `TermBank`. Returns the Most General Unifier (MGU) if one exists.

**Occurs check**: Configurable via `UnificationConfig::enable_occurs_check`. Disabled for situation calculus planning (performance); enabled for general mathematical reasoning (soundness — prevents infinite terms like `X = f(X)`).

#### `resolution.h` — Inference Rules

```cpp
optional<Clause> resolve(bank, clause1, lit_idx1, clause2, lit_idx2);
vector<Clause>   allResolvents(bank, clause1, clause2);
vector<Clause>   factor(bank, clause);
```

**Binary resolution**: Given clauses `{P(x), Q(x)}` and `{¬P(a), R(a)}`, if `P(x)` and `P(a)` unify under `σ = {x→a}`, produce resolvent `{Q(a), R(a)}`.

**Factoring**: Given clause `{P(x), P(a), Q(x)}`, if `P(x)` and `P(a)` unify, produce `{P(a), Q(a)}`. Factoring is necessary for completeness of binary resolution.

---

### 4.4 Index Layer (`index/`)

#### `discrim_tree.h` — Discrimination Tree

A trie-based index over the flattened structure of terms. Supports three query types:

| Query | Use Case | Direction |
|-------|----------|-----------|
| `queryUnifiable(t)` | Find resolution partners | Bidirectional matching |
| `queryInstances(t)` | Backward subsumption | Pattern more general than indexed |
| `queryGeneralizations(t)` | Forward subsumption | Indexed more general than query |

The tree is built by linearizing terms in pre-order: `f(g(a), b)` becomes the key sequence `[f, g, a, *, b]` where `*` represents a variable (wildcard). Lookups traverse the trie, branching at decision points.

---

### 4.5 Simplification Layer (`simplify/`)

#### `tautology.h`

Detects clauses containing both `P` and `¬P` for the same atom. These are logically true and can be safely discarded without affecting completeness.

#### `subsumption.h`

Backed by the discrimination tree. Two operations:

- **Forward subsumption**: Before enqueuing a new clause `C`, check if any existing clause `D` subsumes it (`D ⊆ C` — every literal of `D` matches a literal of `C`). If yes, discard `C`.
- **Backward subsumption**: After adding `C`, check if `C` subsumes any existing clause `D`. If yes, remove `D`.

Subsumption is the single most important simplification rule for keeping the clause set manageable.

---

### 4.6 Search Layer (`search/`)

#### `prover.h` — The Given Clause Loop

The main proof search loop, following the OTTER/E paradigm:

```
procedure GivenClauseLoop(initial_clauses):
    Unprocessed ← priority_queue(initial_clauses)
    Processed   ← empty_set

    while Unprocessed is not empty:
        given ← Unprocessed.pop()          // clause selection

        if given is tautology: continue
        if given is forward-subsumed: continue

        // Backward simplification
        remove from Processed all clauses subsumed by given

        // Inference
        for each clause C in Processed:
            for each resolvent R of (given, C):
                if R is empty clause: RETURN PROOF FOUND
                if R is not tautology and not forward-subsumed:
                    Unprocessed.push(R)

        // Generate factors of given
        for each factor F of given:
            if F is empty clause: RETURN PROOF FOUND
            Unprocessed.push(F)

        Processed.add(given)

    RETURN SATURATED (no proof exists)
```

**V1 configuration**:
```cpp
struct ProverConfig {
    ClauseComparator comparator = bfsCompare;  // BFS for shortest proof
    size_t max_clauses = 100000;
    size_t max_iterations = 500000;
    bool enable_occurs_check = false;
};
```

---

### 4.7 Proof Layer (`proof/`)

#### `proof_trace.h`

When the empty clause `□` is derived, the prover halts. Proof extraction walks the `parent1`/`parent2` pointers stored in each `Clause` via DFS, reconstructing the derivation tree from `□` back to input axioms.

The output is a sequence of `ProofStep` records, each containing the inference rule used and the parent clause IDs. This can be formatted as a human-readable proof or (future) TSTP-compliant output.

---

### 4.8 Extension Point: Rewriting (`rewrite/`)

Scaffolded for V3. Not compiled in V1.

#### `ordering.h` — Abstract Term Ordering

```cpp
class TermOrdering {
    virtual OrderResult compare(const TermBank& bank, TermId t1, TermId t2) const = 0;
};
```

Concrete implementations (future): Knuth-Bendix Ordering (KBO), Lexicographic Path Ordering (LPO). These are required for ordered resolution and superposition calculus — they ensure that rewriting always terminates.

#### `paramodulation.h` — Equality Reasoning

Paramodulation replaces a subterm in one clause with an equal term from another. Combined with a term ordering, this becomes the **superposition calculus** — the dominant approach for first-order logic with equality.

---

### 4.9 Extension Point: Strategy (`strategy/`)

Scaffolded for V2. Not compiled in V1.

The strategy module will enable dynamic algorithm selection based on problem analysis:

```
Input clauses → ProblemAnalyzer → ProblemFeatures → selectCalculus() → Calculus
                                                                         │
                        ┌────────────────────────────────────────────────┘
                        ▼
             Calculus = { generating_rules[], simplifying_rules[],
                          comparator, ordering, resource_limits }
                        │
                        ▼
                   Prover.prove(calculus)
```

This prevents search space explosion by loading only the inference rules relevant to the problem. A pure propositional problem would never load the paramodulation rule; an equality-heavy problem would skip pure resolution in favor of superposition.

---

## 5. Data Flow

### 5.1 End-to-End Pipeline

```
monkey_banana.p (TPTP text file)
       │
       ▼
  [tptp_parser]  ANTLR4 lexer/parser → parse tree → Formula AST
       │
       ▼
  [clausifier]   NNF → Skolemize → CNF → vector<Clause>
       │
       ▼
  [prover]       Given Clause Loop (resolution + factoring + subsumption)
       │
       ├── Normal: derive new clauses, enqueue, repeat
       │
       └── Empty clause found!
              │
              ▼
        [proof_trace]  DFS backtrack through parent pointers
              │
              ▼
        Human-readable proof output
```

### 5.2 Term Lifecycle

```
String "do(walk, S0)"
       │
       ▼ SymbolTable::intern()
  SymbolIds: { do=0, walk=1, S0=2 }
       │
       ▼ TermBank::makeTerm()
  TermIds:   S0_term=0, walk_term=1, do_walk_S0=2
       │     (hash-consed: calling makeTerm(do, [walk, S0]) again returns 2)
       │
       ▼ Used everywhere as integer IDs
  Clause { literals: [ Literal{atom=2, positive=true} ] }
```

### 5.3 Substitution Lifecycle

```
Unify( P(X, f(Y)),  P(a, f(b)) )
       │
       ▼ unify() creates stack-local Substitution
  σ = { X → a,  Y → b }
       │
       ▼ Apply σ to build resolvent clause
  New terms created in TermBank (hash-consed)
       │
       ▼ Substitution goes out of scope — destroyed
  Only the new TermIds persist
```

---

## 6. Known Limitations & Future Work

### 6.1 Critical Gaps (Ordered by Priority)

#### Gap 1: No Variable Standardization Apart

**Impact**: Correctness bug if not addressed before first real resolution.

When resolving clause `{P(X)}` with clause `{¬P(X), Q(X)}`, variable `X` in the two clauses must be treated as distinct. Without renaming one clause's variables (e.g., `X → X'`), the unifier incorrectly constrains both clauses' variables.

**Fix**: Add `renameVariables(Clause&, TermBank&, uint32_t& next_var_id)` to `infer/` before Phase 2 resolution implementation.

#### Gap 2: No Literal Selection Function

**Impact**: Search space is ~10x larger than necessary.

Ordered resolution restricts inference to the **maximal literal** (under a term ordering) in each clause. Our `Prover` currently has no concept of selecting which literal to resolve on — it tries all pairs.

**Fix**: V2 — add `LiteralSelector` interface, parameterized by `TermOrdering`.

#### Gap 3: Single Clause Queue (No Age-Weight Interleaving)

**Impact**: Incomplete or extremely slow on hard problems.

A single `ClauseComparator` cannot implement the age-weight ratio that E Prover uses: "pick the lightest clause 5 times, then the oldest clause once." This interleaving is crucial for both performance and completeness.

**Fix**: V2 — replace `ClauseComparator` with a `ClauseQueue` abstract class supporting multi-queue round-robin.

#### Gap 4: Single Index Type

**Impact**: Suboptimal retrieval performance.

The discrimination tree handles forward matching well but is not ideal for all query patterns. Modern ATPs maintain multiple complementary indices:

| Index Type | Best For |
|-----------|----------|
| Discrimination Tree | Forward matching (generalization queries) |
| Substitution Tree | Backward matching + unification candidates |
| Fingerprint Index | O(1) pre-filtering of non-unifiable pairs |
| Feature Vector Index | Clause-level subsumption checks |

**Fix**: V3 — abstract `TermIndex` interface, multiple implementations.

#### Gap 5: No Preprocessing Pipeline

**Impact**: Missed optimization opportunities.

Between clausification and the main loop, modern ATPs perform definition unfolding, pure literal elimination, equality axiom injection, and miniscoping. Our `normalize/` layer has only the clausifier.

**Fix**: V2 — extend `normalize/` into a transform pipeline with pluggable passes.

#### Gap 6: No AVATAR Splitting

**Impact**: Cannot compete on structured problems.

AVATAR splits clauses with independent variable sets into propositional components managed by an embedded SAT solver. This is the single largest performance innovation in ATP in the last decade (10-100x speedup on structured problems).

**Fix**: V4 — add `sat/` module with MiniSat/CaDiCaL integration + splitting logic in `search/`.

### 6.2 Non-Critical Gaps

| Gap | Category | Target Version |
|-----|----------|----------------|
| TSTP proof output format | Interoperability | V2 |
| Watched literals for unit propagation | Performance | V3 |
| Clause arena (replace `vector<Literal>`) | Memory | V3 |
| ML-guided clause selection | Research | V5+ |
| Parallel portfolio mode | Performance | V5+ |

---

## 7. References

1. Schulz, S. (2013). *System Description: E 1.8*. CADE-24.
2. Kovács, L., Voronkov, A. (2013). *First-Order Theorem Proving and Vampire*. CAV 2013.
3. Voronkov, A. (2014). *AVATAR: The Architecture for First-Order Theorem Provers*. CAV 2014.
4. Nieuwenhuis, R., Rubio, A. (2001). *Paramodulation-Based Theorem Proving*. Handbook of Automated Reasoning.
5. Sutcliffe, G. (2009). *The TPTP Problem Library and Associated Infrastructure*. Journal of Automated Reasoning.
6. Robinson, J.A. (1965). *A Machine-Oriented Logic Based on the Resolution Principle*. JACM.
