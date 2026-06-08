#!/usr/bin/env python3
"""Pack rcc source files into a zip for contest submission, stripping comments.

Source code for RCC-Legacy.
Copyright (C) 2026 Richard Qin

This file is part of RCC.

RCC is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 3, or (at your option) any later
version.

RCC is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with RCC; see the file LICENSE.  If not see
<http://www.gnu.org/licenses/>.

Includes:
  - src/**/*.cc, src/**/*.h
  - include/**/*.h

Strips C/C++ comments (// and /* */) while preserving string/char literals.
Does NOT modify any local files; output goes to ./rcc_submission.zip.

Usage:
    python3 pack_submission.py [output.zip]
"""

import re
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PATTERNS = ("src/**/*.cc", "src/**/*.h", "include/**/*.h")


def strip_comments(src: str) -> str:
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]

        if c == '"':
            out.append(c)
            i += 1
            while i < n:
                ch = src[i]
                out.append(ch)
                i += 1
                if ch == "\\" and i < n:
                    out.append(src[i])
                    i += 1
                elif ch == '"':
                    break
            continue

        if c == "'":
            out.append(c)
            i += 1
            while i < n:
                ch = src[i]
                out.append(ch)
                i += 1
                if ch == "\\" and i < n:
                    out.append(src[i])
                    i += 1
                elif ch == "'":
                    break
            continue

        if c == "/" and i + 1 < n and src[i + 1] == "/":
            while i < n and src[i] != "\n":
                i += 1
            continue

        if c == "/" and i + 1 < n and src[i + 1] == "*":
            i += 2
            while i + 1 < n and not (src[i] == "*" and src[i + 1] == "/"):
                i += 1
            i += 2
            continue

        out.append(c)
        i += 1
    return "".join(out)


def collapse_blank_lines(src: str) -> str:
    src = re.sub(r"[ \t]+\n", "\n", src)
    src = re.sub(r"\n{3,}", "\n\n", src)
    return src


_INCLUDE_RCC_RE = re.compile(r'(#\s*include\s*")rcc/([^"]+")')


def rewrite_includes(src: str) -> str:
    return _INCLUDE_RCC_RE.sub(r"\1\2", src)


def main() -> int:
    out_name = sys.argv[1] if len(sys.argv) > 1 else "rcc_submission.zip"
    out_path = (ROOT / out_name).resolve()

    if out_path.suffix != ".zip":
        print(f"error: refuse to write non-.zip path: {out_path}", file=sys.stderr)
        return 2
    for guarded in ("src", "include"):
        gpath = (ROOT / guarded).resolve()
        try:
            out_path.relative_to(gpath)
        except ValueError:
            continue
        print(f"error: refuse to write inside source tree: {out_path}", file=sys.stderr)
        return 2

    files: list[Path] = []
    for pat in PATTERNS:
        files.extend(ROOT.glob(pat))
    files = sorted(set(files))
    if not files:
        print("error: no source files found", file=sys.stderr)
        return 1

    if out_path.exists():
        if not out_path.is_file():
            print(f"error: output is not a file: {out_path}", file=sys.stderr)
            return 2
        out_path.unlink()

    total_in = total_out = 0
    with zipfile.ZipFile(out_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for f in files:
            rel = f.relative_to(ROOT).as_posix()
            txt = f.read_text(encoding="utf-8", errors="replace")
            stripped = rewrite_includes(collapse_blank_lines(strip_comments(txt)))
            zf.writestr(rel, stripped)
            total_in += len(txt)
            total_out += len(stripped)
            print(f"  {rel:50s} {len(txt):>8d} -> {len(stripped):>8d}")

    print(
        f"\n{len(files)} files, {total_in} -> {total_out} bytes "
        f"({100.0 * total_out / total_in:.1f}%), zip = {out_path.stat().st_size} bytes"
    )
    print(f"output: {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
