# SPDX-License-Identifier: GPL-3.0-or-later

from __future__ import annotations

import argparse
import csv
import dataclasses
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Iterable


IN_SCOPE_CATEGORIES = {"RV64GC_EXACT", "RV64GC_SEMANTIC", "RV64GC_INPUT"}
OUT_OF_SCOPE_CATEGORIES = {
    "OUT_RV32": "rv32",
    "OUT_RVV": "rvv",
    "OUT_EXTENSION": "extension",
}
RV64GC_FEATURES = {"i", "m", "a", "f", "d", "c", "zicsr", "zifencei"}
NON_ISA_FEATURES = {
    "disable-postmisched-load-clustering",
    "disable-postmisched-store-clustering",
    "fusion-add-load",
    "fusion-add-mem",
    "fusion-addi-load",
    "fusion-auipc-addi",
    "fusion-auipc-load",
    "fusion-ld-add",
    "fusion-logic-imm-reg",
    "fusion-logic-reg-imm",
    "fusion-logic-reg-reg",
    "fusion-lui-addi",
    "fusion-lui-load",
    "fusion-mul-add",
    "fusion-shift-bit-extract",
    "fusion-shifted-zextw",
    "fusion-zexth",
    "fusion-zextw",
    "prefer-w-inst",
    "reserve-x6",
    "reserve-x7",
    "save-restore",
    "tagged-globals",
    "unaligned-scalar-mem",
    "use-postra-scheduler",
}
RUN_PATTERN = re.compile(r"^\s*#\s*RUN:\s?(.*)$")
FILECHECK_PATTERN = re.compile(r"(?<![\w./-])FileCheck(?=\s|$)")
LLC_PATTERN = re.compile(r"(?<![\w./-])%?llc(?=\s|$)")
NOT_PATTERN = re.compile(r"(?<![\w./-])not(?=\s|$)")
SUBSTITUTION_PATTERN = re.compile(r"%([A-Za-z][A-Za-z0-9_/.:+-]*)")


@dataclasses.dataclass(frozen=True)
class RunDirective:
    line: int
    command: str


@dataclasses.dataclass(frozen=True)
class TestDescription:
    relative_path: str
    path: Path
    category: str
    reason: str
    runs: tuple[RunDirective, ...]


@dataclasses.dataclass
class GateResult:
    status: str
    diagnostic: str = ""


@dataclasses.dataclass
class RunResult:
    line: int
    command: str
    scope: str
    scope_reason: str
    production: str = "not-applicable"
    filecheck: str = "not-applicable"
    diagnostic: str = ""


