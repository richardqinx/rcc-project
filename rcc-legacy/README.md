# RCC-Legacy

An optimizing compiler for **SysY** (a C subset) targeting **RISC-V 64GC**.

This is the first generation of `rcc`, built as a C++ compiler project inspired
by [QBE](https://c9x.me/compile/)'s SSA-based architecture. It is kept here as a
frozen reference; note that active development has moved to a new implementation
in a separate directory, and this directory will no longer be maintained.

## Pipeline

```
SysY source
  → frontend         lexer / parser / AST
  → IRGen            SSA construction (Braun et al.), QBE-style IR
  → mid-end          SSA optimization passes
  → rv64 backend     instruction selection → register allocation → emit
  → RISC-V assembly
```

Per-function processing is decoupled from global mutable state via an explicit
`Ctx` / `SubPool` model, allowing functions to be compiled independently.

### Mid-end passes

mem2reg / promote, load forwarding, constant folding, copy propagation, GVN,
GCM, LICM, SCCP, reassociation, strength reduction (div / mul), loop strength
reduction, loop idiom recognition (memset / memcpy), inlining, and DCE.

### Backend (RISC-V 64)

ABI lowering, instruction selection (with cmp+branch fusion and address-offset
folding), liveness, spilling, register allocation, peephole, and ELF assembly
emission.

## Build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

The compiler binary is `build/rcc`. Use `--emit-il` to dump the
intermediate representation and `-d <flags>` for per-pass debug output.

## Test

Test cases are stored in `tests/`.

```sh
./tests/run_rv64_e2e.sh
./tests/run_perf.sh
```

You may need to edit the shell scripts in `tests/` to adapt
to your local environment.

## Competition

This compiler was built for the 2026 compiler design competition @ RUC.

## Thanks

- [QBE](https://c9x.me/compile/)
- [TrivialCompiler](https://github.com/TrivialCompiler/TrivialCompiler)
- [indigo](https://github.com/segviol/indigo)
- [sysy-testsuit-collection](https://github.com/jokerwyt/sysy-testsuit-collection)
- [lava-test](https://github.com/ustb-owl/lava-test)

## License

rcc-legacy is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 3, or (at your option) any later
version.