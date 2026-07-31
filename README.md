# RCC Project

RCC is a compiler collection under active development. The current implementation
focuses on a standalone machine-code backend.

## Repository layout

```text
backend/    Standalone backend project
cmake/      Monorepo orchestration support
docs/       Architecture and implementation records
```

Future frontend, middle-end, and driver projects will be sibling components.

## Build the enabled projects

RCC requires Clang and C++23. Both Ninja and Unix Makefiles are supported.

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DRCC_ENABLE_PROJECTS=backend
cmake --build build
ctest --test-dir build --output-on-failure
```

## Build the backend independently

```bash
cmake -S backend -B build-backend -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++
cmake --build build-backend
ctest --test-dir build-backend --output-on-failure
```

Replace `Ninja` with `Unix Makefiles` to build with Make.

Developer checks are exposed as `rcc-backend-format-check`,
`rcc-backend-tidy`, and `rcc-backend-analyze` in a standalone backend build.
The monorepo exposes the aggregate `format-check`, `tidy`, and `analyze`
targets.

## License

RCC is free software distributed under GPL-3.0-or-later. See `LICENSE`.
