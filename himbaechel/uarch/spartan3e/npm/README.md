# @silinx/nextpnr-himbaechel-spartan3e

nextpnr-himbaechel with the Xilinx Spartan-3E micro-architecture (`spartan3e`), in WebAssembly, run by YoWASP's runtime
(`@yowasp/runtime`), in Node and in browsers. It places and routes the Spartan-3E designs of
[Silinx ISE](https://github.com/pmnmalo/Silinx-ISE), whose bitgen writes the bitstream. Its source is the fork
[pmnmalo/nextpnr-spartan](https://github.com/pmnmalo/nextpnr-spartan) (branch `silinx`,
`himbaechel/uarch/spartan3e/`), which says how it is built.

Devices: the XC3S100E, XC3S250E (CP132), XC3S500E and XC3S1200E (FG320): the Digilent Basys2 and Nexys2 boards.

```js
import { runNextpnrHimbaechelSpartan3e, chipdb, Exit } from '@silinx/nextpnr-himbaechel-spartan3e';

const part = 'xc3s250ecp132-4';
const files = await runNextpnrHimbaechelSpartan3e(
  ['--device', part, '--chipdb', 'chipdb.bin', '--json', 'design.json', '--write', 'routed.json', '--seed', '1'],
  { 'chipdb.bin': await chipdb(part), 'design.json': netlist },
  { stdout: bytes => {}, stderr: bytes => {} });
const routed = JSON.parse(files['routed.json']);
```

## The interface

What 1.0.0 freezes (its "Stability" below):

- **The module** (`lib/api.d.ts`, documented there): `runNextpnrHimbaechelSpartan3e`, `commands`, `Exit`,
  `version`, `devices`, `deviceOf(part)`, `chipdbUrl(part)` and `chipdb(part)`, and the types `Tree`, `RunOptions`,
  `InputStream`, `OutputStream`, `ProgressCallback`, `Command`, `Application` (types only).
- **The chip databases:** one file per device, `chipdb/chipdb-<device>.bin.gz` in the package (`<device>`: `xc3s250e`),
  and the same files as assets of the GitHub release `v<version>` of the fork, which an installation without them
  downloads (Silinx's release zip does). `chipdb(part)` reads or fetches one and decompresses it; it works with the
  nextpnr of the same package version only (its format is not part of the interface).
- **The command:** these arguments of nextpnr-himbaechel, as Silinx passes them:

  | argument | |
  |---|---|
  | `--device <part>` | the part, with its package and speed grade: `xc3s250ecp132-4`, `xc3s500efg320-5` |
  | `--chipdb <file>` | the device's chip database, `chipdb(part)`'s bytes |
  | `--json <file>` | the netlist (below) |
  | `--write <file>` | the routed design (below) |
  | `--seed <n>` | the placement's seed |
  | `--freq <MHz>` | the target frequency of the clocks the SDC leaves out |
  | `--sdc <file>` | the clocks' periods: `create_clock -period <ns> [get_nets {<net>}]` |
  | `--timing-allow-fail` | a design whose timing fails is still routed and written |

  | `-o max-visits-per-arc=<n>`, `-o max-visits=<n>` | optional (1.1.0): router 1's effort, the wires its searches may visit per arc and in all, past which router 2 routes the design (defaults 30000 and 200000000; 0: no limit) |
  | `-o router2-max-iter=<n>`, `-o router2-stall-iter=<n>`, `-o router2-grow-iter=<n>` | optional (1.4.0): router 2's iterations at most, past which the routing fails (default 500); the iterations with no lower overuse, past which its congestion weight grows (default 20); the iteration from which it grows in any case (default 200) (0: no limit, never) |

  nextpnr's other arguments work too, without a promise.
- **The netlist:** Yosys's JSON of `synth_xilinx -family xc3se`, with the primitives of the Spartan-3E that Yosys
  maps to (LUTs, flip-flops and latches, carry logic, MUXF5–F8, distributed RAMs and shift registers, block RAMs,
  multipliers, DCMs, clock buffers, IOBs, IDDR2 / ODDR2), and the attributes the packer reads: a port's `LOC` (a pad)
  and `IOSTANDARD`, a cell's `BEL` (a site), `IOB` (a register in its pad's IOB), the inversions `IS_C_INVERTED`, `IS_G_INVERTED`, `IS_CE_INVERTED`, `IS_R_INVERTED`, … of the
  flip-flops and latches.
  A new primitive or attribute comes in a minor version.
- **The routed design:** nextpnr's JSON, with each cell's place in its attribute `NEXTPNR_BEL`
  (`<tile>/S<site index>_<BEL>`: the BEL names of `gen/s3e_gen.py`, as XDL's sites and their elements), each net's
  route in its attribute `ROUTING` (blank for a net with no route; nextpnr's `<wire>;<pip>;<strength>;…`, a wire `<tile>/<name>` and a PIP
  `<tile>/<to>/<from>`, XDL's names), and the parameters the packer writes for
  Silinx (`SILINX_TYPE`, the slices' `CLKINV`, `CEINV`, `SRINV`, …), which Silinx's `nextpnr-xdl.js` turns into XDL.
- **The log:** nextpnr's lines `ERROR: …` (why it stopped), `Warning: …` and
  `Max frequency for clock '<clock>': <f> MHz`. A run that fails throws: `Exit` for an exit code other than 0, or
  WebAssembly's exception for an error nextpnr stops on; its `ERROR:` lines say why.

Not part of it: where cells are placed and how nets are routed, the frequencies, the wording of the messages, the
chip database's format, the C++ code. They change in minor versions (a better placement is a minor version); Silinx
pins an exact version.

## Stability

Since 1.0.0 the package follows [semantic versioning](https://semver.org/) on the interface above:

- **major** (2.0.0): a change that breaks it: an export removed or changed, an argument or attribute no longer
  read the same way, a device dropped, a chip database's file name or a release asset's moved, a routed design a
  reader of 1.x can no longer convert;
- **minor** (1.1.0): an addition (a device, a primitive, an attribute, an export, an argument), and any change of
  the results: placement, routing, timing, messages;
- **patch** (1.0.1): a fix that changes neither the interface nor, except for the fix, the results.

The changes of each version: [CHANGELOG.md](CHANGELOG.md).

## Licence

nextpnr's licence, ISC (the fork's `COPYING`).
