# spartan3e: the Xilinx Spartan-3E in nextpnr-himbaechel

A Himbaechel micro-architecture for the Spartan-3E, developed for [Silinx ISE](https://github.com/pmnmalo/Silinx-ISE)
(the plan: [Silinx #2131](https://github.com/pmnmalo/Silinx-ISE/issues/2131)).

## The chip database

`gen/s3e_gen.py` builds it from Silinx's device graph of a part, `packages/silinx-fpga/devices/<part>.json.gz`:
- **Routing:** the tiles, wires, PIPs and nodes, computed by Silinx from ISE 14.7's `xdl -report -pips -all_conns`.
- **Sites:** the BELs inside them, written by hand: two LUT4s and two flip-flops per slice with their multiplexers,
  one pad buffer per IOB, the BUFGMUX.

Two optional inputs come from Silinx's scripts (`packages/silinx-fpga/research/nextpnr-s3e/`):
- `--unknown-pips`: the PIPs whose bits Silinx's bit database does not know, left out (`export-unknown-pips.mjs`);
- `--timing`: Silinx's delay model, fitted to ISE's `trce` (`export-timing.mjs`). Each PIP takes the delay of the
  node it drives; the LUT and flip-flop arcs come from the slice's delays.

## Build

```
node <silinx>/packages/silinx-fpga/research/nextpnr-s3e/export-unknown-pips.mjs xc3s250ecp132-4 <dir>/unknown-pips-xc3s250e.json
node <silinx>/packages/silinx-fpga/research/nextpnr-s3e/export-timing.mjs xc3s250ecp132-4 <dir>/timing-xc3s250e.json
cmake -S . -B build -DARCH=himbaechel -DHIMBAECHEL_UARCH=spartan3e -DBUILD_PYTHON=OFF -DBUILD_GUI=OFF \
  -DSILINX_DEVICES=<silinx>/packages/silinx-fpga/devices -DSILINX_UNKNOWN_PIPS=<dir> -DSILINX_TIMING=<dir> \
  -DHIMBAECHEL_SPARTAN3E_DEVICES="xc3s250e:cp132"
cmake --build build -j
```

## Run

The netlist comes from Yosys, `synth_xilinx -family xc3se -nocarry -nowidelut` for now; the ports' pads are `LOC`
attributes:

```
build/nextpnr-himbaechel --device xc3s250ecp132-4 --json design.json --write routed.json
```

The routed design goes to XDL, and from there to a `.bit`, with Silinx's scripts:
- `nextpnr-to-xdl.mjs` converts it;
- `xdl-sim-check.mjs` checks the XDL against the netlist;
- `xdl-to-bit.mjs` writes the bitstream.

## Tests

`tests/` holds Yosys netlists of test designs. The fork's CI (`.github/workflows/spartan3e.yml`) places and routes
each one, and checks there's no error and the maximum frequency nextpnr reports.