@dataclasses.dataclass
class FileResult:
    path: str
    category: str
    reason: str
    input_gate: GateResult
    runs: list[RunResult]


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Execute unmodified LLVM RISC-V MIR RUN directives with RCC"
    )
    parser.add_argument("--tool", type=Path, required=True)
    parser.add_argument("--corpus", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--filecheck", type=Path, required=True)
    parser.add_argument("--not-tool", type=Path)
    parser.add_argument("--llvm-bin", type=Path)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--jobs", type=int, default=max(1, min(8, os.cpu_count() or 1)))
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("--validate-only", action="store_true")
    parser.add_argument("--strict", action="store_true")
    parser.add_argument("--max-diagnostics", type=int, default=20)
    return parser.parse_args()


def read_manifest(path: Path) -> dict[str, tuple[str, str]]:
    lines = [line for line in path.read_text(encoding="utf-8").splitlines() if not line.startswith("#")]
    reader = csv.DictReader(lines, delimiter="\t")
    if reader.fieldnames != ["path", "category", "reason"]:
        raise ValueError("classification manifest has an invalid header")
    result: dict[str, tuple[str, str]] = {}
    for row in reader:
        relative_path = row["path"]
        category = row["category"]
        reason = row["reason"]
        if not relative_path or category not in IN_SCOPE_CATEGORIES | set(OUT_OF_SCOPE_CATEGORIES):
            raise ValueError(f"classification manifest has an invalid row for '{relative_path}'")
        if relative_path in result:
            raise ValueError(f"classification manifest repeats '{relative_path}'")
        result[relative_path] = (category, reason)
    return result


def read_run_directives(path: Path) -> tuple[RunDirective, ...]:
    result: list[RunDirective] = []
    command = ""
    start_line = 0
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        match = RUN_PATTERN.match(line)
        if match is None:
            if command:
                raise ValueError(f"{path}:{start_line}: unterminated RUN continuation")
            continue
        fragment = match.group(1).rstrip()
        if not command:
            start_line = line_number
        continued = fragment.endswith("\\")
        if continued:
            fragment = fragment[:-1].rstrip()
        command = f"{command} {fragment}".strip()
        if not continued:
            if not command:
                raise ValueError(f"{path}:{start_line}: empty RUN directive")
            result.append(RunDirective(start_line, command))
            command = ""
    if command:
        raise ValueError(f"{path}:{start_line}: unterminated RUN continuation")
    return tuple(result)


def load_tests(corpus: Path, manifest_path: Path) -> list[TestDescription]:
    manifest = read_manifest(manifest_path)
    corpus_paths = {
        path.relative_to(corpus).as_posix(): path for path in corpus.rglob("*.mir") if path.is_file()
    }
    missing = sorted(set(manifest) - set(corpus_paths))
    unclassified = sorted(set(corpus_paths) - set(manifest))
    if missing or unclassified:
        details = []
        if missing:
            details.append(f"missing files: {', '.join(missing[:5])}")
        if unclassified:
            details.append(f"unclassified files: {', '.join(unclassified[:5])}")
        raise ValueError("classification and corpus differ: " + "; ".join(details))
    tests = []
    for relative_path in sorted(corpus_paths):
        category, reason = manifest[relative_path]
        runs = read_run_directives(corpus_paths[relative_path])
        if not runs:
            raise ValueError(f"{corpus_paths[relative_path]}: no RUN directives")
        tests.append(
            TestDescription(relative_path, corpus_paths[relative_path], category, reason, runs)
        )
    return tests


def option_values(tokens: list[str], names: Iterable[str]) -> list[str]:
    result: list[str] = []
    name_set = set(names)
    index = 0
    while index < len(tokens):
        token = tokens[index]
        if token in name_set:
            if index + 1 < len(tokens):
                result.append(tokens[index + 1])
                index += 2
                continue
        for name in name_set:
            prefix = name + "="
            if token.startswith(prefix):
                result.append(token[len(prefix) :])
                break
        index += 1
    return result


def command_scope(test: TestDescription, command: str) -> tuple[str, str, bool]:
    if test.category in OUT_OF_SCOPE_CATEGORIES:
        scope = OUT_OF_SCOPE_CATEGORIES[test.category]
        return scope, f"file classification {test.category}: {test.reason}", True
    try:
        tokens = shlex.split(command)
    except ValueError as error:
        return "invalid", f"cannot tokenize RUN directive: {error}", True

    triples = option_values(tokens, ("-mtriple", "--mtriple"))
    for triple in triples:
        if triple.startswith("riscv32"):
            return "rv32", f"RUN selects {triple}", True
        if not triple.startswith("riscv64"):
            return "target", f"RUN selects unsupported triple {triple}", True

    abis = option_values(tokens, ("-target-abi", "--target-abi"))
    for abi in abis:
        if abi.startswith("ilp32"):
            return "rv32", f"RUN selects {abi}", True
        if not abi.startswith("lp64"):
            return "target", f"RUN selects unsupported ABI {abi}", True

    cpus = option_values(tokens, ("-mcpu", "--mcpu"))
    for cpu in cpus:
        if cpu not in {"generic-rv64", "generic-rv64gc"}:
            return "cpu", f"RUN selects unsupported CPU {cpu}", True

    attributes = option_values(tokens, ("-mattr", "--mattr"))
    for attribute_list in attributes:
        for attribute in attribute_list.split(","):
            feature = attribute.strip().lstrip("+-")
            if not feature:
                continue
            if feature == "v" or feature.startswith(("zv", "xsfv", "xsfmm")):
                return "rvv", f"RUN selects vector feature {feature}", True
            if feature not in RV64GC_FEATURES | NON_ISA_FEATURES:
                return "extension", f"RUN selects non-RV64GC feature {feature}", True

    has_target_context = bool(triples or abis or cpus or attributes or LLC_PATTERN.search(command))
    return "selected", "RV64GC-compatible RUN", has_target_context


def shell_quote(path: Path | str) -> str:
    return shlex.quote(str(path))


def validate_substitutions(command: str, path: Path, line: int) -> None:
    supported = {"s", "t"}
    for substitution in SUBSTITUTION_PATTERN.findall(command):
        if substitution in supported or re.fullmatch(r"t[0-9]+", substitution):
            continue
        raise ValueError(f"{path}:{line}: unsupported lit substitution %{substitution}")


def substitute_command(
    command: str,
    source: Path,
    temporary_prefix: Path,
    tool: Path,
    filecheck: Path,
    not_tool: Path | None,
) -> str:
    result = LLC_PATTERN.sub(shell_quote(tool), command)
    result = FILECHECK_PATTERN.sub(shell_quote(filecheck), result)
    if not_tool is not None:
        result = NOT_PATTERN.sub(shell_quote(not_tool), result)
    result = re.sub(
        r"%t([0-9]+)", lambda match: shell_quote(f"{temporary_prefix}{match.group(1)}"), result
    )
    result = result.replace("%t", shell_quote(temporary_prefix))
    return result.replace("%s", shell_quote(source))


def filecheck_pipe(command: str) -> int | None:
    match = FILECHECK_PATTERN.search(command)
    if match is None:
        return None
    filecheck_position = match.start()
    quote = ""
    escaped = False
    pipe_positions: list[int] = []
    for index, character in enumerate(command[:filecheck_position]):
        if escaped:
            escaped = False
            continue
        if character == "\\" and quote != "'":
            escaped = True
            continue
        if quote:
            if character == quote:
                quote = ""
            continue
        if character in {"'", '"'}:
            quote = character
            continue
        if character == "|" and (index + 1 >= len(command) or command[index + 1] != "|"):
            pipe_positions.append(index)
    return pipe_positions[-1] if pipe_positions else None


def run_shell(
    command: str,
    cwd: Path,
    environment: dict[str, str],
    timeout: float,
    standard_input: bytes | None = None,
) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(
        ["/bin/bash", "-o", "pipefail", "-c", command],
        cwd=cwd,
        env=environment,
        input=standard_input,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
        check=False,
    )


def diagnostic_for(process: subprocess.CompletedProcess[bytes], command: str) -> str:
    output = process.stderr.decode("utf-8", errors="replace")
    if not output:
        output = process.stdout.decode("utf-8", errors="replace")
    return f"command: {command}\n{output}"[:8000].rstrip()


def execute_run(
    directive: RunDirective,
    scope: str,
    scope_reason: str,
    test: TestDescription,
    temporary_prefix: Path,
    arguments: argparse.Namespace,
    environment: dict[str, str],
) -> RunResult:
    result = RunResult(directive.line, directive.command, scope, scope_reason)
    if scope != "selected":
        return result
    pipe = filecheck_pipe(directive.command)
    try:
        if pipe is not None:
            production_command = substitute_command(
                directive.command[:pipe].strip(),
                test.path,
                temporary_prefix,
                arguments.tool,
                arguments.filecheck,
                arguments.not_tool,
            )
            filecheck_command = substitute_command(
                directive.command[pipe + 1 :].strip(),
                test.path,
                temporary_prefix,
                arguments.tool,
                arguments.filecheck,
                arguments.not_tool,
            )
            production = run_shell(
                production_command, test.path.parent, environment, arguments.timeout
            )
            if production.returncode != 0:
                result.production = "failed"
                result.filecheck = "skipped"
                result.diagnostic = diagnostic_for(production, production_command)
                return result
            result.production = "passed"
            check = run_shell(
                filecheck_command,
                test.path.parent,
                environment,
                arguments.timeout,
                production.stdout,
            )
            if check.returncode == 0:
                result.filecheck = "passed"
            else:
                result.filecheck = "failed"
                result.diagnostic = diagnostic_for(check, filecheck_command)
            return result

        command = substitute_command(
            directive.command,
            test.path,
            temporary_prefix,
            arguments.tool,
            arguments.filecheck,
            arguments.not_tool,
        )
        if FILECHECK_PATTERN.search(directive.command):
            check = run_shell(command, test.path.parent, environment, arguments.timeout)
            result.filecheck = "passed" if check.returncode == 0 else "failed"
            if check.returncode != 0:
                result.diagnostic = diagnostic_for(check, command)
            return result

        production = run_shell(command, test.path.parent, environment, arguments.timeout)
        result.production = "passed" if production.returncode == 0 else "failed"
        if production.returncode != 0:
            result.diagnostic = diagnostic_for(production, command)
    except subprocess.TimeoutExpired:
        result.production = "failed"
        result.diagnostic = f"command timed out after {arguments.timeout:g} seconds: {command}"
    except OSError as error:
        result.production = "failed"
        result.diagnostic = f"cannot execute command: {error}: {command}"
    return result


def execute_input_gate(
    test: TestDescription, arguments: argparse.Namespace
) -> GateResult:
    if test.category not in IN_SCOPE_CATEGORIES:
        return GateResult("not-applicable")
    command = [
        str(arguments.tool),
        "--verify",
        "-mtriple=riscv64",
        "-mattr=+m,+a,+f,+d,+c",
        "-verify-machineinstrs",
        str(test.path),
    ]
    try:
        process = subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=arguments.timeout,
            check=False,
        )
    except subprocess.TimeoutExpired:
        return GateResult("failed", f"input gate timed out after {arguments.timeout:g} seconds")
    except OSError as error:
        return GateResult("failed", f"cannot execute input gate: {error}")
    if process.returncode == 0:
        return GateResult("passed")
    return GateResult("failed", diagnostic_for(process, shlex.join(command)))


