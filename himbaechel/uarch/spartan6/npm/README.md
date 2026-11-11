# @silinx/nextpnr-himbaechel-spartan6

nextpnr-himbaechel with the Xilinx Spartan-6 micro-architecture (`spartan6`), in WebAssembly, run by YoWASP's runtime
(`@yowasp/runtime`), in Node and in browsers. It places and routes the Spartan-6 designs of
[Silinx ISE](https://github.com/pmnmalo/Silinx-ISE), whose bitgen writes the bitstream. Its source is the fork
[pmnmalo/nextpnr-spartan](https://github.com/pmnmalo/nextpnr-spartan) (branch `silinx`,
`himbaechel/uarch/spartan6/`), which says how it is built.

Devices: the XC6SLX16 and XC6SLX45 (CSG324): the Digilent Nexys3 and Atlys boards.

```js
import { runNextpnrHimbaechelSpartan6, chipdb, Exit } from '@silinx/nextpnr-himbaechel-spartan6';

const part = 'xc6slx16csg324-3';
const files = await runNextpnrHimbaechelSpartan6(
  ['--device', part, '--chipdb', 'chipdb.bin', '--json', 'design.json', '--write', 'routed.json', '--seed', '1'],
  { 'chipdb.bin': await chipdb(part), 'design.json': netlist },
  { stdout: bytes => {}, stderr: bytes => {} });
const routed = JSON.parse(files['routed.json']);
```

The module (`lib/api.d.ts`): `runNextpnrHimbaechelSpartan6`, `commands`, `Exit`, `version`, `devices`,
`deviceOf(part)`, `chipdbUrl(part)` and `chipdb(part)`. The chip databases are one file per device,
`chipdb/chipdb-<device>.bin.gz` in the package (`<device>`: `xc6slx16`), and the same files are assets of the fork's
GitHub release `spartan6-v<version>`; `chipdb(part)` reads or fetches one and decompresses it, for the nextpnr of the
same package version only.

The package is before 1.0.0: a minor version may change its interface. Silinx pins an exact version. The changes of
each version: [CHANGELOG.md](CHANGELOG.md).

## Licence

nextpnr's licence, ISC (the fork's `COPYING`).
