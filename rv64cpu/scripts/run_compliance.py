#!/usr/bin/env python3
"""riscv-arch-test compliance orchestration.

//     Richard CPU
//     Copyright (C) 2026  Richard Qin
//
//     This program is free software: you can redistribute it and/or modify
//     it under the terms of the GNU General Public License as published by
//     the Free Software Foundation, either version 3 of the License, or
//     (at your option) any later version.
//
//     This program is distributed in the hope that it will be useful,
//     but WITHOUT ANY WARRANTY; without even the implied warranty of
//     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//     GNU General Public License for more details.
//
//     You should have received a copy of the GNU General Public License
//     along with this program.  If not, see <https://www.gnu.org/licenses/>.

This script does three things:
1. Generates compliance ELFs through riscv-arch-test.
2. Runs the local compliance runner binary for each generated ELF.
3. Optionally compares produced signatures against discovered references.
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Optional

PROJECT_ROOT = Path(__file__).resolve().parent.parent
ARCH_TEST_DIR = PROJECT_ROOT / "sw" / "riscv-arch-test"
BUILD_DIR = PROJECT_ROOT / "build"

SUITES = {
    "i": "I",
    "m": "M",
    "a": "Zaamo,Zalrsc",
    "zicsr": "Zicsr",
    "zifencei": "Zifencei",
    "z": "Zicsr,Zifencei",
    "f": "F",
    "d": "D",
    "c": "Zca,Zcd",
    "priv_s": "S,Sm",
    "priv_sv_base": "Sv,SvPMP,Svade,Svbare",
    "priv_sv_optional": "SvZicbo,Svadu,SvaduPMP,Svnapot,Svpbmt",
    "priv_sv": "Sv,SvPMP,SvZicbo,Svade,Svadu,SvaduPMP,Svbare,Svnapot,Svpbmt",
    "priv_pmp": "PMPSm,PMPZca,PMPmisaligned",
    "priv_all": "S,Sm,Sv,SvPMP,SvZicbo,Svade,Svadu,SvaduPMP,Svbare,Svnapot,Svpbmt,PMPSm,PMPZca,PMPmisaligned",
    "g": "I,M,Zaamo,Zalrsc,F,D,Zicsr,Zifencei",
    "gc": "I,M,Zaamo,Zalrsc,F,D,Zicsr,Zifencei,Zca,Zcd",
    "full": "I,M,Zaamo,Zalrsc,F,D,Zicsr,Zifencei,Zca,Zcd,S,Sm,Sv,SvPMP,SvZicbo,Svade,Svadu,SvaduPMP,Svbare,Svnapot,Svpbmt,PMPSm,PMPZca,PMPmisaligned",
}


def list_suites():
    print("Available suites and ACT extension filters:")
    for name, extensions in SUITES.items():
        print(f"  {name:12s} -> EXTENSIONS={extensions}")


def run_cmd(cmd: List[str], cwd: Optional[Path] = None, verbose: bool = False) -> subprocess.CompletedProcess:
    if verbose:
        print("+", " ".join(cmd))
    return subprocess.run(cmd, cwd=cwd, text=True, capture_output=not verbose, check=False)


def parse_config_name(config_path: Path) -> str:
    with config_path.open("r", encoding="utf-8") as f:
        for raw_line in f:
            line = raw_line.strip()
            if line.startswith("name:"):
                return line.split(":", 1)[1].strip()
    raise RuntimeError(f"Missing 'name' in config: {config_path}")


def discover_elfs(work_dir: Path, config_name: str) -> List[Path]:
    elf_root = work_dir / config_name / "elfs"
    if not elf_root.exists():
        return []
    return sorted(elf_root.rglob("*.elf"))


def parse_extensions(extensions: str) -> List[str]:
    return [ext.strip() for ext in extensions.split(",") if ext.strip()]


def elf_extension_key(elf: Path, elf_root: Path, selected: set[str]) -> Optional[str]:
    rel = elf.relative_to(elf_root)
    parts = rel.parts
    if len(parts) < 2:
        return None
    if parts[0] == "priv":
        for part in parts[1:]:
            if part in selected:
                return part
        return None
    if parts[0].startswith("rv") and parts[1] in selected:
        return parts[1]
    return None


def filter_elfs_by_extensions(elf_files: List[Path], elf_root: Path, extensions: str) -> List[Path]:
    selected = set(parse_extensions(extensions))
    filtered = []
    for elf in elf_files:
        if elf_extension_key(elf, elf_root, selected) is not None:
            filtered.append(elf)
    return filtered


def count_elfs_by_extension(elf_files: List[Path], elf_root: Path, extensions: str) -> Dict[str, int]:
    selected = set(parse_extensions(extensions))
    counts = {ext: 0 for ext in parse_extensions(extensions)}
    for elf in elf_files:
        key = elf_extension_key(elf, elf_root, selected)
        if key is not None:
            counts[key] += 1
    return counts


def find_reference_signature(elf_path: Path, work_dir: Path) -> Optional[Path]:
    candidates = [
        elf_path.with_suffix(".signature"),
        elf_path.with_suffix(".reference_output"),
        elf_path.with_suffix(".ref"),
        elf_path.parent / "signatures" / f"{elf_path.stem}.signature",
    ]

    elf_root = next((parent for parent in elf_path.parents if parent.name == "elfs"), None)
    if elf_root is not None:
        rel = elf_path.relative_to(elf_root)
        candidates.append(elf_root.parent / "build" / rel.with_suffix(".sig"))
        candidates.extend(sorted((work_dir / "common").glob(f"*/build/{rel.with_suffix('.sig')}")))

    for candidate in candidates:
        if candidate.exists():
            return candidate

    # ACT4 places reference signatures under work/<config>/build or work/common/*/build.
    # Prefer raw .sig files for byte-for-byte comparison with DUT output.
    sig_matches = sorted(work_dir.rglob(f"{elf_path.stem}.sig"))
    if sig_matches:
        return sig_matches[0]

    # Fallback when only processed result files are available.
    result_matches = sorted(work_dir.rglob(f"{elf_path.stem}.results"))
    if result_matches:
        return result_matches[0]

    return None


def compare_text_files(a: Path, b: Path) -> bool:
    return a.read_text(encoding="utf-8", errors="ignore") == b.read_text(encoding="utf-8", errors="ignore")


def ensure_submodule_ready(arch_test_dir: Path):
    if not arch_test_dir.exists():
        raise RuntimeError(f"Missing arch-test directory: {arch_test_dir}")
    if not (arch_test_dir / "Makefile").exists():
        raise RuntimeError(
            f"Invalid arch-test checkout (Makefile missing): {arch_test_dir}. "
            "Run: git submodule update --init --recursive"
        )


def ensure_binary(sim_bin: Path, build_dir: Path, verbose: bool):
    if sim_bin.exists():
        return sim_bin

    matches = [p for p in build_dir.rglob(sim_bin.name) if p.is_file() and os.access(p, os.X_OK)]
    if matches:
        matches.sort(key=lambda p: len(str(p)))
        return matches[0]

    print(f"Simulation binary not found: {sim_bin}")
    print("Attempting to build compliance_runner target...")
    result = run_cmd(["cmake", "--build", str(build_dir), "--target", "compliance_runner"], verbose=verbose)
    if result.returncode != 0:
        if not verbose:
            print(result.stdout)
            print(result.stderr)
        raise RuntimeError("Failed to build compliance_runner")

    if sim_bin.exists():
        return sim_bin

    matches = [p for p in build_dir.rglob(sim_bin.name) if p.is_file() and os.access(p, os.X_OK)]
    if matches:
        matches.sort(key=lambda p: len(str(p)))
        print(f"Using discovered simulation binary: {matches[0]}")
        return matches[0]

    raise RuntimeError(f"Build completed but binary still missing: {sim_bin}")


def generate_elfs(
    arch_test_dir: Path,
    config_file: Path,
    work_dir: Path,
    extensions: str,
    jobs: int,
    verbose: bool,
):
    cmd = [
        "make",
        "elfs",
        f"CONFIG_FILES={config_file}",
        f"WORKDIR={work_dir}",
        f"EXTENSIONS={extensions}",
        "EXCLUDE_EXTENSIONS=",
        f"JOBS={jobs}",
        "FAST=True",
    ]
    result = run_cmd(cmd, cwd=arch_test_dir, verbose=verbose)
    if result.returncode != 0:
        merged_output = ""
        if result.stdout:
            merged_output += result.stdout + "\n"
        if result.stderr:
            merged_output += result.stderr

        missing_tooling_hints = [
            "uv command not found",
            "covergroupgen: Permission denied",
            "command not found",
        ]
        if any(hint.lower() in merged_output.lower() for hint in missing_tooling_hints):
            raise RuntimeError(
                "Failed to generate ELFs because ACT tooling is unavailable in this environment. "
                "Install uv and project python deps in sw/riscv-arch-test, or run with --skip-generate "
                "to reuse an existing work directory with prebuilt ELFs."
            )

        if not verbose:
            print(result.stdout)
            print(result.stderr)
        raise RuntimeError("Failed to generate compliance ELFs via riscv-arch-test")


def run_suite(
    suite_name: str,
    build_dir: Path,
    sim_binary: str,
    config_file: Path,
    work_dir: Path,
    max_cycles: int,
    max_tests: int,
    jobs: int,
    require_reference: bool,
    keep_work: bool,
    skip_generate: bool,
    verbose: bool,
):
    if suite_name not in SUITES:
        print(f"Unknown suite: {suite_name}")
        print(f"Available: {', '.join(SUITES.keys())}")
        return False

    ensure_submodule_ready(ARCH_TEST_DIR)

    if not config_file.exists():
        raise RuntimeError(f"Config file not found: {config_file}")

    config_name = parse_config_name(config_file)
    sim_bin = ensure_binary(build_dir / sim_binary, build_dir, verbose)

    if not skip_generate:
        if work_dir.exists() and not keep_work:
            shutil.rmtree(work_dir)
        work_dir.mkdir(parents=True, exist_ok=True)

    extensions = SUITES[suite_name]
    if skip_generate:
        print(f"Skipping ELF generation for suite={suite_name}, extensions={extensions}")
    else:
        print(f"Generating ELFs for suite={suite_name}, extensions={extensions}")
        generate_elfs(ARCH_TEST_DIR, config_file, work_dir, extensions, jobs, verbose)

    elf_root = work_dir / config_name / "elfs"
    discovered_elfs = discover_elfs(work_dir, config_name)
    elf_files = filter_elfs_by_extensions(discovered_elfs, elf_root, extensions)

    if max_tests == 0:
        elf_counts = count_elfs_by_extension(elf_files, elf_root, extensions)
        missing_extensions = [ext for ext, count in elf_counts.items() if count == 0]
        if missing_extensions:
            print("No generated ELF files for requested extensions:")
            for ext in missing_extensions:
                print(f"  {ext}")
            print("Generated ELF counts:")
            for ext, count in elf_counts.items():
                print(f"  {ext:16s} {count}")
            return False

    if max_tests > 0:
        elf_files = elf_files[:max_tests]

    if not elf_files:
        print("No generated ELF files found.")
        return False

    signature_root = build_dir / "compliance_signatures" / suite_name
    signature_root.mkdir(parents=True, exist_ok=True)

    print(f"Running compliance runner on {len(elf_files)} ELF files")

    passed = 0
    failed = 0
    no_reference = 0

    for idx, elf in enumerate(elf_files, start=1):
        rel = elf.relative_to(work_dir / config_name / "elfs")
        sig_path = signature_root / rel.with_suffix(".signature")
        sig_path.parent.mkdir(parents=True, exist_ok=True)
        if sig_path.exists():
            sig_path.unlink()

        if verbose:
            print(f"[{idx}/{len(elf_files)}] {rel}")

        run_result = run_cmd(
            [
                str(sim_bin),
                "--elf",
                str(elf),
                "--max-cycles",
                str(max_cycles),
                "--signature",
                str(sig_path),
            ],
            verbose=verbose,
        )
        if not sig_path.exists():
            failed += 1
            print(f"FAIL {rel} (signature not generated)")
            continue

        ref_sig = find_reference_signature(elf, work_dir)
        if run_result.returncode != 0:
            failed += 1
            if ref_sig is None:
                no_reference += 1
                print(f"FAIL {rel} (runner exit={run_result.returncode}, no reference signature)")
            else:
                if compare_text_files(sig_path, ref_sig):
                    print(f"FAIL {rel} (runner exit={run_result.returncode}, signature matched {ref_sig})")
                else:
                    print(f"FAIL {rel} (runner exit={run_result.returncode}, signature mismatch against {ref_sig})")
            continue

        if ref_sig is None:
            no_reference += 1
            if require_reference:
                failed += 1
                print(f"FAIL {rel} (no reference signature found)")
            else:
                passed += 1
            continue

        if compare_text_files(sig_path, ref_sig):
            passed += 1
        else:
            failed += 1
            if run_result.returncode != 0:
                print(f"FAIL {rel} (runner exit={run_result.returncode}, signature mismatch against {ref_sig})")
            else:
                print(f"FAIL {rel} (signature mismatch against {ref_sig})")

    print("=" * 60)
    print(f"Suite: {suite_name}")
    print(f"Total: {len(elf_files)}  Passed: {passed}  Failed: {failed}  NoRef: {no_reference}")
    print(f"Signatures: {signature_root}")
    print("=" * 60)

    return failed == 0


def main():
    parser = argparse.ArgumentParser(description="Run riscv-arch-test compliance tests")
    parser.add_argument("--suite", type=str, help="Test suite to run")
    parser.add_argument("--build-dir", type=str, default="build", help="Build directory")
    parser.add_argument("--sim", type=str, default="compliance_runner", help="Sim binary name")
    parser.add_argument(
        "--config",
        type=str,
        default="config/spike/spike-rv64-max/test_config.yaml",
        help="Config file path (relative to sw/riscv-arch-test or absolute)",
    )
    parser.add_argument(
        "--work-dir",
        type=str,
        default="build/compliance_work",
        help="Working directory for generated ELFs",
    )
    parser.add_argument("--max-cycles", type=int, default=500000, help="Cycles per test for compliance runner")
    parser.add_argument("--max-tests", type=int, default=0, help="Limit number of ELFs to run (0 = all)")
    parser.add_argument("--jobs", type=int, default=0, help="Parallel jobs for ACT test generation")
    parser.add_argument(
        "--require-reference",
        action="store_true",
        help=argparse.SUPPRESS,
    )
    parser.add_argument(
        "--allow-missing-reference",
        action="store_true",
        help="Pass tests without reference signatures when the runner exits successfully",
    )
    parser.add_argument(
        "--keep-work",
        action="store_true",
        help="Keep existing work directory instead of cleaning it first",
    )
    parser.add_argument(
        "--skip-generate",
        action="store_true",
        help="Skip ACT generation and reuse existing ELFs under --work-dir",
    )
    parser.add_argument("--list", action="store_true", help="List available suites")
    parser.add_argument("-v", "--verbose", action="store_true", help="Verbose output")

    args = parser.parse_args()

    if args.list:
        list_suites()
        return

    if not args.suite:
        parser.print_help()
        sys.exit(1)

    build_dir = Path(args.build_dir).resolve()
    work_dir = Path(args.work_dir).resolve()

    config_file = Path(args.config)
    if not config_file.is_absolute():
        config_file = ARCH_TEST_DIR / config_file

    try:
        success = run_suite(
            suite_name=args.suite,
            build_dir=build_dir,
            sim_binary=args.sim,
            config_file=config_file,
            work_dir=work_dir,
            max_cycles=args.max_cycles,
            max_tests=args.max_tests,
            jobs=args.jobs,
            require_reference=not args.allow_missing_reference,
            keep_work=args.keep_work,
            skip_generate=args.skip_generate,
            verbose=args.verbose,
        )
    except RuntimeError as e:
        print(f"ERROR: {e}")
        success = False

    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
