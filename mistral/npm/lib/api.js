// nextpnr-mistral in WebAssembly (@silinx/nextpnr-mistral), run by YoWASP's runtime (as @yowasp/nextpnr-* are). Mistral's
// routing data are not linked in (mistral/ci/mistral-external-data.mjs): the global data are the runtime's resources
// (/share/mistral/global.bin), and each die's a file of its own, data/<die>-r.bin, which data(part) fetches for the caller
// to pass to nextpnr in its working directory: run(['--device', '5CEBA4F23C7', '--json', 'design.json', '--qsf',
// 'design.qsf', '--rbf', 'design.rbf'], { ...await data('5CEBA4F23C7'), … }).
import { Application } from '@yowasp/runtime';
import * as resources from '../gen/nextpnr-mistral-resources.js';
import { instantiate } from '../gen/nextpnr-mistral.js';
import models from '../gen/models.json';

export { Exit } from '@yowasp/runtime';

const nextpnrMistral = new Application(resources, instantiate, 'yowasp-nextpnr-mistral');
const runNextpnrMistral = nextpnrMistral.run.bind(nextpnrMistral);

export { runNextpnrMistral };

export const commands = {
    'nextpnr-mistral': runNextpnrMistral,
};

export const version = VERSION;

/** The dies whose routing data the package has (e50f, sx120f), as Mistral names them. */
export const dies = [...new Set(Object.values(models))].sort();

/** The die of a part (Mistral's model name, as nextpnr's --device: 5CEBA4F23C7 -> e50f, 5CSEMA5F31C6 -> sx120f); throws
 *  for a part the package has no data of. */
export function dieOf(part) {
    const die = models[String(part).toUpperCase()];
    if (!die)
        throw new Error(`no Mistral data for '${part}' (a model of the dies ${dies.join(', ')}, as 5CEBA4F23C7, 5CSEMA5F31C6, 5CSEBA6U23I7)`);
    return die;
}

/** URL of a part's routing data file (data/<die>-r.bin, an xz stream nextpnr decompresses), beside the package's bundle. */
export const dataUrl = part => new URL(`../data/${dieOf(part)}-r.bin`, import.meta.url);

/** A part's routing data, as the files to give nextpnr ({ '<die>-r.bin': bytes }): read from disk in Node, fetched over
 *  HTTP in a browser. */
export async function data(part) {
    const url = dataUrl(part);
    let bytes;
    if (url.protocol === 'file:') {
        const fs = await import('node:fs/promises');
        bytes = new Uint8Array(await fs.readFile(url));
    } else {
        const response = await fetch(url);
        if (!response.ok)
            throw new Error(`${url}: HTTP ${response.status}`);
        bytes = new Uint8Array(await response.arrayBuffer());
    }
    return { [`${dieOf(part)}-r.bin`]: bytes };
}
