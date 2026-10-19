#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later

"""Run formatting, clang-tidy, or the Clang analyzer for RCC and CFront."""

import argparse
import difflib
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
PROJECTS = ("rcc", "cfront")
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inc"}
HEADER_SUFFIXES = {".h", ".hh", ".hpp", ".hxx", ".inc"}
HUNK = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@", re.MULTILINE)


def git_output(*arguments):
    result = subprocess.run(
        ["git", "-C", str(ROOT), *arguments],
        check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    return result.stdout


def is_source(path, build_dir):
    # The allowlist prevents checks from reaching independent or legacy projects.
    resolved = path.resolve()
    if not any(resolved.is_relative_to(ROOT / project) for project in PROJECTS):
        return False
    if build_dir is not None and resolved.is_relative_to(build_dir.resolve()):
        return False
    relative = resolved.relative_to(ROOT)
    for part in relative.parts[:-1]:
        if part in {"generated", "CMakeFiles", "rcc-legacy"}:
            return False
    return path.suffix in SOURCE_SUFFIXES and path.is_file()


def git_files(*arguments):
    names = git_output(*arguments, "--", *PROJECTS).split(b"\0")
    return {ROOT / name.decode() for name in names if name}


def selected_files(args):
    scopes = [Path(name).resolve() for name in args.paths]
    for scope in scopes:
        if scope != ROOT and not any(scope.is_relative_to(ROOT / p) for p in PROJECTS):
            raise ValueError(f"Selection must be inside rcc/ or cfront/: {scope}")

    whole_files = args.all or (args.paths and not args.changed)
    if whole_files:
        candidates = git_files("ls-files", "--cached", "--others", "--exclude-standard", "-z")
        untracked = set()
    else:
        candidates = git_files(
            "diff", "--no-ext-diff", "--no-renames", "--name-only", "-z", args.base
        )
        untracked = git_files("ls-files", "--others", "--exclude-standard", "-z")
        candidates |= untracked

    selected = {}
    for path in sorted(candidates):
        if not is_source(path, args.build_dir):
            continue
        if scopes and not any(path == scope or path.is_relative_to(scope) for scope in scopes):
            continue
        if whole_files or path in untracked:
            selected[path] = None
            continue
        patch = git_output(
            "diff", "--no-color", "--no-ext-diff", "--no-textconv", "--no-renames", "--unified=0",
            args.base, "--", str(path.relative_to(ROOT)),
        ).decode()
        ranges = []
        for start, count in HUNK.findall(patch):
            length = int(count) if count else 1
            if length == 0:
                continue
            first = int(start)
            ranges.append((first, first + length - 1))
        selected[path] = ranges
    return selected


def check_tool(executable, expected_version, directory=ROOT):
    result = subprocess.run(
        [executable, "--version"], cwd=directory,
        check=True, text=True, stdout=subprocess.PIPE,
    )
    print(result.stdout.strip(), flush=True)
    if expected_version:
        version = re.search(r"\bversion\s+(\d+(?:\.\d+)*)", result.stdout)
        if version is None or version.group(1) != expected_version:
            raise ValueError(f"Expected {executable} version {expected_version}")


def format_sources(args):
    files = selected_files(args)
    if not files:
        print("No matching source files.")
        return 0
    check_tool(args.tool, args.tool_version)
    different = False
    for path, ranges in files.items():
        if ranges == []:
            continue
        command = [args.tool, "--style=file"]
        if ranges is not None:
            command.extend(f"--lines={start}:{end}" for start, end in ranges)
        command.append(str(path))
        original = path.read_text()
        formatted = subprocess.run(
            command, check=True, text=True, stdout=subprocess.PIPE,
        ).stdout
        if original == formatted:
            continue
        different = True
        name = str(path.relative_to(ROOT))
        if args.fix:
            path.write_text(formatted)
            print(f"Formatted {name}")
        else:
            sys.stdout.writelines(difflib.unified_diff(
                original.splitlines(keepends=True), formatted.splitlines(keepends=True),
                fromfile=f"a/{name}", tofile=f"b/{name}",
            ))
    return int(different and not args.fix)


def compilation_entries(args, files):
    database_dir = args.build_dir.resolve()
    with (database_dir / "compile_commands.json").open() as stream:
        database = json.load(stream)
    entries = []
    for entry in database:
        directory = (database_dir / entry["directory"]).resolve()
        source = (directory / entry["file"]).resolve()
        if not is_source(source, args.build_dir):
            continue
        if source in files:
            entries.append((source, directory, entry))
    return entries


def tidy_sources(args):
    files = selected_files(args)
    if not files:
        print("No matching source files.")
        return 0
    if args.all:
        sources = {source for source, _, _ in compilation_entries(args, files)}
    else:
        # Like clang-tidy-diff, pass selected headers directly to clang-tidy.
        # The tool infers their compile commands from the compilation database.
        sources = set(files)
    if not sources:
        raise ValueError("No compilation database entries match the selected sources.")
    check_tool(args.tool, args.tool_version)
    line_filter = []
    if args.line_filter:
        for path, ranges in files.items():
            if ranges == []:
                continue
            item = {"name": str(path)}
            if ranges is not None:
                item["lines"] = ranges
            line_filter.append(item)
        if not line_filter:
            print("No changed lines to report.")
            return 0

    failed = False
    for source in sorted(sources):
        command = [args.tool, str(source), "-p", str(args.build_dir.resolve())]
        if args.line_filter:
            command.append("--line-filter=" + json.dumps(line_filter))
        command.extend(args.extra_arg)
        print(shlex.join(command), flush=True)
        result = subprocess.run(command, cwd=ROOT)
        failed |= result.returncode != 0
    return int(failed)


def compilation_command(entry):
    arguments = entry.get("arguments")
    if arguments is None:
        arguments = shlex.split(entry["command"])
    return arguments


def analyze_sources(args):
    files = selected_files(args)
    if not files:
        print("No matching source files.")
        return 0
    entries = compilation_entries(args, files)
    if not entries:
        raise ValueError(
            "No selected translation units are in the compilation database. "
            "Select source files or use --all to analyze all project translation units."
        )
    if any(path.suffix in HEADER_SUFFIXES for path in files):
        print("Header changes do not expand the selected translation units; use --all for full analysis.")
    reports = (args.output_dir or args.build_dir / "analyzer-reports").resolve()
    failed = False
    check_tool(args.tool, args.tool_version)
    for source, directory, entry in entries:
        arguments = compilation_command(entry)
        driver_index = 1 if Path(arguments[0]).name in {"ccache", "sccache"} else 0
        # Distinguish compilation variants of the same source file.
        identity = json.dumps(entry, sort_keys=True).encode()
        suffix = hashlib.sha256(identity).hexdigest()[:12]
        output = reports / f"{source.stem}-{suffix}"
        output.mkdir(parents=True, exist_ok=True)
        # Run the selected Clang analyzer with the recorded compilation options.
        # Appending -o selects the report directory instead of the object output.
        command = [args.tool, *arguments[driver_index + 1:],
                   "--analyze", "--analyzer-output", "html", "-o", str(output)]
        command.extend(args.extra_arg)
        print(shlex.join(command), flush=True)
        result = subprocess.run(command, cwd=directory)
        failed |= result.returncode != 0
    print(f"Analyzer reports: {reports}")
    return int(failed)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    for name, handler, executable in (
        ("format", format_sources, "clang-format"),
        ("tidy", tidy_sources, "clang-tidy"),
        ("analyze", analyze_sources, "clang++"),
    ):
        command = commands.add_parser(name)
        command.set_defaults(handler=handler)
        command.add_argument("paths", nargs="*", help="Files or module directories; paths select whole files by default")
        mode = command.add_mutually_exclusive_group()
        mode.add_argument("--all", action="store_true", help="Select all tracked and untracked source files")
        mode.add_argument("--changed", action="store_true", help="Select changes since --base, also within explicit paths")
        command.add_argument("--base", default="HEAD", help="Diff base for the working tree (default: HEAD)")
        command.add_argument("--tool", default=executable, help="Tool executable or absolute path")
        command.add_argument("--tool-version", default="", help="Require this exact tool version when provided")
        if name == "format":
            command.add_argument("--build-dir", type=Path, help="Build directory to exclude from source selection")
            command.add_argument("--fix", action="store_true", help="Write formatting changes; otherwise only print a diff")
        else:
            command.add_argument("--build-dir", type=Path, required=True, help="Directory containing compile_commands.json")
            command.add_argument("--extra-arg", action="append", default=[], help="Additional tool option; use --extra-arg=VALUE")
        if name == "tidy":
            command.add_argument("--line-filter", action="store_true", help="Report diagnostics on selected lines; translation units are still fully analyzed")
        if name == "analyze":
            command.description = "Analyze selected translation units with Clang using their recorded compilation options. Use --all for full analysis."
            command.add_argument("--output-dir", type=Path, help="HTML report directory (default: BUILD/analyzer-reports)")

    args = parser.parse_args()
    try:
        return args.handler(args)
    except subprocess.CalledProcessError as error:
        if error.stderr:
            sys.stderr.write(error.stderr.decode())
        print(f"Command failed: {shlex.join(error.cmd)}", file=sys.stderr)
        return 1
    except (OSError, ValueError) as error:
        print(str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
