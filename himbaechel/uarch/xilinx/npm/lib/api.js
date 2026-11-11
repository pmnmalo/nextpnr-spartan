// nextpnr-himbaechel-xilinx in WebAssembly (@silinx/nextpnr-himbaechel-xc7), run by YoWASP's runtime (as
// @yowasp/nextpnr-* are). The chip databases are not the runtime's resources (all of them would be fetched on the first
// run): each device's is a file of its own, chipdb/chipdb-<device>.bin.gz, which chipdb(part) fetches and decompresses
// for the caller to pass to nextpnr: run(['--device', 'xc7a35tcpg236-1', '--chipdb', 'chipdb.bin', …],
// { 'chipdb.bin': await chipdb('xc7a35tcpg236-1'), … }).
import { Application } from '@yowasp/runtime';
import * as resources from '../gen/nextpnr-himbaechel-xilinx-resources.js';
import { instantiate } from '../gen/nextpnr-himbaechel-xilinx.js';

export { Exit } from '@yowasp/runtime';

const nextpnrHimbaechelXilinx = new Application(resources, instantiate, 'yowasp-nextpnr-himbaechel-xilinx');
const runNextpnrHimbaechelXilinx = nextpnrHimbaechelXilinx.run.bind(nextpnrHimbaechelXilinx);

export { runNextpnrHimbaechelXilinx };

export const commands = {
    'nextpnr-himbaechel-xilinx': runNextpnrHimbaechelXilinx,
};

export const version = VERSION;

/** The devices whose chip databases the package has: their names (xc7a50t) as in the chip databases' file names. */
export const devices = ['xc7a50t', 'xc7a100t'];

/** The parts whose fabric is another device's in Project X-Ray's database: the xc7a35t is the xc7a50t's. */
const fabricOf = { xc7a35t: 'xc7a50t' };

/** The device whose chip database a part takes (xc7a100tcsg324-1 -> xc7a100t, xc7a35tcpg236-1 -> xc7a50t). */
export function deviceOf(part) {
    const m = /^(xc7a\d+t)/i.exec(String(part));
    const device = m && (fabricOf[m[1].toLowerCase()] ?? m[1].toLowerCase());
    if (!device || !devices.includes(device))
        throw new Error(`no chip database for '${part}' (the devices: ${devices.join(', ')}; the xc7a35t takes the xc7a50t's)`);
    return device;
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
