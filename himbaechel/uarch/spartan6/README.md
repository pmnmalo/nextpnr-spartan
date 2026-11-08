# spartan6: the Xilinx Spartan-6 in nextpnr-himbaechel

A Himbaechel micro-architecture for the Spartan-6 (XC6SLX16, XC6SLX45), developed for
[Silinx ISE](https://github.com/pmnmalo/Silinx-ISE) ([Silinx #2138](https://github.com/pmnmalo/Silinx-ISE/issues/2138)).

## What it does

It places and routes a design of **whole sites**. Silinx packs the netlist into SLICEX / SLICEL / SLICEM, IOB, BUFGMUX,
RAMB16BWER / RAMB8BWER, DSP48A1 and TIEOFF cells (its `src/fpga/s6/pack.js`, `phys.js`, `nextpnr.js`), whose ports are
the sites' pins, and writes their settings itself afterwards (`xdl.js`). The flow, Silinx's `src/open-flow-nextpnr-s6.js`:
1. a placement, with the pads and global buffers fixed by Silinx (attribute `BEL`), and the block RAMs and DSP48A1s
   until Silinx leaves them to nextpnr (#3137);
2. the placed design written with its constants' sources (TIEOFFs), and routed with `--no-place`.

## The chip database

`gen/s6_gen.py` builds it from Silinx's device graph, with one BEL per site. `--unknown-pips` leaves out the PIPs whose
bits Silinx's bit database does not know; `--timing` gives each routing node its delay as Silinx's timing analysis counts
it (the node's class and wire type, a load's node the model's base and its pin kind's), which nextpnr adds to the PIP
driving the node, so a path's delay is Silinx's (a timing file without node classes: each PIP the delay of the wire class
it drives). A PIP's flags
say what `spartan6.cc` checks:
- a global clock, or a pad into its global buffer, stays off the general interconnect;
- a route-through of the I/O logic or a BUFH is for one net, on a site with no cell;
- COUT -> DMUX is only for that slice's own carry out, when DMUX is free.

## Timing

Each cell brings its own arcs in its `TMG` parameter, from Silinx's model fitted to ISE's `trce`.

## Placer

HeAP, then an annealing refinement whose moves reach 40 tiles (a block RAM or DSP48A1 is a sparse column away from the
next one), at temperature 0.01, 60 passes, criticality exponent 4 (`-o refine-temp=…`, `refine-diameter`,
`refine-passes`, `refine-critexp`). With it nextpnr places the block RAMs and DSP48A1s itself (Silinx #3137). A slice
cell goes only on a slice its type fits (`isBelLocationValid`: HeAP's legaliser tries the bucket SLICE's BELs with it).

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
