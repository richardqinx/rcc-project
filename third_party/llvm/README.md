# LLVM RISC-V MIR Test Assets

This directory contains unmodified LLVM test assets used to verify RCC MachineIR compatibility.

- Upstream project: https://github.com/llvm/llvm-project
- Upstream revision: `2b40c303e79f64c5c7642032554eda026594d604`
- Upstream path: `llvm/test/CodeGen/RISCV`
- Vendored files: all 462 `.mir` files below `llvm/test/CodeGen/RISCV`
- License: Apache License v2.0 with LLVM Exceptions; see `LICENSE.TXT`
- Sorted path-list SHA-256: `5b4dce23ecdca0b1c66eb98e4fa1c9ab41d86ec72a04770f56a451364ead3b5f`
- Aggregate content SHA-256: `6fffb35c378e290a030e4dff07d5a3dbb3d0e16f81012925891872cb051299c9`

The aggregate content digest is the SHA-256 of the sorted, relative-path `sha256sum` output for
all vendored `.mir` files. Test inputs, `RUN:` directives, and FileCheck assertions must remain
unchanged. RCC-specific classifications and acceptance results live under
`docs/compiler-architecture/` rather than in these upstream files.
