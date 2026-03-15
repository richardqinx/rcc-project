# Richard CPU

**RV64GCZicsr_Zifencei** Five-Stage In-Order Pipelined Processor SoC

Written in SystemVerilog, simulated with Verilator, targeting official riscv-arch-test compliance.

## Features

- **ISA**: RV64IMAFDC + Zicsr + Zifencei (RV64GC)
- **Pipeline**: 5-stage in-order single-issue (IF → ID → EX → MEM → WB)
- **Privilege Levels**: M / S / U tri-mode
- **Virtual Memory**: Sv39 (MMU with TLB + Hardware PTW)
- **Cache**: I-Cache 4KB + D-Cache 4KB (AXI4-Full Burst refill)
- **Bus**: AXI4-Full (Cache) + AXI4-Lite (Peripherals) + auto bridge
- **Peripherals**: CLINT / PLIC / UART 16550 / GPIO / SRAM Controller
- **Debug**: JTAG Debug Transport Module (reserved)
- **FPGA**: Xilinx / Gowin synthesis support (reserved)

## Quick Start

### Dependencies

- [Verilator](https://verilator.org) ≥ 5.0
- CMake ≥ 3.20
- C++17 compiler (GCC / Clang)
- [riscv64-unknown-elf-gcc](https://github.com/riscv-collab/riscv-gnu-toolchain) (software compilation, optional)
- [GTKWave](http://gtkwave.sourceforge.net/) (waveform viewer, optional)

### Build Simulation

```bash
# Configure (enable waveform tracing)
cmake -B build -DENABLE_TRACE=ON

# Compile
cmake --build build -j$(nproc)

# Run all tests
ctest --test-dir build --output-on-failure
```

### Build Bare-Metal Programs

```bash
cmake -B build_sw -DBUILD_SW=ON \
      -DCMAKE_TOOLCHAIN_FILE=tools/toolchain-riscv64.cmake

cmake --build build_sw -j$(nproc)
```

### Run Compliance Tests

```bash
# Initialize riscv-arch-test submodule
git submodule update --init --recursive

# Run RV64I test suite
python3 scripts/run_compliance.py --suite rv64i -v
```

### View Waveforms

```bash
./scripts/wave_view.sh                # Open latest waveform
./scripts/wave_view.sh waves/soc.vcd  # Open specific file
```

## Documentation

| Document | Description |
|---|---|
| [docs/architecture.md](docs/architecture.md) | Microarchitecture design (pipeline, privilege levels, MMU, FPU, cache, bus) |
| [docs/memory_map.md](docs/memory_map.md) | Physical address space and peripheral register layout |
| [docs/isa_support.md](docs/isa_support.md) | ISA instruction implementation status matrix |
| [docs/pipeline_diagram.md](docs/pipeline_diagram.md) | Pipeline timing, inter-stage registers, forwarding/hazards |

## Project Structure

```
richard-cpu/
├── CMakeLists.txt              # Top-level build
├── rtl/                        # SystemVerilog RTL
│   ├── core/                   #   CPU core (pipeline stages + functional units)
│   │   ├── include/            #     Global packages (rv64_pkg, csr_pkg)
│   │   ├── mmu/                #     MMU (TLB + PTW)
│   │   ├── fpu/                #     FPU (F/D extensions)
│   │   └── debug/              #     Debug DTM (reserved)
│   ├── cache/                  #   I-Cache + D-Cache
│   ├── bus/                    #   AXI4 interconnect + AXI4-Lite bridge
│   ├── perips/                 #   Peripherals (CLINT/PLIC/UART/GPIO/SRAM)
│   └── soc/                    #   SoC top-level integration
├── tb/                         # Verilator simulation testbench
│   ├── common/                 #   Common infrastructure
│   ├── unit/                   #   Unit tests
│   ├── core/                   #   Core integration tests
│   ├── soc/                    #   SoC-level simulation
│   └── compliance/             #   riscv-arch-test runner
├── sw/                         # Software
│   ├── common/                 #   Startup code + linker script
│   ├── baremetal/              #   Bare-metal test programs
│   └── riscv-arch-test/        #   Official compliance tests (git submodule)
├── fpga/                       # FPGA synthesis
│   ├── xilinx/                 #   Artix-7 top-level + constraints + Tcl
│   └── gowin/                  #   (reserved)
├── scripts/                    # Helper scripts
├── tools/                      # CMake toolchain files
└── docs/                       # Design documentation
```

## Roadmap

| Phase | Milestone |
|---|---|
| P1 | RV64I five-stage pipeline + hazard handling |
| P2 | CSR + M/S/U privilege levels + Trap |
| P3 | MMU (Sv39 TLB + PTW) |
| P4 | Cache + AXI4-Full Burst |
| P5 | Peripherals + AXI4-Lite bridge + SoC integration |
| P6 | Pass riscv-arch-test |
| P7 | M/F/D/A/C extensions + FPGA bring-up |

## License

[GPLv3](LICENSE)
