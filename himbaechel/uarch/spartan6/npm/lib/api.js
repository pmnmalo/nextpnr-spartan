// nextpnr-himbaechel-spartan6 in WebAssembly, run by YoWASP's runtime (as @yowasp/nextpnr-* are). The chip
// databases are not the runtime's resources (all of them would be fetched on the first run): each device's is a file of
// its own, chipdb/chipdb-<device>.bin.gz, which chipdb(device) fetches and decompresses for the caller to pass to
// nextpnr: run(['--device', 'xc6slx16csg324-3', '--chipdb', 'chipdb.bin', …], { 'chipdb.bin': await chipdb('xc6slx16csg324-3'), … }).
import { Application } from '@yowasp/runtime';
import * as resources from '../gen/nextpnr-himbaechel-spartan6-resources.js';
import { instantiate } from '../gen/nextpnr-himbaechel-spartan6.js';

export { Exit } from '@yowasp/runtime';

const nextpnrHimbaechelSpartan6 = new Application(resources, instantiate, 'yowasp-nextpnr-himbaechel-spartan6');
const runNextpnrHimbaechelSpartan6 = nextpnrHimbaechelSpartan6.run.bind(nextpnrHimbaechelSpartan6);

export { runNextpnrHimbaechelSpartan6 };

export const commands = {
    'nextpnr-himbaechel-spartan6': runNextpnrHimbaechelSpartan6,
};

export const version = VERSION;

/** The devices whose chip databases the package has: their names (xc6slx16) as in the chip databases' file names. */
export const devices = ['xc6slx16', 'xc6slx45'];

/** The device of a part name (xc6slx16csg324-3 -> xc6slx16). */
export function deviceOf(part) {
    const m = /^(xc6slx\d+)/i.exec(String(part));
    if (!m || !devices.includes(m[1].toLowerCase()))
        throw new Error(`no chip database for '${part}' (the devices: ${devices.join(', ')})`);
    return m[1].toLowerCase();
}

/** URL of a part's chip database file (gzipped), beside this package's bundle. */
export const chipdbUrl = part => new URL(`../chipdb/chipdb-${deviceOf(part)}.bin.gz`, import.meta.url);

/** A part's chip database, decompressed (Uint8Array): read from disk in Node, fetched over HTTP in a browser. */
export async function chipdb(part) {
    const url = chipdbUrl(part);
    let bytes;
    if (url.protocol === 'file:') {
        const fs = await import('node:fs/promises');
        bytes = await fs.readFile(url);
    } else {
        const response = await fetch(url);
        if (!response.ok)
            throw new Error(`${url}: HTTP ${response.status}`);
        bytes = new Uint8Array(await response.arrayBuffer());
    }
    const stream = new Blob([bytes]).stream().pipeThrough(new DecompressionStream('gzip'));
    return new Uint8Array(await new Response(stream).arrayBuffer());
}
