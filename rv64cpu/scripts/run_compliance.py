#!/usr/bin/env python3
"""
run_compliance.py — riscv-arch-test 自动化运行脚本

Usage:
    python3 scripts/run_compliance.py --suite rv64i
    python3 scripts/run_compliance.py --suite rv64im --build-dir build
    python3 scripts/run_compliance.py --list

Requirements:
    - Verilator simulation binary built (tb_soc or compliance_runner)
    - riscv64-unknown-elf-gcc toolchain in PATH
    - sw/riscv-arch-test submodule initialized
"""

import argparse
import subprocess
import sys
import os
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
ARCH_TEST_DIR = PROJECT_ROOT / "sw" / "riscv-arch-test"
BUILD_DIR = PROJECT_ROOT / "build"

SUITES = {
    "rv64i":   "riscv-test-suite/rv64i_m/I",
    "rv64im":  "riscv-test-suite/rv64i_m/M",
    "rv64ia":  "riscv-test-suite/rv64i_m/A",
    "rv64if":  "riscv-test-suite/rv64i_m/F",
    "rv64id":  "riscv-test-suite/rv64i_m/D",
    "rv64ic":  "riscv-test-suite/rv64i_m/C",
    "rv64priv": "riscv-test-suite/rv64i_m/privilege",
}


def list_suites():
    print("Available test suites:")
    for name, path in SUITES.items():
        exists = "✓" if (ARCH_TEST_DIR / path).exists() else "✗ (not found)"
        print(f"  {name:12s} → {path}  {exists}")


def run_suite(suite_name: str, build_dir: Path, sim_binary: str, verbose: bool):
    if suite_name not in SUITES:
        print(f"Unknown suite: {suite_name}")
        print(f"Available: {', '.join(SUITES.keys())}")
        sys.exit(1)

    suite_path = ARCH_TEST_DIR / SUITES[suite_name]
    if not suite_path.exists():
        print(f"Suite path not found: {suite_path}")
        print("Did you initialize the submodule?")
        print("  git submodule update --init --recursive")
        sys.exit(1)

    sim_bin = build_dir / sim_binary
    if not sim_bin.exists():
        print(f"Simulation binary not found: {sim_bin}")
        print("Build it first: cmake --build build --target compliance_runner")
        sys.exit(1)

    test_files = sorted(suite_path.glob("src/*.S"))
    if not test_files:
        print(f"No test sources found in {suite_path / 'src'}")
        sys.exit(1)

    print(f"\n{'='*60}")
    print(f"Running {suite_name} compliance tests ({len(test_files)} tests)")
    print(f"{'='*60}\n")

    passed = 0
    failed = 0
    errors = []

    for test_file in test_files:
        test_name = test_file.stem
        if verbose:
            print(f"  [{test_name}] ", end="", flush=True)

        # TODO: Implement actual test compilation and simulation
        # This is a skeleton — fill in with your specific flow:
        #   1. Compile test_file with riscv64-unknown-elf-gcc
        #   2. Run the sim binary with the compiled ELF
        #   3. Compare signature output against reference
        if verbose:
            print("⏭ SKIP (runner not yet implemented)")

    print(f"\n{'='*60}")
    print(f"Results: {passed} passed, {failed} failed, {len(test_files) - passed - failed} skipped")
    print(f"{'='*60}\n")

    return failed == 0


def main():
    parser = argparse.ArgumentParser(description="Run riscv-arch-test compliance tests")
    parser.add_argument("--suite", type=str, help="Test suite to run")
    parser.add_argument("--build-dir", type=str, default="build", help="Build directory")
    parser.add_argument("--sim", type=str, default="compliance_runner", help="Sim binary name")
    parser.add_argument("--list", action="store_true", help="List available suites")
    parser.add_argument("-v", "--verbose", action="store_true", help="Verbose output")

    args = parser.parse_args()

    if args.list:
        list_suites()
        return

    if not args.suite:
        parser.print_help()
        sys.exit(1)

    build_dir = Path(args.build_dir)
    success = run_suite(args.suite, build_dir, args.sim, args.verbose)
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
