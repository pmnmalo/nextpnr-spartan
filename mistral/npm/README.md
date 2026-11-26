# @silinx/nextpnr-mistral

nextpnr-mistral (nextpnr's Cyclone V architecture, on the [Mistral](https://github.com/Ravenslofty/mistral) project's
database), in WebAssembly, run by YoWASP's runtime (`@yowasp/runtime`), in Node and in browsers. It places and routes the
Cyclone V designs of [Silinx ISE](https://github.com/pmnmalo/Silinx-ISE) (Yosys's `synth_intel_alm -family cyclonev`) and
writes their bitstream, a `.rbf`. Its source is the fork [pmnmalo/nextpnr-spartan](https://github.com/pmnmalo/nextpnr-spartan)
(branch `silinx`, `mistral/`; `mistral/ci/build-wasm.sh` builds it, with Mistral at the commit `.github/workflows/mistral.yml`
pins). Experimental, as nextpnr-mistral is upstream.

Dies (Mistral's routing data, one file each, `data/<die>-r.bin`):
- **e50f**: the Cyclone V E A2 and A4 (5CEBA2…, 5CEBA4…): the Terasic DE0-CV's 5CEBA4F23C7;
- **sx120f**: the Cyclone V SE / SX A5 and A6 (5CSEMA5…, 5CSEBA6…): the Terasic DE1-SoC's 5CSEMA5F31C6 and the DE10-Nano's
  5CSEBA6U23I7.

```js
import { runNextpnrMistral, data, Exit } from '@silinx/nextpnr-mistral';

const part = '5CEBA4F23C7';
const files = await runNextpnrMistral(
  ['--device', part, '--json', 'design.json', '--qsf', 'design.qsf', '--rbf', 'design.rbf', '--seed', '1'],
  { ...await data(part), 'design.json': netlist, 'design.qsf': qsf },
  { stdout: bytes => {}, stderr: bytes => {}, decodeASCII: false });
const rbf = files['design.rbf'];
```

The interface is `lib/api.d.ts`. Licences: nextpnr's ISC (COPYING), Mistral's BSD-3-Clause (`LICENSE-mistral`: its code,
linked in, and its data, in `data/` and the runtime's resources).
