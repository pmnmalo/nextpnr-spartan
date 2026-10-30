// nextpnr-himbaechel-spartan3e in WebAssembly, run by YoWASP's runtime (as @yowasp/nextpnr-* are). The chip
// databases are not the runtime's resources (all of them would be fetched on the first run): each device's is a file of
// its own, chipdb/chipdb-<device>.bin.gz, which chipdb(device) fetches and decompresses for the caller to pass to
// nextpnr: run(['--device', 'xc3s250ecp132-4', '--chipdb', 'chipdb.bin', …], { 'chipdb.bin': await chipdb('xc3s250ecp132-4'), … }).
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

/** The devices whose chip databases the package has: their names (xc3s250e) as in the chip databases' file names. */
export const devices = ['xc3s100e', 'xc3s250e', 'xc3s500e', 'xc3s1200e'];

/** The device of a part name (xc3s250ecp132-4 -> xc3s250e). */
export function deviceOf(part) {
    const m = /^(xc3s\d+e)/i.exec(String(part));
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
