#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# GABBERKIX's tests (in WSL or on Linux):
#   - the offline render (tools/kick_render.c): the kick's pitch, at the root and an octave either way
#   - the midbody chain (tools/chain_test.c): the curves against the sound, the extremes, the distortion
#   - the host simulator runs every script in app/tests/; a script fails on a FAIL line or a non-zero exit
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
OUT="$ROOT/build/tests"
mkdir -p "$OUT"
fails=0
sh tools/build_render.sh
check() {   # name, then kick_render's arguments
    name="$1"; shift
    if build/kick_render "$OUT/$name.wav" "$@" >"$OUT/$name.log" 2>&1; then echo "ok    $name"; else
        echo "FAIL  $name"; tail -2 "$OUT/$name.log"; fails=$((fails + 1)); fi
}
check render-root --expect-hz 49.00
check render-up 12 --expect-hz 98.00
check render-down -12 --expect-hz 24.50
check render-tune 0 TUNE=5 --expect-hz 65.41
check render-gabber 0 GAIN3=18 Q3=400 DRIVE=12 CUTOFF=900 RESO=60 D.TYPE=GABBER D.DRV=30 LEVEL=-6
if build/chain_test >"$OUT/chain.log" 2>&1; then echo "ok    chain"; else
    echo "FAIL  chain"; grep FAIL "$OUT/chain.log"; fails=$((fails + 1)); fi
sh build.sh --host >"$OUT/host-build.log" 2>&1 || { cat "$OUT/host-build.log"; exit 1; }
for t in app/tests/*.txt; do
    name="$(basename "$t" .txt)"
    mkdir -p "$OUT/$name"
    if app/build/host/gabberkix_host "$t" "$OUT/$name" >"$OUT/$name.log" 2>&1 && ! grep -q '^ *FAIL' "$OUT/$name.log"; then
        echo "ok    $name"
    else
        echo "FAIL  $name"
        grep '^ *FAIL' "$OUT/$name.log" || tail -5 "$OUT/$name.log"
        fails=$((fails + 1))
    fi
done
[ "$fails" = 0 ] || { echo "$fails failed"; exit 1; }
echo "all passed"
