# ATP Engine

A saturation-based automated theorem prover for first-order logic, written in C++20.

## Overview

ATP Engine implements the **Given Clause Loop** (Otter/E-style) with hash-consed term sharing, discrimination tree indexing, and pluggable search heuristics. It reads problems in [TPTP](https://www.tptp.org/) format and produces human-readable proofs via resolution and factoring.

The architecture is designed for extensibility toward equality reasoning (superposition), sorted logic, and theory integration. See [`docs/architecture.md`](docs/architecture.md) for full design documentation.

## Build

Requires CMake 3.20+ and a C++20 compiler (GCC 11+ / Clang 14+).

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
```

### Options

| Flag | Default | Description |
|------|---------|-------------|
| `ATP_ENABLE_CLANG_TIDY` | `OFF` | Run clang-tidy during compilation |
| `ATP_ENABLE_SANITIZERS` | `ON` | ASan + UBSan in Debug builds |
| `ATP_BUILD_TESTS` | `ON` | Build GoogleTest unit tests |

### Format & Lint

```bash
cmake --build build --target format        # auto-format all sources
cmake --build build --target format-check  # check format compliance
```

## Run

```bash
./build/atp data/monkey_banana.p
```

## Test

```bash
cd build && ctest --output-on-failure
```

## License

This project is licensed under the [GNU General Public License v3.0](https://www.gnu.org/licenses/gpl-3.0.html). Also see [LICENSE](LICENSE)
