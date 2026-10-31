# spartan6: the Xilinx Spartan-6 in nextpnr-himbaechel

A Himbaechel micro-architecture for the Spartan-6 (XC6SLX16, XC6SLX45), developed for
[Silinx ISE](https://github.com/pmnmalo/Silinx-ISE) ([Silinx #2138](https://github.com/pmnmalo/Silinx-ISE/issues/2138)).

## What it does

It places and routes a design of **whole sites**. Silinx packs the netlist into SLICEX / SLICEL / SLICEM, IOB, BUFGMUX,
RAMB16BWER / RAMB8BWER, DSP48A1 and TIEOFF cells (its `src/fpga/s6/pack.js`, `phys.js`, `nextpnr.js`), whose ports are
the sites' pins, and writes their settings itself afterwards (`xdl.js`). The flow, Silinx's `src/open-flow-nextpnr-s6.js`:
1. a placement, with the pads, global buffers, block RAMs and DSP48A1s fixed by Silinx (attribute `BEL`);
2. the placed design written with its constants' sources (TIEOFFs), and routed with `--no-place`.

## The chip database

`gen/s6_gen.py` builds it from Silinx's device graph, with one BEL per site. `--unknown-pips` leaves out the PIPs whose
bits Silinx's bit database does not know; `--timing` gives each PIP the delay of the wire class it drives. A PIP's flags
say what `spartan6.cc` checks:
- a global clock, or a pad into its global buffer, stays off the general interconnect;
- a route-through of the I/O logic or a BUFH is for one net, on a site with no cell;
- COUT -> DMUX is only for that slice's own carry out, when DMUX is free.

## Timing

Each cell brings its own arcs in its `TMG` parameter, from Silinx's model fitted to ISE's `trce`.

## Router

`router2` is the default: it asks `checkPipAvailForNet`, while `router1` would ignore the rules above.

## Build

```
node <silinx>/packages/silinx-fpga/research/nextpnr-s6/export-unknown-pips.mjs xc6slx16csg324-3 <dir>/unknown-pips-xc6slx16.json
node <silinx>/packages/silinx-fpga/research/nextpnr-s6/export-timing.mjs xc6slx16csg324-3 <dir>/timing-xc6slx16.json
cmake -S . -B build -DARCH=himbaechel -DHIMBAECHEL_UARCH=spartan6 -DBUILD_PYTHON=OFF -DBUILD_GUI=OFF \
  -DSILINX_DEVICES=<silinx>/packages/silinx-fpga/devices -DSILINX_UNKNOWN_PIPS=<dir> -DSILINX_TIMING=<dir> \
  -DHIMBAECHEL_SPARTAN6_DEVICES="xc6slx16:csg324"
cmake --build build -j
```

`ci/build-wasm.sh` builds the WebAssembly package, as the Spartan-3E's. A `spartan6-v*` tag publishes it as a release.
