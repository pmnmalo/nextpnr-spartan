#!/bin/bash
# Build nextpnr-himbaechel with xilinx (the xc7a50t's chip database, the xc7a35t's fabric) and place and route the test
# netlists (tests/expected.txt): no error and no warning, the carry chains' feed-through LUTs and the LUT cells as
# expected, the LUTs as Vivado counts them at most 15 % over Vivado's, the maximum frequency at or above its floor.
# With Project X-Ray's tools built (xc7frames2bit, bitread), each FASM also goes to a .bit and back: fasm2frames accepts
# every feature, and the .bit read back (bit2fasm) gives the same frames.
#   build-and-test.sh <prjxray-db> [build dir] [prjxray, built]
set -euo pipefail
db=$(cd "$1" && pwd)
build=${2:-build}
xray=${3:-}
uarch=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$build"
cmake -S . -B "$build" -DARCH=himbaechel -DHIMBAECHEL_UARCH=xilinx -DHIMBAECHEL_XILINX_DEVICES=xc7a50t \
  -DHIMBAECHEL_PRJXRAY_DB="$db" -DHIMBAECHEL_SPLIT=ON -DBUILD_PYTHON=OFF -DBUILD_GUI=OFF -DCMAKE_BUILD_TYPE=Release \
  > "$build/configure.log" 2>&1 || { tail -30 "$build/configure.log"; exit 1; }
cmake --build "$build" -j "$(nproc 2>/dev/null || sysctl -n hw.ncpu)" > "$build/build.log" 2>&1 || { grep -E "error|Error" -A3 "$build/build.log" | head -40; exit 1; }
part=xc7a35tcpg236-1
failed=0
while read -r name seed feeds feeds_di luts vivado floor _native; do
  [[ -z "$name" || "$name" == \#* ]] && continue
  out="$build/$name"
  gunzip -c "$uarch/tests/$name.json.gz" > "$out.json"
  if ! "$build/nextpnr-himbaechel-xilinx" --device $part --chipdb "$build/share/himbaechel/xilinx/chipdb-xc7a50t.bin" \
       --json "$out.json" -o xdc="$uarch/tests/$name.xdc" -o fasm="$out.fasm" --write "$out-routed.json" \
       --router router1 --seed "$seed" > "$out.log" 2>&1; then
    echo "FAIL $name: nextpnr failed"; grep -E "ERROR" "$out.log" | head -5; failed=1; continue
  fi
  got_feeds=$(sed -nE 's/.*Created ([0-9]+) feed-through LUTs from carry chains.*/\1/p' "$out.log")
  got_di=$(sed -nE 's/.*from carry chains \(.*; DI ([0-9]+)\).*/\1/p' "$out.log")
  got_luts=$(sed -nE 's/.*SLICE_LUTX: *([0-9]+)\/.*/\1/p' "$out.log" | tail -1)
  read -r _cells _feedcells sites _all <<< "$(python3 "$uarch/ci/lut-count.py" "$out-routed.json")"
  fmax=$(grep -E "Max frequency for clock" "$out.log" | tail -1 | sed -E 's/.*: ([0-9.]+) MHz.*/\1/')
  warnings=$(grep -c '^Warning' "$out.log" || true)
  line="$name: feed-throughs ${got_feeds:-0} (DI ${got_di:-0}), $got_luts LUT cells, $sites LUTs as Vivado counts them (Vivado $vivado), $fmax MHz, $warnings warnings"
  if ! grep -q "Program finished normally" "$out.log"; then echo "FAIL $name: not routed"; failed=1; continue; fi
  if [[ "${got_feeds:-0}" != "$feeds" || "${got_di:-0}" != "$feeds_di" || "$got_luts" != "$luts" ]]; then
    echo "FAIL $line; expected feed-throughs $feeds (DI $feeds_di), $luts LUT cells"; failed=1; continue
  fi
  if (( sites * 100 > vivado * 115 )); then echo "FAIL $line: more than 15 % over Vivado's"; failed=1; continue; fi
  if awk -v f="$fmax" -v m="$floor" 'BEGIN { exit !(f < m) }'; then echo "FAIL $line: below $floor MHz"; failed=1; continue; fi
  if [[ "$warnings" != 0 ]]; then echo "FAIL $line"; grep '^Warning' "$out.log" | sort | uniq -c | head -5; failed=1; continue; fi
  if [[ -n "$xray" ]]; then
    xdb="$db/artix7"
    if ! python3 "$xray/utils/fasm2frames.py" --part $part --db-root "$xdb" "$out.fasm" "$out.frames" 2> "$out.frames.err"; then
      echo "FAIL $line: fasm2frames: $(grep -v '^  ' "$out.frames.err" | tail -2)"; failed=1; continue
    fi
    "$xray/build/tools/xc7frames2bit" --part_file "$xdb/$part/part.yaml" --part_name $part --frm_file "$out.frames" \
      --output_file "$out.bit" 2> "$out.bit.err" || { echo "FAIL $line: xc7frames2bit"; failed=1; continue; }
    python3 "$xray/utils/bit2fasm.py" --db-root "$xdb" --part $part --bitread "$xray/build/tools/bitread" "$out.bit" \
      > "$out.back.fasm" 2> "$out.back.err" || { echo "FAIL $line: bit2fasm"; failed=1; continue; }
    python3 "$xray/utils/fasm2frames.py" --part $part --db-root "$xdb" "$out.back.fasm" "$out.back.frames" 2>> "$out.back.err" \
      || { echo "FAIL $line: fasm2frames of the read-back"; failed=1; continue; }
    if ! cmp -s "$out.frames" "$out.back.frames"; then echo "FAIL $line: the .bit read back differs"; failed=1; continue; fi
    line="$line, .bit $(wc -c < "$out.bit" | tr -d ' ') B read back identical"
  fi
  echo "ok   $line"
done < "$uarch/tests/expected.txt"
exit $failed
