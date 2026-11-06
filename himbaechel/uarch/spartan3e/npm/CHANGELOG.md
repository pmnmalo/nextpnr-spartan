# Changelog of @silinx/nextpnr-himbaechel-spartan3e

Its versions follow [semantic versioning](https://semver.org/) on the interface the [README](README.md) describes
("The interface", "Stability"). Each one is the fork's tag `v<version>`, its GitHub release (the package and the chip
databases) and its npm package. A version's changes are written here before it is tagged.

## 1.1.1

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
