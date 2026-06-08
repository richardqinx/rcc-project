#!/bin/bash
# rcc end-to-end test: rcc backend → RV64 assembly → gcc → qemu
#
#   .sy --rcc -S--> .s --gcc--> ELF --qemu--> stdout/retcode
#
# Usage: ./tests/run_rv64_e2e.sh [test-dir]
# Default: functional
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
RV_GCC="/opt/riscv64-glibc/bin/riscv64-unknown-linux-gnu-gcc"
QEMU="/opt/riscv64-glibc/bin/qemu-riscv64"
TEST_DIR="${1:-$SCRIPT_DIR/functional}"
TIMEOUT=120  # seconds per test case

WORKDIR=$(mktemp -d /tmp/rcc_rv64_e2e_XXXXXX)
trap 'rm -rf "$WORKDIR"' EXIT

# Colors
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[0;33m'; NC='\033[0m'

PASS=0; FAIL=0; ERROR=0; TOTAL=0; FAILURES=""

for sy in $(find "$TEST_DIR" -name '*.sy' | sort); do
    base="${sy%.sy}"; name=$(basename "$base")
    [ -f "$base.out" ] || continue
    TOTAL=$((TOTAL + 1))
    asm="$WORKDIR/$name.s"; elf="$WORKDIR/$name.elf"
    in=/dev/null; [ -f "$base.in" ] && in="$base.in"

    # 1. Compile .sy -> .s
    if ! timeout "$TIMEOUT" "$COMPILER" -S -o "$asm" "$sy" 2>"$WORKDIR/$name.err"; then
        ERROR=$((ERROR + 1))
        FAILURES="$FAILURES  ERROR(rcc): $name\n"
        echo -e "  ${YELLOW}[ERROR]${NC} $name  (rcc compile failed)"
        sed 's/^/         /' "$WORKDIR/$name.err"
        continue
    fi

    # 2. Assemble+link .s -> ELF
    if ! timeout "$TIMEOUT" "$RV_GCC" -static -march=rv64gc -o "$elf" "$asm" "$SYLIB" 2>"$WORKDIR/$name.gcc.err"; then
        ERROR=$((ERROR + 1))
        FAILURES="$FAILURES  ERROR(gcc): $name\n"
        echo -e "  ${YELLOW}[ERROR]${NC} $name  (gcc link failed)"
        sed 's/^/         /' "$WORKDIR/$name.gcc.err"
        continue
    fi

    # 3. Run under qemu with timeout
    actual_out=$(timeout "$TIMEOUT" "$QEMU" "$elf" < "$in" 2>/dev/null)
    actual_ret=$?

    # timeout returns 124 on timeout
    if [ "$actual_ret" -eq 124 ]; then
        ERROR=$((ERROR + 1))
        FAILURES="$FAILURES  ERROR(timeout): $name\n"
        echo -e "  ${YELLOW}[ERROR]${NC} $name  (qemu timeout ${TIMEOUT}s)"
        continue
    fi

    expected=$(cat "$base.out")
    expected_out=$(echo "$expected" | head -n -1)
    expected_ret=$(echo "$expected" | tail -n 1)

    if [ "$actual_out" = "$expected_out" ] && [ "$actual_ret" = "$expected_ret" ]; then
        PASS=$((PASS + 1))
        echo -e "  ${GREEN}[PASS]${NC}  $name"
    else
        FAIL=$((FAIL + 1))
        reason=""
        [ "$actual_ret" != "$expected_ret" ] && reason="ret: expected=$expected_ret got=$actual_ret"
        [ "$actual_out" != "$expected_out" ] && reason="${reason}${reason:+, }stdout mismatch"
        echo -e "  ${RED}[FAIL]${NC}  $name  ($reason)"
        if [ "$actual_out" != "$expected_out" ]; then
            echo "$expected_out" > "$WORKDIR/$name.expected"
            echo "$actual_out"   > "$WORKDIR/$name.actual"
            diff -u "$WORKDIR/$name.expected" "$WORKDIR/$name.actual" | head -20 | sed 's/^/         /'
        fi
        FAILURES="$FAILURES  FAIL: $name  $reason\n"
    fi
done

echo ""
echo "========================================"
echo -e "  TOTAL=$TOTAL  ${GREEN}PASS=$PASS${NC}  ${RED}FAIL=$FAIL${NC}  ${YELLOW}ERROR=$ERROR${NC}"
echo "========================================"
[ -n "$FAILURES" ] && echo -e "\nDetails:\n$FAILURES"
