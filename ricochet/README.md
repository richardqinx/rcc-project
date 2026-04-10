# Ricochet
> **A minimalist CLI web browser for fast, distraction-free navigation.**

**Ricochet** is a terminal-based web browser designed for those who value speed, privacy, and the original spirit of the World Wide Web. By stripping away the "JavaScript Trap" and focus-stealing elements of modern web design, Ricochet delivers content in its purest form directly to your emulator.

## Philosophy
Modern browsing has become a minefield of non-free scripts, tracking pixels, and visual clutter. Ricochet ricochets off the noise:
* **Freedom First:** No JavaScript support by design. We serve the user, not the scripts.
* **Minimalist:** Focused entirely on text and structure.
* **Distraction-Free:** No ads, no popups, no auto-playing media. Just the data you came for.

## Key Features
- [x] **High-Performance C++ Core:** Native speed with minimal memory footprint.
- [x] **Robust HTML5 Parser:** A custom, fault-tolerant DOM builder for the "wild" web.
- [x] **TUI Navigation:** Intuitive, keyboard-driven interface (Vim-like bindings).
- [x] **Privacy Centric:** No cookies by default, no telemetry, and zero JS execution.
- [x] **Secure:** Native HTTPS support via modern SSL/TLS stacks.

## Architecture
Ricochet is built with a strictly modular approach:
* **`Net`**: Handles TCP/TLS streams and HTTP/1.1 protocol state.
* **`Parser`**: A state-machine driven HTML lexer and DOM tree builder.
* **`Layout`**: Calculates box models and word-wrapping for terminal constraints.
* **`TUI`**: An event-driven terminal UI handling viewport rendering and user input.

## Getting Started

### Prerequisites
To build Ricochet, ensure your environment meets the following requirements:
* **Compiler**: A C++ compiler with full **C++23** support (e.g., **GCC 13+** or **Clang 15+**).
* **Build System**: **CMake 3.24.2** or higher.
* **Development Tools** (Optional but recommended): 
    * `clang-format` for automated code styling.
    * `clang-tidy` for static analysis and linting.

### Build Instructions
Ricochet enforces out-of-source builds to keep the project structure clean. By default, the project builds in Debug mode with Address and Undefined Behavior sanitizers enabled.

```bash
# 1. Clone the repository
git clone https://github.com/yourusername/ricochet.git
cd ricochet

# 2. Configure the project
# This will generate compile commands and set the build type to Release
cmake -B build -DCMAKE_BUILD_TYPE=Release

# 3. Compile the project using all available cores
cmake --build build -j$(nproc)

# 4. Launch Ricochet
# The executable is located in the bin/ directory
./build/bin/ricochet https://wikipedia.org 
```

### Build Parameters & Configuration

You can customize the build by passing `-D<VAR>=<VALUE>` to the initial `cmake -B build` command.

#### 1. Build Types (`CMAKE_BUILD_TYPE`)
The project defaults to **Debug** mode if no type is specified.
* **`Debug`**: Enables full sanitizers and debug symbols (Default).
* **`Release`**: Optimizes for performance and reduces binary size.
* **`RelWithDebInfo`**: Optimized build that retains debug symbols.

#### 2. Security & Quality Flags
These parameters are automatically managed by `etc/cflags.cmake`:
* **Sanitizers:** Includes AddressSanitizer (ASan) and UndefinedBehaviorSanitizer (UBSan). Recovery is disabled for all errors (`-fno-sanitize-recover=all`).
* **Warning Level:** Set to extremely strict levels, including `-Wall`, `-Wextra`, `-Wpedantic`, and `-Weffc++`.
* **Warnings as Errors:** All compiler warnings are treated as errors (`-Werror`), ensuring no code is merged with unresolved issues.

#### 3. Output Directories
The build system organizes outputs into the following structure within your build folder:
* **Executables:** `build/bin/`
* **Static/Shared Libraries:** `build/lib/`
* **Compile Commands:** `build/compile_commands.json` is generated for LSP (Language Server) support.

---

### Development & Maintenance Targets

The project includes several custom targets for code maintenance:

| Target | Command | Description |
| :--- | :--- | :--- |
| **Format** | `cmake --build build --target format` | Uses `clang-format` to enforce consistent coding style across all `.cc` and `.hh` files. |
| **Tidy** | `cmake --build build --target tidy` | Runs `clang-tidy` on the core Ricochet source files for static analysis. |
| **Tidy-All** | `cmake --build build --target tidy-all` | Runs `clang-tidy` on every file in the project, including tests. |
| **Test** | `ctest --test-dir build` | Executes the test suite via CTest. |

### Installation
To install the compiled binaries to your system, use:
```bash
cmake --install build --prefix /usr/local
```
This installs the `ricochet` binary to `bin` and supporting libraries to `lib`.
## Navigation
| Key | Action |
| :--- | :--- |
| `j / k` | Scroll Down / Up |
| `Tab` | Cycle through links |
| `Enter` | Follow selected link |
| `b` | Go Back in history |
| `u` | Enter new URL |
| `q` | Quit Ricochet |

## Acknowledgments
The low-level POSIX socket wrappers in `src/net/sys/` are adapted from the Minnow networking framework developed for Stanford University's CS144 course. These utilities are used gracefully to handle file descriptors and network interfaces.

## License
Ricochet is free software: you can redistribute it and/or modify it under the terms of the **GNU General Public License (GPL) v3**.

---
*Built for the terminal. Built for freedom.*