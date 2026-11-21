#!/bin/sh
# Cross-breeding (GAME_SPEC features 1-9): unit checks through the game's own code + real screens.
#   build first:  make EVO_TEST=1
#   test/crossbreed/run.sh [OUTDIR]          exit 0 = every check passed; screenshots in OUTDIR
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:-$ROOT/build/crossbreed-test}
rm -rf "$OUT"; mkdir -p "$OUT"
MAILBOX=$(awk '$2 == "gEvoTest" && $1 ~ /^0x/ { print $1; exit }' "$ROOT/pokeemerald_evotest.map")
[ -n "$MAILBOX" ] || { echo "gEvoTest not in the map (make EVO_TEST=1 first)"; exit 2; }
SCRIPT=$(mktemp -t crossbreed).lua
{ echo "MAILBOX = $MAILBOX"; echo "OUT = \"$OUT\""; cat "$ROOT/test/crossbreed/crossbreed.lua"; } > "$SCRIPT"
set +e
"$HOME/Tools/gba/mgba/bin/realgen-mgba" "$ROOT/pokeemerald_evotest.gba" --frames 40000 --script "$SCRIPT" 2>/dev/null | tee "$OUT/rom.log"
set -e
rm -f "$SCRIPT"
grep -q "rom test finished at frame .*: PASS" "$OUT/rom.log" || { echo "crossbreed ROM test did not pass"; exit 1; }
