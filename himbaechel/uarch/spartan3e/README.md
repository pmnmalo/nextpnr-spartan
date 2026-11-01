# spartan3e: the Xilinx Spartan-3E in nextpnr-himbaechel

A Himbaechel micro-architecture for the Spartan-3E, developed for [Silinx ISE](https://github.com/pmnmalo/Silinx-ISE)
(the plan: [Silinx #2131](https://github.com/pmnmalo/Silinx-ISE/issues/2131)).

## The chip database

`gen/s3e_gen.py` builds it from Silinx's device graph of a part, `packages/silinx-fpga/devices/<part>.json.gz`:
- **Routing:** the tiles, wires, PIPs and nodes, computed by Silinx from ISE 14.7's `xdl -report -pips -all_conns`.
- **Sites:** the BELs inside them, written by hand: per slice two LUT4s, two flip-flops, the carry logic (two MUXCYs,
  two XORCYs), the F5 multiplexer and the FXMUX: an F6 on the lower slices of a CLB, the F7 on X0Y1, the F8 on X1Y1; on a SLICEM the LUTs as memories (MEM16F, MEM16G: RAM16X1S, RAM16X1D, RAM32X1S, SRL16*, SRLC16* through Q15); per IOB the pad buffer and its registers (IFF1 / IFF2, OFF1 / OFF2, TFF1, and the
  multiplexers between them and the pad), the BUFGMUX, the block RAM (RAMB16), the multiplier (MULT18X18SIO) and the DCM,
  one BEL each with the site's pins (not the multiplier's cascade yet); a constant 1 on each VCC site (VCCSRC) and a
  constant 0 on Y of each SLICEL used for nothing else (GNDSRC), for the block RAMs' and multipliers' constant pins:
  XDL's `vcc` and `gnd` nets, which ISE takes for constants.

Slices are packed as Silinx's packer does (`fpga/pack-luts.js`): the free places of a slice filled with unrelated LUTs and
flip-flops, the critical connections first (a criticality computed from the cells alone, before placement), unless that
costs more than 2 % more slices. A carry stage or an F5 reads a copy of a shared LUT (or the LUT itself, when its other
loads can leave by X / Y), not a LUT passing it through, a level of logic less; a stage whose select is the constant 1 is
left out; a carry out read by logic leaves by XB / YB. A flip-flop alone (its D through BX / BY) goes first into a free
flip-flop place of a chain's slices, of its nets if it can, when BX / BY is free or carries the same net and the control
set suits; a chain's carry in at 1 comes from the CLB's VCC site, a stage's constant select from a LUT giving it. A global buffer's clock pins are routed on the global lines before
everything else: on general routing they would be skewed, and ISE's DRC does not tell.

The placement is HeAP's, then a timing-driven annealing (placer1's refinement from a warm temperature, in a window of 30
tiles, 30 passes a temperature) that moves the carry chains and slices whole. The placer's delays are those of Silinx's
routes: from a LUT's output and from a flip-flop's (through an OMUX) apart, by the distance, plus a net's load (about 60
ps a doubling of its fanout). On the examples, periods 2 % shorter than HeAP's greedy refinement, for twice to five
times its time.

The router is router 1, its arcs by their criticality first (`router1/crit_first`, on by default here: a CLB's direct
connections, from X / Y to some pins of its LUTs, are the fastest, and a critical short arc routed last finds them
taken), its estimate of the way left the least a route takes (0.2 ns and 65 ps a tile). With the carry's delays below,
on the examples: periods 1.4 % shorter on average than v0.11's, the routing 30 % faster.

