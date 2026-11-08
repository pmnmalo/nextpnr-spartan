#!/bin/bash
# Spike (Silinx #2305): each FASM in <dir> to frames (fasm2frames) and a .bit (xc7frames2bit), timed, then read back
# (bit2fasm) and compared with the FASM (canonical features: what the .bit holds that the FASM did not set, and back)
#   bitstreams.sh <prjxray> <prjxray-db> <dir>      (<dir>/<name>.<part>.fasm)
set -uo pipefail
xray=$(cd "$1" && pwd); db=$(cd "$2" && pwd)/artix7; dir=$3
here=$(cd "$(dirname "$0")" && pwd)
for fasm in "$dir"/*.fasm; do
  base=${fasm%.fasm}; part=${base##*.}
  t0=$(date +%s.%N)
  python3 "$xray/utils/fasm2frames.py" --part "$part" --db-root "$db" "$fasm" "$base.frames" 2> "$base.frames.err" || { echo "FAIL fasm2frames $(basename "$fasm"): $(tail -3 "$base.frames.err")"; continue; }
  t1=$(date +%s.%N)
  "$xray/build/tools/xc7frames2bit" --part_file "$db/$part/part.yaml" --part_name "$part" --frm_file "$base.frames" --output_file "$base.bit" 2> "$base.bit.err" || { echo "FAIL xc7frames2bit $(basename "$fasm"): $(tail -3 "$base.bit.err")"; continue; }
  t2=$(date +%s.%N)
  python3 "$xray/utils/bit2fasm.py" --db-root "$db" --part "$part" --bitread "$xray/build/tools/bitread" "$base.bit" > "$base.back.fasm" 2> "$base.back.err" || echo "FAIL bit2fasm $(basename "$fasm"): $(tail -3 "$base.back.err")"
  python3 "$here/canon.py" "$fasm" > "$base.canon"; python3 "$here/canon.py" "$base.back.fasm" > "$base.back.canon"
  echo "$(basename "$fasm"): fasm2frames $(echo "$t1 - $t0" | bc) s, xc7frames2bit $(echo "$t2 - $t1" | bc) s, $(stat -c%s "$base.bit") bytes; FASM $(wc -l < "$base.canon") bits; read back: $(comm -23 "$base.canon" "$base.back.canon" | wc -l) missing, $(comm -13 "$base.canon" "$base.back.canon" | wc -l) extra"
  comm -3 "$base.canon" "$base.back.canon" | head -8 | sed 's/^/    /'
done