def execute_file(
    test: TestDescription,
    arguments: argparse.Namespace,
    temporary_root: Path,
    environment: dict[str, str],
) -> FileResult:
    gate = GateResult("not-run") if arguments.validate_only else execute_input_gate(test, arguments)
    results: list[RunResult] = []
    inherited_scope: tuple[str, str] | None = None
    temporary_directory = temporary_root / re.sub(r"[^A-Za-z0-9_.-]", "_", test.relative_path)
    temporary_directory.mkdir(parents=True, exist_ok=True)
    temporary_prefix = temporary_directory / "tmp"
    for directive in test.runs:
        validate_substitutions(directive.command, test.path, directive.line)
        scope, reason, has_target_context = command_scope(test, directive.command)
        if not has_target_context and inherited_scope is not None:
            scope, reason = inherited_scope
        elif has_target_context:
            inherited_scope = (scope, reason)
        if arguments.validate_only:
            results.append(RunResult(directive.line, directive.command, scope, reason))
        else:
            results.append(
                execute_run(
                    directive,
                    scope,
                    reason,
                    test,
                    temporary_prefix,
                    arguments,
                    environment,
                )
            )
    return FileResult(test.relative_path, test.category, test.reason, gate, results)


def status_counts(values: Iterable[str]) -> dict[str, int]:
    return dict(sorted(Counter(values).items()))


