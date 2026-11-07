#!/bin/sh
# test/softlock/run.sh SCENARIO [SHOTDIR]   (build first: make EVO_TEST=1 SL_TEST=1)
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
S=$1; OUT=${2:-$ROOT/build/sl-shots}; mkdir -p "$OUT"
ADDR=$(awk '$2 == "gSlTest" && $1 ~ /^0x/ { print $1; exit }' "$ROOT/pokeemerald_sltest.map")
SCRIPT=$(mktemp -t sl).lua
{ echo "MAILBOX = $ADDR"; echo "SCENARIO = $S"; [ -n "${DEBUGSHOTS:-}" ] && echo "DEBUGSHOTS = true"; echo "SHOT = \"$OUT/s$S.png\""; cat "$ROOT/test/softlock/sl.lua"; } > "$SCRIPT"
"$HOME/Tools/gba/mgba/bin/realgen-mgba" "$ROOT/pokeemerald_sltest.gba" --frames 40000 --script "$SCRIPT" 2>/dev/null | grep -E "RESULT|UNIT|ran |settle" || true
rm -f "$SCRIPT"
