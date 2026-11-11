// @silinx/nextpnr-himbaechel-xc7: nextpnr-himbaechel with the Xilinx 7-series (xilinx, on Project X-Ray's database), in
// WebAssembly, run by YoWASP's runtime (@yowasp/runtime). Everything this file declares is the package's public
// interface; before 1.0.0 a minor version may change it (the package's README: "Stability").

/** A folder of files: a name maps to a sub-folder, or to a file's contents (a string or bytes). */
export type Tree = {
    [name: string]: Tree | string | Uint8Array
};

/** nextpnr's standard input: asked for up to byteLength bytes, returns them, or null at the end. */
export type InputStream =
    (byteLength: number) => Uint8Array | null;

/** nextpnr's standard output or error: given each chunk of bytes, then null when nextpnr ends. */
export type OutputStream =
    (bytes: Uint8Array | null) => void;

/** The download of the runtime's resources (the WebAssembly module and its share/ folder), on the first run. */
export type ProgressCallback =
    (event: { source: Application, totalLength: number, doneLength: number }) => void;

/** How a run talks to its caller (YoWASP's runtime's options). */
export type RunOptions = {
    /** nextpnr's standard input (none: an empty one). */
    stdin?:  InputStream  | null;
    /** nextpnr's standard output (undefined: console.log, line by line; null: dropped). */
    stdout?: OutputStream | null;
    /** nextpnr's standard error (undefined: console.log, line by line; null: dropped). */
    stderr?: OutputStream | null;
    /** The files returned: an ASCII one as a string (true, the default), or every one as bytes (false). */
    decodeASCII?: boolean;
    /** Run synchronously, the result a Tree rather than a Promise (only once a first, asynchronous run has loaded the resources). */
    synchronously?: boolean;
    /** Told of the resources' download. */
    fetchProgress?: ProgressCallback;
};

/** A WebAssembly command (YoWASP's runtime's Application): a type only, the module exports none. */
export interface Application {
    argv0: string;

    /** Runs the command with args, on the files given (its working folder); returns the files it wrote. */
    run(args?: string[], files?: Tree, options?: RunOptions): Promise<Tree> | Tree | undefined;
}

/**
 * Thrown when nextpnr exits with a code other than 0. An error nextpnr stops on may also come out as another exception
 * (WebAssembly's, with no message): either way its "ERROR:" lines on the output say why.
 */
export class Exit extends Error {
    /** nextpnr's exit code. */
    code: number;
    /** The files as they were when nextpnr exited. */
    files: Tree;
}

//--------8<--------8<--------8<--------8<--------8<--------8<--------8<--------8<--------8<--------

/** A run of nextpnr: its command-line arguments, its input files, its streams; resolves to the files it wrote. */
export type Command =
    (args?: string[], files?: Tree, options?: RunOptions) => Promise<Tree> | Tree | undefined;

/**
 * nextpnr-himbaechel with the xilinx micro-architecture. The arguments Silinx passes (the README's "The command"):
 * --device <part> --chipdb <file> --json <netlist> -o xdc=<constraints> -o fasm=<fasm> --write <routed>, and --seed,
 * --freq, --router, --timing-allow-fail. Example:
 *
 *     const files = await runNextpnrHimbaechelXilinx(
 *         ['--device', 'xc7a35tcpg236-1', '--chipdb', 'chipdb.bin', '--json', 'design.json', '-o', 'xdc=design.xdc',
 *          '-o', 'fasm=design.fasm'],
 *         { 'chipdb.bin': await chipdb('xc7a35tcpg236-1'), 'design.json': netlist, 'design.xdc': xdc });
 *     files['design.fasm'];
 */
export const runNextpnrHimbaechelXilinx: Command;

/** The package's commands by name (as @yowasp/* packages list theirs). */
export const commands: {
    'nextpnr-himbaechel-xilinx': Command,
};

/** The package's version (as in its package.json). */
export const version: string;

/** The devices whose chip databases the package has (xc7a50t, xc7a100t): a minor version may add some. */
export const devices: string[];

/**
 * The device whose chip database a part takes, in lower case (xc7a100tcsg324-1 -> xc7a100t; xc7a35tcpg236-1 -> xc7a50t,
 * the xc7a35t being the xc7a50t's fabric in Project X-Ray's database); throws for a part without a chip database.
 */
export function deviceOf(part: string): string;

/** URL of a part's chip database file, gzipped: chipdb/chipdb-<device>.bin.gz, beside the package's bundle. */
export function chipdbUrl(part: string): URL;

/**
 * A part's chip database, decompressed: give it to nextpnr as a file, with --chipdb. Read from disk in Node (a missing
 * file: Node's ENOENT error, with its path), fetched over HTTP in a browser (a failure: an Error with the HTTP status).
 * A chip database works with the nextpnr of its own package version only.
 */
export function chipdb(part: string): Promise<Uint8Array>;