The packer puts a flip-flop into its pad's IOB when asked (the attribute `IOB` on it or on the pad's buffer, which
Silinx writes from the UCF's `IOB = TRUE`), as ISE's map does; an IDDR2 / ODDR2 is always there, as two flip-flops (the
second on the falling edge when its clock is the first's inverted). A global buffer driven by a clock pad is placed
where the pad's dedicated path reaches it, and the router takes that path. A DCM goes on the site where the most of
its clock outputs reach global buffers of their own by dedicated paths, then where a pad's dedicated path reaches its
CLKIN (the UCF's `INST LOC`, which Silinx gives as the attribute `BEL`, is kept); those buffers go there with it.

The flip-flops of the falling edge (`FD*_1`) and the latches (`LD`, `LDE`, `LDCE`, `LDPE` and their `_1`) take the
slice's `CLKINV` as Silinx's packer writes it (a latch with an active-high gate: `CLK_B`); a slice's two flip-flops share
it and the latch mode, and a latch stays out of the IOBs. A clock switch (`BUFGMUX`, `BUFGMUX_1`) keeps its `I1` and `S`,
both its pads on their dedicated paths when one buffer has them; the two buffers of a pair share their input lines, so a
switch and a buffer on its partner go together only when the partner's `I0` is the switch's `I1` (both ways), as
Silinx's placer checks (Silinx #846, #2457).

Two optional inputs come from Silinx's scripts (`packages/silinx-fpga/research/nextpnr-s3e/`):
- `--unknown-pips`: the PIPs whose bits Silinx's bit database does not know, left out (`export-unknown-pips.mjs`);
- `--timing`: Silinx's delay model, fitted to ISE's `trce` (`export-timing.mjs`). Each PIP takes the delay of the
  node it drives; the LUT and flip-flop arcs come from the slice's delays. The carry's are its side's (MUXCYF / MUXCYG,
  XORCYF / XORCYG, the variant each stage's cell gets when the chain is packed) and XB's own PIP: Silinx's arcs run
  from the slice's pins to COUT, X, Y and XB, nextpnr's through the F stage's carry, so CIN's ways are kept whole and a
  pin's is the mean of what the three outputs ask beyond them.

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

The netlist comes from Yosys, `synth_xilinx -family xc3se` (Silinx's synthesis); the ports' pads are `LOC`
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

## WebAssembly

`ci/build-wasm.sh` builds `nextpnr-himbaechel-spartan3e` for single-threaded WASI and packages it for npm, as YoWASP
packages nextpnr (`npm/`: `@silinx/nextpnr-himbaechel-spartan3e`, run by `@yowasp/runtime`). Linux only.
- **Toolchain:** the WASI SDK 34, with C++ exceptions as WebAssembly exceptions; nextpnr needs them since it dropped
  its WASI workarounds. No LTO: exceptions and LTO don't yet work together (WebAssembly/wasi-sdk#629). Boost 1.81 and
  Eigen 3.4 are built for it.
- **The package:** the chip databases are kept outside the binary and read from `/share` (the package's resources).
  `jco` 1.19.0 and `@yowasp/runtime` 11.0.72, as `@yowasp/yosys` 0.70.

A `v*` tag's run also stages the package on npm, as `@silinx/nextpnr-himbaechel-spartan3e`, by npm's trusted
publishing (this repository and `spartan3e.yml`, set on the package's settings at npmjs.com), with its provenance. It
becomes public when the maintainer approves it, with 2FA: `npm stage list`, then `npm stage approve <id>`.

The CI's `wasm` job builds it, then runs the package in Node on the test netlists (`ci/test-wasm.mjs`): the maximum
frequency must equal the native build's exactly.

The chip databases are not the runtime's resources, since the runtime fetches all of its resources on the first run.
Each device's is a file of its own, `chipdb/chipdb-<device>.bin.gz`:
- `chipdb(part)` reads it from disk in Node, or fetches it over HTTP in a browser, and decompresses it;
- the caller gives it to nextpnr as a file, with `--chipdb`;
- only the device in use is fetched.

Sizes (2026-10-30):
- `nextpnr-himbaechel-spartan3e.wasm`: 1.76 MB;
- the chip databases, gzipped (as downloaded) / unpacked:

  | device | gzipped | unpacked |
  |---|---|---|
  | XC3S100E | 4.5 MB | 19.9 MB |
  | XC3S250E | 5.3 MB | 24.3 MB |
  | XC3S500E | 5.5 MB | 27.0 MB |
  | XC3S1200E | 6.1 MB | 32.3 MB |

- the npm package: 19.6 MB.

Reading and decompressing a chip database takes 0.1 s; a run takes 0.2 s on the test netlists.
