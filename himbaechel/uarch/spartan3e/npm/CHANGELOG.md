# Changelog of @silinx/nextpnr-himbaechel-spartan3e

Its versions follow [semantic versioning](https://semver.org/) on the interface the [README](README.md) describes
("The interface", "Stability"). Each one is the fork's tag `v<version>`, its GitHub release (the package and the chip
databases) and its npm package. A version's changes are written here before it is tagged.

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
