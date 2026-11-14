#!/bin/bash
# Build nextpnr-himbaechel with spartan6 and place and route the test netlists (tests/expected.txt): no error, every net
# routed, the maximum frequency at or above its floor; the pads of tests/pads.txt on both devices (routed, no warning);
# then the packer's (tests/pack.txt, ci/test-pack.sh). The chip database's inputs come from a Silinx checkout.
#   build-and-test.sh <silinx checkout> [build dir]
set -euo pipefail
silinx=$(cd "$1" && pwd)
build=${2:-build}
uarch=$(cd "$(dirname "$0")/.." && pwd)
data="$build/silinx-data"
mkdir -p "$data"
research="$silinx/packages/silinx-fpga/research/nextpnr-s6"
for dev in xc6slx16 xc6slx45; do
  node "$research/export-unknown-pips.mjs" ${dev}csg324-3 "$data/unknown-pips-$dev.json"
  node "$research/export-timing.mjs" ${dev}csg324-3 "$data/timing-$dev.json"
done
cmake -S . -B "$build" -DARCH=himbaechel -DHIMBAECHEL_UARCH=spartan6 -DBUILD_PYTHON=OFF -DBUILD_GUI=OFF \
  -DCMAKE_BUILD_TYPE=Release -DSILINX_DEVICES="$silinx/packages/silinx-fpga/devices" \
  -DSILINX_UNKNOWN_PIPS="$(cd "$data" && pwd)" -DSILINX_TIMING="$(cd "$data" && pwd)" \
  -DHIMBAECHEL_SPARTAN6_DEVICES="xc6slx16:csg324;xc6slx45:csg324" > "$build/configure.log" 2>&1 || { tail -30 "$build/configure.log"; exit 1; }
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
# the pads whose path the chip database got from Silinx's held PIPs: placed and routed, no warning
while read -r netlist device _; do
  [[ -z "$netlist" || "$netlist" == \#* ]] && continue
  log="$build/${netlist%.json}.log"
  if ! "$build/nextpnr-himbaechel" --device "$device" --json "$uarch/tests/$netlist" --write "$build/${netlist%.json}-routed.json" \
       --freq 50 --seed 1 > "$log" 2>&1; then
    echo "FAIL $netlist: nextpnr failed"; grep -E "ERROR" "$log" | head -5; failed=1; continue
  fi
  if ! grep -q "Program finished normally" "$log"; then echo "FAIL $netlist: not routed"; failed=1; continue; fi
  if grep -q "^Warning:" "$log"; then echo "FAIL $netlist: warnings"; grep "^Warning:" "$log" | head -5; failed=1; continue; fi
  echo "ok   $netlist: routed on $device, no warning"
done < "$uarch/tests/pads.txt"
# a BSCAN_SPARTAN6 of Yosys's netlist on the BEL its attribute names (the site of its JTAG chain, which the chain needs:
# Silinx's flow writes it, pmnmalo/Silinx#3766): packed, placed there, routed, no warning
log="$build/pack_bscan-placed.log"
if ! "$build/nextpnr-himbaechel" --device xc6slx16csg324-3 --json "$uarch/tests/pack_bscan.json" --write "$build/pack_bscan-routed.json" \
     --freq 50 --seed 1 > "$log" 2>&1; then
  echo "FAIL pack_bscan.json: nextpnr failed"; grep -E "ERROR" "$log" | head -5; failed=1
else
  bel=$(python3 -c 'import json, sys; m = list(json.load(open(sys.argv[1]))["modules"].values())[0]; print(" ".join(c["attributes"]["NEXTPNR_BEL"] for c in m["cells"].values() if c["type"] == "BSCAN"))' "$build/pack_bscan-routed.json")
  if [[ "$bel" != "X58Y2/S0_BSCAN" ]]; then echo "FAIL pack_bscan.json: its BSCAN on $bel, not X58Y2/S0_BSCAN"; failed=1
  elif grep -q "^Warning:" "$log"; then echo "FAIL pack_bscan.json: warnings"; grep "^Warning:" "$log" | head -5; failed=1
  else echo "ok   pack_bscan.json: its BSCAN on $bel (its BEL attribute), routed, no warning"; fi
fi
# a product of two counters of Yosys's netlist (pmnmalo/Silinx#3151: the counters' sums to the DSP48A1s by BMUX / CMUX,
# which no 5FF may take): packed, placed and routed, no warning
log="$build/pack_product-routed.log"
if ! "$build/nextpnr-himbaechel" --device xc6slx16csg324-3 --json "$uarch/tests/pack_product.json" --write "$build/pack_product-routed.json" \
     --freq 50 --seed 1 > "$log" 2>&1; then
  echo "FAIL pack_product.json: nextpnr failed"; grep -E "ERROR" "$log" | head -5; failed=1
elif ! grep -q "Program finished normally" "$log"; then echo "FAIL pack_product.json: not routed"; failed=1
elif grep -q "^Warning:" "$log"; then echo "FAIL pack_product.json: warnings"; grep "^Warning:" "$log" | head -5; failed=1
else echo "ok   pack_product.json: packed, placed and routed, no warning"; fi
# the packer and the pads' rules
"$uarch/ci/test-pack.sh" "$build" || failed=1
exit $failed
