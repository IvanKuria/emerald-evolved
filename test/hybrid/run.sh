#!/bin/sh
# Cross-breed hybrid sprites: render them in-game and diff against the owner's reference PNGs.
#   build first:  make EVO_TEST=1
#   test/hybrid/run.sh [REFDIR] [OUTDIR]
# REFDIR defaults to the canonical modern-art references (modern_hybrids.py output). Exit 0 = every pixel matches.
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
REF=${1:-$HOME/deathnote-lifespan/video/public/mc-chem-v2/crossbreed/sprites_modern}
OUT=${2:-$ROOT/build/hybrid-test}
PY=${PYTHON:-$HOME/.venvs/arg/bin/python}
rm -rf "$OUT"; mkdir -p "$OUT"
MAP="$ROOT/pokeemerald_evotest.map"
MAILBOX=$(awk '$2 == "gEvoTest" && $1 ~ /^0x/ { print $1; exit }' "$MAP")
DUMP=$(awk '$2 == "gHybridDump" && $1 ~ /^0x/ { print $1; exit }' "$MAP")
[ -n "$MAILBOX" ] && [ -n "$DUMP" ] || { echo "gEvoTest / gHybridDump not in $MAP (make EVO_TEST=1 first)"; exit 2; }
SCRIPT=$(mktemp -t hybrid).lua
{ echo "MAILBOX = $MAILBOX"; echo "DUMP = $DUMP"; echo "OUT = \"$OUT\""; cat "$ROOT/test/hybrid/hybrid.lua"; } > "$SCRIPT"
set +e
"$HOME/Tools/gba/mgba/bin/realgen-mgba" "$ROOT/pokeemerald_evotest.gba" --frames 20000 --script "$SCRIPT" 2>/dev/null | tee "$OUT/rom.log"
set -e
rm -f "$SCRIPT"
grep -q "rom test finished at frame .*: PASS" "$OUT/rom.log" || { echo "ROM test did not pass"; exit 1; }
"$PY" "$ROOT/test/hybrid/diff.py" "$OUT" "$REF"
