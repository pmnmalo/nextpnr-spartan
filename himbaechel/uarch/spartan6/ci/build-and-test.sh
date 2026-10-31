#!/bin/bash
# Build nextpnr-himbaechel with spartan6 and place and route the test netlists (tests/expected.txt): no error, every net
# routed, the maximum frequency at or above its floor. The chip database's inputs come from a Silinx checkout.
#   build-and-test.sh <silinx checkout> [build dir]
set -euo pipefail
silinx=$(cd "$1" && pwd)
build=${2:-build}
uarch=$(cd "$(dirname "$0")/.." && pwd)
data="$build/silinx-data"
mkdir -p "$data"
research="$silinx/packages/silinx-fpga/research/nextpnr-s6"
node "$research/export-unknown-pips.mjs" xc6slx16csg324-3 "$data/unknown-pips-xc6slx16.json"
node "$research/export-timing.mjs" xc6slx16csg324-3 "$data/timing-xc6slx16.json"
cmake -S . -B "$build" -DARCH=himbaechel -DHIMBAECHEL_UARCH=spartan6 -DBUILD_PYTHON=OFF -DBUILD_GUI=OFF \
  -DCMAKE_BUILD_TYPE=Release -DSILINX_DEVICES="$silinx/packages/silinx-fpga/devices" \
  -DSILINX_UNKNOWN_PIPS="$(cd "$data" && pwd)" -DSILINX_TIMING="$(cd "$data" && pwd)" \
  -DHIMBAECHEL_SPARTAN6_DEVICES="xc6slx16:csg324" > "$build/configure.log" 2>&1 || { tail -30 "$build/configure.log"; exit 1; }
cmake --build "$build" -j "$(nproc 2>/dev/null || sysctl -n hw.ncpu)" > "$build/build.log" 2>&1 || { grep -E "error|Error" -A3 "$build/build.log" | head -40; exit 1; }
failed=0
while read -r netlist seed floor _native extra; do
  [[ -z "$netlist" || "$netlist" == \#* ]] && continue
  log="$build/${netlist%.json}.log"
  if ! "$build/nextpnr-himbaechel" --device xc6slx16csg324-3 --json "$uarch/tests/$netlist" --write "$build/${netlist%.json}-routed.json" \
       --freq 50 --seed "$seed" $extra > "$log" 2>&1; then
    echo "FAIL $netlist: nextpnr failed"; grep -E "ERROR" "$log" | head -5; failed=1; continue
  fi
  fmax=$(grep -E "Max frequency for clock" "$log" | tail -1 | sed -E 's/.*: ([0-9.]+) MHz.*/\1/')
  if ! grep -q "Program finished normally" "$log"; then echo "FAIL $netlist: not routed"; failed=1; continue; fi
  if awk -v f="$fmax" -v m="$floor" 'BEGIN { exit !(f < m) }'; then echo "FAIL $netlist: $fmax MHz, below $floor MHz"; failed=1; continue; fi
  echo "ok   $netlist: $fmax MHz (floor $floor MHz)"
done < "$uarch/tests/expected.txt"
exit $failed
