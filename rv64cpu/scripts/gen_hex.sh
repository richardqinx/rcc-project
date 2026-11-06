#!/usr/bin/env bash
# ─── gen_hex.sh ──────────────────────────────────────────────────────────────────
# Convert an ELF file to Verilog hex format for $readmemh
#
# Usage: ./scripts/gen_hex.sh <input.elf> [output.hex]
# ─────────────────────────────────────────────────────────────────────────────────
set -euo pipefail

CROSS_PREFIX="${CROSS_PREFIX:-riscv64-unknown-elf-}"

if [ $# -lt 1 ]; then
    echo "Usage: $0 <input.elf> [output.hex]"
    exit 1
fi

INPUT="$1"
OUTPUT="${2:-${INPUT%.elf}.hex}"

${CROSS_PREFIX}objcopy -O verilog "$INPUT" "$OUTPUT"
echo "[gen_hex] $INPUT → $OUTPUT"

# Also generate .bin and disassembly for inspection
${CROSS_PREFIX}objcopy -O binary "$INPUT" "${INPUT%.elf}.bin"
${CROSS_PREFIX}objdump -d -M no-aliases "$INPUT" > "${INPUT%.elf}.dump"
echo "[gen_hex] Also generated .bin and .dump"