def exact_file_status(result: FileResult) -> str:
    if result.input_gate.status == "not-run":
        return "not-run"
    if result.input_gate.status == "failed":
        return "failed"
    selected = [run for run in result.runs if run.scope == "selected"]
    if not selected:
        return "no-compatible-run"
    for run in selected:
        if run.production == "failed" or run.filecheck in {"failed", "skipped"}:
            return "failed"
        if run.production == "not-applicable" and run.filecheck != "passed":
            return "failed"
    return "passed"


def semantic_file_status(result: FileResult) -> str:
    if result.input_gate.status == "not-run":
        return "not-run"
    if result.input_gate.status == "failed":
        return "failed"
    selected = [run for run in result.runs if run.scope == "selected"]
    if not selected:
        return "no-compatible-run"
    return "failed" if any(run.production == "failed" for run in selected) else "passed"


def build_summary(results: list[FileResult]) -> dict[str, object]:
    runs = [run for result in results for run in result.runs]
    exact = [result for result in results if result.category == "RV64GC_EXACT"]
    semantic = [result for result in results if result.category == "RV64GC_SEMANTIC"]
    input_only = [result for result in results if result.category == "RV64GC_INPUT"]
    return {
        "files": len(results),
        "runs": len(runs),
        "categories": status_counts(result.category for result in results),
        "input_gate": status_counts(result.input_gate.status for result in results),
        "scope": status_counts(run.scope for run in runs),
        "production": status_counts(run.production for run in runs),
        "filecheck": status_counts(run.filecheck for run in runs),
        "exact_files": status_counts(exact_file_status(result) for result in exact),
        "semantic_files": status_counts(semantic_file_status(result) for result in semantic),
        "input_files": status_counts(result.input_gate.status for result in input_only),
    }


def count(summary: dict[str, object], section: str, status: str) -> int:
    values = summary[section]
    assert isinstance(values, dict)
    value = values.get(status, 0)
    assert isinstance(value, int)
    return value


def print_summary(summary: dict[str, object]) -> None:
    selected = count(summary, "scope", "selected")
    out_of_scope = int(summary["runs"]) - selected
    print(
        f"LLVM RISC-V MIR corpus: files={summary['files']} runs={summary['runs']} "
        f"selected={selected} out-of-scope={out_of_scope}"
    )
    print(
        "input gate: "
        f"passed={count(summary, 'input_gate', 'passed')} "
        f"failed={count(summary, 'input_gate', 'failed')} "
        f"not-run={count(summary, 'input_gate', 'not-run')}"
    )
    print(
        "production pass: "
        f"passed={count(summary, 'production', 'passed')} "
        f"failed={count(summary, 'production', 'failed')}"
    )
    print(
        "FileCheck: "
        f"passed={count(summary, 'filecheck', 'passed')} "
        f"failed={count(summary, 'filecheck', 'failed')} "
        f"skipped={count(summary, 'filecheck', 'skipped')}"
    )
    print(
        "RV64GC_EXACT files: "
        f"passed={count(summary, 'exact_files', 'passed')} "
        f"failed={count(summary, 'exact_files', 'failed')} "
        f"no-compatible-run={count(summary, 'exact_files', 'no-compatible-run')} "
        f"not-run={count(summary, 'exact_files', 'not-run')}"
    )
    print("out-of-scope RUNs: " + json.dumps(summary["scope"], sort_keys=True))


