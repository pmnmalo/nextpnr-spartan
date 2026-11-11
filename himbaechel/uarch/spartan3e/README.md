# spartan3e: the Xilinx Spartan-3E in nextpnr-himbaechel

A Himbaechel micro-architecture for the Spartan-3E, developed for [Silinx ISE](https://github.com/pmnmalo/Silinx-ISE)
(the plan: [Silinx #2131](https://github.com/pmnmalo/Silinx-ISE/issues/2131)).

## The chip database

`gen/s3e_gen.py` builds it from Silinx's device graph of a part, `packages/silinx-fpga/devices/<part>.json.gz`:
- **Routing:** the tiles, wires, PIPs and nodes, computed by Silinx from ISE 14.7's `xdl -report -pips -all_conns`.
- **Sites:** the BELs inside them, written by hand: per slice two LUT4s, two flip-flops, the carry logic (two MUXCYs,
  two XORCYs), the F5 multiplexer and the FXMUX: an F6 on the lower slices of a CLB, the F7 on X0Y1, the F8 on X1Y1; on a SLICEM the LUTs as memories (MEM16F, MEM16G: RAM16X1S, RAM16X1D, RAM32X1S, SRL16*, SRLC16* through Q15); per IOB the pad buffer and its registers (IFF1 / IFF2, OFF1 / OFF2, TFF1, and the
  multiplexers between them and the pad), the BUFGMUX, the block RAM (RAMB16), the multiplier (MULT18X18SIO), the DCM and
  the JTAG's user access (BSCAN: BSCAN_SPARTAN3's site in the top-right corner, its pins as they are, a constant TDO on a
  slice's GND source, pmnmalo/Silinx#3176), one BEL each with the site's pins (the multiplier's cascade too: BCOUT -> BCIN, a PIP up the column); a constant 1 on each VCC site (VCCSRC) and a
  constant 0 on Y of each SLICEL used for nothing else (GNDSRC), for the block RAMs' and multipliers' constant pins:
  XDL's `vcc` and `gnd` nets, which ISE takes for constants.

Slices are packed as Silinx's packer does (`fpga/pack-luts.js`): the free places of a slice filled with unrelated LUTs and
flip-flops, the critical connections first (a criticality computed from the cells alone, before placement), unless that
costs more than 2 % more slices. A carry stage or an F5 reads a copy of a shared LUT (or the LUT itself, when its other
loads can leave by X / Y), not a LUT passing it through, a level of logic less; a stage whose select is the constant 1 is
left out, and so are a stage of a select at 0 and a sum of an LI at 0 (they pass their DI and their carry in: wires); a
stage's carry out read by logic, the chain's last or one mid-chain, leaves by XB / YB. A flip-flop alone (its D through
BX / BY) goes first into a free flip-flop place of a chain's or a wide multiplexer's slices, of its nets if it can, when
BX / BY is free or carries the same net and the control set suits (a slice's two flip-flops share CLK, CE, SR and whether
the set / reset is synchronous; one may be set and the other reset); a chain's carry in at 1 comes from the CLB's VCC site, a stage's constant select from a LUT giving it. The
other constants that need no LUT are routed as Silinx's router ties them, once placed: a chain's carry in at 0, a pad's
output, a flip-flop's D (through BX / BY), a SLICEM memory's address pin at 0 (a 4-deep FIFO's address bits 2 and 3: a LUT
each before, Silinx #2982), a 1 from the VCC site that reaches the pin, a 0 from the Y of the nearest slice
that holds nothing (not an occupied slice: ISE's map counts none). A flip-flop's enable and set / reset active low
(`IS_CE_INVERTED`, `IS_R_INVERTED`…) are the slice's CEINV / SRINV, its two flip-flops of one polarity. A global buffer's clock pins are routed on the global lines before
everything else: on general routing they would be skewed, and ISE's DRC does not tell.

The clocks keep their dedicated paths, as Silinx's placer kept them (pmnmalo/Silinx-ISE#2770): the DCMs' sites are chosen
together, the most clock outputs on buffers of their own first, then the most clock inputs (CLKIN, CLKFB) from a pad or a
buffer's line; a pad's buffer is one whose line reaches the DCM inputs it drives; a clock switch takes its pad's buffer
before a plain BUFG; and the clock inputs of the DCMs and buffers from a pad or a DCM are routed on their dedicated paths
before the rest. A DCM's CLKIN from a pad that does not reach it so (a LOC, `CLOCK_DEDICATED_ROUTE = FALSE`) is said, with
the pads that do (pmnmalo/Silinx-ISE#2768). The two IOBs of a differential pair share their tile's clock wires: two registers of one kind
on different clocks there take the DIFFM's second flip-flop (IFF2, OFF2, which also drives the pad: OMUX = OFF2), as
ISE's map puts it (pmnmalo/Silinx-ISE#2766).

The placement is HeAP's, then a timing-driven annealing (placer1's refinement from a warm temperature, in a window of 30
tiles, 30 passes a temperature, the timing cost weighted 0.9 against the wirelength's 0.1) that moves the carry chains and
slices whole. The placer's delays from a CLB to a CLB are those Silinx's own placer took: its table of the fastest routes
(`timing.js` delayTable), by the offset across and the offset up or down, from a LUT's output and from a flip-flop's apart
(the hexes and long lines make the delay a step function of the distance, a diagonal slower than a straight line of as
many tiles); to and from the other sites, Silinx's routes' delays by the distance. A net's load adds about 60 ps a doubling
of its fanout, and a SLICEM's LUT is 55 ps slower than a SLICEL's (pmnmalo/Silinx#2982). On the examples, periods 2 %
shorter than HeAP's greedy refinement, for twice to five times its time. The annealing placer alone (`--placer sa`) places
the carry chains too: their cells' places checked against getClusterPlacement's, not the clusters' estimated offsets.

The router is router 1, its arcs by their criticality first (`router1/crit_first`, on by default here: a CLB's direct
connections, from X / Y to some pins of its LUTs, are the fastest, and a critical short arc routed last finds them
taken), its estimate of the way left the least a route takes (0.2 ns and 65 ps a tile). With the carry's delays below,
on the examples: periods 1.4 % shorter on average than v0.11's, the routing 30 % faster.

Router 1 has an effort, the wires its searches may visit in all: 30000 per arc to route, and 200 million at most
(`-o max-visits-per-arc=<n>`, `-o max-visits=<n>`, 0 for no limit; the settings `router1/maxVisitsPerArc` and
`router1/maxVisits`). Past it, router 1 stops, its routes are ripped up and router 2 routes the design. On a congested
design router 1 rips up for minutes to an hour, each search flooding the device before it takes another net's wire:
vtr-diffeq1 of Silinx's design corpus (13 of the XC3S500E's 20 multipliers, their outputs far from their loads), 20
minutes natively, 2 with the effort; on a large one it converges slowly: secworks-aes (93 % of the XC3S500E's LUTs),
over 15 minutes, 5 with the effort (Silinx #2912). The designs router 1 finishes in a few minutes visit at most about
20000 wires per arc and 110 million in all (the test netlists, the corpus's: neorv32), and router 2 would make their
periods 5 to 20 % longer, so they keep router 1. Router 2 routes with its alternate weights (`--router2-alt-weights`:
an estimate weight of 1.0, not 1.25), its critical paths shorter: secworks-sha256 41.5 MHz, 36.2 with the default
weights, 43.8 by router 1 alone; secworks-aes 34.7 MHz, 30.0; epfl-multiplier 14.3, 13.8 (Silinx #3002).

A carry stage's DI comes through pin 1 of its LUT (CY0F / CY0G) or through BX / BY. When the LUT's four inputs are four
other nets, pin 1 is theirs, so the DI's PIP from it is blocked before routing (Silinx #2977): router 2 took that pin
for the DI, a net in no conflict that it never moved, and left the LUT's nets three pins, overused for ever. Router 2
stops after 500 iterations (the setting `router2/maxIter`, `-o router2-max-iter=<n>`; 0, router 2's own default, for no
limit), the routing failed with an `ERROR:` naming the overused wires and their nets, not running with no end.

With the alternate weights router 2's congestion weight does not grow, and a congested wire costs a critical arc little
more than a free one (its criticality weight down to 0.05): two critical nets of Silinx's itc99-b20 shared a LUT's pin
G3 in WebAssembly from about the 400th iteration to the 500th, its pin G1 free, and epfl-multiplier's overuse went down
too slowly to converge in 500 (Silinx #2977). The weight grows (by 2 per iteration, the default weights' rate) after 20
iterations with no lower overuse (`router2/stallIter`, `-o router2-stall-iter=<n>`) and from the 200th iteration on
(`router2/growIter`, `-o router2-grow-iter=<n>`); 0 for neither, router 2's own default. The designs router 2 routes in
fewer iterations, without a stall, route as before.

A chain of multipliers (B_INPUT = CASCADE: B from BCIN, the BCOUT of the one below; its B pins left unconnected, as
ISE's DRC wants) is a cluster placed from its lowest multiplier up, on consecutive sites of a block RAM column, as
Silinx's placer does; a LOC on one of them is refused. A block RAM and the multiplier of its tile share the tile's routing: they go together only when the
block RAM's ports are at most 18 bits wide (a port of 36 takes the multiplier's inputs), as Silinx's placer keeps them.

The packer puts a flip-flop into its pad's IOB when asked (the attribute `IOB` on it or on the pad's buffer, which
Silinx writes from the UCF's `IOB = TRUE`), as ISE's map does; an IDDR2 / ODDR2 is always there, as two flip-flops (the
second on the falling edge when its clock is the first's inverted). A global buffer driven by a clock pad is placed
where the pad's dedicated path reaches it, and the router takes that path. A DCM goes on the site where the most of
its clock outputs reach global buffers of their own by dedicated paths, then where a pad's dedicated path reaches its
CLKIN (the UCF's `INST LOC`, which Silinx gives as the attribute `BEL`, is kept); those buffers go there with it. The
other buffers go on the free top / bottom buffers in Silinx's order (`BUFG_ORDER`), then on the side buffers. A side
buffer drives its line in its half of the chip only, and in each half a line comes from the top / bottom buffer or the
side buffer, never both: the placer keeps every clock pin in reach of its buffer's line (Silinx #95, #2627).

A port with no `LOC` goes on a free bonded pad, as Silinx's placer chooses them (Silinx #2625): an input-only pad for an
input only, a bank whose VCCO its I/O standard needs (`IOSTANDARD`, LVCMOS25 if none), the clock inputs first (a DCM's
CLKIN, then a clock switch's two pads, then a global buffer's: on their dedicated paths), and an I/O with registers kept
out of a differential pair whose other IOB has registers of that kind on another clock (the pair shares its tile's
clock wires, Silinx #749). The chip database gives each pad its site's type and its bank.

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
attributes (a port without one gets a free pad):

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

The package's interface (its module, the chip databases' files, the arguments, the netlist's attributes, the routed
design's and the log's form) is stable since 1.0.0, under semantic versioning: [`npm/README.md`](npm/README.md) says
what it holds and what each kind of version may change; `ci/test-api.mjs` checks the built package against it. A
release is prepared in a pull request: its version in `npm/package.json` and its section in
[`npm/CHANGELOG.md`](npm/CHANGELOG.md); the tag `v<version>` must be that version (`ci/build-wasm.sh` refuses
another), and a build without a tag is `<version>-dev.<commits>`.

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
