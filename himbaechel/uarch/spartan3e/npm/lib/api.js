// nextpnr-himbaechel-spartan3e in WebAssembly, run by YoWASP's runtime (as @yowasp/nextpnr-* are): the chip databases
// (share/himbaechel/spartan3e) are its resources, fetched on the first run.
import { Application } from '@yowasp/runtime';
import * as resources from '../gen/nextpnr-himbaechel-spartan3e-resources.js';
import { instantiate } from '../gen/nextpnr-himbaechel-spartan3e.js';

export { Exit } from '@yowasp/runtime';

const nextpnrHimbaechelSpartan3e = new Application(resources, instantiate, 'yowasp-nextpnr-himbaechel-spartan3e');
const runNextpnrHimbaechelSpartan3e = nextpnrHimbaechelSpartan3e.run.bind(nextpnrHimbaechelSpartan3e);

export { runNextpnrHimbaechelSpartan3e };

export const commands = {
    'nextpnr-himbaechel-spartan3e': runNextpnrHimbaechelSpartan3e,
};

export const version = VERSION;
