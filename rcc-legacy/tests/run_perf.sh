#!/bin/bash
# rcc performance test: compile each .sy with rcc, link via sylib.c, run on qemu,
# record sylib TOTAL: us and wall time. Output TSV.
#
# Compare logic matches run_rv64_e2e.sh: .out = stdout + last line = retcode.
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
TEST_DIR="${1:-$SCRIPT_DIR/performance}"
TIMEOUT=90

WORKDIR=$(mktemp -d /tmp/rcc_perf_XXXXXX)
REPORT="$WORKDIR/perf_report.tsv"
echo -e "case\trcc_us\twall_s\tnote" > "$REPORT"
echo "workdir: $WORKDIR"

parse_total() {
    awk '/TOTAL:/ {
        match($0, /([0-9]+)H-([0-9]+)M-([0-9]+)S-([0-9]+)us/, a)
        printf "%d", (a[1]*3600 + a[2]*60 + a[3]) * 1000000 + a[4]
        exit
    }' "$1"
}

run_one() {
    local sy=$1
    local base="${sy%.sy}"; local name=$(basename "$base")
    local in=/dev/null; [ -f "$base.in" ] && in="$base.in"

    local s="$WORKDIR/$name.s"
    local elf="$WORKDIR/$name.elf"
    if ! timeout 60 "$COMPILER" -S -o "$s" "$sy" 2>"$WORKDIR/$name.cerr"; then
        echo -e "$name\t-\t-\trcc_compile_fail" >> "$REPORT"
        return
    fi
    if ! "$RV_GCC" -static -march=rv64gc -o "$elf" "$s" "$SYLIB" 2>"$WORKDIR/$name.lerr"; then
        echo -e "$name\t-\t-\tlink_fail" >> "$REPORT"
        return
    fi

    local t0 t1
    t0=$(date +%s.%N)
    timeout "$TIMEOUT" "$QEMU" "$elf" <"$in" >"$WORKDIR/$name.out" 2>"$WORKDIR/$name.serr"
    local rc=$?
    t1=$(date +%s.%N)
    local wall=$(awk "BEGIN { printf \"%.3f\", $t1 - $t0 }")

    local us=$(parse_total "$WORKDIR/$name.serr")
    [ -z "$us" ] && us="-"

    local note="ok"
    if [ $rc -eq 124 ]; then
        note="timeout"
    elif [ -f "$base.out" ]; then
        local expected_out=$(head -n -1 "$base.out")
        local expected_ret=$(tail -n 1 "$base.out")
        local actual_out=$(cat "$WORKDIR/$name.out")
        if [ "$actual_out" != "$expected_out" ]; then
            note="WA_stdout"
        elif [ "$rc" != "$expected_ret" ]; then
            note="WA_ret(got=$rc,exp=$expected_ret)"
        fi
    fi

    echo -e "$name\t$us\t$wall\t$note" >> "$REPORT"
    printf "  %-40s %12s us  %8s s  %s\n" "$name" "$us" "$wall" "$note"
}

for sy in $(ls "$TEST_DIR"/*.sy | sort); do
    run_one "$sy"
done

echo ""
echo "report: $REPORT"
