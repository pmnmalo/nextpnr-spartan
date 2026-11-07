# Changelog of @silinx/nextpnr-himbaechel-spartan3e

Its versions follow [semantic versioning](https://semver.org/) on the interface the [README](README.md) describes
("The interface", "Stability"). Each one is the fork's tag `v<version>`, its GitHub release (the package and the chip
databases) and its npm package. A version's changes are written here before it is tagged.

## 1.4.0

Router 2 converges where its alternate weights (1.1.1) left it short of 500 iterations (pmnmalo/Silinx-ISE#2977): with
them its congestion weight did not grow, and a congested wire cost a critical arc little more than a free one (its
criticality weight down to 0.05). In WebAssembly, two critical nets of Silinx's itc99-b20 (XC3S250E) shared a LUT's pin
G3 from about the 400th iteration to the 500th, its pin G1 free; epfl-multiplier (XC3S500E) ended at the 500th with 3
to 10 wires overused, its overuse going down slowly.
- The weight grows by 2 per iteration (the default weights' rate) after 20 iterations with no lower overuse
  (`router2/stallIter`) and from the 200th iteration on (`router2/growIter`): two settings new in nextpnr's router 2, 0
  there (no such growth), set by the Spartan-3E. Natively, Silinx's design corpus: itc99-b21 28.90 MHz in 216
  iterations (325 on 1.3.0), b22 26.66 in 221 (310), secworks-sha256 42.51 in 182 (190), secworks-aes 37.85 in 212
  (37.65 in 361), vtr-diffeq1 26.62 in 214 (26.39 in 236), epfl-multiplier 14.17 in 261 (failed); in WebAssembly
  itc99-b20 routed (it failed). The designs router 1 routes within its effort route as on 1.3.0.
- Router 2's error names every net of each overused wire: `… overused: X11Y8/G3_B_PINWIRE2 (nets a, b), …`.
- The arguments `-o router2-max-iter=<n>`, `-o router2-stall-iter=<n>` and `-o router2-grow-iter=<n>` set them.
- The test netlists: ram_const_addr 324.04 -> 304.97 MHz (one wire overused from the 3rd iteration to the 53rd, now
  converged in 33), and once more with router 2 at most 40 iterations (with `-o router2-stall-iter=0` it fails); the
  others unchanged.

## 1.3.0

Router 2 converges on Silinx's ITC'99 b20, b21 and b22 (XC3S250E), and no longer routes with no end
(pmnmalo/Silinx-ISE#2977). On these designs router 1 reached its effort, and router 2 never converged: 36 to 42 LUT
input pins overused after more than 13000 iterations, with no limit. A carry stage's DI comes through pin 1 of its LUT
(CY0F / CY0G) or through BX / BY. Router 2 took pin 1 for the DI, a net in no conflict that it never moved, which left
the LUT's four nets three pins.
- The DI's PIP from pin 1 is blocked before routing when the LUT's four inputs are four other nets and BX / BY is open
  to the DI (`block_di_on_full_luts`). Natively, b20 is routed by router 1 within its effort (30.80 MHz, 400 s; router
  2 alone: 29.03 MHz, converged in 75 iterations).
- Router 2 stops after 500 iterations (the setting `router2/maxIter`, new in nextpnr's router 2; 0, its default there,
  for no limit), the routing failed with `ERROR: Routing design failed: router 2 did not converge in 500 iterations
  (router2/maxIter), <n> wire(s) still overused: <the first five, with their nets>`.
- The results change where router 1 tried pin 1 for such a DI and ripped up: the test netlists vga 113.44 -> 113.31 MHz,
  its fallback to router 2 (with its alternate weights, 1.1.1) 111.84 -> 110.90, ffcarry 163.69 -> 176.96; the others
  are unchanged.
- The test netlist `di_full_lut.json` (hand-written, routed by router 2: router 1's effort cut to 1 wire per arc): 8
  chains of 32 stages, each stage's LUT on four registers and its DI a fifth; before, router 2 stopped at 500
  iterations with 10 wires overused.

## 1.2.0

A SLICEM memory's address pin at 0 is routed from a free slice's GND source, as Silinx's packer ties it, no longer given
a LUT of its own (pmnmalo/Silinx-ISE#2982). A RAM16X1D of a 4-deep memory has its address bits 2 and 3 at 0 on both
ports (A2, A3, DPRA2, DPRA3), and nextpnr's JSON front end gives each of them its own constant: a FIFO of 8 such RAMs
took 32 LUTs for its address, each a unit of the slice packing that took a slice place and pulled the RAM's slice
toward it. Silinx's design corpus, XC3S1200E at Optimization Speed: iwls05-sasc 80 slices, 4.563 ns -> 52 slices,
4.488 ns (Silinx's former engine: 49 slices); iwls05-simple-spi 111 slices, 6.445 ns -> 79 slices, 5.952 ns.
- The test netlist `ram_const_addr.json` (hand-written): two RAM16X1Ds, A2 / A3 and DPRA2 / DPRA3 at 0, no LUT left.
- The test netlists with such memories, `hard_prim.json` (1 LUT fewer) and `srl_en.json` (12 fewer), are placed
  otherwise: their native frequencies anew (145.79 -> 136.24 and 86.60 -> 84.19 MHz on seed 1, the best of 8 seeds
  147.65 -> 148.77 and 88.18 -> 86.98 MHz); the others are unchanged.

## 1.1.2

A carry chain whose stages share one LUT no longer stops the placer (pmnmalo/Silinx-ISE#2818): a LUT on the S of two
stages of one chain, their sums not used, was taken as the LUT of both, so the chain's cluster held it twice and the
placer bound it on two BELs. The analytical placer then stopped on an assertion (`entry == nullptr`, a BEL bound
twice), and the annealing one left a net it could not route. The second stage now takes a copy of the LUT, as when
the LUT feeds other logic. Silinx's ITC'99 b14 (XC3S250E) is placed and routed. The test netlists' frequencies are
unchanged.
- An exception nextpnr does not handle itself, such as an assertion's failure, is said as an `ERROR:` line, and nextpnr
  ends with code 125. Before, the process stopped on the exception: in WebAssembly an exception has no message, so
  nothing said why.
- The test netlist `chain_shared_lut.json` (hand-written): one LUT on the S of a chain's two first stages.

## 1.1.1

Router 2, where router 1 stops past its effort, routes with its alternate weights (`--router2-alt-weights`: an estimate
weight of 1.0, not 1.25; pmnmalo/Silinx-ISE#3002): its critical paths shorter, in the same time. Natively, Silinx's
design corpus: secworks-sha256 41.5 MHz (36.2 on 1.1.0, 43.8 by router 1 alone, in four times its time), secworks-aes
34.7 (30.0), epfl-multiplier 14.3 (13.8), vtr-diffeq1 25.6 (26.6). The designs router 1 routes within its effort route
as on 1.1.0.

## 1.1.0

Router 1 has an effort (pmnmalo/Silinx-ISE#2912): past 30000 wires visited per arc to route, or 200 million in all, it
stops and router 2 routes the design, router 1's routes ripped up. On a congested design router 1 ripped up for up to
an hour, each search flooding the device, and on a large one it converged slowly. Natively, Silinx's design corpus:
vtr-diffeq1 (13 of the XC3S500E's 20 multipliers) 20 minutes, now 2; secworks-aes (93 % of the XC3S500E's LUTs) over
15, now 5; epfl-multiplier 12, now 4; epfl-log2 9, now 4. The designs router 1 finishes within its effort are routed as
in 1.0.0: the test netlists' frequencies are unchanged.
- The arguments `-o max-visits-per-arc=<n>` and `-o max-visits=<n>` set the effort (0: no limit; both 0: router 1
  alone, as in 1.0.0): the settings `router1/maxVisitsPerArc` and `router1/maxVisits` of nextpnr's router 1
  (`common/route/router1.cc`, 0 by default there), and its fallback to router 2 in himbächel (`himbaechel/arch.cc`).
- The test netlists take more arguments of nextpnr after their native frequency (`tests/expected.txt`): vga with an
  effort of 1000 per arc, so that router 2 finishes it.

## 1.0.0

The first stable version: Silinx ISE's only place and route engine for the Spartan-3E (pmnmalo/Silinx-ISE#2137), at
parity with the packer, placer and router it replaced, every primitive of theirs packed (the last, RAM64X1S:
pmnmalo/Silinx-ISE#2218). Its interface is frozen (pmnmalo/Silinx-ISE#2438):
- the module's exports, documented in `lib/api.d.ts`;
- the chip databases' files and the release's assets;
- the arguments Silinx passes to nextpnr, the netlist's attributes it reads, the routed design's and the log's form;
- the README says what is and what is not part of it, and what a major, a minor and a patch version may change;
- the CI checks the built package's exports against `lib/api.d.ts`, its version against `package.json`'s, and the
  interface's arguments on a test netlist (`ci/test-api.mjs`).

The placement and routing are 0.20.0's.

## 0.1.0 – 0.20.0 (2026-10-30 – 2026-11-05)

The development versions, while the micro-architecture reached parity with Silinx's toolchain (pmnmalo/Silinx-ISE#2135):
their notes are the fork's GitHub releases.
