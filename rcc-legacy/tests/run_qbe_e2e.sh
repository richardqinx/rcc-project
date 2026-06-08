#!/bin/bash
# QBE end-to-end verification script: Temporary pipeline, used after phase 1 to verify the correctness of the rcc frontend IR
#
#   .sy --rcc--> .ssa --qbe--> .s --gcc--> ELF --qemu--> stdout/retcode
#
# Usage: ./tests/run_qbe_e2e.sh [test-dir]
# Defaults to traversing functional.
#
# Source code for RCC-Legacy.
# Copyright (C) 2026 Richard Qin
#
# This file is part of RCC.
#
# RCC is free software; you can redistribute it and/or modify it under
# the terms of the GNU General Public License as published by the Free
# Software Foundation; either version 3, or (at your option) any later
# version.
#
# RCC is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
# for more details.
#
# You should have received a copy of the GNU General Public License
# along with RCC; see the file LICENSE.  If not see
# <http://www.gnu.org/licenses/>.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
COMPILER="$PROJECT_DIR/../build-rcc-legacy/rcc"
SYLIB="$SCRIPT_DIR/runtime/sylib.c"
QBE="$SCRIPT_DIR/runtime/qbe"
RV_GCC="/opt/riscv64-glibc/bin/riscv64-unknown-linux-gnu-gcc"
QEMU="/opt/riscv64-glibc/bin/qemu-riscv64"

TEST_DIR="${1:-$SCRIPT_DIR/functional}"
WORKDIR=$(mktemp -d /tmp/rcc_qbe_e2e_XXXXXX)
trap 'rm -rf "$WORKDIR"' EXIT

PASS=0
FAIL=0
ERROR=0
TOTAL=0
FAILURES=""

for sy in $(find "$TEST_DIR" -name '*.sy' | sort); do
    base="${sy%.sy}"
    name=$(basename "$base")
    [ -f "$base.out" ] || continue
    TOTAL=$((TOTAL + 1))

    ssa="$WORKDIR/$name.ssa"
    asm="$WORKDIR/$name.s"
    elf="$WORKDIR/$name.elf"

    # 1) rcc frontend -> SSA text
    if ! "$COMPILER" "$sy" --emit-il -o "$ssa" 2>"$WORKDIR/$name.err"; then
        ERROR=$((ERROR + 1))
        FAILURES="$FAILURES  ERROR (rcc): $name\n$(sed 's/^/    /' "$WORKDIR/$name.err")\n"
        continue
    fi

    # 2) QBE -> RV64 assembly
    if ! "$QBE" -t rv64 "$ssa" -o "$asm" 2>"$WORKDIR/$name.qbe.err"; then
        ERROR=$((ERROR + 1))
        FAILURES="$FAILURES  ERROR (qbe): $name\n$(sed 's/^/    /' "$WORKDIR/$name.qbe.err")\n"
        continue
    fi

    # 3) Link sylib
    if ! "$RV_GCC" "$asm" "$SYLIB" -o "$elf" -static -lm 2>"$WORKDIR/$name.gcc.err"; then
        ERROR=$((ERROR + 1))
        FAILURES="$FAILURES  ERROR (link): $name\n$(sed 's/^/    /' "$WORKDIR/$name.gcc.err")\n"
        continue
    fi

    # 4) Run
    in_file="$base.in"
    if [ -f "$in_file" ]; then
        actual=$("$QEMU" "$elf" <"$in_file" 2>/dev/null)
    else
        actual=$("$QEMU" "$elf" 2>/dev/null)
    fi
    rc=$?
    actual=$(echo "$actual" | grep -v '^TOTAL:' | grep -v '^Timer#')

    expected=$(cat "$base.out")
    exp_rc=$(echo "$expected" | tail -1)
    exp_stdout=$(echo "$expected" | head -n -1)

    rc_mod=$((rc % 256))
    exp_rc_mod=$((exp_rc % 256))

    if [ "$actual" = "$exp_stdout" ] && [ "$rc_mod" = "$exp_rc_mod" ]; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        FAILURES="$FAILURES  FAIL: $name (rc=$rc_mod exp=$exp_rc_mod)\n"
        if [ "$actual" != "$exp_stdout" ]; then
            FAILURES="$FAILURES    stdout exp: $(echo "$exp_stdout" | head -3 | tr '\n' '|')\n"
            FAILURES="$FAILURES    stdout act: $(echo "$actual" | head -3 | tr '\n' '|')\n"
        fi
    fi
done

echo "======================================="
echo "  QBE-backend E2E (rcc-frontend only)"
echo "======================================="
echo "  PASS:  $PASS"
echo "  FAIL:  $FAIL"
echo "  ERROR: $ERROR"
echo "  TOTAL: $TOTAL"
echo "======================================="
if [ $FAIL -gt 0 ] || [ $ERROR -gt 0 ]; then
    echo
    echo "Details:"
    echo -e "$FAILURES"
    exit 1
fi
