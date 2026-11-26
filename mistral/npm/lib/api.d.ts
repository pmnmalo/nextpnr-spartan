// @silinx/nextpnr-mistral: nextpnr-mistral (the Intel Cyclone V, on Mistral's database), in WebAssembly, run by YoWASP's
// runtime (@yowasp/runtime). Everything this file declares is the package's public interface; before 1.0.0 a minor
// version may change it.

/** A folder of files: a name maps to a sub-folder, or to a file's contents (a string or bytes). */
export type Tree = {
    [name: string]: Tree | string | Uint8Array
};

/** nextpnr's standard output or error: given each chunk of bytes, then null when nextpnr ends. */
export type OutputStream =
    (bytes: Uint8Array | null) => void;

/** How a run talks to its caller (YoWASP's runtime's options). */
export type RunOptions = {
    stdin?:  ((byteLength: number) => Uint8Array | null) | null;
    stdout?: OutputStream | null;
    stderr?: OutputStream | null;
    decodeASCII?: boolean;
    synchronously?: boolean;
    fetchProgress?: (event: { totalLength: number, doneLength: number }) => void;
};

/** Thrown when nextpnr exits with a code other than 0; its "ERROR:" lines on the output say why. */
export class Exit extends Error {
    code: number;
    files: Tree;
}

/** A run of nextpnr: its command-line arguments, its input files, its streams; resolves to the files it wrote. */
export type Command =
    (args?: string[], files?: Tree, options?: RunOptions) => Promise<Tree> | Tree | undefined;

/**
 * nextpnr-mistral: --device <model> --json <netlist> --qsf <constraints> --rbf <bitstream>, and --seed, --freq,
 * --sdc, --router, --timing-allow-fail, --compress-rbf. The part's routing data among the files (data(part)):
 *
 *     const files = await runNextpnrMistral(
 *         ['--device', '5CEBA4F23C7', '--json', 'design.json', '--qsf', 'design.qsf', '--rbf', 'design.rbf'],
 *         { ...await data('5CEBA4F23C7'), 'design.json': netlist, 'design.qsf': qsf }, { decodeASCII: false });
 *     files['design.rbf'];
 */
export const runNextpnrMistral: Command;

export const commands: {
    'nextpnr-mistral': Command,
};

/** The package's version (as in its package.json). */
export const version: string;

/** The dies whose routing data the package has (e50f, sx120f): a minor version may add some. */
export const dies: string[];

/** The die of a part (5CEBA4F23C7 -> e50f; 5CSEMA5F31C6 and 5CSEBA6U23I7 -> sx120f); throws for a part without data. */
export function dieOf(part: string): string;

/** URL of a part's routing data file: data/<die>-r.bin, beside the package's bundle. */
export function dataUrl(part: string): URL;

/** A part's routing data as the files to give nextpnr: { '<die>-r.bin': bytes }. */
export function data(part: string): Promise<{ [name: string]: Uint8Array }>;
