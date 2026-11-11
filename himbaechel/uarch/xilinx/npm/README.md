# @silinx/nextpnr-himbaechel-xc7

nextpnr-himbaechel with the Xilinx 7-series micro-architecture (`xilinx`, on Project X-Ray's database), in WebAssembly,
run by YoWASP's runtime (`@yowasp/runtime`), in Node and in browsers. It places and routes the 7-series designs of
[Silinx ISE](https://github.com/pmnmalo/Silinx-ISE) and writes their FASM, which Project X-Ray's tools turn into a
bitstream. Its source is the fork [pmnmalo/nextpnr-spartan](https://github.com/pmnmalo/nextpnr-spartan) (branch
`silinx`, `himbaechel/uarch/xilinx/`; `ci/build-wasm.sh` builds it), with the carry chains packed as Vivado packs them
(pmnmalo/Silinx#3160).

Devices (the chip databases):
- **xc7a50t**, which the **xc7a35t** takes too (its fabric in Project X-Ray's database): the Digilent Basys 3 and the Arty
  A7-35. nextpnr does not limit a 35t to its own 5,200 slices: it may use the 50t's 8,150;
- **xc7a100t**: the Digilent Nexys A7-100T and the Arty A7-100.

```js
import { runNextpnrHimbaechelXilinx, chipdb, Exit } from '@silinx/nextpnr-himbaechel-xc7';

const part = 'xc7a35tcpg236-1';
const files = await runNextpnrHimbaechelXilinx(
  ['--device', part, '--chipdb', 'chipdb.bin', '--json', 'design.json', '-o', 'xdc=design.xdc', '-o', 'fasm=design.fasm',
   '--write', 'routed.json', '--router', 'router1', '--seed', '1'],
  { 'chipdb.bin': await chipdb(part), 'design.json': netlist, 'design.xdc': xdc },
  { stdout: bytes => {}, stderr: bytes => {} });
const fasm = files['design.fasm'];
```

## The interface

- **The module** (`lib/api.d.ts`, documented there): `runNextpnrHimbaechelXilinx`, `commands`, `Exit`, `version`,
  `devices`, `deviceOf(part)`, `chipdbUrl(part)` and `chipdb(part)`, and the types `Tree`, `RunOptions`, `InputStream`,
  `OutputStream`, `ProgressCallback`, `Command`, `Application` (types only).
- **The chip databases:** one file per device, `chipdb/chipdb-<device>.bin.gz` in the package (`<device>`: `xc7a50t`,
  `xc7a100t`), and the same files as assets of the fork's GitHub release `xc7-v<version>`. `chipdb(part)` reads or
  fetches the one `deviceOf(part)` names and decompresses it; it works with the nextpnr of the same package version only.
- **The command:** nextpnr-himbaechel's arguments, among them:

  | argument | |
  |---|---|
  | `--device <part>` | the part, with its package and speed grade: `xc7a35tcpg236-1`, `xc7a100tcsg324-1` |
  | `--chipdb <file>` | the device's chip database, `chipdb(part)`'s bytes |
  | `--json <file>` | the netlist: Yosys's JSON of `synth_xilinx` (the 7-series' primitives) |
  | `-o xdc=<file>` | the constraints: the ports' `PACKAGE_PIN` and `IOSTANDARD`, the clocks' `create_clock` |
  | `-o fasm=<file>` | the FASM written, for Project X-Ray's `fasm2frames` and `xc7frames2bit` |
  | `--write <file>` | the routed design (nextpnr's JSON) |
  | `--router router1` / `router2` | router 1 takes about 0.4-0.5 GB in WebAssembly, router 2 1.1 GB on the xc7a35t and 1.85 GB on the xc7a100t but is 6 to 30 times faster |
  | `--seed <n>`, `--freq <MHz>`, `--timing-allow-fail` | as nextpnr's |

## Stability

Before 1.0.0 a minor version (0.2.0) may change the interface above; a patch (0.1.1) is a fix that changes neither the
interface nor, except for the fix, the results. Silinx pins an exact version. The changes of each version:
[CHANGELOG.md](CHANGELOG.md).

## Licence

nextpnr's licence, ISC (the fork's `COPYING`). The chip databases are built from Project X-Ray's database
(f4pga/prjxray-db) and nextpnr-xilinx-meta (gatecat), both CC0.
