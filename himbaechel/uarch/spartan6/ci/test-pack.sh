#!/bin/bash
# The packer and the pads' rules on the netlists of tests/pack.txt: each netlist's Info: Packed, Warning: and ERROR:
# lines exactly those listed (an ERROR: nextpnr stops on it). For rule_banks.json, each bank's I/Os of one VCCO.
#   test-pack.sh [build dir]
set -uo pipefail
build=${1:-build}
uarch=$(cd "$(dirname "$0")/.." && pwd)
failed=0
for netlist in $(grep -v '^#' "$uarch/tests/pack.txt" | awk 'NF { print $1 }' | uniq); do
  mode=$(awk -v n="$netlist" '$1 == n { print $2; exit }' "$uarch/tests/pack.txt")
  want=$(awk -v n="$netlist" '$1 == n { $1 = ""; $2 = ""; sub(/^ +/, ""); print }' "$uarch/tests/pack.txt")
  log="$build/${netlist%.json}.log"
  [[ $mode == pack ]] && args=--pack-only || args=--no-route
  "$build/nextpnr-himbaechel" --device xc6slx16csg324-3 --json "$uarch/tests/$netlist" $args --write "$build/${netlist%.json}-out.json" > "$log" 2>&1
  status=$?
  got=$(grep -E '^(Info: Packed|Warning:|ERROR:)' "$log")
  if [[ "$got" != "$want" ]]; then
    echo "FAIL $netlist:"; diff <(echo "$want") <(echo "$got") | head -10; failed=1; continue
  fi
  if grep -q '^ERROR:' <<< "$want"; then [[ $status -ne 0 ]] || { echo "FAIL $netlist: no error"; failed=1; continue; }
  elif [[ $status -ne 0 ]]; then echo "FAIL $netlist: nextpnr failed"; failed=1; continue; fi
  echo "ok   $netlist"
done
# (the I/Os without a LOC: each bank's of one VCCO; the banks from nextpnr's list of the pads, --verbose)
"$build/nextpnr-himbaechel" --device xc6slx16csg324-3 --json "$uarch/tests/rule_banks.json" --no-route --verbose \
  --write "$build/rule_banks-out.json" > "$build/rule_banks-verbose.log" 2>&1
python3 - "$build/rule_banks-out.json" "$build/rule_banks-verbose.log" <<'PY' || failed=1
import json, re, sys
bank = {m[2]: m[3] for m in re.finditer(r"Info: pad (\S+) \((\S+)\): bank (-?\d+)", open(sys.argv[2]).read())}
vcco = {"LVCMOS33": 33, "LVCMOS25": 25, "LVCMOS18": 18, "LVCMOS15": 15}
by = {}
for c in list(json.load(open(sys.argv[1]))["modules"].values())[0]["cells"].values():
    if c["type"] == "IOB":
        by.setdefault(bank[c["attributes"]["NEXTPNR_BEL"]], set()).add(vcco[c["attributes"].get("IOSTANDARD", "LVCMOS25")])
bad = {b: v for b, v in by.items() if len(v) > 1}
print("FAIL rule_banks.json: banks of several VCCO: %s" % bad if bad else "ok   rule_banks.json: each bank's I/Os of one VCCO (%s)" % sorted((b, sorted(v)) for b, v in by.items()))
sys.exit(1 if bad else 0)
PY
exit $failed