def print_diagnostics(results: list[FileResult], maximum: int) -> None:
    emitted = 0
    for result in results:
        if result.input_gate.status == "failed" and emitted < maximum:
            print(f"FAIL input {result.path}:\n{result.input_gate.diagnostic}", file=sys.stderr)
            emitted += 1
        for run in result.runs:
            if not run.diagnostic or emitted >= maximum:
                continue
            print(f"FAIL {result.path}:{run.line}:\n{run.diagnostic}", file=sys.stderr)
            emitted += 1
    total = sum(
        (1 if result.input_gate.status == "failed" else 0)
        + sum(1 for run in result.runs if run.diagnostic)
        for result in results
    )
    if total > emitted:
        print(f"{total - emitted} additional failures omitted", file=sys.stderr)


def strict_failure(summary: dict[str, object]) -> bool:
    return (
        count(summary, "input_gate", "failed") != 0
        or count(summary, "exact_files", "failed") != 0
        or count(summary, "exact_files", "no-compatible-run") != 0
        or count(summary, "semantic_files", "failed") != 0
        or count(summary, "semantic_files", "no-compatible-run") != 0
    )


def validate_paths(arguments: argparse.Namespace) -> None:
    for description, path in (
        ("RCC MIR tool", arguments.tool),
        ("MIR corpus", arguments.corpus),
        ("classification manifest", arguments.manifest),
        ("FileCheck", arguments.filecheck),
    ):
        if not path.exists():
            raise ValueError(f"{description} does not exist: {path}")
    if arguments.not_tool is not None and not arguments.not_tool.exists():
        raise ValueError(f"not tool does not exist: {arguments.not_tool}")
    if arguments.jobs <= 0:
        raise ValueError("--jobs must be positive")
    if arguments.timeout <= 0:
        raise ValueError("--timeout must be positive")


def main() -> int:
    arguments = parse_arguments()
    try:
        validate_paths(arguments)
        arguments.tool = arguments.tool.resolve()
        arguments.corpus = arguments.corpus.resolve()
        arguments.manifest = arguments.manifest.resolve()
        arguments.filecheck = arguments.filecheck.resolve()
        if arguments.not_tool is not None:
            arguments.not_tool = arguments.not_tool.resolve()
        if arguments.llvm_bin is not None:
            arguments.llvm_bin = arguments.llvm_bin.resolve()
        tests = load_tests(arguments.corpus, arguments.manifest)
        environment = os.environ.copy()
        path_entries = []
        if arguments.llvm_bin is not None:
            path_entries.append(str(arguments.llvm_bin.resolve()))
        path_entries.append(str(arguments.filecheck.resolve().parent))
        if arguments.not_tool is not None:
            path_entries.append(str(arguments.not_tool.resolve().parent))
        environment["PATH"] = os.pathsep.join(path_entries + [environment.get("PATH", "")])
        with tempfile.TemporaryDirectory(prefix="rcc-llvm-mir-") as temporary_directory:
            temporary_root = Path(temporary_directory)
            with ThreadPoolExecutor(max_workers=arguments.jobs) as executor:
                results = list(
                    executor.map(
                        lambda test: execute_file(test, arguments, temporary_root, environment), tests
                    )
                )
        results.sort(key=lambda result: result.path)
        summary = build_summary(results)
        print_summary(summary)
        if not arguments.validate_only:
            print_diagnostics(results, arguments.max_diagnostics)
        if arguments.report is not None:
            arguments.report.parent.mkdir(parents=True, exist_ok=True)
            arguments.report.write_text(
                json.dumps(
                    {
                        "summary": summary,
                        "files": [dataclasses.asdict(result) for result in results],
                    },
                    indent=2,
                    sort_keys=True,
                )
                + "\n",
                encoding="utf-8",
            )
        return 1 if arguments.strict and strict_failure(summary) else 0
    except (OSError, ValueError) as error:
        print(f"LLVMRunTestExecutor.py: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
