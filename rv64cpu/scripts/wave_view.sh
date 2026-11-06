#!/usr/bin/env bash
# ─── wave_view.sh ────────────────────────────────────────────────────────────────
# Open a VCD/FST waveform file in GTKWave
#
# Usage: ./scripts/wave_view.sh [waveform_file]
#        ./scripts/wave_view.sh                    # opens latest .vcd in waves/
# ─────────────────────────────────────────────────────────────────────────────────
set -euo pipefail

WAVES_DIR="$(dirname "$0")/../waves"
VIEWER="${WAVE_VIEWER:-gtkwave}"

if [ $# -ge 1 ]; then
    WAVE_FILE="$1"
else
    # Open the most recently modified waveform file
    WAVE_FILE=$(ls -t "$WAVES_DIR"/*.vcd "$WAVES_DIR"/*.fst 2>/dev/null | head -1)
    if [ -z "$WAVE_FILE" ]; then
        echo "No waveform files found in $WAVES_DIR"
        exit 1
    fi
fi

echo "[wave_view] Opening: $WAVE_FILE"
$VIEWER "$WAVE_FILE" &
